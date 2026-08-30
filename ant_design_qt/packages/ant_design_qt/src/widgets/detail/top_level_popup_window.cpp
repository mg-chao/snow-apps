#include "top_level_popup_window.h"

#include <QMetaObject>
#include <QPointer>
#include <QWidget>
#include <QWindow>

namespace adqt::widgets::detail {

void syncTopLevelToolTransientParent(QWidget* toolWindow, QWidget* ownerWindow) {
  if (!toolWindow || !ownerWindow || !toolWindow->isWindow()) {
    return;
  }

  QWidget* ownerTopLevel = ownerWindow->window();
  if (!ownerTopLevel || ownerTopLevel == toolWindow) {
    return;
  }

  const bool ownerStaysOnTop = ownerTopLevel->windowFlags().testFlag(Qt::WindowStaysOnTopHint);
  if (toolWindow->windowFlags().testFlag(Qt::WindowStaysOnTopHint) != ownerStaysOnTop) {
    toolWindow->setWindowFlag(Qt::WindowStaysOnTopHint, ownerStaysOnTop);
  }

  ownerTopLevel->winId();
  toolWindow->winId();
  QWindow* ownerHandle = ownerTopLevel->windowHandle();
  QWindow* toolHandle = toolWindow->windowHandle();
  if (ownerHandle && toolHandle && toolHandle->transientParent() != ownerHandle) {
    toolHandle->setTransientParent(ownerHandle);
  }
}

void releaseTopLevelToolResourcesOnHide(QWidget* toolWindow) {
  if (!toolWindow || !toolWindow->isWindow() || !toolWindow->windowHandle()) {
    return;
  }

  const QPointer<QWidget> guardedToolWindow(toolWindow);
  QMetaObject::invokeMethod(
      toolWindow,
      [guardedToolWindow]() {
        if (!guardedToolWindow || guardedToolWindow->isVisible() ||
            !guardedToolWindow->isWindow() || !guardedToolWindow->windowHandle()) {
          return;
        }
        auto* resourceReleaser =
            dynamic_cast<TopLevelToolResourceReleaser*>(guardedToolWindow.data());
        if (resourceReleaser) {
          resourceReleaser->releaseTopLevelToolResources();
        }
      },
      Qt::QueuedConnection);
}

}  // namespace adqt::widgets::detail
