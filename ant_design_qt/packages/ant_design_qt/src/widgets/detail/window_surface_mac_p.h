#pragma once

class QWidget;
class QWindow;

namespace adqt::widgets::detail {

void applyMacWindowSurfaceChrome(QWidget* surface);
void updateMacWindowSurfaceShadow(QWidget* surface);
void releaseMacWindowSurfaceCursor(QWindow* surface);

}  // namespace adqt::widgets::detail
