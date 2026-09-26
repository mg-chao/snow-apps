if(APPLE)
    set_source_files_properties(src/presentation/core/screenshotgeometry.cpp
        src/presentation/recording/screenrecordingareawindow.cpp
        tests/screen_recording_shortcut_tests.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

cmake_minimum_required(VERSION 3.30)

project(snow_shot VERSION ${SNOW_SHOT_VERSION_NUMERIC} LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(RustStaticLibrary)

set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

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

set(SNOW_PINNED_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/pinned/pinnedwindowplatform.cpp)
if(WIN32)
    list(APPEND SNOW_PINNED_PLATFORM_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/pinnedwindowevents.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/pinnedwindownative.cpp)
endif()
if(APPLE)
    list(APPEND SNOW_PINNED_PLATFORM_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/pinnedwindowplatform.mm)
endif()

option(SNOW_SHOT_ENABLE_STRICT_COMPILE "Enable strict compiler warnings and treat warnings as errors." ON)
option(SNOW_SHOT_BUILD_TESTS "Build Snow Shot test executables." OFF)
option(SNOW_SHOT_BUILD_IMAGE_CODEC_SMOKE_TEST
    "Build the Snow Shot image-codec smoke test without the full QtTest suite." OFF)
option(SNOW_SHOT_BUILD_BENCHMARKS "Build opt-in native Snow Shot benchmarks." OFF)
option(SNOW_SHOT_SCROLLING_PERF_DETAIL "Compile detailed scrolling C++ and Rust timings." OFF)
option(SNOW_SHOT_RELEASE_STATIC "Prefer static Snow Shot release linkage." OFF)
option(SNOW_SHOT_QT_STATIC "Link Snow Shot against a static Qt build." OFF)
set(SNOW_SHOT_WEBSITE_URL "https://snowshot.top" CACHE STRING "Public Snow Shot website URL.")
set(SNOW_SHOT_PROJECT_URL "https://github.com/mg-chao/snow-apps" CACHE STRING
    "Public Snow Shot source repository URL.")
set(SNOW_SHOT_QQ_GROUP_2_URL "https://qm.qq.com/q/hRIRFED9Ze" CACHE STRING
    "QQ group 2 join URL.")
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

find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS Widgets LinguistTools Svg Network)
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
            "SNOW_SHOT_QT_STATIC requires a static Qt 6.11.1 kit. "
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
find_package(Qt6GuiPrivate 6.11.1 EXACT REQUIRED CONFIG
    PATHS "${Qt6_DIR}/.."
    NO_DEFAULT_PATH
)
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
    set(SNOW_SHOT_LIBCLANG_DIR
        "${SNOW_LIBCLANG_BIN_DIR}"
        CACHE PATH
        "Directory containing libclang.dll for ffmpeg-sys bindgen."
    )
    if(EXISTS "${SNOW_SHOT_LIBCLANG_DIR}/libclang.dll")
        set(ENV{LIBCLANG_PATH} "${SNOW_SHOT_LIBCLANG_DIR}")
    endif()
endif()

if(NOT TARGET snow_capture_c)
    set(_snow_scrolling_perf_features)
    if(SNOW_SHOT_SCROLLING_PERF_DETAIL)
        list(APPEND _snow_scrolling_perf_features snow-stitch-images-c/perf-instrumentation)
    endif()
    snow_add_rust_static_libraries(snow_shot_rust_ffi
        MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
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
        MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}" OUTPUT_NAME snow_recording_c)
endif()

if(NOT TARGET snow_recording_c)
    snow_add_rust_static_library(snow_recording_c PACKAGE snow-recording-c
        MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}" OUTPUT_NAME snow_recording_c)
endif()

if(NOT TARGET snow_visual_region_detector_c)
    snow_add_rust_static_library(snow_visual_region_detector_c PACKAGE snow-visual-region-detector-c MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}" OUTPUT_NAME snow_visual_region_detector_c)
endif()

if(NOT TARGET snow_selected_text_c)
    snow_add_rust_static_library(snow_selected_text_c
        PACKAGE snow-selected-text-c
        MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        OUTPUT_NAME snow_selected_text_c
        STRIP_MSVC_DIRECTIVES)
    target_include_directories(snow_selected_text_c INTERFACE
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-selected-text-c/include")
    if(APPLE)
        find_library(SNOW_SELECTED_TEXT_APPKIT_FRAMEWORK NAMES AppKit REQUIRED)
        find_library(SNOW_SELECTED_TEXT_APPLICATION_SERVICES_FRAMEWORK
            NAMES ApplicationServices REQUIRED)
        find_library(SNOW_SELECTED_TEXT_CORE_FOUNDATION_FRAMEWORK NAMES CoreFoundation REQUIRED)
        find_library(SNOW_SELECTED_TEXT_CORE_GRAPHICS_FRAMEWORK NAMES CoreGraphics REQUIRED)
        target_link_libraries(snow_selected_text_c INTERFACE
            "${SNOW_SELECTED_TEXT_APPKIT_FRAMEWORK}"
            "${SNOW_SELECTED_TEXT_APPLICATION_SERVICES_FRAMEWORK}"
            "${SNOW_SELECTED_TEXT_CORE_FOUNDATION_FRAMEWORK}"
            "${SNOW_SELECTED_TEXT_CORE_GRAPHICS_FRAMEWORK}")
    endif()
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
# ONNX Runtime backs the standalone OCR process: static release builds link
# its closure into snow-ocr-process.exe, dynamic builds stage its DLL.
find_package(onnxruntime CONFIG REQUIRED)
find_package(minizip-ng CONFIG REQUIRED)

if(WIN32)
    set(SNOW_SHOT_DIRECTML_RUNTIME_DLL_RELEASE
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/DirectML.dll")
    if(SNOW_SHOT_RELEASE_STATIC)
        set(SNOW_SHOT_DIRECTML_RUNTIME_DLL_DEBUG
            "${SNOW_SHOT_DIRECTML_RUNTIME_DLL_RELEASE}")
        set(_snow_shot_directml_runtimes
            "${SNOW_SHOT_DIRECTML_RUNTIME_DLL_RELEASE}")
    else()
        set(SNOW_SHOT_DIRECTML_RUNTIME_DLL_DEBUG
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/DirectML.dll")
        set(_snow_shot_directml_runtimes
            "${SNOW_SHOT_DIRECTML_RUNTIME_DLL_DEBUG}"
            "${SNOW_SHOT_DIRECTML_RUNTIME_DLL_RELEASE}")
    endif()
    foreach(_snow_shot_directml_runtime IN LISTS _snow_shot_directml_runtimes)
        if(NOT EXISTS "${_snow_shot_directml_runtime}")
            message(FATAL_ERROR
                "The DirectML-enabled ONNX Runtime package is incomplete: "
                "${_snow_shot_directml_runtime} was not found.")
        endif()
    endforeach()
    unset(_snow_shot_directml_runtime)
    unset(_snow_shot_directml_runtimes)
endif()

# The standalone OCR process owns every OCR dependency. Static release builds
include(SnowDiagnostics)

# The standalone OCR process owns every OCR dependency. Static release builds
# link the vcpkg ONNX Runtime closure into the executable itself; the dynamic
# workspace builds keep loading the staged onnxruntime.dll at runtime.
set(SNOW_SHOT_OCR_STATIC_ONNXRUNTIME OFF)
if(SNOW_SHOT_RELEASE_STATIC OR SNOW_APPS_RELEASE_STATIC)
    set(SNOW_SHOT_OCR_STATIC_ONNXRUNTIME ON)
endif()

# Depth-first walk of the onnxruntime::onnxruntime imported-target closure.
# Depth-first expansion keeps every library behind its dependents, matching
# the order the previous in-process C++ link received from CMake. Interface
# libraries are header-only and contribute no linker input; configuration
# generator expressions (Debug-only diagnostics helpers) are skipped.
function(_snow_shot_onnxruntime_link_items out_items)
    set(_pending onnxruntime::onnxruntime)
    set(_visited)
    set(_items)
    while(_pending)
        list(POP_FRONT _pending _current)
        # A LINK_ONLY wrapper may carry either a library file or another
        # target name; strip it before resolving the item.
        string(REGEX REPLACE "^\\$<LINK_ONLY:(.+)>$" "\\1" _current "${_current}")
        if("${_current}" IN_LIST _visited)
            continue()
        endif()
        list(APPEND _visited "${_current}")
        if(NOT TARGET "${_current}")
            string(FIND "${_current}" "$<" _genex_pos)
            if(NOT _current STREQUAL "" AND _genex_pos EQUAL -1)
                list(APPEND _items "${_current}")
            endif()
            continue()
        endif()
        get_target_property(_current_type "${_current}" TYPE)
        if(NOT _current_type STREQUAL "INTERFACE_LIBRARY")
            list(APPEND _items "$<TARGET_FILE:${_current}>")
        endif()
        get_target_property(_current_deps "${_current}" INTERFACE_LINK_LIBRARIES)
        if(_current_deps)
            list(PREPEND _pending ${_current_deps})
        endif()
    endwhile()
    get_target_property(_link_options onnxruntime::onnxruntime INTERFACE_LINK_OPTIONS)
    if(_link_options)
        foreach(_link_option IN LISTS _link_options)
            list(APPEND _items "${_link_option}")
        endforeach()
    endif()
    set(${out_items} "${_items}" PARENT_SCOPE)
endfunction()

set(SNOW_SHOT_OCR_PROCESS_FEATURES)
set(SNOW_SHOT_OCR_PROCESS_NO_DEFAULT_FEATURES)
set(SNOW_SHOT_OCR_PROCESS_ENVIRONMENT)
if(SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
    _snow_shot_onnxruntime_link_items(_snow_shot_onnxruntime_link_items)
    if(NOT _snow_shot_onnxruntime_link_items)
        message(FATAL_ERROR
            "The static ONNX Runtime link closure for the OCR process is empty.")
    endif()
    list(JOIN _snow_shot_onnxruntime_link_items "\n" _snow_shot_onnxruntime_link_manifest)
    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/snow-ocr-process-onnxruntime-$<CONFIG>.rsp"
        CONTENT "${_snow_shot_onnxruntime_link_manifest}\n")
    set(SNOW_SHOT_OCR_PROCESS_FEATURES static-onnx-runtime)
    if(WIN32)
        list(APPEND SNOW_SHOT_OCR_PROCESS_FEATURES directml-provider)
    endif()
    set(SNOW_SHOT_OCR_PROCESS_NO_DEFAULT_FEATURES NO_DEFAULT_FEATURES)
    set(SNOW_SHOT_OCR_PROCESS_ENVIRONMENT
        "SNOW_ONNXRUNTIME_LINK_FILE=${CMAKE_CURRENT_BINARY_DIR}/generated/snow-ocr-process-onnxruntime-$<CONFIG>.rsp")
endif()

if(APPLE AND NOT SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
    set(SNOW_SHOT_OCR_PROCESS_NO_DEFAULT_FEATURES NO_DEFAULT_FEATURES)
    set(SNOW_SHOT_OCR_PROCESS_FEATURES dynamic-onnx-runtime)
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
    ${SNOW_SHOT_OCR_PROCESS_NO_DEFAULT_FEATURES}
    FEATURES ${SNOW_SHOT_OCR_PROCESS_FEATURES}
    ENVIRONMENT ${SNOW_SHOT_OCR_PROCESS_ENVIRONMENT}
)

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
    Qt6::Core
    Qt6::Gui
)
target_compile_features(snow_shot_image_codec PUBLIC cxx_std_20)

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
    include/snow_shot/storage/persistedselectioncodec.h
    include/snow_shot/storage/persistedwindowgeometry.h
    include/snow_shot/storage/pinnedwindowrepository.h
    include/snow_shot/storage/pinnedwindowtypes.h
    include/snow_shot/storage/settingsadapters.h
    include/snow_shot/storage/storagelogging.h
    include/snow_shot/storage/storageresult.h
    include/snow_shot/storage/storageusagetracker.h
    src/storage/applicationstorage.cpp
    src/storage/capturehistorypolicy_p.h
    src/storage/capturehistoryrepository.cpp
    src/storage/configurationarchive.cpp
    src/storage/configurationstore.cpp
    src/storage/configurationschema.cpp
    src/storage/persistedselectioncodec.cpp
    src/storage/persistedwindowgeometry.cpp
    src/storage/pinnedwindowrepository.cpp
    src/storage/preparedpngimage.cpp
    src/storage/settingsadapters.cpp
    src/storage/storagedirectoryutils_p.h
    src/storage/storagelogging.cpp
    src/storage/storageusagetracker.cpp
)
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

add_library(snow_shot_storage STATIC ${SNOW_SHOT_STORAGE_SOURCES})
snow_enable_unity_build(snow_shot_storage)
# Repository implementations have independent anonymous-namespace helpers.
set_source_files_properties(
    src/storage/capturehistoryrepository.cpp
    src/storage/pinnedwindowrepository.cpp
    src/storage/configurationschema.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(snow_shot_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_storage PUBLIC
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
    src/app/translationservice.cpp
    include/snow_shot/presentation/selectedtexttranslationcontroller.h
    src/presentation/services/selectedtexttranslationcontroller.cpp
    include/snow_shot/network/snowshotapiclient.h
    src/network/snowshotapiclient.cpp
    include/snow_shot/presentation/translationlanguages.h
    src/presentation/services/translationlanguages.cpp
    include/snow_shot/presentation/translationpagecontroller.h
    src/presentation/services/translationpagecontroller.cpp
)
snow_enable_unity_build(snow_shot_translation)
target_include_directories(snow_shot_translation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_translation PUBLIC snow_shot_storage Qt6::Network
    PRIVATE snow_selected_text_c)
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

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowShotTranslations.cmake")

set(SNOW_SHOT_SETTINGS_CATALOG_SOURCES
    include/snow_shot/presentation/settings/settingscatalog.h
    include/snow_shot/presentation/settings/settingsregistry.h
    src/presentation/components/icons/snowshoticons.cpp
    src/presentation/settings/settingscatalog.cpp
    src/presentation/settings/settingsregistry.cpp
)
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

set(SNOW_SHOT_SETTINGS_SOURCES
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
    include/snow_shot/presentation/components/globalmouserow.h
    include/snow_shot/presentation/components/toolbareditorsettingswidget.h
    include/snow_shot/presentation/components/infotooltipicon.h
    include/snow_shot/presentation/components/pagecontainerwidget.h
    include/snow_shot/presentation/components/sectionheaderwidget.h
    include/snow_shot/presentation/components/screenshothistorypagewidget.h
    include/snow_shot/presentation/components/emptystateicon.h
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
    include/snow_shot/presentation/settings/textrecognitionacceleration.h
    include/snow_shot/presentation/styles/thememanager.h
    src/presentation/components/aboutpagewidget.cpp
    src/presentation/components/infotooltipicon.cpp
    src/presentation/components/globalmouserow.cpp
    src/presentation/components/pathinput.cpp
    src/presentation/components/pagecontainerwidget.cpp
    src/presentation/components/sectionheaderwidget.cpp
    src/presentation/components/screenshothistorypagewidget.cpp
    include/snow_shot/presentation/components/historyselectionbar.h
    include/snow_shot/presentation/components/historypagecommon.h
    src/presentation/components/historypagecommon.cpp
    include/snow_shot/presentation/components/thumbnailcache.h
    src/presentation/components/thumbnailcache.cpp
    include/snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h
    src/presentation/components/pinnedwindowmanagementpagewidget.cpp
    src/presentation/services/screenshotclipboardcontent.cpp
    src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
    src/presentation/components/customaimodelssettingswidget.cpp
    src/presentation/components/settingscustomwidget.cpp
    src/presentation/components/settingspagewidget.cpp
    src/presentation/components/actionrow.cpp
    src/presentation/components/shortcutkeyrow.cpp
    src/presentation/components/shortcutconfigurationbutton.cpp
    src/presentation/components/storagestatussettingswidget.cpp
    src/presentation/settings/settingsbackend.cpp
    src/presentation/settings/settingsruntimesession.cpp
    src/presentation/settings/applicationpriority.cpp
    src/presentation/settings/textrecognitionacceleration.cpp
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
add_library(snow_shot_login_item STATIC
    include/snow_shot/platform/macos/loginitemservice.h
    src/platform/macos/loginitemservice.cpp)
target_include_directories(snow_shot_login_item PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_login_item PUBLIC Qt6::Core)
if(APPLE)
    target_sources(snow_shot_login_item PRIVATE src/platform/macos/loginitembackend.mm)
    target_link_libraries(snow_shot_login_item PRIVATE snow_shot_storage
        "-framework AppKit" "-framework Carbon" "-framework Security" "-framework ServiceManagement")
endif()
if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-login-item-tests tests/login_item_tests.cpp)
    target_link_libraries(snow-shot-login-item-tests PRIVATE snow_shot_login_item)
    add_test(NAME snow-shot-login-item-tests COMMAND snow-shot-login-item-tests)
    set_tests_properties(snow-shot-login-item-tests PROPERTIES LABELS unit)
endif()
add_library(snow_shot_administrator STATIC
    src/platform/windows/autostartregistration.cpp
    src/platform/windows/administratorlaunch.cpp)
target_include_directories(snow_shot_administrator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_administrator PUBLIC Qt6::Core Qt6::Network)
if(WIN32)
    target_link_libraries(snow_shot_administrator PRIVATE taskschd ole32 oleaut32 advapi32 shell32 uuid comsuppw)
endif()
if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-administrator-launch-tests tests/administrator_launch_tests.cpp)
    target_link_libraries(snow-shot-administrator-launch-tests PRIVATE snow_shot_administrator)
    add_test(NAME snow-shot-administrator-launch-tests COMMAND snow-shot-administrator-launch-tests)
    set_tests_properties(snow-shot-administrator-launch-tests PROPERTIES LABELS "unit;windows" TIMEOUT 30)

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
add_library(snow_shot_settings STATIC ${SNOW_SHOT_SETTINGS_SOURCES}
    include/snow_shot/presentation/permissionguidecontroller.h
    include/snow_shot/presentation/components/permissionguidewidget.h
    src/presentation/services/permissionguidecontroller.cpp
    src/presentation/components/permissionguidewidget.cpp)
if(APPLE)
    target_sources(snow_shot_settings PRIVATE src/platform/macos/permissionguideplatform.mm)
endif()
target_link_libraries(snow_shot_settings PUBLIC snow_shot_permissions snow_shot_login_item)
qt_add_resources(snow_shot_settings "snow_shot_about_assets"
    PREFIX "/snow_shot/about"
    BASE "resources/about"
    FILES resources/about/illustration.svg
)
target_compile_definitions(snow_shot_settings PRIVATE
    SNOW_SHOT_WEBSITE_URL="${SNOW_SHOT_WEBSITE_URL}"
    SNOW_SHOT_PROJECT_URL="${SNOW_SHOT_PROJECT_URL}"
    SNOW_SHOT_QQ_GROUP_2_URL="${SNOW_SHOT_QQ_GROUP_2_URL}"
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
target_include_directories(snow_shot_settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_settings PUBLIC
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

set(PROJECT_HEADERS
    include/snow_shot/app/featureavailability.h
    include/snow_shot/presentation/screenshotencodingsettings.h
    include/snow_shot/presentation/directcapturecontroller.h
    include/snow_shot/presentation/directcaptureworkflow.h
    include/snow_shot/presentation/directcapturehistory.h
    include/snow_shot/presentation/screenshottabledocument.h
    include/snow_shot/presentation/screenshottableeditor.h
    include/snow_shot/app/applicationcontroller.h
    include/snow_shot/app/applicationrestart.h
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
    include/snow_shot/presentation/screenshotcolorpickercontroller.h
    include/snow_shot/presentation/screenshotcolorpickerwindow.h
    include/snow_shot/presentation/screenshotcontroller.h
    include/snow_shot/presentation/screenshotdialogowner.h
    include/snow_shot/presentation/screenshotshortcutexitconfirmation.h
    include/snow_shot/presentation/pinnedwindowgroupmanager.h
    include/snow_shot/presentation/globalshortcuttypes.h
    include/snow_shot/presentation/screenshotdefaultstyles.h
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
    include/snow_shot/presentation/systemtraycontroller.h
)

set(PROJECT_CXX_SOURCES
    ${SNOW_SELECTED_FILES_SOURCES}
    src/presentation/services/screenshotfilepinbatch.cpp
    src/presentation/capture/directcapturecontroller.cpp
    src/presentation/capture/directcapturenative.cpp
    src/presentation/capture/directcaptureworkflow.cpp
    src/presentation/capture/directcapturehistory.cpp
    src/presentation/capture/camerashuttersound.cpp
    src/app/applicationcontroller.cpp
    include/snow_shot/presentation/fullscreencanvascontroller.h
    include/snow_shot/presentation/fullscreencanvaswindow.h
    include/snow_shot/presentation/fullscreencanvasstylepanel.h
    src/presentation/canvas/fullscreencanvascontroller.cpp
    src/presentation/canvas/fullscreencanvaswindow.cpp
    src/presentation/canvas/fullscreencanvasstylepanel.cpp
    src/presentation/canvas/fullscreencanvaslaser.cpp
    src/app/applicationrestart.cpp
    src/app/featureavailability.cpp
    src/app/main.cpp
    src/app/singleinstancecoordinator.cpp
    src/platform/physicalcursor.cpp
    src/platform/windows/monitorgeometry.cpp
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
    src/presentation/capture/latestbridgemailbox.h
    src/presentation/capture/screenshotscrollingcapturecontroller.cpp
    src/presentation/capture/screenshotscrollingpipeline.cpp
    src/presentation/capture/screenshotscrollingnativesource.cpp
    src/presentation/capture/screenshotscrollingthumbnailwidget.cpp
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
    src/presentation/services/screenshotexportartifact.cpp
    src/presentation/services/screenshotexportpinnedsource.cpp
    src/presentation/ocr/screenshotrecognitionimage.cpp
    src/presentation/services/screenshotexportscrollingsource.cpp
    src/presentation/services/screenshotimagefileservice.cpp
    src/presentation/services/screenshotrecognitionfileexport.cpp
        src/presentation/services/screenshotpdfexport.cpp
    src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
    src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
    src/presentation/services/historypinplacement.cpp
    src/presentation/services/screenshotselectionpin.cpp
    src/presentation/core/screenshotgeometry.cpp
    src/presentation/core/screenshotcanvastoolstyles.cpp
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
    src/presentation/ocr/screenshotqrrecognitionservice.cpp
    src/presentation/ocr/screenshotocrpresentation.cpp
    src/presentation/ocr/screenshotocrvisuals.cpp
    src/presentation/ocr/screenshotocrtexteditingsession.cpp
    src/presentation/pinned/screenshotpinnednativegeometrycontroller.cpp
    src/presentation/pinned/screenshotpinnednativegeometrycontroller.h
    src/presentation/pinned/screenshotpinnedclickthroughgeometry.cpp
    src/presentation/pinned/screenshotpinnedclickthroughgeometry.h
    src/presentation/pinned/screenshotpinnedinteraction.cpp
    ${SNOW_PINNED_PLATFORM_SOURCES}
    src/presentation/pinned/screenshotpinnededitcontroller.cpp
    src/presentation/pinned/screenshotpinnedresizegeometry.cpp
    src/presentation/pinned/screenshotpinnedrestoregeometry.cpp
    src/presentation/pinned/screenshotpinnedhidetotopcontroller.cpp
    src/presentation/pinned/screenshotpinnedwindow.cpp
    src/presentation/pinned/screenshotpintoperfinstrumentation.cpp
    src/presentation/services/screenshotpresentationservices.cpp
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
    src/presentation/overlay/screenshotselectionshadowrenderer.cpp
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
    src/presentation/tools/screenshottoolpalettehost.cpp
    src/presentation/toolbar/screenshottoolbarwindow.cpp
    src/presentation/recording/screenrecordingareawindow.cpp
    src/presentation/recording/screenrecordingcontroller.cpp
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
        src/platform/macos/screenshotnative.mm
        src/platform/macos/windowcaptureexclusion.mm)
    list(APPEND PROJECT_HEADERS include/snow_shot/platform/macos/applicationactivation.h)
endif()

if(MSVC)
    set_source_files_properties(src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
    set_source_files_properties(src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx2")
endif()

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
if(APPLE AND SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
    target_compile_definitions(snow_shot PRIVATE SNOW_SHOT_OCR_STATIC_ONNXRUNTIME=1)
endif()
if(WIN32)
    target_compile_definitions(snow_shot PRIVATE
        SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE=\"${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}\"
    )
endif()
target_link_libraries(snow_shot PRIVATE
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

if(APPLE)
    target_link_libraries(snow_shot PRIVATE ${SNOW_SELECTED_FILES_LIBRARIES})
    target_sources(snow_shot PRIVATE src/platform/macos/camerashuttersound.mm)
    target_link_libraries(snow_shot PRIVATE "-framework AVFoundation" "-framework Foundation" objc)
    qt_add_resources(snow_shot "snow_shot_shutter_audio"
        PREFIX "/snow-shot"
        BASE "resources"
        FILES "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}")
endif()

if(EXISTS "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}")
    add_custom_command(TARGET snow_shot POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:snow_shot>/audios"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}"
            "$<TARGET_FILE_DIR:snow_shot>/audios/camera_shutter.mp3"
        COMMENT "Staging the camera shutter audio"
        VERBATIM
    )
else()
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

if(SNOW_SHOT_BUILD_TESTS)
    add_test(NAME snow-shot-translation-catalog-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/translation_catalog_tests.py")
    set_tests_properties(snow-shot-translation-catalog-tests PROPERTIES LABELS "unit")

    if(WIN32)
        find_program(SNOW_SHOT_INSTALLER_TEST_POWERSHELL NAMES pwsh powershell REQUIRED)
        add_test(NAME snow-shot-installer-running-app-tests
            COMMAND "${SNOW_SHOT_INSTALLER_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-snow-shot-installer.ps1")
        set_tests_properties(snow-shot-installer-running-app-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 180)
        add_test(NAME snow-shot-installer-i18n-tests
            COMMAND "${SNOW_SHOT_INSTALLER_TEST_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-snow-shot-installer-i18n.ps1")
        set_tests_properties(snow-shot-installer-i18n-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 180)
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

    add_executable(snow-shot-fullscreen-canvas-laser-tests
        tests/fullscreen_canvas_laser_tests.cpp
        src/presentation/canvas/fullscreencanvaslaser.cpp)
    target_link_libraries(snow-shot-fullscreen-canvas-laser-tests PRIVATE
        Qt6::Widgets snow_draw_engine_qt_canvas)
    snow_shot_import_offscreen_platform(snow-shot-fullscreen-canvas-laser-tests)
    add_test(NAME snow-shot-fullscreen-canvas-laser-tests
        COMMAND snow-shot-fullscreen-canvas-laser-tests -platform offscreen)
    set_tests_properties(snow-shot-fullscreen-canvas-laser-tests PROPERTIES
        LABELS "unit" TIMEOUT 30)

    add_executable(snow-shot-recognition-image-tests tests/screenshot_recognition_image_tests.cpp
        ${SNOW_SHOT_RECOGNITION_IMAGE_RENDER_SOURCES})
    target_include_directories(snow-shot-recognition-image-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-recognition-image-tests PRIVATE Qt6::Gui)
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
    if(WIN32 OR APPLE)
        add_executable(snow-shot-diagnostics-crash-tests tests/diagnostics_crash_tests.cpp)
        snow_add_rust_static_library(snow_diagnostics_test
            PACKAGE snow-diagnostics-test
            MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
            OUTPUT_NAME snow_diagnostics_test
            STRIP_MSVC_DIRECTIVES)
        target_link_libraries(snow-shot-diagnostics-crash-tests PRIVATE snow_diagnostics_test)
        target_link_libraries(snow-shot-diagnostics-crash-tests PRIVATE snow_shot_diagnostics snow_shot_crash_bridge)
        target_compile_definitions(snow-shot-diagnostics-crash-tests PRIVATE
            SNOW_TEST_CRASHPAD_HANDLER="${SNOW_CRASHPAD_HANDLER}")
        add_test(NAME snow-shot-diagnostics-crash-tests COMMAND snow-shot-diagnostics-crash-tests)
        set_tests_properties(snow-shot-diagnostics-crash-tests PROPERTIES LABELS "unit;crash" TIMEOUT 180)
        if(APPLE)
            set_property(TEST snow-shot-diagnostics-crash-tests APPEND PROPERTY LABELS macos)
        else()
            set_property(TEST snow-shot-diagnostics-crash-tests APPEND PROPERTY LABELS windows)
        endif()
    endif()

    snow_shot_offscreen_qpa_environment(_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT)

    # Test binaries must report failures as stderr output plus a non-zero
    # exit code instead of blocking on modal dialogs.
    snow_link_test_cli_guard(qt)

    # The image-codec backend smoke test is Qt-free; give it the Qt-free
    # dialog guard so the guard does not add a Qt runtime dependency.
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_native)

    snow_shot_add_image_codec_smoke_test()
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_qt)
    add_test(NAME snow-shot-image-codec-backend-smoke-tests
             COMMAND snow-shot-image-codec-backend-smoke-tests)

    add_executable(snow-shot-application-storage-tests
        tests/application_storage_tests.cpp
    )
    target_link_libraries(snow-shot-application-storage-tests PRIVATE snow_shot_storage Qt6::Core)
    # Flaky by design: the async policy mutation settles on timers, and
    # CPU contention from parallel tests can stretch the settling window.
    # The test must not run in parallel with other tests (RUN_SERIAL), and
    # intermittent failures are tolerated via the retry launcher.
    add_test(NAME snow-shot-application-storage-tests
        COMMAND ${CMAKE_COMMAND}
            -DTEST_COMMAND=$<TARGET_FILE:snow-shot-application-storage-tests>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_flaky_test_with_retry.cmake
    )
    add_test(NAME snow-shot-storage-quit-lifetime-tests
        COMMAND snow-shot-application-storage-tests --quit-lifetime-only)
    add_test(NAME snow-shot-draw-template-storage-tests
        COMMAND snow-shot-application-storage-tests --draw-template-only)
    set_tests_properties(snow-shot-application-storage-tests
        snow-shot-storage-quit-lifetime-tests snow-shot-draw-template-storage-tests PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-capture-history-repository-tests
        tests/capture_history_repository_tests.cpp
    )

    target_link_libraries(snow-shot-capture-history-repository-tests PRIVATE
        snow_shot_storage
        Qt6::Core
        Qt6::Gui
    )
    add_test(NAME snow-shot-capture-history-repository-tests
             COMMAND snow-shot-capture-history-repository-tests)
    set_tests_properties(snow-shot-capture-history-repository-tests PROPERTIES
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

    add_executable(snow-shot-custom-ai-models-tests tests/custom_ai_models_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_link_libraries(snow-shot-custom-ai-models-tests PRIVATE snow_shot_settings ant_design_qt Qt6::Widgets Qt6::Network)
    snow_shot_add_translations(snow-shot-custom-ai-models-tests)
    snow_shot_import_offscreen_platform(snow-shot-custom-ai-models-tests)
    add_test(NAME snow-shot-custom-ai-models-tests COMMAND snow-shot-custom-ai-models-tests -platform offscreen)
    set_tests_properties(snow-shot-custom-ai-models-tests PROPERTIES TIMEOUT 60)

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
    snow_shot_import_offscreen_platform(snow-shot-pinned-window-management-page-tests)
    add_test(NAME snow-shot-pinned-window-management-page-tests COMMAND snow-shot-pinned-window-management-page-tests)
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
    set_tests_properties(snow-shot-screenshot-history-page-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
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
    add_test(NAME snow-shot-screenshot-result-compositor-tests
             COMMAND snow-shot-screenshot-result-compositor-tests)

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
    target_link_libraries(snow-shot-canvas-color-sampler-tests PRIVATE Qt6::Widgets)
    add_test(NAME snow-shot-canvas-color-sampler-tests
             COMMAND snow-shot-canvas-color-sampler-tests)

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
        SNOW_SHOT_TEST_QQ_GROUP_2_URL="${SNOW_SHOT_QQ_GROUP_2_URL}"
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
    add_test(NAME snow-shot-settings-catalog-tests
             COMMAND snow-shot-settings-catalog-tests)
    set_tests_properties(snow-shot-settings-catalog-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

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
    set_tests_properties(snow-shot-settings-color-commit-tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    snow_shot_import_offscreen_platform(snow-shot-settings-color-commit-tests)

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

    add_executable(snow-shot-translation-settings-tests
        tests/translation_settings_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_link_libraries(snow-shot-translation-settings-tests PRIVATE
        snow_shot_settings
        snow_shot_image_codec
    )
    add_test(NAME snow-shot-translation-settings-tests COMMAND snow-shot-translation-settings-tests)
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
    target_include_directories(snow-shot-window-element-api-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector-c/include"
    )
    target_link_libraries(snow-shot-window-element-api-tests PRIVATE
        snow_shot_settings
        Qt6::Widgets
    )
    if(APPLE)
        target_link_libraries(snow-shot-window-element-api-tests PRIVATE "-framework CoreGraphics")
    endif()
    add_test(NAME snow-shot-window-element-api-tests
             COMMAND snow-shot-window-element-api-tests)
    add_test(NAME snow-shot-selector-phased-tests
             COMMAND snow-shot-window-element-api-tests --selector-only)
    add_test(NAME snow-shot-shortcut-exit-confirmation-settings-tests
             COMMAND snow-shot-window-element-api-tests
                     --shortcut-exit-confirmation-settings-only)
    snow_shot_import_offscreen_platform(snow-shot-window-element-api-tests)
    set_tests_properties(
        snow-shot-window-element-api-tests
        snow-shot-selector-phased-tests
        snow-shot-shortcut-exit-confirmation-settings-tests
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
        ant_design_qt
        snow_shot_settings_catalog
        Qt6::Widgets
    )
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
        target_sources(snow-shot-system-tray-controller-tests PRIVATE
            src/platform/macos/systemtraymenu.mm
            tests/system_tray_menu_native_tests.mm)
        target_link_libraries(snow-shot-system-tray-controller-tests PRIVATE "-framework AppKit")
        add_test(NAME snow-shot-system-tray-menu-native-tests
            COMMAND snow-shot-system-tray-controller-tests --native-menu -platform cocoa)
        set_tests_properties(snow-shot-system-tray-menu-native-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 30)
    endif()

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
    set_tests_properties(snow-shot-pinned-window-group-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    if(APPLE)
        find_package(Qt6 REQUIRED COMPONENTS Test)
        add_executable(snow-shot-action-popup-native-tests tests/action_popup_menu_native_tests.mm)
        target_include_directories(snow-shot-action-popup-native-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-action-popup-native-tests PRIVATE
            ant_design_qt Qt6::Test "-framework AppKit")
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
        src/presentation/services/shortcutrecorder.cpp
        include/snow_shot/presentation/components/infotooltipicon.h
        include/snow_shot/presentation/components/actionrow.h
        include/snow_shot/presentation/components/shortcutkeyrow.h
        include/snow_shot/presentation/components/shortcutconfigurationbutton.h
        include/snow_shot/presentation/styles/buttonborder.h
        include/snow_shot/presentation/styles/mainwindowcomponenttoken.h
        include/snow_shot/presentation/styles/thememanager.h
        include/snow_shot/presentation/styles/themecolorscheme.h
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/components/infotooltipicon.cpp
        src/presentation/components/actionrow.cpp
        src/presentation/components/shortcutkeyrow.cpp
        src/presentation/components/shortcutconfigurationbutton.cpp
        src/presentation/styles/buttonborder.cpp
        src/presentation/styles/mainwindowcomponenttoken.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
    )
    target_include_directories(snow-shot-shortcut-key-row-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-shortcut-key-row-tests PRIVATE
        snow_shot_window_shortcuts
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        Qt6::Widgets
    )
    if(WIN32)
        target_sources(snow-shot-shortcut-key-row-tests PRIVATE
            src/platform/windows/printscreenshortcutrecorder.cpp)
        target_link_libraries(snow-shot-shortcut-key-row-tests PRIVATE user32)
    endif()
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
    target_link_libraries(snow-shot-selection-effects-tests PRIVATE snow_shot_storage Qt6::Widgets)
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
            Qt6::Widgets snow_capture_c snow_recording_c PkgConfig::SNOW_SHOT_FFMPEG
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
    target_include_directories(snow-shot-capture-worker-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/capture"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-recording-c/include")
    target_link_libraries(snow-shot-capture-worker-tests PRIVATE snow_shot_storage Qt6::Gui)
    add_test(NAME snow-shot-capture-worker-tests COMMAND snow-shot-capture-worker-tests)

    if(WIN32)
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
        add_test(NAME snow-shot-recapture-cursor-hidden-native-e2e-test
                 COMMAND snow-shot-recapture-cursor-native-e2e-test --run-native --hide-overlay)
        set_tests_properties(snow-shot-recapture-cursor-native-e2e-test
            snow-shot-recapture-cursor-movement-native-e2e-test
            snow-shot-recapture-cursor-snapshot-native-e2e-test
            snow-shot-recapture-class-cursor-native-e2e-test
            snow-shot-recapture-cursor-hidden-native-e2e-test PROPERTIES
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
    )
    add_test(
        NAME snow-shot-capture-workflow-tests
        COMMAND snow-shot-capture-workflow-tests
    )

    add_executable(snow-shot-capture-modal-closer-tests
        tests/screenshot_capture_modal_closer_tests.cpp
        src/presentation/capture/screenshotcapturemodalcloser.h
    )
    target_link_libraries(snow-shot-capture-modal-closer-tests PRIVATE Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-capture-modal-closer-tests)
    add_test(NAME snow-shot-capture-modal-closer-tests
        COMMAND snow-shot-capture-modal-closer-tests)
    set_tests_properties(snow-shot-capture-modal-closer-tests PROPERTIES
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
    target_link_libraries(snow-shot-multi-region-tests PRIVATE
        Qt6::Gui snow_draw_engine_qt_canvas ant_design_qt)
    if(WIN32)
        target_link_libraries(snow-shot-multi-region-tests PRIVATE user32)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-multi-region-tests)
    add_test(NAME snow-shot-multi-region-tests COMMAND snow-shot-multi-region-tests)
    add_test(NAME snow-shot-region-overlay-input-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --region-input-only -platform offscreen)
    set_tests_properties(snow-shot-region-overlay-input-tests PROPERTIES LABELS "unit")
    add_test(NAME snow-shot-region-shape-input-tests COMMAND snow-shot-screenshot-history-tests --region-shapes-only)
    set_tests_properties(snow-shot-region-shape-input-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit")
    set_tests_properties(snow-shot-multi-region-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}" LABELS "unit")

    add_executable(snow-shot-selection-geometry-tests
        tests/screenshot_selection_geometry_tests.cpp
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
        target_sources(snow-shot-screenshot-color-picker-tests PRIVATE
            ${SNOW_PINNED_PLATFORM_SOURCES})
        target_link_libraries(snow-shot-screenshot-color-picker-tests PRIVATE "-framework AppKit")
    endif()
    add_test(NAME snow-shot-screenshot-color-picker-tests
        COMMAND snow-shot-screenshot-color-picker-tests)
    set_tests_properties(snow-shot-screenshot-color-picker-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-screenshot-history-tests
        tests/screenshot_history_tests.cpp
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
    snow_shot_import_offscreen_platform(snow-shot-screenshot-history-tests)
    add_test(
        NAME snow-shot-screenshot-completion-gesture-tests
        COMMAND snow-shot-screenshot-history-tests --completion-gestures-only
    )
    set_tests_properties(
        snow-shot-direct-capture-history-tests
        snow-shot-screenshot-completion-gesture-tests
        PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(
        NAME snow-shot-screenshot-shortcut-priority-tests
        COMMAND snow-shot-screenshot-history-tests --shortcut-input-only
    )
    add_test(
        NAME snow-shot-intelligent-selection-cursor-shortcut-tests
        COMMAND snow-shot-screenshot-history-tests
                --intelligent-selection-cursor-shortcut-only
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

    # This test is intentionally Qt-free; swap in the Qt-free dialog guard so
    # linking the guard does not add a Qt runtime dependency.
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_native)
    add_executable(snow-shot-latest-bridge-mailbox-tests
        tests/latest_bridge_mailbox_tests.cpp
        src/presentation/capture/latestbridgemailbox.h
    )
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES snow_test_cli_guard_qt)
    target_include_directories(snow-shot-latest-bridge-mailbox-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME snow-shot-latest-bridge-mailbox-tests
        COMMAND snow-shot-latest-bridge-mailbox-tests
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
        snow_shot_storage
        snow_shot_window_shortcuts
        snow_draw_engine_qt_canvas
    )
    add_test(
        NAME snow-shot-screenshot-canvas-renderer-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests
    )
    add_test(NAME snow-shot-direct-capture-history-rendering-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --direct-capture-history-rendering)
    add_test(NAME snow-shot-fractional-dpi-rendering-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --fractional-dpi)
    set_tests_properties(snow-shot-fractional-dpi-rendering-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(NAME snow-shot-scrolling-overlay-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --scrolling-overlay -platform offscreen)
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
    add_test(
        NAME snow-shot-overlay-pool-prewarm-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --overlay-pool-prewarm
    )
    set_tests_properties(
        snow-shot-screenshot-canvas-renderer-tests
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
        tests/fullscreen_canvas_tests.cpp
        tests/fullscreen_canvas_style_tests.cpp
        include/snow_shot/presentation/fullscreencanvascontroller.h
        include/snow_shot/presentation/fullscreencanvaswindow.h
        include/snow_shot/presentation/fullscreencanvasstylepanel.h
        src/presentation/canvas/fullscreencanvascontroller.cpp
        src/presentation/canvas/fullscreencanvaswindow.cpp
        src/presentation/canvas/fullscreencanvasstylepanel.cpp
        src/presentation/canvas/fullscreencanvaslaser.cpp
        tests/screenshot_pinned_hide_to_top_tests.cpp
        src/presentation/services/screenshotfilepinbatch.cpp
        tests/screenshot_pinned_window_tests.cpp
        tests/original_image_translation_tests.cpp
        include/snow_shot/presentation/components/pathinput.h
        src/presentation/components/pathinput.cpp
        include/snow_shot/presentation/screenshotsavepreviewcanvas.h
        src/presentation/services/screenshotsaveasfiledialog.cpp
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
        src/presentation/services/pinnedwindowgroupmanager.cpp
        src/presentation/pinned/screenshotpinnedinteraction.cpp
        ${SNOW_PINNED_PLATFORM_SOURCES}
        src/presentation/services/languagemanager.cpp
        src/presentation/styles/buttonborder.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
    )
    target_include_directories(snow-shot-pinned-window-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_compile_definitions(snow-shot-pinned-window-tests PRIVATE
        SNOW_SHOT_TEST_HOOKS=1
        SNOW_SHOT_TEST_PINNED_TRANSLATION=1
    )
    target_link_libraries(snow-shot-pinned-window-tests PRIVATE
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
        target_link_libraries(snow-shot-pinned-window-tests PRIVATE
            Qt6::GuiPrivate
            comctl32
            dwmapi
            user32
        )
    endif()
    add_test(
        NAME snow-shot-pinned-window-tests
        COMMAND snow-shot-pinned-window-tests
    )
    set_tests_properties(snow-shot-pinned-window-tests PROPERTIES
        LABELS "interactive;e2e;windows" RUN_SERIAL TRUE TIMEOUT 180
        ENVIRONMENT "QT_QPA_PLATFORM=windows")
    snow_shot_import_offscreen_platform(snow-shot-pinned-window-tests)
    add_test(NAME snow-shot-fullscreen-canvas-tests
        COMMAND snow-shot-pinned-window-tests --fullscreen-canvas-only -platform offscreen)
    add_test(NAME snow-shot-fullscreen-canvas-dpi-tests
        COMMAND snow-shot-pinned-window-tests --fullscreen-canvas-only -platform offscreen)
    set_tests_properties(snow-shot-fullscreen-canvas-tests PROPERTIES LABELS "unit" TIMEOUT 90)
    set_tests_properties(snow-shot-fullscreen-canvas-dpi-tests PROPERTIES LABELS "unit" TIMEOUT 90
        ENVIRONMENT "QT_SCALE_FACTOR=1.5")
    if(WIN32)
        add_test(NAME snow-shot-fullscreen-canvas-native-tests
            COMMAND snow-shot-pinned-window-tests --fullscreen-canvas-native-only -platform windows)
        set_tests_properties(snow-shot-fullscreen-canvas-native-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
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
    set_tests_properties(snow-shot-pinned-management-lifecycle-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-hide-to-top-tests
        COMMAND snow-shot-pinned-window-tests --hide-to-top-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-hide-to-top-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-hide-to-top-native-tests
        COMMAND snow-shot-pinned-window-tests --hide-to-top-native-only -platform windows)
    set_tests_properties(snow-shot-pinned-hide-to-top-native-tests PROPERTIES LABELS "unit;windows"
        RUN_SERIAL TRUE TIMEOUT 60)
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
    add_test(NAME snow-shot-pinned-remembered-drawing-tool-tests
        COMMAND snow-shot-pinned-window-tests --remembered-drawing-tool-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-remembered-drawing-tool-tests PROPERTIES LABELS "unit"
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-click-through-native-tests
        COMMAND snow-shot-pinned-window-tests --click-through-native-only -platform windows)
    set_tests_properties(snow-shot-pinned-click-through-native-tests PROPERTIES
        LABELS "interactive;e2e;windows" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-pinned-shortcut-display-tests
        COMMAND snow-shot-pinned-window-tests --shortcut-display-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-shortcut-display-tests PROPERTIES LABELS "unit")
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
        COMMAND snow-shot-pinned-window-tests --thumbnail-reentry-only)
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
    add_test(NAME snow-shot-pinned-save-dialog-tests
        COMMAND snow-shot-pinned-window-tests --save-dialog-only)
    set_tests_properties(snow-shot-pinned-save-dialog-tests PROPERTIES TIMEOUT 90)
    add_test(
        NAME snow-shot-pinned-toolbar-parity-tests
        COMMAND snow-shot-pinned-window-tests --toolbar-parity-only
    )
    add_test(NAME snow-shot-pinned-translation-tests
        COMMAND snow-shot-pinned-window-tests --translation-only)
    add_test(
        NAME snow-shot-pinned-movement-shortcut-tests
        COMMAND snow-shot-pinned-window-tests --movement-shortcut-only
    )
    add_test(
        NAME snow-shot-pinned-drawing-shortcut-toggle-tests
        COMMAND snow-shot-pinned-window-tests --drawing-shortcut-toggle-only
    )
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
    )
    target_include_directories(snow-shot-recognition-file-export-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-recognition-file-export-tests PRIVATE Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-recognition-file-export-tests)
    add_test(NAME snow-shot-recognition-file-export-tests
             COMMAND snow-shot-recognition-file-export-tests)
    set_tests_properties(snow-shot-recognition-file-export-tests PROPERTIES
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

    add_executable(snow-shot-file-pin-batch-tests
        tests/screenshot_file_pin_batch_tests.cpp
        ${SNOW_SELECTED_FILES_SOURCES}
        src/presentation/services/screenshotfilepinbatch.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
        include/snow_shot/presentation/screenshotexportcoordinator.h
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
    )
    target_include_directories(snow-shot-file-pin-batch-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-file-pin-batch-tests PRIVATE snow_shot_image_codec Qt6::Widgets
        ${SNOW_SELECTED_FILES_LIBRARIES})
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

    add_executable(snow-shot-clipboard-content-tests
        tests/screenshot_clipboard_content_tests.cpp
        include/snow_shot/presentation/screenshotclipboardcontent.h
        src/presentation/services/screenshotclipboardcontent.cpp
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
        LABELS "unit" TIMEOUT 90)
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
    add_test(NAME snow-shot-pinned-toolbar-layout-tests
        COMMAND snow-shot-pinned-window-tests --pinned-toolbar-layout-only)
    set_tests_properties(snow-shot-pinned-toolbar-layout-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-pinned-recognition-shortcut-tests
        COMMAND snow-shot-pinned-window-tests --recognition-shortcut-only)
    add_test(NAME snow-shot-pinned-recognition-shortcut-offscreen-tests
        COMMAND snow-shot-pinned-window-tests --recognition-shortcut-only -platform offscreen)
    add_test(NAME snow-shot-pinned-recognition-context-menu-tests
        COMMAND snow-shot-pinned-window-tests --recognition-context-menu-only -platform offscreen)
    set_tests_properties(snow-shot-pinned-recognition-context-menu-tests PROPERTIES
        LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-pinned-transformed-ocr-tests
        COMMAND snow-shot-pinned-window-tests --transformed-ocr-only)
    add_test(NAME snow-shot-pinned-selection-restore-tests
        COMMAND snow-shot-pinned-window-tests --selection-restore-only -platform offscreen)
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
    set_tests_properties(snow-shot-export-coordinator-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-export-dialog-workflow-tests
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        tests/screenshot_export_dialog_workflow_tests.cpp
        include/snow_shot/presentation/screenshotsavepreviewcanvas.h
        src/presentation/services/screenshotsaveasfiledialog.cpp
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
    snow_shot_import_offscreen_platform(snow-shot-export-dialog-workflow-tests)
    add_test(NAME snow-shot-pdf-dialog-tests COMMAND snow-shot-export-dialog-workflow-tests --pdf)
    set_tests_properties(snow-shot-pdf-dialog-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-export-dialog-workflow-tests
        COMMAND snow-shot-export-dialog-workflow-tests)
    add_test(NAME snow-shot-export-dialog-shortcut-tests
        COMMAND snow-shot-export-dialog-workflow-tests --shortcuts)
    add_test(NAME snow-shot-export-preview-wheel-tests
        COMMAND snow-shot-export-dialog-workflow-tests --wheel-zoom -platform offscreen)
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
        TIMEOUT 180
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
        Qt6::Widgets
    )
    if(WIN32)
        target_link_libraries(snow-shot-export-artifact-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-export-artifact-tests COMMAND snow-shot-export-artifact-tests)
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
    set_tests_properties(snow-shot-pinned-recognition-save-tests PROPERTIES LABELS "unit" TIMEOUT 90)
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

    add_executable(snow-shot-screenshot-tool-palette-tests
        tests/screenshot_tool_palette_tests.cpp
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/services/languagemanager.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
        src/presentation/tools/screenshottoolpalette.cpp
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
    snow_shot_add_translations(snow-shot-screenshot-tool-palette-tests)
    add_test(NAME snow-shot-selection-display-unit-toolbar-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --selection-unit-only -platform offscreen)
    set_tests_properties(snow-shot-selection-display-unit-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-move-region-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --move-options-only -platform offscreen)
    set_tests_properties(snow-shot-move-region-toolbar-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-region-switcher-style-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --region-switcher-only -platform offscreen)
    set_tests_properties(snow-shot-region-switcher-style-tests PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(
        NAME snow-shot-screenshot-tool-palette-tests
        COMMAND snow-shot-screenshot-tool-palette-tests
    )
    add_test(NAME snow-shot-scrolling-toolbar-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --scrolling-only -platform offscreen)
    set_tests_properties(snow-shot-scrolling-toolbar-tests PROPERTIES LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-screenshot-tool-palette-remembered-drawing-tool-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --remembered-drawing-tool-only
            -platform offscreen)
    set_tests_properties(snow-shot-screenshot-tool-palette-remembered-drawing-tool-tests
        PROPERTIES LABELS "unit" TIMEOUT 60)
    add_test(NAME snow-shot-spotlight-wheel-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --spotlight-wheel-only -platform offscreen)
    set_tests_properties(snow-shot-spotlight-wheel-tests PROPERTIES LABELS "unit" TIMEOUT 60)
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
    add_test(
        NAME snow-shot-screenshot-action-toolbar-layout-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --action-toolbar-layout-only
    )
    add_test(
        NAME snow-shot-toolbar-color-input-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --color-input-only -platform offscreen
    )
    add_test(
        NAME snow-shot-screenshot-tool-shortcut-toggle-tests
        COMMAND snow-shot-screenshot-tool-palette-tests --tool-shortcuts-only
    )
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
        snow-shot-screenshot-shortcut-tooltip-tests
        snow-shot-screenshot-selection-reset-tests
        snow-shot-screen-recording-toolbar-tests
        snow-shot-toolbar-shared-controls-tests
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
    target_include_directories(snow-shot-screenshot-selection-toolbar-hover-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/selection"
    )
    target_link_libraries(snow-shot-screenshot-selection-toolbar-hover-tests PRIVATE
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
    set_tests_properties(snow-shot-screenshot-selection-toolbar-hover-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    if(WIN32)
        set_tests_properties(snow-shot-screenshot-selection-toolbar-hover-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
        )
    else()
        set_tests_properties(snow-shot-screenshot-selection-toolbar-hover-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        )
    endif()

    add_executable(snow-shot-screenshot-toolbar-remembered-tool-tests
        tests/screenshot_toolbar_remembered_tool_tests.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshottoolbarwindow.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/platform/windows/monitorgeometry.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
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
        src/presentation/toolbar/screenshottoolbarwindow.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
    )
    target_include_directories(snow-shot-screenshot-toolbar-remembered-tool-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-toolbar-remembered-tool-tests PRIVATE
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
    add_test(NAME snow-shot-screenshot-toolbar-remembered-tool-tests
        COMMAND snow-shot-screenshot-toolbar-remembered-tool-tests)
    set_tests_properties(snow-shot-screenshot-toolbar-remembered-tool-tests PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

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
        # Flaky by design: drives a real hover animation over native toolbar
        # surfaces; parallel test load can steal the frame budget and stall
        # the hover transition. The test must not run in parallel with other
        # tests (RUN_SERIAL), and intermittent failures are tolerated via
        # the retry launcher.
        add_test(
            NAME snow-shot-screenshot-toolbar-fill-color-hover-tests
            COMMAND ${CMAKE_COMMAND}
                -DTEST_COMMAND=$<TARGET_FILE:snow-shot-screenshot-toolbar-fill-color-hover-tests>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_flaky_test_with_retry.cmake
        )
        set_tests_properties(
            snow-shot-screenshot-toolbar-fill-color-hover-tests
            PROPERTIES
                RUN_SERIAL TRUE
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
    )
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
    set_tests_properties(snow-shot-screen-recording-shortcut-tests PROPERTIES
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
        src/presentation/toolbar/screenshottoolbarwindow.cpp
    )
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
            ../cmake/test-support/macos_native_input.mm
            src/platform/macos/screenshotnative.mm)
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
    add_executable(snow-shot-color-picker-lifecycle-tests
        tests/screenshot_color_picker_lifecycle_tests.cpp
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
    target_include_directories(snow-shot-color-picker-lifecycle-tests PRIVATE include)
    target_link_libraries(snow-shot-color-picker-lifecycle-tests PRIVATE
        snow_shot_diagnostics snow_shot_storage snow_shot_window_shortcuts
        snow_draw_engine_qt_canvas ant_design_qt adqt_icon_core ant_design_icons_qt Qt6::Widgets)
    if(WIN32)
        target_link_libraries(snow-shot-color-picker-lifecycle-tests PRIVATE comctl32)
    elseif(APPLE)
        target_sources(snow-shot-color-picker-lifecycle-tests PRIVATE
            src/platform/macos/screenshotnative.mm
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
    add_test(NAME snow-shot-color-picker-lifecycle-tests
        COMMAND snow-shot-color-picker-lifecycle-tests)
    set_tests_properties(snow-shot-color-picker-lifecycle-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        TIMEOUT 60)
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
    # The forward and reverse drag tests drive a native toolbar through real
    # hardware-style drag sequences and assert final HWND positions within a
    # few pixels. They are flaky by design under machine load: parallel tests
    # compete for the input queue and DWM frame budget, shifting the settled
    # position. They must not run in parallel with other tests (RUN_SERIAL),
    # and intermittent failures are tolerated via the retry launcher.
    add_test(
        NAME snow-shot-floating-toolbar-drag-tests
        COMMAND ${CMAKE_COMMAND}
            -DTEST_COMMAND=$<TARGET_FILE:snow-shot-floating-toolbar-drag-tests>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_flaky_test_with_retry.cmake
    )
    add_test(
        NAME snow-shot-floating-toolbar-reverse-drag-tests
        COMMAND ${CMAKE_COMMAND}
            "-DTEST_COMMAND=$<TARGET_FILE:snow-shot-floating-toolbar-drag-tests>;--reverse-hardware-drag-only"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_flaky_test_with_retry.cmake
    )
    add_test(
        NAME snow-shot-floating-toolbar-seam-straddle-drag-tests
        COMMAND snow-shot-floating-toolbar-drag-tests --seam-straddle-drag-only
    )
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
    set_tests_properties(snow-shot-floating-toolbar-reverse-drag-tests PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-floating-toolbar-seam-straddle-drag-tests PROPERTIES
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
        set_tests_properties(snow-shot-floating-toolbar-reverse-drag-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        )
        set_tests_properties(snow-shot-floating-toolbar-seam-straddle-drag-tests PROPERTIES
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
        snow_shot_settings_catalog
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

    add_executable(snow-shot-main-content-header-tests
        tests/main_content_header_tests.cpp
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
    add_executable(snow-shot-screen-recording-controller-tests
        ${_recording_test_sources}
        tests/screen_recording_controller_tests.cpp
        include/snow_shot/presentation/screenrecordingcontroller.h
        include/snow_shot/presentation/screenrecordingfolder.h
        include/snow_shot/presentation/screenrecordingareawindow.h
        include/snow_shot/presentation/screenrecordingtoolbarwindow.h
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        src/presentation/recording/screenrecordingcontroller.cpp
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
    target_include_directories(snow-shot-screen-recording-controller-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-recording-c/include")
    target_link_libraries(snow-shot-screen-recording-controller-tests PRIVATE
        adqt_icon_core ant_design_icons_qt ant_design_qt snow_draw_engine_qt_canvas
        snow_shot_storage snow_shot_image_codec snow_shot_window_shortcuts Qt6::Widgets snow_recording_effects_c snow_capture_c)
    if(APPLE)
        target_sources(snow-shot-screen-recording-controller-tests PRIVATE
            src/platform/macos/windowcaptureexclusion.mm
            tests/macos_capture_exclusion_probe.mm)
        target_link_libraries(snow-shot-screen-recording-controller-tests PRIVATE "${SNOW_SHOT_APPKIT_FRAMEWORK}")
    endif()
    target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE SNOW_SHOT_TEST_HOOKS=1)
    target_sources(snow-shot-screen-recording-controller-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../test-support/fonts/recording_test_fonts.qrc")
    # Override only the test executable: exercise the real controller without
    # opening capture devices, recording the desktop, or exporting media.
    foreach(_operation create_direct destroy start pause resume stop state)
        target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
            "snow_recording_session_${_operation}=test_recording_session_${_operation}")
    endforeach()
    target_compile_definitions(snow-shot-screen-recording-controller-tests PRIVATE
        snow_recording_last_error_message=test_recording_last_error_message
        snow_recording_region_output_dimensions=test_recording_region_output_dimensions)
    if(WIN32)
        target_link_libraries(snow-shot-screen-recording-controller-tests PRIVATE user32 dwmapi gdi32)
        target_sources(snow-shot-screen-recording-controller-tests PRIVATE
            tests/screen_recording_toolbar_display_tests.cpp)
        add_test(NAME snow-shot-screen-recording-toolbar-display-tests
            COMMAND snow-shot-screen-recording-controller-tests
                --native-toolbar-display-only -platform windows)
        add_test(NAME snow-shot-screen-recording-toolbar-ready-display-tests
            COMMAND snow-shot-screen-recording-controller-tests
                --native-toolbar-ready-display-only -platform windows)
        set_tests_properties(snow-shot-screen-recording-toolbar-display-tests
            snow-shot-screen-recording-toolbar-ready-display-tests PROPERTIES
            LABELS "interactive;windows" RUN_SERIAL TRUE TIMEOUT 30 SKIP_RETURN_CODE 77
            ENVIRONMENT "QT_QPA_PLATFORM=windows"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-screen-recording-controller-tests
        COMMAND snow-shot-screen-recording-controller-tests)
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
    set_tests_properties(snow-shot-recording-effects-preview-tests PROPERTIES
        ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME snow-shot-recording-capture-exclusion-tests
        COMMAND snow-shot-screen-recording-controller-tests --capture-exclusion-only)
    set_tests_properties(snow-shot-recording-capture-exclusion-tests PROPERTIES
        LABELS "unit" RUN_SERIAL TRUE TIMEOUT 60 ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}")
    if(APPLE)
        add_test(NAME snow-shot-macos-recording-capture-exclusion-tests
            COMMAND snow-shot-screen-recording-controller-tests --capture-exclusion-only -platform cocoa)
        set_tests_properties(snow-shot-macos-recording-capture-exclusion-tests PROPERTIES
            LABELS "interactive;macos" RUN_SERIAL TRUE TIMEOUT 60)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-screen-recording-controller-tests)
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
    target_include_directories(snow-shot-screen-recording-area-window-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(snow-shot-screen-recording-area-window-tests PRIVATE
        ant_design_qt adqt_icon_core
        Qt6::Widgets
        snow_draw_engine_qt_canvas
        snow_shot_storage
    )
    if(APPLE)
        target_sources(snow-shot-screen-recording-area-window-tests PRIVATE
            ../cmake/test-support/macos_native_input.mm)
        target_include_directories(snow-shot-screen-recording-area-window-tests PRIVATE
            ../cmake/test-support)
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
    endif()
    set_tests_properties(snow-shot-screen-recording-area-window-tests PROPERTIES
        ENVIRONMENT
            "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT};QT_SCALE_FACTOR=1.25"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

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
        src/presentation/services/screenshotsourceimagecomposer.cpp
    )
    target_include_directories(snow-shot-ocr-presentation-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-ocr-presentation-tests PRIVATE
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

    add_executable(snow-shot-recognition-window-tests
        tests/screenshot_recognition_window_tests.cpp
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
        src/presentation/ocr/screenshotocrvisuals.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/ocr/screenshottableeditor.cpp
        src/presentation/ocr/screenshotrecognitionwindow.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/styles/buttonborder.cpp
    )
    target_include_directories(snow-shot-recognition-window-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-recognition-window-tests PRIVATE
        ant_design_qt
        Qt6::Widgets
        snow_draw_engine_qt_canvas
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
    add_test(NAME snow-shot-recognition-selection-only-rendering-tests
        COMMAND snow-shot-recognition-window-tests --selection-only-rendering -platform offscreen)
    add_test(NAME snow-shot-recognition-ocr-hover-tests
        COMMAND snow-shot-recognition-window-tests --ocr-hover-only -platform offscreen)
    add_test(
        NAME snow-shot-recognition-window-tests
        COMMAND snow-shot-recognition-window-tests
    )
    if(WIN32)
        set_tests_properties(snow-shot-recognition-window-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-shot-recognition-window-tests PROPERTIES
            ENVIRONMENT
                "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-shot-ocr-recognition-service-tests
        tests/screenshot_ocr_recognition_service_tests.cpp
        tests/screenshot_ocr_process_lifecycle_tests.cpp
        include/snow_shot/presentation/screenshotocrpresentation.h
        include/snow_shot/presentation/screenshotocrrecognitionservice.h
        include/snow_shot/presentation/screenshotocrassets.h
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrrecognitionservice.cpp
        src/presentation/ocr/screenshotocrassets.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
    )
    target_include_directories(snow-shot-ocr-recognition-service-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_DRAW_ENGINE_SOURCE_DIR}/include"
    )
    target_compile_definitions(snow-shot-ocr-recognition-service-tests PRIVATE
        SNOW_TEST_OCR_EXECUTABLE="$<TARGET_FILE:snow_ocr_process>")
    if(SNOW_SHOT_BUILD_BENCHMARKS AND CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_definitions(snow-shot-ocr-recognition-service-tests PRIVATE SNOW_TEST_OCR_PERFORMANCE=1)
    endif()
    target_link_libraries(snow-shot-ocr-recognition-service-tests PRIVATE
        snow_shot_diagnostics
        snow_draw_engine_qt_canvas
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        MINIZIP::minizip-ng
    )
    if(WIN32)
        target_link_libraries(snow-shot-ocr-recognition-service-tests PRIVATE
            bcrypt
            ntdll
        )
        target_compile_definitions(snow-shot-ocr-recognition-service-tests PRIVATE
            SNOW_TEST_CRASHPAD_HANDLER="${SNOW_CRASHPAD_HANDLER}")
        add_test(NAME snow-shot-ocr-crash-tests COMMAND snow-shot-ocr-recognition-service-tests --native-crash)
        set_tests_properties(snow-shot-ocr-crash-tests PROPERTIES TIMEOUT 60 LABELS "unit;windows")
    endif()
    add_test(
        NAME snow-shot-ocr-cpu-recognition-service-tests
        COMMAND snow-shot-ocr-recognition-service-tests
    )
    add_test(
        NAME snow-shot-ocr-directml-recognition-service-tests
        COMMAND snow-shot-ocr-recognition-service-tests --directml
    )
    set_tests_properties(snow-shot-ocr-cpu-recognition-service-tests PROPERTIES
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-ocr-directml-recognition-service-tests PROPERTIES
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(NAME snow-shot-ocr-process-lifecycle-tests
        COMMAND snow-shot-ocr-recognition-service-tests --process-lifecycle)
    add_executable(snow-shot-ocr-assets-tests
        tests/screenshot_ocr_assets_tests.cpp
        include/snow_shot/presentation/screenshotocrassets.h
        src/presentation/ocr/screenshotocrassets.cpp)
    target_include_directories(snow-shot-ocr-assets-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-ocr-assets-tests PRIVATE Qt6::Core Qt6::Network MINIZIP::minizip-ng)
    add_test(NAME snow-shot-ocr-assets-tests COMMAND snow-shot-ocr-assets-tests)
    set_tests_properties(snow-shot-ocr-assets-tests PROPERTIES TIMEOUT 60 LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-ocr-managed-runtime-tests
        COMMAND snow-shot-ocr-recognition-service-tests --managed-runtime-only)
    add_test(NAME snow-shot-ocr-managed-directml-runtime-tests
        COMMAND snow-shot-ocr-recognition-service-tests --managed-runtime-only --directml)
    set_tests_properties(snow-shot-ocr-managed-runtime-tests
        snow-shot-ocr-managed-directml-runtime-tests PROPERTIES
        TIMEOUT 60 LABELS "unit;windows"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(APPLE)
        set_tests_properties(snow-shot-ocr-managed-runtime-tests PROPERTIES LABELS "unit")
        set_tests_properties(snow-shot-ocr-directml-recognition-service-tests PROPERTIES LABELS "unit;windows")
    endif()
    set_tests_properties(snow-shot-ocr-process-lifecycle-tests PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_dependencies(snow-shot-ocr-recognition-service-tests snow_ocr_process)

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
    target_include_directories(snow-shot-recognition-session-controller-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_DRAW_ENGINE_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-recognition-session-controller-tests PRIVATE
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
    add_test(NAME snow-shot-image-conversion-tests
        COMMAND snow-shot-recognition-session-controller-tests --image-conversion-only -platform offscreen)
    set_tests_properties(snow-shot-image-conversion-tests PROPERTIES TIMEOUT 60
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

    add_executable(snow-shot-qr-recognition-service-tests
        tests/screenshot_qr_recognition_service_tests.cpp
        include/snow_shot/presentation/screenshotqrrecognitionservice.h
        src/presentation/ocr/screenshotqrrecognitionservice.cpp
    )
    target_include_directories(snow-shot-qr-recognition-service-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-qr-recognition-service-tests PRIVATE
        opencv_wechat_qrcode
        opencv_objdetect
        Qt6::Core
        Qt6::Gui
    )
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
    )
    target_link_libraries(snow-shot-api-client-tests PRIVATE
        snow_shot_translation
        snow_shot_diagnostics
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        snow_shot_image_codec
    )
    add_test(
        NAME snow-shot-api-client-tests
        COMMAND snow-shot-api-client-tests
    )
    set_tests_properties(snow-shot-api-client-tests PROPERTIES
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
    target_link_libraries(snow-shot-recognition-image-performance-benchmark PRIVATE Qt6::Gui)
    snow_shot_import_offscreen_platform(snow-shot-recognition-image-performance-benchmark)
    qt_import_plugins(snow-shot-recognition-image-performance-benchmark
        NO_DEFAULT INCLUDE Qt6::QOffscreenIntegrationPlugin)
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
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-scrolling-image-replay-tests tests/scrolling_image_replay_tests.cpp)
    snow_shot_configure_scrolling_replay_target(snow-shot-scrolling-image-replay-tests)
    add_test(NAME snow-shot-scrolling-image-replay-tests COMMAND snow-shot-scrolling-image-replay-tests)
    add_test(NAME snow-shot-scrolling-thumbnail-drag-tests
        COMMAND snow-shot-scrolling-image-replay-tests --thumbnail-drag-only)
    set_tests_properties(snow-shot-scrolling-thumbnail-drag-tests PROPERTIES LABELS "unit")
    set_tests_properties(snow-shot-scrolling-image-replay-tests PROPERTIES
        LABELS "unit" ENVIRONMENT "${_SNOW_SHOT_OFFSCREEN_QPA_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

if(WIN32 AND SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-decoration-area-benchmark
        tests/screenshot_decoration_area_benchmark.cpp)
    target_link_libraries(snow-shot-decoration-area-benchmark PRIVATE
        Qt6::Widgets snow_draw_engine_qt_canvas)

    # This executable deliberately is not registered with CTest. It creates a
    # native 4K widget and reports hardware-sensitive timing for explicit
    # Windows performance sessions.
    add_executable(snow-shot-selection-render-benchmark
        tests/screenshot_selection_render_benchmark.cpp
        include/snow_shot/presentation/screenshotcanvasrenderer.h
        include/snow_shot/presentation/screenshotselectionshadowrenderer.h
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtextlayout.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
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
        dwmapi
        user32
    )
    snow_shot_import_offscreen_platform(snow-shot-selection-render-benchmark)

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
        snow_shot_diagnostics
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
        tests/screenshot_file_pin_batch_performance_benchmark.cpp
        ${SNOW_SELECTED_FILES_SOURCES}
        src/presentation/services/screenshotfilepinbatch.cpp
        include/snow_shot/presentation/components/pathinput.h
        src/presentation/components/pathinput.cpp
        include/snow_shot/presentation/screenshotsavepreviewcanvas.h
        src/presentation/services/screenshotsaveasfiledialog.cpp
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
        src/presentation/services/pinnedwindowgroupmanager.cpp
        src/presentation/pinned/screenshotpinnedinteraction.cpp
        ${SNOW_PINNED_PLATFORM_SOURCES}
        src/presentation/services/languagemanager.cpp
        src/presentation/styles/buttonborder.cpp
        src/presentation/styles/themecolorscheme.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalette.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylecomponents.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
        src/presentation/tools/screenshottoolpalettestylepresets.cpp
    )
    target_include_directories(snow-shot-file-pin-batch-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-file-pin-batch-performance-benchmark PRIVATE
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

if(WIN32 AND SNOW_SHOT_BUILD_BENCHMARKS)
    add_executable(snow-shot-uia-qt-performance-fixture
        tests/screenshot_uia_qt_performance_fixture.cpp
    )
    target_link_libraries(snow-shot-uia-qt-performance-fixture PRIVATE Qt6::Widgets user32)
    target_compile_definitions(snow-shot-uia-qt-performance-fixture PRIVATE NOMINMAX)

    add_custom_target(snow-shot-uia-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target x86_64-pc-windows-msvc
            --package snow-ui-selector --example hit_bench --features windows-examples
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    # A standalone selector archive avoids pulling the recording/FFmpeg umbrella
    # into this focused benchmark and matches the preserved baseline archive.
    set(_uia_phased_lib "${CMAKE_BINARY_DIR}/selector-cargo/x86_64-pc-windows-msvc/release/snow_ui_selector_c.lib")
    file(GLOB_RECURSE _uia_selector_sources CONFIGURE_DEPENDS
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector/src/*.rs"
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ui-selector-c/src/*.rs")
    add_custom_command(OUTPUT "${_uia_phased_lib}"
        COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/selector-cargo"
            "RUSTFLAGS=-C target-feature=+crt-static"
            "${CARGO_EXECUTABLE}" build --locked --release --target x86_64-pc-windows-msvc
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
        set(_uia_baseline_lib "${SNOW_SHOT_UIA_BASELINE_DIR}/target/x86_64-pc-windows-msvc/release/snow_ui_selector_c.lib")
        add_custom_command(OUTPUT "${_uia_baseline_lib}"
            COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${SNOW_SHOT_UIA_BASELINE_DIR}/target"
                "RUSTFLAGS=-C target-feature=+crt-static"
                "${CARGO_EXECUTABLE}" build --offline --release --target x86_64-pc-windows-msvc
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
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target x86_64-pc-windows-msvc
            --package snow-recording-export --example direct_recording_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    add_custom_target(snow-shot-recording-effects-performance-benchmark
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target x86_64-pc-windows-msvc
            --package snow-recording-effects --example effects_benchmark
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
            COMMAND "${SNOW_RUST_CARGO_TARGET_DIR}/x86_64-pc-windows-msvc/release/examples/effects_benchmark.exe")
        set_tests_properties(snow-shot-recording-effects-rasterization-benchmark PROPERTIES
            LABELS "benchmark;windows" RUN_SERIAL TRUE TIMEOUT 90)
    endif()

    # Measures the recording window startup phases (controller open ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ windows
    # shown ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ first paint ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ first motion-preview frame) through the
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
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target x86_64-pc-windows-msvc
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
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${SNOW_RUST_CARGO_TARGET_DIR}"
            "FFMPEG_DIR=${SNOW_FFMPEG_ROOT}"
            "${CARGO_EXECUTABLE}" build --locked --release
            --target x86_64-pc-windows-msvc
            --package snow-recording-runtime
            --features bench-stage-timing,bench-compositor-timing,bench-pipeline-timing
            --example realtime_recording_benchmark
        WORKING_DIRECTORY "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        USES_TERMINAL
        VERBATIM
    )
    # Seizes the primary monitor and injects input; interactive label keeps it
    # out of default test presets.
    add_test(NAME snow-shot-realtime-recording-performance-benchmark
        COMMAND "${SNOW_RUST_CARGO_TARGET_DIR}/x86_64-pc-windows-msvc/release/examples/realtime_recording_benchmark.exe"
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

target_include_directories(snow_shot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/tools")
if(TARGET snow-shot-pinned-window-tests)
    target_include_directories(snow-shot-pinned-window-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/tools")
    add_test(NAME snow-shot-fullscreen-canvas-settings-tests
        COMMAND snow-shot-application-storage-tests --fullscreen-canvas-only)
    set_tests_properties(snow-shot-fullscreen-canvas-settings-tests PROPERTIES LABELS "unit" TIMEOUT 60)
endif()
snow_shot_add_translations(snow_shot)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowShotUpdateUiTests.cmake")

if(WIN32)
    # These DLLs are consumed by the standalone OCR process that is staged
    # next to each host target, not by snow_shot itself. Static ONNX Runtime
    # delay-loads DirectML; dynamic builds derive the complete non-system DLL
    # closure from onnxruntime.dll instead of copying its entire bin directory.
    foreach(_ocr_runtime_target IN ITEMS snow_shot snow-shot-ocr-recognition-service-tests)
        if(TARGET ${_ocr_runtime_target})
            if(SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
                add_custom_command(TARGET ${_ocr_runtime_target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<IF:$<CONFIG:Debug>,${SNOW_SHOT_DIRECTML_RUNTIME_DLL_DEBUG},${SNOW_SHOT_DIRECTML_RUNTIME_DLL_RELEASE}>"
                        "$<TARGET_FILE_DIR:${_ocr_runtime_target}>/DirectML.dll"
                    COMMENT "Staging DirectML for ${_ocr_runtime_target}"
                    VERBATIM
                )
            else()
                # Resolve only the DLLs actually imported by ONNX Runtime and
                # its companions. This keeps unrelated vcpkg DLLs out of the
                # application directory and fails the build on a broken closure.
                _snow_shot_onnxruntime_link_items(_snow_shot_onnxruntime_dll_items)
                add_custom_command(TARGET ${_ocr_runtime_target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}"
                        "-DCOPY_FROM=${_snow_shot_onnxruntime_dll_items}"
                        "-DCOPY_DIR=$<TARGET_FILE_DIR:onnxruntime::onnxruntime>"
                        "-DCOPY_TO=$<TARGET_FILE_DIR:${_ocr_runtime_target}>"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/copy_runtime_dlls.cmake"
                    COMMENT "Staging dynamic ONNX Runtime closure for ${_ocr_runtime_target}"
                    VERBATIM
                )
                unset(_snow_shot_onnxruntime_dll_items)
            endif()
        endif()
    endforeach()
    unset(_ocr_runtime_target)
endif()

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
        if(WIN32)
            add_custom_command(TARGET ${_ocr_process_host_target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}"
                    -D "SNOW_OCR_ASSET_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/packaging/snow-shot-ocr-asset-manifest.json"
                    -D "SNOW_OCR_ARTIFACTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../artifacts"
                    -D "SNOW_OCR_DESTINATION=$<TARGET_FILE_DIR:${_ocr_process_host_target}>/assets/ocr"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/StageSnowShotOcrAssets.cmake"
                COMMENT "Staging verified OCR assets for ${_ocr_process_host_target}"
                VERBATIM
            )
        endif()
    endif()
endforeach()
unset(_ocr_process_host_target)

# The in-process barcode recognition service loads the pinned WeChat QR
# models at runtime; stage them next to every host that links the service.
foreach(_qr_host_target IN ITEMS snow_shot snow-shot-qr-recognition-service-tests)
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

if(SNOW_SHOT_ENABLE_AGGRESSIVE_RELEASE_OPTIMIZATION OR SNOW_SHOT_RELEASE_STATIC)
    snow_apply_release_options(snow_shot)
endif()
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
    find_program(PKG_CONFIG_EXECUTABLE
        NAMES pkgconf pkg-config
        HINTS "${VCPKG_INSTALLED_DIR}/x64-windows/tools/pkgconf"
        NO_DEFAULT_PATH
        REQUIRED
    )
    set(_SNOW_SHOT_SAVED_PKG_CONFIG_PATH "$ENV{PKG_CONFIG_PATH}")
    set(ENV{PKG_CONFIG_PATH} "${SNOW_FFMPEG_ROOT}/lib/pkgconfig")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SNOW_SHOT_FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavformat
        libavcodec
        libswresample
        libswscale
        libavutil
        # FFmpeg's generated static .pc files omit this private QSV dependency.
        # Keep it on the shared imported target for every Rust FFI consumer.
        vpl
    )
    set(ENV{PKG_CONFIG_PATH} "${_SNOW_SHOT_SAVED_PKG_CONFIG_PATH}")
    unset(_SNOW_SHOT_SAVED_PKG_CONFIG_PATH)
    # Canvas-only consumers also reach recording symbols in the bundled archive.
    if(TARGET snow_shot_rust_ffi_bundle)
        target_link_libraries(snow_shot_rust_ffi_bundle INTERFACE PkgConfig::SNOW_SHOT_FFMPEG)
    endif()
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
            "libx264-*.dll"
            "libx265*.dll"
            "libwebp*.dll"
            "libsharpyuv*.dll"
            "openh264-*.dll"
            "vpl*.dll"
        )
        set(SNOW_SHOT_FFMPEG_DLLS)
        foreach(_ffmpeg_dll_pattern IN LISTS SNOW_SHOT_FFMPEG_DLL_PATTERNS)
            file(GLOB _ffmpeg_matching_dlls
                "${SNOW_SHOT_FFMPEG_BIN_DIR}/${_ffmpeg_dll_pattern}"
            )
            list(APPEND SNOW_SHOT_FFMPEG_DLLS ${_ffmpeg_matching_dlls})
        endforeach()
        set(SNOW_SHOT_FFMPEG_RUNTIME_FILES ${SNOW_SHOT_FFMPEG_DLLS}
            CACHE INTERNAL "FFmpeg runtime files installed with Snow Shot.")
    endif()
endif()

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
if(EXISTS "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}")
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
if(WIN32 AND NOT SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
    # Dynamic ONNX Runtime for the standalone OCR process; the static release
    # links the runtime into snow-ocr-process.exe instead.
    install(FILES "$<TARGET_FILE:onnxruntime::onnxruntime>"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(WIN32)
    # DirectML remains a runtime dependency of the standalone OCR process;
    # even the statically linked ONNX Runtime delay-loads it for the
    # DirectML execution provider.
    install(FILES "${SNOW_SHOT_DIRECTML_RUNTIME_DLL_RELEASE}"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
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

foreach(_conversion_view_target IN ITEMS snow_shot snow-shot-recognition-window-tests
        snow-shot-recognition-session-controller-tests snow-shot-pinned-window-tests
        snow-shot-file-pin-batch-performance-benchmark)
    if(TARGET ${_conversion_view_target})
        target_sources(${_conversion_view_target} PRIVATE
            include/snow_shot/presentation/screenshotimageconversion.h
            include/snow_shot/presentation/screenshotimageconversionview.h
            src/presentation/ocr/screenshotimageconversionview.cpp)
    endif()
endforeach()
foreach(_conversion_controller_target IN ITEMS snow_shot
        snow-shot-recognition-session-controller-tests snow-shot-pinned-window-tests
        snow-shot-file-pin-batch-performance-benchmark)
    if(TARGET ${_conversion_controller_target})
        target_sources(${_conversion_controller_target} PRIVATE
            include/snow_shot/presentation/components/screenshottranslationsettingsdialog.h
            src/presentation/components/screenshottranslationsettingsdialog.cpp
            include/snow_shot/presentation/screenshotimageconversioncontroller.h
            src/presentation/ocr/screenshotimageconversioncontroller.cpp)
    endif()
endforeach()

qt_finalize_executable(snow_shot)

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-auto-filter-tests
        tests/screenshot_auto_filter_tests.cpp
        src/presentation/tools/screenshotautofiltercontroller.cpp
        include/snow_shot/presentation/screenshotautofiltercontroller.h)
    target_include_directories(snow-shot-auto-filter-tests PRIVATE include "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-visual-region-detector-c/include")
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
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-pinned-placement-tests tests/pinned_placement_tests.cpp
        src/presentation/pinned/screenshotpinnednativegeometrycontroller.cpp
        src/presentation/pinned/screenshotpinnedresizegeometry.cpp)
    target_include_directories(snow-shot-pinned-placement-tests PRIVATE include src)
    target_link_libraries(snow-shot-pinned-placement-tests PRIVATE Qt6::Gui)
    add_test(NAME snow-shot-pinned-placement-tests COMMAND snow-shot-pinned-placement-tests)
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
    target_link_libraries(snow-shot-macos-shell-policy-tests PRIVATE Qt6::Core)
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
    set_target_properties(snow-shot-macos-pinned-window-tests PROPERTIES
        MACOSX_BUNDLE TRUE MACOSX_BUNDLE_GUI_IDENTIFIER "top.snowshot.tests.pinned")
    target_include_directories(snow-shot-macos-pinned-window-tests PRIVATE include src)
    target_link_libraries(snow-shot-macos-pinned-window-tests PRIVATE Qt6::Widgets
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
    set_tests_properties(snow-shot-macos-pinned-window-tests snow-shot-macos-pinned-focus-tests snow-shot-macos-pinned-delivery-tests
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
    target_include_directories(snow-shot-selection-render-tests PRIVATE include
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
    target_link_libraries(snow-shot-selection-render-tests PRIVATE Qt6::Widgets)
    snow_shot_import_offscreen_platform(snow-shot-selection-render-tests)
    add_test(NAME snow-shot-selection-render-tests COMMAND snow-shot-selection-render-tests)
    set_tests_properties(snow-shot-selection-render-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit")
endif()

# Overlay hosts also own the shared region switcher and its custom icon pack.
get_property(_region_switcher_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _region_switcher_targets)
    get_target_property(_sources ${_target} SOURCES)
    if(NOT "${_target}" STREQUAL "snow_shot" AND
       _sources MATCHES "screenshotoverlaywindow.cpp" AND NOT _sources MATCHES "snowshoticons.cpp")
        target_sources(${_target} PRIVATE src/presentation/components/icons/snowshoticons.cpp)
    endif()
endforeach()

if(APPLE)
    get_property(_screenshot_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _screenshot_targets)
        get_target_property(_sources ${_target} SOURCES)
        if(_sources MATCHES "screenshotscrollingcapturecontroller.cpp|directcapturecontroller.cpp|screenshotoverlaywindow.cpp|screenshotrecognitionwindow.cpp|screenrecordingareawindow.cpp|screenrecordingtoolbarwindow.cpp|screenshotfloatingtoolpalettewindow.cpp")
            if(NOT _sources MATCHES "platform/macos/screenshotnative.mm")
                target_sources(${_target} PRIVATE src/platform/macos/screenshotnative.mm)
            endif()
            target_link_libraries(${_target} PRIVATE "-framework AppKit" "-framework ApplicationServices")
        endif()
    endforeach()
endif()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
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
        src/platform/macos/screenshotnative.mm
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/services/screenshotsourceimagecomposer.cpp)
    target_include_directories(snow-shot-macos-screenshot-native-smoke PRIVATE include
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
    target_link_libraries(snow-shot-macos-screenshot-native-smoke PRIVATE Qt6::Widgets snow_capture_c
        "-framework AppKit" "-framework ApplicationServices")
    add_test(NAME snow-shot-macos-screenshot-native-smoke COMMAND snow-shot-macos-screenshot-native-smoke)
    set_tests_properties(snow-shot-macos-screenshot-native-smoke PROPERTIES LABELS "interactive;macos" SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

if(APPLE AND SNOW_SHOT_BUILD_TESTS)
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
        src/platform/macos/screenshotnative.mm
        src/platform/macos/applicationactivation.mm)
    target_include_directories(snow-shot-macos-recapture-native-tests PRIVATE include
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include")
    target_link_libraries(snow-shot-macos-recapture-native-tests PRIVATE Qt6::Widgets snow_capture_c
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
        tests/macos_scrolling_input_tests.mm
        src/platform/macos/screenshotnative.mm)
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

    add_executable(snow-shot-macos-overlay-initialization-tests
        tests/macos_overlay_initialization_tests.mm
        ${SNOW_PINNED_PLATFORM_SOURCES}
        src/platform/macos/screenshotnative.mm
        src/presentation/core/screenshotgeometry.cpp)
    target_include_directories(snow-shot-macos-overlay-initialization-tests PRIVATE include src)
    target_link_libraries(snow-shot-macos-overlay-initialization-tests PRIVATE ant_design_qt Qt6::Widgets
        "-framework AppKit" "-framework ApplicationServices")
    add_test(NAME snow-shot-macos-overlay-initialization-tests
        COMMAND snow-shot-macos-overlay-initialization-tests)
    set_tests_properties(snow-shot-macos-overlay-initialization-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit;macos")
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
    target_link_libraries(snow-shot-login-item-native-tests PRIVATE snow_shot_login_item)
    add_test(NAME snow-shot-login-item-native-tests COMMAND snow-shot-login-item-native-tests)
    set_tests_properties(snow-shot-login-item-native-tests PROPERTIES LABELS "unit;macos")
endif()
