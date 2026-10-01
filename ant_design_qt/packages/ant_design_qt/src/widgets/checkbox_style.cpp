#include "checkbox_style.h"

#include "theme/theme.h"
#include "theme/theme_color_utils.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QColor validColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

template <typename T>
void applyOptional(T* target, const std::optional<T>& value) {
  if (target && value.has_value()) {
    *target = value.value();
  }
}

void applyColors(const AdCheckbox::ColorTokens& tokens, CheckboxStateStyle* state) {
  if (!state) {
    return;
  }
  applyOptional(&state->labelColor, tokens.textColor);
  applyOptional(&state->borderColor, tokens.indicatorBorderColor);
  applyOptional(&state->backgroundColor, tokens.indicatorFillColor);
  applyOptional(&state->markColor, tokens.indicatorMarkColor);
}

void applyMetrics(const AdCheckbox::MetricTokens& tokens, CheckboxMetrics* metrics) {
  if (!metrics) {
    return;
  }
  applyOptional(&metrics->checkboxSize, tokens.checkboxSize);
  applyOptional(&metrics->borderWidth, tokens.borderWidth);
  applyOptional(&metrics->borderRadius, tokens.borderRadius);
  applyOptional(&metrics->markWidth, tokens.markWidth);
  applyOptional(&metrics->labelPaddingInlineStart, tokens.labelPaddingInlineStart);
  applyOptional(&metrics->labelPaddingInlineEnd, tokens.labelPaddingInlineEnd);
  applyOptional(&metrics->textLineHeight, tokens.textLineHeight);
  applyOptional(&metrics->wrapperMarginInlineEnd, tokens.wrapperMarginInlineEnd);
  applyOptional(&metrics->focusOutlineWidth, tokens.focusOutlineWidth);
  applyOptional(&metrics->focusOutlineOffset, tokens.focusOutlineOffset);

  metrics->checkboxSize = std::max(8, metrics->checkboxSize);
  metrics->borderWidth = std::max(0, metrics->borderWidth);
  metrics->borderRadius = std::max(0, std::min(metrics->borderRadius, metrics->checkboxSize / 2));
  metrics->markWidth = std::max(1, metrics->markWidth);
  metrics->labelPaddingInlineStart = std::max(0, metrics->labelPaddingInlineStart);
  metrics->labelPaddingInlineEnd = std::max(0, metrics->labelPaddingInlineEnd);
  metrics->textLineHeight = std::max(0, metrics->textLineHeight);
  metrics->wrapperMarginInlineEnd = std::max(0, metrics->wrapperMarginInlineEnd);
  metrics->focusOutlineWidth = std::max<qreal>(0.0, metrics->focusOutlineWidth);
  metrics->focusOutlineOffset = std::max<qreal>(0.0, metrics->focusOutlineOffset);
}

}  // namespace

CheckboxVisualStyle resolveCheckboxVisualStyle(const CheckboxStyleInput& input,
                                               const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const QColor text = validColor(map.colorText, QColor(QStringLiteral("#141414")));
  const QColor textDisabled = validColor(map.colorTextDisabled, QColor(QStringLiteral("#bfbfbf")));
  const QColor border = validColor(map.colorBorder, QColor(QStringLiteral("#d9d9d9")));
  const QColor container = validColor(map.colorBgContainer, QColor(QStringLiteral("#ffffff")));
  const QColor containerDisabled =
      validColor(map.colorBgContainerDisabled, QColor(QStringLiteral("#f5f5f5")));
  const QColor primary = validColor(map.colorPrimary, QColor(QStringLiteral("#1677ff")));
  const QColor primaryHover = validColor(map.colorPrimaryHover, QColor(QStringLiteral("#4096ff")));
  const QColor white = validColor(map.colorWhite, QColor(QStringLiteral("#ffffff")));

  CheckboxVisualStyle style;
  style.metrics.checkboxSize = std::max(12, qRound(map.fontSizeLG));
  style.metrics.borderWidth = std::max(1, qRound(map.lineWidth));
  style.metrics.borderRadius = std::max(0, qRound(map.borderRadiusSM));
  style.metrics.markWidth = std::max(1, qRound(map.lineWidthBold));
  style.metrics.labelPaddingInlineStart = std::max(4, qRound(map.sizeXS));
  style.metrics.labelPaddingInlineEnd = std::max(4, qRound(map.sizeXS));
  style.metrics.wrapperMarginInlineEnd = std::max(0, qRound(map.sizeXS));
  style.metrics.font = input.baseFont;
  style.metrics.font.setPixelSize(std::max(12, qRound(map.fontSize)));
  style.metrics.textLineHeight =
      std::max(style.metrics.checkboxSize, qRound(style.metrics.font.pixelSize() * map.lineHeight));
  style.metrics.focusOutlineColor =
      validColor(map.colorPrimaryBorder, QColor(QStringLiteral("#91caff")));
  style.metrics.focusOutlineWidth = std::max<qreal>(1.0, map.lineWidth * 3.0);
  style.metrics.focusOutlineOffset = 1.0;

  style.normal = {border, container, white, text};
  style.hover = {primary, container, white, text};
  style.checked = {primary, primary, white, text};
  style.checkedHover = {primaryHover, primaryHover, white, text};
  style.indeterminate = {border, container, primary, text};
  style.indeterminateHover = {primary, container, primary, text};
  style.disabled = {border, containerDisabled, textDisabled, textDisabled};
  style.checkedDisabled = {border, containerDisabled, textDisabled, textDisabled};
  style.indeterminateDisabled = style.checkedDisabled;

  applyMetrics(input.componentTokens.metrics, &style.metrics);
  applyOptional(&style.metrics.focusOutlineColor, input.componentTokens.colors.focusRingColor);
  applyColors(input.componentTokens.colors, &style.normal);
  applyColors(input.componentTokens.colors, &style.hover);
  applyColors(input.componentTokens.colors, &style.checked);
  applyColors(input.componentTokens.colors, &style.checkedHover);
  applyColors(input.componentTokens.colors, &style.indeterminate);
  applyColors(input.componentTokens.colors, &style.indeterminateHover);
  applyColors(input.componentTokens.colors, &style.disabled);
  applyColors(input.componentTokens.colors, &style.checkedDisabled);
  applyColors(input.componentTokens.colors, &style.indeterminateDisabled);
  style.metrics.waveColor =
      input.componentTokens.colors.waveColor.value_or(style.checked.borderColor);
  if (map.backgroundOpacity != 1.0) {
    for (CheckboxStateStyle* state :
         {&style.normal, &style.hover, &style.checked, &style.checkedHover, &style.indeterminate,
          &style.indeterminateHover, &style.disabled, &style.checkedDisabled,
          &style.indeterminateDisabled}) {
      state->backgroundColor =
          adqt::theme::applyBackgroundOpacity(state->backgroundColor, map.backgroundOpacity);
    }
  }
  return style;
}

CheckboxVisualStyle resolveCheckboxVisualStyle(const CheckboxStyleInput& input) {
  return resolveCheckboxVisualStyle(input,
                                    adqt::theme::makeResolvedTheme(adqt::theme::makeTheme()));
}

}  // namespace adqt::widgets::detail
