#pragma once

#include <QStringList>

namespace snow_shot::app {
inline bool shouldShowMainWindowOnStartup(const QStringList& arguments) {
    if (arguments.contains(QStringLiteral("--autostart"))) {
        return false;
    }
#ifdef Q_OS_MACOS
    return true;
#else
    return arguments.contains(QStringLiteral("--show-main-window"));
#endif
}
} // namespace snow_shot::app
