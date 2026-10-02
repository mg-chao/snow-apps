#pragma once

#include <QApplication>
#include <QWidget>

namespace adqt::widgets::detail {

inline QWidget* resolveFeedbackOwner(QWidget* requested) {
  // An explicit owner may intentionally host feedback on a tool or overlay.
  if (requested) {
    return requested->window();
  }

  const auto eligible = [](QWidget* candidate) {
    if (!candidate || !candidate->isVisible() || candidate->isMinimized()) {
      return false;
    }
    // Auxiliary windows can be the only visible surface in a tray application.
    // They are not application content areas and must never be chosen implicitly.
    const auto type = candidate->windowType();
    return type == Qt::Window || type == Qt::Dialog || type == Qt::Sheet;
  };
  if (QWidget* active = QApplication::activeWindow(); eligible(active)) {
    return active;
  }
  for (QWidget* candidate : QApplication::topLevelWidgets()) {
    if (eligible(candidate)) {
      return candidate;
    }
  }
  return nullptr;
}

}  // namespace adqt::widgets::detail
