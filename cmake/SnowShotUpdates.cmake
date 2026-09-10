# The updater deliberately does not depend on presentation, capture, storage, or Rust.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/resources/update-trusted-keys.json" SNOW_SHOT_UPDATE_KEYS_JSON)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/update/updatekeys.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/updatekeys.h" @ONLY)
add_library(snow_shot_update_core STATIC
    src/update/updatecontract.cpp src/update/updatetransaction.cpp src/update/updateerrors.cpp)
target_include_directories(snow_shot_update_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(snow_shot_update_core PUBLIC Qt6::Core PRIVATE MINIZIP::minizip-ng)
if(WIN32)
    target_compile_definitions(snow_shot_update_core PRIVATE NOMINMAX)
    target_link_libraries(snow_shot_update_core PRIVATE bcrypt advapi32)
endif()
add_executable(snow-shot-updater src/update/updatermain.cpp)
target_link_libraries(snow-shot-updater PRIVATE snow_shot_update_core Qt6::Network)
if(WIN32)
    target_link_libraries(snow-shot-updater PRIVATE shell32 advapi32)
    target_compile_definitions(snow-shot-updater PRIVATE NOMINMAX)
    set_target_properties(snow-shot-updater PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        set_property(TARGET snow-shot-updater PROPERTY qt_no_entrypoint TRUE)
        target_link_options(snow-shot-updater PRIVATE /ENTRY:mainCRTStartup)
        # Keep matching symbols for the independently launched recovery helper, just
        # as for snow_shot. CMake's default Release flags do not generate a PDB.
        target_compile_options(snow-shot-updater PRIVATE $<$<CONFIG:Release>:/Z7>)
        target_link_options(snow-shot-updater PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL>)
    endif()
endif()
install(TARGETS snow-shot-updater RUNTIME DESTINATION bin)
add_library(snow_shot_updates STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include/snow_shot/update/updateservice.h"
    src/update/updateservice.cpp)
target_include_directories(snow_shot_updates PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_updates PUBLIC snow_shot_update_core Qt6::Network)
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
