set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
# MSVC 14.51 ARM64 LTCG can emit stack-cookie epilogues that lose LR and spin
# forever. Dependencies must use native objects as well as the application.
string(APPEND VCPKG_C_FLAGS_RELEASE " /O2 /GL- /Gw /Gy")
string(APPEND VCPKG_CXX_FLAGS_RELEASE " /O2 /GL- /Gw /Gy")
string(APPEND VCPKG_LINKER_FLAGS_RELEASE " /OPT:REF /OPT:ICF")
set(VCPKG_PROVIDED_FORTRAN ON)
include("${CMAKE_CURRENT_LIST_DIR}/../vcpkg-windows-triplet-environment.cmake")
