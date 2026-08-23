if(NOT WIN32)
    message(FATAL_ERROR "The Snow Apps MSVC toolchain is Windows-only.")
endif()

if(DEFINED _VCPKG_ROOT_DIR)
    set(_snow_vcpkg_root "${_VCPKG_ROOT_DIR}")
else()
    get_filename_component(_snow_vcpkg_root
        "${CMAKE_CURRENT_LIST_DIR}/../.tools/vcpkg" ABSOLUTE)
endif()
set(_snow_vcpkg_windows_toolchain
    "${_snow_vcpkg_root}/scripts/toolchains/windows.cmake")
if(NOT EXISTS "${_snow_vcpkg_windows_toolchain}")
    message(FATAL_ERROR
        "The vcpkg Windows toolchain was not found: ${_snow_vcpkg_windows_toolchain}")
endif()

# A chainloaded toolchain replaces vcpkg's platform toolchain. Load its Windows
# defaults explicitly so the selected triplet still controls /MT versus /MD,
# optimization flags, and variables forwarded to non-CMake build systems.
include("${_snow_vcpkg_windows_toolchain}")

set(_snow_msvc_145_root "$ENV{VCToolsInstallDir}")
if(_snow_msvc_145_root STREQUAL "")
    find_program(_snow_vswhere vswhere.exe
        PATHS
            "$ENV{SystemDrive}/Program Files (x86)/Microsoft Visual Studio/Installer"
            "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer"
    )
    if(_snow_vswhere)
        execute_process(
            COMMAND "${_snow_vswhere}" -latest -products *
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                -property installationPath
            OUTPUT_VARIABLE _snow_vs_install
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        file(GLOB _snow_msvc_candidates
            "${_snow_vs_install}/VC/Tools/MSVC/14.51.*")
        list(SORT _snow_msvc_candidates COMPARE NATURAL ORDER DESCENDING)
        list(LENGTH _snow_msvc_candidates _snow_msvc_count)
        if(_snow_msvc_count GREATER 0)
            list(GET _snow_msvc_candidates 0 _snow_msvc_145_root)
        endif()
    endif()
endif()
if(_snow_msvc_145_root STREQUAL "")
    message(FATAL_ERROR
        "MSVC 14.51 was not found. Set VCToolsInstallDir or install the Visual Studio MSVC x64 component.")
endif()
string(REGEX REPLACE "[/\\]+$" "" _snow_msvc_145_root "${_snow_msvc_145_root}")
file(TO_CMAKE_PATH "${_snow_msvc_145_root}" _snow_msvc_145_root)
set(_snow_msvc_145_bin "${_snow_msvc_145_root}/bin/HostX64/x64")
set(CMAKE_C_COMPILER "${_snow_msvc_145_bin}/cl.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_snow_msvc_145_bin}/cl.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${_snow_msvc_145_bin}/link.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_snow_msvc_145_bin}/lib.exe" CACHE FILEPATH "" FORCE)
set(_snow_windows_sdk_root "$ENV{WindowsSdkDir}")
string(REGEX REPLACE "[/\\]+$" "" _snow_windows_sdk_root "${_snow_windows_sdk_root}")
set(_snow_windows_sdk_version "$ENV{WindowsSDKVersion}")
string(REGEX REPLACE "[/\\]+$" "" _snow_windows_sdk_version "${_snow_windows_sdk_version}")
if(_snow_windows_sdk_root STREQUAL "")
    set(_snow_windows_kits_root "$ENV{SystemDrive}/Program Files (x86)/Windows Kits/10")
    file(GLOB _snow_windows_sdk_includes "${_snow_windows_kits_root}/Include/*")
    list(SORT _snow_windows_sdk_includes COMPARE NATURAL ORDER DESCENDING)
    list(LENGTH _snow_windows_sdk_includes _snow_sdk_count)
    if(_snow_sdk_count GREATER 0)
        list(GET _snow_windows_sdk_includes 0 _snow_windows_sdk_include)
        get_filename_component(_snow_windows_sdk_version "${_snow_windows_sdk_include}" NAME)
        set(_snow_windows_sdk_root "${_snow_windows_kits_root}")
    endif()
else()
    set(_snow_windows_sdk_include "${_snow_windows_sdk_root}/Include/${_snow_windows_sdk_version}")
endif()
if(_snow_windows_sdk_root STREQUAL "" OR NOT IS_DIRECTORY "${_snow_windows_sdk_include}")
    message(FATAL_ERROR "Windows 10 SDK was not found. Set WindowsSdkDir/WindowsSDKVersion or install a Windows SDK.")
endif()
set(CMAKE_MT "${_snow_windows_sdk_root}/bin/${_snow_windows_sdk_version}/x64/mt.exe"
    CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER "${_snow_windows_sdk_root}/bin/${_snow_windows_sdk_version}/x64/rc.exe"
    CACHE FILEPATH "" FORCE)
set(_snow_include_directories
    "${_snow_msvc_145_root}/include"
    "${_snow_windows_sdk_include}/ucrt"
    "${_snow_windows_sdk_include}/shared"
    "${_snow_windows_sdk_include}/um"
    "${_snow_windows_sdk_include}/winrt"
    "${_snow_windows_sdk_include}/cppwinrt")
set(_snow_library_directories
    "${_snow_msvc_145_root}/lib/x64"
    "${_snow_windows_sdk_root}/Lib/${_snow_windows_sdk_version}/um/x64"
    "${_snow_windows_sdk_root}/Lib/${_snow_windows_sdk_version}/ucrt/x64")
set(_snow_include_flags "")
set(_snow_rc_include_flags "")
set(_snow_linker_paths "")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${_snow_include_directories}" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${_snow_include_directories}" CACHE STRING "" FORCE)
set(CMAKE_RC_STANDARD_INCLUDE_DIRECTORIES "${_snow_include_directories}" CACHE STRING "" FORCE)
link_directories(${_snow_library_directories})

# x265 removes <FLAGS> from CMAKE_RC_COMPILE_OBJECT because it does not want
# C/C++ flags or target defines passed to its resource compiler. Keep the
# compiler and Windows SDK include paths in the rule itself so winresrc.h is
# still discoverable after that project-level customization.
set(CMAKE_RC_COMPILE_OBJECT
    "<CMAKE_RC_COMPILER> <DEFINES> <INCLUDES> ${_snow_rc_include_flags} <FLAGS> /fo <OBJECT> <SOURCE>"
    CACHE STRING "Windows resource compiler rule" FORCE)

foreach(_snow_language IN ITEMS C CXX)
    string(FIND "${CMAKE_${_snow_language}_FLAGS}"
        "${_snow_msvc_145_root}/include" _snow_has_msvc_include)
    if(_snow_has_msvc_include EQUAL -1)
        set(CMAKE_${_snow_language}_FLAGS
            "${CMAKE_${_snow_language}_FLAGS} ${_snow_include_flags}"
            CACHE STRING "" FORCE)
    endif()
endforeach()

string(FIND "${CMAKE_RC_FLAGS}" "${_snow_msvc_145_root}/include"
    _snow_has_rc_msvc_include)
if(_snow_has_rc_msvc_include EQUAL -1)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} ${_snow_rc_include_flags}"
        CACHE STRING "" FORCE)
endif()

foreach(_snow_linker_kind IN ITEMS EXE SHARED MODULE)
    string(FIND "${CMAKE_${_snow_linker_kind}_LINKER_FLAGS}"
        "${_snow_msvc_145_root}/lib/x64" _snow_has_msvc_libpath)
    if(_snow_has_msvc_libpath EQUAL -1)
        set(CMAKE_${_snow_linker_kind}_LINKER_FLAGS
            "${CMAKE_${_snow_linker_kind}_LINKER_FLAGS} ${_snow_linker_paths}"
            CACHE STRING "" FORCE)
    endif()
endforeach()

unset(_snow_vcpkg_root)
unset(_snow_vcpkg_windows_toolchain)
unset(_snow_windows_sdk_root)
unset(_snow_windows_sdk_include)
unset(_snow_msvc_libpath)
unset(_snow_sdk_libpath)
unset(_snow_include_flags)
unset(_snow_rc_include_flags)
unset(_snow_linker_paths)
unset(_snow_language)
unset(_snow_has_msvc_include)
unset(_snow_has_rc_msvc_include)
unset(_snow_linker_kind)
unset(_snow_has_msvc_libpath)
unset(_snow_msvc_145_root)
unset(_snow_msvc_145_bin)
unset(_snow_vswhere)
unset(_snow_vs_install)
unset(_snow_msvc_candidates)
unset(_snow_msvc_count)
unset(_snow_windows_kits_root)
unset(_snow_windows_sdk_includes)
unset(_snow_windows_sdk_version)
unset(_snow_windows_sdk_include)
unset(_snow_sdk_count)
