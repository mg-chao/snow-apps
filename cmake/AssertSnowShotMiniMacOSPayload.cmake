# Installed Mini bundles retain the local OCR runtime on macOS. Models are
# downloaded on demand; no model folders or Full helpers may be packaged.
if(NOT DEFINED SNOW_SHOT_MINI_APP OR NOT IS_DIRECTORY "${SNOW_SHOT_MINI_APP}/Contents")
    message(FATAL_ERROR "Snow Shot Mini bundle was not found")
endif()
set(_mini_runtime "${SNOW_SHOT_MINI_APP}/Contents/MacOS")
if(SNOW_SHOT_MINI_STATIC)
    set(_mini_expected_binaries snow_shot_mini snow-ocr-process crashpad_handler)
    if(SNOW_SHOT_MINI_MCP)
        list(APPEND _mini_expected_binaries snow-shot-mini-mcp)
    endif()
    file(GLOB _mini_runtime_entries RELATIVE "${_mini_runtime}" "${_mini_runtime}/*")
    set(_mini_binaries)
    foreach(_mini_entry IN LISTS _mini_runtime_entries)
        if(NOT _mini_entry STREQUAL "assets" AND NOT _mini_entry STREQUAL "audios")
            list(APPEND _mini_binaries "${_mini_entry}")
        endif()
    endforeach()
    list(SORT _mini_expected_binaries)
    list(SORT _mini_binaries)
    if(NOT _mini_binaries STREQUAL _mini_expected_binaries)
        message(FATAL_ERROR "Mini macOS contains unexpected runtime files: ${_mini_binaries}")
    endif()
    file(GLOB _mini_dynamic_files
        "${SNOW_SHOT_MINI_APP}/Contents/Frameworks/*"
        "${SNOW_SHOT_MINI_APP}/Contents/PlugIns/*")
    if(_mini_dynamic_files)
        message(FATAL_ERROR "Static Mini macOS must not bundle dynamic frameworks or plugins")
    endif()
endif()

set(_mini_expected_assets ocr ocr/asset-manifest.json)
set(_mini_assets "${SNOW_SHOT_MINI_APP}/Contents/Resources/assets")
file(GLOB_RECURSE _mini_actual_assets LIST_DIRECTORIES TRUE RELATIVE "${_mini_assets}"
    "${_mini_assets}/*")
list(SORT _mini_expected_assets)
list(SORT _mini_actual_assets)
if(NOT _mini_actual_assets STREQUAL _mini_expected_assets)
    message(FATAL_ERROR "Mini macOS contains unexpected or missing assets: ${_mini_actual_assets}")
endif()
set(_mini_audio "${SNOW_SHOT_MINI_APP}/Contents/Resources/audios")
file(GLOB_RECURSE _mini_audio_files LIST_DIRECTORIES FALSE RELATIVE "${_mini_audio}"
    "${_mini_audio}/*")
if(_mini_audio_files AND NOT _mini_audio_files STREQUAL "camera_shutter.mp3")
    message(FATAL_ERROR "Mini macOS contains unexpected audio resources: ${_mini_audio_files}")
endif()

# Bundle metadata and license trees are the only resources outside OCR/audio.
# Inspect directories too, so unused model or Full resource folders cannot be
# carried forward by a reused staging directory even when they are empty.
set(_mini_resources "${SNOW_SHOT_MINI_APP}/Contents/Resources")
set(_mini_allowed_resources assets audios audios/camera_shutter.mp3 snow-shot.icns
    en.lproj en.lproj/InfoPlist.strings
    zh-Hans.lproj zh-Hans.lproj/InfoPlist.strings
    zh-Hant.lproj zh-Hant.lproj/InfoPlist.strings
    snow-shot-mini snow-shot-mini/licenses)
foreach(_mini_asset IN LISTS _mini_expected_assets)
    list(APPEND _mini_allowed_resources "assets/${_mini_asset}")
endforeach()
if(NOT SNOW_SHOT_MINI_STATIC)
    # macdeployqt can generate this configuration for its deployed Qt plugins.
    list(APPEND _mini_allowed_resources qt.conf)
endif()
file(GLOB_RECURSE _mini_resource_entries LIST_DIRECTORIES TRUE RELATIVE "${_mini_resources}"
    "${_mini_resources}/*")
foreach(_mini_resource IN LISTS _mini_resource_entries)
    list(FIND _mini_allowed_resources "${_mini_resource}" _mini_allowed_index)
    if(_mini_allowed_index EQUAL -1 AND
            NOT _mini_resource MATCHES "^snow-shot-mini/licenses/")
        message(FATAL_ERROR "Mini macOS contains an unexpected resource: ${_mini_resource}")
    endif()
endforeach()
