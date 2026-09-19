# Copies the runtime dependency closure of a list of DLLs into a destination.
#
# Expected arguments:
#   COPY_FROM - semicolon list of DLL paths (CMake expands generator
#               expressions such as $<TARGET_FILE:...> before the script
#               runs, delivering a native list variable)
#   COPY_TO   - destination directory
#   COPY_DIR  - directory used to resolve non-system dependencies beside the
#               root DLLs (for example protobuf, Abseil, and DirectML).
if(NOT DEFINED COPY_FROM OR NOT DEFINED COPY_TO)
    message(FATAL_ERROR "copy_runtime_dlls.cmake requires COPY_FROM and COPY_TO")
endif()

set(_root_dlls)
foreach(_dll IN LISTS COPY_FROM)
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
        message(FATAL_ERROR "copy_runtime_dlls: required root DLL '${_dll}' does not exist")
    endif()
    list(APPEND _root_dlls "${_dll}")
endforeach()

if(NOT _root_dlls)
    message(FATAL_ERROR "copy_runtime_dlls: COPY_FROM did not contain a runtime DLL")
endif()

set(_search_directories)
if(DEFINED COPY_DIR AND EXISTS "${COPY_DIR}")
    list(APPEND _search_directories "${COPY_DIR}")
endif()

file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES ${_root_dlls}
    DIRECTORIES ${_search_directories}
    RESOLVED_DEPENDENCIES_VAR _resolved_dlls
    UNRESOLVED_DEPENDENCIES_VAR _unresolved_dlls
    CONFLICTING_DEPENDENCIES_PREFIX _conflicting_dlls
    PRE_EXCLUDE_REGEXES
        "^[Aa][Pp][Ii]-[Mm][Ss]-[Ww][Ii][Nn]-.*"
        "^[Ee][Xx][Tt]-[Mm][Ss]-.*"
    POST_EXCLUDE_REGEXES
        ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
)

if(_unresolved_dlls)
    list(JOIN _unresolved_dlls ", " _unresolved_message)
    message(FATAL_ERROR
        "copy_runtime_dlls: unresolved non-system dependencies: ${_unresolved_message}")
endif()
if(_conflicting_dlls_FILENAMES)
    list(JOIN _conflicting_dlls_FILENAMES ", " _conflicting_message)
    message(FATAL_ERROR
        "copy_runtime_dlls: conflicting dependency candidates: ${_conflicting_message}")
endif()

file(MAKE_DIRECTORY "${COPY_TO}")
set(_all_dlls ${_root_dlls} ${_resolved_dlls})
list(REMOVE_DUPLICATES _all_dlls)
foreach(_dll IN LISTS _all_dlls)
    file(COPY "${_dll}" DESTINATION "${COPY_TO}")
endforeach()
