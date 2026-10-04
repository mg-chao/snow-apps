# Editable feature catalogs are merged only in the build tree. Each edition
# shares its lrelease rule while retaining the same runtime resource names.
set(QT_I18N_SOURCE_LANGUAGE en_US)
include("${CMAKE_CURRENT_LIST_DIR}/SnowShotTranslationRelease.cmake")
set(_snow_shot_catalog_dir "${CMAKE_CURRENT_SOURCE_DIR}/i18n")
set(_snow_shot_catalog_tool "${CMAKE_CURRENT_SOURCE_DIR}/scripts/translation_catalogs.py")
set(_snow_shot_merged_dir "${CMAKE_CURRENT_BINARY_DIR}/i18n/merged")
set(_snow_shot_update_dir "${CMAKE_CURRENT_BINARY_DIR}/i18n/update")
file(GLOB_RECURSE TS_FILES CONFIGURE_DEPENDS "${_snow_shot_catalog_dir}/*.ts")
list(SORT TS_FILES)
set(_snow_shot_merged_ts)
set(_snow_shot_update_ts)
foreach(_locale IN ITEMS en_US zh_CN zh_TW)
    list(APPEND _snow_shot_merged_ts "${_snow_shot_merged_dir}/snow_shot_${_locale}.ts")
    list(APPEND _snow_shot_update_ts "${_snow_shot_update_dir}/snow_shot_${_locale}.ts")
endforeach()

# Qt reads the TS language during configuration when merging its own catalogs.
# Seed real catalogs rather than allowing Qt to create empty generated inputs.
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_snow_shot_catalog_tool}" merge
        --catalog-dir "${_snow_shot_catalog_dir}" --output-dir "${_snow_shot_merged_dir}"
    COMMAND_ERROR_IS_FATAL ANY
)
add_custom_command(
    OUTPUT ${_snow_shot_merged_ts}
    COMMAND "${Python3_EXECUTABLE}" "${_snow_shot_catalog_tool}" merge
        --catalog-dir "${_snow_shot_catalog_dir}" --output-dir "${_snow_shot_merged_dir}"
    DEPENDS ${TS_FILES} "${_snow_shot_catalog_dir}/modules.json" "${_snow_shot_catalog_tool}"
    COMMENT "Merging Snow Shot feature translation catalogs"
    VERBATIM
)
snow_shot_release_translation_catalogs(snow_shot_release_translations
    "${_snow_shot_merged_dir}" "${CMAKE_CURRENT_BINARY_DIR}" _snow_shot_qm_files)

if(SNOW_APPS_BUILD_SNOW_SHOT_MINI)
    set(_snow_shot_mini_merged_dir "${CMAKE_CURRENT_BINARY_DIR}/i18n/mini-merged")
    set(_snow_shot_mini_qm_dir "${CMAKE_CURRENT_BINARY_DIR}/i18n/mini")
    set(_snow_shot_mini_merged_ts)
    foreach(_locale IN ITEMS en_US zh_CN zh_TW)
        list(APPEND _snow_shot_mini_merged_ts
            "${_snow_shot_mini_merged_dir}/snow_shot_${_locale}.ts")
    endforeach()
    set(_snow_shot_mini_catalog_arguments --exclude-module translation)
    foreach(_context IN ITEMS SnowShotApiClient CustomAiModelsSettingsWidget
            TextTranslationSettingsWidget ScreenshotQrController
            ScreenshotImageConversionController ScreenshotImageConversionView
            ScreenshotTableEditor ScreenshotRecognitionFileExport)
        list(APPEND _snow_shot_mini_catalog_arguments --exclude-context "${_context}")
    endforeach()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_snow_shot_catalog_tool}" merge
            --catalog-dir "${_snow_shot_catalog_dir}" --output-dir "${_snow_shot_mini_merged_dir}"
            ${_snow_shot_mini_catalog_arguments}
        COMMAND_ERROR_IS_FATAL ANY)
    add_custom_command(
        OUTPUT ${_snow_shot_mini_merged_ts}
        COMMAND "${Python3_EXECUTABLE}" "${_snow_shot_catalog_tool}" merge
            --catalog-dir "${_snow_shot_catalog_dir}" --output-dir "${_snow_shot_mini_merged_dir}"
            ${_snow_shot_mini_catalog_arguments}
        DEPENDS ${TS_FILES} "${_snow_shot_catalog_dir}/modules.json" "${_snow_shot_catalog_tool}"
        COMMENT "Merging Mini translation catalogs without excluded feature contexts"
        VERBATIM)
    snow_shot_release_translation_catalogs(snow_shot_mini_release_translations
        "${_snow_shot_mini_merged_dir}" "${_snow_shot_mini_qm_dir}" _snow_shot_mini_qm_files)
endif()

function(snow_shot_add_translations target)
    if("MINI" IN_LIST ARGN)
        set(_qm_files ${_snow_shot_mini_qm_files})
        set(_qm_base "${_snow_shot_mini_qm_dir}")
        set(_release_target snow_shot_mini_release_translations)
    else()
        set(_qm_files ${_snow_shot_qm_files})
        set(_qm_base "${CMAKE_CURRENT_BINARY_DIR}")
        set(_release_target snow_shot_release_translations)
    endif()
    qt_add_resources(${target} "${target}_translations"
        PREFIX "/i18n"
        BASE "${_qm_base}"
        FILES ${_qm_files}
    )
    add_dependencies(${target} ${_release_target})

    # Test targets consume the complete catalogs but must never extract their
    # partial source lists back into application translations.
    if(NOT target STREQUAL "snow_shot")
        return()
    endif()

    add_custom_target(snow_shot_prepare_translation_update
        COMMAND "${Python3_EXECUTABLE}" "${_snow_shot_catalog_tool}" merge
            --catalog-dir "${_snow_shot_catalog_dir}" --output-dir "${_snow_shot_update_dir}"
        VERBATIM
    )
    qt_add_lupdate(
        SOURCE_TARGETS
            snow_shot_login_item
            snow_shot_administrator
            snow_shot
            snow_shot_storage
            snow_shot_main_window_skin
            snow_shot_settings_catalog
            snow_shot_settings_search
            snow_shot_settings
            snow_shot_permissions
            snow_shot_global_mouse
            snow_shot_translation
            snow_shot_diagnostics
            snow_shot_updates
        # Preserve macOS-only messages when extracting on Windows as well.
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/loginitembackend.mm"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/update/macosupdateservice.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/update/updateservice.cpp"
        TS_FILES ${_snow_shot_update_ts}
        LUPDATE_TARGET snow_shot_update_translations
        OPTIONS -no-obsolete -locations none
    )
    add_dependencies(snow_shot_update_translations snow_shot_prepare_translation_update)
    add_custom_command(TARGET snow_shot_update_translations POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${_snow_shot_catalog_tool}" split
            --catalog-dir "${_snow_shot_catalog_dir}" --input-dir "${_snow_shot_update_dir}"
        COMMENT "Updating Snow Shot feature translation catalogs"
        VERBATIM
    )
endfunction()
