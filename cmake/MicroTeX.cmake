include_guard(GLOBAL)

# The upstream build always creates a sample and selects extra platform dependencies. Build
# only its pinned library closure using Snow Shot's resolved Qt kit and CRT configuration.
function(snow_shot_add_latex_renderer)
    if(TARGET snow_shot_latex_renderer)
        return()
    endif()
    include(FetchContent)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_package(tinyxml2 CONFIG REQUIRED)
    FetchContent_Declare(snow_microtex_source
        URL "https://codeload.github.com/NanoMichael/MicroTeX/zip/0e3707f6dafebb121d98b53c64364d16fefe481d"
        URL_HASH "SHA512=4b6e410fd80488b59a190a9e17658dfb26e02c5c0694a64a981146eef38c4756b2f2d5196b926d8ba1b5e8460cf5082ba99dfee7169716b6117edd15b9e68189"
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR snow-shot-no-upstream-build)
    FetchContent_MakeAvailable(snow_microtex_source)

    set(_support "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/microtex")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_support}/patch_source.py" "${_support}/snow_preview.h" "${_support}/snow_preview.cpp")
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/microtex-source")
    set(_patch_key "0e3707f6dafebb121d98b53c64364d16fefe481d")
    foreach(_patch_file IN ITEMS patch_source.py snow_preview.h snow_preview.cpp)
        file(SHA256 "${_support}/${_patch_file}" _patch_hash)
        string(APPEND _patch_key "${_patch_hash}")
    endforeach()
    string(SHA256 _patch_key "${_patch_key}")
    set(_existing_key "")
    if(EXISTS "${_source}/snow-patch-key.txt")
        file(READ "${_source}/snow-patch-key.txt" _existing_key)
    endif()
    if(NOT _patch_key STREQUAL _existing_key)
        file(MAKE_DIRECTORY "${_source}")
        file(COPY "${snow_microtex_source_SOURCE_DIR}/src" DESTINATION "${_source}")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${_support}/patch_source.py" "${_source}"
            RESULT_VARIABLE _patch_result OUTPUT_VARIABLE _patch_output ERROR_VARIABLE _patch_error)
        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR "MicroTeX integration patch failed: ${_patch_output}${_patch_error}")
        endif()
        configure_file("${_support}/snow_preview.h" "${_source}/src/snow_preview.h" COPYONLY)
        configure_file("${_support}/snow_preview.cpp" "${_source}/src/snow_preview.cpp" COPYONLY)
        file(WRITE "${_source}/snow-patch-key.txt" "${_patch_key}")
    endif()

    # These directories contain the complete math implementation and generated metrics.
    # The immutable SHA prevents new upstream files from silently expanding this closure.
    set(_sources)
    foreach(_directory IN ITEMS atom box core fonts utils res/builtin res/font res/parser res/reg res/sym)
        file(GLOB _directory_sources "${_source}/src/${_directory}/*.cpp")
        list(APPEND _sources ${_directory_sources})
    endforeach()
    list(APPEND _sources "${_source}/src/latex.cpp" "${_source}/src/render.cpp"
        "${_source}/src/platform/qt/graphic_qt.cpp" "${_source}/src/snow_preview.cpp")
    add_library(snow_shot_microtex STATIC ${_sources})
    target_include_directories(snow_shot_microtex SYSTEM PUBLIC "${_source}/src")
    target_compile_features(snow_shot_microtex PUBLIC cxx_std_20)
    target_compile_definitions(snow_shot_microtex PRIVATE BUILD_QT)
    target_link_libraries(snow_shot_microtex PUBLIC Qt6::Gui tinyxml2::tinyxml2)
    if(MSVC)
        # /W0 alone does not override the parent directory's individually enabled
        # diagnostics. Disable those upstream-only warnings explicitly as well.
        target_compile_options(snow_shot_microtex PRIVATE /W0 /WX- /utf-8
            /wd4242 /wd4254 /wd4263 /wd4265 /wd4287 /wd4289 /wd4296 /wd4311
            /wd4545 /wd4546 /wd4547 /wd4549 /wd4555 /wd4619 /wd4640 /wd4826
            /wd4905 /wd4906 /wd4928)
        # MSBuild places inherited /w1NNNN options after DisableSpecificWarnings,
        # re-enabling them despite /W0 and /wdNNNN. Remove only explicit warning
        # enables from this dependency; application targets retain strict checks.
        get_target_property(_microtex_options snow_shot_microtex COMPILE_OPTIONS)
        list(FILTER _microtex_options EXCLUDE REGEX "^/(w[1-4]|we)[0-9]+$")
        set_property(TARGET snow_shot_microtex PROPERTY COMPILE_OPTIONS "${_microtex_options}")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(snow_shot_microtex PRIVATE -w)
    endif()
    set_target_properties(snow_shot_microtex PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_CLANG_TIDY "")

    set(_qrc "<RCC>\n  <qresource prefix=\"/snow-shot/microtex\">\n")
    file(GLOB_RECURSE _font_files RELATIVE "${snow_microtex_source_SOURCE_DIR}/res"
        "${snow_microtex_source_SOURCE_DIR}/res/*.ttf")
    file(GLOB _unicode_xml RELATIVE "${snow_microtex_source_SOURCE_DIR}/res"
        "${snow_microtex_source_SOURCE_DIR}/res/greek/*.xml"
        "${snow_microtex_source_SOURCE_DIR}/res/cyrillic/*.xml")
    list(APPEND _font_files ${_unicode_xml})
    list(SORT _font_files)
    foreach(_resource IN LISTS _font_files)
        string(APPEND _qrc "    <file alias=\"${_resource}\">${snow_microtex_source_SOURCE_DIR}/res/${_resource}</file>\n")
    endforeach()
    string(APPEND _qrc "  </qresource>\n</RCC>\n")
    set(_qrc_path "${CMAKE_CURRENT_BINARY_DIR}/snow_shot_microtex.qrc")
    file(CONFIGURE OUTPUT "${_qrc_path}" CONTENT "${_qrc}" @ONLY)
    qt_add_resources(_resource_sources "${_qrc_path}")
    target_sources(snow_shot_microtex PRIVATE ${_resource_sources})

    # FetchContent is outside vcpkg's license collector. Keep original notices in the repository
    # so every Windows/macOS package collector can include them without locating a build tree.
    set(_repo "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    add_library(snow_shot_latex_renderer STATIC
        "${_repo}/snow_shot/include/snow_shot/presentation/screenshotlatexrenderer.h"
        "${_repo}/snow_shot/src/presentation/ocr/screenshotlatexrenderer.cpp")
    target_include_directories(snow_shot_latex_renderer PUBLIC "${_repo}/snow_shot/include")
    target_compile_features(snow_shot_latex_renderer PUBLIC cxx_std_20)
    target_compile_definitions(snow_shot_latex_renderer PRIVATE BUILD_QT)
    target_link_libraries(snow_shot_latex_renderer PUBLIC Qt6::Core Qt6::Gui PRIVATE snow_shot_microtex)
    set_target_properties(snow_shot_latex_renderer PROPERTIES AUTOMOC ON AUTOUIC OFF AUTORCC OFF)
    if(COMMAND snow_apply_strict_warnings)
        snow_apply_strict_warnings(snow_shot_latex_renderer)
    endif()
endfunction()
