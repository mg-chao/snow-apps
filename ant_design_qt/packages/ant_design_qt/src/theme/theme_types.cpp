#include "theme_types.h"

#include "fast_color_lite.h"
#include "palette_generate.h"
#include "theme_color_utils.h"
#include "theme_palette.h"

#include <algorithm>
#include <cmath>

namespace adqt::theme {

namespace {

QColor validOr(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor tone(const QVector<QColor>& mapped, int index, const QColor& fallback) {
  return index >= 0 && index < mapped.size() && mapped[index].isValid() ? mapped[index] : fallback;
}

QColor compositeOn(const QColor& foreground, const QColor& background, qreal opacityScale = 1.0) {
  if (!foreground.isValid()) {
    return background;
  }
  if (!background.isValid()) {
    QColor opaque = foreground;
    opaque.setAlpha(255);
    return opaque;
  }

  const float alpha = std::clamp(
      static_cast<float>(static_cast<qreal>(foreground.alphaF()) * opacityScale), 0.0F, 1.0F);
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

QColor alphaColor(const QColor& base, qreal alpha) {
  if (!base.isValid()) {
    return base;
  }

  QColor tinted = base.toRgb();
  tinted.setAlphaF(static_cast<float>(std::clamp(alpha, qreal(0.0), qreal(1.0))));
  return tinted;
}

FastColorLite toFastColor(const QColor& color) {
  const QColor rgb = color.toRgb();
  return FastColorLite(rgb.red(), rgb.green(), rgb.blue(), static_cast<double>(rgb.alphaF()));
}

QColor solidColor(const QColor& base, double amountPercent, bool raiseLightness = false) {
  if (!base.isValid()) {
    return base;
  }

  const FastColorLite fast = toFastColor(base);
  if (!fast.isValid()) {
    return base;
  }

  const FastColorLite adjusted =
      raiseLightness ? fast.lighten(amountPercent) : fast.darken(amountPercent);
  return QColor(adjusted.toHexString());
}

QColor mixColor(const QColor& first, const QColor& second, double amountPercent) {
  if (!first.isValid()) {
    return second;
  }
  if (!second.isValid()) {
    return first;
  }

  return QColor(toFastColor(first).mix(toFastColor(second), amountPercent).toHexString());
}

void copyAccents(ThemeAccents* target, const ThemeConfig& source) {
#define ADQT_COPY_ACCENT(name) target->name = source.name;
  ADQT_THEME_ACCENT_FIELDS(ADQT_COPY_ACCENT)
#undef ADQT_COPY_ACCENT
}

void applyTypographyMetrics(ThemeMetrics* metrics) {
  metrics->fontHeight = metrics->fontSize * metrics->lineHeight;
  metrics->fontHeightLG = metrics->fontSizeLG * metrics->lineHeightLG;
  metrics->fontHeightSM = metrics->fontSizeSM * metrics->lineHeightSM;
}

double positiveOr(double value, double fallback) { return value > 0.0 ? value : fallback; }

void applyDensityMetrics(ThemeMetrics* metrics, const ThemeConfig& config) {
  const bool compact = config.density == ThemeDensity::Compact;

  if (compact) {
    metrics->sizeXXL = 40.0;
    metrics->sizeXL = 28.0;
    metrics->sizeLG = 20.0;
    metrics->sizeMD = 16.0;
    metrics->sizeMS = 12.0;
    metrics->size = 12.0;
    metrics->sizeSM = 8.0;
    metrics->sizeXS = 4.0;
    metrics->sizeXXS = 2.0;
  } else {
    metrics->sizeXXL = 48.0;
    metrics->sizeXL = 32.0;
    metrics->sizeLG = 24.0;
    metrics->sizeMD = 20.0;
    metrics->sizeMS = 16.0;
    metrics->size = 16.0;
    metrics->sizeSM = 12.0;
    metrics->sizeXS = 8.0;
    metrics->sizeXXS = 4.0;
  }

  metrics->sizeUnit = positiveOr(config.sizeUnit, 4.0);
  metrics->sizeStep = positiveOr(config.sizeStep, compact ? 3.0 : 4.0);

  metrics->lineWidth = std::max(0.0, config.lineWidth);
  metrics->lineWidthBold = std::max(metrics->lineWidth, metrics->lineWidth * 2.0);

  metrics->borderRadius = std::max(0.0, config.borderRadius);
  metrics->borderRadiusXS = metrics->borderRadius / 3.0;
  metrics->borderRadiusSM = metrics->borderRadius * (2.0 / 3.0);
  metrics->borderRadiusLG = metrics->borderRadius * (4.0 / 3.0);
  metrics->borderRadiusOuter = metrics->borderRadiusLG;

  metrics->controlHeight = positiveOr(config.controlHeight, compact ? 28.0 : 32.0);
  metrics->controlHeightSM = compact ? std::max(16.0, metrics->controlHeight - 6.0)
                                     : std::max(16.0, metrics->controlHeight - 8.0);
  metrics->controlHeightXS = 16.0;
  metrics->controlHeightLG = metrics->controlHeight + 8.0;

  const double baseFontSize = positiveOr(config.fontSize, 14.0);
  metrics->fontSizeSM = std::max(10.0, baseFontSize - 2.0);
  metrics->fontSize = baseFontSize;
  metrics->fontSizeLG = baseFontSize + 2.0;
  metrics->fontSizeXL = baseFontSize + 6.0;
  metrics->fontSizeHeading1 = baseFontSize + 24.0;
  metrics->fontSizeHeading2 = baseFontSize + 16.0;
  metrics->fontSizeHeading3 = baseFontSize + 10.0;
  metrics->fontSizeHeading4 = baseFontSize + 6.0;
  metrics->fontSizeHeading5 = baseFontSize + 2.0;

  metrics->lineHeight = 1.5715;
  metrics->lineHeightLG = 1.5;
  metrics->lineHeightSM = 1.6667;
  metrics->lineHeightHeading1 = 1.2105;
  metrics->lineHeightHeading2 = 1.2667;
  metrics->lineHeightHeading3 = 1.3333;
  metrics->lineHeightHeading4 = 1.4;
  metrics->lineHeightHeading5 = 1.5;

  metrics->popupArrowSize = std::max(0, qRound(config.sizePopupArrow));
  metrics->popupZIndexBase = std::max(0, qRound(config.zIndexPopupBase));
  metrics->opacityImage = std::max(0.0, config.opacityImage);

  applyTypographyMetrics(metrics);
}

void applyMotion(ThemeMotion* motion, const ThemeConfig& config) {
  motion->motion = config.motion;
  motion->motionDurationFast = motion->motion ? 100 : 0;
  motion->motionDurationMid = motion->motion ? 200 : 0;
  motion->motionDurationSlow = motion->motion ? 300 : 0;

  motion->timingFrameIntervalMs = motion->motion ? 25 : 0;
  motion->timingSpinnerCycleMs = motion->motion ? 1000 : 0;
  motion->timingWaveDurationMs = motion->motion ? 560 : 0;
  motion->timingMenuOpenDelayMs = 0;
  motion->timingMenuCloseDelayMs = motion->motion ? 100 : 0;
  motion->timingLoadingDelayMs = 0;

  motion->motionEaseOutCirc = QEasingCurve(QEasingCurve::OutCirc);
  motion->motionEaseInOutCirc = QEasingCurve(QEasingCurve::InOutCirc);
  motion->motionEaseOut = QEasingCurve(QEasingCurve::OutCubic);
  motion->motionEaseInOut = QEasingCurve(QEasingCurve::InOutCubic);
  motion->motionEaseOutBack = QEasingCurve(QEasingCurve::OutBack);
  motion->motionEaseInBack = QEasingCurve(QEasingCurve::InBack);
  motion->motionEaseInQuint = QEasingCurve(QEasingCurve::InQuint);
  motion->motionEaseOutQuint = QEasingCurve(QEasingCurve::OutQuint);
}

ThemeSemanticPalette makeSemanticPalette(const ThemeColors& colors) {
  ThemeSemanticPalette semantic;
  const QColor window = compositeOn(validOr(colors.colorBgLayout, colors.colorBgBase),
                                    validOr(colors.colorBgBase, colors.colorWhite));
  const QColor surface = compositeOn(validOr(colors.colorBgContainer, colors.colorBgBase), window);
  const QColor surfaceDisabled =
      compositeOn(validOr(colors.colorBgContainerDisabled, colors.colorFillTertiary), surface);
  const QColor surfaceElevated =
      compositeOn(validOr(colors.colorBgElevated, colors.colorBgContainer), window);
  const QColor surfaceSubtle =
      compositeOn(validOr(colors.colorFillAlter, colors.colorFillQuaternary), surface);

  semantic.window = window;
  semantic.windowDisabled = window;
  semantic.surface = surface;
  semantic.surfaceDisabled = surfaceDisabled;
  semantic.surfaceElevated = surfaceElevated;
  semantic.surfaceSubtle = surfaceSubtle;
  semantic.surfaceSolid = compositeOn(colors.colorBgSolid, surface);
  semantic.surfaceSolidHover = compositeOn(colors.colorBgSolidHover, surface);
  semantic.surfaceSolidActive = compositeOn(colors.colorBgSolidActive, surface);
  semantic.surfaceSpotlight = compositeOn(colors.colorBgSpotlight, window);
  semantic.mask = colors.colorBgMask;
  semantic.fill = colors.colorFill;
  semantic.fillSecondary = colors.colorFillSecondary;
  semantic.fillTertiary = colors.colorFillTertiary;
  semantic.fillQuaternary = colors.colorFillQuaternary;
  semantic.text = colors.colorText;
  semantic.textSecondary = colors.colorTextSecondary;
  semantic.textTertiary = colors.colorTextTertiary;
  semantic.textQuaternary = colors.colorTextQuaternary;
  semantic.textDisabled = colors.colorTextDisabled;
  semantic.textPlaceholder = colors.colorTextPlaceholder;
  semantic.textOnAccent = colors.colorTextLightSolid;
  semantic.border = compositeOn(colors.colorBorder, surface);
  semantic.borderDisabled = compositeOn(colors.colorBorderDisabled, surfaceDisabled);
  semantic.borderSecondary = compositeOn(colors.colorBorderSecondary, surface);
  semantic.accent = colors.colorPrimary;
  semantic.accentHover = colors.colorPrimaryHover;
  semantic.accentActive = colors.colorPrimaryActive;
  semantic.accentSubtle = compositeOn(colors.colorPrimaryBg, surface);
  semantic.accentSubtleHover = compositeOn(colors.colorPrimaryBgHover, surface);
  semantic.accentBorder = colors.colorPrimaryBorder;
  semantic.accentBorderHover = colors.colorPrimaryBorderHover;
  semantic.accentSolid = colors.colorPrimarySolid;
  semantic.accentSolidHover = colors.colorPrimarySolidHover;
  semantic.accentSolidActive = colors.colorPrimarySolidActive;
  semantic.accentSolidText = colors.colorPrimarySolidText;
  semantic.accentDisabled =
      compositeOn(validOr(colors.colorPrimary, colors.colorPrimarySolid), surfaceDisabled, 0.38);
  semantic.success = colors.colorSuccess;
  semantic.successHover = colors.colorSuccessHover;
  semantic.successActive = colors.colorSuccessActive;
  semantic.successSubtle = compositeOn(colors.colorSuccessBg, surface);
  semantic.successBorder = colors.colorSuccessBorder;
  semantic.warning = colors.colorWarning;
  semantic.warningHover = colors.colorWarningHover;
  semantic.warningActive = colors.colorWarningActive;
  semantic.warningSubtle = compositeOn(colors.colorWarningBg, surface);
  semantic.warningBorder = colors.colorWarningBorder;
  semantic.error = colors.colorError;
  semantic.errorHover = colors.colorErrorHover;
  semantic.errorActive = colors.colorErrorActive;
  semantic.errorSubtle = compositeOn(colors.colorErrorBg, surface);
  semantic.errorBorder = colors.colorErrorBorder;
  semantic.info = colors.colorInfo;
  semantic.infoHover = colors.colorInfoHover;
  semantic.infoActive = colors.colorInfoActive;
  semantic.infoSubtle = compositeOn(colors.colorInfoBg, surface);
  semantic.infoBorder = colors.colorInfoBorder;
  semantic.link = colors.colorLink;
  semantic.linkHover = colors.colorLinkHover;
  semantic.linkActive = colors.colorLinkActive;
  semantic.white = colors.colorWhite;
  return semantic;
}

void flattenTheme(ThemeValues* values, const AdTheme& theme) {
  values->scheme = theme.scheme;
  values->density = theme.density;
  values->appFont = theme.appFont;
  values->codeFont = theme.codeFont;
  values->wireframe = theme.wireframe;
  values->motion = theme.motion.motion;

#define ADQT_FLAT_ACCENT(name) values->name = theme.accents.name;
  ADQT_THEME_ACCENT_FIELDS(ADQT_FLAT_ACCENT)
#undef ADQT_FLAT_ACCENT

#define ADQT_FLAT_COLOR(name) values->name = theme.palette.name;
  ADQT_THEME_COLOR_FIELDS(ADQT_FLAT_COLOR)
#undef ADQT_FLAT_COLOR

#define ADQT_FLAT_DOUBLE(name) values->name = theme.metrics.name;
  ADQT_THEME_DOUBLE_FIELDS(ADQT_FLAT_DOUBLE)
#undef ADQT_FLAT_DOUBLE

#define ADQT_FLAT_METRIC_INT(name) values->name = theme.metrics.name;
  ADQT_THEME_METRIC_INT_FIELDS(ADQT_FLAT_METRIC_INT)
#undef ADQT_FLAT_METRIC_INT

#define ADQT_FLAT_MOTION_INT(name) values->name = theme.motion.name;
  ADQT_THEME_MOTION_INT_FIELDS(ADQT_FLAT_MOTION_INT)
#undef ADQT_FLAT_MOTION_INT

#define ADQT_FLAT_EASING(name) values->name = theme.motion.name;
  ADQT_THEME_EASING_FIELDS(ADQT_FLAT_EASING)
#undef ADQT_FLAT_EASING
}

ThemeConfig themeConfigFromTheme(const AdTheme& theme) {
  ThemeConfig config = defaultThemeConfig(theme.scheme, theme.density);

#define ADQT_COPY_CONFIG_ACCENT(name) config.name = theme.accents.name;
  ADQT_THEME_ACCENT_FIELDS(ADQT_COPY_CONFIG_ACCENT)
#undef ADQT_COPY_CONFIG_ACCENT

  config.primary = theme.palette.colorPrimary;
  config.success = theme.palette.colorSuccess;
  config.warning = theme.palette.colorWarning;
  config.error = theme.palette.colorError;
  config.info = theme.palette.colorInfo;
  config.link = theme.palette.colorLink;
  config.appFont = theme.appFont;
  config.codeFont = theme.codeFont;
  config.fontSize = theme.metrics.fontSize;
  config.lineWidth = theme.metrics.lineWidth;
  config.borderRadius = theme.metrics.borderRadius;
  config.sizeUnit = theme.metrics.sizeUnit;
  config.sizeStep = theme.metrics.sizeStep;
  config.sizePopupArrow = theme.metrics.popupArrowSize;
  config.controlHeight = theme.metrics.controlHeight;
  config.zIndexPopupBase = theme.metrics.popupZIndexBase;
  config.opacityImage = theme.metrics.opacityImage;
  config.wireframe = theme.wireframe;
  config.motion = theme.motion.motion;
  return config;
}

ThemeConfig mergeThemeConfigImpl(const ThemeConfig& base, const ThemeOverride& overrideValue) {
  ThemeConfig merged = base;

  if (overrideValue.scheme.has_value()) {
    merged.scheme = overrideValue.scheme.value();
  }
  if (overrideValue.density.has_value()) {
    merged.density = overrideValue.density.value();
  }

#define ADQT_APPLY_OVERRIDE_ACCENT(name)      \
  if (overrideValue.name.has_value()) {       \
    merged.name = overrideValue.name.value(); \
  }
  ADQT_THEME_ACCENT_FIELDS(ADQT_APPLY_OVERRIDE_ACCENT)
#undef ADQT_APPLY_OVERRIDE_ACCENT

  if (overrideValue.primary.has_value()) {
    merged.primary = overrideValue.primary.value();
  }
  if (overrideValue.success.has_value()) {
    merged.success = overrideValue.success.value();
  }
  if (overrideValue.warning.has_value()) {
    merged.warning = overrideValue.warning.value();
  }
  if (overrideValue.error.has_value()) {
    merged.error = overrideValue.error.value();
  }
  if (overrideValue.info.has_value()) {
    merged.info = overrideValue.info.value();
  }
  if (overrideValue.link.has_value()) {
    merged.link = overrideValue.link.value();
  }

  if (overrideValue.appFont.has_value()) {
    merged.appFont = overrideValue.appFont.value();
  }
  if (overrideValue.codeFont.has_value()) {
    merged.codeFont = overrideValue.codeFont.value();
  }

  if (overrideValue.fontSize.has_value()) {
    merged.fontSize = overrideValue.fontSize.value();
  }
  if (overrideValue.lineWidth.has_value()) {
    merged.lineWidth = overrideValue.lineWidth.value();
  }
  if (overrideValue.borderRadius.has_value()) {
    merged.borderRadius = overrideValue.borderRadius.value();
  }
  if (overrideValue.sizeUnit.has_value()) {
    merged.sizeUnit = overrideValue.sizeUnit.value();
  }
  if (overrideValue.sizeStep.has_value()) {
    merged.sizeStep = overrideValue.sizeStep.value();
  }
  if (overrideValue.sizePopupArrow.has_value()) {
    merged.sizePopupArrow = overrideValue.sizePopupArrow.value();
  }
  if (overrideValue.controlHeight.has_value()) {
    merged.controlHeight = overrideValue.controlHeight.value();
  }
  if (overrideValue.zIndexPopupBase.has_value()) {
    merged.zIndexPopupBase = overrideValue.zIndexPopupBase.value();
  }
  if (overrideValue.opacityImage.has_value()) {
    merged.opacityImage = overrideValue.opacityImage.value();
  }

  if (overrideValue.wireframe.has_value()) {
    merged.wireframe = overrideValue.wireframe.value();
  }
  if (overrideValue.motion.has_value()) {
    merged.motion = overrideValue.motion.value();
  }

  return merged;
}

bool isEmptyThemeOverrideImpl(const ThemeOverride& overrideValue) {
  return overrideValue == ThemeOverride{};
}

void applyLightSemanticColors(ThemeColors* colors, const ThemeConfig& config) {
  const QColor primaryBase =
      validOr(config.primary, validOr(config.blue, QColor(QStringLiteral("#1677ff"))));
  const QColor successBase =
      validOr(config.success, validOr(config.green, QColor(QStringLiteral("#52c41a"))));
  const QColor errorBase = validOr(config.error, QColor(QStringLiteral("#ff4d4f")));
  const QColor warningBase = validOr(config.warning, QColor(QStringLiteral("#faad14")));
  const QColor infoBase = validOr(config.info, primaryBase);
  const QColor linkBase = validOr(config.link, primaryBase);
  const QColor bgBase(Qt::white);
  const QColor textBase(Qt::black);

  const QVector<QColor> primary = generateMappedPalette(primaryBase, false, QColor());
  const QVector<QColor> success = generateMappedPalette(successBase, false, QColor());
  const QVector<QColor> error = generateMappedPalette(errorBase, false, QColor());
  const QVector<QColor> warning = generateMappedPalette(warningBase, false, QColor());
  const QVector<QColor> info = generateMappedPalette(infoBase, false, QColor());
  const QVector<QColor> link = generateMappedPalette(linkBase, false, QColor());

  colors->colorBgBase = bgBase;
  colors->colorTextBase = textBase;
  colors->colorText = alphaColor(textBase, 0.88);
  colors->colorTextSecondary = alphaColor(textBase, 0.65);
  colors->colorTextTertiary = alphaColor(textBase, 0.45);
  colors->colorTextQuaternary = alphaColor(textBase, 0.25);
  colors->colorTextDisabled = colors->colorTextQuaternary;
  colors->colorTextPlaceholder = colors->colorTextQuaternary;
  colors->colorTextLightSolid = QColor(Qt::white);

  colors->colorFill = alphaColor(textBase, 0.15);
  colors->colorFillSecondary = alphaColor(textBase, 0.06);
  colors->colorFillTertiary = alphaColor(textBase, 0.04);
  colors->colorFillQuaternary = alphaColor(textBase, 0.02);
  colors->colorFillAlter = colors->colorFillQuaternary;

  colors->colorBgSolid = alphaColor(textBase, 1.0);
  colors->colorBgSolidHover = alphaColor(textBase, 0.75);
  colors->colorBgSolidActive = alphaColor(textBase, 0.95);
  colors->colorBgLayout = solidColor(bgBase, 4.0);
  colors->colorBgContainer = solidColor(bgBase, 0.0);
  colors->colorBgContainerDisabled = colors->colorFillTertiary;
  colors->colorBgElevated = solidColor(bgBase, 0.0);
  colors->colorBgSpotlight = alphaColor(textBase, 0.85);
  colors->colorBgBlur = QColor(Qt::transparent);

  colors->colorBorder = solidColor(bgBase, 15.0);
  colors->colorBorderDisabled = colors->colorBorder;
  colors->colorBorderSecondary = solidColor(bgBase, 6.0);

  colors->colorPrimaryBg = tone(primary, 1, QColor(QStringLiteral("#e6f4ff")));
  colors->colorPrimaryBgHover = tone(primary, 2, QColor(QStringLiteral("#bae0ff")));
  colors->colorPrimaryBorder = tone(primary, 3, QColor(QStringLiteral("#91caff")));
  colors->colorPrimaryBorderHover = tone(primary, 4, QColor(QStringLiteral("#69b1ff")));
  colors->colorPrimaryHover = tone(primary, 5, QColor(QStringLiteral("#4096ff")));
  colors->colorPrimary = tone(primary, 6, primaryBase);
  colors->colorPrimaryActive = tone(primary, 7, QColor(QStringLiteral("#0958d9")));
  colors->colorPrimarySolid = colors->colorPrimary;
  colors->colorPrimarySolidHover = colors->colorPrimaryHover;
  colors->colorPrimarySolidActive = colors->colorPrimaryActive;
  colors->colorPrimarySolidText = colors->colorTextLightSolid;
  colors->colorPrimaryTextHover = colors->colorPrimaryHover;
  colors->colorPrimaryText = colors->colorPrimary;
  colors->colorPrimaryTextActive = colors->colorPrimaryActive;

  colors->colorSuccessBg = tone(success, 1, QColor(QStringLiteral("#f6ffed")));
  colors->colorSuccessBgHover = tone(success, 2, QColor(QStringLiteral("#d9f7be")));
  colors->colorSuccessBorder = tone(success, 3, QColor(QStringLiteral("#b7eb8f")));
  colors->colorSuccessBorderHover = tone(success, 4, QColor(QStringLiteral("#95de64")));
  colors->colorSuccessHover = tone(success, 4, QColor(QStringLiteral("#95de64")));
  colors->colorSuccess = tone(success, 6, successBase);
  colors->colorSuccessActive = tone(success, 7, QColor(QStringLiteral("#389e0d")));
  colors->colorSuccessSolid = colors->colorSuccess;
  colors->colorSuccessSolidHover = colors->colorSuccessHover;
  colors->colorSuccessSolidActive = colors->colorSuccessActive;
  colors->colorSuccessSolidText = colors->colorTextLightSolid;
  colors->colorSuccessTextHover = tone(success, 8, QColor(QStringLiteral("#73d13d")));
  colors->colorSuccessText = colors->colorSuccess;
  colors->colorSuccessTextActive = colors->colorSuccessActive;

  colors->colorErrorBg = tone(error, 1, QColor(QStringLiteral("#fff2f0")));
  colors->colorErrorBgHover = tone(error, 2, QColor(QStringLiteral("#fff1f0")));
  colors->colorErrorBgActive = tone(error, 3, QColor(QStringLiteral("#ffccc7")));
  colors->colorErrorBgFilledHover =
      mixColor(colors->colorErrorBg, colors->colorErrorBgActive, 50.0);
  colors->colorErrorBorder = tone(error, 3, QColor(QStringLiteral("#ffccc7")));
  colors->colorErrorBorderHover = tone(error, 4, QColor(QStringLiteral("#ff7875")));
  colors->colorErrorHover = tone(error, 5, QColor(QStringLiteral("#ff7875")));
  colors->colorError = tone(error, 6, errorBase);
  colors->colorErrorActive = tone(error, 7, QColor(QStringLiteral("#cf1322")));
  colors->colorErrorSolid = colors->colorError;
  colors->colorErrorSolidHover = colors->colorErrorHover;
  colors->colorErrorSolidActive = colors->colorErrorActive;
  colors->colorErrorSolidText = colors->colorTextLightSolid;
  colors->colorErrorTextHover = tone(error, 8, QColor(QStringLiteral("#ff7875")));
  colors->colorErrorText = colors->colorError;
  colors->colorErrorTextActive = colors->colorErrorActive;

  colors->colorWarningBg = tone(warning, 1, QColor(QStringLiteral("#fffbe6")));
  colors->colorWarningBgHover = tone(warning, 2, QColor(QStringLiteral("#fff1b8")));
  colors->colorWarningBorder = tone(warning, 3, QColor(QStringLiteral("#ffe58f")));
  colors->colorWarningBorderHover = tone(warning, 4, QColor(QStringLiteral("#ffd666")));
  colors->colorWarningHover = tone(warning, 4, QColor(QStringLiteral("#ffd666")));
  colors->colorWarning = tone(warning, 6, warningBase);
  colors->colorWarningActive = tone(warning, 7, QColor(QStringLiteral("#d48806")));
  colors->colorWarningSolid = colors->colorWarning;
  colors->colorWarningSolidHover = colors->colorWarningHover;
  colors->colorWarningSolidActive = colors->colorWarningActive;
  colors->colorWarningSolidText = colors->colorTextLightSolid;
  colors->colorWarningTextHover = tone(warning, 8, QColor(QStringLiteral("#ffc53d")));
  colors->colorWarningText = colors->colorWarning;
  colors->colorWarningTextActive = colors->colorWarningActive;

  colors->colorInfoBg = tone(info, 1, colors->colorPrimaryBg);
  colors->colorInfoBgHover = tone(info, 2, colors->colorPrimaryBgHover);
  colors->colorInfoBorder = tone(info, 3, colors->colorPrimaryBorder);
  colors->colorInfoBorderHover = tone(info, 4, colors->colorPrimaryBorderHover);
  colors->colorInfoHover = tone(info, 4, colors->colorPrimaryBorderHover);
  colors->colorInfo = tone(info, 6, infoBase);
  colors->colorInfoActive = tone(info, 7, colors->colorPrimaryActive);
  colors->colorInfoSolid = colors->colorInfo;
  colors->colorInfoSolidHover = colors->colorInfoHover;
  colors->colorInfoSolidActive = colors->colorInfoActive;
  colors->colorInfoSolidText = colors->colorTextLightSolid;
  colors->colorInfoTextHover = tone(info, 8, colors->colorPrimaryHover);
  colors->colorInfoText = colors->colorInfo;
  colors->colorInfoTextActive = colors->colorInfoActive;

  colors->colorLink = tone(link, 6, linkBase);
  colors->colorLinkHover = tone(link, 4, colors->colorPrimaryBorderHover);
  colors->colorLinkActive = tone(link, 7, colors->colorPrimaryActive);
  colors->colorBgMask = alphaColor(QColor(Qt::black), 0.45);
  colors->colorWhite = QColor(Qt::white);
}

void applyDarkSemanticColors(ThemeColors* colors, const ThemeConfig& config) {
  const QColor primaryBase =
      validOr(config.primary, validOr(config.blue, QColor(QStringLiteral("#1677ff"))));
  const QColor successBase =
      validOr(config.success, validOr(config.green, QColor(QStringLiteral("#52c41a"))));
  const QColor errorBase = validOr(config.error, QColor(QStringLiteral("#ff4d4f")));
  const QColor warningBase = validOr(config.warning, QColor(QStringLiteral("#faad14")));
  const QColor infoBase = validOr(config.info, primaryBase);
  const QColor linkBase = validOr(config.link, primaryBase);
  const QColor darkMixBackground(QStringLiteral("#141414"));
  const QColor bgBase(QStringLiteral("#000000"));
  const QColor textBase(Qt::white);

  const QVector<QColor> primary = generateMappedPalette(primaryBase, true, darkMixBackground);
  const QVector<QColor> success = generateMappedPalette(successBase, true, darkMixBackground);
  const QVector<QColor> error = generateMappedPalette(errorBase, true, darkMixBackground);
  const QVector<QColor> warning = generateMappedPalette(warningBase, true, darkMixBackground);
  const QVector<QColor> info = generateMappedPalette(infoBase, true, darkMixBackground);
  const QVector<QColor> link = generateMappedPalette(linkBase, true, darkMixBackground);

  colors->colorBgBase = bgBase;
  colors->colorTextBase = textBase;
  colors->colorText = alphaColor(textBase, 0.85);
  colors->colorTextSecondary = alphaColor(textBase, 0.65);
  colors->colorTextTertiary = alphaColor(textBase, 0.45);
  colors->colorTextQuaternary = alphaColor(textBase, 0.25);
  colors->colorTextDisabled = colors->colorTextQuaternary;
  colors->colorTextPlaceholder = colors->colorTextQuaternary;
  colors->colorTextLightSolid = QColor(Qt::white);

  colors->colorFill = alphaColor(textBase, 0.18);
  colors->colorFillSecondary = alphaColor(textBase, 0.12);
  colors->colorFillTertiary = alphaColor(textBase, 0.08);
  colors->colorFillQuaternary = alphaColor(textBase, 0.04);
  colors->colorFillAlter = colors->colorFillQuaternary;

  colors->colorBgSolid = alphaColor(textBase, 0.95);
  colors->colorBgSolidHover = alphaColor(textBase, 1.0);
  colors->colorBgSolidActive = alphaColor(textBase, 0.9);
  // Ant Design dark neutrals are derived by lifting the black base toward gray,
  // not by darkening it further.
  colors->colorBgLayout = solidColor(bgBase, 0.0, true);
  colors->colorBgContainer = solidColor(bgBase, 8.0, true);
  colors->colorBgContainerDisabled = colors->colorFillTertiary;
  colors->colorBgElevated = solidColor(bgBase, 12.0, true);
  colors->colorBgSpotlight = solidColor(bgBase, 26.0, true);
  colors->colorBgBlur = alphaColor(textBase, 0.04);

  colors->colorBorder = solidColor(bgBase, 26.0, true);
  colors->colorBorderDisabled = colors->colorBorder;
  colors->colorBorderSecondary = solidColor(bgBase, 19.0, true);

  colors->colorPrimaryBg = tone(primary, 3, QColor(QStringLiteral("#15325b")));
  colors->colorPrimaryBgHover = tone(primary, 4, QColor(QStringLiteral("#15417e")));
  colors->colorPrimaryBorder = tone(primary, 3, QColor(QStringLiteral("#15325b")));
  colors->colorPrimaryBorderHover = tone(primary, 4, QColor(QStringLiteral("#15417e")));
  colors->colorPrimaryHover = tone(primary, 5, QColor(QStringLiteral("#3c89e8")));
  colors->colorPrimary = tone(primary, 6, primaryBase);
  colors->colorPrimaryActive = tone(primary, 7, QColor(QStringLiteral("#1554ad")));
  colors->colorPrimarySolid = colors->colorPrimary;
  colors->colorPrimarySolidHover = colors->colorPrimaryHover;
  colors->colorPrimarySolidActive = colors->colorPrimaryActive;
  colors->colorPrimarySolidText = colors->colorTextLightSolid;
  colors->colorPrimaryTextHover = colors->colorPrimaryHover;
  colors->colorPrimaryText = colors->colorPrimary;
  colors->colorPrimaryTextActive = colors->colorPrimaryActive;

  colors->colorSuccessBg = tone(success, 1, QColor(QStringLiteral("#162312")));
  colors->colorSuccessBgHover = tone(success, 2, QColor(QStringLiteral("#1d3712")));
  colors->colorSuccessBorder = tone(success, 3, QColor(QStringLiteral("#274916")));
  colors->colorSuccessBorderHover = tone(success, 4, QColor(QStringLiteral("#306317")));
  colors->colorSuccessHover = tone(success, 4, QColor(QStringLiteral("#306317")));
  colors->colorSuccess = tone(success, 6, successBase);
  colors->colorSuccessActive = tone(success, 7, QColor(QStringLiteral("#3c8618")));
  colors->colorSuccessSolid = colors->colorSuccess;
  colors->colorSuccessSolidHover = colors->colorSuccessHover;
  colors->colorSuccessSolidActive = colors->colorSuccessActive;
  colors->colorSuccessSolidText = colors->colorTextLightSolid;
  colors->colorSuccessTextHover = tone(success, 8, QColor(QStringLiteral("#6abe39")));
  colors->colorSuccessText = colors->colorSuccess;
  colors->colorSuccessTextActive = colors->colorSuccessActive;

  colors->colorErrorBg = tone(error, 1, QColor(QStringLiteral("#2a1215")));
  colors->colorErrorBgHover = tone(error, 2, QColor(QStringLiteral("#431418")));
  colors->colorErrorBgActive = tone(error, 3, QColor(QStringLiteral("#58181c")));
  colors->colorErrorBgFilledHover =
      mixColor(colors->colorErrorBg, colors->colorErrorBgActive, 50.0);
  colors->colorErrorBorder = tone(error, 3, QColor(QStringLiteral("#58181c")));
  colors->colorErrorBorderHover = tone(error, 4, QColor(QStringLiteral("#791a1f")));
  colors->colorErrorHover = tone(error, 5, QColor(QStringLiteral("#e86b6b")));
  colors->colorError = tone(error, 6, errorBase);
  colors->colorErrorActive = tone(error, 7, QColor(QStringLiteral("#ad393a")));
  colors->colorErrorSolid = colors->colorError;
  colors->colorErrorSolidHover = colors->colorErrorHover;
  colors->colorErrorSolidActive = colors->colorErrorActive;
  colors->colorErrorSolidText = colors->colorTextLightSolid;
  colors->colorErrorTextHover = tone(error, 8, QColor(QStringLiteral("#e86b6b")));
  colors->colorErrorText = colors->colorError;
  colors->colorErrorTextActive = colors->colorErrorActive;

  colors->colorWarningBg = tone(warning, 1, QColor(QStringLiteral("#2b2111")));
  colors->colorWarningBgHover = tone(warning, 2, QColor(QStringLiteral("#443111")));
  colors->colorWarningBorder = tone(warning, 3, QColor(QStringLiteral("#594214")));
  colors->colorWarningBorderHover = tone(warning, 4, QColor(QStringLiteral("#7c5914")));
  colors->colorWarningHover = tone(warning, 4, QColor(QStringLiteral("#7c5914")));
  colors->colorWarning = tone(warning, 6, warningBase);
  colors->colorWarningActive = tone(warning, 7, QColor(QStringLiteral("#ad7412")));
  colors->colorWarningSolid = colors->colorWarning;
  colors->colorWarningSolidHover = colors->colorWarningHover;
  colors->colorWarningSolidActive = colors->colorWarningActive;
  colors->colorWarningSolidText = colors->colorTextLightSolid;
  colors->colorWarningTextHover = tone(warning, 8, QColor(QStringLiteral("#e8b339")));
  colors->colorWarningText = colors->colorWarning;
  colors->colorWarningTextActive = colors->colorWarningActive;

  colors->colorInfoBg = tone(info, 1, colors->colorPrimaryBg);
  colors->colorInfoBgHover = tone(info, 2, colors->colorPrimaryBgHover);
  colors->colorInfoBorder = tone(info, 3, colors->colorPrimaryBorder);
  colors->colorInfoBorderHover = tone(info, 4, colors->colorPrimaryBorderHover);
  colors->colorInfoHover = tone(info, 4, colors->colorPrimaryBorderHover);
  colors->colorInfo = tone(info, 6, infoBase);
  colors->colorInfoActive = tone(info, 7, colors->colorPrimaryActive);
  colors->colorInfoSolid = colors->colorInfo;
  colors->colorInfoSolidHover = colors->colorInfoHover;
  colors->colorInfoSolidActive = colors->colorInfoActive;
  colors->colorInfoSolidText = colors->colorTextLightSolid;
  colors->colorInfoTextHover = tone(info, 8, colors->colorPrimaryHover);
  colors->colorInfoText = colors->colorInfo;
  colors->colorInfoTextActive = colors->colorInfoActive;

  colors->colorLinkHover = tone(link, 4, colors->colorPrimaryBorderHover);
  colors->colorLink = tone(link, 6, colors->colorPrimary);
  colors->colorLinkActive = tone(link, 7, colors->colorPrimaryActive);
  colors->colorBgMask = alphaColor(QColor(Qt::black), 0.45);
  colors->colorWhite = QColor(Qt::white);
}

#define ADQT_EQ_FIELD(name) &&lhs.name == rhs.name

}  // namespace

ThemeConfig mergeThemeConfig(const ThemeConfig& base, const ThemeOverride& overrideValue) {
  return mergeThemeConfigImpl(base, overrideValue);
}

bool isEmptyThemeOverride(const ThemeOverride& overrideValue) {
  return isEmptyThemeOverrideImpl(overrideValue);
}

ThemeConfig defaultThemeConfig(ThemeScheme scheme, ThemeDensity density) {
  ThemeConfig config;
  config.scheme = scheme;
  config.density = density;
  config.blue = QColor(QStringLiteral("#1677ff"));
  config.purple = QColor(QStringLiteral("#722ed1"));
  config.cyan = QColor(QStringLiteral("#13c2c2"));
  config.green = QColor(QStringLiteral("#52c41a"));
  config.magenta = QColor(QStringLiteral("#eb2f96"));
  config.pink = QColor(QStringLiteral("#eb2f96"));
  config.red = QColor(QStringLiteral("#f5222d"));
  config.orange = QColor(QStringLiteral("#fa8c16"));
  config.yellow = QColor(QStringLiteral("#fadb14"));
  config.volcano = QColor(QStringLiteral("#fa541c"));
  config.geekblue = QColor(QStringLiteral("#2f54eb"));
  config.gold = QColor(QStringLiteral("#faad14"));
  config.lime = QColor(QStringLiteral("#a0d911"));

  config.primary = QColor(QStringLiteral("#1677ff"));
  config.success = QColor(QStringLiteral("#52c41a"));
  config.warning = QColor(QStringLiteral("#faad14"));
  config.error = QColor(QStringLiteral("#ff4d4f"));
  config.info = config.primary;
  config.link = QColor();

  config.fontSize = 14.0;
  config.lineWidth = 1.0;
  config.borderRadius = 6.0;
  config.sizeUnit = 4.0;
  config.sizeStep = density == ThemeDensity::Compact ? 3.0 : 4.0;
  config.sizePopupArrow = 16.0;
  config.controlHeight = density == ThemeDensity::Compact ? 28.0 : 32.0;
  config.zIndexPopupBase = 1000.0;
  config.opacityImage = 1.0;
  config.wireframe = false;
  config.motion = true;
  return config;
}

ThemeSeedToken defaultSeedToken(ThemeScheme scheme, ThemeDensity density) {
  return defaultThemeConfig(scheme, density);
}

AdTheme makeTheme(ThemeScheme scheme, ThemeDensity density) {
  return makeTheme(defaultThemeConfig(scheme, density));
}

AdTheme makeTheme(const ThemeConfig& config) {
  AdTheme theme;
  theme.scheme = config.scheme;
  theme.density = config.density;
  theme.appFont = config.appFont;
  theme.codeFont = config.codeFont;
  theme.wireframe = config.wireframe;

  copyAccents(&theme.accents, config);
  applyDensityMetrics(&theme.metrics, config);
  applyMotion(&theme.motion, config);

  if (theme.scheme == ThemeScheme::Dark) {
    applyDarkSemanticColors(&theme.palette, config);
  } else {
    applyLightSemanticColors(&theme.palette, config);
  }
  theme.semantic = makeSemanticPalette(theme.palette);

  return theme;
}

ThemeMapToken resolveThemeValues(const ThemeConfig& config) {
  ThemeValues values;
  flattenTheme(&values, makeTheme(config));
  return values;
}

ResolvedTheme makeResolvedTheme(const ThemeConfig& config) {
  ResolvedTheme resolved;
  resolved.config = config;
  resolved.theme = makeTheme(config);
  resolved.semantic = resolved.theme.semantic;
  flattenTheme(&resolved.values, resolved.theme);
  resolved.palette = buildPalette(resolved.theme);
  return resolved;
}

ResolvedTheme makeResolvedTheme(const AdTheme& theme) {
  ResolvedTheme resolved;
  resolved.config = themeConfigFromTheme(theme);
  resolved.theme = theme;
  resolved.semantic = theme.semantic;
  flattenTheme(&resolved.values, theme);
  resolved.palette = buildPalette(theme);
  return resolved;
}

bool operator==(const ThemeAccents& lhs, const ThemeAccents& rhs) {
  return true
#define ADQT_EQ_ACCENT(name) ADQT_EQ_FIELD(name)
      ADQT_THEME_ACCENT_FIELDS(ADQT_EQ_ACCENT)
#undef ADQT_EQ_ACCENT
          ;
}

bool operator==(const ThemeColors& lhs, const ThemeColors& rhs) {
  return true
#define ADQT_EQ_COLOR(name) ADQT_EQ_FIELD(name)
      ADQT_THEME_COLOR_FIELDS(ADQT_EQ_COLOR)
#undef ADQT_EQ_COLOR
          ;
}

bool operator==(const ThemeSemanticPalette& lhs, const ThemeSemanticPalette& rhs) {
  return true
#define ADQT_EQ_SEMANTIC(name) ADQT_EQ_FIELD(name)
      ADQT_THEME_SEMANTIC_FIELDS(ADQT_EQ_SEMANTIC)
#undef ADQT_EQ_SEMANTIC
          ;
}

bool operator==(const ThemeMetrics& lhs, const ThemeMetrics& rhs) {
  return true
#define ADQT_EQ_DOUBLE(name) ADQT_EQ_FIELD(name)
      ADQT_THEME_DOUBLE_FIELDS(ADQT_EQ_DOUBLE)
#undef ADQT_EQ_DOUBLE
#define ADQT_EQ_METRIC_INT(name) ADQT_EQ_FIELD(name)
          ADQT_THEME_METRIC_INT_FIELDS(ADQT_EQ_METRIC_INT)
#undef ADQT_EQ_METRIC_INT
              ;
}

bool operator==(const ThemeMotion& lhs, const ThemeMotion& rhs) {
  return lhs.motion == rhs.motion
#define ADQT_EQ_MOTION_INT(name) ADQT_EQ_FIELD(name)
                           ADQT_THEME_MOTION_INT_FIELDS(ADQT_EQ_MOTION_INT)
#undef ADQT_EQ_MOTION_INT
#define ADQT_EQ_EASING(name) ADQT_EQ_FIELD(name)
                               ADQT_THEME_EASING_FIELDS(ADQT_EQ_EASING)
#undef ADQT_EQ_EASING
      ;
}

bool operator==(const ThemeConfig& lhs, const ThemeConfig& rhs) {
  return lhs.scheme == rhs.scheme &&
         lhs.density ==
             rhs.density
#define ADQT_EQ_CONFIG_ACCENT(name) ADQT_EQ_FIELD(name)
                 ADQT_THEME_ACCENT_FIELDS(ADQT_EQ_CONFIG_ACCENT)
#undef ADQT_EQ_CONFIG_ACCENT
                     ADQT_EQ_FIELD(primary) ADQT_EQ_FIELD(success) ADQT_EQ_FIELD(warning)
                         ADQT_EQ_FIELD(error) ADQT_EQ_FIELD(info) ADQT_EQ_FIELD(link)
                             ADQT_EQ_FIELD(appFont) ADQT_EQ_FIELD(codeFont) ADQT_EQ_FIELD(fontSize)
                                 ADQT_EQ_FIELD(lineWidth) ADQT_EQ_FIELD(borderRadius)
                                     ADQT_EQ_FIELD(sizeUnit) ADQT_EQ_FIELD(sizeStep)
                                         ADQT_EQ_FIELD(sizePopupArrow) ADQT_EQ_FIELD(controlHeight)
                                             ADQT_EQ_FIELD(zIndexPopupBase)
                                                 ADQT_EQ_FIELD(opacityImage)
                                                     ADQT_EQ_FIELD(wireframe) ADQT_EQ_FIELD(motion);
}

bool operator==(const ThemeOverride& lhs, const ThemeOverride& rhs) {
  return lhs.scheme == rhs.scheme &&
         lhs.density ==
             rhs.density
#define ADQT_EQ_OVERRIDE_ACCENT(name) ADQT_EQ_FIELD(name)
                 ADQT_THEME_ACCENT_FIELDS(ADQT_EQ_OVERRIDE_ACCENT)
#undef ADQT_EQ_OVERRIDE_ACCENT
                     ADQT_EQ_FIELD(primary) ADQT_EQ_FIELD(success) ADQT_EQ_FIELD(warning)
                         ADQT_EQ_FIELD(error) ADQT_EQ_FIELD(info) ADQT_EQ_FIELD(link)
                             ADQT_EQ_FIELD(appFont) ADQT_EQ_FIELD(codeFont) ADQT_EQ_FIELD(fontSize)
                                 ADQT_EQ_FIELD(lineWidth) ADQT_EQ_FIELD(borderRadius)
                                     ADQT_EQ_FIELD(sizeUnit) ADQT_EQ_FIELD(sizeStep)
                                         ADQT_EQ_FIELD(sizePopupArrow) ADQT_EQ_FIELD(controlHeight)
                                             ADQT_EQ_FIELD(zIndexPopupBase)
                                                 ADQT_EQ_FIELD(opacityImage)
                                                     ADQT_EQ_FIELD(wireframe) ADQT_EQ_FIELD(motion);
}

bool operator==(const AdTheme& lhs, const AdTheme& rhs) {
  return lhs.scheme == rhs.scheme && lhs.density == rhs.density && lhs.accents == rhs.accents &&
         lhs.semantic == rhs.semantic && lhs.palette == rhs.palette && lhs.metrics == rhs.metrics &&
         lhs.motion == rhs.motion && lhs.appFont == rhs.appFont && lhs.codeFont == rhs.codeFont &&
         lhs.wireframe == rhs.wireframe;
}

bool operator==(const ThemeValues& lhs, const ThemeValues& rhs) {
  return lhs.scheme == rhs.scheme && lhs.density == rhs.density &&
         lhs.motion == rhs.motion
#define ADQT_EQ_VALUE_ACCENT(name) ADQT_EQ_FIELD(name)
                           ADQT_THEME_ACCENT_FIELDS(ADQT_EQ_VALUE_ACCENT)
#undef ADQT_EQ_VALUE_ACCENT
#define ADQT_EQ_VALUE_COLOR(name) ADQT_EQ_FIELD(name)
                               ADQT_THEME_COLOR_FIELDS(ADQT_EQ_VALUE_COLOR)
#undef ADQT_EQ_VALUE_COLOR
#define ADQT_EQ_VALUE_DOUBLE(name) ADQT_EQ_FIELD(name)
                                   ADQT_THEME_DOUBLE_FIELDS(ADQT_EQ_VALUE_DOUBLE)
#undef ADQT_EQ_VALUE_DOUBLE
#define ADQT_EQ_VALUE_INT(name) ADQT_EQ_FIELD(name)
                                       ADQT_THEME_INT_FIELDS(ADQT_EQ_VALUE_INT)
#undef ADQT_EQ_VALUE_INT
#define ADQT_EQ_VALUE_EASING(name) ADQT_EQ_FIELD(name)
                                           ADQT_THEME_EASING_FIELDS(ADQT_EQ_VALUE_EASING)
#undef ADQT_EQ_VALUE_EASING
                                               ADQT_EQ_FIELD(appFont) ADQT_EQ_FIELD(codeFont)
                                                   ADQT_EQ_FIELD(wireframe);
}

bool operator==(const ResolvedTheme& lhs, const ResolvedTheme& rhs) {
  return lhs.config == rhs.config && lhs.theme == rhs.theme && lhs.semantic == rhs.semantic &&
         lhs.values == rhs.values && lhs.palette == rhs.palette;
}

}  // namespace adqt::theme
