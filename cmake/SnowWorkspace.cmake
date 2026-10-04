include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/SnowRustLibclang.cmake")

function(snow_workspace_configure_paths)
    set(_vcpkg_default "${CMAKE_CURRENT_SOURCE_DIR}/.tools/vcpkg")
    if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
        set(_vcpkg_default "$ENV{VCPKG_ROOT}")
    endif()

    set(SNOW_VCPKG_ROOT "${_vcpkg_default}" CACHE PATH
        "Repository-local vcpkg root used by the active build.")
    set(SNOW_ANT_DESIGN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ant_design_qt" CACHE PATH
        "Ant Design Qt source directory.")
    set(SNOW_IMAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/snow_image" CACHE PATH
        "Snow Image source directory.")
    set(SNOW_DRAW_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/snow_draw_engine_qt" CACHE PATH
        "Snow Draw Engine Qt source directory.")
    set(SNOW_CAPTURE_CRATES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/snow-crates" CACHE PATH
        "Snow crates workspace directory.")
    snow_default_rust_libclang_directory(_snow_libclang_default)
    set(SNOW_LIBCLANG_BIN_DIR "${_snow_libclang_default}" CACHE PATH
        "Directory containing libclang.dll for the host Rust bindgen process.")

    if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
        if(APPLE)
            if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR
               (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"))
                set(_snow_default_triplet "x64-osx-snow-shot")
            else()
                set(_snow_default_triplet "arm64-osx-snow-shot")
            endif()
        elseif(WIN32)
            include(SnowWindowsArchitecture)
            snow_configure_windows_architecture()
            set(_snow_default_triplet "${SNOW_WINDOWS_ARCHITECTURE}-windows")
        else()
            set(_snow_default_triplet "x64-windows")
        endif()
        set(VCPKG_TARGET_TRIPLET "${_snow_default_triplet}" CACHE STRING
            "vcpkg target triplet used by this build." FORCE)
    endif()
    set(_vcpkg_installed_default "${SNOW_VCPKG_ROOT}/installed")
    if(DEFINED VCPKG_INSTALLED_DIR AND NOT VCPKG_INSTALLED_DIR STREQUAL "")
        set(_vcpkg_installed_default "${VCPKG_INSTALLED_DIR}")
    endif()
    set(SNOW_VCPKG_INSTALLED_DIR "${_vcpkg_installed_default}" CACHE PATH
        "vcpkg installed tree used by the active build preset.")
    set(SNOW_FFMPEG_ROOT "${SNOW_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}" CACHE PATH
        "vcpkg installation prefix containing FFmpeg.")

    foreach(_required_dir IN ITEMS
        SNOW_ANT_DESIGN_SOURCE_DIR
        SNOW_IMAGE_SOURCE_DIR
        SNOW_DRAW_ENGINE_SOURCE_DIR
        SNOW_CAPTURE_CRATES_DIR
    )
        if(NOT IS_DIRECTORY "${${_required_dir}}")
            message(FATAL_ERROR
                "${_required_dir} does not exist: ${${_required_dir}}. "
                "Run scripts/bootstrap.ps1 or set the path explicitly.")
        endif()
    endforeach()

    if(NOT EXISTS "${SNOW_CAPTURE_CRATES_DIR}/Cargo.toml")
        message(FATAL_ERROR "Snow crates workspace is missing Cargo.toml: ${SNOW_CAPTURE_CRATES_DIR}")
    endif()
endfunction()

function(snow_workspace_configure_options)
    set(SNOW_ENABLE_CLANG_TIDY ${SNOW_APPS_ENABLE_CLANG_TIDY} CACHE BOOL
        "Run clang-tidy for enabled Snow projects." FORCE)

    set(ADQT_BUILD_TESTS ${SNOW_APPS_BUILD_TESTS} CACHE BOOL
        "Build Ant Design Qt tests." FORCE)
    set(ADQT_BUILD_BENCHMARKS ${SNOW_APPS_BUILD_BENCHMARKS} CACHE BOOL
        "Build Ant Design Qt benchmarks." FORCE)
    set(ADQT_STRICT_COMPILE ${SNOW_APPS_STRICT_COMPILE} CACHE BOOL
        "Strict Ant Design Qt compilation." FORCE)

    set(SNOW_IMAGE_STRICT_COMPILE ${SNOW_APPS_STRICT_COMPILE} CACHE BOOL
        "Strict Snow Image compilation." FORCE)
    set(SNOW_IMAGE_DEFAULT_LINKAGE static CACHE STRING
        "Default Snow Image target linkage." FORCE)

    # Static Qt kits can carry bundled image codecs. Avoid linking second
    # archives with the same symbols; production kits use the system codecs.
    # Read the resolved kit's imported targets rather than its generated files.
    set(SNOW_IMAGE_LINK_PNG_DEPENDENCIES ON CACHE BOOL
        "Link the PNG dependency into the static Snow Image target." FORCE)
    set(SNOW_IMAGE_LINK_JPEG_DEPENDENCIES ON CACHE BOOL
        "Link JPEG dependencies into the static Snow Image target." FORCE)
    if(TARGET Qt6::Gui)
        get_target_property(_snow_workspace_qt_gui_type Qt6::Gui TYPE)
        get_target_property(_snow_workspace_qt_gui_features Qt6::Gui QT_ENABLED_PRIVATE_FEATURES)
        if(_snow_workspace_qt_gui_type STREQUAL "STATIC_LIBRARY")
            if(NOT "system_png" IN_LIST _snow_workspace_qt_gui_features)
                set(SNOW_IMAGE_LINK_PNG_DEPENDENCIES OFF CACHE BOOL
                    "Link the PNG dependency into the static Snow Image target." FORCE)
            endif()
            if(NOT "system_jpeg" IN_LIST _snow_workspace_qt_gui_features)
                set(SNOW_IMAGE_LINK_JPEG_DEPENDENCIES OFF CACHE BOOL
                    "Link JPEG dependencies into the static Snow Image target." FORCE)
            endif()
        endif()
    endif()

    # Static Qt can use either CRT linkage. Its static_runtime feature is the
    # authority, independent of the kit's directory name or library linkage.
    if(MSVC AND TARGET Qt6::Core)
        get_target_property(_snow_workspace_qt_core_features Qt6::Core QT_ENABLED_PUBLIC_FEATURES)
        if("static_runtime" IN_LIST _snow_workspace_qt_core_features)
            set(_snow_workspace_crt "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        else()
            set(_snow_workspace_crt "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "${_snow_workspace_crt}" CACHE STRING
            "MSVC runtime matching the resolved Qt kit." FORCE)
    endif()

    if(SNOW_APPS_RELEASE_STATIC AND MSVC)
        if(TARGET Qt6::Core AND NOT "static_runtime" IN_LIST _snow_workspace_qt_core_features)
            message(FATAL_ERROR
                "SNOW_APPS_RELEASE_STATIC requires a Qt kit built with the static MSVC runtime")
        endif()
        set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded CACHE STRING
            "MSVC runtime used for static release builds." FORCE)
    endif()

    if(SNOW_APPS_RELEASE_STATIC AND VCPKG_TARGET_TRIPLET MATCHES "^(x64|arm64)-windows$")
        message(WARNING
            "SNOW_APPS_RELEASE_STATIC is enabled with ${VCPKG_TARGET_TRIPLET}. "
            "Use the matching Snow Shot release preset for static vcpkg libraries.")
    endif()

    if(SNOW_APPS_API_BASE_URL STREQUAL "")
        if(CMAKE_CONFIGURATION_TYPES)
            set(SNOW_APPS_API_BASE_URL "https://snowshot.top" CACHE STRING
                "Default Snow Shot API base URL. Set explicitly for reproducible builds." FORCE)
        elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(SNOW_APPS_API_BASE_URL "http://localhost:8080" CACHE STRING
                "Default Snow Shot API base URL." FORCE)
        else()
            set(SNOW_APPS_API_BASE_URL "https://snowshot.top" CACHE STRING
                "Default Snow Shot API base URL." FORCE)
        endif()
    endif()
endfunction()
