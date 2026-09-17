# The Rust static archive does not propagate Cargo's native link metadata to CMake.
find_package(PkgConfig REQUIRED)
set(_snow_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "${SNOW_FFMPEG_ROOT}/lib/pkgconfig")
pkg_check_modules(SNOW_SHOT_FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
    libavformat libavcodec libswresample libswscale libavutil)
set(ENV{PKG_CONFIG_PATH} "${_snow_saved_pkg_config_path}")
set(_snow_native_libraries PkgConfig::SNOW_SHOT_FFMPEG objc)
foreach(_framework IN ITEMS AppKit ApplicationServices AVFoundation AudioToolbox
        Carbon CoreAudio CoreFoundation CoreGraphics CoreMedia CoreVideo
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
    INSTALL_RPATH "@executable_path/../Frameworks"
    INSTALL_RPATH_USE_LINK_PATH TRUE)
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

get_target_property(_snow_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_snow_qt_bin "${_snow_qmake}" DIRECTORY)
find_program(SNOW_MACDEPLOYQT NAMES macdeployqt HINTS "${_snow_qt_bin}" REQUIRED)
configure_file("${CMAKE_CURRENT_LIST_DIR}/DeploySnowShotMacOS.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/DeploySnowShotMacOS.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeploySnowShotMacOS.cmake" COMPONENT SnowShot)
