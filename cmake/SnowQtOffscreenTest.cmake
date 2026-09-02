# Offscreen QPA for Qt test executables.
#
# Shared Qt loads qoffscreen from QT_QPA_PLATFORM_PLUGIN_PATH. Static Qt
# registers platform plugins at link time, so tests that request
# QT_QPA_PLATFORM=offscreen must import Qt6::QOffscreenIntegrationPlugin
# instead of looking for a DLL in plugins/platforms.

function(snow_shot_offscreen_qpa_environment out_variable)
    if(QT_FEATURE_static)
        set(${out_variable} "QT_QPA_PLATFORM=offscreen" PARENT_SCOPE)
    else()
        set(${out_variable}
            "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
            PARENT_SCOPE)
    endif()
endfunction()

function(snow_shot_import_offscreen_platform target)
    if(NOT TARGET "${target}")
        return()
    endif()
    if(NOT QT_FEATURE_static)
        return()
    endif()
    if(NOT TARGET Qt6::QOffscreenIntegrationPlugin)
        message(FATAL_ERROR
            "${target} requests the offscreen QPA, but this Qt kit does not "
            "provide Qt6::QOffscreenIntegrationPlugin.")
    endif()
    target_link_libraries("${target}" PRIVATE Qt6::QOffscreenIntegrationPlugin)
    qt_import_plugins("${target}" INCLUDE Qt6::QOffscreenIntegrationPlugin)
endfunction()
