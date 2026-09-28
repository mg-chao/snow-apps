#include "top_level_popup_window.h"
#include "../window_creation_context.h"

#include <QMetaObject>
#include <QPointer>
#include <QWidget>
#include <QWindow>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace adqt::widgets::detail {

Q_LOGGING_CATEGORY(popupLog, "adqt.popup", QtWarningMsg)

#if defined(Q_OS_WIN)
namespace {
void constrainToolPosition(HWND tool, HWND owner, WINDOWPOS* position) {
  if (!position || !owner || !IsWindowVisible(owner) || (position->flags & SWP_NOZORDER) ||
      (position->hwndInsertAfter != HWND_TOP && position->hwndInsertAfter != HWND_TOPMOST)) {
    return;
  }
  // Concrete insertion points can be Windows arranging other owned siblings
  // as part of this operation. Rewriting those would fight the owner ordering.
  position->flags |= SWP_NOOWNERZORDER;

  // Qt's show/raise uses HWND_TOP, which can raise the owner as well. Insert
  // above its existing group instead. Include sibling tools (e.g. a toolbar),
  // but not this tool's descendants, which must remain above this tool.
  const HWND root = GetAncestor(owner, GA_ROOTOWNER);
  HWND anchor = owner;
  for (HWND candidate = GetTopWindow(nullptr); candidate;
       candidate = GetWindow(candidate, GW_HWNDNEXT)) {
    if (!IsWindowVisible(candidate) || GetAncestor(candidate, GA_ROOTOWNER) != root) {
      continue;
    }
    HWND ancestor = candidate;
    while (ancestor && ancestor != tool) {
      ancestor = GetWindow(ancestor, GW_OWNER);
    }
    if (!ancestor) {
      anchor = candidate;
      break;
    }
  }
  const HWND preceding = GetWindow(anchor, GW_HWNDPREV);
  if (preceding == tool) {
    position->flags |= SWP_NOZORDER;
  } else {
    position->hwndInsertAfter = preceding ? preceding : HWND_TOP;
  }
}
}  // namespace
#endif

void constrainTopLevelToolStackingToOwner(QWidget* toolWindow, void* message) {
#if defined(Q_OS_WIN)
  const auto* nativeMessage = static_cast<const MSG*>(message);
  if (!nativeMessage || nativeMessage->message != WM_WINDOWPOSCHANGING) {
    return;
  }
  const HWND tool = nativeMessage->hwnd;
  HWND owner = GetWindow(tool, GW_OWNER);
  if (toolWindow) {
    // Qt can temporarily clear the HWND owner while showing a parentless
    // QWidget tool, then restore its QWindow transient parent in show_sys().
    if (!owner && toolWindow->windowHandle()) {
      if (QWindow* transient = toolWindow->windowHandle()->transientParent();
          transient && transient->handle()) {
        owner = reinterpret_cast<HWND>(transient->winId());
      }
    }
    if (const auto intendedOwner = ScopedWindowCreationOwner::ownerFor(toolWindow)) {
      owner = *intendedOwner ? reinterpret_cast<HWND>((*intendedOwner)->internalWinId()) : nullptr;
    }
  }
  constrainToolPosition(tool, owner, reinterpret_cast<WINDOWPOS*>(nativeMessage->lParam));
#else
  Q_UNUSED(toolWindow)
  Q_UNUSED(message)
#endif
}

void setTopLevelToolTransientParent(QWidget* toolWindow, QWidget* ownerWindow) {
  QWindow* toolHandle = toolWindow ? toolWindow->windowHandle() : nullptr;
  QWindow* ownerHandle = ownerWindow ? ownerWindow->windowHandle() : nullptr;
  if (!toolHandle || !ownerHandle || toolHandle->transientParent() == ownerHandle) {
    return;
  }
  const ScopedWindowCreationOwner creationOwner(toolWindow, ownerWindow);
#if defined(Q_OS_WIN)
  // QWidget initially creates unowned tools at the front. Even while hidden,
  // attaching that HWND can pull its new owner forward. Place it first.
  const HWND tool = reinterpret_cast<HWND>(toolWindow->internalWinId());
  const HWND owner = reinterpret_cast<HWND>(ownerWindow->internalWinId());
  if (tool && owner && IsWindowVisible(owner)) {
    WINDOWPOS position{};
    position.hwnd = tool;
    position.flags = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER;
    constrainToolPosition(tool, owner, &position);
    SetWindowPos(tool, position.hwndInsertAfter, 0, 0, 0, 0, position.flags);
  }
#endif
  toolHandle->setTransientParent(ownerHandle);
}

void syncTopLevelToolTransientParent(QWidget* toolWindow, QWidget* ownerWindow) {
  if (!toolWindow || !ownerWindow || !toolWindow->isWindow()) {
    return;
  }

  QWidget* ownerTopLevel = ownerWindow->window();
  if (!ownerTopLevel || ownerTopLevel == toolWindow) {
    return;
  }

  const ScopedWindowCreationOwner creationOwner(toolWindow, ownerTopLevel);

  const bool ownerStaysOnTop = ownerTopLevel->windowFlags().testFlag(Qt::WindowStaysOnTopHint);
  if (toolWindow->windowFlags().testFlag(Qt::WindowStaysOnTopHint) != ownerStaysOnTop) {
    toolWindow->setWindowFlag(Qt::WindowStaysOnTopHint, ownerStaysOnTop);
  }

  ownerTopLevel->winId();
  const bool creating = !toolWindow->windowHandle();
  toolWindow->winId();
  if (creating) {
    qCDebug(popupLog) << "native.create" << toolWindow->objectName();
  }
  setTopLevelToolTransientParent(toolWindow, ownerTopLevel);
#if defined(Q_OS_MACOS)
  syncMacTopLevelPopupOwnership(toolWindow);
#endif
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
          qCDebug(popupLog) << "native.release" << guardedToolWindow->objectName();
          resourceReleaser->releaseTopLevelToolResources();
        }
      },
      Qt::QueuedConnection);
}

}  // namespace adqt::widgets::detail
