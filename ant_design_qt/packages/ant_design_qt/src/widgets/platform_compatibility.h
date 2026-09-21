#pragma once

class QApplication;

namespace adqt::widgets {
// Call on the GUI thread immediately after constructing QApplication, before
// creating widgets. Installs application-owned Qt platform compatibility repairs
// independently of theming. Repeated calls are harmless; non-macOS is a no-op.
void initializePlatformCompatibility(QApplication& app);
}  // namespace adqt::widgets
