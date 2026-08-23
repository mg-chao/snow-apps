TEMPLATE = lib
TARGET = adqt_icon_core
VERSION = 2.0.0

QT += core gui widgets svg
CONFIG += c++17
CONFIG += staticlib
DEFINES += ADQT_ICON_CORE_LIBRARY

win32-msvc {
    QMAKE_CXXFLAGS += /MP
}

INCLUDEPATH += $$PWD/src

HEADERS +=     src/adqt_icon_core_global.h     src/icon_core.h     src/icon_core_types.h     src/icon_registry.h     src/icon_renderer.h     src/version.h

SOURCES +=     src/icon_registry.cpp

isEmpty(PREFIX) {
    PREFIX = /usr/local
}

win32 {
    PREFIX = C:/ant_design_qt
}

target.path = $$PREFIX/lib
headers.path = $$PREFIX/include/adqt_icon_core
headers.files = $$HEADERS
INSTALLS += target headers
