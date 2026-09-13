#include "color_picker_style.h"

#include <algorithm>
#include <cmath>

#include "theme/theme.h"

namespace adqt::widgets::detail {

namespace {

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor resolveTokenColor(const std::optional<QColor>& value, const QColor& fallback) {
  if (!value.has_value()) {
    return fallback;
  }
  return toColor(value.value(), fallback);
}

bool isStableColorChannel(int channel) { return channel >= 0 && channel <= 255; }

QColor deriveAlphaColor(const QColor& frontColor, const QColor& backgroundColor) {
  if (!frontColor.isValid()) {
    return frontColor;
  }
  if (!backgroundColor.isValid()) {
    QColor fallback = frontColor;
    fallback.setAlpha(255);
    return fallback;
  }

  if (static_cast<double>(frontColor.alphaF()) < 0.999) {
    return frontColor;
  }

  const int fR = frontColor.red();
  const int fG = frontColor.green();
  const int fB = frontColor.blue();
  const int bR = backgroundColor.red();
  const int bG = backgroundColor.green();
  const int bB = backgroundColor.blue();

  for (int alphaPercent = 1; alphaPercent <= 100; ++alphaPercent) {
    const qreal alpha = static_cast<qreal>(alphaPercent) / 100.0;
    const int r = qRound((fR - bR * (1.0 - alpha)) / alpha);
    const int g = qRound((fG - bG * (1.0 - alpha)) / alpha);
    const int b = qRound((fB - bB * (1.0 - alpha)) / alpha);
    if (isStableColorChannel(r) && isStableColorChannel(g) && isStableColorChannel(b)) {
      QColor result(r, g, b);
      result.setAlphaF(static_cast<float>(alpha));
      return result;
    }
  }

  QColor fallback = frontColor;
  fallback.setAlpha(255);
  return fallback;
}

struct RadiusValues {
  double borderRadius = 0.0;
  double borderRadiusXS = 0.0;
  double borderRadiusSM = 0.0;
  double borderRadiusLG = 0.0;
};

RadiusValues deriveRadiusValues(double radiusBase) {
  double radiusLG = radiusBase;
  double radiusSM = radiusBase;
  double radiusXS = radiusBase;

  if (radiusBase < 6.0 && radiusBase >= 5.0) {
    radiusLG = radiusBase + 1.0;
  } else if (radiusBase < 16.0 && radiusBase >= 6.0) {
    radiusLG = radiusBase + 2.0;
  } else if (radiusBase >= 16.0) {
    radiusLG = 16.0;
  }

  if (radiusBase < 7.0 && radiusBase >= 5.0) {
    radiusSM = 4.0;
  } else if (radiusBase < 8.0 && radiusBase >= 7.0) {
    radiusSM = 5.0;
  } else if (radiusBase < 14.0 && radiusBase >= 8.0) {
    radiusSM = 6.0;
  } else if (radiusBase < 16.0 && radiusBase >= 14.0) {
    radiusSM = 7.0;
  } else if (radiusBase >= 16.0) {
    radiusSM = 8.0;
  }

  if (radiusBase < 6.0 && radiusBase >= 2.0) {
    radiusXS = 1.0;
  } else if (radiusBase >= 6.0) {
    radiusXS = 2.0;
  }

  RadiusValues values;
  values.borderRadius = radiusBase;
  values.borderRadiusXS = radiusXS;
  values.borderRadiusSM = radiusSM;
  values.borderRadiusLG = radiusLG;
  return values;
}

}  // namespace

ColorPickerVisualStyle resolveColorPickerVisualStyle(const ColorPickerStyleInput& input,
                                                     const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const adqt::theme::ThemeSemanticPalette& semantic = resolved.theme.semantic;
  const QColor containerBg = toColor(map.colorBgContainer, QColor("#ffffff"));
  const QColor controlOutline =
      deriveAlphaColor(toColor(map.colorPrimaryBg, QColor("#e6f4ff")), containerBg);

  ColorPickerVisualStyle style;
  style.triggerBackground = toColor(map.colorBgElevated, QColor("#ffffff"));
  style.triggerBackgroundDisabled = toColor(map.colorFillTertiary, QColor("#f5f5f5"));
  style.triggerBorder = toColor(map.colorBorder, QColor("#d9d9d9"));
  style.triggerBorderHover = toColor(map.colorPrimaryHover, QColor("#4096ff"));
  style.triggerBorderActive = toColor(map.colorPrimary, QColor("#1677ff"));
  style.triggerFocusOutline = controlOutline;
  style.triggerText = toColor(map.colorText, QColor("#141414"));
  style.triggerTextDisabled = toColor(map.colorTextQuaternary, QColor("#bfbfbf"));
  style.panelBackground = toColor(map.colorBgElevated, QColor("#ffffff"));
  style.panelBorder = toColor(map.colorBorderSecondary, QColor("#f0f0f0"));
  style.panelText = toColor(map.colorText, QColor("#141414"));
  style.swatchBorder = toColor(map.colorBorderSecondary, QColor("#f0f0f0"));
  style.presetBorder = toColor(map.colorBorderSecondary, QColor("#f0f0f0"));
  style.presetBorderHover = toColor(map.colorPrimaryHover, QColor("#4096ff"));
  style.presetText = toColor(semantic.text, style.panelText);
  style.presetArrow = toColor(semantic.textQuaternary, style.triggerTextDisabled);
  style.presetEmptyText = style.presetArrow;
  style.segmentedBackground =
      toColor(semantic.surfaceSubtle, toColor(map.colorBgLayout, QColor("#f5f5f5")));
  style.segmentedItemBackground = toColor(semantic.surface, style.panelBackground);
  style.segmentedItemHoverBackground =
      toColor(semantic.fillSecondary, toColor(map.colorFillSecondary, QColor("#f0f0f0")));
  style.segmentedText = toColor(semantic.textSecondary, style.panelText);
  style.segmentedTextDisabled = toColor(semantic.textQuaternary, style.triggerTextDisabled);
  style.segmentedTextChecked = style.panelText;
  style.clearButtonSlash = toColor(semantic.error, toColor(map.colorError, QColor("#ff4d4f")));
  style.channelHandleBorder = toColor(semantic.white, toColor(map.colorWhite, QColor("#ffffff")));
  style.invalidSwatchFill = toColor(semantic.accent, toColor(map.colorPrimary, QColor("#1677ff")));
  style.transparentCellA = style.panelBackground;
  style.transparentCellB = toColor(map.colorFillSecondary, QColor("#f0f0f0"));

  style.metrics.controlHeightXS = std::max(16, qRound(map.controlHeightXS));
  style.metrics.controlHeightSM = std::max(20, qRound(map.controlHeightSM));
  style.metrics.controlHeight = std::max(24, qRound(map.controlHeight));
  style.metrics.controlHeightLG = std::max(28, qRound(map.controlHeightLG));
  style.metrics.triggerMinWidth = std::max(24, qRound(map.controlHeight));
  style.metrics.triggerRadiusSM = std::max(0, qRound(map.borderRadiusSM));
  style.metrics.triggerRadius = std::max(0, qRound(map.borderRadius));
  style.metrics.triggerRadiusLG = std::max(0, qRound(map.borderRadiusLG));
  style.metrics.triggerPadding = std::max(0, qRound(map.sizeXXS - map.lineWidth));
  style.metrics.triggerTextGap = std::max(0, qRound(map.sizeXS));
  style.metrics.triggerTextMarginEnd =
      std::max(0, qRound(map.sizeXS) - style.metrics.triggerPadding);
  style.metrics.triggerTextFontSizeLG = std::max(12, qRound(map.fontSizeLG));
  style.metrics.triggerTextLineHeightSM = std::max(0, qRound(map.controlHeightXS));
  style.metrics.swatchSizeSM = std::max(8, qRound(map.controlHeightXS));
  style.metrics.swatchSize = std::max(10, qRound(map.controlHeightSM));
  style.metrics.swatchSizeLG = std::max(12, qRound(map.controlHeight));
  style.metrics.swatchRadiusSM = std::max(0, qRound(map.borderRadiusXS));
  style.metrics.swatchRadius = std::max(0, qRound(map.borderRadiusSM));
  style.metrics.swatchRadiusLG = std::max(0, qRound(map.borderRadius));
  style.metrics.panelWidth = 234;
  style.metrics.marginXS = std::max(0, qRound(map.sizeXS));
  style.metrics.marginSM = std::max(style.metrics.marginXS, qRound(map.sizeSM));
  style.metrics.panelPadding = std::max(8, qRound(map.sizeSM));
  style.metrics.panelSpacing = std::max(0, qRound(map.sizeXS));
  style.metrics.presetSwatchSize = 24;
  style.metrics.inputHeight = std::max(20, qRound(map.controlHeightSM));
  style.metrics.borderWidth = std::max<qreal>(1.0, map.lineWidth);
  style.metrics.font = input.baseFont;
  style.metrics.font.setPixelSize(std::max(12, qRound(map.fontSize)));

  // Derived sizes for color picker internals
  style.metrics.saturationPanelHeight = 160;
  style.metrics.saturationPanelRadius = qMax(2, qRound(map.borderRadiusSM));
  style.metrics.sliderHeight = 8;
  style.metrics.sliderControlSize = style.metrics.sliderHeight;
  style.metrics.sliderHandleLineWidth = std::max(1, qRound(map.lineWidthBold));
  style.metrics.sliderHandleLineWidthHover = style.metrics.sliderHandleLineWidth;
  constexpr int kSliderHandleOuterSize = 12;
  style.metrics.sliderHandleSize =
      std::max(4, kSliderHandleOuterSize - style.metrics.sliderHandleLineWidth * 2);
  style.metrics.sliderHandleSizeHover = style.metrics.sliderHandleSize;
  style.metrics.sliderMarginCross =
      std::max(0, (kSliderHandleOuterSize - style.metrics.sliderHeight) / 2);
  // Reserve one extra pixel so the active handle ring is not clipped by antialiasing.
  constexpr qreal kSliderAntialiasPadding = 1.0;
  const qreal sliderOuterRadius =
      style.metrics.sliderHandleSizeHover / 2.0 + style.metrics.sliderHandleLineWidthHover;
  const qreal sliderHalfHeight = style.metrics.sliderHeight / 2.0;
  const int requiredCrossPadding = std::max(
      0,
      static_cast<int>(std::ceil(sliderOuterRadius + kSliderAntialiasPadding - sliderHalfHeight)));
  style.metrics.sliderMarginCross = std::max(style.metrics.sliderMarginCross, requiredCrossPadding);
  // Color picker sliders use a 1px outer shadow ring around the handle.
  // Reserve enough main-axis space so the shadow/border never touches the widget edge.
  constexpr qreal kSliderShadowRingPadding = 1.0;
  const qreal sliderHandleRadius =
      std::max(style.metrics.sliderHandleSize, style.metrics.sliderHandleSizeHover) / 2.0;
  const qreal sliderHandleBorder =
      std::max(static_cast<qreal>(style.metrics.sliderHandleLineWidth),
               static_cast<qreal>(style.metrics.sliderHandleLineWidthHover));
  const int requiredMainPadding =
      std::max(0, static_cast<int>(std::ceil(sliderHandleRadius + sliderHandleBorder +
                                             kSliderShadowRingPadding + kSliderAntialiasPadding)));
  style.metrics.sliderMarginMain = std::max(
      kSliderHandleOuterSize / 2, std::max(style.metrics.sliderMarginCross, requiredMainPadding));
  style.metrics.sliderVisualHeight =
      style.metrics.sliderHeight + style.metrics.sliderMarginCross * 2;

  constexpr int kGradientHandleOuterSize = 10;
  style.metrics.gradientHandleSize =
      std::max(4, kGradientHandleOuterSize - style.metrics.sliderHandleLineWidth * 2);
  style.metrics.gradientHandleSizeHover = style.metrics.sliderHandleSize;

  style.metrics.previewSwatchSize =
      style.metrics.sliderHeight * 2 + std::max(0, style.metrics.marginSM);
  style.metrics.previewSwatchRadius = qMax(2, qRound(map.borderRadiusSM));
  style.metrics.alphaInputWidth = 44;
  style.metrics.focusOutlineWidth = std::max<qreal>(1.0, map.lineWidth * 3.0);
  style.metrics.focusOutlineOffset = 0.0;

  const auto& tokens = input.componentTokens;
  if (tokens.controlHeight.has_value()) {
    style.metrics.controlHeight = std::max(20, tokens.controlHeight.value());
    style.metrics.controlHeightSM = std::max(16, qRound(style.metrics.controlHeight * 0.75));
    style.metrics.controlHeightXS = std::max(12, qRound(style.metrics.controlHeight * 0.5));
    style.metrics.controlHeightLG =
        std::max(style.metrics.controlHeight + 4, qRound(style.metrics.controlHeight * 1.25));
    style.metrics.triggerMinWidth = style.metrics.controlHeight;
    style.metrics.swatchSizeSM = std::max(8, style.metrics.controlHeightXS);
    style.metrics.swatchSize = std::max(10, style.metrics.controlHeightSM);
    style.metrics.swatchSizeLG = std::max(12, style.metrics.controlHeight);
  }
  if (tokens.triggerMinWidth.has_value()) {
    style.metrics.triggerMinWidth = std::max(24, tokens.triggerMinWidth.value());
  }
  if (tokens.triggerRadius.has_value()) {
    const RadiusValues radiusValues =
        deriveRadiusValues(std::max(0.0, static_cast<double>(tokens.triggerRadius.value())));
    style.metrics.triggerRadiusSM = std::max(0, qRound(radiusValues.borderRadiusSM));
    style.metrics.triggerRadius = std::max(0, qRound(radiusValues.borderRadius));
    style.metrics.triggerRadiusLG = std::max(0, qRound(radiusValues.borderRadiusLG));
    style.metrics.swatchRadiusSM = std::max(0, qRound(radiusValues.borderRadiusXS));
    style.metrics.swatchRadius = std::max(0, qRound(radiusValues.borderRadiusSM));
    style.metrics.swatchRadiusLG = std::max(0, qRound(radiusValues.borderRadius));
  }
  if (tokens.panelWidth.has_value()) {
    style.metrics.panelWidth = std::max(1, tokens.panelWidth.value());
  }
  if (tokens.swatchSize.has_value()) {
    style.metrics.swatchSize = std::max(10, tokens.swatchSize.value());
    style.metrics.swatchSizeSM = std::max(8, style.metrics.swatchSize - 8);
    style.metrics.swatchSizeLG = std::max(style.metrics.swatchSize, style.metrics.swatchSize + 8);
  }
  if (tokens.presetSwatchSize.has_value()) {
    style.metrics.presetSwatchSize = std::max(12, tokens.presetSwatchSize.value());
  }
  if (tokens.panelPadding.has_value()) {
    style.metrics.panelPadding = std::max(0, tokens.panelPadding.value());
  }
  if (tokens.inputHeight.has_value()) {
    style.metrics.inputHeight = std::max(20, tokens.inputHeight.value());
  }

  style.metrics.triggerTextLineHeightSM = std::max(0, style.metrics.controlHeightXS);

  style.triggerBackground = resolveTokenColor(tokens.triggerBackground, style.triggerBackground);
  style.triggerBorder = resolveTokenColor(tokens.triggerBorderColor, style.triggerBorder);
  style.triggerBorderHover =
      resolveTokenColor(tokens.triggerBorderHoverColor, style.triggerBorderHover);
  style.triggerText = resolveTokenColor(tokens.triggerTextColor, style.triggerText);
  style.panelBackground = resolveTokenColor(tokens.panelBackground, style.panelBackground);
  style.panelBorder = resolveTokenColor(tokens.panelBorderColor, style.panelBorder);

  style.transparentCellA = style.panelBackground;

  if (input.disabled) {
    style.triggerText = style.triggerTextDisabled;
    style.triggerBorderHover = style.triggerBorder;
    style.triggerBorderActive = style.triggerBorder;
  }

  return style;
}

ColorPickerVisualStyle resolveColorPickerVisualStyle(const ColorPickerStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveColorPickerVisualStyle(input, resolved);
}

}  // namespace adqt::widgets::detail
