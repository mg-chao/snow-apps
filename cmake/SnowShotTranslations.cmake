# Editable feature catalogs are merged only in the build tree. All consumers
# share one lrelease rule and the same runtime resource names.
set(QT_I18N_SOURCE_LANGUAGE en_US)
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
qt_add_lrelease(
    TS_FILES ${_snow_shot_merged_ts}
    LRELEASE_TARGET snow_shot_release_translations
    QM_FILES_OUTPUT_VARIABLE _snow_shot_qm_files
    QM_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    MERGE_QT_TRANSLATIONS
    OPTIONS -fail-on-unfinished
)

function(snow_shot_add_translations target)
    qt_add_resources(${target} "${target}_translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}"
        FILES ${_snow_shot_qm_files}
    )
    add_dependencies(${target} snow_shot_release_translations)

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
            snow_shot
            snow_shot_storage
            snow_shot_settings_catalog
            snow_shot_settings_search
            snow_shot_settings
            snow_shot_global_mouse
            snow_shot_translation
            snow_shot_diagnostics
            snow_shot_updates
            snow_shot_update_core
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
