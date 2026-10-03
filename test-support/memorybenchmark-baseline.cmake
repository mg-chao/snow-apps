# Add current benchmark sources to an older checkout without modifying its
# tracked files. Include at the named child projects through CMake's
# CMAKE_PROJECT_<project-name>_INCLUDE cache options (see PERFORMANCE-BENCHMARKS.md).
if(NOT SNOW_APPS_BUILD_BENCHMARKS)
    message(FATAL_ERROR "Memory comparisons require a performance preset.")
endif()
if(NOT IS_DIRECTORY "${SNOW_MEMORY_BENCHMARK_SOURCE_ROOT}/snow_memory")
    message(FATAL_ERROR
        "Set SNOW_MEMORY_BENCHMARK_SOURCE_ROOT to the checkout containing the benchmark sources.")
endif()

include("${SNOW_MEMORY_BENCHMARK_SOURCE_ROOT}/cmake/ProjectWarnings.cmake")
if(PROJECT_NAME STREQUAL "snow_shot")
    # The project include runs before Snow Shot declares its codec target and
    # source groups. Defer creation until those production declarations exist.
    function(snow_add_baseline_screenshot_memory_benchmark)
        add_executable(snow-shot-memory-performance-benchmark-baseline
            "${SNOW_MEMORY_BENCHMARK_SOURCE_ROOT}/snow_shot/tests/screenshot_memory_performance_benchmark.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/services/screenshotclipboardcontent.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp"
            ${SNOW_SHOT_RECOGNITION_IMAGE_RENDER_SOURCES})
        target_include_directories(snow-shot-memory-performance-benchmark-baseline PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_compile_definitions(snow-shot-memory-performance-benchmark-baseline PRIVATE
            SNOW_SHOT_BENCHMARK_MANAGED_READER=0)
        target_link_libraries(snow-shot-memory-performance-benchmark-baseline PRIVATE
            adqt::widgets snow_shot_image_codec snow_shot_diagnostics
            snow_shot_clipboard_placement Qt6::Widgets)
        if(WIN32)
            target_link_libraries(snow-shot-memory-performance-benchmark-baseline PRIVATE
                user32 psapi)
        endif()
        snow_shot_import_offscreen_platform(snow-shot-memory-performance-benchmark-baseline)
        snow_apply_strict_warnings(snow-shot-memory-performance-benchmark-baseline)
    endfunction()
    cmake_language(DEFER CALL snow_add_baseline_screenshot_memory_benchmark)
    return()
elseif(PROJECT_NAME STREQUAL "snow_image")
    add_executable(snow-image-memory-performance-benchmark-baseline
        "${SNOW_MEMORY_BENCHMARK_SOURCE_ROOT}/snow_image/tests/memory_performance_benchmark.cpp")
    target_link_libraries(snow-image-memory-performance-benchmark-baseline
        PRIVATE snow_image::snow_image)
    if(SNOW_IMAGE_CODEC_PNG AND NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
        target_link_libraries(snow-image-memory-performance-benchmark-baseline PRIVATE PNG::PNG)
    endif()
    set(_snow_memory_comparison_target snow-image-memory-performance-benchmark-baseline)
    set(_snow_memory_comparison_standard 20)
elseif(PROJECT_NAME STREQUAL "snow_draw_engine_qt")
    function(snow_use_current_render_memory_benchmark_sources)
        # Keep workload lifetimes, output checksums, and timing code identical
        # on both revisions under strict warnings.
        foreach(_kind filter watermark smart_erase)
            string(REPLACE "_" "-" _target_kind "${_kind}")
            set(_target "snow-canvas-${_target_kind}-benchmark")
            if(TARGET "${_target}")
                get_target_property(_sources "${_target}" SOURCES)
                list(REMOVE_ITEM _sources "tests/snow_canvas_${_kind}_benchmark.cpp")
                list(APPEND _sources
                    "${SNOW_MEMORY_BENCHMARK_SOURCE_ROOT}/snow_draw_engine_qt/tests/snow_canvas_${_kind}_benchmark.cpp")
                set_property(TARGET "${_target}" PROPERTY SOURCES "${_sources}")
            endif()
        endforeach()
        # The old standalone geometry source has an unused constexpr removed
        # by the branch. Suppress only that diagnostic without changing code.
        if(APPLE AND TARGET snow-canvas-smart-erase-benchmark)
            target_compile_options(snow-canvas-smart-erase-benchmark PRIVATE
                -Wno-unused-const-variable)
        endif()
    endfunction()
    cmake_language(DEFER CALL snow_use_current_render_memory_benchmark_sources)
    add_executable(snow-canvas-image-memory-benchmark-baseline
        "${SNOW_MEMORY_BENCHMARK_SOURCE_ROOT}/snow_draw_engine_qt/tests/snow_canvas_image_memory_benchmark.cpp")
    target_compile_definitions(snow-canvas-image-memory-benchmark-baseline
        PRIVATE SNOW_MEMORY_BENCHMARK_BASELINE=1)
    target_link_libraries(snow-canvas-image-memory-benchmark-baseline PRIVATE Qt6::Gui)
    set(_snow_memory_comparison_target snow-canvas-image-memory-benchmark-baseline)
    set(_snow_memory_comparison_standard 17)
else()
    message(FATAL_ERROR
        "Include this hook only for snow_image, snow_draw_engine_qt and snow_shot.")
endif()

set_target_properties(${_snow_memory_comparison_target} PROPERTIES
    CXX_STANDARD ${_snow_memory_comparison_standard}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
if(WIN32)
    target_link_libraries(${_snow_memory_comparison_target} PRIVATE psapi)
endif()
snow_apply_strict_warnings(${_snow_memory_comparison_target})
# Inherit the performance preset's flags and IPO policy. Adding another release
# policy here would give the comparison binary different LTO/section flags.
unset(_snow_memory_comparison_target)
unset(_snow_memory_comparison_standard)
