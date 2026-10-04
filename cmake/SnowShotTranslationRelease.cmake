include_guard(GLOBAL)

function(snow_shot_release_translation_catalogs target catalog_dir output_dir output_variable)
    set(_qt_translation_dir "${QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR}")
    if(_qt_translation_dir STREQUAL "")
        set(_qt_translation_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}")
    endif()
    foreach(_locale IN ITEMS zh_CN zh_TW)
        if(NOT EXISTS "${_qt_translation_dir}/qtbase_${_locale}.qm")
            message(FATAL_ERROR
                "The selected Qt kit is missing qtbase_${_locale}.qm. "
                "Build/install the qttranslations module to preserve stock-dialog translations.")
        endif()
    endforeach()

    # Qt widgets already use English source text. Merging an empty qtbase_en
    # catalog into en_US adds no strings and causes a misleading missing-catalog warning.
    qt_add_lrelease(
        TS_FILES "${catalog_dir}/snow_shot_en_US.ts"
        LRELEASE_TARGET ${target}_source_language
        QM_FILES_OUTPUT_VARIABLE _source_qm_files
        QM_OUTPUT_DIRECTORY "${output_dir}"
        OPTIONS -fail-on-unfinished)
    qt_add_lrelease(
        TS_FILES "${catalog_dir}/snow_shot_zh_CN.ts" "${catalog_dir}/snow_shot_zh_TW.ts"
        LRELEASE_TARGET ${target}_localized
        QM_FILES_OUTPUT_VARIABLE _localized_qm_files
        QM_OUTPUT_DIRECTORY "${output_dir}"
        MERGE_QT_TRANSLATIONS
        QT_TRANSLATION_CATALOGS qtbase
        OPTIONS -fail-on-unfinished)
    add_custom_target(${target} ALL)
    add_dependencies(${target} ${target}_source_language ${target}_localized)
    set(${output_variable} ${_source_qm_files} ${_localized_qm_files} PARENT_SCOPE)
endfunction()
