include_guard(GLOBAL)

# Scripts and standalone subprojects share one audited Qt toolchain contract.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../scripts/qt-toolchain.json")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../scripts/qt-toolchain.json" _snow_qt_contract)
string(JSON _snow_qt_schema GET "${_snow_qt_contract}" schemaVersion)
if(NOT _snow_qt_schema EQUAL 1)
    message(FATAL_ERROR "Unsupported Qt toolchain contract schema: ${_snow_qt_schema}")
endif()
string(JSON _snow_qt_version GET "${_snow_qt_contract}" qtVersion)
string(JSON _snow_qt_deprecation GET "${_snow_qt_contract}" deprecationVersion)
string(LENGTH "${_snow_qt_deprecation}" _snow_qt_deprecation_length)
if(NOT _snow_qt_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$" OR
   NOT _snow_qt_deprecation MATCHES "^0x[0-9a-fA-F]+$" OR
   NOT _snow_qt_deprecation_length EQUAL 8)
    message(FATAL_ERROR "The Qt toolchain contract contains an invalid version")
endif()
set_property(GLOBAL PROPERTY SNOW_QT_VERSION "${_snow_qt_version}")
set_property(GLOBAL PROPERTY SNOW_QT_DEPRECATION_VERSION "${_snow_qt_deprecation}")
unset(_snow_qt_contract)
unset(_snow_qt_schema)
unset(_snow_qt_version)
unset(_snow_qt_deprecation)
unset(_snow_qt_deprecation_length)

macro(snow_find_qt)
    get_property(SNOW_QT_VERSION GLOBAL PROPERTY SNOW_QT_VERSION)
    find_package(Qt6 "${SNOW_QT_VERSION}" EXACT REQUIRED ${ARGN})
    if(NOT Qt6_VERSION VERSION_EQUAL SNOW_QT_VERSION)
        message(FATAL_ERROR
            "The resolved Qt ${Qt6_VERSION} does not match the audited Qt ${SNOW_QT_VERSION} kit")
    endif()
    # Directory definitions cover project-owned targets and Qt-generated resource
    # targets without changing the imported dependencies' compile interfaces.
    get_property(_snow_qt_policy_set DIRECTORY PROPERTY SNOW_QT_POLICY_SET)
    if(NOT _snow_qt_policy_set)
        get_property(_snow_qt_deprecation GLOBAL PROPERTY SNOW_QT_DEPRECATION_VERSION)
        add_compile_definitions("QT_DISABLE_DEPRECATED_UP_TO=${_snow_qt_deprecation}")
        set_property(DIRECTORY PROPERTY SNOW_QT_POLICY_SET TRUE)
        unset(_snow_qt_deprecation)
    endif()
    unset(_snow_qt_policy_set)
endmacro()

macro(snow_find_qt_private module)
    if(NOT Qt6_VERSION)
        message(FATAL_ERROR "Resolve Qt with snow_find_qt before requesting a private module")
    endif()
    # Private Qt modules have no cross-version ABI guarantee. Resolve them only
    # from the selected public kit, including for standalone application builds.
    find_package(Qt6${module}Private "${Qt6_VERSION}" EXACT REQUIRED CONFIG GLOBAL
        PATHS "${Qt6_DIR}/.." NO_DEFAULT_PATH)
    if(NOT Qt6${module}Private_VERSION VERSION_EQUAL Qt6_VERSION)
        message(FATAL_ERROR "The private Qt ${module} module does not match Qt ${Qt6_VERSION}")
    endif()
endmacro()

function(snow_qt_bin_directory output)
    # qtpaths is available independently of the optional qmake build feature.
    if(TARGET Qt6::qtpaths)
        get_target_property(_snow_qt_paths Qt6::qtpaths IMPORTED_LOCATION)
        if(NOT _snow_qt_paths)
            get_target_property(_snow_qt_configurations Qt6::qtpaths IMPORTED_CONFIGURATIONS)
            foreach(_snow_qt_configuration IN LISTS _snow_qt_configurations)
                string(TOUPPER "${_snow_qt_configuration}" _snow_qt_configuration)
                get_target_property(_snow_qt_paths Qt6::qtpaths
                    "IMPORTED_LOCATION_${_snow_qt_configuration}")
                if(_snow_qt_paths)
                    break()
                endif()
            endforeach()
        endif()
    endif()
    if(_snow_qt_paths)
        get_filename_component(_snow_qt_bin "${_snow_qt_paths}" DIRECTORY)
    elseif(QT6_INSTALL_PREFIX AND QT6_INSTALL_BINS)
        get_filename_component(_snow_qt_bin "${QT6_INSTALL_BINS}" ABSOLUTE
            BASE_DIR "${QT6_INSTALL_PREFIX}")
    else()
        message(FATAL_ERROR "The selected Qt kit does not expose its tools directory")
    endif()
    set(${output} "${_snow_qt_bin}" PARENT_SCOPE)
endfunction()
