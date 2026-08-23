cmake_minimum_required(VERSION 3.25)

foreach(_snow_runtime_required_variable IN ITEMS
    SNOW_RUNTIME_ROOT_LIBRARY
    SNOW_RUNTIME_DESTINATION
)
    if(NOT DEFINED ${_snow_runtime_required_variable} OR
       "${${_snow_runtime_required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "${_snow_runtime_required_variable} is required to stage runtime dependencies.")
    endif()
endforeach()

if(NOT EXISTS "${SNOW_RUNTIME_ROOT_LIBRARY}")
    message(FATAL_ERROR "Runtime root library does not exist: ${SNOW_RUNTIME_ROOT_LIBRARY}")
endif()

get_filename_component(_snow_runtime_root_directory
    "${SNOW_RUNTIME_ROOT_LIBRARY}" DIRECTORY)
set(_snow_runtime_search_directories "${_snow_runtime_root_directory}")
if(DEFINED SNOW_RUNTIME_SEARCH_DIRECTORY AND
   IS_DIRECTORY "${SNOW_RUNTIME_SEARCH_DIRECTORY}")
    list(APPEND _snow_runtime_search_directories
        "${SNOW_RUNTIME_SEARCH_DIRECTORY}")
endif()
list(REMOVE_DUPLICATES _snow_runtime_search_directories)

file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES "${SNOW_RUNTIME_ROOT_LIBRARY}"
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
        "Unable to resolve runtime dependencies for ${SNOW_RUNTIME_ROOT_LIBRARY}: "
        "${_snow_runtime_unresolved_text}")
endif()
if(_snow_runtime_conflicts_FILENAMES)
    list(JOIN _snow_runtime_conflicts_FILENAMES ", " _snow_runtime_conflicts_text)
    message(FATAL_ERROR
        "Conflicting runtime dependencies for ${SNOW_RUNTIME_ROOT_LIBRARY}: "
        "${_snow_runtime_conflicts_text}")
endif()

file(MAKE_DIRECTORY "${SNOW_RUNTIME_DESTINATION}")
set(_snow_runtime_files
    "${SNOW_RUNTIME_ROOT_LIBRARY}"
    ${_snow_runtime_resolved_dependencies}
)
list(REMOVE_DUPLICATES _snow_runtime_files)
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
