add_executable(snow_image_viewer_tests viewer_tests.cpp)
target_link_libraries(snow_image_viewer_tests
    PRIVATE
        snow_image_viewer_lib
        Qt6::Test
)
target_compile_features(snow_image_viewer_tests PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(snow_image_viewer_tests PRIVATE
        /FS /utf-8 /permissive- /Zc:__cplusplus /W4
    )
    if(SNOW_IMAGE_VIEWER_STRICT_COMPILE)
        target_compile_options(snow_image_viewer_tests PRIVATE /WX)
    endif()
endif()

add_test(NAME snow_image_viewer_tests COMMAND snow_image_viewer_tests)

add_executable(snow_image_viewer_generated_icon_pack_tests
    generated_icon_pack_tests.cpp
)
target_link_libraries(snow_image_viewer_generated_icon_pack_tests
    PRIVATE
        snow_image_viewer_lib
)
target_compile_features(snow_image_viewer_generated_icon_pack_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(snow_image_viewer_generated_icon_pack_tests PRIVATE
        /FS /utf-8 /permissive- /Zc:__cplusplus /W4
    )
endif()
add_test(
    NAME snow_image_viewer_generated_icon_pack_tests
    COMMAND snow_image_viewer_generated_icon_pack_tests
)
if(WIN32)
    set_tests_properties(snow_image_viewer_generated_icon_pack_tests PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
else()
    set_tests_properties(snow_image_viewer_generated_icon_pack_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

add_test(
    NAME snow_image_viewer_edit_mode_performance
    COMMAND $<TARGET_FILE:snow_image_viewer>
        --edit-performance-test
        --edit-performance-output
        $<TARGET_FILE_DIR:snow_image_viewer>/edit-mode-performance.json
        ${CMAKE_SOURCE_DIR}/test-images/preview.png
)
set_tests_properties(snow_image_viewer_edit_mode_performance PROPERTIES
    LABELS "performance;gui"
    TIMEOUT 130
)

if(WIN32)
    add_executable(snow_image_viewer_rhi_tests
        rhi_resize_tests.cpp
        ../resources/resources.qrc
    )
    target_link_libraries(snow_image_viewer_rhi_tests
        PRIVATE snow_image_viewer_lib Qt6::Test
    )
    target_compile_features(snow_image_viewer_rhi_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(snow_image_viewer_rhi_tests PRIVATE
            /FS /utf-8 /permissive- /Zc:__cplusplus /W4
        )
        if(SNOW_IMAGE_VIEWER_STRICT_COMPILE)
            target_compile_options(snow_image_viewer_rhi_tests PRIVATE /WX)
        endif()
    endif()
    add_test(NAME snow_image_viewer_rhi_tests
             COMMAND snow_image_viewer_rhi_tests --backend=d3d11)
    add_test(NAME snow_image_viewer_rhi_opengl_tests
             COMMAND snow_image_viewer_rhi_tests --backend=opengl --allow-unavailable)
    add_test(NAME snow_image_viewer_rhi_vulkan_tests
             COMMAND snow_image_viewer_rhi_tests --backend=vulkan --allow-unavailable)
    set_tests_properties(
        snow_image_viewer_rhi_tests
        snow_image_viewer_rhi_opengl_tests
        snow_image_viewer_rhi_vulkan_tests
        PROPERTIES
            ENVIRONMENT "QT_PLUGIN_PATH=${Qt6_DIR}/../../../plugins"
    )
    set_tests_properties(
        snow_image_viewer_rhi_opengl_tests
        snow_image_viewer_rhi_vulkan_tests
        PROPERTIES SKIP_RETURN_CODE 77
    )
endif()

add_custom_command(TARGET snow_image_viewer_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:snow_image_edit_worker>
        $<TARGET_FILE_DIR:snow_image_viewer_tests>
)
