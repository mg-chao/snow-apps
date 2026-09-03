cmake_minimum_required(VERSION 3.30)

project(snow_shot VERSION ${SNOW_SHOT_VERSION} LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(RustStaticLibrary)

set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SNOW_SHOT_ENABLE_STRICT_COMPILE "Enable strict compiler warnings and treat warnings as errors." ON)
option(SNOW_SHOT_BUILD_TESTS "Build Snow Shot test executables." OFF)
option(SNOW_SHOT_BUILD_IMAGE_CODEC_SMOKE_TEST
    "Build the Snow Shot image-codec smoke test without the full QtTest suite." OFF)
option(SNOW_SHOT_BUILD_BENCHMARKS "Build opt-in native Snow Shot benchmarks." OFF)
option(SNOW_SHOT_RELEASE_STATIC "Prefer static Snow Shot release linkage." OFF)
option(SNOW_SHOT_QT_STATIC "Link Snow Shot against a static Qt build." OFF)
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

if(SNOW_SHOT_QT_STATIC)
    if(NOT QT_FEATURE_static)
        message(FATAL_ERROR
            "SNOW_SHOT_QT_STATIC requires a static Qt 6.11.1 kit. "
            "Set SNOW_QT_STATIC_DIR or Qt6_DIR to its lib/cmake/Qt6 directory.")
    endif()
    foreach(_snow_shot_static_qt_plugin IN ITEMS
        Qt6::QWindowsIntegrationPlugin
        Qt6::QSchannelBackendPlugin
    )
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
                "scripts/build-static-qt.ps1; bundled Qt codecs cannot be linked "
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
if(WIN32)
    find_package(Qt6GuiPrivate 6.11.1 EXACT REQUIRED CONFIG
        PATHS "${Qt6_DIR}/.."
        NO_DEFAULT_PATH
    )
endif()

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
    snow_add_rust_static_libraries(snow_shot_rust_ffi
        MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
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

# zxing-cpp backs the in-process barcode recognition service (QR, DataMatrix,
# Aztec, PDF417 and 1D symbologies); text recognition lives in the standalone
# OCR process.
find_package(ZXing CONFIG REQUIRED)
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
    set(SNOW_SHOT_OCR_PROCESS_FEATURES static-onnx-runtime directml-provider)
    set(SNOW_SHOT_OCR_PROCESS_NO_DEFAULT_FEATURES NO_DEFAULT_FEATURES)
    set(SNOW_SHOT_OCR_PROCESS_ENVIRONMENT
        "SNOW_ONNXRUNTIME_LINK_FILE=${CMAKE_CURRENT_BINARY_DIR}/generated/snow-ocr-process-onnxruntime-$<CONFIG>.rsp")
endif()

snow_add_rust_executable(snow_ocr_process
    PACKAGE snow-ocr-process
    MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
    OUTPUT_NAME snow-ocr-process
    PERFORMANCE_PROFILE
    ${SNOW_SHOT_OCR_PROCESS_NO_DEFAULT_FEATURES}
    FEATURES ${SNOW_SHOT_OCR_PROCESS_FEATURES}
    ENVIRONMENT ${SNOW_SHOT_OCR_PROCESS_ENVIRONMENT}
)

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
    if(NOT SNOW_IMAGE_LINK_JPEG_DEPENDENCIES)
        find_package(libjpeg-turbo CONFIG REQUIRED)
        target_link_libraries(snow_shot_image_codec_backend PRIVATE
            $<IF:$<TARGET_EXISTS:libjpeg-turbo::jpeg>,libjpeg-turbo::jpeg,libjpeg-turbo::jpeg-static>
            $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
        )
    endif()
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
    include/snow_shot/storage/applicationstorage.h
    include/snow_shot/storage/capturehistoryrepository.h
    include/snow_shot/storage/capturehistorytypes.h
    include/snow_shot/storage/configurationstore.h
    include/snow_shot/storage/configurationschema.h
    include/snow_shot/storage/persistedselectioncodec.h
    include/snow_shot/storage/pinnedwindowrepository.h
    include/snow_shot/storage/pinnedwindowtypes.h
    include/snow_shot/storage/settingsadapters.h
    include/snow_shot/storage/storagelogging.h
    include/snow_shot/storage/storageresult.h
    src/storage/applicationstorage.cpp
    src/storage/capturehistorypolicy_p.h
    src/storage/capturehistoryrepository.cpp
    src/storage/configurationstore.cpp
    src/storage/configurationschema.cpp
    src/storage/persistedselectioncodec.cpp
    src/storage/pinnedwindowrepository.cpp
    src/storage/settingsadapters.cpp
    src/storage/storagelogging.cpp
)
add_library(snow_shot_storage STATIC ${SNOW_SHOT_STORAGE_SOURCES})
snow_enable_unity_build(snow_shot_storage)
target_include_directories(snow_shot_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_storage PUBLIC
    snow_shot_image_codec
    Qt6::Core
    Qt6::Gui
)

add_library(snow_shot_window_shortcuts STATIC
    include/snow_shot/presentation/windowshortcutmanager.h
    src/presentation/services/windowshortcutmanager.cpp
)
target_include_directories(snow_shot_window_shortcuts PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(snow_shot_window_shortcuts PUBLIC Qt6::Widgets)

set(QT_I18N_SOURCE_LANGUAGE en_US)
file(GLOB TS_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/i18n/snow_shot_*.ts"
)
list(SORT TS_FILES)

function(snow_shot_add_translations target)
    if(target STREQUAL "snow_shot")
        set(_lupdate_target snow_shot_update_translations)
        set(_lrelease_target snow_shot_release_translations)
        set(_translation_source_targets
            ${target}
            snow_shot_settings_catalog
            snow_shot_settings_search
            snow_shot_settings
        )
    else()
        set(_lupdate_target "${target}_update_translations")
        set(_lrelease_target "${target}_release_translations")
        set(_translation_source_targets ${target})
    endif()

    qt_add_translations(
        ${target}
        TS_FILES ${TS_FILES}
        SOURCE_TARGETS ${_translation_source_targets}
        RESOURCE_PREFIX "/i18n"
        MERGE_QT_TRANSLATIONS
        LUPDATE_TARGET ${_lupdate_target}
        LUPDATE_OPTIONS -no-obsolete
        LRELEASE_TARGET ${_lrelease_target}
        LRELEASE_OPTIONS -fail-on-unfinished
    )
endfunction()

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
    include/snow_shot/platform/windows/autostartregistration.h
    include/snow_shot/platform/windows/focusedfullscreenwindow.h
    include/snow_shot/presentation/globalshortcutmanager.h
    include/snow_shot/presentation/globalshortcuttypes.h
    include/snow_shot/presentation/components/drawingtoolbareditorsettingswidget.h
    include/snow_shot/presentation/components/infotooltipicon.h
    include/snow_shot/presentation/components/pagecontainerwidget.h
    include/snow_shot/presentation/components/sectionheaderwidget.h
    include/snow_shot/presentation/components/screenshothistorypagewidget.h
    include/snow_shot/presentation/components/settingscustomwidget.h
    include/snow_shot/presentation/components/settingspagewidget.h
    include/snow_shot/presentation/components/shortcutkeyrow.h
    include/snow_shot/presentation/components/storagestatussettingswidget.h
    include/snow_shot/presentation/screenshottoolbarlayoutmodel.h
    include/snow_shot/presentation/settings/settingsbackend.h
    include/snow_shot/presentation/settings/settingsruntimesession.h
    include/snow_shot/presentation/settings/applicationpriority.h
    include/snow_shot/presentation/settings/textrecognitionacceleration.h
    include/snow_shot/presentation/styles/thememanager.h
    src/presentation/components/infotooltipicon.cpp
    src/presentation/components/pagecontainerwidget.cpp
    src/presentation/components/sectionheaderwidget.cpp
    src/presentation/components/screenshothistorypagewidget.cpp
    src/presentation/components/settingscustomwidget.cpp
    src/presentation/components/settingspagewidget.cpp
    src/presentation/components/shortcutkeyrow.cpp
    src/presentation/components/storagestatussettingswidget.cpp
    src/presentation/services/globalshortcutmanager.cpp
    src/presentation/settings/settingsbackend.cpp
    src/presentation/settings/settingsruntimesession.cpp
    src/presentation/settings/applicationpriority.cpp
    src/presentation/settings/textrecognitionacceleration.cpp
    src/presentation/styles/buttonborder.cpp
    src/presentation/styles/mainwindowcomponenttoken.cpp
    src/presentation/styles/themecolorscheme.cpp
    src/presentation/styles/thememanager.cpp
    src/platform/windows/autostartregistration.cpp
    src/platform/windows/focusedfullscreenwindow.cpp
)
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
add_library(snow_shot_settings STATIC ${SNOW_SHOT_SETTINGS_SOURCES})
snow_enable_unity_build(snow_shot_settings)
target_include_directories(snow_shot_settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_settings PUBLIC
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
    include/snow_shot/network/snowshotapiclient.h
    include/snow_shot/presentation/screenshottabledocument.h
    include/snow_shot/presentation/screenshottableeditor.h
    include/snow_shot/app/applicationcontroller.h
    include/snow_shot/app/singleinstancecoordinator.h
    include/snow_shot/platform/windows/autostartregistration.h
    include/snow_shot/platform/windows/focusedfullscreenwindow.h
    include/snow_shot/platform/windows/windowchrome.h
    include/snow_shot/platform/physicalcursor.h
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
    include/snow_shot/presentation/screenshotcanvascolorsampler.h
    include/snow_shot/presentation/screenshotcanvascolorsamplerwindow.h
    include/snow_shot/presentation/screenshotcaptureworkflowports.h
    include/snow_shot/presentation/screenshotcanvasrenderer.h
    include/snow_shot/presentation/screenshotclipboardcontent.h
    include/snow_shot/presentation/screenshotclipboardservice.h
    include/snow_shot/presentation/screenshotresultcompositor.h
    include/snow_shot/presentation/screenshotselectionshadowrenderer.h
    include/snow_shot/presentation/screenshotcolorpickercontroller.h
    include/snow_shot/presentation/screenshotcolorpickerwidget.h
    include/snow_shot/presentation/screenshotcontroller.h
    include/snow_shot/presentation/globalshortcuttypes.h
    include/snow_shot/presentation/screenshotdefaultstyles.h
    include/snow_shot/presentation/screenshotcanvastoolstyles.h
    include/snow_shot/presentation/screenshotdisplayconfigurationobserver.h
    include/snow_shot/presentation/screenshotdisplaysession.h
    include/snow_shot/presentation/screenshotexportservice.h
    include/snow_shot/presentation/screenshotexportcoordinator.h
    include/snow_shot/presentation/screenshotimagefileservice.h
    include/snow_shot/presentation/screenshotimagerowsource.h
    include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
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
    include/snow_shot/presentation/screenshotpinnedcopyservice.h
    include/snow_shot/presentation/screenshotpinnedwindow.h
    include/snow_shot/presentation/screenshotpresentationservices.h
    include/snow_shot/presentation/screenshotshortcuthints.h
    include/snow_shot/presentation/screenshotsmartselectiontransition.h
    include/snow_shot/presentation/screenshotselectioneditworkflow.h
    include/snow_shot/presentation/screenshotselectioneditworkflowports.h
    include/snow_shot/presentation/screenshotselectionexportworkflow.h
    include/snow_shot/presentation/screenshotselectionexportworkflowports.h
    include/snow_shot/presentation/screenshotselectionexportuiservices.h
    include/snow_shot/presentation/screenshotselectiongeometry.h
    include/snow_shot/presentation/screenshotselectionlimits.h
    include/snow_shot/presentation/screenshotselectionmodel.h
    include/snow_shot/presentation/screenshotselectionparams.h
    include/snow_shot/presentation/screenshotselectionresizemodalcontent.h
    include/snow_shot/presentation/screenshotselectionresizeworkflow.h
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
    include/snow_shot/presentation/screenrecordingareawindow.h
    include/snow_shot/presentation/screenrecordingcontroller.h
    include/snow_shot/presentation/screenrecordingtoolbarwindow.h
    include/snow_shot/presentation/styles/mainwindowcomponenttoken.h
    include/snow_shot/presentation/styles/buttonborder.h
    include/snow_shot/presentation/styles/themecolorscheme.h
    include/snow_shot/presentation/systemtraycontroller.h
)

set(PROJECT_CXX_SOURCES
    src/app/applicationcontroller.cpp
    src/app/main.cpp
    src/app/singleinstancecoordinator.cpp
    src/platform/physicalcursor.cpp
    src/platform/windows/windowchrome.cpp
    src/presentation/components/contentcardwidget.cpp
    src/presentation/components/maincontentheaderwidget.cpp
    src/presentation/components/sidebarwidget.cpp
    src/presentation/components/titlebarwidget.cpp
    src/presentation/mainwindow.cpp
    src/presentation/capture/screenshotcapturecoordinator.cpp
    src/presentation/capture/screenshotcapturedisplaymodelreconciler.cpp
    src/presentation/capture/screenshotcaptureruntimeadapter.cpp
    src/presentation/capture/screenshotcaptureworker.cpp
    src/presentation/capture/screenshotcaptureperfinstrumentation.cpp
    src/presentation/capture/screenshotcaptureworkflow.cpp
    src/presentation/capture/adaptivescrollingcapturecadence.h
    src/presentation/capture/latestbridgemailbox.h
    src/presentation/capture/screenshotscrollingcapturecontroller.cpp
    src/presentation/capture/screenshotscrollingthumbnailwidget.cpp
    src/presentation/services/screenshotcanvascolorsampler.cpp
    src/presentation/services/screenshotcanvascolorsamplerwindow.cpp
    src/presentation/services/screenshotcolorpickercontroller.cpp
    src/presentation/services/screenshotclipboardcontent.cpp
    src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
    src/presentation/services/screenshotclipboardservice.cpp
    src/network/snowshotapiclient.cpp
    src/presentation/services/screenshotcolorpickerwidget.cpp
    src/presentation/services/screenshotcontroller.cpp
    src/presentation/services/screenshotexportcoordinator.cpp
    src/presentation/services/screenshotmessageservice.cpp
    src/presentation/services/screenshothistoryservice.cpp
    src/presentation/services/screenshotdisplayconfigurationobserver.cpp
    src/presentation/services/screenshotexportservice.cpp
    src/presentation/services/screenshotimagefileservice.cpp
    src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
    src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
    src/presentation/core/screenshotgeometry.cpp
    src/presentation/core/screenshotcanvastoolstyles.cpp
    src/presentation/core/screenshotintelligentselectionmodel.cpp
    src/presentation/core/screenshotinteractionstate.cpp
    src/presentation/core/screenshotsmartselectiontransition.cpp
    src/presentation/ocr/screenshotocrcontroller.cpp
    src/presentation/ocr/screenshotrecognitionsessioncontroller.cpp
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
    src/presentation/pinned/screenshotpinnedcopyservice.cpp
    src/presentation/pinned/screenshotpinnedwindownative.cpp
    src/presentation/pinned/screenshotpinnededitcontroller.cpp
    src/presentation/pinned/screenshotpinnedresizegeometry.cpp
    src/presentation/pinned/screenshotpinnedrestoregeometry.cpp
    src/presentation/pinned/screenshotpinnedwindow.cpp
    src/presentation/pinned/screenshotpintoperfinstrumentation.cpp
    src/presentation/services/screenshotpresentationservices.cpp
    src/presentation/services/screenshotsourceimagecomposer.cpp
    src/presentation/services/systemtraycontroller.cpp
    src/presentation/selection/screenshotselectioneditworkflow.cpp
    src/presentation/selection/screenshotselectionexportworkflow.cpp
    src/presentation/selection/screenshotselectionexportuiservices.cpp
    src/presentation/selection/screenshotselectiongeometry.cpp
    src/presentation/selection/screenshotselectionmodel.cpp
    src/presentation/selection/screenshotselectionparams.cpp
    src/presentation/selection/screenshotselectionresizemodalcontent.cpp
    src/presentation/selection/screenshotselectionresizeworkflow.cpp
    src/presentation/selection/screenshotselectionsettingsstore.cpp
    src/presentation/selection/screenshotselectiontoolbarwidget.cpp
    src/presentation/selection/screenshotselectiontoolbarwidgets.cpp
    src/presentation/overlay/screenshotoverlaycanvaspresenter.cpp
    src/presentation/overlay/screenshotoverlaycanvaswidget.cpp
    src/presentation/overlay/screenshotoverlaycanvaswidget.h
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
    src/presentation/tools/screenshottoolcommandworkflow.cpp
    src/presentation/tools/screenshottoolbarmainpanel.cpp
    src/presentation/tools/screenshottoolpalettebuttons.cpp
    src/presentation/tools/screenshottoolpalettestylecontrols.cpp
    src/presentation/tools/screenshottoolpalettestylemodel.cpp
    src/presentation/tools/screenshottoolpalette.cpp
    src/presentation/tools/screenshottoolpalettehost.cpp
    src/presentation/toolbar/screenshottoolbarwindow.cpp
    src/presentation/recording/screenrecordingareawindow.cpp
    src/presentation/recording/screenrecordingcontroller.cpp
    src/presentation/recording/screenrecordinggeometry.cpp
    src/presentation/recording/screenrecordingtoolbarwindow.cpp
)

if(MSVC)
    set_source_files_properties(src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
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
if(WIN32)
    target_compile_definitions(snow_shot PRIVATE
        SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE=\"${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}\"
    )
endif()
target_link_libraries(snow_shot PRIVATE
    snow_shot_image_codec
    MINIZIP::minizip-ng
)

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

if(SNOW_SHOT_BUILD_TESTS)
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
    set_tests_properties(snow-shot-application-storage-tests PROPERTIES
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
        include/snow_shot/presentation/screenshotclipboardservice.h
        include/snow_shot/presentation/screenshotresultcompositor.h
        include/snow_shot/presentation/screenshotselectionshadowrenderer.h
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotclipboardservice.cpp
    )
    target_include_directories(snow-shot-screenshot-result-compositor-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-result-compositor-tests PRIVATE
        Qt6::Widgets
    )
    if(WIN32)
        target_link_libraries(snow-shot-screenshot-result-compositor-tests PRIVATE user32)
    endif()
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
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotexportservice.cpp
    )
    target_compile_definitions(snow-shot-screenshot-export-service-tests PRIVATE NOMINMAX)
    target_include_directories(snow-shot-screenshot-export-service-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-screenshot-export-service-tests PRIVATE
        Qt6::Widgets
        snow_draw_engine_qt_canvas
    )
    if(WIN32)
        target_link_libraries(snow-shot-screenshot-export-service-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-screenshot-export-service-tests
             COMMAND snow-shot-screenshot-export-service-tests)

    add_executable(snow-shot-screenshot-result-geometry-tests
        tests/screenshot_result_geometry_tests.cpp
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
    target_link_libraries(snow-shot-physical-cursor-tests PRIVATE Qt6::Core)
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

    add_executable(snow-shot-system-tray-controller-tests
        tests/system_tray_controller_tests.cpp
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/systemtraycontroller.h
        src/presentation/services/languagemanager.cpp
        src/presentation/services/systemtraycontroller.cpp
    )
    target_include_directories(snow-shot-system-tray-controller-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
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

    add_executable(snow-shot-context-menu-rendering-tests
        tests/context_menu_rendering_tests.cpp
    )
    target_link_libraries(snow-shot-context-menu-rendering-tests PRIVATE
        ant_design_qt
        Qt6::Widgets
    )
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
        include/snow_shot/presentation/components/infotooltipicon.h
        include/snow_shot/presentation/components/shortcutkeyrow.h
        include/snow_shot/presentation/styles/buttonborder.h
        include/snow_shot/presentation/styles/mainwindowcomponenttoken.h
        include/snow_shot/presentation/styles/thememanager.h
        include/snow_shot/presentation/styles/themecolorscheme.h
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/components/infotooltipicon.cpp
        src/presentation/components/shortcutkeyrow.cpp
        src/presentation/styles/buttonborder.cpp
        src/presentation/styles/mainwindowcomponenttoken.cpp
        src/presentation/styles/thememanager.cpp
        src/presentation/styles/themecolorscheme.cpp
    )
    target_include_directories(snow-shot-shortcut-key-row-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-shortcut-key-row-tests PRIVATE
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        Qt6::Widgets
    )
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

    add_executable(snow-shot-capture-workflow-tests
        tests/screenshot_capture_workflow_tests.cpp
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

    add_executable(snow-shot-selection-geometry-tests
        tests/screenshot_selection_geometry_tests.cpp
        include/snow_shot/presentation/screenshotgeometry.h
        include/snow_shot/presentation/screenshotselectiongeometry.h
        include/snow_shot/presentation/screenshotselectionmodel.h
        include/snow_shot/presentation/screenshotselectionparams.h
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
    )
    target_include_directories(snow-shot-selection-geometry-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-selection-geometry-tests PRIVATE
        Qt6::Gui
    )
    add_test(
        NAME snow-shot-selection-geometry-tests
        COMMAND snow-shot-selection-geometry-tests
    )
    set_tests_properties(snow-shot-selection-geometry-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-selection-export-workflow-tests
        tests/screenshot_selection_export_workflow_tests.cpp
        include/snow_shot/presentation/screenshotclipboardservice.h
        include/snow_shot/presentation/screenshotselectionexportworkflow.h
        include/snow_shot/presentation/screenshotselectionexportworkflowports.h
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
    target_link_libraries(snow-shot-selection-export-workflow-tests PRIVATE Qt6::Gui)
    if(WIN32)
        target_link_libraries(snow-shot-selection-export-workflow-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-selection-export-workflow-tests
             COMMAND snow-shot-selection-export-workflow-tests)
    set_tests_properties(snow-shot-selection-export-workflow-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-shot-screenshot-history-tests
        tests/screenshot_history_tests.cpp
        include/snow_shot/presentation/screenshothistoryservice.h
        include/snow_shot/presentation/screenshotdisplaysession.h
        include/snow_shot/presentation/screenshotgeometry.h
        include/snow_shot/presentation/screenshotintelligentselectionmodel.h
        include/snow_shot/presentation/screenshotinteractionstate.h
        include/snow_shot/presentation/screenshotoverlayinputhandler.h
        include/snow_shot/presentation/screenshotoverlayshortcutcontroller.h
        include/snow_shot/presentation/screenshotselectiongeometry.h
        include/snow_shot/presentation/screenshotselectionmodel.h
        include/snow_shot/presentation/screenshotselectionparams.h
        src/presentation/services/screenshothistoryservice.cpp
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
    )
    target_link_libraries(snow-shot-screenshot-history-tests PRIVATE
        snow_draw_engine_qt_canvas
        snow_shot_window_shortcuts
        Qt6::Widgets
    )
    add_test(
        NAME snow-shot-screenshot-history-tests
        COMMAND snow-shot-screenshot-history-tests
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
    target_include_directories(snow-shot-scrolling-capture-cadence-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME snow-shot-scrolling-capture-cadence-tests
        COMMAND snow-shot-scrolling-capture-cadence-tests
    )

    add_executable(snow-shot-screenshot-canvas-renderer-tests
        tests/screenshot_canvas_renderer_tests.cpp
        include/snow_shot/presentation/screenshotocrtextlayer.h
        include/snow_shot/presentation/screenshotmessageservice.h
        include/snow_shot/presentation/screenshotoverlaywindow.h
        include/snow_shot/presentation/screenshotscrollingtypes.h
        include/snow_shot/presentation/screenshotscrollingthumbnailwidget.h
        src/presentation/capture/screenshotscrollingthumbnailwidget.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/overlay/screenshotoverlaycanvaspresenter.cpp
        src/presentation/overlay/screenshotoverlayframepresenter.cpp
        src/presentation/overlay/screenshotoverlayframepresenter.h
        src/presentation/overlay/screenshotoverlaywindow.cpp
        src/presentation/services/screenshotmessageservice.cpp
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
        ant_design_qt
        Qt6::Widgets
        snow_shot_storage
        snow_draw_engine_qt_canvas
    )
    add_test(
        NAME snow-shot-screenshot-canvas-renderer-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests
    )
    add_test(
        NAME snow-shot-ocr-canvas-renderer-tests
        COMMAND snow-shot-screenshot-canvas-renderer-tests --ocr-presentation
    )
    set_tests_properties(snow-shot-screenshot-canvas-renderer-tests PROPERTIES
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
    add_test(
        NAME snow-shot-overlay-frame-presenter-tests
        COMMAND snow-shot-overlay-frame-presenter-tests
    )
    set_tests_properties(snow-shot-overlay-frame-presenter-tests PROPERTIES
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
        tests/screenshot_pinned_window_tests.cpp
        src/platform/physicalcursor.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshotcanvascolorsampler.h
        include/snow_shot/presentation/screenshotcanvascolorsamplerwindow.h
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/screenshotpinnededitcontroller.h
        include/snow_shot/presentation/screenshotpinnedcopyservice.h
        include/snow_shot/presentation/screenshotpinnedwindow.h
        include/snow_shot/presentation/screenshotrecognitionsessioncontroller.h
        include/snow_shot/presentation/screenshotrecognitionwindow.h
        include/snow_shot/presentation/screenshottableeditor.h
        include/snow_shot/presentation/screenshotmessageservice.h
        include/snow_shot/presentation/screenshotselectiongeometry.h
        include/snow_shot/presentation/screenshotselectionmodel.h
        include/snow_shot/presentation/screenshotselectionparams.h
        include/snow_shot/network/snowshotapiclient.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
        src/presentation/ocr/screenshotocrtexteditingsession.cpp
        src/presentation/ocr/screenshotocrtexttransform.cpp
        src/presentation/ocr/screenshotrecognitionsessioncontroller.cpp
        src/presentation/ocr/screenshotrecognitionwindow.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/ocr/screenshottableeditor.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotcanvascolorsampler.cpp
        src/presentation/services/screenshotcanvascolorsamplerwindow.cpp
        src/presentation/services/screenshotclipboardcontent.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotimagefileservice.cpp
        src/presentation/services/screenshotmessageservice.cpp
        src/presentation/selection/screenshotselectiongeometry.cpp
        src/presentation/selection/screenshotselectionmodel.cpp
        src/presentation/selection/screenshotselectionparams.cpp
        src/network/snowshotapiclient.cpp
        src/presentation/pinned/screenshotpinnededitcontroller.cpp
        src/presentation/pinned/screenshotpinnedcopyservice.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
        src/presentation/selection/screenshotselectionexportuiservices.cpp
        src/presentation/pinned/screenshotpinnednativegeometrycontroller.cpp
        src/presentation/pinned/screenshotpinnedresizegeometry.cpp
        src/presentation/pinned/screenshotpinnedrestoregeometry.cpp
        src/presentation/pinned/screenshotpinnedwindow.cpp
        src/presentation/pinned/screenshotpinnedwindownative.cpp
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
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
    )
    target_include_directories(snow-shot-pinned-window-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_compile_definitions(snow-shot-pinned-window-tests PRIVATE
        SNOW_SHOT_TEST_HOOKS=1
    )
    target_link_libraries(snow-shot-pinned-window-tests PRIVATE
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
    add_test(
        NAME snow-shot-pinned-toolbar-parity-tests
        COMMAND snow-shot-pinned-window-tests --toolbar-parity-only
    )
    if(WIN32)
        add_test(
            NAME snow-shot-pinned-native-drag-shortcut-tests
            COMMAND snow-shot-pinned-window-tests --native-drag-shortcut-only
        )
    endif()

    add_executable(snow-shot-screenshot-image-file-service-tests
        tests/screenshot_image_file_service_tests.cpp
        include/snow_shot/presentation/screenshotimagefileservice.h
        src/presentation/services/screenshotimagefileservice.cpp
    )
    target_include_directories(snow-shot-screenshot-image-file-service-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/image"
    )
    target_link_libraries(snow-shot-screenshot-image-file-service-tests PRIVATE
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
    set_tests_properties(snow-shot-clipboard-content-tests PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(
        NAME snow-shot-pinned-async-presentation-tests
        COMMAND snow-shot-pinned-window-tests --async-presentation-only
    )
    add_test(
        NAME snow-shot-pinned-restore-wiring-tests
        COMMAND snow-shot-pinned-window-tests --restore-wiring-only
    )
    set(_snow_shot_pinned_window_test_names
        snow-shot-pinned-window-tests
        snow-shot-pinned-async-presentation-tests
        snow-shot-pinned-restore-wiring-tests
        snow-shot-pinned-toolbar-parity-tests
    )
    if(WIN32)
        list(APPEND _snow_shot_pinned_window_test_names
            snow-shot-pinned-native-drag-shortcut-tests
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

    add_executable(snow-shot-pinned-copyservice-tests
        tests/screenshot_pinned_copyservice_tests.cpp
        include/snow_shot/presentation/screenshotclipboardservice.h
        include/snow_shot/presentation/screenshotpinnedcopyservice.h
        include/snow_shot/presentation/screenshotresultcompositor.h
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/pinned/screenshotpinnedcopyservice.cpp
        src/presentation/services/screenshotexportcoordinator.cpp
    )
    target_include_directories(snow-shot-pinned-copyservice-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-pinned-copyservice-tests PRIVATE
        Qt6::Widgets
        snow_draw_engine_qt_canvas
    )
    if(WIN32)
        target_link_libraries(snow-shot-pinned-copyservice-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-pinned-copyservice-tests
             COMMAND snow-shot-pinned-copyservice-tests)
    set_tests_properties(snow-shot-pinned-copyservice-tests PROPERTIES
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
    target_link_libraries(snow-shot-export-coordinator-tests PRIVATE Qt6::Widgets)
    if(WIN32)
        target_link_libraries(snow-shot-export-coordinator-tests PRIVATE user32)
    endif()
    add_test(NAME snow-shot-export-coordinator-tests
             COMMAND snow-shot-export-coordinator-tests)
    set_tests_properties(snow-shot-export-coordinator-tests PROPERTIES
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
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
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
    add_test(
        NAME snow-shot-screenshot-tool-palette-tests
        COMMAND snow-shot-screenshot-tool-palette-tests
    )
    set_tests_properties(snow-shot-screenshot-tool-palette-tests PROPERTIES
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

    if(WIN32)
        add_executable(snow-shot-screenshot-toolbar-fill-color-hover-tests
            tests/screenshot_toolbar_fill_color_hover_tests.cpp
            include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
            include/snow_shot/presentation/screenshottoolbarwindow.h
            include/snow_shot/presentation/screenshottoolpalette.h
            include/snow_shot/presentation/screenshottoolpalettehost.h
            include/snow_shot/presentation/styles/thememanager.h
            src/presentation/components/icons/snowshoticons.cpp
            src/presentation/core/screenshotgeometry.cpp
            src/presentation/core/screenshotcanvastoolstyles.cpp
            src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
            src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
            src/presentation/tools/screenshottoolbarmainpanel.cpp
            src/presentation/tools/screenshottoolpalettebuttons.cpp
            src/presentation/tools/screenshottoolpalettehost.cpp
            src/presentation/tools/screenshottoolpalettestylecontrols.cpp
            src/presentation/tools/screenshottoolpalettestylemodel.cpp
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
            src/presentation/core/screenshotgeometry.cpp
            src/presentation/core/screenshotcanvastoolstyles.cpp
            src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
            src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
            src/presentation/tools/screenshottoolbarmainpanel.cpp
            src/presentation/tools/screenshottoolpalettebuttons.cpp
            src/presentation/tools/screenshottoolpalettehost.cpp
            src/presentation/tools/screenshottoolpalettestylecontrols.cpp
            src/presentation/tools/screenshottoolpalettestylemodel.cpp
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

    add_executable(snow-shot-floating-toolbar-drag-tests
        tests/screenshot_floating_toolbar_drag_tests.cpp
        include/snow_shot/presentation/screenshotfloatingtoolpalettewindow.h
        include/snow_shot/presentation/screenshottoolbarwindow.h
        include/snow_shot/presentation/screenshottoolpalette.h
        include/snow_shot/presentation/screenshottoolpalettehost.h
        include/snow_shot/presentation/styles/thememanager.h
        src/presentation/components/icons/snowshoticons.cpp
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
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
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
    add_test(
        NAME snow-shot-adqt-dpi-stable-tests
        COMMAND snow-shot-adqt-dpi-stable-tests
    )
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
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
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

    add_executable(snow-shot-screen-recording-geometry-tests
        tests/screen_recording_geometry_tests.cpp
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

    add_executable(snow-shot-ocr-presentation-tests
        tests/screenshot_ocr_presentation_tests.cpp
        include/snow_shot/presentation/screenshotocrpresentation.h
        include/snow_shot/presentation/screenshotocrtexteditingsession.h
        include/snow_shot/presentation/screenshotocrtexttransform.h
        include/snow_shot/presentation/screenshottabledocument.h
        include/snow_shot/presentation/screenshotsourceimagecomposer.h
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
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/ocr/screenshottableeditor.cpp
        src/presentation/ocr/screenshotrecognitionwindow.cpp
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
    target_link_libraries(snow-shot-ocr-recognition-service-tests PRIVATE
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
    add_dependencies(snow-shot-ocr-recognition-service-tests snow_ocr_process)

    add_executable(snow-shot-recognition-session-controller-tests
        tests/screenshot_recognition_session_controller_tests.cpp
        include/snow_shot/network/snowshotapiclient.h
        include/snow_shot/presentation/languagemanager.h
        include/snow_shot/presentation/screenshotocrpresentation.h
        include/snow_shot/presentation/screenshotrecognitionsessioncontroller.h
        include/snow_shot/presentation/screenshotrecognitionwindow.h
        include/snow_shot/presentation/screenshottabledocument.h
        include/snow_shot/presentation/screenshottableeditor.h
        src/network/snowshotapiclient.cpp
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrtexteditingsession.cpp
        src/presentation/ocr/screenshotocrtexttransform.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
        src/presentation/ocr/screenshotrecognitionsessioncontroller.cpp
        src/presentation/ocr/screenshotrecognitionwindow.cpp
        src/presentation/ocr/screenshottabledocument.cpp
        src/presentation/ocr/screenshottableeditor.cpp
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

    add_executable(snow-shot-qr-recognition-service-tests
        tests/screenshot_qr_recognition_service_tests.cpp
        include/snow_shot/presentation/screenshotqrrecognitionservice.h
        src/presentation/ocr/screenshotqrrecognitionservice.cpp
    )
    target_include_directories(snow-shot-qr-recognition-service-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-qr-recognition-service-tests PRIVATE
        ZXing::ZXing
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

    add_executable(snow-shot-api-client-tests
        tests/snowshot_api_client_tests.cpp
        include/snow_shot/network/snowshotapiclient.h
        src/network/snowshotapiclient.cpp
    )
    target_include_directories(snow-shot-api-client-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-api-client-tests PRIVATE
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
        snow-shot-language-manager-tests
        snow-shot-shortcut-key-row-tests
        snow-shot-form-layout-tests
        snow-shot-selection-resize-workflow-tests
        snow-shot-screenshot-canvas-renderer-tests
        snow-shot-screenshot-tool-palette-tests
        snow-shot-application-icon-tests
        snow-shot-toolbar-main-panel-tests
        snow-shot-screen-recording-geometry-tests
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

if(WIN32 AND SNOW_SHOT_BUILD_BENCHMARKS)
    # This executable deliberately is not registered with CTest. It creates a
    # native 4K widget and reports hardware-sensitive timing for explicit
    # Windows performance sessions.
    add_executable(snow-shot-selection-render-benchmark
        tests/screenshot_selection_render_benchmark.cpp
        include/snow_shot/presentation/screenshotcanvasrenderer.h
        include/snow_shot/presentation/screenshotselectionshadowrenderer.h
        src/presentation/ocr/screenshotocrpresentation.cpp
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
        snow_draw_engine_qt_canvas
        dwmapi
        user32
    )

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
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/core/screenshotcanvastoolstyles.cpp
        src/presentation/tools/screenshotfloatingtoolpalettenative.cpp
        src/presentation/tools/screenshotfloatingtoolpalettewindow.cpp
        src/presentation/tools/screenshottoolbarmainpanel.cpp
        src/presentation/tools/screenshottoolpalettebuttons.cpp
        src/presentation/tools/screenshottoolpalettehost.cpp
        src/presentation/tools/screenshottoolpalettestylecontrols.cpp
        src/presentation/tools/screenshottoolpalettestylemodel.cpp
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
        src/presentation/core/screenshotgeometry.cpp
        src/presentation/overlay/screenshotselectionshadowrenderer.cpp
        src/presentation/services/screenshotclipboardperfinstrumentation.h
        src/presentation/services/screenshotclipboardservice.cpp
        src/presentation/services/screenshotexportservice.cpp
    )
    target_compile_definitions(snow-shot-clipboard-performance-benchmark PRIVATE
        NOMINMAX
        SNOW_SHOT_CLIPBOARD_PERF_INSTRUMENTATION=1
    )
    target_include_directories(snow-shot-clipboard-performance-benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(snow-shot-clipboard-performance-benchmark PRIVATE
        Qt6::Widgets
        snow_draw_engine_qt_canvas
        Psapi
        user32
    )
endif()

target_include_directories(snow_shot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-capture-c/include"
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
        adqt_icon_core
        ant_design_icons_qt
        ant_design_qt
        snow_shot_settings
        snow_shot_storage
        snow_shot_window_shortcuts
        snow_draw_engine_qt_canvas
        snow_capture_c
        snow_ui_selector_c
        snow_stitch_images_c
        ZXing::ZXing
        Qt6::Widgets
        Qt6::Svg
        Qt6::Network
)

target_compile_definitions(snow_shot PRIVATE
    $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
)
add_dependencies(snow_shot snow_ocr_process)

if(WIN32 AND SNOW_SHOT_BUILD_BENCHMARKS)
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

if(WIN32)
    # These DLLs are consumed by the standalone OCR process that is staged
    # next to each host target, not by snow_shot itself. DirectML is
    # delay-loaded by the statically linked ONNX Runtime; onnxruntime.dll is
    # only needed when the process links it dynamically.
    foreach(_ocr_runtime_target IN ITEMS snow_shot snow-shot-ocr-recognition-service-tests)
        if(TARGET ${_ocr_runtime_target})
            add_custom_command(TARGET ${_ocr_runtime_target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<IF:$<CONFIG:Debug>,${SNOW_SHOT_DIRECTML_RUNTIME_DLL_DEBUG},${SNOW_SHOT_DIRECTML_RUNTIME_DLL_RELEASE}>"
                    "$<TARGET_FILE_DIR:${_ocr_runtime_target}>/DirectML.dll"
                COMMENT "Staging DirectML for ${_ocr_runtime_target}"
                VERBATIM
            )
            if(NOT SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
                add_custom_command(TARGET ${_ocr_runtime_target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:onnxruntime::onnxruntime>"
                        "$<TARGET_FILE_DIR:${_ocr_runtime_target}>/onnxruntime.dll"
                    COMMENT "Staging dynamic ONNX Runtime for ${_ocr_runtime_target}"
                    VERBATIM
                )
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
    )
    set(ENV{PKG_CONFIG_PATH} "${_SNOW_SHOT_SAVED_PKG_CONFIG_PATH}")
    unset(_SNOW_SHOT_SAVED_PKG_CONFIG_PATH)
    target_link_libraries(snow_shot PRIVATE PkgConfig::SNOW_SHOT_FFMPEG)
    if(TARGET snow-shot-screenshot-canvas-renderer-tests)
        target_link_libraries(snow-shot-screenshot-canvas-renderer-tests PRIVATE
            PkgConfig::SNOW_SHOT_FFMPEG
        )
    endif()
    # The toolbar benchmarks link the canvas engine, which pulls the Rust FFI
    # bundle and with it the FFmpeg import libraries.
    foreach(_snow_shot_ffmpeg_toolbar_benchmark IN ITEMS
            snow-shot-screenshot-toolbar-render-benchmark
            snow-shot-screenshot-toolbar-display-benchmark)
        if(TARGET ${_snow_shot_ffmpeg_toolbar_benchmark})
            target_link_libraries(${_snow_shot_ffmpeg_toolbar_benchmark} PRIVATE
                PkgConfig::SNOW_SHOT_FFMPEG
            )
        endif()
    endforeach()
    unset(_snow_shot_ffmpeg_toolbar_benchmark)
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
    if(MSVC)
        target_compile_options(snow_shot PRIVATE
            /WX
            /W4
            /sdl
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /w14242
            /w14254
            /w14263
            /w14265
            /w14287
            /we4289
            /w14296
            /w14311
            /w14545
            /w14546
            /w14547
            /w14549
            /w14555
            /w14619
            /w14640
            /w14826
            /w14905
            /w14906
            /w14928
        )
    else()
        target_compile_options(snow_shot PRIVATE
            -Werror
            -Wall
            -Wextra
            -Wpedantic
            -Wcast-qual
            -Wcast-align
            -Wconversion
            -Wdouble-promotion
            -Wextra-semi
            -Wfloat-equal
            -Wformat=2
            -Wimplicit-fallthrough
            -Wmissing-declarations
            -Wnon-virtual-dtor
            -Wnull-dereference
            -Wold-style-cast
            -Woverloaded-virtual
            -Wredundant-decls
            -Wshadow
            -Wsign-conversion
            -Wswitch-default
            -Wswitch-enum
            -Wundef
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
install(TARGETS snow_shot
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES "$<TARGET_FILE:snow_ocr_process>"
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
        INCLUDE
            Qt6::QWindowsIntegrationPlugin
            Qt6::QSchannelBackendPlugin
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

qt_finalize_executable(snow_shot)
