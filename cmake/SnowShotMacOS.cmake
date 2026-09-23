# The Rust static archive does not propagate Cargo's native link metadata to CMake.
find_package(PkgConfig REQUIRED)
set(_snow_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "${SNOW_FFMPEG_ROOT}/lib/pkgconfig")
set(_snow_saved_pkg_config_argn "${PKG_CONFIG_ARGN}")
if(SNOW_SHOT_RELEASE_STATIC)
    set(PKG_CONFIG_ARGN --static)
endif()
pkg_check_modules(SNOW_SHOT_FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
    libavformat libavcodec libswresample libswscale libavutil)
set(PKG_CONFIG_ARGN "${_snow_saved_pkg_config_argn}")
set(ENV{PKG_CONFIG_PATH} "${_snow_saved_pkg_config_path}")

# FindPkgConfig places Apple's two-token `-framework Name` pairs in
# INTERFACE_LINK_OPTIONS. CMake de-duplicates repeated options, which can leave
# later framework names as bare linker inputs. The complete framework closure
# is resolved with find_library below, so remove the unsafe pkg-config pairs
# while preserving unrelated options such as -pthread.
include("${CMAKE_CURRENT_LIST_DIR}/SnowPkgConfigAppleFrameworks.cmake")
get_target_property(_snow_ffmpeg_link_options
    PkgConfig::SNOW_SHOT_FFMPEG INTERFACE_LINK_OPTIONS)
if(_snow_ffmpeg_link_options)
    snow_strip_pkg_config_apple_framework_options(
        _snow_ffmpeg_link_options ${_snow_ffmpeg_link_options})
    set_property(TARGET PkgConfig::SNOW_SHOT_FFMPEG PROPERTY
        INTERFACE_LINK_OPTIONS "${_snow_ffmpeg_link_options}")
endif()
set(_snow_native_libraries PkgConfig::SNOW_SHOT_FFMPEG objc)
foreach(_framework IN ITEMS AppKit ApplicationServices AVFoundation AudioToolbox
        Carbon CoreAudio CoreFoundation CoreGraphics CoreImage CoreMedia CoreVideo
        Foundation IOSurface Metal ScreenCaptureKit Security VideoToolbox)
    find_library(SNOW_MACOS_${_framework} NAMES ${_framework} REQUIRED)
    list(APPEND _snow_native_libraries "${SNOW_MACOS_${_framework}}")
endforeach()
if(TARGET snow_shot_rust_ffi_bundle)
    target_link_libraries(snow_shot_rust_ffi_bundle INTERFACE ${_snow_native_libraries})
else()
    target_link_libraries(snow_recording_c INTERFACE ${_snow_native_libraries})
    target_link_libraries(snow_capture_c INTERFACE ${_snow_native_libraries})
endif()

set_target_properties(snow_shot PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/../snow_shot/packaging/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "Snow Shot"
    MACOSX_BUNDLE_BUNDLE_VERSION "${SNOW_SHOT_VERSION_NUMERIC}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${SNOW_SHOT_VERSION_NUMERIC}"
    MACOSX_BUNDLE_ICON_FILE "snow-shot.icns"
    INSTALL_RPATH "@executable_path/../Frameworks"
    INSTALL_RPATH_USE_LINK_PATH TRUE)

foreach(_snow_bundle_language IN ITEMS en zh-Hans zh-Hant)
    set(_snow_bundle_strings
        "${CMAKE_CURRENT_LIST_DIR}/../snow_shot/packaging/macos/${_snow_bundle_language}.lproj/InfoPlist.strings")
    set_source_files_properties("${_snow_bundle_strings}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/${_snow_bundle_language}.lproj")
    target_sources(snow_shot PRIVATE "${_snow_bundle_strings}")
endforeach()

set(_snow_macos_icon_artwork
    "${CMAKE_CURRENT_LIST_DIR}/../snow_shot/resources/app-icon.svg")
set(_snow_macos_icon "${CMAKE_CURRENT_BINARY_DIR}/macos/snow-shot.icns")
add_custom_command(
    OUTPUT "${_snow_macos_icon}"
    COMMAND "${CMAKE_COMMAND}"
        -DSNOW_MACOS_ICON_ARTWORK=${_snow_macos_icon_artwork}
        -DSNOW_MACOS_ICON_OUTPUT=${_snow_macos_icon}
        -P "${CMAKE_CURRENT_LIST_DIR}/GenerateMacOSIcon.cmake"
    DEPENDS
        "${_snow_macos_icon_artwork}"
        "${CMAKE_CURRENT_LIST_DIR}/GenerateMacOSIcon.cmake"
    COMMENT "Generating the Snow Shot macOS application icon"
    VERBATIM)
set_source_files_properties("${_snow_macos_icon}" PROPERTIES
    GENERATED TRUE
    MACOSX_PACKAGE_LOCATION Resources)
target_sources(snow_shot PRIVATE "${_snow_macos_icon}")
if(TARGET snow_shot_image_codec_backend)
    set_target_properties(snow_shot_image_codec_backend PROPERTIES
        INSTALL_RPATH "@loader_path" INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()
target_link_options(snow_shot PRIVATE -Wl,-headerpad_max_install_names)

if(SNOW_SHOT_RELEASE_STATIC)
    get_filename_component(_snow_static_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    set(_snow_static_qt_licenses "${_snow_static_qt_prefix}/share/snow-apps/qt-licenses")
    if(NOT IS_DIRECTORY "${_snow_static_qt_licenses}")
        message(FATAL_ERROR
            "The audited static Qt license bundle is missing: ${_snow_static_qt_licenses}")
    endif()
    install(DIRECTORY "${_snow_static_qt_licenses}/"
        DESTINATION "snow_shot.app/Contents/Resources/snow-shot/licenses/third-party/qt"
        COMPONENT SnowShot)
    install(DIRECTORY "${SNOW_FFMPEG_ROOT}/share/"
        DESTINATION "snow_shot.app/Contents/Resources/snow-shot/licenses/third-party/vcpkg"
        COMPONENT SnowShot FILES_MATCHING PATTERN "copyright")
endif()

# Dynamic development builds load ONNX Runtime with dlopen. Production builds
# link it into the helper and therefore stage no runtime dylib.
if(NOT SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
    add_custom_command(TARGET snow_shot POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:onnxruntime::onnxruntime>"
            "$<TARGET_FILE_DIR:snow_shot>/libonnxruntime.dylib"
        VERBATIM)
    install(FILES "$<TARGET_FILE:onnxruntime::onnxruntime>"
        DESTINATION "snow_shot.app/Contents/MacOS" RENAME libonnxruntime.dylib
        COMPONENT SnowShot)
endif()

# Keep the crash collector inside the app, beside the main executable and worker.
set(_snow_handler_zlib "")
if(NOT SNOW_SHOT_RELEASE_STATIC)
    set(_snow_handler_zlib "$<TARGET_FILE:ZLIB::ZLIB>")
endif()
add_custom_command(TARGET snow_shot POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DSNOW_HANDLER_SOURCE=${SNOW_CRASHPAD_HANDLER}"
        "-DSNOW_HANDLER_DIRECTORY=$<TARGET_FILE_DIR:snow_shot>"
        "-DSNOW_HANDLER_ZLIB=${_snow_handler_zlib}"
        -P "${CMAKE_CURRENT_LIST_DIR}/StageSnowShotCrashHandler.cmake"
    VERBATIM)
install(PROGRAMS "${SNOW_CRASHPAD_HANDLER}"
    DESTINATION "snow_shot.app/Contents/MacOS" COMPONENT SnowShot)

# Archive UUID-matched symbols outside the app bundle for offline symbolication.
find_program(SNOW_DSYMUTIL dsymutil REQUIRED)
if(TARGET snow_ocr_process)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/GenerateSnowShotDiagnosticsSymbols.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/GenerateSnowShotDiagnosticsSymbols.cmake.in" @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GenerateSnowShotDiagnosticsSymbols-$<CONFIG>.cmake"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/GenerateSnowShotDiagnosticsSymbols.cmake.in")
    add_custom_target(snow-shot-diagnostics-symbols
        COMMAND "${CMAKE_COMMAND}" -P
            "${CMAKE_CURRENT_BINARY_DIR}/GenerateSnowShotDiagnosticsSymbols-$<CONFIG>.cmake"
        DEPENDS snow_shot snow_ocr_process
        VERBATIM)
endif()
target_compile_options(snow_shot PRIVATE -g)
foreach(_snow_diagnostics_target snow_shot_diagnostics snow_shot_crash_bridge snow_ocr_diagnostics_bridge)
    if(TARGET ${_snow_diagnostics_target})
        target_compile_options(${_snow_diagnostics_target} PRIVATE -g)
    endif()
endforeach()

set(SNOW_MACOS_OCR_ASSETS_ENABLED OFF)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SNOW_MACOS_OCR_TOOL "${CMAKE_CURRENT_LIST_DIR}/../scripts/snow-shot-macos-ocr.py")
set(SNOW_MACOS_OCR_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../snow_shot/packaging/snow-shot-ocr-asset-manifest.json")
if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" AND TARGET snow_ocr_process)
    set(SNOW_MACOS_OCR_ASSETS_ENABLED ON)
    set(_snow_macos_ocr_runtime_arguments)
    if(SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
        list(APPEND _snow_macos_ocr_runtime_arguments --static-runtime)
    else()
        list(APPEND _snow_macos_ocr_runtime_arguments
            --library "$<TARGET_FILE:onnxruntime::onnxruntime>")
    endif()
    foreach(_host IN ITEMS snow_shot snow-shot-ocr-recognition-service-tests)
        if(TARGET ${_host})
            # Always check the content hashes: Cargo can rebuild the worker without
            # relinking its Qt host. A host POST_BUILD hook would miss that update.
            add_custom_target(${_host}-ocr-assets
                COMMAND "${Python3_EXECUTABLE}" "${SNOW_MACOS_OCR_TOOL}" stage
                    --manifest "${SNOW_MACOS_OCR_MANIFEST}"
                    --runtime-dir "$<TARGET_FILE_DIR:${_host}>"
                    --worker "$<TARGET_FILE:snow_ocr_process>"
                    ${_snow_macos_ocr_runtime_arguments}
                DEPENDS snow_ocr_process onnxruntime::onnxruntime
                    "${SNOW_MACOS_OCR_TOOL}" "${SNOW_MACOS_OCR_MANIFEST}"
                VERBATIM)
            add_dependencies(${_host} ${_host}-ocr-assets)
        endif()
    endforeach()
    install(DIRECTORY "$<TARGET_FILE_DIR:snow_shot>/assets/ocr"
        DESTINATION "snow_shot.app/Contents/MacOS/assets" COMPONENT SnowShot)
endif()

if(NOT SNOW_SHOT_QT_STATIC)
    get_target_property(_snow_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_snow_qt_bin "${_snow_qmake}" DIRECTORY)
    find_file(SNOW_QT_OFFSCREEN_PLUGIN NAMES libqoffscreen.dylib
        HINTS "${_snow_qt_bin}/../plugins/platforms" NO_DEFAULT_PATH REQUIRED)
    install(FILES "${SNOW_QT_OFFSCREEN_PLUGIN}"
        DESTINATION "snow_shot.app/Contents/PlugIns/platforms" COMPONENT SnowShot)
    find_program(SNOW_MACDEPLOYQT NAMES macdeployqt HINTS "${_snow_qt_bin}" REQUIRED)
else()
    set(SNOW_MACDEPLOYQT "")
endif()
set(SNOW_MACOS_CODESIGN_IDENTITY "-" CACHE STRING
    "Code-signing certificate name or SHA-1; AUTO provisions the local development identity; '-' uses ad-hoc signing")
if(SNOW_MACOS_CODESIGN_IDENTITY STREQUAL "")
    message(FATAL_ERROR "SNOW_MACOS_CODESIGN_IDENTITY must be AUTO, a certificate identity, or '-'")
elseif(SNOW_MACOS_CODESIGN_IDENTITY STREQUAL "AUTO")
    execute_process(
        COMMAND "${CMAKE_CURRENT_LIST_DIR}/../scripts/ensure-macos-codesign-identity.sh"
        RESULT_VARIABLE _snow_codesign_result
        OUTPUT_VARIABLE SNOW_MACOS_CODESIGN_IDENTITY
        ERROR_VARIABLE _snow_codesign_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(LENGTH "${SNOW_MACOS_CODESIGN_IDENTITY}" _snow_codesign_length)
    if(NOT _snow_codesign_result EQUAL 0 OR
       NOT SNOW_MACOS_CODESIGN_IDENTITY MATCHES "^[0-9A-Fa-f]+$" OR
       NOT _snow_codesign_length EQUAL 40)
        message(FATAL_ERROR
            "Could not provision the local macOS signing identity: ${_snow_codesign_error}")
    endif()
endif()
if(NOT SNOW_MACOS_CODESIGN_IDENTITY STREQUAL "-")
    # Ninja otherwise leaves the raw executable linker/ad-hoc signed, making its
    # TCC designated requirement change on every relink during IDE debugging.
    # Sign a standalone copy because codesign treats the in-bundle executable as
    # the whole bundle, while the raw development layout keeps data under MacOS.
    add_custom_command(TARGET snow_shot POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:snow_shot>"
            "$<TARGET_FILE:snow_shot>.snow-signing"
        COMMAND /usr/bin/codesign --force --sign "${SNOW_MACOS_CODESIGN_IDENTITY}"
            --identifier com.snowshot.snow_shot "$<TARGET_FILE:snow_shot>.snow-signing"
        COMMAND /usr/bin/codesign --verify --strict "$<TARGET_FILE:snow_shot>.snow-signing"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:snow_shot>.snow-signing"
            "$<TARGET_FILE:snow_shot>"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE:snow_shot>.snow-signing"
        VERBATIM)
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/DeploySnowShotMacOS.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/DeploySnowShotMacOS.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeploySnowShotMacOS.cmake" COMPONENT SnowShot)
