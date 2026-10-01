#include "radio_style.h"

#include "theme/theme.h"
#include "theme/theme_color_utils.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

template <typename T>
void applyOptional(T* target, const std::optional<T>& value) {
  if (target && value.has_value()) {
    *target = value.value();
  }
}

RadioMetrics baseMetrics(AdRadio::ControlSize controlSize, const QFont& baseFont,
                         const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;

  RadioMetrics metrics;
  metrics.borderWidth = std::max(1, qRound(map.lineWidth));
  metrics.radioSize = std::max(12, qRound(map.fontSizeLG));
  metrics.dotSize = std::max(4, metrics.radioSize - (4 + metrics.borderWidth) * 2);
  metrics.labelPaddingInlineStart = std::max(4, qRound(map.sizeXS));
  metrics.labelPaddingInlineEnd = std::max(4, qRound(map.sizeXS));
  metrics.wrapperMarginInlineEnd = std::max(0, qRound(map.sizeXS));
  metrics.buttonPaddingInline = std::max(4, qRound(map.sizeMS - map.lineWidth));
  metrics.buttonBorderRadius = std::max(0, qRound(map.borderRadius));
  metrics.buttonHeight = std::max(24, qRound(map.controlHeight));
  metrics.font = baseFont;

  int fontPixelSize = std::max(12, qRound(map.fontSize));
  if (controlSize == AdRadio::ControlSize::Large) {
    metrics.buttonHeight = std::max(28, qRound(map.controlHeightLG));
    metrics.buttonBorderRadius = std::max(0, qRound(map.borderRadiusLG));
    fontPixelSize = std::max(12, qRound(map.fontSizeLG));
  } else if (controlSize == AdRadio::ControlSize::Small) {
    metrics.buttonHeight = std::max(24, qRound(map.controlHeightSM));
    metrics.buttonBorderRadius = std::max(0, qRound(map.borderRadiusSM));
    metrics.buttonPaddingInline = std::max(4, qRound(map.sizeXS - map.lineWidth));
    fontPixelSize = std::max(12, qRound(map.fontSize));
  }

  metrics.font.setPixelSize(fontPixelSize);
  metrics.textLineHeight = std::max(metrics.radioSize, qRound(fontPixelSize * map.lineHeight));
  metrics.focusOutlineColor = toColor(map.colorPrimaryBorder, QColor(QStringLiteral("#91caff")));
  metrics.focusOutlineWidth = std::max<qreal>(1.0, map.lineWidth * 3.0);
  metrics.focusOutlineOffset = 1.0;
  return metrics;
}

void applyMetricTokens(const AdRadio::MetricTokens& tokens, RadioMetrics* metrics) {
  if (!metrics) {
    return;
  }

  applyOptional(&metrics->radioSize, tokens.radioSize);
  applyOptional(&metrics->dotSize, tokens.dotSize);
  applyOptional(&metrics->borderWidth, tokens.borderWidth);
  applyOptional(&metrics->labelPaddingInlineStart, tokens.labelPaddingInlineStart);
  applyOptional(&metrics->labelPaddingInlineEnd, tokens.labelPaddingInlineEnd);
  applyOptional(&metrics->textLineHeight, tokens.textLineHeight);
  applyOptional(&metrics->wrapperMarginInlineEnd, tokens.wrapperMarginInlineEnd);
  applyOptional(&metrics->buttonHeight, tokens.buttonHeight);
  applyOptional(&metrics->buttonPaddingInline, tokens.buttonPaddingInline);
  applyOptional(&metrics->buttonBorderRadius, tokens.buttonBorderRadius);
  applyOptional(&metrics->focusOutlineWidth, tokens.focusOutlineWidth);
  applyOptional(&metrics->focusOutlineOffset, tokens.focusOutlineOffset);

  metrics->radioSize = std::max(metrics->radioSize, 12);
  metrics->dotSize = std::max(0, std::min(metrics->dotSize, metrics->radioSize));
  metrics->borderWidth = std::max(metrics->borderWidth, 0);
  metrics->labelPaddingInlineStart = std::max(metrics->labelPaddingInlineStart, 0);
  metrics->labelPaddingInlineEnd = std::max(metrics->labelPaddingInlineEnd, 0);
  metrics->textLineHeight = std::max(metrics->textLineHeight, 0);
  metrics->wrapperMarginInlineEnd = std::max(metrics->wrapperMarginInlineEnd, 0);
  metrics->buttonHeight = std::max(metrics->buttonHeight, 0);
  metrics->buttonPaddingInline = std::max(metrics->buttonPaddingInline, 0);
  metrics->buttonBorderRadius = std::max(metrics->buttonBorderRadius, 0);
  metrics->focusOutlineWidth = std::max<qreal>(0.0, metrics->focusOutlineWidth);
  metrics->focusOutlineOffset = std::max<qreal>(0.0, metrics->focusOutlineOffset);
}

void applyDefaultColorTokens(const AdRadio::ColorTokens& tokens, RadioDotStateStyle* state) {
  if (!state) {
    return;
  }
  applyOptional(&state->labelColor, tokens.textColor);
  applyOptional(&state->borderColor, tokens.indicatorBorderColor);
  applyOptional(&state->backgroundColor, tokens.indicatorFillColor);
  applyOptional(&state->dotColor, tokens.indicatorDotColor);
}

void applyButtonColorTokens(const AdRadio::ColorTokens& tokens, RadioButtonStateStyle* state) {
  if (!state) {
    return;
  }
  applyOptional(&state->textColor, tokens.buttonTextColor);
  applyOptional(&state->backgroundColor, tokens.buttonFillColor);
  applyOptional(&state->borderColor, tokens.buttonBorderColor);
}

}  // namespace

RadioVisualStyle resolveRadioVisualStyle(const RadioStyleInput& input,
                                         const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const bool wireframe = resolved.config.wireframe;

  const QColor colorText = toColor(map.colorText, QColor(QStringLiteral("#141414")));
  const QColor colorTextDisabled =
      toColor(map.colorTextDisabled, QColor(QStringLiteral("#bfbfbf")));
  const QColor colorBorder = toColor(map.colorBorder, QColor(QStringLiteral("#d9d9d9")));
  const QColor colorBgContainer = toColor(map.colorBgContainer, QColor(QStringLiteral("#ffffff")));
  const QColor colorBgContainerDisabled =
      toColor(map.colorBgContainerDisabled, QColor(QStringLiteral("#f5f5f5")));
  const QColor colorPrimary = toColor(map.colorPrimary, QColor(QStringLiteral("#1677ff")));
  const QColor colorPrimaryHover =
      toColor(map.colorPrimaryHover, QColor(QStringLiteral("#4096ff")));
  const QColor colorWhite = toColor(map.colorWhite, QColor(QStringLiteral("#ffffff")));

  RadioVisualStyle style;
  style.metrics = baseMetrics(input.controlSize, input.baseFont, resolved);
  if (wireframe) {
    style.metrics.dotSize = std::max(4, style.metrics.radioSize - 8);
  }

  const QColor dotColor = wireframe ? colorPrimary : colorWhite;
  const QColor dotColorDisabled = colorTextDisabled;
  const QColor checkedBackground = wireframe ? colorBgContainer : colorPrimary;
  const QColor checkedHoverBackground = colorPrimaryHover;

  style.normal = {colorBorder, colorBgContainer, dotColor, colorText};
  style.hover = {colorPrimary, colorBgContainer, dotColor, colorText};
  style.active = style.hover;
  style.checked = {colorPrimary, checkedBackground, dotColor, colorText};
  style.checkedHover = {colorPrimaryHover, checkedHoverBackground, dotColor, colorText};
  style.disabled = {colorBorder, colorBgContainerDisabled, dotColorDisabled, colorTextDisabled};
  style.checkedDisabled = {colorBorder, colorBgContainerDisabled, dotColorDisabled,
                           colorTextDisabled};

  applyMetricTokens(input.componentTokens.metrics, &style.metrics);
  applyOptional(&style.metrics.focusOutlineColor, input.componentTokens.colors.focusRingColor);

  applyDefaultColorTokens(input.componentTokens.colors, &style.normal);
  applyDefaultColorTokens(input.componentTokens.colors, &style.hover);
  applyDefaultColorTokens(input.componentTokens.colors, &style.active);
  applyDefaultColorTokens(input.componentTokens.colors, &style.checked);
  applyDefaultColorTokens(input.componentTokens.colors, &style.checkedHover);
  applyDefaultColorTokens(input.componentTokens.colors, &style.disabled);
  applyDefaultColorTokens(input.componentTokens.colors, &style.checkedDisabled);

  style.metrics.waveColor =
      input.componentTokens.colors.waveColor.value_or(style.checked.borderColor);
  if (map.backgroundOpacity != 1.0) {
    for (RadioDotStateStyle* state :
         {&style.normal, &style.hover, &style.active, &style.checked, &style.checkedHover,
          &style.disabled, &style.checkedDisabled}) {
      state->backgroundColor =
          adqt::theme::applyBackgroundOpacity(state->backgroundColor, map.backgroundOpacity);
    }
  }
  return style;
}

RadioButtonVisualStyle resolveRadioButtonVisualStyle(const RadioButtonStyleInput& input,
                                                     const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const QColor colorPrimary = toColor(map.colorPrimary, QColor(QStringLiteral("#1677ff")));
  const QColor colorPrimaryHover =
      toColor(map.colorPrimaryHover, QColor(QStringLiteral("#4096ff")));
  const QColor colorPrimaryActive =
      toColor(map.colorPrimaryActive, QColor(QStringLiteral("#0958d9")));
  const QColor colorText = toColor(map.colorText, QColor(QStringLiteral("#141414")));
  const QColor colorTextDisabled =
      toColor(map.colorTextDisabled, QColor(QStringLiteral("#bfbfbf")));
  const QColor colorBorder = toColor(map.colorBorder, QColor(QStringLiteral("#d9d9d9")));
  const QColor colorBgContainer = toColor(map.colorBgContainer, QColor(QStringLiteral("#ffffff")));
  const QColor colorBgContainerDisabled =
      toColor(map.colorBgContainerDisabled, QColor(QStringLiteral("#f5f5f5")));
  const QColor colorWhite = toColor(map.colorWhite, QColor(QStringLiteral("#ffffff")));

  RadioButtonVisualStyle style;
  style.metrics = baseMetrics(input.controlSize, input.baseFont, resolved);

  style.normal = {colorText, colorBgContainer, colorBorder};
  style.hover = {colorPrimary, colorBgContainer, colorBorder};
  style.active = style.hover;
  style.checked = {colorPrimary, colorBgContainer, colorPrimary};
  style.checkedHover = {colorPrimaryHover, colorBgContainer, colorPrimaryHover};
  style.checkedActive = {colorPrimaryActive, colorBgContainer, colorPrimaryActive};
  style.disabled = {colorTextDisabled, colorBgContainerDisabled, colorBorder};
  style.checkedDisabled = {colorTextDisabled, colorBgContainerDisabled, colorBorder};

  if (input.buttonStyle == AdRadio::ButtonStyle::Solid) {
    style.checked = {colorWhite, colorPrimary, colorPrimary};
    style.checkedHover = {colorWhite, colorPrimaryHover, colorPrimaryHover};
    style.checkedActive = {colorWhite, colorPrimaryActive, colorPrimaryActive};
  }

  applyMetricTokens(input.componentTokens.metrics, &style.metrics);
  applyOptional(&style.metrics.focusOutlineColor, input.componentTokens.colors.focusRingColor);

  applyButtonColorTokens(input.componentTokens.colors, &style.normal);
  applyButtonColorTokens(input.componentTokens.colors, &style.hover);
  applyButtonColorTokens(input.componentTokens.colors, &style.active);
  applyButtonColorTokens(input.componentTokens.colors, &style.checked);
  applyButtonColorTokens(input.componentTokens.colors, &style.checkedHover);
  applyButtonColorTokens(input.componentTokens.colors, &style.checkedActive);
  applyButtonColorTokens(input.componentTokens.colors, &style.disabled);
  applyButtonColorTokens(input.componentTokens.colors, &style.checkedDisabled);

  style.metrics.waveColor =
      input.componentTokens.colors.waveColor.value_or(style.checked.borderColor);
  if (map.backgroundOpacity != 1.0) {
    for (RadioButtonStateStyle* state :
         {&style.normal, &style.hover, &style.active, &style.checked, &style.checkedHover,
          &style.checkedActive, &style.disabled, &style.checkedDisabled}) {
      state->backgroundColor =
          adqt::theme::applyBackgroundOpacity(state->backgroundColor, map.backgroundOpacity);
    }
  }
  return style;
}

RadioVisualStyle resolveRadioVisualStyle(const RadioStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveRadioVisualStyle(input, resolved);
}

RadioButtonVisualStyle resolveRadioButtonVisualStyle(const RadioButtonStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveRadioButtonVisualStyle(input, resolved);
}

}  // namespace adqt::widgets::detail
