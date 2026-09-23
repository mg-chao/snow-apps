# The pinned upstream port interpolates MSVC flags into GN string literals
# without escaping SDK include paths. Keep its source/patch set and escape only
# the variables entering those literals (the workspace chainload adds SDK paths).
set(_snow_upstream "${VCPKG_ROOT_DIR}/ports/crashpad")
# Snow's Windows executables and OCR runtime use the static CRT.
if(VCPKG_TARGET_IS_WINDOWS)
    set(VCPKG_CRT_LINKAGE static)
endif()
file(READ "${_snow_upstream}/portfile.cmake" _snow_port)
string(REPLACE "z;zlib;zlibd" "z;zlib;zlibd;zlibstatic;zlibstaticd" _snow_port "${_snow_port}")
foreach(_snow_patch fix-linux.patch fix-lib-name-conflict.patch crashpad-memset-errors-5758170.diff
        fix-std-20.patch ndk-toolchain.diff fix-lib-name-conflict-1.patch)
    string(REPLACE "        ${_snow_patch}" "        \"${_snow_upstream}/${_snow_patch}\""
        _snow_port "${_snow_port}")
endforeach()
if(VCPKG_TARGET_IS_OSX)
    # Honor the triplet's minimum OS and tolerate deprecated SDK APIs in pinned
    # mini_chromium, without relaxing warnings in Snow's own code.
    set(_snow_mac_options " mac_deployment_target=\"${VCPKG_OSX_DEPLOYMENT_TARGET}\" extra_cflags=\"-Wno-error=deprecated-declarations\"")
    string(REPLACE "set(OPTIONS_DBG \"is_debug=true\")"
        "string(APPEND OPTIONS [==[${_snow_mac_options}]==])\nset(OPTIONS_DBG \"is_debug=true\")"
        _snow_port "${_snow_port}")
    string(REPLACE "\${CMAKE_CURRENT_LIST_DIR}" "${_snow_upstream}" _snow_port "${_snow_port}")
    file(WRITE "${CURRENT_BUILDTREES_DIR}/snow-port.cmake" "${_snow_port}")
    include("${CURRENT_BUILDTREES_DIR}/snow-port.cmake")
    return()
endif()

set(_snow_escape [=[
    foreach(_snow_flags VCPKG_COMBINED_C_FLAGS_DEBUG VCPKG_COMBINED_CXX_FLAGS_DEBUG
            VCPKG_COMBINED_C_FLAGS_RELEASE VCPKG_COMBINED_CXX_FLAGS_RELEASE
            VCPKG_COMBINED_SHARED_LINKER_FLAGS_DEBUG VCPKG_COMBINED_SHARED_LINKER_FLAGS_RELEASE
            VCPKG_COMBINED_STATIC_LINKER_FLAGS_DEBUG VCPKG_COMBINED_STATIC_LINKER_FLAGS_RELEASE)
        string(REPLACE "-MD" "-MT" ${_snow_flags} "${${_snow_flags}}")
        string(REPLACE "/MD" "/MT" ${_snow_flags} "${${_snow_flags}}")
        string(REGEX REPLACE "[-/]GL([ ;]|$)" "/GL-\\1" ${_snow_flags} "${${_snow_flags}}")
        string(REPLACE "\\" "\\\\" ${_snow_flags} "${${_snow_flags}}")
        string(REPLACE "\"" "\\\"" ${_snow_flags} "${${_snow_flags}}")
    endforeach()
]=])
string(REPLACE "include(\"\${cmake_vars_file}\")"
    "include(\"\${cmake_vars_file}\")\n${_snow_escape}" _snow_port "${_snow_port}")
string(REPLACE "\${CMAKE_CURRENT_LIST_DIR}" "${_snow_upstream}" _snow_port "${_snow_port}")
set(_snow_build_tools [=[
# Chromium's exception-disabled STL definitions must not participate in the
# application's exception-enabled LTCG compilation. Keep optimized native objects.
vcpkg_replace_string("${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/build/config/BUILD.gn"
    "\"/GL\"" "\"/GL-\"")
vcpkg_replace_string("${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/build/win_helper.py"
    "[vswhere_path, '-latest', '-property', 'installationPath']"
    "[vswhere_path, '-latest', '-products', '*', '-property', 'installationPath']")
]=])
string(REPLACE "vcpkg_gn_configure(" "${_snow_build_tools}\nvcpkg_gn_configure("
    _snow_port "${_snow_port}")
file(WRITE "${CURRENT_BUILDTREES_DIR}/snow-port.cmake" "${_snow_port}")
include("${CURRENT_BUILDTREES_DIR}/snow-port.cmake")
