#pragma once

#include "select.h"

#include <QColor>
#include <QFont>

namespace adqt::theme {
struct ResolvedTheme;
}

namespace adqt::widgets::detail {

struct SelectMetrics {
  int height = 32;
  int borderRadius = 6;
  int popupBorderRadius = 8;
  int optionBorderRadius = 4;
  int borderWidth = 1;
  qreal focusOutlineWidth = 3.0;
  qreal focusOutlineOffset = 0.0;
  int inputPaddingHorizontalBase = 11;
  int horizontalPadding = 11;
  int popupPadding = 4;
  int popupOffset = 4;
  int popupMaxHeight = 256;
  int optionHeight = 32;
  int emptyStateHeight = 90;
  int emptyStateIconWidth = 55;
  int emptyStateIconHeight = 35;
  int emptyStateMarginBlock = 8;
  int emptyStateMarginInline = 8;
  int emptyStateImageMarginBottom = 8;
  int emptyDescriptionFontSize = 14;
  int emptyDescriptionLineHeight = 22;
  int optionPaddingHorizontal = 12;
  int optionPaddingVertical = 5;
  int tagHeight = 20;
  int tagBorderRadius = 4;
  int tagPaddingInlineStart = 8;
  int tagPaddingInlineEnd = 4;
  int tagContentGap = 4;
  int tagItemMargin = 2;
  int tagItemGap = 4;
  int optionStateGap = 4;
  int multiplePaddingInlineStart = 3;
  int multiplePaddingVertical = 1;
  int multipleItemPaddingHorizontal = 8;
  int iconSize = 14;
  int spacing = 4;
  QFont selectorFont;
  QFont optionFont;
};

struct SelectVisualStyle {
  QColor selectorBg;
  QColor selectorHoverBg;
  QColor selectorActiveBg;
  QColor selectorBorderColor;
  QColor selectorHoverBorderColor;
  QColor selectorActiveBorderColor;
  QColor selectorFocusOutlineColor;
  QColor selectorTextColor;
  QColor placeholderColor;
  QColor popupBg;
  QColor popupBorderColor;
  QColor optionTextColor;
  QColor optionHoverBg;
  QColor optionSelectedBg;
  QColor optionSelectedColor;
  QColor tagBg;
  QColor tagBorderColor;
  QColor tagTextColor;
  QColor clearColor;
  QColor clearHoverColor;
  QColor clearBg;
  QColor prefixColor;
  QColor suffixColor;
  QColor emptyTextColor;
  QColor emptyBorderColor;
  QColor emptyShadowColor;
  QColor emptyContentColor;
  QColor disabledTextColor;
  QColor groupTitleColor;
  QColor disabledBg;
  QColor disabledBorderColor;
  SelectMetrics metrics;
};

struct SelectStyleInput {
  AdSelect::Mode mode = AdSelect::Mode::Single;
  AdSelect::ControlSize controlSize = AdSelect::ControlSize::Middle;
  AdSelect::Variant variant = AdSelect::Variant::Outlined;
  AdSelect::Status status = AdSelect::Status::None;
  bool disabled = false;
  QFont baseFont;
  AdSelect::ComponentTokens componentTokens;
  AdSelect::SemanticStyles semanticStyles;
};

SelectVisualStyle resolveSelectVisualStyle(const SelectStyleInput& input,
                                           const adqt::theme::ResolvedTheme& resolvedTheme);
SelectVisualStyle resolveSelectVisualStyle(const SelectStyleInput& input);

}  // namespace adqt::widgets::detail
