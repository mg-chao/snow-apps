#include "window_modality.h"

#include <QtGui/private/qguiapplication_p.h>

namespace adqt::widgets::detail {

QWindow* blockingModalWindow(QWindow* window) {
  QWindow* blocker = nullptr;
  return window && QGuiApplicationPrivate::instance()->isWindowBlocked(window, &blocker) ? blocker
                                                                                         : nullptr;
}

}  // namespace adqt::widgets::detail
