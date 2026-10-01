# Apply before Mini clones the Full targets so both editions use the same policy.
# These libraries contain configuration metadata and infrequent OS/network tasks.
# Mixed settings/storage libraries deliberately keep a speed policy: history
# thumbnails, PNG preparation, clipboard decoding and global shortcuts live there.
set(SNOW_SHOT_RELEASE_SIZE_TARGETS
    snow_shot_settings_catalog snow_shot_settings_search
    snow_shot_translation snow_shot_login_item snow_shot_administrator
    snow_shot_permissions snow_shot_updates snow_shot_crash_bridge)
set(SNOW_SHOT_RELEASE_SPEED_TARGETS
    snow_shot snow_shot_storage snow_shot_settings snow_shot_shortcuts
    snow_shot_window_shortcuts snow_shot_global_mouse snow_shot_diagnostics
    snow_shot_image_codec snow_shot_image_codec_backend
    snow_shot_history_pin snow_shot_clipboard_placement
    snow_shot_macos_clipboard snow_shot_macos_cursor snow_shot_macos_window_platform)

# Serialization/import/export do not participate in image processing loops.
set(SNOW_SHOT_RELEASE_SIZE_STORAGE_SOURCES
    src/storage/configurationarchive.cpp
    src/storage/configurationstore.cpp
    src/storage/configurationschema.cpp
    src/storage/persistedselectioncodec.cpp
    src/storage/persistedwindowgeometry.cpp
    src/storage/settingsadapters.cpp
    src/storage/storagedirectorychange.cpp)

# Settings forms and schema adapters are cold; history, thumbnail/clipboard
# code, streaming translation, theme rendering and shortcut dispatch stay fast.
set(SNOW_SHOT_RELEASE_SIZE_SETTINGS_SOURCES
    src/presentation/components/aboutpagewidget.cpp
    src/presentation/components/customaimodelssettingswidget.cpp
    src/presentation/components/settingscustomwidget.cpp
    src/presentation/components/settingspagewidget.cpp
    src/presentation/components/storagestatussettingswidget.cpp
    src/presentation/components/texttranslationsettingswidget.cpp
    src/presentation/settings/settingsbackend.cpp
    src/presentation/settings/settingsformfield.cpp
    src/presentation/settings/settingsruntimesession.cpp)

# The main executable mixes the shell with the latency-sensitive capture UI.
set(SNOW_SHOT_RELEASE_SIZE_SHELL_SOURCES
    src/app/updateconfirmationdialog.cpp
    src/app/applicationrestart.cpp
    src/app/featureavailability.cpp
    src/presentation/components/contentcardwidget.cpp
    src/presentation/components/maincontentheaderwidget.cpp
    src/presentation/components/sidebarwidget.cpp
    src/presentation/components/titlebarwidget.cpp)

if(SNOW_SHOT_ENABLE_AGGRESSIVE_RELEASE_OPTIMIZATION OR SNOW_SHOT_RELEASE_STATIC)
    foreach(_target IN LISTS SNOW_SHOT_RELEASE_SPEED_TARGETS SNOW_SHOT_RELEASE_SIZE_TARGETS)
        if(TARGET "${_target}")
            set(_policy speed)
            if(SNOW_APPS_ENABLE_RELEASE_SIZE_OPTIMIZATION AND
               _target IN_LIST SNOW_SHOT_RELEASE_SIZE_TARGETS)
                set(_policy size)
            endif()
            snow_apply_release_options("${_target}" POLICY "${_policy}")
        endif()
    endforeach()

    snow_apply_release_size_sources(snow_shot_storage SOURCES
        ${SNOW_SHOT_RELEASE_SIZE_STORAGE_SOURCES})

    snow_apply_release_size_sources(snow_shot_settings SOURCES
        ${SNOW_SHOT_RELEASE_SIZE_SETTINGS_SOURCES})

    snow_apply_release_size_sources(snow_shot SOURCES
        ${SNOW_SHOT_RELEASE_SIZE_SHELL_SOURCES})
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_test(NAME snow-shot-release-optimization-policy-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-release-optimization.py")
    set_tests_properties(snow-shot-release-optimization-policy-tests PROPERTIES
        LABELS unit TIMEOUT 60)
    if(WIN32)
        add_test(NAME snow-shot-release-optimization-pch-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-release-optimization-pch.py")
        set_tests_properties(snow-shot-release-optimization-pch-tests PROPERTIES
            LABELS "unit;windows" TIMEOUT 120)
    endif()
endif()
