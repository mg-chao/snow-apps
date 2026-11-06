if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SnowTestCliGuard.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/SnowTestCliGuard.cmake")
    # Keep failure reports on stderr instead of modal dialogs; the native
    # guard variant adds no Qt dependency.
    snow_link_test_cli_guard(native)
endif()

add_executable(snow_image_core_tests core_tests.cpp)
target_link_libraries(snow_image_core_tests PRIVATE snow_image::snow_image)
if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
    # The workspace's static Qt executable supplies bundled codec symbols,
    # but this non-Qt test executable still needs explicit codec archives.
    target_link_libraries(snow_image_core_tests PRIVATE PNG::PNG)
endif()
target_compile_features(snow_image_core_tests PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(snow_image_core_tests PRIVATE /utf-8 /permissive- /Zc:__cplusplus /W4)
else()
    target_compile_options(snow_image_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME snow_image_core_tests COMMAND snow_image_core_tests)
add_test(NAME snow_image_raster_store_tests COMMAND snow_image_core_tests --raster-stores-only)
add_test(NAME snow_image_alpha_tests COMMAND snow_image_core_tests --alpha-only)
add_test(NAME snow_image_crc32c_tests COMMAND snow_image_core_tests --crc32c-only)
add_test(NAME snow_image_pixel_ownership_tests COMMAND snow_image_core_tests --ownership-only)
add_test(NAME snow_image_buffer_paths_tests COMMAND snow_image_core_tests --buffer-paths-only)
add_test(NAME snow_image_processing_tests COMMAND snow_image_core_tests --processing-only)
add_test(NAME snow_image_resize_tests COMMAND snow_image_core_tests --resize-only)
add_test(NAME snow_image_heif_tests COMMAND snow_image_core_tests --heif-only)
add_test(NAME snow_image_jpeg_tests COMMAND snow_image_core_tests --jpeg-only)
add_test(NAME snow_image_jxl_tests COMMAND snow_image_core_tests --jxl-only)
add_test(NAME snow_image_png_tests COMMAND snow_image_core_tests --png-only)
add_test(NAME snow_image_webp_tests COMMAND snow_image_core_tests --webp-only)

add_executable(snow_image_input_bytes_tests input_bytes_tests.cpp)
target_link_libraries(snow_image_input_bytes_tests PRIVATE snow_image::snow_image)
if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
    target_link_libraries(snow_image_input_bytes_tests PRIVATE PNG::PNG)
endif()
target_compile_features(snow_image_input_bytes_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(snow_image_input_bytes_tests PRIVATE /utf-8 /permissive- /Zc:__cplusplus /W4)
else()
    target_compile_options(snow_image_input_bytes_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME snow_image_input_bytes_tests COMMAND snow_image_input_bytes_tests)

if(SNOW_IMAGE_CODEC_TIFF)
    add_executable(snow_image_tiff_tests tiff_tests.cpp)
    target_link_libraries(snow_image_tiff_tests PRIVATE snow_image::snow_image TIFF::TIFF)
    if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
        target_link_libraries(snow_image_tiff_tests PRIVATE PNG::PNG)
    endif()
    target_compile_features(snow_image_tiff_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(snow_image_tiff_tests PRIVATE /utf-8 /permissive- /Zc:__cplusplus /W4)
    else()
        target_compile_options(snow_image_tiff_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snow_image_tiff_tests COMMAND snow_image_tiff_tests)
endif()

add_executable(snow_image_animation_tests animation_tests.cpp)
target_link_libraries(snow_image_animation_tests PRIVATE snow_image::snow_image)
if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
    target_link_libraries(snow_image_animation_tests PRIVATE PNG::PNG)
endif()
target_compile_features(snow_image_animation_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(snow_image_animation_tests PRIVATE /utf-8 /permissive- /Zc:__cplusplus /W4)
else()
    target_compile_options(snow_image_animation_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME snow_image_animation_tests COMMAND snow_image_animation_tests)

add_executable(snow_image_raster_conversion_tests raster_conversion_tests.cpp)
target_link_libraries(snow_image_raster_conversion_tests
    PRIVATE snow_image::snow_image ${SNOW_IMAGE_PRIVATE_LIBRARIES})
target_compile_definitions(snow_image_raster_conversion_tests
    PRIVATE ${SNOW_IMAGE_PRIVATE_DEFINITIONS})
target_compile_features(snow_image_raster_conversion_tests PRIVATE cxx_std_20)
add_test(NAME snow_image_raster_conversion_tests COMMAND snow_image_raster_conversion_tests)

add_executable(snow_image_raster_export_tests raster_export_tests.cpp)
target_link_libraries(snow_image_raster_export_tests PRIVATE snow_image::snow_image)
if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
    target_link_libraries(snow_image_raster_export_tests PRIVATE PNG::PNG)
endif()
target_compile_features(snow_image_raster_export_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(snow_image_raster_export_tests PRIVATE
        /utf-8 /permissive- /Zc:__cplusplus /W4
    )
else()
    target_compile_options(snow_image_raster_export_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME snow_image_raster_export_tests COMMAND snow_image_raster_export_tests)
add_test(NAME snow_image_lazy_resize_tests COMMAND snow_image_raster_export_tests --resize-only)

add_executable(snow_image_resource_plan_tests resource_plan_tests.cpp)
target_link_libraries(snow_image_resource_plan_tests PRIVATE snow_image::snow_image)
if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
    target_link_libraries(snow_image_resource_plan_tests PRIVATE PNG::PNG)
endif()
target_compile_features(snow_image_resource_plan_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(snow_image_resource_plan_tests PRIVATE
        /utf-8 /permissive- /Zc:__cplusplus /W4
    )
else()
    target_compile_options(snow_image_resource_plan_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME snow_image_resource_plan_tests COMMAND snow_image_resource_plan_tests)

if(SNOW_IMAGE_CODEC_HEIF)
    add_executable(snow_image_avif_encoding_tests avif_encoding_tests.cpp)
    target_link_libraries(snow_image_avif_encoding_tests PRIVATE snow_image::snow_image heif)
    if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES)
        target_link_libraries(snow_image_avif_encoding_tests PRIVATE PNG::PNG)
    endif()
    target_compile_features(snow_image_avif_encoding_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(snow_image_avif_encoding_tests PRIVATE
            /utf-8 /permissive- /Zc:__cplusplus /W4
        )
    else()
        target_compile_options(snow_image_avif_encoding_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME snow_image_avif_encoding_tests COMMAND snow_image_avif_encoding_tests)

    if(APPLE)
        find_package(AOM CONFIG QUIET)
        if(TARGET AOM::aom)
            get_target_property(_SNOW_IMAGE_AOM_TYPE AOM::aom TYPE)
            if(_SNOW_IMAGE_AOM_TYPE STREQUAL "SHARED_LIBRARY")
                # Interpose the shared dependency only in this offscreen test process.
                # Exact native init/destroy counts detect leaks without RSS thresholds.
                add_library(snow_image_avif_lifecycle_probe SHARED avif_lifecycle_probe.cpp)
                target_link_libraries(snow_image_avif_lifecycle_probe PRIVATE AOM::aom)
                target_compile_features(snow_image_avif_lifecycle_probe PRIVATE cxx_std_20)
                target_compile_options(snow_image_avif_lifecycle_probe PRIVATE
                    -Wall -Wextra -Wpedantic)
                add_executable(snow_image_avif_lifecycle_tests avif_lifecycle_tests.cpp)
                target_link_libraries(snow_image_avif_lifecycle_tests
                    PRIVATE heif snow_image_avif_lifecycle_probe)
                target_compile_features(snow_image_avif_lifecycle_tests PRIVATE cxx_std_20)
                target_compile_options(snow_image_avif_lifecycle_tests PRIVATE
                    -Wall -Wextra -Wpedantic)
                add_test(NAME snow_image_avif_lifecycle_tests
                    COMMAND "${CMAKE_COMMAND}" -E env
                        "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:snow_image_avif_lifecycle_probe>"
                        "$<TARGET_FILE:snow_image_avif_lifecycle_tests>")
            endif()
        endif()
    endif()
endif()

if(MSVC)
    # Regression coverage for the test CLI dialog guard (cmake/test-support):
    # reports raised through the shared dynamic debug CRT instance that
    # /MD-built dependency DLLs use must land on stderr, never on a modal
    # dialog. The trigger DLL is deliberately built against the dynamic CRT,
    # so it must not receive the static-CRT guard objects from this scope.
    get_directory_property(_SNOW_IMAGE_TEST_GUARD_LIBS LINK_LIBRARIES)
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES "")
    add_library(snow_image_test_cli_guard_trigger SHARED test_cli_guard_trigger.cpp)
    set_target_properties(snow_image_test_cli_guard_trigger PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
    set_property(DIRECTORY PROPERTY LINK_LIBRARIES ${_SNOW_IMAGE_TEST_GUARD_LIBS})
    add_executable(snow_image_test_cli_guard_tests test_cli_guard_tests.cpp)
    # Linking the trigger DLL makes the executable load the shared dynamic
    # debug CRT at process start, before the guard's global constructor runs,
    # mirroring production executables that link /MD-built vcpkg codec DLLs.
    target_link_libraries(snow_image_test_cli_guard_tests
        PRIVATE snow_image_test_cli_guard_trigger
    )
    target_compile_features(snow_image_test_cli_guard_tests PRIVATE cxx_std_20)
    target_compile_options(snow_image_test_cli_guard_tests PRIVATE
        /utf-8 /permissive- /Zc:__cplusplus /W4
    )
    add_test(NAME snow_image_test_cli_guard_tests COMMAND snow_image_test_cli_guard_tests)
endif()
