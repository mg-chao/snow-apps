#include "pagination_style.h"

#include "theme/theme_manager.h"
#include "theme/theme_color_utils.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QColor withAlpha(const QColor& color, int alpha) {
  QColor result = color;
  result.setAlpha(alpha);
  return result;
}

template <typename T>
T resolved(const std::optional<T>& overrideValue, const T& fallback) {
  return overrideValue.value_or(fallback);
}

}  // namespace

PaginationVisualStyle resolvePaginationVisualStyle(
    const PaginationStyleInput& input, const adqt::theme::ResolvedTheme& resolvedTheme) {
  const auto& values = resolvedTheme.values;
  const auto& semantic = resolvedTheme.semantic;
  const auto& colors = input.componentTokens.colors;
  const auto& metrics = input.componentTokens.metrics;

  PaginationVisualStyle style;
  style.rootBackground = resolvedTheme.palette.color(QPalette::Window);
  style.text = resolved(colors.itemText, semantic.text);
  style.itemBackground = resolved(colors.itemBackground, semantic.surface);
  style.itemHoverBackground = resolved(colors.itemHoverBackground, semantic.fillQuaternary);
  style.itemPressedBackground = resolved(colors.itemPressedBackground, semantic.fillTertiary);
  style.activeBackground = resolved(colors.itemActiveBackground, semantic.surface);
  style.activeText = resolved(colors.itemActiveText, semantic.accent);
  style.activeHoverText = resolved(colors.itemActiveHoverText, semantic.accentHover);
  style.activeBorder = resolved(colors.itemActiveBorder, semantic.accent);
  style.disabledText = resolved(colors.itemDisabledText, semantic.textDisabled);
  style.activeDisabledBackground =
      resolved(colors.itemActiveDisabledBackground, semantic.fillTertiary);
  style.focusOutline = resolved(colors.focusOutline, withAlpha(semantic.accent, 45));

  const int medium = std::max(1, qRound(values.controlHeight));
  const int small = std::max(1, qRound(values.controlHeightSM));
  const int large = std::max(1, qRound(values.controlHeightLG));
  switch (input.controlSize) {
    case AdPagination::ControlSize::Large:
      style.itemSize = resolved(metrics.itemSizeLarge, large);
      break;
    case AdPagination::ControlSize::Small:
      style.itemSize = resolved(metrics.itemSizeSmall, small);
      break;
    case AdPagination::ControlSize::Medium:
    default:
      style.itemSize = resolved(metrics.itemSize, medium);
      break;
  }

  style.spacing = std::max(0, resolved(metrics.itemSpacing, qRound(values.sizeXS)));
  style.radius = std::max(0, resolved(metrics.borderRadius, qRound(values.borderRadius)));
  style.borderWidth = std::max(1, qRound(values.lineWidth));
  style.quickJumperWidth = std::max(32, resolved(metrics.quickJumperWidth, qRound(50.0)));
  style.font = input.baseFont;
  style.font.setPixelSize(std::max(1, qRound(values.fontSize)));

  if (input.semanticStyles.root.backgroundColor) {
    style.rootBackground = *input.semanticStyles.root.backgroundColor;
  }
  if (input.semanticStyles.root.textColor) {
    style.text = *input.semanticStyles.root.textColor;
  }
  if (input.semanticStyles.root.borderColor) {
    style.rootBorder = *input.semanticStyles.root.borderColor;
    style.hasRootBorder = true;
  }
  if (input.semanticStyles.item.backgroundColor) {
    style.itemBackground = *input.semanticStyles.item.backgroundColor;
    style.activeBackground = *input.semanticStyles.item.backgroundColor;
  }
  if (input.semanticStyles.item.textColor) {
    style.text = *input.semanticStyles.item.textColor;
  }
  if (input.semanticStyles.item.borderColor) {
    style.activeBorder = *input.semanticStyles.item.borderColor;
  }

  if (values.backgroundOpacity != 1.0) {
    for (QColor* background :
         {&style.rootBackground, &style.itemBackground, &style.itemHoverBackground,
          &style.itemPressedBackground, &style.activeBackground, &style.activeDisabledBackground}) {
      *background = adqt::theme::applyBackgroundOpacity(*background, values.backgroundOpacity);
    }
  }
  return style;
}

}  // namespace adqt::widgets::detail
