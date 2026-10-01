# Full-only leaf implementations must never enter Mini through a copied source
# list or a transitive static/object library. Shared image codecs and recording
# dependencies remain available because Mini still saves images and records.
include_guard(GLOBAL)

set(SNOW_SHOT_MINI_EXCLUDED_SOURCE_NAMES
    selectedtexttranslationcoordinator selectedtexttranslationcontroller
    standalonetranslationwindow translationpagewidget translationpagecontroller
    translationservice translationlanguages snowshotapiclient
    texttranslationsettingswidget customaimodelssettingswidget
    screenshottranslationsettingsdialog
    screenshotqrcontroller screenshotqrrecognitionservice
    screenshotimageconversioncontroller screenshotimageconversionview
    screenshottabledocument screenshottableeditor screenshotrecognitionfileexport)
list(JOIN SNOW_SHOT_MINI_EXCLUDED_SOURCE_NAMES "|" _snow_mini_source_names)
set(SNOW_SHOT_MINI_EXCLUDED_SOURCE_PATTERN
    "(^|[/,:])(${_snow_mini_source_names})\\.(cpp|h|mm)($|>)")
set(SNOW_SHOT_MINI_EXCLUDED_LINK_TARGETS
    snow_shot_translation snow_selected_text_c snow_shot_full_rust_ffi_bundle
    opencv_wechat_qrcode opencv_objdetect opencv_dnn)
set(SNOW_SHOT_MINI_EXCLUDED_TARGETS
    ${SNOW_SHOT_MINI_EXCLUDED_LINK_TARGETS}
    snow_shot snow_shot_edition_full
    snow_shot_storage snow_shot_settings_catalog snow_shot_settings_search
    snow_shot_settings snow_shot_global_mouse snow_shot_login_item
    snow_shot_administrator snow_shot_permissions snow_shot_updates
    snow_shot_diagnostics snow_shot_crash_bridge
    snow_shot_mcp snow_shot_mcp_build
    snow-shot-updater-binary snow-shot-updater-binary_build
    snow_shot_release_translations)
if(WIN32)
    # Windows Mini acquires its trusted text OCR worker on demand. macOS Mini
    # deliberately bundles the local worker and its default model instead.
    list(APPEND SNOW_SHOT_MINI_EXCLUDED_TARGETS
        snow_ocr_process snow_ocr_process_build snow_ocr_diagnostics_bridge
        onnxruntime::onnxruntime)
endif()

function(snow_shot_mini_filter_sources output)
    set(_sources ${ARGN})
    list(FILTER _sources EXCLUDE REGEX "${SNOW_SHOT_MINI_EXCLUDED_SOURCE_PATTERN}")
    list(FILTER _sources EXCLUDE REGEX "qrc_snow_shot_translations\\.cpp$|/snow_shot_translations\\.qrc$")
    # Qt also attaches generated .qm inputs as header-only target sources. Do
    # not copy these, or building Mini still invokes Full's lrelease commands.
    list(FILTER _sources EXCLUDE REGEX "(^|/)snow_shot_(en_US|zh_CN|zh_TW)\\.qm$")
    set(${output} "${_sources}" PARENT_SCOPE)
endfunction()

# Inspect target references even inside LINK_ONLY, TARGET_OBJECTS and conditional
# generator expressions. Reject all configurations, including currently inactive
# branches, so switching a preset cannot silently restore an excluded feature.
function(snow_shot_assert_mini_build_contract root)
    set(_pending ${root})
    set(_visited)
    while(_pending)
        list(POP_FRONT _pending _target)
        if(_target IN_LIST _visited)
            continue()
        endif()
        list(APPEND _visited "${_target}")
        get_target_property(_alias ${_target} ALIASED_TARGET)
        if(_alias)
            list(APPEND _pending "${_alias}")
        endif()
        if(_target IN_LIST SNOW_SHOT_MINI_EXCLUDED_TARGETS)
            message(FATAL_ERROR "Mini build includes Full-only target: ${_target} (from ${root})")
        endif()
        foreach(_property IN ITEMS SOURCES INTERFACE_SOURCES LINK_LIBRARIES
                INTERFACE_LINK_LIBRARIES MANUALLY_ADDED_DEPENDENCIES)
            get_target_property(_items ${_target} ${_property})
            if(NOT _items)
                continue()
            endif()
            if(_property MATCHES "SOURCES$")
                foreach(_source IN LISTS _items)
                    string(REPLACE "\\" "/" _source "${_source}")
                    if(_source MATCHES "${SNOW_SHOT_MINI_EXCLUDED_SOURCE_PATTERN}" OR
                            _source MATCHES "qrc_snow_shot_translations\\.cpp$|/snow_shot_translations\\.qrc$" OR
                            (_source MATCHES "(^|/)snow_shot_(en_US|zh_CN|zh_TW)\\.qm$" AND
                                NOT _source MATCHES "(^|/)i18n/mini/snow_shot_[^/]+\\.qm$"))
                        message(FATAL_ERROR "Mini build includes Full-only source: ${_source} (${_target})")
                    endif()
                endforeach()
            endif()
            set(_expressions)
            foreach(_item IN LISTS _items)
                if(TARGET "${_item}")
                    list(APPEND _pending "${_item}")
                elseif(_property MATCHES "SOURCES$")
                    string(REGEX MATCHALL "\\$<TARGET_OBJECTS:[^>]+>" _objects "${_item}")
                    list(APPEND _expressions ${_objects})
                elseif(_item MATCHES "\\$<")
                    list(APPEND _expressions "${_item}")
                endif()
            endforeach()
            string(REGEX MATCHALL "[A-Za-z0-9_.:+-]+" _references "${_expressions}")
            foreach(_reference IN LISTS _references)
                # ':' is also a generator-expression separator. Keep namespace
                # aliases such as Qt6::Core intact while unwrapping expressions.
                string(REGEX REPLACE "(^|[^:]):([^:])" "\\1;\\2" _references_in_item "${_reference}")
                foreach(_dependency IN LISTS _references_in_item)
                    if(TARGET "${_dependency}")
                        list(APPEND _pending "${_dependency}")
                    endif()
                endforeach()
            endforeach()
        endforeach()
    endwhile()
endfunction()
