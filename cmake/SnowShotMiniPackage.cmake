if(NOT TARGET snow_shot_mini)
    return()
endif()

# Use CPack's normal encoder to create a second configuration. The standard
# CPackConfig.cmake remains the historical full product configuration.
function(snow_shot_configure_mini_package)
    # include(CPack) leaves source-package variables in the caller's scope.
    # Start from the saved binary configuration, including its generator and
    # install inputs, so Mini cannot accidentally package the source tree.
    get_cmake_property(_snow_cpack_variables VARIABLES)
    foreach(_variable IN LISTS _snow_cpack_variables)
        if(_variable MATCHES "^CPACK_")
            unset(${_variable})
        endif()
    endforeach()
    include("${CMAKE_BINARY_DIR}/CPackConfig.cmake")
    set(CPACK_PACKAGE_NAME snow-shot-mini)
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Snow Shot Mini screenshot utility")
    set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/snow_shot/packaging/README-mini.txt")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/snow_shot/packaging/README-mini.txt")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY SnowShotMini)
    set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY SnowShotMini)
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${CMAKE_PROJECT_NAME};SnowShotMini;/")
    set(CPACK_COMPONENTS_ALL SnowShotMini)
    if(APPLE)
        set(CPACK_PACKAGE_FILE_NAME "snow-shot-mini-${SNOW_SHOT_VERSION}-macos-arm64")
        set(CPACK_DMG_VOLUME_NAME "Snow Shot Mini")
        set(CPACK_SNOW_SHOT_BUNDLE_NAME snow_shot_mini)
        set(CPACK_SNOW_SHOT_PRODUCT_NAME "Snow Shot Mini")
        set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${_snow_dmg_assets}/dmg-mini-layout.applescript")
        set(_background "${CMAKE_BINARY_DIR}/macos/dmg-mini-background.svg")
        set(CPACK_DMG_BACKGROUND_IMAGE "${CMAKE_BINARY_DIR}/macos/dmg-mini-background.png")
        configure_file("${_snow_dmg_assets}/dmg-mini-background.svg" "${_background}" @ONLY)
        execute_process(COMMAND /usr/bin/sips -s format png "${_background}"
            --out "${CPACK_DMG_BACKGROUND_IMAGE}" OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
    else()
        set(SNOW_SHOT_PRODUCT_NAME "Snow Shot Mini")
        set(SNOW_SHOT_EXECUTABLE_NAME snow_shot_mini)
        if(NOT SNOW_WINDOWS_PLATFORM)
            if(NOT SNOW_WINDOWS_ARCHITECTURE)
                set(SNOW_WINDOWS_ARCHITECTURE x64)
            endif()
            set(SNOW_WINDOWS_PLATFORM "windows-${SNOW_WINDOWS_ARCHITECTURE}")
        endif()
        set(CPACK_PACKAGE_FILE_NAME "snow-shot-mini-${SNOW_SHOT_VERSION}-${SNOW_WINDOWS_PLATFORM}")
        set(CPACK_PACKAGE_EXECUTABLES snow_shot_mini "Snow Shot Mini")
        set(CPACK_CREATE_DESKTOP_LINKS snow_shot_mini)
        set(CPACK_NSIS_DISPLAY_NAME "Snow Shot Mini")
        set(CPACK_NSIS_PACKAGE_NAME "Snow Shot Mini")
        set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\snow_shot_mini.exe")
        set(CPACK_NSIS_MUI_FINISHPAGE_RUN snow_shot_mini.exe)
        set(CPACK_NSIS_UNINSTALL_NAME "Uninstall Snow Shot Mini")
        set(CPACK_NSIS_BRANDING_TEXT "Snow Shot Mini ${SNOW_SHOT_VERSION}")
        set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "")
        set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "")
        string(CONCAT CPACK_NSIS_DEFINES
            "VIProductVersion \"${SNOW_SHOT_VERSION_NUMERIC}.0\"\n"
            "VIAddVersionKey /LANG=1033 \"CompanyName\" \"${SNOW_SHOT_VENDOR}\"\n"
            "VIAddVersionKey /LANG=1033 \"FileDescription\" \"Snow Shot Mini installer\"\n"
            "VIAddVersionKey /LANG=1033 \"FileVersion\" \"${SNOW_SHOT_VERSION_NUMERIC}.0\"\n"
            "VIAddVersionKey /LANG=1033 \"InternalName\" \"snow-shot-mini-installer\"\n"
            "VIAddVersionKey /LANG=1033 \"LegalCopyright\" \"${SNOW_SHOT_COPYRIGHT}\"\n"
            "VIAddVersionKey /LANG=1033 \"OriginalFilename\" \"${CPACK_PACKAGE_FILE_NAME}.exe\"\n"
            "VIAddVersionKey /LANG=1033 \"ProductName\" \"Snow Shot Mini\"\n"
            "VIAddVersionKey /LANG=1033 \"ProductVersion\" \"${SNOW_SHOT_VERSION}\"\n"
            "!define SNOW_SHOT_INSTALLER_PRODUCT_NAME \"Snow Shot Mini\"\n"
            "!define SNOW_SHOT_INSTALLER_EXECUTABLE \"snow_shot_mini\"\n"
            "!define SNOW_SHOT_INSTALLER_UPDATER \"snow-shot-mini-updater\"\n")
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SnowShotPackageCompression.cmake")
        snow_shot_apply_nsis_compression()
        set(SNOW_SHOT_NSIS_DIRECTORY "${CMAKE_BINARY_DIR}/snow-shot-mini-nsis")
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SnowShotInstaller.cmake")
        set(CPACK_MODULE_PATH "${CMAKE_MODULE_PATH}")
        foreach(_var IN ITEMS CPACK_NSIS_EXTRA_INSTALL_COMMANDS CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS)
            string(REPLACE "snow-shot-updater.exe" "snow-shot-mini-updater.exe" ${_var} "${${_var}}")
        endforeach()
    endif()
    cpack_encode_variables()
    configure_file("${CMAKE_ROOT}/Templates/CPackConfig.cmake.in"
        "${CMAKE_BINARY_DIR}/CPackSnowShotMiniConfig.cmake" @ONLY)
endfunction()
snow_shot_configure_mini_package()
