#pragma once

#include "button.h"

#include <QColor>
#include <QFont>

namespace adqt::theme {
struct ResolvedTheme;
}

namespace adqt::widgets::detail {

struct ButtonStateStyle {
  QColor text;
  QColor background;
  QColor border;
  QColor shadow;
  Qt::PenStyle borderStyle = Qt::SolidLine;
};

struct ButtonMetrics {
  int height = 32;
  int horizontalPadding = 14;
  int borderRadius = 6;
  int borderWidth = 1;
  int shadowOffsetY = 2;
  int iconGap = 8;
  int menuIndicatorSize = 8;
  int menuIndicatorGap = 8;
  QFont font;
  QColor focusOutline;
  qreal focusOutlineWidth = 3.0;
  qreal focusOutlineOffset = 1.0;
  QColor defaultOutline;
  qreal defaultOutlineWidth = 1.0;
  qreal defaultOutlineOffset = 2.0;
};

struct ResolvedRole {
  AdButton::AccentRole accentRole = AdButton::AccentRole::Neutral;
  AdButton::ButtonStyle buttonStyle = AdButton::ButtonStyle::Outline;
  AdButton::ButtonStyle visualStyle = AdButton::ButtonStyle::Outline;
  bool ghost = false;
  bool unbordered = false;
  bool defaultButton = false;
  bool hasMenu = false;
};

struct ButtonVisualStyle {
  ButtonStateStyle normal;
  ButtonStateStyle hover;
  ButtonStateStyle active;
  ButtonStateStyle checked;
  ButtonStateStyle disabled;
  ButtonMetrics metrics;
  ResolvedRole role;
};

struct ButtonStyleInput {
  AdButton::ButtonStyle buttonStyle = AdButton::ButtonStyle::Outline;
  AdButton::AccentRole accentRole = AdButton::AccentRole::Neutral;
  AdButton::SizeClass sizeClass = AdButton::SizeClass::Medium;
  bool flat = false;
  bool defaultButton = false;
  bool hasMenu = false;
  bool joinsEdges = false;
  QFont baseFont;
};

ResolvedRole resolveRole(const ButtonStyleInput& input);
ButtonVisualStyle resolveButtonVisualStyle(const ButtonStyleInput& input,
                                           const adqt::theme::ResolvedTheme& resolvedTheme);
ButtonVisualStyle resolveButtonVisualStyle(const ButtonStyleInput& input);

}  // namespace adqt::widgets::detail
