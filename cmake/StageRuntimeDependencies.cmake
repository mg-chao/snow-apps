cmake_minimum_required(VERSION 3.25)

if(SNOW_RUNTIME_ROOT_LIBRARY)
    list(APPEND SNOW_RUNTIME_ROOT_LIBRARIES "${SNOW_RUNTIME_ROOT_LIBRARY}")
endif()
if(NOT SNOW_RUNTIME_ROOT_LIBRARIES)
    message(FATAL_ERROR "A runtime root library is required to resolve runtime dependencies.")
endif()
if(NOT SNOW_RUNTIME_DESTINATION AND NOT SNOW_RUNTIME_MANIFEST)
    message(FATAL_ERROR "A runtime destination or manifest is required.")
endif()

set(_snow_runtime_search_directories)
foreach(_snow_runtime_root IN LISTS SNOW_RUNTIME_ROOT_LIBRARIES)
    if(NOT EXISTS "${_snow_runtime_root}")
        message(FATAL_ERROR "Runtime root library does not exist: ${_snow_runtime_root}")
    endif()
    get_filename_component(_snow_runtime_root_directory "${_snow_runtime_root}" DIRECTORY)
    list(APPEND _snow_runtime_search_directories "${_snow_runtime_root_directory}")
endforeach()

if(DEFINED SNOW_RUNTIME_SEARCH_DIRECTORY AND
   IS_DIRECTORY "${SNOW_RUNTIME_SEARCH_DIRECTORY}")
    list(APPEND _snow_runtime_search_directories
        "${SNOW_RUNTIME_SEARCH_DIRECTORY}")
endif()
list(REMOVE_DUPLICATES _snow_runtime_search_directories)

file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES ${SNOW_RUNTIME_ROOT_LIBRARIES}
    DIRECTORIES ${_snow_runtime_search_directories}
    RESOLVED_DEPENDENCIES_VAR _snow_runtime_resolved_dependencies
    UNRESOLVED_DEPENDENCIES_VAR _snow_runtime_unresolved_dependencies
    CONFLICTING_DEPENDENCIES_PREFIX _snow_runtime_conflicts
    PRE_EXCLUDE_REGEXES
        "api-ms-.*"
        "ext-ms-.*"
        "msvcp.*\\.dll"
        "ucrtbased\\.dll"
        "vcruntime.*\\.dll"
    POST_EXCLUDE_REGEXES
        ".*[\\\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\\\/].*"
)

if(_snow_runtime_unresolved_dependencies)
    list(JOIN _snow_runtime_unresolved_dependencies ", " _snow_runtime_unresolved_text)
    message(FATAL_ERROR
        "Unable to resolve runtime dependencies for ${SNOW_RUNTIME_ROOT_LIBRARIES}: "
        "${_snow_runtime_unresolved_text}")
endif()
if(_snow_runtime_conflicts_FILENAMES)
    list(JOIN _snow_runtime_conflicts_FILENAMES ", " _snow_runtime_conflicts_text)
    message(FATAL_ERROR
        "Conflicting runtime dependencies for ${SNOW_RUNTIME_ROOT_LIBRARIES}: "
        "${_snow_runtime_conflicts_text}")
endif()

set(_snow_runtime_files
    ${SNOW_RUNTIME_ROOT_LIBRARIES}
    ${_snow_runtime_resolved_dependencies}
)
list(REMOVE_DUPLICATES _snow_runtime_files)
if(SNOW_RUNTIME_MANIFEST)
    file(WRITE "${SNOW_RUNTIME_MANIFEST}" "set(SNOW_RUNTIME_FILES\n")
    foreach(_snow_runtime_file IN LISTS _snow_runtime_files)
        file(APPEND "${SNOW_RUNTIME_MANIFEST}" "    [==[${_snow_runtime_file}]==]\n")
    endforeach()
    file(APPEND "${SNOW_RUNTIME_MANIFEST}" ")\n")
endif()
if(NOT SNOW_RUNTIME_DESTINATION)
    return()
endif()
file(MAKE_DIRECTORY "${SNOW_RUNTIME_DESTINATION}")
foreach(_snow_runtime_file IN LISTS _snow_runtime_files)
    get_filename_component(_snow_runtime_filename "${_snow_runtime_file}" NAME)
    set(_snow_runtime_destination_file
        "${SNOW_RUNTIME_DESTINATION}/${_snow_runtime_filename}")
    if(NOT _snow_runtime_file STREQUAL _snow_runtime_destination_file)
        file(COPY_FILE
            "${_snow_runtime_file}"
            "${_snow_runtime_destination_file}"
            ONLY_IF_DIFFERENT
        )
    endif()
endforeach()
