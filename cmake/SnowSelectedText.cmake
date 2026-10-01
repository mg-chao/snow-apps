if(NOT TARGET snow_selected_text_c)
    # Selected-text capture is Full-only. Keep it outside the shared capture /
    # recording / canvas archive so Mini never compiles or links its engines.
    set(_snow_selected_text_features)
    set(_snow_selected_text_archive snow_selected_text_c)
    if(TARGET snow_shot_rust_ffi_bundle)
        list(APPEND _snow_selected_text_features host-application-qos)
        set(_snow_selected_text_archive snow_selected_text_c_archive)
    endif()
    snow_add_rust_static_library(${_snow_selected_text_archive}
        PACKAGE snow-selected-text-c
        MANIFEST_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}"
        OUTPUT_NAME snow_selected_text_c
        FEATURES ${_snow_selected_text_features}
        STRIP_MSVC_DIRECTIVES)
    if(TARGET snow_shot_rust_ffi_bundle)
        # The fat-LTO bundle owns the host Rust runtime. Load it first so the
        # separate archive contributes selected-text code without extracting
        # another runtime and defining the same symbols twice.
        add_library(snow_selected_text_c INTERFACE)
        target_link_libraries(snow_selected_text_c INTERFACE
            snow_shot_rust_ffi_bundle ${_snow_selected_text_archive})
        # Other capture/recording consumers also depend on the bundle. Promote
        # it to a direct dependency of the final consumer so CMake's transitive
        # ordering cannot move it behind the selected-text archive.
        set_property(TARGET snow_selected_text_c PROPERTY
            INTERFACE_LINK_LIBRARIES_DIRECT snow_shot_rust_ffi_bundle)
    endif()
    unset(_snow_selected_text_features)
    unset(_snow_selected_text_archive)
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
