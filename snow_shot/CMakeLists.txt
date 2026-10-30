if(APPLE)
    set_source_files_properties(src/presentation/core/screenshotgeometry.cpp
        src/presentation/selector/screenshotselectorserviceclient.cpp
        src/presentation/recording/screenrecordingareawindow.cpp
        src/presentation/recording/recordingregioninputrouter.cpp
        src/presentation/globalcanvas/globalcanvasplatform.cpp
        tests/global_canvas_tests.cpp
        tests/screen_recording_shortcut_tests.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

cmake_minimum_required(VERSION 3.30)

project(snow_shot VERSION ${SNOW_SHOT_VERSION_NUMERIC} LANGUAGES CXX)

add_compile_definitions(SNOW_SHOT_CONFIGURED_STORE_BUILD=${_SNOW_SHOT_CONFIGURED_STORE_BUILD}
    "SNOW_SHOT_PRIVACY_POLICY_URL=\"${SNOW_SHOT_PRIVACY_POLICY_URL}\"")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(RustStaticLibrary)
include(SnowShotSourceDependencies)
snow_configure_windows_architecture()
snow_detect_rust_target(SNOW_SHOT_RUST_TARGET)

if(WIN32)
    if(SNOW_WINDOWS_ARCHITECTURE STREQUAL "arm64")
        set(_snow_ocr_manifest_default
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/snow-shot-ocr-asset-manifest-arm64.json")
    else()
        set(_snow_ocr_manifest_default
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/snow-shot-ocr-asset-manifest.json")
    endif()
    set(SNOW_SHOT_OCR_ASSET_MANIFEST "${_snow_ocr_manifest_default}" CACHE FILEPATH
        "Verified OCR asset manifest for the selected Windows architecture.")
    set(_snow_ocr_artifacts_directory "${CMAKE_CURRENT_SOURCE_DIR}/../artifacts")
    if(SNOW_WINDOWS_ARCHITECTURE STREQUAL "arm64")
        string(APPEND _snow_ocr_artifacts_directory "/windows-arm64")
    endif()
    if(EXISTS "${SNOW_SHOT_OCR_ASSET_MANIFEST}")
        file(READ "${SNOW_SHOT_OCR_ASSET_MANIFEST}" _snow_ocr_manifest)
        string(JSON _snow_ocr_manifest_platform GET "${_snow_ocr_manifest}" runtime platform)
        if(NOT _snow_ocr_manifest_platform STREQUAL SNOW_WINDOWS_PLATFORM)
            message(FATAL_ERROR
                "OCR asset manifest platform ${_snow_ocr_manifest_platform} disagrees with ${SNOW_WINDOWS_PLATFORM}")
        endif()
    else()
        message(WARNING
            "Verified ${SNOW_WINDOWS_PLATFORM} OCR assets are not prepared. Build the release OCR runtime, "
            "generate its audited asset manifest, then configure SNOW_SHOT_OCR_ASSET_MANIFEST to stage managed OCR assets.")
    endif()
endif()

set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME SnowShot)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

if(APPLE)
    set(SNOW_SELECTED_FILES_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/selectedfiles.mm)
    set(SNOW_SELECTED_FILES_LIBRARIES "-framework AppKit" "-framework ApplicationServices")
else()
    set(SNOW_SELECTED_FILES_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/selectedfiles.cpp)
endif()

set(SNOW_PINNED_PLATFORM_SOURCES)
if(NOT APPLE)
    list(APPEND SNOW_PINNED_PLATFORM_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/pinned/pinnedwindowplatform.cpp)
endif()
if(WIN32)
    list(APPEND SNOW_PINNED_PLATFORM_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/pinnedwindowevents.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/pinnedwindownative.cpp)
endif()

option(SNOW_SHOT_ENABLE_STRICT_COMPILE "Enable strict compiler warnings and treat warnings as errors." ON)
option(SNOW_SHOT_BUILD_TESTS "Build Snow Shot test executables." OFF)
option(SNOW_SHOT_BUILD_RUNTIME_TESTS
    "Build focused OCR and crash diagnostics tests without unrelated test targets." OFF)
option(SNOW_SHOT_BUILD_IMAGE_CODEC_SMOKE_TEST
    "Build the Snow Shot image-codec smoke test without the full QtTest suite." OFF)
option(SNOW_SHOT_BUILD_BENCHMARKS "Build opt-in native Snow Shot benchmarks." OFF)
option(SNOW_SHOT_ENABLE_MCP "Build and package the local Snow Shot MCP bridge." ON)
option(SNOW_SHOT_SCROLLING_PERF_DETAIL "Compile detailed scrolling C++ and Rust timings." OFF)
option(SNOW_SHOT_RELEASE_STATIC "Prefer static Snow Shot release linkage." OFF)
option(SNOW_SHOT_QT_STATIC "Link Snow Shot against a static Qt build." OFF)
set(SNOW_SHOT_WEBSITE_URL "https://snowshot.top" CACHE STRING "Public Snow Shot website URL.")
set(SNOW_SHOT_PROJECT_URL "https://github.com/mg-chao/snow-apps" CACHE STRING
    "Public Snow Shot source repository URL.")
set(SNOW_SHOT_QQ_GROUP_3_URL "https://qm.qq.com/q/Ts3cwNdykW" CACHE STRING
    "QQ group 3 join URL.")
unset(SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE CACHE)
set(
    SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/audios/camera_shutter.mp3"
)
option(
    SNOW_SHOT_ENABLE_AGGRESSIVE_RELEASE_OPTIMIZATION
    "Enable aggressive compiler and linker optimizations for Release."
    ON
)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ProjectWarnings.cmake")
if(SNOW_SHOT_ENABLE_STRICT_COMPILE)
    snow_enable_strict_warnings()
endif()
snow_enable_clang_tidy()

if(WIN32 AND (SNOW_SHOT_BUILD_TESTS OR SNOW_SHOT_DISTRIBUTION_CHANNEL STREQUAL "store"))
    add_subdirectory(src/platform/windows/explorercommand)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowQt.cmake")
snow_find_qt(COMPONENTS Widgets LinguistTools Svg Network)

add_library(snow_shot_distribution STATIC
    include/snow_shot/app/distribution.h
    src/app/distribution.cpp)
target_include_directories(snow_shot_distribution PUBLIC include)
target_link_libraries(snow_shot_distribution PUBLIC Qt6::Core)
# Every Snow Shot target follows the same channel policy. Keep native SDK headers out of
# the shared policy header so their macros cannot change unrelated callers' C++ tokens.
link_libraries(snow_shot_distribution)

add_library(snow_shot_store_lifecycle STATIC
    include/snow_shot/app/storeupdatelifecycle.h
    src/app/storeupdatelifecycle.cpp)
target_include_directories(snow_shot_store_lifecycle PUBLIC include)
target_link_libraries(snow_shot_store_lifecycle PUBLIC Qt6::Core)
if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-store-distribution-tests tests/store_distribution_tests.cpp)
    target_link_libraries(snow-shot-store-distribution-tests PRIVATE snow_shot_store_lifecycle)
    add_test(NAME snow-shot-store-distribution-tests COMMAND snow-shot-store-distribution-tests)
    set_tests_properties(snow-shot-store-distribution-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    add_executable(snow-shot-recording-recovery-tests tests/recording_recovery_tests.cpp)
    target_link_libraries(snow-shot-recording-recovery-tests PRIVATE Qt6::Core)
    add_test(NAME snow-shot-recording-recovery-tests COMMAND snow-shot-recording-recovery-tests)
    set_tests_properties(snow-shot-recording-recovery-tests PROPERTIES LABELS "unit" TIMEOUT 15)
endif()

add_library(snow_shot_canvas_status_readout STATIC
    include/snow_shot/presentation/canvasstatusreadout.h
    src/presentation/components/canvasstatusreadout.cpp)
target_include_directories(snow_shot_canvas_status_readout PUBLIC include)
target_link_libraries(snow_shot_canvas_status_readout PUBLIC Qt6::Widgets)

add_library(snow_shot_recording_effects_source STATIC
    src/presentation/recording/recordingeffectssource.cpp
    src/presentation/recording/recordingeffectssource.h)
target_link_libraries(snow_shot_recording_effects_source PUBLIC Qt6::Gui snow_recording_effects_c)
if(APPLE)
    add_library(snow_shot_macos_clipboard STATIC src/platform/macos/imageclipboard.mm)
    target_link_libraries(snow_shot_macos_clipboard PUBLIC Qt6::Gui
        PRIVATE "-framework AppKit" "-framework CoreGraphics" "-framework ImageIO")
    add_library(snow_shot_macos_cursor STATIC
        src/platform/macos/windowcursorcoordinator.cpp
        src/platform/macos/windowcursorcoordinator.mm)
    target_link_libraries(snow_shot_macos_cursor PUBLIC Qt6::Widgets PRIVATE "-framework AppKit")

    # Window adapters own their shared cursor dependency. Link this target instead
    # of compiling adapter sources in each executable.
    add_library(snow_shot_macos_window_platform STATIC
        src/platform/macos/screenshotnative.mm
        src/platform/macos/pinnedwindowplatform.mm
        src/presentation/pinned/pinnedwindowplatform.cpp)
    target_include_directories(snow_shot_macos_window_platform PRIVATE include)
    target_link_libraries(snow_shot_macos_window_platform PUBLIC Qt6::Widgets
        PRIVATE Qt6::GuiPrivate adqt::native_window snow_shot_macos_cursor "-framework AppKit" "-framework ApplicationServices"
        "-framework QuartzCore")
endif()


function(snow_shot_link_macos_window_platform target)
    if(APPLE)
        target_link_libraries(${target} PRIVATE snow_shot_macos_window_platform)
    endif()
endfunction()
include(SnowTestCliGuard)
include(SnowQtOffscreenTest)

set(_snow_shot_static_qt_plugins
    # The full-color application icon embeds SVG images. QtSvg renders the
    # outer document, but QImageReader needs this plugin for those images.
    Qt6::QSvgPlugin
    # The --update-probe startup path switches QT_QPA_PLATFORM to "offscreen"
    # before constructing QApplication. Static Qt registers platform plugins
    # at link time, so the release binary must import it here or the packaged
    # startup probe blocks forever on the missing-plugin dialog.
    Qt6::QOffscreenIntegrationPlugin
)
if(WIN32)
    list(APPEND _snow_shot_static_qt_plugins
        Qt6::QWindowsIntegrationPlugin
        Qt6::QSchannelBackendPlugin)
elseif(APPLE)
    list(APPEND _snow_shot_static_qt_plugins
        Qt6::QCocoaIntegrationPlugin
        Qt6::QMacStylePlugin
        Qt6::QSecureTransportBackendPlugin)
endif()

if(SNOW_SHOT_QT_STATIC)
    if(NOT QT_FEATURE_static)
        message(FATAL_ERROR
            "SNOW_SHOT_QT_STATIC requires a static Qt ${SNOW_QT_VERSION} kit. "
            "Set SNOW_QT_STATIC_DIR or Qt6_DIR to its lib/cmake/Qt6 directory.")
    endif()
    foreach(_snow_shot_static_qt_plugin IN LISTS _snow_shot_static_qt_plugins)
        if(NOT TARGET ${_snow_shot_static_qt_plugin})
            message(FATAL_ERROR
                "The static Qt kit does not provide ${_snow_shot_static_qt_plugin}.")
        endif()
    endforeach()
    unset(_snow_shot_static_qt_plugin)

    if(SNOW_SHOT_RELEASE_STATIC)
        get_target_property(_snow_shot_qt_core_private_features
            Qt6::Core QT_ENABLED_PRIVATE_FEATURES)
        get_target_property(_snow_shot_qt_gui_private_features
            Qt6::Gui QT_ENABLED_PRIVATE_FEATURES)
        if(NOT "system_zlib" IN_LIST _snow_shot_qt_core_private_features OR
           NOT "system_png" IN_LIST _snow_shot_qt_gui_private_features)
            message(FATAL_ERROR
                "The production static build requires Qt to use the same system "
                "zlib and libpng packages as Snow Image. Rebuild Qt with "
                "the platform build-static-qt script; bundled Qt codecs cannot be linked "
                "safely into the unified executable.")
        endif()
        unset(_snow_shot_qt_core_private_features)
        unset(_snow_shot_qt_gui_private_features)
    endif()
endif()

if(NOT DEFINED SNOW_SHOT_API_BASE_URL OR SNOW_SHOT_API_BASE_URL STREQUAL "")
    if(CMAKE_CONFIGURATION_TYPES)
        set(SNOW_SHOT_API_BASE_URL "https://snowshot.top" CACHE STRING
            "Snow Shot API base URL." FORCE)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(SNOW_SHOT_API_BASE_URL "http://localhost:8080" CACHE STRING
            "Snow Shot API base URL." FORCE)
    else()
        set(SNOW_SHOT_API_BASE_URL "https://snowshot.top" CACHE STRING
            "Snow Shot API base URL." FORCE)
    endif()
endif()
snow_find_qt_private(Gui)
set(SNOW_SHOT_DRAW_ENGINE_QT_DIR
    "${SNOW_DRAW_ENGINE_SOURCE_DIR}"
    CACHE PATH
    "Path to the snow_draw_engine_qt project."
)
set(SNOW_SHOT_CAPTURE_CRATES_DIR
    "${SNOW_CAPTURE_CRATES_DIR}"
    CACHE PATH
    "Path to the snow-crates workspace."
)

set(SNOW_DRAW_ENGINE_QT_BUILD_DEMO OFF CACHE BOOL "Build Snow Draw Engine Qt demo." FORCE)
if(NOT SNOW_SHOT_BUILD_TESTS)
    # Keep the production configuration free of test targets from the nested
    # draw engine project, including values left in an existing build cache.
    set(
        SNOW_DRAW_ENGINE_QT_BUILD_TESTS
        OFF
        CACHE BOOL
        "Build Snow Draw Engine Qt test executables."
        FORCE
    )
    set(
        SNOW_DRAW_ENGINE_QT_BUILD_BENCHMARKS
        OFF
        CACHE BOOL
        "Build opt-in Snow Draw Engine Qt benchmarks."
        FORCE
    )
endif()
if(NOT TARGET snow_draw_engine_qt_canvas)
    add_subdirectory("${SNOW_SHOT_DRAW_ENGINE_QT_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/snow_draw_engine_qt")
endif()

if(WIN32)
    snow_resolve_rust_libclang_directory(_snow_shot_libclang_dir)
    set(SNOW_SHOT_LIBCLANG_DIR
        "${_snow_shot_libclang_dir}"
        CACHE PATH
        "Directory containing host-compatible libclang.dll for ffmpeg-sys bindgen."
        FORCE
    )
endif()

if(NOT TARGET snow_capture_c)
    set(_snow_scrolling_perf_features)
    if(SNOW_SHOT_SCROLLING_PERF_DETAIL)
        list(APPEND _snow_scrolling_perf_features snow-stitch-images-c/perf-instrumentation)
    endif()
    snow_add_rust_static_libraries(snow_shot_rust_ffi
        MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        NO_DEFAULT_FEATURES
        FEATURES ${_snow_scrolling_perf_features}
        TARGETS
            snow_capture_c
            snow_ui_selector_c
            snow_stitch_images_c
        PACKAGES
            snow-capture-c
            snow-ui-selector-c
            snow-stitch-images-c
        OUTPUT_NAMES
            snow_capture_c
            snow_ui_selector_c
            snow_stitch_images_c
        STRIP_MSVC_DIRECTIVES
    )
endif()

# Selector framework requirements travel with the Rust static library on both Apple targets.
if(APPLE)
    target_link_libraries(snow_ui_selector_c INTERFACE
        "-framework ApplicationServices" "-framework CoreFoundation" "-framework CoreGraphics")
endif()

if(NOT TARGET snow_recording_c)
    snow_add_rust_static_library(snow_recording_c PACKAGE snow-recording-c
        MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}" OUTPUT_NAME snow_recording_c
        NO_DEFAULT_FEATURES)
endif()

if(NOT TARGET snow_visual_region_detector_c)
    snow_add_rust_static_library(snow_visual_region_detector_c PACKAGE snow-visual-region-detector-c MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}" OUTPUT_NAME snow_visual_region_detector_c)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowSelectedText.cmake")

add_library(snow_shot_crosshair_cursor STATIC
    src/presentation/core/crosshaircursor.cpp
    include/snow_shot/presentation/crosshaircursor.h)
target_include_directories(snow_shot_crosshair_cursor PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_crosshair_cursor PUBLIC Qt6::Widgets
    PRIVATE snow_draw_engine_qt_canvas)
if(WIN32)
    target_include_directories(snow_shot_crosshair_cursor PRIVATE
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
    target_link_libraries(snow_shot_crosshair_cursor PRIVATE snow_capture_c user32 gdi32)
endif()

# OpenCV's contrib wechat_qrcode module backs in-process QR recognition and
# objdetect's cv::barcode detector covers EAN/UPC 1-D symbologies; text
# recognition lives in the standalone OCR process. Pin the config package to
# the manifest-installed copy: the Rust OpenCV crates export a user-level
# OpenCV_DIR that would otherwise shadow it.
set(OpenCV_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/opencv4" CACHE PATH
    "OpenCV package configuration directory" FORCE)
find_package(OpenCV CONFIG REQUIRED)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/FetchSnowShotQrModels.cmake")
snow_shot_fetch_qr_models(SNOW_SHOT_QR_MODELS_DIR)
find_package(minizip-ng CONFIG REQUIRED)
include(SnowDiagnostics)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowOcrToolchain.cmake")
set(SNOW_SHOT_OCR_PROCESS_FEATURES)
set(SNOW_SHOT_OCR_PROCESS_ENVIRONMENT)
if(WIN32 OR (APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"))
    list(APPEND SNOW_SHOT_OCR_PROCESS_FEATURES gpu)
endif()

if(WIN32 OR APPLE)
    list(APPEND SNOW_SHOT_OCR_PROCESS_FEATURES crash-diagnostics)
    list(APPEND SNOW_SHOT_OCR_PROCESS_ENVIRONMENT
        "SNOW_CRASHPAD_LINK_FILE=${CMAKE_CURRENT_BINARY_DIR}/generated/snow-ocr-crash-$<CONFIG>.rsp"
        "CARGO_PROFILE_RELEASE_DEBUG=2" "CARGO_PROFILE_RELEASE_FAST_DEBUG=2")
endif()
if(APPLE)
    list(APPEND SNOW_SHOT_OCR_PROCESS_ENVIRONMENT
        "CARGO_PROFILE_RELEASE_SPLIT_DEBUGINFO=packed"
        "CARGO_PROFILE_RELEASE_FAST_SPLIT_DEBUGINFO=packed")
endif()
snow_add_rust_executable(snow_ocr_process
    PACKAGE snow-ocr-process
    MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
    OUTPUT_NAME snow-ocr-process
    PERFORMANCE_PROFILE
    REPRODUCIBLE
    TOOLCHAIN "${SNOW_SHOT_OCR_TOOLCHAIN}"
    FEATURES ${SNOW_SHOT_OCR_PROCESS_FEATURES}
    ENVIRONMENT ${SNOW_SHOT_OCR_PROCESS_ENVIRONMENT}
)

# The MCP bridge is deliberately a small standalone process.  It only speaks
# MCP over stdio and connects to an already running Snow Shot instance through
# the per-user local endpoint published by the application.
if(SNOW_SHOT_ENABLE_MCP)
    set(_snow_shot_mcp_features)
    if(SNOW_SHOT_DISTRIBUTION_CHANNEL STREQUAL "store")
        list(APPEND _snow_shot_mcp_features store)
    endif()
    snow_add_rust_executable(snow_shot_mcp
        PACKAGE snow-shot-mcp
        MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust/snow-shot-mcp"
        OUTPUT_NAME snow-shot-mcp
        PRODUCTION_PROFILE release-size
        REPRODUCIBLE
        SIZE_OPTIMIZED
        FEATURES ${_snow_shot_mcp_features}
    )
endif()

if(WIN32 OR APPLE)
    add_dependencies(snow_ocr_process_build snow_ocr_diagnostics_bridge)
endif()

set(SNOW_SHOT_SNOW_IMAGE_DIR
    "${SNOW_IMAGE_SOURCE_DIR}"
    CACHE PATH
    "Path to the snow_image project used by Snow Shot."
)
if(NOT TARGET snow_image::snow_image)
    set(SNOW_IMAGE_BUILD_TESTS OFF CACHE BOOL "Build snow_image tests." FORCE)
    set(SNOW_IMAGE_BUILD_SHARED OFF CACHE BOOL "Build the shared snow_image library." FORCE)
    set(SNOW_IMAGE_BUILD_STATIC ON CACHE BOOL "Build the static snow_image library." FORCE)
    set(SNOW_IMAGE_CODEC_PNG ON CACHE BOOL "Enable the libpng codec." FORCE)
    set(SNOW_IMAGE_CODEC_JPEG ON CACHE BOOL "Enable the libjpeg-turbo codec." FORCE)
    set(SNOW_IMAGE_CODEC_GIF ON CACHE BOOL "Enable the giflib codec." FORCE)
    set(SNOW_IMAGE_CODEC_HEIF ON CACHE BOOL "Enable HEIF and AVIF codecs." FORCE)
    set(SNOW_IMAGE_CODEC_JXL ON CACHE BOOL "Enable JPEG XL codecs." FORCE)
    set(SNOW_IMAGE_CODEC_WEBP ON CACHE BOOL "Enable WebP codecs." FORCE)
    add_subdirectory("${SNOW_SHOT_SNOW_IMAGE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/snow_image")
endif()
if(NOT TARGET snow_image::snow_image_static)
    message(FATAL_ERROR
        "Snow Shot requires the snow_image static target for its isolated image codec backend.")
endif()
if(TARGET snow_image_static)
    set_property(TARGET snow_image_static PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

set(SNOW_SHOT_IMAGE_CODEC_BACKEND_SOURCES
    src/image/snowimagecodecbridge.h
    src/image/snowimagecodecbackend.cpp
    src/image/snowimageanimationbackend.cpp
)
if(SNOW_SHOT_RELEASE_STATIC AND SNOW_SHOT_QT_STATIC)
    set(SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC ON CACHE INTERNAL
        "Link the Snow Shot image codec backend statically." FORCE)
    set(_SNOW_SHOT_IMAGE_CODEC_BACKEND_TYPE STATIC)
else()
    set(SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC OFF CACHE INTERNAL
        "Link the Snow Shot image codec backend statically." FORCE)
    set(_SNOW_SHOT_IMAGE_CODEC_BACKEND_TYPE SHARED)
endif()
add_library(snow_shot_image_codec_backend ${_SNOW_SHOT_IMAGE_CODEC_BACKEND_TYPE}
    ${SNOW_SHOT_IMAGE_CODEC_BACKEND_SOURCES}
)
unset(_SNOW_SHOT_IMAGE_CODEC_BACKEND_TYPE)
target_include_directories(snow_shot_image_codec_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
)
target_link_libraries(snow_shot_image_codec_backend PRIVATE
    $<BUILD_INTERFACE:snow_pixel_buffer>
    snow_image::snow_image_static
)
if(NOT SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
    # This backend is a DLL in the Debug/performance workspace builds and
    # cannot borrow codec symbols from the Qt executable. Keep its suppressed
    # static-library dependencies local to the DLL.
    if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
        find_package(PNG REQUIRED)
        target_link_libraries(snow_shot_image_codec_backend PRIVATE PNG::PNG)
    endif()
    # Snow Image selects its JPEG headers and library together, including Qt's
    # bundled libjpeg when requested. Its static target propagates those link
    # dependencies; adding vcpkg libjpeg here would override the matching ABI.
endif()
if(SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
    target_compile_definitions(snow_shot_image_codec_backend PUBLIC
        SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC=1
    )
else()
    target_compile_definitions(snow_shot_image_codec_backend PRIVATE
        SNOW_SHOT_IMAGE_CODEC_BACKEND_BUILD=1
    )
endif()
target_compile_features(snow_shot_image_codec_backend PUBLIC cxx_std_20)
set_target_properties(snow_shot_image_codec_backend PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

set(SNOW_SHOT_IMAGE_CODEC_SOURCES
    src/image/snowimageqtcodec.h
    src/image/snowimageqtcodec.cpp
    src/image/snowimageanimationqt.cpp
    src/image/snowimageqtsrgbrowreader.h
    src/image/screenshotimageencoding.cpp
)
add_library(snow_shot_image_codec STATIC ${SNOW_SHOT_IMAGE_CODEC_SOURCES})
snow_enable_unity_build(snow_shot_image_codec)
target_include_directories(snow_shot_image_codec PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
    "${SNOW_SHOT_SNOW_IMAGE_DIR}/include"
)
target_link_libraries(snow_shot_image_codec PUBLIC
    snow_shot_image_codec_backend
    snow_canvas_image
    Qt6::Core
    Qt6::Gui
)
target_compile_features(snow_shot_image_codec PUBLIC cxx_std_20)
add_library(snow_shot_animation_playback STATIC
    include/snow_shot/presentation/screenshotanimationplayback.h
    src/presentation/services/screenshotanimationplayback.cpp
    src/presentation/services/screenshotpresentationframescheduler.cpp)
target_include_directories(snow_shot_animation_playback PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_animation_playback PUBLIC
    Qt6::Widgets snow_shot_image_codec snow_canvas_image)
target_compile_features(snow_shot_animation_playback PUBLIC cxx_std_20)
if(APPLE)
    target_link_libraries(snow_shot_macos_clipboard PRIVATE snow_shot_image_codec)
endif()

if(WIN32 AND NOT SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
    set(_SNOW_SHOT_IMAGE_RUNTIME_STAGING_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/StageRuntimeDependencies.cmake"
    )
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(_SNOW_SHOT_IMAGE_RUNTIME_SEARCH_DIRECTORY
            "$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin>"
        )
    else()
        set(_SNOW_SHOT_IMAGE_RUNTIME_SEARCH_DIRECTORY
            "$<TARGET_FILE_DIR:snow_shot_image_codec_backend>"
        )
    endif()

    function(_snow_shot_stage_image_runtime target)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DSNOW_RUNTIME_ROOT_LIBRARY=$<TARGET_FILE:snow_shot_image_codec_backend>"
                "-DSNOW_RUNTIME_DESTINATION=$<TARGET_FILE_DIR:${target}>"
                "-DSNOW_RUNTIME_SEARCH_DIRECTORY=${_SNOW_SHOT_IMAGE_RUNTIME_SEARCH_DIRECTORY}"
                -P "${_SNOW_SHOT_IMAGE_RUNTIME_STAGING_SCRIPT}"
            COMMENT "Staging Snow Image runtime dependencies for ${target}"
            VERBATIM
        )
    endfunction()

    function(_snow_shot_target_uses_image_codec target output visited_targets)
        if("${target}" IN_LIST visited_targets)
            set(${output} FALSE PARENT_SCOPE)
            return()
        endif()
        list(APPEND visited_targets "${target}")

        get_target_property(_snow_shot_link_libraries "${target}" LINK_LIBRARIES)
        if(NOT _snow_shot_link_libraries)
            set(${output} FALSE PARENT_SCOPE)
            return()
        endif()

        foreach(_snow_shot_link_library IN LISTS _snow_shot_link_libraries)
            if(_snow_shot_link_library MATCHES "snow_shot_image_codec")
                set(${output} TRUE PARENT_SCOPE)
                return()
            endif()
            if(TARGET "${_snow_shot_link_library}")
                get_target_property(_snow_shot_link_target_imported
                    "${_snow_shot_link_library}" IMPORTED)
                if(NOT _snow_shot_link_target_imported)
                    _snow_shot_target_uses_image_codec(
                        "${_snow_shot_link_library}"
                        _snow_shot_transitively_uses_image_codec
                        "${visited_targets}"
                    )
                    if(_snow_shot_transitively_uses_image_codec)
                        set(${output} TRUE PARENT_SCOPE)
                        return()
                    endif()
                endif()
            endif()
        endforeach()

        set(${output} FALSE PARENT_SCOPE)
    endfunction()
endif()

set(
    SNOW_SHOT_ADQT_DIR
    "${SNOW_ANT_DESIGN_SOURCE_DIR}"
    CACHE PATH
    "Path to the ant_design_qt source tree for standalone builds."
)

if(NOT EXISTS "${SNOW_SHOT_ADQT_DIR}/packages/ant_design_qt")
    message(
        FATAL_ERROR
        "Unable to locate ant_design_qt. Set SNOW_SHOT_ADQT_DIR to an external source tree for a standalone build."
    )
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Recording input validation and both MCP catalogs share one reviewed contract.
# Check at configure time as well as in tests, including builds with MCP disabled.
set(_snow_mcp_contract_generator
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_mcp_capabilities.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_snow_mcp_contract_generator}"
    "${CMAKE_CURRENT_SOURCE_DIR}/mcp-capabilities.source.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/mcp-capabilities.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/mcp-capabilities-mini.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/snow_shot/app/mcp/recordingoptioncontracts.h")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_snow_mcp_contract_generator}" --check
    COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(snow_shot_check_mcp_capabilities
    COMMAND "${Python3_EXECUTABLE}" "${_snow_mcp_contract_generator}" --check
    VERBATIM)

set(SNOW_SHOT_ICON_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons.manifest.json"
)
set(SNOW_SHOT_ICON_MANIFEST_BUILDER
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_icon_manifest.py"
)
set(SNOW_SHOT_ICON_PACK_GENERATOR
    "${SNOW_SHOT_ADQT_DIR}/tools/generate_icon_pack.py"
)
set(SNOW_SHOT_ICON_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/include/snow_shot/presentation/components/icons/snowshoticons.h"
)
set(SNOW_SHOT_ICON_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/components/icons/snowshoticons.cpp"
)
file(GLOB SNOW_SHOT_ICON_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/components/icons/resources/*.svg"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SNOW_SHOT_ICON_MANIFEST}"
    "${SNOW_SHOT_ICON_MANIFEST_BUILDER}"
    "${SNOW_SHOT_ICON_PACK_GENERATOR}"
    ${SNOW_SHOT_ICON_ASSETS}
)

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${SNOW_SHOT_ICON_MANIFEST_BUILDER}" --check
    RESULT_VARIABLE _snow_shot_icon_manifest_is_stale
    OUTPUT_QUIET
    ERROR_QUIET
)
if(_snow_shot_icon_manifest_is_stale)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${SNOW_SHOT_ICON_MANIFEST_BUILDER}"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

execute_process(
    COMMAND
        "${Python3_EXECUTABLE}" "${SNOW_SHOT_ICON_PACK_GENERATOR}"
        "${SNOW_SHOT_ICON_MANIFEST}"
        --header "${SNOW_SHOT_ICON_HEADER}"
        --source "${SNOW_SHOT_ICON_SOURCE}"
        --check
    RESULT_VARIABLE _snow_shot_icon_pack_is_stale
    OUTPUT_QUIET
    ERROR_QUIET
)
if(_snow_shot_icon_pack_is_stale)
    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}" "${SNOW_SHOT_ICON_PACK_GENERATOR}"
            "${SNOW_SHOT_ICON_MANIFEST}"
            --header "${SNOW_SHOT_ICON_HEADER}"
            --source "${SNOW_SHOT_ICON_SOURCE}"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

set(ADQT_ICON_CORE_DIR "${SNOW_SHOT_ADQT_DIR}/packages/adqt_icon_core")
set(ANT_DESIGN_ICONS_QT_DIR "${SNOW_SHOT_ADQT_DIR}/packages/ant_design_icons_qt")
set(ANT_DESIGN_QT_DIR "${SNOW_SHOT_ADQT_DIR}/packages/ant_design_qt")

if(NOT TARGET adqt::widgets)
    add_subdirectory("${SNOW_SHOT_ADQT_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ant_design_qt")
endif()

set(SNOW_SHOT_STORAGE_SOURCES
    include/snow_shot/storage/preparedpngimage.h
    include/snow_shot/storage/applicationstorage.h
    include/snow_shot/storage/appstorageusage.h
    include/snow_shot/storage/capturehistoryrepository.h
    include/snow_shot/storage/capturehistorytypes.h
    include/snow_shot/storage/configurationarchive.h
    include/snow_shot/storage/configurationstore.h
    include/snow_shot/storage/configurationschema.h
    include/snow_shot/storage/exportfilename.h
    include/snow_shot/storage/timevariables.h
    include/snow_shot/storage/persistedselectioncodec.h
    include/snow_shot/storage/persistedwindowgeometry.h
    include/snow_shot/storage/pinnedwindowrepository.h
    include/snow_shot/storage/pinnedwindowtypes.h
    include/snow_shot/storage/settingsadapters.h
    include/snow_shot/storage/storagelogging.h
    include/snow_shot/storage/storageresult.h
    include/snow_shot/storage/storageusagetracker.h
    src/storage/applicationstorage.cpp
    src/storage/storagedirectorychange.cpp
    src/storage/capturehistorypolicy_p.h
    src/storage/capturehistoryrepository.cpp
    src/storage/configurationarchive.cpp
    src/storage/configurationstore.cpp
    src/storage/configurationschema.cpp
    src/storage/exportfilename.cpp
    src/storage/timevariables.cpp
    src/storage/screenshotexportvariables.cpp
    src/storage/persistedselectioncodec.cpp
    src/storage/persistedwindowgeometry.cpp
    src/storage/pinnedwindowrepository.cpp
    src/storage/preparedpngimage.cpp
    src/storage/settingsadapters.cpp
    src/storage/storagedirectoryutils_p.h
    src/storage/storagelogging.cpp
    src/storage/storageusagetracker.cpp
)
add_library(snow_shot_runtime_activity INTERFACE)
target_include_directories(snow_shot_runtime_activity INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_sources(snow_shot_runtime_activity INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/snow_shot/runtime/runtimeactivitytracker.h")
add_library(snow_shot_memory_optimization STATIC
    src/runtime/memoryoptimizationcontroller.cpp)
target_link_libraries(snow_shot_memory_optimization PUBLIC snow_shot_runtime_activity Qt6::Core)
if(APPLE)
    target_sources(snow_shot_memory_optimization PRIVATE src/platform/macos/memoryoptimization.cpp)
    target_link_libraries(snow_shot_memory_optimization PRIVATE Qt6::Gui)
else()
    target_sources(snow_shot_memory_optimization PRIVATE src/platform/windows/memoryoptimization.cpp)
endif()
if(WIN32)
    target_link_libraries(snow_shot_memory_optimization PRIVATE psapi user32)
endif()

add_library(snow_shot_shortcuts STATIC
    include/snow_shot/shortcuts/shortcutbinding.h
    include/snow_shot/shortcuts/shortcutdisplayservice.h
    src/shortcuts/shortcutbinding.cpp
    src/shortcuts/shortcutdisplayservice.cpp
)
target_include_directories(snow_shot_shortcuts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_shortcuts PUBLIC Qt6::Core Qt6::Gui)
if(APPLE)
    find_library(SNOW_SHOT_CARBON_FRAMEWORK NAMES Carbon REQUIRED)
    find_library(SNOW_SHOT_CORE_FOUNDATION_FRAMEWORK NAMES CoreFoundation REQUIRED)
    target_link_libraries(snow_shot_shortcuts PRIVATE
        "${SNOW_SHOT_CARBON_FRAMEWORK}"
        "${SNOW_SHOT_CORE_FOUNDATION_FRAMEWORK}"
    )
endif()

add_library(snow_shot_clipboard_files STATIC
    include/snow_shot/storage/clipboardfilecache.h
    src/storage/clipboardfilecache.cpp)
target_include_directories(snow_shot_clipboard_files PUBLIC include)
target_link_libraries(snow_shot_clipboard_files PUBLIC Qt6::Core)

add_library(snow_shot_storage STATIC ${SNOW_SHOT_STORAGE_SOURCES})
snow_enable_unity_build(snow_shot_storage)
# Repository implementations have independent anonymous-namespace helpers.
set_source_files_properties(
    src/storage/capturehistoryrepository.cpp
    src/storage/pinnedwindowrepository.cpp
    src/storage/configurationschema.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(snow_shot_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_storage PRIVATE snow_draw_engine_c)
target_link_libraries(snow_shot_storage PUBLIC
    snow_shot_runtime_activity
    snow_shot_diagnostics
    snow_shot_image_codec
    snow_shot_shortcuts
    MINIZIP::minizip-ng
    Qt6::Core
    Qt6::Gui
)

add_library(snow_shot_translation STATIC
    include/snow_shot/translation/translationservice.h
    include/snow_shot/translation/translationlanguages.h
    include/snow_shot/translation/translationlanguagecatalog.h
    include/snow_shot/translation/translationlanguagedetector.h
    src/app/translationlanguagedetector.cpp
    src/app/translationservice.cpp
    include/snow_shot/presentation/selectedtexttranslationcontroller.h
    src/presentation/services/selectedtexttranslationcontroller.cpp
    include/snow_shot/network/snowshotapiclient.h
    src/network/boundednetworkresponse.h
    src/network/snowshotapiclient.cpp
    include/snow_shot/presentation/translationlanguages.h
    src/presentation/services/translationlanguages.cpp
    include/snow_shot/presentation/translationpagecontroller.h
    src/presentation/services/translationpagecontroller.cpp
)
snow_enable_unity_build(snow_shot_translation)
target_include_directories(snow_shot_translation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_translation PUBLIC snow_shot_storage Qt6::Network
    PRIVATE snow_selected_text_c snow_language_detection_c)
target_compile_definitions(snow_shot_translation PRIVATE
    SNOW_SHOT_API_BASE_URL=\"${SNOW_SHOT_API_BASE_URL}\"
)

add_library(snow_shot_window_shortcuts STATIC
    include/snow_shot/presentation/mousereleaseactioncontroller.h
    src/presentation/services/mousereleaseactioncontroller.cpp
    include/snow_shot/presentation/windowshortcutmanager.h
    src/presentation/services/windowshortcutmanager.cpp
)
target_include_directories(snow_shot_window_shortcuts PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(snow_shot_window_shortcuts PUBLIC Qt6::Widgets)
target_link_libraries(snow_shot_window_shortcuts PUBLIC snow_shot_shortcuts)
if(APPLE)
    target_sources(snow_shot_window_shortcuts PRIVATE src/platform/macos/windowshortcutinput.mm)
    target_link_libraries(snow_shot_window_shortcuts PRIVATE "-framework AppKit")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowShotTranslations.cmake")

set(SNOW_SHOT_SETTINGS_CATALOG_SOURCES
    include/snow_shot/presentation/settings/settingscatalog.h
    include/snow_shot/presentation/settings/settingsregistry.h
    src/presentation/components/icons/snowshoticons.cpp
    src/presentation/settings/settingscatalog.cpp
    src/presentation/settings/settingsregistry.cpp
)
# Widget targets also compile these icons. Keep their definitions separate from
# the catalog so pulling the catalog from its archive does not pull duplicate icons.
set_source_files_properties(src/presentation/components/icons/snowshoticons.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
add_library(snow_shot_settings_catalog STATIC ${SNOW_SHOT_SETTINGS_CATALOG_SOURCES})
snow_enable_unity_build(snow_shot_settings_catalog)
target_include_directories(snow_shot_settings_catalog PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_settings_catalog PUBLIC
    ant_design_icons_qt
    snow_shot_storage
    Qt6::Widgets
)

set(SNOW_SHOT_SETTINGS_SEARCH_SOURCES
    include/snow_shot/presentation/components/applicationsearchwidget.h
    include/snow_shot/presentation/languagemanager.h
    include/snow_shot/presentation/settings/settingssearchindex.h
    src/presentation/components/applicationsearchwidget.cpp
    src/presentation/services/languagemanager.cpp
    src/presentation/settings/settingssearchindex.cpp
)
add_library(snow_shot_settings_search STATIC ${SNOW_SHOT_SETTINGS_SEARCH_SOURCES})
snow_enable_unity_build(snow_shot_settings_search)
target_include_directories(snow_shot_settings_search PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_settings_search PUBLIC
    ant_design_icons_qt
    ant_design_qt
    snow_shot_settings_catalog
    snow_shot_storage
    Qt6::Widgets
)

add_library(snow_shot_main_window_skin STATIC
    include/snow_shot/presentation/mainwindowskincontroller.h
    include/snow_shot/presentation/mainwindowskinwidget.h
    src/presentation/services/mainwindowskincontroller.cpp
    src/presentation/components/mainwindowskinwidget.cpp)
target_include_directories(snow_shot_main_window_skin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_main_window_skin PUBLIC Qt6::Widgets
    PRIVATE snow_shot_storage snow_shot_image_codec snow_draw_engine_qt_canvas)

set(SNOW_SHOT_SETTINGS_SOURCES
    src/presentation/services/pinnedtextstylecapture.cpp
    include/snow_shot/presentation/selectedtexttranslationcoordinator.h
    src/presentation/services/selectedtexttranslationcoordinator.cpp
    include/snow_shot/presentation/components/standalonetranslationwindow.h
    src/presentation/components/standalonetranslationwindow.cpp
    include/snow_shot/presentation/windowgeometrymemory.h
    src/presentation/windowgeometrymemory.cpp
    include/snow_shot/presentation/components/translationpagewidget.h
    src/presentation/components/translationpagewidget.cpp
    include/snow_shot/presentation/components/aboutpagewidget.h
    include/snow_shot/platform/windows/autostartregistration.h
    include/snow_shot/platform/windows/focusedfullscreenwindow.h
    include/snow_shot/platform/windows/printscreenshortcutrecorder.h
    include/snow_shot/shortcuts/shortcutrecorder.h
    include/snow_shot/presentation/components/pathinput.h
    include/snow_shot/presentation/components/formfields.h
    include/snow_shot/presentation/translatabletext.h
    include/snow_shot/presentation/settings/settingsformfield.h
    include/snow_shot/presentation/components/globalmouserow.h
    include/snow_shot/presentation/components/toolbareditorsettingswidget.h
    include/snow_shot/presentation/components/infotooltipicon.h
    include/snow_shot/presentation/components/pagecontainerwidget.h
    include/snow_shot/presentation/components/sectionheaderwidget.h
    include/snow_shot/presentation/components/confirmationskipbutton.h
    include/snow_shot/presentation/components/overlaycontrolbutton.h
    include/snow_shot/presentation/components/canvasactiontoolbar.h
    include/snow_shot/presentation/styles/sequencenumberbadgestyle.h
    include/snow_shot/presentation/components/imagedropsurface.h
    include/snow_shot/presentation/components/screenshothistorypagewidget.h
    include/snow_shot/presentation/components/emptystateicon.h
    include/snow_shot/presentation/components/texttranslationsettingswidget.h
    include/snow_shot/presentation/components/customaimodelssettingswidget.h
    include/snow_shot/presentation/components/settingscustomwidget.h
    include/snow_shot/presentation/components/settingspagewidget.h
    include/snow_shot/presentation/shortcutdisplaytext.h
    include/snow_shot/presentation/components/actionrow.h
    include/snow_shot/presentation/components/shortcutkeyrow.h
    include/snow_shot/presentation/components/shortcutconfigurationbutton.h
    include/snow_shot/presentation/components/storagestatussettingswidget.h
    include/snow_shot/presentation/screenshottoolbarlayoutmodel.h
    include/snow_shot/presentation/settings/settingsbackend.h
    include/snow_shot/presentation/settings/settingsruntimesession.h
    include/snow_shot/presentation/settings/applicationpriority.h
    include/snow_shot/presentation/styles/thememanager.h
    src/presentation/components/aboutpagewidget.cpp
    src/presentation/components/infotooltipicon.cpp
    src/presentation/components/globalmouserow.cpp
    src/presentation/components/pathinput.cpp
    src/presentation/components/formfields.cpp
    src/presentation/settings/settingsformfield.cpp
    src/presentation/components/pagecontainerwidget.cpp
    src/presentation/components/sectionheaderwidget.cpp
    src/presentation/components/screenshothistorypagewidget.cpp
    include/snow_shot/presentation/components/historyentrycard.h
    src/presentation/components/historyentrycard.cpp
    include/snow_shot/presentation/components/historyselectionbar.h
    include/snow_shot/presentation/components/historypagecommon.h
    src/presentation/components/historypagecommon.cpp
    include/snow_shot/presentation/components/thumbnailcache.h
    src/presentation/components/thumbnailcache.cpp
    include/snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h
    src/presentation/components/pinnedwindowmanagementpagewidget.cpp
    src/presentation/services/screenshotclipboardcontent.cpp
    src/presentation/services/pinnedcolorswatch.cpp
    src/presentation/services/imageimportsourcereader_p.h
    src/presentation/services/imageimportsourcereader_p.cpp
    src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
    src/presentation/components/texttranslationsettingswidget.cpp
    src/presentation/components/customaimodelssettingswidget.cpp
    src/presentation/components/settingscustomwidget.cpp
    src/presentation/components/settingspagewidget.cpp
    include/snow_shot/presentation/components/screenrecordingsettingsdialog.h
    include/snow_shot/presentation/components/screenrecordingmodal.h
    src/presentation/components/screenrecordingsettingsdialog.cpp
    src/presentation/components/actionrow.cpp
    src/presentation/components/shortcutkeyrow.cpp
    src/presentation/components/shortcutconfigurationbutton.cpp
    src/presentation/components/storagestatussettingswidget.cpp
    src/presentation/settings/settingsbackend.cpp
    src/presentation/settings/settingsruntimesession.cpp
    src/presentation/settings/applicationpriority.cpp
    src/presentation/styles/buttonborder.cpp
    src/presentation/styles/mainwindowcomponenttoken.cpp
    src/presentation/styles/themecolorscheme.cpp
    src/presentation/styles/thememanager.cpp
    src/presentation/services/shortcutrecorder.cpp
)
if(WIN32)
    list(APPEND SNOW_SHOT_SETTINGS_SOURCES
        src/platform/windows/printscreenshortcutrecorder.cpp)
endif()
set(SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES
    include/snow_shot/platform/focusedfullscreenwindow.h
    include/snow_shot/presentation/globalshortcutmanager.h
    include/snow_shot/presentation/globalshortcuttypes.h
    src/presentation/services/globalshortcutbackend_p.h
    src/presentation/services/globalshortcutbackend.cpp
    src/presentation/services/globalshortcutmanager.cpp
    src/platform/focusedfullscreenwindow.cpp
)
if(WIN32)
    list(APPEND SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES
        include/snow_shot/platform/windows/windowalwaysontop.h
        src/platform/windows/windowalwaysontop.cpp
        src/platform/windows/globalshortcutbackend.cpp
        src/platform/windows/focusedfullscreenwindow.cpp)
elseif(APPLE)
    enable_language(OBJCXX)
    list(APPEND SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES
        src/platform/macos/globalshortcutbackend.cpp
        src/platform/macos/focusedfullscreenwindow.mm)
endif()
list(APPEND SNOW_SHOT_SETTINGS_SOURCES ${SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES})
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/components/screenshothistorypagewidget.cpp"
     _snow_shot_history_source)
string(FIND "${_snow_shot_history_source}" "QThreadPool::globalInstance()"
       _snow_shot_history_global_pool_call)
if(NOT _snow_shot_history_global_pool_call EQUAL -1)
    message(FATAL_ERROR
        "Screenshot History must use its dedicated executor, not QThreadPool::globalInstance().")
endif()
unset(_snow_shot_history_source)
unset(_snow_shot_history_global_pool_call)
add_library(snow_shot_file_manager STATIC
    include/snow_shot/platform/filemanager.h
    src/platform/filemanager.cpp)
target_include_directories(snow_shot_file_manager PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_file_manager PUBLIC Qt6::Gui)
if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-file-manager-tests tests/file_manager_tests.cpp)
    target_link_libraries(snow-shot-file-manager-tests PRIVATE snow_shot_file_manager)
    add_test(NAME snow-shot-file-manager-tests COMMAND snow-shot-file-manager-tests)
    set_tests_properties(snow-shot-file-manager-tests PROPERTIES LABELS unit
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_library(snow_shot_login_item STATIC
    include/snow_shot/platform/macos/loginitemservice.h
    src/platform/macos/loginitemservice.cpp)
target_include_directories(snow_shot_login_item PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_login_item PUBLIC Qt6::Core)
if(APPLE)
    target_sources(snow_shot_login_item PRIVATE src/platform/macos/loginitembackend.mm)
    target_link_libraries(snow_shot_login_item PRIVATE snow_shot_storage
        "-framework AppKit" "-framework Carbon" "-framework ServiceManagement")
endif()
if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-login-item-tests tests/login_item_tests.cpp)
    target_link_libraries(snow-shot-login-item-tests PRIVATE snow_shot_login_item)
    add_test(NAME snow-shot-login-item-tests COMMAND snow-shot-login-item-tests)
    set_tests_properties(snow-shot-login-item-tests PROPERTIES LABELS unit)
endif()
add_library(snow_shot_system_notification_controller STATIC
    include/snow_shot/presentation/systemnotificationcontroller.h
    src/presentation/services/systemnotificationcontroller.cpp
    src/presentation/services/systemnotificationfeedback.cpp)
target_include_directories(snow_shot_system_notification_controller PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_system_notification_controller PUBLIC Qt6::Core
    PRIVATE ant_design_qt Qt6::Widgets)
if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-system-notification-controller-tests
        tests/system_notification_controller_tests.cpp)
    target_link_libraries(snow-shot-system-notification-controller-tests PRIVATE
        snow_shot_system_notification_controller ant_design_qt Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-system-notification-controller-tests)
    snow_shot_offscreen_qpa_environment(_SNOW_NOTIFICATION_QPA_ENVIRONMENT)
    add_test(NAME snow-shot-system-notification-controller-tests
        COMMAND snow-shot-system-notification-controller-tests -platform offscreen)
    set_tests_properties(snow-shot-system-notification-controller-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 ENVIRONMENT "${_SNOW_NOTIFICATION_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
if(APPLE)
    add_library(snow_shot_system_notifications STATIC
        src/platform/macos/systemnotificationservice.h
        src/platform/macos/systemnotificationservice.mm)
    target_include_directories(snow_shot_system_notifications PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos"
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow_shot_system_notifications PUBLIC Qt6::Core
        PRIVATE "-framework Foundation" "-framework UserNotifications")
    if(SNOW_SHOT_BUILD_TESTS)
        add_executable(snow-shot-macos-system-notification-tests
            tests/macos_system_notification_tests.mm)
        target_link_libraries(snow-shot-macos-system-notification-tests PRIVATE
            snow_shot_system_notifications "-framework UserNotifications")
        add_test(NAME snow-shot-macos-system-notification-tests
            COMMAND snow-shot-macos-system-notification-tests)
        set_tests_properties(snow-shot-macos-system-notification-tests PROPERTIES
            LABELS "unit;macos" TIMEOUT 30)
    endif()
endif()
add_library(snow_shot_administrator STATIC
    src/platform/windows/autostartregistration.cpp
    src/platform/windows/shellcontextmenuregistration.cpp
    src/platform/windows/administratorlaunch.cpp)
target_include_directories(snow_shot_administrator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_administrator PUBLIC Qt6::Core Qt6::Network)
if(WIN32)
    target_link_libraries(snow_shot_administrator PRIVATE taskschd ole32 oleaut32 advapi32 shell32 uuid comsuppw windowsapp)
endif()
if(SNOW_SHOT_BUILD_TESTS)
    if(WIN32)
        add_executable(snow-shot-store-platform-tests
            tests/store_platform_tests.cpp
            src/storage/storagedirectorychange.cpp
            src/platform/windows/autostartregistration.cpp
            src/platform/windows/shellcontextmenuregistration.cpp
            src/platform/windows/administratorlaunch.cpp)
        target_include_directories(snow-shot-store-platform-tests PRIVATE include)
        target_compile_definitions(snow-shot-store-platform-tests PRIVATE SNOW_SHOT_STORE_BUILD=1)
        target_link_libraries(snow-shot-store-platform-tests PRIVATE snow_shot_shortcuts
            Qt6::Core Qt6::Network taskschd ole32 oleaut32 advapi32 shell32 uuid comsuppw windowsapp)
        add_test(NAME snow-shot-store-platform-tests COMMAND snow-shot-store-platform-tests)
        set_tests_properties(snow-shot-store-platform-tests PROPERTIES LABELS "unit;windows" TIMEOUT 30)
    endif()
    add_executable(snow-shot-shell-image-launch-tests tests/shell_image_launch_tests.cpp)
    target_include_directories(snow-shot-shell-image-launch-tests PRIVATE include)
    target_link_libraries(snow-shot-shell-image-launch-tests PRIVATE Qt6::Core)
    add_test(NAME snow-shot-shell-image-launch-tests COMMAND snow-shot-shell-image-launch-tests)
    set_tests_properties(snow-shot-shell-image-launch-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-administrator-launch-tests tests/administrator_launch_tests.cpp)
    target_link_libraries(snow-shot-administrator-launch-tests PRIVATE snow_shot_administrator)
    add_test(NAME snow-shot-administrator-launch-tests COMMAND snow-shot-administrator-launch-tests)
    set_tests_properties(snow-shot-administrator-launch-tests PROPERTIES LABELS "unit;windows" TIMEOUT 30)

    add_executable(snow-shot-startup-registration-report-tests
        tests/startup_registration_report_tests.cpp)
    target_link_libraries(snow-shot-startup-registration-report-tests PRIVATE Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-startup-registration-report-tests)
    snow_shot_offscreen_qpa_environment(_SNOW_SHOT_STARTUP_REPORT_QPA_ENVIRONMENT)
    add_test(NAME snow-shot-startup-registration-report-tests
        COMMAND snow-shot-startup-registration-report-tests)
    set_tests_properties(snow-shot-startup-registration-report-tests PROPERTIES
        LABELS unit TIMEOUT 30 ENVIRONMENT "${_SNOW_SHOT_STARTUP_REPORT_QPA_ENVIRONMENT}")

    add_executable(snow-shot-application-restart-tests
        tests/application_restart_tests.cpp
        src/app/applicationrestart.cpp)
    target_include_directories(snow-shot-application-restart-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-application-restart-tests PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME snow-shot-application-restart-tests COMMAND snow-shot-application-restart-tests)
    set_tests_properties(snow-shot-application-restart-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
add_library(snow_shot_permissions STATIC
    include/snow_shot/presentation/apppermissionservice.h
    src/presentation/services/apppermissionservice.cpp)
target_include_directories(snow_shot_permissions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_permissions PUBLIC Qt6::Widgets snow_shot_shortcuts
    PRIVATE snow_shot_diagnostics)
if(APPLE)
    target_sources(snow_shot_permissions PRIVATE src/platform/macos/apppermissionbackend.mm)
    target_link_libraries(snow_shot_permissions PRIVATE
        "-framework AppKit"
        "-framework ApplicationServices"
        "-framework AVFoundation")
endif()
find_package(Qt6 REQUIRED COMPONENTS Concurrent)
add_library(snow_shot_cloud_upload STATIC
    include/snow_shot/presentation/screenshotclouduploadservice.h
    src/presentation/services/screenshotclouduploadservice.cpp)
target_include_directories(snow_shot_cloud_upload PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_cloud_upload PUBLIC snow_shot_storage Qt6::Network Qt6::Concurrent)

add_library(snow_shot_settings STATIC ${SNOW_SHOT_SETTINGS_SOURCES}
    include/snow_shot/presentation/permissionguidecontroller.h
    include/snow_shot/presentation/components/permissionguidewidget.h
    src/presentation/services/permissionguidecontroller.cpp
    src/presentation/components/permissionguidewidget.cpp)
if(APPLE)
    target_sources(snow_shot_settings PRIVATE src/platform/macos/permissionguideplatform.mm)
endif()
target_sources(snow_shot_settings PRIVATE
    include/snow_shot/presentation/components/clouduploadsettingswidget.h
    src/presentation/components/clouduploadsettingswidget.cpp)
target_link_libraries(snow_shot_settings PUBLIC snow_shot_permissions snow_shot_login_item
    snow_shot_store_lifecycle)
qt_add_resources(snow_shot_settings "snow_shot_about_assets"
    PREFIX "/snow_shot/about"
    BASE "resources/about"
    FILES resources/about/illustration.svg
)
target_compile_definitions(snow_shot_settings PRIVATE
    SNOW_SHOT_WEBSITE_URL="${SNOW_SHOT_WEBSITE_URL}"
    SNOW_SHOT_PROJECT_URL="${SNOW_SHOT_PROJECT_URL}"
    SNOW_SHOT_QQ_GROUP_3_URL="${SNOW_SHOT_QQ_GROUP_3_URL}"
)
target_link_libraries(snow_shot_settings PRIVATE snow_shot_image_codec Qt6::Svg Qt6::Network snow_shot_administrator)
add_library(snow_shot_global_mouse STATIC
    include/snow_shot/presentation/globalmousemanager.h
    include/snow_shot/presentation/globalmousegesture.h
    include/snow_shot/presentation/globalmousetypes.h
    src/presentation/services/globalmousemanager.cpp
    src/presentation/services/globalmousegesture.cpp
    src/platform/globalmousebackend.cpp
)
target_include_directories(snow_shot_global_mouse PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_global_mouse PUBLIC snow_shot_storage Qt6::Gui)
if(WIN32)
    target_sources(snow_shot_global_mouse PRIVATE
        src/platform/windows/globalmousebackend.cpp
        src/platform/windows/globalmousebackend_p.h)
    target_link_libraries(snow_shot_global_mouse PRIVATE dwmapi user32)
endif()
if(APPLE)
    enable_language(OBJCXX)
    target_sources(snow_shot_global_mouse PRIVATE
        src/platform/macos/globalmousebackend.mm
        src/platform/macos/globalmouseinput.cpp
        src/platform/macos/globalmousebackend_p.h)
    find_library(SNOW_SHOT_APPKIT_FRAMEWORK NAMES AppKit REQUIRED)
    find_library(SNOW_SHOT_APPLICATION_SERVICES_FRAMEWORK NAMES ApplicationServices REQUIRED)
    find_library(SNOW_SHOT_CORE_FOUNDATION_FRAMEWORK NAMES CoreFoundation REQUIRED)
    find_library(SNOW_SHOT_CORE_GRAPHICS_FRAMEWORK NAMES CoreGraphics REQUIRED)
    target_link_libraries(snow_shot_global_mouse PRIVATE
        "${SNOW_SHOT_CARBON_FRAMEWORK}"
        "${SNOW_SHOT_APPKIT_FRAMEWORK}"
        "${SNOW_SHOT_APPLICATION_SERVICES_FRAMEWORK}"
        "${SNOW_SHOT_CORE_FOUNDATION_FRAMEWORK}"
        "${SNOW_SHOT_CORE_GRAPHICS_FRAMEWORK}")
    target_link_libraries(snow_shot_settings PRIVATE
        "${SNOW_SHOT_CARBON_FRAMEWORK}"
        "${SNOW_SHOT_APPKIT_FRAMEWORK}"
        "${SNOW_SHOT_CORE_GRAPHICS_FRAMEWORK}")
endif()
target_link_libraries(snow_shot_settings PRIVATE snow_shot_global_mouse)
snow_enable_unity_build(snow_shot_settings)
set_source_files_properties(
    src/presentation/styles/thememanager.cpp
    src/presentation/components/shortcutkeyrow.cpp
    src/platform/macos/globalshortcutbackend.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(snow_shot_settings
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/network")
target_link_libraries(snow_shot_settings PUBLIC
    snow_shot_file_manager
    snow_shot_main_window_skin
    snow_shot_window_shortcuts
    snow_shot_translation
    ant_design_icons_qt
    ant_design_qt
    snow_shot_settings_search
    snow_shot_storage
    Qt6::Widgets
)
if(WIN32)
    target_link_libraries(snow_shot_settings PUBLIC advapi32 bcrypt dwmapi ntdll user32)
endif()

add_library(snow_shot_collage_core STATIC
    include/snow_shot/presentation/collagedocument.h
    src/presentation/collage/collagedocument.cpp
    src/presentation/collage/collagetransformedrows_p.h
    src/presentation/collage/collageresourcebudget.cpp
    src/presentation/collage/collageresourcebudget_p.h)
target_include_directories(snow_shot_collage_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
target_link_libraries(snow_shot_collage_core PUBLIC snow_shot_image_codec
    snow_draw_engine_qt_canvas Qt6::Widgets)

add_library(snow_shot_collage STATIC
    include/snow_shot/presentation/collagewindow.h
    include/snow_shot/presentation/collagewindowcoordinator.h
    include/snow_shot/presentation/collagesettingsstore.h
    include/snow_shot/presentation/components/collageeditorwidget.h
    include/snow_shot/presentation/components/collagepdfpagegrid.h
    include/snow_shot/presentation/components/collagepdfsettingswidget.h
    include/snow_shot/presentation/collagepdfexport.h
    src/presentation/collage/collagewindow.cpp
    src/presentation/collage/collagewindowcoordinator.cpp
    src/presentation/collage/collageeditorwidget.cpp
    src/presentation/collage/collagepdfpagegrid.cpp
    src/presentation/collage/collagepdfsettingswidget.cpp
    src/presentation/collage/collagepdfexport.cpp)
target_include_directories(snow_shot_collage PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
target_link_libraries(snow_shot_collage PUBLIC snow_shot_collage_core snow_shot_settings)

add_library(snow_shot_image_split_core STATIC
    include/snow_shot/presentation/imagesplitdocument.h
    src/presentation/imagesplit/imagesplitdocument.cpp)
target_include_directories(snow_shot_image_split_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_image_split_core PUBLIC Qt6::Widgets)
add_library(snow_shot_image_split STATIC
    include/snow_shot/presentation/imagesplitwindow.h
    include/snow_shot/presentation/components/imagespliteditorwidget.h
    src/presentation/imagesplit/imagespliteditorwidget.cpp
    src/presentation/imagesplit/imagesplitwindow.cpp
    src/presentation/imagesplit/imagesplitimagepreparation_p.cpp
    src/presentation/imagesplit/imagesplitexport.cpp)
target_link_libraries(snow_shot_image_split PUBLIC snow_shot_image_split_core
    snow_shot_collage_core snow_shot_settings)

# Tests reuse the same export implementations as the application; the collage
# library itself supplies composition and never publishes history or auto-saves.
# Clipboard content and DIB decoding are supplied by snow_shot_settings.
set(SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES
    include/snow_shot/presentation/screenshotsavepreviewcanvas.h
    src/presentation/services/screenshotmanualsave.cpp
    src/presentation/services/screenshotsaveasfiledialog.cpp
    src/presentation/services/screenshotsavefooter.h
    src/presentation/services/screenshotsavefooter.cpp
    src/presentation/services/screenshotsaveresult.h
    src/presentation/services/screenshotsaveresult.cpp
    src/presentation/services/screenshotsaveexportpipeline.cpp
    src/presentation/services/screenshotsavepreviewcanvas.cpp
    src/presentation/services/screenshotclipboardservice.cpp
    src/presentation/services/screenshotexportartifact.cpp
    src/presentation/services/screenshotexportcoordinator.cpp
    src/presentation/services/screenshotexportpinnedsource.cpp
    src/presentation/services/screenshotimagefileservice.cpp
    src/presentation/services/screenshotpdfexport.cpp
    src/presentation/ocr/screenshotrecognitionimage.cpp
    src/presentation/ocr/screenshotocrpresentation.cpp
    src/presentation/ocr/screenshotocrtextlayout.cpp
    src/presentation/overlay/screenshotselectionshadowrenderer.cpp)
if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-collage-pdf-document-tests tests/collage_pdf_document_tests.cpp)
    target_link_libraries(snow-shot-collage-pdf-document-tests PRIVATE
        snow_shot_collage_core snow_shot_storage)
    add_test(NAME snow-shot-collage-pdf-document-tests COMMAND snow-shot-collage-pdf-document-tests)
    set_tests_properties(snow-shot-collage-pdf-document-tests PROPERTIES LABELS unit)
    add_executable(snow-shot-collage-pdf-export-tests tests/collage_pdf_export_tests.cpp
        src/presentation/collage/collagepdfexport.cpp
        src/presentation/services/screenshotpdfexport.cpp)
    target_include_directories(snow-shot-collage-pdf-export-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    target_link_libraries(snow-shot-collage-pdf-export-tests PRIVATE
        snow_shot_collage_core snow_shot_storage)
    add_test(NAME snow-shot-collage-pdf-export-tests COMMAND snow-shot-collage-pdf-export-tests)
    set_tests_properties(snow-shot-collage-pdf-export-tests PROPERTIES LABELS unit)
    add_executable(snow-shot-collage-document-tests tests/collage_document_tests.cpp)
    target_link_libraries(snow-shot-collage-document-tests PRIVATE snow_shot_collage_core)
    add_test(NAME snow-shot-collage-document-tests COMMAND snow-shot-collage-document-tests)
    set_tests_properties(snow-shot-collage-document-tests PROPERTIES LABELS unit
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    foreach(_collage_test IN ITEMS editor window)
        add_executable(snow-shot-collage-${_collage_test}-tests
            tests/collage_${_collage_test}_tests.cpp ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
        target_include_directories(snow-shot-collage-${_collage_test}-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/services")
        target_link_libraries(snow-shot-collage-${_collage_test}-tests PRIVATE
            snow_shot_collage snow_shot_diagnostics)
        snow_shot_import_offscreen_platform(snow-shot-collage-${_collage_test}-tests)
        snow_shot_add_translations(snow-shot-collage-${_collage_test}-tests)
        add_test(NAME snow-shot-collage-${_collage_test}-tests
            COMMAND snow-shot-collage-${_collage_test}-tests -platform offscreen)
        set_tests_properties(snow-shot-collage-${_collage_test}-tests PROPERTIES LABELS unit
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    foreach(_collage_pdf_test IN ITEMS grid settings window)
        add_executable(snow-shot-collage-pdf-${_collage_pdf_test}-tests
            tests/collage_pdf_${_collage_pdf_test}_tests.cpp ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
        target_include_directories(snow-shot-collage-pdf-${_collage_pdf_test}-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/services")
        target_link_libraries(snow-shot-collage-pdf-${_collage_pdf_test}-tests PRIVATE
            snow_shot_collage snow_shot_diagnostics)
        snow_shot_import_offscreen_platform(snow-shot-collage-pdf-${_collage_pdf_test}-tests)
        snow_shot_add_translations(snow-shot-collage-pdf-${_collage_pdf_test}-tests)
        add_test(NAME snow-shot-collage-pdf-${_collage_pdf_test}-tests
            COMMAND snow-shot-collage-pdf-${_collage_pdf_test}-tests -platform offscreen)
        set_tests_properties(snow-shot-collage-pdf-${_collage_pdf_test}-tests PROPERTIES LABELS unit
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    foreach(_collage_drop_test IN ITEMS editor window)
        add_test(NAME snow-shot-collage-${_collage_drop_test}-drag-drop-tests
            COMMAND snow-shot-collage-${_collage_drop_test}-tests --drag-drop-only -platform offscreen)
        set_tests_properties(snow-shot-collage-${_collage_drop_test}-drag-drop-tests PROPERTIES
            LABELS unit
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    add_test(NAME snow-shot-collage-selection-outline-tests
        COMMAND snow-shot-collage-editor-tests --selection-outline-only -platform offscreen)
    set_tests_properties(snow-shot-collage-selection-outline-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-footer-tests
        COMMAND snow-shot-collage-window-tests --footer-only -platform offscreen)
    set_tests_properties(snow-shot-collage-footer-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-export-feedback-tests
        COMMAND snow-shot-collage-window-tests --export-feedback-only -platform offscreen)
    set_tests_properties(snow-shot-collage-export-feedback-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-import-feedback-tests
        COMMAND snow-shot-collage-window-tests --import-feedback-only -platform offscreen)
    set_tests_properties(snow-shot-collage-import-feedback-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-file-dialog-tests
        COMMAND snow-shot-collage-window-tests --file-dialog-only -platform offscreen)
    set_tests_properties(snow-shot-collage-file-dialog-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-singleton-tests
        COMMAND snow-shot-collage-window-tests --singleton-only -platform offscreen)
    set_tests_properties(snow-shot-collage-singleton-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-selected-files-tests
        COMMAND snow-shot-collage-window-tests --selected-files-only -platform offscreen)
    set_tests_properties(snow-shot-collage-selected-files-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-layout-control-tests
        COMMAND snow-shot-collage-window-tests --layout-controls-only -platform offscreen)
    set_tests_properties(snow-shot-collage-layout-control-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-fit-control-tests
        COMMAND snow-shot-collage-editor-tests --fit-control-only -platform offscreen)
    set_tests_properties(snow-shot-collage-fit-control-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-thumbnail-action-tests
        COMMAND snow-shot-collage-editor-tests --thumbnail-actions-only -platform offscreen)
    set_tests_properties(snow-shot-collage-thumbnail-action-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-spacing-unit-tests
        COMMAND snow-shot-collage-editor-tests --spacing-units-only -platform offscreen)
    set_tests_properties(snow-shot-collage-spacing-unit-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-empty-state-tests
        COMMAND snow-shot-collage-editor-tests --empty-state-only -platform offscreen)
    set_tests_properties(snow-shot-collage-empty-state-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-history-import-tests
        COMMAND snow-shot-capture-history-repository-tests --collage-only)
    add_test(NAME snow-shot-collage-history-selection-tests
        COMMAND snow-shot-screenshot-history-page-tests --collage-only -platform offscreen)
    add_test(NAME snow-shot-collage-pinned-import-tests
        COMMAND snow-shot-collage-window-tests --pinned-import-only -platform offscreen)
    add_test(NAME snow-shot-collage-live-pinned-import-tests
        COMMAND snow-shot-collage-window-tests --live-pinned-import-only -platform offscreen)
    set_tests_properties(snow-shot-collage-live-pinned-import-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-collage-pinned-import-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-collage-history-import-tests
        snow-shot-collage-history-selection-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
if(SNOW_APPS_BUILD_BENCHMARKS)
    add_executable(snow-shot-collage-pdf-performance-benchmark
        tests/collage_pdf_performance_benchmark.cpp ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
    target_link_libraries(snow-shot-collage-pdf-performance-benchmark PRIVATE
        snow_shot_collage snow_shot_diagnostics)
    target_include_directories(snow-shot-collage-pdf-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    snow_shot_import_offscreen_platform(snow-shot-collage-pdf-performance-benchmark)
    add_executable(snow-shot-collage-performance-benchmark tests/collage_performance_benchmark.cpp
        ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
    target_link_libraries(snow-shot-collage-performance-benchmark PRIVATE
        snow_shot_collage snow_shot_diagnostics)
    target_include_directories(snow-shot-collage-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    if(WIN32)
        target_link_libraries(snow-shot-collage-performance-benchmark PRIVATE psapi)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-collage-performance-benchmark)

    add_executable(snow-shot-collage-rows-performance-benchmark
        tests/collage_rows_performance_benchmark.cpp)
    target_link_libraries(snow-shot-collage-rows-performance-benchmark PRIVATE
        snow_shot_collage_core)
    if(WIN32)
        target_link_libraries(snow-shot-collage-rows-performance-benchmark PRIVATE psapi)
    endif()

    add_executable(snow-shot-collage-workflow-performance-benchmark
        tests/collage_workflow_performance_benchmark.cpp
        ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
    target_link_libraries(snow-shot-collage-workflow-performance-benchmark PRIVATE
        snow_shot_collage snow_shot_diagnostics)
    target_include_directories(snow-shot-collage-workflow-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    snow_shot_import_offscreen_platform(snow-shot-collage-workflow-performance-benchmark)

    add_executable(snow-shot-collage-clipboard-performance-benchmark
        tests/collage_clipboard_performance_benchmark.cpp
        ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
    target_link_libraries(snow-shot-collage-clipboard-performance-benchmark PRIVATE
        snow_shot_collage snow_shot_diagnostics)
    target_include_directories(snow-shot-collage-clipboard-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    snow_shot_import_offscreen_platform(snow-shot-collage-clipboard-performance-benchmark)

    add_executable(snow-shot-collage-import-performance-benchmark
        tests/collage_import_performance_benchmark.cpp
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/pinnedcolorswatch.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp)
    target_link_libraries(snow-shot-collage-import-performance-benchmark PRIVATE
        snow_shot_storage snow_shot_clipboard_placement snow_shot_image_codec
        snow_shot_diagnostics Qt6::Widgets)
    target_include_directories(snow-shot-collage-import-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    snow_shot_import_offscreen_platform(snow-shot-collage-import-performance-benchmark)
    if(WIN32)
        target_link_libraries(snow-shot-collage-workflow-performance-benchmark PRIVATE psapi)
        target_link_libraries(snow-shot-collage-clipboard-performance-benchmark PRIVATE psapi)
        target_link_libraries(snow-shot-collage-import-performance-benchmark PRIVATE psapi)
    endif()
    add_custom_target(snow-shot-collage-workflow-benchmarks DEPENDS
        snow-shot-collage-performance-benchmark
        snow-shot-collage-rows-performance-benchmark
        snow-shot-collage-workflow-performance-benchmark
        snow-shot-collage-import-performance-benchmark
        snow-shot-collage-clipboard-performance-benchmark
        snow-shot-save-to-file-performance-benchmark
        snow_image_memory_performance_benchmark)
endif()

if(SNOW_SHOT_BUILD_TESTS)
    # Compile the focused collage workflows in one dependency graph so MSBuild
    # can share codec/library work while building independent test executables.
    add_custom_target(snow-shot-collage-optimization-tests DEPENDS
        snow-shot-collage-document-tests
        snow-shot-collage-editor-tests
        snow-shot-collage-window-tests
        snow-shot-pinned-window-repository-tests
        snow-shot-clipboard-content-tests
        snow-shot-export-artifact-tests
        snow-shot-export-dialog-workflow-tests)
    if(TARGET snow-canvas-image-tests)
        add_dependencies(snow-shot-collage-optimization-tests snow-canvas-image-tests)
    endif()
    foreach(_collage_native_test IN ITEMS snow_image_core_tests snow_image_raster_export_tests
            snow_image_resource_plan_tests)
        if(TARGET ${_collage_native_test})
            add_dependencies(snow-shot-collage-optimization-tests ${_collage_native_test})
        endif()
    endforeach()
    add_test(NAME snow-shot-collage-source-only-pin-tests
        COMMAND snow-shot-pinned-window-repository-tests --source-only)
    add_test(NAME snow-shot-collage-first-frame-import-tests
        COMMAND snow-shot-clipboard-content-tests --first-frame-only)
    add_test(NAME snow-shot-collage-lazy-row-tests
        COMMAND snow-shot-export-artifact-tests --row-source-only)
    add_test(NAME snow-shot-collage-fused-clipboard-tests
        COMMAND snow-shot-export-artifact-tests --clipboard-fusion-only)
    set_tests_properties(snow-shot-collage-source-only-pin-tests
        snow-shot-collage-first-frame-import-tests
        snow-shot-collage-lazy-row-tests
        snow-shot-collage-fused-clipboard-tests PROPERTIES
        LABELS unit TIMEOUT 90
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(SNOW_APPS_BUILD_BENCHMARKS)
        add_custom_target(snow-shot-collage-validation DEPENDS
            snow-shot-collage-optimization-tests snow-shot-collage-workflow-benchmarks)
    endif()
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-image-split-document-tests tests/imagesplit_document_tests.cpp)
    snow_shot_offscreen_qpa_environment(_SNOW_SHOT_IMAGE_SPLIT_QPA_ENVIRONMENT)
    if(WIN32)
        list(APPEND _SNOW_SHOT_IMAGE_SPLIT_QPA_ENVIRONMENT "QT_QPA_FONTDIR=$ENV{SystemRoot}/Fonts")
    endif()
    target_link_libraries(snow-shot-image-split-document-tests PRIVATE snow_shot_image_split_core)
    add_test(NAME snow-shot-image-split-document-tests COMMAND snow-shot-image-split-document-tests)
    set_tests_properties(snow-shot-image-split-document-tests PROPERTIES LABELS unit
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    foreach(_image_split_test IN ITEMS editor window export)
        add_executable(snow-shot-image-split-${_image_split_test}-tests
            tests/imagesplit_${_image_split_test}_tests.cpp ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
        target_include_directories(snow-shot-image-split-${_image_split_test}-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/services")
        target_link_libraries(snow-shot-image-split-${_image_split_test}-tests PRIVATE
            snow_shot_image_split snow_shot_diagnostics)
        snow_shot_import_offscreen_platform(snow-shot-image-split-${_image_split_test}-tests)
        snow_shot_add_translations(snow-shot-image-split-${_image_split_test}-tests)
        add_test(NAME snow-shot-image-split-${_image_split_test}-tests
            COMMAND snow-shot-image-split-${_image_split_test}-tests -platform offscreen)
        set_tests_properties(snow-shot-image-split-${_image_split_test}-tests PROPERTIES LABELS unit
            ENVIRONMENT "${_SNOW_SHOT_IMAGE_SPLIT_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    add_test(NAME snow-shot-split-selected-images-tests
        COMMAND snow-shot-image-split-window-tests --selected-files-only -platform offscreen)
    set_tests_properties(snow-shot-split-selected-images-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_IMAGE_SPLIT_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-image-paste-tests tests/image_paste_tests.cpp
        ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
    target_include_directories(snow-shot-image-paste-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/services")
    target_link_libraries(snow-shot-image-paste-tests PRIVATE
        snow_shot_image_split snow_shot_collage snow_shot_diagnostics)
    snow_shot_import_offscreen_platform(snow-shot-image-paste-tests)
    snow_shot_add_translations(snow-shot-image-paste-tests)
    add_test(NAME snow-shot-image-paste-tests COMMAND snow-shot-image-paste-tests -platform offscreen)
    set_tests_properties(snow-shot-image-paste-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_IMAGE_SPLIT_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-image-split-preview-tests tests/imagesplit_preview_tests.cpp)
    target_include_directories(snow-shot-image-split-preview-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/imagesplit")
    target_link_libraries(snow-shot-image-split-preview-tests PRIVATE snow_shot_image_split)
    add_test(NAME snow-shot-image-split-preview-tests COMMAND snow-shot-image-split-preview-tests)
    set_tests_properties(snow-shot-image-split-preview-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_IMAGE_SPLIT_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-image-split-canvas-tests
        COMMAND snow-shot-image-split-editor-tests --canvas-only -platform offscreen)
    set_tests_properties(snow-shot-image-split-canvas-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_IMAGE_SPLIT_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-image-split-icon-tests
        COMMAND snow-shot-generated-icon-pack-tests --image-splitting-only)
    add_test(NAME snow-shot-image-split-pinned-source-tests
        COMMAND snow-shot-pinned-window-tests --splitting-snapshot-only -platform offscreen)
    set_tests_properties(snow-shot-image-split-icon-tests snow-shot-image-split-pinned-source-tests
        PROPERTIES LABELS unit ENVIRONMENT "${_SNOW_SHOT_IMAGE_SPLIT_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        set_tests_properties(snow-shot-image-split-icon-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>")
    endif()
endif()
if(SNOW_APPS_BUILD_BENCHMARKS)
    add_executable(snow-shot-image-split-performance-benchmark
        tests/imagesplit_performance_benchmark.cpp ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
    target_link_libraries(snow-shot-image-split-performance-benchmark PRIVATE
        snow_shot_image_split snow_shot_diagnostics)
    target_include_directories(snow-shot-image-split-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/imagesplit")
    if(WIN32)
        target_link_libraries(snow-shot-image-split-performance-benchmark PRIVATE psapi)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-image-split-performance-benchmark)
    # Capture these two Release libraries before a change to compare new scenarios against the
    # actual previous implementation with the same fixture, dependencies and measurement code.
    set(SNOW_SHOT_IMAGE_SPLIT_BASELINE_LIBRARY_DIR "" CACHE PATH
        "Directory containing captured Release Image Splitting libraries for comparison")
    if(SNOW_SHOT_IMAGE_SPLIT_BASELINE_LIBRARY_DIR)
        foreach(_part IN ITEMS core ui)
            add_library(snow_shot_image_split_baseline_${_part} STATIC IMPORTED)
        endforeach()
        set_target_properties(snow_shot_image_split_baseline_core PROPERTIES
            IMPORTED_LOCATION "${SNOW_SHOT_IMAGE_SPLIT_BASELINE_LIBRARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}snow_shot_image_split_core${CMAKE_STATIC_LIBRARY_SUFFIX}"
            INTERFACE_LINK_LIBRARIES Qt6::Widgets)
        set_target_properties(snow_shot_image_split_baseline_ui PROPERTIES
            IMPORTED_LOCATION "${SNOW_SHOT_IMAGE_SPLIT_BASELINE_LIBRARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}snow_shot_image_split${CMAKE_STATIC_LIBRARY_SUFFIX}"
            INTERFACE_LINK_LIBRARIES "snow_shot_image_split_baseline_core;snow_shot_collage_core;snow_shot_settings")
        add_executable(snow-shot-image-split-baseline-performance-benchmark
            tests/imagesplit_performance_benchmark.cpp ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES})
        target_compile_definitions(snow-shot-image-split-baseline-performance-benchmark PRIVATE
            SNOW_SHOT_IMAGE_SPLIT_BASELINE=1)
        target_include_directories(snow-shot-image-split-baseline-performance-benchmark PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/imagesplit")
        target_link_libraries(snow-shot-image-split-baseline-performance-benchmark PRIVATE
            snow_shot_image_split_baseline_ui snow_shot_diagnostics)
        if(TARGET snow_shot_full_rust_ffi_bundle)
            set_property(TARGET snow-shot-image-split-baseline-performance-benchmark PROPERTY
                SNOW_RUST_BUNDLE_OVERRIDE snow_shot_full_rust_ffi_bundle)
        endif()
        if(WIN32)
            target_link_libraries(snow-shot-image-split-baseline-performance-benchmark PRIVATE psapi)
        endif()
        snow_shot_import_offscreen_platform(snow-shot-image-split-baseline-performance-benchmark)
    endif()
endif()

set(PROJECT_HEADERS
    include/snow_shot/app/mcp/screenshotmcpsession.h
    include/snow_shot/app/mcp/screenshotmcpserver.h
    include/snow_shot/app/mcp/mcpapplicationservice.h
    include/snow_shot/app/mcp/mcpdocumentservice.h
    include/snow_shot/app/mcp/mcpjobregistry.h
    include/snow_shot/app/mcp/mcpmediaservice.h
    include/snow_shot/app/featureavailability.h
    include/snow_shot/app/applicationinputguard.h
    include/snow_shot/presentation/screenshotencodingsettings.h
    include/snow_shot/presentation/directcapturecontroller.h
    include/snow_shot/presentation/directcaptureworkflow.h
    include/snow_shot/presentation/directcapturehistory.h
    include/snow_shot/presentation/screenshottabledocument.h
    include/snow_shot/presentation/screenshottableeditor.h
    include/snow_shot/app/applicationcontroller.h
    include/snow_shot/app/applicationrestart.h
    include/snow_shot/app/updateconfirmationdialog.h
    include/snow_shot/app/singleinstancecoordinator.h
    include/snow_shot/platform/windows/autostartregistration.h
    include/snow_shot/platform/windows/focusedfullscreenwindow.h
    include/snow_shot/platform/windows/monitorgeometry.h
    include/snow_shot/platform/windows/windowchrome.h
    include/snow_shot/platform/windowcaptureexclusion.h
    include/snow_shot/platform/physicalcursor.h
    include/snow_shot/presentation/components/aspectratiolockbutton.h
    include/snow_shot/presentation/components/contentcardwidget.h
    include/snow_shot/presentation/components/maincontentheaderwidget.h
    include/snow_shot/presentation/components/icons/snowshoticons.h
    include/snow_shot/presentation/components/settingspageutils.h
    include/snow_shot/presentation/components/sidebarwidget.h
    include/snow_shot/presentation/components/titlebarwidget.h
    include/snow_shot/presentation/mainwindow.h
    include/snow_shot/presentation/screenshotcapturecoordinator.h
    include/snow_shot/presentation/screenshotcapturedisplaymodelreconciler.h
    include/snow_shot/presentation/screenshotcaptureruntimeadapter.h
    include/snow_shot/presentation/screenshotcapturestate.h
    include/snow_shot/presentation/screenshotcaptureworkflow.h
    include/snow_shot/presentation/screenshotstartupcontext.h
    include/snow_shot/presentation/screenshotcanvascolorsampler.h
    include/snow_shot/presentation/screenshotcanvascolorsamplerwindow.h
    include/snow_shot/presentation/screenshotcaptureworkflowports.h
    include/snow_shot/presentation/screenshotcanvasrenderer.h
    include/snow_shot/presentation/screenshotclipboardcontent.h
    include/snow_shot/presentation/screenshotclipboardservice.h
    include/snow_shot/presentation/screenshotresultcompositor.h
    include/snow_shot/presentation/screenshotselectionshadowrenderer.h
    include/snow_shot/presentation/screenshotshadowprofile.h
    include/snow_shot/presentation/screenshotcolorpickercontroller.h
    include/snow_shot/presentation/screenshotcolorpickerwindow.h
    include/snow_shot/presentation/screenshotcontroller.h
    include/snow_shot/presentation/screenshotdialogowner.h
    include/snow_shot/presentation/screenshotshortcutexitconfirmation.h
    include/snow_shot/presentation/windowgroupswitchercontroller.h
    include/snow_shot/presentation/windowgroupswitcherpopup.h
    include/snow_shot/presentation/pinnedwindowgroupmanager.h
    include/snow_shot/presentation/globalshortcuttypes.h
    include/snow_shot/presentation/screenshotdefaultstyles.h
    include/snow_shot/presentation/canvasdrawingpreferences.h
    include/snow_shot/presentation/screenshotcanvastoolstyles.h
    include/snow_shot/presentation/screenshotdisplayconfigurationobserver.h
    include/snow_shot/presentation/screenshotdisplaysession.h
    include/snow_shot/presentation/screenshotexportservice.h
    include/snow_shot/presentation/screenshotexportartifact.h
    include/snow_shot/presentation/screenshotexportcoordinator.h
    include/snow_shot/presentation/screenshotimagefileservice.h
    include/snow_shot/presentation/screenshotimagerowsource.h
    include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
    include/snow_shot/presentation/historypinplacement.h
    include/snow_shot/presentation/screenshotselectionpin.h
    include/snow_shot/presentation/screenshotgeometry.h
    include/snow_shot/presentation/screenshotguidelinerendering.h
    include/snow_shot/presentation/screenshothistoryservice.h
    include/snow_shot/presentation/screenshothistorytypes.h
    include/snow_shot/presentation/screenshotintelligentselectionmodel.h
    include/snow_shot/presentation/screenshotinteractionstate.h
    include/snow_shot/presentation/screenshotocrcontroller.h
    include/snow_shot/presentation/screenshotocrrecognitionservice.h
    include/snow_shot/presentation/screenshotocrassets.h
    include/snow_shot/presentation/screenshotrecognitionresults.h
    include/snow_shot/presentation/screenshotqrrecognitionservice.h
    include/snow_shot/presentation/screenshotmessageservice.h
    include/snow_shot/presentation/screenshotocrpresentation.h
    include/snow_shot/ocrtextoptions.h
    include/snow_shot/presentation/screenshotocrtexteditingsession.h
    include/snow_shot/presentation/screenshotocrvisuals.h
    include/snow_shot/presentation/screenshotocrtextlayer.h
    include/snow_shot/presentation/screenshotrecognitionwindow.h
    include/snow_shot/presentation/screenshotrecognitionsessioncontroller.h
    include/snow_shot/presentation/screenshotpinnededitcontroller.h
    include/snow_shot/presentation/screenshotpinnedwindow.h
    include/snow_shot/presentation/screenshotpresentationservices.h
    include/snow_shot/presentation/screenshotshortcuthints.h
    include/snow_shot/presentation/screenshotsmartselectiontransition.h
    include/snow_shot/presentation/screenshotselectioneditworkflow.h
    include/snow_shot/presentation/screenshotselectioneditworkflowports.h
    include/snow_shot/presentation/screenshotselectionexportuiservices.h
    include/snow_shot/presentation/screenshotselectiongeometry.h
    include/snow_shot/presentation/screenshotselectionlimits.h
    include/snow_shot/image/screenshotregiongeometry.h
    include/snow_shot/image/screenshotregionpoints.h
    include/snow_shot/presentation/screenshotregionpreferences.h
    include/snow_shot/presentation/screenshotregiontypecontrol.h
    include/snow_shot/presentation/screenshotselectionmodel.h
    include/snow_shot/presentation/screenshotselectionexportworkflowports.h
    include/snow_shot/presentation/screenshotselectionparams.h
    include/snow_shot/presentation/screenshotselectionresizemodalcontent.h
    include/snow_shot/presentation/screenshotselectionresizeworkflow.h
    include/snow_shot/presentation/screenshotsaveasfiledialog.h
    include/snow_shot/presentation/screenshotselectionsettingsstore.h
    include/snow_shot/presentation/screenshotsourceimagecomposer.h
    include/snow_shot/presentation/screenshotselectiontoolbarwidget.h
    include/snow_shot/presentation/screenshotscrollingcapturecontroller.h
    include/snow_shot/presentation/screenshotscrollingsnapshot.h
    include/snow_shot/presentation/screenshotscrollingtypes.h
    include/snow_shot/presentation/screenshotscrollingthumbnailwidget.h
    include/snow_shot/presentation/screenshotoverlaycanvaspresenter.h
    include/snow_shot/presentation/screenshotoverlaycoordinator.h
    include/snow_shot/presentation/screenshotoverlayeventsink.h
    include/snow_shot/presentation/screenshotoverlayinteractionadapter.h
    include/snow_shot/presentation/screenshotoverlayinputhandler.h
    include/snow_shot/presentation/screenshotoverlayshortcutcontroller.h
    include/snow_shot/presentation/screenshotoverlaypool.h
    include/snow_shot/presentation/screenshotoverlayuihost.h
    include/snow_shot/presentation/screenshotoverlaywindow.h
    include/snow_shot/presentation/screenshotselectorcoordinator.h
    src/presentation/selector/screenshotselectorpolicy.h
    src/presentation/selector/screenshotselectordisplaygeometry.h
    include/snow_shot/presentation/screenshotselectorworkflow.h
    include/snow_shot/presentation/screenshotselectorworkflowports.h
    include/snow_shot/presentation/screenshottoolbarcommands.h
    include/snow_shot/presentation/screenshottoolbarpresenter.h
    include/snow_shot/presentation/screenshottoolbarpresentationstatefactory.h
    include/snow_shot/presentation/screenshottoolcommandworkflow.h
    include/snow_shot/presentation/screenshottoolcommandworkflowports.h
    include/snow_shot/presentation/screenshottoolbarmainpanel.h
    include/snow_shot/presentation/screenshottoolpalette.h
    include/snow_shot/presentation/screenshottoolpalettehost.h
    include/snow_shot/presentation/screenshottoolbarwindow.h
    include/snow_shot/presentation/screenshottypes.h
    include/snow_shot/presentation/screenshotuipreferences.h
    include/snow_shot/presentation/screenshotselectiondisplayunit.h
    include/snow_shot/presentation/screenrecordingareawindow.h
    include/snow_shot/presentation/screenrecordingcontroller.h
    include/snow_shot/presentation/screenrecordingfolder.h
    include/snow_shot/presentation/screenrecordingshortcutcontroller.h
    include/snow_shot/presentation/screenrecordingtoolbarwindow.h
    include/snow_shot/presentation/styles/mainwindowcomponenttoken.h
    include/snow_shot/presentation/styles/buttonborder.h
    include/snow_shot/presentation/styles/themecolorscheme.h
    include/snow_shot/presentation/floatingtoolbarcontroller.h
    include/snow_shot/presentation/systemtraycontroller.h
)

add_library(snow_shot_airdrop STATIC src/presentation/services/screenshotairdropservice.cpp)
snow_shot_register_source_capability(airdrop
    FULL_TARGET snow_shot_airdrop MINI_TARGET snow_shot_airdrop_mini)
snow_shot_source_requires(airdrop SOURCES
    src/presentation/services/screenshotcontroller.cpp
    src/presentation/pinned/screenshotpinnedwindow.cpp)
target_include_directories(snow_shot_airdrop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_airdrop PUBLIC Qt6::Widgets PRIVATE Qt6::Concurrent snow_shot_diagnostics)
if(APPLE)
    target_sources(snow_shot_airdrop PRIVATE src/platform/macos/nativeairdrop.mm)
    target_compile_options(snow_shot_airdrop PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(snow_shot_airdrop PRIVATE "-framework AppKit")
else()
    target_sources(snow_shot_airdrop PRIVATE src/presentation/services/nativeairdropunsupported.cpp)
endif()

add_library(snow_shot_print STATIC src/presentation/services/screenshotprintservice.cpp)
snow_shot_register_source_capability(print
    FULL_TARGET snow_shot_print MINI_TARGET snow_shot_print_mini)
snow_shot_source_requires(print SOURCES
    src/presentation/services/screenshotcontroller.cpp
    src/presentation/pinned/screenshotpinnedwindow.cpp
    src/presentation/imageediting/imageeditingexportcontroller.cpp)
target_include_directories(snow_shot_print PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_print PUBLIC Qt6::Widgets PRIVATE Qt6::Concurrent snow_shot_diagnostics)
if(WIN32)
    target_sources(snow_shot_print PRIVATE src/platform/windows/nativeprint.cpp
        src/platform/windows/nativeprintdocument.cpp)
    target_link_libraries(snow_shot_print PRIVATE shell32 uuid ole32 runtimeobject windowsapp
        d2d1 d3d11 dxgi windowscodecs)
elseif(APPLE)
    target_sources(snow_shot_print PRIVATE src/platform/macos/nativeprint.mm)
    target_compile_options(snow_shot_print PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(snow_shot_print PRIVATE "-framework AppKit")
else()
    target_sources(snow_shot_print PRIVATE src/presentation/services/nativeprintunsupported.cpp)
endif()

add_library(snow_shot_export_naming STATIC)
target_include_directories(snow_shot_export_naming PUBLIC include)
target_link_libraries(snow_shot_export_naming PUBLIC Qt6::Gui)
if(APPLE)
    target_sources(snow_shot_export_naming PRIVATE src/platform/macos/screenshotwindownames.mm)
    target_link_libraries(snow_shot_export_naming PRIVATE "-framework AppKit" "-framework CoreGraphics")
else()
    target_sources(snow_shot_export_naming PRIVATE src/platform/windows/screenshotwindownames.cpp)
    if(WIN32)
        target_link_libraries(snow_shot_export_naming PRIVATE dwmapi version)
    endif()
endif()

set(SNOW_SHOT_IMAGE_EDITING_SOURCES
    include/snow_shot/presentation/imageeditingdocument.h
    include/snow_shot/presentation/imageeditingcanvas.h
    include/snow_shot/presentation/imageeditingwindow.h
    src/presentation/imageediting/imageeditingdocument.cpp
    src/presentation/imageediting/imageeditingexportcontroller.cpp
    src/presentation/imageediting/imageeditingexportcontroller.h
    src/presentation/imageediting/imageeditingsource.cpp
    src/presentation/imageediting/imageeditingcanvas.cpp
    src/presentation/imageediting/imageeditingwindow.cpp)

set(PROJECT_CXX_SOURCES
    ${SNOW_SHOT_IMAGE_EDITING_SOURCES}
    ${SNOW_SELECTED_FILES_SOURCES}
    src/presentation/services/screenshotfilepinbatch.cpp
    src/presentation/capture/directcapturecontroller.cpp
    src/presentation/capture/directcapturenative.cpp
    src/presentation/capture/directcaptureworkflow.cpp
    src/presentation/capture/directcapturehistory.cpp
    src/presentation/capture/camerashuttersound.cpp
    src/app/applicationcontroller.cpp
    src/app/updateconfirmationdialog.cpp
    src/app/mcp/screenshotmcpsession.cpp
    src/app/mcp/screenshotmcpserver.cpp
    src/app/mcp/mcpapplicationservice.cpp
    src/app/mcp/mcpdocumentservice.cpp
    src/app/mcp/mcpjobregistry.cpp
    src/app/mcp/mcpmediaservice.cpp
    src/app/mcp/mcppinnedstate.cpp
    src/app/applicationrestart.cpp
    src/app/featureavailability.cpp
    src/app/main.cpp
    src/app/singleinstancecoordinator.cpp
    src/platform/physicalcursor.cpp
    src/platform/windows/windowchrome.cpp
    src/platform/windows/windowcursorrefresh.h
    src/platform/windows/scrollinput.cpp
    src/presentation/components/contentcardwidget.cpp
    src/presentation/components/maincontentheaderwidget.cpp
    src/presentation/components/sidebarwidget.cpp
    src/presentation/components/titlebarwidget.cpp
    src/presentation/mainwindow.cpp
    src/presentation/capture/screenshotcapturecoordinator.cpp
    src/presentation/capture/screenshotcapturedisplaymodelreconciler.cpp
    src/presentation/capture/screenshotcaptureruntimeadapter.cpp
    src/presentation/capture/screenshotcaptureworker.cpp
    src/presentation/capture/screenshotcapturepolicy.cpp
    src/presentation/capture/screenshotcaptureperfinstrumentation.cpp
    src/presentation/capture/screenshotcaptureworkflow.cpp
    src/presentation/capture/screenshotglobalmousedrag.cpp
    src/presentation/capture/adaptivescrollingcapturecadence.h
    src/presentation/capture/windowcaptureexclusion.h
    src/presentation/capture/windowinputtransparency.h
    src/presentation/capture/latestframemailbox.h
    src/presentation/capture/screenshotscrollingcapturecontroller.cpp
    src/presentation/capture/screenshotscrollingpipeline.cpp
    src/presentation/capture/screenshotscrollingnativesource.cpp
    src/presentation/capture/screenshotscrollingthumbnailwidget.cpp
    src/presentation/capture/scrollinghoverpreview.h
    src/presentation/services/screenshotcanvascolorsampler.cpp
    src/presentation/services/screenshotcanvascolorsamplerwindow.cpp
    src/presentation/services/screenshotcolorpickercontroller.cpp
    src/presentation/services/screenshotclipboardservice.cpp
    src/presentation/services/screenshotcolorpickerwindow.cpp
    src/presentation/services/screenshotcontroller.cpp
    src/presentation/services/screenshotshortcutexitconfirmation.cpp
    src/presentation/services/screenshotexportcoordinator.cpp
    src/presentation/services/screenshotmessageservice.cpp
    src/presentation/services/screenshothistoryservice.cpp
    src/presentation/services/screenshotdisplayconfigurationobserver.cpp
    src/presentation/services/screenshotexportservice.cpp
    src/presentation/services/screenshotmanualsave.cpp
    src/presentation/services/screenshotexportartifact.cpp
    src/presentation/services/screenshotexportpinnedsource.cpp
    src/presentation/ocr/screenshotrecognitionimage.cpp
    src/presentation/services/screenshotexportscrollingsource.cpp
    src/presentation/services/screenshotimagefileservice.cpp
    src/presentation/services/screenshotrecognitionfileexport.cpp
        src/presentation/services/screenshotpdfexport.cpp
    src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
    src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
    src/presentation/core/screenshotcanvastoolstyles.cpp
    src/presentation/core/screenshotstylebinding.cpp
    src/presentation/core/screenshotintelligentselectionmodel.cpp
    src/presentation/core/screenshotinteractionstate.cpp
    src/presentation/core/screenshotsmartselectiontransition.cpp
    src/presentation/ocr/screenshotocrcontroller.cpp
    src/presentation/ocr/screenshotrecognitionsessioncontroller.cpp
    src/presentation/ocr/screenshotocrlayout.cpp
    src/presentation/ocr/screenshotrecognitionwindow.cpp
    src/presentation/ocr/screenshottabledocument.cpp
    src/presentation/ocr/screenshottableeditor.cpp
    src/presentation/ocr/screenshotocrtexttransform.cpp
    src/presentation/ocr/screenshotocrrecognitionservice.cpp
    src/presentation/ocr/screenshotocrassets.cpp
    src/presentation/ocr/screenshotqrcontroller.cpp
    include/snow_shot/presentation/screenshotqrcontroller.h
    src/presentation/ocr/screenshotqrrecognitionservice.cpp
    src/presentation/ocr/screenshotocrpresentation.cpp
    src/presentation/ocr/screenshotocrvisuals.cpp
    src/presentation/ocr/screenshotocrtexteditingsession.cpp
    src/presentation/pinned/screenshotpinnednativegeometrycontroller.cpp
    src/presentation/pinned/screenshotpinnednativegeometrycontroller.h
    src/presentation/pinned/screenshotpinnedclickthroughgeometry.cpp
    src/presentation/pinned/screenshotpinnedclickthroughgeometry.h
    src/presentation/pinned/screenshotpinnedinteraction.cpp
    include/snow_shot/presentation/pinnedwindowselectioncontroller.h
    src/presentation/pinned/pinnedwindowselectioncontroller.cpp
    src/presentation/pinned/pinnedwindowselectioninteraction.cpp
    src/presentation/pinned/pinnedwindowselectiongeometry.cpp
    src/presentation/pinned/screenshotpinneddragexport.cpp
    ${SNOW_PINNED_PLATFORM_SOURCES}
    src/presentation/pinned/screenshotpinnededitcontroller.cpp
    include/snow_shot/presentation/globalcanvascontroller.h
    src/presentation/globalcanvas/globalcanvascontroller.cpp
    src/presentation/globalcanvas/globalcanvasplatform.cpp
    src/presentation/pinned/screenshotpinnedresizegeometry.cpp
    src/presentation/pinned/screenshotpinnedrestoregeometry.cpp
    src/presentation/pinned/screenshotpinnedhidetotopcontroller.cpp
    src/presentation/pinned/screenshotpinnedwindow.cpp
    src/presentation/pinned/screenshotpinnedbatchsave.cpp
    src/presentation/pinned/screenshotpintoperfinstrumentation.cpp
    src/presentation/services/screenshotpresentationservices.cpp
    src/presentation/services/screenshotpresentationframescheduler.cpp
    src/presentation/services/windowgroupswitchercontroller.cpp
    src/presentation/components/windowgroupswitcherpopup.cpp
    src/presentation/services/pinnedwindowgroupmanager.cpp
    src/presentation/services/screenshotsourceimagecomposer.cpp
    src/presentation/services/systemtraycontroller.cpp
    src/presentation/selection/screenshotselectioneditworkflow.cpp
    src/presentation/selection/screenshotselectionexportuiservices.cpp
    src/presentation/selection/screenshotselectiongeometry.cpp
    src/presentation/selection/screenshotselectionmodel.cpp
    src/presentation/selection/screenshotselectionparams.cpp
    src/presentation/selection/screenshotselectionresizemodalcontent.cpp
    src/presentation/selection/screenshotselectionresizeworkflow.cpp
    src/presentation/services/screenshotsaveasfiledialog.cpp
    src/presentation/services/screenshotsavefooter.h
    src/presentation/services/screenshotsavefooter.cpp
    src/presentation/services/screenshotsaveresult.h
    src/presentation/services/screenshotsaveresult.cpp
    src/presentation/services/screenshotsaveexportpipeline.cpp
    include/snow_shot/presentation/screenshotsavepreviewcanvas.h
    src/presentation/services/screenshotsavepreviewcanvas.cpp
    src/presentation/selection/screenshotselectionsettingsstore.cpp
    src/presentation/selection/screenshotselectiontoolbarwidget.cpp
    src/presentation/selection/screenshotselectiontoolbarwidgets.cpp
    src/presentation/overlay/screenshotoverlaycanvaspresenter.cpp
    src/presentation/overlay/screenshotoverlaycanvaswidget.cpp
    src/presentation/overlay/screenshotoverlaycanvaswidget.h
    src/presentation/ocr/screenshotocrtextlayout.cpp
    src/presentation/overlay/screenshotcanvasrenderer.cpp
    src/presentation/overlay/screenshotoverlaycoordinator.cpp
    src/presentation/overlay/screenshotoverlayinteractionadapter.cpp
    src/presentation/overlay/screenshotoverlayinputhandler.cpp
    src/presentation/overlay/screenshotoverlayshortcutcontroller.cpp
    src/presentation/overlay/screenshotoverlaypool.cpp
    src/presentation/overlay/screenshotoverlayuihost.cpp
    src/presentation/overlay/screenshotoverlayframepresenter.cpp
    src/presentation/overlay/screenshotoverlayframepresenter.h
    src/presentation/overlay/screenshotoverlaywindow.cpp
    src/presentation/selector/screenshotselectorserviceclient.cpp
    src/presentation/selector/screenshotselectorcoordinator.cpp
    src/presentation/selector/screenshotselectorworkflow.cpp
    src/presentation/toolbar/screenshottoolbarpresenter.cpp
    src/presentation/toolbar/screenshottoolbarpresentationstatefactory.cpp
    src/presentation/tools/screenshotautofiltercontroller.cpp
    include/snow_shot/presentation/screenshotautofiltercontroller.h
    src/presentation/tools/screenshottoolcommandworkflow.cpp
    src/presentation/tools/screenshottoolbarmainpanel.cpp
    src/presentation/tools/screenshottoolpalettebuttons.cpp
    src/presentation/tools/screenshottoolpalettestylecontrols.cpp
    src/presentation/tools/screenshottoolpalettestylecomponents.cpp
    src/presentation/tools/screenshottoolpalettestylemodel.cpp
    src/presentation/tools/screenshottoolpalettestylepresets.cpp
    src/presentation/tools/screenshottoolpalette.cpp
    src/presentation/recording/recordingaudiogainpopover.cpp
    src/presentation/recording/recordingaudiogainpopover.h
    src/presentation/tools/screenshottoolpalettehost.cpp
    src/presentation/toolbar/floatingtoolbarcontroller.cpp
    src/presentation/toolbar/screenshottoolbarwindow.cpp
    src/presentation/recording/screenrecordingareawindow.cpp
    src/presentation/recording/recordingtrimtoolbar.cpp
    src/presentation/recording/recordingtrimsession.cpp
    src/presentation/recording/recordingrenderdialog.cpp
    src/presentation/recording/recordingrenderdialog.h
    src/presentation/recording/recordingrenderjob.cpp
    src/presentation/recording/recordingrenderjob.h
    src/presentation/recording/screenrecordingcontroller.cpp
    src/presentation/recording/recordingcolorsampler.cpp
    src/presentation/recording/recordingcolorsampler.h
    src/presentation/recording/screenrecordingfolder.cpp
    src/presentation/recording/recordingcountdownoverlay.cpp
    src/presentation/recording/recordingcountdownoverlay.h
        src/presentation/recording/recordingeffectpreview.cpp
        src/presentation/recording/recordingeffectpreview.h
        src/presentation/recording/recordingeffectstyle.h
    src/presentation/recording/screenrecordingshortcutcontroller.cpp
    src/presentation/recording/screenrecordinggeometry.cpp
    src/presentation/recording/screenrecordingtoolbarwindow.cpp
)
if(APPLE)
    list(APPEND PROJECT_CXX_SOURCES
        src/platform/macos/applicationactivation.mm
        src/platform/macos/systemtraymenu.mm
        src/platform/macos/windowcaptureexclusion.mm)
    list(APPEND PROJECT_HEADERS include/snow_shot/platform/macos/applicationactivation.h)
endif()

include(SnowSimd)
snow_configure_avx2_sources(src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp)

set(PROJECT_RCC_SOURCES)

set(PROJECT_SOURCES
    ${PROJECT_CXX_SOURCES}
    ${PROJECT_HEADERS}
    ${PROJECT_RCC_SOURCES}
    ${TS_FILES}
)

qt_add_executable(snow_shot
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
)
snow_shot_link_macos_window_platform(snow_shot)
if(WIN32 AND SNOW_SHOT_DISTRIBUTION_CHANNEL STREQUAL "store")
    add_dependencies(snow_shot snow_shot_explorer_command)
endif()
if(SNOW_SHOT_BUILD_TESTS)
    target_compile_definitions(snow_shot PRIVATE SNOW_SHOT_MCP_TEST_FIXTURE=1)
    target_compile_definitions(snow_shot PRIVATE SNOW_SHOT_CONTROLLER_LIFETIME_TESTS=1)
    target_sources(snow_shot PRIVATE tests/screenshot_controller_lifetime_tests.cpp
        tests/screenshot_controller_clipboard_tests.cpp
        tests/mcp_application_lifecycle_tests.cpp)
    snow_shot_import_offscreen_platform(snow_shot)
endif()

if(WIN32)
    configure_file(
        resources/windows-app-resource.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/windows-app-resource.rc"
        @ONLY
    )
    target_sources(snow_shot PRIVATE
        resources/app-icon.ico
        "${CMAKE_CURRENT_BINARY_DIR}/generated/windows-app-resource.rc"
    )
endif()

target_compile_definitions(snow_shot PRIVATE
    SNOW_SHOT_API_BASE_URL=\"${SNOW_SHOT_API_BASE_URL}\"
)
if(WIN32)
    target_compile_definitions(snow_shot PRIVATE
        SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE=\"${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}\"
    )
endif()
target_link_libraries(snow_shot PRIVATE
    snow_shot_export_naming
    snow_shot_collage
    snow_shot_image_split
    snow_shot_animation_playback
    snow_shot_memory_optimization
    snow_shot_crash_bridge
    snow_shot_image_codec
    MINIZIP::minizip-ng
)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowShotUpdates.cmake")
if(WIN32)
    target_link_libraries(snow_shot PRIVATE ole32 oleaut32 shell32 uuid)
endif()
target_compile_definitions(snow_shot PRIVATE
    QT_MESSAGELOGCONTEXT
    SNOW_DIAGNOSTICS_VERSION="${SNOW_SHOT_VERSION}"
    SNOW_DIAGNOSTICS_REVISION="${SNOW_DIAGNOSTICS_REVISION}"
    SNOW_DIAGNOSTICS_BUILD="$<CONFIG>")
if(WIN32)
    add_custom_command(TARGET snow_shot POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SNOW_CRASHPAD_HANDLER}"
            "$<TARGET_FILE_DIR:snow_shot>/crashpad_handler.exe")
    install(PROGRAMS "${SNOW_CRASHPAD_HANDLER}" DESTINATION bin)
endif()
if(MSVC)
    target_compile_options(snow_shot PRIVATE $<$<CONFIG:Release>:/Z7>)
    target_link_options(snow_shot PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL>)
endif()

target_link_libraries(snow_shot PRIVATE snow_shot_system_notification_controller)
if(APPLE)
    target_link_libraries(snow_shot PRIVATE ${SNOW_SELECTED_FILES_LIBRARIES})
    target_link_libraries(snow_shot PRIVATE snow_shot_system_notifications)
    target_sources(snow_shot PRIVATE src/platform/macos/camerashuttersound.mm)
    target_link_libraries(snow_shot PRIVATE "-framework AVFoundation" "-framework Foundation" objc)
    qt_add_resources(snow_shot "snow_shot_shutter_audio"
        PREFIX "/snow-shot"
        BASE "resources"
        FILES "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}")
endif()

if(WIN32 AND EXISTS "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}")
    add_custom_command(TARGET snow_shot POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:snow_shot>/audios"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}"
            "$<TARGET_FILE_DIR:snow_shot>/audios/camera_shutter.mp3"
        COMMENT "Staging the camera shutter audio"
        VERBATIM
    )
elseif(WIN32)
    message(WARNING
        "Camera shutter audio was not found: ${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}")
endif()

qt_add_resources(snow_shot "snow_shot_app_assets"
    PREFIX "/snow-shot/app-icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-default.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-light.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-dark.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-snow-default.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-snow-light.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-snow-dark.png"
)

function(snow_shot_add_image_codec_smoke_test)
    add_executable(snow-shot-image-codec-backend-smoke-tests
        tests/snowimage_codec_backend_smoke_tests.cpp
    )
    target_link_libraries(snow-shot-image-codec-backend-smoke-tests PRIVATE
        snow_shot_image_codec_backend
    )
    set_target_properties(snow-shot-image-codec-backend-smoke-tests PROPERTIES
        AUTOMOC OFF
        AUTORCC OFF
        AUTOUIC OFF
    )
endfunction()

if(SNOW_SHOT_BUILD_IMAGE_CODEC_SMOKE_TEST AND NOT SNOW_SHOT_BUILD_TESTS)
    snow_shot_add_image_codec_smoke_test()
    set_target_properties(snow-shot-image-codec-backend-smoke-tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/verification-bin"
    )
endif()

set(SNOW_SHOT_RECOGNITION_IMAGE_RENDER_SOURCES
    src/presentation/ocr/screenshotrecognitionimage.cpp
    src/presentation/ocr/screenshotocrtextlayout.cpp
    src/presentation/ocr/screenshotocrpresentation.cpp
    src/presentation/overlay/screenshotselectionshadowrenderer.cpp)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowShotRuntimeTests.cmake")

if(SNOW_SHOT_BUILD_TESTS)
    # Private C ABI names keep these fixtures' mocks distinct from the real
    # exports in the Rust bundle used by unrelated transitive dependencies.
    function(snow_shot_alias_test_rust_ffi target)
        foreach(symbol IN LISTS ARGN)
            string(REGEX REPLACE "^snow_" "snow_test_" alias "${symbol}")
            target_compile_definitions(${target} PRIVATE "${symbol}=${alias}")
        endforeach()
    endfunction()

    add_test(NAME snow-shot-translation-catalog-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/translation_catalog_tests.py")
    set_tests_properties(snow-shot-translation-catalog-tests PROPERTIES LABELS "unit")

    add_test(NAME snow-shot-recording-build-profiles-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-recording-build-profiles.py")
    set_tests_properties(snow-shot-recording-build-profiles-tests PROPERTIES
        LABELS "unit" TIMEOUT 180)

    if(WIN32)
        add_test(NAME snow-shot-windows-architecture-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-windows-architecture.py")
        set_tests_properties(snow-shot-windows-architecture-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 120)
        find_program(SNOW_SHOT_INSTALLER_TEST_POWERSHELL NAMES pwsh powershell REQUIRED)
        add_test(NAME snow-shot-msix-packaging-tests
            COMMAND "${SNOW_SHOT_INSTALLER_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-snow-shot-msix.ps1")
        set_tests_properties(snow-shot-msix-packaging-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 120)
        add_test(NAME snow-shot-installer-running-app-tests
            COMMAND "${SNOW_SHOT_INSTALLER_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-snow-shot-installer.ps1")
        set_tests_properties(snow-shot-installer-running-app-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 180)
        if(TARGET snow-shot-updater-binary)
            add_test(NAME snow-shot-installer-upgrade-tests
                COMMAND "${SNOW_SHOT_INSTALLER_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-snow-shot-installer-upgrade.ps1"
                    -HelperPath "$<TARGET_FILE:snow-shot-updater-binary>"
                    -Architecture "${SNOW_WINDOWS_ARCHITECTURE}")
            set_tests_properties(snow-shot-installer-upgrade-tests PROPERTIES
                LABELS "unit;windows" TIMEOUT 180)
        endif()
        add_test(NAME snow-shot-installer-i18n-tests
            COMMAND "${SNOW_SHOT_INSTALLER_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-snow-shot-installer-i18n.ps1")
        set_tests_properties(snow-shot-installer-i18n-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 180)
        add_test(NAME snow-shot-package-compression-tests
            COMMAND "${SNOW_SHOT_INSTALLER_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-snow-shot-package-compression.ps1")
        set_tests_properties(snow-shot-package-compression-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 60)
    endif()
    if(DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(_SNOW_SHOT_RUNTIME_OUTPUT_DIRECTORY_WAS_DEFINED TRUE)
        set(
            _SNOW_SHOT_SAVED_RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        )
    else()
        set(_SNOW_SHOT_RUNTIME_OUTPUT_DIRECTORY_WAS_DEFINED FALSE)
    endif()
    set(
        CMAKE_RUNTIME_OUTPUT_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/test-bin"
    )

    enable_testing()

    add_executable(snow-shot-recognition-image-tests tests/screenshot_recognition_image_tests.cpp
        ${SNOW_SHOT_RECOGNITION_IMAGE_RENDER_SOURCES})
    target_include_directories(snow-shot-recognition-image-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-recognition-image-tests PRIVATE adqt::widgets Qt6::Gui snow_canvas_image)
    snow_shot_import_offscreen_platform(snow-shot-recognition-image-tests)
    qt_import_plugins(snow-shot-recognition-image-tests
        NO_DEFAULT INCLUDE Qt6::QOffscreenIntegrationPlugin)
    add_test(NAME snow-shot-recognition-image-tests
        COMMAND snow-shot-recognition-image-tests -platform offscreen)
    set_tests_properties(snow-shot-recognition-image-tests PROPERTIES LABELS "unit" TIMEOUT 60)

    if(WIN32 OR APPLE)
        if(APPLE)
            set(_snow_shot_shortcut_platform_label macos)
        else()
            set(_snow_shot_shortcut_platform_label windows)
        endif()
        add_executable(snow-shot-global-shortcut-manager-tests
            tests/global_shortcut_manager_tests.cpp
            ${SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES})
        target_link_libraries(snow-shot-global-shortcut-manager-tests PRIVATE
            snow_shot_global_mouse Qt6::Widgets)
        add_test(NAME snow-shot-global-shortcut-manager-tests
            COMMAND snow-shot-global-shortcut-manager-tests)

        set_tests_properties(snow-shot-global-shortcut-manager-tests PROPERTIES
            LABELS "unit;${_snow_shot_shortcut_platform_label}" TIMEOUT 60)
        add_test(NAME snow-shot-global-shortcut-native-smoke-tests
            COMMAND snow-shot-global-shortcut-manager-tests --native-registration-smoke)
        set_tests_properties(snow-shot-global-shortcut-native-smoke-tests PROPERTIES
            LABELS "interactive;${_snow_shot_shortcut_platform_label}" TIMEOUT 60)
        if(APPLE)
            target_link_libraries(snow-shot-global-shortcut-manager-tests PRIVATE
                "${SNOW_SHOT_CARBON_FRAMEWORK}"
                "${SNOW_SHOT_APPKIT_FRAMEWORK}"
                "${SNOW_SHOT_CORE_GRAPHICS_FRAMEWORK}")
        endif()
        unset(_snow_shot_shortcut_platform_label)
    endif()

    add_executable(snow-shot-global-mouse-tests tests/global_mouse_tests.cpp
        ${SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES}
        src/presentation/capture/screenshotglobalmousedrag.cpp)
    target_link_libraries(snow-shot-global-mouse-tests PRIVATE snow_shot_global_mouse Qt6::Widgets)
    if(APPLE)
        target_link_libraries(snow-shot-global-mouse-tests PRIVATE
            "${SNOW_SHOT_CARBON_FRAMEWORK}"
            "${SNOW_SHOT_APPKIT_FRAMEWORK}"
            "${SNOW_SHOT_CORE_GRAPHICS_FRAMEWORK}")
    endif()
    add_test(NAME snow-shot-global-mouse-tests COMMAND snow-shot-global-mouse-tests)
    add_test(NAME snow-shot-global-mouse-storage-tests
        COMMAND snow-shot-application-storage-tests --global-mouse-only)
    add_test(NAME snow-shot-global-mouse-settings-runtime-tests
        COMMAND snow-shot-settings-runtime-session-tests --global-mouse-only)
    set_tests_properties(snow-shot-global-mouse-storage-tests
        snow-shot-global-mouse-settings-runtime-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    set_tests_properties(snow-shot-global-mouse-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    snow_shot_import_offscreen_platform(snow-shot-global-mouse-tests)
    if(WIN32)
        target_sources(snow-shot-global-mouse-tests PRIVATE tests/global_mouse_native_input_tests.cpp)
        add_test(NAME snow-shot-global-mouse-native-smoke-tests
            COMMAND snow-shot-global-mouse-tests --native-hook-smoke)
        set_tests_properties(snow-shot-global-mouse-native-smoke-tests PROPERTIES
            LABELS "interactive;windows" TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=windows")
    elseif(APPLE)
        add_executable(snow-shot-macos-global-mouse-tests tests/macos_global_mouse_tests.mm)
        target_link_libraries(snow-shot-macos-global-mouse-tests PRIVATE snow_shot_global_mouse Qt6::Widgets)
        add_test(NAME snow-shot-macos-global-mouse-tests COMMAND snow-shot-macos-global-mouse-tests)
        set_tests_properties(snow-shot-macos-global-mouse-tests PROPERTIES
            LABELS "unit;macos" TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_test(NAME snow-shot-global-mouse-native-smoke-tests
            COMMAND snow-shot-macos-global-mouse-tests --native-hook-smoke)
        set_tests_properties(snow-shot-global-mouse-native-smoke-tests PROPERTIES
            LABELS "interactive;macos" TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
    endif()

    add_executable(snow-shot-diagnostics-tests tests/diagnostics_tests.cpp)
    target_link_libraries(snow-shot-diagnostics-tests PRIVATE snow_shot_diagnostics)
    add_test(NAME snow-shot-diagnostics-tests COMMAND snow-shot-diagnostics-tests)
    set_tests_properties(snow-shot-diagnostics-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    if(WIN32)
        set_tests_properties(snow-shot-diagnostics-tests PROPERTIES
            ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;$ENV{PATH}")
    endif()
    snow_shot_add_diagnostics_crash_tests()

    snow_shot_offscreen_qpa_environment(_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT)

    # Test binaries must report failures as stderr output plus a non-zero
    # exit code instead of blocking on modal dialogs.
    snow_link_test_cli_guard(qt)

    # The image-codec backend smoke test is Qt-free; give it the Qt-free
    # dialog guard so the guard does not add a Qt runtime dependency.
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_native)

    snow_shot_add_image_codec_smoke_test()
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_qt)

    add_executable(snow-shot-qt-graphicsview-rendering-tests tests/qt_graphicsview_rendering_tests.cpp)
    target_link_libraries(snow-shot-qt-graphicsview-rendering-tests PRIVATE Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-qt-graphicsview-rendering-tests)
    snow_apply_strict_warnings(snow-shot-qt-graphicsview-rendering-tests)
    snow_apply_release_options(snow-shot-qt-graphicsview-rendering-tests POLICY size)
    add_test(NAME snow-shot-qt-graphicsview-rendering-tests
        COMMAND snow-shot-qt-graphicsview-rendering-tests -platform offscreen)
    set_tests_properties(snow-shot-qt-graphicsview-rendering-tests PROPERTIES
        LABELS unit TIMEOUT 30 ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")

    add_test(NAME snow-shot-image-codec-backend-smoke-tests
             COMMAND snow-shot-image-codec-backend-smoke-tests)

    add_test(NAME snow-shot-arm64-stack-cookie-image-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-arm64-stack-cookie.py")
    set_tests_properties(snow-shot-arm64-stack-cookie-image-tests PROPERTIES
        LABELS unit TIMEOUT 30)

    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND
       SNOW_WINDOWS_ARCHITECTURE STREQUAL "arm64")
        add_executable(snow-shot-arm64-stack-cookie-tests tests/arm64_stack_cookie_tests.cpp)
        target_link_libraries(snow-shot-arm64-stack-cookie-tests PRIVATE Qt6::Core)
        snow_apply_strict_warnings(snow-shot-arm64-stack-cookie-tests)
        snow_apply_release_options(snow-shot-arm64-stack-cookie-tests POLICY size)
        add_test(NAME snow-shot-arm64-stack-cookie-tests COMMAND snow-shot-arm64-stack-cookie-tests)
        set_tests_properties(snow-shot-arm64-stack-cookie-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    if(WIN32)
        add_executable(snow-shot-storage-directory-change-tests tests/storage_directory_change_tests.cpp)
        target_link_libraries(snow-shot-storage-directory-change-tests PRIVATE snow_shot_storage Qt6::Core)
        add_test(NAME snow-shot-storage-directory-change-tests COMMAND snow-shot-storage-directory-change-tests)
        set_tests_properties(snow-shot-storage-directory-change-tests PROPERTIES LABELS "unit;windows" TIMEOUT 90
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

        add_executable(snow-shot-storage-directory-crash-capture-tests
            tests/storage_directory_crash_capture_tests.cpp)
        target_link_libraries(snow-shot-storage-directory-crash-capture-tests PRIVATE
            snow_shot_storage Qt6::Core)
        target_compile_definitions(snow-shot-storage-directory-crash-capture-tests PRIVATE
            SNOW_TEST_CRASHPAD_HANDLER="${SNOW_CRASHPAD_HANDLER}")
        add_test(NAME snow-shot-storage-directory-crash-capture-tests
            COMMAND snow-shot-storage-directory-crash-capture-tests)
        set_tests_properties(snow-shot-storage-directory-crash-capture-tests PROPERTIES
            LABELS "unit;windows;crash" TIMEOUT 120)
    endif()
    add_executable(snow-shot-application-storage-tests
        tests/application_storage_tests.cpp
    )
    target_link_libraries(snow-shot-application-storage-tests PRIVATE snow_shot_storage Qt6::Core)
    add_test(NAME snow-shot-portable-system-integration-tests
        COMMAND snow-shot-application-storage-tests --portable-system-integration-only)
    set_tests_properties(snow-shot-portable-system-integration-tests PROPERTIES LABELS "unit"
        TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-tray-theme-icon-storage-tests
        COMMAND snow-shot-application-storage-tests --tray-theme-icons-only)
    set_tests_properties(snow-shot-tray-theme-icon-storage-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME snow-shot-local-shortcut-migration-tests
        COMMAND snow-shot-application-storage-tests --local-shortcut-migration-only)
    set_tests_properties(snow-shot-local-shortcut-migration-tests PROPERTIES LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-update-settings-tests
        COMMAND snow-shot-application-storage-tests --update-settings-only)
    set_tests_properties(snow-shot-update-settings-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(APPLE)
        add_test(NAME snow-shot-storage-startup-directory-tests
            COMMAND snow-shot-application-storage-tests --directory-resolution-only)
        set_tests_properties(snow-shot-storage-startup-directory-tests PROPERTIES
            LABELS "unit;macos" TIMEOUT 30)
    endif()
    add_test(NAME snow-shot-selection-aspect-ratio-settings-tests
        COMMAND snow-shot-application-storage-tests --selection-aspect-ratio-only)
    set_tests_properties(snow-shot-selection-aspect-ratio-settings-tests PROPERTIES LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-selection-transition-animation-storage-tests
        COMMAND snow-shot-application-storage-tests --selection-transition-animation-only)
    set_tests_properties(snow-shot-selection-transition-animation-storage-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recording-post-processing-settings-tests
        COMMAND snow-shot-application-storage-tests --recording-post-processing-only)
    set_tests_properties(snow-shot-recording-post-processing-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-scrolling-interval-settings-tests
        COMMAND snow-shot-application-storage-tests --scrolling-interval-only)
    set_tests_properties(snow-shot-scrolling-interval-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pin-to-screen-storage-tests
        COMMAND snow-shot-application-storage-tests --pin-shortcuts-only)
    set_tests_properties(snow-shot-pin-to-screen-storage-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-translation-confidence-storage-tests
        COMMAND snow-shot-application-storage-tests --translation-settings-only)
    set_tests_properties(snow-shot-translation-confidence-storage-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-application-storage-tests
        COMMAND snow-shot-application-storage-tests)
    add_test(NAME snow-shot-screenshot-mouse-action-storage-tests
        COMMAND snow-shot-application-storage-tests --screenshot-mouse-actions-only)
    set_tests_properties(snow-shot-screenshot-mouse-action-storage-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-auto-save-execution-storage-tests
        COMMAND snow-shot-application-storage-tests --auto-save-after-execution-only)
    set_tests_properties(snow-shot-auto-save-execution-storage-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-storage-quit-lifetime-tests
        COMMAND snow-shot-application-storage-tests --quit-lifetime-only)
    set_tests_properties(snow-shot-storage-quit-lifetime-tests PROPERTIES
        ENVIRONMENT "QT_FATAL_WARNINGS=1")
    add_test(NAME snow-shot-draw-template-storage-tests
        COMMAND snow-shot-application-storage-tests --draw-template-only)
    add_test(NAME snow-shot-watermark-content-storage-tests
        COMMAND snow-shot-application-storage-tests --watermark-content-only)
    set_tests_properties(snow-shot-watermark-content-storage-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    set_tests_properties(snow-shot-application-storage-tests
        snow-shot-storage-quit-lifetime-tests snow-shot-draw-template-storage-tests
        snow-shot-watermark-content-storage-tests PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-capture-history-repository-tests
        tests/capture_history_repository_tests.cpp
    )

    add_executable(snow-shot-file-location-repository-tests tests/file_location_repository_tests.cpp)
    target_link_libraries(snow-shot-file-location-repository-tests PRIVATE snow_shot_storage Qt6::Gui)
    add_test(NAME snow-shot-file-location-repository-tests COMMAND snow-shot-file-location-repository-tests)
    set_tests_properties(snow-shot-file-location-repository-tests PROPERTIES LABELS unit
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    target_link_libraries(snow-shot-capture-history-repository-tests PRIVATE
        snow_shot_storage
        Qt6::Core
        Qt6::Gui
    )
    add_test(NAME snow-shot-capture-history-repository-tests
             COMMAND snow-shot-capture-history-repository-tests)
    add_test(NAME snow-shot-capture-history-revision-tests
             COMMAND snow-shot-capture-history-repository-tests --revision-only)
    set_tests_properties(snow-shot-capture-history-repository-tests
        snow-shot-capture-history-revision-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-pinned-window-repository-tests
        tests/pinned_window_repository_tests.cpp
    )
    target_link_libraries(snow-shot-pinned-window-repository-tests PRIVATE
        snow_shot_storage
        Qt6::Core
        Qt6::Gui
    )
    add_test(NAME snow-shot-pinned-window-repository-tests
             COMMAND snow-shot-pinned-window-repository-tests)
    add_test(NAME snow-shot-pinned-text-style-storage-tests
        COMMAND snow-shot-pinned-window-repository-tests --text-style-only)
    add_test(NAME snow-shot-pinned-window-visibility-storage-tests
             COMMAND snow-shot-pinned-window-repository-tests --visibility-only)
    add_test(NAME snow-shot-pinned-text-type-storage-tests
        COMMAND snow-shot-pinned-window-repository-tests --text-type-only)
    set_tests_properties(snow-shot-pinned-text-type-storage-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-pinned-window-visibility-storage-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    set_tests_properties(snow-shot-pinned-window-repository-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-storage-usage-tracker-tests
        tests/storage_usage_tracker_tests.cpp
    )
    target_link_libraries(snow-shot-storage-usage-tracker-tests PRIVATE
        snow_shot_storage
        Qt6::Core
    )
    add_test(NAME snow-shot-storage-usage-tracker-tests
             COMMAND snow-shot-storage-usage-tracker-tests)
    set_tests_properties(snow-shot-storage-usage-tracker-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-form-fields-tests tests/form_fields_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-form-fields-tests PRIVATE snow_shot_settings)
    snow_shot_import_offscreen_platform(snow-shot-form-fields-tests)
    add_test(NAME snow-shot-form-fields-tests COMMAND snow-shot-form-fields-tests)
    set_tests_properties(snow-shot-form-fields-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-text-translation-settings-tests tests/text_translation_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-text-translation-settings-tests PRIVATE snow_shot_settings ant_design_qt Qt6::Widgets Qt6::Network)
    snow_shot_add_translations(snow-shot-text-translation-settings-tests)
    snow_shot_import_offscreen_platform(snow-shot-text-translation-settings-tests)
    add_test(NAME snow-shot-text-translation-settings-tests COMMAND snow-shot-text-translation-settings-tests -platform offscreen)
    set_tests_properties(snow-shot-text-translation-settings-tests PROPERTIES LABELS "unit" TIMEOUT 60)

    add_executable(snow-shot-text-translation-tests tests/text_translation_tests.cpp)
    target_link_libraries(snow-shot-text-translation-tests PRIVATE snow_shot_translation)
    add_test(NAME snow-shot-text-translation-tests COMMAND snow-shot-text-translation-tests)
    set_tests_properties(snow-shot-text-translation-tests PROPERTIES LABELS "unit" TIMEOUT 60)

    add_executable(snow-shot-custom-ai-models-tests tests/custom_ai_models_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-custom-ai-models-tests PRIVATE snow_shot_settings ant_design_qt Qt6::Widgets Qt6::Network)
    snow_shot_add_translations(snow-shot-custom-ai-models-tests)
    snow_shot_import_offscreen_platform(snow-shot-custom-ai-models-tests)
    add_test(NAME snow-shot-custom-ai-models-tests COMMAND snow-shot-custom-ai-models-tests -platform offscreen)
    set_tests_properties(snow-shot-custom-ai-models-tests PROPERTIES TIMEOUT 60)

    add_executable(snow-shot-mcp-settings-widget-tests
        tests/mcp_settings_widget_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-mcp-settings-widget-tests PRIVATE snow_shot_settings)
    add_test(NAME snow-shot-mcp-settings-widget-tests COMMAND snow-shot-mcp-settings-widget-tests)
    set_tests_properties(snow-shot-mcp-settings-widget-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-mcp-settings-widget-tests)

    add_executable(snow-shot-storage-status-widget-tests
        tests/storage_status_widget_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_include_directories(snow-shot-storage-status-widget-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-storage-status-widget-tests PRIVATE snow_shot_image_codec
        snow_shot_settings
        ant_design_qt
        Qt6::Widgets
    )
    add_test(
        NAME snow-shot-storage-status-widget-tests
        COMMAND snow-shot-storage-status-widget-tests
    )
    if(WIN32)
        set_tests_properties(snow-shot-storage-status-widget-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-shot-storage-status-widget-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    snow_shot_import_offscreen_platform(snow-shot-storage-status-widget-tests)
    add_test(NAME snow-shot-annotation-toolbar-editor-tests
        COMMAND snow-shot-storage-status-widget-tests --toolbar-editor-only -platform offscreen)
    set_tests_properties(snow-shot-annotation-toolbar-editor-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-storage-directory-widget-tests
        COMMAND snow-shot-storage-status-widget-tests --storage-directory-only -platform offscreen)
    set_tests_properties(snow-shot-storage-directory-widget-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-configuration-transfer-widget-tests
        tests/configuration_transfer_widget_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_include_directories(snow-shot-configuration-transfer-widget-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-configuration-transfer-widget-tests PRIVATE
        snow_shot_image_codec
        snow_shot_settings
        snow_shot_settings_catalog
        snow_shot_storage
        ant_design_qt
        Qt6::Widgets
    )
    add_test(
        NAME snow-shot-configuration-transfer-widget-tests
        COMMAND snow-shot-configuration-transfer-widget-tests
    )
    if(WIN32)
        set_tests_properties(snow-shot-configuration-transfer-widget-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-shot-configuration-transfer-widget-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-pinned-window-management-page-tests tests/pinned_window_management_page_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-pinned-window-management-page-tests PRIVATE snow_shot_settings Qt6::Widgets)
    snow_shot_add_translations(snow-shot-pinned-window-management-page-tests)
    snow_shot_import_offscreen_platform(snow-shot-pinned-window-management-page-tests)
    add_test(NAME snow-shot-pinned-window-management-page-tests COMMAND snow-shot-pinned-window-management-page-tests)
    add_test(NAME snow-shot-collage-pinned-selection-tests
        COMMAND snow-shot-pinned-window-management-page-tests --collage-only)
    set_tests_properties(snow-shot-collage-pinned-selection-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-pinned-window-management-page-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-screenshot-history-page-tests
        tests/screenshot_history_page_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_include_directories(snow-shot-screenshot-history-page-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-history-page-tests PRIVATE snow_shot_image_codec
        snow_shot_settings
        ant_design_qt
        Qt6::Widgets
    )
    add_test(
        NAME snow-shot-screenshot-history-page-tests
        COMMAND snow-shot-screenshot-history-page-tests
    )
    add_test(NAME snow-shot-screenshot-history-page-loading-tests
        COMMAND snow-shot-screenshot-history-page-tests --loading-only)
    add_test(NAME snow-shot-history-sticky-selection-tests
        COMMAND snow-shot-screenshot-history-page-tests --sticky-only)
    add_test(NAME snow-shot-pinned-sticky-selection-tests
        COMMAND snow-shot-pinned-window-management-page-tests --sticky-only)
    add_test(NAME snow-shot-history-page-size-tests
        COMMAND snow-shot-screenshot-history-page-tests --page-size-only -platform offscreen)
    set_tests_properties(snow-shot-history-sticky-selection-tests
        snow-shot-pinned-sticky-selection-tests snow-shot-history-page-size-tests PROPERTIES
        LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-history-entry-layout-tests
        COMMAND snow-shot-screenshot-history-page-tests --entry-layout-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-history-page-tests
        snow-shot-screenshot-history-page-loading-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-history-entry-layout-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-screenshot-history-context-menu-tests
        COMMAND snow-shot-screenshot-history-page-tests --context-menu-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-history-context-menu-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    snow_shot_add_translations(snow-shot-screenshot-history-page-tests)
    snow_shot_import_offscreen_platform(snow-shot-screenshot-history-page-tests)

    add_executable(snow-shot-single-instance-coordinator-tests
        tests/single_instance_coordinator_tests.cpp
        include/snow_shot/app/singleinstancecoordinator.h
        src/app/singleinstancecoordinator.cpp
    )
    target_include_directories(snow-shot-single-instance-coordinator-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-single-instance-coordinator-tests PRIVATE
        snow_shot_distribution
        Qt6::Core
        Qt6::Network
    )
    add_test(NAME snow-shot-single-instance-coordinator-tests
             COMMAND snow-shot-single-instance-coordinator-tests)
    set_tests_properties(snow-shot-single-instance-coordinator-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_test(NAME snow-shot-storage-boundary-check
        COMMAND ${CMAKE_COMMAND}
            -DSNOW_SHOT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_storage_boundary.cmake
    )
    add_test(NAME snow-shot-storage-boundary-policy-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_boundary_tests.py")
    set_tests_properties(snow-shot-storage-boundary-policy-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME snow-shot-test-runner-policy-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-test-runner-policy.py")
    set_tests_properties(snow-shot-test-runner-policy-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-source-dependency-contract-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_dependency_contract_tests.py")
    set_tests_properties(snow-shot-source-dependency-contract-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-mcp-capability-generation-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_capability_generation_tests.py")
    set_tests_properties(snow-shot-mcp-capability-generation-tests PROPERTIES LABELS "unit" TIMEOUT 30)

    add_executable(snow-shot-screenshot-result-compositor-tests
        tests/screenshot_result_compositor_tests.cpp
        include/snow_shot/presentation/screenshotresultcompositor.h
        include/snow_shot/presentation/screenshotselectionshadowrenderer.h
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
    )
    target_include_directories(snow-shot-screenshot-result-compositor-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-result-compositor-tests PRIVATE
        ant_design_qt
        snow_draw_engine_qt_canvas
        Qt6::Widgets
    )
    snow_shot_import_offscreen_platform(snow-shot-screenshot-result-compositor-tests)
    add_test(NAME snow-shot-screenshot-result-compositor-tests
             COMMAND snow-shot-screenshot-result-compositor-tests -platform offscreen)
    set_tests_properties(snow-shot-screenshot-result-compositor-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-screenshot-export-service-tests
        tests/screenshot_export_service_tests.cpp
        include/snow_shot/presentation/screenshotclipboardservice.h
        include/snow_shot/presentation/screenshotdisplaysession.h
        include/snow_shot/presentation/screenshotexportservice.h
        include/snow_shot/presentation/screenshotgeometry.h
        include/snow_shot/presentation/screenshotresultcompositor.h
        include/snow_shot/presentation/screenshotselectionshadowrenderer.h
        include/snow_shot/presentation/screenshotselectionpin.h
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotexportservice.cpp
        src/presentation/services/screenshotselectionpin.cpp
    )
    target_compile_definitions(snow-shot-screenshot-export-service-tests PRIVATE NOMINMAX)
    target_include_directories(snow-shot-screenshot-export-service-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-export-service-tests PRIVATE snow_shot_image_codec
        snow_shot_storage
        snow_shot_diagnostics
        ant_design_qt
        Qt6::Widgets
        snow_draw_engine_qt_canvas
    )
    if(WIN32)
        target_link_libraries(snow-shot-screenshot-export-service-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-screenshot-export-service-tests
             COMMAND snow-shot-screenshot-export-service-tests)
    add_test(NAME snow-shot-fractional-dpi-export-tests
             COMMAND snow-shot-screenshot-export-service-tests --fractional-dpi)
    set_tests_properties(snow-shot-screenshot-export-service-tests
        snow-shot-fractional-dpi-export-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-screenshot-export-service-tests)

    add_executable(snow-shot-screenshot-result-geometry-tests
        tests/screenshot_result_geometry_tests.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
    )
    target_include_directories(snow-shot-screenshot-result-geometry-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-result-geometry-tests PRIVATE Qt6::Widgets)
    add_test(NAME snow-shot-screenshot-result-geometry-tests
             COMMAND snow-shot-screenshot-result-geometry-tests)

    add_executable(snow-shot-physical-cursor-tests
        tests/physical_cursor_tests.cpp
        include/snow_shot/platform/physicalcursor.h
        src/platform/physicalcursor.cpp
    )
    target_include_directories(snow-shot-physical-cursor-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-physical-cursor-tests PRIVATE Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-physical-cursor-tests)
    if(APPLE)
        target_link_libraries(snow-shot-physical-cursor-tests PRIVATE "-framework CoreGraphics")
    endif()
    if(WIN32)
        target_compile_definitions(snow-shot-physical-cursor-tests PRIVATE NOMINMAX)
        target_link_libraries(snow-shot-physical-cursor-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-physical-cursor-tests
             COMMAND snow-shot-physical-cursor-tests)

    if(WIN32)
        add_executable(snow-shot-window-chrome-tests
            tests/window_chrome_tests.cpp
            include/snow_shot/platform/windows/windowchrome.h
            src/platform/windows/windowchrome.cpp
        )
        target_compile_definitions(snow-shot-window-chrome-tests PRIVATE NOMINMAX)
        target_include_directories(snow-shot-window-chrome-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(snow-shot-window-chrome-tests PRIVATE
            Qt6::Widgets
            dwmapi
            user32
        )
        add_test(NAME snow-shot-window-chrome-tests
                 COMMAND snow-shot-window-chrome-tests)
        set_tests_properties(snow-shot-window-chrome-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )

        add_executable(snow-shot-window-always-on-top-tests
            tests/window_always_on_top_tests.cpp
            src/platform/windows/windowalwaysontop.cpp)
        target_include_directories(snow-shot-window-always-on-top-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-window-always-on-top-tests PRIVATE Qt6::Widgets user32)
        snow_shot_import_offscreen_platform(snow-shot-window-always-on-top-tests)
        add_test(NAME snow-shot-window-always-on-top-tests
            COMMAND snow-shot-window-always-on-top-tests -platform offscreen)
        set_tests_properties(snow-shot-window-always-on-top-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 20
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

        add_executable(snow-shot-window-cursor-refresh-tests
            tests/window_cursor_refresh_tests.cpp
            src/platform/windows/windowchrome.cpp
            src/platform/windows/windowcursorrefresh.h
        )
        target_compile_definitions(snow-shot-window-cursor-refresh-tests PRIVATE NOMINMAX)
        target_include_directories(snow-shot-window-cursor-refresh-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        target_link_libraries(snow-shot-window-cursor-refresh-tests PRIVATE
            Qt6::Widgets dwmapi user32
        )
        snow_shot_import_offscreen_platform(snow-shot-window-cursor-refresh-tests)
        add_test(NAME snow-shot-window-cursor-refresh-tests
                 COMMAND snow-shot-window-cursor-refresh-tests)
        set_tests_properties(snow-shot-window-cursor-refresh-tests PROPERTIES
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            TIMEOUT 20
            LABELS "unit;windows"
        )
    endif()

    add_executable(snow-shot-canvas-color-sampler-tests
        tests/screenshot_canvas_color_sampler_tests.cpp
        include/snow_shot/presentation/screenshotcanvascolorsampler.h
        src/presentation/services/screenshotcanvascolorsampler.cpp
    )
    target_include_directories(snow-shot-canvas-color-sampler-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-canvas-color-sampler-tests PRIVATE
        Qt6::Widgets snow_canvas_image snow_draw_engine_qt_canvas)
    snow_shot_import_offscreen_platform(snow-shot-canvas-color-sampler-tests)
    add_test(NAME snow-shot-canvas-color-sampler-tests
             COMMAND snow-shot-canvas-color-sampler-tests -platform offscreen)

    add_executable(snow-shot-language-manager-tests
        tests/language_manager_tests.cpp
        include/snow_shot/presentation/languagemanager.h
        src/presentation/services/languagemanager.cpp
    )
    target_include_directories(snow-shot-language-manager-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-language-manager-tests PRIVATE
        ant_design_qt
        Qt6::Widgets
    )
    snow_shot_add_translations(snow-shot-language-manager-tests)
    add_test(
        NAME snow-shot-language-manager-tests
        COMMAND snow-shot-language-manager-tests
    )
    set_tests_properties(snow-shot-language-manager-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-about-page-tests
        tests/about_page_tests.cpp
        include/snow_shot/presentation/mainwindow.h
        include/snow_shot/presentation/components/contentcardwidget.h
        include/snow_shot/presentation/components/maincontentheaderwidget.h
        include/snow_shot/presentation/components/sidebarwidget.h
        include/snow_shot/presentation/components/titlebarwidget.h
        src/presentation/mainwindow.cpp
        src/presentation/components/contentcardwidget.cpp
        src/presentation/components/maincontentheaderwidget.cpp
        src/presentation/components/sidebarwidget.cpp
        src/presentation/components/titlebarwidget.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/platform/windows/windowchrome.cpp
    )
    if(APPLE)
        target_sources(snow-shot-about-page-tests PRIVATE
            src/platform/macos/applicationactivation.mm)
        target_link_libraries(snow-shot-about-page-tests PRIVATE "-framework AppKit")
    endif()
    target_link_libraries(snow-shot-about-page-tests PRIVATE
        snow_shot_settings
        snow_shot_image_codec
    )
    target_compile_definitions(snow-shot-about-page-tests PRIVATE
        SNOW_SHOT_TEST_VERSION="${SNOW_SHOT_VERSION}"
        SNOW_SHOT_TEST_TRANSLATIONS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        SNOW_SHOT_TEST_WEBSITE_URL="${SNOW_SHOT_WEBSITE_URL}"
        SNOW_SHOT_TEST_PROJECT_URL="${SNOW_SHOT_PROJECT_URL}"
        SNOW_SHOT_TEST_QQ_GROUP_3_URL="${SNOW_SHOT_QQ_GROUP_3_URL}"
    )
    add_dependencies(snow-shot-about-page-tests snow_shot_release_translations)
    snow_shot_import_offscreen_platform(snow-shot-about-page-tests)
    add_test(NAME snow-shot-about-page-tests COMMAND snow-shot-about-page-tests)
    target_link_libraries(snow-shot-about-page-tests PRIVATE snow_shot_updates)
    set_tests_properties(snow-shot-about-page-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(APPLE)
        add_test(NAME snow-shot-tray-icon-color-settings-tests
            COMMAND snow-shot-about-page-tests --tray-icon-color-only)
        set_tests_properties(snow-shot-tray-icon-color-settings-tests PROPERTIES
            LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    get_target_property(_main_window_font_sources snow-shot-about-page-tests SOURCES)
    list(REMOVE_ITEM _main_window_font_sources tests/about_page_tests.cpp)
    add_executable(snow-shot-main-window-font-tests
        tests/main_window_font_tests.cpp
        ${_main_window_font_sources}
    )
    target_link_libraries(snow-shot-main-window-font-tests PRIVATE
        snow_shot_settings snow_shot_image_codec snow_shot_updates)
    if(APPLE)
        target_link_libraries(snow-shot-main-window-font-tests PRIVATE "-framework AppKit")
    endif()
    snow_shot_import_offscreen_platform(snow-shot-main-window-font-tests)
    add_test(NAME snow-shot-main-window-font-tests COMMAND snow-shot-main-window-font-tests)
    add_test(NAME snow-shot-collage-pinned-navigation-tests
        COMMAND snow-shot-main-window-font-tests --pinned-collage-only)
    set_tests_properties(snow-shot-collage-pinned-navigation-tests PROPERTIES
        LABELS "unit" ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    set_tests_properties(snow-shot-main-window-font-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=1.5"
    )

    get_target_property(_window_geometry_memory_sources snow-shot-about-page-tests SOURCES)
    list(REMOVE_ITEM _window_geometry_memory_sources tests/about_page_tests.cpp)
    add_executable(snow-shot-window-geometry-memory-tests
        tests/window_geometry_memory_tests.cpp
        ${_window_geometry_memory_sources}
    )
    target_link_libraries(snow-shot-window-geometry-memory-tests PRIVATE
        snow_shot_settings snow_shot_image_codec snow_shot_updates)
    if(APPLE)
        target_link_libraries(snow-shot-window-geometry-memory-tests PRIVATE "-framework AppKit")
    endif()
    snow_shot_import_offscreen_platform(snow-shot-window-geometry-memory-tests)
    add_test(NAME snow-shot-window-geometry-memory-tests
             COMMAND snow-shot-window-geometry-memory-tests)
    set_tests_properties(snow-shot-window-geometry-memory-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
    )

    add_executable(snow-shot-settings-catalog-tests
        tests/settings_catalog_tests.cpp
    )
    target_include_directories(snow-shot-settings-catalog-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-settings-catalog-tests PRIVATE
        snow_shot_settings_search
        Qt6::Core
    )
    add_test(NAME snow-shot-storage-directory-catalog-tests
        COMMAND snow-shot-settings-catalog-tests --storage-directory-only)
    set_tests_properties(snow-shot-storage-directory-catalog-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-batch-shortcut-catalog-tests
        COMMAND snow-shot-settings-catalog-tests --pinned-batch-shortcuts-only)
    set_tests_properties(snow-shot-pinned-batch-shortcut-catalog-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-settings-catalog-tests
             COMMAND snow-shot-settings-catalog-tests)
    add_test(NAME snow-shot-tray-icon-settings-catalog-tests
        COMMAND snow-shot-settings-catalog-tests --tray-icons-only)
    set_tests_properties(snow-shot-tray-icon-settings-catalog-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-translation-confidence-catalog-tests
        COMMAND snow-shot-settings-catalog-tests --translation-confidence-only)
    set_tests_properties(snow-shot-translation-confidence-catalog-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-global-hotkeys-catalog-tests
        COMMAND snow-shot-settings-catalog-tests --global-hotkeys-only)
    set_tests_properties(snow-shot-global-hotkeys-catalog-tests PROPERTIES LABELS unit TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-ocr-text-options-catalog-tests
             COMMAND snow-shot-settings-catalog-tests --ocr-text-options-only)
    add_test(NAME snow-shot-ocr-highlighting-settings-catalog-tests
        COMMAND snow-shot-settings-catalog-tests --ocr-highlighting-only)
    set_tests_properties(snow-shot-ocr-highlighting-settings-catalog-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-ocr-text-options-catalog-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-settings-catalog-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-main-window-skin-codec-tests tests/main_window_skin_codec_tests.cpp)
    target_link_libraries(snow-shot-main-window-skin-codec-tests PRIVATE snow_shot_image_codec)
    add_test(NAME snow-shot-main-window-skin-codec-tests COMMAND snow-shot-main-window-skin-codec-tests)
    set_tests_properties(snow-shot-main-window-skin-codec-tests PROPERTIES LABELS unit TIMEOUT 60)
    add_test(NAME snow-shot-managed-image-reader-tests
        COMMAND snow-shot-main-window-skin-codec-tests --managed-reader-only)
    set_tests_properties(snow-shot-managed-image-reader-tests PROPERTIES LABELS unit TIMEOUT 60)

    add_executable(snow-shot-main-window-skin-tests tests/main_window_skin_tests.cpp)
    target_link_libraries(snow-shot-main-window-skin-tests PRIVATE
        snow_shot_main_window_skin snow_shot_storage snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-main-window-skin-tests)
    add_test(NAME snow-shot-main-window-skin-tests COMMAND snow-shot-main-window-skin-tests)
    set_tests_properties(snow-shot-main-window-skin-tests PROPERTIES LABELS unit TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")

    add_executable(snow-shot-skin-settings-tests tests/skin_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-skin-settings-tests PRIVATE snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-skin-settings-tests)
    snow_shot_add_translations(snow-shot-skin-settings-tests)
    add_test(NAME snow-shot-skin-settings-tests COMMAND snow-shot-skin-settings-tests)
    set_tests_properties(snow-shot-skin-settings-tests PROPERTIES LABELS unit TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")

    add_executable(snow-shot-app-font-tests
        tests/app_font_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-app-font-tests PRIVATE
        snow_shot_settings
        snow_shot_image_codec
    )
    add_test(NAME snow-shot-app-font-tests
             COMMAND snow-shot-app-font-tests)
    set_tests_properties(snow-shot-app-font-tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-app-font-tests)
    snow_shot_add_translations(snow-shot-app-font-tests)

    add_executable(snow-shot-theme-primary-color-tests
        tests/theme_primary_color_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-theme-primary-color-tests PRIVATE
        snow_shot_settings
        snow_shot_image_codec
    )
    add_test(NAME snow-shot-theme-primary-color-tests
             COMMAND snow-shot-theme-primary-color-tests)
    set_tests_properties(snow-shot-theme-primary-color-tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-theme-primary-color-tests)
    snow_shot_add_translations(snow-shot-theme-primary-color-tests)

    add_executable(snow-shot-settings-color-commit-tests
        tests/settings_color_commit_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-settings-color-commit-tests PRIVATE
        snow_shot_settings
        snow_shot_image_codec
    )
    add_test(NAME snow-shot-settings-color-commit-tests
             COMMAND snow-shot-settings-color-commit-tests)
    add_test(NAME snow-shot-custom-color-presets-settings-tests
             COMMAND snow-shot-settings-color-commit-tests --color-presets-only)
    set_tests_properties(snow-shot-settings-color-commit-tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-custom-color-presets-settings-tests PROPERTIES
        LABELS "unit"
        TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-settings-color-commit-tests)

    add_executable(snow-shot-settings-slider-commit-tests
        tests/settings_slider_commit_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-settings-slider-commit-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-settings-slider-commit-tests)
    add_test(NAME snow-shot-settings-slider-commit-tests
        COMMAND snow-shot-settings-slider-commit-tests)
    set_tests_properties(snow-shot-settings-slider-commit-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    snow_find_qt(COMPONENTS Test)
    add_executable(snow-shot-settings-page-loading-tests
        tests/settings_page_loading_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-settings-page-loading-tests PRIVATE
        snow_shot_settings snow_shot_image_codec Qt6::Test)
    snow_shot_import_offscreen_platform(snow-shot-settings-page-loading-tests)
    snow_shot_add_translations(snow-shot-settings-page-loading-tests)
    add_test(NAME snow-shot-settings-select-tests
        COMMAND snow-shot-settings-page-loading-tests --selects-only)
    add_test(NAME snow-shot-pinned-text-color-settings-tests
        COMMAND snow-shot-settings-page-loading-tests --pinned-text-colors-only)
    set_tests_properties(snow-shot-pinned-text-color-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-settings-select-tests PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(NAME snow-shot-settings-page-loading-tests
        COMMAND snow-shot-settings-page-loading-tests)
    add_test(NAME snow-shot-settings-variable-help-tests
        COMMAND snow-shot-settings-page-loading-tests --variable-help-only -platform offscreen)
    set_tests_properties(snow-shot-settings-variable-help-tests PROPERTIES
        LABELS unit RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-stitch-selected-images-settings-tests
        COMMAND snow-shot-settings-page-loading-tests --stitch-selected-images-only)
    set_tests_properties(snow-shot-stitch-selected-images-settings-tests PROPERTIES LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-settings-page-loading-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-settings-navigation-tests
        COMMAND snow-shot-settings-page-loading-tests --navigation-only)
    add_test(NAME snow-shot-action-toolbar-settings-tests
        COMMAND snow-shot-settings-page-loading-tests --action-toolbar-settings-only)
    add_test(NAME snow-shot-settings-layout-stability-tests
        COMMAND snow-shot-settings-page-loading-tests --layout-stability-only)
    add_test(NAME snow-shot-tray-selection-settings-tests
        COMMAND snow-shot-settings-page-loading-tests --tray-selection-only)
    add_test(NAME snow-shot-settings-catalog-reset-tests
        COMMAND snow-shot-settings-page-loading-tests --catalog-resets-only)
    set_tests_properties(snow-shot-settings-navigation-tests snow-shot-action-toolbar-settings-tests
        snow-shot-settings-layout-stability-tests snow-shot-tray-selection-settings-tests
        snow-shot-settings-catalog-reset-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-annotation-shortcut-settings-tests
        tests/annotation_shortcut_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-annotation-shortcut-settings-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-annotation-shortcut-settings-tests)
    add_test(NAME snow-shot-annotation-shortcut-settings-tests
             COMMAND snow-shot-annotation-shortcut-settings-tests)
    set_tests_properties(snow-shot-annotation-shortcut-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-screen-recording-shortcut-settings-tests
        tests/screen_recording_shortcut_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-screen-recording-shortcut-settings-tests PRIVATE
        snow_shot_settings
        snow_shot_image_codec
    )
    add_test(NAME snow-shot-screen-recording-shortcut-settings-tests
             COMMAND snow-shot-screen-recording-shortcut-settings-tests)
    set_tests_properties(snow-shot-screen-recording-shortcut-settings-tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-screen-recording-shortcut-settings-tests)

    add_executable(snow-shot-local-shortcut-reset-tests
        tests/local_shortcut_reset_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-local-shortcut-reset-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    add_test(NAME snow-shot-local-shortcut-reset-tests COMMAND snow-shot-local-shortcut-reset-tests)
    set_tests_properties(snow-shot-local-shortcut-reset-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    snow_shot_import_offscreen_platform(snow-shot-local-shortcut-reset-tests)

    add_executable(snow-shot-pin-to-screen-shortcut-settings-tests
        tests/pin_to_screen_shortcut_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-pin-to-screen-shortcut-settings-tests PRIVATE
        snow_shot_settings
        snow_shot_image_codec
    )
    add_test(NAME snow-shot-pin-to-screen-shortcut-settings-tests
             COMMAND snow-shot-pin-to-screen-shortcut-settings-tests)
    set_tests_properties(snow-shot-pin-to-screen-shortcut-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-pin-to-screen-shortcut-settings-tests)

    add_executable(snow-shot-translation-settings-tests
        tests/translation_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-translation-settings-tests PRIVATE
        snow_shot_settings
        snow_shot_image_codec
    )
    add_test(NAME snow-shot-translation-settings-tests COMMAND snow-shot-translation-settings-tests)
    if(WIN32)
        add_test(NAME snow-shot-window-always-on-top-settings-tests
            COMMAND snow-shot-translation-settings-tests --window-always-on-top-only -platform offscreen)
        set_tests_properties(snow-shot-window-always-on-top-settings-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 30
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-translation-confidence-settings-tests
        COMMAND snow-shot-translation-settings-tests --translation-confidence-only -platform offscreen)
    set_tests_properties(snow-shot-translation-confidence-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-ocr-highlighting-settings-tests
        COMMAND snow-shot-translation-settings-tests --ocr-highlighting-only -platform offscreen)
    add_test(NAME snow-shot-ocr-highlighting-storage-tests
        COMMAND snow-shot-application-storage-tests --ocr-highlighting-only)
    add_test(NAME snow-shot-ocr-settings-storage-tests
        COMMAND snow-shot-application-storage-tests --ocr-settings-only)
    set_tests_properties(snow-shot-ocr-highlighting-settings-tests
        snow-shot-ocr-highlighting-storage-tests snow-shot-ocr-settings-storage-tests
        PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-text-detection-processing-settings-tests
        COMMAND snow-shot-translation-settings-tests --text-detection-processing-only -platform offscreen)
    set_tests_properties(snow-shot-text-detection-processing-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-translation-settings-tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-translation-settings-tests)

    add_executable(snow-shot-settings-runtime-session-tests
        tests/settings_runtime_session_tests.cpp
        include/snow_shot/presentation/settings/settingsruntimesession.h
        include/snow_shot/presentation/settings/settingsbackend.h
        src/presentation/settings/settingsruntimesession.cpp
    )
    target_include_directories(snow-shot-settings-runtime-session-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-settings-runtime-session-tests PRIVATE
        snow_shot_global_mouse
        snow_shot_settings_catalog
        snow_shot_storage
        Qt6::Core
        Qt6::Gui
    )
    add_test(NAME snow-shot-settings-runtime-session-tests
             COMMAND snow-shot-settings-runtime-session-tests)
    set_tests_properties(snow-shot-settings-runtime-session-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-configuration-archive-tests
        tests/configuration_archive_tests.cpp
    )
    target_include_directories(snow-shot-configuration-archive-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-configuration-archive-tests PRIVATE
        snow_shot_storage
        Qt6::Core
        Qt6::Gui
    )
    add_test(NAME snow-shot-configuration-archive-tests
             COMMAND snow-shot-configuration-archive-tests)
    add_test(NAME snow-shot-auto-save-execution-archive-tests
        COMMAND snow-shot-configuration-archive-tests --auto-save-after-execution-only)
    set_tests_properties(snow-shot-auto-save-execution-archive-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-selection-aspect-ratio-archive-tests
        COMMAND snow-shot-configuration-archive-tests --selection-aspect-ratio-only)
    set_tests_properties(snow-shot-selection-aspect-ratio-archive-tests PROPERTIES LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-configuration-archive-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-button-border-tests
        tests/button_border_tests.cpp
        src/presentation/styles/buttonborder.cpp
    )
    target_include_directories(snow-shot-button-border-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-button-border-tests PRIVATE Qt6::Gui)
    add_test(NAME snow-shot-button-border-tests COMMAND snow-shot-button-border-tests)
    set_tests_properties(snow-shot-button-border-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-global-mouse-page-tests
        tests/global_mouse_page_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-global-mouse-page-tests PRIVATE
        snow_shot_settings
        Qt6::Widgets
    )
    add_test(NAME snow-shot-global-mouse-page-tests
             COMMAND snow-shot-global-mouse-page-tests)
    set_tests_properties(snow-shot-global-mouse-page-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-global-mouse-page-tests)

    add_executable(snow-shot-permission-guide-tests tests/permission_guide_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-permission-guide-tests PRIVATE snow_shot_settings)
    snow_shot_add_translations(snow-shot-permission-guide-tests)
    add_test(NAME snow-shot-permission-guide-tests COMMAND snow-shot-permission-guide-tests)
    set_tests_properties(snow-shot-permission-guide-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    snow_shot_import_offscreen_platform(snow-shot-permission-guide-tests)

    add_executable(snow-shot-app-permissions-tests tests/app_permissions_tests.cpp
        src/app/featureavailability.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-app-permissions-tests PRIVATE snow_shot_settings)
    add_test(NAME snow-shot-app-permissions-tests COMMAND snow-shot-app-permissions-tests)
    add_test(NAME snow-shot-pinned-visibility-permission-routing-tests
        COMMAND snow-shot-app-permissions-tests --routing-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-visibility-permission-routing-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    set_tests_properties(snow-shot-app-permissions-tests PROPERTIES
        LABELS "unit" ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    snow_shot_import_offscreen_platform(snow-shot-app-permissions-tests)

    if(APPLE)
        add_executable(snow-shot-macos-permission-guide-tests MACOSX_BUNDLE
            tests/macos_permission_guide_tests.mm
            src/presentation/services/screenshotclipboardservice.cpp)
        target_link_libraries(snow-shot-macos-permission-guide-tests PRIVATE
            snow_shot_settings "-framework AppKit")
        snow_shot_add_translations(snow-shot-macos-permission-guide-tests)
        set_target_properties(snow-shot-macos-permission-guide-tests PROPERTIES
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.snowshot.tests.permission-guide"
            MACOSX_BUNDLE_BUNDLE_NAME "Snow Shot Permission Guide")
        add_test(NAME snow-shot-macos-permission-guide-tests
            COMMAND snow-shot-macos-permission-guide-tests)
        set_tests_properties(snow-shot-macos-permission-guide-tests PROPERTIES
            LABELS "interactive;macos" TIMEOUT 30 RUN_SERIAL TRUE)
        add_executable(snow-shot-macos-smart-selection-smoke
            tests/macos_smart_selection_smoke_test.mm
            src/platform/macos/windowcaptureexclusion.mm)
        target_include_directories(snow-shot-macos-smart-selection-smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector-c/include")
        target_link_libraries(snow-shot-macos-smart-selection-smoke PRIVATE
            snow_ui_selector_c Qt6::Widgets "-framework AppKit")
        add_test(NAME snow-shot-macos-smart-selection-smoke COMMAND snow-shot-macos-smart-selection-smoke)
        set_tests_properties(snow-shot-macos-smart-selection-smoke PROPERTIES
            LABELS "interactive;macos" SKIP_RETURN_CODE 77 TIMEOUT 30 RUN_SERIAL TRUE)
    endif()

    add_executable(snow-shot-selector-policy-tests
        tests/screenshot_selector_policy_tests.cpp
        src/presentation/selector/screenshotselectorpolicy.h
    )
    target_include_directories(snow-shot-selector-policy-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/selector"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector-c/include"
    )
    target_link_libraries(snow-shot-selector-policy-tests PRIVATE
        Qt6::Core
    )
    add_test(NAME snow-shot-selector-policy-tests
             COMMAND snow-shot-selector-policy-tests)
    set_tests_properties(snow-shot-selector-policy-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-window-element-api-tests
        tests/window_element_api_tests.cpp
        include/snow_shot/presentation/screenshotselectorcoordinator.h
        src/presentation/selector/screenshotselectorcoordinator.cpp
        src/presentation/selector/screenshotselectorserviceclient.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    snow_shot_alias_test_rust_ffi(snow-shot-window-element-api-tests
        snow_ui_selector_service_create
        snow_ui_selector_service_destroy
        snow_ui_selector_service_invalidate_refinement
        snow_ui_selector_service_query
        snow_ui_selector_service_refine
        snow_ui_selector_service_refresh
        snow_ui_selector_service_refresh_with_displays
        snow_ui_selector_service_release_cache)
    target_include_directories(snow-shot-window-element-api-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector-c/include"
    )
    target_link_libraries(snow-shot-window-element-api-tests PRIVATE
        snow_shot_settings
        Qt6::Widgets
    )
    if(APPLE)
        target_link_libraries(snow-shot-window-element-api-tests PRIVATE
            "-framework CoreGraphics" "-framework AppKit")
    endif()
    add_test(NAME snow-shot-window-element-api-tests
             COMMAND snow-shot-window-element-api-tests)
    add_test(NAME snow-shot-selector-phased-tests
             COMMAND snow-shot-window-element-api-tests --selector-only)
    add_test(NAME snow-shot-selector-lifecycle-tests
             COMMAND snow-shot-window-element-api-tests --selector-lifecycle-only)
    add_test(NAME snow-shot-shortcut-exit-confirmation-settings-tests
             COMMAND snow-shot-window-element-api-tests
                     --shortcut-exit-confirmation-settings-only)
    add_test(NAME snow-shot-auto-qr-settings-tests
             COMMAND snow-shot-window-element-api-tests --auto-qr-settings-only)
    add_test(NAME snow-shot-screenshot-guide-settings-reset-tests
             COMMAND snow-shot-window-element-api-tests --guide-settings-only)
    snow_shot_import_offscreen_platform(snow-shot-window-element-api-tests)
    set_tests_properties(
        snow-shot-window-element-api-tests
        snow-shot-selector-phased-tests
        snow-shot-selector-lifecycle-tests
        snow-shot-shortcut-exit-confirmation-settings-tests
        snow-shot-auto-qr-settings-tests
        snow-shot-screenshot-guide-settings-reset-tests
        PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-system-tray-controller-tests
        tests/system_tray_controller_tests.cpp
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/pinnedwindowgroupmanager.h
        include/snow_shot/presentation/systemtraycontroller.h
        src/presentation/services/languagemanager.cpp
        src/presentation/services/pinnedwindowgroupmanager.cpp
        src/presentation/services/systemtraycontroller.cpp
    )
    target_include_directories(snow-shot-system-tray-controller-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_SHOT_DRAW_ENGINE_QT_DIR}/include"
    )
    target_link_libraries(snow-shot-system-tray-controller-tests PRIVATE
        snow_shot_system_notification_controller
        snow_shot_image_codec
        ant_design_qt
        snow_shot_settings_catalog
        Qt6::Widgets
    )
    # Match deployment: ICO decoding must not depend on the optional Qt plugin.
    qt_import_plugins(snow-shot-system-tray-controller-tests EXCLUDE Qt6::QICOPlugin)
    qt_add_resources(snow-shot-system-tray-controller-tests "snow_shot_tray_test_assets"
        PREFIX "/snow-shot/app-icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-default.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-light.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-dark.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-snow-default.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-snow-light.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-snow-dark.png"
    )
    snow_shot_add_translations(snow-shot-system-tray-controller-tests)
    add_test(
        NAME snow-shot-system-tray-controller-tests
        COMMAND snow-shot-system-tray-controller-tests
    )
    add_test(NAME snow-shot-tray-click-action-tests
        COMMAND snow-shot-system-tray-controller-tests --click-actions-only -platform offscreen)
    set_tests_properties(snow-shot-tray-click-action-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-visibility-tray-tests
        COMMAND snow-shot-system-tray-controller-tests --hide-show-tray-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-visibility-tray-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-tray-skin-tests
        COMMAND snow-shot-system-tray-controller-tests --skin-only -platform offscreen)
    add_test(NAME snow-shot-recording-export-tray-tests
        COMMAND snow-shot-system-tray-controller-tests --recording-export-only -platform offscreen)
    set_tests_properties(snow-shot-recording-export-tray-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-tray-group-refresh-tests
        COMMAND snow-shot-system-tray-controller-tests --group-refresh-only -platform offscreen)
    add_test(NAME snow-shot-tray-menu-position-tests
        COMMAND snow-shot-system-tray-controller-tests --menu-position-only -platform offscreen)
    add_test(NAME snow-shot-tray-capture-input-tests
        COMMAND snow-shot-system-tray-controller-tests --capture-input-only -platform offscreen)
    set_tests_properties(snow-shot-tray-capture-input-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-tray-menu-selection-tests
        COMMAND snow-shot-system-tray-controller-tests --configured-menu-only -platform offscreen)
    add_test(NAME snow-shot-tray-fullscreen-suppression-tests
        COMMAND snow-shot-system-tray-controller-tests --fullscreen-suppression-only -platform offscreen)
    add_test(NAME snow-shot-tray-theme-icon-tests
        COMMAND snow-shot-system-tray-controller-tests --theme-icons-only -platform offscreen)
    set_tests_properties(snow-shot-tray-theme-icon-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-tray-fullscreen-suppression-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-tray-menu-selection-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    snow_shot_import_offscreen_platform(snow-shot-system-tray-controller-tests)
    set_tests_properties(snow-shot-tray-skin-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-tray-group-refresh-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-tray-menu-position-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        set_tests_properties(snow-shot-system-tray-controller-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-shot-system-tray-controller-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    if(APPLE)
        target_link_libraries(snow-shot-system-tray-controller-tests PRIVATE
            snow_shot_system_notifications)
        target_sources(snow-shot-system-tray-controller-tests PRIVATE
            src/platform/macos/systemtraymenu.mm
            tests/system_tray_menu_native_tests.mm)
        target_link_libraries(snow-shot-system-tray-controller-tests PRIVATE "-framework AppKit")
        add_test(NAME snow-shot-system-tray-menu-native-tests
            COMMAND snow-shot-system-tray-controller-tests --native-menu -platform cocoa)
        set_tests_properties(snow-shot-system-tray-menu-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 30)
    endif()

    add_executable(snow-shot-window-group-switcher-tests
        tests/window_group_switcher_tests.cpp
        ${SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES}
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
        include/snow_shot/presentation/windowgroupswitchercontroller.h
        include/snow_shot/presentation/windowgroupswitcherpopup.h
        include/snow_shot/presentation/pinnedwindowgroupmanager.h
        include/snow_shot/presentation/languagemanager.h
        src/presentation/services/windowgroupswitchercontroller.cpp
        src/presentation/components/windowgroupswitcherpopup.cpp
        src/presentation/services/pinnedwindowgroupmanager.cpp
        src/presentation/services/languagemanager.cpp)
    target_include_directories(snow-shot-window-group-switcher-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${SNOW_SHOT_DRAW_ENGINE_QT_DIR}/include")
    target_link_libraries(snow-shot-window-group-switcher-tests PRIVATE snow_shot_global_mouse ant_design_qt Qt6::Widgets)
    if(APPLE)
        set_source_files_properties(tests/window_group_switcher_tests.cpp PROPERTIES LANGUAGE OBJCXX)
        target_link_libraries(snow-shot-window-group-switcher-tests PRIVATE
            "${SNOW_SHOT_CARBON_FRAMEWORK}" "${SNOW_SHOT_APPKIT_FRAMEWORK}" "${SNOW_SHOT_CORE_GRAPHICS_FRAMEWORK}")
        add_test(NAME snow-shot-window-group-switcher-native-tests
            COMMAND snow-shot-window-group-switcher-tests -platform cocoa)
        set_tests_properties(snow-shot-window-group-switcher-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    if(WIN32)
        add_test(NAME snow-shot-window-group-switcher-native-tests
            COMMAND snow-shot-window-group-switcher-tests --native -platform windows)
        set_tests_properties(snow-shot-window-group-switcher-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-window-group-switcher-tests)
    snow_shot_add_translations(snow-shot-window-group-switcher-tests)
    add_test(NAME snow-shot-window-group-switcher-tests COMMAND snow-shot-window-group-switcher-tests)
    set_tests_properties(snow-shot-window-group-switcher-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS unit)

    add_executable(snow-shot-pinned-window-group-tests
        tests/pinned_window_group_tests.cpp
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/pinnedwindowgroupmanager.h
        src/presentation/services/languagemanager.cpp
        src/presentation/services/pinnedwindowgroupmanager.cpp
    )
    target_include_directories(snow-shot-pinned-window-group-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_SHOT_DRAW_ENGINE_QT_DIR}/include"
    )
    target_link_libraries(snow-shot-pinned-window-group-tests PRIVATE
        ant_design_qt
        snow_shot_storage
        Qt6::Widgets
    )
    snow_shot_add_translations(snow-shot-pinned-window-group-tests)
    add_test(
        NAME snow-shot-pinned-window-group-tests
        COMMAND snow-shot-pinned-window-group-tests
    )
    add_test(NAME snow-shot-pinned-window-visibility-group-tests
        COMMAND snow-shot-pinned-window-group-tests --visibility-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-window-visibility-group-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    set_tests_properties(snow-shot-pinned-window-group-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-action-popup-menu-tests tests/action_popup_menu_tests.cpp)
    target_include_directories(snow-shot-action-popup-menu-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-action-popup-menu-tests PRIVATE
        ant_design_qt snow_shot_shortcuts Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-action-popup-menu-tests)
    add_test(NAME snow-shot-action-popup-menu-tests
        COMMAND snow-shot-action-popup-menu-tests -platform offscreen)
    set_tests_properties(snow-shot-action-popup-menu-tests PROPERTIES
        LABELS "unit" TIMEOUT 20
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    if(APPLE)
        snow_find_qt(COMPONENTS Test)
        add_executable(snow-shot-action-popup-native-tests tests/action_popup_menu_native_tests.mm)
        target_include_directories(snow-shot-action-popup-native-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-action-popup-native-tests PRIVATE
            ant_design_qt snow_shot_shortcuts Qt6::Test "-framework AppKit")
        add_test(NAME snow-shot-action-popup-native-tests COMMAND snow-shot-action-popup-native-tests)
        set_tests_properties(snow-shot-action-popup-native-tests PROPERTIES
            LABELS "interactive"
            ENVIRONMENT "QT_QPA_PLATFORM=cocoa"
            TIMEOUT 20)
    endif()

    add_executable(snow-shot-context-menu-rendering-tests
        tests/context_menu_rendering_tests.cpp
    )
    target_link_libraries(snow-shot-context-menu-rendering-tests PRIVATE
        ant_design_qt
        Qt6::Widgets
    )
    snow_shot_import_offscreen_platform(snow-shot-context-menu-rendering-tests)
    add_test(
        NAME snow-shot-context-menu-rendering-tests
        COMMAND snow-shot-context-menu-rendering-tests
    )
    if(WIN32)
        set_tests_properties(snow-shot-context-menu-rendering-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-shot-context-menu-rendering-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-window-shortcut-manager-tests
        tests/window_shortcut_manager_tests.cpp
    )
    add_executable(snow-shot-mouse-release-action-tests tests/mouse_release_action_tests.cpp)
    target_link_libraries(snow-shot-mouse-release-action-tests PRIVATE snow_shot_window_shortcuts)
    snow_shot_import_offscreen_platform(snow-shot-mouse-release-action-tests)
    add_test(NAME snow-shot-mouse-release-action-tests
        COMMAND snow-shot-mouse-release-action-tests -platform offscreen)
    set_tests_properties(snow-shot-mouse-release-action-tests PROPERTIES LABELS "unit")
    target_link_libraries(snow-shot-window-shortcut-manager-tests PRIVATE
        snow_shot_window_shortcuts
        Qt6::Widgets
    )
    add_test(
        NAME snow-shot-window-shortcut-manager-tests
        COMMAND snow-shot-window-shortcut-manager-tests
    )
    set_tests_properties(snow-shot-window-shortcut-manager-tests PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=$<IF:$<BOOL:${WIN32}>,windows,offscreen>"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-shortcut-key-row-tests
        tests/shortcut_key_row_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_include_directories(snow-shot-shortcut-key-row-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-shortcut-key-row-tests PRIVATE
        snow_shot_settings
        Qt6::Widgets
    )
    snow_shot_import_offscreen_platform(snow-shot-shortcut-key-row-tests)
    add_test(NAME snow-shot-recording-area-shortcut-editor-tests
        COMMAND snow-shot-shortcut-key-row-tests --recording-area-only -platform offscreen)
    set_tests_properties(snow-shot-recording-area-shortcut-editor-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(
        NAME snow-shot-shortcut-key-row-tests
        COMMAND snow-shot-shortcut-key-row-tests
    )
    set_tests_properties(snow-shot-shortcut-key-row-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(WIN32)
        add_executable(snow-shot-print-screen-shortcut-recorder-tests
            tests/print_screen_shortcut_recorder_tests.cpp
            src/platform/windows/printscreenshortcutrecorder.cpp)
        target_include_directories(snow-shot-print-screen-shortcut-recorder-tests PRIVATE include)
        target_link_libraries(snow-shot-print-screen-shortcut-recorder-tests PRIVATE Qt6::Widgets)
        snow_shot_import_offscreen_platform(snow-shot-print-screen-shortcut-recorder-tests)
        add_test(NAME snow-shot-print-screen-shortcut-recorder-tests
            COMMAND snow-shot-print-screen-shortcut-recorder-tests -platform offscreen)
        set_tests_properties(snow-shot-print-screen-shortcut-recorder-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 30)
        add_test(
            NAME snow-shot-shortcut-key-row-native-tests
            COMMAND snow-shot-shortcut-key-row-tests --native-print-screen-only
        )
        set_tests_properties(snow-shot-shortcut-key-row-native-tests PROPERTIES
            LABELS "interactive;windows"
            RUN_SERIAL TRUE
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-screenshot-shortcut-hints-tests
        tests/screenshot_shortcut_hints_tests.cpp
        include/snow_shot/presentation/screenshotshortcuthints.h
    )
    target_include_directories(snow-shot-screenshot-shortcut-hints-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-shortcut-hints-tests PRIVATE
        snow_draw_engine_qt_canvas
        snow_shot_storage
        Qt6::Core
    )
    add_test(
        NAME snow-shot-screenshot-shortcut-hints-tests
        COMMAND snow-shot-screenshot-shortcut-hints-tests
    )
    set_tests_properties(snow-shot-screenshot-shortcut-hints-tests PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=$<IF:$<BOOL:${WIN32}>,windows,offscreen>"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-form-layout-tests
        tests/form_layout_tests.cpp
        include/snow_shot/presentation/screenshotselectionresizemodalcontent.h
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/selection/screenshotselectionresizemodalcontent.cpp
        src/presentation/selection/screenshotselectionparams.cpp
    )
    target_link_libraries(snow-shot-form-layout-tests PRIVATE
        ant_design_qt
        Qt6::Widgets
    )
    target_include_directories(snow-shot-form-layout-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(
        NAME snow-shot-form-layout-tests
        COMMAND snow-shot-form-layout-tests
    )
    set_tests_properties(snow-shot-form-layout-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-selection-resize-workflow-tests
        tests/screenshot_selection_resize_workflow_tests.cpp
        include/snow_shot/presentation/screenshotselectioneditworkflowports.h
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/screenshotselectionresizemodalcontent.h
        include/snow_shot/presentation/screenshotselectionresizeworkflow.h
        include/snow_shot/presentation/screenshotselectionsettingsstore.h
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/services/languagemanager.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/selection/screenshotselectionresizemodalcontent.cpp
        src/presentation/selection/screenshotselectionresizeworkflow.cpp
        src/presentation/selection/screenshotselectionsettingsstore.cpp
    )
    target_include_directories(snow-shot-selection-resize-workflow-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-selection-resize-workflow-tests PRIVATE
        ant_design_qt
        Qt6::Widgets
    )
    snow_shot_add_translations(snow-shot-selection-resize-workflow-tests)
    add_test(
        NAME snow-shot-selection-resize-workflow-tests
        COMMAND snow-shot-selection-resize-workflow-tests
    )
    set_tests_properties(snow-shot-selection-resize-workflow-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-selection-effects-tests
        tests/screenshot_selection_effects_tests.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotinteractionstate.cpp
        src/presentation/selection/screenshotselectioneditworkflow.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/selection/screenshotselectionsettingsstore.cpp
    )
    target_link_libraries(snow-shot-selection-effects-tests PRIVATE
        snow_shot_storage snow_draw_engine_qt_canvas Qt6::Widgets)
    add_test(NAME snow-shot-selection-effects-tests COMMAND snow-shot-selection-effects-tests)
    add_executable(snow-shot-direct-capture-workflow-tests
        tests/direct_capture_workflow_tests.cpp
        src/presentation/capture/directcaptureworkflow.cpp
    )
    target_include_directories(snow-shot-direct-capture-workflow-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-direct-capture-workflow-tests PRIVATE
        snow_shot_image_codec
        Qt6::Gui
    )
    add_test(NAME snow-shot-direct-capture-workflow-tests
        COMMAND snow-shot-direct-capture-workflow-tests)

    if(APPLE)
        add_executable(snow-shot-camera-shutter-sound-tests
            tests/camera_shutter_sound_tests.mm
            src/presentation/capture/camerashuttersound.cpp
            src/platform/macos/camerashuttersound.mm)
        target_include_directories(snow-shot-camera-shutter-sound-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/capture")
        target_link_libraries(snow-shot-camera-shutter-sound-tests PRIVATE
            Qt6::Core "-framework AVFoundation" "-framework Foundation" objc)
        qt_add_resources(snow-shot-camera-shutter-sound-tests "snow_shot_shutter_audio_tests"
            PREFIX "/snow-shot"
            BASE "resources"
            FILES "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}")
        add_test(NAME snow-shot-camera-shutter-sound-tests
            COMMAND snow-shot-camera-shutter-sound-tests)
        set_tests_properties(snow-shot-camera-shutter-sound-tests PROPERTIES
            LABELS "unit" TIMEOUT 30)
    endif()

    add_executable(snow-shot-direct-capture-frame-tests tests/direct_capture_frame_tests.cpp)
    target_include_directories(snow-shot-direct-capture-frame-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/capture"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-recording-c/include")
    target_link_libraries(snow-shot-direct-capture-frame-tests PRIVATE Qt6::Gui)
    add_test(NAME snow-shot-direct-capture-frame-tests COMMAND snow-shot-direct-capture-frame-tests)

    add_executable(snow-shot-direct-capture-native-tests
        tests/direct_capture_native_tests.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/presentation/capture/directcapturenative.cpp)
    snow_shot_alias_test_rust_ffi(snow-shot-direct-capture-native-tests
        snow_capture_target_screenshot
        snow_capture_desktop_session_capture
        snow_capture_desktop_session_create
        snow_capture_desktop_session_destroy
        snow_capture_frame_lease_release
        snow_capture_last_error_message
        snow_capture_screenshot_result_destroy
        snow_capture_screenshot_result_display_count
        snow_capture_screenshot_result_display_geometry
        snow_capture_screenshot_result_display_info
        snow_capture_screenshot_result_display_retain
        snow_capture_screenshot_result_focused_window_geometry
        snow_capture_screenshot_result_focused_window_info
        snow_capture_screenshot_result_focused_window_retain)
    target_include_directories(snow-shot-direct-capture-native-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/capture"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-recording-c/include")
    target_link_libraries(snow-shot-direct-capture-native-tests PRIVATE snow_shot_storage
        snow_shot_image_codec Qt6::Gui)
    add_test(NAME snow-shot-direct-capture-native-tests COMMAND snow-shot-direct-capture-native-tests)

    if(WIN32)
        # Explicit opt-in: captures a visible fixture and its actual monitor.
        add_executable(snow-shot-direct-capture-native-smoke
            tests/direct_capture_native_smoke.cpp
            src/presentation/capture/directcapturenative.cpp)
        target_compile_definitions(snow-shot-direct-capture-native-smoke PRIVATE NOMINMAX)
        target_include_directories(snow-shot-direct-capture-native-smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/capture"
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-recording-c/include")
        target_link_libraries(snow-shot-direct-capture-native-smoke PRIVATE
            Qt6::Widgets snow_shot_image_codec snow_capture_c snow_recording_c PkgConfig::SNOW_SHOT_FFMPEG
            d3d11 dxgi dxguid dwmapi gdi32 ole32 oleaut32 runtimeobject
            user32 userenv winmm windowsapp ws2_32 advapi32 bcrypt psapi)
    endif()

    add_executable(snow-shot-capture-worker-tests
        tests/screenshot_capture_worker_tests.cpp
        include/snow_shot/presentation/screenshotcapturecoordinator.h
        src/presentation/capture/screenshotcapturecoordinator.cpp
        src/presentation/capture/screenshotcaptureworker.cpp
        src/presentation/capture/screenshotcapturepolicy.cpp
    )
    snow_shot_alias_test_rust_ffi(snow-shot-capture-worker-tests
        snow_capture_cancellation_token_cancel
        snow_capture_cancellation_token_create
        snow_capture_cancellation_token_destroy
        snow_capture_cursor_snapshot_create
        snow_capture_cursor_snapshot_destroy
        snow_capture_desktop_layout_count
        snow_capture_desktop_layout_destroy
        snow_capture_desktop_layout_display
        snow_capture_desktop_session_capture
        snow_capture_desktop_session_capture_with_layout
        snow_capture_desktop_session_create
        snow_capture_desktop_session_destroy
        snow_capture_desktop_session_layout_snapshot
        snow_capture_desktop_session_prepare
        snow_capture_desktop_session_refresh_layout
        snow_capture_desktop_session_reset_to_prepared
        snow_capture_desktop_session_state
        snow_capture_frame_lease_release
        snow_capture_last_error_message
        snow_capture_screenshot_result_composite_cursor
        snow_capture_screenshot_result_cursor_patch
        snow_capture_screenshot_result_destroy
        snow_capture_screenshot_result_display_count
        snow_capture_screenshot_result_display_geometry
        snow_capture_screenshot_result_display_info
        snow_capture_screenshot_result_display_retain
        snow_capture_screenshot_result_focused_window_geometry)
    target_include_directories(snow-shot-capture-worker-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/capture"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-recording-c/include")
    target_link_libraries(snow-shot-capture-worker-tests PRIVATE snow_shot_storage Qt6::Gui)
    add_test(NAME snow-shot-capture-worker-tests COMMAND snow-shot-capture-worker-tests)

    if(WIN32)
        add_executable(snow-shot-capture-policy-tests
            tests/screenshot_capture_policy_tests.cpp
            src/presentation/capture/screenshotcapturepolicy.cpp)
        target_include_directories(snow-shot-capture-policy-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/capture"
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
        target_link_libraries(snow-shot-capture-policy-tests PRIVATE Qt6::Gui user32)
        add_test(NAME snow-shot-capture-policy-tests COMMAND snow-shot-capture-policy-tests)
        set_tests_properties(snow-shot-capture-policy-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

        add_executable(snow-shot-recapture-cursor-native-e2e-test
            tests/recapture_cursor_native_e2e_test.cpp
            include/snow_shot/presentation/screenshotcapturecoordinator.h
            src/presentation/capture/screenshotcapturecoordinator.cpp
            src/presentation/capture/screenshotcaptureworker.cpp
            src/presentation/capture/screenshotcapturepolicy.cpp
            src/platform/windows/windowchrome.cpp)
        target_compile_definitions(snow-shot-recapture-cursor-native-e2e-test PRIVATE NOMINMAX)
        target_include_directories(snow-shot-recapture-cursor-native-e2e-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/capture"
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
        target_link_libraries(snow-shot-recapture-cursor-native-e2e-test PRIVATE
            snow_shot_storage Qt6::Widgets snow_capture_c PkgConfig::SNOW_SHOT_FFMPEG
            d3d11 dxgi dxguid dwmapi gdi32 ole32 oleaut32 runtimeobject
            user32 userenv winmm windowsapp ws2_32 advapi32 bcrypt psapi)
        add_test(NAME snow-shot-recapture-cursor-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native)
        add_test(NAME snow-shot-recapture-cursor-movement-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --move-during-refresh)
        add_test(NAME snow-shot-recapture-cursor-snapshot-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --change-after-ready)
        add_test(NAME snow-shot-recapture-class-cursor-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --native-target)
        add_test(NAME snow-shot-recapture-stock-crosshair-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --native-target --stock-crosshair)
        add_test(NAME snow-shot-recapture-cursor-handover-movement-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --native-target --move-after-handover)
        add_test(NAME snow-shot-recapture-cursor-hidden-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --hide-overlay)
        add_test(NAME snow-shot-recapture-cursor-own-window-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --same-thread)
        add_test(NAME snow-shot-recapture-cursor-unchanged-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --same-thread --same-cursor)
        add_test(NAME snow-shot-recapture-cursor-own-hidden-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --same-thread --hide-overlay)
        add_test(NAME snow-shot-recapture-cursor-own-titlebar-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --same-thread --nonclient)
        set_tests_properties(snow-shot-recapture-cursor-native-e2e-test
            snow-shot-recapture-cursor-movement-native-e2e-test
            snow-shot-recapture-cursor-snapshot-native-e2e-test
            snow-shot-recapture-class-cursor-native-e2e-test
            snow-shot-recapture-stock-crosshair-native-e2e-test
            snow-shot-recapture-cursor-handover-movement-native-e2e-test
            snow-shot-recapture-cursor-hidden-native-e2e-test
            snow-shot-recapture-cursor-own-window-native-e2e-test
            snow-shot-recapture-cursor-unchanged-native-e2e-test
            snow-shot-recapture-cursor-own-hidden-native-e2e-test
            snow-shot-recapture-cursor-own-titlebar-native-e2e-test PROPERTIES
            LABELS "interactive;e2e;windows"
            RUN_SERIAL TRUE
            TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=windows")
    endif()

    add_executable(snow-shot-capture-workflow-tests
        tests/screenshot_capture_workflow_tests.cpp
        src/presentation/toolbar/screenshottoolbarpresentationstatefactory.cpp
        src/presentation/selector/screenshotselectorworkflow.cpp
        include/snow_shot/presentation/screenshotcapturedisplaymodelreconciler.h
        include/snow_shot/presentation/screenshotcapturestate.h
        include/snow_shot/presentation/screenshotcaptureworkflow.h
        include/snow_shot/presentation/screenshotcaptureworkflowports.h
        include/snow_shot/presentation/screenshotdisplaysession.h
        include/snow_shot/presentation/screenshotgeometry.h
        include/snow_shot/presentation/screenshotintelligentselectionmodel.h
        include/snow_shot/presentation/screenshotinteractionstate.h
        include/snow_shot/presentation/screenshotselectiongeometry.h
        include/snow_shot/presentation/screenshotselectionmodel.h
        include/snow_shot/presentation/screenshotselectionparams.h
        src/presentation/capture/screenshotcapturedisplaymodelreconciler.cpp
        src/presentation/capture/screenshotcaptureworkflow.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotintelligentselectionmodel.cpp
        src/presentation/core/screenshotinteractionstate.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
    )
    target_include_directories(snow-shot-capture-workflow-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-capture-workflow-tests PRIVATE
        Qt6::Gui
        snow_draw_engine_qt_canvas
    )
    add_test(
        NAME snow-shot-capture-workflow-tests
        COMMAND snow-shot-capture-workflow-tests
    )

    add_executable(snow-shot-capture-input-blockers-tests
        tests/screenshot_capture_input_blockers_tests.cpp
        src/presentation/capture/screenshotcaptureinputblockers.h
    )
    target_link_libraries(snow-shot-capture-input-blockers-tests PRIVATE Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-capture-input-blockers-tests)
    add_test(NAME snow-shot-capture-input-blockers-tests
        COMMAND snow-shot-capture-input-blockers-tests)
    set_tests_properties(snow-shot-capture-input-blockers-tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-tool-command-workflow-tests
        tests/screenshot_tool_command_workflow_tests.cpp
        include/snow_shot/presentation/screenshotcapturestate.h
        include/snow_shot/presentation/screenshotdisplaysession.h
        include/snow_shot/presentation/screenshotgeometry.h
        include/snow_shot/presentation/screenshotintelligentselectionmodel.h
        include/snow_shot/presentation/screenshotinteractionstate.h
        include/snow_shot/presentation/screenshotselectiongeometry.h
        include/snow_shot/presentation/screenshotselectionmodel.h
        include/snow_shot/presentation/screenshotselectionparams.h
        include/snow_shot/presentation/screenshottoolcommandworkflow.h
        include/snow_shot/presentation/screenshottoolcommandworkflowports.h
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotintelligentselectionmodel.cpp
        src/presentation/core/screenshotinteractionstate.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/tools/screenshottoolcommandworkflow.cpp
    )
    target_include_directories(snow-shot-tool-command-workflow-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-tool-command-workflow-tests PRIVATE
        Qt6::Widgets
        snow_draw_engine_qt_canvas
    )
    add_test(
        NAME snow-shot-tool-command-workflow-tests
        COMMAND snow-shot-tool-command-workflow-tests
    )

    add_executable(snow-shot-smart-selection-transition-tests
        tests/screenshot_smart_selection_transition_tests.cpp
        include/snow_shot/presentation/screenshotsmartselectiontransition.h
        src/presentation/core/screenshotsmartselectiontransition.cpp
    )
    target_include_directories(snow-shot-smart-selection-transition-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-smart-selection-transition-tests PRIVATE
        Qt6::Core
    )
    add_test(
        NAME snow-shot-smart-selection-transition-tests
        COMMAND snow-shot-smart-selection-transition-tests
    )

    add_executable(snow-shot-multi-region-tests
        tests/screenshot_multi_region_tests.cpp
        src/presentation/core/screenshotsmartselectiontransition.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotinteractionstate.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/storage/persistedselectioncodec.cpp
    )
    target_include_directories(snow-shot-multi-region-tests PRIVATE include)
    target_compile_definitions(snow-shot-multi-region-tests PRIVATE SNOW_SHOT_BENCH_INTERNALS=1)
    target_link_libraries(snow-shot-multi-region-tests PRIVATE
        Qt6::Gui snow_draw_engine_qt_canvas ant_design_qt)
    if(WIN32)
        target_link_libraries(snow-shot-multi-region-tests PRIVATE user32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-multi-region-tests)
    add_test(NAME snow-shot-multi-region-tests COMMAND snow-shot-multi-region-tests)
    add_test(NAME snow-shot-region-overlay-input-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --region-input-only -platform offscreen)
    add_test(NAME snow-shot-region-snapping-rendering-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --region-snapping-only -platform offscreen)
    set_tests_properties(snow-shot-region-snapping-rendering-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-screenshot-magnifier-wheel-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --magnifier-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-magnifier-wheel-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-screenshot-angle-wheel-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --angle-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-angle-wheel-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-screenshot-bracket-wheel-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --bracket-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-bracket-wheel-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    set_tests_properties(snow-shot-region-overlay-input-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-independent-region-input-tests
        COMMAND snow-shot-screenshot-history-tests --independent-regions-only)
    set_tests_properties(snow-shot-independent-region-input-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit")
    add_test(NAME snow-shot-region-shape-input-tests COMMAND snow-shot-screenshot-history-tests --region-shapes-only)
    set_tests_properties(snow-shot-region-shape-input-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit")
    set_tests_properties(snow-shot-multi-region-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}" LABELS "unit")

    add_executable(snow-shot-selection-geometry-tests
        tests/screenshot_selection_geometry_tests.cpp
        include/snow_shot/presentation/screenshotinteractionstate.h
        src/presentation/core/screenshotinteractionstate.cpp
        include/snow_shot/platform/windows/monitorgeometry.h
        include/snow_shot/presentation/historypinplacement.h
        include/snow_shot/presentation/screenshotgeometry.h
        include/snow_shot/presentation/screenshotresultcompositor.h
        include/snow_shot/presentation/screenshotselectionpin.h
        include/snow_shot/presentation/screenshotselectiongeometry.h
        include/snow_shot/presentation/screenshotselectionmodel.h
        include/snow_shot/presentation/screenshotselectionparams.h
        include/snow_shot/presentation/screenshotselectionshadowrenderer.h
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/services/historypinplacement.cpp
        src/presentation/services/screenshotselectionpin.cpp
    )
    target_include_directories(snow-shot-selection-geometry-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-selection-geometry-tests PRIVATE
        snow_shot_storage
        snow_draw_engine_qt_canvas
        ant_design_qt
        Qt6::Gui
    )
    if(WIN32)
        target_link_libraries(snow-shot-selection-geometry-tests PRIVATE user32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-selection-geometry-tests)
    add_test(
        NAME snow-shot-selection-geometry-tests
        COMMAND snow-shot-selection-geometry-tests
    )
    set_tests_properties(snow-shot-selection-geometry-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(WIN32)
        # Re-runs the binary on the real Windows platform, where the native monitor query
        # inside historyPinDesktopUsesNativeMonitorRects executes; the offscreen pass
        # skips that query by design.
        add_test(
            NAME snow-shot-selection-geometry-windows-desktop-tests
            COMMAND snow-shot-selection-geometry-tests
        )
        set_tests_properties(snow-shot-selection-geometry-windows-desktop-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            LABELS "unit;windows"
        )
        add_test(NAME snow-shot-selection-geometry-windows-fractional-tests
            COMMAND snow-shot-selection-geometry-tests)
        set_tests_properties(snow-shot-selection-geometry-windows-fractional-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_SCALE_FACTOR=1.5;QT_ENABLE_HIGHDPI_SCALING=1;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            LABELS "unit;windows"
        )
    endif()

    add_executable(snow-shot-selection-export-workflow-tests
        tests/screenshot_selection_export_workflow_tests.cpp
        include/snow_shot/presentation/screenshotclipboardservice.h
        include/snow_shot/presentation/screenshotselectionexportworkflow.h
        include/snow_shot/presentation/screenshotselectionexportworkflowports.h
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/selection/screenshotselectionexportworkflow.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_include_directories(snow-shot-selection-export-workflow-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-selection-export-workflow-tests PRIVATE
        snow_shot_diagnostics
        snow_shot_image_codec
        snow_draw_engine_qt_canvas
        Qt6::Gui
    )
    if(WIN32)
        target_link_libraries(snow-shot-selection-export-workflow-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-selection-export-workflow-tests
             COMMAND snow-shot-selection-export-workflow-tests)
    set_tests_properties(snow-shot-selection-export-workflow-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-screenshot-color-picker-tests
        tests/screenshot_color_picker_tests.cpp
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/services/screenshotcolorpickerwindow.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
    )
    snow_shot_link_macos_window_platform(snow-shot-screenshot-color-picker-tests)
    target_include_directories(snow-shot-screenshot-color-picker-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-color-picker-tests PRIVATE
        snow_shot_storage snow_draw_engine_qt_canvas ant_design_qt Qt6::Widgets
    )
    snow_shot_import_offscreen_platform(snow-shot-screenshot-color-picker-tests)
    if(APPLE)
        set_source_files_properties(tests/screenshot_color_picker_tests.cpp
            PROPERTIES LANGUAGE OBJCXX)
        target_link_libraries(snow-shot-screenshot-color-picker-tests PRIVATE "-framework AppKit")
        add_test(NAME snow-shot-macos-color-picker-animation-native-tests
            COMMAND snow-shot-screenshot-color-picker-tests --magnifier-animation-only -platform cocoa)
        set_tests_properties(snow-shot-macos-color-picker-animation-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 30)
    endif()
    add_test(NAME snow-shot-screenshot-color-picker-tests
        COMMAND snow-shot-screenshot-color-picker-tests)
    set_tests_properties(snow-shot-screenshot-color-picker-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-screenshot-history-tests
        tests/screenshot_history_tests.cpp
        src/presentation/overlay/screenshotoverlayinteractionadapter.cpp
        src/presentation/selector/screenshotselectorworkflow.cpp
        src/presentation/capture/directcapturehistory.cpp
        include/snow_shot/presentation/screenshothistoryservice.h
        include/snow_shot/presentation/screenshotdisplaysession.h
        include/snow_shot/presentation/screenshotgeometry.h
        include/snow_shot/presentation/screenshotintelligentselectionmodel.h
        include/snow_shot/presentation/screenshotinteractionstate.h
        include/snow_shot/presentation/screenshotoverlayinputhandler.h
        include/snow_shot/presentation/screenshotoverlayshortcutcontroller.h
        include/snow_shot/presentation/screenshotshortcutexitconfirmation.h
        include/snow_shot/presentation/screenshotselectiongeometry.h
        include/snow_shot/presentation/screenshotselectionmodel.h
        include/snow_shot/presentation/screenshotselectionparams.h
        src/presentation/services/screenshothistoryservice.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/presentation/services/screenshotshortcutexitconfirmation.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotintelligentselectionmodel.cpp
        src/presentation/core/screenshotinteractionstate.cpp
        src/presentation/overlay/screenshotoverlayinputhandler.cpp
        src/presentation/overlay/screenshotoverlayshortcutcontroller.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
    )
    target_include_directories(snow-shot-screenshot-history-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
    )
    target_link_libraries(snow-shot-screenshot-history-tests PRIVATE
        snow_draw_engine_qt_canvas
        snow_shot_image_codec
        snow_shot_window_shortcuts
        ant_design_qt
        Qt6::Widgets
    )
    add_test(
        NAME snow-shot-screenshot-history-tests
        COMMAND snow-shot-screenshot-history-tests
    )
    add_test(
        NAME snow-shot-direct-capture-history-tests
        COMMAND snow-shot-screenshot-history-tests --direct-capture-only
    )
    add_test(
        NAME snow-shot-history-metadata-lifecycle-tests
        COMMAND snow-shot-screenshot-history-tests --history-metadata-lifecycle-only
    )
    add_test(
        NAME snow-shot-selection-border-resize-tests
        COMMAND snow-shot-screenshot-history-tests --selection-border-resize-only
    )
    add_test(NAME snow-shot-selection-aspect-ratio-confirmation-tests
        COMMAND snow-shot-screenshot-history-tests --selection-aspect-ratio-only)
    add_test(NAME snow-shot-screenshot-eraser-wheel-tests
        COMMAND snow-shot-screenshot-history-tests --eraser-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-eraser-wheel-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-smart-selection-wheel-tests
        COMMAND snow-shot-screenshot-history-tests --smart-selection-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-smart-selection-wheel-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    snow_shot_import_offscreen_platform(snow-shot-screenshot-history-tests)
    add_test(NAME snow-shot-select-all-screens-shortcut-tests
        COMMAND snow-shot-screenshot-history-tests --select-all-screens-only -platform offscreen)
    set_tests_properties(snow-shot-select-all-screens-shortcut-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-previous-selection-shortcut-tests
        COMMAND snow-shot-screenshot-history-tests --previous-selection-only -platform offscreen)
    set_tests_properties(snow-shot-previous-selection-shortcut-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(
        NAME snow-shot-screenshot-completion-gesture-tests
        COMMAND snow-shot-screenshot-history-tests --completion-gestures-only
    )
    set_tests_properties(
        snow-shot-direct-capture-history-tests
        snow-shot-history-metadata-lifecycle-tests
        snow-shot-selection-border-resize-tests
        snow-shot-selection-aspect-ratio-confirmation-tests
        snow-shot-screenshot-completion-gesture-tests
        PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(
        NAME snow-shot-screenshot-shortcut-priority-tests
        COMMAND snow-shot-screenshot-history-tests --shortcut-input-only
    )
    add_test(NAME snow-shot-global-screenshot-shortcut-tests
        COMMAND snow-shot-screenshot-history-tests --global-screenshot-shortcuts-only -platform offscreen)
    set_tests_properties(snow-shot-global-screenshot-shortcut-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-screenshot-exit-confirmation-tests
        COMMAND snow-shot-screenshot-history-tests --shortcut-exit-confirmation-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-exit-confirmation-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(
        NAME snow-shot-intelligent-selection-cursor-shortcut-tests
        COMMAND snow-shot-screenshot-history-tests
                --intelligent-selection-cursor-shortcut-only
    )
    add_test(NAME snow-shot-screenshot-cursor-shortcut-tests
        COMMAND snow-shot-screenshot-history-tests --cursor-shortcuts-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-cursor-shortcut-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(
        NAME snow-shot-canvas-color-sampling-tests
        COMMAND snow-shot-screenshot-history-tests --canvas-color-sampling-only
    )
    set_tests_properties(
        snow-shot-screenshot-history-tests
        snow-shot-screenshot-shortcut-priority-tests
        snow-shot-intelligent-selection-cursor-shortcut-tests
        snow-shot-canvas-color-sampling-tests
        PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=$<IF:$<BOOL:${WIN32}>,windows,offscreen>"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    if(SNOW_APPS_BUILD_BENCHMARKS)
        add_executable(snow-shot-scrolling-preview-benchmark
            tests/scrolling_preview_benchmark.cpp
            include/snow_shot/presentation/screenshotscrollingtypes.h
            include/snow_shot/presentation/screenshotscrollingthumbnailwidget.h
            src/presentation/capture/screenshotscrollingthumbnailwidget.cpp
        )
        target_compile_definitions(snow-shot-scrolling-preview-benchmark PRIVATE
            SNOW_SHOT_BENCH_INTERNALS=1
        )
        target_include_directories(snow-shot-scrolling-preview-benchmark PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(snow-shot-scrolling-preview-benchmark PRIVATE
            snow_canvas_image
            ant_design_qt
            Qt6::Widgets
        )
        snow_shot_import_offscreen_platform(snow-shot-scrolling-preview-benchmark)

        add_executable(snow-shot-scrolling-result-async-benchmark
            tests/scrolling_result_async_benchmark.cpp
        )
        target_include_directories(snow-shot-scrolling-result-async-benchmark PRIVATE
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-stitch-images-c/include"
        )
        target_link_libraries(snow-shot-scrolling-result-async-benchmark PRIVATE
            snow_stitch_images_c
            snow_shot_image_codec
            Qt6::Widgets
        )
        if(WIN32)
            target_link_libraries(snow-shot-scrolling-result-async-benchmark PRIVATE
                Ntdll
                Ws2_32
            )
        endif()
    endif()

    # This test is intentionally Qt-free; swap in the Qt-free dialog guard so
    # linking the guard does not add a Qt runtime dependency.
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_native)
    add_executable(snow-shot-latest-frame-mailbox-tests
        tests/latest_frame_mailbox_tests.cpp
        src/presentation/capture/latestframemailbox.h
    )
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_qt)
    target_include_directories(snow-shot-latest-frame-mailbox-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME snow-shot-latest-frame-mailbox-tests
        COMMAND snow-shot-latest-frame-mailbox-tests
    )

    add_executable(snow-shot-scrolling-capture-cadence-tests
        tests/scrolling_capture_cadence_tests.cpp
        src/presentation/capture/adaptivescrollingcapturecadence.h
    )
    add_executable(snow-shot-scrolling-selection-movement-tests
        tests/scrolling_selection_movement_tests.cpp)
    target_include_directories(snow-shot-scrolling-selection-movement-tests PRIVATE include src)
    target_link_libraries(snow-shot-scrolling-selection-movement-tests PRIVATE Qt6::Core Qt6::Gui)
    add_test(NAME snow-shot-scrolling-selection-movement-tests
        COMMAND snow-shot-scrolling-selection-movement-tests)

    add_executable(snow-shot-scrolling-auto-scroll-tests
        tests/scrolling_auto_scroll_tests.cpp
        src/platform/windows/scrollinput.cpp
    )
    target_include_directories(snow-shot-scrolling-auto-scroll-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-scrolling-auto-scroll-tests PRIVATE Qt6::Core Qt6::Gui)
    add_test(NAME snow-shot-scrolling-auto-scroll-tests
        COMMAND snow-shot-scrolling-auto-scroll-tests)
    target_include_directories(snow-shot-scrolling-capture-cadence-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME snow-shot-scrolling-capture-cadence-tests
        COMMAND snow-shot-scrolling-capture-cadence-tests
    )

    add_executable(snow-shot-screenshot-canvas-renderer-tests
        tests/screenshot_canvas_renderer_tests.cpp
        tests/screenshot_session_input_cleanup_tests.cpp
        tests/screenshot_overlay_input_tests.cpp
        tests/screenshot_selection_effect_editor_tests.cpp
        src/presentation/overlay/screenshotoverlayinputhandler.cpp
        src/presentation/overlay/screenshotoverlayinteractionadapter.cpp
        src/presentation/overlay/screenshotoverlayshortcutcontroller.cpp
        include/snow_shot/presentation/screenshotoverlayshortcutcontroller.h
        src/presentation/selection/screenshotselectioneditworkflow.cpp
        src/presentation/selection/screenshotselectionsettingsstore.cpp
        tests/screenshot_cursor_tests.cpp
        src/presentation/services/screenshotsourceimagecomposer.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotpdfexport.cpp
        include/snow_shot/presentation/screenshothistoryservice.h
        src/presentation/capture/directcapturehistory.cpp
        src/presentation/services/screenshothistoryservice.cpp
        src/presentation/core/screenshotintelligentselectionmodel.cpp
        src/presentation/core/screenshotinteractionstate.cpp
        include/snow_shot/presentation/screenshotocrtextlayer.h
        include/snow_shot/presentation/screenshotmessageservice.h
        include/snow_shot/presentation/screenshotoverlaywindow.h
        include/snow_shot/presentation/screenshotoverlaypool.h
        include/snow_shot/presentation/screenshotscrollingtypes.h
        include/snow_shot/presentation/screenshotscrollingthumbnailwidget.h
        src/presentation/capture/screenshotscrollingthumbnailwidget.cpp
        src/presentation/capture/screenshotcapturedisplaymodelreconciler.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/overlay/screenshotoverlaycanvaspresenter.cpp
        src/presentation/overlay/screenshotoverlayframepresenter.cpp
        src/presentation/overlay/screenshotoverlayframepresenter.h
        src/presentation/overlay/screenshotoverlaycanvaswidget.cpp
        src/presentation/overlay/screenshotoverlaypool.cpp
        src/presentation/overlay/screenshotoverlaywindow.cpp
        src/presentation/services/screenshotmessageservice.cpp
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
        src/presentation/ocr/screenshottabledocument.cpp
    )
    if(SNOW_APPS_BUILD_BENCHMARKS)
        target_sources(snow-shot-screenshot-canvas-renderer-tests PRIVATE
            tests/screenshot_cursor_performance_benchmark.cpp)
        target_compile_definitions(snow-shot-screenshot-canvas-renderer-tests PRIVATE
            SNOW_SHOT_CURSOR_BENCHMARK=1)
        target_sources(snow-shot-screenshot-canvas-renderer-tests PRIVATE
            tests/screenshot_ocr_highlighting_performance_benchmark.cpp)
        target_compile_definitions(snow-shot-screenshot-canvas-renderer-tests PRIVATE
            SNOW_SHOT_OCR_HIGHLIGHTING_BENCHMARK=1)
        add_test(NAME snow-shot-ocr-highlighting-performance-benchmark
            COMMAND snow-shot-screenshot-canvas-renderer-tests --ocr-highlighting-benchmark -platform offscreen)
        set_tests_properties(snow-shot-ocr-highlighting-performance-benchmark PROPERTIES
            LABELS "benchmark" TIMEOUT 120
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        if(WIN32)
            target_link_libraries(snow-shot-screenshot-canvas-renderer-tests PRIVATE Psapi)
        endif()
        add_test(NAME snow-shot-screenshot-cursor-benchmark
            COMMAND snow-shot-screenshot-canvas-renderer-tests --cursor-benchmark -platform offscreen)
        set_tests_properties(snow-shot-screenshot-cursor-benchmark PROPERTIES LABELS "benchmark")
    endif()
    add_test(NAME snow-shot-screenshot-cursor-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --cursor-only -platform offscreen)
    add_test(NAME snow-shot-magnifier-image-source-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --magnifier-only -platform offscreen)
    set_tests_properties(snow-shot-magnifier-image-source-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    set_tests_properties(snow-shot-screenshot-cursor-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-selection-shortcut-hint-stages-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --shortcut-hints-only -platform offscreen)
    set_tests_properties(snow-shot-selection-shortcut-hint-stages-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-selection-effect-editor-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --selection-effect-editors-only -platform offscreen)
    set_tests_properties(snow-shot-selection-effect-editor-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-selection-effect-preview-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --selection-effect-preview-only -platform offscreen)
    set_tests_properties(snow-shot-selection-effect-preview-tests PROPERTIES LABELS "unit")
    snow_shot_link_macos_window_platform(snow-shot-screenshot-canvas-renderer-tests)
    target_include_directories(snow-shot-screenshot-canvas-renderer-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_compile_definitions(snow-shot-screenshot-canvas-renderer-tests PRIVATE
        SNOW_SHOT_BENCH_INTERNALS=1
    )
    target_link_libraries(snow-shot-screenshot-canvas-renderer-tests PRIVATE
        snow_shot_diagnostics
        ant_design_qt
        Qt6::Widgets
        Qt6::GuiPrivate
        snow_shot_storage
        snow_shot_window_shortcuts
        snow_draw_engine_qt_canvas
    )
    add_test(
        NAME snow-shot-screenshot-canvas-renderer-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests
    )
    add_test(NAME snow-shot-screenshot-guide-preferences-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --screenshot-ui-preferences)
    add_test(NAME snow-shot-screenshot-guide-rendering-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --guide-rendering-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-guide-rendering-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-overlay-text-wrap-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --text-wrap-only -platform offscreen)
    set_tests_properties(snow-shot-overlay-text-wrap-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-direct-capture-history-rendering-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --direct-capture-history-rendering)
    add_test(NAME snow-shot-pinned-filter-reference-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --pinned-filter-reference-only -platform offscreen)
    add_test(NAME snow-shot-fractional-dpi-rendering-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --fractional-dpi)
    add_test(NAME snow-shot-pinned-result-scaling-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --pinned-result-scaling -platform offscreen)
    set_tests_properties(snow-shot-pinned-result-scaling-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-fractional-dpi-rendering-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(NAME snow-shot-scrolling-overlay-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --scrolling-overlay -platform offscreen)
    if(WIN32)
        add_test(NAME snow-shot-scrolling-overlay-native-tests
            COMMAND snow-shot-screenshot-canvas-renderer-tests --scrolling-overlay -platform windows)
        set_tests_properties(snow-shot-scrolling-overlay-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-overlay-safe-area-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --overlay-safe-area -platform offscreen)
    add_test(NAME snow-shot-overlay-window-shadow-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --window-shadow-only -platform offscreen)
    set_tests_properties(snow-shot-overlay-window-shadow-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-overlay-boundary-input-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --overlay-boundary-input -platform offscreen)
    set_tests_properties(snow-shot-overlay-boundary-input-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60)
    foreach(_overlay_boundary_dpr IN ITEMS 1.25 1.5 1.75 2)
        set(_overlay_boundary_test "snow-shot-overlay-boundary-input-dpr${_overlay_boundary_dpr}-tests")
        add_test(NAME ${_overlay_boundary_test}
            COMMAND snow-shot-screenshot-canvas-renderer-tests --overlay-boundary-input -platform offscreen)
        set_tests_properties(${_overlay_boundary_test} PROPERTIES
            LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
            ENVIRONMENT "QT_SCALE_FACTOR=${_overlay_boundary_dpr};${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    add_test(NAME snow-shot-overlay-backing-store-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --overlay-backing-store-only -platform offscreen)
    add_test(NAME snow-shot-overlay-backing-store-dpr2-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --overlay-backing-store-only -platform offscreen)
    set_tests_properties(snow-shot-overlay-backing-store-tests
        snow-shot-overlay-backing-store-dpr2-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    set_tests_properties(snow-shot-overlay-backing-store-dpr2-tests PROPERTIES
        ENVIRONMENT "QT_SCALE_FACTOR=2")
    if(APPLE)
        target_sources(snow-shot-screenshot-canvas-renderer-tests PRIVATE
            ../cmake/test-support/macos_native_input.mm)
        target_include_directories(snow-shot-screenshot-canvas-renderer-tests PRIVATE
            ../cmake/test-support)
        add_test(NAME snow-shot-macos-overlay-window-shadow-native-tests
            COMMAND snow-shot-screenshot-canvas-renderer-tests --window-shadow-only -platform cocoa)
        set_tests_properties(snow-shot-macos-overlay-window-shadow-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 30)
        add_test(NAME snow-shot-macos-overlay-boundary-input-native-tests
            COMMAND snow-shot-screenshot-canvas-renderer-tests --overlay-boundary-input -platform cocoa)
        set_tests_properties(snow-shot-macos-overlay-boundary-input-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 30)
    endif()
    set_tests_properties(snow-shot-scrolling-overlay-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-overlay-close-release-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --close-release-only -platform offscreen)
    set_tests_properties(snow-shot-direct-capture-history-rendering-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(
        NAME snow-shot-ocr-canvas-renderer-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --ocr-presentation
    )
    add_test(NAME snow-shot-ocr-line-box-rendering-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --ocr-line-box-rendering-only
            -platform offscreen)
    add_test(NAME snow-shot-ocr-renderer-reset-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --ocr-reset-only -platform offscreen)
    add_test(NAME snow-shot-session-input-cleanup-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --session-input-cleanup-only -platform offscreen)
    set_tests_properties(snow-shot-session-input-cleanup-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-ocr-renderer-reset-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-ocr-line-box-rendering-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(
        NAME snow-shot-overlay-pool-prewarm-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --overlay-pool-prewarm
    )
    set_tests_properties(
        snow-shot-screenshot-canvas-renderer-tests
        snow-shot-screenshot-guide-preferences-tests
        snow-shot-screenshot-guide-rendering-tests
        snow-shot-overlay-text-wrap-tests
        snow-shot-overlay-pool-prewarm-tests
        PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-ocr-canvas-renderer-tests PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(WIN32)
        add_test(
            NAME snow-shot-overlay-native-surface-retirement-test
            COMMAND snow-shot-screenshot-canvas-renderer-tests
                --overlay-native-surface-retirement
        )
        set_tests_properties(snow-shot-overlay-native-surface-retirement-test PROPERTIES
            LABELS "e2e;interactive;windows"
            RUN_SERIAL TRUE
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-overlay-frame-presenter-tests
        tests/screenshot_overlay_frame_presenter_tests.cpp
        src/presentation/overlay/screenshotoverlayframepresenter.cpp
        src/presentation/overlay/screenshotoverlayframepresenter.h
    )
    target_include_directories(snow-shot-overlay-frame-presenter-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/overlay"
    )
    target_compile_definitions(snow-shot-overlay-frame-presenter-tests PRIVATE
        SNOW_SHOT_BENCH_INTERNALS=1
    )
    target_link_libraries(snow-shot-overlay-frame-presenter-tests PRIVATE
        Qt6::Widgets
    )
    snow_shot_import_offscreen_platform(snow-shot-overlay-frame-presenter-tests)
    add_test(
        NAME snow-shot-overlay-frame-presenter-tests
        COMMAND snow-shot-overlay-frame-presenter-tests
    )
    set_tests_properties(snow-shot-overlay-frame-presenter-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(WIN32)
        target_link_libraries(snow-shot-overlay-frame-presenter-tests PRIVATE dwmapi)
        add_test(
            NAME snow-shot-overlay-frame-presenter-native-reveal-test
            COMMAND snow-shot-overlay-frame-presenter-tests --native-reveal
        )
        set_tests_properties(snow-shot-overlay-frame-presenter-native-reveal-test PROPERTIES
            LABELS "e2e;interactive;windows"
            RUN_SERIAL TRUE
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-pinned-geometry-mapping-tests
        tests/screenshot_pinned_geometry_mapping_tests.cpp)
    target_include_directories(snow-shot-pinned-geometry-mapping-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/pinned")
    target_link_libraries(snow-shot-pinned-geometry-mapping-tests PRIVATE Qt6::Core)
    add_test(NAME snow-shot-pinned-geometry-mapping-tests COMMAND snow-shot-pinned-geometry-mapping-tests)
    set_tests_properties(snow-shot-pinned-geometry-mapping-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE)

    add_executable(snow-shot-pinned-shake-detector-tests
        tests/screenshot_pinned_shake_detector_tests.cpp)
    target_include_directories(snow-shot-pinned-shake-detector-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/pinned")
    target_link_libraries(snow-shot-pinned-shake-detector-tests PRIVATE Qt6::Core)
    add_test(NAME snow-shot-pinned-shake-detector-tests COMMAND snow-shot-pinned-shake-detector-tests)
    set_tests_properties(snow-shot-pinned-shake-detector-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE)

    add_executable(snow-shot-pinned-resize-geometry-tests
        tests/screenshot_pinned_resize_geometry_tests.cpp
        src/presentation/pinned/screenshotpinnedresizegeometry.cpp
    )
    target_include_directories(snow-shot-pinned-resize-geometry-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/pinned"
    )
    target_link_libraries(snow-shot-pinned-resize-geometry-tests PRIVATE
        Qt6::Core
    )
    add_test(
        NAME snow-shot-pinned-resize-geometry-tests
        COMMAND snow-shot-pinned-resize-geometry-tests
    )

    add_executable(snow-shot-pinned-native-geometry-controller-tests
        tests/screenshot_pinned_native_geometry_controller_tests.cpp
        src/presentation/pinned/screenshotpinnednativegeometrycontroller.cpp
        src/presentation/pinned/screenshotpinnedresizegeometry.cpp
    )
    target_include_directories(snow-shot-pinned-native-geometry-controller-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/pinned"
    )
    target_link_libraries(snow-shot-pinned-native-geometry-controller-tests PRIVATE
        Qt6::Core
    )
    add_test(
        NAME snow-shot-pinned-native-geometry-controller-tests
        COMMAND snow-shot-pinned-native-geometry-controller-tests
    )

    add_executable(snow-shot-pinned-restore-geometry-tests
        tests/screenshot_pinned_restore_geometry_tests.cpp
        src/presentation/pinned/screenshotpinnedrestoregeometry.cpp
    )
    target_include_directories(snow-shot-pinned-restore-geometry-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/pinned"
    )
    target_link_libraries(snow-shot-pinned-restore-geometry-tests PRIVATE
        Qt6::Core
    )
    add_test(
        NAME snow-shot-pinned-restore-geometry-tests
        COMMAND snow-shot-pinned-restore-geometry-tests
    )

    add_executable(snow-shot-pinned-window-tests
        tests/spreadsheet_open_test_support.h
        ${SNOW_SHOT_IMAGE_EDITING_SOURCES}
        tests/imageediting_tests.cpp
        tests/imageediting_canvas_tests.cpp
        tests/imageediting_document_tests.cpp
        tests/imageediting_translation_tests.cpp
        tests/imageediting_branch_tests.cpp
        tests/imageediting_lifetime_tests.cpp
        tests/imageediting_close_tests.cpp
        tests/imageediting_source_tests.cpp
        tests/imageediting_smart_erase_tests.cpp
        tests/imageediting_save_tests.cpp
        src/presentation/capture/screenshotscrollingpipeline.cpp
        src/presentation/services/screenshotexportscrollingsource.cpp
        src/presentation/services/screenshotmanualsave.cpp
        src/presentation/services/pinnedtextstylecapture.cpp
        src/app/mcp/mcppinnedstate.cpp
        tests/screenshot_pinned_hide_to_top_tests.cpp
        src/presentation/services/screenshotfilepinbatch.cpp
        tests/screenshot_pinned_window_tests.cpp
        tests/original_image_translation_tests.cpp
        include/snow_shot/presentation/components/pathinput.h
        src/presentation/components/pathinput.cpp
        include/snow_shot/presentation/screenshotsavepreviewcanvas.h
        src/presentation/services/screenshotsaveasfiledialog.cpp
        src/presentation/services/screenshotsavefooter.h
        src/presentation/services/screenshotsavefooter.cpp
        src/presentation/services/screenshotsaveresult.h
        src/presentation/services/screenshotsaveresult.cpp
        src/presentation/services/screenshotsaveexportpipeline.cpp
        src/presentation/services/screenshotsavepreviewcanvas.cpp
        src/platform/physicalcursor.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshotcanvascolorsampler.h
        include/snow_shot/presentation/screenshotcanvascolorsamplerwindow.h
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/screenshotpinnededitcontroller.h
        include/snow_shot/presentation/screenshotpinnedwindow.h
        include/snow_shot/presentation/pinnedwindowgroupmanager.h
        include/snow_shot/presentation/screenshotrecognitionsessioncontroller.h
        include/snow_shot/presentation/screenshotrecognitionwindow.h
        include/snow_shot/presentation/screenshottableeditor.h
        include/snow_shot/presentation/screenshotmessageservice.h
        include/snow_shot/presentation/screenshotselectiongeometry.h
        include/snow_shot/presentation/screenshotselectionmodel.h
        include/snow_shot/presentation/screenshotselectionparams.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
        src/presentation/ocr/screenshotocrtexteditingsession.cpp
        src/presentation/ocr/screenshotocrtexttransform.cpp
        src/presentation/ocr/screenshotrecognitionsessioncontroller.cpp
        src/presentation/ocr/screenshotocrlayout.cpp
        src/presentation/ocr/screenshotrecognitionwindow.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/ocr/screenshottableeditor.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotcanvascolorsampler.cpp
        src/presentation/services/screenshotcanvascolorsamplerwindow.cpp
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/pinnedcolorswatch.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotrecognitionfileexport.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/presentation/services/screenshotmessageservice.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/pinned/screenshotpinnededitcontroller.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
        src/presentation/services/screenshotexportartifact.cpp
        src/presentation/services/screenshotexportpinnedsource.cpp
        src/presentation/ocr/screenshotrecognitionimage.cpp
        src/presentation/selection/screenshotselectionexportuiservices.cpp
        src/presentation/pinned/screenshotpinnednativegeometrycontroller.cpp
        src/presentation/pinned/screenshotpinnedclickthroughgeometry.cpp
        src/presentation/pinned/screenshotpinnedresizegeometry.cpp
        src/presentation/pinned/screenshotpinnedrestoregeometry.cpp
        src/presentation/pinned/screenshotpinnedhidetotopcontroller.cpp
        src/presentation/pinned/screenshotpinnedwindow.cpp
        src/presentation/pinned/screenshotpinnedbatchsave.cpp
        src/presentation/services/pinnedwindowgroupmanager.cpp
        src/presentation/pinned/screenshotpinnedinteraction.cpp
        include/snow_shot/presentation/pinnedwindowselectioncontroller.h
        src/presentation/pinned/pinnedwindowselectioncontroller.cpp
        src/presentation/pinned/pinnedwindowselectioninteraction.cpp
        src/presentation/pinned/pinnedwindowselectiongeometry.cpp
        src/presentation/pinned/screenshotpinneddragexport.cpp
        ${SNOW_PINNED_PLATFORM_SOURCES}
        src/presentation/services/languagemanager.cpp
        src/presentation/styles/buttonborder.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
    )
    if(MSVC)
        set_property(SOURCE tests/screenshot_pinned_window_tests.cpp APPEND PROPERTY
            COMPILE_OPTIONS /bigobj)
    endif()
    snow_shot_link_macos_window_platform(snow-shot-pinned-window-tests)
    target_include_directories(snow-shot-pinned-window-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-stitch-images-c/include"
    )
    target_compile_definitions(snow-shot-pinned-window-tests PRIVATE
        SNOW_SHOT_TEST_HOOKS=1
        SNOW_SHOT_TEST_PINNED_TRANSLATION=1
    )
    if(MSVC)
        target_compile_options(snow-shot-pinned-window-tests PRIVATE /bigobj)
    endif()
    target_link_libraries(snow-shot-pinned-window-tests PRIVATE
        snow_shot_file_manager
        snow_stitch_images_c
        snow_shot_diagnostics
        MINIZIP::minizip-ng
        snow_shot_animation_playback
        snow_shot_translation
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        Qt6::Network
        Qt6::Widgets
        Qt6::GuiPrivate
        snow_draw_engine_qt_canvas
        snow_shot_image_codec
        snow_shot_storage
        snow_shot_window_shortcuts
    )
    if(WIN32)
        target_sources(snow-shot-pinned-window-tests PRIVATE
            src/platform/windows/windowalwaysontop.cpp)
        target_link_libraries(snow-shot-pinned-window-tests PRIVATE
            comctl32
            dwmapi
            user32
            psapi
        )
    endif()
    add_test(
        NAME snow-shot-pinned-window-tests
        COMMAND snow-shot-pinned-window-tests
    )
    set_tests_properties(snow-shot-pinned-window-tests PROPERTIES
        LABELS "interactive;e2e;windows" RUN_SERIAL TRUE
        ENVIRONMENT "QT_QPA_PLATFORM=windows")
    snow_shot_import_offscreen_platform(snow-shot-pinned-window-tests)
    add_test(NAME snow-shot-image-editing-tests
        COMMAND snow-shot-pinned-window-tests --image-editing-only -platform offscreen)
    add_test(NAME snow-shot-image-editing-spreadsheet-open-tests
        COMMAND snow-shot-pinned-window-tests --image-editing-only --spreadsheet-open-only
            -platform offscreen)
    set_tests_properties(snow-shot-image-editing-spreadsheet-open-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    foreach(_snow_editor_regression IN ITEMS
        close source smart-erase save canvas document translation branch history conversion-availability
        automatic-selection)
        set(_snow_editor_regression_test
            "snow-shot-image-editing-${_snow_editor_regression}-tests")
        add_test(NAME ${_snow_editor_regression_test}
            COMMAND snow-shot-pinned-window-tests --image-editing-only
                "--image-editing-${_snow_editor_regression}-only" -platform offscreen)
        set_tests_properties(${_snow_editor_regression_test} PROPERTIES
            LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    endforeach()
    add_test(NAME snow-shot-image-editing-lifetime-tests
        COMMAND snow-shot-pinned-window-tests --image-editing-only
            --image-editing-lifetime-only -platform offscreen)
    set_tests_properties(snow-shot-image-editing-lifetime-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-image-editing-recognition-feedback-tests
        COMMAND snow-shot-pinned-window-tests --image-editing-only
            --image-editing-recognition-feedback-only -platform offscreen)
    set_tests_properties(snow-shot-image-editing-recognition-feedback-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-image-editing-entry-points-tests
        COMMAND snow-shot-pinned-window-tests --image-editing-entry-points-only -platform offscreen)
    set_tests_properties(snow-shot-image-editing-entry-points-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    set_tests_properties(snow-shot-image-editing-tests PROPERTIES LABELS "unit" RUN_SERIAL TRUE TIMEOUT 120
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    foreach(_snow_editor_toolbar_dpr IN ITEMS 1 1.25 2)
        set(_snow_editor_toolbar_test
            "snow-shot-image-editing-toolbar-placement-${_snow_editor_toolbar_dpr}-tests")
        add_test(NAME ${_snow_editor_toolbar_test}
            COMMAND snow-shot-pinned-window-tests --image-editing-only --image-editing-toolbar-only
                -platform offscreen)
        set_tests_properties(${_snow_editor_toolbar_test} PROPERTIES
            LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "QT_SCALE_FACTOR=${_snow_editor_toolbar_dpr};${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    endforeach()
    if(WIN32)
        add_test(NAME snow-shot-image-editing-toolbar-native-tests
            COMMAND snow-shot-pinned-window-tests --image-editing-only
                --image-editing-toolbar-native-only -platform windows)
        set_tests_properties(snow-shot-image-editing-toolbar-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=windows")
    elseif(APPLE)
        target_sources(snow-shot-pinned-window-tests PRIVATE
            tests/macos_image_editing_toolbar_tests.mm)
        add_test(NAME snow-shot-image-editing-toolbar-stacking-tests
            COMMAND snow-shot-pinned-window-tests --image-editing-toolbar-stacking-only
                -platform offscreen)
        set_tests_properties(snow-shot-image-editing-toolbar-stacking-tests PROPERTIES
            LABELS "unit;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
        add_test(NAME snow-shot-image-editing-toolbar-stacking-native-tests
            COMMAND snow-shot-pinned-window-tests --image-editing-toolbar-stacking-only
                -platform cocoa)
        set_tests_properties(snow-shot-image-editing-toolbar-stacking-native-tests PROPERTIES
            LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
    endif()
    add_test(NAME snow-shot-pinned-wheel-step-tests
        COMMAND snow-shot-pinned-window-tests --wheel-step-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-wheel-step-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    if(WIN32)
        add_test(NAME snow-shot-pinned-native-capture-release-tests
            COMMAND snow-shot-pinned-window-tests --native-capture-release-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-native-capture-release-tests PROPERTIES
            LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
        add_test(NAME snow-shot-pinned-click-wheel-native-tests
            COMMAND snow-shot-pinned-window-tests --click-wheel-native-only -platform windows)
        set_tests_properties(snow-shot-pinned-click-wheel-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    add_test(NAME snow-shot-pinned-angle-wheel-tests
        COMMAND snow-shot-pinned-window-tests --angle-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-angle-wheel-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-pinned-restored-text-theme-tests
        COMMAND snow-shot-pinned-window-tests --restored-text-theme-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-restored-text-theme-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-file-pin-presentation-tests
        COMMAND snow-shot-pinned-window-tests --file-pin-batch-only)
    set_tests_properties(snow-shot-file-pin-presentation-tests PROPERTIES
        LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-management-lifecycle-tests
        COMMAND snow-shot-pinned-window-tests --management-only -platform offscreen)
    add_test(NAME snow-shot-pinned-batch-restore-tests
        COMMAND snow-shot-pinned-window-tests --batch-restore-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-batch-restore-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 30)
    add_test(NAME snow-shot-pinned-close-persistence-tests
        COMMAND snow-shot-pinned-window-tests --deferred-user-close-only -platform offscreen)
    add_test(NAME snow-shot-pinned-hidden-window-lifecycle-tests
        COMMAND snow-shot-pinned-window-tests --hidden-window-lifecycle-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-hidden-window-lifecycle-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-mcp-pinned-workflow-lifetime-tests
        COMMAND snow-shot-pinned-window-tests --mcp-workflow-lifetime-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-pinned-workflow-lifetime-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-pinned-window-confirmation-tests
        COMMAND snow-shot-pinned-window-tests --window-confirmation-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-window-confirmation-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-drawing-close-tests
        COMMAND snow-shot-pinned-window-tests --drawing-close-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-drawing-close-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 30)
    set_tests_properties(snow-shot-pinned-close-persistence-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-transaction-lifetime-tests
        COMMAND snow-shot-pinned-window-tests --pin-lifetime-only -platform offscreen)
    add_test(NAME snow-shot-pinned-pixel-lifetime-tests
        COMMAND snow-shot-pinned-window-tests --pixel-lifetime-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-pixel-lifetime-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-pinned-transaction-lifetime-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-duplicate-pin-tests
        COMMAND snow-shot-pinned-window-tests --duplicate-pin-only -platform offscreen)
    set_tests_properties(snow-shot-duplicate-pin-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    set_tests_properties(snow-shot-pinned-management-lifecycle-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-batch-actions-tests
        COMMAND snow-shot-pinned-window-tests --batch-actions-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-batch-actions-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 90)
    add_test(NAME snow-shot-pinned-hide-to-top-tests
        COMMAND snow-shot-pinned-window-tests --hide-to-top-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-hide-to-top-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-hide-to-top-native-tests
        COMMAND snow-shot-pinned-window-tests --hide-to-top-native-only -platform windows)
    set_tests_properties(snow-shot-pinned-hide-to-top-native-tests PROPERTIES LABELS "unit;windows"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-lock-tests
        COMMAND snow-shot-pinned-window-tests --lock-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-lock-tests PROPERTIES LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    if(WIN32)
        add_test(NAME snow-shot-pinned-lock-native-tests
            COMMAND snow-shot-pinned-window-tests --lock-native-only -platform windows)
        set_tests_properties(snow-shot-pinned-lock-native-tests PROPERTIES
            LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    add_test(NAME snow-shot-pinned-click-through-tests
        COMMAND snow-shot-pinned-window-tests --click-through-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-click-through-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-always-on-top-tests
        COMMAND snow-shot-pinned-window-tests --always-on-top-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-always-on-top-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-show-border-tests
        COMMAND snow-shot-pinned-window-tests --show-border-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-show-border-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-transparent-edges-tests
        COMMAND snow-shot-pinned-window-tests --transparent-edges-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-transparent-edges-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-checkerboard-tests
        COMMAND snow-shot-pinned-window-tests --checkerboard-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-checkerboard-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-selection-annotations-tests
        COMMAND snow-shot-pinned-window-tests --selection-annotations-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-selection-annotations-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-eraser-tests
        COMMAND snow-shot-pinned-window-tests --eraser-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-eraser-tests PROPERTIES LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-remembered-drawing-tool-tests
        COMMAND snow-shot-pinned-window-tests --remembered-drawing-tool-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-remembered-drawing-tool-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-click-through-native-tests
        COMMAND snow-shot-pinned-window-tests --click-through-native-only -platform windows)
    set_tests_properties(snow-shot-pinned-click-through-native-tests PROPERTIES
        LABELS "interactive;e2e;windows" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-image-processing-shortcut-tests
        COMMAND snow-shot-pinned-window-tests --image-processing-shortcuts-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-image-processing-shortcut-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-pinned-shortcut-display-tests
        COMMAND snow-shot-pinned-window-tests --shortcut-display-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-shortcut-display-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-pinned-menu-language-tests
        COMMAND snow-shot-pinned-window-tests --menu-language-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-menu-language-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-text-type-tests
        COMMAND snow-shot-pinned-window-tests --text-type-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-text-type-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-destroy-shortcut-tests
        COMMAND snow-shot-pinned-window-tests --pinned-destroy-shortcut-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-destroy-shortcut-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-pinned-resize-window-tool-tests
        COMMAND snow-shot-pinned-window-tests --resize-window-tool-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-resize-window-tool-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 90)
    if(WIN32)
        add_test(NAME snow-shot-pinned-resize-window-native-tests
            COMMAND snow-shot-pinned-window-tests --resize-window-native-only -platform windows)
        set_tests_properties(snow-shot-pinned-resize-window-native-tests PROPERTIES
            LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 120)
    endif()
    foreach(_thumbnail_scenario IN ITEMS appearance dpi snapshot copy)
        add_test(NAME snow-shot-pinned-thumbnail-${_thumbnail_scenario}-tests
            COMMAND snow-shot-pinned-window-tests --thumbnail-${_thumbnail_scenario}-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-thumbnail-${_thumbnail_scenario}-tests PROPERTIES
            TIMEOUT 30 RUN_SERIAL TRUE
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    set_tests_properties(snow-shot-pinned-thumbnail-copy-tests PROPERTIES
        ENVIRONMENT "QT_SCALE_FACTOR=2")
    add_test(NAME snow-shot-pinned-thumbnail-copy-fractional-tests
        COMMAND snow-shot-pinned-window-tests --thumbnail-copy-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-thumbnail-copy-fractional-tests PROPERTIES
        TIMEOUT 30 RUN_SERIAL TRUE ENVIRONMENT "QT_SCALE_FACTOR=1.25"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-thumbnail-background-tests
        COMMAND snow-shot-pinned-window-tests --thumbnail-background-only)
    add_test(NAME snow-shot-pinned-thumbnail-recreation-tests
        COMMAND snow-shot-pinned-window-tests --thumbnail-recreation-only)
    add_test(NAME snow-shot-pinned-thumbnail-reentry-tests
        COMMAND snow-shot-pinned-window-tests --thumbnail-reentry-only -platform offscreen)
    add_test(NAME snow-shot-pinned-middle-click-tests
        COMMAND snow-shot-pinned-window-tests --middle-click-only)
    add_test(NAME snow-shot-pinned-middle-click-offscreen-tests
        COMMAND snow-shot-pinned-window-tests --middle-click-only)
    set_tests_properties(snow-shot-pinned-middle-click-offscreen-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-pinned-double-click-tests
        COMMAND snow-shot-pinned-window-tests --double-click-only)
    add_test(NAME snow-shot-pinned-double-click-offscreen-tests
        COMMAND snow-shot-pinned-window-tests --double-click-only)
    set_tests_properties(snow-shot-pinned-double-click-offscreen-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-pinned-text-double-click-tests
        COMMAND snow-shot-pinned-window-tests --text-double-click-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-text-double-click-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-text-wrap-tests
        COMMAND snow-shot-pinned-window-tests --text-wrap-only -platform offscreen)
    add_test(NAME snow-shot-pinned-text-wrap-fractional-tests
        COMMAND snow-shot-pinned-window-tests --text-wrap-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-text-wrap-tests snow-shot-pinned-text-wrap-fractional-tests
        PROPERTIES LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-pinned-text-wrap-fractional-tests PROPERTIES
        ENVIRONMENT "QT_SCALE_FACTOR=1.5;SNOW_PIN_TEST_DPR=1.5")
    add_test(NAME snow-shot-pinned-save-dialog-tests
        COMMAND snow-shot-pinned-window-tests --save-dialog-only)
    set_tests_properties(snow-shot-pinned-save-dialog-tests PROPERTIES TIMEOUT 90)
    add_test(NAME snow-shot-pinned-style-wheel-tests
        COMMAND snow-shot-pinned-window-tests --arrow-label-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-style-wheel-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(
        NAME snow-shot-pinned-toolbar-parity-tests
        COMMAND snow-shot-pinned-window-tests --toolbar-parity-only -platform offscreen
    )
    add_test(NAME snow-shot-pinned-translation-tests
        COMMAND snow-shot-pinned-window-tests --translation-only)
    add_test(
        NAME snow-shot-pinned-movement-shortcut-tests
        COMMAND snow-shot-pinned-window-tests --movement-shortcut-only -platform offscreen
    )
    add_test(
        NAME snow-shot-pinned-drawing-shortcut-toggle-tests
        COMMAND snow-shot-pinned-window-tests --drawing-shortcut-toggle-only
    )
    add_test(NAME snow-shot-pinned-history-shortcut-tests
        COMMAND snow-shot-pinned-window-tests --history-shortcuts-only -platform offscreen)
    add_test(NAME snow-shot-pinned-recognition-history-tests
        COMMAND snow-shot-pinned-window-tests --recognition-history-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-recognition-history-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    if(WIN32)
        add_test(
            NAME snow-shot-pinned-native-drag-shortcut-tests
            COMMAND snow-shot-pinned-window-tests --native-drag-shortcut-only
        )
        add_test(
            NAME snow-shot-pinned-system-move-shortcut-real-mouse-tests
            COMMAND snow-shot-pinned-window-tests --system-move-loop-only
        )
        set_tests_properties(snow-shot-pinned-system-move-shortcut-real-mouse-tests PROPERTIES
            LABELS "interactive;windows"
        )
        add_test(
            NAME snow-shot-pinned-native-drag-dpi-tests
            COMMAND snow-shot-pinned-window-tests --native-drag-dpi-only
        )
    endif()

    add_executable(snow-shot-pdf-export-tests
        tests/screenshot_pdf_export_tests.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/presentation/services/screenshotimagefileservice.cpp
    )
    target_include_directories(snow-shot-pdf-export-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    target_link_libraries(snow-shot-pdf-export-tests PRIVATE snow_shot_storage snow_shot_image_codec Qt6::Gui)
    add_test(NAME snow-shot-pdf-export-tests COMMAND snow-shot-pdf-export-tests)
    set_tests_properties(snow-shot-pdf-export-tests PROPERTIES
        LABELS "unit" ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    if(APPLE OR WIN32)
        add_executable(snow-shot-window-names-native-tests tests/screenshot_window_names_tests.cpp)
        target_link_libraries(snow-shot-window-names-native-tests PRIVATE snow_shot_export_naming Qt6::Widgets)
        if(APPLE)
            set_source_files_properties(tests/screenshot_window_names_tests.cpp PROPERTIES LANGUAGE OBJCXX)
            target_link_libraries(snow-shot-window-names-native-tests PRIVATE "-framework AppKit")
        endif()
        add_test(NAME snow-shot-window-names-native-tests COMMAND snow-shot-window-names-native-tests)
        set_tests_properties(snow-shot-window-names-native-tests PROPERTIES
            LABELS "interactive;e2e" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 15)
    endif()

    add_executable(snow-shot-save-directory-time-variables-tests
        tests/save_directory_time_variables_tests.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/presentation/recording/screenrecordingfolder.cpp)
    target_include_directories(snow-shot-save-directory-time-variables-tests PRIVATE
        include src/image)
    target_link_libraries(snow-shot-save-directory-time-variables-tests PRIVATE
        snow_shot_storage snow_shot_image_codec snow_shot_file_manager Qt6::Gui)
    add_test(NAME snow-shot-save-directory-time-variables-tests
        COMMAND snow-shot-save-directory-time-variables-tests -platform offscreen)
    snow_shot_import_offscreen_platform(snow-shot-save-directory-time-variables-tests)
    set_tests_properties(snow-shot-save-directory-time-variables-tests PROPERTIES
        LABELS unit RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-screenshot-image-file-service-tests
        tests/screenshot_image_file_service_tests.cpp
        include/snow_shot/presentation/screenshotimagefileservice.h
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotpdfexport.cpp
    )
    target_include_directories(snow-shot-screenshot-image-file-service-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
    )
    target_link_libraries(snow-shot-screenshot-image-file-service-tests PRIVATE snow_shot_storage
        snow_shot_image_codec
        Qt6::Gui
    )
    add_test(
        NAME snow-shot-screenshot-image-file-service-tests
        COMMAND snow-shot-screenshot-image-file-service-tests
    )
    set_tests_properties(snow-shot-screenshot-image-file-service-tests PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-recognition-file-export-tests
        tests/screenshot_recognition_file_export_tests.cpp
        src/presentation/services/screenshotrecognitionfileexport.cpp
        src/storage/exportfilename.cpp
        src/presentation/ocr/screenshottabledocument.cpp
    )
    target_include_directories(snow-shot-recognition-file-export-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-recognition-file-export-tests PRIVATE Qt6::Widgets MINIZIP::minizip-ng)
    snow_shot_add_translations(snow-shot-recognition-file-export-tests)
    snow_shot_import_offscreen_platform(snow-shot-recognition-file-export-tests)
    add_test(NAME snow-shot-recognition-file-export-tests
             COMMAND snow-shot-recognition-file-export-tests)
    set_tests_properties(snow-shot-recognition-file-export-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-update-confirmation-tests
        tests/update_confirmation_dialog_tests.cpp
        src/app/updateconfirmationdialog.cpp
    )
    target_include_directories(snow-shot-update-confirmation-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-update-confirmation-tests PRIVATE Qt6::Widgets)
    snow_shot_add_translations(snow-shot-update-confirmation-tests)
    snow_shot_import_offscreen_platform(snow-shot-update-confirmation-tests)
    add_test(NAME snow-shot-update-confirmation-tests
             COMMAND snow-shot-update-confirmation-tests)
    set_tests_properties(snow-shot-update-confirmation-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-file-pin-settings-tests tests/file_pin_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-file-pin-settings-tests PRIVATE snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-file-pin-settings-tests)
    add_test(NAME snow-shot-file-pin-settings-tests COMMAND snow-shot-file-pin-settings-tests)
    set_tests_properties(snow-shot-file-pin-settings-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-custom-server-settings-tests
        tests/custom_server_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-custom-server-settings-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-custom-server-settings-tests)
    add_test(NAME snow-shot-custom-server-settings-tests COMMAND snow-shot-custom-server-settings-tests)
    set_tests_properties(snow-shot-custom-server-settings-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-selection-snap-shortcut-settings-tests
        tests/selection_snap_shortcut_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-selection-snap-shortcut-settings-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-selection-snap-shortcut-settings-tests)
    add_test(NAME snow-shot-selection-snap-shortcut-settings-tests
             COMMAND snow-shot-selection-snap-shortcut-settings-tests)
    set_tests_properties(snow-shot-selection-snap-shortcut-settings-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-selection-resize-mode-settings-tests
        tests/selection_resize_mode_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-selection-resize-mode-settings-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-selection-resize-mode-settings-tests)
    add_test(NAME snow-shot-selection-resize-mode-settings-tests
             COMMAND snow-shot-selection-resize-mode-settings-tests)
    set_tests_properties(snow-shot-selection-resize-mode-settings-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-selection-transition-animation-settings-tests
        tests/selection_transition_animation_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-selection-transition-animation-settings-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-selection-transition-animation-settings-tests)
    add_test(NAME snow-shot-selection-transition-animation-settings-tests
        COMMAND snow-shot-selection-transition-animation-settings-tests)
    set_tests_properties(snow-shot-selection-transition-animation-settings-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-remember-last-used-tool-settings-tests
        tests/remember_last_used_tool_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-remember-last-used-tool-settings-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-remember-last-used-tool-settings-tests)
    add_test(NAME snow-shot-remember-last-used-tool-settings-tests
             COMMAND snow-shot-remember-last-used-tool-settings-tests)
    set_tests_properties(snow-shot-remember-last-used-tool-settings-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-screen-recording-interaction-settings-tests
        tests/screen_recording_interaction_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-screen-recording-interaction-settings-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-screen-recording-interaction-settings-tests)
    add_test(NAME snow-shot-screen-recording-interaction-settings-tests
        COMMAND snow-shot-screen-recording-interaction-settings-tests)
    set_tests_properties(snow-shot-screen-recording-interaction-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-edit-selection-toolbar-settings-tests
        tests/edit_selection_toolbar_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-edit-selection-toolbar-settings-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-edit-selection-toolbar-settings-tests)
    add_test(NAME snow-shot-edit-selection-toolbar-settings-tests
             COMMAND snow-shot-edit-selection-toolbar-settings-tests)
    set_tests_properties(snow-shot-edit-selection-toolbar-settings-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-file-pin-batch-tests
        tests/screenshot_file_pin_batch_tests.cpp
        src/presentation/services/pinnedtextstylecapture.cpp
        ${SNOW_SELECTED_FILES_SOURCES}
        src/presentation/services/screenshotfilepinbatch.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
        include/snow_shot/presentation/screenshotexportcoordinator.h
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/pinnedcolorswatch.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
    )
    target_include_directories(snow-shot-file-pin-batch-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-file-pin-batch-tests PRIVATE snow_shot_storage snow_shot_image_codec ant_design_qt Qt6::Widgets
        ${SNOW_SELECTED_FILES_LIBRARIES})
    if(APPLE)
        target_link_libraries(snow-shot-file-pin-batch-tests PRIVATE snow_shot_rust_ffi_selector)
    endif()
    if(WIN32)
        target_link_libraries(snow-shot-file-pin-batch-tests PRIVATE ole32 oleaut32 shell32 uuid)
        add_executable(snow-shot-selected-files-native-smoke-tests
            tests/selected_files_native_smoke_tests.cpp ${SNOW_SELECTED_FILES_SOURCES})
        target_include_directories(snow-shot-selected-files-native-smoke-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-selected-files-native-smoke-tests PRIVATE
            Qt6::Core ole32 oleaut32 shell32 uuid user32)
        add_test(NAME snow-shot-selected-files-native-smoke-tests
            COMMAND snow-shot-selected-files-native-smoke-tests)
        set_tests_properties(snow-shot-selected-files-native-smoke-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60)
        add_test(NAME snow-shot-selected-files-basic-native-smoke-tests
            COMMAND snow-shot-selected-files-native-smoke-tests --basic-only)
        set_tests_properties(snow-shot-selected-files-basic-native-smoke-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    if(APPLE)
        add_executable(snow-shot-macos-selected-files-tests
            tests/macos_selected_files_tests.mm ${SNOW_SELECTED_FILES_SOURCES})
        target_include_directories(snow-shot-macos-selected-files-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-macos-selected-files-tests PRIVATE
            Qt6::Core ${SNOW_SELECTED_FILES_LIBRARIES})
        add_test(NAME snow-shot-macos-selected-files-tests COMMAND snow-shot-macos-selected-files-tests)
        add_executable(snow-shot-macos-selected-files-native-smoke MACOSX_BUNDLE
            tests/macos_selected_files_native_smoke.mm ${SNOW_SELECTED_FILES_SOURCES})
        target_include_directories(snow-shot-macos-selected-files-native-smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-macos-selected-files-native-smoke PRIVATE
            Qt6::Gui ${SNOW_SELECTED_FILES_LIBRARIES})
        set_target_properties(snow-shot-macos-selected-files-native-smoke PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/tests/macos_selected_files_smoke.plist.in")
        add_test(NAME snow-shot-macos-selected-files-native-smoke
            COMMAND snow-shot-macos-selected-files-native-smoke)
        set_tests_properties(snow-shot-macos-selected-files-native-smoke PROPERTIES
            LABELS "interactive" RUN_SERIAL TRUE TIMEOUT 120 SKIP_RETURN_CODE 77)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-file-pin-batch-tests)
    add_test(NAME snow-shot-file-pin-batch-tests COMMAND snow-shot-file-pin-batch-tests)
    set_tests_properties(snow-shot-file-pin-batch-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../test-support/clipboard-dib"
        "${CMAKE_CURRENT_BINARY_DIR}/clipboard-dib-tests")
    add_executable(snow-shot-clipboard-content-tests
        tests/screenshot_clipboard_content_tests.cpp
        include/snow_shot/presentation/screenshotclipboardcontent.h
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/pinnedcolorswatch.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
    )
    target_include_directories(snow-shot-clipboard-content-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
    )
    target_link_libraries(snow-shot-clipboard-content-tests PRIVATE
        snow_shot_image_codec
        Qt6::Widgets
    )
    add_test(NAME snow-shot-clipboard-content-tests
             COMMAND snow-shot-clipboard-content-tests)
    snow_shot_import_offscreen_platform(snow-shot-clipboard-content-tests)
    add_test(NAME snow-shot-clipboard-mime-data-tests
             COMMAND snow-shot-clipboard-content-tests --mime-data-only)
    add_test(NAME snow-shot-clipboard-text-layout-tests
             COMMAND snow-shot-clipboard-content-tests --text-layout-only)
    set_tests_properties(snow-shot-clipboard-text-layout-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-clipboard-import-routing-tests
             COMMAND snow-shot-clipboard-content-tests --import-routing-only)
    set_tests_properties(snow-shot-clipboard-import-routing-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-clipboard-embedded-images-tests
             COMMAND snow-shot-clipboard-content-tests --embedded-images-only)
    set_tests_properties(snow-shot-clipboard-embedded-images-tests PROPERTIES
        LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-clipboard-mime-data-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-clipboard-content-tests PROPERTIES
        RUN_SERIAL TRUE
        # The live snapshot exercises an in-process mime provider that hides
        # the Qt image format; only the offscreen clipboard keeps that
        # in-process semantics, the native OS clipboard drops the payload.
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(WIN32)
        add_executable(snow-shot-clipboard-roundtrip-tests
            tests/screenshot_clipboard_roundtrip_tests.cpp
            src/presentation/services/screenshotclipboardservice.cpp
            src/presentation/services/screenshotclipboardcontent.cpp
            src/presentation/services/pinnedcolorswatch.cpp
            src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
        )
        target_include_directories(snow-shot-clipboard-roundtrip-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
        )
        target_link_libraries(snow-shot-clipboard-roundtrip-tests PRIVATE
            snow_shot_image_codec snow_shot_diagnostics Qt6::Widgets user32
        )
        add_test(NAME snow-shot-clipboard-roundtrip-tests
                 COMMAND snow-shot-clipboard-roundtrip-tests)
        # This exercises the real Windows clipboard without showing any windows.
        set_tests_properties(snow-shot-clipboard-roundtrip-tests PROPERTIES
            LABELS "unit;windows"
            RUN_SERIAL TRUE
            TIMEOUT 30
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()
    add_test(
        NAME snow-shot-pinned-async-presentation-tests
        COMMAND snow-shot-pinned-window-tests --async-presentation-only
    )
    add_test(NAME snow-shot-pinned-content-replacement-tests
        COMMAND snow-shot-pinned-window-tests --content-replacement-only -platform offscreen)
    add_test(NAME snow-shot-pinned-file-drop-tests
        COMMAND snow-shot-pinned-window-tests --file-drop-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-file-drop-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" TIMEOUT 90)
    set_tests_properties(snow-shot-pinned-content-replacement-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" TIMEOUT 90 RUN_SERIAL TRUE)
    add_test(
        NAME snow-shot-pinned-window-pool-tests
        COMMAND snow-shot-pinned-window-tests --pooling-only
    )
    add_test(
        NAME snow-shot-pinned-group-menu-tests
        COMMAND snow-shot-pinned-window-tests --group-menu-only
    )
    add_test(
        NAME snow-shot-pinned-cached-ocr-tests
        COMMAND snow-shot-pinned-window-tests --cached-ocr-only
    )
    add_test(
        NAME snow-shot-pinned-restored-ocr-tests
        COMMAND snow-shot-pinned-window-tests --restored-ocr-only
    )
    add_test(NAME snow-shot-pinned-initial-ocr-tests
        COMMAND snow-shot-pinned-window-tests --initial-ocr-only)
    add_test(NAME snow-shot-pinned-early-ocr-snapshot-tests
        COMMAND snow-shot-pinned-window-tests --early-ocr-snapshot-only)
    add_test(NAME snow-shot-pinned-invalid-ocr-restore-tests
        COMMAND snow-shot-pinned-window-tests --invalid-ocr-restore-only)
    add_test(NAME snow-shot-pinned-cached-ocr-provider-tests
        COMMAND snow-shot-pinned-window-tests --cached-ocr-provider-only)
    add_test(NAME snow-shot-pinned-cloud-upload-tests COMMAND snow-shot-pinned-window-tests --cloud-upload-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-cloud-upload-tests PROPERTIES LABELS "unit" TIMEOUT 45
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-toolbar-layout-tests
        COMMAND snow-shot-pinned-window-tests --pinned-toolbar-layout-only)
    set_tests_properties(snow-shot-pinned-toolbar-layout-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-pinned-recognition-shortcut-tests
        COMMAND snow-shot-pinned-window-tests --recognition-shortcut-only)
    # QR shortcut/payload semantics use the isolated Qt clipboard. Native clipboard
    # publication and recognition surfaces have their own dedicated registrations.
    add_test(NAME snow-shot-pinned-qr-copy-tests
        COMMAND snow-shot-pinned-window-tests --qr-copy-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-qr-copy-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-recognition-shortcut-offscreen-tests
        COMMAND snow-shot-pinned-window-tests --recognition-shortcut-only -platform offscreen)
    add_test(NAME snow-shot-pinned-mouse-recognition-tests
        COMMAND snow-shot-pinned-window-tests --mouse-recognition-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-mouse-recognition-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-recognition-context-menu-tests
        COMMAND snow-shot-pinned-window-tests --recognition-context-menu-only -platform offscreen)
    add_test(NAME snow-shot-pinned-recognition-selection-copy-tests
        COMMAND snow-shot-pinned-window-tests --recognition-selection-copy-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-recognition-selection-copy-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-ocr-box-selection-tests
        COMMAND snow-shot-pinned-window-tests --ocr-box-selection-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-ocr-box-selection-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        add_test(NAME snow-shot-pinned-ocr-box-selection-native-routing-tests
            COMMAND snow-shot-pinned-window-tests --ocr-box-selection-native-only -platform windows)
        set_tests_properties(snow-shot-pinned-ocr-box-selection-native-routing-tests PROPERTIES
            LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    set_tests_properties(snow-shot-pinned-recognition-context-menu-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-pinned-transformed-ocr-tests
        COMMAND snow-shot-pinned-window-tests --transformed-ocr-only)
    add_test(NAME snow-shot-pinned-transform-geometry-tests
        COMMAND snow-shot-pinned-window-tests --transform-geometry-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-transform-geometry-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-pinned-selection-restore-tests
        COMMAND snow-shot-pinned-window-tests --selection-restore-only -platform offscreen)
    add_test(NAME snow-shot-pinned-restored-state-persistence-tests
        COMMAND snow-shot-pinned-window-tests --restored-state-persistence-only -platform offscreen)
    foreach(scale IN ITEMS 1 1.5 2)
        add_test(NAME snow-shot-pinned-partial-placement-restore-${scale}-tests
            COMMAND snow-shot-pinned-window-tests --partial-placement-restore-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-partial-placement-restore-${scale}-tests PROPERTIES
            LABELS "unit" TIMEOUT 60 RUN_SERIAL TRUE SNOW_TEST_CROSS_PLATFORM TRUE
            ENVIRONMENT "QT_SCALE_FACTOR=${scale};${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    endforeach()
    set_tests_properties(snow-shot-pinned-restored-state-persistence-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    if(WIN32)
        add_test(NAME snow-shot-pinned-resize-style-tests
            COMMAND snow-shot-pinned-window-tests --resize-style-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-resize-style-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 60)
    endif()
    add_test(
        NAME snow-shot-pinned-restore-wiring-tests
        COMMAND snow-shot-pinned-window-tests --restore-wiring-only
    )
    if(WIN32)
        add_test(NAME snow-shot-pinned-escape-activation-tests
            COMMAND snow-shot-pinned-window-tests --escape-activation-only)
        set_tests_properties(snow-shot-pinned-escape-activation-tests PROPERTIES
            LABELS "interactive;windows"
            RUN_SERIAL TRUE
            TIMEOUT 90
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-pinned-escape-burst-tests
        COMMAND snow-shot-pinned-window-tests --escape-burst-only)
    add_test(NAME snow-shot-pinned-tool-rotation-tests
        COMMAND snow-shot-pinned-window-tests --tool-rotation-only)
    add_test(NAME snow-shot-pinned-scale-readout-tests
        COMMAND snow-shot-pinned-window-tests --scale-readout-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-scale-readout-tests PROPERTIES
        LABELS "unit" TIMEOUT 90
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-passive-geometry-tests
        COMMAND snow-shot-pinned-window-tests --passive-geometry-only)
    add_test(NAME snow-shot-pinned-pointer-presence-offscreen-tests
        COMMAND snow-shot-pinned-window-tests --pointer-presence-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-pointer-presence-offscreen-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-pinned-passive-geometry-tests PROPERTIES
        LABELS "unit"
        RUN_SERIAL TRUE
        TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        add_test(NAME snow-shot-pinned-passive-native-geometry-tests
            COMMAND snow-shot-pinned-window-tests --passive-geometry-only)
        set_tests_properties(snow-shot-pinned-passive-native-geometry-tests PROPERTIES
            LABELS "unit;windows"
            RUN_SERIAL TRUE
            TIMEOUT 30
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        add_test(NAME snow-shot-pinned-pointer-presence-tests
            COMMAND snow-shot-pinned-window-tests --pointer-presence-only)
        set_tests_properties(snow-shot-pinned-pointer-presence-tests PROPERTIES
            LABELS "unit;windows"
            RUN_SERIAL TRUE
            TIMEOUT 30
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    set(_snow_shot_pinned_window_test_names
        snow-shot-pinned-escape-burst-tests
        snow-shot-pinned-tool-rotation-tests
        snow-shot-pinned-middle-click-tests
        snow-shot-pinned-middle-click-offscreen-tests
        snow-shot-pinned-double-click-tests
        snow-shot-pinned-double-click-offscreen-tests
        snow-shot-pinned-window-tests
        snow-shot-pinned-translation-tests
        snow-shot-pinned-async-presentation-tests
        snow-shot-pinned-window-pool-tests
        snow-shot-pinned-group-menu-tests
        snow-shot-pinned-cached-ocr-tests
        snow-shot-pinned-restored-ocr-tests
        snow-shot-pinned-initial-ocr-tests
        snow-shot-pinned-early-ocr-snapshot-tests
        snow-shot-pinned-invalid-ocr-restore-tests
        snow-shot-pinned-cached-ocr-provider-tests
        snow-shot-pinned-recognition-shortcut-tests
        snow-shot-pinned-recognition-shortcut-offscreen-tests
        snow-shot-pinned-transformed-ocr-tests
        snow-shot-pinned-restore-wiring-tests
        snow-shot-pinned-selection-restore-tests
        snow-shot-pinned-thumbnail-recreation-tests
        snow-shot-pinned-thumbnail-reentry-tests
        snow-shot-pinned-thumbnail-background-tests
        snow-shot-pinned-toolbar-parity-tests
        snow-shot-pinned-movement-shortcut-tests
        snow-shot-pinned-drawing-shortcut-toggle-tests
        snow-shot-pinned-history-shortcut-tests
    )
    if(WIN32)
        list(APPEND _snow_shot_pinned_window_test_names
            snow-shot-pinned-native-drag-shortcut-tests
            snow-shot-pinned-system-move-shortcut-real-mouse-tests
            snow-shot-pinned-native-drag-dpi-tests
        )
    endif()
    set_tests_properties(
        ${_snow_shot_pinned_window_test_names}
        PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 90
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    # The aggregate executes every legacy scenario; the focused aliases above
    # retain their shorter deadline. Do not overwrite the aggregate in that group.
    set_tests_properties(snow-shot-pinned-window-tests PROPERTIES TIMEOUT 180)

    add_executable(snow-shot-clipboard-preparation-tests
        tests/screenshot_clipboard_preparation_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_include_directories(snow-shot-clipboard-preparation-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    target_link_libraries(snow-shot-clipboard-preparation-tests PRIVATE
        snow_shot_image_codec snow_shot_diagnostics Qt6::Gui)
    if(WIN32)
        target_link_libraries(snow-shot-clipboard-preparation-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-clipboard-preparation-tests
        COMMAND snow-shot-clipboard-preparation-tests)
    set_tests_properties(snow-shot-clipboard-preparation-tests PROPERTIES
        LABELS unit TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-export-coordinator-tests
        tests/screenshot_export_coordinator_tests.cpp
        include/snow_shot/presentation/screenshotclipboardservice.h
        include/snow_shot/presentation/screenshotexportcoordinator.h
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
    )
    target_include_directories(snow-shot-export-coordinator-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-export-coordinator-tests PRIVATE snow_shot_image_codec snow_shot_diagnostics Qt6::Widgets)
    if(WIN32)
        target_link_libraries(snow-shot-export-coordinator-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-export-coordinator-tests
             COMMAND snow-shot-export-coordinator-tests)
    add_test(NAME snow-shot-export-scheduling-tests
             COMMAND snow-shot-export-coordinator-tests --scheduling-only)
    add_test(NAME snow-shot-export-queue-tests
        COMMAND snow-shot-export-coordinator-tests --queue-only)
    set_tests_properties(snow-shot-export-queue-tests PROPERTIES
        LABELS unit TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-export-scheduling-tests PROPERTIES
        LABELS unit TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-export-coordinator-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-export-dialog-workflow-tests
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        tests/screenshot_export_dialog_workflow_tests.cpp
        tests/screenshot_save_result_tests.cpp
        include/snow_shot/presentation/screenshotsavepreviewcanvas.h
        src/presentation/services/screenshotsaveasfiledialog.cpp
        src/presentation/services/screenshotsavefooter.h
        src/presentation/services/screenshotsavefooter.cpp
        src/presentation/services/screenshotsaveresult.h
        src/presentation/services/screenshotsaveresult.cpp
        src/presentation/services/screenshotsaveexportpipeline.cpp
        src/presentation/services/screenshotsavepreviewcanvas.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotexportartifact.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
        src/presentation/services/screenshotexportpinnedsource.cpp
        src/presentation/ocr/screenshotrecognitionimage.cpp
    )
    target_include_directories(snow-shot-export-dialog-workflow-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/services"
    )
    target_link_libraries(snow-shot-export-dialog-workflow-tests PRIVATE
        snow_shot_image_codec snow_shot_settings snow_draw_engine_qt_canvas Qt6::Widgets
    )
    if(APPLE)
        target_sources(snow-shot-export-dialog-workflow-tests PRIVATE
            ../cmake/test-support/macos_native_input.mm)
        target_include_directories(snow-shot-export-dialog-workflow-tests PRIVATE ../cmake/test-support)
        target_link_libraries(snow-shot-export-dialog-workflow-tests PRIVATE "-framework AppKit")
    endif()
    snow_shot_import_offscreen_platform(snow-shot-export-dialog-workflow-tests)
    foreach(_snow_save_preview_dpr IN ITEMS 1 1.25 1.5 2)
        set(_snow_save_preview_detail_test
            "snow-shot-export-preview-detail-${_snow_save_preview_dpr}-tests")
        add_test(NAME ${_snow_save_preview_detail_test}
            COMMAND snow-shot-export-dialog-workflow-tests --canvas -platform offscreen)
        set_tests_properties(${_snow_save_preview_detail_test} PROPERTIES
            LABELS unit TIMEOUT 30
            ENVIRONMENT "QT_SCALE_FACTOR=${_snow_save_preview_dpr};${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    add_test(NAME snow-shot-pdf-dialog-tests COMMAND snow-shot-export-dialog-workflow-tests --pdf)
    set_tests_properties(snow-shot-pdf-dialog-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-export-dialog-workflow-tests
        COMMAND snow-shot-export-dialog-workflow-tests)
    # Keep a realistic desktop resolution when scaling the offscreen screen to 200%.
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/save-result-screen.json"
        CONTENT "{\"screens\":[{\"name\":\"Save result desktop\",\"width\":1920,\"height\":1080,\"logicalDpi\":96,\"logicalBaseDpi\":96,\"dpr\":1}]}\n")
    foreach(_snow_save_result_dpr IN ITEMS 1 2)
        set(_snow_save_result_test "snow-shot-save-result-${_snow_save_result_dpr}-tests")
        add_test(NAME ${_snow_save_result_test}
            COMMAND snow-shot-export-dialog-workflow-tests --save-result-only
                -platform offscreen:configfile=save-result-screen.json)
        set_tests_properties(${_snow_save_result_test} PROPERTIES
            LABELS unit RUN_SERIAL TRUE TIMEOUT 90
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "QT_SCALE_FACTOR=${_snow_save_result_dpr};${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        if(WIN32)
            set_property(TEST ${_snow_save_result_test} APPEND PROPERTY
                ENVIRONMENT "QT_QPA_FONTDIR=$ENV{SystemRoot}/Fonts")
        endif()
    endforeach()
    add_test(NAME snow-shot-export-dialog-shortcut-tests
        COMMAND snow-shot-export-dialog-workflow-tests --shortcuts)
    add_test(NAME snow-shot-export-dialog-time-variables-tests
        COMMAND snow-shot-export-dialog-workflow-tests --time-variables -platform offscreen)
    set_tests_properties(snow-shot-export-dialog-time-variables-tests PROPERTIES
        LABELS unit RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-collage-export-dialog-tests
        COMMAND snow-shot-export-dialog-workflow-tests --collage-modality -platform offscreen)
    set_tests_properties(snow-shot-collage-export-dialog-tests PROPERTIES
        TIMEOUT 60 LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-export-filename-warning-tests
        COMMAND snow-shot-export-dialog-workflow-tests --filename-warning -platform offscreen)
    set_tests_properties(snow-shot-export-filename-warning-tests PROPERTIES
        TIMEOUT 30 LABELS unit
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-export-preview-wheel-tests
        COMMAND snow-shot-export-dialog-workflow-tests --wheel-zoom -platform offscreen)
    add_test(NAME snow-shot-export-preview-hint-tests
        COMMAND snow-shot-export-dialog-workflow-tests --zoom-hint -platform offscreen)
    set_tests_properties(snow-shot-export-preview-hint-tests PROPERTIES
        TIMEOUT 30 LABELS unit
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-export-preview-wheel-tests PROPERTIES
        TIMEOUT 30 LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    # These tests drive QWidget events synchronously. Cocoa menu tracking has its
    # own event loop and is exercised by snow-shot-action-popup-native-tests.
    set_tests_properties(
        snow-shot-export-dialog-workflow-tests snow-shot-export-dialog-shortcut-tests
        PROPERTIES ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    set_tests_properties(snow-shot-export-dialog-shortcut-tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-save-format-tests
        COMMAND snow-shot-export-dialog-workflow-tests --remember-format)
    set_tests_properties(snow-shot-save-format-tests PROPERTIES
        TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS "unit")
    add_test(NAME snow-shot-export-dialog-pipeline-tests
        COMMAND snow-shot-export-dialog-workflow-tests --optimized-pipeline)
    add_test(NAME snow-shot-export-dialog-placement-tests
        COMMAND snow-shot-export-dialog-workflow-tests --placement)
    set_tests_properties(snow-shot-export-dialog-placement-tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-export-dialog-workflow-tests PROPERTIES
        TIMEOUT 180 RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-export-dialog-pipeline-tests PROPERTIES
        TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-export-artifact-tests
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        tests/screenshot_export_artifact_tests.cpp
        tests/screenshot_export_row_performance_benchmark.cpp
        include/snow_shot/presentation/screenshotclipboardservice.h
        include/snow_shot/presentation/screenshotexportartifact.h
        include/snow_shot/presentation/screenshotexportcoordinator.h
        include/snow_shot/presentation/screenshotresultcompositor.h
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotexportartifact.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
        src/presentation/services/screenshotexportpinnedsource.cpp
        src/presentation/ocr/screenshotrecognitionimage.cpp
    )
    target_include_directories(snow-shot-export-artifact-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
    )
    target_link_libraries(snow-shot-export-artifact-tests PRIVATE
        snow_shot_diagnostics
        snow_shot_image_codec
        snow_shot_storage
        snow_draw_engine_qt_canvas
        adqt::widgets
        Qt6::Widgets
    )
    if(WIN32)
        target_link_libraries(snow-shot-export-artifact-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-export-artifact-tests COMMAND snow-shot-export-artifact-tests)
    add_test(NAME snow-shot-export-compositor-cache-tests
        COMMAND snow-shot-export-artifact-tests --compositor-cache-only -platform offscreen)
    add_test(NAME snow-shot-export-subscriber-lifecycle-tests
        COMMAND snow-shot-export-artifact-tests --subscriber-lifecycle-only -platform offscreen)
    set_tests_properties(snow-shot-export-compositor-cache-tests snow-shot-export-subscriber-lifecycle-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-export-window-variables-tests
        COMMAND snow-shot-export-artifact-tests --window-variables-only)
    set_tests_properties(snow-shot-export-window-variables-tests PROPERTIES LABELS unit RUN_SERIAL TRUE)

    add_test(NAME snow-shot-clipboard-file-export-tests
        COMMAND snow-shot-export-artifact-tests --clipboard-files-only)
    set_tests_properties(snow-shot-clipboard-file-export-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-configured-save-directory-tests
        COMMAND snow-shot-export-artifact-tests --configured-directory-only)
    set_tests_properties(snow-shot-configured-save-directory-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-export-lifetime-tests
        COMMAND snow-shot-export-artifact-tests --pinned-lifetime-only)
    set_tests_properties(snow-shot-pinned-export-lifetime-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    get_target_property(_snow_mcp_test_sources snow-shot-export-artifact-tests SOURCES)
    list(REMOVE_ITEM _snow_mcp_test_sources tests/screenshot_export_artifact_tests.cpp
        tests/screenshot_export_row_performance_benchmark.cpp)
    add_executable(snow-shot-cloud-upload-tests ${_snow_mcp_test_sources} tests/cloud_upload_tests.cpp)
    target_include_directories(snow-shot-cloud-upload-tests PRIVATE include src/image)
    target_link_libraries(snow-shot-cloud-upload-tests PRIVATE snow_shot_cloud_upload snow_shot_settings
        snow_shot_diagnostics snow_shot_image_codec snow_shot_storage snow_draw_engine_qt_canvas ant_design_qt Qt6::Widgets)
    if(WIN32)
        target_link_libraries(snow-shot-cloud-upload-tests PRIVATE user32)
    endif()
    snow_shot_add_translations(snow-shot-cloud-upload-tests)
    snow_shot_import_offscreen_platform(snow-shot-cloud-upload-tests)
    add_test(NAME snow-shot-cloud-upload-tests COMMAND snow-shot-cloud-upload-tests -platform offscreen)
    add_test(NAME snow-shot-cloud-upload-settings-tests
        COMMAND snow-shot-cloud-upload-tests --settings-only -platform offscreen)
    set_tests_properties(snow-shot-cloud-upload-tests snow-shot-cloud-upload-settings-tests
        PROPERTIES LABELS "unit" TIMEOUT 90
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(snow-shot-mcp-tests ${_snow_mcp_test_sources}
        tests/screenshot_mcp_tests.cpp
        include/snow_shot/app/mcp/screenshotmcpserver.h
        include/snow_shot/app/mcp/screenshotmcpsession.h
        src/app/mcp/screenshotmcpserver.cpp
        src/app/mcp/screenshotmcpsession.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/platform/windows/monitorgeometry.cpp)
    target_include_directories(snow-shot-mcp-tests PRIVATE include src/image)
    target_link_libraries(snow-shot-mcp-tests PRIVATE snow_shot_diagnostics
        snow_shot_image_codec snow_shot_storage snow_draw_engine_qt_canvas ant_design_qt Qt6::Widgets Qt6::Network)
    if(WIN32)
        target_link_libraries(snow-shot-mcp-tests PRIVATE user32 advapi32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-mcp-tests)
    add_test(NAME snow-shot-mcp-tests COMMAND snow-shot-mcp-tests -platform offscreen)
    add_test(NAME snow-shot-mcp-selection-style-tests
        COMMAND snow-shot-mcp-tests --selection-style-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-selection-style-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME snow-shot-mcp-replay-lifecycle-tests
        COMMAND snow-shot-mcp-tests --replay-lifecycle-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-replay-lifecycle-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-stroke-rendering-mcp-tests
        COMMAND snow-shot-mcp-tests --stroke-rendering-only -platform offscreen)
    set_tests_properties(snow-shot-stroke-rendering-mcp-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    add_test(NAME snow-shot-speech-bubble-mcp-capabilities-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_mcp_capabilities.py"
            --speech-bubble-only)
    set_tests_properties(snow-shot-speech-bubble-mcp-capabilities-tests PROPERTIES TIMEOUT 30 LABELS unit)
    add_test(NAME snow-shot-border-fill-mcp-tests
        COMMAND snow-shot-mcp-tests --border-fill-only -platform offscreen)
    set_tests_properties(snow-shot-border-fill-mcp-tests PROPERTIES TIMEOUT 30 LABELS unit)
    add_test(NAME snow-shot-magnifier-mcp-style-tests
        COMMAND snow-shot-mcp-tests --magnifier-only -platform offscreen)
    set_tests_properties(snow-shot-magnifier-mcp-style-tests PROPERTIES TIMEOUT 30 LABELS unit)
    if(Python3_Interpreter_FOUND)
        add_test(NAME snow-shot-magnifier-mcp-capabilities-tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_mcp_capabilities.py"
                --magnifier-only)
        set_tests_properties(snow-shot-magnifier-mcp-capabilities-tests PROPERTIES TIMEOUT 30 LABELS unit)
    endif()
    if(SNOW_SHOT_ENABLE_MCP)
        add_test(NAME snow-shot-mcp-stdio-tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_stdio_tests.py"
                "$<TARGET_FILE_DIR:snow_shot>/$<TARGET_FILE_NAME:snow_shot_mcp>")
        set_tests_properties(snow-shot-mcp-stdio-tests PROPERTIES TIMEOUT 45 LABELS unit)
        add_test(NAME snow-shot-mcp-capabilities-tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_mcp_capabilities.py")
        set_tests_properties(snow-shot-mcp-capabilities-tests PROPERTIES TIMEOUT 30 LABELS unit)
        add_test(NAME snow-shot-angle-mcp-capabilities-tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_mcp_capabilities.py"
                --angle-only)
        set_tests_properties(snow-shot-angle-mcp-capabilities-tests PROPERTIES TIMEOUT 30 LABELS unit)
    endif()

    set_tests_properties(snow-shot-mcp-tests PROPERTIES TIMEOUT 90 LABELS unit
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_test(NAME snow-shot-quick-save-command-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --quick-save-only -platform offscreen)
    add_test(NAME snow-shot-quick-save-layout-storage-tests
        COMMAND snow-shot-application-storage-tests --toolbar-layout-only)
    add_test(NAME snow-shot-quick-save-export-tests
        COMMAND snow-shot-export-artifact-tests --quick-save-only)
    add_test(NAME snow-shot-pinned-hidden-text-selection-restore-tests
        COMMAND snow-shot-pinned-window-tests --hidden-text-selection-restore-only)
    add_test(NAME snow-shot-pinned-hidden-text-selection-tests
        COMMAND snow-shot-pinned-window-tests --hidden-text-selection-only -platform offscreen)
    add_test(NAME snow-shot-pinned-recognition-save-tests
        COMMAND snow-shot-pinned-window-tests --recognition-save-only -platform offscreen)
    add_test(NAME snow-shot-pinned-ocr-highlighting-tests
        COMMAND snow-shot-pinned-window-tests --ocr-highlighting-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-ocr-highlighting-tests PROPERTIES LABELS "unit" TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-pinned-recognition-save-tests PROPERTIES LABELS "unit" TIMEOUT 90)
    add_test(NAME snow-shot-pinned-shared-image-export-tests
        COMMAND snow-shot-pinned-window-tests --shared-image-export-only -platform offscreen)
    add_test(NAME snow-shot-pin-execution-autosave-tests
        COMMAND snow-shot-pinned-window-tests --pin-execution-autosave-only -platform offscreen)
    set_tests_properties(snow-shot-pin-execution-autosave-tests PROPERTIES
        LABELS "unit" TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-pinned-shared-image-export-tests PROPERTIES
        LABELS "unit" TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-opacity-export-tests
        COMMAND snow-shot-pinned-window-tests --opacity-export-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-opacity-export-tests PROPERTIES
        LABELS "unit" TIMEOUT 90)
    add_test(NAME snow-shot-pinned-quick-save-tests
        COMMAND snow-shot-pinned-window-tests --quick-save-only -platform $<IF:$<PLATFORM_ID:Darwin>,offscreen,windows>)
    set_tests_properties(snow-shot-pinned-quick-save-tests PROPERTIES LABELS "unit;windows")
    add_test(NAME snow-shot-quick-save-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --quick-save-only -platform offscreen)
    set_tests_properties(snow-shot-quick-save-export-tests snow-shot-pinned-quick-save-tests
        snow-shot-quick-save-toolbar-tests snow-shot-quick-save-command-tests
        snow-shot-quick-save-layout-storage-tests PROPERTIES TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    set_tests_properties(snow-shot-export-artifact-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-angle-annotation-integration-tests
        tests/angle_annotation_integration_tests.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp)
    target_include_directories(snow-shot-angle-annotation-integration-tests PRIVATE include)
    target_link_libraries(snow-shot-angle-annotation-integration-tests PRIVATE
        snow_shot_storage snow_shot_window_shortcuts snow_draw_engine_qt_canvas
        ant_design_qt adqt_icon_core ant_design_icons_qt Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-angle-annotation-integration-tests)
    add_test(NAME snow-shot-angle-annotation-integration-tests
        COMMAND snow-shot-angle-annotation-integration-tests -platform offscreen)
    set_tests_properties(snow-shot-angle-annotation-integration-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-bracket-annotation-integration-tests
        tests/bracket_annotation_integration_tests.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp)
    target_include_directories(snow-shot-bracket-annotation-integration-tests PRIVATE include)
    target_link_libraries(snow-shot-bracket-annotation-integration-tests PRIVATE
        snow_shot_storage snow_shot_window_shortcuts snow_draw_engine_qt_canvas
        ant_design_qt adqt_icon_core ant_design_icons_qt Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-bracket-annotation-integration-tests)
    add_test(NAME snow-shot-bracket-annotation-integration-tests
        COMMAND snow-shot-bracket-annotation-integration-tests -platform offscreen)
    set_tests_properties(snow-shot-bracket-annotation-integration-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-distance-annotation-integration-tests
        tests/distance_annotation_integration_tests.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp)
    target_include_directories(snow-shot-distance-annotation-integration-tests PRIVATE include)
    target_link_libraries(snow-shot-distance-annotation-integration-tests PRIVATE
        snow_shot_storage snow_shot_window_shortcuts snow_draw_engine_qt_canvas
        ant_design_qt adqt_icon_core ant_design_icons_qt Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-distance-annotation-integration-tests)
    add_test(NAME snow-shot-distance-annotation-integration-tests
        COMMAND snow-shot-distance-annotation-integration-tests -platform offscreen)
    set_tests_properties(snow-shot-distance-annotation-integration-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-speech-bubble-integration-tests
        tests/speech_bubble_integration_tests.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp)
    target_include_directories(snow-shot-speech-bubble-integration-tests PRIVATE include)
    target_link_libraries(snow-shot-speech-bubble-integration-tests PRIVATE
        snow_shot_storage snow_shot_window_shortcuts snow_draw_engine_qt_canvas
        ant_design_qt adqt_icon_core ant_design_icons_qt Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-speech-bubble-integration-tests)
    add_test(NAME snow-shot-speech-bubble-integration-tests
        COMMAND snow-shot-speech-bubble-integration-tests -platform offscreen)
    set_tests_properties(snow-shot-speech-bubble-integration-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-screenshot-tool-palette-tests
        tests/screenshot_tool_palette_tests.cpp
        tests/screenshot_style_binding_tests.cpp
        tests/screenshot_selected_filter_style_tests.cpp
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
        src/presentation/services/languagemanager.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
    )
    target_include_directories(snow-shot-screenshot-tool-palette-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_compile_definitions(snow-shot-screenshot-tool-palette-tests PRIVATE
        SNOW_SHOT_TEST_HOOKS=1
    )
    target_link_libraries(snow-shot-screenshot-tool-palette-tests PRIVATE
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        Qt6::Widgets
        snow_draw_engine_qt_canvas
    )
    target_sources(snow-shot-screenshot-tool-palette-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../test-support/fonts/recording_test_fonts.qrc")
    snow_shot_add_translations(snow-shot-screenshot-tool-palette-tests)
    add_test(NAME snow-shot-custom-color-presets-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --custom-color-presets-only -platform offscreen)
    set_tests_properties(snow-shot-custom-color-presets-toolbar-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-ocr-text-options-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --ocr-translation-only -platform offscreen)
    set_tests_properties(snow-shot-ocr-text-options-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-selection-display-unit-toolbar-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --selection-unit-only -platform offscreen)
    set_tests_properties(snow-shot-selection-display-unit-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-move-region-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --move-options-only -platform offscreen)
    set_tests_properties(snow-shot-move-region-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-region-switcher-style-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --region-switcher-only -platform offscreen)
    set_tests_properties(snow-shot-region-switcher-style-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-screenshot-serial-number-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --serial-number-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-serial-number-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(
        NAME snow-shot-screenshot-tool-palette-tests
        COMMAND snow-shot-screenshot-tool-palette-tests
    )
    add_test(NAME snow-shot-border-fill-controls-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --border-fill-only -platform offscreen)
    set_tests_properties(snow-shot-border-fill-controls-tests PROPERTIES TIMEOUT 60 LABELS unit)
    add_test(NAME snow-shot-screenshot-tool-palette-distance-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --distance-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-tool-palette-distance-tests
        PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-screenshot-tool-palette-magnifier-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --magnifier-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-tool-palette-magnifier-tests
        PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-screenshot-tool-palette-bracket-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --bracket-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-tool-palette-bracket-tests
        PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-screenshot-tool-palette-angle-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --angle-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-tool-palette-angle-tests
        PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-scrolling-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --scrolling-only -platform offscreen)
    add_test(NAME snow-shot-arrow-outline-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --arrow-outline-only -platform offscreen)
    set_tests_properties(snow-shot-arrow-outline-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    add_test(NAME snow-shot-text-style-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --text-style-only -platform offscreen)
    set_tests_properties(snow-shot-text-style-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-text-alignment-editing-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --text-alignment-only -platform offscreen)
    set_tests_properties(snow-shot-text-alignment-editing-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    set_tests_properties(snow-shot-scrolling-toolbar-tests PROPERTIES LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-screenshot-tool-palette-remembered-drawing-tool-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --remembered-drawing-tool-only
            -platform offscreen)
    set_tests_properties(snow-shot-screenshot-tool-palette-remembered-drawing-tool-tests
        PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-eraser-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --eraser-only -platform offscreen)
    set_tests_properties(snow-shot-eraser-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-eraser-source-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --eraser-source-only -platform offscreen)
    set_tests_properties(snow-shot-eraser-source-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-style-commit-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --style-commit-only -platform offscreen)
    set_tests_properties(snow-shot-style-commit-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-canvas-tool-bindings-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --canvas-tool-bindings-only
            -platform offscreen)
    set_tests_properties(snow-shot-canvas-tool-bindings-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-watermark-persistence-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --watermark-persistence-only
            -platform offscreen)
    add_test(NAME snow-shot-watermark-template-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --watermark-template-only
            -platform offscreen)
    set_tests_properties(snow-shot-watermark-persistence-tests snow-shot-watermark-template-tests
        PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-spotlight-wheel-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --spotlight-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-spotlight-wheel-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-spotlight-shape-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --spotlight-shape-only -platform offscreen)
    set_tests_properties(snow-shot-spotlight-shape-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-watermark-template-width-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --watermark-template-width-only
            -platform offscreen)
    set_tests_properties(snow-shot-watermark-template-width-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-draw-template-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --draw-template-only -platform offscreen)
    set_tests_properties(snow-shot-draw-template-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-toolbar-popup-recovery-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --popup-recovery-only -platform offscreen)
    set_tests_properties(snow-shot-toolbar-popup-recovery-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-toolbar-popover-lifecycle-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --popover-lifecycle-only
            -platform offscreen)
    set_tests_properties(snow-shot-toolbar-popover-lifecycle-tests PROPERTIES LABELS "unit"
        TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-retired-style-editor-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --retired-style-editors-only -platform offscreen)
    set_tests_properties(snow-shot-retired-style-editor-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-toolbar-popover-hover-switching-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --popover-hover-switching-only
            -platform offscreen)
    set_tests_properties(snow-shot-toolbar-popover-hover-switching-tests PROPERTIES LABELS "unit"
        TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-cloud-upload-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --cloud-upload-only -platform offscreen)
    set_tests_properties(snow-shot-cloud-upload-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 45
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(
        NAME snow-shot-screenshot-action-toolbar-layout-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --action-toolbar-layout-only
    )
    add_test(NAME snow-shot-screenshot-result-toolbar-layout-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --screenshot-result-layout-only
                -platform offscreen)
    set_tests_properties(snow-shot-screenshot-result-toolbar-layout-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(
        NAME snow-shot-toolbar-color-input-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --color-input-only -platform offscreen
    )
    add_test(
        NAME snow-shot-screenshot-tool-shortcut-toggle-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --tool-shortcuts-only
    )
    add_test(NAME snow-shot-hidden-toolbar-shortcut-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --hidden-tools-only -platform offscreen)
    add_test(NAME snow-shot-annotation-toolbar-layout-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --toolbar-layout-only -platform offscreen)
    add_test(NAME snow-shot-toolbar-group-button-settings-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --first-group-button-only -platform offscreen)
    set_tests_properties(snow-shot-toolbar-group-button-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-annotation-toolbar-layout-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-hidden-toolbar-shortcut-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-screenshot-action-shortcut-display-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --screenshot-actions-tooltips-only
            -platform offscreen)
    set_tests_properties(snow-shot-screenshot-action-shortcut-display-tests PROPERTIES LABELS "unit")
    add_test(
        NAME snow-shot-screenshot-shortcut-tooltip-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --shortcut-tooltips-only
    )
    add_test(
        NAME snow-shot-screenshot-selection-reset-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --selection-reset-only
    )
    add_test(
        NAME snow-shot-screen-recording-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --recording-controls-only
    )
    add_test(
        NAME snow-shot-toolbar-shared-controls-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --color-control-styles-only
    )
    add_test(NAME snow-shot-pressure-brush-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --pressure-brush-only -platform offscreen)
    set_tests_properties(snow-shot-pressure-brush-toolbar-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    add_test(NAME snow-shot-stroke-rendering-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --stroke-rendering-only -platform offscreen)
    set_tests_properties(snow-shot-stroke-rendering-toolbar-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    add_test(NAME snow-shot-styled-template-storage-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --styled-template-storage-only -platform offscreen)
    set_tests_properties(snow-shot-styled-template-storage-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    set_tests_properties(snow-shot-toolbar-shared-controls-tests PROPERTIES TIMEOUT 180)
    add_test(
        NAME snow-shot-screenshot-toolbar-i18n-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --dynamic-i18n-only
    )
    set_tests_properties(
        snow-shot-screenshot-tool-palette-tests
        snow-shot-screenshot-tool-palette-remembered-drawing-tool-tests
        snow-shot-screenshot-action-toolbar-layout-tests
        snow-shot-screenshot-tool-shortcut-toggle-tests
        snow-shot-hidden-toolbar-shortcut-tests
        snow-shot-screenshot-shortcut-tooltip-tests
        snow-shot-screenshot-selection-reset-tests
        snow-shot-screen-recording-toolbar-tests
        snow-shot-toolbar-shared-controls-tests
        snow-shot-pressure-brush-toolbar-tests
        snow-shot-screenshot-toolbar-i18n-tests
        PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-screenshot-selection-toolbar-hover-tests
        tests/screenshot_selection_toolbar_hover_tests.cpp
        include/snow_shot/presentation/screenshotselectiontoolbarwidget.h
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/selection/screenshotselectiontoolbarwidget.cpp
        src/presentation/selection/screenshotselectiontoolbarwidgets.h
        src/presentation/selection/screenshotselectiontoolbarwidgets.cpp
    )
    snow_shot_import_offscreen_platform(snow-shot-screenshot-selection-toolbar-hover-tests)
    target_include_directories(snow-shot-screenshot-selection-toolbar-hover-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/selection"
    )
    target_link_libraries(snow-shot-screenshot-selection-toolbar-hover-tests PRIVATE
        snow_shot_window_shortcuts
        adqt::widgets
        adqt_icon_core
        ant_design_icons_qt
        Qt6::Widgets
        snow_draw_engine_qt_canvas
    )
    add_test(
        NAME snow-shot-screenshot-selection-toolbar-hover-tests
        COMMAND snow-shot-screenshot-selection-toolbar-hover-tests
    )
    add_test(
        NAME snow-shot-screenshot-selection-toolbar-compact-ratio-tests
        COMMAND snow-shot-screenshot-selection-toolbar-hover-tests --compact-ratio-tests
    )
    add_test(
        NAME snow-shot-screenshot-selection-toolbar-icon-centering-tests
        COMMAND snow-shot-screenshot-selection-toolbar-hover-tests --icon-centering-tests
    )
    add_test(
        NAME snow-shot-screenshot-selection-toolbar-popup-regression-tests
        COMMAND snow-shot-screenshot-selection-toolbar-hover-tests --popup-regression-tests
    )
    set_tests_properties(snow-shot-screenshot-selection-toolbar-popup-regression-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit"
    )
    set_tests_properties(snow-shot-screenshot-selection-toolbar-icon-centering-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit"
    )
    set_tests_properties(snow-shot-screenshot-selection-toolbar-compact-ratio-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit"
    )
    set_tests_properties(snow-shot-screenshot-selection-toolbar-hover-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
    )
    if(WIN32)
        add_test(NAME snow-shot-screenshot-selection-toolbar-native-tests
            COMMAND snow-shot-screenshot-selection-toolbar-hover-tests -platform windows)
        set_tests_properties(snow-shot-screenshot-selection-toolbar-native-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60
        )
    endif()

    add_executable(snow-shot-screenshot-toolbar-remembered-tool-tests
        tests/screenshot_toolbar_remembered_tool_tests.cpp
        src/presentation/core/screenshotinteractionstate.cpp
        src/presentation/core/screenshotintelligentselectionmodel.cpp
        src/presentation/overlay/screenshotoverlayinputhandler.cpp
        src/presentation/overlay/screenshotoverlayshortcutcontroller.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/toolbar/screenshottoolbarpresentationstatefactory.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshottoolbarwindow.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h
        src/presentation/toolbar/screenshottoolbarwindow.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
    )
    snow_shot_link_macos_window_platform(snow-shot-screenshot-toolbar-remembered-tool-tests)
    target_include_directories(snow-shot-screenshot-toolbar-remembered-tool-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-toolbar-remembered-tool-tests PRIVATE
        snow_shot_window_shortcuts
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        Qt6::Widgets
        snow_draw_engine_qt_canvas
        snow_shot_storage
    )
    if(WIN32)
        target_link_libraries(snow-shot-screenshot-toolbar-remembered-tool-tests PRIVATE comctl32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-screenshot-toolbar-remembered-tool-tests)
    add_test(NAME snow-shot-speech-bubble-command-tests
        COMMAND snow-shot-screenshot-toolbar-remembered-tool-tests --speech-bubble-only -platform offscreen)
    set_tests_properties(snow-shot-speech-bubble-command-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-scrolling-interval-command-tests
        COMMAND snow-shot-screenshot-toolbar-remembered-tool-tests --scrolling-interval-only -platform offscreen)
    add_test(NAME snow-shot-edit-selection-toolbar-visibility-tests
        COMMAND snow-shot-screenshot-toolbar-remembered-tool-tests --edit-selection-toolbar-only -platform offscreen)
    add_test(NAME snow-shot-selection-toolbar-lifecycle-tests
        COMMAND snow-shot-screenshot-toolbar-remembered-tool-tests --selection-toolbar-lifecycle-only -platform offscreen)
    set_tests_properties(snow-shot-edit-selection-toolbar-visibility-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-selection-toolbar-lifecycle-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-eraser-commands-tests
        COMMAND snow-shot-screenshot-toolbar-remembered-tool-tests --eraser-only -platform offscreen)
    set_tests_properties(snow-shot-eraser-commands-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-screenshot-toolbar-remembered-tool-tests
        COMMAND snow-shot-screenshot-toolbar-remembered-tool-tests)
    set_tests_properties(snow-shot-scrolling-interval-command-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    set_tests_properties(snow-shot-screenshot-toolbar-remembered-tool-tests
        snow-shot-scrolling-interval-command-tests PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-global-canvas-tests
        tests/global_canvas_tests.cpp
        tests/global_canvas_laser_tests.cpp
        src/presentation/services/screenshotcanvascolorsampler.cpp
        src/presentation/services/screenshotcanvascolorsamplerwindow.cpp
        include/snow_shot/presentation/globalcanvascontroller.h
        src/presentation/globalcanvas/globalcanvascontroller.cpp
        src/presentation/globalcanvas/globalcanvasplatform.cpp
        src/platform/physicalcursor.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
    )
    target_include_directories(snow-shot-global-canvas-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-global-canvas-tests PRIVATE
        snow_shot_window_shortcuts snow_shot_storage snow_draw_engine_qt_canvas
        adqt_icon_core ant_design_icons_qt ant_design_qt Qt6::Widgets Qt6::GuiPrivate)
    snow_shot_link_macos_window_platform(snow-shot-global-canvas-tests)
    if(WIN32)
        target_sources(snow-shot-global-canvas-tests PRIVATE src/platform/windows/pinnedwindownative.cpp)
        target_link_libraries(snow-shot-global-canvas-tests PRIVATE Qt6::GuiPrivate comctl32)
    endif()
    if(WIN32)
        add_test(NAME snow-shot-global-canvas-native-tests
            COMMAND snow-shot-global-canvas-tests --native-only -platform windows)
        set_tests_properties(snow-shot-global-canvas-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        add_test(NAME snow-shot-global-canvas-color-sampling-native-tests
            COMMAND snow-shot-global-canvas-tests --color-sampling-native-only -platform windows)
        set_tests_properties(snow-shot-global-canvas-color-sampling-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    if(APPLE)
        add_test(NAME snow-shot-global-canvas-platform-tests
            COMMAND snow-shot-global-canvas-tests --platform-only -platform offscreen)
        set_tests_properties(snow-shot-global-canvas-platform-tests PROPERTIES
            LABELS "unit;macos" TIMEOUT 30)
        foreach(_safe_area_scale IN ITEMS 1 2)
            add_test(NAME snow-shot-global-canvas-safe-area-${_safe_area_scale}-tests
                COMMAND snow-shot-global-canvas-tests --safe-area-only -platform offscreen)
            set_tests_properties(snow-shot-global-canvas-safe-area-${_safe_area_scale}-tests PROPERTIES
                LABELS "unit;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 30
                ENVIRONMENT "QT_SCALE_FACTOR=${_safe_area_scale}")
        endforeach()
        add_test(NAME snow-shot-global-canvas-native-tests
            COMMAND snow-shot-global-canvas-tests --native-only -platform cocoa)
        set_tests_properties(snow-shot-global-canvas-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 90)
        add_test(NAME snow-shot-global-canvas-window-shadow-native-tests
            COMMAND snow-shot-global-canvas-tests --window-shadow-only -platform cocoa)
        set_tests_properties(snow-shot-global-canvas-window-shadow-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 30)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-global-canvas-tests)
    foreach(_laser_scale IN ITEMS 1 1.25 1.5 2)
        add_test(NAME snow-shot-global-canvas-laser-${_laser_scale}-tests
            COMMAND snow-shot-global-canvas-tests --laser-pointer-only -platform offscreen)
        set_tests_properties(snow-shot-global-canvas-laser-${_laser_scale}-tests PROPERTIES
            LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 30
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=${_laser_scale}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    if(WIN32)
        add_test(NAME snow-shot-global-canvas-laser-native-tests
            COMMAND snow-shot-global-canvas-tests --laser-native-only -platform windows)
        set_tests_properties(snow-shot-global-canvas-laser-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-global-canvas-magnifier-availability-tests
        COMMAND snow-shot-global-canvas-tests --magnifier-availability-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-magnifier-availability-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-global-canvas-click-through-opacity-tests
        COMMAND snow-shot-global-canvas-tests --click-through-opacity-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-click-through-opacity-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-global-canvas-click-through-style-tests
        COMMAND snow-shot-global-canvas-tests --click-through-style-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-click-through-style-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-global-canvas-angle-wheel-tests
        COMMAND snow-shot-global-canvas-tests --angle-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-angle-wheel-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-global-canvas-window-shadow-tests
        COMMAND snow-shot-global-canvas-tests --window-shadow-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-window-shadow-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-global-canvas-history-shortcut-tests
        COMMAND snow-shot-global-canvas-tests --history-shortcuts-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-history-shortcut-tests PROPERTIES
        LABELS "unit" TIMEOUT 90 RUN_SERIAL TRUE)
    add_test(NAME snow-shot-global-canvas-saved-style-tests
        COMMAND snow-shot-global-canvas-tests --saved-styles-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-saved-style-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-global-canvas-quick-selection-settings-tests
        COMMAND snow-shot-global-canvas-tests --quick-selection-settings-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-quick-selection-settings-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-global-canvas-tests COMMAND snow-shot-global-canvas-tests -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-tests PROPERTIES LABELS "unit" TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-global-canvas-eraser-tests
        COMMAND snow-shot-global-canvas-tests --eraser-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-eraser-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-global-canvas-color-sampling-tests
        COMMAND snow-shot-global-canvas-tests --color-sampling-only -platform offscreen)
    set_tests_properties(snow-shot-global-canvas-color-sampling-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    if(WIN32)
        add_executable(snow-shot-screenshot-toolbar-fill-color-hover-tests
            tests/screenshot_toolbar_fill_color_hover_tests.cpp
            include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
            include/snow_shot/presentation/screenshottoolbarwindow.h
            include/snow_shot/presentation/screenshottoolpalette.h
            include/snow_shot/presentation/screenshottoolpalettehost.h
            include/snow_shot/presentation/styles/thememanager.h
            src/presentation/components/icons/snowshoticons.cpp
            src/platform/windows/monitorgeometry.cpp
            src/presentation/core/screenshotgeometry.cpp
            src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
            src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
            src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
            src/presentation/tools/screenshottoolbarmainpanel.cpp
            src/presentation/tools/screenshottoolpalettebuttons.cpp
            src/presentation/tools/screenshottoolpalettehost.cpp
            src/presentation/tools/screenshottoolpalettestylecontrols.cpp
            src/presentation/tools/screenshottoolpalettestylecomponents.cpp
            src/presentation/tools/screenshottoolpalettestylemodel.cpp
            src/presentation/tools/screenshottoolpalettestylepresets.cpp
            src/presentation/tools/screenshottoolpalette.cpp
            src/presentation/recording/recordingaudiogainpopover.cpp
            src/presentation/recording/recordingaudiogainpopover.h
            src/presentation/toolbar/screenshottoolbarwindow.cpp
            src/presentation/styles/thememanager.cpp
            src/presentation/styles/themecolorscheme.cpp
        )
        target_include_directories(
            snow-shot-screenshot-toolbar-fill-color-hover-tests
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(
            snow-shot-screenshot-toolbar-fill-color-hover-tests
            PRIVATE
                adqt_icon_core
                ant_design_icons_qt
                ant_design_qt
                Qt6::Widgets
                snow_draw_engine_qt_canvas
                comctl32
        )
        add_test(
            NAME snow-shot-screenshot-toolbar-fill-color-hover-tests
            COMMAND snow-shot-screenshot-toolbar-fill-color-hover-tests
        )
        set_tests_properties(
            snow-shot-screenshot-toolbar-fill-color-hover-tests
            PROPERTIES
                LABELS "interactive;windows"
                RUN_SERIAL TRUE
                TIMEOUT 90
                ENVIRONMENT
                    "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )

        # This is intentionally not registered with CTest. It creates native
        # always-on-top windows, moves the real cursor, and is designed for
        # explicit Release benchmarking sessions.
        add_executable(snow-shot-screenshot-toolbar-render-benchmark
            tests/screenshot_toolbar_render_benchmark.cpp
            include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
            include/snow_shot/presentation/screenshottoolbarwindow.h
            include/snow_shot/presentation/screenshottoolpalette.h
            include/snow_shot/presentation/screenshottoolpalettehost.h
            include/snow_shot/presentation/styles/thememanager.h
            src/presentation/components/icons/snowshoticons.cpp
            src/platform/windows/monitorgeometry.cpp
            src/presentation/core/screenshotgeometry.cpp
            src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
            src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
            src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
            src/presentation/tools/screenshottoolbarmainpanel.cpp
            src/presentation/tools/screenshottoolpalettebuttons.cpp
            src/presentation/tools/screenshottoolpalettehost.cpp
            src/presentation/tools/screenshottoolpalettestylecontrols.cpp
            src/presentation/tools/screenshottoolpalettestylecomponents.cpp
            src/presentation/tools/screenshottoolpalettestylemodel.cpp
            src/presentation/tools/screenshottoolpalettestylepresets.cpp
            src/presentation/tools/screenshottoolpalette.cpp
            src/presentation/recording/recordingaudiogainpopover.cpp
            src/presentation/recording/recordingaudiogainpopover.h
            src/presentation/tools/screenshottoolbarperfinstrumentation.h
            src/presentation/toolbar/screenshottoolbarwindow.cpp
            src/presentation/styles/thememanager.cpp
            src/presentation/styles/themecolorscheme.cpp
        )
        target_compile_definitions(
            snow-shot-screenshot-toolbar-render-benchmark
            PRIVATE
                NOMINMAX
                SNOW_SHOT_TOOLBAR_PERF_INSTRUMENTATION=1
        )
        target_include_directories(
            snow-shot-screenshot-toolbar-render-benchmark
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(
            snow-shot-screenshot-toolbar-render-benchmark
            PRIVATE
                adqt_icon_core
                ant_design_icons_qt
                ant_design_qt
                Qt6::Widgets
                snow_draw_engine_qt_canvas
                comctl32
                dwmapi
                user32
        )
    endif()

    add_executable(snow-shot-screen-recording-shortcut-tests
        tests/screen_recording_shortcut_tests.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenrecordingareawindow.h
        include/snow_shot/presentation/screenrecordingtoolbarwindow.h
        include/snow_shot/presentation/screenrecordingshortcutcontroller.h
        src/presentation/recording/screenrecordingareawindow.cpp
        src/presentation/recording/screenrecordingtoolbarwindow.cpp
        src/presentation/recording/screenrecordingshortcutcontroller.cpp
        src/presentation/recording/screenrecordinggeometry.cpp
        src/presentation/recording/recordingcountdownoverlay.cpp
        src/presentation/recording/recordingcountdownoverlay.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h
    )
    snow_shot_link_macos_window_platform(snow-shot-screen-recording-shortcut-tests)
    target_compile_definitions(snow-shot-screen-recording-shortcut-tests PRIVATE
        SNOW_SHOT_TEST_HOOKS=1
    )
    target_include_directories(snow-shot-screen-recording-shortcut-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screen-recording-shortcut-tests PRIVATE
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        Qt6::Widgets
        snow_draw_engine_qt_canvas
        snow_shot_window_shortcuts
        snow_shot_storage
    )
    if(WIN32)
        target_link_libraries(snow-shot-screen-recording-shortcut-tests PRIVATE user32 comctl32)
    endif()
    add_test(NAME snow-shot-screen-recording-shortcut-tests
        COMMAND snow-shot-screen-recording-shortcut-tests)
    add_test(NAME snow-shot-recording-auto-hide-shortcut-tests
        COMMAND snow-shot-screen-recording-shortcut-tests --auto-hide-shortcuts-only -platform offscreen)
    set_tests_properties(snow-shot-recording-auto-hide-shortcut-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    if(WIN32)
        foreach(_close_release_pair IN ITEMS
                "overlay|snow-shot-screenshot-canvas-renderer-tests"
                "pinned|snow-shot-pinned-window-tests"
                "recording|snow-shot-screen-recording-shortcut-tests")
            string(REPLACE "|" ";" _close_release_parts "${_close_release_pair}")
            list(GET _close_release_parts 0 _close_release_name)
            list(GET _close_release_parts 1 _close_release_target)
            add_test(NAME snow-shot-${_close_release_name}-close-release-native-tests
                COMMAND ${_close_release_target} --close-release-native -platform windows)
            set_tests_properties(snow-shot-${_close_release_name}-close-release-native-tests
                PROPERTIES LABELS "interactive;e2e;windows" RUN_SERIAL TRUE TIMEOUT 60
                ENVIRONMENT "QT_QPA_PLATFORM=windows")
        endforeach()
    endif()
    if(APPLE)
        set(_snow_recording_shortcut_platform cocoa)
        set(_snow_recording_shortcut_os macos)
    else()
        set(_snow_recording_shortcut_platform windows)
        set(_snow_recording_shortcut_os windows)
    endif()
    add_test(NAME snow-shot-recording-area-shortcut-tests
        COMMAND snow-shot-screen-recording-shortcut-tests --area-history-only -platform offscreen)
    set_tests_properties(snow-shot-recording-area-shortcut-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recording-history-shortcut-tests
        COMMAND snow-shot-screen-recording-shortcut-tests --history-shortcuts-only)
    set_tests_properties(snow-shot-screen-recording-shortcut-tests
        snow-shot-recording-history-shortcut-tests PROPERTIES
        # The shortcut gating asserts real keyboard focus ownership, which the
        # non-activating floating toolbar only supports on a native platform.
        LABELS "interactive;e2e;${_snow_recording_shortcut_os}" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=${_snow_recording_shortcut_platform}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-floating-toolbar-drag-tests
        tests/screenshot_floating_toolbar_drag_tests.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshottoolbarwindow.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h
        src/presentation/toolbar/screenshottoolbarwindow.cpp
    )
    snow_shot_link_macos_window_platform(snow-shot-floating-toolbar-drag-tests)
    target_compile_definitions(snow-shot-floating-toolbar-drag-tests PRIVATE
        SNOW_SHOT_TEST_HOOKS=1
    )
    target_include_directories(snow-shot-floating-toolbar-drag-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-floating-toolbar-drag-tests PRIVATE
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        Qt6::Widgets
        snow_draw_engine_qt_canvas
    )
    if(WIN32)
        target_link_libraries(snow-shot-floating-toolbar-drag-tests PRIVATE
            comctl32
        )
    endif()
    if(APPLE)
        target_sources(snow-shot-floating-toolbar-drag-tests PRIVATE
            ../cmake/test-support/macos_native_input.mm)
        target_link_libraries(snow-shot-floating-toolbar-drag-tests PRIVATE "-framework AppKit")
        target_include_directories(snow-shot-floating-toolbar-drag-tests PRIVATE ../cmake/test-support)
        add_test(NAME snow-shot-macos-toolbar-shadow-input-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --macos-shadow-input-only -platform cocoa)
        set_tests_properties(snow-shot-macos-toolbar-shadow-input-tests PROPERTIES
            LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60)
        add_test(NAME snow-shot-macos-toolbar-cross-display-drag-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --macos-cross-display-only -platform cocoa)
        set_tests_properties(snow-shot-macos-toolbar-cross-display-drag-tests PROPERTIES
            LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE
            TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
    # Exercise the real UI host with the same overlay and toolbar implementations.
    get_target_property(_color_picker_overlay_sources
        snow-shot-screenshot-canvas-renderer-tests SOURCES)
    get_target_property(_color_picker_toolbar_sources
        snow-shot-screenshot-toolbar-remembered-tool-tests SOURCES)
    set(_color_picker_lifecycle_sources
        ${_color_picker_overlay_sources} ${_color_picker_toolbar_sources})
    list(FILTER _color_picker_lifecycle_sources EXCLUDE REGEX "^tests/")
    list(FILTER _color_picker_lifecycle_sources EXCLUDE
        REGEX "/(directcapturehistory|screenshothistoryservice)[.]")
    list(REMOVE_DUPLICATES _color_picker_lifecycle_sources)
    # Exercise scrolling session replacement with synthetic frames and native policy adapters.
    add_library(snow-shot-scrolling-settings-test-runtime OBJECT
        include/snow_shot/presentation/screenshotscrollingcapturecontroller.h
        src/presentation/capture/screenshotscrollingcapturecontroller.cpp
        src/presentation/capture/screenshotscrollingpipeline.cpp)
    target_include_directories(snow-shot-scrolling-settings-test-runtime PRIVATE include src
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-stitch-images-c/include")
    target_link_libraries(snow-shot-scrolling-settings-test-runtime PRIVATE
        Qt6::Widgets snow_draw_engine_qt_canvas snow_canvas_image snow_stitch_images_c
        snow_shot_diagnostics ant_design_qt adqt_icon_core ant_design_icons_qt)
    target_compile_definitions(snow-shot-scrolling-settings-test-runtime PRIVATE
        setWindowExcludedFromCapture=snowTestSetWindowExcludedFromCapture
        captureWindowId=snowTestCaptureWindowId
        screenshotScrollPermission=snowTestScrollPermission
        sendScreenshotScroll=snowTestSendScreenshotScroll
        sendScrollingWheelStep=snowTestSendScrollingWheelStep
        flushWindowComposition=snowTestFlushWindowComposition)
    set(_scrolling_settings_test_sources ${_color_picker_lifecycle_sources}
        ${SNOW_SHOT_COLLAGE_EXPORT_TEST_SOURCES}
        src/presentation/services/screenshotexportscrollingsource.cpp)
    list(REMOVE_DUPLICATES _scrolling_settings_test_sources)
    add_executable(snow-shot-scrolling-capture-settings-tests
        tests/scrolling_capture_settings_tests.cpp
        ${_scrolling_settings_test_sources}
        src/presentation/overlay/screenshotoverlaycoordinator.cpp
        src/presentation/overlay/screenshotoverlayuihost.cpp
        src/presentation/services/screenshotcolorpickerwindow.cpp
        include/snow_shot/presentation/screenshotselectiontoolbarwidget.h
        src/presentation/selection/screenshotselectiontoolbarwidget.cpp
        src/presentation/selection/screenshotselectiontoolbarwidgets.h
        src/presentation/selection/screenshotselectiontoolbarwidgets.cpp)
    target_include_directories(snow-shot-scrolling-capture-settings-tests PRIVATE include src
        src/image src/presentation/services)
    target_link_libraries(snow-shot-scrolling-capture-settings-tests PRIVATE
        snow-shot-scrolling-settings-test-runtime snow_shot_diagnostics snow_shot_storage
        snow_shot_image_split
        snow_shot_window_shortcuts snow_draw_engine_qt_canvas snow_canvas_image snow_stitch_images_c
        ant_design_qt adqt_icon_core ant_design_icons_qt Qt6::Widgets)
    snow_shot_link_macos_window_platform(snow-shot-scrolling-capture-settings-tests)
    if(WIN32)
        target_link_libraries(snow-shot-scrolling-capture-settings-tests PRIVATE comctl32)
    elseif(APPLE)
        target_sources(snow-shot-scrolling-capture-settings-tests PRIVATE
            src/platform/macos/windowcaptureexclusion.mm)
        target_link_libraries(snow-shot-scrolling-capture-settings-tests PRIVATE
            "-framework AppKit" "-framework CoreGraphics")
    endif()
    snow_shot_import_offscreen_platform(snow-shot-scrolling-capture-settings-tests)
    add_test(NAME snow-shot-scrolling-capture-settings-tests
        COMMAND snow-shot-scrolling-capture-settings-tests -platform offscreen)
    add_test(NAME snow-shot-scrolling-image-split-tests
        COMMAND snow-shot-scrolling-capture-settings-tests --image-split-only -platform offscreen)
    set_tests_properties(snow-shot-scrolling-capture-settings-tests
        snow-shot-scrolling-image-split-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(snow-shot-color-picker-lifecycle-tests
        tests/screenshot_color_picker_lifecycle_tests.cpp
        src/presentation/services/screenshotcanvascolorsampler.cpp
        src/presentation/services/screenshotcanvascolorsamplerwindow.cpp
        src/presentation/services/screenshotcolorpickercontroller.cpp
        src/platform/physicalcursor.cpp
        ${_color_picker_lifecycle_sources}
        src/presentation/overlay/screenshotoverlaycoordinator.cpp
        src/presentation/overlay/screenshotoverlayuihost.cpp
        src/presentation/services/screenshotcolorpickerwindow.cpp
        include/snow_shot/presentation/screenshotselectiontoolbarwidget.h
        src/presentation/selection/screenshotselectiontoolbarwidget.cpp
        src/presentation/selection/screenshotselectiontoolbarwidgets.h
        src/presentation/selection/screenshotselectiontoolbarwidgets.cpp
    )
    add_test(NAME snow-shot-screenshot-style-binding-tests
        COMMAND snow-shot-color-picker-lifecycle-tests --style-binding-only -platform offscreen)
    set_tests_properties(snow-shot-screenshot-style-binding-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-selection-toolbar-visibility-tests
        COMMAND snow-shot-color-picker-lifecycle-tests --selection-toolbar-visibility-only -platform offscreen)
    set_tests_properties(snow-shot-selection-toolbar-visibility-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    snow_shot_link_macos_window_platform(snow-shot-color-picker-lifecycle-tests)
    target_compile_definitions(snow-shot-color-picker-lifecycle-tests PRIVATE
        SNOW_SHOT_BENCH_INTERNALS=1)
    target_include_directories(snow-shot-color-picker-lifecycle-tests PRIVATE include)
    target_link_libraries(snow-shot-color-picker-lifecycle-tests PRIVATE
        snow_shot_diagnostics snow_shot_storage snow_shot_window_shortcuts
        snow_draw_engine_qt_canvas ant_design_qt adqt_icon_core ant_design_icons_qt Qt6::Widgets)
    if(WIN32)
        target_link_libraries(snow-shot-color-picker-lifecycle-tests PRIVATE comctl32)
    elseif(APPLE)
        target_sources(snow-shot-color-picker-lifecycle-tests PRIVATE
            src/platform/macos/windowcaptureexclusion.mm)
        target_link_libraries(snow-shot-color-picker-lifecycle-tests PRIVATE
            "-framework AppKit" "-framework CoreGraphics")
    endif()
    if(APPLE)
        set_source_files_properties(tests/screenshot_color_picker_lifecycle_tests.cpp
            PROPERTIES LANGUAGE OBJCXX)
        add_test(NAME snow-shot-macos-canvas-sampler-stacking-native-tests
            COMMAND snow-shot-color-picker-lifecycle-tests --canvas-sampler-only -platform cocoa)
        set_tests_properties(snow-shot-macos-canvas-sampler-stacking-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-color-picker-lifecycle-tests)
    add_test(NAME snow-shot-canvas-sampling-session-tests
        COMMAND snow-shot-color-picker-lifecycle-tests --canvas-sampler-only -platform offscreen)
    set_tests_properties(snow-shot-canvas-sampling-session-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-overlay-reveal-tests
        COMMAND snow-shot-color-picker-lifecycle-tests --overlay-reveal-only -platform offscreen)
    set_tests_properties(snow-shot-overlay-reveal-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-color-picker-lifecycle-tests
        COMMAND snow-shot-color-picker-lifecycle-tests)
    add_test(NAME snow-shot-toolbar-picker-hover-tests
        COMMAND snow-shot-color-picker-lifecycle-tests --toolbar-picker-hover-only)
    add_test(NAME snow-shot-toolbar-guide-hover-tests
        COMMAND snow-shot-color-picker-lifecycle-tests --toolbar-guide-hover-only)
    set_tests_properties(snow-shot-toolbar-guide-hover-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        TIMEOUT 60)
    set_tests_properties(snow-shot-toolbar-picker-hover-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        TIMEOUT 60)
    if(WIN32)
        add_test(NAME snow-shot-tool-stacking-native-tests
            COMMAND snow-shot-color-picker-lifecycle-tests --stacking-only -platform windows)
        set_tests_properties(snow-shot-tool-stacking-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE)
    endif()
    set_tests_properties(snow-shot-color-picker-lifecycle-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        TIMEOUT 60)
    # Share the real screenshot UI implementation between deterministic frame tests
    # and the preparation benchmark; the fixture never substitutes the presenter.
    set(_selection_presentation_sources ${_color_picker_lifecycle_sources}
        src/presentation/services/screenshotcolorpickercontroller.cpp
        src/platform/physicalcursor.cpp
        src/presentation/overlay/screenshotoverlaycoordinator.cpp
        src/presentation/overlay/screenshotoverlayuihost.cpp
        src/presentation/services/screenshotcolorpickerwindow.cpp
        include/snow_shot/presentation/screenshotselectiontoolbarwidget.h
        src/presentation/selection/screenshotselectiontoolbarwidget.cpp
        src/presentation/selection/screenshotselectiontoolbarwidgets.h
        src/presentation/selection/screenshotselectiontoolbarwidgets.cpp
        src/presentation/services/screenshotpresentationservices.cpp
        src/presentation/core/screenshotsmartselectiontransition.cpp
        src/presentation/toolbar/screenshottoolbarpresenter.cpp
        src/presentation/toolbar/screenshottoolbarpresentationstatefactory.cpp)
    list(REMOVE_DUPLICATES _selection_presentation_sources)
    function(snow_shot_selection_presentation_runtime target source_root canvas_target)
        set(_sources)
        foreach(_source IN LISTS _selection_presentation_sources)
            list(APPEND _sources "${source_root}/${_source}")
        endforeach()
        add_library(${target} STATIC ${_sources})
        target_include_directories(${target} BEFORE PUBLIC "${source_root}/include")
        target_compile_definitions(${target} PUBLIC SNOW_SHOT_BENCH_INTERNALS=1)
        target_link_libraries(${target} PUBLIC snow_shot_diagnostics snow_shot_storage
            snow_shot_window_shortcuts ${canvas_target} ant_design_qt adqt_icon_core
            ant_design_icons_qt Qt6::Widgets Qt6::GuiPrivate)
        snow_shot_link_macos_window_platform(${target})
        if(WIN32)
            target_link_libraries(${target} PUBLIC comctl32)
        elseif(APPLE)
            target_sources(${target} PRIVATE "${source_root}/src/platform/macos/windowcaptureexclusion.mm")
            target_link_libraries(${target} PUBLIC "-framework AppKit" "-framework CoreGraphics")
        endif()
    endfunction()
    snow_shot_selection_presentation_runtime(snow-shot-selection-presentation-runtime
        "${CMAKE_CURRENT_SOURCE_DIR}" snow_draw_engine_qt_canvas)
    target_sources(snow-shot-selection-presentation-runtime PRIVATE
        src/presentation/services/screenshotpresentationframescheduler.cpp)
    add_executable(snow-shot-presentation-frame-scheduler-tests
        tests/screenshot_presentation_frame_scheduler_tests.cpp
        src/presentation/services/screenshotpresentationframescheduler.cpp)
    target_include_directories(snow-shot-presentation-frame-scheduler-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-presentation-frame-scheduler-tests PRIVATE Qt6::Core)
    add_test(NAME snow-shot-presentation-frame-scheduler-tests
        COMMAND snow-shot-presentation-frame-scheduler-tests)
    set_tests_properties(snow-shot-presentation-frame-scheduler-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(snow-shot-animation-codec-tests tests/animation_codec_tests.cpp)
    target_include_directories(snow-shot-animation-codec-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(snow-shot-animation-codec-tests PRIVATE snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-animation-codec-tests)
    add_test(NAME snow-shot-animation-codec-tests
        COMMAND snow-shot-animation-codec-tests -platform offscreen)
    set_tests_properties(snow-shot-animation-codec-tests PROPERTIES LABELS "unit" TIMEOUT 60)

    # Exercise the in-executable codec ABI with static Qt, including Qt builds
    # that provide bundled libpng. APNG must use only the ordinary PNG API.
    add_library(snow_shot_animation_static_backend STATIC ${SNOW_SHOT_IMAGE_CODEC_BACKEND_SOURCES})
    target_include_directories(snow_shot_animation_static_backend PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    target_compile_definitions(snow_shot_animation_static_backend PUBLIC
        SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC=1)
    target_link_libraries(snow_shot_animation_static_backend PRIVATE
        snow_pixel_buffer snow_image::snow_image_static)
    target_compile_features(snow_shot_animation_static_backend PUBLIC cxx_std_20)
    set_target_properties(snow_shot_animation_static_backend PROPERTIES
        AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
    add_library(snow_shot_animation_static_qt_codec STATIC ${SNOW_SHOT_IMAGE_CODEC_SOURCES})
    target_include_directories(snow_shot_animation_static_qt_codec PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
        "${SNOW_SHOT_SNOW_IMAGE_DIR}/include")
    target_link_libraries(snow_shot_animation_static_qt_codec PUBLIC
        snow_shot_animation_static_backend snow_canvas_image Qt6::Core Qt6::Gui)
    target_compile_features(snow_shot_animation_static_qt_codec PUBLIC cxx_std_20)
    add_executable(snow-shot-animation-static-codec-tests tests/animation_codec_tests.cpp)
    target_include_directories(snow-shot-animation-static-codec-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(snow-shot-animation-static-codec-tests PRIVATE
        snow_shot_animation_static_qt_codec)
    snow_shot_import_offscreen_platform(snow-shot-animation-static-codec-tests)
    add_test(NAME snow-shot-animation-static-codec-tests
        COMMAND snow-shot-animation-static-codec-tests -platform offscreen)
    set_tests_properties(snow-shot-animation-static-codec-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    if(WIN32 AND NOT SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
        add_custom_command(TARGET snow-shot-animation-static-codec-tests POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DSNOW_RUNTIME_ROOT_LIBRARY=$<TARGET_FILE:snow-shot-animation-static-codec-tests>"
                "-DSNOW_RUNTIME_DESTINATION=$<TARGET_FILE_DIR:snow-shot-animation-static-codec-tests>"
                "-DSNOW_RUNTIME_SEARCH_DIRECTORY=${_SNOW_SHOT_IMAGE_RUNTIME_SEARCH_DIRECTORY}"
                -P "${_SNOW_SHOT_IMAGE_RUNTIME_STAGING_SCRIPT}"
            COMMENT "Staging static animation codec test runtime dependencies" VERBATIM)
    endif()

    add_executable(snow-shot-animation-playback-tests tests/screenshot_animation_playback_tests.cpp)
    target_link_libraries(snow-shot-animation-playback-tests PRIVATE snow_shot_animation_playback)
    snow_shot_import_offscreen_platform(snow-shot-animation-playback-tests)
    add_test(NAME snow-shot-animation-playback-tests
        COMMAND snow-shot-animation-playback-tests -platform offscreen)
    set_tests_properties(snow-shot-animation-playback-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    if(SNOW_SHOT_BUILD_BENCHMARKS)
        add_executable(snow-shot-animation-playback-benchmark
            tests/screenshot_animation_playback_performance_benchmark.cpp)
        target_link_libraries(snow-shot-animation-playback-benchmark PRIVATE snow_shot_animation_playback)
        snow_shot_import_offscreen_platform(snow-shot-animation-playback-benchmark)
        add_executable(snow-shot-presentation-frame-scheduler-benchmark
            tests/screenshot_presentation_frame_scheduler_performance_benchmark.cpp
            src/presentation/services/screenshotpresentationframescheduler.cpp)
        target_include_directories(snow-shot-presentation-frame-scheduler-benchmark PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-presentation-frame-scheduler-benchmark PRIVATE Qt6::Core)
    endif()
    add_executable(snow-shot-selection-presentation-tests
        tests/screenshot_selection_presentation_tests.cpp)
    target_link_libraries(snow-shot-selection-presentation-tests PRIVATE
        snow-shot-selection-presentation-runtime)
    snow_shot_import_offscreen_platform(snow-shot-selection-presentation-tests)
    add_test(NAME snow-shot-selection-presentation-tests
        COMMAND snow-shot-selection-presentation-tests -platform offscreen)
    add_test(NAME snow-shot-selection-color-picker-tests
        COMMAND snow-shot-selection-presentation-tests --picker-drag-only -platform offscreen)
    add_test(NAME snow-shot-toolbar-presenter-lifetime-tests
        COMMAND snow-shot-selection-presentation-tests --toolbar-presenter-lifetime-only -platform offscreen)
    add_executable(snow-shot-selection-wheel-tests
        tests/screenshot_selection_wheel_tests.cpp)
    target_link_libraries(snow-shot-selection-wheel-tests PRIVATE
        snow-shot-selection-presentation-runtime)
    snow_shot_import_offscreen_platform(snow-shot-selection-wheel-tests)
    add_test(NAME snow-shot-selection-wheel-tests
        COMMAND snow-shot-selection-wheel-tests -platform offscreen)
    set_tests_properties(snow-shot-selection-wheel-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(snow-shot-selection-quick-set-tests
        tests/screenshot_selection_quick_sets_tests.cpp
        src/presentation/services/screenshothistoryservice.cpp
        src/presentation/services/screenshotimagefileservice.cpp)
    target_include_directories(snow-shot-selection-quick-set-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image")
    target_link_libraries(snow-shot-selection-quick-set-tests PRIVATE
        snow-shot-selection-presentation-runtime snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-selection-quick-set-tests)
    add_test(NAME snow-shot-selection-quick-set-compound-tests
        COMMAND snow-shot-selection-quick-set-tests --compound-only -platform offscreen)
    add_test(NAME snow-shot-selection-quick-set-restore-tests
        COMMAND snow-shot-selection-quick-set-tests --restore-only -platform offscreen)
    set_tests_properties(snow-shot-selection-quick-set-compound-tests
        snow-shot-selection-quick-set-restore-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-overlay-preparation-tests
        COMMAND snow-shot-color-picker-lifecycle-tests --overlay-preparation-only -platform offscreen)
    add_test(NAME snow-shot-pointer-presentation-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --pointer-presentation-only -platform offscreen)
    add_test(NAME snow-shot-background-coverage-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --background-coverage-only -platform offscreen)
    add_test(NAME snow-shot-rectangular-selection-rendering-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --rectangular-selection-rendering-only -platform offscreen)
    set_tests_properties(snow-shot-selection-presentation-tests snow-shot-selection-color-picker-tests
        snow-shot-toolbar-presenter-lifetime-tests snow-shot-overlay-preparation-tests
        snow-shot-background-coverage-tests snow-shot-pointer-presentation-tests
        snow-shot-rectangular-selection-rendering-tests
        PROPERTIES LABELS "unit" TIMEOUT 60
        SNOW_TEST_CROSS_PLATFORM TRUE ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(SNOW_SHOT_BUILD_BENCHMARKS)
        if(APPLE)
            # Opt-in native startup timing; intentionally not registered with CTest.
            add_executable(snow-shot-backing-store-startup-benchmark
                tests/screenshot_backing_store_startup_performance_benchmark.cpp)
            target_include_directories(snow-shot-backing-store-startup-benchmark PRIVATE
                "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
            target_link_libraries(snow-shot-backing-store-startup-benchmark PRIVATE
                snow-shot-selection-presentation-runtime snow_capture_c)
        endif()
        add_executable(snow-shot-selection-presentation-benchmark
            tests/screenshot_selection_presentation_performance_benchmark.cpp)
        target_link_libraries(snow-shot-selection-presentation-benchmark PRIVATE
            snow-shot-selection-presentation-runtime)
        snow_shot_import_offscreen_platform(snow-shot-selection-presentation-benchmark)
        set(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR "" CACHE PATH
            "Optional archived source tree for the same presentation benchmark")
        set(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_CANVAS_LIBRARY "" CACHE FILEPATH
            "Canvas library built from the archived sources")
        if(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR AND
           SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_CANVAS_LIBRARY)
            add_library(snow-shot-selection-baseline-canvas STATIC IMPORTED)
            set_target_properties(snow-shot-selection-baseline-canvas PROPERTIES
                SYSTEM FALSE
                IMPORTED_LOCATION "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_CANVAS_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES
                    "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR}/snow_draw_engine_qt/include")
            get_target_property(_canvas_dependencies snow_draw_engine_qt_canvas INTERFACE_LINK_LIBRARIES)
            set_target_properties(snow-shot-selection-baseline-canvas PROPERTIES
                INTERFACE_LINK_LIBRARIES "${_canvas_dependencies}")
            snow_shot_selection_presentation_runtime(snow-shot-selection-presentation-baseline-runtime
                "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR}/snow_shot"
                snow-shot-selection-baseline-canvas)
            target_sources(snow-shot-selection-presentation-baseline-runtime PRIVATE
                "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR}/snow_shot/src/presentation/overlay/screenshotimagerendering.cpp")
            # Reuse unchanged support archives, replacing the canvas throughout their
            # transitive link interfaces (storage's image codec also links the canvas).
            function(snow_shot_baseline_needs_canvas dependency output visited)
                if(dependency MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
                    snow_shot_baseline_needs_canvas("${CMAKE_MATCH_1}" _needed "${visited}")
                    set(${output} "${_needed}" PARENT_SCOPE)
                    return()
                endif()
                if(dependency STREQUAL "snow_draw_engine_qt_canvas")
                    set(${output} TRUE PARENT_SCOPE)
                    return()
                endif()
                set(${output} FALSE PARENT_SCOPE)
                if(NOT TARGET "${dependency}" OR dependency IN_LIST visited)
                    return()
                endif()
                get_target_property(_type "${dependency}" TYPE)
                get_target_property(_imported "${dependency}" IMPORTED)
                if(NOT _type STREQUAL "STATIC_LIBRARY" OR _imported)
                    return()
                endif()
                list(APPEND visited "${dependency}")
                get_target_property(_links "${dependency}" INTERFACE_LINK_LIBRARIES)
                if(_links)
                    foreach(_link IN LISTS _links)
                        snow_shot_baseline_needs_canvas("${_link}" _needed "${visited}")
                        if(_needed)
                            set(${output} TRUE PARENT_SCOPE)
                            return()
                        endif()
                    endforeach()
                endif()
            endfunction()
            function(snow_shot_baseline_dependency dependency output)
                if(dependency MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
                    snow_shot_baseline_dependency("${CMAKE_MATCH_1}" _mapped)
                    set(${output} "$<LINK_ONLY:${_mapped}>" PARENT_SCOPE)
                    return()
                endif()
                if(dependency STREQUAL "snow_draw_engine_qt_canvas")
                    set(${output} snow-shot-selection-baseline-canvas PARENT_SCOPE)
                    return()
                endif()
                if(TARGET "${dependency}")
                    get_target_property(_type "${dependency}" TYPE)
                    get_target_property(_imported "${dependency}" IMPORTED)
                    snow_shot_baseline_needs_canvas("${dependency}" _needs_override "")
                    if(_type STREQUAL "STATIC_LIBRARY" AND NOT _imported AND _needs_override)
                        string(REPLACE "::" "-" _wrapper "snow-shot-baseline-${dependency}")
                        if(NOT TARGET "${_wrapper}")
                            add_library("${_wrapper}" INTERFACE)
                            add_dependencies("${_wrapper}" "${dependency}")
                            target_link_libraries("${_wrapper}" INTERFACE "$<TARGET_FILE:${dependency}>")
                            foreach(_property IN ITEMS INTERFACE_INCLUDE_DIRECTORIES
                                INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS
                                INTERFACE_COMPILE_FEATURES)
                                get_target_property(_value "${dependency}" "${_property}")
                                if(_value)
                                    set_target_properties("${_wrapper}" PROPERTIES "${_property}" "${_value}")
                                endif()
                            endforeach()
                            get_target_property(_links "${dependency}" INTERFACE_LINK_LIBRARIES)
                            if(_links)
                                foreach(_link IN LISTS _links)
                                    snow_shot_baseline_dependency("${_link}" _mapped)
                                    target_link_libraries("${_wrapper}" INTERFACE "${_mapped}")
                                endforeach()
                            endif()
                        endif()
                        set(${output} "${_wrapper}" PARENT_SCOPE)
                        return()
                    endif()
                endif()
                set(${output} "${dependency}" PARENT_SCOPE)
            endfunction()
            function(snow_shot_finish_baseline_dependencies)
                get_target_property(_baseline_links snow-shot-selection-presentation-baseline-runtime
                    INTERFACE_LINK_LIBRARIES)
                set(_mapped_baseline_links)
                foreach(_link IN LISTS _baseline_links)
                    snow_shot_baseline_dependency("${_link}" _mapped)
                    list(APPEND _mapped_baseline_links "${_mapped}")
                endforeach()
                set_target_properties(snow-shot-selection-presentation-baseline-runtime PROPERTIES
                    INTERFACE_LINK_LIBRARIES "${_mapped_baseline_links}")
            endfunction()
            # Image-codec backends receive their canvas link later in this directory.
            cmake_language(DEFER CALL snow_shot_finish_baseline_dependencies)
            target_include_directories(snow-shot-selection-presentation-baseline-runtime BEFORE PUBLIC
                "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR}/snow_draw_engine_qt/include")
            target_compile_definitions(snow-shot-selection-presentation-baseline-runtime PUBLIC
                SNOW_SHOT_SELECTION_PRESENTATION_BASELINE=1)
            file(READ "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR}/snow_shot/include/snow_shot/presentation/screenshotpresentationservices.h"
                _baseline_presentation_header)
            if(_baseline_presentation_header MATCHES "flushPendingFrame" AND
               _baseline_presentation_header MATCHES "monotonicNanoseconds")
                target_compile_definitions(snow-shot-selection-presentation-baseline-runtime PUBLIC
                    SNOW_SHOT_SELECTION_BASELINE_HAS_FRAME_SCHEDULER=1)
            endif()
            add_executable(snow-shot-selection-presentation-baseline-benchmark
                tests/screenshot_selection_presentation_performance_benchmark.cpp)
            target_link_libraries(snow-shot-selection-presentation-baseline-benchmark PRIVATE
                snow-shot-selection-presentation-baseline-runtime)
            snow_shot_import_offscreen_platform(snow-shot-selection-presentation-baseline-benchmark)
        endif()
    endif()
    if(SNOW_SHOT_BUILD_BENCHMARKS AND TARGET snow-shot-selection-baseline-canvas)
        # Keep the harness identical while freezing renderer code, inline geometry,
        # and canvas implementation at the archived comparison revision.
        set(_selection_render_baseline_root
            "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR}/snow_shot")
        set(_selection_render_baseline_sources)
        foreach(_source IN ITEMS
            include/snow_shot/presentation/screenshotcanvasrenderer.h
            include/snow_shot/presentation/screenshotselectionshadowrenderer.h
            src/presentation/ocr/screenshotocrpresentation.cpp
            src/presentation/ocr/screenshotocrtextlayout.cpp
            src/presentation/ocr/screenshotocrtexttransform.cpp
            src/presentation/ocr/screenshotocrlayout.cpp
            src/presentation/ocr/screenshottabledocument.cpp
            src/presentation/overlay/screenshotcanvasrenderer.cpp
            src/presentation/overlay/screenshotimagerendering.cpp
            src/presentation/overlay/screenshotselectionshadowrenderer.cpp)
            list(APPEND _selection_render_baseline_sources
                "${_selection_render_baseline_root}/${_source}")
        endforeach()
        add_executable(snow-shot-selection-render-baseline-benchmark
            tests/screenshot_selection_render_benchmark.cpp
            ${_selection_render_baseline_sources})
        target_compile_definitions(snow-shot-selection-render-baseline-benchmark PRIVATE
            NOMINMAX SNOW_SHOT_BENCH_INTERNALS=1 SNOW_SHOT_SELECTION_RENDER_BASELINE=1)
        target_include_directories(snow-shot-selection-render-baseline-benchmark BEFORE PRIVATE
            "${_selection_render_baseline_root}/include"
            "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR}/snow_draw_engine_qt/include"
            "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR}/snow_draw_engine_qt/src/rendering"
            "${SNOW_SHOT_SELECTION_PRESENTATION_BASELINE_DIR}/snow_draw_engine_qt/crates/snow-draw-engine-c/include")
        snow_shot_baseline_dependency(ant_design_qt _selection_render_baseline_antd)
        target_link_libraries(snow-shot-selection-render-baseline-benchmark PRIVATE
            Qt6::Widgets ${_selection_render_baseline_antd} snow-shot-selection-baseline-canvas)
        if(WIN32)
            target_link_libraries(snow-shot-selection-render-baseline-benchmark PRIVATE dwmapi user32)
        endif()
        snow_shot_import_offscreen_platform(snow-shot-selection-render-baseline-benchmark)
        unset(_selection_render_baseline_root)
        unset(_selection_render_baseline_sources)
        unset(_selection_render_baseline_antd)
    endif()
    if(WIN32 AND SNOW_SHOT_BUILD_BENCHMARKS AND
       TARGET snow-shot-selection-presentation-baseline-runtime)
        add_executable(snow-shot-selection-toolbar-baseline-performance-benchmark
            tests/screenshot_selection_toolbar_performance_benchmark.cpp)
        target_compile_definitions(snow-shot-selection-toolbar-baseline-performance-benchmark PRIVATE
            SNOW_SELECTION_TOOLBAR_BASELINE=1)
        target_link_libraries(snow-shot-selection-toolbar-baseline-performance-benchmark PRIVATE
            snow-shot-selection-presentation-baseline-runtime)
        snow_shot_import_offscreen_platform(snow-shot-selection-toolbar-baseline-performance-benchmark)
    endif()
    if(SNOW_SHOT_BUILD_BENCHMARKS)
        set(_selection_validation_targets snow_shot snow-shot-selection-presentation-tests
            snow-shot-selection-presentation-benchmark snow-shot-screenshot-canvas-renderer-tests
            snow-shot-color-picker-lifecycle-tests snow-shot-smart-selection-transition-tests
            snow-shot-presentation-frame-scheduler-tests
            snow-shot-presentation-frame-scheduler-benchmark)
        if(WIN32)
            list(APPEND _selection_validation_targets
                snow-shot-selection-render-benchmark
                snow-shot-selection-toolbar-performance-benchmark
                snow-shot-screenshot-selection-toolbar-hover-tests)
        endif()
        foreach(_target IN ITEMS snow-shot-selection-presentation-baseline-benchmark
            snow-shot-selection-render-baseline-benchmark
            snow-shot-selection-toolbar-baseline-performance-benchmark
            snow-canvas-custom-renderer-tests snow-canvas-distance-tests)
            if(TARGET ${_target})
                list(APPEND _selection_validation_targets ${_target})
            endif()
        endforeach()
        add_custom_target(snow-shot-selection-presentation-validation-build
            DEPENDS ${_selection_validation_targets})
        unset(_selection_validation_targets)
    endif()
    unset(_selection_presentation_sources)
    unset(_color_picker_overlay_sources)
    unset(_color_picker_toolbar_sources)
    unset(_color_picker_lifecycle_sources)

    snow_shot_import_offscreen_platform(snow-shot-floating-toolbar-drag-tests)
    add_test(NAME snow-shot-toolbar-stable-tool-frame-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --stable-tool-frame-only -platform offscreen)
    set_tests_properties(snow-shot-toolbar-stable-tool-frame-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE)
    add_test(NAME snow-shot-border-cursor-tool-restoration-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --border-cursor-only -platform offscreen)
    set_tests_properties(snow-shot-border-cursor-tool-restoration-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-interrupted-toolbar-drag-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --interrupted-drag-only -platform offscreen)
    set_tests_properties(snow-shot-interrupted-toolbar-drag-tests PROPERTIES LABELS "unit")
    if(NOT APPLE)
        add_test(NAME snow-shot-floating-toolbar-dpi-frame-reconcile-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --dpi-frame-reconcile-only)
        set_tests_properties(snow-shot-floating-toolbar-dpi-frame-reconcile-tests PROPERTIES
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    if(APPLE)
        foreach(_toolbar_dpr IN ITEMS 1 2)
            add_test(NAME snow-shot-macos-toolbar-logical-${_toolbar_dpr}x-tests
                COMMAND snow-shot-floating-toolbar-drag-tests --macos-logical-only)
            set_tests_properties(snow-shot-macos-toolbar-logical-${_toolbar_dpr}x-tests PROPERTIES
                LABELS "unit;macos" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
                ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=${_toolbar_dpr}")
        endforeach()
        add_test(NAME snow-shot-macos-toolbar-native-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --macos-logical-only -platform cocoa)
        add_test(NAME snow-shot-macos-toolbar-focus-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --keyboard-focus-only -platform cocoa)
        set_tests_properties(snow-shot-macos-toolbar-native-tests snow-shot-macos-toolbar-focus-tests
            PROPERTIES LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE
            RUN_SERIAL TRUE TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
    endif()
    if(WIN32)
        add_test(NAME snow-shot-floating-toolbar-capture-screen-switch-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --capture-screen-switch-only)
        set_tests_properties(snow-shot-floating-toolbar-capture-screen-switch-tests PROPERTIES
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            LABELS "interactive;windows"
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-floating-toolbar-keyboard-focus-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --keyboard-focus-only)
    set_tests_properties(snow-shot-floating-toolbar-keyboard-focus-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        add_test(NAME snow-shot-floating-toolbar-keyboard-focus-windows-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --keyboard-focus-only)
        set_tests_properties(snow-shot-floating-toolbar-keyboard-focus-windows-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-floating-toolbar-drag-tests
        COMMAND snow-shot-floating-toolbar-drag-tests)
    if(WIN32)
        add_test(NAME snow-shot-floating-toolbar-hardware-drag-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --hardware-drag-only)
        add_test(NAME snow-shot-floating-toolbar-reverse-drag-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --reverse-hardware-drag-only)
        add_test(NAME snow-shot-floating-toolbar-seam-straddle-drag-tests
            COMMAND snow-shot-floating-toolbar-drag-tests --seam-straddle-drag-only)
        set_tests_properties(
            snow-shot-floating-toolbar-hardware-drag-tests
            snow-shot-floating-toolbar-reverse-drag-tests
            snow-shot-floating-toolbar-seam-straddle-drag-tests
            PROPERTIES LABELS "interactive;windows" RUN_SERIAL TRUE SKIP_RETURN_CODE 77
            TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(
        NAME snow-shot-floating-toolbar-action-layout-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --action-layout-only
    )
    add_test(
        NAME snow-shot-floating-toolbar-recording-hit-test-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --recording-hit-test-only
    )
    add_test(
        NAME snow-shot-screenshot-selection-reset-command-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --selection-reset-only
    )
    set_tests_properties(snow-shot-screenshot-selection-reset-command-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(
        NAME snow-shot-screenshot-cancel-ordering-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --cancel-ordering-only -platform offscreen
    )
    set_tests_properties(snow-shot-screenshot-cancel-ordering-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(
        NAME snow-shot-jump-to-translation-page-toolbar-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --jump-to-translation-page-only
            -platform offscreen
    )
    set_tests_properties(snow-shot-jump-to-translation-page-toolbar-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-floating-toolbar-drag-tests PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-floating-toolbar-action-layout-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-floating-toolbar-recording-hit-test-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(NOT WIN32)
        set_tests_properties(snow-shot-floating-toolbar-drag-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        )
        set_tests_properties(snow-shot-floating-toolbar-action-layout-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        )
        set_tests_properties(snow-shot-floating-toolbar-recording-hit-test-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        )
    endif()

    add_executable(snow-shot-adqt-dpi-stable-tests
        tests/adqt_dpi_stable_tests.cpp
    )
    if(WIN32)
        target_link_libraries(snow-shot-adqt-dpi-stable-tests PRIVATE comctl32)
    endif()
    target_link_libraries(snow-shot-adqt-dpi-stable-tests PRIVATE
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        Qt6::Widgets
        Qt6::Svg
    )
    snow_shot_import_offscreen_platform(snow-shot-adqt-dpi-stable-tests)
    add_test(
        NAME snow-shot-adqt-dpi-stable-tests
        COMMAND snow-shot-adqt-dpi-stable-tests
    )
    add_test(NAME snow-shot-toolbar-control-scaling-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --dpi-scaling-only -platform offscreen)
    set_tests_properties(snow-shot-toolbar-control-scaling-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-adqt-dpi-stable-offscreen-tests
        COMMAND snow-shot-adqt-dpi-stable-tests -platform offscreen)
    set_tests_properties(snow-shot-adqt-dpi-stable-offscreen-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-floating-toolbar-dpi-transaction-offscreen-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --dpi-frame-reconcile-only -platform offscreen)
    set_tests_properties(snow-shot-floating-toolbar-dpi-transaction-offscreen-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        set_tests_properties(snow-shot-adqt-dpi-stable-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-shot-adqt-dpi-stable-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-generated-icon-pack-tests
        tests/generated_icon_pack_tests.cpp
        src/presentation/components/icons/snowshoticons.cpp
    )
    target_include_directories(snow-shot-generated-icon-pack-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-generated-icon-pack-tests PRIVATE
        ant_design_icons_qt
        Qt6::Widgets
        Qt6::Svg
    )
    if(WIN32 AND QT_FEATURE_static)
        # Exercise the release application's explicit plugin policy even
        # when the Debug preset enables Qt's default plugin imports.
        qt_import_plugins(snow-shot-generated-icon-pack-tests
            NO_DEFAULT INCLUDE ${_snow_shot_static_qt_plugins})
    endif()
    add_test(NAME snow-shot-border-fill-icon-tests
        COMMAND snow-shot-generated-icon-pack-tests --border-fill-only -platform offscreen)
    set_tests_properties(snow-shot-border-fill-icon-tests PROPERTIES TIMEOUT 30 LABELS unit)
    add_test(NAME snow-shot-reveal-icon-tests
        COMMAND snow-shot-generated-icon-pack-tests --reveal-only -platform offscreen)
    set_tests_properties(snow-shot-reveal-icon-tests PROPERTIES TIMEOUT 30 LABELS unit
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(
        NAME snow-shot-generated-icon-pack-tests
        COMMAND snow-shot-generated-icon-pack-tests
    )
    if(WIN32)
        set_tests_properties(snow-shot-generated-icon-pack-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-shot-generated-icon-pack-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-application-icon-tests
        tests/application_icon_tests.cpp
        src/presentation/components/icons/snowshoticons.cpp
    )
    if(WIN32)
        target_sources(snow-shot-application-icon-tests PRIVATE
            resources/app-icon.ico
            resources/windows-app-icon.rc
        )
    endif()
    target_include_directories(snow-shot-application-icon-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-application-icon-tests PRIVATE
        ant_design_icons_qt
        Qt6::Widgets
        Qt6::Svg
    )
    if(WIN32 AND QT_FEATURE_static)
        qt_import_plugins(snow-shot-application-icon-tests
            NO_DEFAULT INCLUDE ${_snow_shot_static_qt_plugins})
    endif()
    add_test(
        NAME snow-shot-application-icon-tests
        COMMAND snow-shot-application-icon-tests
    )
    set_tests_properties(snow-shot-application-icon-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-sidebar-widget-tests
        tests/sidebar_widget_tests.cpp
        include/snow_shot/presentation/components/sidebarwidget.h
        include/snow_shot/presentation/styles/themecolorscheme.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/sidebarwidget.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/styles/thememanager.cpp
    )
    target_include_directories(snow-shot-sidebar-widget-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-sidebar-widget-tests PRIVATE
        ant_design_qt
        snow_shot_settings_search
        Qt6::Widgets
    )
    add_test(
        NAME snow-shot-sidebar-widget-tests
        COMMAND snow-shot-sidebar-widget-tests
    )
    if(WIN32)
        set_tests_properties(snow-shot-sidebar-widget-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-shot-sidebar-widget-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-application-search-tests
        tests/application_search_tests.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../test-support/fonts/recording_test_fonts.qrc"
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/styles/thememanager.cpp
    )
    target_link_libraries(snow-shot-application-search-tests PRIVATE
        snow_shot_settings_search
    )
    snow_shot_import_offscreen_platform(snow-shot-application-search-tests)
    add_test(
        NAME snow-shot-application-search-tests
        COMMAND snow-shot-application-search-tests -platform offscreen
    )
    set_tests_properties(snow-shot-application-search-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit"
    )

    add_executable(snow-shot-main-content-header-tests
        tests/main_content_header_tests.cpp
        include/snow_shot/presentation/components/sidebarwidget.h
        src/presentation/components/sidebarwidget.cpp
        include/snow_shot/presentation/components/maincontentheaderwidget.h
        include/snow_shot/presentation/styles/themecolorscheme.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/maincontentheaderwidget.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/styles/thememanager.cpp
    )
    target_include_directories(snow-shot-main-content-header-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-main-content-header-tests PRIVATE
        ant_design_icons_qt
        ant_design_qt
        snow_shot_settings_search
        Qt6::Widgets
        snow_shot_storage
    )
    snow_shot_import_offscreen_platform(snow-shot-main-content-header-tests)
    add_test(
        NAME snow-shot-main-content-header-tests
        COMMAND snow-shot-main-content-header-tests
    )
    if(WIN32)
        set_tests_properties(snow-shot-main-content-header-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-shot-main-content-header-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-toolbar-main-panel-tests
        tests/screenshot_toolbar_main_panel_tests.cpp
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h
    )
    target_include_directories(snow-shot-toolbar-main-panel-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-toolbar-main-panel-tests PRIVATE
        ant_design_qt
        snow_draw_engine_qt_canvas
        Qt6::Widgets
        Qt6::Svg
        Qt6::Network
    )
    add_test(
        NAME snow-shot-toolbar-main-panel-tests
        COMMAND snow-shot-toolbar-main-panel-tests
    )
    set_tests_properties(snow-shot-toolbar-main-panel-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(NAME snow-shot-speech-bubble-toolbar-tests
        COMMAND snow-shot-toolbar-main-panel-tests --speech-bubble-only -platform offscreen)
    add_test(NAME snow-shot-recording-audio-icon-tests
        COMMAND snow-shot-toolbar-main-panel-tests --recording-audio-only -platform offscreen)
    set_tests_properties(snow-shot-recording-audio-icon-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-speech-bubble-toolbar-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-toolbar-skin-tests
        COMMAND snow-shot-toolbar-main-panel-tests --skin-only -platform offscreen)
    set_tests_properties(snow-shot-toolbar-skin-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(
        NAME snow-shot-toolbar-icon-centering-hidpi-tests
        COMMAND snow-shot-toolbar-main-panel-tests --icon-centering-only
    )
    set_tests_properties(snow-shot-toolbar-icon-centering-hidpi-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=2"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit;windows"
    )

    if(APPLE)
        add_executable(snow-shot-macos-window-capture-exclusion-tests
            tests/macos_window_capture_exclusion_tests.mm
            src/platform/macos/windowcaptureexclusion.mm)
        target_include_directories(snow-shot-macos-window-capture-exclusion-tests PRIVATE
            include src "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
        target_link_libraries(snow-shot-macos-window-capture-exclusion-tests PRIVATE
            Qt6::Widgets snow_capture_c "${SNOW_SHOT_APPKIT_FRAMEWORK}")
        add_test(NAME snow-shot-macos-window-capture-exclusion-tests
            COMMAND snow-shot-macos-window-capture-exclusion-tests -platform cocoa)
        add_test(NAME snow-shot-macos-window-capture-exclusion-pixels-tests
            COMMAND snow-shot-macos-window-capture-exclusion-tests -platform cocoa --pixels)
        set_tests_properties(snow-shot-macos-window-capture-exclusion-tests
            snow-shot-macos-window-capture-exclusion-pixels-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()

    add_executable(snow-shot-window-capture-exclusion-tests
        tests/window_capture_exclusion_tests.cpp
        src/presentation/capture/windowcaptureexclusion.h
        src/presentation/capture/windowinputtransparency.h
    )
    target_include_directories(snow-shot-window-capture-exclusion-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(snow-shot-window-capture-exclusion-tests PRIVATE Qt6::Widgets)
    add_test(
        NAME snow-shot-window-capture-exclusion-tests
        COMMAND snow-shot-window-capture-exclusion-tests
    )
    set_tests_properties(snow-shot-window-capture-exclusion-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-scrolling-capture-exclusion-tests
        tests/scrolling_capture_exclusion_tests.cpp
        src/presentation/capture/screenshotscrollingpipeline.cpp
        src/presentation/capture/screenshotscrollingnativesource.cpp)
    target_include_directories(snow-shot-scrolling-capture-exclusion-tests PRIVATE
        include src "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-stitch-images-c/include")
    target_link_libraries(snow-shot-scrolling-capture-exclusion-tests PRIVATE
        snow_canvas_image
        Qt6::Widgets snow_stitch_images_c snow_shot_diagnostics)
    foreach(_capture_test_function IN ITEMS stream_create_region stream_receive stream_stop
            stream_destroy stream_stats stream_set_target_fps stream_frame_info
            stream_frame_release last_error_message)
        target_compile_definitions(snow-shot-scrolling-capture-exclusion-tests PRIVATE
            "snow_capture_${_capture_test_function}=snow_test_capture_${_capture_test_function}")
    endforeach()
    snow_shot_import_offscreen_platform(snow-shot-scrolling-capture-exclusion-tests)
    add_test(NAME snow-shot-scrolling-capture-exclusion-tests
        COMMAND snow-shot-scrolling-capture-exclusion-tests -platform offscreen)
    set_tests_properties(snow-shot-scrolling-capture-exclusion-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    get_target_property(_recording_test_sources snow-shot-screenshot-tool-palette-tests SOURCES)
    list(FILTER _recording_test_sources EXCLUDE REGEX "tests/screenshot_tool_palette_tests.cpp$")
    # The shared settings libraries supply the icons, theme objects and Qt metaobjects.
    list(REMOVE_ITEM _recording_test_sources
        src/presentation/components/icons/snowshoticons.cpp
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp)
    add_executable(snow-shot-screen-recording-controller-tests
        ${_recording_test_sources}
        tests/screen_recording_controller_tests.cpp
        tests/screen_recording_toolbar_auto_hide_tests.cpp
        tests/recording_color_sampler_tests.cpp
        tests/recording_popup_exclusion_tests.cpp
        include/snow_shot/presentation/screenrecordingcontroller.h
        include/snow_shot/presentation/screenrecordingfolder.h
        include/snow_shot/presentation/screenrecordingareawindow.h
        include/snow_shot/presentation/screenrecordingtoolbarwindow.h
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        src/presentation/recording/recordingtrimtoolbar.cpp
        src/presentation/recording/recordingtrimsession.cpp
        src/presentation/recording/recordingrenderdialog.cpp
        src/presentation/recording/recordingrenderdialog.h
        src/presentation/recording/recordingrenderjob.cpp
        src/presentation/recording/recordingrenderjob.h
        src/presentation/recording/screenrecordingcontroller.cpp
        src/presentation/recording/recordingcolorsampler.cpp
        src/presentation/recording/recordingcolorsampler.h
        src/presentation/services/screenshotcanvascolorsampler.cpp
        src/presentation/services/screenshotcanvascolorsamplerwindow.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/recording/screenrecordingfolder.cpp
        src/presentation/recording/recordingeffectpreview.cpp
        src/presentation/recording/recordingeffectpreview.h
        src/presentation/recording/recordingeffectstyle.h
        include/snow_shot/presentation/screenrecordingshortcutcontroller.h
        src/presentation/recording/screenrecordingshortcutcontroller.cpp
        src/presentation/recording/screenrecordingareawindow.cpp
        src/presentation/recording/screenrecordingtoolbarwindow.cpp
        src/presentation/recording/screenrecordinggeometry.cpp
        src/presentation/recording/recordingcountdownoverlay.cpp
        src/presentation/recording/recordingcountdownoverlay.h
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/platform/windows/windowchrome.cpp
    )
    snow_shot_link_macos_window_platform(snow-shot-screen-recording-controller-tests)
    target_include_directories(snow-shot-screen-recording-controller-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-recording-c/include")
    target_link_libraries(snow-shot-screen-recording-controller-tests PRIVATE
        adqt_icon_core ant_design_icons_qt ant_design_qt snow_draw_engine_qt_canvas
        snow_shot_storage snow_shot_image_codec snow_shot_window_shortcuts snow_shot_settings Qt6::Widgets snow_recording_effects_c snow_capture_c)
    if(APPLE)
        target_sources(snow-shot-screen-recording-controller-tests PRIVATE
            src/platform/macos/windowcaptureexclusion.mm
            tests/macos_capture_exclusion_probe.mm
            tests/macos_recording_modal_probe.mm)
        target_link_libraries(snow-shot-screen-recording-controller-tests PRIVATE "${SNOW_SHOT_APPKIT_FRAMEWORK}")
    endif()
    target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE SNOW_SHOT_TEST_HOOKS=1)
    target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
        SNOW_SHOT_TEST_TRANSLATIONS_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(snow-shot-screen-recording-controller-tests snow_shot_release_translations)
    target_sources(snow-shot-screen-recording-controller-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../test-support/fonts/recording_test_fonts.qrc")
    # Override only the test executable: exercise the real controller without
    # opening capture devices, recording the desktop, or exporting media.
    target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
        screenshotWindowNamesAtPoint=test_recording_window_names_at_point)
    foreach(_operation create_direct create_deferred finalize_deferred destroy start pause resume request_stop stop state set_audio_gain set_audio_metering take_audio_levels request_exclusions exclusion_status)
        target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
            "snow_recording_session_${_operation}=test_recording_session_${_operation}")
    endforeach()
    foreach(_operation create cancel destroy set_gain set_metering take_levels)
        target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
            "snow_recording_audio_monitor_${_operation}=test_recording_audio_monitor_${_operation}")
    endforeach()
    foreach(_operation create configure set_active retry destroy error)
        target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
            "snow_webcam_${_operation}=test_recording_webcam_${_operation}")
    endforeach()
    foreach(_operation open destroy info boundary seek set_speed acquire frame_destroy error export_start export_start_with_speed export_poll export_error export_cancel export_destroy)
        target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
            "snow_recording_clip_${_operation}=test_recording_clip_${_operation}")
    endforeach()
    target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
        snow_recording_source_render_start=test_recording_source_render_start
        snow_recording_source_path=test_recording_source_path
        snow_recording_source_discard=test_recording_source_discard
        snow_recording_source_destroy=test_recording_source_destroy
        snow_recording_render_task_poll=test_recording_render_task_poll
        snow_recording_render_task_cancel=test_recording_render_task_cancel
        snow_recording_render_task_error=test_recording_render_task_error
        snow_recording_render_task_destroy=test_recording_render_task_destroy
        snow_recording_last_error_message=test_recording_last_error_message
        snow_recording_region_output_dimensions=test_recording_region_output_dimensions)
    if(WIN32)
        target_link_libraries(snow-shot-screen-recording-controller-tests PRIVATE user32 dwmapi gdi32)
        target_sources(snow-shot-screen-recording-controller-tests PRIVATE
            tests/screen_recording_toolbar_display_tests.cpp)
        add_test(NAME snow-shot-screen-recording-toolbar-ready-display-tests
            COMMAND snow-shot-screen-recording-controller-tests
                --native-toolbar-ready-display-only -platform windows)
        add_test(NAME snow-shot-recording-color-sampler-native-tests
            COMMAND snow-shot-screen-recording-controller-tests
                --color-sampling-native-only -platform windows)
        set_tests_properties(snow-shot-recording-color-sampler-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 30
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNOW_FFMPEG_ROOT}/bin")
        set_tests_properties(snow-shot-screen-recording-toolbar-ready-display-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 30 SKIP_RETURN_CODE 77
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-recording-color-sampler-tests
        COMMAND snow-shot-screen-recording-controller-tests --color-sampling-only -platform offscreen)
    set_tests_properties(snow-shot-recording-color-sampler-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-recording-tool-toggle-tests
        COMMAND snow-shot-screen-recording-controller-tests --tool-toggle-only -platform offscreen)
    set_tests_properties(snow-shot-recording-tool-toggle-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-recording-eraser-tests
        COMMAND snow-shot-screen-recording-controller-tests --eraser-only -platform offscreen)
    set_tests_properties(snow-shot-recording-eraser-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-recording-style-wheel-tests
        COMMAND snow-shot-screen-recording-controller-tests --style-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-recording-style-wheel-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-recording-trimming-tests
        COMMAND snow-shot-screen-recording-controller-tests --trim-only -platform offscreen)
    set_tests_properties(snow-shot-recording-trimming-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-recording-export-naming-tests
        COMMAND snow-shot-screen-recording-controller-tests --export-naming-only -platform offscreen)
    set_tests_properties(snow-shot-recording-export-naming-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    if(WIN32)
        add_test(NAME snow-shot-recording-trim-ui-tests
            COMMAND snow-shot-screen-recording-controller-tests --trim-ui-only -platform windows)
        set_tests_properties(snow-shot-recording-trim-ui-tests PROPERTIES
            LABELS "interactive;e2e;windows" TIMEOUT 60)
    endif()
    add_test(NAME snow-shot-recording-post-processing-tests
        COMMAND snow-shot-screen-recording-controller-tests --post-processing-only -platform offscreen)
    set_tests_properties(snow-shot-recording-post-processing-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-recording-render-layout-tests
        COMMAND snow-shot-screen-recording-controller-tests --render-layout-only -platform offscreen)
    set_tests_properties(snow-shot-recording-render-layout-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME snow-shot-recording-settings-dialog-tests
        COMMAND snow-shot-screen-recording-controller-tests --settings-dialog-only -platform offscreen)
    set_tests_properties(snow-shot-recording-settings-dialog-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-screen-recording-toolbar-auto-hide-tests
        COMMAND snow-shot-screen-recording-controller-tests --toolbar-auto-hide-only -platform offscreen)
    set_tests_properties(snow-shot-screen-recording-toolbar-auto-hide-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    if(APPLE)
        add_test(NAME snow-shot-macos-recording-toolbar-auto-hide-tests
            COMMAND snow-shot-screen-recording-controller-tests --toolbar-auto-hide-only -platform cocoa)
        set_tests_properties(snow-shot-macos-recording-toolbar-auto-hide-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    endif()
    add_test(NAME snow-shot-recording-api-mode-tests
        COMMAND snow-shot-screen-recording-controller-tests --api-mode-only -platform offscreen)
    set_tests_properties(snow-shot-recording-api-mode-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recording-area-history-tests
        COMMAND snow-shot-screen-recording-controller-tests --area-history-only -platform offscreen)
    set_tests_properties(snow-shot-recording-area-history-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recording-startup-retry-tests
        COMMAND snow-shot-screen-recording-controller-tests --startup-retry-only -platform offscreen)
    set_tests_properties(snow-shot-recording-startup-retry-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recording-modal-stacking-tests
        COMMAND snow-shot-screen-recording-controller-tests --modal-stacking-only -platform offscreen)
    set_tests_properties(snow-shot-recording-modal-stacking-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    if(APPLE)
        add_test(NAME snow-shot-macos-recording-modal-stacking-tests
            COMMAND snow-shot-screen-recording-controller-tests --modal-stacking-only -platform cocoa)
        set_tests_properties(snow-shot-macos-recording-modal-stacking-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    add_test(NAME snow-shot-screen-recording-controller-tests
        COMMAND snow-shot-screen-recording-controller-tests)
    add_test(NAME snow-shot-recording-finalization-close-tests
        COMMAND snow-shot-screen-recording-controller-tests --finalization-close-only -platform offscreen)
    set_tests_properties(snow-shot-recording-finalization-close-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-recording-delete-on-close-tests
        COMMAND snow-shot-screen-recording-controller-tests --delete-on-close-only -platform offscreen)
    set_tests_properties(snow-shot-recording-delete-on-close-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-recording-auto-exit-tests
        COMMAND snow-shot-screen-recording-controller-tests --auto-exit-only -platform offscreen)
    set_tests_properties(snow-shot-recording-auto-exit-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-recording-export-notification-tests
        COMMAND snow-shot-screen-recording-controller-tests --export-notifications-only -platform offscreen)
    set_tests_properties(snow-shot-recording-export-notification-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    if(WIN32)
        add_test(NAME snow-shot-recording-effects-preview-native-tests
            COMMAND snow-shot-screen-recording-controller-tests --effects-preview-native -platform windows)
        set_tests_properties(snow-shot-recording-effects-preview-native-tests PROPERTIES
            LABELS "interactive;e2e;windows" RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNOW_FFMPEG_ROOT}/bin")
    endif()
    add_test(NAME snow-shot-recording-effects-preview-tests
        COMMAND snow-shot-screen-recording-controller-tests --effects-preview-only)
    add_test(NAME snow-shot-recording-store-preservation-tests
        COMMAND snow-shot-screen-recording-controller-tests --store-preservation-only -platform offscreen)
    set_tests_properties(snow-shot-recording-store-preservation-tests PROPERTIES
        LABELS "unit;windows" TIMEOUT 60)
    set_tests_properties(snow-shot-recording-effects-preview-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    foreach(_preview_scale IN ITEMS 1 1.25 1.5 1.75 2)
        add_test(NAME snow-shot-recording-playback-preview-dpi-${_preview_scale}-tests
            COMMAND snow-shot-screen-recording-controller-tests --playback-preview-only)
        set_tests_properties(snow-shot-recording-playback-preview-dpi-${_preview_scale}-tests
            PROPERTIES
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=${_preview_scale}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    endforeach()
    add_test(NAME snow-shot-recording-webcam-preview-tests
        COMMAND snow-shot-screen-recording-controller-tests --webcam-preview-only)
    add_test(NAME snow-shot-recording-webcam-interaction-tests
        COMMAND snow-shot-screen-recording-area-window-tests --webcam-only -platform offscreen)
    set_tests_properties(snow-shot-recording-webcam-interaction-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-recording-webcam-settings-tests
        COMMAND snow-shot-screen-recording-controller-tests --webcam-settings-only)
    add_test(NAME snow-shot-recording-warning-placement-tests
        COMMAND snow-shot-screen-recording-controller-tests --warnings-only -platform offscreen)
    set_tests_properties(snow-shot-recording-webcam-preview-tests
        snow-shot-recording-webcam-settings-tests snow-shot-recording-warning-placement-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    if(APPLE)
        foreach(_preview_scale IN ITEMS 1 1.25 1.5 1.75 2)
            add_test(NAME snow-shot-recording-effects-preview-dpi-${_preview_scale}-tests
                COMMAND snow-shot-screen-recording-controller-tests --effects-preview-dpi-only)
            set_tests_properties(snow-shot-recording-effects-preview-dpi-${_preview_scale}-tests
                PROPERTIES
                ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=${_preview_scale}"
                LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
        endforeach()
    endif()
    add_test(NAME snow-shot-recording-capture-exclusion-tests
        COMMAND snow-shot-screen-recording-controller-tests --capture-exclusion-only)
    add_test(NAME snow-shot-recording-toolbar-ownership-tests
        COMMAND snow-shot-screen-recording-controller-tests --toolbar-ownership-only -platform offscreen)
    set_tests_properties(snow-shot-recording-toolbar-ownership-tests PROPERTIES
        LABELS "unit" TIMEOUT 30)
    if(WIN32)
        add_test(NAME snow-shot-recording-toolbar-exclusion-stacking-tests
            COMMAND snow-shot-screen-recording-controller-tests --toolbar-exclusion-stacking-only
                -platform windows)
        set_tests_properties(snow-shot-recording-toolbar-exclusion-stacking-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    set_tests_properties(snow-shot-recording-capture-exclusion-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60 ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    if(APPLE)
        add_test(NAME snow-shot-macos-recording-capture-exclusion-tests
            COMMAND snow-shot-screen-recording-controller-tests --capture-exclusion-only -platform cocoa)
        set_tests_properties(snow-shot-macos-recording-capture-exclusion-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-screen-recording-controller-tests)
    add_test(NAME snow-shot-recording-magnifier-availability-tests
        COMMAND snow-shot-screen-recording-controller-tests --magnifier-availability-only -platform offscreen)
    set_tests_properties(snow-shot-recording-magnifier-availability-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-screen-recording-controller-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        RUN_SERIAL TRUE TIMEOUT 60)

    add_executable(snow-shot-screenshot-recording-workflow-tests
        tests/screenshot_recording_workflow_tests.cpp
    )
    target_link_libraries(snow-shot-screenshot-recording-workflow-tests PRIVATE Qt6::Core)
    add_test(NAME snow-shot-screenshot-recording-workflow-tests
        COMMAND snow-shot-screenshot-recording-workflow-tests)
    set_tests_properties(snow-shot-screenshot-recording-workflow-tests PROPERTIES
        LABELS "unit" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(snow-shot-screenshot-recording-workflow-tests PROPERTIES
            ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;$ENV{PATH}")
    endif()

    add_executable(snow-shot-screen-recording-geometry-tests
        tests/screen_recording_geometry_tests.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/recording/screenrecordinggeometry.cpp
    )
    target_include_directories(snow-shot-screen-recording-geometry-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(snow-shot-screen-recording-geometry-tests PRIVATE
        Qt6::Widgets
    )
    add_test(
        NAME snow-shot-screen-recording-geometry-tests
        COMMAND snow-shot-screen-recording-geometry-tests
    )
    set_tests_properties(snow-shot-screen-recording-geometry-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=1.25"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-screen-recording-area-window-tests
        tests/screen_recording_area_window_tests.cpp
        include/snow_shot/presentation/screenrecordingareawindow.h
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/recording/screenrecordingareawindow.cpp
        src/presentation/recording/screenrecordinggeometry.cpp
        src/presentation/recording/recordingcountdownoverlay.cpp
        src/presentation/recording/recordingcountdownoverlay.h
    )
    snow_shot_link_macos_window_platform(snow-shot-screen-recording-area-window-tests)
    target_include_directories(snow-shot-screen-recording-area-window-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(snow-shot-screen-recording-area-window-tests PRIVATE
        snow_shot_window_shortcuts
        ant_design_qt adqt_icon_core
        Qt6::Widgets
        snow_draw_engine_qt_canvas
        snow_shot_storage
    )
    if(APPLE)
        target_sources(snow-shot-screen-recording-area-window-tests PRIVATE
            ../cmake/test-support/macos_native_input.mm)
        target_link_libraries(snow-shot-screen-recording-area-window-tests PRIVATE Qt6::GuiPrivate)
        foreach(_safe_area_scale IN ITEMS 1 2)
            add_test(NAME snow-shot-recording-safe-area-${_safe_area_scale}-tests
                COMMAND snow-shot-screen-recording-area-window-tests --safe-area-only -platform offscreen)
            set_tests_properties(snow-shot-recording-safe-area-${_safe_area_scale}-tests PROPERTIES
                LABELS "unit;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 30
                ENVIRONMENT "QT_SCALE_FACTOR=${_safe_area_scale}")
        endforeach()
        target_include_directories(snow-shot-screen-recording-area-window-tests PRIVATE
            ../cmake/test-support)
        add_test(NAME snow-shot-macos-recording-border-input-tests
            COMMAND snow-shot-screen-recording-area-window-tests --native-border-input-only -platform cocoa)
        set_tests_properties(snow-shot-macos-recording-border-input-tests PROPERTIES
            LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE
            TIMEOUT 60 SKIP_RETURN_CODE 77)
        add_test(NAME snow-shot-macos-recording-cross-display-drag-tests
            COMMAND snow-shot-screen-recording-area-window-tests --native-logical-drag-only -platform cocoa)
        set_tests_properties(snow-shot-macos-recording-cross-display-drag-tests PROPERTIES
            LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE
            TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
    if(WIN32)
        target_link_libraries(snow-shot-screen-recording-area-window-tests PRIVATE user32)
    endif()
    add_test(
        NAME snow-shot-screen-recording-area-window-tests
        COMMAND snow-shot-screen-recording-area-window-tests
    )
    if(WIN32)
        add_test(NAME snow-shot-screen-recording-area-native-tests
            COMMAND snow-shot-screen-recording-area-window-tests --native-geometry-only -platform windows)
        set_tests_properties(snow-shot-screen-recording-area-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 45)
        add_test(NAME snow-shot-recording-webcam-native-tests
            COMMAND snow-shot-screen-recording-area-window-tests --webcam-native-only -platform windows)
        set_tests_properties(snow-shot-recording-webcam-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 30 SKIP_RETURN_CODE 77)
    endif()
    set_tests_properties(snow-shot-screen-recording-area-window-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=1.25"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(NAME snow-shot-recording-angle-wheel-tests
        COMMAND snow-shot-screen-recording-area-window-tests --angle-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-recording-angle-wheel-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=1.25")

    add_executable(snow-shot-ocr-background-tests
        tests/screenshot_ocr_background_tests.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp)
    target_include_directories(snow-shot-ocr-background-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-ocr-background-tests PRIVATE Qt6::Gui snow_draw_engine_qt_canvas)
    add_test(NAME snow-shot-ocr-background-tests COMMAND snow-shot-ocr-background-tests)
    set_tests_properties(snow-shot-ocr-background-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-ocr-presentation-tests
        tests/screenshot_ocr_presentation_tests.cpp
        include/snow_shot/presentation/screenshotocrpresentation.h
        include/snow_shot/presentation/screenshotocrtexteditingsession.h
        include/snow_shot/presentation/screenshotocrtexttransform.h
        include/snow_shot/presentation/screenshottabledocument.h
        include/snow_shot/presentation/screenshotsourceimagecomposer.h
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtexteditingsession.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/ocr/screenshotocrtexttransform.cpp
        src/presentation/ocr/screenshotocrlayout.cpp
        src/presentation/services/screenshotsourceimagecomposer.cpp
    )
    target_include_directories(snow-shot-ocr-presentation-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-ocr-presentation-tests PRIVATE
        snow_canvas_image
        Qt6::Widgets
    )
    add_test(
        NAME snow-shot-ocr-presentation-tests
        COMMAND snow-shot-ocr-presentation-tests
    )
    set_tests_properties(snow-shot-ocr-presentation-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(WIN32)
        target_link_libraries(snow-shot-ocr-presentation-tests PRIVATE ole32 user32)
        add_test(NAME snow-shot-table-clipboard-native-tests
            COMMAND snow-shot-ocr-presentation-tests --native-table-clipboard -platform windows)
        set_tests_properties(snow-shot-table-clipboard-native-tests PROPERTIES
            LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    add_executable(snow-shot-recognition-window-tests
        tests/screenshot_recognition_window_tests.cpp
        src/presentation/ocr/screenshotrecognitionimage.cpp
        include/snow_shot/presentation/screenshotcanvasrenderer.h
        include/snow_shot/presentation/screenshotgeometry.h
        include/snow_shot/presentation/screenshotocrpresentation.h
        include/snow_shot/presentation/screenshotocrtextlayer.h
        include/snow_shot/presentation/screenshottabledocument.h
        include/snow_shot/presentation/screenshottableeditor.h
        include/snow_shot/presentation/screenshotrecognitionwindow.h
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtexttransform.cpp
        src/presentation/ocr/screenshotocrlayout.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/ocr/screenshottableeditor.cpp
        src/presentation/ocr/screenshotrecognitionwindow.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/styles/buttonborder.cpp
    )
    snow_shot_link_macos_window_platform(snow-shot-recognition-window-tests)
    target_compile_definitions(snow-shot-recognition-window-tests PRIVATE SNOW_SHOT_BENCH_INTERNALS=1)
    target_include_directories(snow-shot-recognition-window-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-recognition-window-tests PRIVATE
        ant_design_qt
        Qt6::Widgets
        snow_draw_engine_qt_canvas
        snow_shot_storage
        snow_shot_window_shortcuts
    )
    if(WIN32)
        target_link_libraries(snow-shot-recognition-window-tests PRIVATE
            user32
        )
    endif()
    snow_shot_import_offscreen_platform(snow-shot-recognition-window-tests)
    if(APPLE)
        set_source_files_properties(tests/screenshot_recognition_window_tests.cpp
            PROPERTIES LANGUAGE OBJCXX)
        target_link_libraries(snow-shot-recognition-window-tests PRIVATE "-framework AppKit")
    endif()
    add_test(NAME snow-shot-recognition-message-shadow-tests
        COMMAND snow-shot-recognition-window-tests --message-shadow-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-message-shadow-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-recognition-context-menu-tests
        COMMAND snow-shot-recognition-window-tests --context-menu-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-context-menu-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-recognition-double-click-tests
        COMMAND snow-shot-recognition-window-tests --ocr-double-click-only -platform offscreen)
    add_test(NAME snow-shot-recognition-box-selection-tests
        COMMAND snow-shot-recognition-window-tests --ocr-box-selection-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-box-selection-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-recognition-double-click-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recognition-selection-only-rendering-tests
        COMMAND snow-shot-recognition-window-tests --selection-only-rendering -platform offscreen)
    add_test(NAME snow-shot-recognition-ocr-highlighting-tests
        COMMAND snow-shot-recognition-window-tests --ocr-highlighting-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-ocr-highlighting-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recognition-ocr-hover-tests
        COMMAND snow-shot-recognition-window-tests --ocr-hover-only -platform offscreen)
    add_test(NAME snow-shot-recognition-text-editor-layout-tests
        COMMAND snow-shot-recognition-window-tests --text-editor-layout-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-text-editor-layout-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recognition-text-editor-history-tests
        COMMAND snow-shot-recognition-window-tests --text-editor-history-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-text-editor-history-tests PROPERTIES
        LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(
        NAME snow-shot-recognition-window-tests
        COMMAND snow-shot-recognition-window-tests -platform offscreen
    )
    set_tests_properties(snow-shot-recognition-window-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        add_test(NAME snow-shot-recognition-native-surface-tests
            COMMAND snow-shot-recognition-window-tests --native-surface-only -platform windows)
        set_tests_properties(snow-shot-recognition-native-surface-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    snow_shot_add_ocr_runtime_tests()

    add_executable(snow-shot-ocr-layout-tests
        tests/screenshot_ocr_layout_tests.cpp
        src/presentation/ocr/screenshotocrlayout.cpp
    )
    target_include_directories(snow-shot-ocr-layout-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-ocr-layout-tests PRIVATE Qt6::Gui)
    add_test(NAME snow-shot-ocr-layout-tests COMMAND snow-shot-ocr-layout-tests)
    set_tests_properties(snow-shot-ocr-layout-tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-recognition-session-controller-tests
        src/presentation/services/screenshotrecognitionfileexport.cpp
        src/presentation/ocr/screenshotrecognitionimage.cpp
        tests/screenshot_recognition_session_controller_tests.cpp
        tests/original_image_translation_tests.cpp
        tests/image_conversion_tests.cpp
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/screenshotocrpresentation.h
        include/snow_shot/presentation/screenshotrecognitionsessioncontroller.h
        include/snow_shot/presentation/screenshotrecognitionwindow.h
        include/snow_shot/presentation/screenshottabledocument.h
        include/snow_shot/presentation/screenshottableeditor.h
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtexteditingsession.cpp
        src/presentation/ocr/screenshotocrtexttransform.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
        src/presentation/ocr/screenshotrecognitionsessioncontroller.cpp
        src/presentation/ocr/screenshotocrlayout.cpp
        src/presentation/ocr/screenshotrecognitionwindow.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/ocr/screenshottableeditor.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/languagemanager.cpp
        src/presentation/styles/buttonborder.cpp
    )
    snow_shot_link_macos_window_platform(snow-shot-recognition-session-controller-tests)
    target_include_directories(snow-shot-recognition-session-controller-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_DRAW_ENGINE_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-recognition-session-controller-tests PRIVATE
        MINIZIP::minizip-ng
        snow_shot_translation
        ant_design_qt
        Qt6::Network
        Qt6::Widgets
        snow_draw_engine_qt_canvas
        snow_shot_image_codec
        snow_shot_storage
        snow_shot_window_shortcuts
    )
    if(WIN32)
        target_link_libraries(snow-shot-recognition-session-controller-tests PRIVATE
            user32
        )
    endif()
    add_test(
        NAME snow-shot-recognition-session-controller-tests
        COMMAND snow-shot-recognition-session-controller-tests
    )
    set_tests_properties(snow-shot-recognition-session-controller-tests PROPERTIES
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(WIN32)
        set_tests_properties(snow-shot-recognition-session-controller-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
        )
    else()
        set_tests_properties(snow-shot-recognition-session-controller-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        )
    endif()

    foreach(_original_image_test IN ITEMS screenshot-tool-palette recognition-window recognition-session-controller)
        add_test(NAME snow-shot-${_original_image_test}-original-image-tests
            COMMAND snow-shot-${_original_image_test}-tests --original-image-only -platform offscreen)
        set_tests_properties(snow-shot-${_original_image_test}-original-image-tests
            PROPERTIES LABELS "unit" TIMEOUT 90)
    endforeach()

    add_test(NAME snow-shot-recognition-defaults-tests
        COMMAND snow-shot-recognition-session-controller-tests --ocr-defaults-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-defaults-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_test(NAME snow-shot-recognition-image-snapshot-tests
        COMMAND snow-shot-recognition-window-tests --image-snapshot-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-image-snapshot-tests PROPERTIES LABELS "unit" TIMEOUT 60)

    add_test(NAME snow-shot-original-image-translation-tests
        COMMAND snow-shot-recognition-session-controller-tests --translation-only)
    set_tests_properties(snow-shot-original-image-translation-tests PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(NAME snow-shot-recognition-notification-lifetime-tests
        COMMAND snow-shot-recognition-session-controller-tests --notification-lifetime-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-notification-lifetime-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-image-conversion-tests
        COMMAND snow-shot-recognition-session-controller-tests --image-conversion-only -platform offscreen)
    set_tests_properties(snow-shot-image-conversion-tests PROPERTIES TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-image-conversion-network-tests
        COMMAND snow-shot-recognition-session-controller-tests --image-conversion-network-only -platform offscreen)
    set_tests_properties(snow-shot-image-conversion-network-tests PROPERTIES TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-image-conversion-clipboard-tests
        COMMAND snow-shot-recognition-session-controller-tests --image-conversion-clipboard-only -platform offscreen)
    set_tests_properties(snow-shot-image-conversion-clipboard-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-image-conversion-storage-tests
        COMMAND snow-shot-application-storage-tests --image-conversion-only)
    set_tests_properties(snow-shot-image-conversion-storage-tests PROPERTIES TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-image-conversion-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --image-conversion-only -platform offscreen)
    set_tests_properties(snow-shot-image-conversion-toolbar-tests PROPERTIES TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-image-conversion-pinned-tests
        COMMAND snow-shot-pinned-window-tests --image-conversion-only)
    set_tests_properties(snow-shot-image-conversion-pinned-tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=windows"
        LABELS "unit;windows"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-table-recognition-session-tests
        COMMAND snow-shot-recognition-session-controller-tests --table-only -platform offscreen)
    set_tests_properties(snow-shot-table-recognition-session-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    snow_shot_import_offscreen_platform(snow-shot-recognition-session-controller-tests)
    get_target_property(_translation_tool_sources snow-shot-color-picker-lifecycle-tests SOURCES)
    get_target_property(_translation_session_sources snow-shot-recognition-session-controller-tests SOURCES)
    list(APPEND _translation_tool_sources ${_translation_session_sources})
    list(FILTER _translation_tool_sources EXCLUDE REGEX "^tests/")
    list(REMOVE_DUPLICATES _translation_tool_sources)
    add_executable(snow-shot-translation-tool-tests ${_translation_tool_sources}
        tests/screenshot_translation_tool_tests.cpp
        include/snow_shot/presentation/screenshotocrcontroller.h
        src/presentation/ocr/screenshotocrcontroller.cpp)
    target_include_directories(snow-shot-translation-tool-tests PRIVATE include src/image)
    target_link_libraries(snow-shot-translation-tool-tests PRIVATE snow_shot_translation
        snow_shot_diagnostics snow_shot_storage snow_shot_image_codec snow_shot_window_shortcuts
        snow_draw_engine_qt_canvas ant_design_qt adqt_icon_core ant_design_icons_qt
        Qt6::Widgets Qt6::Network Qt6::GuiPrivate)
    snow_shot_link_macos_window_platform(snow-shot-translation-tool-tests)
    if(WIN32)
        target_link_libraries(snow-shot-translation-tool-tests PRIVATE user32 comctl32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-translation-tool-tests)
    add_test(NAME snow-shot-translation-tool-tests
        COMMAND snow-shot-translation-tool-tests -platform offscreen)
    set_tests_properties(snow-shot-translation-tool-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    unset(_translation_tool_sources)
    unset(_translation_session_sources)
    get_target_property(_snow_mcp_document_sources snow-shot-recognition-session-controller-tests SOURCES)
    list(REMOVE_ITEM _snow_mcp_document_sources
        tests/screenshot_recognition_session_controller_tests.cpp
        tests/original_image_translation_tests.cpp tests/image_conversion_tests.cpp
        src/presentation/styles/buttonborder.cpp)
    get_target_property(_snow_mcp_transport_sources snow-shot-mcp-tests SOURCES)
    list(REMOVE_ITEM _snow_mcp_transport_sources tests/screenshot_mcp_tests.cpp)
    add_executable(snow-shot-mcp-screenshot-fixture
        ${_snow_mcp_transport_sources} tests/mcp_screenshot_fixture.cpp)
    target_include_directories(snow-shot-mcp-screenshot-fixture PRIVATE include src/image)
    target_link_libraries(snow-shot-mcp-screenshot-fixture PRIVATE snow_shot_diagnostics
        snow_shot_image_codec snow_shot_storage snow_draw_engine_qt_canvas ant_design_qt Qt6::Widgets Qt6::Network)
    if(WIN32)
        target_link_libraries(snow-shot-mcp-screenshot-fixture PRIVATE user32 advapi32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-mcp-screenshot-fixture)
    list(APPEND _snow_mcp_document_sources ${_snow_mcp_transport_sources})
    list(REMOVE_DUPLICATES _snow_mcp_document_sources)
    add_executable(snow-shot-mcp-document-tests ${_snow_mcp_document_sources}
        tests/mcp_document_tests.cpp
        tests/mcp_application_tests.cpp
        include/snow_shot/app/mcp/mcpapplicationservice.h
        src/app/mcp/mcpapplicationservice.cpp
        include/snow_shot/app/mcp/mcpdocumentservice.h
        include/snow_shot/app/mcp/mcpjobregistry.h
        src/app/mcp/mcpdocumentservice.cpp src/app/mcp/mcpjobregistry.cpp)
    snow_shot_link_macos_window_platform(snow-shot-mcp-document-tests)
    target_include_directories(snow-shot-mcp-document-tests PRIVATE include src/image)
    target_link_libraries(snow-shot-mcp-document-tests PRIVATE snow_shot_translation
        snow_shot_settings snow_shot_updates
        snow_shot_diagnostics snow_shot_storage snow_shot_image_codec snow_shot_window_shortcuts
        snow_draw_engine_qt_canvas ant_design_qt Qt6::Widgets Qt6::Network)
    if(WIN32)
        target_link_libraries(snow-shot-mcp-document-tests PRIVATE user32 advapi32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-mcp-document-tests)
    add_test(NAME snow-shot-mcp-storage-guard-tests COMMAND snow-shot-mcp-document-tests --jobs-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-storage-guard-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-mcp-document-tests COMMAND snow-shot-mcp-document-tests -platform offscreen)
    add_test(NAME snow-shot-mcp-document-selection-style-tests
        COMMAND snow-shot-mcp-document-tests --selection-style-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-document-selection-style-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME snow-shot-mcp-memory-activity-tests
        COMMAND snow-shot-mcp-document-tests --activity-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-memory-activity-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-mcp-recognition-model-tests
        COMMAND snow-shot-mcp-document-tests --recognition-models-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-recognition-model-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-mcp-eraser-tests COMMAND snow-shot-mcp-document-tests --eraser-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-eraser-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-mcp-application-tests
        COMMAND snow-shot-mcp-document-tests --application-only -platform offscreen)
    add_test(NAME snow-shot-mcp-application-replay-tests
        COMMAND snow-shot-mcp-document-tests --application-replay-only -platform offscreen)
    add_test(NAME snow-shot-mcp-document-replay-tests
        COMMAND snow-shot-mcp-document-tests --replay-cache-only -platform offscreen)
    add_test(NAME snow-shot-mcp-document-render-cache-tests
        COMMAND snow-shot-mcp-document-tests --render-cache-cleanup-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-application-replay-tests snow-shot-mcp-document-replay-tests
        snow-shot-mcp-document-render-cache-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recording-auto-hide-mcp-settings-tests
        COMMAND snow-shot-mcp-document-tests --application-only --auto-hide-settings-only -platform offscreen)
    set_tests_properties(snow-shot-recording-auto-hide-mcp-settings-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    add_test(NAME snow-shot-mcp-translation-tests
        COMMAND snow-shot-mcp-document-tests --translation-only -platform offscreen)
    set_tests_properties(snow-shot-mcp-translation-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(SNOW_SHOT_ENABLE_MCP)
        add_test(NAME snow-shot-mcp-application-lifecycle-tests
            COMMAND snow_shot --mcp-lifecycle-tests)
        set_tests_properties(snow-shot-mcp-application-lifecycle-tests PROPERTIES
            LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 30
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    endif()
    if(Python3_Interpreter_FOUND AND SNOW_SHOT_ENABLE_MCP)
        add_test(NAME snow-shot-mcp-fixture-tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_fixture_tests.py"
                "$<TARGET_FILE:snow_shot_mcp>" "$<TARGET_FILE:snow-shot-mcp-document-tests>"
                --screenshot-fixture "$<TARGET_FILE:snow-shot-mcp-screenshot-fixture>" --samples 1)
        set_tests_properties(snow-shot-mcp-fixture-tests PROPERTIES TIMEOUT 120 LABELS unit
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        add_test(NAME snow-shot-mcp-application-fixture-tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_application_fixture_tests.py"
                "$<TARGET_FILE:snow_shot_mcp>" "$<TARGET_FILE:snow_shot>")
        set_tests_properties(snow-shot-mcp-application-fixture-tests PROPERTIES TIMEOUT 120 LABELS unit
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-mcp-history-transient-tests
        COMMAND snow-shot-screenshot-history-tests --mcp-transient-only -platform offscreen)
    add_test(NAME snow-shot-mcp-recording-tests COMMAND snow-shot-screen-recording-controller-tests --automation-only)
    add_test(NAME snow-shot-mcp-pinned-tests COMMAND snow-shot-pinned-window-tests --automation-only)
    set_tests_properties(snow-shot-mcp-document-tests snow-shot-mcp-application-tests snow-shot-mcp-recording-tests
        snow-shot-mcp-pinned-tests snow-shot-mcp-history-transient-tests PROPERTIES TIMEOUT 120 LABELS unit
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    if(SNOW_SHOT_BUILD_BENCHMARKS)
        if(SNOW_SHOT_ENABLE_MCP AND Python3_Interpreter_FOUND)
            add_custom_target(snow-shot-mcp-performance-benchmark
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_fixture_tests.py"
                    "$<TARGET_FILE:snow_shot_mcp>" "$<TARGET_FILE:snow-shot-mcp-document-tests>"
                    --benchmark --samples 50 --screenshot-fixture "$<TARGET_FILE:snow-shot-mcp-screenshot-fixture>"
                    --output "${CMAKE_CURRENT_BINARY_DIR}/mcp-performance.json"
                DEPENDS snow_shot_mcp snow-shot-mcp-document-tests snow-shot-mcp-screenshot-fixture
                USES_TERMINAL)
        endif()
    endif()

    add_executable(snow-shot-qr-dnn-model-tests tests/qr_dnn_model_tests.cpp)
    target_link_libraries(snow-shot-qr-dnn-model-tests PRIVATE
        opencv_wechat_qrcode opencv_objdetect)
    set(_snow_qr_model_test_arguments "${SNOW_SHOT_QR_MODELS_DIR}")
    if(OpenCV_SNOW_SHOT_QR_PROFILE)
        list(APPEND _snow_qr_model_test_arguments --minimal-registry)
    endif()
    add_test(NAME snow-shot-qr-dnn-model-tests
        COMMAND snow-shot-qr-dnn-model-tests ${_snow_qr_model_test_arguments})
    unset(_snow_qr_model_test_arguments)
    set_tests_properties(snow-shot-qr-dnn-model-tests PROPERTIES LABELS unit TIMEOUT 60)

    add_executable(snow-shot-qr-recognition-service-tests
        tests/screenshot_qr_recognition_service_tests.cpp
        include/snow_shot/presentation/screenshotqrrecognitionservice.h
        src/presentation/ocr/screenshotqrrecognitionservice.cpp
    )
    target_include_directories(snow-shot-qr-recognition-service-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-qr-recognition-service-tests PRIVATE
        snow_canvas_image
        opencv_wechat_qrcode
        opencv_objdetect
        Qt6::Core
        Qt6::Gui
    )
    if(APPLE)
        target_link_libraries(snow-shot-qr-recognition-service-tests PRIVATE snow_shot_rust_ffi_selector)
    endif()
    add_test(
        NAME snow-shot-qr-recognition-service-tests
        COMMAND snow-shot-qr-recognition-service-tests
    )
    set_tests_properties(snow-shot-qr-recognition-service-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-selected-text-translation-tests
        tests/selected_text_translation_tests.cpp)
    target_link_libraries(snow-shot-selected-text-translation-tests PRIVATE snow_shot_translation)
    add_test(NAME snow-shot-selected-text-translation-tests
        COMMAND snow-shot-selected-text-translation-tests)
    set_tests_properties(snow-shot-selected-text-translation-tests PROPERTIES LABELS "unit" TIMEOUT 30)

    # Compile the production backend against a fake C boundary; do not link the Rust runtime.
    add_executable(snow-shot-selected-text-strategy-tests
        tests/selected_text_strategy_tests.cpp
        src/presentation/services/selectedtexttranslationcontroller.cpp
        include/snow_shot/presentation/selectedtexttranslationcontroller.h)
    target_include_directories(snow-shot-selected-text-strategy-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-selected-text-c/include")
    target_link_libraries(snow-shot-selected-text-strategy-tests PRIVATE Qt6::Core)
    add_test(NAME snow-shot-selected-text-strategy-tests
        COMMAND snow-shot-selected-text-strategy-tests)
    set_tests_properties(snow-shot-selected-text-strategy-tests PROPERTIES LABELS "unit" TIMEOUT 30)

    add_executable(snow-shot-translation-page-controller-tests
        tests/translation_page_controller_tests.cpp
        tests/translation_test_support.h
    )
    target_link_libraries(snow-shot-translation-page-controller-tests PRIVATE snow_shot_translation)
    add_test(NAME snow-shot-translation-page-controller-tests
             COMMAND snow-shot-translation-page-controller-tests)
    set_tests_properties(snow-shot-translation-page-controller-tests PROPERTIES LABELS "unit")

    add_executable(snow-shot-translation-service-tests tests/translation_service_tests.cpp)
    target_link_libraries(snow-shot-translation-service-tests PRIVATE snow_shot_translation)
    add_test(NAME snow-shot-translation-service-tests COMMAND snow-shot-translation-service-tests)
    set_tests_properties(snow-shot-translation-service-tests PROPERTIES LABELS "unit")
    add_executable(snow-shot-translation-language-detection-tests
        tests/translation_language_detection_tests.cpp)
    target_link_libraries(snow-shot-translation-language-detection-tests PRIVATE snow_shot_translation)
    add_test(NAME snow-shot-translation-language-detection-tests
        COMMAND snow-shot-translation-language-detection-tests)
    set_tests_properties(snow-shot-translation-language-detection-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-translation-notification-lifetime-tests
        COMMAND snow-shot-translation-service-tests --notification-lifetime-only)
    set_tests_properties(snow-shot-translation-notification-lifetime-tests PROPERTIES
        LABELS "unit" TIMEOUT 30)

    get_target_property(_translation_window_sources snow-shot-about-page-tests SOURCES)
    list(REMOVE_ITEM _translation_window_sources tests/about_page_tests.cpp)
    add_executable(snow-shot-translation-page-tests
        tests/translation_page_tests.cpp
        src/presentation/components/screenshottranslationsettingsdialog.cpp
        tests/translation_test_support.h
        ${_translation_window_sources}
    )
    target_link_libraries(snow-shot-translation-page-tests PRIVATE snow_shot_settings)
    target_compile_definitions(snow-shot-translation-page-tests PRIVATE
        SNOW_SHOT_TEST_TRANSLATIONS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_dependencies(snow-shot-translation-page-tests snow_shot_release_translations)
    snow_shot_import_offscreen_platform(snow-shot-translation-page-tests)
    add_test(NAME snow-shot-translation-page-tests COMMAND snow-shot-translation-page-tests)
    add_test(NAME snow-shot-screenshot-translation-settings-dialog-tests
        COMMAND snow-shot-translation-page-tests --screenshot-settings)
    set_tests_properties(snow-shot-screenshot-translation-settings-dialog-tests PROPERTIES
        LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-translation-page-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
    )

    add_executable(snow-shot-standalone-translation-tests
        tests/standalone_translation_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-standalone-translation-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    if(WIN32)
        target_link_libraries(snow-shot-standalone-translation-tests PRIVATE PkgConfig::SNOW_SHOT_FFMPEG)
    endif()
    target_compile_definitions(snow-shot-standalone-translation-tests PRIVATE
        SNOW_SHOT_TEST_TRANSLATIONS_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(snow-shot-standalone-translation-tests snow_shot_release_translations)
    snow_shot_import_offscreen_platform(snow-shot-standalone-translation-tests)
    add_test(NAME snow-shot-standalone-translation-tests COMMAND snow-shot-standalone-translation-tests)
    set_tests_properties(snow-shot-standalone-translation-tests PROPERTIES
        LABELS "unit" ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")

    add_executable(snow-shot-translation-page-streaming-tests
        tests/translation_page_streaming_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-translation-page-streaming-tests PRIVATE
        snow_shot_settings snow_shot_image_codec)
    if(WIN32)
        target_link_libraries(snow-shot-translation-page-streaming-tests PRIVATE
            PkgConfig::SNOW_SHOT_FFMPEG)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-translation-page-streaming-tests)
    add_test(NAME snow-shot-translation-page-streaming-tests
             COMMAND snow-shot-translation-page-streaming-tests)
    set_tests_properties(snow-shot-translation-page-streaming-tests PROPERTIES
        LABELS "unit" ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")

    if(SNOW_APPS_BUILD_BENCHMARKS)
        add_executable(snow-shot-table-preparation-benchmark
            tests/table_preparation_performance_benchmark.cpp)
        target_include_directories(snow-shot-table-preparation-benchmark PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-table-preparation-benchmark PRIVATE
            snow_shot_translation snow_shot_diagnostics Qt6::Core Qt6::Gui Qt6::Network
            snow_shot_image_codec)
    endif()

    add_executable(snow-shot-api-client-tests
        tests/snowshot_api_client_tests.cpp
    )
    target_include_directories(snow-shot-api-client-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/network"
    )
    target_link_libraries(snow-shot-api-client-tests PRIVATE
        snow_shot_translation
        snow_shot_diagnostics
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        snow_shot_image_codec
    )
    if(SNOW_SHOT_ENABLE_AGGRESSIVE_RELEASE_OPTIMIZATION OR SNOW_SHOT_RELEASE_STATIC)
        snow_apply_release_options(snow-shot-api-client-tests POLICY speed)
    endif()
    add_test(
        NAME snow-shot-api-client-tests
        COMMAND snow-shot-api-client-tests
    )
    add_test(NAME snow-shot-api-client-lifetime-tests
        COMMAND snow-shot-api-client-tests --lifetime-only)
    set_tests_properties(snow-shot-api-client-lifetime-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME snow-shot-api-client-stream-lifetime-tests
        COMMAND snow-shot-api-client-tests --stream-lifetime-only)
    set_tests_properties(snow-shot-api-client-stream-lifetime-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    set_tests_properties(snow-shot-api-client-tests snow-shot-api-client-lifetime-tests
        snow-shot-api-client-stream-lifetime-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    if(WIN32)
        # This UIA end-to-end driver is Qt-free; give it the Qt-free dialog
        # guard so the guard does not add a Qt runtime dependency.
        set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_native)
        add_executable(snow-shot-scrolling-thumbnail-uia-e2e-test
            tests/scrolling_thumbnail_uia_e2e_test.cpp
        )
        set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_qt)
        target_include_directories(snow-shot-scrolling-thumbnail-uia-e2e-test PRIVATE
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-stitch-images-c/include"
        )
        target_link_libraries(snow-shot-scrolling-thumbnail-uia-e2e-test PRIVATE
            snow_stitch_images_c
            Ole32
            Uiautomationcore
            Gdi32
            User32
            Userenv
            Ntdll
            Ws2_32
        )
        add_dependencies(snow-shot-scrolling-thumbnail-uia-e2e-test snow_shot)
        add_test(
            NAME snow-shot-scrolling-thumbnail-uia-e2e-test
            COMMAND snow-shot-scrolling-thumbnail-uia-e2e-test $<TARGET_FILE:snow_shot>
        )
        add_test(
            NAME snow-shot-scrolling-thumbnail-capture-excluded-uia-e2e-test
            COMMAND snow-shot-scrolling-thumbnail-uia-e2e-test
                $<TARGET_FILE:snow_shot>
                --capture-excluded
        )
        set_tests_properties(snow-shot-scrolling-thumbnail-uia-e2e-test PROPERTIES
            LABELS "e2e;uia;interactive"
            RUN_SERIAL TRUE
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        )
        set_tests_properties(snow-shot-scrolling-thumbnail-capture-excluded-uia-e2e-test PROPERTIES
            LABELS "e2e;uia;interactive;windows"
            RUN_SERIAL TRUE
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        )

        # This UIA end-to-end driver is Qt-free; give it the Qt-free dialog
        # guard so the guard does not add a Qt runtime dependency.
        set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_native)
        add_executable(snow-shot-screenshot-ctrl-c-uia-e2e-test
            tests/screenshot_ctrl_c_uia_e2e_test.cpp
        )
        set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_qt)
        target_link_libraries(snow-shot-screenshot-ctrl-c-uia-e2e-test PRIVATE
            Ole32
            Uiautomationcore
            User32
        )
        add_dependencies(snow-shot-screenshot-ctrl-c-uia-e2e-test snow_shot)
        add_test(
            NAME snow-shot-screenshot-ctrl-c-uia-e2e-test
            COMMAND snow-shot-screenshot-ctrl-c-uia-e2e-test $<TARGET_FILE:snow_shot>
        )
        set_tests_properties(snow-shot-screenshot-ctrl-c-uia-e2e-test PROPERTIES
            LABELS "e2e;uia;interactive"
            RUN_SERIAL TRUE
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        )
    endif()

    foreach(_offscreen_qpa_test_target IN ITEMS
        snow-shot-floating-toolbar-drag-tests
        snow-shot-language-manager-tests
        snow-shot-shortcut-key-row-tests
        snow-shot-form-layout-tests
        snow-shot-selection-resize-workflow-tests
        snow-shot-screenshot-canvas-renderer-tests
        snow-shot-screenshot-tool-palette-tests
        snow-shot-application-icon-tests
        snow-shot-toolbar-main-panel-tests
        snow-shot-screen-recording-geometry-tests
        snow-shot-screen-recording-area-window-tests
        snow-shot-screen-recording-shortcut-tests
        snow-shot-window-capture-exclusion-tests
        snow-shot-ocr-presentation-tests
    )
        snow_shot_import_offscreen_platform(${_offscreen_qpa_test_target})
    endforeach()
    if(NOT WIN32)
        foreach(_offscreen_qpa_test_target IN ITEMS
            snow-shot-system-tray-controller-tests
            snow-shot-context-menu-rendering-tests
            snow-shot-screenshot-selection-toolbar-hover-tests
            snow-shot-floating-toolbar-drag-tests
            snow-shot-adqt-dpi-stable-tests
            snow-shot-generated-icon-pack-tests
            snow-shot-sidebar-widget-tests
            snow-shot-main-content-header-tests
            snow-shot-recognition-window-tests
            snow-shot-recognition-session-controller-tests
            snow-shot-storage-status-widget-tests
            snow-shot-configuration-transfer-widget-tests
        )
            snow_shot_import_offscreen_platform(${_offscreen_qpa_test_target})
        endforeach()
    endif()

    foreach(_storage_test_target IN ITEMS
        snow-shot-language-manager-tests
        snow-shot-system-tray-controller-tests
        snow-shot-shortcut-key-row-tests
        snow-shot-selection-resize-workflow-tests
        snow-shot-screenshot-history-tests
        snow-shot-pinned-window-tests
        snow-shot-screenshot-tool-palette-tests
        snow-shot-screenshot-toolbar-fill-color-hover-tests
        snow-shot-screenshot-toolbar-render-benchmark
        snow-shot-floating-toolbar-drag-tests
        snow-shot-sidebar-widget-tests
        snow-shot-toolbar-main-panel-tests
    )
        if(TARGET ${_storage_test_target})
            target_link_libraries(${_storage_test_target} PRIVATE snow_shot_storage)
        endif()
    endforeach()

    if(_SNOW_SHOT_RUNTIME_OUTPUT_DIRECTORY_WAS_DEFINED)
        set(
            CMAKE_RUNTIME_OUTPUT_DIRECTORY
            "${_SNOW_SHOT_SAVED_RUNTIME_OUTPUT_DIRECTORY}"
        )
    else()
        unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    endif()
    unset(_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT)
    unset(_offscreen_qpa_test_target)
    unset(_SNOW_SHOT_SAVED_RUNTIME_OUTPUT_DIRECTORY)
    unset(_SNOW_SHOT_RUNTIME_OUTPUT_DIRECTORY_WAS_DEFINED)

endif()

function(snow_shot_configure_scrolling_replay_target target)
    target_sources(${target} PRIVATE
        include/snow_shot/presentation/screenshotscrollingthumbnailwidget.h
        tests/scrolling_image_replay.cpp
        src/presentation/capture/screenshotscrollingpipeline.cpp
        src/presentation/capture/screenshotscrollingthumbnailwidget.cpp
    )
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-stitch-images-c/include"
    )
    target_compile_definitions(${target} PRIVATE
        SNOW_SHOT_BENCH_INTERNALS=1
        SNOW_SHOT_SCROLLING_PERF_INSTRUMENTATION=1
        SNOW_SCROLLING_DEFAULT_IMAGE="${CMAKE_CURRENT_SOURCE_DIR}/test-imgs/scrollscreenshot-test.png"
        SNOW_SCROLLING_DEFAULT_OUTPUT="${CMAKE_BINARY_DIR}/scrolling-image-performance"
        SNOW_SCROLLING_BUILD_CONFIG="$<CONFIG>"
    )
    if(SNOW_SHOT_SCROLLING_PERF_DETAIL)
        target_compile_definitions(${target} PRIVATE
            SNOW_SHOT_SCROLLING_PERF_DETAIL=1 SNOW_STITCH_PERF_INSTRUMENTATION=1)
    endif()
    target_link_libraries(${target} PRIVATE
        snow_stitch_images_c snow_shot_image_codec snow_shot_diagnostics ant_design_qt Qt6::Widgets)
    if(WIN32)
        target_link_libraries(${target} PRIVATE Ntdll Ws2_32)
        target_link_libraries(${target} PRIVATE PkgConfig::SNOW_SHOT_FFMPEG)
    endif()
    snow_shot_import_offscreen_platform(${target})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-bin")
endfunction()

if(APPLE AND SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-permission-guide-benchmark MACOSX_BUNDLE
        tests/macos_permission_guide_tests.mm
        src/presentation/services/screenshotclipboardservice.cpp)
    target_compile_definitions(snow-shot-permission-guide-benchmark PRIVATE
        SNOW_PERMISSION_GUIDE_BENCHMARK=1)
    target_link_libraries(snow-shot-permission-guide-benchmark PRIVATE
        snow_shot_settings "-framework AppKit")
    set_target_properties(snow-shot-permission-guide-benchmark PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.snowshot.tests.permission-guide-benchmark"
        MACOSX_BUNDLE_BUNDLE_NAME "Snow Shot Permission Guide Benchmark")
endif()

if(SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-window-group-switcher-benchmark
        tests/window_group_switcher_benchmark.cpp
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
        include/snow_shot/presentation/windowgroupswitcherpopup.h
        src/presentation/components/windowgroupswitcherpopup.cpp)
    target_include_directories(snow-shot-window-group-switcher-benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-window-group-switcher-benchmark PRIVATE snow_shot_storage ant_design_qt Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-window-group-switcher-benchmark)
    add_test(NAME snow-shot-window-group-switcher-benchmark COMMAND snow-shot-window-group-switcher-benchmark)
    set_tests_properties(snow-shot-window-group-switcher-benchmark PROPERTIES
        LABELS benchmark ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
endif()

if(SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-settings-page-performance-benchmark
        tests/settings_page_performance_benchmark.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-settings-page-performance-benchmark PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-settings-page-performance-benchmark)

    add_executable(snow-shot-main-page-performance-benchmark
        tests/main_page_performance_benchmark.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-main-page-performance-benchmark PRIVATE
        snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-main-page-performance-benchmark)

    add_executable(snow-shot-main-window-skin-performance-benchmark
        tests/main_window_skin_performance_benchmark.cpp)
    target_link_libraries(snow-shot-main-window-skin-performance-benchmark PRIVATE
        snow_shot_main_window_skin snow_shot_storage snow_shot_image_codec snow_draw_engine_qt_canvas)
    snow_shot_import_offscreen_platform(snow-shot-main-window-skin-performance-benchmark)

    add_executable(snow-shot-main-window-no-skin-performance-benchmark
        tests/main_window_no_skin_performance_benchmark.cpp
        include/snow_shot/presentation/mainwindow.h
        include/snow_shot/presentation/components/contentcardwidget.h
        include/snow_shot/presentation/components/maincontentheaderwidget.h
        include/snow_shot/presentation/components/sidebarwidget.h
        include/snow_shot/presentation/components/titlebarwidget.h
        src/presentation/mainwindow.cpp
        src/presentation/components/contentcardwidget.cpp
        src/presentation/components/maincontentheaderwidget.cpp
        src/presentation/components/sidebarwidget.cpp
        src/presentation/components/titlebarwidget.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/platform/windows/windowchrome.cpp)
    if(APPLE)
        target_sources(snow-shot-main-window-no-skin-performance-benchmark PRIVATE
            src/platform/macos/applicationactivation.mm)
        target_link_libraries(snow-shot-main-window-no-skin-performance-benchmark PRIVATE
            "-framework AppKit")
    endif()
    target_link_libraries(snow-shot-main-window-no-skin-performance-benchmark PRIVATE
        snow_shot_settings snow_shot_image_codec snow_shot_updates)
    snow_shot_import_offscreen_platform(snow-shot-main-window-no-skin-performance-benchmark)
    snow_shot_add_translations(snow-shot-main-window-no-skin-performance-benchmark)

    add_executable(snow-shot-pinned-window-management-performance-benchmark
        tests/pinned_window_management_performance_benchmark.cpp)
    target_link_libraries(snow-shot-pinned-window-management-performance-benchmark PRIVATE
        snow_shot_storage Qt6::Core Qt6::Gui)
    if(MSVC)
        # Text-source and bulk-removal cases do not call the image codec. Delay its
        # loading so unrelated HEIF codec startup does not affect these measurements.
        target_link_options(snow-shot-pinned-window-management-performance-benchmark PRIVATE
            "/DELAYLOAD:snow_shot_image_codec_backend.dll")
        target_link_libraries(snow-shot-pinned-window-management-performance-benchmark PRIVATE
            delayimp)
    endif()
    add_test(NAME snow-shot-pinned-window-management-performance-benchmark
        COMMAND snow-shot-pinned-window-management-performance-benchmark)
    set_tests_properties(snow-shot-pinned-window-management-performance-benchmark PROPERTIES
        LABELS "benchmark" TIMEOUT 180
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-ocr-background-performance-benchmark
        tests/screenshot_ocr_background_performance_benchmark.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp)
    target_include_directories(snow-shot-ocr-background-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-ocr-background-performance-benchmark PRIVATE
        Qt6::Gui snow_draw_engine_qt_canvas)
    add_test(NAME snow-shot-ocr-background-performance-benchmark
        COMMAND snow-shot-ocr-background-performance-benchmark)
    set_tests_properties(snow-shot-ocr-background-performance-benchmark PROPERTIES
        LABELS "benchmark" TIMEOUT 120
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-recognition-image-performance-benchmark
        tests/screenshot_recognition_image_performance_benchmark.cpp
        ${SNOW_SHOT_RECOGNITION_IMAGE_RENDER_SOURCES})
    target_include_directories(snow-shot-recognition-image-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-recognition-image-performance-benchmark PRIVATE
        adqt::widgets Qt6::Gui snow_canvas_image)
    snow_shot_import_offscreen_platform(snow-shot-recognition-image-performance-benchmark)
    qt_import_plugins(snow-shot-recognition-image-performance-benchmark
        NO_DEFAULT INCLUDE Qt6::QOffscreenIntegrationPlugin)

    add_executable(snow-shot-ocr-selection-performance-benchmark
        tests/screenshot_ocr_selection_performance_benchmark.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp)
    target_include_directories(snow-shot-ocr-selection-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(snow-shot-ocr-selection-performance-benchmark PRIVATE
        SNOW_SHOT_BENCH_INTERNALS=1)
    target_link_libraries(snow-shot-ocr-selection-performance-benchmark PRIVATE
        ant_design_qt Qt6::Widgets snow_draw_engine_qt_canvas)
    snow_shot_import_offscreen_platform(snow-shot-ocr-selection-performance-benchmark)
    add_test(NAME snow-shot-ocr-selection-performance-benchmark
        COMMAND snow-shot-ocr-selection-performance-benchmark -platform offscreen)
    set_tests_properties(snow-shot-ocr-selection-performance-benchmark PROPERTIES
        LABELS "benchmark" RUN_SERIAL TRUE TIMEOUT 600
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-memory-performance-benchmark
        tests/screenshot_memory_performance_benchmark.cpp
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/pinnedcolorswatch.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
        ${SNOW_SHOT_RECOGNITION_IMAGE_RENDER_SOURCES})
    target_include_directories(snow-shot-memory-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(snow-shot-memory-performance-benchmark PRIVATE
        SNOW_SHOT_BENCHMARK_MANAGED_READER=1)
    target_link_libraries(snow-shot-memory-performance-benchmark PRIVATE
        adqt::widgets snow_shot_image_codec snow_shot_diagnostics Qt6::Widgets)
    if(WIN32)
        target_link_libraries(snow-shot-memory-performance-benchmark PRIVATE user32 psapi)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-memory-performance-benchmark)
    add_test(NAME snow-shot-memory-performance-benchmark
        COMMAND snow-shot-memory-performance-benchmark -platform offscreen)
    set_tests_properties(snow-shot-memory-performance-benchmark PROPERTIES
        LABELS "benchmark" RUN_SERIAL TRUE TIMEOUT 180
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-translation-page-streaming-benchmark
        tests/translation_page_streaming_benchmark.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-translation-page-streaming-benchmark PRIVATE
        snow_shot_settings snow_shot_image_codec)
    if(WIN32)
        target_link_libraries(snow-shot-translation-page-streaming-benchmark PRIVATE
            PkgConfig::SNOW_SHOT_FFMPEG)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-translation-page-streaming-benchmark)

    add_executable(snow-shot-scrolling-image-performance-benchmark
        tests/scrolling_image_performance_benchmark.cpp)
    snow_shot_configure_scrolling_replay_target(snow-shot-scrolling-image-performance-benchmark)
    add_executable(snow-shot-scrolling-hover-preview-benchmark
        tests/scrolling_hover_preview_benchmark.cpp)
    snow_shot_configure_scrolling_replay_target(snow-shot-scrolling-hover-preview-benchmark)
    add_executable(snow-shot-scrolling-frame-handoff-benchmark
        tests/scrolling_frame_handoff_benchmark.cpp)
    target_include_directories(snow-shot-scrolling-frame-handoff-benchmark PRIVATE
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-stitch-images-c/include")
    target_link_libraries(snow-shot-scrolling-frame-handoff-benchmark PRIVATE snow_stitch_images_c)
    target_compile_definitions(snow-shot-scrolling-frame-handoff-benchmark PRIVATE
        SNOW_SCROLLING_BUILD_CONFIG="$<CONFIG>")
    set_target_properties(snow-shot-scrolling-frame-handoff-benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-bin")
    if(SNOW_SHOT_SCROLLING_PERF_DETAIL)
        target_compile_definitions(snow-shot-scrolling-frame-handoff-benchmark PRIVATE
            SNOW_SHOT_SCROLLING_PERF_DETAIL=1 SNOW_STITCH_PERF_INSTRUMENTATION=1)
    endif()
    if(WIN32)
        target_link_libraries(snow-shot-scrolling-frame-handoff-benchmark PRIVATE psapi)
        target_compile_definitions(snow-shot-scrolling-frame-handoff-benchmark PRIVATE NOMINMAX)
    endif()
    add_test(NAME snow-shot-scrolling-frame-handoff-benchmark
        COMMAND snow-shot-scrolling-frame-handoff-benchmark)
    set_tests_properties(snow-shot-scrolling-frame-handoff-benchmark PROPERTIES
        LABELS "benchmark" RUN_SERIAL TRUE TIMEOUT 120)
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-scrolling-hover-preview-tests tests/scrolling_hover_preview_tests.cpp)
    target_include_directories(snow-shot-scrolling-hover-preview-tests PRIVATE src)
    target_link_libraries(snow-shot-scrolling-hover-preview-tests PRIVATE Qt6::Gui)
    add_test(NAME snow-shot-scrolling-hover-preview-tests
        COMMAND snow-shot-scrolling-hover-preview-tests)
    set_tests_properties(snow-shot-scrolling-hover-preview-tests PROPERTIES LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(snow-shot-scrolling-hover-geometry-tests
        tests/scrolling_hover_geometry_tests.cpp
        include/snow_shot/presentation/screenshotscrollingthumbnailwidget.h
        src/presentation/capture/screenshotscrollingthumbnailwidget.cpp)
    target_include_directories(snow-shot-scrolling-hover-geometry-tests PRIVATE include)
    target_compile_definitions(snow-shot-scrolling-hover-geometry-tests PRIVATE
        SNOW_SHOT_BENCH_INTERNALS=1)
    target_link_libraries(snow-shot-scrolling-hover-geometry-tests PRIVATE
        snow_canvas_image ant_design_qt Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-scrolling-hover-geometry-tests)
    add_test(NAME snow-shot-scrolling-hover-geometry-tests
        COMMAND snow-shot-scrolling-hover-geometry-tests -platform offscreen)
    set_tests_properties(snow-shot-scrolling-hover-geometry-tests PROPERTIES LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        target_sources(snow-shot-scrolling-hover-geometry-tests PRIVATE
            src/platform/windows/windowchrome.cpp)
        target_link_libraries(snow-shot-scrolling-hover-geometry-tests PRIVATE dwmapi user32)
        add_test(NAME snow-shot-scrolling-thumbnail-capture-composition-tests
            COMMAND snow-shot-scrolling-hover-geometry-tests --capture-composition-only
                -platform windows)
        set_tests_properties(snow-shot-scrolling-thumbnail-capture-composition-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60)
    endif()

    add_executable(snow-shot-scrolling-image-replay-tests tests/scrolling_image_replay_tests.cpp)
    snow_shot_configure_scrolling_replay_target(snow-shot-scrolling-image-replay-tests)
    add_test(NAME snow-shot-scrolling-image-replay-tests COMMAND snow-shot-scrolling-image-replay-tests)
    add_test(NAME snow-shot-scrolling-snapshot-lifetime-tests
        COMMAND snow-shot-scrolling-image-replay-tests --snapshot-teardown-only)
    add_test(NAME snow-shot-scrolling-async-teardown-tests
        COMMAND snow-shot-scrolling-image-replay-tests --async-teardown-only)
    set_tests_properties(snow-shot-scrolling-async-teardown-tests PROPERTIES
        LABELS "unit" ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-scrolling-snapshot-lifetime-tests PROPERTIES
        LABELS "unit" ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-scrolling-thumbnail-drag-tests
        COMMAND snow-shot-scrolling-image-replay-tests --thumbnail-drag-only)
    add_test(NAME snow-shot-scrolling-hover-pipeline-tests
        COMMAND snow-shot-scrolling-image-replay-tests --hover-preview-only)
    set_tests_properties(snow-shot-scrolling-hover-pipeline-tests PROPERTIES LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(snow-shot-scrolling-thumbnail-drag-tests PROPERTIES LABELS "unit")
    set_tests_properties(snow-shot-scrolling-image-replay-tests PROPERTIES
        LABELS "unit" ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(APPLE)
        foreach(_snow_qos IN ITEMS user_interactive user_initiated default utility background)
            add_test(NAME snow-shot-scrolling-qos-${_snow_qos}-tests
                COMMAND snow-shot-scrolling-image-replay-tests --qos-only ${_snow_qos})
            set_tests_properties(snow-shot-scrolling-qos-${_snow_qos}-tests PROPERTIES
                LABELS "unit;macos" TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endforeach()
        unset(_snow_qos)
    endif()
endif()

if(SNOW_SHOT_BUILD_BENCHMARKS)
    # This executable deliberately is not registered with CTest. It creates a
    # native widget and reports hardware-sensitive timing for explicit
    # Release performance sessions on Windows and macOS.
    add_executable(snow-shot-selection-render-benchmark
        tests/screenshot_selection_render_benchmark.cpp
        include/snow_shot/presentation/screenshotcanvasrenderer.h
        include/snow_shot/presentation/screenshotselectionshadowrenderer.h
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        src/presentation/ocr/screenshotocrtexttransform.cpp
        src/presentation/ocr/screenshotocrlayout.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotimagerendering.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
    )
    target_compile_definitions(snow-shot-selection-render-benchmark PRIVATE
        NOMINMAX
        SNOW_SHOT_BENCH_INTERNALS=1
    )
    target_include_directories(snow-shot-selection-render-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_SHOT_DRAW_ENGINE_QT_DIR}/src/rendering"
        "${SNOW_SHOT_DRAW_ENGINE_QT_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-shot-selection-render-benchmark PRIVATE
        Qt6::Widgets
        ant_design_qt
        snow_draw_engine_qt_canvas
    )
    if(WIN32)
        target_link_libraries(snow-shot-selection-render-benchmark PRIVATE dwmapi user32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-selection-render-benchmark)
endif()

if(WIN32 AND SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-selection-toolbar-performance-benchmark
        tests/screenshot_selection_toolbar_performance_benchmark.cpp
        include/snow_shot/presentation/screenshotselectiontoolbarwidget.h
        src/platform/windows/monitorgeometry.cpp
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/selection/screenshotselectiontoolbarwidget.cpp
        src/presentation/selection/screenshotselectiontoolbarwidgets.h
        src/presentation/selection/screenshotselectiontoolbarwidgets.cpp)
    target_include_directories(snow-shot-selection-toolbar-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/selection")
    target_link_libraries(snow-shot-selection-toolbar-performance-benchmark PRIVATE
        adqt::widgets adqt_icon_core ant_design_icons_qt Qt6::Widgets snow_draw_engine_qt_canvas
        user32)
    snow_shot_import_offscreen_platform(snow-shot-selection-toolbar-performance-benchmark)

    add_executable(snow-shot-decoration-area-benchmark
        tests/screenshot_decoration_area_benchmark.cpp)
    target_link_libraries(snow-shot-decoration-area-benchmark PRIVATE
        Qt6::Widgets snow_draw_engine_qt_canvas)

    # This executable deliberately is not registered with CTest. It creates
    # real always-on-top toolbar windows and measures real DWM presentation
    # for explicit Release benchmarking sessions. It compiles instrumented
    # copies of the toolbar sources; production builds compile the perf
    # scopes to no-ops because SNOW_SHOT_TOOLBAR_PERF_INSTRUMENTATION is not
    # defined for them.
    add_executable(snow-shot-screenshot-toolbar-display-benchmark
        tests/screenshot_toolbar_display_benchmark.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshottoolbarwindow.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h
        src/presentation/tools/screenshottoolbarperfinstrumentation.h
        src/presentation/toolbar/screenshottoolbarwindow.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
    )
    target_compile_definitions(
        snow-shot-screenshot-toolbar-display-benchmark
        PRIVATE
            NOMINMAX
            SNOW_SHOT_TOOLBAR_PERF_INSTRUMENTATION=1
    )
    target_include_directories(
        snow-shot-screenshot-toolbar-display-benchmark
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(
        snow-shot-screenshot-toolbar-display-benchmark
        PRIVATE
            adqt_icon_core
            ant_design_icons_qt
            ant_design_qt
            Qt6::Widgets
            snow_draw_engine_qt_canvas
            snow_shot_storage
            comctl32
            dwmapi
            user32
    )

    add_executable(snow-shot-history-performance-benchmark
        tests/screenshot_history_performance_benchmark.cpp
        include/snow_shot/presentation/components/contentcardwidget.h
        src/presentation/components/contentcardwidget.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_compile_definitions(snow-shot-history-performance-benchmark PRIVATE
        NOMINMAX
        SNOW_SHOT_HISTORY_BENCHMARK_BASELINE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/baselines/screenshot_history_performance.json"
    )
    target_include_directories(snow-shot-history-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-history-performance-benchmark PRIVATE
        snow_shot_settings
        snow_shot_storage
        snow_shot_settings_catalog
        ant_design_qt
        Qt6::Widgets
        Psapi
    )
    if(WIN32)
        target_link_libraries(snow-shot-history-performance-benchmark PRIVATE
            PkgConfig::SNOW_SHOT_FFMPEG
        )
    endif()

    # This executable deliberately is not registered with CTest. It publishes
    # generated images through the real Windows clipboard for explicit Release
    # performance sessions and leaves the final fixture on the clipboard.
    add_executable(snow-shot-clipboard-performance-benchmark
        tests/screenshot_clipboard_performance_benchmark.cpp
        include/snow_shot/presentation/screenshotclipboardservice.h
        include/snow_shot/presentation/screenshotdisplaysession.h
        include/snow_shot/presentation/screenshotexportservice.h
        include/snow_shot/presentation/screenshotgeometry.h
        include/snow_shot/presentation/screenshotresultcompositor.h
        include/snow_shot/presentation/screenshotselectionshadowrenderer.h
        include/snow_shot/presentation/screenshotselectionpin.h
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotclipboardperfinstrumentation.h
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotexportservice.cpp
        src/presentation/services/screenshotselectionpin.cpp
    )
    target_compile_definitions(snow-shot-clipboard-performance-benchmark PRIVATE
        NOMINMAX
        SNOW_SHOT_CLIPBOARD_PERF_INSTRUMENTATION=1
    )
    target_include_directories(snow-shot-clipboard-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-clipboard-performance-benchmark PRIVATE snow_shot_image_codec
        snow_shot_storage
        snow_shot_diagnostics
        adqt::widgets
        Qt6::Widgets
        snow_draw_engine_qt_canvas
        Psapi
        user32
    )

    # This is intentionally not registered with CTest. It exercises the real
    # Save-to-File pixel preparation, encoding, and preview decode routes and
    # reports hardware-sensitive medians for explicit Release sessions.
    add_executable(snow-shot-save-to-file-performance-benchmark
        tests/screenshot_save_to_file_performance_benchmark.cpp
        src/presentation/services/screenshotsaveexportpipeline.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
    )
    target_include_directories(snow-shot-save-to-file-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/services"
    )
    target_link_libraries(snow-shot-save-to-file-performance-benchmark PRIVATE
        snow_shot_image_codec
        snow_shot_storage
        Qt6::Gui
    )

    # This is intentionally not registered with CTest. It drives the real
    # file-pin batch snapshot/decode/presentation pipeline with real pinned
    # windows and reports hardware-sensitive medians for explicit Release
    # sessions.
    add_executable(snow-shot-file-pin-batch-performance-benchmark
        src/presentation/services/pinnedtextstylecapture.cpp
        tests/screenshot_file_pin_batch_performance_benchmark.cpp
        tests/screenshot_pinned_lifecycle_performance_benchmark.cpp
        src/presentation/pinned/screenshotpinnedclickthroughgeometry.cpp
        ${SNOW_SELECTED_FILES_SOURCES}
        src/presentation/services/screenshotfilepinbatch.cpp
        include/snow_shot/presentation/components/pathinput.h
        src/presentation/components/pathinput.cpp
        include/snow_shot/presentation/screenshotsavepreviewcanvas.h
        src/presentation/services/screenshotsaveasfiledialog.cpp
        src/presentation/services/screenshotsavefooter.h
        src/presentation/services/screenshotsavefooter.cpp
        src/presentation/services/screenshotsaveresult.h
        src/presentation/services/screenshotsaveresult.cpp
        src/presentation/services/screenshotsaveexportpipeline.cpp
        src/presentation/services/screenshotsavepreviewcanvas.cpp
        src/platform/physicalcursor.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshotcanvascolorsampler.h
        include/snow_shot/presentation/screenshotcanvascolorsamplerwindow.h
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/screenshotpinnededitcontroller.h
        include/snow_shot/presentation/screenshotpinnedwindow.h
        include/snow_shot/presentation/pinnedwindowgroupmanager.h
        include/snow_shot/presentation/screenshotrecognitionsessioncontroller.h
        include/snow_shot/presentation/screenshotrecognitionwindow.h
        include/snow_shot/presentation/screenshottableeditor.h
        include/snow_shot/presentation/screenshotmessageservice.h
        include/snow_shot/presentation/screenshotselectiongeometry.h
        include/snow_shot/presentation/screenshotselectionmodel.h
        include/snow_shot/presentation/screenshotselectionparams.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotstylebinding.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
        src/presentation/ocr/screenshotocrtexteditingsession.cpp
        src/presentation/ocr/screenshotocrtexttransform.cpp
        src/presentation/ocr/screenshotrecognitionsessioncontroller.cpp
        src/presentation/ocr/screenshotocrlayout.cpp
        src/presentation/ocr/screenshotrecognitionwindow.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/ocr/screenshottableeditor.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotcanvascolorsampler.cpp
        src/presentation/services/screenshotcanvascolorsamplerwindow.cpp
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/pinnedcolorswatch.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotrecognitionfileexport.cpp
        src/presentation/services/screenshotpdfexport.cpp
        src/presentation/services/screenshotmessageservice.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/pinned/screenshotpinnededitcontroller.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
        src/presentation/services/screenshotexportartifact.cpp
        src/presentation/services/screenshotexportpinnedsource.cpp
        src/presentation/ocr/screenshotrecognitionimage.cpp
        src/presentation/selection/screenshotselectionexportuiservices.cpp
        src/presentation/pinned/screenshotpinnednativegeometrycontroller.cpp
        src/presentation/pinned/screenshotpinnedresizegeometry.cpp
        src/presentation/pinned/screenshotpinnedrestoregeometry.cpp
        src/presentation/pinned/screenshotpinnedhidetotopcontroller.cpp
        src/presentation/pinned/screenshotpinnedwindow.cpp
        src/presentation/pinned/screenshotpinnedbatchsave.cpp
        src/presentation/services/pinnedwindowgroupmanager.cpp
        src/presentation/pinned/screenshotpinnedinteraction.cpp
        include/snow_shot/presentation/pinnedwindowselectioncontroller.h
        src/presentation/pinned/pinnedwindowselectioncontroller.cpp
        src/presentation/pinned/pinnedwindowselectioninteraction.cpp
        src/presentation/pinned/pinnedwindowselectiongeometry.cpp
        src/presentation/pinned/screenshotpinneddragexport.cpp
        ${SNOW_PINNED_PLATFORM_SOURCES}
        src/presentation/services/languagemanager.cpp
        src/presentation/styles/buttonborder.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
    )
    target_include_directories(snow-shot-file-pin-batch-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../snow_draw_engine_qt/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-shot-file-pin-batch-performance-benchmark PRIVATE
        snow_shot_file_manager
        MINIZIP::minizip-ng
        snow_shot_animation_playback
        ${SNOW_SELECTED_FILES_LIBRARIES}
        snow_shot_translation
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        Qt6::Network
        Qt6::Widgets
        snow_draw_engine_qt_canvas
        snow_shot_image_codec
        snow_shot_storage
        snow_shot_window_shortcuts
    )
    if(WIN32)
        target_link_libraries(snow-shot-file-pin-batch-performance-benchmark PRIVATE
            Qt6::GuiPrivate
            comctl32
            dwmapi
            user32
            ole32
            oleaut32
            shell32
            uuid
        )
    endif()
    if(QT_FEATURE_static)
        # Static Qt registers platform plugins at link time; the benchmark
        # presents real windows, so it must import the windows integration.
        target_link_libraries(snow-shot-file-pin-batch-performance-benchmark PRIVATE
            Qt6::QWindowsIntegrationPlugin
        )
        qt_import_plugins(snow-shot-file-pin-batch-performance-benchmark
            INCLUDE Qt6::QWindowsIntegrationPlugin
        )
    endif()
endif()

target_include_directories(snow_shot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-recording-c/include"
    "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-visual-region-detector-c/include"
    "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector-c/include"
    "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-stitch-images-c/include"
)
target_precompile_headers(snow_shot PRIVATE
    <algorithm>
    <memory>
    <optional>
    <string>
    <vector>
    <QtCore/QObject>
    <QtCore/QString>
    <QtGui/QColor>
    <QtGui/QImage>
    <QtGui/QPainter>
    <QtWidgets/QWidget>
)

target_link_libraries(
    snow_shot PRIVATE
        snow_recording_effects_c
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        snow_shot_settings
        snow_shot_storage
        snow_shot_window_shortcuts
        snow_draw_engine_qt_canvas
        snow_capture_c
        snow_recording_c
        snow_ui_selector_c
        snow_visual_region_detector_c
        snow_stitch_images_c
        opencv_wechat_qrcode
        opencv_objdetect
        Qt6::Widgets
        Qt6::Svg
        Qt6::Network
)

target_compile_definitions(snow_shot PRIVATE
    $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
)
add_dependencies(snow_shot snow_ocr_process)
if(SNOW_SHOT_ENABLE_MCP)
    add_dependencies(snow_shot snow_shot_mcp)
    add_custom_command(TARGET snow_shot POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:snow_shot_mcp>"
            "$<TARGET_FILE_DIR:snow_shot>/$<TARGET_FILE_NAME:snow_shot_mcp>"
        COMMENT "Staging Snow Shot MCP bridge"
        VERBATIM
    )
endif()

# Synthetic clean sources exercise deferred replay without desktop capture permissions.
if(SNOW_SHOT_BUILD_BENCHMARKS AND (WIN32 OR APPLE))
    set(_snow_postprocessing_benchmark_environment)
    if(WIN32)
        snow_windows_rust_environment(_snow_postprocessing_benchmark_environment
            "${SNOW_SHOT_RUST_TARGET}")
        list(APPEND _snow_postprocessing_benchmark_environment
            "LIBCLANG_PATH=${SNOW_SHOT_LIBCLANG_DIR}")
    else()
        list(APPEND _snow_postprocessing_benchmark_environment
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "CARGO_PROFILE_RELEASE_DEBUG=2")
    endif()
    add_custom_target(snow-shot-recording-postprocessing-export-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_postprocessing_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-export --features bench-timing
            --example postprocessing_export_benchmark
            --example native_effects_compositor_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL VERBATIM)
    unset(_snow_postprocessing_benchmark_environment)
endif()

# The same fixed-frame encoder experiment validates VideoToolbox on macOS.
if(APPLE AND SNOW_SHOT_BUILD_BENCHMARKS)
    add_custom_target(snow-shot-recording-encoder-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "CARGO_PROFILE_RELEASE_DEBUG=2"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-export --example recording_encoder_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL VERBATIM)
endif()

if(WIN32 AND SNOW_SHOT_BUILD_BENCHMARKS)
    snow_windows_rust_environment(_snow_shot_benchmark_environment "${SNOW_SHOT_RUST_TARGET}")
    list(APPEND _snow_shot_benchmark_environment "LIBCLANG_PATH=${SNOW_SHOT_LIBCLANG_DIR}")
    add_executable(snow-shot-uia-qt-performance-fixture
        tests/screenshot_uia_qt_performance_fixture.cpp
    )
    target_link_libraries(snow-shot-uia-qt-performance-fixture PRIVATE Qt6::Widgets user32)
    target_compile_definitions(snow-shot-uia-qt-performance-fixture PRIVATE NOMINMAX)

    add_custom_target(snow-shot-uia-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-ui-selector --example hit_bench --features windows-examples
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    # A standalone selector archive avoids pulling the recording/FFmpeg umbrella
    # into this focused benchmark and matches the preserved baseline archive.
    set(_uia_phased_lib "${CMAKE_BINARY_DIR}/selector-cargo/${SNOW_SHOT_RUST_TARGET}/release/snow_ui_selector_c.lib")
    file(GLOB_RECURSE _uia_selector_sources CONFIGURE_DEPENDS
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector/src/*.rs"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector-c/src/*.rs")
    add_custom_command(OUTPUT "${_uia_phased_lib}"
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment} "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/selector-cargo"
            "RUSTFLAGS=-C target-feature=+crt-static"
            "${CARGO_EXECUTABLE}" build --locked --release --target ${SNOW_SHOT_RUST_TARGET}
            --manifest-path "${SNOW_SHOT_CAPTURE_CRATES_DIR}/Cargo.toml" -p snow-ui-selector-c
        DEPENDS ${_uia_selector_sources}
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/Cargo.lock"
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/Cargo.toml"
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector/Cargo.toml"
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector-c/Cargo.toml"
        VERBATIM)
    add_custom_target(snow-shot-selector-phased-rust DEPENDS "${_uia_phased_lib}")
    add_executable(snow-shot-selector-phased-performance-benchmark
        tests/screenshot_selector_phased_performance_benchmark.cpp
        include/snow_shot/presentation/screenshotselectorcoordinator.h
        src/presentation/selector/screenshotselectorcoordinator.cpp
        src/presentation/selector/screenshotselectorserviceclient.cpp
        src/presentation/core/screenshotintelligentselectionmodel.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_include_directories(snow-shot-selector-phased-performance-benchmark PRIVATE
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector-c/include")
    target_link_libraries(snow-shot-selector-phased-performance-benchmark PRIVATE
        snow_shot_settings "${_uia_phased_lib}" Qt6::Widgets
        user32 userenv ole32 oleaut32 oleacc gdi32 dwmapi ntdll ws2_32 bcrypt advapi32)
    add_dependencies(snow-shot-selector-phased-performance-benchmark snow-shot-selector-phased-rust)
    qt_import_plugins(snow-shot-selector-phased-performance-benchmark INCLUDE Qt6::QWindowsIntegrationPlugin)

    set(SNOW_SHOT_UIA_BASELINE_DIR "" CACHE PATH "Preserved pre-refactor selector sources and standalone Rust workspace for comparison.")
    if(SNOW_SHOT_UIA_BASELINE_DIR)
        set(_uia_baseline_lib "${SNOW_SHOT_UIA_BASELINE_DIR}/target/${SNOW_SHOT_RUST_TARGET}/release/snow_ui_selector_c.lib")
        add_custom_command(OUTPUT "${_uia_baseline_lib}"
            COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment} "CARGO_TARGET_DIR=${SNOW_SHOT_UIA_BASELINE_DIR}/target"
                "RUSTFLAGS=-C target-feature=+crt-static"
                "${CARGO_EXECUTABLE}" build --offline --release --target ${SNOW_SHOT_RUST_TARGET}
                --manifest-path "${SNOW_SHOT_UIA_BASELINE_DIR}/Cargo.toml" -p snow-ui-selector-c
            VERBATIM)
        add_custom_target(snow-shot-selector-baseline-rust DEPENDS "${_uia_baseline_lib}")
        add_executable(snow-shot-selector-baseline-performance-benchmark
            tests/screenshot_selector_phased_performance_benchmark.cpp
            "${SNOW_SHOT_UIA_BASELINE_DIR}/snow_shot/include/snow_shot/presentation/screenshotselectorcoordinator.h"
            "${SNOW_SHOT_UIA_BASELINE_DIR}/snow_shot/src/presentation/selector/screenshotselectorcoordinator.cpp"
            "${SNOW_SHOT_UIA_BASELINE_DIR}/snow_shot/src/presentation/selector/screenshotselectorserviceclient.cpp"
            src/presentation/core/screenshotintelligentselectionmodel.cpp
            src/presentation/services/screenshotclipboardservice.cpp)
        target_compile_definitions(snow-shot-selector-baseline-performance-benchmark PRIVATE SNOW_UIA_LEGACY_BENCHMARK)
        target_include_directories(snow-shot-selector-baseline-performance-benchmark BEFORE PRIVATE
            "${SNOW_SHOT_UIA_BASELINE_DIR}/snow_shot/include"
            "${SNOW_SHOT_UIA_BASELINE_DIR}/crates/snow-ui-selector-c/include")
        target_link_libraries(snow-shot-selector-baseline-performance-benchmark PRIVATE
            snow_shot_settings "${_uia_baseline_lib}" Qt6::Widgets
            user32 userenv ole32 oleaut32 oleacc gdi32 dwmapi ntdll ws2_32 bcrypt advapi32)
        add_dependencies(snow-shot-selector-baseline-performance-benchmark snow-shot-selector-baseline-rust)
        qt_import_plugins(snow-shot-selector-baseline-performance-benchmark INCLUDE Qt6::QWindowsIntegrationPlugin)
    endif()

    # Not registered with CTest: this generates final-container fixtures through
    # the streaming encoder used by direct recording and reports Release metrics.
    add_custom_target(snow-shot-direct-recording-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-export --example direct_recording_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    add_custom_target(snow-shot-recording-trim-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-export --example recording_trim_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL VERBATIM)

    add_custom_target(snow-shot-recording-encoder-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-export --example recording_encoder_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    add_custom_target(snow-shot-gif-encoding-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "SNOW_GIF_BENCH_OUTPUT=${CMAKE_BINARY_DIR}/gif-encoding-performance"
            "${CARGO_EXECUTABLE}" test --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-export --test gif_encoding
            gif_encoding_release_comparison -- --ignored --exact --nocapture
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    add_custom_target(snow-shot-recording-effects-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-effects --example effects_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL VERBATIM)
    add_custom_target(snow-shot-recording-postprocessing-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-effects --example postprocessing_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL VERBATIM)
    if(WIN32 AND TARGET snow-shot-screen-recording-controller-tests)
        target_sources(snow-shot-screen-recording-controller-tests PRIVATE
            tests/recording_effects_performance_benchmark.cpp)
        target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
            SNOW_RECORDING_EFFECTS_BENCHMARK=1)
        add_dependencies(snow-shot-recording-effects-performance-benchmark
            snow-shot-screen-recording-controller-tests)
        add_test(NAME snow-shot-recording-effects-presentation-benchmark
            COMMAND snow-shot-screen-recording-controller-tests --effects-preview-performance -platform windows)
        set_tests_properties(snow-shot-recording-effects-presentation-benchmark PROPERTIES
            LABELS "benchmark;interactive;windows" RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNOW_FFMPEG_ROOT}/bin")
        add_test(NAME snow-shot-recording-effects-rasterization-benchmark
            COMMAND "${SNOW_RUST_CARGO_TARGET_DIR}/${SNOW_SHOT_RUST_TARGET}/release/examples/effects_benchmark.exe")
        set_tests_properties(snow-shot-recording-effects-rasterization-benchmark PROPERTIES
            LABELS "benchmark;windows" RUN_SERIAL TRUE TIMEOUT 90)
    endif()

    # Measures the recording window startup phases (controller open ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¾ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ windows
    # shown ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¾ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ first paint ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¾ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ first motion-preview frame) through the
    # compile-time gated recording_perf instrumentation. Deterministic and
    # offscreen, so unlike the effects presentation benchmark it stays out of
    # the interactive label.
    if(TARGET snow-shot-screen-recording-controller-tests)
        target_sources(snow-shot-screen-recording-controller-tests PRIVATE
            tests/screen_recording_window_startup_performance_benchmark.cpp
            tests/screen_recording_window_startup_performance_benchmark.h
            src/presentation/recording/screenrecordingperfinstrumentation.cpp
            src/presentation/recording/screenrecordingperfinstrumentation.h)
        target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
            SNOW_RECORDING_WINDOW_STARTUP_BENCHMARK=1
            SNOW_SHOT_RECORDING_PERF_INSTRUMENTATION=1)
        add_test(NAME snow-shot-recording-window-startup-performance-benchmark
            COMMAND snow-shot-screen-recording-controller-tests
                --recording-window-startup-performance)
        set_tests_properties(snow-shot-recording-window-startup-performance-benchmark
            PROPERTIES LABELS "benchmark" RUN_SERIAL TRUE TIMEOUT 180
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    add_custom_target(snow-shot-keyboard-recording-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-runtime --example keyboard_overlay_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    # Drives the real DirectRecordingSession path (capture stream, input
    # hooks, overlay compositing, encoder) against a temporary animated
    # fullscreen window with simulated mouse/keyboard input. The metric
    # groups are compile-time cargo features so each is measured without
    # the instrumentation overhead of the others.
    add_custom_target(snow-shot-realtime-recording-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env ${_snow_shot_benchmark_environment}
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target ${SNOW_SHOT_RUST_TARGET}
            --package snow-recording-runtime
            --features bench-synthetic-input,bench-stage-timing,bench-compositor-timing,bench-pipeline-timing
            --example realtime_recording_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL
        VERBATIM
    )
    # Seizes the primary monitor and injects input; interactive label keeps it
    # out of default test presets.
    add_test(NAME snow-shot-realtime-recording-performance-benchmark
        COMMAND "${SNOW_RUST_CARGO_TARGET_DIR}/${SNOW_SHOT_RUST_TARGET}/release/examples/realtime_recording_benchmark.exe"
            --duration-seconds 4 --warmup-seconds 1)
    set_tests_properties(snow-shot-realtime-recording-performance-benchmark PROPERTIES
        LABELS "benchmark;interactive;windows" RUN_SERIAL TRUE TIMEOUT 300
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNOW_FFMPEG_ROOT}/bin")

    add_executable(snow-shot-pin-to-screen-performance-benchmark
        tests/screenshot_pin_to_screen_performance_benchmark.cpp
    )
    target_include_directories(snow-shot-pin-to-screen-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-pin-to-screen-performance-benchmark PRIVATE
        Qt6::Core
        Qt6::Gui
        dwmapi
        gdi32
        ole32
        oleaut32
        psapi
        Uiautomationcore
        user32
    )
    target_compile_definitions(snow-shot-pin-to-screen-performance-benchmark PRIVATE NOMINMAX)

    target_compile_definitions(snow_shot PRIVATE
        SNOW_SHOT_PIN_PERF_INSTRUMENTATION=1
    )

    # Not registered with CTest: drives the real snow_shot.exe through UIA and
    # reports hardware-sensitive first/second-capture timing for explicit
    # Windows performance sessions.
    add_executable(snow-shot-capture-startup-performance-benchmark
        tests/screenshot_capture_startup_performance_benchmark.cpp
    )
    target_include_directories(snow-shot-capture-startup-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-capture-startup-performance-benchmark PRIVATE
        Qt6::Core
        dwmapi
        gdi32
        ole32
        oleaut32
        psapi
        Uiautomationcore
        user32
    )
    target_compile_definitions(snow-shot-capture-startup-performance-benchmark PRIVATE NOMINMAX)

    target_compile_definitions(snow_shot PRIVATE
        SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION=1
    )
endif()

snow_shot_add_translations(snow_shot)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowShotUpdateUiTests.cmake")

foreach(_ocr_process_host_target IN ITEMS snow_shot snow-shot-ocr-recognition-service-tests)
    if(TARGET ${_ocr_process_host_target})
        add_dependencies(${_ocr_process_host_target} snow_ocr_process)
        add_custom_command(TARGET ${_ocr_process_host_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:snow_ocr_process>"
                "$<TARGET_FILE_DIR:${_ocr_process_host_target}>/$<TARGET_FILE_NAME:snow_ocr_process>"
            COMMENT "Staging standalone OCR process for ${_ocr_process_host_target}"
            VERBATIM
        )
        # Packaged installers ship the verified OCR payload (or, for the online
        # variant, its trusted manifest) next to the executable. Development
        # builds must stage the same payload explicitly, otherwise the managed
        # asset pipeline finds no manifest and rejects every OCR request with
        # "Text recognition components could not be prepared".
        if(WIN32 AND EXISTS "${SNOW_SHOT_OCR_ASSET_MANIFEST}")
            add_custom_command(TARGET ${_ocr_process_host_target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}"
                    -D "SNOW_OCR_ASSET_MANIFEST=${SNOW_SHOT_OCR_ASSET_MANIFEST}"
                    -D "SNOW_OCR_PLATFORM=${SNOW_WINDOWS_PLATFORM}"
                    -D "SNOW_OCR_ARTIFACTS_DIR=${_snow_ocr_artifacts_directory}"
                    -D "SNOW_OCR_DESTINATION=$<TARGET_FILE_DIR:${_ocr_process_host_target}>/assets/ocr"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/StageSnowShotOcrAssets.cmake"
                COMMENT "Staging verified OCR assets for ${_ocr_process_host_target}"
                VERBATIM
            )
        endif()
    endif()
endforeach()
unset(_ocr_process_host_target)

if(SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-qr-performance-benchmark
        tests/screenshot_qr_performance_benchmark.cpp
        src/presentation/ocr/screenshotqrcontroller.cpp
        include/snow_shot/presentation/screenshotqrcontroller.h
        src/presentation/ocr/screenshotqrrecognitionservice.cpp
        include/snow_shot/presentation/screenshotqrrecognitionservice.h
        src/presentation/components/icons/snowshoticons.cpp)
    target_include_directories(snow-shot-qr-performance-benchmark PRIVATE include)
    target_link_libraries(snow-shot-qr-performance-benchmark PRIVATE snow_draw_engine_qt_canvas
        ant_design_qt opencv_wechat_qrcode opencv_objdetect Qt6::Widgets)
    if(WIN32)
        target_link_libraries(snow-shot-qr-performance-benchmark PRIVATE psapi)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-qr-performance-benchmark)
endif()

# The in-process barcode recognition service loads the pinned WeChat QR
# models at runtime; stage them next to every host that links the service.
foreach(_qr_host_target IN ITEMS snow_shot snow-shot-qr-recognition-service-tests
        snow-shot-qr-performance-benchmark)
    if(TARGET ${_qr_host_target})
        add_custom_command(TARGET ${_qr_host_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:${_qr_host_target}>/assets/qrcode"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SNOW_SHOT_QR_MODELS_DIR}/detect.prototxt"
                "${SNOW_SHOT_QR_MODELS_DIR}/detect.caffemodel"
                "${SNOW_SHOT_QR_MODELS_DIR}/sr.prototxt"
                "${SNOW_SHOT_QR_MODELS_DIR}/sr.caffemodel"
                "$<TARGET_FILE_DIR:${_qr_host_target}>/assets/qrcode"
            COMMENT "Staging WeChat QR models for ${_qr_host_target}"
            VERBATIM
        )
    endif()
endforeach()
unset(_qr_host_target)

if(MSVC AND SNOW_SHOT_RELEASE_STATIC)
    target_link_options(snow_shot PRIVATE
        $<$<CONFIG:Release>:/MAP>
        $<$<CONFIG:Release>:/MAPINFO:EXPORTS>
    )
endif()

if(WIN32)
    target_link_libraries(snow_shot PRIVATE
        Qt6::GuiPrivate
        dwmapi
        advapi32
        bcrypt
        comctl32
        d3d11
        dxgi
        dxguid
        gdi32
        ole32
        oleacc
        oleaut32
        psapi
        runtimeobject
        user32
        userenv
        winmm
        windowsapp
        ws2_32
    )
endif()

if(WIN32)
    set(SNOW_SHOT_FFMPEG_BIN_DIR
        "${SNOW_FFMPEG_ROOT}/bin"
        CACHE PATH
        "Directory containing the FFmpeg runtime DLLs used by snow-screen-recorder."
    )
    if(VCPKG_HOST_TRIPLET)
        set(_snow_pkgconf_host_triplet "${VCPKG_HOST_TRIPLET}")
    else()
        snow_windows_normalize_architecture(_snow_pkgconf_host_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        set(_snow_pkgconf_host_triplet "${_snow_pkgconf_host_arch}-windows")
    endif()
    find_program(PKG_CONFIG_EXECUTABLE
        NAMES pkgconf pkg-config
        HINTS "${VCPKG_INSTALLED_DIR}/${_snow_pkgconf_host_triplet}/tools/pkgconf"
        NO_DEFAULT_PATH
        REQUIRED
    )
    set(_SNOW_SHOT_SAVED_PKG_CONFIG_PATH "$ENV{PKG_CONFIG_PATH}")
    set(ENV{PKG_CONFIG_PATH} "${SNOW_FFMPEG_ROOT}/lib/pkgconfig")
    find_package(PkgConfig REQUIRED)
    set(_snow_ffmpeg_modules libavfilter libavformat libavcodec libswresample libswscale libavutil)
    if(SNOW_WINDOWS_ARCHITECTURE STREQUAL "x64")
        # FFmpeg's static .pc files omit this private QSV dependency.
        list(APPEND _snow_ffmpeg_modules vpl)
    endif()
    pkg_check_modules(SNOW_SHOT_FFMPEG REQUIRED IMPORTED_TARGET GLOBAL ${_snow_ffmpeg_modules})
    set(ENV{PKG_CONFIG_PATH} "${_SNOW_SHOT_SAVED_PKG_CONFIG_PATH}")
    unset(_SNOW_SHOT_SAVED_PKG_CONFIG_PATH)
    # Canvas-only consumers also reach recording symbols in the bundled archive.
    foreach(_snow_rust_bundle IN ITEMS snow_shot_rust_ffi_bundle snow_shot_full_rust_ffi_bundle)
        if(TARGET ${_snow_rust_bundle})
            target_link_libraries(${_snow_rust_bundle} INTERFACE PkgConfig::SNOW_SHOT_FFMPEG)
        endif()
    endforeach()
    unset(_snow_rust_bundle)
    target_link_libraries(snow_shot PRIVATE PkgConfig::SNOW_SHOT_FFMPEG)
    if(TARGET snow-shot-screen-recording-controller-tests)
        target_link_libraries(snow-shot-screen-recording-controller-tests PRIVATE PkgConfig::SNOW_SHOT_FFMPEG)
        set_property(TEST snow-shot-screen-recording-controller-tests snow-shot-recording-effects-preview-tests APPEND PROPERTY
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNOW_FFMPEG_ROOT}/bin")
    endif()
    if(TARGET snow-shot-screenshot-canvas-renderer-tests)
        target_link_libraries(snow-shot-screenshot-canvas-renderer-tests PRIVATE
            PkgConfig::SNOW_SHOT_FFMPEG
        )
    endif()
    # The toolbar benchmarks link the canvas engine, which pulls the Rust FFI
    # bundle and with it the FFmpeg import libraries.
    foreach(_snow_shot_ffmpeg_toolbar_benchmark IN ITEMS
            snow-shot-table-preparation-benchmark
            snow-shot-screenshot-toolbar-render-benchmark
            snow-shot-screenshot-toolbar-display-benchmark
            snow-shot-file-pin-batch-performance-benchmark)
        if(TARGET ${_snow_shot_ffmpeg_toolbar_benchmark})
            target_link_libraries(${_snow_shot_ffmpeg_toolbar_benchmark} PRIVATE
                PkgConfig::SNOW_SHOT_FFMPEG
            )
        endif()
    endforeach()
    unset(_snow_shot_ffmpeg_toolbar_benchmark)
    # The OCR background executables also reach the shared Rust bundle through the canvas.
    foreach(_snow_shot_ocr_background_target IN ITEMS
            snow-shot-ocr-background-tests
            snow-shot-ocr-background-performance-benchmark)
        if(TARGET ${_snow_shot_ocr_background_target})
            target_link_libraries(${_snow_shot_ocr_background_target} PRIVATE
                PkgConfig::SNOW_SHOT_FFMPEG)
            set_property(TEST ${_snow_shot_ocr_background_target} APPEND PROPERTY
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNOW_FFMPEG_ROOT}/bin")
        endif()
    endforeach()
    unset(_snow_shot_ocr_background_target)
    if(EXISTS "${SNOW_SHOT_FFMPEG_BIN_DIR}" AND NOT SNOW_SHOT_RELEASE_STATIC)
        set(SNOW_SHOT_FFMPEG_DLL_PATTERNS
            "avcodec-*.dll"
            "avdevice-*.dll"
            "avfilter-*.dll"
            "avformat-*.dll"
            "avutil-*.dll"
            "swresample-*.dll"
            "swscale-*.dll"
        )
        set(SNOW_SHOT_FFMPEG_DLLS)
        foreach(_ffmpeg_dll_pattern IN LISTS SNOW_SHOT_FFMPEG_DLL_PATTERNS)
            file(GLOB _ffmpeg_matching_dlls
                CONFIGURE_DEPENDS "${SNOW_SHOT_FFMPEG_BIN_DIR}/${_ffmpeg_dll_pattern}"
            )
            list(APPEND SNOW_SHOT_FFMPEG_DLLS ${_ffmpeg_matching_dlls})
        endforeach()
        # FFmpeg is built with the release CRT even in Debug. Resolve its DLL
        # imports from its own bin directory, including dependencies whose names
        # differ from the application's Debug codecs (zlib1.dll, libvpl.dll).
        set(_snow_ffmpeg_runtime_manifest
            "${CMAKE_CURRENT_BINARY_DIR}/generated/ffmpeg-runtime.cmake")
        execute_process(COMMAND "${CMAKE_COMMAND}"
            "-DSNOW_RUNTIME_ROOT_LIBRARIES=${SNOW_SHOT_FFMPEG_DLLS}"
            "-DSNOW_RUNTIME_MANIFEST=${_snow_ffmpeg_runtime_manifest}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/StageRuntimeDependencies.cmake"
            COMMAND_ERROR_IS_FATAL ANY)
        include("${_snow_ffmpeg_runtime_manifest}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${SNOW_RUNTIME_FILES})
        set(SNOW_SHOT_FFMPEG_RUNTIME_FILES ${SNOW_RUNTIME_FILES}
            CACHE INTERNAL "FFmpeg runtime files installed with Snow Shot.")
        unset(SNOW_RUNTIME_FILES)
    endif()
endif()

target_sources(snow_shot PRIVATE src/diagnostics/ffmpegdiagnostics.cpp)
if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-recording-diagnostics-tests
        tests/recording_diagnostics_tests.cpp src/diagnostics/ffmpegdiagnostics.cpp)
    target_link_libraries(snow-shot-recording-diagnostics-tests PRIVATE
        snow_shot_diagnostics PkgConfig::SNOW_SHOT_FFMPEG)
    if(APPLE)
        target_include_directories(snow-shot-recording-diagnostics-tests PRIVATE
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-recording-c/include")
        target_link_libraries(snow-shot-recording-diagnostics-tests PRIVATE snow_recording_c)
    endif()
    add_test(NAME snow-shot-recording-diagnostics-tests
        COMMAND snow-shot-recording-diagnostics-tests)
    set_tests_properties(snow-shot-recording-diagnostics-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(snow-shot-recording-diagnostics-tests PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNOW_FFMPEG_ROOT}/bin")
    endif()
endif()

function(snow_shot_stage_ffmpeg_runtime target)
    if(WIN32 AND NOT SNOW_SHOT_RELEASE_STATIC AND SNOW_SHOT_FFMPEG_RUNTIME_FILES)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${SNOW_SHOT_FFMPEG_RUNTIME_FILES}
                "$<TARGET_FILE_DIR:${target}>" COMMAND_EXPAND_LISTS VERBATIM)
    endif()
endfunction()

if(MSVC)
    # Parallel CL invocations otherwise race over the target's shared PDB.
    target_compile_options(snow_shot PRIVATE /FS /utf-8)
endif()

if(SNOW_SHOT_ENABLE_STRICT_COMPILE)
    # The directory-wide policy above covers snow_shot too. Keep only the
    # executable's additional GCC/Clang diagnostics here.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(snow_shot PRIVATE
            -Wextra-semi
            -Wredundant-decls
            -Wunreachable-code
            -Wzero-as-null-pointer-constant
        )
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(snow_shot PRIVATE
            -Wcomma
            -Wdocumentation
            -Wimplicit-int-conversion
            -Wnewline-eof
            -Wreserved-identifier
            -Wshorten-64-to-32
            -Wthread-safety
        )
    endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(snow_shot PRIVATE
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
        )
    endif()
endif()

set_target_properties(snow_shot PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER com.snowshot.snow_shot
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)
if(MSVC AND SNOW_SHOT_RELEASE_STATIC AND NOT SNOW_SHOT_QT_STATIC)
    # The shared Qt kit's entry-point shim is built with /MD. Use the static
    # CRT startup directly so the /MT release does not mix CRT variants.
    set_property(TARGET snow_shot PROPERTY qt_no_entrypoint TRUE)
    target_link_options(snow_shot PRIVATE /ENTRY:mainCRTStartup)
endif()

if(WIN32 AND NOT SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
    get_property(_snow_shot_directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_snow_shot_directory_target IN LISTS _snow_shot_directory_targets)
        get_target_property(_snow_shot_directory_target_type
            "${_snow_shot_directory_target}" TYPE)
        if(_snow_shot_directory_target_type STREQUAL "EXECUTABLE")
            _snow_shot_target_uses_image_codec(
                "${_snow_shot_directory_target}"
                _snow_shot_needs_image_runtime
                ""
            )
            if(_snow_shot_needs_image_runtime)
                _snow_shot_stage_image_runtime("${_snow_shot_directory_target}")
            endif()
        endif()
    endforeach()
    unset(_snow_shot_directory_target)
    unset(_snow_shot_directory_target_type)
    unset(_snow_shot_directory_targets)
    unset(_snow_shot_needs_image_runtime)
endif()

include(GNUInstallDirs)
if(APPLE)
    set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME SnowShot)
    set(CMAKE_INSTALL_BINDIR "snow_shot.app/Contents/MacOS")
    set(CMAKE_INSTALL_LIBDIR "snow_shot.app/Contents/Frameworks")
    set(CMAKE_INSTALL_DATADIR "snow_shot.app/Contents/Resources")
endif()
install(TARGETS snow_shot
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(PROGRAMS "$<TARGET_FILE:snow_ocr_process>"
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(SNOW_SHOT_ENABLE_MCP)
    install(PROGRAMS "$<TARGET_FILE:snow_shot_mcp>"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
if(WIN32 AND EXISTS "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}")
    install(FILES "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/audios"
        RENAME camera_shutter.mp3
    )
endif()
if(NOT SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
    install(TARGETS snow_shot_image_codec_backend
        RUNTIME_DEPENDENCY_SET snow_shot_image_runtime_dependencies
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    if(WIN32)
        install(RUNTIME_DEPENDENCY_SET snow_shot_image_runtime_dependencies
            DIRECTORIES
                "$<TARGET_FILE_DIR:snow_shot_image_codec_backend>"
                "${_SNOW_SHOT_IMAGE_RUNTIME_SEARCH_DIRECTORY}"
            PRE_EXCLUDE_REGEXES
                "api-ms-.*"
                "ext-ms-.*"
                "msvcp.*\\.dll"
                "ucrtbased\\.dll"
                "vcruntime.*\\.dll"
            POST_EXCLUDE_REGEXES
                ".*[\\\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\\\/].*"
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
endif()
# WeChat QR models for the in-process barcode recognition service; the runtime
# resolves them relative to the executable as assets/qrcode.
install(FILES
    "${SNOW_SHOT_QR_MODELS_DIR}/detect.prototxt"
    "${SNOW_SHOT_QR_MODELS_DIR}/detect.caffemodel"
    "${SNOW_SHOT_QR_MODELS_DIR}/sr.prototxt"
    "${SNOW_SHOT_QR_MODELS_DIR}/sr.caffemodel"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/assets/qrcode"
)

if(WIN32 AND NOT SNOW_SHOT_RELEASE_STATIC)
    snow_shot_stage_ffmpeg_runtime(snow_shot)
    if(SNOW_SHOT_FFMPEG_RUNTIME_FILES)
        install(FILES ${SNOW_SHOT_FFMPEG_RUNTIME_FILES}
            DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
endif()

if(WIN32 AND NOT SNOW_SHOT_QT_STATIC)
    set(_snow_shot_qt_deploy_arguments)
    # The release staging policy keeps only the platform and Schannel TLS
    # plugins. Snow Shot uses its project-owned resources and statically linked
    # image codecs, so Qt's optional image, style, and generic plugins are not
    # needed in the installer.
    list(APPEND _snow_shot_qt_deploy_arguments
        EXCLUDE_PLUGIN_TYPES
            generic
            iconengines
            imageformats
            networkinformation
            styles
        EXCLUDE_PLUGINS
            qcertonlybackend
    )
    if(SNOW_SHOT_RELEASE_STATIC)
        list(APPEND _snow_shot_qt_deploy_arguments
            NO_COMPILER_RUNTIME
            DEPLOY_TOOL_OPTIONS
                --release
        )
    endif()
    qt_generate_deploy_app_script(
        TARGET snow_shot
        OUTPUT_SCRIPT SNOW_SHOT_QT_DEPLOY_SCRIPT
        NO_UNSUPPORTED_PLATFORM_ERROR
        ${_snow_shot_qt_deploy_arguments}
    )
    install(SCRIPT "${SNOW_SHOT_QT_DEPLOY_SCRIPT}")

    # Qt's deployment helper copies every Qt base translation catalog. Snow
    # Shot ships only en_US, zh_CN, and zh_TW application catalogs, so retain
    # only the corresponding Qt catalogs in the final install tree.
    install(CODE [=[
        set(_snow_shot_qt_translation_dir
            "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/translations")
        set(_snow_shot_allowed_qt_translations
            qt_en.qm
            qt_zh_CN.qm
            qt_zh_TW.qm
        )
        if(IS_DIRECTORY "${_snow_shot_qt_translation_dir}")
            file(GLOB _snow_shot_qt_translation_catalogs
                "${_snow_shot_qt_translation_dir}/qt_*.qm")
            foreach(_snow_shot_qt_catalog IN LISTS _snow_shot_qt_translation_catalogs)
                get_filename_component(_snow_shot_qt_catalog_name
                    "${_snow_shot_qt_catalog}" NAME)
                if(NOT _snow_shot_qt_catalog_name IN_LIST _snow_shot_allowed_qt_translations)
                    file(REMOVE "${_snow_shot_qt_catalog}")
                endif()
            endforeach()
        endif()
        unset(_snow_shot_qt_translation_dir)
        unset(_snow_shot_allowed_qt_translations)
        unset(_snow_shot_qt_translation_catalogs)
        unset(_snow_shot_qt_catalog)
        unset(_snow_shot_qt_catalog_name)
    ]=])

    # windeployqt deploys optional Qt plugins based on broad module metadata.
    # Snow Shot only needs the Windows platform integration and the Schannel
    # TLS backend at runtime. Remove optional image, style, input,
    # network-information, icon-engine, and certificate-only backends.
    install(CODE [=[
        set(_snow_shot_qt_plugin_dir
            "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/plugins")
        set(_snow_shot_allowed_qt_plugins
            platforms/qwindows.dll
            tls/qschannelbackend.dll
        )
        if(IS_DIRECTORY "${_snow_shot_qt_plugin_dir}")
            file(GLOB_RECURSE _snow_shot_qt_plugin_files
                RELATIVE "${_snow_shot_qt_plugin_dir}"
                "${_snow_shot_qt_plugin_dir}/*.dll")
            foreach(_snow_shot_qt_plugin IN LISTS _snow_shot_qt_plugin_files)
                if(NOT _snow_shot_qt_plugin IN_LIST _snow_shot_allowed_qt_plugins)
                    file(REMOVE "${_snow_shot_qt_plugin_dir}/${_snow_shot_qt_plugin}")
                endif()
            endforeach()
        endif()
        unset(_snow_shot_qt_plugin_dir)
        unset(_snow_shot_allowed_qt_plugins)
        unset(_snow_shot_qt_plugin_files)
        unset(_snow_shot_qt_plugin)
    ]=])
endif()

if(SNOW_SHOT_QT_STATIC)
    # Static Qt does not load these plugins from a deployment directory.
    # Import only the integrations required by the Windows release binary.
    qt_import_plugins(snow_shot
        NO_DEFAULT
        INCLUDE ${_snow_shot_static_qt_plugins}
    )
endif()

if(WIN32)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowShotLicenseBundle.cmake")
    snow_install_license_bundle(
        SOURCE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        THIRD_PARTY_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/third-party-licenses/third-party"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/snow-shot/licenses"
        COMPONENT SnowShot PRODUCT "Snow Shot")
else()
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/COPYRIGHT"
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/snow-shot/licenses"
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../ant_design_qt/THIRD_PARTY_NOTICES.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/snow-shot/licenses/third-party/project-notices/ant-design-icons"
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../snow_image/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../snow_image/COPYRIGHT"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/snow-shot/licenses/components/snow-image"
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../ant_design_qt/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../ant_design_qt/COPYRIGHT"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/snow-shot/licenses/components/ant-design-qt"
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../snow-crates/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../snow-crates/COPYRIGHT"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/snow-shot/licenses/components/snow-crates"
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../snow_draw_engine_qt/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../snow_draw_engine_qt/COPYRIGHT"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/snow-shot/licenses/components/snow-draw-engine-qt"
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../snow_rust_ffi/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../snow_rust_ffi/COPYRIGHT"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/snow-shot/licenses/components/snow-rust-ffi"
    )
    install(DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/third-party-licenses/third-party/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/snow-shot/licenses/third-party"
        OPTIONAL
    )
endif()

foreach(_conversion_view_target IN ITEMS snow_shot snow-shot-recognition-window-tests
        snow-shot-mcp-document-tests
        snow-shot-recognition-session-controller-tests snow-shot-translation-tool-tests
        snow-shot-pinned-window-tests
        snow-shot-file-pin-batch-performance-benchmark)
    if(TARGET ${_conversion_view_target})
        target_sources(${_conversion_view_target} PRIVATE
            include/snow_shot/presentation/screenshotimageconversion.h
            include/snow_shot/presentation/screenshotimageconversionclipboard.h
            include/snow_shot/presentation/screenshotimageconversionview.h
            src/presentation/ocr/screenshotimageconversionview.cpp)
    endif()
endforeach()
foreach(_conversion_controller_target IN ITEMS snow_shot
        snow-shot-mcp-document-tests
        snow-shot-recognition-session-controller-tests snow-shot-translation-tool-tests
        snow-shot-pinned-window-tests
        snow-shot-file-pin-batch-performance-benchmark)
    if(TARGET ${_conversion_controller_target})
        target_sources(${_conversion_controller_target} PRIVATE
            include/snow_shot/presentation/components/screenshottranslationsettingsdialog.h
            src/presentation/components/screenshottranslationsettingsdialog.cpp
            include/snow_shot/presentation/screenshotimageconversioncontroller.h
            src/presentation/ocr/screenshotimageconversioncontroller.cpp)
        # Settings already owns these widget implementations and their Qt metaobjects.
        # Standalone conversion hosts compile them locally only without that library.
        get_target_property(_conversion_controller_libraries
            ${_conversion_controller_target} LINK_LIBRARIES)
        if(NOT "snow_shot_settings" IN_LIST _conversion_controller_libraries)
            target_sources(${_conversion_controller_target} PRIVATE
                include/snow_shot/presentation/components/formfields.h
                src/presentation/components/formfields.cpp
                include/snow_shot/presentation/components/pathinput.h
                src/presentation/components/pathinput.cpp)
            target_link_libraries(${_conversion_controller_target} PRIVATE
                snow_shot_settings_catalog)
            get_target_property(_dialog_host_sources ${_conversion_controller_target} SOURCES)
            if(NOT "${_dialog_host_sources}" MATCHES "styles/thememanager\\.cpp")
                target_sources(${_conversion_controller_target} PRIVATE
                    include/snow_shot/presentation/styles/thememanager.h
                    src/presentation/styles/thememanager.cpp
                    src/presentation/styles/themecolorscheme.cpp)
            endif()
        endif()
    endif()
endforeach()

# Standalone palette hosts need the same shared form fields as the scrolling settings dialog.
# Hosts linked to Settings already own their widget implementations and Qt metaobjects.
get_property(_palette_form_host_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_palette_form_host_target IN LISTS _palette_form_host_targets)
    get_target_property(_palette_form_host_sources ${_palette_form_host_target} SOURCES)
    if(NOT "${_palette_form_host_sources}" MATCHES "presentation/tools/screenshottoolpalette\\.cpp")
        continue()
    endif()
    get_target_property(_palette_form_host_libraries ${_palette_form_host_target} LINK_LIBRARIES)
    if(NOT "snow_shot_settings" IN_LIST _palette_form_host_libraries AND
       NOT "${_palette_form_host_sources}" MATCHES "presentation/components/formfields\\.cpp")
        target_sources(${_palette_form_host_target} PRIVATE
            include/snow_shot/presentation/components/formfields.h
            src/presentation/components/formfields.cpp
            include/snow_shot/presentation/components/pathinput.h
            src/presentation/components/pathinput.cpp)
        target_link_libraries(${_palette_form_host_target} PRIVATE snow_shot_settings_catalog)
    endif()
endforeach()

qt_finalize_executable(snow_shot)

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-auto-filter-tests
        tests/screenshot_auto_filter_tests.cpp
        src/presentation/tools/screenshotautofiltercontroller.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
        include/snow_shot/presentation/screenshotautofiltercontroller.h)
    target_include_directories(snow-shot-auto-filter-tests PRIVATE include
        "${SNOW_SHOT_SNOW_IMAGE_DIR}/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-visual-region-detector-c/include")
    target_link_libraries(snow-shot-auto-filter-tests PRIVATE snow_draw_engine_qt_canvas snow_visual_region_detector_c Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-auto-filter-tests)
    add_test(NAME snow-shot-auto-filter-tests COMMAND snow-shot-auto-filter-tests)
    set_tests_properties(snow-shot-auto-filter-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit;windows")
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_test(NAME snow-shot-auto-filter-toolbar-tests COMMAND snow-shot-screenshot-tool-palette-tests --auto-filter-only)
    set_tests_properties(snow-shot-auto-filter-toolbar-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit;windows")
endif()

foreach(_auto_filter_host_target IN ITEMS snow-shot-pinned-window-tests
        snow-shot-file-pin-batch-performance-benchmark)
    if(TARGET ${_auto_filter_host_target})
        target_sources(${_auto_filter_host_target} PRIVATE
            src/presentation/tools/screenshotautofiltercontroller.cpp
            include/snow_shot/presentation/screenshotautofiltercontroller.h)
        target_include_directories(${_auto_filter_host_target} PRIVATE
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-visual-region-detector-c/include")
        target_link_libraries(${_auto_filter_host_target} PRIVATE snow_visual_region_detector_c)
    endif()
endforeach()
if(SNOW_SHOT_BUILD_TESTS)
    add_test(NAME snow-shot-auto-filter-pinned-tests
        COMMAND snow-shot-pinned-window-tests --auto-filter-only -platform offscreen)
    set_tests_properties(snow-shot-auto-filter-pinned-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit;windows" TIMEOUT 60)
endif()

if(SNOW_SHOT_BUILD_TESTS)
    # These toolbar cases exercise Qt behavior on both Cocoa and Windows. Opt
    # them out of the repository's legacy name-based Windows label heuristic.
    set_tests_properties(
        snow-shot-pinned-toolbar-parity-tests
        snow-shot-pinned-toolbar-layout-tests
        snow-shot-interrupted-toolbar-drag-tests
        snow-shot-floating-toolbar-keyboard-focus-tests
        snow-shot-floating-toolbar-action-layout-tests
        snow-shot-floating-toolbar-recording-hit-test-tests
        snow-shot-toolbar-popup-recovery-tests
        snow-shot-toolbar-color-input-tests
        snow-shot-toolbar-shared-controls-tests
        snow-shot-scrolling-toolbar-tests
        snow-shot-screen-recording-toolbar-tests
        PROPERTIES SNOW_TEST_CROSS_PLATFORM TRUE)
    if(APPLE)
        set_tests_properties(snow-shot-pinned-toolbar-parity-tests
            snow-shot-pinned-toolbar-layout-tests PROPERTIES
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    endif()
endif()

if(APPLE)
    include(SnowShotMacOS)
    if(SNOW_SHOT_BUILD_TESTS AND SNOW_SHOT_RELEASE_STATIC)
        set(SNOW_X264_ROOT "${SNOW_FFMPEG_ROOT}")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../test-support/x264"
            "${CMAKE_CURRENT_BINARY_DIR}/x264-tests")
    endif()
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-pinned-placement-tests tests/pinned_placement_tests.cpp
        src/presentation/pinned/screenshotpinnednativegeometrycontroller.cpp
        src/presentation/pinned/screenshotpinnedresizegeometry.cpp)
    target_include_directories(snow-shot-pinned-placement-tests PRIVATE include src)
    target_link_libraries(snow-shot-pinned-placement-tests PRIVATE Qt6::Gui)
    add_test(NAME snow-shot-pinned-placement-tests COMMAND snow-shot-pinned-placement-tests)
    add_executable(snow-shot-feature-action-router-tests tests/feature_action_router_tests.cpp
        src/app/featureavailability.cpp)
    target_include_directories(snow-shot-feature-action-router-tests PRIVATE include)
    target_link_libraries(snow-shot-feature-action-router-tests PRIVATE Qt6::Gui)
    add_test(NAME snow-shot-feature-action-router-tests COMMAND snow-shot-feature-action-router-tests)
    set_tests_properties(snow-shot-feature-action-router-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE)
    add_test(NAME snow-shot-screenshot-controller-lifetime-tests
        COMMAND snow_shot --controller-lifetime-tests)
    set_tests_properties(snow-shot-screenshot-controller-lifetime-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-screenshot-controller-clipboard-tests
        COMMAND snow_shot --clipboard-pin-tests)
    set_tests_properties(snow-shot-screenshot-controller-clipboard-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-copy-after-pinning-tests
        COMMAND snow_shot --clipboard-pin-tests --copy-after-pin-tests)
    set_tests_properties(snow-shot-copy-after-pinning-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 90 RUN_SERIAL TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-pinned-controlled-interaction-tests
        COMMAND snow-shot-pinned-window-tests --controlled-interaction-only -platform offscreen)
    add_test(NAME snow-shot-pinned-pointer-routing-tests
        COMMAND snow-shot-pinned-window-tests --pointer-routing-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-controlled-interaction-tests PROPERTIES
        LABELS unit RUN_SERIAL TRUE TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    set_tests_properties(snow-shot-pinned-pointer-routing-tests PROPERTIES
        LABELS unit RUN_SERIAL TRUE TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE)
    set_tests_properties(snow-shot-pinned-movement-shortcut-tests PROPERTIES
        SNOW_TEST_CROSS_PLATFORM TRUE)
endif()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
    add_test(NAME snow-shot-macos-startup-tests COMMAND snow_shot --startup-probe)
    set_tests_properties(snow-shot-macos-startup-tests PROPERTIES
        LABELS "unit;macos" TIMEOUT 30)
    add_executable(snow-shot-macos-shell-policy-tests
        tests/macos_shell_policy_tests.cpp
        include/snow_shot/app/featureavailability.h
        src/app/featureavailability.cpp)
    target_include_directories(snow-shot-macos-shell-policy-tests PRIVATE include)
    target_link_libraries(snow-shot-macos-shell-policy-tests PRIVATE Qt6::Core Qt6::Gui)
    add_test(NAME snow-shot-macos-shell-policy-tests
        COMMAND snow-shot-macos-shell-policy-tests)
    set_tests_properties(snow-shot-macos-shell-policy-tests PROPERTIES
        LABELS "unit;macos" TIMEOUT 30)

    add_executable(snow-shot-macos-shell-native-tests
        tests/macos_shell_native_tests.mm
        include/snow_shot/platform/macos/applicationactivation.h
        src/platform/macos/applicationactivation.mm)
    target_include_directories(snow-shot-macos-shell-native-tests PRIVATE include)
    target_compile_definitions(snow-shot-macos-shell-native-tests PRIVATE
        SNOW_SHOT_MACOS_INFO_PLIST="${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in")
    target_link_libraries(snow-shot-macos-shell-native-tests PRIVATE
        snow_shot_login_item Qt6::Widgets "-framework AppKit")
    add_test(NAME snow-shot-macos-shell-native-tests
        COMMAND snow-shot-macos-shell-native-tests -platform cocoa)
    set_tests_properties(snow-shot-macos-shell-native-tests PROPERTIES
        LABELS "unit;macos" RUN_SERIAL TRUE TIMEOUT 30)

    add_executable(snow-shot-macos-pinned-window-tests
        tests/macos_pinned_window_tests.mm
        ${SNOW_PINNED_PLATFORM_SOURCES}
        src/presentation/services/mousereleaseactioncontroller.cpp)
    snow_shot_link_macos_window_platform(snow-shot-macos-pinned-window-tests)
    set_target_properties(snow-shot-macos-pinned-window-tests PROPERTIES
        MACOSX_BUNDLE TRUE MACOSX_BUNDLE_GUI_IDENTIFIER "top.snowshot.tests.pinned")
    target_include_directories(snow-shot-macos-pinned-window-tests PRIVATE include src)
    target_link_libraries(snow-shot-macos-pinned-window-tests PRIVATE Qt6::Widgets adqt::widgets
        "-framework AppKit" "-framework CoreGraphics")
    add_test(NAME snow-shot-pinned-auxiliary-ownership-tests
        COMMAND snow-shot-macos-pinned-window-tests --ownership-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-auxiliary-ownership-tests PROPERTIES
        LABELS "unit;macos" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 30)
    add_test(NAME snow-shot-macos-pinned-window-tests
        COMMAND snow-shot-macos-pinned-window-tests -platform cocoa)
    add_test(NAME snow-shot-macos-pinned-focus-tests
        COMMAND snow-shot-macos-pinned-window-tests --focus -platform cocoa)
    add_test(NAME snow-shot-macos-pinned-delivery-tests
        COMMAND snow-shot-macos-pinned-window-tests --delivery -platform cocoa)
    add_test(NAME snow-shot-macos-pinned-modal-delivery-tests
        COMMAND snow-shot-macos-pinned-window-tests --modal-delivery -platform cocoa)
    add_test(NAME snow-shot-macos-pinned-application-modal-delivery-tests
        COMMAND snow-shot-macos-pinned-window-tests --application-modal-delivery -platform cocoa)
    set_tests_properties(snow-shot-macos-pinned-window-tests snow-shot-macos-pinned-focus-tests snow-shot-macos-pinned-delivery-tests snow-shot-macos-pinned-modal-delivery-tests snow-shot-macos-pinned-application-modal-delivery-tests
        PROPERTIES LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()

if(SNOW_SHOT_BUILD_TESTS)
    set_tests_properties(snow-shot-pinned-window-repository-tests snow-shot-pinned-window-group-tests
        PROPERTIES SNOW_TEST_CROSS_PLATFORM TRUE)
endif()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
    # Shared behavior fixtures synthesize Qt input. Real Cocoa delivery and
    # native window policy are covered by the serial interactive targets above.
    set_tests_properties(${_snow_shot_pinned_window_test_names}
        snow-shot-pinned-save-dialog-tests
        snow-shot-pinned-hidden-text-selection-restore-tests
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" SNOW_TEST_CROSS_PLATFORM TRUE)
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_test(NAME snow-shot-pinned-physical-authority-tests
        COMMAND snow-shot-pinned-window-tests --physical-authority-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-physical-authority-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-pinned-passive-publication-tests
        COMMAND snow-shot-pinned-window-tests --passive-publication-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-passive-publication-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-clipboard-appearance-presentation-tests
        COMMAND snow-shot-pinned-window-tests --clipboard-appearance-only -platform offscreen)
    set_tests_properties(snow-shot-clipboard-appearance-presentation-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-decoded-content-presentation-tests
        COMMAND snow-shot-pinned-window-tests --decoded-content-only -platform offscreen)
    set_tests_properties(snow-shot-decoded-content-presentation-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-pinned-color-space-tests
        COMMAND snow-shot-pinned-window-tests --color-space-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-color-space-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_test(NAME snow-shot-history-selection-presentation-tests
        COMMAND snow-shot-pinned-window-tests --history-selection-only -platform offscreen)
    set_tests_properties(snow-shot-history-selection-presentation-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
endif()

if(WIN32 AND SNOW_SHOT_BUILD_TESTS)
    # Qt does not activate a screen-factor override of 1 by itself. Use an
    # explicit 2x screen factor and a global multiplier to test exact DPRs,
    # independently of the host monitor's configured scale.
    set(_pin_dprs 1.0 1.25 1.5 1.75 2.0)
    set(_pin_factors 0.5 0.625 0.75 0.875 1.0)
    foreach(_pin_scale _pin_factor IN ZIP_LISTS _pin_dprs _pin_factors)
        add_test(NAME snow-shot-pinned-small-extent-${_pin_scale}-tests
            COMMAND snow-shot-pinned-window-tests --small-extent-only -platform windows)
        set_tests_properties(snow-shot-pinned-small-extent-${_pin_scale}-tests PROPERTIES
            LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 30
            ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_SCREEN_SCALE_FACTORS=2;QT_SCALE_FACTOR=${_pin_factor};SNOW_PIN_TEST_DPR=${_pin_scale}")
        add_test(NAME snow-shot-pinned-pixel-alignment-${_pin_scale}-tests
            COMMAND snow-shot-pinned-window-tests --pixel-alignment-only -platform windows)
        set_tests_properties(snow-shot-pinned-pixel-alignment-${_pin_scale}-tests PROPERTIES
            LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_SCREEN_SCALE_FACTORS=2;QT_SCALE_FACTOR=${_pin_factor};SNOW_PIN_TEST_DPR=${_pin_scale}")
        add_test(NAME snow-shot-pinned-selection-content-alignment-${_pin_scale}-tests
            COMMAND snow-shot-pinned-window-tests --selection-content-alignment-only -platform windows)
        set_tests_properties(snow-shot-pinned-selection-content-alignment-${_pin_scale}-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_SCREEN_SCALE_FACTORS=2;QT_SCALE_FACTOR=${_pin_factor};SNOW_PIN_TEST_DPR=${_pin_scale}")
        add_test(NAME snow-shot-pinned-native-transparent-edges-${_pin_scale}-tests
            COMMAND snow-shot-pinned-window-tests --native-transparent-edges-only -platform windows)
        set_tests_properties(snow-shot-pinned-native-transparent-edges-${_pin_scale}-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_SCREEN_SCALE_FACTORS=2;QT_SCALE_FACTOR=${_pin_factor};SNOW_PIN_TEST_DPR=${_pin_scale}")
    endforeach()
    # A deterministic capture fixture published through the native layered
    # window. Also run at real monitor DPI, without inherited Qt overrides.
    add_test(NAME snow-shot-pinned-selection-content-alignment-tests
        COMMAND snow-shot-pinned-window-tests --selection-content-alignment-only -platform windows)
    set_tests_properties(snow-shot-pinned-selection-content-alignment-tests PROPERTIES
        LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=windows"
        ENVIRONMENT_MODIFICATION "QT_SCREEN_SCALE_FACTORS=unset:;QT_SCALE_FACTOR=unset:;SNOW_PIN_TEST_DPR=unset:")
endif()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
    add_test(NAME snow-shot-pinned-safe-area-tests
        COMMAND snow-shot-pinned-window-tests --safe-area-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-safe-area-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE)
    add_test(NAME snow-shot-pinned-retina-safe-area-tests
        COMMAND snow-shot-pinned-window-tests --safe-area-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-retina-safe-area-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE ENVIRONMENT "QT_SCALE_FACTOR=2" RUN_SERIAL TRUE)
    add_test(NAME snow-shot-pinned-pixel-alignment-tests
        COMMAND snow-shot-pinned-window-tests --pixel-alignment-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-pixel-alignment-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE ENVIRONMENT "QT_SCALE_FACTOR=2" RUN_SERIAL TRUE)
    add_test(NAME snow-shot-pinned-retina-interaction-tests
        COMMAND snow-shot-pinned-window-tests --controlled-interaction-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-retina-interaction-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE ENVIRONMENT "QT_SCALE_FACTOR=2" RUN_SERIAL TRUE)
    add_test(NAME snow-shot-pinned-creation-tests
        COMMAND snow-shot-pinned-window-tests --creation-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-creation-tests PROPERTIES
        LABELS "unit;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-macos-pinned-creation-tests
        COMMAND snow-shot-pinned-window-tests --creation-only -platform cocoa)
    set_tests_properties(snow-shot-macos-pinned-creation-tests PROPERTIES
        LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-macos-pinned-drag-tests
        COMMAND snow-shot-pinned-window-tests --native-controlled-drag-only -platform cocoa)
    set_tests_properties(snow-shot-macos-pinned-drag-tests PROPERTIES
        LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE
        TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME snow-shot-macos-pinned-pixel-alignment-tests
        COMMAND snow-shot-pinned-window-tests --pixel-alignment-only -platform cocoa)
    set_tests_properties(snow-shot-macos-pinned-pixel-alignment-tests PROPERTIES
        LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60)
endif()

if(SNOW_SHOT_BUILD_TESTS)
    set_tests_properties(snow-shot-pinned-recognition-context-menu-tests
        snow-shot-image-conversion-pinned-tests snow-shot-auto-filter-pinned-tests
        PROPERTIES SNOW_TEST_CROSS_PLATFORM TRUE LABELS unit)
    if(APPLE)
        set_tests_properties(snow-shot-image-conversion-pinned-tests
            snow-shot-pinned-quick-save-tests
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" SNOW_TEST_CROSS_PLATFORM TRUE LABELS unit)
    endif()
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-selection-render-tests
        tests/screenshot_selection_render_tests.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/services/screenshotsourceimagecomposer.cpp)
    if(WIN32)
        target_sources(snow-shot-selection-render-tests PRIVATE
            src/platform/windows/monitorgeometry.cpp)
        target_link_libraries(snow-shot-selection-render-tests PRIVATE user32)
    endif()
    target_include_directories(snow-shot-selection-render-tests PRIVATE include
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
    target_link_libraries(snow-shot-selection-render-tests PRIVATE Qt6::Widgets snow_canvas_image)
    snow_shot_import_offscreen_platform(snow-shot-selection-render-tests)
    add_test(NAME snow-shot-selection-render-tests COMMAND snow-shot-selection-render-tests)
    set_tests_properties(snow-shot-selection-render-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit")
endif()

if(WIN32)
    add_library(snow_shot_capture_exclusion_composition STATIC
        src/platform/windows/captureexclusioncomposition.cpp)
    target_link_libraries(snow_shot_capture_exclusion_composition
        PUBLIC Qt6::Widgets PRIVATE Qt6::GuiPrivate user32 gdi32)

    snow_shot_register_source_capability(capture_exclusion_composition
        FULL_TARGET snow_shot_capture_exclusion_composition
        MINI_TARGET snow_shot_capture_exclusion_composition)
    snow_shot_source_requires(capture_exclusion_composition SOURCES
        src/platform/windows/windowchrome.cpp
        tests/window_capture_exclusion_tests.cpp)
endif()

# Overlay and recording hosts share widget controls and their custom icon pack.
if(WIN32 AND SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-capture-composition-tests tests/capture_composition_tests.cpp)
    target_link_libraries(snow-shot-capture-composition-tests PRIVATE
        snow_shot_capture_exclusion_composition Qt6::GuiPrivate)
    snow_shot_import_offscreen_platform(snow-shot-capture-composition-tests)
    add_test(NAME snow-shot-capture-composition-tests
        COMMAND snow-shot-capture-composition-tests -platform offscreen)
    set_tests_properties(snow-shot-capture-composition-tests PROPERTIES LABELS "unit;windows")
    foreach(_scale IN ITEMS 1 1.25 1.5 2)
        add_test(NAME snow-shot-capture-composition-native-${_scale}-tests
            COMMAND snow-shot-capture-composition-tests --native -platform windows)
        set_tests_properties(snow-shot-capture-composition-native-${_scale}-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 30
            ENVIRONMENT "QT_SCALE_FACTOR=${_scale}")
    endforeach()
    add_test(NAME snow-shot-scrolling-toolbar-capture-composition-tests
        COMMAND snow-shot-screenshot-toolbar-remembered-tool-tests --scrolling-composition-only
            -platform windows)
    set_tests_properties(snow-shot-scrolling-toolbar-capture-composition-tests PROPERTIES
        LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 30)
    foreach(_kind IN ITEMS screenshot recording)
        if(_kind STREQUAL "screenshot")
            set(_toolbar_test snow-shot-screenshot-toolbar-remembered-tool-tests)
            target_sources(${_toolbar_test} PRIVATE src/platform/windows/windowchrome.cpp)
        else()
            set(_toolbar_test snow-shot-screen-recording-controller-tests)
        endif()
        target_sources(${_toolbar_test} PRIVATE tests/toolbar_capture_exclusion_native_tests.cpp)
        target_link_libraries(${_toolbar_test} PRIVATE snow_capture_c Qt6::GuiPrivate dwmapi user32 psapi)
        target_include_directories(${_toolbar_test} PRIVATE
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
        add_test(NAME snow-shot-${_kind}-toolbar-capture-exclusion-native-tests
            COMMAND ${_toolbar_test} --native-capture-exclusion-only -platform windows)
        set_tests_properties(snow-shot-${_kind}-toolbar-capture-exclusion-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 120)
        add_test(NAME snow-shot-${_kind}-toolbar-capture-exclusion-affinity-tests
            COMMAND ${_toolbar_test} --native-capture-exclusion-affinity-only -platform windows)
        set_tests_properties(snow-shot-${_kind}-toolbar-capture-exclusion-affinity-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60)
        if(SNOW_APPS_BUILD_BENCHMARKS)
            add_test(NAME snow-shot-${_kind}-toolbar-composition-memory
                COMMAND ${_toolbar_test} --native-capture-exclusion-affinity-only --measure-memory
                    -platform windows)
            set_tests_properties(snow-shot-${_kind}-toolbar-composition-memory PROPERTIES
                LABELS "benchmark;interactive;windows" RUN_SERIAL TRUE TIMEOUT 180)
            add_test(NAME snow-shot-${_kind}-toolbar-capture-exclusion-memory
                COMMAND ${_toolbar_test} --native-capture-exclusion-only --measure-memory
                    -platform windows)
            set_tests_properties(snow-shot-${_kind}-toolbar-capture-exclusion-memory PROPERTIES
                LABELS "benchmark;interactive;windows" RUN_SERIAL TRUE TIMEOUT 300)
        endif()
    endforeach()
endif()

get_property(_overlay_widget_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _overlay_widget_targets)
    get_target_property(_sources ${_target} SOURCES)
    if(_sources MATCHES "screenrecordingareawindow.cpp")
        target_sources(${_target} PRIVATE
            src/presentation/recording/recordingwebcamcontrol.cpp
            src/presentation/recording/recordingwebcamcontrol.h
            src/presentation/recording/recordingregiondraghandle.cpp
            src/presentation/recording/recordingregiondraghandle.h
            src/presentation/recording/recordingregioninputrouter.cpp
            src/presentation/recording/recordingregioninputrouter.h)
    endif()
    if(WIN32 AND _sources MATCHES "screenshotoverlaywindow.cpp" AND
       NOT _sources MATCHES "src/platform/windows/windowchrome.cpp")
        target_sources(${_target} PRIVATE src/platform/windows/windowchrome.cpp)
        target_link_libraries(${_target} PRIVATE dwmapi user32)
    endif()
    get_target_property(_sources ${_target} SOURCES)
    if(NOT "${_target}" STREQUAL "snow_shot" AND
       _sources MATCHES "(screenshotoverlaywindow|screenrecordingareawindow).cpp" AND
       NOT _sources MATCHES "snowshoticons.cpp")
        target_sources(${_target} PRIVATE src/presentation/components/icons/snowshoticons.cpp)
    endif()
endforeach()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-macos-raster-color-space-tests
        tests/macos_raster_color_space_tests.mm)
    target_include_directories(snow-shot-macos-raster-color-space-tests PRIVATE include)
    target_link_libraries(snow-shot-macos-raster-color-space-tests PRIVATE Qt6::Widgets
        "-framework AppKit")
    snow_shot_import_offscreen_platform(snow-shot-macos-raster-color-space-tests)
    add_test(NAME snow-shot-raster-color-space-tests
        COMMAND snow-shot-macos-raster-color-space-tests -platform offscreen)
    set_tests_properties(snow-shot-raster-color-space-tests PROPERTIES LABELS "unit;macos")
    add_test(NAME snow-shot-macos-raster-color-space-tests
        COMMAND snow-shot-macos-raster-color-space-tests -platform cocoa)
    set_tests_properties(snow-shot-macos-raster-color-space-tests PROPERTIES LABELS "unit;macos")

    add_executable(snow-shot-macos-screenshot-window-target-tests
        tests/macos_screenshot_window_target_tests.mm)
    target_link_libraries(snow-shot-macos-screenshot-window-target-tests PRIVATE Qt6::Core
        "-framework Foundation" "-framework CoreGraphics")
    add_test(NAME snow-shot-macos-screenshot-window-target-tests
        COMMAND snow-shot-macos-screenshot-window-target-tests)
    set_tests_properties(snow-shot-macos-screenshot-window-target-tests PROPERTIES LABELS "unit;macos")

    add_executable(snow-shot-macos-screenshot-native-smoke
        tests/macos_screenshot_native_smoke.mm
        src/presentation/capture/directcapturenative.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/services/screenshotsourceimagecomposer.cpp)
    snow_shot_link_macos_window_platform(snow-shot-macos-screenshot-native-smoke)
    target_include_directories(snow-shot-macos-screenshot-native-smoke PRIVATE include
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
    target_link_libraries(snow-shot-macos-screenshot-native-smoke PRIVATE Qt6::Widgets snow_capture_c snow_canvas_image
        snow_image::snow_image_static
        "-framework AppKit" "-framework ApplicationServices")
    add_test(NAME snow-shot-macos-screenshot-native-smoke COMMAND snow-shot-macos-screenshot-native-smoke)
    set_tests_properties(snow-shot-macos-screenshot-native-smoke PROPERTIES LABELS "interactive;macos" SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-macos-window-shortcut-input-tests
        tests/macos_window_shortcut_input_tests.mm)
    target_link_libraries(snow-shot-macos-window-shortcut-input-tests PRIVATE
        snow_shot_window_shortcuts "-framework AppKit")
    snow_shot_import_offscreen_platform(snow-shot-macos-window-shortcut-input-tests)
    add_test(NAME snow-shot-macos-window-shortcut-input-tests
        COMMAND snow-shot-macos-window-shortcut-input-tests -platform offscreen)
    set_tests_properties(snow-shot-macos-window-shortcut-input-tests PROPERTIES
        LABELS "unit;macos" TIMEOUT 30)

    add_executable(snow-shot-macos-region-shortcut-native-tests
        tests/macos_region_shortcut_native_tests.mm)
    target_include_directories(snow-shot-macos-region-shortcut-native-tests PRIVATE include)
    target_link_libraries(snow-shot-macos-region-shortcut-native-tests PRIVATE
        snow_shot_window_shortcuts Qt6::Widgets "-framework AppKit"
        "-framework ApplicationServices")
    add_test(NAME snow-shot-macos-region-shortcut-native-tests
        COMMAND snow-shot-macos-region-shortcut-native-tests)
    set_tests_properties(snow-shot-macos-region-shortcut-native-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa" LABELS "interactive;macos"
        RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 30)

    add_executable(snow-shot-macos-recapture-native-tests
        tests/macos_recapture_native_tests.mm
        src/platform/macos/applicationactivation.mm)
    snow_shot_link_macos_window_platform(snow-shot-macos-recapture-native-tests)
    target_include_directories(snow-shot-macos-recapture-native-tests PRIVATE include
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
    target_link_libraries(snow-shot-macos-recapture-native-tests PRIVATE Qt6::Widgets snow_capture_c
        snow_shot_login_item
        "-framework AppKit" "-framework ApplicationServices")
    add_test(NAME snow-shot-macos-recapture-native-tests COMMAND snow-shot-macos-recapture-native-tests)
    set_tests_properties(snow-shot-macos-recapture-native-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa" LABELS "interactive;macos"
        RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 30)
    foreach(target_kind IN ITEMS floating local local-floating)
        add_test(NAME snow-shot-macos-recapture-${target_kind}-tests
            COMMAND snow-shot-macos-recapture-native-tests --${target_kind})
        set_tests_properties(snow-shot-macos-recapture-${target_kind}-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=cocoa" LABELS "interactive;macos"
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 30)
    endforeach()

    add_executable(snow-shot-macos-recapture-focus-tests tests/macos_recapture_focus_tests.cpp)
    target_include_directories(snow-shot-macos-recapture-focus-tests PRIVATE include)
    target_link_libraries(snow-shot-macos-recapture-focus-tests PRIVATE Qt6::Core)
    add_test(NAME snow-shot-macos-recapture-focus-tests COMMAND snow-shot-macos-recapture-focus-tests)
    set_tests_properties(snow-shot-macos-recapture-focus-tests PROPERTIES LABELS "unit;macos")

    add_executable(snow-shot-macos-scrolling-input-tests
        tests/macos_scrolling_input_tests.mm)
    snow_shot_link_macos_window_platform(snow-shot-macos-scrolling-input-tests)
    target_include_directories(snow-shot-macos-scrolling-input-tests PRIVATE include)
    target_link_libraries(snow-shot-macos-scrolling-input-tests PRIVATE Qt6::Widgets
        "-framework AppKit" "-framework ApplicationServices")
    add_test(NAME snow-shot-macos-scrolling-input-tests
        COMMAND snow-shot-macos-scrolling-input-tests)
    set_tests_properties(snow-shot-macos-scrolling-input-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit;macos")
    add_test(NAME snow-shot-macos-scrolling-input-native-tests
        COMMAND snow-shot-macos-scrolling-input-tests -platform cocoa)
    set_tests_properties(snow-shot-macos-scrolling-input-native-tests PROPERTIES
        LABELS "interactive;macos" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 30)

    add_executable(snow-shot-macos-screenshot-window-release-tests
        tests/macos_screenshot_window_release_tests.mm)
    snow_shot_link_macos_window_platform(snow-shot-macos-screenshot-window-release-tests)
    target_include_directories(snow-shot-macos-screenshot-window-release-tests PRIVATE include)
    target_link_libraries(snow-shot-macos-screenshot-window-release-tests PRIVATE Qt6::Widgets
        "-framework AppKit" "-framework QuartzCore" "-framework IOSurface")
    add_test(NAME snow-shot-macos-screenshot-window-release-tests
        COMMAND snow-shot-macos-screenshot-window-release-tests -platform offscreen)
    set_tests_properties(snow-shot-macos-screenshot-window-release-tests PROPERTIES
        LABELS "unit;macos" TIMEOUT 30)
    add_test(NAME snow-shot-macos-screenshot-window-release-native-tests
        COMMAND snow-shot-macos-screenshot-window-release-tests -platform cocoa)
    set_tests_properties(snow-shot-macos-screenshot-window-release-native-tests PROPERTIES
        LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 30)

    add_executable(snow-shot-macos-overlay-initialization-tests
        tests/macos_overlay_initialization_tests.mm
        ${SNOW_PINNED_PLATFORM_SOURCES}
        src/presentation/core/screenshotgeometry.cpp)
    snow_shot_link_macos_window_platform(snow-shot-macos-overlay-initialization-tests)
    target_include_directories(snow-shot-macos-overlay-initialization-tests PRIVATE include src)
    target_link_libraries(snow-shot-macos-overlay-initialization-tests PRIVATE ant_design_qt Qt6::Widgets
        "-framework AppKit" "-framework ApplicationServices")
    add_test(NAME snow-shot-macos-overlay-initialization-tests
        COMMAND snow-shot-macos-overlay-initialization-tests)
    set_tests_properties(snow-shot-macos-overlay-initialization-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit;macos")
    add_test(NAME snow-shot-macos-native-view-lifecycle-tests
        COMMAND snow-shot-macos-overlay-initialization-tests --native-view-lifecycle-only -platform offscreen)
    set_tests_properties(snow-shot-macos-native-view-lifecycle-tests PROPERTIES LABELS "unit;macos")
    add_test(NAME snow-shot-macos-native-view-lifecycle-native-tests
        COMMAND snow-shot-macos-overlay-initialization-tests --native-view-lifecycle-only -platform cocoa)
    set_tests_properties(snow-shot-macos-native-view-lifecycle-native-tests PROPERTIES
        LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 30)
    add_test(NAME snow-shot-macos-overlay-animation-tests
        COMMAND snow-shot-macos-overlay-initialization-tests --overlay-animation-only -platform offscreen)
    set_tests_properties(snow-shot-macos-overlay-animation-tests PROPERTIES LABELS "unit;macos")
    add_test(NAME snow-shot-macos-overlay-animation-native-tests
        COMMAND snow-shot-macos-overlay-initialization-tests --overlay-animation-only -platform cocoa)
    set_tests_properties(snow-shot-macos-overlay-animation-native-tests PROPERTIES
        LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 30)
    add_test(NAME snow-shot-macos-recognition-stacking-native-tests
        COMMAND snow-shot-macos-overlay-initialization-tests --recognition-stacking-only -platform cocoa)
    set_tests_properties(snow-shot-macos-recognition-stacking-native-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa" LABELS "interactive;macos"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-macos-screenshot-stacking-native-tests
        COMMAND snow-shot-macos-overlay-initialization-tests -platform cocoa)
    set_tests_properties(snow-shot-macos-screenshot-stacking-native-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa" LABELS "interactive;macos"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-macos-modal-stacking-tests
        COMMAND snow-shot-macos-overlay-initialization-tests --modal-stacking-only -platform offscreen)
    set_tests_properties(snow-shot-macos-modal-stacking-tests PROPERTIES LABELS "unit;macos" TIMEOUT 60)
    add_test(NAME snow-shot-macos-modal-stacking-native-tests
        COMMAND snow-shot-macos-overlay-initialization-tests --modal-stacking-only -platform cocoa)
    set_tests_properties(snow-shot-macos-modal-stacking-native-tests PROPERTIES
        LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-macos-file-panel-stacking-tests
        COMMAND snow-shot-macos-overlay-initialization-tests --native-file-panels-only -platform offscreen)
    set_tests_properties(snow-shot-macos-file-panel-stacking-tests PROPERTIES
        LABELS "unit;macos" TIMEOUT 60)
    add_test(NAME snow-shot-macos-file-panel-stacking-native-tests
        COMMAND snow-shot-macos-overlay-initialization-tests --native-file-panels-only -platform cocoa)
    set_tests_properties(snow-shot-macos-file-panel-stacking-native-tests PROPERTIES
        LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60)
endif()

# Physical cursor warps enter Qt's platform input path so Qt's pointer state and
# widget grabs remain consistent with subsequent native mouse events.
foreach(_cursor_host IN ITEMS snow_shot snow-shot-physical-cursor-tests snow-shot-color-picker-lifecycle-tests
        snow-shot-pinned-window-tests snow-shot-file-pin-batch-performance-benchmark)
    if(TARGET ${_cursor_host})
        target_link_libraries(${_cursor_host} PRIVATE Qt6::GuiPrivate)
    endif()
endforeach()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-application-qos-tests tests/application_qos_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-application-qos-tests PRIVATE snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-application-qos-tests)
    foreach(_snow_qos IN ITEMS user_interactive user_initiated default utility background invalid missing)
        add_test(NAME snow-shot-application-qos-${_snow_qos}-tests COMMAND snow-shot-application-qos-tests ${_snow_qos})
        set_tests_properties(snow-shot-application-qos-${_snow_qos}-tests PROPERTIES LABELS "unit;macos" TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    unset(_snow_qos)
endif()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-login-item-settings-tests tests/login_item_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-login-item-settings-tests PRIVATE snow_shot_settings snow_shot_image_codec)
    snow_shot_import_offscreen_platform(snow-shot-login-item-settings-tests)
    snow_shot_add_translations(snow-shot-login-item-settings-tests)
    add_test(NAME snow-shot-login-item-settings-tests COMMAND snow-shot-login-item-settings-tests)
    set_tests_properties(snow-shot-login-item-settings-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit;macos")
endif()
if(APPLE AND SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-login-item-native-tests tests/login_item_native_tests.mm)
    target_link_libraries(snow-shot-login-item-native-tests PRIVATE snow_shot_login_item
        "-framework ServiceManagement")
    add_test(NAME snow-shot-login-item-native-tests COMMAND snow-shot-login-item-native-tests)
    set_tests_properties(snow-shot-login-item-native-tests PROPERTIES LABELS "unit;macos")
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-qr-controller-tests
        tests/screenshot_qr_controller_tests.cpp
        src/presentation/ocr/screenshotqrcontroller.cpp
        include/snow_shot/presentation/screenshotqrcontroller.h
        src/presentation/components/icons/snowshoticons.cpp)
    target_include_directories(snow-shot-qr-controller-tests PRIVATE include)
    target_link_libraries(snow-shot-qr-controller-tests PRIVATE snow_shot_shortcuts snow_draw_engine_qt_canvas
        ant_design_qt Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-qr-controller-tests)
    add_test(NAME snow-shot-qr-controller-tests COMMAND snow-shot-qr-controller-tests)
    if(WIN32)
        add_test(NAME snow-shot-qr-stacking-native-tests
            COMMAND snow-shot-qr-controller-tests --stacking-only -platform windows)
        set_tests_properties(snow-shot-qr-stacking-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE)
    endif()
    set_tests_properties(snow-shot-qr-controller-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit")
endif()

if(TARGET snow-shot-recognition-session-controller-tests)
    target_sources(snow-shot-recognition-session-controller-tests PRIVATE
        tests/screenshot_recognition_model_tests.cpp)
    snow_shot_add_translations(snow-shot-recognition-session-controller-tests)
    add_test(NAME snow-shot-recognition-model-session-tests
        COMMAND snow-shot-recognition-session-controller-tests --recognition-model-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-model-session-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-latex-recognition-tests COMMAND snow-shot-recognition-session-controller-tests --latex-only -platform offscreen)
    set_tests_properties(snow-shot-latex-recognition-tests PROPERTIES LABELS "unit" TIMEOUT 60)
endif()

if(TARGET snow-shot-pinned-window-tests)
    add_test(NAME snow-shot-pinned-template-dialog-tests
        COMMAND snow-shot-pinned-window-tests --template-dialogs-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-template-dialog-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-latex-tests COMMAND snow-shot-pinned-window-tests --latex-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-latex-tests PROPERTIES LABELS "unit" TIMEOUT 60)
endif()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-macos-cursor-tests tests/macos_cursor_tests.cpp)
    target_link_libraries(snow-shot-macos-cursor-tests PRIVATE snow_shot_macos_cursor)
    snow_shot_import_offscreen_platform(snow-shot-macos-cursor-tests)
    add_test(NAME snow-shot-macos-cursor-tests COMMAND snow-shot-macos-cursor-tests -platform offscreen)
    set_tests_properties(snow-shot-macos-cursor-tests PROPERTIES LABELS "unit;macos")

    add_executable(snow-shot-macos-cursor-native-tests tests/macos_cursor_native_tests.mm
        ../cmake/test-support/macos_native_input.mm)
    snow_shot_link_macos_window_platform(snow-shot-macos-cursor-native-tests)
    target_include_directories(snow-shot-macos-cursor-native-tests PRIVATE include src)
    target_link_libraries(snow-shot-macos-cursor-native-tests PRIVATE snow_shot_macos_cursor
        "-framework AppKit" "-framework ApplicationServices")
    add_test(NAME snow-shot-macos-cursor-native-tests
        COMMAND snow-shot-macos-cursor-native-tests -platform cocoa)
    set_tests_properties(snow-shot-macos-cursor-native-tests PROPERTIES
        LABELS "interactive;macos" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

if(TARGET snow-shot-pinned-window-tests)
    add_test(NAME snow-shot-pinned-drag-export-tests
        COMMAND snow-shot-pinned-window-tests --drag-export-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-drag-export-tests PROPERTIES LABELS "unit")
    if(WIN32)
        add_test(NAME snow-shot-pinned-ctrl-hover-tests
            COMMAND snow-shot-pinned-window-tests --ctrl-hover-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-ctrl-hover-tests PROPERTIES LABELS "unit;windows")
        add_test(NAME snow-shot-pinned-drag-export-native-tests
            COMMAND snow-shot-pinned-window-tests --drag-export-native-only -platform windows)
        set_tests_properties(snow-shot-pinned-drag-export-native-tests PROPERTIES LABELS "unit;windows")
    endif()
endif()

if(SNOW_SHOT_BUILD_TESTS)
    snow_shot_offscreen_qpa_environment(_snow_clipboard_qpa_environment)
    add_executable(snow-shot-clipboard-lifecycle-tests
        tests/screenshot_clipboard_lifecycle_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_include_directories(snow-shot-clipboard-lifecycle-tests PRIVATE include)
    target_link_libraries(snow-shot-clipboard-lifecycle-tests PRIVATE
        snow_shot_image_codec snow_shot_diagnostics snow_shot_clipboard_files Qt6::Widgets)
    if(WIN32)
        target_link_libraries(snow-shot-clipboard-lifecycle-tests PRIVATE user32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-clipboard-lifecycle-tests)
    add_test(NAME snow-shot-clipboard-lifecycle-tests COMMAND snow-shot-clipboard-lifecycle-tests)
    set_tests_properties(snow-shot-clipboard-lifecycle-tests PROPERTIES
        ENVIRONMENT "${_snow_clipboard_qpa_environment}" LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE)

    if(TARGET snow-shot-pinned-window-tests)
        add_test(NAME snow-shot-selection-clipboard-lifecycle-tests
            COMMAND snow-shot-pinned-window-tests --clipboard-publication-only -platform offscreen)
        set_tests_properties(snow-shot-selection-clipboard-lifecycle-tests PROPERTIES
            ENVIRONMENT "${_snow_clipboard_qpa_environment}" LABELS "unit" TIMEOUT 60)
    endif()
    if(Python3_Interpreter_FOUND AND SNOW_SHOT_ENABLE_MCP)
        add_test(NAME snow-shot-mcp-media-clipboard-tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_application_fixture_tests.py"
                "$<TARGET_FILE:snow_shot_mcp>" "$<TARGET_FILE:snow_shot>" --clipboard-only)
        set_tests_properties(snow-shot-mcp-media-clipboard-tests PROPERTIES TIMEOUT 120
            LABELS "unit" RUN_SERIAL TRUE ENVIRONMENT "${_snow_clipboard_qpa_environment}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        add_test(NAME snow-shot-mcp-animation-tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_application_fixture_tests.py"
                "$<TARGET_FILE:snow_shot_mcp>" "$<TARGET_FILE:snow_shot>" --animation-only)
        set_tests_properties(snow-shot-mcp-animation-tests PROPERTIES TIMEOUT 120
            LABELS "unit" RUN_SERIAL TRUE ENVIRONMENT "${_snow_clipboard_qpa_environment}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    if(APPLE)
        add_executable(snow-shot-macos-clipboard-roundtrip-tests
            tests/macos_clipboard_roundtrip_tests.mm
            src/presentation/services/screenshotclipboardservice.cpp
            src/presentation/services/screenshotclipboardcontent.cpp
            src/presentation/services/pinnedcolorswatch.cpp)
        target_include_directories(snow-shot-macos-clipboard-roundtrip-tests PRIVATE include)
        target_link_libraries(snow-shot-macos-clipboard-roundtrip-tests PRIVATE
            snow_shot_image_codec snow_shot_diagnostics Qt6::Widgets
            "-framework AppKit" "-framework ImageIO")
        add_test(NAME snow-shot-macos-clipboard-roundtrip-tests
            COMMAND snow-shot-macos-clipboard-roundtrip-tests -platform cocoa)
        set_tests_properties(snow-shot-macos-clipboard-roundtrip-tests PROPERTIES
            LABELS "unit;macos" RUN_SERIAL TRUE TIMEOUT 60)
        add_test(NAME snow-shot-macos-html-clipboard-tests
            COMMAND snow-shot-macos-clipboard-roundtrip-tests --html-only -platform cocoa)
        set_tests_properties(snow-shot-macos-html-clipboard-tests PROPERTIES
            LABELS "unit;macos" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    unset(_snow_clipboard_qpa_environment)
endif()

# History copying shares the same placement and outline calculations as direct pinning.
add_library(snow_shot_history_pin STATIC
    src/platform/windows/monitorgeometry.cpp
    src/presentation/services/historypinplacement.cpp
    src/presentation/services/screenshotselectionpin.cpp
    src/presentation/core/screenshotgeometry.cpp
    src/presentation/overlay/screenshotselectionshadowrenderer.cpp)
target_include_directories(snow_shot_history_pin PUBLIC include)
target_link_libraries(snow_shot_history_pin PUBLIC Qt6::Widgets PRIVATE ant_design_qt snow_canvas_image)
if(APPLE)
    target_link_libraries(snow_shot_history_pin PRIVATE "-framework AppKit")
elseif(WIN32)
    target_link_libraries(snow_shot_history_pin PRIVATE user32)
endif()
target_link_libraries(snow_shot_settings PRIVATE snow_shot_history_pin snow_shot_clipboard_placement)

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-clipboard-file-cache-tests tests/clipboard_file_cache_tests.cpp)
    target_link_libraries(snow-shot-clipboard-file-cache-tests PRIVATE snow_shot_clipboard_files)
    add_test(NAME snow-shot-clipboard-file-cache-tests COMMAND snow-shot-clipboard-file-cache-tests)
    set_tests_properties(snow-shot-clipboard-file-cache-tests PROPERTIES LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

# Shared clipboard metadata must have one native MIME converter per process.
add_library(snow_shot_clipboard_placement STATIC
    src/presentation/services/screenshotclipboardplacement.cpp
    src/presentation/services/screenshotclipboardappearance.cpp)
target_include_directories(snow_shot_clipboard_placement PUBLIC include)
target_link_libraries(snow_shot_clipboard_placement PUBLIC Qt6::Gui)
if(APPLE)
    target_sources(snow_shot_clipboard_placement PRIVATE src/platform/macos/clipboardrevision.mm)
    target_link_libraries(snow_shot_clipboard_placement PRIVATE "-framework AppKit")
elseif(WIN32)
    target_link_libraries(snow_shot_clipboard_placement PRIVATE user32)
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-clipboard-placement-tests
        tests/screenshot_clipboard_placement_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/pinnedcolorswatch.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp)
    target_include_directories(snow-shot-clipboard-placement-tests PRIVATE include src/image)
    target_link_libraries(snow-shot-clipboard-placement-tests PRIVATE
        snow_shot_clipboard_placement snow_shot_image_codec snow_shot_diagnostics Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-clipboard-placement-tests)
    add_test(NAME snow-shot-clipboard-placement-tests COMMAND snow-shot-clipboard-placement-tests)
    snow_shot_offscreen_qpa_environment(_snow_clipboard_placement_qpa_environment)
    set_tests_properties(snow-shot-clipboard-placement-tests PROPERTIES
        LABELS "unit" ENVIRONMENT "${_snow_clipboard_placement_qpa_environment}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    unset(_snow_clipboard_placement_qpa_environment)
    if(APPLE OR WIN32)
        add_test(NAME snow-shot-clipboard-placement-native-tests
            COMMAND snow-shot-clipboard-placement-tests --native-roundtrip)
        set_tests_properties(snow-shot-clipboard-placement-native-tests PROPERTIES
            LABELS "interactive" RUN_SERIAL TRUE TIMEOUT 60)
        if(APPLE)
            set_tests_properties(snow-shot-clipboard-placement-native-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
        else()
            target_link_libraries(snow-shot-clipboard-placement-tests PRIVATE user32)
            set_tests_properties(snow-shot-clipboard-placement-native-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        endif()
    endif()
endif()

if(SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-clipboard-placement-benchmark
        tests/screenshot_clipboard_placement_benchmark.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/pinnedcolorswatch.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp)
    target_include_directories(snow-shot-clipboard-placement-benchmark PRIVATE include src/image)
    target_link_libraries(snow-shot-clipboard-placement-benchmark PRIVATE
        snow_shot_clipboard_placement snow_shot_image_codec snow_shot_diagnostics Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-clipboard-placement-benchmark)
    if(WIN32)
        target_link_libraries(snow-shot-clipboard-placement-benchmark PRIVATE user32)
    endif()
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_test(NAME snow-shot-recording-audio-settings-tests
        COMMAND snow-shot-application-storage-tests --recording-audio-only)
    set_tests_properties(snow-shot-recording-audio-settings-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-recording-audio-controller-tests
        COMMAND snow-shot-screen-recording-controller-tests --audio-gain-only -platform offscreen)
    set_tests_properties(snow-shot-recording-audio-controller-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

if(SNOW_SHOT_BUILD_TESTS)
    snow_find_qt(COMPONENTS Test)
    add_executable(snow-shot-recording-audio-gain-popover-tests
        tests/recording_audio_gain_popover_tests.cpp
        src/presentation/recording/recordingaudiogainpopover.cpp
        src/presentation/recording/recordingaudiogainpopover.h)
    set_target_properties(snow-shot-recording-audio-gain-popover-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(snow-shot-recording-audio-gain-popover-tests PRIVATE ant_design_qt Qt6::Widgets Qt6::Test)
    target_include_directories(snow-shot-recording-audio-gain-popover-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    get_target_property(_snow_audio_qt_core_type Qt6::Core TYPE)
    if(_snow_audio_qt_core_type STREQUAL "STATIC_LIBRARY" AND TARGET Qt6::QOffscreenIntegrationPlugin)
        target_link_libraries(snow-shot-recording-audio-gain-popover-tests PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(snow-shot-recording-audio-gain-popover-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME snow-shot-recording-audio-gain-popover-tests
        COMMAND snow-shot-recording-audio-gain-popover-tests -platform offscreen)
    set_tests_properties(snow-shot-recording-audio-gain-popover-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
if(SNOW_SHOT_BUILD_TESTS)
    # This isolated skin test exercises portable widget surfaces, including
    # builds where the legacy toolbar executable's other cases are Windows-only.
    set_tests_properties(snow-shot-toolbar-skin-tests PROPERTIES LABELS "unit")
endif()

if(SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-skin-surface-performance-benchmark
        tests/skin_surface_performance_benchmark.cpp
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/pinnedwindowgroupmanager.h
        include/snow_shot/presentation/styles/thememanager.h
        include/snow_shot/presentation/systemtraycontroller.h
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/services/languagemanager.cpp
        src/presentation/services/pinnedwindowgroupmanager.cpp
        src/presentation/services/systemtraycontroller.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp)
    target_include_directories(snow-shot-skin-surface-performance-benchmark PRIVATE include)
    target_compile_definitions(snow-shot-skin-surface-performance-benchmark PRIVATE
        SNOW_SKIN_SURFACE_BUILD_CONFIG="$<CONFIG>")
    target_link_libraries(snow-shot-skin-surface-performance-benchmark PRIVATE
        snow_shot_system_notification_controller
        snow_shot_storage snow_shot_image_codec snow_shot_settings_catalog
        snow_draw_engine_qt_canvas ant_design_qt Qt6::Widgets)
    if(APPLE)
        target_sources(snow-shot-skin-surface-performance-benchmark PRIVATE
            src/platform/macos/systemtraymenu.mm)
        target_link_libraries(snow-shot-skin-surface-performance-benchmark PRIVATE
            snow_shot_system_notifications "-framework AppKit")
    endif()
    qt_import_plugins(snow-shot-skin-surface-performance-benchmark EXCLUDE Qt6::QICOPlugin)
    qt_add_resources(snow-shot-skin-surface-performance-benchmark "snow_shot_skin_surface_tray_assets"
        PREFIX "/snow-shot/app-icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-default.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-light.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-dark.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-snow-default.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-snow-light.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/snow-shot-tray-snow-dark.png")
    snow_shot_add_translations(snow-shot-skin-surface-performance-benchmark)
    snow_shot_import_offscreen_platform(snow-shot-skin-surface-performance-benchmark)
endif()

# Desktop floating toolbar coverage also supplies the focused Release benchmark.
if(SNOW_SHOT_BUILD_TESTS OR SNOW_APPS_BUILD_BENCHMARKS)
    set(_snow_floating_test_sources
        tests/floating_toolbar_tests.cpp
        include/snow_shot/presentation/floatingtoolbarcontroller.h
        src/presentation/toolbar/floatingtoolbarcontroller.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    set(_snow_floating_targets)
    if(SNOW_SHOT_BUILD_TESTS)
        add_executable(snow-shot-floating-toolbar-tests ${_snow_floating_test_sources})
        target_compile_definitions(snow-shot-floating-toolbar-tests PRIVATE
            SNOW_FLOATING_TEST_TRANSLATIONS_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        add_dependencies(snow-shot-floating-toolbar-tests snow_shot_release_translations)
        list(APPEND _snow_floating_targets snow-shot-floating-toolbar-tests)
        add_test(NAME snow-shot-floating-toolbar-tests COMMAND snow-shot-floating-toolbar-tests)
        add_test(NAME snow-shot-floating-toolbar-icon-activation-tests
            COMMAND snow-shot-floating-toolbar-tests --icon-activation-only -platform offscreen)
        set_tests_properties(snow-shot-floating-toolbar-icon-activation-tests PROPERTIES
            LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/test-support/menu-screen.json"
            CONTENT "{\"screens\":[{\"name\":\"menu-screen\",\"width\":240,\"height\":600,\"dpr\":1.0}]}\n")
        add_test(NAME snow-shot-floating-toolbar-menu-lifecycle-tests
            COMMAND snow-shot-floating-toolbar-tests --menu-lifecycle-only
                -platform "offscreen:configfile=menu-screen.json")
        set_tests_properties(snow-shot-floating-toolbar-menu-lifecycle-tests PROPERTIES
            LABELS "unit" TIMEOUT 30 SNOW_TEST_CROSS_PLATFORM TRUE
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-support"
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        add_test(NAME snow-shot-floating-toolbar-content-drop-tests
            COMMAND snow-shot-floating-toolbar-tests --content-drops-only -platform offscreen)
        set_tests_properties(snow-shot-floating-toolbar-content-drop-tests PROPERTIES
            LABELS "unit" TIMEOUT 60 SNOW_TEST_CROSS_PLATFORM TRUE
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        set_tests_properties(snow-shot-floating-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60
            SNOW_TEST_CROSS_PLATFORM TRUE
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        if(WIN32)
            add_test(NAME snow-shot-floating-toolbar-native-tests
                COMMAND snow-shot-floating-toolbar-tests --native-probe -platform windows)
            set_tests_properties(snow-shot-floating-toolbar-native-tests PROPERTIES
                LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    endif()
    if(SNOW_APPS_BUILD_BENCHMARKS)
        add_executable(snow-shot-floating-toolbar-performance-benchmark ${_snow_floating_test_sources})
        list(APPEND _snow_floating_targets snow-shot-floating-toolbar-performance-benchmark)
    endif()
    if(APPLE AND SNOW_SHOT_BUILD_TESTS)
        set(_snow_floating_macos_sources ${_snow_floating_test_sources})
        list(REMOVE_ITEM _snow_floating_macos_sources tests/floating_toolbar_tests.cpp)
        add_executable(snow-shot-macos-floating-toolbar-drop-tests
            tests/macos_floating_toolbar_drop_tests.mm
            ../cmake/test-support/macos_native_input.mm
            ${_snow_floating_macos_sources})
        list(APPEND _snow_floating_targets snow-shot-macos-floating-toolbar-drop-tests)
        target_include_directories(snow-shot-macos-floating-toolbar-drop-tests PRIVATE src)
        target_link_libraries(snow-shot-macos-floating-toolbar-drop-tests PRIVATE
            "-framework AppKit" "-framework ApplicationServices")
        add_test(NAME snow-shot-macos-floating-toolbar-drop-policy-tests
            COMMAND snow-shot-macos-floating-toolbar-drop-tests -platform offscreen)
        set_tests_properties(snow-shot-macos-floating-toolbar-drop-policy-tests PROPERTIES
            LABELS "unit;macos" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 30)
        add_test(NAME snow-shot-macos-floating-toolbar-content-drop-native-tests
            COMMAND snow-shot-macos-floating-toolbar-drop-tests --native -platform cocoa)
        set_tests_properties(snow-shot-macos-floating-toolbar-content-drop-native-tests PROPERTIES
            LABELS "interactive;macos" SNOW_TEST_CROSS_PLATFORM TRUE
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 30)
    endif()
    foreach(_snow_floating_target IN LISTS _snow_floating_targets)
        target_include_directories(${_snow_floating_target} PRIVATE include src/image)
        target_link_libraries(${_snow_floating_target} PRIVATE
            snow_shot_settings snow_shot_image_codec snow_shot_clipboard_placement
            snow_draw_engine_qt_canvas ant_design_qt Qt6::Widgets)
        snow_shot_import_offscreen_platform(${_snow_floating_target})
        snow_shot_link_macos_window_platform(${_snow_floating_target})
        if(WIN32)
            target_link_libraries(${_snow_floating_target} PRIVATE dwmapi gdi32 user32)
            if(NOT SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
                _snow_shot_stage_image_runtime(${_snow_floating_target})
            endif()
        endif()
    endforeach()
endif()

# Toolbar and tray sources are also compiled directly by focused tests and
# benchmarks. Keep the skin dependency identical for every consumer.
get_property(_snow_skin_consumers DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_snow_target IN LISTS _snow_skin_consumers)
    get_target_property(_snow_sources ${_snow_target} SOURCES)
    if("${_snow_sources}" MATCHES "(screenshottoolbarmainpanel|systemtraycontroller)\\.cpp")
        target_link_libraries(${_snow_target} PRIVATE snow_shot_main_window_skin)
    endif()
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MicroTeX.cmake")
snow_shot_add_latex_renderer()

# Recognition surfaces are also compiled directly by focused fixtures. Keep
# their formula renderer dependency identical without adding it to the shared
# original-image preview library, which remains available in Mini.
get_property(_snow_latex_consumers DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_snow_target IN LISTS _snow_latex_consumers)
    get_target_property(_snow_sources ${_snow_target} SOURCES)
    if("${_snow_sources}" MATCHES "screenshotrecognitionwindow\\.cpp")
        target_link_libraries(${_snow_target} PRIVATE snow_shot_latex_renderer)
    endif()
endforeach()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-latex-renderer-tests tests/screenshot_latex_renderer_tests.cpp)
    target_link_libraries(snow-shot-latex-renderer-tests PRIVATE
        snow_shot_latex_renderer snow_shot_microtex)
    snow_shot_import_offscreen_platform(snow-shot-latex-renderer-tests)
    add_test(NAME snow-shot-latex-renderer-tests
        COMMAND snow-shot-latex-renderer-tests -platform offscreen)
    set_tests_properties(snow-shot-latex-renderer-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-latex-preview-window-tests
        COMMAND snow-shot-recognition-window-tests --latex-preview-only -platform offscreen)
    set_tests_properties(snow-shot-latex-preview-window-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-latex-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --latex-only -platform offscreen)
    set_tests_properties(snow-shot-latex-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-recognition-model-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --recognition-model-only -platform offscreen)
    set_tests_properties(snow-shot-recognition-model-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
if(SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-latex-renderer-performance-benchmark
        tests/screenshot_latex_renderer_performance_benchmark.cpp)
    target_link_libraries(snow-shot-latex-renderer-performance-benchmark PRIVATE
        snow_shot_latex_renderer)
    snow_shot_import_offscreen_platform(snow-shot-latex-renderer-performance-benchmark)
    add_test(NAME snow-shot-latex-renderer-performance-benchmark
        COMMAND snow-shot-latex-renderer-performance-benchmark -platform offscreen)
    set_tests_properties(snow-shot-latex-renderer-performance-benchmark PROPERTIES
        LABELS "benchmark" TIMEOUT 120
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_library(snow_shot_original_image_preview STATIC
    include/snow_shot/presentation/screenshotoriginalimagepreviewwindow.h
    src/presentation/ocr/screenshotoriginalimagepreviewwindow.cpp
    src/presentation/overlay/screenshotimagerendering.cpp)
target_include_directories(snow_shot_original_image_preview PUBLIC include)
target_link_libraries(snow_shot_original_image_preview PUBLIC Qt6::Widgets snow_canvas_image
    PRIVATE adqt::dpi_window)
snow_shot_link_macos_window_platform(snow_shot_original_image_preview)
if(WIN32)
    target_link_libraries(snow_shot_original_image_preview PRIVATE user32 gdi32)
endif()

get_property(_snow_preview_consumers DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_snow_target IN LISTS _snow_preview_consumers)
    get_target_property(_snow_sources ${_snow_target} SOURCES)
    if("${_snow_sources}" MATCHES "screenshot(recognitionwindow|canvasrenderer)\\.cpp")
        target_link_libraries(${_snow_target} PRIVATE snow_shot_original_image_preview)
    endif()
endforeach()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-original-image-preview-tests
        tests/screenshot_original_image_preview_tests.cpp)
    target_link_libraries(snow-shot-original-image-preview-tests PRIVATE
        snow_shot_original_image_preview)
    snow_shot_import_offscreen_platform(snow-shot-original-image-preview-tests)
    add_test(NAME snow-shot-original-image-preview-tests
        COMMAND snow-shot-original-image-preview-tests -platform offscreen)
    set_tests_properties(snow-shot-original-image-preview-tests PROPERTIES LABELS "unit"
        TIMEOUT 60 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-original-image-preview-mixed-dpi-tests
        COMMAND snow-shot-original-image-preview-tests --mixed-dpi-only
            -platform offscreen:configfile=original_image_preview_mixed_dpi_screens.json)
    set_tests_properties(snow-shot-original-image-preview-mixed-dpi-tests PROPERTIES
        LABELS "unit" TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(APPLE)
        set_source_files_properties(tests/screenshot_original_image_preview_tests.cpp
            PROPERTIES LANGUAGE OBJCXX)
        target_link_libraries(snow-shot-original-image-preview-tests PRIVATE "-framework AppKit")
        add_test(NAME snow-shot-original-image-preview-native-macos-tests
            COMMAND snow-shot-original-image-preview-tests --native-only -platform cocoa)
        set_tests_properties(snow-shot-original-image-preview-native-macos-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 120)
    endif()
    if(WIN32)
        set(_preview_scales 1 1.25 1.5 1.75 2)
        set(_preview_factors 0.5 0.625 0.75 0.875 1)
        foreach(_preview_scale _preview_factor IN ZIP_LISTS _preview_scales _preview_factors)
            add_test(NAME snow-shot-original-image-preview-native-${_preview_scale}-tests
                COMMAND snow-shot-original-image-preview-tests --native-only -platform windows)
            set_tests_properties(snow-shot-original-image-preview-native-${_preview_scale}-tests
                PROPERTIES LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 60
                ENVIRONMENT "QT_SCREEN_SCALE_FACTORS=2;QT_SCALE_FACTOR=${_preview_factor};SNOW_PREVIEW_TEST_DPR=${_preview_scale}"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
            add_test(NAME snow-shot-pinned-original-image-preview-native-${_preview_scale}-tests
                COMMAND snow-shot-pinned-window-tests --original-image-preview-only -platform windows)
            set_tests_properties(snow-shot-pinned-original-image-preview-native-${_preview_scale}-tests
                PROPERTIES LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 120
                ENVIRONMENT "QT_SCREEN_SCALE_FACTORS=2;QT_SCALE_FACTOR=${_preview_factor};SNOW_PREVIEW_TEST_DPR=${_preview_scale}"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endforeach()
        add_test(NAME snow-shot-original-image-preview-native-platform-tests
            COMMAND snow-shot-original-image-preview-tests --native-only -platform windows)
        add_test(NAME snow-shot-pinned-original-image-preview-native-platform-tests
            COMMAND snow-shot-pinned-window-tests --original-image-preview-only -platform windows)
        set_tests_properties(snow-shot-original-image-preview-native-platform-tests
            snow-shot-pinned-original-image-preview-native-platform-tests PROPERTIES
            LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 120
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_SCREEN_SCALE_FACTORS=unset:;QT_SCALE_FACTOR=unset:;SNOW_PREVIEW_TEST_DPR=unset:")
    endif()
    add_test(NAME snow-shot-recognition-original-image-preview-tests
        COMMAND snow-shot-recognition-session-controller-tests --original-image-preview-only
            -platform offscreen)
    add_test(NAME snow-shot-recognition-original-image-preview-window-tests
        COMMAND snow-shot-recognition-window-tests --preview-window-only -platform offscreen)
    add_test(NAME snow-shot-pinned-original-image-preview-tests
        COMMAND snow-shot-pinned-window-tests --original-image-preview-only -platform offscreen)
    add_test(NAME snow-shot-original-image-preview-settings-tests
        COMMAND snow-shot-translation-settings-tests --original-image-preview-only -platform offscreen)
    add_test(NAME snow-shot-large-image-slice-rendering-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --large-image-slice-rendering
            -platform offscreen)
    set_tests_properties(snow-shot-recognition-original-image-preview-tests
        snow-shot-recognition-original-image-preview-window-tests
        snow-shot-pinned-original-image-preview-tests snow-shot-original-image-preview-settings-tests
        snow-shot-large-image-slice-rendering-tests
        PROPERTIES LABELS "unit" TIMEOUT 120
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    foreach(_preview_scale IN ITEMS 1.25 2)
        add_test(NAME snow-shot-pinned-original-image-preview-${_preview_scale}-tests
            COMMAND snow-shot-pinned-window-tests --original-image-preview-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-original-image-preview-${_preview_scale}-tests
            PROPERTIES LABELS "unit" TIMEOUT 120 ENVIRONMENT "QT_SCALE_FACTOR=${_preview_scale}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    add_test(NAME snow-shot-original-image-preview-retina-tests
        COMMAND snow-shot-original-image-preview-tests -platform offscreen)
    set_tests_properties(snow-shot-original-image-preview-retina-tests PROPERTIES LABELS "unit"
        TIMEOUT 60 ENVIRONMENT "QT_SCALE_FACTOR=2"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
if(SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-original-image-preview-performance-benchmark
        tests/screenshot_original_image_preview_performance_benchmark.cpp)
    target_link_libraries(snow-shot-original-image-preview-performance-benchmark PRIVATE
        snow_shot_original_image_preview)
    snow_shot_import_offscreen_platform(snow-shot-original-image-preview-performance-benchmark)
    add_test(NAME snow-shot-original-image-preview-performance-benchmark
        COMMAND snow-shot-original-image-preview-performance-benchmark -platform offscreen)
    set_tests_properties(snow-shot-original-image-preview-performance-benchmark PROPERTIES
        LABELS "benchmark" TIMEOUT 120
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        add_test(NAME snow-shot-original-image-preview-native-performance-benchmark
            COMMAND snow-shot-original-image-preview-performance-benchmark
                --native-only -platform windows)
        set_tests_properties(snow-shot-original-image-preview-native-performance-benchmark PROPERTIES
            LABELS "benchmark;windows" RUN_SERIAL TRUE TIMEOUT 120
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()

# Link after all clipboard test and benchmark targets have been declared so
# metadata and native image converters are available to every consumer.
get_property(_snow_clipboard_consumers DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_snow_target IN LISTS _snow_clipboard_consumers)
    get_target_property(_snow_sources ${_snow_target} SOURCES)
    if("${_snow_sources}" MATCHES "screenshotclipboard(service|content)\\.cpp")
        target_link_libraries(${_snow_target} PRIVATE snow_shot_clipboard_placement)
    endif()
    if("${_snow_sources}" MATCHES "screenshotexportartifact\\.cpp")
        target_link_libraries(${_snow_target} PRIVATE snow_shot_clipboard_files)
    endif()
    if(APPLE AND "${_snow_sources}" MATCHES "screenshotclipboard(service|content)\\.cpp")
        target_link_libraries(${_snow_target} PRIVATE snow_shot_macos_clipboard)
    endif()
endforeach()

foreach(_snow_hidden_shortcut_pair IN ITEMS
        "capture|snow-shot-screenshot-toolbar-remembered-tool-tests"
        "pinned|snow-shot-pinned-window-tests"
        "recording|snow-shot-screen-recording-shortcut-tests"
        "global-canvas|snow-shot-global-canvas-tests"
        "floating|snow-shot-global-shortcut-manager-tests")
    string(REPLACE "|" ";" _snow_hidden_shortcut_parts "${_snow_hidden_shortcut_pair}")
    list(GET _snow_hidden_shortcut_parts 0 _snow_hidden_shortcut_context)
    list(GET _snow_hidden_shortcut_parts 1 _snow_hidden_shortcut_target)
    if(TARGET ${_snow_hidden_shortcut_target})
        add_test(NAME snow-shot-${_snow_hidden_shortcut_context}-hidden-tool-shortcut-tests
            COMMAND ${_snow_hidden_shortcut_target} --hidden-tools-only -platform offscreen)
        set_tests_properties(snow-shot-${_snow_hidden_shortcut_context}-hidden-tool-shortcut-tests
            PROPERTIES LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endforeach()

foreach(_right_selection_host IN ITEMS overlay pinned recording global)
    if(_right_selection_host STREQUAL "overlay")
        set(_right_selection_target snow-shot-screenshot-canvas-renderer-tests)
    elseif(_right_selection_host STREQUAL "pinned")
        set(_right_selection_target snow-shot-pinned-window-tests)
    elseif(_right_selection_host STREQUAL "recording")
        set(_right_selection_target snow-shot-screen-recording-area-window-tests)
    else()
        set(_right_selection_target snow-shot-global-canvas-tests)
    endif()
    if(TARGET ${_right_selection_target})
        add_test(NAME snow-shot-${_right_selection_host}-right-quick-selection-tests
            COMMAND ${_right_selection_target} --right-quick-selection-only -platform offscreen)
        set_tests_properties(snow-shot-${_right_selection_host}-right-quick-selection-tests PROPERTIES
            LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endforeach()

foreach(_text_delete_host IN ITEMS overlay pinned recording global image-editor)
    set(_text_delete_arguments --delete-text-after-cursor-only)
    if(_text_delete_host STREQUAL "overlay")
        set(_text_delete_target snow-shot-screenshot-canvas-renderer-tests)
    elseif(_text_delete_host STREQUAL "pinned")
        set(_text_delete_target snow-shot-pinned-window-tests)
    elseif(_text_delete_host STREQUAL "recording")
        set(_text_delete_target snow-shot-screen-recording-area-window-tests)
    elseif(_text_delete_host STREQUAL "global")
        set(_text_delete_target snow-shot-global-canvas-tests)
    else()
        set(_text_delete_target snow-shot-pinned-window-tests)
        set(_text_delete_arguments --image-editing-text-preferences-only)
    endif()
    if(TARGET ${_text_delete_target})
        add_test(NAME snow-shot-${_text_delete_host}-delete-text-preference-tests
            COMMAND ${_text_delete_target} ${_text_delete_arguments} -platform offscreen)
        set_tests_properties(snow-shot-${_text_delete_host}-delete-text-preference-tests PROPERTIES
            LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowShotReleaseOptimization.cmake")
# Standalone test targets reuse the pinned/controller sources as well as the application.
get_property(_snow_cloud_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_snow_cloud_target IN LISTS _snow_cloud_targets)
    get_target_property(_snow_cloud_sources ${_snow_cloud_target} SOURCES)
    foreach(_snow_cloud_source IN LISTS _snow_cloud_sources)
        if(_snow_cloud_source MATCHES "screenshot(pinnedwindow|controller)\\.cpp$")
            target_link_libraries(${_snow_cloud_target} PRIVATE snow_shot_cloud_upload)
            break()
        endif()
    endforeach()
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowShotMini.cmake")

if(SNOW_SHOT_BUILD_TESTS AND WIN32)
    foreach(_shell_edition IN ITEMS full mini)
        set(_shell_suffix "")
        if(_shell_edition STREQUAL "mini")
            if(NOT TARGET snow_shot_settings_mini)
                continue()
            endif()
            set(_shell_suffix "_mini")
        endif()
        set(_shell_registration_test "snow-shot-shell-context-menu-registration-${_shell_edition}-tests")
        add_executable(${_shell_registration_test} tests/shell_context_menu_registration_tests.cpp)
        target_link_libraries(${_shell_registration_test} PRIVATE snow_shot_administrator${_shell_suffix})
        add_test(NAME ${_shell_registration_test} COMMAND ${_shell_registration_test})
        set_tests_properties(${_shell_registration_test} PROPERTIES LABELS "unit;windows" TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

        set(_shell_settings_test "snow-shot-shell-context-menu-settings-${_shell_edition}-tests")
        add_executable(${_shell_settings_test} tests/shell_context_menu_settings_tests.cpp
            src/presentation/services/screenshotclipboardservice.cpp)
        target_link_libraries(${_shell_settings_test} PRIVATE snow_shot_settings${_shell_suffix}
            snow_shot_image_codec)
        snow_shot_import_offscreen_platform(${_shell_settings_test})
        if(_shell_edition STREQUAL "mini")
            snow_shot_add_translations(${_shell_settings_test} MINI)
        else()
            snow_shot_add_translations(${_shell_settings_test})
        endif()
        add_test(NAME ${_shell_settings_test} COMMAND ${_shell_settings_test} -platform offscreen)
        set_tests_properties(${_shell_settings_test} PROPERTIES LABELS "unit;windows" TIMEOUT 60
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-memory-optimization-controller-tests
        tests/memory_optimization_controller_tests.cpp)
    target_link_libraries(snow-shot-memory-optimization-controller-tests PRIVATE
        snow_shot_memory_optimization Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-memory-optimization-controller-tests)
    add_test(NAME snow-shot-memory-optimization-controller-tests
        COMMAND snow-shot-memory-optimization-controller-tests -platform offscreen)
    set_tests_properties(snow-shot-memory-optimization-controller-tests PROPERTIES
        LABELS "unit" TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    foreach(_memory_settings_edition IN ITEMS full mini)
        set(_memory_settings_suffix "")
        set(_memory_settings_library snow_shot_settings)
        if(_memory_settings_edition STREQUAL "mini")
            if(NOT TARGET snow_shot_settings_mini)
                continue()
            endif()
            set(_memory_settings_suffix "-mini")
            set(_memory_settings_library snow_shot_settings_mini)
        endif()
        set(_memory_settings_target "snow-shot-memory-optimization-settings${_memory_settings_suffix}-tests")
        add_executable(${_memory_settings_target}
            tests/memory_optimization_settings_tests.cpp
            src/presentation/services/screenshotclipboardservice.cpp)
        target_link_libraries(${_memory_settings_target} PRIVATE
            ${_memory_settings_library} snow_shot_image_codec snow_shot_memory_optimization)
        snow_shot_import_offscreen_platform(${_memory_settings_target})
        add_test(NAME ${_memory_settings_target} COMMAND ${_memory_settings_target} -platform offscreen)
        set_tests_properties(${_memory_settings_target} PROPERTIES LABELS "unit" TIMEOUT 60
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    add_test(NAME snow-shot-memory-optimization-catalog-tests
        COMMAND snow-shot-settings-catalog-tests --memory-optimization-only)
    set_tests_properties(snow-shot-memory-optimization-catalog-tests PROPERTIES LABELS "unit"
        TIMEOUT 30 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-capture-history-memory-activity-tests
        COMMAND snow-shot-capture-history-repository-tests --memory-activity-only)
    set_tests_properties(snow-shot-capture-history-memory-activity-tests PROPERTIES LABELS "unit"
        TIMEOUT 30 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-window-memory-activity-tests
        COMMAND snow-shot-pinned-window-repository-tests --memory-activity-only)
    set_tests_properties(snow-shot-pinned-window-memory-activity-tests PROPERTIES LABELS "unit"
        TIMEOUT 30 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-pinned-window-memory-idle-tests
        COMMAND snow-shot-pinned-window-tests --memory-idle-only -platform offscreen)
    add_test(NAME snow-shot-floating-toolbar-memory-idle-tests
        COMMAND snow-shot-floating-toolbar-tests --memory-idle-only -platform offscreen)
    add_test(NAME snow-shot-storage-memory-activity-tests
        COMMAND snow-shot-application-storage-tests --memory-activity-only)
    set_tests_properties(snow-shot-pinned-window-memory-idle-tests
        snow-shot-floating-toolbar-memory-idle-tests snow-shot-storage-memory-activity-tests
        PROPERTIES LABELS "unit" TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    if(APPLE)
        add_executable(snow-shot-macos-memory-optimization-tests
            tests/macos_memory_optimization_tests.cpp)
        target_link_libraries(snow-shot-macos-memory-optimization-tests PRIVATE
            snow_shot_memory_optimization)
        add_test(NAME snow-shot-macos-memory-optimization-tests
            COMMAND snow-shot-macos-memory-optimization-tests)
        set_tests_properties(snow-shot-macos-memory-optimization-tests PROPERTIES
            LABELS "unit" TIMEOUT 30)
    endif()

    if(WIN32)
        add_executable(snow-shot-memory-optimization-native-smoke-tests
            tests/memory_optimization_native_smoke_tests.cpp)
        target_link_libraries(snow-shot-memory-optimization-native-smoke-tests PRIVATE
            snow_shot_memory_optimization)
        add_test(NAME snow-shot-memory-optimization-native-smoke-tests
            COMMAND snow-shot-memory-optimization-native-smoke-tests)
        set_tests_properties(snow-shot-memory-optimization-native-smoke-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        if(SNOW_SHOT_BUILD_BENCHMARKS)
            get_target_property(_memory_pin_sources snow-shot-pinned-window-tests SOURCES)
            get_target_property(_memory_selection_sources snow-shot-selection-presentation-runtime SOURCES)
            set(_memory_ui_sources)
            foreach(_memory_source IN LISTS _memory_pin_sources _memory_selection_sources)
                if(NOT IS_ABSOLUTE "${_memory_source}")
                    set(_memory_source "${CMAKE_CURRENT_SOURCE_DIR}/${_memory_source}")
                endif()
                list(APPEND _memory_ui_sources "${_memory_source}")
            endforeach()
            list(FILTER _memory_ui_sources EXCLUDE REGEX "/tests/")
            list(REMOVE_DUPLICATES _memory_ui_sources)
            add_executable(snow-shot-memory-optimization-performance-benchmark
                tests/memory_optimization_performance_benchmark.cpp ${_memory_ui_sources})
            foreach(_memory_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES)
                get_target_property(_memory_value snow-shot-pinned-window-tests ${_memory_property})
                if(NOT _memory_value STREQUAL "_memory_value-NOTFOUND")
                    set_property(TARGET snow-shot-memory-optimization-performance-benchmark
                        PROPERTY ${_memory_property} "${_memory_value}")
                endif()
            endforeach()
            get_target_property(_memory_selection_links snow-shot-selection-presentation-runtime LINK_LIBRARIES)
            target_compile_definitions(snow-shot-memory-optimization-performance-benchmark PRIVATE
                SNOW_SHOT_BENCH_INTERNALS=1)
            target_link_libraries(snow-shot-memory-optimization-performance-benchmark PRIVATE
                snow_shot_memory_optimization ${_memory_selection_links})
            snow_shot_import_offscreen_platform(snow-shot-memory-optimization-performance-benchmark)
        endif()
    endif()
endif()

if(SNOW_SHOT_BUILD_TESTS)
    target_sources(snow-shot-pinned-window-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../test-support/fonts/recording_test_fonts.qrc")
    add_executable(snow-shot-airdrop-service-tests tests/screenshot_airdrop_service_tests.cpp)
    target_link_libraries(snow-shot-airdrop-service-tests PRIVATE snow_shot_airdrop)
    snow_shot_import_offscreen_platform(snow-shot-airdrop-service-tests)
    add_test(NAME snow-shot-airdrop-service-tests
        COMMAND snow-shot-airdrop-service-tests -platform offscreen)
    set_tests_properties(snow-shot-airdrop-service-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    foreach(_airdrop_host IN ITEMS pinned-window toolbar-main-panel floating-toolbar-drag)
        add_test(NAME snow-shot-${_airdrop_host}-airdrop-tests
            COMMAND snow-shot-${_airdrop_host}-tests --airdrop-only -platform offscreen)
        set_tests_properties(snow-shot-${_airdrop_host}-airdrop-tests PROPERTIES
            LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    endforeach()
    add_test(NAME snow-shot-airdrop-icon-tests
        COMMAND snow-shot-generated-icon-pack-tests --airdrop-only -platform offscreen)
    set_tests_properties(snow-shot-airdrop-icon-tests PROPERTIES LABELS unit TIMEOUT 30)
    add_executable(snow-shot-print-service-tests tests/screenshot_print_service_tests.cpp)
    target_link_libraries(snow-shot-print-service-tests PRIVATE snow_shot_print snow_shot_diagnostics)
    snow_shot_import_offscreen_platform(snow-shot-print-service-tests)
    add_test(NAME snow-shot-print-service-tests
        COMMAND snow-shot-print-service-tests -platform offscreen)
    set_tests_properties(snow-shot-print-service-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    add_executable(snow-shot-native-print-smoke tests/native_print_smoke.cpp)
    target_link_libraries(snow-shot-native-print-smoke PRIVATE snow_shot_print)
    if(APPLE)
        add_executable(snow-shot-macos-airdrop-native-tests tests/macos_airdrop_native_tests.mm)
        target_include_directories(snow-shot-macos-airdrop-native-tests PRIVATE include src)
        target_link_libraries(snow-shot-macos-airdrop-native-tests PRIVATE
            snow_shot_airdrop snow_shot_macos_window_platform "-framework AppKit")
        add_test(NAME snow-shot-macos-airdrop-native-tests
            COMMAND snow-shot-macos-airdrop-native-tests -platform cocoa)
        set_tests_properties(snow-shot-macos-airdrop-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 75 SKIP_RETURN_CODE 77)
        add_executable(snow-shot-macos-native-print-stacking-tests
            tests/macos_native_print_stacking_tests.mm)
        target_include_directories(snow-shot-macos-native-print-stacking-tests PRIVATE include src)
        target_link_libraries(snow-shot-macos-native-print-stacking-tests PRIVATE
            snow_shot_print snow_shot_macos_window_platform "-framework AppKit")
        add_test(NAME snow-shot-macos-native-print-stacking-tests
            COMMAND snow-shot-macos-native-print-stacking-tests -platform offscreen)
        set_tests_properties(snow-shot-macos-native-print-stacking-tests PROPERTIES
            LABELS "unit;macos" TIMEOUT 30)
        add_test(NAME snow-shot-macos-native-print-stacking-native-tests
            COMMAND snow-shot-macos-native-print-stacking-tests -platform cocoa)
        set_tests_properties(snow-shot-macos-native-print-stacking-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    if(WIN32)
        add_executable(snow-shot-windows-print-dialog-tests tests/windows_print_dialog_tests.cpp)
        target_link_libraries(snow-shot-windows-print-dialog-tests PRIVATE snow_shot_print snow_shot_diagnostics)
        snow_shot_import_offscreen_platform(snow-shot-windows-print-dialog-tests)
        add_test(NAME snow-shot-windows-print-dialog-tests
            COMMAND snow-shot-windows-print-dialog-tests -platform offscreen)
        set_tests_properties(snow-shot-windows-print-dialog-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 60
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
        add_executable(snow-shot-windows-print-document-tests
            tests/windows_print_document_tests.cpp)
        target_link_libraries(snow-shot-windows-print-document-tests PRIVATE snow_shot_print snow_shot_diagnostics)
        add_test(NAME snow-shot-windows-print-document-tests
            COMMAND snow-shot-windows-print-document-tests)
        set_tests_properties(snow-shot-windows-print-document-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 60)
    endif()
    foreach(_print_host IN ITEMS pinned-window toolbar-main-panel floating-toolbar-drag)
        add_test(NAME snow-shot-${_print_host}-print-tests
            COMMAND snow-shot-${_print_host}-tests --print-only -platform offscreen)
        set_tests_properties(snow-shot-${_print_host}-print-tests PROPERTIES
            LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    endforeach()
    add_test(NAME snow-shot-print-export-snapshot-tests
        COMMAND snow-shot-export-artifact-tests --print-snapshots-only -platform offscreen)
    set_tests_properties(snow-shot-print-export-snapshot-tests PROPERTIES
        LABELS unit SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-pinned-selection-geometry-tests
        tests/pinned_window_selection_geometry_tests.cpp
        src/presentation/pinned/pinnedwindowselectiongeometry.cpp)
    target_include_directories(snow-shot-pinned-selection-geometry-tests PRIVATE
        src/presentation/pinned "${CMAKE_SOURCE_DIR}/snow_draw_engine_qt/include")
    target_link_libraries(snow-shot-pinned-selection-geometry-tests PRIVATE Qt6::Gui)
    add_test(NAME snow-shot-pinned-selection-geometry-tests
        COMMAND snow-shot-pinned-selection-geometry-tests)
    set_tests_properties(snow-shot-pinned-selection-geometry-tests PROPERTIES LABELS "unit")
    if(TARGET snow-shot-pinned-window-tests)
        add_test(NAME snow-shot-pinned-stitching-snapshot-tests
            COMMAND snow-shot-pinned-window-tests --stitching-snapshot-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-stitching-snapshot-tests PROPERTIES
            LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
        add_test(NAME snow-shot-pinned-multi-selection-tests
            COMMAND snow-shot-pinned-window-tests --multi-selection-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-multi-selection-tests PROPERTIES
            LABELS "unit" RUN_SERIAL TRUE TIMEOUT 90)
        if(APPLE)
            add_test(NAME snow-shot-macos-pinned-multi-selection-geometry-tests
                COMMAND snow-shot-pinned-window-tests --multi-selection-geometry-only -platform cocoa)
            set_tests_properties(snow-shot-macos-pinned-multi-selection-geometry-tests PROPERTIES
                LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60)
            add_test(NAME snow-shot-macos-pinned-multi-selection-tests
                COMMAND snow-shot-pinned-window-tests --multi-selection-native-only -platform cocoa)
            set_tests_properties(snow-shot-macos-pinned-multi-selection-tests PROPERTIES
                LABELS "interactive;e2e;macos" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 60)
        elseif(WIN32)
            add_test(NAME snow-shot-pinned-multi-selection-native-tests
                COMMAND snow-shot-pinned-window-tests --multi-selection-native-only -platform windows)
            set_tests_properties(snow-shot-pinned-multi-selection-native-tests PROPERTIES
                LABELS "interactive;e2e;windows" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 60)
        endif()
    endif()
    if(TARGET snow-shot-pinned-window-group-tests)
        add_test(NAME snow-shot-pinned-batch-group-tests
            COMMAND snow-shot-pinned-window-group-tests --batch-group-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-batch-group-tests PROPERTIES LABELS "unit")
    endif()
    if(TARGET snow-shot-pinned-window-repository-tests)
        add_test(NAME snow-shot-pinned-batch-group-repository-tests
            COMMAND snow-shot-pinned-window-repository-tests --batch-group-only)
        set_tests_properties(snow-shot-pinned-batch-group-repository-tests PROPERTIES LABELS "unit")
    endif()
endif()
if(SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-pinned-selection-performance-benchmark
        tests/pinned_window_selection_performance_benchmark.cpp
        src/presentation/pinned/pinnedwindowselectiongeometry.cpp)
    target_include_directories(snow-shot-pinned-selection-performance-benchmark PRIVATE
        src/presentation/pinned "${CMAKE_SOURCE_DIR}/snow_draw_engine_qt/include")
    target_link_libraries(snow-shot-pinned-selection-performance-benchmark PRIVATE Qt6::Gui)
    add_test(NAME snow-shot-pinned-selection-performance-benchmark
        COMMAND snow-shot-pinned-selection-performance-benchmark)
    set_tests_properties(snow-shot-pinned-selection-performance-benchmark PROPERTIES LABELS "benchmark")
    if(TARGET snow-shot-pinned-window-tests)
        add_test(NAME snow-shot-pinned-multi-selection-performance-benchmark
            COMMAND snow-shot-pinned-window-tests --multi-selection-performance-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-multi-selection-performance-benchmark PROPERTIES
            LABELS "benchmark" RUN_SERIAL TRUE TIMEOUT 120)
    endif()
endif()

add_library(snow_shot_pinned_decoration STATIC
    src/presentation/pinned/screenshotpinneddecorationrenderer.cpp)
target_include_directories(snow_shot_pinned_decoration PUBLIC src/presentation/pinned)
target_link_libraries(snow_shot_pinned_decoration PUBLIC Qt6::Widgets PRIVATE snow_shot_history_pin)

# Application, benchmark and integration-test hosts all compile the pinned window.
get_directory_property(_snow_pinned_hosts BUILDSYSTEM_TARGETS)
foreach(_snow_pinned_host IN LISTS _snow_pinned_hosts)
    get_target_property(_snow_pinned_sources ${_snow_pinned_host} SOURCES)
    if(_snow_pinned_sources MATCHES "screenshotpinnedwindow\\.cpp")
        target_link_libraries(${_snow_pinned_host} PRIVATE
            snow_shot_pinned_decoration snow_shot_collage_core)
    endif()
endforeach()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-pinned-shadow-renderer-tests
        tests/screenshot_pinned_shadow_renderer_tests.cpp)
    target_include_directories(snow-shot-pinned-shadow-renderer-tests PRIVATE include)
    target_link_libraries(snow-shot-pinned-shadow-renderer-tests PRIVATE
        snow_shot_pinned_decoration snow_shot_history_pin)
    get_target_property(_snow_pinned_shadow_qt_type Qt6::Core TYPE)
    if(_snow_pinned_shadow_qt_type STREQUAL "STATIC_LIBRARY")
        target_link_libraries(snow-shot-pinned-shadow-renderer-tests PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(snow-shot-pinned-shadow-renderer-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME snow-shot-pinned-shadow-renderer-tests
        COMMAND snow-shot-pinned-shadow-renderer-tests -platform offscreen)
    set_tests_properties(snow-shot-pinned-shadow-renderer-tests PROPERTIES LABELS "unit"
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(TARGET snow-shot-pinned-window-tests)
        foreach(_snow_pinned_toolbar_dpr IN ITEMS 1 1.25 1.5 1.75 2)
            set(_snow_pinned_toolbar_test
                "snow-shot-pinned-toolbar-placement-${_snow_pinned_toolbar_dpr}-tests")
            add_test(NAME ${_snow_pinned_toolbar_test}
                COMMAND snow-shot-pinned-window-tests --toolbar-placement-only -platform offscreen)
            set_tests_properties(${_snow_pinned_toolbar_test} PROPERTIES
                LABELS "unit" RUN_SERIAL TRUE TIMEOUT 90
                ENVIRONMENT "QT_SCALE_FACTOR=${_snow_pinned_toolbar_dpr};${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
        endforeach()
        add_test(NAME snow-shot-pinned-show-shadow-tests
            COMMAND snow-shot-pinned-window-tests --show-shadow-only -platform offscreen)
        foreach(_snow_pinned_filter_alpha_dpr IN ITEMS 1 1.25 2)
            set(_snow_pinned_filter_alpha_test
                "snow-shot-pinned-filter-alpha-${_snow_pinned_filter_alpha_dpr}-tests")
            add_test(NAME ${_snow_pinned_filter_alpha_test}
                COMMAND snow-shot-pinned-window-tests --filter-alpha-only -platform offscreen)
            set_tests_properties(${_snow_pinned_filter_alpha_test} PROPERTIES
                LABELS "unit" RUN_SERIAL TRUE TIMEOUT 90
                ENVIRONMENT "QT_SCALE_FACTOR=${_snow_pinned_filter_alpha_dpr};${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
        endforeach()
        add_test(NAME snow-shot-pinned-shadow-geometry-tests
            COMMAND snow-shot-pinned-window-tests --shadow-geometry-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-show-shadow-tests
            snow-shot-pinned-shadow-geometry-tests PROPERTIES
            LABELS "unit" RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
        if(WIN32)
            foreach(_snow_shadow_geometry_dpr IN ITEMS 1.25 1.5 1.75 2)
                set(_snow_shadow_geometry_test
                    "snow-shot-pinned-shadow-geometry-${_snow_shadow_geometry_dpr}-tests")
                add_test(NAME ${_snow_shadow_geometry_test}
                    COMMAND snow-shot-pinned-window-tests --shadow-geometry-only -platform offscreen)
                set_tests_properties(${_snow_shadow_geometry_test} PROPERTIES
                    LABELS "unit;windows" RUN_SERIAL TRUE TIMEOUT 90
                    ENVIRONMENT "QT_SCALE_FACTOR=${_snow_shadow_geometry_dpr};${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
            endforeach()
        endif()
    endif()
endif()
if(SNOW_SHOT_BUILD_BENCHMARKS)
    # The lifecycle host is also run headlessly with the static Qt kit.
    get_target_property(_snow_pinned_shadow_qt_type Qt6::Core TYPE)
    if(_snow_pinned_shadow_qt_type STREQUAL "STATIC_LIBRARY" AND
        TARGET snow-shot-file-pin-batch-performance-benchmark)
        target_link_libraries(snow-shot-file-pin-batch-performance-benchmark PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(snow-shot-file-pin-batch-performance-benchmark INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_executable(snow-shot-pinned-shadow-performance-benchmark
        tests/screenshot_pinned_shadow_performance_benchmark.cpp)
    target_include_directories(snow-shot-pinned-shadow-performance-benchmark PRIVATE include)
    target_link_libraries(snow-shot-pinned-shadow-performance-benchmark PRIVATE
        snow_shot_pinned_decoration snow_shot_history_pin)
    if(MSVC)
        target_link_options(snow-shot-pinned-shadow-performance-benchmark PRIVATE /MAP)
    endif()
    get_target_property(_snow_pinned_shadow_qt_type Qt6::Core TYPE)
    if(_snow_pinned_shadow_qt_type STREQUAL "STATIC_LIBRARY")
        target_link_libraries(snow-shot-pinned-shadow-performance-benchmark PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(snow-shot-pinned-shadow-performance-benchmark INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
endif()

if(TARGET snow-shot-configuration-archive-tests)
    add_test(NAME snow-shot-recognition-model-settings-tests
        COMMAND snow-shot-configuration-archive-tests --recognition-models-only)
    set_tests_properties(snow-shot-recognition-model-settings-tests PROPERTIES LABELS "unit" TIMEOUT 60)
endif()
if(TARGET snow-shot-pinned-window-tests)
    add_test(NAME snow-shot-pinned-animation-tests
        COMMAND snow-shot-pinned-window-tests --animation-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-animation-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-pinned-recognition-models-tests
        COMMAND snow-shot-pinned-window-tests --recognition-models-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-recognition-models-tests PROPERTIES LABELS "unit" TIMEOUT 60)
endif()

if(SNOW_APPS_BUILD_BENCHMARKS AND TARGET snow-shot-recognition-session-controller-tests AND
        TARGET snow-shot-screenshot-tool-palette-tests)
    # Reuse the completed standalone production hosts, excluding their test entry points.
    # This block stays last so the shared recognition/preview sources and libraries are present.
    set(_recognition_model_benchmark_sources)
    foreach(_recognition_model_host IN ITEMS snow-shot-recognition-session-controller-tests
            snow-shot-screenshot-tool-palette-tests)
        get_target_property(_recognition_model_host_sources ${_recognition_model_host} SOURCES)
        list(APPEND _recognition_model_benchmark_sources ${_recognition_model_host_sources})
    endforeach()
    list(FILTER _recognition_model_benchmark_sources EXCLUDE REGEX "(^|[/\\\\])tests[/\\\\]")
    list(REMOVE_DUPLICATES _recognition_model_benchmark_sources)
    add_executable(snow-shot-recognition-model-benchmark
        tests/recognition_model_performance_benchmark.cpp ${_recognition_model_benchmark_sources})
    foreach(_recognition_model_property IN ITEMS INCLUDE_DIRECTORIES LINK_LIBRARIES
            COMPILE_DEFINITIONS COMPILE_OPTIONS)
        set(_recognition_model_property_values)
        foreach(_recognition_model_host IN ITEMS snow-shot-recognition-session-controller-tests
                snow-shot-screenshot-tool-palette-tests)
            get_target_property(_recognition_model_host_values ${_recognition_model_host}
                ${_recognition_model_property})
            if(_recognition_model_host_values)
                list(APPEND _recognition_model_property_values ${_recognition_model_host_values})
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _recognition_model_property_values)
        if(_recognition_model_property_values)
            set_property(TARGET snow-shot-recognition-model-benchmark PROPERTY
                ${_recognition_model_property} "${_recognition_model_property_values}")
        endif()
    endforeach()
    snow_shot_import_offscreen_platform(snow-shot-recognition-model-benchmark)
    snow_shot_offscreen_qpa_environment(_recognition_model_benchmark_environment)
    add_test(NAME snow-shot-recognition-model-benchmark
        COMMAND snow-shot-recognition-model-benchmark -platform offscreen)
    set_tests_properties(snow-shot-recognition-model-benchmark PROPERTIES LABELS "benchmark"
        TIMEOUT 120 ENVIRONMENT "${_recognition_model_benchmark_environment}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

# Standalone tests compile presentation hosts independently of the application. Keep
# every host, including Mini and benchmark hosts, on the same compiled readout.
get_property(_snow_readout_consumers DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_snow_target IN LISTS _snow_readout_consumers)
    get_target_property(_snow_sources ${_snow_target} SOURCES)
    if("${_snow_sources}" MATCHES "(screenshot(pinnedwindow|savepreviewcanvas|overlaywindow|originalimagepreviewwindow)|recordingeffectpreview)\\.cpp")
        target_link_libraries(${_snow_target} PRIVATE snow_shot_canvas_status_readout)
    endif()
    if("${_snow_sources}" MATCHES "(screenshot(pinnedwindow|overlaywindow|recognitionwindow|canvascolorsamplerwindow)|globalcanvascontroller|screenrecordingareawindow)\\.cpp")
        target_link_libraries(${_snow_target} PRIVATE snow_shot_crosshair_cursor)
    endif()
    if("${_snow_sources}" MATCHES "(globalcanvascontroller|recordingeffectpreview)\\.cpp")
        target_link_libraries(${_snow_target} PRIVATE snow_shot_recording_effects_source)
    endif()
endforeach()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-crosshair-cursor-tests tests/crosshair_cursor_tests.cpp)
    target_link_libraries(snow-shot-crosshair-cursor-tests PRIVATE
        snow_shot_crosshair_cursor snow_draw_engine_qt_canvas)
    if(WIN32)
        target_include_directories(snow-shot-crosshair-cursor-tests PRIVATE
            "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
    endif()
    snow_shot_import_offscreen_platform(snow-shot-crosshair-cursor-tests)
    add_test(NAME snow-shot-crosshair-cursor-tests
        COMMAND snow-shot-crosshair-cursor-tests -platform offscreen)
    set_tests_properties(snow-shot-crosshair-cursor-tests PROPERTIES LABELS unit TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32)
        add_test(NAME snow-shot-crosshair-cursor-native-tests
            COMMAND snow-shot-crosshair-cursor-tests --native-only -platform windows)
        set_tests_properties(snow-shot-crosshair-cursor-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    foreach(_crosshair_host IN ITEMS global-canvas screen-recording-area-window pinned-window)
        add_test(NAME snow-shot-${_crosshair_host}-crosshair-tests
            COMMAND snow-shot-${_crosshair_host}-tests --crosshair-only -platform offscreen)
        set_tests_properties(snow-shot-${_crosshair_host}-crosshair-tests PROPERTIES
            LABELS unit TIMEOUT 60
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()

    add_executable(snow-shot-canvas-status-readout-tests tests/canvas_status_readout_tests.cpp)
    target_link_libraries(snow-shot-canvas-status-readout-tests PRIVATE snow_shot_canvas_status_readout)
    snow_shot_import_offscreen_platform(snow-shot-canvas-status-readout-tests)
    add_test(NAME snow-shot-canvas-status-readout-tests
        COMMAND snow-shot-canvas-status-readout-tests -platform offscreen)
    set_tests_properties(snow-shot-canvas-status-readout-tests PROPERTIES LABELS unit TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-region-shadow-renderer-tests
        tests/screenshot_region_shadow_renderer_tests.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp)
    target_include_directories(snow-shot-region-shadow-renderer-tests PRIVATE include)
    target_link_libraries(snow-shot-region-shadow-renderer-tests PRIVATE
        Qt6::Widgets ant_design_qt snow_draw_engine_qt_canvas)
    snow_shot_import_offscreen_platform(snow-shot-region-shadow-renderer-tests)
    add_test(NAME snow-shot-region-shadow-renderer-tests COMMAND snow-shot-region-shadow-renderer-tests)
    set_tests_properties(snow-shot-region-shadow-renderer-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(MSVC AND WIN32)
        add_executable(snow-shot-shadow-startup-tests
            tests/screenshot_shadow_startup_tests.cpp
            ../test-support/windows_early_tls.cpp
            src/presentation/overlay/screenshotselectionshadowrenderer.cpp)
        target_include_directories(snow-shot-shadow-startup-tests PRIVATE include
            "${CMAKE_CURRENT_SOURCE_DIR}/../test-support")
        target_link_libraries(snow-shot-shadow-startup-tests PRIVATE
            Qt6::Widgets ant_design_qt snow_draw_engine_qt_canvas)
        add_test(NAME snow-shot-shadow-startup-tests COMMAND snow-shot-shadow-startup-tests)
        set_tests_properties(snow-shot-shadow-startup-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_executable(snow-shot-selection-background-tests
        tests/screenshot_selection_background_tests.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp)
    target_include_directories(snow-shot-selection-background-tests PRIVATE include)
    target_link_libraries(snow-shot-selection-background-tests PRIVATE
        Qt6::Widgets ant_design_qt snow_draw_engine_qt_canvas)
    snow_shot_import_offscreen_platform(snow-shot-selection-background-tests)
    add_test(NAME snow-shot-selection-background-tests COMMAND snow-shot-selection-background-tests)
    add_test(NAME snow-shot-selection-background-preview-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --background-opacity-only)
    set_tests_properties(snow-shot-selection-background-tests snow-shot-selection-background-preview-tests
        PROPERTIES LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(snow-shot-layout-compaction-tests
        tests/screenshot_layout_compaction_tests.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp)
    target_include_directories(snow-shot-layout-compaction-tests PRIVATE include)
    target_link_libraries(snow-shot-layout-compaction-tests PRIVATE
        Qt6::Widgets ant_design_qt snow_draw_engine_qt_canvas)
    snow_shot_import_offscreen_platform(snow-shot-layout-compaction-tests)
    add_test(NAME snow-shot-layout-compaction-tests COMMAND snow-shot-layout-compaction-tests)
    add_test(NAME snow-shot-layout-compaction-export-tests
        COMMAND snow-shot-screenshot-export-service-tests --compaction-only)
    add_test(NAME snow-shot-layout-compaction-document-tests
        COMMAND snow-shot-mcp-document-tests --compaction-only)
    add_test(NAME snow-shot-layout-compaction-pin-tests
        COMMAND snow-shot-pinned-window-tests --layout-compaction-only -platform offscreen)
    add_test(NAME snow-shot-layout-compaction-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --compaction-only)
    set_tests_properties(snow-shot-layout-compaction-tests snow-shot-layout-compaction-export-tests
        snow-shot-layout-compaction-toolbar-tests snow-shot-layout-compaction-document-tests
        snow-shot-layout-compaction-pin-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(SNOW_APPS_BUILD_BENCHMARKS)
    add_executable(snow-shot-selection-background-benchmark
        tests/screenshot_selection_background_benchmark.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp)
    target_include_directories(snow-shot-selection-background-benchmark PRIVATE include)
    target_link_libraries(snow-shot-selection-background-benchmark PRIVATE
        Qt6::Widgets ant_design_qt snow_draw_engine_qt_canvas)
    snow_shot_import_offscreen_platform(snow-shot-selection-background-benchmark)
    add_executable(snow-shot-layout-compaction-benchmark
        tests/screenshot_layout_compaction_benchmark.cpp)
    target_include_directories(snow-shot-layout-compaction-benchmark PRIVATE include
        "${CMAKE_SOURCE_DIR}/snow_draw_engine_qt/crates/snow-draw-engine-c/include")
    target_link_libraries(snow-shot-layout-compaction-benchmark PRIVATE
        Qt6::Widgets snow_draw_engine_qt_canvas)
    snow_shot_import_offscreen_platform(snow-shot-layout-compaction-benchmark)
endif()

if(TARGET snow-shot-clipboard-content-tests)
    add_test(NAME snow-shot-pinned-color-content-tests
        COMMAND snow-shot-clipboard-content-tests --color-swatch-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-color-content-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET snow-shot-pinned-window-tests)
    add_test(NAME snow-shot-pinned-color-window-tests
        COMMAND snow-shot-pinned-window-tests --color-swatch-only -platform offscreen)
    add_test(NAME snow-shot-pinned-color-repository-tests
        COMMAND snow-shot-pinned-window-repository-tests --color-swatch-only)
    set_tests_properties(snow-shot-pinned-color-window-tests snow-shot-pinned-color-repository-tests
        PROPERTIES LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(SNOW_APPS_BUILD_BENCHMARKS AND TARGET snow-shot-pinned-window-tests)
    get_target_property(_image_editing_benchmark_sources snow-shot-pinned-window-tests SOURCES)
    list(FILTER _image_editing_benchmark_sources EXCLUDE REGEX "(^|[/\\])tests[/\\]")
    list(REMOVE_DUPLICATES _image_editing_benchmark_sources)
    add_executable(snow-shot-image-editing-benchmark tests/imageediting_performance_benchmark.cpp
        ${_image_editing_benchmark_sources})
    foreach(_property IN ITEMS INCLUDE_DIRECTORIES LINK_LIBRARIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
        get_target_property(_values snow-shot-pinned-window-tests ${_property})
        if(_values)
            set_property(TARGET snow-shot-image-editing-benchmark PROPERTY ${_property} "${_values}")
        endif()
    endforeach()
    snow_shot_import_offscreen_platform(snow-shot-image-editing-benchmark)
    snow_shot_offscreen_qpa_environment(_image_editing_environment)
    add_test(NAME snow-shot-image-editing-benchmark
        COMMAND snow-shot-image-editing-benchmark -platform offscreen)
    set_tests_properties(snow-shot-image-editing-benchmark PROPERTIES LABELS "benchmark"
        TIMEOUT 240 ENVIRONMENT "${_image_editing_environment}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

foreach(_automatic_selection_pair IN ITEMS
        "capture|snow_shot"
        "pinned|snow-shot-pinned-window-tests"
        "recording|snow-shot-screen-recording-area-window-tests"
        "global|snow-shot-global-canvas-tests")
    string(REPLACE "|" ";" _automatic_selection_parts "${_automatic_selection_pair}")
    list(GET _automatic_selection_parts 0 _automatic_selection_host)
    list(GET _automatic_selection_parts 1 _automatic_selection_target)
    set(_automatic_selection_arguments --automatic-selection-only -platform offscreen)
    if(_automatic_selection_host STREQUAL "capture")
        set(_automatic_selection_arguments --automatic-selection-tests)
    endif()
    if(SNOW_SHOT_BUILD_TESTS AND TARGET ${_automatic_selection_target})
        add_test(NAME snow-shot-${_automatic_selection_host}-automatic-selection-tests
            COMMAND ${_automatic_selection_target} ${_automatic_selection_arguments})
        set_tests_properties(snow-shot-${_automatic_selection_host}-automatic-selection-tests PROPERTIES
            LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endforeach()

if(SNOW_APPS_BUILD_BENCHMARKS)
    add_executable(snow-shot-image-content-detection-performance-benchmark
        tests/image_content_detection_performance_benchmark.cpp)
    target_link_libraries(snow-shot-image-content-detection-performance-benchmark PRIVATE
        snow_shot_image_codec)
    if(WIN32 AND NOT SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
        _snow_shot_stage_image_runtime(snow-shot-image-content-detection-performance-benchmark)
    endif()
    add_test(NAME snow-shot-image-content-detection-performance-benchmark
        COMMAND snow-shot-image-content-detection-performance-benchmark)
    set_tests_properties(snow-shot-image-content-detection-performance-benchmark PROPERTIES
        LABELS benchmark TIMEOUT 120
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

if(SNOW_APPS_BUILD_BENCHMARKS)
    add_executable(snow-shot-pinned-shake-performance-benchmark
        tests/screenshot_pinned_shake_performance_benchmark.cpp)
    target_include_directories(snow-shot-pinned-shake-performance-benchmark PRIVATE
        src/presentation/pinned)
    target_link_libraries(snow-shot-pinned-shake-performance-benchmark PRIVATE Qt6::Core)
    add_test(NAME snow-shot-pinned-shake-performance-benchmark
        COMMAND snow-shot-pinned-shake-performance-benchmark)
    set_tests_properties(snow-shot-pinned-shake-performance-benchmark PROPERTIES
        LABELS "benchmark" TIMEOUT 60)
    if(TARGET snow-shot-pinned-window-tests)
        add_test(NAME snow-shot-pinned-shake-drag-performance-benchmark
            COMMAND snow-shot-pinned-window-tests --shake-sampling-performance-only -platform offscreen)
        set_tests_properties(snow-shot-pinned-shake-drag-performance-benchmark PROPERTIES
            LABELS "benchmark" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
endif()
if(TARGET snow-shot-pinned-window-tests)
    add_test(NAME snow-shot-pinned-shake-window-tests
        COMMAND snow-shot-pinned-window-tests --shake-hide-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-shake-window-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        add_test(NAME snow-shot-pinned-shake-native-tests
            COMMAND snow-shot-pinned-window-tests --shake-hide-native-only -platform windows)
        set_tests_properties(snow-shot-pinned-shake-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
endif()
if(TARGET snow-shot-settings-catalog-tests)
    add_test(NAME snow-shot-pinned-shake-settings-catalog-tests
        COMMAND snow-shot-settings-catalog-tests --pinned-shake-only)
    set_tests_properties(snow-shot-pinned-shake-settings-catalog-tests PROPERTIES LABELS "unit")
endif()
if(TARGET snow-shot-settings-page-loading-tests)
    add_test(NAME snow-shot-pinned-shake-settings-tests
        COMMAND snow-shot-settings-page-loading-tests --pinned-shake-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-shake-settings-tests PROPERTIES
        LABELS "unit" SNOW_TEST_CROSS_PLATFORM TRUE RUN_SERIAL TRUE TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(SNOW_SHOT_BUILD_TESTS)
    include(SnowShotTestResources)
endif()

# All focused fixtures and edition clones now exist. Resolve their source-owned
# capabilities before the deferred Mini audit inspects the completed link graphs.
snow_shot_resolve_source_dependencies()
