TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS += \
    adqt_icon_core \
    ant_design_icons_qt \
    ant_design_qt \
    carousel_tests \
    checkbox_tests \
    descriptions_tests \
    divider_tests \
    image_tests \
    ant_design_qt_tests \
    notification_tests \
    pagination_tests \
    spin_tests \
    segmented_tests \
    tabs_tests \
    theme_demo

adqt_icon_core.subdir = packages/adqt_icon_core
ant_design_icons_qt.subdir = packages/ant_design_icons_qt
ant_design_icons_qt.depends = adqt_icon_core
ant_design_qt.subdir = packages/ant_design_qt
ant_design_qt.depends = adqt_icon_core ant_design_icons_qt
carousel_tests.file = packages/ant_design_qt/tests/carousel-tests.pro
carousel_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
checkbox_tests.file = packages/ant_design_qt/tests/checkbox-tests.pro
checkbox_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
descriptions_tests.file = packages/ant_design_qt/tests/descriptions-tests.pro
descriptions_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
divider_tests.file = packages/ant_design_qt/tests/divider-tests.pro
divider_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
image_tests.file = packages/ant_design_qt/tests/image-tests.pro
image_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
ant_design_qt_tests.file = packages/ant_design_qt/tests/ant_design_qt-tests.pro
ant_design_qt_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
notification_tests.file = packages/ant_design_qt/tests/notification-tests.pro
notification_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
pagination_tests.file = packages/ant_design_qt/tests/pagination-tests.pro
pagination_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
spin_tests.file = packages/ant_design_qt/tests/spin-tests.pro
spin_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
segmented_tests.file = packages/ant_design_qt/tests/segmented-tests.pro
segmented_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
tabs_tests.file = packages/ant_design_qt/tests/tabs-tests.pro
tabs_tests.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
theme_demo.subdir = examples/theme-demo
theme_demo.depends = adqt_icon_core ant_design_qt ant_design_icons_qt
