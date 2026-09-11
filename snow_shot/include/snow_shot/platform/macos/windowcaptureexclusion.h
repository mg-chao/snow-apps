#pragma once

class QWidget;

namespace snow_shot::platform::macos {
[[nodiscard]] bool setWindowExcludedFromCapture(QWidget* window, bool excluded);
}
