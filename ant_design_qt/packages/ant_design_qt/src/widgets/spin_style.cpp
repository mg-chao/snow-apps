#include "spin_style.h"

#include <algorithm>

#include "theme/theme_types.h"
#include "theme/theme_color_utils.h"

namespace adqt::widgets::detail {

namespace {

template <typename T>
T resolved(const std::optional<T>& overrideValue, const T& fallback) {
  return overrideValue.value_or(fallback);
}

QColor withAlpha(const QColor& color, int alpha) {
  QColor result = color;
  result.setAlpha(std::clamp(alpha, 0, 255));
  return result;
}

}  // namespace

SpinVisualStyle resolveSpinVisualStyle(const SpinStyleInput& input,
                                       const adqt::theme::ResolvedTheme& resolvedTheme) {
  const auto& values = resolvedTheme.values;
  const auto& semantic = resolvedTheme.semantic;
  const auto& colors = input.componentTokens.colors;
  const auto& metrics = input.componentTokens.metrics;

  SpinVisualStyle style;
  style.rootBackground = semantic.surface;
  style.indicator = resolved(colors.indicator, semantic.accent);
  style.description = resolved(colors.description, semantic.accent);
  style.progressTrack = resolved(colors.progressTrack, semantic.fillSecondary);
  // Ant composites a 40% surface mask inside content whose group opacity is
  // 50%. Over the same container background that is equivalent to a 70% native
  // mask.
  style.containerOverlay =
      resolved(colors.containerOverlay, withAlpha(semantic.surface, qRound(255.0 * 0.7)));
  style.fullscreenMask = resolved(colors.fullscreenMask, semantic.mask);
  style.fullscreenIndicator = resolved(colors.fullscreenIndicator, semantic.white);
  style.fullscreenDescription = resolved(colors.fullscreenDescription, semantic.textOnAccent);

  const int medium = std::max(1, qRound(values.controlHeightLG / 2.0));
  const int small = std::max(1, qRound(values.controlHeightLG * 0.35));
  const int large = std::max(1, qRound(values.controlHeight));
  switch (input.sizeClass) {
    case AdSpin::SizeClass::Small:
      style.dotSize = resolved(metrics.dotSizeSmall, small);
      break;
    case AdSpin::SizeClass::Large:
      style.dotSize = resolved(metrics.dotSizeLarge, large);
      break;
    case AdSpin::SizeClass::Medium:
    default:
      style.dotSize = resolved(metrics.dotSize, medium);
      break;
  }

  style.descriptionGap = std::max(0, resolved(metrics.descriptionGap, qRound(values.sizeSM)));
  style.animationCycleMs = std::max(0, resolved(metrics.animationCycleMs, 1200));
  style.autoProgressIntervalMs = std::max(1, resolved(metrics.autoProgressIntervalMs, 200));
  style.font = input.baseFont;
  style.font.setPixelSize(std::max(1, qRound(values.fontSize)));

  if (input.semanticStyles.root.backgroundColor) {
    style.rootBackground = *input.semanticStyles.root.backgroundColor;
    style.hasRootBackground = true;
  }
  if (input.semanticStyles.root.textColor) {
    style.indicator = *input.semanticStyles.root.textColor;
    style.description = *input.semanticStyles.root.textColor;
    style.fullscreenIndicator = *input.semanticStyles.root.textColor;
    style.fullscreenDescription = *input.semanticStyles.root.textColor;
  }
  if (input.semanticStyles.section.textColor) {
    style.indicator = *input.semanticStyles.section.textColor;
    style.description = *input.semanticStyles.section.textColor;
    style.fullscreenIndicator = *input.semanticStyles.section.textColor;
    style.fullscreenDescription = *input.semanticStyles.section.textColor;
  }
  if (input.semanticStyles.section.backgroundColor) {
    style.sectionBackground = *input.semanticStyles.section.backgroundColor;
    style.hasSectionBackground = true;
  }
  if (input.semanticStyles.indicator.textColor) {
    style.indicator = *input.semanticStyles.indicator.textColor;
    style.fullscreenIndicator = *input.semanticStyles.indicator.textColor;
  }
  if (input.semanticStyles.indicator.backgroundColor) {
    style.indicatorBackground = *input.semanticStyles.indicator.backgroundColor;
    style.hasIndicatorBackground = true;
  }
  if (input.semanticStyles.description.textColor) {
    style.description = *input.semanticStyles.description.textColor;
    style.fullscreenDescription = *input.semanticStyles.description.textColor;
  }
  if (input.semanticStyles.description.backgroundColor) {
    style.descriptionBackground = *input.semanticStyles.description.backgroundColor;
    style.hasDescriptionBackground = true;
  }
  if (input.semanticStyles.container.backgroundColor) {
    style.containerOverlay = *input.semanticStyles.container.backgroundColor;
  }

  if (values.backgroundOpacity != 1.0) {
    for (QColor* background :
         {&style.rootBackground, &style.sectionBackground, &style.indicatorBackground,
          &style.descriptionBackground, &style.progressTrack, &style.containerOverlay}) {
      *background = adqt::theme::applyBackgroundOpacity(*background, values.backgroundOpacity);
    }
  }
  return style;
}

}  // namespace adqt::widgets::detail
