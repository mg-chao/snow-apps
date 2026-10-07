#pragma once

#include <Qt>

class QWidget;

namespace adqt::widgets::detail {

// Native frame decoration for windows that paint their own header and controls.
Qt::WindowFlags nativeWindowChromeFlags();
void applyNativeWindowChrome(QWidget* surface);

}  // namespace adqt::widgets::detail
