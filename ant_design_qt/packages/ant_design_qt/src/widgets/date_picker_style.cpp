#include "date_picker_style.h"

#include "theme/fast_color_lite.h"
#include "theme/theme.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor blend(const QColor& a, const QColor& b, qreal amount) {
  const float t = static_cast<float>(std::clamp(amount, qreal(0.0), qreal(1.0)));
  QColor out;
  out.setRedF(a.redF() * (1.0F - t) + b.redF() * t);
  out.setGreenF(a.greenF() * (1.0F - t) + b.greenF() * t);
  out.setBlueF(a.blueF() * (1.0F - t) + b.blueF() * t);
  out.setAlphaF(a.alphaF() * (1.0F - t) + b.alphaF() * t);
  return out;
}

QColor lighten(const QColor& value, qreal amountPercent, const QColor& fallback) {
  if (!value.isValid()) {
    return fallback;
  }
  const QColor rgb = value.toRgb();
  return QColor(adqt::theme::FastColorLite(rgb.red(), rgb.green(), rgb.blue(),
                                           static_cast<double>(rgb.alphaF()))
                    .lighten(amountPercent)
                    .toHexString());
}

QColor overrideColor(const std::optional<QColor>& value, const QColor& fallback) {
  return value.has_value() && value->isValid() ? *value : fallback;
}

int overrideMetric(const std::optional<int>& value, int fallback, int minimum) {
  return value.has_value() ? std::max(minimum, *value) : fallback;
}

void applySemanticSlot(const AdDatePickerPanel::SemanticSlotStyle& slot, QColor* textColor,
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

DatePickerVisualStyle resolveDatePickerVisualStyle(const DatePickerStyleInput& input,
                                                   const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const QColor panelBg = toColor(map.colorBgElevated, QColor("#ffffff"));
  const QColor primary = toColor(map.colorPrimary, QColor("#1677ff"));
  const QColor primaryBg = toColor(map.colorPrimaryBg, QColor("#e6f4ff"));
  const QColor text = toColor(map.colorText, QColor("#141414"));
  const QColor disabledText = toColor(map.colorTextDisabled, QColor("#bfbfbf"));

  DatePickerVisualStyle style;
  style.panelBackground = panelBg;
  style.panelBorderColor = toColor(map.colorBorderSecondary, QColor("#f0f0f0"));
  style.headerBackground = panelBg;
  style.bodyBackground = panelBg;
  style.contentBackground = panelBg;
  style.itemBackground = QColor(0, 0, 0, 0);
  style.itemBorderColor = QColor(0, 0, 0, 0);
  style.footerBackground = panelBg;
  style.headerTextColor = text;
  style.textColor = text;
  style.secondaryTextColor = toColor(map.colorTextTertiary, QColor("#8c8c8c"));
  style.disabledTextColor = disabledText;
  style.disabledCellBackground = toColor(map.colorBgContainerDisabled, QColor("#f5f5f5"));
  style.hoverBackground = toColor(map.colorFillTertiary, QColor("#f5f5f5"));
  style.rangeBackground = primaryBg;
  style.rangeHoverBackground = lighten(primary, 35.0, blend(primaryBg, primary, 0.08));
  style.rangeBorderColor = lighten(primary, 20.0, blend(primary, panelBg, 0.28));
  style.selectedBackground = primary;
  style.selectedTextColor = toColor(map.colorTextLightSolid, QColor("#ffffff"));
  style.todayBorderColor = primary;
  style.footerBorderColor = toColor(map.colorBorderSecondary, QColor("#f0f0f0"));
  style.linkColor = primary;

  style.metrics.font = input.baseFont;
  style.metrics.font.setPixelSize(std::max(12, qRound(map.fontSize)));
  style.metrics.smallFont = style.metrics.font;
  style.metrics.smallFont.setPixelSize(std::max(11, qRound(map.fontSizeSM)));
  style.metrics.headerFont = style.metrics.font;
  style.metrics.headerFont.setWeight(QFont::DemiBold);
  style.metrics.controlHeight = std::max(24, qRound(map.controlHeight));
  style.metrics.popupOffset = std::max(2, qRound(map.sizeXXS));
  style.metrics.popupPadding = 0;
  style.metrics.panelPaddingHorizontal = std::max(12, qRound(map.size + map.sizeXXS / 2.0));
  style.metrics.weekPanelPaddingHorizontal = std::max(0, qRound(map.sizeSM));
  style.metrics.panelPaddingVertical = std::max(6, qRound(map.sizeXS));
  style.metrics.headerHeight = std::max(34, qRound(map.controlHeightLG));
  style.metrics.footerHeight = std::max(32, qRound(map.controlHeightLG));
  style.metrics.cellHeight = std::max(22, qRound(map.controlHeightSM));
  style.metrics.cellPaddingVertical = std::max(0, qRound(map.sizeXXS + map.sizeXXS / 2.0));
  style.metrics.dateCellWidth =
      std::max(style.metrics.cellHeight, qRound(map.controlHeightSM * 1.5));
  style.metrics.yearMonthCellWidth =
      std::max(style.metrics.cellHeight, qRound(map.controlHeightLG * 1.5));
  style.metrics.textHeight = std::max(style.metrics.cellHeight, qRound(map.controlHeightLG));
  style.metrics.monthCellHeight =
      std::max(style.metrics.cellHeight, qRound(map.controlHeightLG * 1.65));
  style.metrics.quarterPanelContentHeight =
      std::max(style.metrics.cellHeight, qRound(map.controlHeightLG * 1.4));
  style.metrics.unitPanelPaddingHorizontal = std::max(0, qRound(map.sizeXS));
  style.metrics.panelWidth =
      style.metrics.dateCellWidth * 7 + style.metrics.panelPaddingHorizontal * 2;
  style.metrics.presetsWidth = 120;
  style.metrics.presetsMaxWidth = 200;
  style.metrics.zIndexPopup = std::max(0, qRound(map.popupZIndexBase + 50.0));
  style.metrics.timeColumnWidth = std::max(40, qRound(map.controlHeightLG * 1.4));
  style.metrics.timeColumnHeight = 28 * 8;
  style.metrics.timeColumnMarginVertical = std::max(0, qRound(map.sizeXXS));
  style.metrics.timePanelPaddingTop = std::max(0, qRound(map.sizeXXS));
  style.metrics.timeCellHeight = 28;
  style.metrics.borderRadius = std::max(0, qRound(map.borderRadiusLG));
  style.metrics.cellRadius = std::max(0, qRound(map.borderRadiusSM));
  style.metrics.borderWidth = std::max(1, qRound(map.lineWidth));
  style.metrics.focusOutlineWidth = std::max(1, qRound(map.lineWidth * 3.0));

  if (input.disabled) {
    style.textColor = disabledText;
    style.headerTextColor = disabledText;
    style.linkColor = disabledText;
  }

  const AdDatePickerPanel::ComponentTokens& tokens = input.componentTokens;
  const bool panelWidthOverridden = tokens.panelWidth.has_value();
  const bool cellWidthOverridden = tokens.cellWidth.has_value();
  style.metrics.panelWidth = overrideMetric(tokens.panelWidth, style.metrics.panelWidth, 180);
  style.metrics.presetsWidth = overrideMetric(tokens.presetsWidth, style.metrics.presetsWidth, 1);
  style.metrics.presetsMaxWidth = overrideMetric(
      tokens.presetsMaxWidth, style.metrics.presetsMaxWidth, style.metrics.presetsWidth);
  style.metrics.zIndexPopup = overrideMetric(tokens.zIndexPopup, style.metrics.zIndexPopup, 0);
  style.metrics.timeColumnWidth =
      overrideMetric(tokens.timeColumnWidth, style.metrics.timeColumnWidth, 24);
  style.metrics.timeColumnHeight =
      overrideMetric(tokens.timeColumnHeight, style.metrics.timeColumnHeight, 24);
  style.metrics.timeCellHeight =
      overrideMetric(tokens.timeCellHeight, style.metrics.timeCellHeight, 18);
  style.metrics.cellHeight = overrideMetric(tokens.cellHeight, style.metrics.cellHeight, 18);
  style.metrics.dateCellWidth =
      overrideMetric(tokens.cellWidth, style.metrics.dateCellWidth, style.metrics.cellHeight);
  if (!cellWidthOverridden) {
    style.metrics.yearMonthCellWidth =
        std::max(style.metrics.cellHeight, qRound(style.metrics.textHeight * 1.5));
  }
  if (!cellWidthOverridden && panelWidthOverridden) {
    style.metrics.dateCellWidth =
        std::max(style.metrics.cellHeight,
                 (style.metrics.panelWidth - style.metrics.panelPaddingHorizontal * 2) / 7);
  } else if (cellWidthOverridden && !panelWidthOverridden) {
    style.metrics.panelWidth =
        style.metrics.dateCellWidth * 7 + style.metrics.panelPaddingHorizontal * 2;
  }
  style.metrics.textHeight = overrideMetric(tokens.textHeight, style.metrics.textHeight, 18);
  style.metrics.headerHeight = std::max(style.metrics.headerHeight, style.metrics.textHeight);
  style.metrics.footerHeight = std::max(style.metrics.footerHeight, style.metrics.textHeight);
  style.metrics.footerLineHeight =
      std::max(0, style.metrics.textHeight - style.metrics.borderWidth * 2);
  style.metrics.monthCellHeight =
      overrideMetric(tokens.withoutTimeCellHeight, style.metrics.monthCellHeight, 24);
  style.metrics.borderRadius = overrideMetric(tokens.borderRadius, style.metrics.borderRadius, 0);
  style.panelBackground = overrideColor(tokens.panelBackground, style.panelBackground);
  style.panelBorderColor = overrideColor(tokens.panelBorderColor, style.panelBorderColor);
  style.hoverBackground = overrideColor(tokens.cellHoverBackground, style.hoverBackground);
  style.selectedBackground = overrideColor(tokens.cellSelectedBackground, style.selectedBackground);
  style.rangeBackground = overrideColor(tokens.cellRangeBackground, style.rangeBackground);
  style.rangeHoverBackground =
      overrideColor(tokens.cellRangeHoverBackground, style.rangeHoverBackground);
  style.rangeBorderColor = overrideColor(tokens.cellRangeBorderColor, style.rangeBorderColor);
  style.textColor = overrideColor(tokens.textColor, style.textColor);
  style.disabledTextColor = overrideColor(tokens.textDisabledColor, style.disabledTextColor);

  style.headerBackground = style.panelBackground;
  style.bodyBackground = style.panelBackground;
  style.contentBackground = style.panelBackground;
  style.footerBackground = style.panelBackground;

  const AdDatePickerPanel::SemanticStyles& semantic = input.semanticStyles;
  applySemanticSlot(semantic.root, &style.textColor, &style.panelBackground,
                    &style.panelBorderColor);
  applySemanticSlot(semantic.container, &style.textColor, &style.panelBackground,
                    &style.panelBorderColor);
  style.headerBackground = style.panelBackground;
  style.bodyBackground = style.panelBackground;
  style.contentBackground = style.panelBackground;
  style.footerBackground = style.panelBackground;
  applySemanticSlot(semantic.header, &style.headerTextColor, &style.headerBackground, nullptr);
  applySemanticSlot(semantic.body, &style.textColor, &style.bodyBackground, nullptr);
  style.contentBackground = style.bodyBackground;
  applySemanticSlot(semantic.content, &style.textColor, &style.contentBackground, nullptr);
  applySemanticSlot(semantic.item, &style.textColor, &style.itemBackground, &style.itemBorderColor);
  applySemanticSlot(semantic.footer, &style.linkColor, &style.footerBackground,
                    &style.footerBorderColor);

  return style;
}

DatePickerVisualStyle resolveDatePickerVisualStyle(const DatePickerStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveDatePickerVisualStyle(input, resolved);
}

}  // namespace adqt::widgets::detail
