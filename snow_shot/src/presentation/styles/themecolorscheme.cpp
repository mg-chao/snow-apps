#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include "theme/theme_types.h"

#include <QtGlobal>

#include <algorithm>

namespace snow_shot::presentation::styles {
namespace {
constexpr int DEFAULT_CONTROL_PADDING_HORIZONTAL = 12;
constexpr int DEFAULT_CONTROL_PADDING_HORIZONTAL_SM = 8;

ThemeAppearance toThemeAppearance(adqt::theme::ThemeScheme scheme) {
    return scheme == adqt::theme::ThemeScheme::Dark ? ThemeAppearance::Dark
                                                    : ThemeAppearance::Light;
}

ThemePreset toThemePreset(adqt::theme::ThemeDensity density) {
    return density == adqt::theme::ThemeDensity::Compact ? ThemePreset::Compact
                                                         : ThemePreset::Default;
}

adqt::theme::ThemeScheme toAdqtScheme(ThemeAppearance appearance) {
    return appearance == ThemeAppearance::Dark ? adqt::theme::ThemeScheme::Dark
                                               : adqt::theme::ThemeScheme::Light;
}

adqt::theme::ThemeDensity toAdqtDensity(ThemePreset preset) {
    return preset == ThemePreset::Compact ? adqt::theme::ThemeDensity::Compact
                                          : adqt::theme::ThemeDensity::Comfortable;
}

int roundMetric(double value) {
    return qRound(value);
}

int roundLineWidth(double value) {
    return std::max(1, roundMetric(value));
}

ThemePresetColorMap normalizePresetColorMap(const ThemePresetColorMap& presetColors) {
    ThemePresetColorMap normalized;
    for (auto it = presetColors.cbegin(); it != presetColors.cend(); ++it) {
        normalized.insert(it.key().trimmed().toLower(), it.value());
    }
    return normalized;
}

QColor presetColorValue(const ThemePresetColorMap& normalizedPresetColors, const QString& key) {
    const auto it = normalizedPresetColors.constFind(key);
    return it != normalizedPresetColors.cend() ? it.value() : QColor();
}

void applyPresetColors(adqt::theme::ThemeConfig* target, const ThemePresetColorMap& presetColors) {
    if (target == nullptr) {
        return;
    }

    const ThemePresetColorMap normalizedPresetColors = normalizePresetColorMap(presetColors);

    const QColor blue = presetColorValue(normalizedPresetColors, QStringLiteral("blue"));
    const QColor purple = presetColorValue(normalizedPresetColors, QStringLiteral("purple"));
    const QColor cyan = presetColorValue(normalizedPresetColors, QStringLiteral("cyan"));
    const QColor green = presetColorValue(normalizedPresetColors, QStringLiteral("green"));
    const QColor magenta = presetColorValue(normalizedPresetColors, QStringLiteral("magenta"));
    const QColor pink = presetColorValue(normalizedPresetColors, QStringLiteral("pink"));
    const QColor red = presetColorValue(normalizedPresetColors, QStringLiteral("red"));
    const QColor orange = presetColorValue(normalizedPresetColors, QStringLiteral("orange"));
    const QColor yellow = presetColorValue(normalizedPresetColors, QStringLiteral("yellow"));
    const QColor volcano = presetColorValue(normalizedPresetColors, QStringLiteral("volcano"));
    const QColor geekblue = presetColorValue(normalizedPresetColors, QStringLiteral("geekblue"));
    const QColor gold = presetColorValue(normalizedPresetColors, QStringLiteral("gold"));
    const QColor lime = presetColorValue(normalizedPresetColors, QStringLiteral("lime"));

    if (blue.isValid()) {
        target->blue = blue;
    }
    if (purple.isValid()) {
        target->purple = purple;
    }
    if (cyan.isValid()) {
        target->cyan = cyan;
    }
    if (green.isValid()) {
        target->green = green;
    }
    if (magenta.isValid()) {
        target->magenta = magenta;
    }
    if (pink.isValid()) {
        target->pink = pink;
    }
    if (red.isValid()) {
        target->red = red;
    }
    if (orange.isValid()) {
        target->orange = orange;
    }
    if (yellow.isValid()) {
        target->yellow = yellow;
    }
    if (volcano.isValid()) {
        target->volcano = volcano;
    }
    if (geekblue.isValid()) {
        target->geekblue = geekblue;
    }
    if (gold.isValid()) {
        target->gold = gold;
    }
    if (lime.isValid()) {
        target->lime = lime;
    }
}

ThemePresetColorMap buildPresetColorMap(const adqt::theme::ThemeAccents& accents) {
    ThemePresetColorMap presetColors;
    presetColors.insert(QStringLiteral("blue"), accents.blue);
    presetColors.insert(QStringLiteral("purple"), accents.purple);
    presetColors.insert(QStringLiteral("cyan"), accents.cyan);
    presetColors.insert(QStringLiteral("green"), accents.green);
    presetColors.insert(QStringLiteral("magenta"), accents.magenta);
    presetColors.insert(QStringLiteral("pink"), accents.pink);
    presetColors.insert(QStringLiteral("red"), accents.red);
    presetColors.insert(QStringLiteral("orange"), accents.orange);
    presetColors.insert(QStringLiteral("yellow"), accents.yellow);
    presetColors.insert(QStringLiteral("volcano"), accents.volcano);
    presetColors.insert(QStringLiteral("geekblue"), accents.geekblue);
    presetColors.insert(QStringLiteral("gold"), accents.gold);
    presetColors.insert(QStringLiteral("lime"), accents.lime);
    return presetColors;
}

adqt::theme::ThemeConfig toAdqtThemeConfig(const ThemeStyleConfig& config) {
    adqt::theme::ThemeConfig adqtConfig = adqt::theme::defaultThemeConfig(
        toAdqtScheme(config.appearance), toAdqtDensity(config.preset));

    if (config.colorPrimary.isValid()) {
        adqtConfig.primary = config.colorPrimary;
    }
    if (config.colorSuccess.isValid()) {
        adqtConfig.success = config.colorSuccess;
    }
    if (config.colorWarning.isValid()) {
        adqtConfig.warning = config.colorWarning;
    }
    if (config.colorError.isValid()) {
        adqtConfig.error = config.colorError;
    }
    if (config.colorInfo.isValid()) {
        adqtConfig.info = config.colorInfo;
    }
    if (config.colorLink.isValid()) {
        adqtConfig.link = config.colorLink;
    }

    adqtConfig.fontSize = static_cast<double>(config.fontSize);
    adqtConfig.lineWidth = static_cast<double>(config.lineWidth);
    adqtConfig.borderRadius = static_cast<double>(config.borderRadius);
    adqtConfig.sizeUnit = static_cast<double>(config.sizeUnit);
    adqtConfig.sizeStep = static_cast<double>(config.sizeStep);
    adqtConfig.controlHeight = static_cast<double>(config.controlHeight);
    adqtConfig.motion = config.motionUnit > 0.0;

    applyPresetColors(&adqtConfig, config.presetColors);
    return adqtConfig;
}

ThemeStyleConfig toThemeStyleConfig(const adqt::theme::ResolvedTheme& resolvedTheme) {
    ThemeStyleConfig config;
    config.preset = toThemePreset(resolvedTheme.config.density);
    config.appearance = toThemeAppearance(resolvedTheme.config.scheme);
    config.colorPrimary = resolvedTheme.config.primary;
    config.colorSuccess = resolvedTheme.config.success;
    config.colorWarning = resolvedTheme.config.warning;
    config.colorError = resolvedTheme.config.error;
    config.colorInfo = resolvedTheme.config.info;
    config.colorLink = resolvedTheme.config.link;
    config.colorTextBase = resolvedTheme.values.colorTextBase;
    config.colorBgBase = resolvedTheme.values.colorBgBase;
    config.presetColors =
        buildPresetColorMap(static_cast<const adqt::theme::ThemeAccents&>(resolvedTheme.values));

    config.fontSize = roundMetric(resolvedTheme.config.fontSize);
    config.lineWidth = roundLineWidth(resolvedTheme.config.lineWidth);
    config.borderRadius = roundMetric(resolvedTheme.config.borderRadius);
    config.sizeUnit = roundMetric(resolvedTheme.config.sizeUnit);
    config.sizeStep = roundMetric(resolvedTheme.config.sizeStep);
    config.controlHeight = roundMetric(resolvedTheme.config.controlHeight);
    config.motionUnit = resolvedTheme.config.motion ? 0.1 : 0.0;
    return config;
}

ThemeMapColorToken buildColorMapToken(const adqt::theme::ResolvedTheme& resolvedTheme) {
    const adqt::theme::ThemeMapToken& values = resolvedTheme.values;

    ThemeMapColorToken mapToken;
    mapToken.colorBgBase = values.colorBgBase;
    mapToken.colorTextBase = values.colorTextBase;
    mapToken.colorText = values.colorText;
    mapToken.colorTextSecondary = values.colorTextSecondary;
    mapToken.colorTextTertiary = values.colorTextTertiary;
    mapToken.colorTextQuaternary = values.colorTextQuaternary;
    mapToken.colorFill = values.colorFill;
    mapToken.colorFillSecondary = values.colorFillSecondary;
    mapToken.colorFillTertiary = values.colorFillTertiary;
    mapToken.colorFillQuaternary = values.colorFillQuaternary;
    mapToken.colorBgSolid = values.colorBgSolid;
    mapToken.colorBgSolidHover = values.colorBgSolidHover;
    mapToken.colorBgSolidActive = values.colorBgSolidActive;
    mapToken.colorBgLayout = values.colorBgLayout;
    mapToken.colorBgContainer = values.colorBgContainer;
    mapToken.colorBgElevated = values.colorBgElevated;
    mapToken.colorBgSpotlight = values.colorBgSpotlight;
    mapToken.colorBgBlur = values.colorBgBlur;
    mapToken.colorBorder = values.colorBorder;
    mapToken.colorBorderDisabled = values.colorBorderDisabled;
    mapToken.colorBorderSecondary = values.colorBorderSecondary;
    mapToken.colorSplit = values.colorBorderSecondary;
    mapToken.colorPrimaryBg = values.colorPrimaryBg;
    mapToken.colorPrimaryBgHover = values.colorPrimaryBgHover;
    mapToken.colorPrimaryBorder = values.colorPrimaryBorder;
    mapToken.colorPrimaryBorderHover = values.colorPrimaryBorderHover;
    mapToken.colorPrimaryHover = values.colorPrimaryHover;
    mapToken.colorPrimary = values.colorPrimary;
    mapToken.colorPrimaryActive = values.colorPrimaryActive;
    mapToken.colorPrimaryTextHover = values.colorPrimaryTextHover;
    mapToken.colorPrimaryText = values.colorPrimaryText;
    mapToken.colorPrimaryTextActive = values.colorPrimaryTextActive;

    mapToken.colorSuccessBg = values.colorSuccessBg;
    mapToken.colorSuccessBgHover = values.colorSuccessBgHover;
    mapToken.colorSuccessBorder = values.colorSuccessBorder;
    mapToken.colorSuccessBorderHover = values.colorSuccessBorderHover;
    mapToken.colorSuccessHover = values.colorSuccessHover;
    mapToken.colorSuccess = values.colorSuccess;
    mapToken.colorSuccessActive = values.colorSuccessActive;
    mapToken.colorSuccessTextHover = values.colorSuccessTextHover;
    mapToken.colorSuccessText = values.colorSuccessText;
    mapToken.colorSuccessTextActive = values.colorSuccessTextActive;

    mapToken.colorErrorBg = values.colorErrorBg;
    mapToken.colorErrorBgHover = values.colorErrorBgHover;
    mapToken.colorErrorBgFilledHover = values.colorErrorBgFilledHover;
    mapToken.colorErrorBgActive = values.colorErrorBgActive;
    mapToken.colorErrorBorder = values.colorErrorBorder;
    mapToken.colorErrorBorderHover = values.colorErrorBorderHover;
    mapToken.colorErrorHover = values.colorErrorHover;
    mapToken.colorError = values.colorError;
    mapToken.colorErrorActive = values.colorErrorActive;
    mapToken.colorErrorTextHover = values.colorErrorTextHover;
    mapToken.colorErrorText = values.colorErrorText;
    mapToken.colorErrorTextActive = values.colorErrorTextActive;

    mapToken.colorWarningBg = values.colorWarningBg;
    mapToken.colorWarningBgHover = values.colorWarningBgHover;
    mapToken.colorWarningBorder = values.colorWarningBorder;
    mapToken.colorWarningBorderHover = values.colorWarningBorderHover;
    mapToken.colorWarningHover = values.colorWarningHover;
    mapToken.colorWarning = values.colorWarning;
    mapToken.colorWarningActive = values.colorWarningActive;
    mapToken.colorWarningTextHover = values.colorWarningTextHover;
    mapToken.colorWarningText = values.colorWarningText;
    mapToken.colorWarningTextActive = values.colorWarningTextActive;

    mapToken.colorInfoBg = values.colorInfoBg;
    mapToken.colorInfoBgHover = values.colorInfoBgHover;
    mapToken.colorInfoBorder = values.colorInfoBorder;
    mapToken.colorInfoBorderHover = values.colorInfoBorderHover;
    mapToken.colorInfoHover = values.colorInfoHover;
    mapToken.colorInfo = values.colorInfo;
    mapToken.colorInfoActive = values.colorInfoActive;
    mapToken.colorInfoTextHover = values.colorInfoTextHover;
    mapToken.colorInfoText = values.colorInfoText;
    mapToken.colorInfoTextActive = values.colorInfoTextActive;

    mapToken.colorLinkHover = values.colorLinkHover;
    mapToken.colorLink = values.colorLink;
    mapToken.colorLinkActive = values.colorLinkActive;
    mapToken.presetColorHover =
        buildPresetColorMap(static_cast<const adqt::theme::ThemeAccents&>(resolvedTheme.values));
    mapToken.colorBgMask = values.colorBgMask;
    mapToken.colorWhite = values.colorWhite;
    return mapToken;
}

ThemeMetricMapToken buildMetricMapToken(const adqt::theme::ResolvedTheme& resolvedTheme) {
    const adqt::theme::ThemeMapToken& values = resolvedTheme.values;

    ThemeMetricMapToken metricMap;

    metricMap.size.sizeXXL = roundMetric(values.sizeXXL);
    metricMap.size.sizeXL = roundMetric(values.sizeXL);
    metricMap.size.sizeLG = roundMetric(values.sizeLG);
    metricMap.size.sizeMD = roundMetric(values.sizeMD);
    metricMap.size.sizeMS = roundMetric(values.sizeMS);
    metricMap.size.size = roundMetric(values.size);
    metricMap.size.sizeSM = roundMetric(values.sizeSM);
    metricMap.size.sizeXS = roundMetric(values.sizeXS);
    metricMap.size.sizeXXS = roundMetric(values.sizeXXS);

    metricMap.font.fontSizeSM = roundMetric(values.fontSizeSM);
    metricMap.font.fontSize = roundMetric(values.fontSize);
    metricMap.font.fontSizeLG = roundMetric(values.fontSizeLG);
    metricMap.font.fontSizeXL = roundMetric(values.fontSizeXL);

    metricMap.font.fontSizeHeading1 = roundMetric(values.fontSizeHeading1);
    metricMap.font.fontSizeHeading2 = roundMetric(values.fontSizeHeading2);
    metricMap.font.fontSizeHeading3 = roundMetric(values.fontSizeHeading3);
    metricMap.font.fontSizeHeading4 = roundMetric(values.fontSizeHeading4);
    metricMap.font.fontSizeHeading5 = roundMetric(values.fontSizeHeading5);

    metricMap.font.lineHeight = values.lineHeight;
    metricMap.font.lineHeightLG = values.lineHeightLG;
    metricMap.font.lineHeightSM = values.lineHeightSM;

    metricMap.font.fontHeight = roundMetric(values.fontHeight);
    metricMap.font.fontHeightLG = roundMetric(values.fontHeightLG);
    metricMap.font.fontHeightSM = roundMetric(values.fontHeightSM);

    metricMap.font.lineHeightHeading1 = values.lineHeightHeading1;
    metricMap.font.lineHeightHeading2 = values.lineHeightHeading2;
    metricMap.font.lineHeightHeading3 = values.lineHeightHeading3;
    metricMap.font.lineHeightHeading4 = values.lineHeightHeading4;
    metricMap.font.lineHeightHeading5 = values.lineHeightHeading5;

    metricMap.control.controlHeight = roundMetric(values.controlHeight);
    metricMap.control.controlHeightSM = roundMetric(values.controlHeightSM);
    metricMap.control.controlHeightXS = roundMetric(values.controlHeightXS);
    metricMap.control.controlHeightLG = roundMetric(values.controlHeightLG);

    metricMap.control.lineWidth = roundLineWidth(values.lineWidth);
    metricMap.control.lineWidthBold =
        std::max(metricMap.control.lineWidth + 1, roundLineWidth(values.lineWidthBold));
    metricMap.control.lineWidthFocus = std::max(3, metricMap.control.lineWidth * 3);
    metricMap.control.controlInteractiveSize = roundMetric(values.controlHeight / 2.0);

    metricMap.control.motionDurationFastMs = values.motionDurationFast;
    metricMap.control.motionDurationMidMs = values.motionDurationMid;
    metricMap.control.motionDurationSlowMs = values.motionDurationSlow;

    metricMap.radius.borderRadius = roundMetric(values.borderRadius);
    metricMap.radius.borderRadiusXS = roundMetric(values.borderRadiusXS);
    metricMap.radius.borderRadiusSM = roundMetric(values.borderRadiusSM);
    metricMap.radius.borderRadiusLG = roundMetric(values.borderRadiusLG);
    metricMap.radius.borderRadiusOuter = roundMetric(values.borderRadiusOuter);

    return metricMap;
}

ThemeAliasColorToken buildAliasColorToken(const ThemeMapColorToken& mapToken) {
    ThemeAliasColorToken alias;
    alias.windowBg = mapToken.colorBgLayout;
    alias.surfaceBg = mapToken.colorBgContainer;
    alias.surfaceAltBg = mapToken.colorFillQuaternary;
    alias.contentBg = mapToken.colorBgLayout;
    alias.border = mapToken.colorBorder;
    alias.subtleBorder = mapToken.colorSplit;
    alias.textPrimary = mapToken.colorText;
    alias.textMuted = mapToken.colorTextSecondary;
    alias.textWeak = mapToken.colorTextTertiary;
    alias.accent = mapToken.colorPrimary;
    alias.accentText = mapToken.colorWhite;
    alias.hoverBg = mapToken.colorFillSecondary;
    alias.subHoverBg = mapToken.colorFillTertiary;
    alias.accentContainerBg = mapToken.colorPrimaryBg;
    alias.accentSoftBorder = mapToken.colorPrimaryBorderHover;
    alias.accentSoftBg = mapToken.colorPrimaryBgHover;
    alias.success = mapToken.colorSuccess;
    alias.warning = mapToken.colorWarning;
    alias.error = mapToken.colorError;
    return alias;
}

ThemeAliasMetricToken buildAliasMetricToken(const ThemeMetricMapToken& metricMap) {
    ThemeAliasMetricToken alias;

    alias.controlPaddingHorizontal = DEFAULT_CONTROL_PADDING_HORIZONTAL;
    alias.controlPaddingHorizontalSM = DEFAULT_CONTROL_PADDING_HORIZONTAL_SM;

    alias.paddingXXS = metricMap.size.sizeXXS;
    alias.paddingXS = metricMap.size.sizeXS;
    alias.paddingSM = metricMap.size.sizeSM;
    alias.padding = metricMap.size.size;
    alias.paddingMD = metricMap.size.sizeMD;
    alias.paddingLG = metricMap.size.sizeLG;
    alias.paddingXL = metricMap.size.sizeXL;

    alias.paddingContentHorizontalLG = metricMap.size.sizeLG;
    alias.paddingContentVerticalLG = metricMap.size.sizeMS;
    alias.paddingContentHorizontal = metricMap.size.sizeMS;
    alias.paddingContentVertical = metricMap.size.sizeSM;
    alias.paddingContentHorizontalSM = metricMap.size.size;
    alias.paddingContentVerticalSM = metricMap.size.sizeXS;

    alias.marginXXS = metricMap.size.sizeXXS;
    alias.marginXS = metricMap.size.sizeXS;
    alias.marginSM = metricMap.size.sizeSM;
    alias.margin = metricMap.size.size;
    alias.marginMD = metricMap.size.sizeMD;
    alias.marginLG = metricMap.size.sizeLG;
    alias.marginXL = metricMap.size.sizeXL;
    alias.marginXXL = metricMap.size.sizeXXL;

    alias.lineWidth = metricMap.control.lineWidth;
    alias.lineWidthBold = metricMap.control.lineWidthBold;
    alias.lineWidthFocus = metricMap.control.lineWidthFocus;

    alias.borderRadius = metricMap.radius.borderRadius;
    alias.borderRadiusXS = metricMap.radius.borderRadiusXS;
    alias.borderRadiusSM = metricMap.radius.borderRadiusSM;
    alias.borderRadiusLG = metricMap.radius.borderRadiusLG;
    alias.borderRadiusOuter = metricMap.radius.borderRadiusOuter;

    alias.fontSizeIcon = metricMap.font.fontSizeSM;
    alias.fontSizeSM = metricMap.font.fontSizeSM;
    alias.fontSize = metricMap.font.fontSize;
    alias.fontSizeLG = metricMap.font.fontSizeLG;
    alias.fontSizeXL = metricMap.font.fontSizeXL;
    alias.fontSizeHeading1 = metricMap.font.fontSizeHeading1;
    alias.fontSizeHeading2 = metricMap.font.fontSizeHeading2;
    alias.fontSizeHeading3 = metricMap.font.fontSizeHeading3;
    alias.fontSizeHeading4 = metricMap.font.fontSizeHeading4;
    alias.fontSizeHeading5 = metricMap.font.fontSizeHeading5;

    alias.controlHeight = metricMap.control.controlHeight;
    alias.controlHeightSM = metricMap.control.controlHeightSM;
    alias.controlHeightXS = metricMap.control.controlHeightXS;
    alias.controlHeightLG = metricMap.control.controlHeightLG;
    alias.controlInteractiveSize = metricMap.control.controlInteractiveSize;

    alias.motionDurationFastMs = metricMap.control.motionDurationFastMs;
    alias.motionDurationMidMs = metricMap.control.motionDurationMidMs;
    alias.motionDurationSlowMs = metricMap.control.motionDurationSlowMs;

    alias.scrollbarThickness = metricMap.size.sizeXS;
    alias.scrollbarMargin = metricMap.size.sizeXXS;
    alias.scrollbarMinLength = metricMap.size.sizeXXL;

    return alias;
}
} // namespace

ThemeColorScheme generateThemeColorScheme(const ThemeStyleConfig& config) {
    const adqt::theme::ResolvedTheme resolvedTheme =
        adqt::theme::makeResolvedTheme(toAdqtThemeConfig(config));
    const ThemeStyleConfig normalizedConfig = toThemeStyleConfig(resolvedTheme);

    ThemeColorScheme scheme;
    scheme.appearance = normalizedConfig.appearance;
    scheme.map = buildColorMapToken(resolvedTheme);
    scheme.metricMap = buildMetricMapToken(resolvedTheme);
    scheme.alias = buildAliasColorToken(scheme.map);
    scheme.metricAlias = buildAliasMetricToken(scheme.metricMap);
    return scheme;
}

ThemeColorScheme generateThemeColorScheme() {
    return ThemeManager::instance().themeColorScheme();
}
} // namespace snow_shot::presentation::styles
