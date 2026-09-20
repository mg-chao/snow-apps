#pragma once

class QMenu;
class QSystemTrayIcon;

namespace snow_shot::platform::macos {
// Called synchronously from QSystemTrayIcon::Context while its native event is current.
void showSystemTrayMenu(QSystemTrayIcon* trayIcon, QMenu* menu);
} // namespace snow_shot::platform::macos
