# Keep production-kit acceptance independent of QtTest and unrelated UI tests.
# General and focused modes share these declarations and their runtime contracts.
function(snow_shot_add_diagnostics_crash_tests)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-bin")
    if(WIN32 OR APPLE)
        add_executable(snow-shot-diagnostics-crash-tests tests/diagnostics_crash_tests.cpp)
        snow_add_rust_static_library(snow_diagnostics_test
            PACKAGE snow-diagnostics-test
            MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
            OUTPUT_NAME snow_diagnostics_test
            STRIP_MSVC_DIRECTIVES)
        target_link_libraries(snow-shot-diagnostics-crash-tests PRIVATE snow_diagnostics_test)
        target_link_libraries(snow-shot-diagnostics-crash-tests PRIVATE snow_shot_diagnostics snow_shot_crash_bridge)
        if(APPLE)
            set_property(TARGET snow-shot-diagnostics-crash-tests PROPERTY
                SNOW_RUST_BUNDLE_OVERRIDE snow_diagnostics_test)
        endif()
        target_compile_definitions(snow-shot-diagnostics-crash-tests PRIVATE
            SNOW_TEST_CRASHPAD_HANDLER="${SNOW_CRASHPAD_HANDLER}")
        add_test(NAME snow-shot-diagnostics-crash-tests COMMAND snow-shot-diagnostics-crash-tests)
        set_tests_properties(snow-shot-diagnostics-crash-tests PROPERTIES LABELS "unit;crash" TIMEOUT 180)
        if(APPLE)
            set_property(TEST snow-shot-diagnostics-crash-tests APPEND PROPERTY LABELS macos)
        else()
            set_property(TEST snow-shot-diagnostics-crash-tests APPEND PROPERTY LABELS windows)
        endif()
    endif()
endfunction()

function(snow_shot_add_ocr_runtime_tests)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-bin")
    add_executable(snow-shot-ocr-recognition-service-tests
        tests/screenshot_ocr_recognition_service_tests.cpp
        tests/screenshot_ocr_process_lifecycle_tests.cpp
        include/snow_shot/presentation/screenshotocrpresentation.h
        include/snow_shot/presentation/screenshotocrrecognitionservice.h
        include/snow_shot/presentation/screenshotocrassets.h
        src/presentation/ocr/screenshotocrpresentation.cpp
        src/presentation/ocr/screenshotocrrecognitionservice.cpp
        src/presentation/ocr/screenshotocrassets.cpp
        src/presentation/ocr/screenshotocrvisuals.cpp
    )
    target_include_directories(snow-shot-ocr-recognition-service-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${SNOW_DRAW_ENGINE_SOURCE_DIR}/include"
    )
    target_compile_definitions(snow-shot-ocr-recognition-service-tests PRIVATE
        SNOW_TEST_OCR_EXECUTABLE="$<TARGET_FILE:snow_ocr_process>")
    if(APPLE AND SNOW_SHOT_OCR_STATIC_ONNXRUNTIME)
        # The test host compiles the same asset parser and consumes the worker's
        # generated manifest, so its runtime linkage contract must match the app.
        target_compile_definitions(snow-shot-ocr-recognition-service-tests PRIVATE
            SNOW_SHOT_OCR_STATIC_ONNXRUNTIME=1)
    endif()
    if(SNOW_SHOT_BUILD_BENCHMARKS AND CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_definitions(snow-shot-ocr-recognition-service-tests PRIVATE SNOW_TEST_OCR_PERFORMANCE=1)
    endif()
    target_link_libraries(snow-shot-ocr-recognition-service-tests PRIVATE
        snow_shot_diagnostics
        snow_draw_engine_qt_canvas
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        MINIZIP::minizip-ng
    )
    if(WIN32)
        target_link_libraries(snow-shot-ocr-recognition-service-tests PRIVATE
            bcrypt
            ntdll
        )
        target_compile_definitions(snow-shot-ocr-recognition-service-tests PRIVATE
            SNOW_TEST_CRASHPAD_HANDLER="${SNOW_CRASHPAD_HANDLER}")
        add_test(NAME snow-shot-ocr-crash-tests COMMAND snow-shot-ocr-recognition-service-tests --native-crash)
        set_tests_properties(snow-shot-ocr-crash-tests PROPERTIES TIMEOUT 60 LABELS "unit;windows")
    endif()
    add_test(
        NAME snow-shot-ocr-cpu-recognition-service-tests
        COMMAND snow-shot-ocr-recognition-service-tests
    )
    add_test(
        NAME snow-shot-ocr-directml-recognition-service-tests
        COMMAND snow-shot-ocr-recognition-service-tests --directml
    )
    set_tests_properties(snow-shot-ocr-cpu-recognition-service-tests PROPERTIES
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    set_tests_properties(snow-shot-ocr-directml-recognition-service-tests PROPERTIES
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
    add_test(NAME snow-shot-ocr-storage-relocation-tests
        COMMAND snow-shot-ocr-recognition-service-tests --storage-relocation-only)
    set_tests_properties(snow-shot-ocr-storage-relocation-tests PROPERTIES LABELS "unit" TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME snow-shot-ocr-process-lifecycle-tests
        COMMAND snow-shot-ocr-recognition-service-tests --process-lifecycle)
    add_executable(snow-shot-ocr-assets-tests
        tests/screenshot_ocr_assets_tests.cpp
        include/snow_shot/presentation/screenshotocrassets.h
        src/presentation/ocr/screenshotocrassets.cpp)
    target_include_directories(snow-shot-ocr-assets-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(snow-shot-ocr-assets-tests PRIVATE Qt6::Core Qt6::Network MINIZIP::minizip-ng)
    if(APPLE)
        target_link_libraries(snow-shot-ocr-assets-tests PRIVATE snow_shot_rust_ffi_selector)
    endif()
    add_test(NAME snow-shot-ocr-assets-tests COMMAND snow-shot-ocr-assets-tests)
    set_tests_properties(snow-shot-ocr-assets-tests PROPERTIES TIMEOUT 60 LABELS "unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(WIN32 AND SNOW_WINDOWS_ARCHITECTURE STREQUAL "x64")
        add_executable(snow-shot-ocr-assets-arm64-contract-tests
            tests/screenshot_ocr_assets_arm64_tests.cpp
            tests/screenshot_ocr_assets_arm64_fixture.cpp
            include/snow_shot/presentation/screenshotocrassets.h)
        target_include_directories(snow-shot-ocr-assets-arm64-contract-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(snow-shot-ocr-assets-arm64-contract-tests PRIVATE
            Qt6::Core Qt6::Network MINIZIP::minizip-ng)
        add_test(NAME snow-shot-ocr-assets-arm64-contract-tests
            COMMAND snow-shot-ocr-assets-arm64-contract-tests)
        set_tests_properties(snow-shot-ocr-assets-arm64-contract-tests PROPERTIES
            TIMEOUT 60 LABELS "unit;windows"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    add_test(NAME snow-shot-ocr-managed-runtime-tests
        COMMAND snow-shot-ocr-recognition-service-tests --managed-runtime-only)
    add_test(NAME snow-shot-ocr-managed-directml-runtime-tests
        COMMAND snow-shot-ocr-recognition-service-tests --managed-runtime-only --directml)
    set_tests_properties(snow-shot-ocr-managed-runtime-tests
        snow-shot-ocr-managed-directml-runtime-tests PROPERTIES
        TIMEOUT 60 LABELS "unit;windows"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    if(APPLE)
        set_tests_properties(snow-shot-ocr-managed-runtime-tests PROPERTIES LABELS "unit")
        set_tests_properties(snow-shot-ocr-directml-recognition-service-tests PROPERTIES LABELS "unit;windows")
    endif()
    set_tests_properties(snow-shot-ocr-process-lifecycle-tests PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_dependencies(snow-shot-ocr-recognition-service-tests snow_ocr_process)
    if(NOT SNOW_SHOT_BUILD_TESTS)
        snow_add_qt_test_cli_guard()
        foreach(_runtime_test IN ITEMS snow-shot-ocr-recognition-service-tests
                snow-shot-ocr-assets-tests snow-shot-ocr-assets-arm64-contract-tests)
            if(TARGET ${_runtime_test})
                target_link_libraries(${_runtime_test} PRIVATE snow_test_cli_guard_qt)
            endif()
        endforeach()
    endif()
endfunction()

if(SNOW_SHOT_BUILD_RUNTIME_TESTS AND NOT SNOW_SHOT_BUILD_TESTS)
    enable_testing()
    snow_shot_add_diagnostics_crash_tests()
    snow_shot_add_ocr_runtime_tests()
endif()
