if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
    # Snow Shot links the static Qt kit and therefore uses the static CRT even
    # in its debug preset. zxing-cpp is linked into that executable directly.
    set(VCPKG_CRT_LINKAGE static)
endif()

# The auto-generated GitHub source archives of this repository are unusable
# (their submodule content is missing), so consume the tarball attached to
# the v3.1.1 release instead.
vcpkg_download_distfile(
    ARCHIVE
    URLS "https://github.com/zxing-cpp/zxing-cpp/releases/download/v${VERSION}/zxing-cpp-${VERSION}.tar.gz"
    FILENAME "zxing-cpp-${VERSION}.tar.gz"
    SHA512 154a9ed31ef8a9b442f552ce479fd2beb1ebc5c6789fcffa2fbcbbc9ec2bbd57aabcc05cb1dd96d1d72459018524fc1f976dd85d95f2da0bc074fd5fe76ae8dc
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DZXING_EXAMPLES=OFF
        -DZXING_UNIT_TESTS=OFF
        -DZXING_WRITERS=OFF
        # The C-API wrapper test harness pulls stb via FetchContent; the C++
        # reader API is all Snow Shot consumes.
        -DZXING_C_API=OFF
    OPTIONS_RELEASE
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    OPTIONS_DEBUG
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug
)
vcpkg_cmake_install()
vcpkg_fixup_pkgconfig()

vcpkg_cmake_config_fixup(
    CONFIG_PATH lib/cmake/ZXing
    PACKAGE_NAME ZXing
)

file(READ "${CURRENT_PACKAGES_DIR}/share/ZXing/ZXingConfig.cmake" _contents)
file(WRITE "${CURRENT_PACKAGES_DIR}/share/ZXing/ZXingConfig.cmake" "
include(CMakeFindDependencyMacro)
find_dependency(Threads)
${_contents}")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
