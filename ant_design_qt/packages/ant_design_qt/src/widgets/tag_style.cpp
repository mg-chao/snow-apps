#include "tag_style.h"

#include "theme/theme.h"
#include "theme/theme_color_utils.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

template <typename T>
void applyOptional(T* target, const std::optional<T>& value) {
  if (target && value.has_value()) {
    *target = value.value();
  }
}

QColor withAlpha(const QColor& color, int alpha) {
  QColor resolved = color;
  resolved.setAlpha(std::clamp(alpha, 0, 255));
  return resolved;
}

QColor bestTextColorForBackground(const QColor& background, const QColor& lightCandidate,
                                  const QColor& darkCandidate) {
  const QColor light = toColor(lightCandidate, QColor(Qt::white));
  const QColor dark = toColor(darkCandidate, QColor(Qt::black));
  if (!background.isValid()) {
    return light;
  }

  const qreal lightContrast = adqt::theme::colorContrastRatio(background, light);
  const qreal darkContrast = adqt::theme::colorContrastRatio(background, dark);
  return darkContrast > lightContrast ? dark : light;
}

bool isDarkTheme(const adqt::theme::ResolvedTheme& resolvedTheme) {
  return resolvedTheme.config.scheme == adqt::theme::ThemeScheme::Dark;
}

TagMetrics baseMetrics(const QFont& baseFont, const adqt::theme::ResolvedTheme& resolvedTheme) {
  const adqt::theme::ThemeMapToken& map = resolvedTheme.values;

  TagMetrics metrics;
  metrics.font = baseFont;
  metrics.font.setPixelSize(
      std::max(12, qRound(map.fontSizeSM > 0.0 ? map.fontSizeSM : map.fontSize)));
  metrics.height = std::max(20, qRound(metrics.font.pixelSize() * std::max(1.2, map.lineHeightSM)));
  metrics.borderRadius = std::max(0, qRound(map.borderRadiusSM));
  metrics.borderWidth = std::max(1, qRound(map.lineWidth));
  metrics.paddingHorizontal = 8;
  metrics.iconSize = std::max(10, metrics.font.pixelSize() - 1);
  metrics.contentGap = 4;
  metrics.closeGap = 3;
  metrics.focusOutlineColor = toColor(map.colorPrimaryBorder, QColor(QStringLiteral("#91caff")));
  metrics.focusOutlineWidth = std::max<qreal>(1.0, map.lineWidth * 3.0);
  metrics.focusOutlineOffset = 1.0;
  metrics.waveColor = toColor(map.colorPrimaryBorder, QColor(QStringLiteral("#1677ff")));
  return metrics;
}

void applyMetricTokens(const AdTag::MetricTokens& tokens, TagMetrics* metrics) {
  if (!metrics) {
    return;
  }

  applyOptional(&metrics->height, tokens.height);
  applyOptional(&metrics->borderRadius, tokens.borderRadius);
  applyOptional(&metrics->borderWidth, tokens.borderWidth);
  applyOptional(&metrics->paddingHorizontal, tokens.paddingHorizontal);
  applyOptional(&metrics->iconSize, tokens.iconSize);
  applyOptional(&metrics->contentGap, tokens.contentGap);
  applyOptional(&metrics->closeGap, tokens.closeGap);
  applyOptional(&metrics->focusOutlineWidth, tokens.focusOutlineWidth);
  applyOptional(&metrics->focusOutlineOffset, tokens.focusOutlineOffset);

  metrics->height = std::max(metrics->height, 16);
  metrics->borderRadius = std::max(metrics->borderRadius, 0);
  metrics->borderWidth = std::max(metrics->borderWidth, 0);
  metrics->paddingHorizontal = std::max(metrics->paddingHorizontal, 0);
  metrics->iconSize = std::max(metrics->iconSize, 8);
  metrics->contentGap = std::max(metrics->contentGap, 0);
  metrics->closeGap = std::max(metrics->closeGap, 0);
  metrics->focusOutlineWidth = std::max<qreal>(0.0, metrics->focusOutlineWidth);
  metrics->focusOutlineOffset = std::max<qreal>(0.0, metrics->focusOutlineOffset);
}

struct SchemePalette {
  QColor lightBg;
  QColor lightBorder;
  QColor text;
  QColor solidBg;
  QColor solidBorder;
  QColor solidText;
  bool isDefault = false;
};

QColor customLightBackground(const QColor& color) {
  if (!color.isValid()) {
    return color;
  }

  float hue = 0.0f;
  float saturation = 0.0f;
  float lightness = 0.0f;
  float alpha = 1.0f;
  color.getHslF(&hue, &saturation, &lightness, &alpha);
  if (hue < 0.0f) {
    hue = 0.0f;
  }

  QColor resolved;
  resolved.setHslF(hue, saturation, 0.95f, 1.0f);
  return resolved;
}

QColor accentBaseColor(AdTag::ColorScheme scheme, const adqt::theme::ResolvedTheme& resolvedTheme) {
  const adqt::theme::ThemeAccents& accents = resolvedTheme.theme.accents;
  switch (scheme) {
    case AdTag::ColorScheme::Blue:
      return accents.blue;
    case AdTag::ColorScheme::Purple:
      return accents.purple;
    case AdTag::ColorScheme::Cyan:
      return accents.cyan;
    case AdTag::ColorScheme::Green:
      return accents.green;
    case AdTag::ColorScheme::Magenta:
      return accents.magenta;
    case AdTag::ColorScheme::Pink:
      return accents.pink;
    case AdTag::ColorScheme::Red:
      return accents.red;
    case AdTag::ColorScheme::Orange:
      return accents.orange;
    case AdTag::ColorScheme::Yellow:
      return accents.yellow;
    case AdTag::ColorScheme::Volcano:
      return accents.volcano;
    case AdTag::ColorScheme::Geekblue:
      return accents.geekblue;
    case AdTag::ColorScheme::Lime:
      return accents.lime;
    case AdTag::ColorScheme::Gold:
      return accents.gold;
    default:
      return QColor();
  }
}

SchemePalette resolveDefaultPalette(const AdTag::ColorTokens& tokens,
                                    const adqt::theme::ResolvedTheme& resolvedTheme) {
  const adqt::theme::ThemeMapToken& map = resolvedTheme.values;

  SchemePalette palette;
  palette.isDefault = true;
  palette.lightBg =
      tokens.defaultBg.value_or(toColor(map.colorFillTertiary, QColor(QStringLiteral("#f0f0f0"))));
  palette.lightBorder = tokens.defaultBorderColor.value_or(
      toColor(map.colorBorder, QColor(QStringLiteral("#d9d9d9"))));
  palette.text =
      tokens.defaultColor.value_or(toColor(map.colorText, QColor(QStringLiteral("#141414"))));
  palette.solidBg = toColor(map.colorBgSolid, QColor(QStringLiteral("#595959")));
  palette.solidBorder = Qt::transparent;
  palette.solidText = tokens.solidTextColor.value_or(
      bestTextColorForBackground(palette.solidBg, toColor(map.colorWhite, QColor(Qt::white)),
                                 toColor(map.colorText, QColor(Qt::black))));
  return palette;
}

SchemePalette resolveAccentPalette(AdTag::ColorScheme scheme, const QColor& customColor,
                                   const adqt::theme::ResolvedTheme& resolvedTheme) {
  const adqt::theme::ThemeMapToken& map = resolvedTheme.values;
  const QColor base =
      scheme == AdTag::ColorScheme::Custom ? customColor : accentBaseColor(scheme, resolvedTheme);
  const QColor fallback = toColor(map.colorPrimary, QColor(QStringLiteral("#1677ff")));
  const QColor resolvedBase = toColor(base, fallback);
  const QVector<QColor> mapped = adqt::theme::generateMappedPalette(
      resolvedBase, isDarkTheme(resolvedTheme),
      toColor(map.colorBgContainer, QColor(QStringLiteral("#ffffff"))));
  const QColor colorTextLightSolid =
      toColor(map.colorTextLightSolid, QColor(QStringLiteral("#ffffff")));

  SchemePalette palette;
  if (scheme == AdTag::ColorScheme::Custom) {
    palette.lightBg = toColor(customLightBackground(resolvedBase), resolvedBase.lighter(185));
    palette.lightBorder = resolvedBase;
    palette.text = resolvedBase;
    palette.solidBg = resolvedBase;
    palette.solidBorder = Qt::transparent;
    palette.solidText = colorTextLightSolid;
    return palette;
  }

  palette.lightBg = toColor(mapped.value(1), resolvedBase.lighter(185));
  palette.lightBorder = toColor(mapped.value(3), resolvedBase);
  palette.text = toColor(mapped.value(7), resolvedBase.darker(120));
  palette.solidBg = toColor(mapped.value(6), resolvedBase);
  palette.solidBorder = palette.solidBg;
  palette.solidText = colorTextLightSolid;
  return palette;
}

SchemePalette resolveStatusPalette(AdTag::ColorScheme scheme,
                                   const adqt::theme::ResolvedTheme& resolvedTheme) {
  const adqt::theme::ThemeMapToken& map = resolvedTheme.values;

  SchemePalette palette;
  switch (scheme) {
    case AdTag::ColorScheme::Success:
      palette.lightBg = toColor(map.colorSuccessBg, QColor(QStringLiteral("#f6ffed")));
      palette.lightBorder = toColor(map.colorSuccessBorder, QColor(QStringLiteral("#b7eb8f")));
      palette.text = toColor(map.colorSuccess, QColor(QStringLiteral("#52c41a")));
      palette.solidBg = toColor(map.colorSuccess, QColor(QStringLiteral("#52c41a")));
      palette.solidBorder = palette.solidBg;
      palette.solidText = toColor(map.colorSuccessSolidText, QColor(Qt::white));
      break;
    case AdTag::ColorScheme::Processing:
      palette.lightBg = toColor(map.colorInfoBg, QColor(QStringLiteral("#e6f4ff")));
      palette.lightBorder = toColor(map.colorInfoBorder, QColor(QStringLiteral("#91caff")));
      palette.text = toColor(map.colorInfo, QColor(QStringLiteral("#1677ff")));
      palette.solidBg = toColor(map.colorInfo, QColor(QStringLiteral("#1677ff")));
      palette.solidBorder = palette.solidBg;
      palette.solidText = toColor(map.colorInfoSolidText, QColor(Qt::white));
      break;
    case AdTag::ColorScheme::Warning:
      palette.lightBg = toColor(map.colorWarningBg, QColor(QStringLiteral("#fffbe6")));
      palette.lightBorder = toColor(map.colorWarningBorder, QColor(QStringLiteral("#ffe58f")));
      palette.text = toColor(map.colorWarning, QColor(QStringLiteral("#faad14")));
      palette.solidBg = toColor(map.colorWarning, QColor(QStringLiteral("#faad14")));
      palette.solidBorder = palette.solidBg;
      palette.solidText = toColor(map.colorWarningSolidText, QColor(Qt::white));
      break;
    case AdTag::ColorScheme::Error:
      palette.lightBg = toColor(map.colorErrorBg, QColor(QStringLiteral("#fff2f0")));
      palette.lightBorder = toColor(map.colorErrorBorder, QColor(QStringLiteral("#ffccc7")));
      palette.text = toColor(map.colorError, QColor(QStringLiteral("#ff4d4f")));
      palette.solidBg = toColor(map.colorError, QColor(QStringLiteral("#ff4d4f")));
      palette.solidBorder = palette.solidBg;
      palette.solidText = toColor(map.colorErrorSolidText, QColor(Qt::white));
      break;
    default:
      break;
  }
  if (!palette.solidText.isValid()) {
    palette.solidText =
        bestTextColorForBackground(palette.solidBg, toColor(map.colorWhite, QColor(Qt::white)),
                                   toColor(map.colorText, QColor(Qt::black)));
  }
  return palette;
}

SchemePalette resolveSchemePalette(const TagStyleInput& input,
                                   const adqt::theme::ResolvedTheme& resolvedTheme) {
  switch (input.colorScheme) {
    case AdTag::ColorScheme::Default:
      return resolveDefaultPalette(input.componentTokens.colors, resolvedTheme);
    case AdTag::ColorScheme::Success:
    case AdTag::ColorScheme::Processing:
    case AdTag::ColorScheme::Warning:
    case AdTag::ColorScheme::Error:
      return resolveStatusPalette(input.colorScheme, resolvedTheme);
    case AdTag::ColorScheme::Custom:
      return resolveAccentPalette(input.colorScheme, input.customColor, resolvedTheme);
    default:
      return resolveAccentPalette(input.colorScheme, QColor(), resolvedTheme);
  }
}

void applyRootSemanticStyle(const AdTag::SemanticSlotStyle& slot, TagVisualStyle* style) {
  if (!style) {
    return;
  }

  applyOptional(&style->backgroundColor, slot.backgroundColor);
  applyOptional(&style->borderColor, slot.borderColor);
  if (slot.textColor.has_value()) {
    style->contentColor = slot.textColor.value();
    style->iconColor = slot.textColor.value();
    style->closeColor = slot.textColor.value();
    style->closeHoverColor = slot.textColor.value();
  }
}

void applySemanticStyles(const AdTag::SemanticStyles& semanticStyles, TagVisualStyle* style) {
  if (!style) {
    return;
  }

  applyRootSemanticStyle(semanticStyles.root, style);
  if (semanticStyles.icon.textColor.has_value()) {
    style->iconColor = semanticStyles.icon.textColor.value();
  }
  if (semanticStyles.content.textColor.has_value()) {
    style->contentColor = semanticStyles.content.textColor.value();
  }
  if (semanticStyles.closeIcon.textColor.has_value()) {
    style->closeColor = semanticStyles.closeIcon.textColor.value();
    style->closeHoverColor = semanticStyles.closeIcon.textColor.value();
  }
  if (semanticStyles.closeIcon.backgroundColor.has_value()) {
    style->closeHoverBackground = semanticStyles.closeIcon.backgroundColor.value();
  }
}

}  // namespace

TagVisualStyle resolveTagVisualStyle(const TagStyleInput& input,
                                     const adqt::theme::ResolvedTheme& resolvedTheme) {
  const adqt::theme::ThemeMapToken& map = resolvedTheme.values;
  const AdTag::ColorTokens& tokens = input.componentTokens.colors;

  const QColor colorText = toColor(map.colorText, QColor(QStringLiteral("#141414")));
  const QColor colorTextDisabled = tokens.colorTextDisabled.value_or(
      toColor(map.colorTextDisabled, QColor(QStringLiteral("#bfbfbf"))));
  const QColor colorBorderDisabled = tokens.colorBorderDisabled.value_or(
      toColor(map.colorBorderDisabled, QColor(QStringLiteral("#d9d9d9"))));
  const QColor colorBgDisabled = tokens.colorBgContainerDisabled.value_or(
      toColor(map.colorBgContainerDisabled, QColor(QStringLiteral("#f5f5f5"))));
  const QColor colorFillSecondary =
      toColor(map.colorFillSecondary, QColor(QStringLiteral("#f5f5f5")));
  const QColor colorPrimary = toColor(map.colorPrimary, QColor(QStringLiteral("#1677ff")));
  const QColor colorPrimaryHover =
      toColor(map.colorPrimaryHover, QColor(QStringLiteral("#4096ff")));
  const QColor colorPrimaryActive =
      toColor(map.colorPrimaryActive, QColor(QStringLiteral("#0958d9")));
  const QColor colorTextLightSolid =
      toColor(map.colorTextLightSolid, QColor(QStringLiteral("#ffffff")));

  TagVisualStyle style;
  style.metrics = baseMetrics(input.baseFont, resolvedTheme);
  applyMetricTokens(input.componentTokens.metrics, &style.metrics);
  applyOptional(&style.metrics.focusOutlineColor, tokens.focusOutlineColor);
  applyOptional(&style.metrics.waveColor, tokens.waveColor);
  style.focusOutlineColor = style.metrics.focusOutlineColor;
  style.borderPenStyle =
      input.borderStyle == AdTag::BorderStyle::Dashed ? Qt::DashLine : Qt::SolidLine;

  const SchemePalette schemePalette = resolveSchemePalette(input, resolvedTheme);

  if (input.checkable) {
    style.borderColor = Qt::transparent;
    style.backgroundColor = Qt::transparent;
    style.contentColor = colorText;
    style.iconColor = colorText;

    if (input.disabled) {
      style.contentColor = colorTextDisabled;
      style.iconColor = colorTextDisabled;
      if (input.checked) {
        style.backgroundColor = colorBgDisabled;
      }
    } else if (input.checked) {
      style.backgroundColor = tokens.checkableCheckedBg.value_or(colorPrimary);
      style.contentColor = tokens.checkableCheckedColor.value_or(colorTextLightSolid);
      style.iconColor = style.contentColor;
      if (input.hovered) {
        style.backgroundColor = tokens.checkableCheckedHoverBg.value_or(colorPrimaryHover);
      }
      if (input.pressed) {
        style.backgroundColor = tokens.checkableActiveBg.value_or(colorPrimaryActive);
      }
    } else {
      if (input.hovered) {
        style.backgroundColor = tokens.checkableHoverBg.value_or(colorFillSecondary);
        style.contentColor = tokens.checkableHoverColor.value_or(colorPrimary);
        style.iconColor = style.contentColor;
      }
      if (input.pressed) {
        style.backgroundColor = tokens.checkableActiveBg.value_or(colorPrimaryActive);
        style.contentColor = tokens.checkableCheckedColor.value_or(colorTextLightSolid);
        style.iconColor = style.contentColor;
      }
    }
  } else if (input.disabled) {
    style.backgroundColor = colorBgDisabled;
    style.borderColor =
        input.variant == AdTag::Variant::Outlined ? colorBorderDisabled : Qt::transparent;
    style.contentColor = colorTextDisabled;
    style.iconColor = colorTextDisabled;
  } else {
    switch (input.variant) {
      case AdTag::Variant::Filled:
        style.backgroundColor = schemePalette.lightBg;
        style.borderColor = Qt::transparent;
        style.contentColor = schemePalette.text;
        break;
      case AdTag::Variant::Solid:
        style.backgroundColor = schemePalette.solidBg;
        style.borderColor = schemePalette.solidBorder;
        style.contentColor = schemePalette.solidText;
        break;
      case AdTag::Variant::Outlined:
        style.backgroundColor = schemePalette.lightBg;
        style.borderColor = schemePalette.lightBorder;
        style.contentColor = schemePalette.text;
        break;
    }
    style.iconColor = style.contentColor;
  }

  style.closeColor = input.disabled
                         ? colorTextDisabled
                         : tokens.closeColor.value_or(
                               toColor(map.colorTextTertiary, withAlpha(style.contentColor, 180)));
  style.closeHoverColor =
      input.disabled ? colorTextDisabled
                     : tokens.closeHoverColor.value_or(toColor(map.colorText, style.contentColor));
  style.closeHoverBackground =
      input.disabled
          ? Qt::transparent
          : toColor(tokens.closeHoverBackground.value_or(Qt::transparent), Qt::transparent);

  applySemanticStyles(input.semanticStyles, &style);
  style.focusOutlineColor = style.metrics.focusOutlineColor;
  if (map.backgroundOpacity != 1.0) {
    style.backgroundColor =
        adqt::theme::applyBackgroundOpacity(style.backgroundColor, map.backgroundOpacity);
    style.closeHoverBackground =
        adqt::theme::applyBackgroundOpacity(style.closeHoverBackground, map.backgroundOpacity);
  }
  return style;
}

TagVisualStyle resolveTagVisualStyle(const TagStyleInput& input) {
  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveTagVisualStyle(input, resolvedTheme);
}

}  // namespace adqt::widgets::detail
