include_guard(GLOBAL)

function(snow_workspace_configure_paths)
    set(_vcpkg_default "${CMAKE_CURRENT_SOURCE_DIR}/.tools/vcpkg")
    if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
        set(_vcpkg_default "$ENV{VCPKG_ROOT}")
    endif()

    set(SNOW_VCPKG_ROOT "${_vcpkg_default}" CACHE PATH
        "Repository-local vcpkg root used by the active build.")
    set(SNOW_ANT_DESIGN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ant_design_qt" CACHE PATH
        "Ant Design Qt source directory.")
    set(SNOW_IMAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/snow_image" CACHE PATH
        "Snow Image source directory.")
    set(SNOW_DRAW_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/snow_draw_engine_qt" CACHE PATH
        "Snow Draw Engine Qt source directory.")
    set(SNOW_CAPTURE_CRATES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/snow-crates" CACHE PATH
        "Snow crates workspace directory.")
    set(SNOW_LIBCLANG_BIN_DIR "${SNOW_VCPKG_ROOT}/../llvm/bin" CACHE PATH
        "Directory containing libclang.dll for Rust bindgen.")

    if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING
            "vcpkg target triplet used by this build." FORCE)
    endif()
    set(_vcpkg_installed_default "${SNOW_VCPKG_ROOT}/installed")
    if(DEFINED VCPKG_INSTALLED_DIR AND NOT VCPKG_INSTALLED_DIR STREQUAL "")
        set(_vcpkg_installed_default "${VCPKG_INSTALLED_DIR}")
    endif()
    set(SNOW_VCPKG_INSTALLED_DIR "${_vcpkg_installed_default}" CACHE PATH
        "vcpkg installed tree used by the active build preset.")
    set(SNOW_FFMPEG_ROOT "${SNOW_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}" CACHE PATH
        "vcpkg installation prefix containing FFmpeg and OpenCV.")

    foreach(_required_dir IN ITEMS
        SNOW_ANT_DESIGN_SOURCE_DIR
        SNOW_IMAGE_SOURCE_DIR
        SNOW_DRAW_ENGINE_SOURCE_DIR
        SNOW_CAPTURE_CRATES_DIR
    )
        if(NOT IS_DIRECTORY "${${_required_dir}}")
            message(FATAL_ERROR
                "${_required_dir} does not exist: ${${_required_dir}}. "
                "Run scripts/bootstrap.ps1 or set the path explicitly.")
        endif()
    endforeach()

    if(NOT EXISTS "${SNOW_CAPTURE_CRATES_DIR}/Cargo.toml")
        message(FATAL_ERROR "Snow crates workspace is missing Cargo.toml: ${SNOW_CAPTURE_CRATES_DIR}")
    endif()
endfunction()

function(snow_workspace_configure_options)
    set(SNOW_ENABLE_CLANG_TIDY ${SNOW_APPS_ENABLE_CLANG_TIDY} CACHE BOOL
        "Run clang-tidy for enabled Snow projects." FORCE)

    set(ADQT_BUILD_TESTS ${SNOW_APPS_BUILD_TESTS} CACHE BOOL
        "Build Ant Design Qt tests." FORCE)
    set(ADQT_BUILD_BENCHMARKS ${SNOW_APPS_BUILD_BENCHMARKS} CACHE BOOL
        "Build Ant Design Qt benchmarks." FORCE)
    set(ADQT_STRICT_COMPILE ${SNOW_APPS_STRICT_COMPILE} CACHE BOOL
        "Strict Ant Design Qt compilation." FORCE)

    set(SNOW_IMAGE_STRICT_COMPILE ${SNOW_APPS_STRICT_COMPILE} CACHE BOOL
        "Strict Snow Image compilation." FORCE)
    set(SNOW_IMAGE_DEFAULT_LINKAGE static CACHE STRING
        "Default Snow Image target linkage." FORCE)

    # Development workspace presets use static Qt kits whose GUI module ships
    # bundled image codecs. Avoid propagating second vcpkg archives from the
    # static Snow Image target, which otherwise produces duplicate png_*
    # static Snow Image target, which otherwise produces duplicate codec
    # symbols in the final executable. Production static Qt is validated to
    # use system codecs and keeps the normal Snow Image dependencies.
    set(SNOW_IMAGE_LINK_PNG_DEPENDENCIES ON CACHE BOOL
        "Link the PNG dependency into the static Snow Image target." FORCE)
    set(SNOW_IMAGE_LINK_JPEG_DEPENDENCIES ON CACHE BOOL
        "Link JPEG dependencies into the static Snow Image target." FORCE)
    set(_snow_workspace_qt_gui_targets "")
    if(DEFINED Qt6_DIR AND NOT Qt6_DIR STREQUAL "")
        set(_snow_workspace_qt_gui_targets
            "${Qt6_DIR}/../Qt6Gui/Qt6GuiTargets.cmake")
    elseif(DEFINED ENV{SNOW_QT_STATIC_DIR} AND
           NOT "$ENV{SNOW_QT_STATIC_DIR}" STREQUAL "")
        set(_snow_workspace_qt_gui_targets
            "$ENV{SNOW_QT_STATIC_DIR}/../Qt6Gui/Qt6GuiTargets.cmake")
    endif()
    if(EXISTS "${_snow_workspace_qt_gui_targets}")
        file(READ "${_snow_workspace_qt_gui_targets}" _snow_workspace_qt_gui_text)
        if(_snow_workspace_qt_gui_text MATCHES
           "add_library\\(Qt6::Gui STATIC IMPORTED\\)")
            if(NOT _snow_workspace_qt_gui_text MATCHES
               "QT_ENABLED_PRIVATE_FEATURES [^\"]*system_png")
                set(SNOW_IMAGE_LINK_PNG_DEPENDENCIES OFF CACHE BOOL
                    "Link the PNG dependency into the static Snow Image target." FORCE)
            endif()
            if(NOT _snow_workspace_qt_gui_text MATCHES
               "QT_ENABLED_PRIVATE_FEATURES [^\"]*system_jpeg")
                set(SNOW_IMAGE_LINK_JPEG_DEPENDENCIES OFF CACHE BOOL
                    "Link JPEG dependencies into the static Snow Image target." FORCE)
            endif()
        endif()
        unset(_snow_workspace_qt_gui_text)
    endif()
    unset(_snow_workspace_qt_gui_targets)

    # The repository's portable Qt kit is static. Qt's imported targets carry
    # a static CRT requirement, so align every workspace target before nested
    # projects are added. Otherwise a static-CRT executable can link against
    # a dynamic-CRT Snow Image library and fail on standard CRT symbols.
    if(MSVC AND (SNOW_APPS_QT_STATIC OR
                 (DEFINED Qt6_DIR AND Qt6_DIR MATCHES "[Ss][Tt][Aa][Tt][Ii][Cc]") OR
                 (DEFINED ENV{SNOW_QT_STATIC_DIR} AND
                  "$ENV{SNOW_QT_STATIC_DIR}" MATCHES "[Ss][Tt][Aa][Tt][Ii][Cc]")))
        set(CMAKE_MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>"
            CACHE STRING
            "MSVC runtime used with the static Qt kit." FORCE)
    endif()

    if(SNOW_APPS_RELEASE_STATIC AND MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded CACHE STRING
            "MSVC runtime used for static release builds." FORCE)
    endif()

    if(SNOW_APPS_RELEASE_STATIC AND VCPKG_TARGET_TRIPLET STREQUAL "x64-windows")
        message(WARNING
            "SNOW_APPS_RELEASE_STATIC is enabled with x64-windows. "
            "Use the snow-shot-msvc-release preset for static vcpkg libraries.")
    endif()

    if(SNOW_APPS_API_BASE_URL STREQUAL "")
        if(CMAKE_CONFIGURATION_TYPES)
            set(SNOW_APPS_API_BASE_URL "https://snowshot.top" CACHE STRING
                "Default Snow Shot API base URL. Set explicitly for reproducible builds." FORCE)
        elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(SNOW_APPS_API_BASE_URL "http://localhost:8080" CACHE STRING
                "Default Snow Shot API base URL." FORCE)
        else()
            set(SNOW_APPS_API_BASE_URL "https://snowshot.top" CACHE STRING
                "Default Snow Shot API base URL." FORCE)
        endif()
    endif()
endfunction()
