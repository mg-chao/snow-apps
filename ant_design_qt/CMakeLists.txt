cmake_minimum_required(VERSION 3.22)
project(ant_design_qt VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)

option(ADQT_BUILD_TESTS "Build ant_design_qt icon architecture tests." OFF)
option(ADQT_BUILD_BENCHMARKS "Build ant_design_qt performance benchmarks." OFF)
option(ADQT_STRICT_COMPILE "Treat ant_design_qt compiler warnings as errors." ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ProjectWarnings.cmake")
if(ADQT_STRICT_COMPILE)
    snow_enable_strict_warnings()
endif()
snow_enable_clang_tidy()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network LinguistTools)

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
add_library(adqt::widgets ALIAS ant_design_qt)
target_include_directories(ant_design_qt SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_qt/src"
)
target_link_libraries(ant_design_qt PUBLIC adqt::icon_core adqt::icons
    Qt6::Widgets Qt6::Svg Qt6::Network)
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

if(ADQT_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
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

    add_executable(adqt-icon-core-v2-tests tests/icon_core_v2_tests.cpp)
    target_link_libraries(adqt-icon-core-v2-tests PRIVATE adqt::widgets Qt6::Widgets)
    add_test(NAME adqt-icon-core-v2-tests COMMAND adqt-icon-core-v2-tests)
    if(WIN32)
        set_tests_properties(adqt-icon-core-v2-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(adqt-icon-core-v2-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
    add_executable(adqt-image-tests packages/ant_design_qt/tests/tst_image.cpp)
    set_target_properties(adqt-image-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-image-tests PRIVATE adqt::widgets Qt6::Test)
    add_test(NAME adqt-image-tests COMMAND adqt-image-tests)
    set_tests_properties(adqt-image-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-pagination-tests packages/ant_design_qt/tests/tst_pagination.cpp)
    set_target_properties(adqt-pagination-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-pagination-tests PRIVATE adqt::widgets Qt6::Test)
    add_test(NAME adqt-pagination-tests COMMAND adqt-pagination-tests)
    set_tests_properties(adqt-pagination-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-tabs-tests packages/ant_design_qt/tests/tst_tabs.cpp)
    set_target_properties(adqt-tabs-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-tabs-tests PRIVATE adqt::widgets Qt6::Test)
    add_test(NAME adqt-tabs-tests COMMAND adqt-tabs-tests)
    set_tests_properties(adqt-tabs-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-segmented-tests packages/ant_design_qt/tests/tst_segmented.cpp)
    set_target_properties(adqt-segmented-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-segmented-tests PRIVATE adqt::widgets Qt6::Test)
    add_test(NAME adqt-segmented-tests COMMAND adqt-segmented-tests)
    set_tests_properties(adqt-segmented-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-scroll-area-tests packages/ant_design_qt/tests/tst_scroll_area.cpp)
    set_target_properties(adqt-scroll-area-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-scroll-area-tests PRIVATE adqt::widgets Qt6::Test)
    add_test(NAME adqt-scroll-area-tests COMMAND adqt-scroll-area-tests)
    set_tests_properties(adqt-scroll-area-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
    add_test(NAME adqt-locale-manager-tests COMMAND adqt-locale-manager-tests)
    set_tests_properties(adqt-locale-manager-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(adqt-divider-tests packages/ant_design_qt/tests/tst_divider.cpp)
    set_target_properties(adqt-divider-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(adqt-divider-tests PRIVATE adqt::widgets Qt6::Test)
    add_test(NAME adqt-divider-tests COMMAND adqt-divider-tests)
    set_tests_properties(adqt-divider-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(ADQT_BUILD_BENCHMARKS)
    add_executable(adqt-date-picker-perf tools/date-picker-perf/main.cpp)
    target_link_libraries(adqt-date-picker-perf PRIVATE adqt::widgets Qt6::Widgets)
endif()
