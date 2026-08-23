QT += core gui widgets svg network
CONFIG += c++17
TEMPLATE = app
TARGET = theme-demo

win32-msvc {
    QMAKE_CXXFLAGS += /MP
    CONFIG += precompile_header
    PRECOMPILED_HEADER = theme_demo_pch.h
}

# The demo is built out-of-source.  Use libraries built by the same Qt
# toolchain as qmake, while still allowing callers to override each path.
win32-msvc {
    ADQT_DEFAULT_BUILD_DIR = build-msvc
} else {
    ADQT_DEFAULT_BUILD_DIR = build-mingw
}

INCLUDEPATH += ../../packages/ant_design_qt/src
INCLUDEPATH += ../../packages/adqt_icon_core/src
INCLUDEPATH += ../../packages/ant_design_icons_qt/src

HEADERS += \
    checkbox_docs_page.h \
    carousel_docs_page.h \
    theme_demo_pch.h \
    alert_docs_page.h \
    color_picker_docs_page.h \
    date_picker_docs_page.h \
    divider_docs_page.h \
    descriptions_docs_page.h \
    form_docs_page.h \
    icon_theme_adapter.h \
    image_docs_page.h \
    input_docs_page.h \
    input_number_docs_page.h \
    menu_docs_page.h \
    message_docs_page.h \
    modal_docs_page.h \
    notification_docs_page.h \
    pagination_docs_page.h \
    popconfirm_docs_page.h \
    popover_docs_page.h \
    radio_docs_page.h \
    segmented_docs_page.h \
    select_docs_page.h \
    slider_docs_page.h \
    spin_docs_page.h \
    switch_docs_page.h \
    tabs_docs_page.h \
    tag_docs_page.h \
    tooltip_docs_page.h

SOURCES += \
    checkbox_docs_page.cpp \
    carousel_docs_page.cpp \
    alert_docs_page.cpp \
    color_picker_docs_page.cpp \
    date_picker_docs_page.cpp \
    divider_docs_page.cpp \
    descriptions_docs_page.cpp \
    form_docs_page.cpp \
    icon_theme_adapter.cpp \
    image_docs_page.cpp \
    input_docs_page.cpp \
    input_number_docs_page.cpp \
    main.cpp \
    menu_docs_page.cpp \
    message_docs_page.cpp \
    modal_docs_page.cpp \
    notification_docs_page.cpp \
    pagination_docs_page.cpp \
    popconfirm_docs_page.cpp \
    popover_docs_page.cpp \
    radio_docs_page.cpp \
    segmented_docs_page.cpp \
    select_docs_page.cpp \
    slider_docs_page.cpp \
    spin_docs_page.cpp \
    switch_docs_page.cpp \
    tabs_docs_page.cpp \
    tag_docs_page.cpp \
    tooltip_docs_page.cpp

isEmpty(ADQT_LIB_BUILD_DIR) {
    ADQT_LIB_BUILD_DIR = $$clean_path($$OUT_PWD/../../packages/ant_design_qt)
}
!exists($$ADQT_LIB_BUILD_DIR) {
    ADQT_LIB_BUILD_DIR = $$clean_path($$PWD/../../packages/ant_design_qt/$$ADQT_DEFAULT_BUILD_DIR)
}

isEmpty(ADQT_ICON_CORE_LIB_BUILD_DIR) {
    ADQT_ICON_CORE_LIB_BUILD_DIR = $$clean_path($$OUT_PWD/../../packages/adqt_icon_core)
}
!exists($$ADQT_ICON_CORE_LIB_BUILD_DIR) {
    ADQT_ICON_CORE_LIB_BUILD_DIR = $$clean_path($$PWD/../../packages/adqt_icon_core/$$ADQT_DEFAULT_BUILD_DIR)
}

isEmpty(ADQT_ICONS_LIB_BUILD_DIR) {
    ADQT_ICONS_LIB_BUILD_DIR = $$clean_path($$OUT_PWD/../../packages/ant_design_icons_qt)
}
!exists($$ADQT_ICONS_LIB_BUILD_DIR) {
    ADQT_ICONS_LIB_BUILD_DIR = $$clean_path($$PWD/../../packages/ant_design_icons_qt/$$ADQT_DEFAULT_BUILD_DIR)
}

CONFIG(debug, debug|release) {
    ADQT_LIB_DIR = $$ADQT_LIB_BUILD_DIR/debug
    ADQT_ICON_CORE_LIB_DIR = $$ADQT_ICON_CORE_LIB_BUILD_DIR/debug
    ADQT_ICONS_LIB_DIR = $$ADQT_ICONS_LIB_BUILD_DIR/debug
} else {
    ADQT_LIB_DIR = $$ADQT_LIB_BUILD_DIR/release
    ADQT_ICON_CORE_LIB_DIR = $$ADQT_ICON_CORE_LIB_BUILD_DIR/release
    ADQT_ICONS_LIB_DIR = $$ADQT_ICONS_LIB_BUILD_DIR/release
}

win32-g++ {
    PRE_TARGETDEPS += $$ADQT_LIB_DIR/libant_design_qt.a
    PRE_TARGETDEPS += $$ADQT_ICON_CORE_LIB_DIR/libadqt_icon_core.a
    PRE_TARGETDEPS += $$ADQT_ICONS_LIB_DIR/libant_design_icons_qt.a
    LIBS += -L$$ADQT_LIB_DIR -lant_design_qt
    LIBS += -L$$ADQT_ICON_CORE_LIB_DIR -ladqt_icon_core
    LIBS += -L$$ADQT_ICONS_LIB_DIR -lant_design_icons_qt
}

win32-msvc {
    PRE_TARGETDEPS += $$ADQT_LIB_DIR/ant_design_qt.lib
    PRE_TARGETDEPS += $$ADQT_ICON_CORE_LIB_DIR/adqt_icon_core.lib
    PRE_TARGETDEPS += $$ADQT_ICONS_LIB_DIR/ant_design_icons_qt.lib
    LIBS += $$ADQT_LIB_DIR/ant_design_qt.lib
    LIBS += $$ADQT_ICON_CORE_LIB_DIR/adqt_icon_core.lib
    LIBS += $$ADQT_ICONS_LIB_DIR/ant_design_icons_qt.lib
}

# ant_design_qt is a static library.  Its native modal implementation uses
# both DWM and User32, so the final application must link those system libs.
win32:LIBS += -ldwmapi -luser32
