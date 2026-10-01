#include "slider_style.h"

#include <algorithm>
#include <cmath>

#include "theme/theme.h"
#include "theme/theme_color_utils.h"

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

QColor withAlpha(const QColor& color, float alpha) {
  QColor updated = color;
  updated.setAlphaF(std::clamp(alpha, 0.0F, 1.0F));
  return updated;
}

QColor compositeOverBackground(const QColor& foreground, const QColor& background) {
  if (!foreground.isValid()) {
    return background;
  }
  if (!background.isValid()) {
    return foreground;
  }

  const float fgAlpha = std::clamp(foreground.alphaF(), 0.0F, 1.0F);
  const float bgAlpha = std::clamp(background.alphaF(), 0.0F, 1.0F);
  const float outAlpha = fgAlpha + bgAlpha * (1.0F - fgAlpha);
  if (outAlpha <= 0.0F) {
    return QColor(0, 0, 0, 0);
  }

  auto compositeChannel = [fgAlpha, bgAlpha, outAlpha](int fg, int bg) {
    const float channel =
        (static_cast<float>(fg) * fgAlpha + static_cast<float>(bg) * bgAlpha * (1.0F - fgAlpha)) /
        outAlpha;
    return std::clamp(static_cast<int>(std::round(channel)), 0, 255);
  };

  QColor result(compositeChannel(foreground.red(), background.red()),
                compositeChannel(foreground.green(), background.green()),
                compositeChannel(foreground.blue(), background.blue()));
  result.setAlphaF(outAlpha);
  return result;
}

void applySemanticSlotColor(const AdMultiSlider::SemanticSlotStyle& slot, QColor* textColor,
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

int resolveMainAxisMargin(const SliderMetrics& metrics) {
  const qreal handleRadius = std::max(metrics.handleSize, metrics.handleSizeHover) / 2.0;
  const qreal handleBorder = std::max<qreal>(metrics.handleLineWidth, metrics.handleLineWidthHover);
  const qreal handleOutline = std::max<qreal>(0.0, metrics.focusOutlineSize);
  // Reserve one extra pixel for antialiasing at the viewport boundary.
  constexpr qreal kAntialiasPadding = 1.0;
  return std::max(
      0, static_cast<int>(
             std::ceil(handleRadius + std::max(handleBorder, handleOutline) + kAntialiasPadding)));
}

}  // namespace

SliderVisualStyle resolveSliderVisualStyle(const SliderStyleInput& input,
                                           const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const adqt::theme::ThemeSeedToken& seed = resolved.config;

  SliderVisualStyle style;
  style.backgroundOpacity = map.backgroundOpacity;
  style.rootBg = QColor(0, 0, 0, 0);
  style.railBg = toColor(map.colorFillTertiary, QColor("#f5f5f5"));
  style.railHoverBg = toColor(map.colorFillSecondary, QColor("#f0f0f0"));
  style.useRailBrush = false;
  style.trackBg = toColor(map.colorPrimaryBorder, QColor("#91caff"));
  style.trackHoverBg = toColor(map.colorPrimaryBorderHover, QColor("#69b1ff"));
  style.handleColor = toColor(map.colorPrimaryBorder, QColor("#91caff"));
  style.handleHoverColor = toColor(map.colorPrimaryBorderHover, QColor("#69b1ff"));
  style.handleActiveColor = toColor(map.colorPrimary, QColor("#1677ff"));
  style.handleActiveOutlineColor = withAlpha(style.handleActiveColor, 0.2F);
  style.handleShadowColor = QColor(0, 0, 0, 0);
  style.handleActiveShadowColor = QColor(0, 0, 0, 0);
  style.handleColorDisabled =
      compositeOverBackground(toColor(map.colorTextDisabled, QColor(0, 0, 0, 64)),
                              toColor(map.colorBgContainer, QColor("#ffffff")));
  style.surfaceBg = toColor(map.colorBgElevated, QColor("#ffffff"));
  style.useHandleBrush = false;
  style.dotBorderColor = toColor(map.colorBorderSecondary, QColor("#f0f0f0"));
  style.dotHoverBorderColor = toColor(map.colorFill, QColor(0, 0, 0, 38));
  style.dotActiveBorderColor = toColor(map.colorPrimaryBorder, QColor("#91caff"));
  style.trackBgDisabled = toColor(map.colorFillTertiary, QColor("#f5f5f5"));
  style.markColor = toColor(map.colorTextTertiary, QColor("#8c8c8c"));
  style.markActiveColor = toColor(map.colorText, QColor("#141414"));
  style.tooltipBg = toColor(map.colorBgSpotlight, QColor("#141414"));
  style.tooltipText = toColor(map.colorTextLightSolid, QColor("#ffffff"));
  style.useTracksBrush = false;

  const int defaultControlSize = std::max(8, qRound(map.controlHeightLG / 4.0));
  const int defaultControlSizeHover = std::max(8, qRound(map.controlHeightSM / 2.0));
  style.metrics.controlSize = defaultControlSize;
  style.metrics.railSize = 4;
  style.metrics.handleSize = defaultControlSize;
  style.metrics.handleSizeHover = defaultControlSizeHover;
  style.metrics.handleLineWidth = std::max<qreal>(1.0, map.lineWidth + 1.0);
  style.metrics.handleLineWidthHover = std::max<qreal>(1.0, map.lineWidth + 1.5);
  style.metrics.dotSize = 8;
  style.metrics.marginMain = resolveMainAxisMargin(style.metrics);
  style.metrics.marginCross =
      std::max(0, qRound((map.controlHeight - style.metrics.controlSize) / 2.0));
  style.metrics.markGap = std::max(0, qRound(map.controlHeightLG - style.metrics.controlSize));
  style.metrics.focusOutlineSize = 6.0;
  style.metrics.tooltipPaddingH = std::max(0, qRound(map.sizeXS));
  style.metrics.tooltipPaddingV = std::max(0, qRound(map.sizeSM / 2.0));
  style.metrics.tooltipRadius = std::max(0, qRound(map.borderRadius));
  style.metrics.tooltipOffset = std::max(0, qRound(map.sizeXXS));
  style.metrics.tooltipArrowSize = std::max(0, qRound(seed.sizePopupArrow / 2.0));
  style.metrics.font = input.baseFont;
  style.metrics.font.setPixelSize(std::max(12, qRound(map.fontSize)));

  const auto& tokens = input.componentTokens;
  if (tokens.controlSize.has_value()) {
    style.metrics.controlSize = std::max(8, tokens.controlSize.value());
  }
  if (tokens.railSize.has_value()) {
    style.metrics.railSize = std::max(2, tokens.railSize.value());
  }
  if (tokens.handleSize.has_value()) {
    style.metrics.handleSize = std::max(8, tokens.handleSize.value());
  }
  if (tokens.handleSizeHover.has_value()) {
    style.metrics.handleSizeHover = std::max(4, tokens.handleSizeHover.value());
  }
  if (tokens.handleLineWidth.has_value()) {
    style.metrics.handleLineWidth = std::max<qreal>(1.0, tokens.handleLineWidth.value());
  }
  if (tokens.handleLineWidthHover.has_value()) {
    style.metrics.handleLineWidthHover = std::max<qreal>(1.0, tokens.handleLineWidthHover.value());
  }
  if (tokens.focusOutlineSize.has_value()) {
    style.metrics.focusOutlineSize = std::max<qreal>(0.0, tokens.focusOutlineSize.value());
  }
  if (tokens.dotSize.has_value()) {
    style.metrics.dotSize = std::max(4, tokens.dotSize.value());
  }

  if (tokens.marginMain.has_value()) {
    style.metrics.marginMain = std::max(0, tokens.marginMain.value());
  } else {
    style.metrics.marginMain = resolveMainAxisMargin(style.metrics);
  }

  if (tokens.marginCross.has_value()) {
    style.metrics.marginCross = std::max(0, tokens.marginCross.value());
  } else {
    style.metrics.marginCross =
        std::max(0, qRound((map.controlHeight - style.metrics.controlSize) / 2.0));
  }

  if (tokens.markGap.has_value()) {
    style.metrics.markGap = std::max(0, tokens.markGap.value());
  } else {
    style.metrics.markGap = std::max(0, qRound(map.controlHeightLG - style.metrics.controlSize));
  }

  style.railBg = resolveTokenColor(tokens.railBg, style.railBg);
  style.railHoverBg = resolveTokenColor(tokens.railHoverBg, style.railHoverBg);
  style.trackBg = resolveTokenColor(tokens.trackBg, style.trackBg);
  style.trackHoverBg = resolveTokenColor(tokens.trackHoverBg, style.trackHoverBg);
  style.handleColor = resolveTokenColor(tokens.handleColor, style.handleColor);
  style.handleHoverColor = toColor(map.colorPrimaryBorderHover, style.handleColor);
  style.handleActiveColor = resolveTokenColor(tokens.handleActiveColor, style.handleActiveColor);
  style.handleActiveOutlineColor = withAlpha(style.handleActiveColor, 0.2F);
  style.handleActiveOutlineColor =
      resolveTokenColor(tokens.handleActiveOutlineColor, style.handleActiveOutlineColor);
  style.handleShadowColor = resolveTokenColor(tokens.handleShadowColor, style.handleShadowColor);
  style.handleActiveShadowColor =
      resolveTokenColor(tokens.handleActiveShadowColor, style.handleActiveShadowColor);
  if (!tokens.handleActiveShadowColor.has_value()) {
    style.handleActiveShadowColor = style.handleShadowColor;
  }
  style.handleColorDisabled =
      resolveTokenColor(tokens.handleColorDisabled, style.handleColorDisabled);
  style.dotBorderColor = resolveTokenColor(tokens.dotBorderColor, style.dotBorderColor);
  style.dotHoverBorderColor = toColor(map.colorFill, style.dotBorderColor);
  style.dotActiveBorderColor =
      resolveTokenColor(tokens.dotActiveBorderColor, style.dotActiveBorderColor);
  style.trackBgDisabled = resolveTokenColor(tokens.trackBgDisabled, style.trackBgDisabled);

  return input.deferSemanticStyles
             ? style
             : applySliderSemanticStyles(style, input.semanticStyles, input.disabled);
}

SliderVisualStyle applySliderSemanticStyles(const SliderVisualStyle& baseStyle,
                                            const AdMultiSlider::SemanticStyles& semanticStyles,
                                            bool disabled) {
  SliderVisualStyle style = baseStyle;
  const auto& semantic = semanticStyles;
  const bool hasSemanticRailBg = semantic.rail.backgroundColor.has_value();
  const bool hasSemanticTrackBg = semantic.track.backgroundColor.has_value();
  const bool hasSemanticTracksBg = semantic.tracks.backgroundColor.has_value();
  applySemanticSlotColor(semantic.root, nullptr, &style.rootBg, nullptr);
  applySemanticSlotColor(semantic.rail, nullptr, &style.railBg, nullptr);
  applySemanticSlotColor(semantic.track, nullptr, &style.trackBg, nullptr);
  applySemanticSlotColor(semantic.tracks, nullptr, &style.trackBg, nullptr);
  applySemanticSlotColor(semantic.handle, nullptr, nullptr, &style.handleColor);
  if (semantic.rail.brush.has_value()) {
    style.railBrush = semantic.rail.brush.value();
    style.useRailBrush = true;
  }
  if (hasSemanticRailBg) {
    style.railHoverBg = style.railBg;
  }
  if (hasSemanticTrackBg || hasSemanticTracksBg) {
    style.trackHoverBg = style.trackBg;
  }
  if (semantic.handle.borderColor.has_value()) {
    style.handleHoverColor = semantic.handle.borderColor.value();
    style.handleActiveColor = semantic.handle.borderColor.value();
    style.handleActiveOutlineColor = withAlpha(style.handleActiveColor, 0.2F);
  }
  if (semantic.handle.backgroundColor.has_value()) {
    style.surfaceBg = semantic.handle.backgroundColor.value();
  }
  if (semantic.handle.brush.has_value()) {
    style.handleBrush = semantic.handle.brush.value();
    style.useHandleBrush = true;
  }
  applySemanticSlotColor(semantic.mark, &style.markColor, nullptr, nullptr);
  applySemanticSlotColor(semantic.markActive, &style.markActiveColor, nullptr, nullptr);
  if (semantic.tracks.brush.has_value()) {
    style.tracksBrush = semantic.tracks.brush.value();
    style.useTracksBrush = true;
  }

  if (disabled) {
    style.railHoverBg = style.railBg;
    style.trackBg = style.trackBgDisabled;
    style.trackHoverBg = style.trackBgDisabled;
    style.handleColor = style.handleColorDisabled;
    style.handleHoverColor = style.handleColorDisabled;
    style.handleActiveColor = style.handleColorDisabled;
    style.handleActiveOutlineColor = QColor(0, 0, 0, 0);
    style.handleShadowColor = QColor(0, 0, 0, 0);
    style.handleActiveShadowColor = QColor(0, 0, 0, 0);
    style.dotBorderColor = style.trackBgDisabled;
    style.dotHoverBorderColor = style.trackBgDisabled;
    style.dotActiveBorderColor = style.trackBgDisabled;
    style.useTracksBrush = false;
    style.useHandleBrush = false;
  }

  if (style.backgroundOpacity != 1.0) {
    for (QColor* background : {&style.rootBg, &style.railBg, &style.railHoverBg, &style.trackBg,
                               &style.trackHoverBg, &style.trackBgDisabled}) {
      *background = adqt::theme::applyBackgroundOpacity(*background, style.backgroundOpacity);
    }
    // Color-channel gradients and checkerboard textures are content, while solid
    // semantic brushes are another way to customize the control background.
    for (QBrush* brush : {&style.railBrush, &style.tracksBrush}) {
      if (!brush->gradient() && brush->style() != Qt::TexturePattern) {
        brush->setColor(
            adqt::theme::applyBackgroundOpacity(brush->color(), style.backgroundOpacity));
      }
    }
  }
  return style;
}

SliderVisualStyle resolveSliderVisualStyle(const SliderStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveSliderVisualStyle(input, resolved);
}

}  // namespace adqt::widgets::detail
