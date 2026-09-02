if(NOT DEFINED SNOW_SHOT_SOURCE_DIR)
    message(FATAL_ERROR "SNOW_SHOT_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE _snow_shot_sources
    "${SNOW_SHOT_SOURCE_DIR}/include/*.h"
    "${SNOW_SHOT_SOURCE_DIR}/include/*.hpp"
    "${SNOW_SHOT_SOURCE_DIR}/src/*.h"
    "${SNOW_SHOT_SOURCE_DIR}/src/*.cpp"
)

foreach(_source IN LISTS _snow_shot_sources)
    file(TO_CMAKE_PATH "${_source}" _normalized_source)
    file(READ "${_source}" _contents)

    string(FIND "${_contents}" "QSettings" _qsettings_position)
    if(NOT _qsettings_position EQUAL -1)
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

    foreach(_forbidden IN ITEMS
        "config.json"
        "__data_directory"
        "capture_history_records"
        "capture_history_quarantine"
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
endforeach()
