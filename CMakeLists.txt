cmake_minimum_required(VERSION 4.2)

project(snow_apps VERSION 0.2.0 LANGUAGES CXX)

set(SNOW_SHOT_VERSION "${PROJECT_VERSION}")
set(SNOW_SHOT_PRODUCT_NAME "Snow Shot")
set(SNOW_SHOT_EXECUTABLE_NAME "snow_shot")
set(SNOW_SHOT_VENDOR "Snow Apps")
set(SNOW_SHOT_DESCRIPTION
    "A Windows screenshot utility for capturing, annotating, pinning, and recognizing screen content.")
set(SNOW_SHOT_HOMEPAGE_URL "https://snowshot.top")
set(SNOW_SHOT_SUPPORT_EMAIL "chao@mgchao.top")
set(SNOW_SHOT_COPYRIGHT "Copyright (C) 2025-2026 mg-chao")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/MP)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectWarnings)
include(SnowWorkspace)
include(RustStaticLibrary)

option(SNOW_APPS_BUILD_ANT_DESIGN_QT "Build the Ant Design Qt libraries." ON)
option(SNOW_APPS_BUILD_SNOW_IMAGE "Build the Snow Image library." ON)
option(SNOW_APPS_BUILD_DRAW_ENGINE "Build the Snow Draw Engine Qt library." ON)
option(SNOW_APPS_BUILD_IMAGE_VIEWER "Build Snow Image Viewer." ON)
option(SNOW_APPS_BUILD_SNOW_SHOT "Build Snow Shot." ON)
option(SNOW_APPS_BUILD_TESTS "Build tests for all enabled projects." ON)
option(SNOW_APPS_BUILD_BENCHMARKS "Build opt-in benchmark targets." OFF)
option(SNOW_APPS_STRICT_COMPILE "Enable strict warnings for all projects." ON)
option(SNOW_APPS_ENABLE_CLANG_TIDY "Run clang-tidy while compiling C++ targets." OFF)
option(SNOW_APPS_RELEASE_STATIC "Prefer static libraries and the static MSVC runtime." OFF)
option(SNOW_APPS_QT_STATIC "Link the release executable against a static Qt build." OFF)
option(SNOW_APPS_PACKAGE_SNOW_SHOT "Configure the Snow Shot NSIS package." OFF)
option(
    SNOW_APPS_ENABLE_RELEASE_OPTIMIZATION
    "Enable IPO/LTO and size/dead-code linker optimizations for Release targets."
    ON
)

set(SNOW_QT_STATIC_DIR "" CACHE PATH
    "Qt 6 CMake package directory for the static release kit.")
if(SNOW_APPS_QT_STATIC AND SNOW_QT_STATIC_DIR STREQUAL "" AND
   DEFINED ENV{SNOW_QT_STATIC_DIR} AND NOT "$ENV{SNOW_QT_STATIC_DIR}" STREQUAL "")
    set(SNOW_QT_STATIC_DIR "$ENV{SNOW_QT_STATIC_DIR}" CACHE PATH
        "Qt 6 CMake package directory for the static release kit." FORCE)
endif()
if(SNOW_APPS_QT_STATIC AND NOT SNOW_QT_STATIC_DIR STREQUAL "")
    set(Qt6_DIR "${SNOW_QT_STATIC_DIR}" CACHE PATH
        "Qt 6 CMake package directory." FORCE)
endif()

set(SNOW_APPS_API_BASE_URL "" CACHE STRING
    "Default Snow Shot API base URL. Set explicitly for reproducible builds.")
set(SNOW_RUST_CARGO_TARGET_DIR
    "${CMAKE_BINARY_DIR}/cargo"
    CACHE PATH
    "Cargo target directory for the active CMake build tree.")
set(SNOW_RUST_RELEASE_PROFILE "release" CACHE STRING
    "Cargo profile used for non-Debug Rust builds.")

snow_workspace_configure_paths()
snow_workspace_configure_options()

if(SNOW_APPS_ENABLE_RELEASE_OPTIMIZATION)
    # CMake's IPO abstraction selects /GL + /LTCG on MSVC and -flto on GCC/Clang.
    # Applying it at directory scope also covers the C++ libraries that feed the
    # Snow Shot executable, instead of optimizing only the final executable.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
endif()

# Resolve the Snow Image codec profile before any child project can configure
# the shared snow_image target. Child-level cache writes would otherwise occur
# after target creation and leave the cache claiming codecs that were never
# compiled into the library.
if(NOT DEFINED SNOW_IMAGE_PROFILE OR SNOW_IMAGE_PROFILE STREQUAL "")
    set(SNOW_IMAGE_PROFILE "full" CACHE STRING
        "Snow Image codec profile used by the workspace." FORCE)
endif()
set(_snow_workspace_all_image_codecs
    PNG JPEG GIF ICON XBITMAP SVG HEIF JXL EXR WEBP
)
if(SNOW_IMAGE_PROFILE STREQUAL "full")
    set(_snow_workspace_image_codecs
        ${_snow_workspace_all_image_codecs}
    )
elseif(SNOW_APPS_BUILD_SNOW_SHOT)
    # Snow Shot's Save As workflow supports all five raster formats in every
    # application profile, so keep the corresponding codecs in the minimal
    # workspace configuration as well as the full profile.
    set(_snow_workspace_image_codecs PNG JPEG HEIF JXL WEBP)
else()
    set(_snow_workspace_image_codecs)
endif()
foreach(_snow_image_codec IN LISTS _snow_workspace_all_image_codecs)
    list(FIND _snow_workspace_image_codecs "${_snow_image_codec}" _snow_image_codec_index)
    if(_snow_image_codec_index GREATER -1)
        set(_snow_image_codec_value ON)
    else()
        set(_snow_image_codec_value OFF)
    endif()
    set(SNOW_IMAGE_CODEC_${_snow_image_codec} ${_snow_image_codec_value} CACHE BOOL
        "Enable the ${_snow_image_codec} Snow Image codec for this workspace profile." FORCE)
endforeach()
unset(_snow_image_codec)
unset(_snow_image_codec_index)
unset(_snow_image_codec_value)
unset(_snow_workspace_all_image_codecs)
unset(_snow_workspace_image_codecs)

if(SNOW_APPS_BUILD_TESTS)
    enable_testing()
endif()

if(SNOW_APPS_BUILD_SNOW_SHOT)
    snow_add_rust_static_library(snow_shot_rust_ffi_bundle
        PACKAGE snow-shot-rust-ffi
        MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/snow_rust_ffi"
        OUTPUT_NAME snow_shot_rust_ffi
        STRIP_MSVC_DIRECTIVES
    )
    foreach(_snow_rust_ffi_target IN ITEMS
            snow_capture_c
            snow_ui_selector_c
            snow_stitch_images_c
            snow_ocr_c
            snow_draw_engine_c)
        add_library("${_snow_rust_ffi_target}" INTERFACE)
        target_link_libraries("${_snow_rust_ffi_target}" INTERFACE
            snow_shot_rust_ffi_bundle)
    endforeach()
    unset(_snow_rust_ffi_target)
endif()

if(SNOW_APPS_BUILD_IMAGE_VIEWER AND NOT SNOW_APPS_BUILD_SNOW_IMAGE)
    message(FATAL_ERROR "SNOW_APPS_BUILD_IMAGE_VIEWER requires SNOW_APPS_BUILD_SNOW_IMAGE=ON")
endif()
if(SNOW_APPS_BUILD_SNOW_SHOT AND
   (NOT SNOW_APPS_BUILD_ANT_DESIGN_QT OR
    NOT SNOW_APPS_BUILD_SNOW_IMAGE OR
    NOT SNOW_APPS_BUILD_DRAW_ENGINE))
    message(FATAL_ERROR
        "SNOW_APPS_BUILD_SNOW_SHOT requires Ant Design Qt, Snow Image, and the Draw Engine")
endif()

if(SNOW_APPS_BUILD_ANT_DESIGN_QT)
    add_subdirectory(ant_design_qt "${CMAKE_BINARY_DIR}/ant_design_qt")
endif()

if(SNOW_APPS_BUILD_SNOW_IMAGE)
    set(SNOW_IMAGE_BUILD_TESTS ${SNOW_APPS_BUILD_TESTS} CACHE BOOL
        "Build Snow Image tests." FORCE)
    if(SNOW_APPS_RELEASE_STATIC)
        set(SNOW_IMAGE_BUILD_SHARED OFF CACHE BOOL
            "Build the shared Snow Image library." FORCE)
    else()
        set(SNOW_IMAGE_BUILD_SHARED ON CACHE BOOL
            "Build the shared Snow Image library." FORCE)
    endif()
    set(SNOW_IMAGE_BUILD_STATIC ON CACHE BOOL
        "Build the static Snow Image library." FORCE)
    set(SNOW_IMAGE_BUILD_BENCHMARKS ${SNOW_APPS_BUILD_BENCHMARKS} CACHE BOOL
        "Build Snow Image benchmarks." FORCE)
    set(SNOW_IMAGE_ENABLE_INSTALL OFF CACHE BOOL
        "Install Snow Image as a standalone package." FORCE)
    add_subdirectory(snow_image "${CMAKE_BINARY_DIR}/snow_image")
endif()

if(SNOW_APPS_BUILD_DRAW_ENGINE)
    set(SNOW_DRAW_ENGINE_QT_BUILD_DEMO OFF CACHE BOOL
        "Build the Snow Draw Engine Qt demo." FORCE)
    set(SNOW_DRAW_ENGINE_QT_BUILD_TESTS ${SNOW_APPS_BUILD_TESTS} CACHE BOOL
        "Build Snow Draw Engine Qt tests." FORCE)
    set(SNOW_DRAW_ENGINE_QT_BUILD_BENCHMARKS ${SNOW_APPS_BUILD_BENCHMARKS} CACHE BOOL
        "Build Snow Draw Engine Qt benchmarks." FORCE)
    set(SNOW_DRAW_ENGINE_QT_STRICT_COMPILE ${SNOW_APPS_STRICT_COMPILE} CACHE BOOL
        "Strict Snow Draw Engine Qt compilation." FORCE)
    add_subdirectory(snow_draw_engine_qt "${CMAKE_BINARY_DIR}/snow_draw_engine_qt")
endif()

if(SNOW_APPS_BUILD_IMAGE_VIEWER)
    set(SNOW_IMAGE_VIEWER_BUILD_TESTS ${SNOW_APPS_BUILD_TESTS} CACHE BOOL
        "Build Snow Image Viewer tests." FORCE)
    set(SNOW_IMAGE_VIEWER_STRICT_COMPILE ${SNOW_APPS_STRICT_COMPILE} CACHE BOOL
        "Strict Snow Image Viewer compilation." FORCE)
    set(SNOW_IMAGE_VIEWER_VCPKG_ROOT "${SNOW_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        CACHE PATH "Root-managed vcpkg installation used for runtime staging." FORCE)
    add_subdirectory(snow_image_viewer "${CMAKE_BINARY_DIR}/snow_image_viewer")
endif()

if(SNOW_APPS_BUILD_SNOW_SHOT)
    set(SNOW_SHOT_BUILD_TESTS ${SNOW_APPS_BUILD_TESTS} CACHE BOOL
        "Build Snow Shot tests." FORCE)
    set(SNOW_SHOT_BUILD_BENCHMARKS ${SNOW_APPS_BUILD_BENCHMARKS} CACHE BOOL
        "Build Snow Shot benchmarks." FORCE)
    set(SNOW_SHOT_ENABLE_STRICT_COMPILE ${SNOW_APPS_STRICT_COMPILE} CACHE BOOL
        "Strict Snow Shot compilation." FORCE)
    set(SNOW_SHOT_RELEASE_STATIC ${SNOW_APPS_RELEASE_STATIC} CACHE BOOL
        "Prefer static Snow Shot release linkage." FORCE)
    set(SNOW_SHOT_QT_STATIC ${SNOW_APPS_QT_STATIC} CACHE BOOL
        "Link Snow Shot against a static Qt build." FORCE)
    set(SNOW_SHOT_API_BASE_URL "${SNOW_APPS_API_BASE_URL}" CACHE STRING
        "Snow Shot API base URL." FORCE)
    set(SNOW_SHOT_LIBCLANG_DIR "${SNOW_LIBCLANG_BIN_DIR}" CACHE PATH
        "Directory containing libclang.dll for Rust bindgen." FORCE)
    set(SNOW_SHOT_FFMPEG_BIN_DIR "${SNOW_FFMPEG_ROOT}/bin" CACHE PATH
        "Directory containing FFmpeg runtime DLLs." FORCE)
    set(SNOW_DRAW_ENGINE_QT_BUILD_DEMO OFF CACHE BOOL
        "Build the nested draw engine demo." FORCE)
    set(SNOW_DRAW_ENGINE_QT_BUILD_TESTS ${SNOW_APPS_BUILD_TESTS} CACHE BOOL
        "Build nested draw engine tests." FORCE)
    set(SNOW_DRAW_ENGINE_QT_BUILD_BENCHMARKS ${SNOW_APPS_BUILD_BENCHMARKS} CACHE BOOL
        "Build nested draw engine benchmarks." FORCE)
    add_subdirectory(snow_shot "${CMAKE_BINARY_DIR}/snow_shot")
endif()

function(_snow_apply_test_labels directory)
    get_property(_directory_tests DIRECTORY "${directory}" PROPERTY TESTS)
    foreach(_snow_test IN LISTS _directory_tests)
        unset(_snow_test_labels)
        get_test_property(_snow_test_labels "${_snow_test}" LABELS)
        if(NOT _snow_test_labels OR
           _snow_test_labels STREQUAL "NOTFOUND" OR
           _snow_test_labels MATCHES "-NOTFOUND$")
            set(_snow_test_labels unit)
        endif()
        if(_snow_test MATCHES "real-mouse|uia-e2e")
            list(APPEND _snow_test_labels interactive e2e)
        elseif(_snow_test MATCHES "benchmark|performance")
            list(APPEND _snow_test_labels benchmark)
        elseif(_snow_test MATCHES "pinned-window|context-menu|toolbar|tooltip|recognition-window")
            list(APPEND _snow_test_labels windows)
        endif()
        list(REMOVE_DUPLICATES _snow_test_labels)
        set_tests_properties("${_snow_test}" DIRECTORY "${directory}"
            PROPERTIES LABELS "${_snow_test_labels}")
    endforeach()

    get_property(_child_directories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_child_directory IN LISTS _child_directories)
        _snow_apply_test_labels("${_child_directory}")
    endforeach()
endfunction()

if(SNOW_APPS_BUILD_TESTS)
    _snow_apply_test_labels("${CMAKE_CURRENT_BINARY_DIR}")
endif()

function(_snow_collect_build_targets directory output_variable)
    get_property(_directory_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_child_directories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    set(_collected_targets ${_directory_targets})
    foreach(_child_directory IN LISTS _child_directories)
        _snow_collect_build_targets("${_child_directory}" _child_targets)
        list(APPEND _collected_targets ${_child_targets})
    endforeach()
    list(REMOVE_DUPLICATES _collected_targets)
    set(${output_variable} "${_collected_targets}" PARENT_SCOPE)
endfunction()

if(NOT TARGET snow-all)
    add_custom_target(snow-all)
    _snow_collect_build_targets("${CMAKE_CURRENT_BINARY_DIR}" _snow_workspace_targets)
    foreach(_snow_target IN LISTS _snow_workspace_targets)
        if(TARGET ${_snow_target})
            get_target_property(_snow_target_type "${_snow_target}" TYPE)
        endif()
        if(_snow_target_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            add_dependencies(snow-all ${_snow_target})
        endif()
        unset(_snow_target_type)
    endforeach()
    unset(_snow_workspace_targets)
endif()

if(NOT TARGET snow-tests)
    set(_snow_deterministic_test_exclude "interactive^|e2e^|benchmark")
    add_custom_target(snow-tests
        COMMAND ${CMAKE_CTEST_COMMAND}
            --test-dir "${CMAKE_BINARY_DIR}"
            -C "$<CONFIG>"
            -LE ${_snow_deterministic_test_exclude}
            --output-on-failure
        USES_TERMINAL
    )
    add_dependencies(snow-tests snow-all)
    unset(_snow_deterministic_test_exclude)
endif()

if(NOT TARGET snow-benchmarks)
    add_custom_target(snow-benchmarks)
    foreach(_snow_benchmark IN ITEMS
        snow-shot-history-performance-benchmark
        snow-shot-selection-render-benchmark
        snow-shot-screenshot-toolbar-render-benchmark
        snow-shot-scrolling-preview-benchmark
        snow-shot-scrolling-result-async-benchmark
    )
        if(TARGET ${_snow_benchmark})
            add_dependencies(snow-benchmarks ${_snow_benchmark})
        endif()
    endforeach()
endif()

if(NOT TARGET snow-format)
    add_custom_target(snow-format
        COMMAND "${CMAKE_COMMAND}" -E env
            "SNOW_WORKSPACE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-cpp-format.ps1" -Check
        USES_TERMINAL
    )
endif()

if(NOT TARGET snow-lint)
    add_custom_target(snow-lint
        COMMAND "${CMAKE_COMMAND}" -E env
            "SNOW_WORKSPACE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-rust.ps1"
        USES_TERMINAL
    )
endif()

if(SNOW_APPS_PACKAGE_SNOW_SHOT)
    if(NOT TARGET snow_shot)
        message(FATAL_ERROR "SNOW_APPS_PACKAGE_SNOW_SHOT requires SNOW_APPS_BUILD_SNOW_SHOT=ON")
    endif()
    set(CPACK_PACKAGE_NAME "snow-shot")
    set(CPACK_PACKAGE_VENDOR "${SNOW_SHOT_VENDOR}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${SNOW_SHOT_PRODUCT_NAME} screenshot utility")
    set(CPACK_PACKAGE_DESCRIPTION "${SNOW_SHOT_DESCRIPTION}")
    set(CPACK_PACKAGE_HOMEPAGE_URL "${SNOW_SHOT_HOMEPAGE_URL}")
    set(CPACK_PACKAGE_VERSION "${SNOW_SHOT_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "SnowShot")
    set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "SnowShot")
    set(CPACK_PACKAGE_FILE_NAME "snow-shot-${SNOW_SHOT_VERSION}-windows-x64")
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CPACK_PACKAGE_CHECKSUM "SHA256")
    # LZMA gives the smallest installer while solid compression keeps repeated
    # Qt/runtime data from being stored more than once.
    set(CPACK_NSIS_COMPRESSOR "lzma")
    set(CPACK_NSIS_COMPRESSOR_SOLID ON)
    set(CPACK_PACKAGE_DESCRIPTION_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/snow_shot/packaging/README.txt")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/snow_shot/LICENSE")
    set(CPACK_RESOURCE_FILE_README
        "${CMAKE_CURRENT_SOURCE_DIR}/snow_shot/packaging/README.txt")
    set(CPACK_PACKAGE_EXECUTABLES
        "${SNOW_SHOT_EXECUTABLE_NAME}" "${SNOW_SHOT_PRODUCT_NAME}")
    set(CPACK_CREATE_DESKTOP_LINKS "${SNOW_SHOT_EXECUTABLE_NAME}")
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "${SNOW_SHOT_PRODUCT_NAME}")
    set(CPACK_NSIS_PACKAGE_NAME "${SNOW_SHOT_PRODUCT_NAME}")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY "bin")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\${SNOW_SHOT_EXECUTABLE_NAME}.exe")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/snow_shot/resources/app-icon.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/snow_shot/resources/app-icon.ico")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "${SNOW_SHOT_EXECUTABLE_NAME}.exe")
    set(CPACK_NSIS_UNINSTALL_NAME "Uninstall ${SNOW_SHOT_PRODUCT_NAME}")
    set(CPACK_NSIS_WELCOME_TITLE "Welcome to ${SNOW_SHOT_PRODUCT_NAME} Setup")
    set(CPACK_NSIS_FINISH_TITLE "${SNOW_SHOT_PRODUCT_NAME} installation complete")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
    string(CONCAT CPACK_NSIS_DEFINES
        "VIProductVersion \"${SNOW_SHOT_VERSION}.0\"\n"
        "VIAddVersionKey /LANG=1033 \"CompanyName\" \"${SNOW_SHOT_VENDOR}\"\n"
        "VIAddVersionKey /LANG=1033 \"FileDescription\" \"${SNOW_SHOT_PRODUCT_NAME} installer\"\n"
        "VIAddVersionKey /LANG=1033 \"FileVersion\" \"${SNOW_SHOT_VERSION}.0\"\n"
        "VIAddVersionKey /LANG=1033 \"InternalName\" \"snow-shot-installer\"\n"
        "VIAddVersionKey /LANG=1033 \"LegalCopyright\" \"${SNOW_SHOT_COPYRIGHT}\"\n"
        "VIAddVersionKey /LANG=1033 \"OriginalFilename\" \"${CPACK_PACKAGE_FILE_NAME}.exe\"\n"
        "VIAddVersionKey /LANG=1033 \"ProductName\" \"${SNOW_SHOT_PRODUCT_NAME}\"\n"
        "VIAddVersionKey /LANG=1033 \"ProductVersion\" \"${SNOW_SHOT_VERSION}\""
    )
    set(CPACK_NSIS_BRANDING_TEXT "${SNOW_SHOT_PRODUCT_NAME} ${SNOW_SHOT_VERSION}")
    set(CPACK_NSIS_BRANDING_TEXT_TRIM_POSITION "CENTER")
    set(CPACK_NSIS_HELP_LINK "${SNOW_SHOT_HOMEPAGE_URL}")
    set(CPACK_NSIS_URL_INFO_ABOUT "${SNOW_SHOT_HOMEPAGE_URL}")
    set(CPACK_NSIS_CONTACT "${SNOW_SHOT_SUPPORT_EMAIL}")
    set(CPACK_VERBATIM_VARIABLES ON)
    include(CPack)
endif()
