#include "popup_surface_guard.h"

#include "detail/overlay_popup_controller.h"
#include "detail/overlay_popup_surface.h"
#include "detail/top_level_popup_window.h"

#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <utility>

namespace adqt::widgets {
namespace {
QVector<QPointer<AdPopupSurfaceGuard>>& guards() {
  static QVector<QPointer<AdPopupSurfaceGuard>> value;
  return value;
}
QVector<QPointer<detail::OverlayPopupController>>& controllers() {
  static QVector<QPointer<detail::OverlayPopupController>> value;
  return value;
}
}  // namespace

AdPopupSurfaceGuard::AdPopupSurfaceGuard(QWidget* scopeWindow, Guard guard)
    : QObject(scopeWindow), scopeWindow_(scopeWindow), guard_(std::move(guard)) {
  guards().push_back(this);
}

AdPopupSurfaceGuard::~AdPopupSurfaceGuard() {
  auto& active = guards();
  active.erase(std::remove_if(active.begin(), active.end(),
                              [this](const auto& guard) { return !guard || guard == this; }),
               active.end());
  for (const auto& retained : surfaces_) {
    if (!retained.surface) continue;
    retained.surface->setNativeSurfaceRetained(retained.originallyRetained);
    if (!retained.originallyRetained && !retained.surface->isVisible()) {
      detail::releaseTopLevelToolResourcesOnHide(retained.surface);
    }
  }
}

AdPopupSurfaceGuard* AdPopupSurfaceGuard::forScope(QWidget* scope) {
  if (guards().isEmpty()) return nullptr;
  for (QWidget* widget = scope; widget; widget = widget->parentWidget()) {
    for (auto it = guards().crbegin(); it != guards().crend(); ++it) {
      if (*it && (*it)->scopeWindow_ == widget) return *it;
    }
  }
  // An overlay keeps its logical owner when Qt replaces or temporarily releases
  // its native handle. Resolve that owner before consulting native transients.
  QWidget* surfaceWindow = scope ? scope->window() : nullptr;
  for (const auto& controller : controllers()) {
    if (!controller || !controller->delegate()) continue;
    auto* delegate = controller->delegate();
    if (delegate->popupSurfaceWidget() == surfaceWindow &&
        delegate->popupScopeWindow() != surfaceWindow) {
      return forScope(delegate->popupScopeWindow());
    }
  }
  // QtTool popup surfaces are detached QWidgets. Follow native ownership for
  // controls hosted inside those surfaces instead of QObject parenting.
  QWindow* window = scope && scope->window() ? scope->window()->windowHandle() : nullptr;
  while (window) {
    for (auto it = guards().crbegin(); it != guards().crend(); ++it) {
      if (*it && (*it)->scopeWindow_ && (*it)->scopeWindow_->windowHandle() == window) return *it;
    }
    window = window->transientParent();
  }
  return nullptr;
}

void AdPopupSurfaceGuard::registerController(detail::OverlayPopupController* controller) {
  auto& registered = controllers();
  registered.erase(std::remove_if(registered.begin(), registered.end(),
                                  [](const auto& existing) { return !existing; }),
                   registered.end());
  registered.push_back(controller);
}

void AdPopupSurfaceGuard::refresh() {
  // A refresh can materialize more controllers; iterate a stable weak snapshot.
  const auto snapshot = controllers();
  for (const auto& controller : snapshot) {
    if (controller && controller->delegate() &&
        forScope(controller->delegate()->popupScopeWindow()) == this) {
      controller->refreshVisiblePopup();
    }
  }
}

bool AdPopupSurfaceGuard::canShow(QWidget* surface) {
  if (auto* popup = dynamic_cast<detail::OverlayPopupSurface*>(surface)) {
    const bool tracked =
        std::any_of(surfaces_.cbegin(), surfaces_.cend(),
                    [popup](const auto& retained) { return retained.surface == popup; });
    if (!tracked) {
      surfaces_.push_back({popup, popup->nativeSurfaceRetained()});
      popup->setNativeSurfaceRetained(true);
    }
  }
  return !guard_ || guard_(surface);
}
}  // namespace adqt::widgets
