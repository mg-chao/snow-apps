#include "button_style.h"

#include "theme/theme.h"
#include "theme/theme_color_utils.h"

#include <QHash>

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace adqt::widgets::detail {

namespace {

using adqt::theme::ThemeMapToken;
using adqt::theme::ThemeSeedToken;

struct ColorFamily {
  QColor base;
  QColor hover;
  QColor active;
  QColor light;
  QColor lightHover;
  QColor lightActive;

  QColor outlinedText;
  QColor outlinedTextHover;
  QColor outlinedTextActive;

  QColor filledText;
  QColor filledTextHover;
  QColor filledTextActive;

  QColor textText;
  QColor textTextHover;
  QColor textTextActive;

  QColor solidBg;
  QColor solidBgHover;
  QColor solidBgActive;
  QColor solidText;
  QColor shadow;
};

constexpr int kPresetPaletteCacheMaxEntries = 128;

struct PresetPaletteKey {
  QString base;
  QString background;
  bool darkTheme = false;
};

bool operator==(const PresetPaletteKey& lhs, const PresetPaletteKey& rhs) {
  return lhs.base == rhs.base && lhs.background == rhs.background && lhs.darkTheme == rhs.darkTheme;
}

std::size_t qHash(const PresetPaletteKey& key, std::size_t seed) {
  seed = ::qHash(key.base, seed);
  seed = ::qHash(key.background, seed);
  seed = ::qHash(key.darkTheme, seed);
  return seed;
}

QHash<PresetPaletteKey, QVector<QColor>>& presetPaletteCache() {
  static QHash<PresetPaletteKey, QVector<QColor>> cache;
  return cache;
}

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor withAlpha(const QColor& color, double alpha) {
  QColor copy = color;
  copy.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
  return copy;
}

bool isStableChannel(int value) { return value >= 0 && value <= 255; }

QColor resolveAlphaColor(const QColor& frontColor, const QColor& backgroundColor) {
  if (frontColor.alphaF() < 1.0F) {
    return frontColor;
  }

  const int fR = frontColor.red();
  const int fG = frontColor.green();
  const int fB = frontColor.blue();

  const int bR = backgroundColor.red();
  const int bG = backgroundColor.green();
  const int bB = backgroundColor.blue();

  for (int i = 1; i <= 100; ++i) {
    const double alpha = i / 100.0;
    const int r = qRound((fR - bR * (1.0 - alpha)) / alpha);
    const int g = qRound((fG - bG * (1.0 - alpha)) / alpha);
    const int b = qRound((fB - bB * (1.0 - alpha)) / alpha);
    if (isStableChannel(r) && isStableChannel(g) && isStableChannel(b)) {
      return QColor(r, g, b, qRound(alpha * 255.0));
    }
  }

  return frontColor;
}

bool isBright(const QColor& color) {
  const int brightness = qGray(color.rgb());
  return brightness >= 186;
}

QString colorKey(const QColor& color) {
  return color.isValid() ? color.name(QColor::HexArgb) : QString();
}

QColor presetSeed(AdButton::AccentRole accentRole, const ThemeSeedToken& seed) {
  switch (accentRole) {
    case AdButton::AccentRole::Blue:
      return seed.blue;
    case AdButton::AccentRole::Purple:
      return seed.purple;
    case AdButton::AccentRole::Cyan:
      return seed.cyan;
    case AdButton::AccentRole::Green:
      return seed.green;
    case AdButton::AccentRole::Magenta:
      return seed.magenta;
    case AdButton::AccentRole::Pink:
      return seed.pink;
    case AdButton::AccentRole::Red:
      return seed.red;
    case AdButton::AccentRole::Orange:
      return seed.orange;
    case AdButton::AccentRole::Yellow:
      return seed.yellow;
    case AdButton::AccentRole::Volcano:
      return seed.volcano;
    case AdButton::AccentRole::Geekblue:
      return seed.geekblue;
    case AdButton::AccentRole::Lime:
      return seed.lime;
    case AdButton::AccentRole::Gold:
      return seed.gold;
    case AdButton::AccentRole::Neutral:
    case AdButton::AccentRole::Primary:
    case AdButton::AccentRole::Danger:
    default:
      return QColor();
  }
}

QVector<QColor> generateMappedPreset(const QColor& base, bool darkTheme,
                                     const QColor& backgroundColor) {
  PresetPaletteKey key;
  key.base = colorKey(base);
  key.background = colorKey(backgroundColor);
  key.darkTheme = darkTheme;

  auto& cache = presetPaletteCache();
  const auto cached = cache.constFind(key);
  if (cached != cache.constEnd()) {
    return cached.value();
  }

  const QVector<QColor> mapped =
      adqt::theme::generateMappedPalette(base, darkTheme, backgroundColor);

  if (cache.size() >= kPresetPaletteCacheMaxEntries) {
    cache.clear();
  }
  cache.insert(key, mapped);
  return mapped;
}

ColorFamily makeDefaultFamily(const ThemeMapToken& map) {
  ColorFamily family;

  family.base = toColor(map.colorBorder, QColor("#d9d9d9"));
  family.hover = toColor(map.colorPrimaryHover, QColor("#4096ff"));
  family.active = toColor(map.colorPrimaryActive, QColor("#0958d9"));
  family.light = toColor(map.colorFillTertiary, QColor("#f5f5f5"));
  family.lightHover = toColor(map.colorFillSecondary, QColor("#f0f0f0"));
  family.lightActive = toColor(map.colorFill, QColor("#d9d9d9"));

  family.outlinedText = toColor(map.colorText, QColor("#141414"));
  family.outlinedTextHover = family.hover;
  family.outlinedTextActive = family.active;

  family.filledText = toColor(map.colorText, QColor("#141414"));
  family.filledTextHover = family.filledText;
  family.filledTextActive = family.filledText;

  family.textText = toColor(map.colorText, QColor("#141414"));
  family.textTextHover = family.textText;
  family.textTextActive = family.textText;

  family.solidBg = toColor(map.colorBgSolid, QColor("#141414"));
  family.solidBgHover = toColor(map.colorBgSolidHover, QColor("#303030"));
  family.solidBgActive = toColor(map.colorBgSolidActive, QColor("#000000"));
  family.solidText = isBright(family.solidBg) ? QColor("#000000") : QColor("#ffffff");

  family.shadow = toColor(map.colorFillQuaternary, withAlpha(QColor("#000000"), 0.02));

  return family;
}

ColorFamily makePrimaryFamily(const ThemeMapToken& map) {
  ColorFamily family;
  const QColor containerBg = toColor(map.colorBgContainer, QColor("#ffffff"));

  family.base = toColor(map.colorPrimary, QColor("#1677ff"));
  family.hover = toColor(map.colorPrimaryHover, QColor("#4096ff"));
  family.active = toColor(map.colorPrimaryActive, QColor("#0958d9"));
  family.light = toColor(map.colorPrimaryBg, QColor("#e6f4ff"));
  family.lightHover = toColor(map.colorPrimaryBgHover, QColor("#bae0ff"));
  family.lightActive = toColor(map.colorPrimaryBorder, QColor("#91caff"));

  family.outlinedText = family.base;
  family.outlinedTextHover = family.hover;
  family.outlinedTextActive = family.active;

  family.filledText = family.base;
  family.filledTextHover = family.hover;
  family.filledTextActive = family.active;

  family.textText = family.base;
  family.textTextHover = family.hover;
  family.textTextActive = family.active;

  family.solidText = toColor(map.colorTextLightSolid, toColor(map.colorWhite, QColor("#ffffff")));
  family.solidBg = family.base;
  family.solidBgHover = family.hover;
  family.solidBgActive = family.active;

  family.shadow = resolveAlphaColor(toColor(map.colorPrimaryBg, QColor("#e6f4ff")), containerBg);

  return family;
}

ColorFamily makeDangerFamily(const ThemeMapToken& map) {
  ColorFamily family;
  const QColor containerBg = toColor(map.colorBgContainer, QColor("#ffffff"));

  family.base = toColor(map.colorError, QColor("#ff4d4f"));
  family.hover = toColor(map.colorErrorHover, QColor("#ff7875"));
  family.active = toColor(map.colorErrorActive, QColor("#cf1322"));
  family.light = toColor(map.colorErrorBg, QColor("#fff2f0"));
  family.lightHover = toColor(map.colorErrorBgFilledHover, QColor("#fff1f0"));
  family.lightActive = toColor(map.colorErrorBgActive, QColor("#ffccc7"));

  family.outlinedText = family.base;
  family.outlinedTextHover = family.hover;
  family.outlinedTextActive = family.active;

  family.filledText = family.base;
  family.filledTextHover = family.hover;
  family.filledTextActive = family.active;

  family.textText = family.base;
  family.textTextHover = family.hover;
  family.textTextActive = family.active;

  family.solidText = toColor(map.colorTextLightSolid, toColor(map.colorWhite, QColor("#ffffff")));
  family.solidBg = family.base;
  family.solidBgHover = family.hover;
  family.solidBgActive = family.active;

  family.shadow = resolveAlphaColor(toColor(map.colorErrorBg, QColor("#fff2f0")), containerBg);

  return family;
}

ColorFamily makeSuccessFamily(const ThemeMapToken& map) {
  ColorFamily family;
  const QColor containerBg = toColor(map.colorBgContainer, QColor("#ffffff"));

  family.base = toColor(map.colorSuccess, QColor("#52c41a"));
  family.hover = toColor(map.colorSuccessHover, QColor("#95de64"));
  family.active = toColor(map.colorSuccessActive, QColor("#389e0d"));
  family.light = toColor(map.colorSuccessBg, QColor("#f6ffed"));
  family.lightHover = toColor(map.colorSuccessBgHover, QColor("#d9f7be"));
  family.lightActive = toColor(map.colorSuccessBorder, QColor("#b7eb8f"));

  family.outlinedText = family.base;
  family.outlinedTextHover = family.hover;
  family.outlinedTextActive = family.active;

  family.filledText = family.base;
  family.filledTextHover = family.hover;
  family.filledTextActive = family.active;

  family.textText = family.base;
  family.textTextHover = family.hover;
  family.textTextActive = family.active;

  family.solidText = toColor(map.colorTextLightSolid, toColor(map.colorWhite, QColor("#ffffff")));
  family.solidBg = family.base;
  family.solidBgHover = family.hover;
  family.solidBgActive = family.active;

  family.shadow = resolveAlphaColor(toColor(map.colorSuccessBg, QColor("#f6ffed")), containerBg);

  return family;
}

ColorFamily makePresetFamily(AdButton::AccentRole accentRole, const ThemeMapToken& map,
                             const ThemeSeedToken& seed) {
  const QColor preset = presetSeed(accentRole, seed);
  const bool darkTheme = map.scheme == adqt::theme::ThemeScheme::Dark;
  const QColor presetBackground = toColor(map.colorBgContainer, map.colorBgBase);
  const QVector<QColor> mapped = generateMappedPreset(preset, darkTheme, presetBackground);
  const QColor containerBg = toColor(map.colorBgContainer, QColor("#ffffff"));
  // Ant Design exposes preset hover/active steps differently in dark mode than the base accent
  // tokens.
  const int hoverIndex = darkTheme ? 7 : 5;
  const int activeIndex = darkTheme ? 5 : 7;

  ColorFamily family;
  family.base = toColor(mapped.value(6), QColor("#1677ff"));
  family.hover = toColor(mapped.value(hoverIndex), family.base.lighter(112));
  family.active = toColor(mapped.value(activeIndex), family.base.darker(112));
  family.light = toColor(mapped.value(1), family.base.lighter(170));
  family.lightHover = toColor(mapped.value(2), family.base.lighter(160));
  family.lightActive = toColor(mapped.value(3), family.base.lighter(145));

  family.outlinedText = family.base;
  family.outlinedTextHover = family.hover;
  family.outlinedTextActive = family.active;

  family.filledText = family.base;
  family.filledTextHover = family.hover;
  family.filledTextActive = family.active;

  family.textText = family.base;
  family.textTextHover = family.hover;
  family.textTextActive = family.active;

  family.solidText = toColor(map.colorTextLightSolid, toColor(map.colorWhite, QColor("#ffffff")));
  family.solidBg = family.base;
  family.solidBgHover = family.hover;
  family.solidBgActive = family.active;

  family.shadow =
      resolveAlphaColor(toColor(mapped.value(1), family.base.lighter(170)), containerBg);

  return family;
}

ColorFamily makeFamily(AdButton::AccentRole accentRole, const ThemeMapToken& map,
                       const ThemeSeedToken& seed) {
  switch (accentRole) {
    case AdButton::AccentRole::Success:
      return makeSuccessFamily(map);
    case AdButton::AccentRole::Primary:
      return makePrimaryFamily(map);
    case AdButton::AccentRole::Danger:
      return makeDangerFamily(map);
    case AdButton::AccentRole::Blue:
    case AdButton::AccentRole::Purple:
    case AdButton::AccentRole::Cyan:
    case AdButton::AccentRole::Green:
    case AdButton::AccentRole::Magenta:
    case AdButton::AccentRole::Pink:
    case AdButton::AccentRole::Red:
    case AdButton::AccentRole::Orange:
    case AdButton::AccentRole::Yellow:
    case AdButton::AccentRole::Volcano:
    case AdButton::AccentRole::Geekblue:
    case AdButton::AccentRole::Lime:
    case AdButton::AccentRole::Gold:
      return makePresetFamily(accentRole, map, seed);
    case AdButton::AccentRole::Neutral:
    default:
      return makeDefaultFamily(map);
  }
}

void applyVariantStyle(ButtonVisualStyle& style, const ResolvedRole& role,
                       const ColorFamily& family, const ThemeMapToken& map) {
  const QColor containerBg = toColor(map.colorBgContainer, QColor("#ffffff"));
  const QColor transparent(0, 0, 0, 0);
  const QColor linkBase = toColor(map.colorLink, family.base);
  const QColor linkHover = toColor(map.colorLinkHover, family.hover);
  const QColor linkActive = toColor(map.colorLinkActive, family.active);

  style.normal.shadow = transparent;
  style.hover.shadow = transparent;
  style.active.shadow = transparent;
  style.checked.shadow = transparent;
  style.disabled.shadow = transparent;

  style.normal.borderStyle =
      role.visualStyle == AdButton::ButtonStyle::Dashed ? Qt::DashLine : Qt::SolidLine;
  style.hover.borderStyle = style.normal.borderStyle;
  style.active.borderStyle = style.normal.borderStyle;
  style.checked.borderStyle = style.normal.borderStyle;
  style.disabled.borderStyle = style.normal.borderStyle;

  switch (role.visualStyle) {
    case AdButton::ButtonStyle::Solid:
      style.normal.shadow = family.shadow;
      style.hover.shadow = family.shadow;
      style.active.shadow = family.shadow;

      style.normal.background = family.solidBg;
      style.hover.background = family.solidBgHover;
      style.active.background = family.solidBgActive;

      style.normal.border = transparent;
      style.hover.border = transparent;
      style.active.border = transparent;

      style.normal.text = family.solidText;
      style.hover.text = family.solidText;
      style.active.text = family.solidText;
      break;
    case AdButton::ButtonStyle::Tonal:
      style.normal.background = family.light;
      style.hover.background = family.lightHover;
      style.active.background = family.lightActive;

      style.normal.border = transparent;
      style.hover.border = transparent;
      style.active.border = transparent;

      style.normal.text = family.filledText;
      style.hover.text = family.filledTextHover;
      style.active.text = family.filledTextActive;
      break;
    case AdButton::ButtonStyle::Text:
      style.normal.background = transparent;
      style.hover.background = family.light;
      style.active.background = family.lightActive;

      style.normal.border = transparent;
      style.hover.border = transparent;
      style.active.border = transparent;

      style.normal.text = family.textText;
      style.hover.text = family.textTextHover;
      style.active.text = family.textTextActive;
      break;
    case AdButton::ButtonStyle::Link:
      style.normal.background = transparent;
      style.hover.background = transparent;
      style.active.background = transparent;

      style.normal.border = transparent;
      style.hover.border = transparent;
      style.active.border = transparent;

      style.normal.text =
          role.accentRole == AdButton::AccentRole::Neutral ? linkBase : family.textText;
      style.hover.text =
          role.accentRole == AdButton::AccentRole::Neutral ? linkHover : family.textTextHover;
      style.active.text =
          role.accentRole == AdButton::AccentRole::Neutral ? linkActive : family.textTextActive;
      break;
    case AdButton::ButtonStyle::Dashed:
    case AdButton::ButtonStyle::Outline:
    default:
      style.normal.shadow = family.shadow;
      style.hover.shadow = family.shadow;
      style.active.shadow = family.shadow;

      style.normal.background = containerBg;
      style.hover.background = containerBg;
      style.active.background = containerBg;

      style.normal.border = family.base;
      style.hover.border = family.hover;
      style.active.border = family.active;

      style.normal.text = family.outlinedText;
      style.hover.text = family.outlinedTextHover;
      style.active.text = family.outlinedTextActive;
      break;
  }

  style.checked = style.active;
}

void applyGhostStyle(ButtonVisualStyle& style, const ResolvedRole& role, const ThemeMapToken& map) {
  if (!role.ghost) {
    return;
  }

  const QColor transparent(0, 0, 0, 0);
  const QColor defaultGhost = toColor(map.colorBgContainer, QColor("#ffffff"));

  style.normal.background = transparent;
  style.hover.background = transparent;
  style.active.background = transparent;
  style.checked.background = transparent;

  style.normal.shadow = transparent;
  style.hover.shadow = transparent;
  style.active.shadow = transparent;
  style.checked.shadow = transparent;

  if (role.visualStyle == AdButton::ButtonStyle::Outline ||
      role.visualStyle == AdButton::ButtonStyle::Dashed) {
    if (role.accentRole == AdButton::AccentRole::Neutral) {
      style.normal.text = defaultGhost;
      style.normal.border = defaultGhost;
    }
  }
}

void applyDisabledStyle(ButtonVisualStyle& style, const ResolvedRole& role,
                        const ThemeMapToken& map) {
  const QColor disabledText = toColor(map.colorTextQuaternary, QColor("#bfbfbf"));
  const QColor disabledBorder = toColor(map.colorBorderDisabled, QColor("#d9d9d9"));
  const QColor disabledBg = toColor(map.colorFillTertiary, QColor("#f5f5f5"));
  const QColor transparent(0, 0, 0, 0);

  style.disabled.text = disabledText;

  if (role.visualStyle == AdButton::ButtonStyle::Text ||
      role.visualStyle == AdButton::ButtonStyle::Link) {
    style.disabled.border = transparent;
    style.disabled.background = transparent;
  } else {
    style.disabled.border = disabledBorder;
    style.disabled.background = role.ghost ? transparent : disabledBg;
  }
}

ButtonMetrics resolveMetrics(const ButtonStyleInput& input, const ThemeMapToken& map) {
  ButtonMetrics metrics;
  const int lineWidth = std::max(1, qRound(map.lineWidth));
  metrics.borderWidth = lineWidth;
  metrics.shadowOffsetY = std::max(1, qRound(map.lineWidth * 2.0));
  metrics.iconGap = std::max(4, qRound(map.sizeXS));
  metrics.menuIndicatorSize = std::max(6, qRound(map.fontSizeSM));
  metrics.menuIndicatorGap = std::max(4, qRound(map.sizeXXS));

  int fontSize;

  switch (input.sizeClass) {
    case AdButton::SizeClass::Small:
      metrics.height = std::max(20, qRound(map.controlHeightSM));
      metrics.borderRadius = std::max(0, qRound(map.borderRadiusSM));
      fontSize = qRound(map.fontSize);
      break;
    case AdButton::SizeClass::Large:
      metrics.height = std::max(28, qRound(map.controlHeightLG));
      metrics.borderRadius = std::max(0, qRound(map.borderRadiusLG));
      fontSize = qRound(map.fontSizeLG);
      break;
    case AdButton::SizeClass::Medium:
    default:
      metrics.height = std::max(24, qRound(map.controlHeight));
      metrics.borderRadius = std::max(0, qRound(map.borderRadius));
      fontSize = qRound(map.fontSize);
      break;
  }

  const int inlinePadding = (input.sizeClass == AdButton::SizeClass::Small)
                                ? qRound(8.0 - map.lineWidth)
                                : qRound(map.sizeMS - map.lineWidth);
  metrics.horizontalPadding = std::max(0, inlinePadding);

  metrics.font = input.baseFont;
  if (fontSize > 0) {
    metrics.font.setPixelSize(fontSize);
  }

  metrics.focusOutline = toColor(map.colorPrimaryBorder, QColor("#91caff"));
  metrics.focusOutlineWidth = std::max<qreal>(1.0, map.lineWidth * 3.0);
  metrics.focusOutlineOffset = 1.0;
  metrics.defaultOutline = toColor(map.colorPrimaryBorder, QColor("#91caff"));
  metrics.defaultOutlineWidth = std::max<qreal>(1.0, map.lineWidthBold);
  metrics.defaultOutlineOffset = std::max<qreal>(1.0, map.lineWidth * 2.0);

  return metrics;
}

}  // namespace

ResolvedRole resolveRole(const ButtonStyleInput& input) {
  ResolvedRole role;
  role.accentRole = input.accentRole;
  role.buttonStyle = input.buttonStyle;
  role.visualStyle = input.buttonStyle;
  role.defaultButton = input.defaultButton;
  role.hasMenu = input.hasMenu;

  if (input.flat && role.visualStyle != AdButton::ButtonStyle::Link) {
    role.buttonStyle = AdButton::ButtonStyle::Text;
    role.visualStyle = AdButton::ButtonStyle::Text;
  }

  switch (role.buttonStyle) {
    case AdButton::ButtonStyle::GhostOutline:
      role.visualStyle = AdButton::ButtonStyle::Outline;
      role.ghost = true;
      break;
    case AdButton::ButtonStyle::GhostDashed:
      role.visualStyle = AdButton::ButtonStyle::Dashed;
      role.ghost = true;
      break;
    default:
      role.visualStyle = role.buttonStyle;
      break;
  }

  role.unbordered = role.visualStyle == AdButton::ButtonStyle::Text ||
                    role.visualStyle == AdButton::ButtonStyle::Link;
  if (role.unbordered) {
    role.ghost = false;
  }

  return role;
}

ButtonVisualStyle resolveButtonVisualStyle(const ButtonStyleInput& input,
                                           const adqt::theme::ResolvedTheme& resolved) {
  ButtonVisualStyle style;
  const ThemeMapToken& map = resolved.values;
  const ThemeSeedToken& seed = resolved.config;

  style.role = resolveRole(input);
  style.metrics = resolveMetrics(input, map);

  const ColorFamily family = makeFamily(style.role.accentRole, map, seed);
  applyVariantStyle(style, style.role, family, map);
  applyGhostStyle(style, style.role, map);
  applyDisabledStyle(style, style.role, map);

  return style;
}

ButtonVisualStyle resolveButtonVisualStyle(const ButtonStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveButtonVisualStyle(input, resolved);
}

}  // namespace adqt::widgets::detail
