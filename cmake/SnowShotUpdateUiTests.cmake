# Focused offscreen checks also work with the production Qt kit, which omits QtTest.
if(SNOW_SHOT_BUILD_UPDATE_TESTS AND NOT SNOW_SHOT_BUILD_TESTS)
    add_executable(snow-shot-update-about-tests
        tests/about_page_tests.cpp
        include/snow_shot/presentation/mainwindow.h
        include/snow_shot/presentation/components/contentcardwidget.h
        include/snow_shot/presentation/components/maincontentheaderwidget.h
        include/snow_shot/presentation/components/sidebarwidget.h
        include/snow_shot/presentation/components/titlebarwidget.h
        src/presentation/mainwindow.cpp
        src/presentation/components/contentcardwidget.cpp
        src/presentation/components/maincontentheaderwidget.cpp
        src/presentation/components/sidebarwidget.cpp
        src/presentation/components/titlebarwidget.cpp
        src/presentation/services/screenshotclipboardservice.cpp
        src/platform/windows/windowchrome.cpp)
    target_link_libraries(snow-shot-update-about-tests PRIVATE snow_shot_settings snow_shot_image_codec)
    if(WIN32)
        target_link_libraries(snow-shot-update-about-tests PRIVATE PkgConfig::SNOW_SHOT_FFMPEG)
    endif()
    target_compile_definitions(snow-shot-update-about-tests PRIVATE
        SNOW_SHOT_TEST_VERSION="${SNOW_SHOT_VERSION}"
        SNOW_SHOT_TEST_WEBSITE_URL="${SNOW_SHOT_WEBSITE_URL}"
        SNOW_SHOT_TEST_PROJECT_URL="${SNOW_SHOT_PROJECT_URL}"
        SNOW_SHOT_TEST_TRANSLATIONS_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(snow-shot-update-about-tests snow_shot_release_translations)
    snow_shot_import_offscreen_platform(snow-shot-update-about-tests)
    add_test(NAME snow-shot-update-about-tests COMMAND snow-shot-update-about-tests)
    set_tests_properties(snow-shot-update-about-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "unit;windows" TIMEOUT 120)
    add_executable(snow-shot-update-settings-tests tests/settings_catalog_tests.cpp)
    target_link_libraries(snow-shot-update-settings-tests PRIVATE snow_shot_settings_search Qt6::Core)
    add_test(NAME snow-shot-update-settings-tests COMMAND snow-shot-update-settings-tests)
    set_tests_properties(snow-shot-update-settings-tests PROPERTIES LABELS "unit;windows" TIMEOUT 60)
endif()
