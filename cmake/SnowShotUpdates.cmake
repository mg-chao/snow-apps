# The GPL updater is a standalone Rust package inside the Snow Shot license boundary. The main
# application links only the Qt process/protocol adapter below.
set(_snow_shot_updater_manifest_dir
    "${CMAKE_CURRENT_SOURCE_DIR}/rust/snow-shot-updater")
snow_add_rust_executable(snow-shot-updater-binary
    PACKAGE snow-shot-updater
    MANIFEST_DIR "${_snow_shot_updater_manifest_dir}"
    OUTPUT_NAME snow-shot-updater
    PRODUCTION_PROFILE release-size
    REPRODUCIBLE
    SIZE_OPTIMIZED
    ENVIRONMENT
        "SNOW_SHOT_UPDATE_KEYS_PATH=${CMAKE_CURRENT_SOURCE_DIR}/resources/update-trusted-keys.json"
        "SNOW_SHOT_VERSION=${SNOW_SHOT_VERSION}"
        "SNOW_SHOT_ICON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/resources/app-icon.ico"
)
# Imported executable targets do not create build-system targets. Keep the
# public snow-shot-updater target used by scripts and release automation as a
# real proxy for the Cargo build while the imported target supplies its path.
# Also materialize the traditional build-tree output when this target is built
# directly, rather than only as a dependency of snow_shot.
add_custom_target(snow-shot-updater
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:snow_shot>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:snow-shot-updater-binary>"
        "$<TARGET_FILE_DIR:snow_shot>/$<TARGET_FILE_NAME:snow-shot-updater-binary>"
    DEPENDS snow-shot-updater-binary_build
    VERBATIM)

add_library(snow_shot_updates STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include/snow_shot/update/updateservice.h"
    src/update/updateservice.cpp
    # This file is an extraction-only inventory for stable translated Rust error messages.
    src/update/updateerrors.cpp)
target_include_directories(snow_shot_updates PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_updates PUBLIC Qt6::Core)
target_link_libraries(snow_shot_settings PUBLIC snow_shot_updates)

add_dependencies(snow_shot snow-shot-updater)
target_link_libraries(snow_shot PRIVATE snow_shot_updates)
add_custom_command(TARGET snow_shot POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:snow-shot-updater-binary>"
        "$<TARGET_FILE_DIR:snow_shot>/$<TARGET_FILE_NAME:snow-shot-updater-binary>")

if(APPLE)
    install(PROGRAMS "$<TARGET_FILE:snow-shot-updater-binary>"
        DESTINATION "snow_shot.app/Contents/MacOS" COMPONENT SnowShot)
else()
    install(PROGRAMS "$<TARGET_FILE:snow-shot-updater-binary>"
        DESTINATION bin COMPONENT SnowShot)
endif()

option(SNOW_SHOT_BUILD_UPDATE_TESTS "Build focused update tests without unrelated test targets"
    ${SNOW_SHOT_BUILD_TESTS})
if(SNOW_SHOT_BUILD_UPDATE_TESTS)
    enable_testing()
    add_executable(snow-shot-update-adapter-tests tests/update_adapter_tests.cpp)
    target_link_libraries(snow-shot-update-adapter-tests PRIVATE snow_shot_updates Qt6::Core)
    add_test(NAME snow-shot-update-adapter-tests COMMAND snow-shot-update-adapter-tests)
    set_tests_properties(snow-shot-update-adapter-tests PROPERTIES LABELS "unit" TIMEOUT 30)
endif()
