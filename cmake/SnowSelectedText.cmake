if(NOT TARGET snow_selected_text_c)
    if(TARGET snow_shot_rust_ffi_bundle)
        # Fat LTO can internalize runtime functions in one archive while another
        # archive still needs the original std object. Compile Full's Rust code
        # together so Rust resolves its runtime once. Separate Cargo outputs
        # keep Mini's default archive free of selected-text dependencies.
        function(_snow_add_full_rust_bundle)
            set(SNOW_RUST_CARGO_TARGET_DIR "${SNOW_RUST_CARGO_TARGET_DIR}/full")
            snow_add_rust_static_library(snow_shot_full_rust_ffi_bundle
                PACKAGE snow-shot-rust-ffi
                MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}/../snow_rust_ffi"
                OUTPUT_NAME snow_shot_rust_ffi
                FEATURES selected-text ${_snow_scrolling_perf_features}
                STRIP_MSVC_DIRECTIVES)
        endfunction()
        _snow_add_full_rust_bundle()
        snow_add_rust_bundle_selector(snow_shot_rust_ffi_selector snow_shot_rust_ffi_bundle)
        add_library(snow_selected_text_c INTERFACE)
        target_link_libraries(snow_selected_text_c INTERFACE
            snow_shot_full_rust_ffi_bundle)
        # Full hosts select this superset for every shared C ABI dependency;
        # Mini and canvas-only hosts retain the default archive.
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
            CALL snow_select_rust_bundle_consumers
                snow_selected_text_c snow_shot_full_rust_ffi_bundle)
    else()
        snow_add_rust_static_library(snow_selected_text_c
            PACKAGE snow-selected-text-c
            MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
            OUTPUT_NAME snow_selected_text_c
            STRIP_MSVC_DIRECTIVES)
    endif()
    target_include_directories(snow_selected_text_c INTERFACE
        "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-selected-text-c/include")
    if(APPLE)
        find_library(SNOW_SELECTED_TEXT_APPKIT_FRAMEWORK NAMES AppKit REQUIRED)
        find_library(SNOW_SELECTED_TEXT_APPLICATION_SERVICES_FRAMEWORK
            NAMES ApplicationServices REQUIRED)
        find_library(SNOW_SELECTED_TEXT_CORE_FOUNDATION_FRAMEWORK NAMES CoreFoundation REQUIRED)
        find_library(SNOW_SELECTED_TEXT_CORE_GRAPHICS_FRAMEWORK NAMES CoreGraphics REQUIRED)
        target_link_libraries(snow_selected_text_c INTERFACE
            "${SNOW_SELECTED_TEXT_APPKIT_FRAMEWORK}"
            "${SNOW_SELECTED_TEXT_APPLICATION_SERVICES_FRAMEWORK}"
            "${SNOW_SELECTED_TEXT_CORE_FOUNDATION_FRAMEWORK}"
            "${SNOW_SELECTED_TEXT_CORE_GRAPHICS_FRAMEWORK}")
    endif()
endif()
