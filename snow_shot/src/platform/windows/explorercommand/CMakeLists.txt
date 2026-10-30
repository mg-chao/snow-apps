# This subdirectory is added before Snow Shot's directory-wide Qt linkage. Explorer must
# be able to load the packaged COM server without loading the application's Qt runtime.
set(_snow_explorer_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(_snow_explorer_suffixes)
set(SNOW_EXPLORER_IMAGE_SUFFIXES "")
foreach(_header image/supportedimagefiles.h app/supportedpinfiles.h)
    set(_path "${_snow_explorer_root}/include/snow_shot/${_header}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
    file(READ "${_path}" _source)
    string(REGEX MATCHALL "\\{\"[a-z0-9]+\",[ \t\r\n]*(ImageFileCodec|PinDocumentFormat)::"
        _formats "${_source}")
    foreach(_format IN LISTS _formats)
        string(REGEX REPLACE "^\\{\"([a-z0-9]+)\".*" "\\1" _suffix "${_format}")
        list(APPEND _snow_explorer_suffixes "${_suffix}")
        if(_header STREQUAL "image/supportedimagefiles.h")
            string(APPEND SNOW_EXPLORER_IMAGE_SUFFIXES "    L\".${_suffix}\",\n")
        endif()
    endforeach()
endforeach()
list(LENGTH _snow_explorer_suffixes _snow_explorer_suffix_count)
list(REMOVE_DUPLICATES _snow_explorer_suffixes)
list(LENGTH _snow_explorer_suffixes _snow_explorer_unique_suffix_count)
if(_snow_explorer_suffix_count LESS 2 OR
   NOT _snow_explorer_suffix_count EQUAL _snow_explorer_unique_suffix_count)
    message(FATAL_ERROR "The Explorer command needs unique supported pin-file extensions.")
endif()
set(SNOW_EXPLORER_SUFFIXES "")
foreach(_suffix IN LISTS _snow_explorer_suffixes)
    string(APPEND SNOW_EXPLORER_SUFFIXES "    L\".${_suffix}\",\n")
endforeach()
configure_file(supportedformats.h.in generated/supportedformats.h @ONLY)

# Use the package's existing localized titles, keeping native resources independent of Qt.
foreach(_language en-US zh-CN zh-TW)
    set(_path "${_snow_explorer_root}/packaging/msix/Strings/${_language}/Resources.resw")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
    file(READ "${_path}" _source)
    string(REGEX MATCH "name=\"EditVerb\"[^>]*><value>([^<]+)</value>" _match "${_source}")
    if(NOT _match)
        message(FATAL_ERROR "Missing MSIX EditVerb title for ${_language}.")
    endif()
    set(_title "${CMAKE_MATCH_1}")
    string(REPLACE "&amp;" "&" _title "${_title}")
    string(REPLACE "&quot;" "\"" _title "${_title}")
    string(REPLACE "&apos;" "'" _title "${_title}")
    string(REPLACE "\\" "\\\\" _title "${_title}")
    string(REPLACE "\"" "\"\"" _title "${_title}")
    set("SNOW_EXPLORER_TITLE_${_language}" "${_title}")
endforeach()
configure_file(explorercommand.rc.in generated/explorercommand.rc @ONLY)

add_library(snow_shot_explorer_command SHARED
    explorercommand.cpp explorercommand.h explorercommand.def
    "${CMAKE_CURRENT_BINARY_DIR}/generated/explorercommand.rc")
set_target_properties(snow_shot_explorer_command PROPERTIES
    OUTPUT_NAME SnowShotExplorerCommand
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(snow_shot_explorer_command PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${_snow_explorer_root}/include")
target_compile_definitions(snow_shot_explorer_command PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(snow_shot_explorer_command PRIVATE ole32 shell32 shlwapi uuid user32)
snow_apply_release_options(snow_shot_explorer_command POLICY size)

if(SNOW_SHOT_DISTRIBUTION_CHANNEL STREQUAL "store")
    install(TARGETS snow_shot_explorer_command RUNTIME DESTINATION bin COMPONENT SnowShot)
endif()

if(SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-explorer-command-tests
        "${_snow_explorer_root}/tests/explorer_command_tests.cpp"
        explorercommand.cpp explorercommand.h
        "${CMAKE_CURRENT_BINARY_DIR}/generated/explorercommand.rc")
    set_target_properties(snow-shot-explorer-command-tests PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_include_directories(snow-shot-explorer-command-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${_snow_explorer_root}/include")
    target_compile_definitions(snow-shot-explorer-command-tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_link_libraries(snow-shot-explorer-command-tests PRIVATE ole32 shell32 shlwapi uuid user32)
    add_dependencies(snow-shot-explorer-command-tests snow_shot_explorer_command)
    add_test(NAME snow-shot-explorer-command-tests
        COMMAND snow-shot-explorer-command-tests "$<TARGET_FILE:snow_shot_explorer_command>")
    set_tests_properties(snow-shot-explorer-command-tests PROPERTIES LABELS "unit;windows" TIMEOUT 30)
endif()
