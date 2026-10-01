include_guard(GLOBAL)

option(SNOW_ENABLE_CLANG_TIDY "Run clang-tidy while compiling C++ targets." OFF)
option(SNOW_APPS_ENABLE_RELEASE_SIZE_OPTIMIZATION
    "Prefer smaller code in explicitly selected cold Snow Shot Release modules." OFF)

function(snow_enable_unity_build target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "snow_enable_unity_build target does not exist: ${target}")
    endif()
    set_target_properties("${target}" PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 8
    )
endfunction()

function(snow_apply_strict_warnings target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "snow_apply_strict_warnings target does not exist: ${target}")
    endif()

    if(MSVC)
        target_compile_options("${target}" PRIVATE
            /FS /W4 /WX /permissive- /sdl /utf-8
            /Zc:__cplusplus /Zc:inline
            /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296
            /w14311 /w14545 /w14546 /w14547 /w14549 /w14555 /w14619
            /w14640 /w14826 /w14905 /w14906 /w14928 /wd4702
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options("${target}" PRIVATE
                -Wcast-align -Wcast-qual -Wconversion -Wdouble-promotion
                -Wformat=2 -Wimplicit-fallthrough -Wmissing-declarations
                -Wnon-virtual-dtor -Wnull-dereference -Wold-style-cast
                -Woverloaded-virtual -Wshadow -Wsign-conversion -Wundef
            )
        else()
            target_compile_options("${target}" PRIVATE /Zc:preprocessor)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options("${target}" PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wcast-align -Wcast-qual -Wconversion -Wdouble-promotion
            -Wformat=2 -Wimplicit-fallthrough -Wmissing-declarations
            -Wnon-virtual-dtor -Wnull-dereference -Wold-style-cast
            -Woverloaded-virtual -Wshadow -Wsign-conversion -Wundef
        )
    else()
        message(WARNING "Strict warning flags are not defined for ${CMAKE_CXX_COMPILER_ID}.")
    endif()
endfunction()

function(_snow_release_compile_options policy output)
    set(_options)
    if(MSVC)
        if(policy STREQUAL "size")
            set(_options /O1 /Os /Oi)
        else()
            set(_options /O2 /Oi /Ot)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        if(policy STREQUAL "size")
            set(_options -Os)
        else()
            set(_options -O3)
        endif()
    endif()
    set(_release_options)
    foreach(_option IN LISTS _options)
        list(APPEND _release_options "$<$<CONFIG:Release>:${_option}>")
    endforeach()
    set(${output} "${_release_options}" PARENT_SCOPE)
endfunction()

# Keep speed as the default for capture, rendering, codecs and native adapters.
# Callers opt cold modules into the size policy; both retain IPO and data stripping.
function(snow_apply_release_options target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "snow_apply_release_options target does not exist: ${target}")
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 _snow "" "POLICY" "")
    if(_snow_UNPARSED_ARGUMENTS OR _snow_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "snow_apply_release_options requires POLICY speed or size")
    endif()
    if(NOT DEFINED _snow_POLICY)
        set(_snow_POLICY speed)
    endif()
    if(NOT _snow_POLICY MATCHES "^(speed|size)$")
        message(FATAL_ERROR "Unknown release optimization policy: ${_snow_POLICY}")
    endif()
    set_property(TARGET "${target}" PROPERTY SNOW_RELEASE_OPTIMIZATION_POLICY "${_snow_POLICY}")

    if(MSVC AND SNOW_APPS_BUILD_SNOW_SHOT)
        target_compile_options("${target}" PRIVATE $<$<CONFIG:Release>:/Z7>)
        target_link_options("${target}" PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL>)
    endif()
    if(DEFINED SNOW_APPS_ENABLE_RELEASE_OPTIMIZATION AND
       NOT SNOW_APPS_ENABLE_RELEASE_OPTIMIZATION)
        return()
    endif()

    set_property(TARGET "${target}" PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    _snow_release_compile_options("${_snow_POLICY}" _compile_options)
    target_compile_options("${target}" PRIVATE ${_compile_options})
    if(MSVC)
        target_compile_options("${target}" PRIVATE
            $<$<CONFIG:Release>:/Oy>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/Gw>
            $<$<CONFIG:Release>:/Gy>
        )
        target_link_options("${target}" PRIVATE
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options("${target}" PRIVATE
            $<$<CONFIG:Release>:-flto>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
        )
        target_link_options("${target}" PRIVATE $<$<CONFIG:Release>:-flto>)
        if(APPLE AND SNOW_APPS_ENABLE_RELEASE_SIZE_OPTIMIZATION)
            # Function/data sections alone do not enable the Mach-O linker's
            # dead stripping. Keep it limited to shipping size-optimized builds.
            target_link_options("${target}" PRIVATE $<$<CONFIG:Release>:LINKER:-dead_strip>)
        endif()
    endif()
endfunction()

# Mixed libraries keep their speed policy. Only these translation units receive
# size flags, which CMake appends after target options and excludes from unity.
# They cannot reuse a PCH compiled with the target's different optimization flags.
function(snow_apply_release_size_sources target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "snow_apply_release_size_sources target does not exist: ${target}")
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 _snow "" "" "SOURCES")
    if(_snow_UNPARSED_ARGUMENTS OR _snow_KEYWORDS_MISSING_VALUES OR NOT _snow_SOURCES)
        message(FATAL_ERROR "snow_apply_release_size_sources requires SOURCES")
    endif()
    if(NOT SNOW_APPS_ENABLE_RELEASE_SIZE_OPTIMIZATION OR
       (DEFINED SNOW_APPS_ENABLE_RELEASE_OPTIMIZATION AND
        NOT SNOW_APPS_ENABLE_RELEASE_OPTIMIZATION))
        return()
    endif()
    get_target_property(_sources "${target}" SOURCES)
    foreach(_source IN LISTS _snow_SOURCES)
        if(NOT _source IN_LIST _sources)
            message(FATAL_ERROR "Size-optimized source is absent from ${target}: ${_source}")
        endif()
    endforeach()
    _snow_release_compile_options(size _compile_options)
    set_property(SOURCE ${_snow_SOURCES} TARGET_DIRECTORY "${target}"
        APPEND PROPERTY COMPILE_OPTIONS ${_compile_options})
    set_property(SOURCE ${_snow_SOURCES} TARGET_DIRECTORY "${target}"
        PROPERTY SKIP_PRECOMPILE_HEADERS ON)
    set_property(TARGET "${target}" APPEND PROPERTY SNOW_RELEASE_SIZE_SOURCES ${_snow_SOURCES})
endfunction()

function(snow_enable_strict_warnings)
    if(MSVC)
        add_compile_options(
            /FS /W4 /WX /permissive- /sdl /utf-8
            /Zc:__cplusplus /Zc:inline
            /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296
            /w14311 /w14545 /w14546 /w14547 /w14549 /w14555 /w14619
            /w14640 /w14826 /w14905 /w14906 /w14928 /wd4702
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            add_compile_options(
                -Wcast-align -Wcast-qual -Wconversion -Wdouble-promotion
                -Wformat=2 -Wimplicit-fallthrough -Wmissing-declarations
                -Wnon-virtual-dtor -Wnull-dereference -Wold-style-cast
                -Woverloaded-virtual -Wshadow -Wsign-conversion -Wundef
            )
        else()
            add_compile_options(/Zc:preprocessor)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(
            -Wall -Wextra -Wpedantic -Werror
            -Wcast-align -Wcast-qual -Wconversion -Wdouble-promotion
            -Wformat=2 -Wimplicit-fallthrough -Wmissing-declarations
            -Wnon-virtual-dtor -Wnull-dereference -Wold-style-cast
            -Woverloaded-virtual -Wshadow -Wsign-conversion -Wundef
        )
    else()
        message(WARNING "Strict warning flags are not defined for ${CMAKE_CXX_COMPILER_ID}.")
    endif()
endfunction()

function(snow_enable_clang_tidy)
    if(NOT SNOW_ENABLE_CLANG_TIDY)
        return()
    endif()

    find_program(SNOW_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set(_command
        "${SNOW_CLANG_TIDY_EXECUTABLE}"
        "--config-file=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.clang-tidy"
    )
    if(MSVC)
        list(APPEND _command "--extra-arg=/EHsc")
    endif()
    set(CMAKE_CXX_CLANG_TIDY "${_command}" PARENT_SCOPE)
endfunction()
