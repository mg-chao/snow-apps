if(NOT APPLE)
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

endif()

if(APPLE)
    set(_snow_update_source src/update/macosupdateservice.cpp)
else()
    set(_snow_update_source src/update/updateservice.cpp)
endif()

add_library(snow_shot_updates STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include/snow_shot/update/updateservice.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/snow_shot/update/startupupdate.h"
    ${_snow_update_source}
    src/update/startupupdate.cpp
    # This file is an extraction-only inventory for stable translated Rust error messages.
    src/update/updateerrors.cpp)
target_include_directories(snow_shot_updates PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_updates PUBLIC Qt6::Core)
if(APPLE)
    target_link_libraries(snow_shot_updates PRIVATE Qt6::Network)
    target_compile_definitions(snow_shot_updates PRIVATE SNOW_SHOT_VERSION="${SNOW_SHOT_VERSION}")
endif()
target_link_libraries(snow_shot_settings PUBLIC snow_shot_updates)

target_link_libraries(snow_shot PRIVATE snow_shot_updates)
if(APPLE)
    # An incremental build may still contain the helper copied by an older configuration.
    add_custom_command(TARGET snow_shot POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:snow_shot>/snow-shot-updater")
endif()
if(NOT APPLE)
    add_dependencies(snow_shot snow-shot-updater)
    add_custom_command(TARGET snow_shot POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:snow-shot-updater-binary>"
            "$<TARGET_FILE_DIR:snow_shot>/$<TARGET_FILE_NAME:snow-shot-updater-binary>")

    install(PROGRAMS "$<TARGET_FILE:snow-shot-updater-binary>"
        DESTINATION bin COMPONENT SnowShot)
endif()

option(SNOW_SHOT_BUILD_UPDATE_TESTS "Build focused update tests without unrelated test targets"
    ${SNOW_SHOT_BUILD_TESTS})
if(SNOW_SHOT_BUILD_UPDATE_TESTS)
    enable_testing()
    if(APPLE)
        add_executable(snow-shot-update-adapter-tests tests/macos_update_tests.cpp)
        target_link_libraries(snow-shot-update-adapter-tests PRIVATE Qt6::Network)
    else()
        # The macOS version-check service uses portable Qt networking. Exercise it on Windows
        # as well, without linking the Windows helper adapter into the same executable.
        add_executable(snow-shot-macos-update-tests tests/macos_update_tests.cpp
            src/update/macosupdateservice.cpp include/snow_shot/update/updateservice.h)
        target_include_directories(snow-shot-macos-update-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-macos-update-tests PRIVATE Qt6::Core Qt6::Network)
        target_compile_definitions(snow-shot-macos-update-tests PRIVATE SNOW_SHOT_VERSION="${SNOW_SHOT_VERSION}")
        add_test(NAME snow-shot-macos-update-tests COMMAND snow-shot-macos-update-tests)
        set_tests_properties(snow-shot-macos-update-tests PROPERTIES LABELS "unit" TIMEOUT 30)
        add_executable(snow-shot-update-adapter-tests tests/update_adapter_tests.cpp)
        add_executable(snow-shot-startup-update-tests tests/startup_update_tests.cpp
            src/app/singleinstancecoordinator.cpp
            include/snow_shot/app/singleinstancecoordinator.h)
        target_link_libraries(snow-shot-startup-update-tests PRIVATE
            snow_shot_updates Qt6::Core Qt6::Network)
        add_test(NAME snow-shot-startup-update-tests COMMAND snow-shot-startup-update-tests)
        set_tests_properties(snow-shot-startup-update-tests PROPERTIES LABELS "unit" TIMEOUT 30)
    endif()
    target_link_libraries(snow-shot-update-adapter-tests PRIVATE snow_shot_updates Qt6::Core)
    add_test(NAME snow-shot-update-adapter-tests COMMAND snow-shot-update-adapter-tests)
    # This process fixture launches a helper for each operation and policy case.
    # Leave headroom for debug process startup while each assertion keeps its own deadline.
    set_tests_properties(snow-shot-update-adapter-tests PROPERTIES LABELS "unit" TIMEOUT 60)
endif()
