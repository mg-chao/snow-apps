if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SnowTestCliGuard.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SnowTestCliGuard.cmake")
    # Keep failure reports on stderr instead of modal dialogs; the native
    # guard variant adds no Qt dependency.
    snow_link_test_cli_guard(native)
endif()

add_executable(snow_image_core_tests core_tests.cpp)
target_link_libraries(snow_image_core_tests PRIVATE snow_image::snow_image)
target_compile_features(snow_image_core_tests PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(snow_image_core_tests PRIVATE /utf-8 /permissive- /Zc:__cplusplus /W4)
else()
    target_compile_options(snow_image_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME snow_image_core_tests COMMAND snow_image_core_tests)
