# Instantiate a second product from the same source lists and build settings.
# Edition-sensitive libraries are recompiled; codecs, Qt, canvas and native FFI
# remain shared. No full-edition static library may leak into the Mini graph.
add_library(snow_shot_edition_full INTERFACE)
target_compile_definitions(snow_shot_edition_full INTERFACE SNOW_SHOT_EDITION_MINI=0)
foreach(_feature IN ITEMS QR_RECOGNITION TABLE_RECOGNITION IMAGE_CONVERSION
        LATEX_RECOGNITION TEXT_TRANSLATION API_CONFIGURATION EXTENDED_FEATURES)
    target_compile_definitions(snow_shot_edition_full INTERFACE SNOW_SHOT_ENABLE_${_feature}=1)
endforeach()

set(_snow_mini_libraries
    snow_shot_storage snow_shot_settings_catalog snow_shot_settings_search
    snow_shot_settings snow_shot_global_mouse snow_shot_login_item
    snow_shot_administrator snow_shot_permissions snow_shot_updates
    snow_shot_diagnostics snow_shot_crash_bridge)

if(NOT SNOW_APPS_BUILD_SNOW_SHOT_MINI)
    foreach(_original IN LISTS _snow_mini_libraries)
        target_link_libraries(${_original} PUBLIC snow_shot_edition_full)
    endforeach()
    target_link_libraries(snow_shot PRIVATE snow_shot_edition_full)
    target_sources(snow_shot PRIVATE include/snow_shot/app/edition.h)
    return()
endif()
add_library(snow_shot_edition_mini INTERFACE)
target_compile_definitions(snow_shot_edition_mini INTERFACE SNOW_SHOT_EDITION_MINI=1)
foreach(_feature IN ITEMS QR_RECOGNITION TABLE_RECOGNITION IMAGE_CONVERSION
        LATEX_RECOGNITION TEXT_TRANSLATION API_CONFIGURATION EXTENDED_FEATURES)
    target_compile_definitions(snow_shot_edition_mini INTERFACE SNOW_SHOT_ENABLE_${_feature}=0)
endforeach()

function(_snow_mini_rewrite out value)
    set(_index 0)
    foreach(_original IN LISTS _snow_mini_libraries)
        # Qt resource object targets have names such as settings_resources_1.
        # Only rewrite a complete target name, including in generator expressions.
        string(REGEX REPLACE "${_original}($|[>;])"
            "__SNOW_MINI_TARGET_${_index}__\\1" value "${value}")
        math(EXPR _index "${_index} + 1")
    endforeach()
    set(_index 0)
    foreach(_original IN LISTS _snow_mini_libraries)
        string(REPLACE "__SNOW_MINI_TARGET_${_index}__" "${_original}_mini" value "${value}")
        math(EXPR _index "${_index} + 1")
    endforeach()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

function(_snow_mini_copy_build_properties original target)
    foreach(_property IN ITEMS INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES COMPILE_OPTIONS INTERFACE_COMPILE_OPTIONS
            COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS COMPILE_FEATURES
            INTERFACE_COMPILE_FEATURES LINK_OPTIONS INTERFACE_LINK_OPTIONS
            PRECOMPILE_HEADERS CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS
            MSVC_RUNTIME_LIBRARY UNITY_BUILD UNITY_BUILD_BATCH_SIZE
            INTERPROCEDURAL_OPTIMIZATION_RELEASE STATIC_LIBRARY_OPTIONS)
        get_target_property(_value ${original} ${_property})
        if(NOT _value STREQUAL "_value-NOTFOUND")
            _snow_mini_rewrite(_value "${_value}")
            set_property(TARGET ${target} PROPERTY ${_property} "${_value}")
        endif()
    endforeach()
    foreach(_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_value ${original} ${_property})
        if(NOT _value STREQUAL "_value-NOTFOUND")
            list(FILTER _value EXCLUDE REGEX "^(snow_shot_translation|opencv_wechat_qrcode|opencv_objdetect)$")
            _snow_mini_rewrite(_value "${_value}")
            set_property(TARGET ${target} PROPERTY ${_property} "${_value}")
        endif()
    endforeach()
endfunction()

# Generated resources are edition-neutral. Give their custom commands one
# owning target so concurrent builds never write or compile the same rcc output.
function(_snow_mini_share_resources target)
    get_target_property(_sources ${target} SOURCES)
    set(_resources ${_sources})
    list(FILTER _resources INCLUDE REGEX "/qrc_[^/]+\\.cpp$")
    if(_resources)
        list(REMOVE_ITEM _sources ${_resources})
        set_property(TARGET ${target} PROPERTY SOURCES "${_sources}")
        add_library(${target}_shared_resources OBJECT ${_resources})
        set_target_properties(${target}_shared_resources PROPERTIES AUTOMOC OFF AUTORCC OFF)
        target_link_libraries(${target}_shared_resources PRIVATE Qt6::Core)
        target_link_libraries(${target} PRIVATE ${target}_shared_resources)
    endif()
endfunction()

foreach(_original IN LISTS _snow_mini_libraries)
    _snow_mini_share_resources(${_original})
    get_target_property(_sources ${_original} SOURCES)
    # Translation/API configuration views are leaf sources of settings.
    list(FILTER _sources EXCLUDE REGEX
        "(selectedtexttranslationcoordinator|standalonetranslationwindow|translationpagewidget|texttranslationsettingswidget|customaimodelssettingswidget)\\.(cpp|h)$")
    add_library(${_original}_mini STATIC ${_sources})
    _snow_mini_copy_build_properties(${_original} ${_original}_mini)
    target_link_libraries(${_original}_mini PUBLIC snow_shot_edition_mini)
endforeach()
target_compile_definitions(snow_shot_crash_bridge_mini PRIVATE
    SNOW_DIAGNOSTICS_PRODUCT="Snow Shot Mini")

_snow_mini_share_resources(snow_shot)
get_target_property(_sources snow_shot SOURCES)
list(FILTER _sources EXCLUDE REGEX "/packaging/macos/[^/]+\\.lproj/InfoPlist\\.strings$")
list(FILTER _sources EXCLUDE REGEX "windows-app-resource\\.rc$|_plugin_import\\.cpp$")
list(FILTER _sources EXCLUDE REGEX
    "(screenshotqrrecognitionservice|selectedtexttranslationcontroller|screenshottranslationsettingsdialog|screenshotimageconversionview|screenshottableeditor)\\.(cpp|h)$")
list(FILTER _sources EXCLUDE REGEX "(screenshotimageconversioncontroller|screenshottabledocument)\\.cpp$")
qt_add_executable(snow_shot_mini MANUAL_FINALIZATION ${_sources})
_snow_mini_copy_build_properties(snow_shot snow_shot_mini)
target_link_libraries(snow_shot_mini PRIVATE snow_shot_edition_mini)
set_target_properties(snow_shot_mini PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/snow_shot_mini"
    MACOSX_BUNDLE TRUE WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER com.snowshot.snow_shot_mini
    MACOSX_BUNDLE_BUNDLE_NAME "Snow Shot Mini"
    MACOSX_BUNDLE_BUNDLE_VERSION "${SNOW_SHOT_VERSION_NUMERIC}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${SNOW_SHOT_VERSION_NUMERIC}")
if(MSVC AND SNOW_SHOT_RELEASE_STATIC AND NOT SNOW_SHOT_QT_STATIC)
    set_property(TARGET snow_shot_mini PROPERTY qt_no_entrypoint TRUE)
endif()
if(WIN32)
    set(SNOW_SHOT_PRODUCT_NAME "Snow Shot Mini")
    set(SNOW_SHOT_EXECUTABLE_NAME snow_shot_mini)
    configure_file(resources/windows-app-resource.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/windows-mini-app-resource.rc" @ONLY)
    target_sources(snow_shot_mini PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/windows-mini-app-resource.rc")
    set(SNOW_SHOT_PRODUCT_NAME "Snow Shot")
    set(SNOW_SHOT_EXECUTABLE_NAME snow_shot)
endif()

function(_snow_add_mini_helpers)
    # Cargo features and build-script identity change these executables. Separate
    # output roots avoid races and accidentally staging the other edition.
    set(SNOW_RUST_CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo-mini")
    if(NOT APPLE)
        snow_add_rust_executable(snow-shot-mini-updater-binary
            PACKAGE snow-shot-updater MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust/snow-shot-updater"
            OUTPUT_NAME snow-shot-updater PRODUCTION_PROFILE release-size
            FEATURES mini REPRODUCIBLE SIZE_OPTIMIZED
            ENVIRONMENT
                "SNOW_SHOT_UPDATE_KEYS_PATH=${CMAKE_CURRENT_SOURCE_DIR}/resources/update-trusted-keys.json"
                "SNOW_SHOT_VERSION=${SNOW_SHOT_VERSION}"
                "SNOW_SHOT_ICON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/resources/app-icon.ico")
    endif()
    if(SNOW_SHOT_ENABLE_MCP)
        snow_add_rust_executable(snow_shot_mini_mcp
            PACKAGE snow-shot-mcp MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust/snow-shot-mcp"
            OUTPUT_NAME snow-shot-mcp PRODUCTION_PROFILE release-size
            FEATURES mini REPRODUCIBLE SIZE_OPTIMIZED)
    endif()
endfunction()
_snow_add_mini_helpers()
if(WIN32)
    set(_mini_bindir bin)
    set(_mini_datadir share)
else()
    set(_mini_bindir "snow_shot_mini.app/Contents/MacOS")
    set(_mini_datadir "snow_shot_mini.app/Contents/Resources")
endif()
install(TARGETS snow_shot_mini BUNDLE DESTINATION . COMPONENT SnowShotMini
    RUNTIME DESTINATION "${_mini_bindir}" COMPONENT SnowShotMini)
foreach(_helper IN ITEMS snow-shot-mini-updater-binary snow_shot_mini_mcp)
    if(TARGET ${_helper})
        if(_helper STREQUAL snow_shot_mini_mcp)
            set(_name snow-shot-mini-mcp)
        else()
            set(_name snow-shot-mini-updater)
        endif()
        if(WIN32)
            string(APPEND _name .exe)
        endif()
        add_dependencies(snow_shot_mini ${_helper}_build)
        add_custom_command(TARGET snow_shot_mini POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${_helper}>"
                "$<TARGET_FILE_DIR:snow_shot_mini>/${_name}" VERBATIM)
        install(PROGRAMS "$<TARGET_FILE:${_helper}>" DESTINATION "${_mini_bindir}"
            RENAME "${_name}" COMPONENT SnowShotMini)
    endif()
endforeach()
if(APPLE)
    add_custom_command(TARGET snow_shot_mini POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DSNOW_HANDLER_SOURCE=${SNOW_CRASHPAD_HANDLER}"
            "-DSNOW_HANDLER_DIRECTORY=$<TARGET_FILE_DIR:snow_shot_mini>"
            "-DSNOW_HANDLER_ZLIB=${_snow_handler_zlib}"
            -P "${CMAKE_CURRENT_LIST_DIR}/StageSnowShotCrashHandler.cmake" VERBATIM)
else()
    add_custom_command(TARGET snow_shot_mini POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SNOW_CRASHPAD_HANDLER}"
            "$<TARGET_FILE_DIR:snow_shot_mini>/crashpad_handler.exe" VERBATIM)
endif()
install(PROGRAMS "${SNOW_CRASHPAD_HANDLER}" DESTINATION "${_mini_bindir}" COMPONENT SnowShotMini)
if(NOT SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
    install(TARGETS snow_shot_image_codec_backend
        RUNTIME_DEPENDENCY_SET snow_shot_mini_image_runtime_dependencies
        LIBRARY DESTINATION "${_mini_bindir}" COMPONENT SnowShotMini
        RUNTIME DESTINATION "${_mini_bindir}" COMPONENT SnowShotMini)
    if(WIN32)
        install(RUNTIME_DEPENDENCY_SET snow_shot_mini_image_runtime_dependencies
            DIRECTORIES "$<TARGET_FILE_DIR:snow_shot_image_codec_backend>"
                "${_SNOW_SHOT_IMAGE_RUNTIME_SEARCH_DIRECTORY}"
            PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*" "msvcp.*\\.dll"
                "ucrtbased\\.dll" "vcruntime.*\\.dll"
            POST_EXCLUDE_REGEXES ".*[\\\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\\\/].*"
            RUNTIME DESTINATION "${_mini_bindir}" COMPONENT SnowShotMini)
    endif()
endif()
if(WIN32)
    add_custom_command(TARGET snow_shot_mini POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:snow_shot_mini>/assets/ocr"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/snow-shot-ocr-asset-manifest.json"
            "$<TARGET_FILE_DIR:snow_shot_mini>/assets/ocr/asset-manifest.json" VERBATIM)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/snow-shot-ocr-asset-manifest.json"
        DESTINATION "${_mini_bindir}/assets/ocr" RENAME asset-manifest.json COMPONENT SnowShotMini)
    if(NOT SNOW_SHOT_RELEASE_STATIC)
        add_custom_command(TARGET snow_shot_mini POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_RUNTIME_DLLS:snow_shot_mini>
                "$<TARGET_FILE_DIR:snow_shot_mini>" COMMAND_EXPAND_LISTS VERBATIM)
        if(SNOW_SHOT_FFMPEG_RUNTIME_FILES)
            add_custom_command(TARGET snow_shot_mini POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${SNOW_SHOT_FFMPEG_RUNTIME_FILES}
                    "$<TARGET_FILE_DIR:snow_shot_mini>" COMMAND_EXPAND_LISTS VERBATIM)
            install(FILES ${SNOW_SHOT_FFMPEG_RUNTIME_FILES} DESTINATION "${_mini_bindir}"
                COMPONENT SnowShotMini)
        endif()
    endif()
endif()
if(EXISTS "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}")
    add_custom_command(TARGET snow_shot_mini POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:snow_shot_mini>/audios"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}"
            "$<TARGET_FILE_DIR:snow_shot_mini>/audios/camera_shutter.mp3" VERBATIM)
    install(FILES "${SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE}" DESTINATION "${_mini_bindir}/audios"
        RENAME camera_shutter.mp3 COMPONENT SnowShotMini)
endif()
install(FILES LICENSE COPYRIGHT THIRD_PARTY_NOTICES.md
    DESTINATION "${_mini_datadir}/snow-shot-mini/licenses"
    COMPONENT SnowShotMini)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../ant_design_qt/THIRD_PARTY_NOTICES.md"
    DESTINATION "${_mini_datadir}/snow-shot-mini/licenses/third-party/project-notices/ant-design-icons"
    COMPONENT SnowShotMini)
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/third-party-licenses/third-party/"
    DESTINATION "${_mini_datadir}/snow-shot-mini/licenses/third-party" OPTIONAL COMPONENT SnowShotMini)
foreach(_project IN ITEMS snow_image ant_design_qt snow-crates snow_draw_engine_qt snow_rust_ffi)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../${_project}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../${_project}/COPYRIGHT"
        DESTINATION "${_mini_datadir}/snow-shot-mini/licenses/components/${_project}"
        COMPONENT SnowShotMini)
endforeach()
if(APPLE)
    include("${CMAKE_CURRENT_LIST_DIR}/SnowShotMiniMacOS.cmake")
elseif(NOT SNOW_SHOT_QT_STATIC)
    qt_generate_deploy_app_script(TARGET snow_shot_mini OUTPUT_SCRIPT _mini_qt_deploy
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT "${_mini_qt_deploy}" COMPONENT SnowShotMini)
endif()
if(SNOW_SHOT_QT_STATIC)
    qt_import_plugins(snow_shot_mini INCLUDE ${_snow_shot_static_qt_plugins})
endif()
qt_finalize_executable(snow_shot_mini)

# Full's fallback defaults also remain valid when Mini is switched off.
foreach(_original IN LISTS _snow_mini_libraries)
    target_link_libraries(${_original} PUBLIC snow_shot_edition_full)
endforeach()
target_link_libraries(snow_shot PRIVATE snow_shot_edition_full)
target_sources(snow_shot PRIVATE include/snow_shot/app/edition.h)

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-mini-edition-tests tests/mini_edition_tests.cpp)
    target_include_directories(snow-shot-mini-edition-tests PRIVATE include)
    target_link_libraries(snow-shot-mini-edition-tests PRIVATE
        snow_shot_settings_search_mini snow_shot_storage_mini snow_shot_edition_mini Qt6::Core)
    add_executable(snow-shot-mini-settings-reset-tests tests/mini_settings_reset_tests.cpp
        src/presentation/services/screenshotclipboardservice.cpp)
    target_include_directories(snow-shot-mini-settings-reset-tests PRIVATE include)
    target_link_libraries(snow-shot-mini-settings-reset-tests PRIVATE
        snow_shot_settings_mini snow_shot_edition_mini snow_shot_image_codec
        snow_shot_clipboard_placement Qt6::Widgets)
    if(APPLE)
        target_link_libraries(snow-shot-mini-settings-reset-tests PRIVATE snow_shot_macos_clipboard)
    endif()
    snow_shot_import_offscreen_platform(snow-shot-mini-settings-reset-tests)

    get_target_property(_mini_test_sources snow-shot-recognition-session-controller-tests SOURCES)
    list(FILTER _mini_test_sources EXCLUDE REGEX "(^tests/|/qrc_[^/]+\\.cpp$|(screenshotimageconversioncontroller|screenshottabledocument)\\.cpp$|(screenshottableeditor|screenshotimageconversionview)\\.(cpp|h)$)")
    list(FILTER _mini_test_sources EXCLUDE REGEX
        "(screenshotqrrecognitionservice|selectedtexttranslationcontroller|screenshottranslationsettingsdialog)\\.(cpp|h)$")
    add_executable(snow-shot-mini-recognition-tests ${_mini_test_sources}
        tests/mini_recognition_tests.cpp
        include/snow_shot/presentation/screenshotimageconversioncontroller.h)
    _snow_mini_copy_build_properties(snow-shot-recognition-session-controller-tests
        snow-shot-mini-recognition-tests)
    target_link_libraries(snow-shot-mini-recognition-tests PRIVATE snow_shot_edition_mini)
    snow_shot_add_translations(snow-shot-mini-recognition-tests)
    snow_shot_import_offscreen_platform(snow-shot-mini-recognition-tests)

    foreach(_edition IN ITEMS full mini)
        add_executable(snow-shot-${_edition}-mcp-edition-tests tests/mcp_edition_tests.cpp)
        target_include_directories(snow-shot-${_edition}-mcp-edition-tests PRIVATE include)
        target_link_libraries(snow-shot-${_edition}-mcp-edition-tests PRIVATE
            snow_shot_edition_${_edition} Qt6::Core)
    endforeach()
    get_target_property(_branding_sources snow-shot-about-page-tests SOURCES)
    list(REMOVE_ITEM _branding_sources tests/about_page_tests.cpp)
    foreach(_edition IN ITEMS full mini)
        set(_suffix "")
        if(_edition STREQUAL mini)
            set(_suffix _mini)
        endif()
        add_executable(snow-shot-${_edition}-branding-tests ${_branding_sources}
            tests/mini_branding_tests.cpp src/app/updateconfirmationdialog.cpp)
        target_include_directories(snow-shot-${_edition}-branding-tests PRIVATE include)
        target_compile_definitions(snow-shot-${_edition}-branding-tests PRIVATE
            SNOW_SHOT_TEST_TRANSLATIONS_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        target_link_libraries(snow-shot-${_edition}-branding-tests PRIVATE
            snow_shot_edition_${_edition} snow_shot_settings${_suffix}
            snow_shot_updates${_suffix} snow_shot_image_codec snow_shot_clipboard_placement Qt6::Widgets)
        if(APPLE)
            target_link_libraries(snow-shot-${_edition}-branding-tests PRIVATE
                snow_shot_macos_clipboard "-framework AppKit")
        endif()
        snow_shot_import_offscreen_platform(snow-shot-${_edition}-branding-tests)
        add_dependencies(snow-shot-${_edition}-branding-tests snow_shot_release_translations)
    endforeach()
    foreach(_test IN ITEMS snow-shot-mini-edition-tests snow-shot-mini-recognition-tests
            snow-shot-mini-settings-reset-tests
            snow-shot-full-mcp-edition-tests snow-shot-mini-mcp-edition-tests
            snow-shot-full-branding-tests snow-shot-mini-branding-tests)
        add_test(NAME ${_test} COMMAND ${_test})
        set_tests_properties(${_test} PROPERTIES LABELS unit TIMEOUT 60
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
    snow_shot_offscreen_qpa_environment(_mini_qpa_environment)
    set_tests_properties(snow-shot-mini-recognition-tests
        snow-shot-mini-settings-reset-tests
        snow-shot-full-branding-tests snow-shot-mini-branding-tests PROPERTIES
        ENVIRONMENT "${_mini_qpa_environment}")
    if(Python3_Interpreter_FOUND AND SNOW_SHOT_ENABLE_MCP)
        add_test(NAME snow-shot-mini-mcp-capabilities-tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_mcp_capabilities.py" --mini)
        set_tests_properties(snow-shot-mini-mcp-capabilities-tests PROPERTIES LABELS unit TIMEOUT 30)
    endif()
endif()
