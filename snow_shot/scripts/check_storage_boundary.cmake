if(NOT DEFINED SNOW_SHOT_SOURCE_DIR)
    message(FATAL_ERROR "SNOW_SHOT_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE _snow_shot_sources
    "${SNOW_SHOT_SOURCE_DIR}/include/*.h"
    "${SNOW_SHOT_SOURCE_DIR}/include/*.hpp"
    "${SNOW_SHOT_SOURCE_DIR}/src/*.h"
    "${SNOW_SHOT_SOURCE_DIR}/src/*.cpp"
)

# Sources allowed to mention storage internals. The update transaction writes
# the Windows uninstall registry entry (QSettings::Registry32Format) and
# verifies the deployed tree's `bin/__data_directory` marker; both describe
# the installation layout, not application-settings storage, so they do not
# bypass the ConfigurationStore boundary this check protects.
set(_snow_shot_boundary_allowlist
    "${SNOW_SHOT_SOURCE_DIR}/src/update/updatetransaction.cpp"
)

foreach(_source IN LISTS _snow_shot_sources)
    file(TO_CMAKE_PATH "${_source}" _normalized_source)
    file(READ "${_source}" _contents)

    string(FIND "${_contents}" "QSettings" _qsettings_position)
    if(NOT _qsettings_position EQUAL -1 AND NOT _normalized_source IN_LIST _snow_shot_boundary_allowlist)
        message(FATAL_ERROR
            "Storage boundary violation: 'QSettings' appears in ${_source}"
        )
    endif()

    if(_normalized_source MATCHES "/(include/snow_shot/storage|src/storage)/")
        string(FIND "${_contents}" "snow_shot/presentation" _presentation_position)
        if(NOT _presentation_position EQUAL -1)
            message(FATAL_ERROR
                "Storage boundary violation: storage includes presentation in ${_source}"
            )
        endif()
        continue()
    endif()

    if(NOT _normalized_source IN_LIST _snow_shot_boundary_allowlist)
        foreach(_forbidden IN ITEMS
            "config.json"
            "__data_directory"
            "canvas_history.json"
            "display_0.png"
        )
            string(FIND "${_contents}" "${_forbidden}" _position)
            if(NOT _position EQUAL -1)
                message(FATAL_ERROR
                    "Storage boundary violation: '${_forbidden}' appears in ${_source}"
                )
            endif()
        endforeach()
    endif()
endforeach()
