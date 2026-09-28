#pragma once

class QWindow;

namespace adqt::widgets::detail {

// Query Qt's full modal stack and transient hierarchy. The active modal widget
// and QObject ancestry alone cannot describe which windows accept input.
QWindow* blockingModalWindow(QWindow* window);

}  // namespace adqt::widgets::detail
