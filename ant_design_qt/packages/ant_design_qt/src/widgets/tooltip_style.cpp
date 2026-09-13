#include "tooltip_style.h"

#include "theme/theme.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor resolveTokenColor(const std::optional<QColor>& token, const QColor& fallback) {
  if (!token.has_value()) {
    return fallback;
  }
  return toColor(token.value(), fallback);
}

QColor textColorForBackground(const QColor& background) {
  if (!background.isValid()) {
    return QColor("#ffffff");
  }
  const qreal luminance = (0.299 * static_cast<double>(background.redF()) +
                           0.587 * static_cast<double>(background.greenF()) +
                           0.114 * static_cast<double>(background.blueF()));
  return luminance < 0.5 ? QColor("#ffffff") : QColor("#000000");
}

void applySemanticSlot(const AdTooltip::SemanticSlotStyle& slot, QColor* textColor,
                       QColor* backgroundColor, QColor* borderColor) {
  if (textColor && slot.textColor.has_value()) {
    *textColor = slot.textColor.value();
  }
  if (backgroundColor && slot.backgroundColor.has_value()) {
    *backgroundColor = slot.backgroundColor.value();
  }
  if (borderColor && slot.borderColor.has_value()) {
    *borderColor = slot.borderColor.value();
  }
}

}  // namespace

TooltipVisualStyle resolveTooltipVisualStyle(const TooltipStyleInput& input,
                                             const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const adqt::theme::ThemeSeedToken& seed = resolved.config;

  TooltipVisualStyle style;
  style.surfaceBackground = toColor(map.colorBgSpotlight, QColor("#141414"));
  style.surfaceBorderColor = QColor(Qt::transparent);
  style.contentColor = toColor(map.colorTextLightSolid, QColor("#ffffff"));
  style.arrowBackground = style.surfaceBackground;
  style.arrowBorderColor = style.surfaceBorderColor;

  style.metrics.popupMaximumWidth = 250;
  style.metrics.popupMinimumHeight = std::max(0, qRound(map.controlHeight));
  style.metrics.borderRadius = std::max(0, qRound(map.borderRadius));
  style.metrics.borderWidth = 0;
  style.metrics.arrowSize = std::max(0, qRound(seed.sizePopupArrow / 2.0));
  style.metrics.popupOffset = std::max(0, qRound(map.sizeXXS));
  style.metrics.padding =
      QMargins(std::max(0, qRound(map.sizeXS)), std::max(0, qRound(map.sizeSM / 2.0)),
               std::max(0, qRound(map.sizeXS)), std::max(0, qRound(map.sizeSM / 2.0)));
  style.metrics.textFont = input.baseFont;
  style.metrics.textFont.setPixelSize(std::max(12, qRound(map.fontSize)));

  const AdTooltip::ComponentTokens& tokens = input.componentTokens;
  if (tokens.popupMaximumWidth.has_value()) {
    style.metrics.popupMaximumWidth = std::max(1, tokens.popupMaximumWidth.value());
  }
  if (tokens.popupMinimumHeight.has_value()) {
    style.metrics.popupMinimumHeight = std::max(0, tokens.popupMinimumHeight.value());
  }
  if (tokens.borderRadius.has_value()) {
    style.metrics.borderRadius = std::max(0, tokens.borderRadius.value());
  }
  if (tokens.borderWidth.has_value()) {
    style.metrics.borderWidth = std::max(0, tokens.borderWidth.value());
  }
  if (tokens.arrowSize.has_value()) {
    style.metrics.arrowSize = std::max(0, tokens.arrowSize.value());
  }
  if (tokens.popupOffset.has_value()) {
    style.metrics.popupOffset = std::max(0, tokens.popupOffset.value());
  }
  if (tokens.padding.has_value()) {
    const QMargins margins = tokens.padding.value();
    style.metrics.padding = QMargins(std::max(0, margins.left()), std::max(0, margins.top()),
                                     std::max(0, margins.right()), std::max(0, margins.bottom()));
  }
  if (tokens.textFont.has_value()) {
    style.metrics.textFont = tokens.textFont.value();
  }

  const bool explicitBackground = tokens.popupBg.has_value();
  style.surfaceBackground = resolveTokenColor(tokens.popupBg, style.surfaceBackground);
  style.surfaceBorderColor = resolveTokenColor(tokens.popupBorderColor, style.surfaceBorderColor);
  style.contentColor = resolveTokenColor(
      tokens.textColor,
      explicitBackground ? textColorForBackground(style.surfaceBackground) : style.contentColor);
  style.arrowBackground = style.surfaceBackground;
  style.arrowBorderColor = style.surfaceBorderColor;

  const AdTooltip::SemanticStyles& semantic = input.semanticStyles;
  applySemanticSlot(semantic.surface, nullptr, &style.surfaceBackground, &style.surfaceBorderColor);
  applySemanticSlot(semantic.content, &style.contentColor, nullptr, nullptr);
  applySemanticSlot(semantic.arrow, nullptr, &style.arrowBackground, &style.arrowBorderColor);

  if (!semantic.arrow.backgroundColor.has_value()) {
    style.arrowBackground = style.surfaceBackground;
  }
  if (!semantic.arrow.borderColor.has_value()) {
    style.arrowBorderColor = style.surfaceBorderColor;
  }

  if (input.disabled) {
    style.contentColor = toColor(map.colorTextQuaternary, QColor("#bfbfbf"));
  }

  return style;
}

TooltipVisualStyle resolveTooltipVisualStyle(const TooltipStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveTooltipVisualStyle(input, resolved);
}

}  // namespace adqt::widgets::detail
