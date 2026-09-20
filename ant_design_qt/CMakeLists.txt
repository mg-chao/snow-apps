cmake_minimum_required(VERSION 3.22)
project(ant_design_qt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)

option(ADQT_BUILD_TESTS "Build ant_design_qt tests." OFF)
option(ADQT_BUILD_BENCHMARKS "Build ant_design_qt performance benchmarks." OFF)
option(ADQT_STRICT_COMPILE "Treat ant_design_qt compiler warnings as errors." ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ProjectWarnings.cmake")
if(ADQT_STRICT_COMPILE)
    snow_enable_strict_warnings()
endif()
snow_enable_clang_tidy()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network LinguistTools)
if(NOT TARGET snow_image::snow_image)
    find_package(snow_image CONFIG REQUIRED)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SnowTestCliGuard.cmake")

file(GLOB ADQT_ICON_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/adqt_icon_core/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/adqt_icon_core/src/*.h"
)
add_library(adqt_icon_core STATIC ${ADQT_ICON_CORE_SOURCES})
add_library(adqt::icon_core ALIAS adqt_icon_core)
target_include_directories(adqt_icon_core SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/adqt_icon_core/src"
)
target_link_libraries(adqt_icon_core PUBLIC Qt6::Widgets Qt6::Svg)

file(GLOB_RECURSE ADQT_ICONS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_icons_qt/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_icons_qt/src/*.h"
)
# The checked-in icon registry is validated by its generator and is too large
# for useful source-level linting. It remains subject to strict compilation.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_icons_qt/src/antd_icons.cpp"
    PROPERTIES
        SKIP_LINTING ON
        SKIP_UNITY_BUILD_INCLUSION ON
)
add_library(ant_design_icons_qt STATIC ${ADQT_ICONS_SOURCES})
add_library(adqt::icons ALIAS ant_design_icons_qt)
target_include_directories(ant_design_icons_qt SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_icons_qt/src"
)
target_link_libraries(ant_design_icons_qt PUBLIC adqt::icon_core Qt6::Widgets Qt6::Svg)

file(GLOB_RECURSE ADQT_WIDGET_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_qt/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_qt/src/*.h"
)
add_library(ant_design_qt STATIC ${ADQT_WIDGET_SOURCES})
if(APPLE)
    enable_language(OBJCXX)
    target_sources(ant_design_qt PRIVATE packages/ant_design_qt/src/widgets/modal_mac.mm)
    target_link_libraries(ant_design_qt PRIVATE "-framework AppKit")
endif()
add_library(adqt::widgets ALIAS ant_design_qt)
target_include_directories(ant_design_qt SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_qt/src"
)
target_link_libraries(ant_design_qt PUBLIC adqt::icon_core adqt::icons
    Qt6::Widgets Qt6::Svg Qt6::Network snow_image::snow_image_static)
target_precompile_headers(ant_design_qt PRIVATE
    <algorithm>
    <memory>
    <optional>
    <string>
    <vector>
    <QtCore/QObject>
    <QtCore/QString>
    <QtGui/QColor>
    <QtGui/QImage>
    <QtGui/QPainter>
    <QtWidgets/QWidget>
)
# Widget sources reuse common anonymous-namespace helper names extensively,
# which makes this target unsafe to combine into unity translation units.

set(QT_I18N_SOURCE_LANGUAGE en_US)
file(GLOB ADQT_TS_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_qt/i18n/ant_design_qt_*.ts"
)
list(SORT ADQT_TS_FILES)
qt_add_translations(
    ant_design_qt
    TS_FILES ${ADQT_TS_FILES}
    SOURCE_TARGETS ant_design_qt
    RESOURCE_PREFIX "/ant_design_qt/i18n"
    LUPDATE_TARGET ant_design_qt_update_translations
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_TARGET ant_design_qt_release_translations
    LRELEASE_OPTIONS
)

if(WIN32)
    target_compile_definitions(ant_design_qt PRIVATE NOMINMAX)
    target_link_libraries(ant_design_qt PUBLIC comctl32 dwmapi user32)
endif()
if(MSVC)
    target_compile_options(adqt_icon_core PRIVATE /utf-8)
    target_compile_options(ant_design_icons_qt PRIVATE /utf-8)
    target_compile_options(ant_design_qt PRIVATE /utf-8)
endif()

file(GLOB ADQT_THEME_DEMO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/theme-demo/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/theme-demo/*.h"
)
add_executable(theme-demo WIN32 EXCLUDE_FROM_ALL ${ADQT_THEME_DEMO_SOURCES})
target_link_libraries(theme-demo PRIVATE adqt::widgets Qt6::Widgets)
if(MSVC)
    target_compile_options(theme-demo PRIVATE /utf-8)
    target_precompile_headers(theme-demo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/theme-demo/theme_demo_pch.h"
    )
endif()

if(ADQT_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    # Test binaries must report failures as stderr output plus a non-zero
    # exit code instead of blocking on modal dialogs.
    snow_link_test_cli_guard(qt)
    add_executable(adqt-widget-diagnostics-tests tests/widget_diagnostics_tests.cpp)
    target_link_libraries(adqt-widget-diagnostics-tests PRIVATE adqt::widgets Qt6::Widgets)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-widget-diagnostics-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-widget-diagnostics-tests COMMAND adqt-widget-diagnostics-tests)
    set_tests_properties(adqt-widget-diagnostics-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    add_executable(adqt-radio-rendering-tests tests/radio_rendering_tests.cpp)
    target_link_libraries(adqt-radio-rendering-tests PRIVATE adqt::widgets Qt6::Widgets)
    get_target_property(_adqt_radio_qt_core_type Qt6::Core TYPE)
    if(_adqt_radio_qt_core_type STREQUAL "STATIC_LIBRARY" AND TARGET Qt6::QOffscreenIntegrationPlugin)
        target_link_libraries(adqt-radio-rendering-tests PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-radio-rendering-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-radio-rendering-tests COMMAND adqt-radio-rendering-tests)
    set_tests_properties(adqt-radio-rendering-tests PROPERTIES
        LABELS "unit"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(adqt-button-lifetime-tests tests/button_lifetime_tests.cpp)
    target_link_libraries(adqt-button-lifetime-tests PRIVATE adqt::widgets Qt6::Widgets)
    get_target_property(_adqt_button_qt_core_type Qt6::Core TYPE)
    if(_adqt_button_qt_core_type STREQUAL "STATIC_LIBRARY" AND TARGET Qt6::QOffscreenIntegrationPlugin)
        target_link_libraries(adqt-button-lifetime-tests PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-button-lifetime-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-button-lifetime-tests COMMAND adqt-button-lifetime-tests)
    set_tests_properties(adqt-button-lifetime-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(adqt-icon-cache-tests tests/icon_cache_tests.cpp)
    target_link_libraries(adqt-icon-cache-tests PRIVATE adqt::icon_core Qt6::Widgets)
    add_test(NAME adqt-icon-cache-tests COMMAND adqt-icon-cache-tests)
    if(WIN32)
        set_tests_properties(adqt-icon-cache-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(adqt-icon-cache-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    add_executable(adqt-icon-core-tests tests/icon_core_tests.cpp)
    target_link_libraries(adqt-icon-core-tests PRIVATE adqt::widgets Qt6::Widgets)
    add_test(NAME adqt-icon-core-tests COMMAND adqt-icon-core-tests)
    if(WIN32)
        set_tests_properties(adqt-icon-core-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(adqt-icon-core-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
    add_executable(adqt-image-tests packages/ant_design_qt/tests/tst_image.cpp)
    set_target_properties(adqt-image-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-image-tests PRIVATE adqt::widgets Qt6::Test)
    add_test(NAME adqt-image-tests COMMAND adqt-image-tests)
    if(WIN32)
        set_tests_properties(adqt-image-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(adqt-image-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    add_executable(adqt-select-tests packages/ant_design_qt/tests/select_tests.cpp)
    set_target_properties(adqt-select-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-select-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        target_link_libraries(adqt-select-tests PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-select-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-select-tests COMMAND adqt-select-tests)
    set_tests_properties(adqt-select-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-spin-tests packages/ant_design_qt/tests/tst_spin.cpp)
    set_target_properties(adqt-spin-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-spin-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        target_link_libraries(adqt-spin-tests PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-spin-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-spin-tests COMMAND adqt-spin-tests)
    set_tests_properties(adqt-spin-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-pagination-tests packages/ant_design_qt/tests/tst_pagination.cpp)
    set_target_properties(adqt-pagination-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-pagination-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-pagination-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-pagination-tests COMMAND adqt-pagination-tests)
    set_tests_properties(adqt-pagination-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-tabs-tests packages/ant_design_qt/tests/tst_tabs.cpp)
    set_target_properties(adqt-tabs-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-tabs-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-tabs-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-tabs-tests COMMAND adqt-tabs-tests)
    set_tests_properties(adqt-tabs-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-modal-window-tests packages/ant_design_qt/tests/tst_modal_window.cpp)
    if(APPLE)
        set_source_files_properties(packages/ant_design_qt/tests/tst_modal_window.cpp
            PROPERTIES LANGUAGE OBJCXX)
        target_link_libraries(adqt-modal-window-tests PRIVATE "-framework AppKit")
    endif()
    set_target_properties(adqt-modal-window-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-modal-window-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-modal-window-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-modal-window-tests COMMAND adqt-modal-window-tests)
    if(WIN32)
        set_tests_properties(adqt-modal-window-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(adqt-modal-window-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    add_executable(adqt-segmented-tests packages/ant_design_qt/tests/tst_segmented.cpp)
    set_target_properties(adqt-segmented-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-segmented-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-segmented-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-segmented-tests COMMAND adqt-segmented-tests)
    set_tests_properties(adqt-segmented-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-scroll-area-tests packages/ant_design_qt/tests/tst_scroll_area.cpp)
    set_target_properties(adqt-scroll-area-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-scroll-area-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-scroll-area-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-scroll-area-tests COMMAND adqt-scroll-area-tests)
    set_tests_properties(adqt-scroll-area-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-timing-hub-tests tests/timing_hub_tests.cpp)
    target_link_libraries(adqt-timing-hub-tests PRIVATE adqt::widgets Qt6::Widgets)
    add_test(NAME adqt-timing-hub-tests COMMAND adqt-timing-hub-tests)
    set_tests_properties(adqt-timing-hub-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-qt-tool-popup-tests
        packages/ant_design_qt/tests/qt_tool_popup_tests.cpp)
    set_target_properties(adqt-qt-tool-popup-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-qt-tool-popup-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        target_link_libraries(adqt-qt-tool-popup-tests PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-qt-tool-popup-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-select-popup-lifetime-tests
        COMMAND adqt-qt-tool-popup-tests selectSurvivesPopupContainerDestructionOrder
                -platform offscreen)
    add_test(NAME adqt-popup-hover-recovery-tests
        COMMAND adqt-qt-tool-popup-tests siblingPopoversReopenAfterOverdueHoverTasks
                siblingPopoversReopenAfterScopeRecreation -platform offscreen)
    set_tests_properties(adqt-popup-hover-recovery-tests PROPERTIES LABELS "unit")
    add_test(NAME adqt-popover-content-lifecycle-tests
        COMMAND adqt-qt-tool-popup-tests retainedFactoryContentIsLazyAndOwnerBound
                recreateFactoryContentIsReleasedAfterHide
                directContentRemainsCompatibleWithFactoryApi
                colorPickerPrewarmCanBeDisabled -platform offscreen)
    set_tests_properties(adqt-popover-content-lifecycle-tests PROPERTIES LABELS "unit")
    add_executable(adqt-popup-hover-geometry-tests tests/popup_hover_geometry_tests.cpp)
    target_link_libraries(adqt-popup-hover-geometry-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        target_link_libraries(adqt-popup-hover-geometry-tests PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-popup-hover-geometry-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-popup-hover-geometry-tests COMMAND adqt-popup-hover-geometry-tests)
    set_tests_properties(adqt-popup-hover-geometry-tests PROPERTIES LABELS "unit")
    add_test(NAME adqt-qt-tool-popup-tests COMMAND adqt-qt-tool-popup-tests)
    if(WIN32)
        set_tests_properties(adqt-qt-tool-popup-tests PROPERTIES
            # Native popup windows only settle reliably on an interactive
            # desktop; the popup logic itself is platform-neutral, so the
            # suite runs offscreen like its sibling recovery entry.
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(adqt-qt-tool-popup-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    add_executable(adqt-date-picker-performance-tests
        packages/ant_design_qt/tests/tst_date_picker_performance.cpp)
    set_target_properties(adqt-date-picker-performance-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-date-picker-performance-tests PRIVATE adqt::widgets Qt6::Test)
    add_test(NAME adqt-date-picker-performance-tests COMMAND adqt-date-picker-performance-tests)
    set_tests_properties(adqt-date-picker-performance-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-locale-manager-tests
        packages/ant_design_qt/tests/locale_manager_tests.cpp)
    target_link_libraries(adqt-locale-manager-tests PRIVATE adqt::widgets Qt6::Widgets)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-locale-manager-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-locale-manager-tests COMMAND adqt-locale-manager-tests)
    set_tests_properties(adqt-locale-manager-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-theme-typography-tests
        packages/ant_design_qt/tests/theme_typography_tests.cpp)
    target_link_libraries(adqt-theme-typography-tests PRIVATE adqt::widgets Qt6::Widgets)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        target_link_libraries(adqt-theme-typography-tests PRIVATE Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-theme-typography-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-theme-typography-tests COMMAND adqt-theme-typography-tests)
    set_tests_properties(adqt-theme-typography-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-divider-tests packages/ant_design_qt/tests/tst_divider.cpp)
    set_target_properties(adqt-divider-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-divider-tests PRIVATE adqt::widgets Qt6::Test)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(adqt-divider-tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
    add_test(NAME adqt-divider-tests COMMAND adqt-divider-tests)
    set_tests_properties(adqt-divider-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(ADQT_BUILD_BENCHMARKS)
    add_executable(adqt-date-picker-perf tools/date-picker-perf/main.cpp)
    target_link_libraries(adqt-date-picker-perf PRIVATE adqt::widgets Qt6::Widgets)
endif()
