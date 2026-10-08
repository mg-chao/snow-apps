#pragma once

#include <Qt>

namespace adqt::widgets::detail {

// Popup, activation and programmatic focus do not identify keyboard input.
// Only explicit keyboard navigation or a shortcut should enable a focus outline.
constexpr bool isKeyboardFocusReason(Qt::FocusReason reason) {
  return reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason ||
         reason == Qt::ShortcutFocusReason;
}

}  // namespace adqt::widgets::detail
