#pragma once

namespace adqt::widgets {
class AdContextMenu;
}
class QSystemTrayIcon;

namespace snow_shot::platform::macos {
// Called synchronously from QSystemTrayIcon::Context while its native event is current.
void showSystemTrayMenu(QSystemTrayIcon* trayIcon, adqt::widgets::AdContextMenu* menu);
} // namespace snow_shot::platform::macos
