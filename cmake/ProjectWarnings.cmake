include_guard(GLOBAL)

option(SNOW_ENABLE_CLANG_TIDY "Run clang-tidy while compiling C++ targets." OFF)

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
            /Zc:__cplusplus /Zc:preprocessor /Zc:inline
            /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296
            /w14311 /w14545 /w14546 /w14547 /w14549 /w14555 /w14619
            /w14640 /w14826 /w14905 /w14906 /w14928 /wd4702
        )
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

function(snow_apply_release_options target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "snow_apply_release_options target does not exist: ${target}")
    endif()

    if(DEFINED SNOW_APPS_ENABLE_RELEASE_OPTIMIZATION AND
       NOT SNOW_APPS_ENABLE_RELEASE_OPTIMIZATION)
        return()
    endif()

    set_property(TARGET "${target}" PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    if(MSVC)
        target_compile_options("${target}" PRIVATE
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/Ot>
            $<$<CONFIG:Release>:/Oy>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/Gw>
            $<$<CONFIG:Release>:/Gy>
        )
        target_link_options("${target}" PRIVATE
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/CGTHREADS:1>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options("${target}" PRIVATE
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-flto>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
        )
        target_link_options("${target}" PRIVATE $<$<CONFIG:Release>:-flto>)
    endif()
endfunction()

function(snow_enable_strict_warnings)
    if(MSVC)
        add_compile_options(
            /FS /W4 /WX /permissive- /sdl /utf-8
            /Zc:__cplusplus /Zc:preprocessor /Zc:inline
            /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296
            /w14311 /w14545 /w14546 /w14547 /w14549 /w14555 /w14619
            /w14640 /w14826 /w14905 /w14906 /w14928 /wd4702
        )
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
