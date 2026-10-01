#pragma once

#include <QColor>
#include <QEasingCurve>
#include <QFont>
#include <QMetaType>
#include <QPalette>

#include <optional>

namespace adqt::theme {

enum class ThemeScheme {
  Light,
  Dark,
};

enum class ThemeDensity {
  Comfortable,
  Compact,
};

using AdThemeColorScheme = ThemeScheme;
using AdThemeDensity = ThemeDensity;

#define ADQT_THEME_ACCENT_FIELDS(X) \
  X(blue)                           \
  X(purple)                         \
  X(cyan)                           \
  X(green)                          \
  X(magenta)                        \
  X(pink)                           \
  X(red)                            \
  X(orange)                         \
  X(yellow)                         \
  X(volcano)                        \
  X(geekblue)                       \
  X(gold)                           \
  X(lime)

#define ADQT_THEME_COLOR_FIELDS(X) \
  X(colorBgBase)                   \
  X(colorTextBase)                 \
  X(colorText)                     \
  X(colorTextSecondary)            \
  X(colorTextTertiary)             \
  X(colorTextQuaternary)           \
  X(colorTextDisabled)             \
  X(colorTextPlaceholder)          \
  X(colorTextLightSolid)           \
  X(colorFill)                     \
  X(colorFillAlter)                \
  X(colorFillSecondary)            \
  X(colorFillTertiary)             \
  X(colorFillQuaternary)           \
  X(colorBgSolid)                  \
  X(colorBgSolidHover)             \
  X(colorBgSolidActive)            \
  X(colorBgLayout)                 \
  X(colorBgContainer)              \
  X(colorBgContainerDisabled)      \
  X(colorBgElevated)               \
  X(colorBgSpotlight)              \
  X(colorBgBlur)                   \
  X(colorBorder)                   \
  X(colorBorderDisabled)           \
  X(colorBorderSecondary)          \
  X(colorPrimaryBg)                \
  X(colorPrimaryBgHover)           \
  X(colorPrimaryBorder)            \
  X(colorPrimaryBorderHover)       \
  X(colorPrimaryHover)             \
  X(colorPrimary)                  \
  X(colorPrimaryActive)            \
  X(colorPrimarySolid)             \
  X(colorPrimarySolidHover)        \
  X(colorPrimarySolidActive)       \
  X(colorPrimarySolidText)         \
  X(colorPrimaryTextHover)         \
  X(colorPrimaryText)              \
  X(colorPrimaryTextActive)        \
  X(colorSuccessBg)                \
  X(colorSuccessBgHover)           \
  X(colorSuccessBorder)            \
  X(colorSuccessBorderHover)       \
  X(colorSuccessHover)             \
  X(colorSuccess)                  \
  X(colorSuccessActive)            \
  X(colorSuccessSolid)             \
  X(colorSuccessSolidHover)        \
  X(colorSuccessSolidActive)       \
  X(colorSuccessSolidText)         \
  X(colorSuccessTextHover)         \
  X(colorSuccessText)              \
  X(colorSuccessTextActive)        \
  X(colorErrorBg)                  \
  X(colorErrorBgHover)             \
  X(colorErrorBgFilledHover)       \
  X(colorErrorBgActive)            \
  X(colorErrorBorder)              \
  X(colorErrorBorderHover)         \
  X(colorErrorHover)               \
  X(colorError)                    \
  X(colorErrorActive)              \
  X(colorErrorSolid)               \
  X(colorErrorSolidHover)          \
  X(colorErrorSolidActive)         \
  X(colorErrorSolidText)           \
  X(colorErrorTextHover)           \
  X(colorErrorText)                \
  X(colorErrorTextActive)          \
  X(colorWarningBg)                \
  X(colorWarningBgHover)           \
  X(colorWarningBorder)            \
  X(colorWarningBorderHover)       \
  X(colorWarningHover)             \
  X(colorWarning)                  \
  X(colorWarningActive)            \
  X(colorWarningSolid)             \
  X(colorWarningSolidHover)        \
  X(colorWarningSolidActive)       \
  X(colorWarningSolidText)         \
  X(colorWarningTextHover)         \
  X(colorWarningText)              \
  X(colorWarningTextActive)        \
  X(colorInfoBg)                   \
  X(colorInfoBgHover)              \
  X(colorInfoBorder)               \
  X(colorInfoBorderHover)          \
  X(colorInfoHover)                \
  X(colorInfo)                     \
  X(colorInfoActive)               \
  X(colorInfoSolid)                \
  X(colorInfoSolidHover)           \
  X(colorInfoSolidActive)          \
  X(colorInfoSolidText)            \
  X(colorInfoTextHover)            \
  X(colorInfoText)                 \
  X(colorInfoTextActive)           \
  X(colorLinkHover)                \
  X(colorLink)                     \
  X(colorLinkActive)               \
  X(colorBgMask)                   \
  X(colorWhite)

#define ADQT_THEME_SEMANTIC_FIELDS(X) \
  X(window)                           \
  X(windowDisabled)                   \
  X(surface)                          \
  X(surfaceDisabled)                  \
  X(surfaceElevated)                  \
  X(surfaceSubtle)                    \
  X(surfaceSolid)                     \
  X(surfaceSolidHover)                \
  X(surfaceSolidActive)               \
  X(surfaceSpotlight)                 \
  X(mask)                             \
  X(fill)                             \
  X(fillSecondary)                    \
  X(fillTertiary)                     \
  X(fillQuaternary)                   \
  X(text)                             \
  X(textSecondary)                    \
  X(textTertiary)                     \
  X(textQuaternary)                   \
  X(textDisabled)                     \
  X(textPlaceholder)                  \
  X(textOnAccent)                     \
  X(border)                           \
  X(borderDisabled)                   \
  X(borderSecondary)                  \
  X(accent)                           \
  X(accentHover)                      \
  X(accentActive)                     \
  X(accentSubtle)                     \
  X(accentSubtleHover)                \
  X(accentBorder)                     \
  X(accentBorderHover)                \
  X(accentSolid)                      \
  X(accentSolidHover)                 \
  X(accentSolidActive)                \
  X(accentSolidText)                  \
  X(accentDisabled)                   \
  X(success)                          \
  X(successHover)                     \
  X(successActive)                    \
  X(successSubtle)                    \
  X(successBorder)                    \
  X(warning)                          \
  X(warningHover)                     \
  X(warningActive)                    \
  X(warningSubtle)                    \
  X(warningBorder)                    \
  X(error)                            \
  X(errorHover)                       \
  X(errorActive)                      \
  X(errorSubtle)                      \
  X(errorBorder)                      \
  X(info)                             \
  X(infoHover)                        \
  X(infoActive)                       \
  X(infoSubtle)                       \
  X(infoBorder)                       \
  X(link)                             \
  X(linkHover)                        \
  X(linkActive)                       \
  X(white)

#define ADQT_THEME_DOUBLE_FIELDS(X) \
  X(sizeXXL)                        \
  X(sizeXL)                         \
  X(sizeLG)                         \
  X(sizeMD)                         \
  X(sizeMS)                         \
  X(size)                           \
  X(sizeSM)                         \
  X(sizeXS)                         \
  X(sizeXXS)                        \
  X(fontSizeSM)                     \
  X(fontSize)                       \
  X(fontSizeLG)                     \
  X(fontSizeXL)                     \
  X(fontSizeHeading1)               \
  X(fontSizeHeading2)               \
  X(fontSizeHeading3)               \
  X(fontSizeHeading4)               \
  X(fontSizeHeading5)               \
  X(lineHeight)                     \
  X(lineHeightLG)                   \
  X(lineHeightSM)                   \
  X(lineHeightHeading1)             \
  X(lineHeightHeading2)             \
  X(lineHeightHeading3)             \
  X(lineHeightHeading4)             \
  X(lineHeightHeading5)             \
  X(fontHeight)                     \
  X(fontHeightLG)                   \
  X(fontHeightSM)                   \
  X(lineWidth)                      \
  X(lineWidthBold)                  \
  X(borderRadius)                   \
  X(borderRadiusXS)                 \
  X(borderRadiusSM)                 \
  X(borderRadiusLG)                 \
  X(borderRadiusOuter)              \
  X(controlHeight)                  \
  X(controlHeightSM)                \
  X(controlHeightXS)                \
  X(controlHeightLG)                \
  X(sizeUnit)                       \
  X(sizeStep)                       \
  X(opacityImage)                   \
  X(backgroundOpacity)

#define ADQT_THEME_INT_FIELDS(X) \
  X(motionDurationFast)          \
  X(motionDurationMid)           \
  X(motionDurationSlow)          \
  X(timingFrameIntervalMs)       \
  X(timingSpinnerCycleMs)        \
  X(timingWaveDurationMs)        \
  X(timingMenuOpenDelayMs)       \
  X(timingMenuCloseDelayMs)      \
  X(timingLoadingDelayMs)        \
  X(popupArrowSize)              \
  X(popupZIndexBase)

#define ADQT_THEME_MOTION_INT_FIELDS(X) \
  X(motionDurationFast)                 \
  X(motionDurationMid)                  \
  X(motionDurationSlow)                 \
  X(timingFrameIntervalMs)              \
  X(timingSpinnerCycleMs)               \
  X(timingWaveDurationMs)               \
  X(timingMenuOpenDelayMs)              \
  X(timingMenuCloseDelayMs)             \
  X(timingLoadingDelayMs)

#define ADQT_THEME_METRIC_INT_FIELDS(X) \
  X(popupArrowSize)                     \
  X(popupZIndexBase)

#define ADQT_THEME_EASING_FIELDS(X) \
  X(motionEaseOutCirc)              \
  X(motionEaseInOutCirc)            \
  X(motionEaseOut)                  \
  X(motionEaseInOut)                \
  X(motionEaseOutBack)              \
  X(motionEaseInBack)               \
  X(motionEaseInQuint)              \
  X(motionEaseOutQuint)

struct ThemeAccents {
#define ADQT_THEME_ACCENT_DECL(name) QColor name;
  ADQT_THEME_ACCENT_FIELDS(ADQT_THEME_ACCENT_DECL)
#undef ADQT_THEME_ACCENT_DECL
};

struct ThemeColors {
#define ADQT_THEME_COLOR_DECL(name) QColor name;
  ADQT_THEME_COLOR_FIELDS(ADQT_THEME_COLOR_DECL)
#undef ADQT_THEME_COLOR_DECL
};

struct ThemeSemanticPalette {
#define ADQT_THEME_SEMANTIC_DECL(name) QColor name;
  ADQT_THEME_SEMANTIC_FIELDS(ADQT_THEME_SEMANTIC_DECL)
#undef ADQT_THEME_SEMANTIC_DECL
};

struct ThemeMetrics {
#define ADQT_THEME_DOUBLE_DECL(name) double name = 0.0;
  ADQT_THEME_DOUBLE_FIELDS(ADQT_THEME_DOUBLE_DECL)
#undef ADQT_THEME_DOUBLE_DECL

#define ADQT_THEME_METRIC_INT_DECL(name) int name = 0;
  ADQT_THEME_METRIC_INT_FIELDS(ADQT_THEME_METRIC_INT_DECL)
#undef ADQT_THEME_METRIC_INT_DECL
};

struct ThemeMotion {
  bool motion = true;

#define ADQT_THEME_MOTION_INT_DECL(name) int name = 0;
  ADQT_THEME_MOTION_INT_FIELDS(ADQT_THEME_MOTION_INT_DECL)
#undef ADQT_THEME_MOTION_INT_DECL

#define ADQT_THEME_EASING_DECL(name) QEasingCurve name;
  ADQT_THEME_EASING_FIELDS(ADQT_THEME_EASING_DECL)
#undef ADQT_THEME_EASING_DECL
};

struct ThemeConfig : public ThemeAccents {
  ThemeScheme scheme = ThemeScheme::Light;
  ThemeDensity density = ThemeDensity::Comfortable;

  QColor primary;
  QColor success;
  QColor warning;
  QColor error;
  QColor info;
  QColor link;

  QFont appFont;
  QFont codeFont;

  double fontSize = 14.0;
  double lineWidth = 1.0;
  double borderRadius = 6.0;
  double sizeUnit = 4.0;
  double sizeStep = 4.0;
  double sizePopupArrow = 16.0;
  double controlHeight = 32.0;
  double zIndexPopupBase = 1000.0;
  double opacityImage = 1.0;
  double backgroundOpacity = 1.0;

  bool wireframe = false;
  bool motion = true;
};

struct ThemeOverride {
  std::optional<ThemeScheme> scheme;
  std::optional<ThemeDensity> density;

#define ADQT_THEME_OVERRIDE_ACCENT_DECL(name) std::optional<QColor> name;
  ADQT_THEME_ACCENT_FIELDS(ADQT_THEME_OVERRIDE_ACCENT_DECL)
#undef ADQT_THEME_OVERRIDE_ACCENT_DECL

  std::optional<QColor> primary;
  std::optional<QColor> success;
  std::optional<QColor> warning;
  std::optional<QColor> error;
  std::optional<QColor> info;
  std::optional<QColor> link;

  std::optional<QFont> appFont;
  std::optional<QFont> codeFont;

  std::optional<double> fontSize;
  std::optional<double> lineWidth;
  std::optional<double> borderRadius;
  std::optional<double> sizeUnit;
  std::optional<double> sizeStep;
  std::optional<double> sizePopupArrow;
  std::optional<double> controlHeight;
  std::optional<double> zIndexPopupBase;
  std::optional<double> opacityImage;
  std::optional<double> backgroundOpacity;

  std::optional<bool> wireframe;
  std::optional<bool> motion;
};

struct AdTheme {
  ThemeScheme scheme = ThemeScheme::Light;
  ThemeDensity density = ThemeDensity::Comfortable;
  ThemeAccents accents;
  ThemeSemanticPalette semantic;
  ThemeColors palette;
  ThemeMetrics metrics;
  ThemeMotion motion;
  QFont appFont;
  QFont codeFont;
  bool wireframe = false;
};

struct ThemeValues : public ThemeAccents,
                     public ThemeColors,
                     public ThemeMetrics,
                     public ThemeMotion {
  ThemeScheme scheme = ThemeScheme::Light;
  ThemeDensity density = ThemeDensity::Comfortable;
  QFont appFont;
  QFont codeFont;
  bool wireframe = false;
};

using ThemeSeedToken = ThemeConfig;
using ThemeMapToken = ThemeValues;
using AdThemeAccents = ThemeAccents;
using AdThemePalette = ThemeColors;
using AdThemeMetrics = ThemeMetrics;
using AdThemeMotion = ThemeMotion;

struct ResolvedTheme {
  ThemeConfig config;
  AdTheme theme;
  ThemeSemanticPalette semantic;
  ThemeValues values;
  QPalette palette;
};

ThemeConfig mergeThemeConfig(const ThemeConfig& base, const ThemeOverride& overrideValue);
bool isEmptyThemeOverride(const ThemeOverride& overrideValue);

ThemeConfig defaultThemeConfig(ThemeScheme scheme = ThemeScheme::Light,
                               ThemeDensity density = ThemeDensity::Comfortable);
ThemeSeedToken defaultSeedToken(ThemeScheme scheme = ThemeScheme::Light,
                                ThemeDensity density = ThemeDensity::Comfortable);

AdTheme makeTheme(ThemeScheme scheme = ThemeScheme::Light,
                  ThemeDensity density = ThemeDensity::Comfortable);
AdTheme makeTheme(const ThemeConfig& config);
ThemeMapToken resolveThemeValues(const ThemeConfig& config);

ResolvedTheme makeResolvedTheme(const ThemeConfig& config);
ResolvedTheme makeResolvedTheme(const AdTheme& theme);

bool operator==(const ThemeAccents& lhs, const ThemeAccents& rhs);
bool operator==(const ThemeColors& lhs, const ThemeColors& rhs);
bool operator==(const ThemeSemanticPalette& lhs, const ThemeSemanticPalette& rhs);
bool operator==(const ThemeMetrics& lhs, const ThemeMetrics& rhs);
bool operator==(const ThemeMotion& lhs, const ThemeMotion& rhs);
bool operator==(const ThemeConfig& lhs, const ThemeConfig& rhs);
bool operator==(const ThemeOverride& lhs, const ThemeOverride& rhs);
bool operator==(const AdTheme& lhs, const AdTheme& rhs);
bool operator==(const ThemeValues& lhs, const ThemeValues& rhs);
bool operator==(const ResolvedTheme& lhs, const ResolvedTheme& rhs);

}  // namespace adqt::theme

Q_DECLARE_METATYPE(adqt::theme::ThemeAccents)
Q_DECLARE_METATYPE(adqt::theme::ThemeColors)
Q_DECLARE_METATYPE(adqt::theme::ThemeSemanticPalette)
Q_DECLARE_METATYPE(adqt::theme::ThemeMetrics)
Q_DECLARE_METATYPE(adqt::theme::ThemeMotion)
Q_DECLARE_METATYPE(adqt::theme::ThemeConfig)
Q_DECLARE_METATYPE(adqt::theme::ThemeOverride)
Q_DECLARE_METATYPE(adqt::theme::AdTheme)
Q_DECLARE_METATYPE(adqt::theme::ThemeValues)
Q_DECLARE_METATYPE(adqt::theme::ResolvedTheme)
