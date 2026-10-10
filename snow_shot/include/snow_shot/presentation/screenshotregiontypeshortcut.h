#pragma once

#include <QKeyCombination>

[[nodiscard]] constexpr QKeyCombination screenshotRegionTypeCycleKey(bool reverse = false) {
#ifdef Q_OS_MACOS
    // Qt maps the physical Control key to Meta on macOS.
    constexpr auto modifier = Qt::MetaModifier;
#else
    constexpr auto modifier = Qt::ControlModifier;
#endif
    return QKeyCombination(reverse ? modifier | Qt::ShiftModifier : modifier, Qt::Key_Tab);
}
