cmake_minimum_required(VERSION 3.22)

project(snow_draw_engine_qt VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/Zc:__cplusplus /permissive-)
endif()
option(SNOW_DRAW_ENGINE_QT_BUILD_DEMO "Build the Snow Draw Engine Qt demo executable." ON)
option(SNOW_DRAW_ENGINE_QT_BUILD_TESTS "Build the Snow Draw Engine Qt test executables." ON)
option(SNOW_DRAW_ENGINE_QT_BUILD_BENCHMARKS "Build opt-in Snow Draw Engine Qt benchmarks." OFF)
option(SNOW_DRAW_ENGINE_QT_STRICT_COMPILE "Treat Snow Draw Engine Qt warnings as errors." ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(RustStaticLibrary)
include(ProjectWarnings)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SnowCanvasQtSources.cmake")
if(SNOW_DRAW_ENGINE_QT_STRICT_COMPILE)
    snow_enable_strict_warnings()
endif()
snow_enable_clang_tidy()

set(CMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders TRUE)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
qt_standard_project_setup()

if(NOT TARGET adqt::icons)
    set(SNOW_DRAW_ENGINE_ADQT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../ant_design_qt"
        CACHE PATH "Path to the ant_design_qt repository")
    if(NOT EXISTS "${SNOW_DRAW_ENGINE_ADQT_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Unable to locate ant_design_qt relative to this project. Set SNOW_DRAW_ENGINE_ADQT_DIR for an external checkout.")
    endif()
    add_subdirectory("${SNOW_DRAW_ENGINE_ADQT_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/ant_design_qt")
endif()

if(NOT TARGET snow_draw_engine_c)
    snow_add_rust_static_library(snow_draw_engine_c
        PACKAGE snow-draw-engine-c
        MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_NAME snow_draw_engine_c
        STRIP_MSVC_DIRECTIVES
    )
endif()

qt_add_library(snow_draw_engine_qt_canvas STATIC
    ${SNOW_CANVAS_QT_SOURCES}
    src/icons/draw_engine_icons.cpp
    src/icons/draw_engine_icons.h
)

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
set_source_files_properties(
        src/rendering/snow_canvas_filter_avx2.cpp
        src/rendering/snow_canvas_pen_mask_avx2.cpp
        PROPERTIES
            COMPILE_OPTIONS "/arch:AVX2"
            SKIP_UNITY_BUILD_INCLUSION ON
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
    set_source_files_properties(
        src/rendering/snow_canvas_filter_avx2.cpp
        src/rendering/snow_canvas_pen_mask_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx2"
    )
endif()

target_include_directories(snow_draw_engine_qt_canvas
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
)

target_link_libraries(snow_draw_engine_qt_canvas
    PUBLIC
        Qt6::Widgets
    PRIVATE
        Qt6::Svg
        adqt::icons
        snow_draw_engine_c
        advapi32
        bcrypt
        ntdll
        shell32
        userenv
        ws2_32
)
target_precompile_headers(snow_draw_engine_qt_canvas PRIVATE
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
snow_enable_unity_build(snow_draw_engine_qt_canvas)

if(SNOW_DRAW_ENGINE_QT_BUILD_DEMO)
    add_subdirectory(qt-demo)
endif()

if(SNOW_DRAW_ENGINE_QT_BUILD_BENCHMARKS)
    add_executable(snow-canvas-rectangle-benchmark
        tests/snow_canvas_rectangle_benchmark.cpp
    )
    target_include_directories(snow-canvas-rectangle-benchmark PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-rectangle-benchmark PRIVATE
        snow_draw_engine_qt_canvas
    )

    add_executable(snow-canvas-filter-benchmark
        tests/snow_canvas_filter_benchmark.cpp
    )
    target_include_directories(snow-canvas-filter-benchmark PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-filter-benchmark PRIVATE
        snow_draw_engine_qt_canvas
    )

    add_executable(snow-canvas-pen-filter-pipeline-benchmark
        tests/snow_canvas_pen_filter_pipeline_benchmark.cpp
    )
    target_include_directories(snow-canvas-pen-filter-pipeline-benchmark PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-pen-filter-pipeline-benchmark PRIVATE
        snow_draw_engine_qt_canvas
    )

    add_executable(snow-canvas-watermark-benchmark
        tests/snow_canvas_watermark_benchmark.cpp
    )
    target_include_directories(snow-canvas-watermark-benchmark PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-watermark-benchmark PRIVATE
        snow_draw_engine_qt_canvas
    )

    add_executable(snow-canvas-spotlight-benchmark
        tests/snow_canvas_spotlight_benchmark.cpp
    )
    target_include_directories(snow-canvas-spotlight-benchmark PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-spotlight-benchmark PRIVATE
        snow_draw_engine_qt_canvas
    )
endif()

if(SNOW_DRAW_ENGINE_QT_BUILD_TESTS)
    enable_testing()

    add_executable(snow-canvas-text-draft-tests
        tests/snow_canvas_text_draft_tests.cpp
    )
    target_include_directories(snow-canvas-text-draft-tests PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/qt-demo"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-text-draft-tests PRIVATE
        snow_draw_engine_qt_canvas
    )
    add_test(NAME snow-canvas-text-draft-tests COMMAND snow-canvas-text-draft-tests)
    set_tests_properties(snow-canvas-text-draft-tests PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-canvas-custom-renderer-tests
        tests/snow_canvas_custom_renderer_tests.cpp
    )
    target_include_directories(snow-canvas-custom-renderer-tests PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-custom-renderer-tests PRIVATE
        snow_draw_engine_qt_canvas
        adqt::icons
        Qt6::Svg
    )
    add_test(
        NAME snow-canvas-custom-renderer-tests
        COMMAND snow-canvas-custom-renderer-tests
    )
    if(WIN32)
        set_tests_properties(snow-canvas-custom-renderer-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    else()
        set_tests_properties(snow-canvas-custom-renderer-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(snow-canvas-state-tests
        tests/snow_canvas_state_tests.cpp
    )
    target_include_directories(snow-canvas-state-tests PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-state-tests PRIVATE
        snow_draw_engine_qt_canvas
    )
    add_test(NAME snow-canvas-state-tests COMMAND snow-canvas-state-tests)

    add_executable(snow-canvas-filter-render-tests
        tests/snow_canvas_filter_render_tests.cpp
    )
    target_include_directories(snow-canvas-filter-render-tests PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-filter-render-tests PRIVATE
        snow_draw_engine_qt_canvas
    )
    add_test(NAME snow-canvas-filter-render-tests COMMAND snow-canvas-filter-render-tests)
    set_tests_properties(snow-canvas-filter-render-tests PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(snow-canvas-spotlight-render-tests
        tests/snow_canvas_spotlight_render_tests.cpp
    )
    target_include_directories(snow-canvas-spotlight-render-tests PRIVATE
        ${SNOW_CANVAS_QT_PRIVATE_SOURCE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/snow-draw-engine-c/include"
    )
    target_link_libraries(snow-canvas-spotlight-render-tests PRIVATE
        snow_draw_engine_qt_canvas
    )
    add_test(
        NAME snow-canvas-spotlight-render-tests
        COMMAND snow-canvas-spotlight-render-tests
    )
    set_tests_properties(snow-canvas-spotlight-render-tests PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
endif()
