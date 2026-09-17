#include "input_number_style.h"

#include <algorithm>

#include "theme/theme.h"

namespace adqt::widgets::detail {

namespace {

bool shouldShowInputNumberHandles(const InputNumberStyleInput& input) {
  return input.stepButtonLayout == AdInputNumber::StepButtonLayout::Split || input.hovered;
}

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor resolveTokenColor(const std::optional<QColor>& token, const QColor& fallback) {
  if (!token.has_value()) {
    return fallback;
  }
  return toColor(token.value(), fallback);
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

  const float fgAlpha = std::clamp(foreground.alphaF(), 0.0F, 1.0F);
  if (fgAlpha >= 0.999F) {
    return foreground;
  }

  QColor mixed;
  mixed.setRedF(foreground.redF() * fgAlpha + background.redF() * (1.0F - fgAlpha));
  mixed.setGreenF(foreground.greenF() * fgAlpha + background.greenF() * (1.0F - fgAlpha));
  mixed.setBlueF(foreground.blueF() * fgAlpha + background.blueF() * (1.0F - fgAlpha));
  mixed.setAlpha(255);
  return mixed;
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

void applyAppearanceRole(const AdInputNumber::AppearanceRole& role, QColor* textColor,
                         QColor* backgroundColor, QColor* borderColor) {
  if (textColor && role.textColor.has_value()) {
    *textColor = role.textColor.value();
  }
  if (backgroundColor && role.backgroundColor.has_value()) {
    *backgroundColor = role.backgroundColor.value();
  }
  if (borderColor && role.borderColor.has_value()) {
    *borderColor = role.borderColor.value();
  }
}

}  // namespace

InputNumberVisualStyle resolveInputNumberVisualStyle(const InputNumberStyleInput& input,
                                                     const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const QColor transparent(0, 0, 0, 0);
  const QColor containerBg = toColor(map.colorBgContainer, QColor("#ffffff"));

  InputNumberVisualStyle style;
  style.selectorBg = containerBg;
  style.selectorHoverBg = style.selectorBg;
  style.selectorActiveBg = style.selectorBg;
  style.selectorBorderColor = toColor(map.colorBorder, QColor("#d9d9d9"));
  style.selectorHoverBorderColor = toColor(map.colorPrimaryHover, QColor("#4096ff"));
  style.selectorActiveBorderColor = toColor(map.colorPrimary, QColor("#1677ff"));
  style.selectorFocusOutlineColor =
      deriveAlphaColor(toColor(map.colorPrimaryBg, QColor("#e6f4ff")), containerBg);
  style.selectorTextColor = toColor(map.colorText, QColor("#141414"));
  style.placeholderColor = toColor(map.colorTextPlaceholder, QColor("#bfbfbf"));
  style.prefixColor = style.selectorTextColor;
  style.suffixColor = style.selectorTextColor;
  style.handleBg = containerBg;
  style.handleActiveBg = toColor(map.colorFillAlter, QColor("#f5f5f5"));
  style.handleBorderColor = toColor(map.colorBorder, QColor("#d9d9d9"));
  style.handleHoverColor = toColor(map.colorPrimary, QColor("#1677ff"));
  style.handleIconColor = toColor(map.colorTextQuaternary, QColor("#bfbfbf"));
  style.outOfRangeTextColor = toColor(map.colorError, QColor("#ff4d4f"));
  style.disabledTextColor = toColor(map.colorTextDisabled, QColor("#bfbfbf"));
  style.disabledBg = toColor(map.colorBgContainerDisabled, QColor("#f5f5f5"));
  style.disabledBorderColor = toColor(map.colorBorderDisabled, QColor("#d9d9d9"));

  style.metrics.font = input.baseFont;
  style.metrics.font.setPixelSize(std::max(12, qRound(map.fontSize)));
  style.metrics.inputFontSize = std::max(12, qRound(map.fontSize));
  style.metrics.height = std::max(24, qRound(map.controlHeight));
  style.metrics.width = 90;
  style.metrics.borderRadius = std::max(0, qRound(map.borderRadius));
  style.metrics.borderWidth = std::max(1, qRound(map.lineWidth));
  style.metrics.horizontalPadding = std::max(8, qRound(map.sizeSM - map.lineWidth));
  style.metrics.affixPadding = std::max(2, qRound(map.sizeXXS));
  style.metrics.affixItemGap = std::max(style.metrics.affixPadding, qRound(map.sizeXS));
  style.metrics.iconSize = std::max(10, style.metrics.font.pixelSize());
  style.metrics.handleIconSize = std::max(6, qRound(map.fontSize / 2.0));
  style.metrics.splitIconSize = std::max(8, style.metrics.inputFontSize);
  style.metrics.handleWidth = std::max(16, qRound(map.controlHeightSM - map.lineWidth * 2.0));
  style.metrics.handleVisibleWidth =
      input.stepButtonsVisible
          ? (shouldShowInputNumberHandles(input) ? style.metrics.handleWidth : 0)
          : 0;
  style.metrics.focusOutlineWidth = std::max<qreal>(1.0, map.lineWidth * 3.0);
  style.metrics.focusOutlineOffset = 0.0;

  if (input.controlSize == AdInputNumber::ControlSize::Large) {
    style.metrics.height = std::max(28, qRound(map.controlHeightLG));
    style.metrics.borderRadius = std::max(0, qRound(map.borderRadiusLG));
    style.metrics.inputFontSize = std::max(12, qRound(map.fontSizeLG));
    style.metrics.font.setPixelSize(style.metrics.inputFontSize);
    style.metrics.iconSize = std::max(10, style.metrics.font.pixelSize());
    style.metrics.splitIconSize = std::max(8, style.metrics.inputFontSize);
  } else if (input.controlSize == AdInputNumber::ControlSize::Small) {
    style.metrics.height = std::max(22, qRound(map.controlHeightSM));
    style.metrics.borderRadius = std::max(0, qRound(map.borderRadiusSM));
    style.metrics.inputFontSize = std::max(12, qRound(map.fontSizeSM));
    style.metrics.font.setPixelSize(style.metrics.inputFontSize);
    style.metrics.horizontalPadding = std::max(6, qRound(map.sizeXS - map.lineWidth));
    style.metrics.iconSize = std::max(10, style.metrics.font.pixelSize());
    style.metrics.splitIconSize = std::max(8, style.metrics.inputFontSize);
  }

  if (input.variant == AdInputNumber::Variant::Filled) {
    style.selectorBg = toColor(map.colorFillTertiary, QColor("#f5f5f5"));
    style.selectorHoverBg = toColor(map.colorFillSecondary, QColor("#f0f0f0"));
    style.selectorActiveBg = containerBg;
    style.selectorBorderColor = transparent;
    style.selectorHoverBorderColor = transparent;
    style.selectorFocusOutlineColor = transparent;
    const QColor filledHandleBg =
        compositeOn(toColor(map.colorFillSecondary, QColor("#f0f0f0")), containerBg);
    style.handleBg = input.focused ? containerBg : filledHandleBg;
  } else if (input.variant == AdInputNumber::Variant::Borderless) {
    style.selectorBg = transparent;
    style.selectorHoverBg = transparent;
    style.selectorActiveBg = transparent;
    style.selectorBorderColor = transparent;
    style.selectorHoverBorderColor = transparent;
    style.selectorActiveBorderColor = transparent;
    style.selectorFocusOutlineColor = transparent;
    style.handleBg = transparent;
    style.handleBorderColor = transparent;
  } else if (input.variant == AdInputNumber::Variant::Underlined) {
    style.underlined = true;
    style.metrics.borderRadius = 0;
    style.selectorFocusOutlineColor = transparent;
  }

  if (input.status == AdInputNumber::Status::Error) {
    const QColor statusColor = toColor(map.colorError, QColor("#ff4d4f"));
    style.outOfRangeTextColor = statusColor;
    if (input.variant == AdInputNumber::Variant::Filled) {
      style.selectorBg = toColor(map.colorErrorBg, QColor("#fff2f0"));
      style.selectorHoverBg = toColor(map.colorErrorBgHover, QColor("#fff1f0"));
      style.selectorActiveBorderColor = statusColor;
      style.selectorTextColor = toColor(map.colorErrorText, QColor("#ff4d4f"));
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
    } else if (input.variant == AdInputNumber::Variant::Borderless) {
      style.selectorTextColor = statusColor;
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
    } else {
      style.selectorBorderColor = statusColor;
      style.selectorHoverBorderColor = toColor(map.colorErrorBorderHover, QColor("#ff7875"));
      style.selectorActiveBorderColor = statusColor;
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
      if (input.variant == AdInputNumber::Variant::Outlined) {
        style.selectorFocusOutlineColor =
            deriveAlphaColor(toColor(map.colorErrorBg, QColor("#fff2f0")), containerBg);
      }
    }
  } else if (input.status == AdInputNumber::Status::Warning) {
    const QColor statusColor = toColor(map.colorWarning, QColor("#faad14"));
    if (input.variant == AdInputNumber::Variant::Filled) {
      style.selectorBg = toColor(map.colorWarningBg, QColor("#fffbe6"));
      style.selectorHoverBg = toColor(map.colorWarningBgHover, QColor("#fffbe6"));
      style.selectorActiveBorderColor = statusColor;
      style.selectorTextColor = toColor(map.colorWarningText, QColor("#d48806"));
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
    } else if (input.variant == AdInputNumber::Variant::Borderless) {
      style.selectorTextColor = statusColor;
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
    } else {
      style.selectorBorderColor = statusColor;
      style.selectorHoverBorderColor = toColor(map.colorWarningBorderHover, QColor("#ffd666"));
      style.selectorActiveBorderColor = statusColor;
      style.prefixColor = statusColor;
      style.suffixColor = statusColor;
      if (input.variant == AdInputNumber::Variant::Outlined) {
        style.selectorFocusOutlineColor =
            deriveAlphaColor(toColor(map.colorWarningBg, QColor("#fffbe6")), containerBg);
      }
    }
  }

  const auto& metrics = input.appearanceOverrides.metrics;
  if (metrics.controlWidth.has_value()) {
    style.metrics.width = std::max(48, metrics.controlWidth.value());
  }
  if (metrics.controlHeight.has_value()) {
    style.metrics.height = std::max(20, metrics.controlHeight.value());
  }
  if (metrics.borderRadius.has_value()) {
    style.metrics.borderRadius = std::max(0, metrics.borderRadius.value());
  }
  if (metrics.borderWidth.has_value()) {
    style.metrics.borderWidth = std::max(0, metrics.borderWidth.value());
  }
  if (metrics.horizontalPadding.has_value()) {
    style.metrics.horizontalPadding = std::max(0, metrics.horizontalPadding.value());
  }
  if (metrics.iconSize.has_value()) {
    const int resolvedSize = std::max(8, metrics.iconSize.value());
    style.metrics.iconSize = resolvedSize;
    style.metrics.handleIconSize = resolvedSize;
    style.metrics.splitIconSize = resolvedSize;
  }
  if (metrics.handleWidth.has_value()) {
    style.metrics.handleWidth = std::max(12, metrics.handleWidth.value());
  }
  if (metrics.handleVisibleWidth.has_value()) {
    style.metrics.handleVisibleWidth =
        input.stepButtonsVisible ? std::max(0, metrics.handleVisibleWidth.value()) : 0;
  } else if (metrics.handleWidth.has_value()) {
    style.metrics.handleVisibleWidth =
        input.stepButtonsVisible
            ? (shouldShowInputNumberHandles(input) ? style.metrics.handleWidth : 0)
            : 0;
  }

  if (metrics.inputFontSize.has_value()) {
    style.metrics.inputFontSize = std::max(8, metrics.inputFontSize.value());
    style.metrics.font.setPixelSize(style.metrics.inputFontSize);
  }
  if (input.controlSize == AdInputNumber::ControlSize::Small &&
      metrics.inputFontSizeSM.has_value()) {
    style.metrics.inputFontSize = std::max(8, metrics.inputFontSizeSM.value());
    style.metrics.font.setPixelSize(style.metrics.inputFontSize);
  }
  if (input.controlSize == AdInputNumber::ControlSize::Large &&
      metrics.inputFontSizeLG.has_value()) {
    style.metrics.inputFontSize = std::max(8, metrics.inputFontSizeLG.value());
    style.metrics.font.setPixelSize(style.metrics.inputFontSize);
  }
  if (input.controlSize == AdInputNumber::ControlSize::Large &&
      metrics.largeHorizontalPadding.has_value()) {
    style.metrics.horizontalPadding = std::max(0, metrics.largeHorizontalPadding.value());
  }
  if (!metrics.iconSize.has_value()) {
    style.metrics.iconSize = std::max(10, style.metrics.font.pixelSize());
    style.metrics.splitIconSize = std::max(8, style.metrics.inputFontSize);
  }

  const auto& colors = input.appearanceOverrides.colors;
  if (colors.backgroundColor.has_value()) {
    style.selectorBg = toColor(colors.backgroundColor.value(), style.selectorBg);
    if (input.variant != AdInputNumber::Variant::Filled) {
      style.selectorHoverBg = style.selectorBg;
      style.selectorActiveBg = style.selectorBg;
    }
  }
  style.selectorBorderColor = resolveTokenColor(colors.borderColor, style.selectorBorderColor);
  style.selectorHoverBorderColor =
      resolveTokenColor(colors.hoverBorderColor, style.selectorHoverBorderColor);
  style.selectorActiveBorderColor =
      resolveTokenColor(colors.activeBorderColor, style.selectorActiveBorderColor);
  style.selectorTextColor = resolveTokenColor(colors.textColor, style.selectorTextColor);
  style.placeholderColor = resolveTokenColor(colors.placeholderColor, style.placeholderColor);
  style.prefixColor = resolveTokenColor(colors.prefixColor, style.prefixColor);
  style.suffixColor = resolveTokenColor(colors.suffixColor, style.suffixColor);
  style.handleBg = resolveTokenColor(colors.actionBackgroundColor, style.handleBg);
  style.handleActiveBg =
      resolveTokenColor(colors.actionPressedBackgroundColor, style.handleActiveBg);
  style.handleBorderColor = resolveTokenColor(colors.actionBorderColor, style.handleBorderColor);
  style.handleHoverColor = resolveTokenColor(colors.actionHoverColor, style.handleHoverColor);
  style.handleIconColor = resolveTokenColor(colors.actionIconColor, style.handleIconColor);

  const auto& appearance = input.appearanceOverrides;
  applyAppearanceRole(appearance.frame, nullptr, &style.selectorBg, &style.selectorBorderColor);
  applyAppearanceRole(appearance.input, &style.selectorTextColor, &style.selectorBg,
                      &style.selectorBorderColor);
  applyAppearanceRole(appearance.prefix, &style.prefixColor, nullptr, nullptr);
  applyAppearanceRole(appearance.suffix, &style.suffixColor, nullptr, nullptr);
  applyAppearanceRole(appearance.actions, &style.handleIconColor, &style.handleBg,
                      &style.handleBorderColor);
  applyAppearanceRole(appearance.action, &style.handleIconColor, &style.handleActiveBg, nullptr);

  if (input.variant != AdInputNumber::Variant::Filled) {
    style.selectorHoverBg = style.selectorBg;
    style.selectorActiveBg = style.selectorBg;
  }

  if (input.disabled) {
    if (input.variant == AdInputNumber::Variant::Borderless) {
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
      if (input.variant != AdInputNumber::Variant::Underlined) {
        style.selectorBorderColor = style.disabledBorderColor;
      }
      style.selectorHoverBorderColor = style.selectorBorderColor;
      style.selectorActiveBorderColor = style.selectorBorderColor;
      style.selectorFocusOutlineColor = transparent;
    }

    style.selectorTextColor = style.disabledTextColor;
    style.placeholderColor = style.disabledTextColor;
    style.prefixColor = style.disabledTextColor;
    style.suffixColor = style.disabledTextColor;
    style.handleIconColor = style.disabledTextColor;
    style.handleHoverColor = style.disabledTextColor;
    style.handleBg = style.disabledBg;
    style.handleActiveBg = style.disabledBg;
    style.handleBorderColor = style.disabledBorderColor;
  }

  return style;
}

InputNumberVisualStyle resolveInputNumberVisualStyle(const InputNumberStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveInputNumberVisualStyle(input, resolved);
}

}  // namespace adqt::widgets::detail
