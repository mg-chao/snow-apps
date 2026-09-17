# The update helper and the release contract live in the pure-Rust snow-updater
# crate. The application consumes the contract through the Rust FFI archive, so
# there is exactly one verifier implementation on both sides of the handoff.
snow_add_rust_executable(snow-shot-updater
    PACKAGE snow-updater
    MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
    OUTPUT_NAME snow-shot-updater
    PROFILE release-size
    REPRODUCIBLE
    ENVIRONMENT SNOW_SHOT_UPDATER_VERSION=${SNOW_SHOT_VERSION})
add_library(snow_shot_update_core STATIC
    src/update/updateffi.cpp src/update/updateerrors.cpp)
target_include_directories(snow_shot_update_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_update_core PUBLIC Qt6::Core snow_shot_rust_ffi_bundle)
if(WIN32)
    target_compile_definitions(snow_shot_update_core PRIVATE NOMINMAX)
endif()
install(FILES "$<TARGET_FILE:snow-shot-updater>" DESTINATION bin)
add_library(snow_shot_updates STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include/snow_shot/update/updateservice.h"
    src/update/updateservice.cpp)
target_include_directories(snow_shot_updates PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_updates PUBLIC snow_shot_update_core Qt6::Network snow_shot_administrator)
target_link_libraries(snow_shot_settings PUBLIC snow_shot_updates)
add_dependencies(snow_shot snow-shot-updater)
snow_shot_import_offscreen_platform(snow_shot)
target_link_libraries(snow_shot PRIVATE snow_shot_updates)
add_custom_command(TARGET snow_shot POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:snow-shot-updater>"
        "$<TARGET_FILE_DIR:snow_shot>/snow-shot-updater.exe")

option(SNOW_SHOT_BUILD_UPDATE_TESTS "Build focused update tests without unrelated test targets" ${SNOW_SHOT_BUILD_TESTS})
if(SNOW_SHOT_BUILD_UPDATE_TESTS AND WIN32)
    enable_testing()
    add_executable(snow-shot-update-tests tests/update_tests.cpp)
    target_compile_definitions(snow-shot-update-tests PRIVATE NOMINMAX)
    target_link_libraries(snow-shot-update-tests PRIVATE snow_shot_updates MINIZIP::minizip-ng bcrypt)
    add_test(NAME snow-shot-update-tests COMMAND snow-shot-update-tests)
    set_tests_properties(snow-shot-update-tests PROPERTIES LABELS "unit;windows" TIMEOUT 120)
endif()
