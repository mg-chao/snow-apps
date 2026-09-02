#ifndef SNOW_SHOT_PRESENTATION_STYLES_THEMECOLORSCHEME_H
#define SNOW_SHOT_PRESENTATION_STYLES_THEMECOLORSCHEME_H

#include <QColor>
#include <QHash>
#include <QString>


namespace snow_shot::presentation::styles {
using ThemePresetColorMap = QHash<QString, QColor>;

enum class ThemeAppearance {
    Light,
    Dark,
};

enum class ThemeMode {
    FollowSystem,
    Light,
    Dark,
};

enum class ThemePreset {
    Default,
    Compact,
};

struct ThemeStyleConfig {
    ThemePreset preset = ThemePreset::Default;
    ThemeAppearance appearance = ThemeAppearance::Light;

    QColor colorPrimary;
    QColor colorSuccess;
    QColor colorWarning;
    QColor colorError;
    QColor colorInfo;
    QColor colorLink;
    QColor colorTextBase;
    QColor colorBgBase;
    ThemePresetColorMap presetColors;

    int fontSize = 14;
    int lineWidth = 1;
    int borderRadius = 6;

    int sizeUnit = 4;
    int sizeStep = 4;
    int controlHeight = 32;

    double motionUnit = 0.1;
};

struct ThemeMapColorToken {
    QColor colorBgBase;
    QColor colorTextBase;

    QColor colorText;
    QColor colorTextSecondary;
    QColor colorTextTertiary;
    QColor colorTextQuaternary;

    QColor colorFill;
    QColor colorFillSecondary;
    QColor colorFillTertiary;
    QColor colorFillQuaternary;

    QColor colorBgSolid;
    QColor colorBgSolidHover;
    QColor colorBgSolidActive;

    QColor colorBgLayout;
    QColor colorBgContainer;
    QColor colorBgElevated;
    QColor colorBgSpotlight;
    QColor colorBgBlur;

    QColor colorBorder;
    QColor colorBorderDisabled;
    QColor colorBorderSecondary;
    QColor colorSplit;

    QColor colorPrimaryBg;
    QColor colorPrimaryBgHover;
    QColor colorPrimaryBorder;
    QColor colorPrimaryBorderHover;
    QColor colorPrimaryHover;
    QColor colorPrimary;
    QColor colorPrimaryActive;
    QColor colorPrimaryTextHover;
    QColor colorPrimaryText;
    QColor colorPrimaryTextActive;

    QColor colorSuccessBg;
    QColor colorSuccessBgHover;
    QColor colorSuccessBorder;
    QColor colorSuccessBorderHover;
    QColor colorSuccessHover;
    QColor colorSuccess;
    QColor colorSuccessActive;
    QColor colorSuccessTextHover;
    QColor colorSuccessText;
    QColor colorSuccessTextActive;

    QColor colorErrorBg;
    QColor colorErrorBgHover;
    QColor colorErrorBgFilledHover;
    QColor colorErrorBgActive;
    QColor colorErrorBorder;
    QColor colorErrorBorderHover;
    QColor colorErrorHover;
    QColor colorError;
    QColor colorErrorActive;
    QColor colorErrorTextHover;
    QColor colorErrorText;
    QColor colorErrorTextActive;

    QColor colorWarningBg;
    QColor colorWarningBgHover;
    QColor colorWarningBorder;
    QColor colorWarningBorderHover;
    QColor colorWarningHover;
    QColor colorWarning;
    QColor colorWarningActive;
    QColor colorWarningTextHover;
    QColor colorWarningText;
    QColor colorWarningTextActive;

    QColor colorInfoBg;
    QColor colorInfoBgHover;
    QColor colorInfoBorder;
    QColor colorInfoBorderHover;
    QColor colorInfoHover;
    QColor colorInfo;
    QColor colorInfoActive;
    QColor colorInfoTextHover;
    QColor colorInfoText;
    QColor colorInfoTextActive;

    QColor colorLinkHover;
    QColor colorLink;
    QColor colorLinkActive;
    ThemePresetColorMap presetColorHover;

    QColor colorBgMask;
    QColor colorWhite;
};

struct ThemeMapSizeToken {
    int sizeXXL = 48;
    int sizeXL = 32;
    int sizeLG = 24;
    int sizeMD = 20;
    int sizeMS = 16;
    int size = 16;
    int sizeSM = 12;
    int sizeXS = 8;
    int sizeXXS = 4;
};

struct ThemeMapFontToken {
    int fontSizeSM = 12;
    int fontSize = 14;
    int fontSizeLG = 16;
    int fontSizeXL = 20;

    int fontSizeHeading1 = 38;
    int fontSizeHeading2 = 30;
    int fontSizeHeading3 = 24;
    int fontSizeHeading4 = 20;
    int fontSizeHeading5 = 16;

    double lineHeight = 1.57;
    double lineHeightLG = 1.5;
    double lineHeightSM = 1.67;

    int fontHeight = 22;
    int fontHeightLG = 24;
    int fontHeightSM = 20;

    double lineHeightHeading1 = 1.21;
    double lineHeightHeading2 = 1.27;
    double lineHeightHeading3 = 1.33;
    double lineHeightHeading4 = 1.4;
    double lineHeightHeading5 = 1.5;
};

struct ThemeMapControlToken {
    int controlHeight = 32;
    int controlHeightSM = 24;
    int controlHeightXS = 16;
    int controlHeightLG = 40;

    int lineWidth = 1;
    int lineWidthBold = 2;
    int lineWidthFocus = 3;
    int controlInteractiveSize = 16;

    int motionDurationFastMs = 100;
    int motionDurationMidMs = 200;
    int motionDurationSlowMs = 300;
};

struct ThemeMapRadiusToken {
    int borderRadius = 6;
    int borderRadiusXS = 2;
    int borderRadiusSM = 4;
    int borderRadiusLG = 8;
    int borderRadiusOuter = 4;
};

struct ThemeMetricMapToken {
    ThemeMapSizeToken size;
    ThemeMapFontToken font;
    ThemeMapControlToken control;
    ThemeMapRadiusToken radius;
};

struct ThemeAliasColorToken {
    QColor windowBg;
    QColor surfaceBg;
    QColor surfaceAltBg;
    QColor contentBg;
    QColor border;
    QColor subtleBorder;
    QColor textPrimary;
    QColor textMuted;
    QColor textWeak;
    QColor accent;
    QColor accentText;
    QColor hoverBg;
    QColor subHoverBg;
    QColor accentContainerBg;
    QColor accentSoftBorder;
    QColor accentSoftBg;

    QColor success;
    QColor warning;
    QColor error;
};

struct ThemeAliasMetricToken {
    int controlPaddingHorizontal = 12;
    int controlPaddingHorizontalSM = 8;

    int paddingXXS = 4;
    int paddingXS = 8;
    int paddingSM = 12;
    int padding = 16;
    int paddingMD = 20;
    int paddingLG = 24;
    int paddingXL = 32;

    int paddingContentHorizontalLG = 24;
    int paddingContentVerticalLG = 16;
    int paddingContentHorizontal = 16;
    int paddingContentVertical = 12;
    int paddingContentHorizontalSM = 16;
    int paddingContentVerticalSM = 8;

    int marginXXS = 4;
    int marginXS = 8;
    int marginSM = 12;
    int margin = 16;
    int marginMD = 20;
    int marginLG = 24;
    int marginXL = 32;
    int marginXXL = 48;

    int lineWidth = 1;
    int lineWidthBold = 2;
    int lineWidthFocus = 3;

    int borderRadius = 6;
    int borderRadiusXS = 2;
    int borderRadiusSM = 4;
    int borderRadiusLG = 8;
    int borderRadiusOuter = 4;

    int fontSizeIcon = 12;
    int fontSizeSM = 12;
    int fontSize = 14;
    int fontSizeLG = 16;
    int fontSizeXL = 20;
    int fontSizeHeading1 = 38;
    int fontSizeHeading2 = 30;
    int fontSizeHeading3 = 24;
    int fontSizeHeading4 = 20;
    int fontSizeHeading5 = 16;

    int controlHeight = 32;
    int controlHeightSM = 24;
    int controlHeightXS = 16;
    int controlHeightLG = 40;
    int controlInteractiveSize = 16;

    int motionDurationFastMs = 100;
    int motionDurationMidMs = 200;
    int motionDurationSlowMs = 300;

    int scrollbarThickness = 8;
    int scrollbarMargin = 4;
    int scrollbarMinLength = 48;
};

struct ThemeColorScheme {
    ThemeAppearance appearance = ThemeAppearance::Light;
    ThemeMapColorToken map;
    ThemeMetricMapToken metricMap;
    ThemeAliasColorToken alias;
    ThemeAliasMetricToken metricAlias;
};

ThemeColorScheme generateThemeColorScheme(const ThemeStyleConfig& config);
ThemeColorScheme generateThemeColorScheme();
} // namespace snow_shot::presentation::styles

#endif // SNOW_SHOT_PRESENTATION_STYLES_THEMECOLORSCHEME_H
