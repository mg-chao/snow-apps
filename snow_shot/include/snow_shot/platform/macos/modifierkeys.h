#pragma once

#include <QCoreApplication>
#include <Qt>

namespace snow_shot::platform::macos {
inline Qt::KeyboardModifier commandModifier() {
    return QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta) ? Qt::MetaModifier
                                                                          : Qt::ControlModifier;
}
inline Qt::KeyboardModifier controlModifier() {
    return QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta) ? Qt::ControlModifier
                                                                          : Qt::MetaModifier;
}
} // namespace snow_shot::platform::macos
