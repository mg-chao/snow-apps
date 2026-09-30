#pragma once

class QWidget;
class QWindow;

namespace adqt::widgets::detail {

void updateMacWindowSurfaceShadow(QWidget* surface);
void releaseMacWindowSurfaceCursor(QWindow* surface);

}  // namespace adqt::widgets::detail
