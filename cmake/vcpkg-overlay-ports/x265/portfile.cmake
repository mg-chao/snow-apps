vcpkg_from_bitbucket(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO multicoreware/x265_git
    REF "${VERSION}"
    SHA512 4b7d71f22f0a7f12ff93f9a01e361df2b80532cd8dac01b5465e63b5d8182f1a05c0289ad95f3aa972c963aa6cd90cb3d594f8b9a96f556a006cf7e1bdd9edda
    HEAD_REF master
    PATCHES
        "${VCPKG_ROOT_DIR}/ports/x265/disable-install-pdb.patch"
        "${VCPKG_ROOT_DIR}/ports/x265/version.patch"
        "${VCPKG_ROOT_DIR}/ports/x265/linkage.diff"
        "${VCPKG_ROOT_DIR}/ports/x265/pkgconfig.diff"
        "${VCPKG_ROOT_DIR}/ports/x265/pthread.diff"
        "${VCPKG_ROOT_DIR}/ports/x265/compiler-target.diff"
        "${VCPKG_ROOT_DIR}/ports/x265/neon.diff"
        "${VCPKG_ROOT_DIR}/ports/x265/fix-cmake-4.patch"
        macos-arm64-deployment-target.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS OPTIONS
    FEATURES
        tool   ENABLE_CLI
)

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86" OR VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    vcpkg_find_acquire_program(NASM)
    list(APPEND OPTIONS "-DNASM_EXECUTABLE=${NASM}")
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "static" AND NOT VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_OSX)
        # x265 doesn't create sufficient PIC for asm, breaking usage
        # in shared libs, e.g. the libheif gdk pixbuf plugin.
        # Users can override this in custom triplets.
        list(APPEND OPTIONS "-DENABLE_ASSEMBLY=OFF")
    endif()
elseif(VCPKG_TARGET_IS_WINDOWS)
    list(APPEND OPTIONS "-DENABLE_ASSEMBLY=OFF")
endif()

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" ENABLE_SHARED)

if("main10" IN_LIST FEATURES)
    # Build the namespace-isolated Main10 implementation first. It has no
    # public C API and is linked into the normal 8-bit API dispatcher below.
    # Separate trees keep configuration/header/assembly outputs from colliding.
    set(SNOW_X265_BUILDTREES_DIR "${CURRENT_BUILDTREES_DIR}")
    set(SNOW_X265_MAIN10_DIR "${CURRENT_BUILDTREES_DIR}/main10")
    set(CURRENT_BUILDTREES_DIR "${SNOW_X265_MAIN10_DIR}")
    file(MAKE_DIRECTORY "${CURRENT_BUILDTREES_DIR}")
    vcpkg_cmake_configure(
        SOURCE_PATH "${SOURCE_PATH}/source"
        OPTIONS
            ${OPTIONS}
            -DENABLE_CLI=OFF
            -DENABLE_SHARED=OFF
            -DENABLE_PIC=ON
            -DENABLE_LIBNUMA=OFF
            -DHIGH_BIT_DEPTH=ON
            -DMAIN12=OFF
            -DEXPORT_C_API=OFF
            "-DVERSION=${VERSION}"
        MAYBE_UNUSED_VARIABLES ENABLE_LIBNUMA
    )
    vcpkg_cmake_build(TARGET x265-static LOGFILE_BASE build-main10)
    set(CURRENT_BUILDTREES_DIR "${SNOW_X265_BUILDTREES_DIR}")
    if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
        set(SNOW_X265_MAIN10_ARCHIVE x265-static.lib)
        set(SNOW_X265_MAIN10_INSTALLED x265-main10.lib)
    else()
        set(SNOW_X265_MAIN10_ARCHIVE libx265.a)
        set(SNOW_X265_MAIN10_INSTALLED libx265-main10.a)
    endif()
    list(APPEND OPTIONS -DLINKED_10BIT=ON -DHIGH_BIT_DEPTH=OFF)
    set(SNOW_X265_MAIN10_RELEASE
        "-DEXTRA_LIB=${SNOW_X265_MAIN10_DIR}/${TARGET_TRIPLET}-rel/${SNOW_X265_MAIN10_ARCHIVE}")
    set(SNOW_X265_MAIN10_DEBUG
        "-DEXTRA_LIB=${SNOW_X265_MAIN10_DIR}/${TARGET_TRIPLET}-dbg/${SNOW_X265_MAIN10_ARCHIVE}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/source"
    OPTIONS
        ${OPTIONS}
        -DENABLE_SHARED=${ENABLE_SHARED}
        -DENABLE_PIC=ON
        -DENABLE_LIBNUMA=OFF
        "-DVERSION=${VERSION}"
    OPTIONS_RELEASE
        ${SNOW_X265_MAIN10_RELEASE}
    OPTIONS_DEBUG
        -DENABLE_CLI=OFF
        ${SNOW_X265_MAIN10_DEBUG}
    MAYBE_UNUSED_VARIABLES
        ENABLE_LIBNUMA
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

if("main10" IN_LIST FEATURES AND VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    # Static archives do not absorb target_link_libraries dependencies. Ship
    # the namespace-isolated archive and expose it to static pkg-config users.
    foreach(SNOW_X265_CONFIG IN ITEMS release debug)
        if(NOT VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL SNOW_X265_CONFIG)
            if(SNOW_X265_CONFIG STREQUAL "debug")
                set(SNOW_X265_CONFIG_PREFIX debug/)
                set(SNOW_X265_CONFIG_SUFFIX dbg)
            else()
                set(SNOW_X265_CONFIG_PREFIX "")
                set(SNOW_X265_CONFIG_SUFFIX rel)
            endif()
            file(INSTALL
                "${SNOW_X265_MAIN10_DIR}/${TARGET_TRIPLET}-${SNOW_X265_CONFIG_SUFFIX}/${SNOW_X265_MAIN10_ARCHIVE}"
                DESTINATION "${CURRENT_PACKAGES_DIR}/${SNOW_X265_CONFIG_PREFIX}lib"
                RENAME "${SNOW_X265_MAIN10_INSTALLED}")
            vcpkg_replace_string(
                "${CURRENT_PACKAGES_DIR}/${SNOW_X265_CONFIG_PREFIX}lib/pkgconfig/x265.pc"
                "Libs.private: " "Libs.private: -lx265-main10 ")
        endif()
    endforeach()
endif()
if("main10" IN_LIST FEATURES)
    file(INSTALL "${CURRENT_PORT_DIR}/snow-main10-capability.json"
        DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
endif()
vcpkg_fixup_pkgconfig()

if("tool" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES x265 AUTO_CLEAN)
endif()

if(VCPKG_TARGET_IS_WINDOWS AND VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/x265.h" "#ifdef X265_API_IMPORTS" "#if 1")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
