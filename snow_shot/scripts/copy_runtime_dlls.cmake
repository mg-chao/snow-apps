# Copies a list of runtime DLLs into a destination directory.
#
# Expected arguments:
#   COPY_FROM - semicolon list of DLL paths (CMake expands generator
#               expressions such as $<TARGET_FILE:...> before the script
#               runs, delivering a native list variable)
#   COPY_TO   - destination directory
#   COPY_DIR  - optional; when set, every DLL in this directory is staged as
#               well. vcpkg shared companions (for example the debug flavor's
#               libprotobuf/abseil DLLs) sit beside onnxruntime.dll without
#               being declared in the imported target's link interface.
#
# Missing entries produce a warning instead of a hard error so partial
# toolchains (for example a release flavor without debug companions) do not
# break the build; the consumers that need a DLL report it at runtime.
if(NOT DEFINED COPY_FROM OR NOT DEFINED COPY_TO)
    message(FATAL_ERROR "copy_runtime_dlls.cmake requires COPY_FROM and COPY_TO")
endif()

set(_all_dlls "${COPY_FROM}")
if(DEFINED COPY_DIR AND EXISTS "${COPY_DIR}")
    file(GLOB _directory_dlls "${COPY_DIR}/*.dll")
    list(APPEND _all_dlls ${_directory_dlls})
endif()

foreach(_dll IN LISTS _all_dlls)
    if("${_dll}" STREQUAL "")
        continue()
    endif()
    # Generator expressions such as $<TARGET_FILE:t> evaluate to plain paths;
    # skip anything that is not a DLL (static libraries from the closure).
    string(TOLOWER "${_dll}" _dll_lower)
    if(NOT _dll_lower MATCHES "\\.dll$")
        continue()
    endif()
    if(NOT EXISTS "${_dll}")
        message(WARNING "copy_runtime_dlls: '${_dll}' does not exist; skipped")
        continue()
    endif()
    file(COPY "${_dll}" DESTINATION "${COPY_TO}")
endforeach()
