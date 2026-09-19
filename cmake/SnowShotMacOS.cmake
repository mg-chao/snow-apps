# The Rust static archive does not propagate Cargo's native link metadata to CMake.
find_package(PkgConfig REQUIRED)
set(_snow_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "${SNOW_FFMPEG_ROOT}/lib/pkgconfig")
pkg_check_modules(SNOW_SHOT_FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
    libavformat libavcodec libswresample libswscale libavutil)
set(ENV{PKG_CONFIG_PATH} "${_snow_saved_pkg_config_path}")
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

# ONNX Runtime is loaded with dlopen, so deployment cannot discover it from
# the application's Mach-O dependencies. Keep its stable name beside the helper.
add_custom_command(TARGET snow_shot POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:onnxruntime::onnxruntime>"
        "$<TARGET_FILE_DIR:snow_shot>/libonnxruntime.dylib"
    VERBATIM)
install(FILES "$<TARGET_FILE:onnxruntime::onnxruntime>"
    DESTINATION "snow_shot.app/Contents/MacOS" RENAME libonnxruntime.dylib
    COMPONENT SnowShot)

set(SNOW_MACOS_OCR_ASSETS_ENABLED OFF)
if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" AND TARGET snow_ocr_process)
    set(SNOW_MACOS_OCR_ASSETS_ENABLED ON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(SNOW_MACOS_OCR_TOOL "${CMAKE_CURRENT_LIST_DIR}/../scripts/snow-shot-macos-ocr.py")
    set(SNOW_MACOS_OCR_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../snow_shot/packaging/snow-shot-ocr-asset-manifest.json")
    foreach(_host IN ITEMS snow_shot snow-shot-ocr-recognition-service-tests)
        if(TARGET ${_host})
            # Always check the content hashes: Cargo can rebuild the worker without
            # relinking its Qt host. A host POST_BUILD hook would miss that update.
            add_custom_target(${_host}-ocr-assets
                COMMAND "${Python3_EXECUTABLE}" "${SNOW_MACOS_OCR_TOOL}" stage
                    --manifest "${SNOW_MACOS_OCR_MANIFEST}"
                    --runtime-dir "$<TARGET_FILE_DIR:${_host}>"
                    --worker "$<TARGET_FILE:snow_ocr_process>"
                    --library "$<TARGET_FILE:onnxruntime::onnxruntime>"
                DEPENDS snow_ocr_process onnxruntime::onnxruntime
                    "${SNOW_MACOS_OCR_TOOL}" "${SNOW_MACOS_OCR_MANIFEST}"
                VERBATIM)
            add_dependencies(${_host} ${_host}-ocr-assets)
        endif()
    endforeach()
    install(DIRECTORY "$<TARGET_FILE_DIR:snow_shot>/assets/ocr"
        DESTINATION "snow_shot.app/Contents/MacOS/assets" COMPONENT SnowShot)
endif()

get_target_property(_snow_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_snow_qt_bin "${_snow_qmake}" DIRECTORY)
find_file(SNOW_QT_OFFSCREEN_PLUGIN NAMES libqoffscreen.dylib
    HINTS "${_snow_qt_bin}/../plugins/platforms" NO_DEFAULT_PATH REQUIRED)
install(FILES "${SNOW_QT_OFFSCREEN_PLUGIN}"
    DESTINATION "snow_shot.app/Contents/PlugIns/platforms" COMPONENT SnowShot)
find_program(SNOW_MACDEPLOYQT NAMES macdeployqt HINTS "${_snow_qt_bin}" REQUIRED)
set(SNOW_MACOS_CODESIGN_IDENTITY "-" CACHE STRING
    "Code-signing certificate name or SHA-1; '-' uses ad-hoc signing (permissions may reset after rebuilds)")
if(SNOW_MACOS_CODESIGN_IDENTITY STREQUAL "")
    message(FATAL_ERROR "SNOW_MACOS_CODESIGN_IDENTITY must be a certificate identity or '-'")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/DeploySnowShotMacOS.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/DeploySnowShotMacOS.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeploySnowShotMacOS.cmake" COMPONENT SnowShot)
