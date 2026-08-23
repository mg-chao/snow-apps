include_guard(GLOBAL)

function(snow_add_rust_static_libraries batch_name)
    set(options STRIP_MSVC_DIRECTIVES)
    set(oneValueArgs MANIFEST_DIR)
    set(multiValueArgs TARGETS PACKAGES OUTPUT_NAMES)
    cmake_parse_arguments(SNOW_RUST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SNOW_RUST_MANIFEST_DIR)
        message(FATAL_ERROR "snow_add_rust_static_libraries requires MANIFEST_DIR")
    endif()
    foreach(_list_name IN ITEMS TARGETS PACKAGES OUTPUT_NAMES)
        list(LENGTH SNOW_RUST_${_list_name} _list_length)
        if(_list_length EQUAL 0)
            message(FATAL_ERROR
                "snow_add_rust_static_libraries requires a non-empty ${_list_name} list")
        endif()
        if(DEFINED _rust_library_count AND NOT _list_length EQUAL _rust_library_count)
            message(FATAL_ERROR
                "TARGETS, PACKAGES, and OUTPUT_NAMES must have the same length")
        endif()
        set(_rust_library_count ${_list_length})
    endforeach()

    find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)
    if(NOT DEFINED SNOW_RUST_CARGO_TARGET_DIR OR SNOW_RUST_CARGO_TARGET_DIR STREQUAL "")
        set(SNOW_RUST_CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo" CACHE PATH
            "Cargo target directory for the active CMake build tree.")
    endif()
    if(NOT DEFINED SNOW_RUST_RELEASE_PROFILE OR SNOW_RUST_RELEASE_PROFILE STREQUAL "")
        set(SNOW_RUST_RELEASE_PROFILE "release" CACHE STRING
            "Cargo profile used for non-Debug Rust builds.")
    endif()
    if(NOT DEFINED SNOW_VCPKG_ROOT OR SNOW_VCPKG_ROOT STREQUAL "")
        set(SNOW_VCPKG_ROOT "$ENV{VCPKG_ROOT}")
    endif()
    if(NOT DEFINED SNOW_FFMPEG_ROOT OR SNOW_FFMPEG_ROOT STREQUAL "")
        if(DEFINED VCPKG_INSTALLED_DIR AND NOT VCPKG_INSTALLED_DIR STREQUAL "")
            set(SNOW_FFMPEG_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        else()
            set(SNOW_FFMPEG_ROOT "${SNOW_VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}")
        endif()
    endif()
    if(NOT DEFINED SNOW_LIBCLANG_BIN_DIR OR SNOW_LIBCLANG_BIN_DIR STREQUAL "")
        set(SNOW_LIBCLANG_BIN_DIR "${SNOW_VCPKG_ROOT}/../llvm/bin")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|MinGW")
        set(SNOW_RUST_TARGET "x86_64-pc-windows-gnu")
    elseif(MSVC)
        set(SNOW_RUST_TARGET "x86_64-pc-windows-msvc")
    else()
        set(SNOW_RUST_TARGET "x86_64-unknown-linux-gnu")
    endif()

    execute_process(
        COMMAND "${CARGO_EXECUTABLE}" metadata --format-version 1
            --filter-platform "${SNOW_RUST_TARGET}" --no-deps
        WORKING_DIRECTORY "${SNOW_RUST_MANIFEST_DIR}"
        RESULT_VARIABLE _metadata_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT _metadata_result EQUAL 0)
        message(FATAL_ERROR
            "Rust target '${SNOW_RUST_TARGET}' is not installed or usable. "
            "Run scripts/bootstrap.ps1.")
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        set(_profile "$<IF:$<CONFIG:Debug>,debug,${SNOW_RUST_RELEASE_PROFILE}>")
        set(_cargo_profile "$<IF:$<CONFIG:Debug>,dev,${SNOW_RUST_RELEASE_PROFILE}>")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_profile debug)
        set(_cargo_profile dev)
    else()
        set(_profile "${SNOW_RUST_RELEASE_PROFILE}")
        set(_cargo_profile "${SNOW_RUST_RELEASE_PROFILE}")
    endif()

    if(SNOW_RUST_TARGET MATCHES "msvc$")
        set(_lib_prefix "")
        set(_lib_suffix ".lib")
    else()
        set(_lib_prefix "lib")
        set(_lib_suffix ".a")
    endif()

    set(_static_libraries)
    set(_debug_libraries)
    set(_release_libraries)
    set(_cargo_package_args)
    math(EXPR _rust_library_last "${_rust_library_count} - 1")
    foreach(_index RANGE ${_rust_library_last})
        list(GET SNOW_RUST_PACKAGES ${_index} _package)
        list(GET SNOW_RUST_OUTPUT_NAMES ${_index} _output_name)
        string(REPLACE "-" "_" _output_stem "${_output_name}")
        list(APPEND _cargo_package_args --package "${_package}")
        list(APPEND _static_libraries
            "${SNOW_RUST_CARGO_TARGET_DIR}/${SNOW_RUST_TARGET}/${_profile}/${_lib_prefix}${_output_stem}${_lib_suffix}")
        list(APPEND _debug_libraries
            "${SNOW_RUST_CARGO_TARGET_DIR}/${SNOW_RUST_TARGET}/debug/${_lib_prefix}${_output_stem}${_lib_suffix}")
        list(APPEND _release_libraries
            "${SNOW_RUST_CARGO_TARGET_DIR}/${SNOW_RUST_TARGET}/${SNOW_RUST_RELEASE_PROFILE}/${_lib_prefix}${_output_stem}${_lib_suffix}")
    endforeach()

    set(_libclang_dir "")
    if(EXISTS "${SNOW_LIBCLANG_BIN_DIR}/libclang.dll")
        set(_libclang_dir "${SNOW_LIBCLANG_BIN_DIR}")
    else()
        find_file(_libclang_dll
            NAMES libclang.dll clang.dll
            HINTS
                "$ENV{LIBCLANG_PATH}"
                "$ENV{LLVMInstallDir}/bin"
                "C:/Program Files/LLVM/bin"
        )
        if(_libclang_dll)
            get_filename_component(_libclang_dir "${_libclang_dll}" DIRECTORY)
        endif()
    endif()

    set(_snow_rust_static_crt FALSE)
    if(SNOW_APPS_RELEASE_STATIC OR SNOW_SHOT_RELEASE_STATIC OR
       (MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded" AND
        NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL"))
        set(_snow_rust_static_crt TRUE)
    endif()

    set(_vcpkg_dynamic 1)
    if(SNOW_APPS_RELEASE_STATIC OR SNOW_SHOT_RELEASE_STATIC)
        set(_vcpkg_dynamic 0)
    endif()
    set(_cargo_environment
        "VCPKG_ROOT=${SNOW_VCPKG_ROOT}"
        "VCPKGRS_TRIPLET=${VCPKG_TARGET_TRIPLET}"
        "VCPKGRS_DYNAMIC=${_vcpkg_dynamic}"
        "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
        "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
    )
    if(_libclang_dir)
        list(APPEND _cargo_environment "LIBCLANG_PATH=${_libclang_dir}")
    endif()

    if(MSVC AND (CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug" OR
                 SNOW_APPS_RELEASE_STATIC OR SNOW_SHOT_RELEASE_STATIC))
        if(_snow_rust_static_crt)
            set(_rust_debug_runtime "/MTd /D_DEBUG")
            set(_rust_release_runtime "/MT")
        else()
            set(_rust_debug_runtime "/MDd /D_DEBUG")
            set(_rust_release_runtime "/MD")
        endif()
        if(CMAKE_CONFIGURATION_TYPES)
            set(_rust_cxxflags
                "$<IF:$<CONFIG:Debug>,$ENV{CXXFLAGS} ${_rust_debug_runtime},$ENV{CXXFLAGS} ${_rust_release_runtime}>")
            set(_rust_cflags
                "$<IF:$<CONFIG:Debug>,$ENV{CFLAGS} ${_rust_debug_runtime},$ENV{CFLAGS} ${_rust_release_runtime}>")
        elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(_rust_cxxflags "$ENV{CXXFLAGS} ${_rust_debug_runtime}")
            set(_rust_cflags "$ENV{CFLAGS} ${_rust_debug_runtime}")
        else()
            set(_rust_cxxflags "$ENV{CXXFLAGS} ${_rust_release_runtime}")
            set(_rust_cflags "$ENV{CFLAGS} ${_rust_release_runtime}")
        endif()
        list(INSERT _cargo_environment 0
            "CXXFLAGS=${_rust_cxxflags}"
            "CFLAGS=${_rust_cflags}"
        )
        if(_snow_rust_static_crt)
            list(INSERT _cargo_environment 0
                "RUSTFLAGS=$ENV{RUSTFLAGS} -Dwarnings -C target-feature=+crt-static")
        endif()
    endif()

    set(_cargo_commands
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_cargo_environment}
            "${CARGO_EXECUTABLE}" build --locked
            ${_cargo_package_args}
            --target "${SNOW_RUST_TARGET}"
            --profile "${_cargo_profile}"
    )
    if(SNOW_RUST_STRIP_MSVC_DIRECTIVES AND SNOW_RUST_TARGET MATCHES "gnu$")
        find_program(RUST_ARCHIVE_OBJCOPY NAMES llvm-objcopy objcopy REQUIRED)
        foreach(_static_library IN LISTS _static_libraries)
            list(APPEND _cargo_commands
                COMMAND "${RUST_ARCHIVE_OBJCOPY}" --remove-section=.drectve "${_static_library}")
        endforeach()
    endif()

    add_custom_target("${batch_name}_build"
        ${_cargo_commands}
        BYPRODUCTS ${_static_libraries}
        WORKING_DIRECTORY "${SNOW_RUST_MANIFEST_DIR}"
        USES_TERMINAL
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    foreach(_index RANGE ${_rust_library_last})
        list(GET SNOW_RUST_TARGETS ${_index} _target_name)
        list(GET _static_libraries ${_index} _static_library)
        list(GET _debug_libraries ${_index} _debug_library)
        list(GET _release_libraries ${_index} _release_library)
        add_library("${_target_name}" STATIC IMPORTED GLOBAL)
        if(CMAKE_CONFIGURATION_TYPES)
            set_target_properties("${_target_name}" PROPERTIES
                IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
                IMPORTED_LOCATION_DEBUG "${_debug_library}"
                IMPORTED_LOCATION_RELEASE "${_release_library}"
                IMPORTED_LOCATION_RELWITHDEBINFO "${_release_library}"
                IMPORTED_LOCATION_MINSIZEREL "${_release_library}"
            )
        else()
            set_target_properties("${_target_name}" PROPERTIES
                IMPORTED_LOCATION "${_static_library}")
        endif()
        add_dependencies("${_target_name}" "${batch_name}_build")
    endforeach()
endfunction()

function(snow_add_rust_static_library target_name)
    set(options STRIP_MSVC_DIRECTIVES)
    set(oneValueArgs PACKAGE MANIFEST_DIR OUTPUT_NAME)
    cmake_parse_arguments(SNOW_RUST "${options}" "${oneValueArgs}" "" ${ARGN})

    foreach(_required IN ITEMS PACKAGE MANIFEST_DIR)
        if(NOT SNOW_RUST_${_required})
            message(FATAL_ERROR "snow_add_rust_static_library requires ${_required}")
        endif()
    endforeach()
    if(NOT SNOW_RUST_OUTPUT_NAME)
        set(SNOW_RUST_OUTPUT_NAME "${SNOW_RUST_PACKAGE}")
    endif()
    set(_strip_option)
    if(SNOW_RUST_STRIP_MSVC_DIRECTIVES)
        set(_strip_option STRIP_MSVC_DIRECTIVES)
    endif()
    snow_add_rust_static_libraries("${target_name}_rust"
        MANIFEST_DIR "${SNOW_RUST_MANIFEST_DIR}"
        TARGETS "${target_name}"
        PACKAGES "${SNOW_RUST_PACKAGE}"
        OUTPUT_NAMES "${SNOW_RUST_OUTPUT_NAME}"
        ${_strip_option}
    )
endfunction()
