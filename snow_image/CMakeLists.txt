cmake_minimum_required(VERSION 3.25)

project(snow_image VERSION 3.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SNOW_IMAGE_BUILD_TESTS "Build snow_image tests." ON)
option(SNOW_IMAGE_BUILD_SHARED "Build the shared snow_image library." ON)
option(SNOW_IMAGE_BUILD_STATIC "Build the static snow_image library." ON)
option(SNOW_IMAGE_STRICT_COMPILE "Treat snow_image warnings as errors." ON)
option(SNOW_IMAGE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer." OFF)
option(SNOW_IMAGE_BUILD_FUZZERS "Build libFuzzer codec entry points." OFF)
option(SNOW_IMAGE_BUILD_BENCHMARKS "Build the command-line codec benchmark." OFF)
option(SNOW_IMAGE_ENABLE_INSTALL "Install Snow Image as a standalone package." ON)
option(SNOW_IMAGE_CODEC_PNG "Enable the libpng codec." OFF)
option(SNOW_IMAGE_CODEC_JPEG "Enable the libjpeg-turbo codec." OFF)
option(SNOW_IMAGE_CODEC_GIF "Enable the giflib codec." OFF)
option(SNOW_IMAGE_CODEC_ICON "Enable ICO and CUR container codecs." OFF)
option(SNOW_IMAGE_CODEC_XBITMAP "Enable XBM and XPM codecs." OFF)
option(SNOW_IMAGE_CODEC_SVG "Enable SVG and SVGZ codecs." OFF)
option(SNOW_IMAGE_CODEC_HEIF "Enable HEIF, HEIC, HIF, and AVIF codecs." OFF)
option(SNOW_IMAGE_CODEC_JXL "Enable JPEG XL codecs." OFF)
option(SNOW_IMAGE_CODEC_EXR "Enable OpenEXR codecs." OFF)
option(SNOW_IMAGE_CODEC_WEBP "Enable WebP codecs." OFF)
set(SNOW_IMAGE_PROFILE "full" CACHE STRING "Codec profile: full or redistributable.")
set_property(CACHE SNOW_IMAGE_PROFILE PROPERTY STRINGS full redistributable)
set(SNOW_IMAGE_DEFAULT_LINKAGE "static" CACHE STRING "Default snow_image target linkage.")
set_property(CACHE SNOW_IMAGE_DEFAULT_LINKAGE PROPERTY STRINGS static shared)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ProjectWarnings.cmake")
if(SNOW_IMAGE_STRICT_COMPILE)
    snow_enable_strict_warnings()
endif()
snow_enable_clang_tidy()

if(NOT SNOW_IMAGE_BUILD_STATIC AND NOT SNOW_IMAGE_BUILD_SHARED)
    message(FATAL_ERROR "At least one of SNOW_IMAGE_BUILD_STATIC or SNOW_IMAGE_BUILD_SHARED must be ON.")
endif()
if(NOT SNOW_IMAGE_PROFILE STREQUAL "full" AND
   NOT SNOW_IMAGE_PROFILE STREQUAL "redistributable")
    message(FATAL_ERROR "SNOW_IMAGE_PROFILE must be full or redistributable.")
endif()

set(SNOW_IMAGE_PUBLIC_HEADERS
    include/snow/image/codec.h
    include/snow/image/document.h
    include/snow/image/export.h
    include/snow/image/format.h
    include/snow/image/image.h
    include/snow/image/io.h
    include/snow/image/processing.h
    include/snow/image/raster.h
    include/snow/image/raster_conversion.h
    include/snow/image/result.h
    include/snow/image/resource_estimate.h
    include/snow/image/resource_plan.h
    include/snow/image/service.h
    include/snow/image/version.h
)

set(SNOW_IMAGE_SOURCES
    src/codec_registry.cpp
    src/codec_registry.h
    src/compression_backend.cpp
    src/codecs/bmp_codec.cpp
    src/codecs/bmp_codec.h
    src/codecs/pnm_codec.cpp
    src/codecs/pnm_codec.h
    src/format.cpp
    src/exif_orientation.cpp
    src/exif_orientation.h
    src/image.cpp
    src/io.cpp
    src/planar_raster_io.cpp
    src/planar_raster_io.h
    src/processing.cpp
    src/raster.cpp
    src/raster_buffer.cpp
    src/raster_conversion.cpp
    src/resource_estimate.cpp
    src/resource_plan.cpp
    src/service.cpp
)

set(SNOW_IMAGE_PRIVATE_LIBRARIES)
set(SNOW_IMAGE_PRIVATE_DEFINITIONS)
set(SNOW_IMAGE_PRIVATE_INCLUDE_DIRS)

set(SNOW_IMAGE_LIBYUV_AVAILABLE OFF)
find_package(libyuv CONFIG QUIET)
if(TARGET yuv)
    set(SNOW_IMAGE_LIBYUV_AVAILABLE ON)
    list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES yuv)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_LIBYUV=1)
endif()

if(SNOW_IMAGE_ENABLE_SANITIZERS)
    add_library(snow_image_sanitizers INTERFACE)
    if(MSVC)
        target_compile_options(snow_image_sanitizers INTERFACE /fsanitize=address)
        target_link_options(snow_image_sanitizers INTERFACE /INFERASANLIBS)
    else()
        target_compile_options(snow_image_sanitizers INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(snow_image_sanitizers INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
    endif()
endif()

if(SNOW_IMAGE_CODEC_PNG)
    find_package(PNG REQUIRED)
    find_package(ZLIB REQUIRED)
    include(CheckCXXSourceCompiles)
    set(_snow_image_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_LIBRARIES ZLIB::ZLIB)
    check_cxx_source_compiles(
        "#include <zlib.h>\n#ifndef ZLIBNG_VERSION\n#error zlib-ng required\n#endif\nint main() { return 0; }"
        SNOW_IMAGE_HAS_ZLIB_NG_COMPAT)
    set(CMAKE_REQUIRED_LIBRARIES "${_snow_image_required_libraries}")
    unset(_snow_image_required_libraries)
    if(NOT SNOW_IMAGE_HAS_ZLIB_NG_COMPAT)
        message(FATAL_ERROR
            "PNG support requires zlib-ng compatibility mode. Configure through the repository vcpkg overlay.")
    endif()
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/png_codec.cpp src/codecs/png_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES PNG::PNG ZLIB::ZLIB)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_PNG=1)
endif()

if(SNOW_IMAGE_CODEC_JPEG)
    find_package(libjpeg-turbo CONFIG REQUIRED)
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/jpeg_codec.cpp src/codecs/jpeg_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES
        $<IF:$<TARGET_EXISTS:libjpeg-turbo::jpeg>,libjpeg-turbo::jpeg,libjpeg-turbo::jpeg-static>
        $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
    )
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_JPEG=1)
endif()

if(SNOW_IMAGE_CODEC_GIF)
    find_package(GIF REQUIRED)
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/gif_codec.cpp src/codecs/gif_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES GIF::GIF)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_GIF=1)
endif()

if(SNOW_IMAGE_CODEC_ICON)
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/icon_codec.cpp src/codecs/icon_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_ICON=1)
endif()

if(SNOW_IMAGE_CODEC_XBITMAP)
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/xbitmap_codec.cpp src/codecs/xbitmap_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_XBITMAP=1)
endif()

if(SNOW_IMAGE_CODEC_SVG)
    find_package(lunasvg CONFIG REQUIRED)
    find_package(ZLIB REQUIRED)
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/svg_codec.cpp src/codecs/svg_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES lunasvg::lunasvg ZLIB::ZLIB)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_SVG=1)
endif()

if(SNOW_IMAGE_CODEC_HEIF)
    find_package(libheif CONFIG REQUIRED)
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/heif_codec.cpp src/codecs/heif_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES heif)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_HEIF=1)
endif()

if(SNOW_IMAGE_CODEC_JXL)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(JXL QUIET IMPORTED_TARGET libjxl)
        pkg_check_modules(JXL_THREADS QUIET IMPORTED_TARGET libjxl_threads)
    endif()
    if(NOT WIN32 AND TARGET PkgConfig::JXL AND TARGET PkgConfig::JXL_THREADS)
        list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES PkgConfig::JXL PkgConfig::JXL_THREADS)
    else()
        find_package(hwy CONFIG REQUIRED)
        find_package(lcms2 CONFIG REQUIRED)
        find_package(unofficial-brotli CONFIG REQUIRED)
        find_path(SNOW_IMAGE_JXL_INCLUDE_DIR NAMES jxl/decode.h REQUIRED)
        find_library(SNOW_IMAGE_JXL_LIBRARY NAMES jxl REQUIRED)
        find_library(SNOW_IMAGE_JXL_CMS_LIBRARY NAMES jxl_cms REQUIRED)
        find_library(SNOW_IMAGE_JXL_THREADS_LIBRARY NAMES jxl_threads REQUIRED)
        if(NOT TARGET snow_image_dependencies::jxl_cms)
            add_library(snow_image_dependencies::jxl_cms UNKNOWN IMPORTED)
            set_target_properties(snow_image_dependencies::jxl_cms PROPERTIES
                IMPORTED_LOCATION "${SNOW_IMAGE_JXL_CMS_LIBRARY}"
                INTERFACE_COMPILE_DEFINITIONS JXL_CMS_STATIC_DEFINE
                INTERFACE_LINK_LIBRARIES lcms2::lcms2
            )
        endif()
        if(NOT TARGET snow_image_dependencies::jxl)
            add_library(snow_image_dependencies::jxl UNKNOWN IMPORTED)
            set_target_properties(snow_image_dependencies::jxl PROPERTIES
                IMPORTED_LOCATION "${SNOW_IMAGE_JXL_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${SNOW_IMAGE_JXL_INCLUDE_DIR}"
                INTERFACE_COMPILE_DEFINITIONS JXL_STATIC_DEFINE
                INTERFACE_LINK_LIBRARIES
                    "hwy::hwy;unofficial::brotli::brotlienc;unofficial::brotli::brotlidec;snow_image_dependencies::jxl_cms"
            )
        endif()
        if(NOT TARGET snow_image_dependencies::jxl_threads)
            add_library(snow_image_dependencies::jxl_threads UNKNOWN IMPORTED)
            set_target_properties(snow_image_dependencies::jxl_threads PROPERTIES
                IMPORTED_LOCATION "${SNOW_IMAGE_JXL_THREADS_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${SNOW_IMAGE_JXL_INCLUDE_DIR}"
                INTERFACE_COMPILE_DEFINITIONS JXL_THREADS_STATIC_DEFINE
            )
        endif()
        list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES
            snow_image_dependencies::jxl
            snow_image_dependencies::jxl_threads
        )
    endif()
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/jxl_codec.cpp src/codecs/jxl_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_JXL=1)
endif()

if(SNOW_IMAGE_CODEC_EXR)
    find_package(OpenEXR CONFIG REQUIRED)
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/exr_codec.cpp src/codecs/exr_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES OpenEXR::OpenEXR)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_EXR=1)
endif()

if(SNOW_IMAGE_CODEC_WEBP)
    find_package(WebP CONFIG REQUIRED)
    list(APPEND SNOW_IMAGE_SOURCES src/codecs/webp_codec.cpp src/codecs/webp_codec.h)
    list(APPEND SNOW_IMAGE_PRIVATE_LIBRARIES WebP::webp WebP::webpdemux WebP::libwebpmux)
    list(APPEND SNOW_IMAGE_PRIVATE_DEFINITIONS SNOW_IMAGE_HAS_WEBP=1)
endif()

function(snow_image_configure_target target_name linkage)
    add_library(${target_name} ${linkage} ${SNOW_IMAGE_PUBLIC_HEADERS} ${SNOW_IMAGE_SOURCES})
    target_compile_features(${target_name} PUBLIC cxx_std_20)
    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${SNOW_IMAGE_PRIVATE_INCLUDE_DIRS}
    )
    target_compile_definitions(${target_name}
        PRIVATE
            SNOW_IMAGE_BUILDING_LIBRARY=1
            ${SNOW_IMAGE_PRIVATE_DEFINITIONS}
            $<$<STREQUAL:${SNOW_IMAGE_PROFILE},full>:SNOW_IMAGE_FULL_PROFILE=1>
    )
    target_link_libraries(${target_name} PRIVATE ${SNOW_IMAGE_PRIVATE_LIBRARIES})
    if(SNOW_IMAGE_ENABLE_SANITIZERS)
        target_link_libraries(${target_name} PUBLIC snow_image_sanitizers)
    endif()
    set_target_properties(${target_name} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
    if(linkage STREQUAL "SHARED")
        target_compile_definitions(${target_name} PUBLIC SNOW_IMAGE_SHARED=1)
    endif()
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /utf-8
            /permissive-
            /Zc:__cplusplus
            /EHsc
            /W4
        )
        if(linkage STREQUAL "SHARED")
            # The public C++20 API intentionally owns standard-library values. Consumers and
            # the DLL use the same MSVC runtime; cross-toolchain ABI is not promised.
            target_compile_options(${target_name} PRIVATE /wd4251)
        endif()
        if(SNOW_IMAGE_STRICT_COMPILE)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        if(SNOW_IMAGE_STRICT_COMPILE)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(SNOW_IMAGE_BUILD_STATIC)
    snow_image_configure_target(snow_image_static STATIC)
    target_precompile_headers(snow_image_static PRIVATE
        <algorithm>
        <array>
        <cstdint>
        <memory>
        <optional>
        <span>
        <string>
        <vector>
    )
    snow_enable_unity_build(snow_image_static)
    add_library(snow_image::snow_image_static ALIAS snow_image_static)
endif()
if(SNOW_IMAGE_BUILD_SHARED)
    snow_image_configure_target(snow_image_shared SHARED)
    add_library(snow_image::snow_image_shared ALIAS snow_image_shared)
endif()

add_library(snow_image INTERFACE)
if(SNOW_IMAGE_DEFAULT_LINKAGE STREQUAL "shared")
    if(NOT TARGET snow_image_shared)
        message(FATAL_ERROR "SNOW_IMAGE_DEFAULT_LINKAGE=shared requires SNOW_IMAGE_BUILD_SHARED=ON.")
    endif()
    target_link_libraries(snow_image INTERFACE snow_image_shared)
else()
    if(NOT TARGET snow_image_static)
        message(FATAL_ERROR "SNOW_IMAGE_DEFAULT_LINKAGE=static requires SNOW_IMAGE_BUILD_STATIC=ON.")
    endif()
    target_link_libraries(snow_image INTERFACE snow_image_static)
endif()
add_library(snow_image::snow_image ALIAS snow_image)

if(SNOW_IMAGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SNOW_IMAGE_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
if(SNOW_IMAGE_BUILD_BENCHMARKS)
    add_subdirectory(tools)
endif()

if(SNOW_IMAGE_ENABLE_INSTALL)
    set(SNOW_IMAGE_INSTALL_TARGETS snow_image)
    if(TARGET snow_image_sanitizers)
        list(APPEND SNOW_IMAGE_INSTALL_TARGETS snow_image_sanitizers)
    endif()
    if(TARGET snow_image_static)
        list(APPEND SNOW_IMAGE_INSTALL_TARGETS snow_image_static)
    endif()
    if(TARGET snow_image_shared)
        list(APPEND SNOW_IMAGE_INSTALL_TARGETS snow_image_shared)
    endif()

    install(TARGETS ${SNOW_IMAGE_INSTALL_TARGETS}
        EXPORT snow_imageTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT snow_imageTargets
        FILE snow_imageTargets.cmake
        NAMESPACE snow_image::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/snow_image
    )

    configure_package_config_file(
        cmake/snow_imageConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/snow_imageConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/snow_image
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/snow_imageConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/snow_imageConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/snow_imageConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/snow_image
    )
    install(FILES LICENSE COPYRIGHT
        DESTINATION ${CMAKE_INSTALL_DATADIR}/snow-image/licenses
    )
endif()
