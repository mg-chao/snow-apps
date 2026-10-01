#include "descriptions_style.h"

#include "theme/theme.h"
#include "theme/theme_color_utils.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QColor resolved(const std::optional<QColor>& value, const QColor& fallback) {
  return value && value->isValid() ? *value : fallback;
}

int positiveMetric(const std::optional<int>& value, int fallback) {
  return value ? std::max(0, *value) : std::max(0, fallback);
}

}  // namespace

DescriptionsAppearance resolveDescriptionsAppearance(
    const AdDescriptions* descriptions, const AdDescriptions::ComponentTokens& tokens,
    const AdDescriptions::SemanticStyles& semantics) {
  const adqt::theme::ResolvedTheme theme =
      adqt::theme::ThemeManager::instance().resolve(descriptions);
  const auto& colors = theme.theme.palette;
  const auto& metrics = theme.theme.metrics;

  DescriptionsAppearance result;
  result.backgroundOpacity = theme.values.backgroundOpacity;
  result.rootBackground = descriptions->palette().color(QPalette::Window);
  result.labelBackground =
      colors.colorFillAlter.isValid() ? colors.colorFillAlter : QColor("#fafafa");
  result.labelColor =
      colors.colorTextTertiary.isValid() ? colors.colorTextTertiary : QColor("#8c8c8c");
  result.titleColor = colors.colorText.isValid() ? colors.colorText : QColor("#141414");
  result.contentColor = result.titleColor;
  result.extraColor = result.titleColor;
  result.borderColor =
      colors.colorBorderSecondary.isValid() ? colors.colorBorderSecondary : QColor("#f0f0f0");

  result.metrics.titleMarginBottom = std::max(0, qRound(metrics.fontSizeSM * metrics.lineHeightSM));
  result.metrics.itemPaddingBottom = std::max(0, qRound(metrics.size));
  result.metrics.itemPaddingEnd = std::max(0, qRound(metrics.size));
  result.metrics.colonMarginLeft = std::max(0, qRound(metrics.sizeXXS / 2.0));
  result.metrics.colonMarginRight = std::max(0, qRound(metrics.sizeXS));
  result.metrics.borderedPaddingBlock = std::max(0, qRound(metrics.size));
  result.metrics.borderedPaddingInline = std::max(0, qRound(metrics.sizeLG));
  result.metrics.borderWidth = std::max(0, qRound(metrics.lineWidth));
  result.metrics.borderRadius = std::max(0, qRound(metrics.borderRadiusLG));
  result.metrics.lineHeight = std::max(1, qRound(metrics.fontSize * metrics.lineHeight));
  result.metrics.textFont = theme.theme.appFont;
  if (result.metrics.textFont.family().isEmpty()) {
    result.metrics.textFont = descriptions->font();
  }
  result.metrics.textFont.setPixelSize(std::max(1, qRound(metrics.fontSize)));
  result.metrics.titleFont = result.metrics.textFont;
  result.metrics.titleFont.setPixelSize(std::max(1, qRound(metrics.fontSizeLG)));
  result.metrics.titleFont.setWeight(QFont::DemiBold);

  if (descriptions->descriptionSize() == AdDescriptions::Size::Middle) {
    result.metrics.itemPaddingBottom = std::max(0, qRound(metrics.sizeSM));
    result.metrics.borderedPaddingBlock = std::max(0, qRound(metrics.sizeSM));
  } else if (descriptions->descriptionSize() == AdDescriptions::Size::Small) {
    result.metrics.itemPaddingBottom = std::max(0, qRound(metrics.sizeXS));
    result.metrics.borderedPaddingBlock = std::max(0, qRound(metrics.sizeXS));
    result.metrics.borderedPaddingInline = std::max(0, qRound(metrics.size));
  }

  result.labelBackground = resolved(tokens.colors.labelBackground, result.labelBackground);
  result.labelColor = resolved(tokens.colors.labelColor, result.labelColor);
  result.titleColor = resolved(tokens.colors.titleColor, result.titleColor);
  result.contentColor = resolved(tokens.colors.contentColor, result.contentColor);
  result.extraColor = resolved(tokens.colors.extraColor, result.extraColor);
  result.borderColor = resolved(tokens.colors.borderColor, result.borderColor);
  result.metrics.titleMarginBottom =
      positiveMetric(tokens.metrics.titleMarginBottom, result.metrics.titleMarginBottom);
  result.metrics.itemPaddingBottom =
      positiveMetric(tokens.metrics.itemPaddingBottom, result.metrics.itemPaddingBottom);
  result.metrics.itemPaddingEnd =
      positiveMetric(tokens.metrics.itemPaddingEnd, result.metrics.itemPaddingEnd);
  result.metrics.colonMarginLeft =
      positiveMetric(tokens.metrics.colonMarginLeft, result.metrics.colonMarginLeft);
  result.metrics.colonMarginRight =
      positiveMetric(tokens.metrics.colonMarginRight, result.metrics.colonMarginRight);
  result.metrics.borderedPaddingBlock =
      positiveMetric(tokens.metrics.borderedPaddingBlock, result.metrics.borderedPaddingBlock);
  result.metrics.borderedPaddingInline =
      positiveMetric(tokens.metrics.borderedPaddingInline, result.metrics.borderedPaddingInline);
  result.metrics.borderWidth =
      positiveMetric(tokens.metrics.borderWidth, result.metrics.borderWidth);
  result.metrics.borderRadius =
      positiveMetric(tokens.metrics.borderRadius, result.metrics.borderRadius);

  result.rootBackground = resolved(semantics.root.backgroundColor, result.rootBackground);
  result.borderColor = resolved(semantics.root.borderColor, result.borderColor);
  result.titleColor = resolved(semantics.title.textColor, result.titleColor);
  result.extraColor = resolved(semantics.extra.textColor, result.extraColor);
  result.labelColor = resolved(semantics.label.textColor, result.labelColor);
  result.labelBackground = resolved(semantics.label.backgroundColor, result.labelBackground);
  result.contentColor = resolved(semantics.content.textColor, result.contentColor);
  if (result.backgroundOpacity != 1.0) {
    result.rootBackground =
        adqt::theme::applyBackgroundOpacity(result.rootBackground, result.backgroundOpacity);
    result.labelBackground =
        adqt::theme::applyBackgroundOpacity(result.labelBackground, result.backgroundOpacity);
  }
  return result;
}

}  // namespace adqt::widgets::detail
