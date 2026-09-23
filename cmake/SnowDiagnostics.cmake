set(SNOW_DIAGNOSTICS_NATIVE_DIR "${SNOW_SHOT_CAPTURE_CRATES_DIR}/crates/snow-ocr-process/native")
if(WIN32 OR APPLE)
    find_package(crashpad CONFIG REQUIRED)
    find_program(SNOW_CRASHPAD_HANDLER NAMES crashpad_handler
        PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/crashpad"
              "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools"
        NO_DEFAULT_PATH REQUIRED)
endif()

set(_snow_bridge_source "${SNOW_DIAGNOSTICS_NATIVE_DIR}/diagnosticsbridge.cpp")
if(APPLE)
    set(_snow_bridge_source "${SNOW_DIAGNOSTICS_NATIVE_DIR}/diagnosticsbridge_macos.cpp")
endif()
add_library(snow_shot_crash_bridge STATIC "${_snow_bridge_source}")
if(APPLE)
    target_link_libraries(snow_shot_crash_bridge PUBLIC crashpad::crashpad)
endif()
target_include_directories(snow_shot_crash_bridge PUBLIC "${SNOW_DIAGNOSTICS_NATIVE_DIR}")
if(WIN32)
    target_link_libraries(snow_shot_crash_bridge PUBLIC crashpad::crashpad dbghelp winhttp rpcrt4 version)
endif()

execute_process(COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE SNOW_DIAGNOSTICS_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT SNOW_DIAGNOSTICS_REVISION)
    set(SNOW_DIAGNOSTICS_REVISION unknown)
endif()
target_compile_definitions(snow_shot_crash_bridge PRIVATE
    SNOW_DIAGNOSTICS_BUILD="$<CONFIG>" SNOW_DIAGNOSTICS_REVISION="${SNOW_DIAGNOSTICS_REVISION}")
add_library(snow_shot_diagnostics STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include/snow_shot/diagnostics/diagnostics.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics/diagnostics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics/crashcollector.cpp")
target_include_directories(snow_shot_diagnostics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(snow_shot_diagnostics PUBLIC Qt6::Core PRIVATE snow_shot_crash_bridge)
target_compile_definitions(snow_shot_diagnostics PUBLIC QT_MESSAGELOGCONTEXT)
target_compile_definitions(snow_shot_diagnostics PRIVATE
    SNOW_DIAGNOSTICS_VERSION="${SNOW_SHOT_VERSION}"
    SNOW_DIAGNOSTICS_REVISION="${SNOW_DIAGNOSTICS_REVISION}")
if(MSVC)
    target_compile_options(snow_shot_diagnostics PRIVATE $<$<CONFIG:Release>:/Z7>)
    target_compile_options(snow_shot_crash_bridge PRIVATE $<$<CONFIG:Release>:/Z7>)
endif()

# OCR runs the release Cargo profile in every preset; its bridge must match that CRT.
if(WIN32)
    add_library(snow_ocr_diagnostics_bridge STATIC "${SNOW_DIAGNOSTICS_NATIVE_DIR}/diagnosticsbridge.cpp")
    set_target_properties(snow_ocr_diagnostics_bridge PROPERTIES AUTOMOC OFF)
    target_include_directories(snow_ocr_diagnostics_bridge SYSTEM PRIVATE
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/crashpad"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    if(SNOW_SHOT_RELEASE_STATIC OR SNOW_APPS_RELEASE_STATIC OR
       (CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded" AND
        NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL"))
        set_property(TARGET snow_ocr_diagnostics_bridge PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
    else()
        set_property(TARGET snow_ocr_diagnostics_bridge PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
    endif()
    target_compile_options(snow_ocr_diagnostics_bridge PRIVATE /Brepro /Z7 /U_DEBUG)
    set_property(TARGET snow_ocr_diagnostics_bridge PROPERTY STATIC_LIBRARY_OPTIONS /Brepro)
    target_compile_definitions(snow_ocr_diagnostics_bridge PRIVATE NDEBUG _ITERATOR_DEBUG_LEVEL=0 NOMINMAX WIN32_LEAN_AND_MEAN)
    # The independently versioned OCR payload must not change after unrelated app commits.
    target_compile_definitions(snow_ocr_diagnostics_bridge PRIVATE SNOW_DIAGNOSTICS_BUILD="Release")
    set(_snow_crash_links "$<TARGET_FILE:snow_ocr_diagnostics_bridge>")
    foreach(_lib vcpkg_crashpad_client vcpkg_crashpad_client_common vcpkg_crashpad_util vcpkg_crashpad_base)
        list(APPEND _snow_crash_links "${CRASHPAD_${_lib}_LIBRARY_RELEASE}")
    endforeach()
    list(APPEND _snow_crash_links "${ZLIB_LIBRARY_RELEASE}" dbghelp.lib winhttp.lib rpcrt4.lib version.lib)
    list(JOIN _snow_crash_links "\n" _snow_crash_link_manifest)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/snow-ocr-crash-$<CONFIG>.rsp"
        CONTENT "${_snow_crash_link_manifest}\n")
endif()

if(APPLE)
    # The independently versioned worker must not embed the application's revision.
    add_library(snow_ocr_diagnostics_bridge STATIC "${_snow_bridge_source}")
    set_target_properties(snow_ocr_diagnostics_bridge PROPERTIES AUTOMOC OFF)
    target_link_libraries(snow_ocr_diagnostics_bridge PUBLIC crashpad::crashpad)
    target_compile_definitions(snow_ocr_diagnostics_bridge PRIVATE SNOW_DIAGNOSTICS_BUILD="Release")
    set(_snow_crash_links "$<TARGET_FILE:snow_ocr_diagnostics_bridge>")
    foreach(_lib vcpkg_crashpad_client vcpkg_crashpad_client_common vcpkg_crashpad_util vcpkg_crashpad_base mig_output)
        list(APPEND _snow_crash_links "${CRASHPAD_${_lib}_LIBRARY_RELEASE}")
    endforeach()
    list(APPEND _snow_crash_links "${ZLIB_LIBRARY_RELEASE}" -lc++ -lbsm
        "-Wl,-rpath,@loader_path" "-Wl,-headerpad_max_install_names")
    foreach(_framework ApplicationServices CoreFoundation Foundation IOKit Security)
        list(APPEND _snow_crash_links -framework "${_framework}")
    endforeach()
    list(JOIN _snow_crash_links "\n" _snow_crash_link_manifest)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/snow-ocr-crash-$<CONFIG>.rsp"
        CONTENT "${_snow_crash_link_manifest}\n")
endif()
