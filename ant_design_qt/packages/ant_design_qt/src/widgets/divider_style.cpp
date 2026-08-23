#include "divider_style.h"

#include "theme/theme.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QColor validOr(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor paletteColor(const QPalette& palette, QPalette::ColorRole role, QPalette::ColorGroup group,
                    const QColor& fallback) {
  return validOr(palette.color(group, role), fallback);
}

template <typename T>
void applyOptional(T* target, const std::optional<T>& value) {
  if (target && value.has_value()) {
    *target = value.value();
  }
}

int scaledUnit(qreal unit, qreal multiplier) {
  return std::max(0, qRound(std::max<qreal>(1.0, unit) * multiplier));
}

}  // namespace

DividerAppearance resolveDividerAppearance(const DividerStyleInput& input,
                                           const adqt::theme::ResolvedTheme& resolvedTheme) {
  const adqt::theme::ThemeMapToken& map = resolvedTheme.values;

  DividerAppearance appearance;
  appearance.rootBackground = Qt::transparent;
  appearance.contentBackground = Qt::transparent;
  appearance.lineColor = validOr(map.colorBorderSecondary, QColor(QStringLiteral("#f0f0f0")));
  appearance.textColor = validOr(map.colorText, QColor(QStringLiteral("#141414")));

  if (!input.enabled) {
    appearance.lineColor = validOr(map.colorBorderDisabled, appearance.lineColor);
    appearance.textColor = validOr(map.colorTextDisabled, QColor(QStringLiteral("#bfbfbf")));
  }

  if (input.hasPaletteOverride) {
    const QPalette::ColorGroup group = input.enabled ? input.paletteGroup : QPalette::Disabled;
    appearance.rootBackground =
        paletteColor(input.palette, QPalette::Window, group, appearance.rootBackground);
    appearance.lineColor = paletteColor(input.palette, QPalette::Mid, group, appearance.lineColor);
    appearance.textColor =
        paletteColor(input.palette, QPalette::WindowText, group, appearance.textColor);
  }

  DividerMetrics& metrics = appearance.metrics;
  metrics.lineWidth = std::max<qreal>(0.0, map.lineWidth);
  metrics.orientationMargin = 0.05;
  metrics.verticalMarginInline = scaledUnit(map.sizeUnit, 2.0);
  metrics.horizontalMarginSmall = scaledUnit(map.sizeUnit, 2.0);
  metrics.horizontalMarginMiddle = scaledUnit(map.sizeUnit, 4.0);
  metrics.horizontalMarginLarge = scaledUnit(map.sizeUnit, 6.0);
  metrics.horizontalMarginWithText = scaledUnit(map.sizeUnit, 4.0);
  metrics.verticalHeightFactor = 0.9;

  metrics.contentFont = input.baseFont;
  metrics.contentFont.setPixelSize(
      std::max(1, qRound(input.plain ? map.fontSize : map.fontSizeLG)));
  metrics.contentFont.setWeight(input.plain ? QFont::Normal : QFont::Medium);
  const qreal lineHeightFactor = input.plain ? map.lineHeight : map.lineHeightLG;
  metrics.textLineHeight =
      std::max(1, qRound(metrics.contentFont.pixelSize() * std::max<qreal>(1.0, lineHeightFactor)));
  metrics.textPaddingInline = std::max(0, metrics.contentFont.pixelSize());
  metrics.verticalHeight =
      std::max(1, qRound(std::max<qreal>(1.0, map.fontSize) * metrics.verticalHeightFactor));

  const AdDivider::ColorTokens& colors = input.componentTokens.colors;
  if (colors.splitColor.has_value()) {
    appearance.lineColor = colors.splitColor.value();
  }
  if (input.plain && colors.textColor.has_value()) {
    appearance.textColor = colors.textColor.value();
  }
  if (!input.plain && colors.headingTextColor.has_value()) {
    appearance.textColor = colors.headingTextColor.value();
  }

  const AdDivider::MetricTokens& componentMetrics = input.componentTokens.metrics;
  applyOptional(&metrics.lineWidth, componentMetrics.lineWidth);
  applyOptional(&metrics.textPaddingInline, componentMetrics.textPaddingInline);
  applyOptional(&metrics.orientationMargin, componentMetrics.orientationMargin);
  applyOptional(&metrics.verticalMarginInline, componentMetrics.verticalMarginInline);
  applyOptional(&metrics.horizontalMarginSmall, componentMetrics.horizontalMarginSmall);
  applyOptional(&metrics.horizontalMarginMiddle, componentMetrics.horizontalMarginMiddle);
  applyOptional(&metrics.horizontalMarginLarge, componentMetrics.horizontalMarginLarge);
  applyOptional(&metrics.horizontalMarginWithText, componentMetrics.horizontalMarginWithText);
  applyOptional(&metrics.verticalHeightFactor, componentMetrics.verticalHeightFactor);

  const AdDivider::SemanticStyles& semantics = input.semanticStyles;
  applyOptional(&appearance.rootBackground, semantics.root.backgroundColor);
  applyOptional(&appearance.lineColor, semantics.root.borderColor);
  applyOptional(&metrics.lineWidth, semantics.root.borderWidth);
  applyOptional(&appearance.textColor, semantics.root.textColor);
  applyOptional(&metrics.contentFont, semantics.root.font);

  applyOptional(&appearance.lineColor, semantics.rail.borderColor);
  applyOptional(&metrics.lineWidth, semantics.rail.borderWidth);
  applyOptional(&appearance.contentBackground, semantics.content.backgroundColor);
  applyOptional(&appearance.textColor, semantics.content.textColor);
  applyOptional(&metrics.contentFont, semantics.content.font);
  applyOptional(&metrics.contentMarginStart, semantics.content.marginStart);
  applyOptional(&metrics.contentMarginEnd, semantics.content.marginEnd);

  metrics.lineWidth = std::max<qreal>(0.0, metrics.lineWidth);
  metrics.textPaddingInline = std::max(0, metrics.textPaddingInline);
  metrics.orientationMargin = qIsFinite(metrics.orientationMargin)
                                  ? std::clamp<qreal>(metrics.orientationMargin, 0.0, 1.0)
                                  : 0.05;
  metrics.verticalMarginInline = std::max(0, metrics.verticalMarginInline);
  metrics.horizontalMarginSmall = std::max(0, metrics.horizontalMarginSmall);
  metrics.horizontalMarginMiddle = std::max(0, metrics.horizontalMarginMiddle);
  metrics.horizontalMarginLarge = std::max(0, metrics.horizontalMarginLarge);
  metrics.horizontalMarginWithText = std::max(0, metrics.horizontalMarginWithText);
  metrics.verticalHeightFactor = qIsFinite(metrics.verticalHeightFactor)
                                     ? std::max<qreal>(0.0, metrics.verticalHeightFactor)
                                     : 0.9;
  metrics.contentMarginStart = std::max(0, metrics.contentMarginStart);
  metrics.contentMarginEnd = std::max(0, metrics.contentMarginEnd);

  const int fontPixelSize = metrics.contentFont.pixelSize() > 0 ? metrics.contentFont.pixelSize()
                                                                : std::max(1, qRound(map.fontSize));
  metrics.textLineHeight =
      std::max(QFontMetrics(metrics.contentFont).height(),
               qRound(fontPixelSize *
                      std::max<qreal>(1.0, input.plain ? map.lineHeight : map.lineHeightLG)));
  metrics.verticalHeight =
      std::max(1, qRound(std::max<qreal>(1.0, map.fontSize) * metrics.verticalHeightFactor));
  return appearance;
}

DividerAppearance resolveDividerAppearance(const DividerStyleInput& input) {
  return resolveDividerAppearance(input,
                                  adqt::theme::ThemeManager::instance().globalResolvedTheme());
}

}  // namespace adqt::widgets::detail
