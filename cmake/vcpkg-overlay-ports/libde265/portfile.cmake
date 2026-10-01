set(_snow_vcpkg_libde265_port "${VCPKG_ROOT_DIR}/ports/libde265")

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO strukturag/libde265
    REF "v${VERSION}"
    SHA512 bda239b4827c81552855dc540724b74c86f6b02bcd0fe556650bc16d665a8eed1ddbde76ac0972d26b3002b14575bb9b6f70b367c39eb7de45c5c9df324e3d05
    HEAD_REF master
    PATCHES
        "${_snow_vcpkg_libde265_port}/fix-interface-include.patch"
        "${_snow_vcpkg_libde265_port}/pkgconfig-cxx-linkage.diff"
        bounded-scan-table-initialization.patch
)

set(_snow_libde265_check_options)
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW AND NOT VCPKG_CROSSCOMPILING)
    # Exercise the actual scan source with the same optimized compiler settings
    # as the DLL. Loading libheif calls this code from its static plugin registry.
    list(APPEND _snow_libde265_check_options
        "-DCMAKE_PROJECT_INCLUDE=${CURRENT_PORT_DIR}/scan-table-check.cmake")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENABLE_SDL=OFF
        ${_snow_libde265_check_options}
)

if(_snow_libde265_check_options)
    vcpkg_cmake_build(TARGET snow_libde265_scan_table_check LOGFILE_BASE build-scan-table-check)
    foreach(_snow_libde265_config IN ITEMS rel dbg)
        if(NOT VCPKG_BUILD_TYPE OR
           (VCPKG_BUILD_TYPE STREQUAL "release" AND _snow_libde265_config STREQUAL "rel") OR
           (VCPKG_BUILD_TYPE STREQUAL "debug" AND _snow_libde265_config STREQUAL "dbg"))
            vcpkg_execute_required_process(
                COMMAND "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-${_snow_libde265_config}/snow_libde265_scan_table_check.exe"
                WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-${_snow_libde265_config}"
                LOGNAME "scan-table-check-${TARGET_TRIPLET}-${_snow_libde265_config}"
                TIMEOUT 30
            )
        endif()
    endforeach()
endif()

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/libde265)
vcpkg_copy_tools(TOOL_NAMES dec265 AUTO_CLEAN)
vcpkg_fixup_pkgconfig()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libde265/de265.h" "!defined(LIBDE265_STATIC_BUILD)" "0")
else()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libde265/de265.h" "!defined(LIBDE265_STATIC_BUILD)" "1")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
