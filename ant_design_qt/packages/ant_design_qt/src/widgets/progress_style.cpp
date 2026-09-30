#include "progress_style.h"

#include <algorithm>
#include <cmath>
#include "theme/theme_types.h"

namespace adqt::widgets::detail {

ProgressVisualStyle resolveProgressVisualStyle(const AdProgress* widget,
                                               const adqt::theme::ResolvedTheme& theme) {
  const auto tokens = widget->componentTokens();
  const auto semantic = widget->semanticStyles();
  const bool small = widget->sizeClass() == AdProgress::Size::Small;
  ProgressVisualStyle style;
  style.fill = tokens.colors.defaultColor.value_or(theme.values.colorInfo);
  style.normalFill = style.fill;
  style.rail = tokens.colors.remainingColor.value_or(theme.values.colorFillSecondary);
  style.success = tokens.colors.successColor.value_or(theme.values.colorSuccess);
  style.exception = tokens.colors.exceptionColor.value_or(theme.values.colorError);
  style.text = theme.values.colorText;
  style.circleText = tokens.colors.circleTextColor.value_or(style.text);
  style.innerText = theme.values.colorWhite;
  style.shimmer = theme.values.colorBgContainer;
  style.font = widget->font();
  if (!widget->testAttribute(Qt::WA_SetFont)) {
    style.font.setPixelSize(
        std::max(1, qRound(small ? theme.values.fontSizeSM : theme.values.fontSize)));
  }
  const qreal diameter =
      widget->progressSize().width() > 0 ? widget->progressSize().width() : (small ? 60.0 : 120.0);
  const auto positive = [](qreal value, qreal fallback) {
    return std::isfinite(value) && value > 0 ? value : fallback;
  };
  style.circleTextSize =
      positive(tokens.metrics.circleTextFontSize.value_or(diameter * 0.15 + 6), 24);
  style.circleIconSize =
      positive(tokens.metrics.circleIconFontSize.value_or(style.circleTextSize * 14 / 12), 28);
  style.lineHeight =
      widget->progressSize().height() > 0
          ? widget->progressSize().height()
          : (widget->strokeWidth() > 0 ? widget->strokeWidth() : (small ? 6.0 : 8.0));
  style.lineRadius = positive(tokens.metrics.lineBorderRadius.value_or(100), 100);
  if (tokens.metrics.lineBorderRadius == 0) style.lineRadius = 0;
  style.animationDurationMs =
      std::clamp(tokens.metrics.animationDurationMs.value_or(300), 0, 10000);
  style.motion = theme.config.motion && widget->animationEnabled();
  if (widget->effectiveStatus() == AdProgress::Status::Success) style.fill = style.success;
  if (widget->effectiveStatus() == AdProgress::Status::Exception) style.fill = style.exception;
  if (widget->strokeColor().isValid()) style.normalFill = style.fill = widget->strokeColor();
  if (widget->railColor().isValid()) style.rail = widget->railColor();
  if (widget->successColor().isValid()) style.success = widget->successColor();
  if (semantic.root.backgroundColor) style.rootBackground = *semantic.root.backgroundColor;
  if (semantic.root.textColor) style.text = style.circleText = *semantic.root.textColor;
  if (semantic.root.font) style.font = *semantic.root.font;
  if (semantic.body.backgroundColor) style.bodyBackground = *semantic.body.backgroundColor;
  if (semantic.rail.backgroundColor) style.rail = *semantic.rail.backgroundColor;
  if (semantic.track.backgroundColor)
    style.normalFill = style.fill = *semantic.track.backgroundColor;
  if (semantic.indicator.backgroundColor)
    style.indicatorBackground = *semantic.indicator.backgroundColor;
  if (semantic.indicator.textColor)
    style.text = style.circleText = style.innerText = *semantic.indicator.textColor;
  if (semantic.indicator.font) {
    style.font = *semantic.indicator.font;
    style.circleTextSize = style.font.pixelSize() > 0
                               ? style.font.pixelSize()
                               : style.font.pointSizeF() * widget->logicalDpiY() / 72;
  }
  const QColor firstColor =
      widget->strokeGradient().isEmpty() ? style.fill : widget->strokeGradient().first().second;
  if (!semantic.indicator.textColor && (firstColor.redF() * 0.299 + firstColor.greenF() * 0.587 +
                                        firstColor.blueF() * 0.114) > 0.6) {
    style.innerText = QColor(0, 0, 0, 115);
  }
  return style;
}

}  // namespace adqt::widgets::detail
