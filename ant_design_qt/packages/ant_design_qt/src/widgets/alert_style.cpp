#include "alert_style.h"

#include "theme/theme.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor paletteColor(const QPalette& palette, QPalette::ColorRole role, QPalette::ColorGroup group,
                    const QColor& fallback) {
  const QColor color = palette.color(group, role);
  return color.isValid() ? color : fallback;
}

int parseDurationMs(int value) { return std::max(0, value); }

QColor fallbackHoverBackground(const QColor& base) {
  QColor color = base;
  if (!color.isValid()) {
    return QColor(0, 0, 0, 10);
  }
  color.setAlpha(std::max(20, color.alpha()));
  return color;
}

}  // namespace

AlertVisualStyle resolveAlertVisualStyle(const AlertStyleInput& input,
                                         const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::AdThemePalette& colors = resolved.theme.palette;
  const adqt::theme::AdThemeMetrics& metrics = resolved.theme.metrics;
  const adqt::theme::AdThemeMotion& motion = resolved.theme.motion;

  AlertVisualStyle style;
  style.background = toColor(colors.colorInfoBg, QColor("#e6f4ff"));
  style.border = toColor(colors.colorInfoBorder, QColor("#91caff"));
  style.iconColor = toColor(colors.colorInfo, QColor("#1677ff"));
  style.textColor = toColor(colors.colorText, QColor("#141414"));
  style.informativeTextColor = toColor(colors.colorText, QColor("#141414"));
  style.closeIconColor = toColor(colors.colorTextTertiary, QColor("#8c8c8c"));
  style.closeIconHoverColor = toColor(colors.colorText, QColor("#141414"));
  style.closeButtonHoverBackground = toColor(colors.colorFillQuaternary, QColor(0, 0, 0, 10));
  style.closeButtonPressedBackground =
      toColor(colors.colorFillSecondary, style.closeButtonHoverBackground);
  style.closeButtonFocusOutlineColor = toColor(colors.colorPrimaryBorder, QColor("#91caff"));

  switch (input.severity) {
    case AdAlert::Severity::Success:
      style.background = toColor(colors.colorSuccessBg, style.background);
      style.border = toColor(colors.colorSuccessBorder, style.border);
      style.iconColor = toColor(colors.colorSuccess, style.iconColor);
      break;
    case AdAlert::Severity::Info:
      style.background = toColor(colors.colorInfoBg, style.background);
      style.border = toColor(colors.colorInfoBorder, style.border);
      style.iconColor = toColor(colors.colorInfo, style.iconColor);
      break;
    case AdAlert::Severity::Warning:
      style.background = toColor(colors.colorWarningBg, style.background);
      style.border = toColor(colors.colorWarningBorder, style.border);
      style.iconColor = toColor(colors.colorWarning, style.iconColor);
      break;
    case AdAlert::Severity::Error:
      style.background = toColor(colors.colorErrorBg, style.background);
      style.border = toColor(colors.colorErrorBorder, style.border);
      style.iconColor = toColor(colors.colorError, style.iconColor);
      break;
  }

  style.metrics.borderWidth = std::max(0, qRound(metrics.lineWidth));
  style.metrics.borderRadius = std::max(0, qRound(metrics.borderRadiusLG));
  style.metrics.paddingInline = std::max(8, qRound(metrics.sizeSM));
  style.metrics.paddingBlock = std::max(4, qRound(metrics.sizeXS));
  style.metrics.paddingWithInformativeTextInline =
      std::max(style.metrics.paddingInline, qRound(metrics.sizeLG));
  style.metrics.paddingWithInformativeTextBlock = style.metrics.paddingWithInformativeTextInline;
  style.metrics.gapLeadingContent = std::max(4, qRound(metrics.sizeXS));
  style.metrics.gapLeadingContentWithInformativeText =
      std::max(style.metrics.gapLeadingContent, qRound(metrics.sizeSM));
  style.metrics.gapContentActions = std::max(4, qRound(metrics.sizeXS));
  style.metrics.gapActionsDismiss = std::max(4, qRound(metrics.sizeXS));
  style.metrics.textInformativeTextGap = std::max(0, qRound(metrics.sizeXS));
  style.metrics.iconSize = std::max(12, qRound(metrics.fontSize));
  style.metrics.iconSizeWithInformativeText = std::max(12, qRound(metrics.fontSizeHeading3));
  style.metrics.closeIconSize = std::max(10, qRound(metrics.fontSizeSM));
  style.metrics.closeButtonSize = std::max(24, qRound(metrics.controlHeightSM));
  style.metrics.closeButtonFocusOutlineWidth = std::max(1, qRound(metrics.lineWidthBold));
  style.metrics.closeAnimationMs = parseDurationMs(motion.motionDurationSlow);
  style.metrics.closeAnimationEasing = motion.motionEaseInOut;

  style.metrics.textFont = input.baseFont;
  style.metrics.textFont.setPixelSize(std::max(12, qRound(metrics.fontSize)));
  style.metrics.textFont.setWeight(QFont::Normal);

  style.metrics.textWithInformativeTextFont = input.baseFont;
  style.metrics.textWithInformativeTextFont.setPixelSize(std::max(12, qRound(metrics.fontSizeLG)));
  style.metrics.textWithInformativeTextFont.setWeight(QFont::Normal);

  style.metrics.informativeTextFont = input.baseFont;
  style.metrics.informativeTextFont.setPixelSize(std::max(12, qRound(metrics.fontSize)));
  style.metrics.informativeTextFont.setWeight(QFont::Normal);

  if (input.displayMode == AdAlert::DisplayMode::Banner) {
    style.metrics.borderRadius = 0;
    style.metrics.borderWidth = 0;
  }

  if (!motion.motion) {
    style.metrics.closeAnimationMs = 0;
  }

  if (!input.enabled) {
    const QColor disabledText = toColor(colors.colorTextDisabled, QColor("#bfbfbf"));
    const QColor disabledIcon = toColor(colors.colorTextQuaternary, disabledText);
    style.textColor = disabledText;
    style.informativeTextColor = disabledText;
    style.iconColor = disabledIcon;
    style.closeIconColor = disabledIcon;
    style.closeIconHoverColor = disabledIcon;
    style.closeButtonHoverBackground = QColor(Qt::transparent);
    style.closeButtonPressedBackground = QColor(Qt::transparent);
    style.closeButtonFocusOutlineColor = disabledIcon;
  }

  if (input.hasPaletteOverride) {
    const QPalette::ColorGroup group = input.enabled ? input.paletteGroup : QPalette::Disabled;
    style.background = paletteColor(input.palette, QPalette::Window, group, style.background);
    style.border = paletteColor(input.palette, QPalette::Mid, group, style.border);
    style.textColor = paletteColor(input.palette, QPalette::WindowText, group, style.textColor);
    style.informativeTextColor =
        paletteColor(input.palette, QPalette::Text, group, style.informativeTextColor);
    style.iconColor = paletteColor(input.palette, QPalette::WindowText, group, style.iconColor);
    style.closeIconColor =
        paletteColor(input.palette, QPalette::ButtonText, group, style.closeIconColor);
    style.closeIconHoverColor =
        paletteColor(input.palette, QPalette::Text, group, style.closeIconHoverColor);
    style.closeButtonHoverBackground = paletteColor(input.palette, QPalette::AlternateBase, group,
                                                    style.closeButtonHoverBackground);
    style.closeButtonPressedBackground =
        paletteColor(input.palette, QPalette::Button, group, style.closeButtonPressedBackground);
    style.closeButtonFocusOutlineColor =
        paletteColor(input.palette, QPalette::Highlight, group, style.closeButtonFocusOutlineColor);
    if (!style.closeButtonHoverBackground.isValid()) {
      style.closeButtonHoverBackground = fallbackHoverBackground(style.background);
    }
  }

  return style;
}

AlertVisualStyle resolveAlertVisualStyle(const AlertStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveAlertVisualStyle(input, resolved);
}

}  // namespace adqt::widgets::detail
