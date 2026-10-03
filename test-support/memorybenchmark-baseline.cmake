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
if(PROJECT_NAME STREQUAL "snow_image")
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
    add_executable(snow-canvas-image-memory-benchmark-baseline
        "${SNOW_MEMORY_BENCHMARK_SOURCE_ROOT}/snow_draw_engine_qt/tests/snow_canvas_image_memory_benchmark.cpp")
    target_compile_definitions(snow-canvas-image-memory-benchmark-baseline
        PRIVATE SNOW_MEMORY_BENCHMARK_BASELINE=1)
    target_link_libraries(snow-canvas-image-memory-benchmark-baseline PRIVATE Qt6::Gui)
    set(_snow_memory_comparison_target snow-canvas-image-memory-benchmark-baseline)
    set(_snow_memory_comparison_standard 17)
else()
    message(FATAL_ERROR "Include this hook only for snow_image and snow_draw_engine_qt.")
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
