add_executable(snow_image_benchmark codec_benchmark.cpp)
target_link_libraries(snow_image_benchmark PRIVATE snow_image::snow_image)
target_compile_features(snow_image_benchmark PRIVATE cxx_std_20)

add_executable(snow_image_memory_performance_benchmark
    ../tests/memory_performance_benchmark.cpp)
target_link_libraries(snow_image_memory_performance_benchmark PRIVATE snow_image::snow_image)
target_compile_features(snow_image_memory_performance_benchmark PRIVATE cxx_std_20)
if(WIN32)
    target_link_libraries(snow_image_memory_performance_benchmark PRIVATE psapi)
endif()
if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
    target_link_libraries(snow_image_memory_performance_benchmark PRIVATE PNG::PNG)
endif()
