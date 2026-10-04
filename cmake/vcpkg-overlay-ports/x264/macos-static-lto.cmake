if(VCPKG_TARGET_IS_OSX AND
   VCPKG_TARGET_ARCHITECTURE STREQUAL "x64" AND
   VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    # x264 selects a 64-byte stack alignment for its x86 assembly. Clang records
    # this in LTO's override-stack-alignment module flag, which conflicts with
    # the app's 16-byte ABI when LLVM merges the modules. Emit native x264
    # objects while preserving its stack alignment, assembly, and optimization.
    foreach(_snow_x264_flags IN ITEMS
        VCPKG_C_FLAGS VCPKG_CXX_FLAGS VCPKG_LINKER_FLAGS
        VCPKG_C_FLAGS_DEBUG VCPKG_CXX_FLAGS_DEBUG VCPKG_LINKER_FLAGS_DEBUG
        VCPKG_C_FLAGS_RELEASE VCPKG_CXX_FLAGS_RELEASE VCPKG_LINKER_FLAGS_RELEASE)
        string(APPEND ${_snow_x264_flags} " -fno-lto")
    endforeach()
endif()
