set_target_properties(snow_shot_mini PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info-mini.plist.in"
    MACOSX_BUNDLE_ICON_FILE snow-shot.icns
    INSTALL_RPATH "@executable_path/../Frameworks" INSTALL_RPATH_USE_LINK_PATH TRUE)
target_sources(snow_shot_mini PRIVATE "${_snow_macos_icon}")
foreach(_language IN ITEMS en zh-Hans zh-Hant)
    set(_strings "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/mini/${_language}.lproj/InfoPlist.strings")
    set_source_files_properties("${_strings}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/${_language}.lproj")
    target_sources(snow_shot_mini PRIVATE "${_strings}")
endforeach()
target_link_options(snow_shot_mini PRIVATE -Wl,-headerpad_max_install_names)
if(SNOW_SHOT_RELEASE_STATIC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # The shared OCR worker's dSYM is collected by the Full symbol rule.
    file(READ "${CMAKE_CURRENT_LIST_DIR}/GenerateSnowShotDiagnosticsSymbols.cmake.in" _mini_symbols)
    string(REGEX REPLACE "# Cargo.*$" "" _mini_symbols "${_mini_symbols}")
    string(REPLACE "snow_shot>" "snow_shot_mini>" _mini_symbols "${_mini_symbols}")
    string(REPLACE "snow_shot.dSYM" "snow_shot_mini.dSYM" _mini_symbols "${_mini_symbols}")
    string(CONFIGURE "${_mini_symbols}" _mini_symbols @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GenerateSnowShotMiniDiagnosticsSymbols-$<CONFIG>.cmake"
        CONTENT "${_mini_symbols}")
    add_custom_command(TARGET snow_shot_mini POST_BUILD COMMAND "${CMAKE_COMMAND}" -P
        "${CMAKE_CURRENT_BINARY_DIR}/GenerateSnowShotMiniDiagnosticsSymbols-$<CONFIG>.cmake"
        VERBATIM)
endif()
if(NOT SNOW_MACOS_CODESIGN_IDENTITY STREQUAL "-")
    add_custom_command(TARGET snow_shot_mini POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:snow_shot_mini>"
            "$<TARGET_FILE:snow_shot_mini>.snow-signing"
        COMMAND /usr/bin/codesign --force --sign "${SNOW_MACOS_CODESIGN_IDENTITY}"
            --identifier com.snowshot.snow_shot_mini "$<TARGET_FILE:snow_shot_mini>.snow-signing"
        COMMAND /usr/bin/codesign --verify --strict "$<TARGET_FILE:snow_shot_mini>.snow-signing"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:snow_shot_mini>.snow-signing"
            "$<TARGET_FILE:snow_shot_mini>"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE:snow_shot_mini>.snow-signing"
        VERBATIM)
endif()
add_custom_target(snow_shot_mini-ocr-assets
    COMMAND "${Python3_EXECUTABLE}" "${SNOW_MACOS_OCR_TOOL}" stage
        --arch "${SNOW_MACOS_OCR_ARCH}"
        --manifest "${SNOW_MACOS_OCR_MANIFEST}"
        --runtime-dir "$<TARGET_FILE_DIR:snow_shot_mini>"
        --worker "$<TARGET_FILE:snow_ocr_process>" --runtime-only ${_snow_macos_ocr_runtime_arguments}
    DEPENDS snow_ocr_process onnxruntime::onnxruntime VERBATIM)
add_dependencies(snow_shot_mini snow_shot_mini-ocr-assets)
install(PROGRAMS "$<TARGET_FILE:snow_ocr_process>" DESTINATION "${_mini_bindir}" COMPONENT SnowShotMini)
install(DIRECTORY "$<TARGET_FILE_DIR:snow_shot_mini>/assets/ocr"
    DESTINATION "${_mini_bindir}/assets" COMPONENT SnowShotMini PATTERN models EXCLUDE)
if(NOT SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
    install(FILES "$<TARGET_FILE:onnxruntime::onnxruntime>" DESTINATION "${_mini_bindir}"
        RENAME libonnxruntime.dylib COMPONENT SnowShotMini)
endif()
if(NOT SNOW_SHOT_QT_STATIC)
    install(FILES "${SNOW_QT_OFFSCREEN_PLUGIN}"
        DESTINATION "snow_shot_mini.app/Contents/PlugIns/platforms" COMPONENT SnowShotMini)
endif()
if(SNOW_SHOT_RELEASE_STATIC)
    install(DIRECTORY "${_snow_static_qt_licenses}/"
        DESTINATION "${_mini_datadir}/snow-shot-mini/licenses/third-party/qt" COMPONENT SnowShotMini)
    install(DIRECTORY "${SNOW_FFMPEG_ROOT}/share/"
        DESTINATION "${_mini_datadir}/snow-shot-mini/licenses/third-party/vcpkg"
        COMPONENT SnowShotMini FILES_MATCHING PATTERN copyright)
endif()
file(READ "${CMAKE_CURRENT_LIST_DIR}/DeploySnowShotMacOS.cmake.in" _mini_deploy)
string(REPLACE "@SNOW_MACOS_OCR_RUNTIME_ONLY@" "ON" _mini_deploy "${_mini_deploy}")
string(REPLACE "snow_shot.app" "snow_shot_mini.app" _mini_deploy "${_mini_deploy}")
string(REPLACE "snow-shot-mcp" "snow-shot-mini-mcp" _mini_deploy "${_mini_deploy}")
string(REPLACE "snow-shot-updater" "snow-shot-mini-updater" _mini_deploy "${_mini_deploy}")
string(REPLACE "macos-ocr-verification.json" "macos-mini-ocr-verification.json" _mini_deploy "${_mini_deploy}")
string(APPEND _mini_deploy [==[
set(SNOW_SHOT_MINI_APP "${_app}")
set(SNOW_SHOT_MINI_STATIC @SNOW_SHOT_RELEASE_STATIC@)
set(SNOW_SHOT_MINI_MCP @SNOW_SHOT_ENABLE_MCP@)
include("@CMAKE_CURRENT_LIST_DIR@/AssertSnowShotMiniMacOSPayload.cmake")
]==])
string(CONFIGURE "${_mini_deploy}" _mini_deploy @ONLY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/DeploySnowShotMiniMacOS.cmake" "${_mini_deploy}")
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeploySnowShotMiniMacOS.cmake" COMPONENT SnowShotMini)
