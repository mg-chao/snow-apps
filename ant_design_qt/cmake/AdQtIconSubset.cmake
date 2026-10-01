# The source scan deliberately keeps all color variants and falls back to the
# full pack if application source requests pack-level enumeration/registration.
# Headers stay complete; standalone builds continue using the checked-in pack.
function(adqt_generate_icon_subset output_source)
    cmake_parse_arguments(PARSE_ARGV 1 ADQT_SUBSET "" "NAME;NAMESPACE" "USAGE_ROOTS")
    if(NOT ADQT_SUBSET_NAME OR NOT ADQT_SUBSET_USAGE_ROOTS)
        message(FATAL_ERROR "adqt_generate_icon_subset requires NAME and USAGE_ROOTS")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_subset_generator "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_icon_pack.py")
    set(_subset_manifest
        "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_icons_qt/resources/antd.manifest.json")
    set(_subset_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/${ADQT_SUBSET_NAME}")
    set(_subset_header "${_subset_directory}/${ADQT_SUBSET_NAME}.h")
    set(_subset_source "${_subset_directory}/${ADQT_SUBSET_NAME}.cpp")
    set(_subset_report "${_subset_directory}/usage.json")
    set(_subset_arguments)
    set(_subset_dependencies)
    foreach(_usage_root IN LISTS ADQT_SUBSET_USAGE_ROOTS)
        if(NOT IS_DIRECTORY "${_usage_root}")
            message(FATAL_ERROR "AdQt icon usage root does not exist: ${_usage_root}")
        endif()
        # CONFIGURE_DEPENDS makes additions/removals update the command's input
        # list. Individual dependencies regenerate after source content changes.
        file(GLOB_RECURSE _usage_files CONFIGURE_DEPENDS
            "${_usage_root}/*.c" "${_usage_root}/*.cc" "${_usage_root}/*.cpp"
            "${_usage_root}/*.cxx" "${_usage_root}/*.h" "${_usage_root}/*.hh"
            "${_usage_root}/*.hpp" "${_usage_root}/*.mm")
        list(APPEND _subset_dependencies ${_usage_files})
        list(APPEND _subset_arguments --usage-root "${_usage_root}")
    endforeach()
    if(ADQT_SUBSET_NAMESPACE)
        list(APPEND _subset_arguments --cpp-namespace "${ADQT_SUBSET_NAMESPACE}")
    endif()
    file(GLOB_RECURSE _subset_templates CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/packages/ant_design_icons_qt/resources/templates/*.svg")
    add_custom_command(
        OUTPUT "${_subset_source}" "${_subset_header}" "${_subset_report}"
        COMMAND "${Python3_EXECUTABLE}" "${_subset_generator}" "${_subset_manifest}"
            --header "${_subset_header}" --source "${_subset_source}"
            --usage-report "${_subset_report}" --no-format ${_subset_arguments}
        DEPENDS "${_subset_generator}" "${_subset_manifest}"
            ${_subset_templates} ${_subset_dependencies}
        COMMENT "Generating the ${ADQT_SUBSET_NAME} icon usage subset"
        VERBATIM)
    set_source_files_properties("${_subset_source}" PROPERTIES
        SKIP_LINTING ON SKIP_UNITY_BUILD_INCLUSION ON)
    set(${output_source} "${_subset_source}" PARENT_SCOPE)
endfunction()
