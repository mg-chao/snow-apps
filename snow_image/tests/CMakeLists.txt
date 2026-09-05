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
if(NOT SNOW_IMAGE_LINK_JPEG_DEPENDENCIES)
    target_link_libraries(snow_image_core_tests PRIVATE
        $<IF:$<TARGET_EXISTS:libjpeg-turbo::jpeg>,libjpeg-turbo::jpeg,libjpeg-turbo::jpeg-static>
        $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
    )
endif()
target_compile_features(snow_image_core_tests PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(snow_image_core_tests PRIVATE /utf-8 /permissive- /Zc:__cplusplus /W4)
else()
    target_compile_options(snow_image_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME snow_image_core_tests COMMAND snow_image_core_tests)

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
