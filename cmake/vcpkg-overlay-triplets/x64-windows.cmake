set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_PROVIDED_FORTRAN ON)
# Host-tool packaging uses dumpbin to copy dependent DLLs next to executables.
# Preserve the bootstrap-prepared MSVC path inside vcpkg's sanitized build env.
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED PATH INCLUDE LIB WindowsSdkDir WindowsSDKVersion)
get_filename_component(_snow_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${_snow_repo_root}/cmake/vcpkg-msvc-145-14.51-toolchain.cmake")
unset(_snow_repo_root)
