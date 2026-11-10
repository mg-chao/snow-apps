include_guard(GLOBAL)

if(WIN32)
    # Compiler initialization must happen at file scope so its CMake rule
    # variables remain available when the root project generates native files.
    enable_language(C)
endif()

# Only the reduced, public-domain C decoder is linked into the temporary
# installer helper. The SDK's native 7zr encoder is a build-time tool.
function(snow_shot_add_installer_payload)
    if(TARGET snow-shot-payload-extractor)
        return()
    endif()
    if(NOT WIN32)
        message(FATAL_ERROR "The Snow Shot installer payload helper requires Windows.")
    endif()
    include(FetchContent)
    FetchContent_Declare(snow_lzma_sdk
        URL "https://github.com/ip7z/7zip/releases/download/26.05/lzma2605.7z"
        URL_HASH "SHA256=668fda469b7f584f246008cf28359c66ef98d65fafb3b22a52672eebe6970c28"
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR snow-no-upstream-build)
    FetchContent_MakeAvailable(snow_lzma_sdk)

    set(_host_arch "$ENV{PROCESSOR_ARCHITEW6432}")
    if(_host_arch STREQUAL "")
        set(_host_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    string(TOLOWER "${_host_arch}" _host_arch)
    if(_host_arch MATCHES "arm64|aarch64")
        set(_encoder "${snow_lzma_sdk_SOURCE_DIR}/bin/arm64/7zr.exe")
    elseif(_host_arch MATCHES "amd64|x86_64|x64")
        set(_encoder "${snow_lzma_sdk_SOURCE_DIR}/bin/x64/7zr.exe")
    else()
        set(_encoder "${snow_lzma_sdk_SOURCE_DIR}/bin/7zr.exe")
    endif()
    set(SNOW_SHOT_PAYLOAD_ENCODER "${_encoder}" CACHE FILEPATH
        "Pinned LZMA SDK encoder used only while packaging Windows installers." FORCE)
    set(SNOW_SHOT_PAYLOAD_SDK_LICENSE "${snow_lzma_sdk_SOURCE_DIR}/DOC/lzma-sdk.txt"
        CACHE FILEPATH "Original public-domain LZMA SDK notice." FORCE)

    set(_sdk "${snow_lzma_sdk_SOURCE_DIR}/C")
    set(_sources
        7zAlloc.c 7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zFile.c 7zStream.c
        Bcj2.c Bra.c Bra86.c BraIA64.c CpuArch.c Delta.c Lzma2Dec.c LzmaDec.c)
    list(TRANSFORM _sources PREPEND "${_sdk}/")
    # C is enabled after the root project establishes its C++ IPO policy. Do not
    # inherit an IPO requirement for this small, separately compiled C closure.
    set(_saved_ipo "${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
    set(_saved_release_ipo "${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE}")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
    add_library(snow_installer_lzma_decoder STATIC ${_sources})
    target_include_directories(snow_installer_lzma_decoder SYSTEM PUBLIC "${_sdk}")
    target_compile_definitions(snow_installer_lzma_decoder PRIVATE Z7_EXTRACT_ONLY)
    set_target_properties(snow_installer_lzma_decoder PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_CLANG_TIDY ""
        INTERPROCEDURAL_OPTIMIZATION FALSE INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION "${_saved_ipo}")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE "${_saved_release_ipo}")
    add_executable(snow-shot-payload-extractor WIN32
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../snow_shot/packaging/payload/extractor.cpp")
    target_compile_features(snow-shot-payload-extractor PRIVATE cxx_std_20)
    target_compile_definitions(snow-shot-payload-extractor PRIVATE UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(snow-shot-payload-extractor PRIVATE
        snow_installer_lzma_decoder bcrypt shell32)
    set_target_properties(snow-shot-payload-extractor PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/installer-payload/$<CONFIG>")
    if(MSVC)
        # Parent warning groups are intentionally not imposed on pinned upstream C.
        get_target_property(_sdk_options snow_installer_lzma_decoder COMPILE_OPTIONS)
        if(_sdk_options)
            list(FILTER _sdk_options EXCLUDE REGEX "^/(w[1-4]|we)[0-9]+$")
            set_property(TARGET snow_installer_lzma_decoder PROPERTY COMPILE_OPTIONS "${_sdk_options}")
        endif()
        target_compile_options(snow_installer_lzma_decoder PRIVATE /W0 /WX-
            $<$<CONFIG:Release>:/O1> $<$<CONFIG:Release>:/Os>
            $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw>)
        target_compile_options(snow-shot-payload-extractor PRIVATE /W4 /WX /permissive- /utf-8
            /Zc:__cplusplus /Zc:preprocessor
            $<$<CONFIG:Release>:/O1> $<$<CONFIG:Release>:/Os>
            $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw>)
        target_link_options(snow-shot-payload-extractor PRIVATE
            $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/INCREMENTAL:NO>)
    endif()
endfunction()
