#include "switch_style.h"

#include "antd_icons.h"
#include "theme/theme.h"

#include <QFontMetrics>

#include <algorithm>
#include <cmath>

namespace adqt::widgets::detail {

namespace {

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor multiplyAlpha(const QColor& color, qreal opacity) {
  if (!color.isValid()) {
    return color;
  }
  QColor out = color;
  out.setAlphaF(
      static_cast<float>(std::clamp(static_cast<double>(color.alphaF()) * opacity, 0.0, 1.0)));
  return out;
}

int parseDurationMs(int value, int fallbackMs) {
  Q_UNUSED(fallbackMs)
  return std::max(0, value);
}

qreal snapToDevicePixelCoord(qreal value, qreal dpr) {
  if (dpr <= 0.0) {
    return value;
  }
  return qRound(value * dpr) / dpr;
}

QRectF snapRectToDevicePixels(const QRectF& rect, qreal dpr) {
  if (dpr <= 0.0) {
    return rect;
  }

  const qreal left = snapToDevicePixelCoord(rect.left(), dpr);
  const qreal top = snapToDevicePixelCoord(rect.top(), dpr);
  const qreal right = snapToDevicePixelCoord(rect.left() + rect.width(), dpr);
  const qreal bottom = snapToDevicePixelCoord(rect.top() + rect.height(), dpr);
  const qreal minSize = 1.0 / dpr;

  return QRectF(left, top, std::max(minSize, right - left), std::max(minSize, bottom - top));
}

void recomputeDerivedMetrics(SwitchMetrics* metrics) {
  if (!metrics) {
    return;
  }

  metrics->contentInsetNear = std::max(0, metrics->thumbSize / 2);
  metrics->contentInsetFar =
      std::max(metrics->contentInsetNear, metrics->thumbSize + metrics->trackPadding * 3);
  metrics->contentInsetNearSmall = std::max(0, metrics->thumbSizeSmall / 2);
  metrics->contentInsetFarSmall =
      std::max(metrics->contentInsetNearSmall, metrics->thumbSizeSmall + metrics->trackPadding * 3);
}

void applyMetricTokens(SwitchMetrics* metrics, const AdSwitch::MetricTokens& overrides) {
  if (!metrics) {
    return;
  }

  if (overrides.trackHeight.has_value()) {
    metrics->trackHeight = std::max(10, overrides.trackHeight.value());
  }
  if (overrides.smallTrackHeight.has_value()) {
    metrics->trackHeightSmall = std::max(8, overrides.smallTrackHeight.value());
  }
  if (overrides.trackMinWidth.has_value()) {
    metrics->trackMinWidth = std::max(16, overrides.trackMinWidth.value());
  }
  if (overrides.smallTrackMinWidth.has_value()) {
    metrics->trackMinWidthSmall = std::max(12, overrides.smallTrackMinWidth.value());
  }
  if (overrides.trackPadding.has_value()) {
    metrics->trackPadding = std::max(0, overrides.trackPadding.value());
  }
  if (overrides.thumbSize.has_value()) {
    metrics->thumbSize = std::max(6, overrides.thumbSize.value());
  }
  if (overrides.smallThumbSize.has_value()) {
    metrics->thumbSizeSmall = std::max(4, overrides.smallThumbSize.value());
  }
  if (overrides.loadingIndicatorSize.has_value()) {
    metrics->loadingIndicatorSize = std::max(6, overrides.loadingIndicatorSize.value());
  }
  if (overrides.disabledOpacity.has_value()) {
    metrics->disabledOpacity = std::clamp(overrides.disabledOpacity.value(), 0.0, 1.0);
  }

  recomputeDerivedMetrics(metrics);
}

void applyColorTokens(SwitchAppearance* appearance, const AdSwitch::ColorTokens& palette) {
  if (!appearance) {
    return;
  }

  if (palette.uncheckedTrack.has_value()) {
    appearance->uncheckedTrackColor = palette.uncheckedTrack.value();
  }
  if (palette.uncheckedTrackHover.has_value()) {
    appearance->uncheckedTrackHoverColor = palette.uncheckedTrackHover.value();
  } else if (palette.uncheckedTrack.has_value()) {
    appearance->uncheckedTrackHoverColor = appearance->uncheckedTrackColor;
  }

  if (palette.checkedTrack.has_value()) {
    appearance->checkedTrackColor = palette.checkedTrack.value();
  }
  if (palette.checkedTrackHover.has_value()) {
    appearance->checkedTrackHoverColor = palette.checkedTrackHover.value();
  } else if (palette.checkedTrack.has_value()) {
    appearance->checkedTrackHoverColor = appearance->checkedTrackColor;
  }

  if (palette.thumb.has_value()) {
    appearance->thumbColor = palette.thumb.value();
  }
  if (palette.thumbBorder.has_value()) {
    appearance->thumbBorderColor = palette.thumbBorder.value();
  }
  if (palette.thumbShadow.has_value()) {
    appearance->metrics.thumbShadowColor = palette.thumbShadow.value();
  }
  if (palette.content.has_value()) {
    appearance->contentColor = palette.content.value();
  }
  if (palette.loadingIndicator.has_value()) {
    appearance->loadingIndicatorColor = palette.loadingIndicator.value();
  }
  if (palette.checkedLoadingIndicator.has_value()) {
    appearance->checkedLoadingIndicatorColor = palette.checkedLoadingIndicator.value();
  } else if (palette.checkedTrack.has_value() && !palette.loadingIndicator.has_value()) {
    appearance->checkedLoadingIndicatorColor = appearance->checkedTrackColor;
  }
  if (palette.focusRing.has_value()) {
    appearance->focusRingColor = palette.focusRing.value();
  }
  if (palette.wave.has_value()) {
    appearance->waveColor = palette.wave.value();
  }
}

}  // namespace

SwitchAppearance resolveSwitchAppearance(const SwitchAppearanceInput& input,
                                         const adqt::theme::ResolvedTheme& resolved) {
  const adqt::theme::AdThemePalette& colors = resolved.theme.palette;
  const adqt::theme::ThemeSemanticPalette& semantic = resolved.theme.semantic;
  const adqt::theme::AdThemeMetrics& metrics = resolved.theme.metrics;
  const adqt::theme::AdThemeMotion& motion = resolved.theme.motion;

  constexpr int kTrackPadding = 2;
  const int trackHeight = std::max(12, qRound(metrics.fontSize * metrics.lineHeight));
  const int trackHeightSmall = std::max(10, qRound(metrics.controlHeight / 2.0));
  const int thumbSize = std::max(8, trackHeight - kTrackPadding * 2);
  const int thumbSizeSmall = std::max(6, trackHeightSmall - kTrackPadding * 2);
  const QColor primaryColor = toColor(colors.colorPrimary, QColor(QStringLiteral("#1677ff")));
  const QColor primaryHoverColor =
      toColor(colors.colorPrimaryHover, QColor(QStringLiteral("#4096ff")));
  const QColor contentColor =
      toColor(semantic.textOnAccent, toColor(colors.colorWhite, QColor(QStringLiteral("#ffffff"))));
  const QColor uncheckedIndicatorBase =
      toColor(semantic.textSecondary,
              toColor(colors.colorTextSecondary, QColor(QStringLiteral("#8c8c8c"))));
  const QColor checkedIndicatorBase = toColor(semantic.accent, primaryColor);
  const QColor thumbShadowBase =
      toColor(semantic.text, toColor(colors.colorText, QColor(QStringLiteral("#141414"))));

  SwitchAppearance appearance;
  appearance.uncheckedTrackColor =
      toColor(colors.colorTextQuaternary, QColor(QStringLiteral("#bfbfbf")));
  appearance.uncheckedTrackHoverColor =
      toColor(colors.colorTextTertiary, QColor(QStringLiteral("#8c8c8c")));
  appearance.checkedTrackColor = primaryColor;
  appearance.checkedTrackHoverColor = primaryHoverColor;
  appearance.trackBorderColor = QColor(0, 0, 0, 0);
  appearance.thumbColor = toColor(colors.colorWhite, QColor(QStringLiteral("#ffffff")));
  appearance.thumbBorderColor = QColor(0, 0, 0, 0);
  appearance.contentColor = contentColor;
  appearance.loadingIndicatorColor = multiplyAlpha(uncheckedIndicatorBase, 0.65);
  appearance.checkedLoadingIndicatorColor = checkedIndicatorBase;
  appearance.waveColor = primaryColor;
  appearance.focusRingColor = toColor(colors.colorPrimaryBorder, QColor(QStringLiteral("#91caff")));

  appearance.metrics.trackHeight = trackHeight;
  appearance.metrics.trackHeightSmall = trackHeightSmall;
  appearance.metrics.trackPadding = kTrackPadding;
  appearance.metrics.thumbSize = thumbSize;
  appearance.metrics.thumbSizeSmall = thumbSizeSmall;
  appearance.metrics.trackMinWidth = thumbSize * 2 + kTrackPadding * 4;
  appearance.metrics.trackMinWidthSmall = thumbSizeSmall * 2 + kTrackPadding * 2;
  appearance.metrics.loadingIndicatorSize = std::max(8, qRound(metrics.fontSizeSM * 0.75));
  appearance.metrics.fontSize = std::max(10, qRound(metrics.fontSizeSM));
  appearance.metrics.contentGap = 4;
  appearance.metrics.animationDurationMs = parseDurationMs(motion.motionDurationMid, 200);
  appearance.metrics.disabledOpacity = 0.65;
  appearance.metrics.focusRingWidth = std::max<qreal>(1.0, metrics.lineWidth * 3.0);
  appearance.metrics.focusRingOffset = 1.0;
  appearance.metrics.thumbShadowColor = multiplyAlpha(thumbShadowBase, 0.14);
  appearance.metrics.thumbShadowOffsetY = 2.0;
  appearance.metrics.thumbActiveStretchRatio = 0.3;
  appearance.metrics.contentPressOffset = kTrackPadding * 2;
  appearance.metrics.contentPressOffsetSmall = std::max(1, qRound(metrics.sizeXXS / 2.0));
  recomputeDerivedMetrics(&appearance.metrics);

  if (!motion.motion) {
    appearance.metrics.animationDurationMs = 0;
  }

  applyMetricTokens(&appearance.metrics, input.componentTokens.metrics);
  applyColorTokens(&appearance, input.componentTokens.colors);

  if (input.disabled || input.loading) {
    appearance.uncheckedTrackHoverColor = appearance.uncheckedTrackColor;
    appearance.checkedTrackHoverColor = appearance.checkedTrackColor;

    const qreal opacity = appearance.metrics.disabledOpacity;
    appearance.uncheckedTrackColor = multiplyAlpha(appearance.uncheckedTrackColor, opacity);
    appearance.uncheckedTrackHoverColor =
        multiplyAlpha(appearance.uncheckedTrackHoverColor, opacity);
    appearance.checkedTrackColor = multiplyAlpha(appearance.checkedTrackColor, opacity);
    appearance.checkedTrackHoverColor = multiplyAlpha(appearance.checkedTrackHoverColor, opacity);
    appearance.trackBorderColor = multiplyAlpha(appearance.trackBorderColor, opacity);
    appearance.thumbColor = multiplyAlpha(appearance.thumbColor, opacity);
    appearance.thumbBorderColor = multiplyAlpha(appearance.thumbBorderColor, opacity);
    appearance.contentColor = multiplyAlpha(appearance.contentColor, opacity);
    appearance.loadingIndicatorColor = multiplyAlpha(appearance.loadingIndicatorColor, opacity);
    appearance.checkedLoadingIndicatorColor =
        multiplyAlpha(appearance.checkedLoadingIndicatorColor, opacity);
    appearance.waveColor = multiplyAlpha(appearance.waveColor, opacity);
    appearance.metrics.thumbShadowColor =
        multiplyAlpha(appearance.metrics.thumbShadowColor, opacity);
  }

  if (!input.componentTokens.colors.wave.has_value()) {
    appearance.waveColor =
        input.checked ? appearance.checkedTrackColor : appearance.uncheckedTrackColor;
  }

  return appearance;
}

SwitchAppearance resolveSwitchAppearance(const SwitchAppearanceInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveSwitchAppearance(input, resolved);
}

SwitchGeometry buildSwitchGeometry(const QRectF& bounds, Qt::LayoutDirection direction,
                                   const SwitchAppearance& appearance,
                                   AdSwitch::ControlSize controlSize, qreal thumbProgress,
                                   qreal pressProgress, qreal pressDirection,
                                   qreal devicePixelRatio) {
  const bool small = controlSize == AdSwitch::ControlSize::Small;
  const int trackHeight =
      small ? appearance.metrics.trackHeightSmall : appearance.metrics.trackHeight;
  const int thumbSize = small ? appearance.metrics.thumbSizeSmall : appearance.metrics.thumbSize;
  const int trackPadding = appearance.metrics.trackPadding;

  SwitchGeometry geometry;
  geometry.trackRect = bounds;
  if (geometry.trackRect.height() > trackHeight) {
    const qreal top = geometry.trackRect.top() + (geometry.trackRect.height() - trackHeight) / 2.0;
    geometry.trackRect.setTop(top);
    geometry.trackRect.setHeight(trackHeight);
  }
  geometry.trackRect =
      snapRectToDevicePixels(geometry.trackRect.adjusted(0.5, 0.5, -0.5, -0.5), devicePixelRatio);
  geometry.trackRadius = geometry.trackRect.height() / 2.0;

  const bool uncheckedOnLeft = direction == Qt::LeftToRight;
  const qreal uncheckedThumbLeft = uncheckedOnLeft
                                       ? geometry.trackRect.left() + trackPadding
                                       : geometry.trackRect.right() - trackPadding - thumbSize;
  const qreal checkedThumbLeft = uncheckedOnLeft
                                     ? geometry.trackRect.right() - trackPadding - thumbSize
                                     : geometry.trackRect.left() + trackPadding;
  const qreal thumbLeft =
      uncheckedThumbLeft + (checkedThumbLeft - uncheckedThumbLeft) * thumbProgress;

  geometry.thumbRect =
      QRectF(thumbLeft, geometry.trackRect.top() + (geometry.trackRect.height() - thumbSize) / 2.0,
             thumbSize, thumbSize);
  geometry.thumbRect =
      snapRectToDevicePixels(geometry.thumbRect.adjusted(0.5, 0.5, -0.5, -0.5), devicePixelRatio);
  geometry.thumbVisualRect = geometry.thumbRect;
  if (pressProgress > 0.0 && appearance.metrics.thumbActiveStretchRatio > 0.0) {
    const qreal activeInset =
        thumbSize * appearance.metrics.thumbActiveStretchRatio * pressProgress;
    if (pressDirection < 0.0) {
      geometry.thumbVisualRect = geometry.thumbVisualRect.adjusted(-activeInset, 0.0, 0.0, 0.0);
    } else {
      geometry.thumbVisualRect = geometry.thumbVisualRect.adjusted(0.0, 0.0, activeInset, 0.0);
    }
  }
  geometry.thumbVisualRect = snapRectToDevicePixels(geometry.thumbVisualRect, devicePixelRatio);

  return geometry;
}

int switchContentWidth(const QString& text, bool hasIcon, const SwitchAppearance& appearance,
                       const QFont& baseFont) {
  const bool hasText = !text.trimmed().isEmpty();
  if (!hasIcon && !hasText) {
    return 0;
  }

  QFont contentFont = baseFont;
  contentFont.setPixelSize(appearance.metrics.fontSize);
  const QFontMetrics fm(contentFont);
  const int iconSide = std::max(10, appearance.metrics.fontSize);
  const int textWidth = hasText ? std::max(0, fm.horizontalAdvance(text)) : 0;
  const int gap = hasIcon && hasText ? appearance.metrics.contentGap : 0;
  return (hasIcon ? iconSide : 0) + gap + textWidth;
}

}  // namespace adqt::widgets::detail
