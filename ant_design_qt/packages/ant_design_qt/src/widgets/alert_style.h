#pragma once

#include <QColor>
#include <QEasingCurve>
#include <QFont>
#include <QPalette>

#include "alert.h"

namespace adqt::theme {
struct ResolvedTheme;
}

namespace adqt::widgets::detail {

struct AlertMetrics {
  int borderWidth = 1;
  int borderRadius = 8;
  int paddingInline = 12;
  int paddingBlock = 8;
  int paddingWithInformativeTextInline = 24;
  int paddingWithInformativeTextBlock = 24;
  int gapLeadingContent = 8;
  int gapLeadingContentWithInformativeText = 12;
  int gapContentActions = 8;
  int gapActionsDismiss = 8;
  int textInformativeTextGap = 8;
  int iconSize = 14;
  int iconSizeWithInformativeText = 24;
  int closeIconSize = 12;
  int closeButtonSize = 24;
  int closeButtonFocusOutlineWidth = 2;
  int closeAnimationMs = 300;
  QEasingCurve closeAnimationEasing = QEasingCurve(QEasingCurve::InOutCubic);
  QFont textFont;
  QFont textWithInformativeTextFont;
  QFont informativeTextFont;
};

struct AlertVisualStyle {
  QColor background;
  QColor border;
  QColor textColor;
  QColor informativeTextColor;
  QColor iconColor;
  QColor closeIconColor;
  QColor closeIconHoverColor;
  QColor closeButtonHoverBackground;
  QColor closeButtonPressedBackground;
  QColor closeButtonFocusOutlineColor;
  AlertMetrics metrics;
};

struct AlertStyleInput {
  AdAlert::Severity severity = AdAlert::Severity::Info;
  AdAlert::DisplayMode displayMode = AdAlert::DisplayMode::Inline;
  bool enabled = true;
  bool hasPaletteOverride = false;
  QPalette::ColorGroup paletteGroup = QPalette::Active;
  QFont baseFont;
  QPalette palette;
};

AlertVisualStyle resolveAlertVisualStyle(const AlertStyleInput& input,
                                         const adqt::theme::ResolvedTheme& resolvedTheme);
AlertVisualStyle resolveAlertVisualStyle(const AlertStyleInput& input);

}  // namespace adqt::widgets::detail
