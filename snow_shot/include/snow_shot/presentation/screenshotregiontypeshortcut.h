#pragma once

#include <QKeyCombination>

// macOS delivers Option+Tab to the screenshot overlay as a key press.
[[nodiscard]] constexpr QKeyCombination screenshotRegionTypeCycleKey(bool reverse = false) {
#ifdef Q_OS_MACOS
    constexpr auto modifier = Qt::AltModifier;
#else
    constexpr auto modifier = Qt::ControlModifier;
#endif
    return QKeyCombination(reverse ? modifier | Qt::ShiftModifier : modifier, Qt::Key_Tab);
}
