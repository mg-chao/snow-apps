# A chainloaded toolchain disables vcpkg's vcvars environment by default. Keep
# it enabled: each host/target port needs its own compiler PATH and CRT LIB,
# independently of the target selected by the parent PowerShell process.
set(VCPKG_LOAD_VCVARS_ENV ON)
set(VCPKG_PLATFORM_TOOLSET v145)
set(VCPKG_PLATFORM_TOOLSET_VERSION 14.51)
if(NOT "$ENV{VSINSTALLDIR}" STREQUAL "")
    file(TO_NATIVE_PATH "$ENV{VSINSTALLDIR}" _snow_visual_studio_path)
    string(REGEX REPLACE "[/\\]+$" "" _snow_visual_studio_path
        "${_snow_visual_studio_path}")
    set(VCPKG_VISUAL_STUDIO_PATH "${_snow_visual_studio_path}")
    unset(_snow_visual_studio_path)
endif()

# SDK selection and the native compiler host are shared; PATH, INCLUDE and LIB
# belong to each port's vcvars environment and must never cross architectures.
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED
    WindowsSdkDir WindowsSDKVersion SNOW_MSVC_HOST_ARCHITECTURE)
list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_FILE}")
get_filename_component(_snow_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${_snow_repo_root}/cmake/vcpkg-msvc-145-14.51-toolchain.cmake")
unset(_snow_repo_root)
