#include "input_style.h"

#include "theme/theme.h"

#include <algorithm>
#include <cmath>

namespace adqt::widgets::detail {

namespace {

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
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

QColor compositeOn(const QColor& foreground, const QColor& background) {
  if (!foreground.isValid()) {
    return background;
  }
  if (!background.isValid()) {
    QColor opaque = foreground;
    opaque.setAlpha(255);
    return opaque;
  }

  const float alpha = std::clamp(foreground.alphaF(), 0.0F, 1.0F);
  if (alpha >= 0.999F) {
    QColor opaque = foreground;
    opaque.setAlpha(255);
    return opaque;
  }

  QColor mixed;
  mixed.setRedF(foreground.redF() * alpha + background.redF() * (1.0F - alpha));
  mixed.setGreenF(foreground.greenF() * alpha + background.greenF() * (1.0F - alpha));
  mixed.setBlueF(foreground.blueF() * alpha + background.blueF() * (1.0F - alpha));
  mixed.setAlpha(255);
  return mixed;
}

double roundToSingleDecimal(double value) { return std::round(value * 10.0) / 10.0; }

double ceilToSingleDecimal(double value) { return std::ceil(value * 10.0) / 10.0; }

int resolvePaddingBlock(double controlHeight, double fontSize, double lineHeight, double lineWidth,
                        bool useCeil) {
  const double base = (controlHeight - fontSize * lineHeight) / 2.0;
  const double rounded = useCeil ? ceilToSingleDecimal(base) : roundToSingleDecimal(base);
  return std::max(0, qRound(rounded - lineWidth));
}

}  // namespace

InputVisualStyle resolveInputVisualStyle(const InputStyleInput& input,
                                         const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const QColor transparent(0, 0, 0, 0);
  const QColor containerBg = toColor(map.colorBgContainer, QColor("#ffffff"));
  const QColor controlOutline =
      deriveAlphaColor(toColor(map.colorPrimaryBg, QColor("#e6f4ff")), containerBg);
  const QColor errorOutline =
      deriveAlphaColor(toColor(map.colorErrorBg, QColor("#fff2f0")), containerBg);
  const QColor warningOutline =
      deriveAlphaColor(toColor(map.colorWarningBg, QColor("#fffbe6")), containerBg);

  InputVisualStyle style;
  style.selectorBg = containerBg;
  style.selectorHoverBg = style.selectorBg;
  style.selectorActiveBg = style.selectorBg;
  style.selectorBorderColor = toColor(map.colorBorder, QColor("#d9d9d9"));
  style.selectorHoverBorderColor = toColor(map.colorPrimaryHover, QColor("#4096ff"));
  style.selectorActiveBorderColor = toColor(map.colorPrimary, QColor("#1677ff"));
  style.selectorFocusOutlineColor = controlOutline;
  style.selectorTextColor = toColor(map.colorText, QColor("#141414"));
  style.placeholderColor = toColor(map.colorTextPlaceholder, QColor("#bfbfbf"));
  style.clearColor = toColor(map.colorTextQuaternary, QColor("#bfbfbf"));
  style.clearHoverColor = toColor(map.colorTextTertiary, QColor("#8c8c8c"));
  style.clearActiveColor = toColor(map.colorText, QColor("#141414"));
  style.prefixColor = style.selectorTextColor;
  style.suffixColor = style.selectorTextColor;
  style.suffixActionColor = toColor(map.colorTextTertiary, QColor("#8c8c8c"));
  style.suffixActionHoverColor = toColor(map.colorText, QColor("#141414"));
  style.countColor = toColor(map.colorTextTertiary, QColor("#8c8c8c"));
  style.disabledTextColor = toColor(map.colorTextDisabled, QColor("#bfbfbf"));
  style.disabledBg = toColor(map.colorBgContainerDisabled, QColor("#f5f5f5"));
  style.disabledBorderColor = toColor(map.colorBorderDisabled, QColor("#d9d9d9"));

  style.metrics.font = input.baseFont;
  style.metrics.font.setPixelSize(std::max(12, qRound(map.fontSize)));
  style.metrics.height = std::max(24, qRound(map.controlHeight));
  style.metrics.borderRadius = std::max(0, qRound(map.borderRadius));
  style.metrics.borderWidth = std::max(1, qRound(map.lineWidth));
  style.metrics.horizontalPadding = std::max(8, qRound(map.sizeSM - map.lineWidth));
  style.metrics.verticalPadding =
      resolvePaddingBlock(map.controlHeight, map.fontSize, map.lineHeight, map.lineWidth, false);
  style.metrics.textLineHeight = std::max(1, qRound(map.fontHeight));
  style.metrics.affixPadding = std::max(2, qRound(map.sizeXXS));
  style.metrics.affixItemGap = std::max(style.metrics.affixPadding, qRound(map.sizeXS));
  // Affix icons follow the resolved input font size so they scale with large inputs
  // and any explicit font override, while clear icons keep the shared icon token size.
  style.metrics.affixIconSize = std::max(10, style.metrics.font.pixelSize());
  style.metrics.clearIconSize = std::max(10, qRound(map.fontSizeSM));
  style.metrics.multilineAffixTopInset =
      std::max(style.metrics.verticalPadding, qRound(map.sizeXS));
  style.metrics.countTopMargin = std::max(2, qRound(map.sizeXXS));
  style.metrics.countHeight = std::max(16, style.metrics.textLineHeight);
  style.metrics.focusOutlineWidth = std::max<qreal>(1.0, map.lineWidth * 3.0);
  style.metrics.focusOutlineOffset = 0.0;

  if (input.controlSize == AdLineEdit::ControlSize::Large) {
    style.metrics.height = std::max(28, qRound(map.controlHeightLG));
    style.metrics.borderRadius = std::max(0, qRound(map.borderRadiusLG));
    style.metrics.font.setPixelSize(std::max(12, qRound(map.fontSizeLG)));
    style.metrics.affixIconSize = std::max(10, style.metrics.font.pixelSize());
    style.metrics.verticalPadding = resolvePaddingBlock(map.controlHeightLG, map.fontSizeLG,
                                                        map.lineHeightLG, map.lineWidth, true);
    style.metrics.textLineHeight = std::max(1, qRound(map.fontHeightLG));
    style.metrics.countHeight = std::max(16, style.metrics.textLineHeight);
  } else if (input.controlSize == AdLineEdit::ControlSize::Small) {
    style.metrics.height = std::max(22, qRound(map.controlHeightSM));
    style.metrics.borderRadius = std::max(0, qRound(map.borderRadiusSM));
    style.metrics.horizontalPadding = std::max(6, qRound(map.sizeXS - map.lineWidth));
    style.metrics.verticalPadding = resolvePaddingBlock(map.controlHeightSM, map.fontSize,
                                                        map.lineHeight, map.lineWidth, false);
    style.metrics.textLineHeight = std::max(1, qRound(map.fontHeightSM));
    style.metrics.countHeight = std::max(16, style.metrics.textLineHeight);
  }

  style.metrics.multilineAffixTopInset =
      std::max(style.metrics.verticalPadding, qRound(map.sizeXS));

  if (input.multiline) {
    style.metrics.height = std::max(style.metrics.height, qRound(map.controlHeightLG));
  }

  if (input.variant == AdLineEdit::Variant::Filled) {
    style.selectorBg = toColor(map.colorFillTertiary, QColor("#f5f5f5"));
    style.selectorHoverBg = toColor(map.colorFillSecondary, QColor("#f0f0f0"));
    style.selectorActiveBg = toColor(map.colorBgContainer, QColor("#ffffff"));
    style.selectorBorderColor = transparent;
    style.selectorHoverBorderColor = transparent;
    style.selectorFocusOutlineColor = transparent;
  } else if (input.variant == AdLineEdit::Variant::Borderless) {
    style.selectorBg = transparent;
    style.selectorHoverBg = transparent;
    style.selectorActiveBg = transparent;
    style.selectorBorderColor = transparent;
    style.selectorHoverBorderColor = transparent;
    style.selectorActiveBorderColor = transparent;
    style.selectorFocusOutlineColor = transparent;
  } else if (input.variant == AdLineEdit::Variant::Underlined) {
    style.underlined = true;
    style.metrics.borderRadius = 0;
    style.selectorFocusOutlineColor = transparent;
  }

  if (input.status == AdLineEdit::Status::Error) {
    const QColor statusColor = toColor(map.colorError, QColor("#ff4d4f"));
    style.countColor = statusColor;
    if (input.variant == AdLineEdit::Variant::Filled) {
      style.selectorBg = toColor(map.colorErrorBg, QColor("#fff2f0"));
      style.selectorHoverBg = toColor(map.colorErrorBgHover, QColor("#fff1f0"));
      style.selectorActiveBorderColor = statusColor;
      style.selectorTextColor = toColor(map.colorErrorText, QColor("#ff4d4f"));
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
    } else if (input.variant == AdLineEdit::Variant::Borderless) {
      style.selectorTextColor = statusColor;
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
    } else {
      style.selectorBorderColor = statusColor;
      style.selectorHoverBorderColor = toColor(map.colorErrorBorderHover, QColor("#ff7875"));
      style.selectorActiveBorderColor = statusColor;
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
      if (input.variant == AdLineEdit::Variant::Outlined) {
        style.selectorFocusOutlineColor = errorOutline;
      }
    }
  } else if (input.status == AdLineEdit::Status::Warning) {
    const QColor statusColor = toColor(map.colorWarning, QColor("#faad14"));
    style.countColor = statusColor;
    if (input.variant == AdLineEdit::Variant::Filled) {
      style.selectorBg = toColor(map.colorWarningBg, QColor("#fffbe6"));
      style.selectorHoverBg = toColor(map.colorWarningBgHover, QColor("#fffbe6"));
      style.selectorActiveBorderColor = statusColor;
      style.selectorTextColor = toColor(map.colorWarningText, QColor("#d48806"));
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
    } else if (input.variant == AdLineEdit::Variant::Borderless) {
      style.selectorTextColor = statusColor;
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
    } else {
      style.selectorBorderColor = statusColor;
      style.selectorHoverBorderColor = toColor(map.colorWarningBorderHover, QColor("#ffd666"));
      style.selectorActiveBorderColor = statusColor;
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
      if (input.variant == AdLineEdit::Variant::Outlined) {
        style.selectorFocusOutlineColor = warningOutline;
      }
    }
  }

  if (input.variant != AdLineEdit::Variant::Filled) {
    style.selectorHoverBg = style.selectorBg;
    style.selectorActiveBg = style.selectorBg;
  }

  if (input.disabled) {
    if (input.variant == AdLineEdit::Variant::Borderless) {
      style.selectorBg = transparent;
      style.selectorHoverBg = transparent;
      style.selectorActiveBg = transparent;
      style.selectorBorderColor = transparent;
      style.selectorHoverBorderColor = transparent;
      style.selectorActiveBorderColor = transparent;
      style.selectorFocusOutlineColor = transparent;
    } else {
      style.selectorBg = style.disabledBg;
      style.selectorHoverBg = style.disabledBg;
      style.selectorActiveBg = style.disabledBg;
      if (input.variant != AdLineEdit::Variant::Underlined) {
        style.selectorBorderColor = style.disabledBorderColor;
      }
      style.selectorHoverBorderColor = style.selectorBorderColor;
      style.selectorActiveBorderColor = style.selectorBorderColor;
      style.selectorFocusOutlineColor = transparent;
    }

    style.selectorTextColor = style.disabledTextColor;
    style.placeholderColor = style.disabledTextColor;
    style.clearColor = style.disabledTextColor;
    style.clearHoverColor = style.disabledTextColor;
    style.clearActiveColor = style.disabledTextColor;
    style.prefixColor = style.disabledTextColor;
    style.suffixColor = style.disabledTextColor;
    // Keep the password visibility toggle readable as a weak action even in the
    // disabled state. Ant Design treats it differently from passive affix content:
    // the eye icon stays tertiary instead of collapsing to disabled text color.
    style.suffixActionHoverColor = style.suffixActionColor;
    style.countColor = style.disabledTextColor;
  }

  if (input.variant != AdLineEdit::Variant::Borderless) {
    // Qt paints the shell directly onto the parent surface. Resolve semi-transparent
    // theme fills against the container color so filled/disabled inputs match the
    // stable Ant Design appearance instead of drifting with arbitrary parent backgrounds.
    style.selectorBg = compositeOn(style.selectorBg, containerBg);
    style.selectorHoverBg = compositeOn(style.selectorHoverBg, containerBg);
    style.selectorActiveBg = compositeOn(style.selectorActiveBg, containerBg);
    style.disabledBg = compositeOn(style.disabledBg, containerBg);
  }

  return style;
}

InputVisualStyle resolveInputVisualStyle(const InputStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveInputVisualStyle(input, resolved);
}

TextControlVisualStyle resolveTextControlVisualStyle(
    const TextControlStyleInput& input, const adqt::theme::ResolvedTheme& resolvedTheme) {
  return resolveInputVisualStyle(input, resolvedTheme);
}

TextControlVisualStyle resolveTextControlVisualStyle(const TextControlStyleInput& input) {
  return resolveInputVisualStyle(input);
}

}  // namespace adqt::widgets::detail
