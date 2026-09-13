#include "menu_style.h"

#include "theme/theme.h"

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

using adqt::theme::ThemeManager;
using adqt::theme::ThemeMapToken;

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor withAlpha(const QColor& color, float alpha) {
  QColor copy = color;
  copy.setAlphaF(std::clamp(alpha, 0.0F, 1.0F));
  return copy;
}

QColor resolveTokenColorChain(const std::optional<QColor>& primaryToken,
                              const std::optional<QColor>& secondaryToken, const QColor& fallback) {
  if (primaryToken.has_value()) {
    return toColor(primaryToken.value(), fallback);
  }
  if (secondaryToken.has_value()) {
    return toColor(secondaryToken.value(), fallback);
  }
  return fallback;
}

const AdNavigationMenu::SharedColorTokens& sharedColors(
    const AdNavigationMenu::ComponentTokens& tokens) {
  return tokens.colors.shared;
}

const AdNavigationMenu::SchemeColorTokens& lightColors(
    const AdNavigationMenu::ComponentTokens& tokens) {
  return tokens.colors.light;
}

const AdNavigationMenu::SchemeColorTokens& darkColors(
    const AdNavigationMenu::ComponentTokens& tokens) {
  return tokens.colors.dark;
}

QColor resolveSchemeColor(const std::optional<QColor>& schemeToken,
                          const std::optional<QColor>& sharedToken, const QColor& fallback) {
  return resolveTokenColorChain(schemeToken, sharedToken, fallback);
}

void applyTokenOverrides(MenuMetrics& metrics, const AdNavigationMenu::ComponentTokens& tokens) {
  const auto& metricTokens = tokens.metrics;
  if (metricTokens.itemHeight.has_value()) {
    metrics.itemHeight = std::max(20, metricTokens.itemHeight.value());
  }
  if (metricTokens.itemPaddingInline.has_value()) {
    metrics.itemPaddingInline = std::max(0, metricTokens.itemPaddingInline.value());
  }
  if (metricTokens.itemMarginInline.has_value()) {
    metrics.itemMarginInline = std::max(0, metricTokens.itemMarginInline.value());
  }
  if (metricTokens.itemMarginBlock.has_value()) {
    metrics.itemMarginBlock = std::max(0, metricTokens.itemMarginBlock.value());
  }
  if (metricTokens.rootPaddingBlockStart.has_value()) {
    metrics.rootPaddingBlockStart = std::max(0, metricTokens.rootPaddingBlockStart.value());
  }
  if (metricTokens.itemBorderRadius.has_value()) {
    metrics.itemBorderRadius = std::max(0, metricTokens.itemBorderRadius.value());
  }
  if (metricTokens.horizontalItemBorderRadius.has_value()) {
    metrics.horizontalItemBorderRadius =
        std::max(0, metricTokens.horizontalItemBorderRadius.value());
  }
  if (metricTokens.subMenuItemBorderRadius.has_value()) {
    metrics.subMenuItemBorderRadius = std::max(0, metricTokens.subMenuItemBorderRadius.value());
  }
  if (metricTokens.indentation.has_value()) {
    metrics.indentation = std::max(0, metricTokens.indentation.value());
  } else if (metricTokens.inlineIndent.has_value()) {
    metrics.indentation = std::max(0, metricTokens.inlineIndent.value());
  }
  if (metricTokens.iconSize.has_value()) {
    metrics.iconSize = std::max(10, metricTokens.iconSize.value());
  }
  if (metricTokens.iconMarginInlineEnd.has_value()) {
    metrics.iconMarginInlineEnd = std::max(0, metricTokens.iconMarginInlineEnd.value());
  }
  if (metricTokens.activeBarWidth.has_value()) {
    metrics.activeBarWidth = std::max(0, metricTokens.activeBarWidth.value());
  }
  if (metricTokens.borderWidth.has_value()) {
    metrics.borderWidth = std::max(0, metricTokens.borderWidth.value());
  }
  if (metricTokens.groupTitleFontSize.has_value()) {
    metrics.groupTitleFontSize = std::max(10, metricTokens.groupTitleFontSize.value());
  }
  if (metricTokens.groupTitleLineHeight.has_value()) {
    metrics.groupTitleLineHeight =
        std::max(metrics.groupTitleFontSize, metricTokens.groupTitleLineHeight.value());
  }
}

void applySemanticSlot(AdNavigationMenu::SemanticSlotStyle slot, MenuStateStyle& state) {
  if (slot.textColor.has_value()) {
    state.text = slot.textColor.value();
  }
  if (slot.backgroundColor.has_value()) {
    state.background = slot.backgroundColor.value();
  }
}

void applySemanticStyles(const AdNavigationMenu::SemanticStyles& semantic, MenuVisualStyle& style) {
  if (semantic.root.backgroundColor.has_value()) {
    style.menuBackground = semantic.root.backgroundColor.value();
  }
  if (semantic.root.borderColor.has_value()) {
    style.borderColor = semantic.root.borderColor.value();
    style.popupBorderColor = semantic.root.borderColor.value();
    style.dividerColor = semantic.root.borderColor.value();
  }

  applySemanticSlot(semantic.item, style.normal);
  applySemanticSlot(semantic.item, style.horizontalNormal);
  applySemanticSlot(semantic.itemContent, style.hover);
  applySemanticSlot(semantic.subMenuItem, style.active);
  applySemanticSlot(semantic.subMenuItem, style.dangerActive);
  if (semantic.itemTitle.textColor.has_value()) {
    style.groupTitleColor = semantic.itemTitle.textColor.value();
  }
  if (semantic.subMenuItemTitle.textColor.has_value()) {
    style.subMenuItemSelectedColor = semantic.subMenuItemTitle.textColor.value();
  }

  if (semantic.subMenuList.backgroundColor.has_value()) {
    style.subMenuBackground = semantic.subMenuList.backgroundColor.value();
  }
  if (semantic.popup.backgroundColor.has_value()) {
    style.popupBackground = semantic.popup.backgroundColor.value();
  }
  if (semantic.popup.borderColor.has_value()) {
    style.popupBorderColor = semantic.popup.borderColor.value();
  }
}

MenuMetrics resolveMetrics(const ThemeMapToken& map, const QFont& baseFont,
                           const AdNavigationMenu::ComponentTokens& tokenOverrides) {
  MenuMetrics metrics;

  metrics.itemHeight = std::max(28, qRound(map.controlHeightLG));
  metrics.horizontalLineHeight = std::max(metrics.itemHeight, qRound(map.controlHeightLG * 1.15));
  metrics.itemPaddingInline = std::max(8, qRound(map.sizeMS));
  metrics.itemMarginInline = std::max(2, qRound(map.sizeXXS));
  metrics.itemMarginBlock = std::max(1, qRound(map.sizeXXS));
  metrics.itemBorderRadius = std::max(0, qRound(map.borderRadiusLG));
  metrics.horizontalItemBorderRadius = 0;
  metrics.subMenuItemBorderRadius = std::max(0, qRound(map.borderRadiusSM));
  metrics.popupBorderRadius = std::max(0, qRound(map.borderRadiusLG));
  metrics.indentation = 24;

  const int itemFontSize = std::max(12, qRound(map.fontSize));
  metrics.font = baseFont;
  metrics.font.setPixelSize(itemFontSize);

  metrics.iconSize = std::max(12, qRound(map.fontSizeLG));
  metrics.collapsedIconSize = std::max(metrics.iconSize, qRound(map.fontSizeLG));
  metrics.iconMarginInlineEnd = std::max(6, qRound(map.controlHeightSM - map.fontSize));
  metrics.activeBarWidth = 0;
  metrics.activeBarHeight = std::max(1, qRound(map.lineWidthBold));

  metrics.borderWidth = std::max(1, qRound(map.lineWidth));
  metrics.dividerMarginBlock = metrics.borderWidth;

  metrics.groupTitleFontSize = std::max(10, qRound(map.fontSize));
  metrics.groupTitleLineHeight =
      std::max(metrics.groupTitleFontSize, qRound(map.lineHeight * metrics.groupTitleFontSize));
  metrics.groupTitleHorizontalPadding = metrics.itemPaddingInline;
  metrics.groupTitleVerticalPadding = std::max(4, qRound(map.sizeXS));
  metrics.popupPlacementGap = std::max(0, qRound(map.sizeXS));
  metrics.horizontalSpacing = 0;

  applyTokenOverrides(metrics, tokenOverrides);
  metrics.collapsedIconSize = std::max(metrics.iconSize, std::max(12, qRound(map.fontSizeLG)));
  metrics.horizontalLineHeight =
      std::max(metrics.itemHeight, std::max(1, qRound(map.controlHeightLG * 1.15)));
  return metrics;
}

MenuVisualStyle makeLightStyle(const ThemeMapToken& map, const QFont& baseFont,
                               const AdNavigationMenu::ComponentTokens& tokens) {
  MenuVisualStyle style;
  style.metrics = resolveMetrics(map, baseFont, tokens);

  const auto& shared = sharedColors(tokens);
  const auto& light = lightColors(tokens);
  const QColor transparent(0, 0, 0, 0);

  style.menuBackground = resolveSchemeColor(light.itemBackground, shared.itemBackground,
                                            toColor(map.colorBgContainer, QColor("#ffffff")));
  style.borderColor = toColor(map.colorBorderSecondary, QColor("#f0f0f0"));
  style.dividerColor = style.borderColor;
  style.groupTitleColor = resolveSchemeColor(light.groupTitleText, shared.groupTitleText,
                                             toColor(map.colorTextTertiary, QColor("#8c8c8c")));
  style.subMenuBackground = resolveSchemeColor(
      light.subMenuItemBackground, shared.subMenuItemBackground,
      toColor(map.colorFillAlter, toColor(map.colorFillQuaternary, QColor("#fafafa"))));
  style.popupBackground = resolveSchemeColor(light.popupBackground, shared.popupBackground,
                                             toColor(map.colorBgElevated, QColor("#ffffff")));
  style.popupBorderColor = style.borderColor;

  style.normal.text = resolveSchemeColor(light.itemText, shared.itemText,
                                         toColor(map.colorText, QColor("#141414")));
  style.normal.background = transparent;

  style.hover.text = resolveSchemeColor(light.itemHoverText, shared.itemHoverText,
                                        toColor(map.colorText, QColor("#141414")));
  style.hover.background = resolveSchemeColor(light.itemHoverBackground, shared.itemHoverBackground,
                                              toColor(map.colorFillSecondary, QColor("#f5f5f5")));

  style.active.text = toColor(map.colorText, QColor("#141414"));
  style.active.background =
      resolveSchemeColor(light.itemActiveBackground, shared.itemActiveBackground,
                         toColor(map.colorPrimaryBg, QColor("#e6f4ff")));

  style.selected.text = resolveSchemeColor(light.itemSelectedText, shared.itemSelectedText,
                                           toColor(map.colorPrimary, QColor("#1677ff")));
  style.selected.background =
      resolveSchemeColor(light.itemSelectedBackground, shared.itemSelectedBackground,
                         toColor(map.colorPrimaryBg, QColor("#e6f4ff")));
  style.subMenuItemSelectedColor = resolveSchemeColor(
      light.subMenuItemSelectedText, shared.subMenuItemSelectedText, style.selected.text);

  style.disabled.text = resolveSchemeColor(light.itemDisabledText, shared.itemDisabledText,
                                           toColor(map.colorTextQuaternary, QColor("#bfbfbf")));
  style.disabled.background = transparent;

  style.danger.text = resolveSchemeColor(light.dangerItemText, shared.dangerItemText,
                                         toColor(map.colorError, QColor("#ff4d4f")));
  style.danger.background = transparent;

  style.dangerHover.text =
      resolveSchemeColor(light.dangerItemHoverText, shared.dangerItemHoverText, style.danger.text);
  style.dangerHover.background = style.hover.background;

  style.dangerActive.text = style.danger.text;
  style.dangerActive.background =
      resolveSchemeColor(light.dangerItemActiveBackground, shared.dangerItemActiveBackground,
                         toColor(map.colorErrorBg, QColor("#fff2f0")));

  style.dangerSelected.text = resolveSchemeColor(light.dangerItemSelectedText,
                                                 shared.dangerItemSelectedText, style.danger.text);
  style.dangerSelected.background =
      resolveSchemeColor(light.dangerItemSelectedBackground, shared.dangerItemSelectedBackground,
                         toColor(map.colorErrorBg, QColor("#fff2f0")));

  style.horizontalNormal = style.normal;
  style.horizontalHover = style.hover;
  style.horizontalHover.text = resolveSchemeColor(light.horizontalItemHoverText,
                                                  shared.horizontalItemHoverText, style.hover.text);
  style.horizontalActive = style.horizontalHover;
  style.horizontalSelected.text = resolveSchemeColor(
      light.horizontalItemSelectedText, shared.horizontalItemSelectedText, style.selected.text);
  style.horizontalSelected.background = resolveSchemeColor(
      light.horizontalItemSelectedBackground, shared.horizontalItemSelectedBackground, transparent);

  if (light.horizontalItemHoverBackground.has_value() ||
      shared.horizontalItemHoverBackground.has_value()) {
    style.horizontalHover.background =
        resolveSchemeColor(light.horizontalItemHoverBackground,
                           shared.horizontalItemHoverBackground, style.horizontalHover.background);
  } else {
    style.horizontalHover.background = transparent;
  }

  return style;
}

MenuVisualStyle makeDarkStyle(const ThemeMapToken& map, const QFont& baseFont,
                              const AdNavigationMenu::ComponentTokens& tokens) {
  MenuVisualStyle style;
  style.metrics = resolveMetrics(map, baseFont, tokens);

  const auto& shared = sharedColors(tokens);
  const auto& dark = darkColors(tokens);
  const QColor primaryColor = toColor(map.colorPrimary, QColor("#1677ff"));
  const QColor textLight = toColor(map.colorWhite, QColor("#ffffff"));
  const QColor textDark = withAlpha(textLight, 0.65F);
  const QColor dangerColor = toColor(map.colorError, QColor("#ff4d4f"));
  const QColor dangerHoverColor = toColor(map.colorErrorHover, QColor("#ff7875"));
  const QColor transparent(0, 0, 0, 0);

  style.menuBackground =
      resolveSchemeColor(dark.itemBackground, shared.itemBackground, QColor("#001529"));
  style.borderColor = transparent;
  style.dividerColor = withAlpha(textLight, 0.12F);
  style.groupTitleColor = resolveSchemeColor(dark.groupTitleText, shared.groupTitleText, textDark);
  style.subMenuBackground = resolveSchemeColor(dark.subMenuItemBackground,
                                               shared.subMenuItemBackground, QColor("#000c17"));
  style.popupBackground =
      resolveSchemeColor(dark.popupBackground, shared.popupBackground, style.menuBackground);
  style.popupBorderColor = toColor(map.colorBorderSecondary, withAlpha(textLight, 0.12F));

  style.normal.text = resolveSchemeColor(dark.itemText, shared.itemText, textDark);
  style.normal.background = transparent;

  style.hover.text = resolveSchemeColor(dark.itemHoverText, shared.itemHoverText, textLight);
  style.hover.background =
      resolveSchemeColor(dark.itemHoverBackground, shared.itemHoverBackground, transparent);

  style.active.text = textLight;
  style.active.background =
      resolveSchemeColor(dark.itemActiveBackground, shared.itemActiveBackground,
                         toColor(map.colorPrimaryBg, QColor("#15325b")));

  style.selected.text =
      resolveSchemeColor(dark.itemSelectedText, shared.itemSelectedText, textLight);
  style.selected.background =
      resolveSchemeColor(dark.itemSelectedBackground, shared.itemSelectedBackground, primaryColor);
  style.subMenuItemSelectedColor = resolveSchemeColor(
      dark.subMenuItemSelectedText, shared.subMenuItemSelectedText, style.selected.text);

  style.disabled.text = resolveSchemeColor(dark.itemDisabledText, shared.itemDisabledText,
                                           withAlpha(textLight, 0.25F));
  style.disabled.background = transparent;

  style.danger.text = resolveSchemeColor(dark.dangerItemText, shared.dangerItemText, dangerColor);
  style.danger.background = transparent;

  style.dangerHover.text =
      resolveSchemeColor(dark.dangerItemHoverText, shared.dangerItemHoverText, dangerHoverColor);
  style.dangerHover.background = style.hover.background;

  style.dangerActive.text =
      resolveSchemeColor(dark.dangerItemSelectedText, shared.dangerItemSelectedText, textLight);
  style.dangerActive.background = resolveSchemeColor(
      dark.dangerItemActiveBackground, shared.dangerItemActiveBackground, dangerColor);

  style.dangerSelected.text = style.dangerActive.text;
  style.dangerSelected.background = resolveSchemeColor(
      dark.dangerItemSelectedBackground, shared.dangerItemSelectedBackground, dangerColor);

  style.horizontalNormal = style.normal;
  style.horizontalHover = style.hover;
  style.horizontalHover.text = resolveSchemeColor(dark.horizontalItemHoverText,
                                                  shared.horizontalItemHoverText, style.hover.text);
  style.horizontalActive = style.horizontalHover;
  style.horizontalSelected.text = resolveSchemeColor(
      dark.horizontalItemSelectedText, shared.horizontalItemSelectedText, style.selected.text);
  style.horizontalSelected.background =
      resolveSchemeColor(dark.horizontalItemSelectedBackground,
                         shared.horizontalItemSelectedBackground, style.selected.background);

  if (dark.horizontalItemHoverBackground.has_value() ||
      shared.horizontalItemHoverBackground.has_value()) {
    style.horizontalHover.background =
        resolveSchemeColor(dark.horizontalItemHoverBackground, shared.horizontalItemHoverBackground,
                           style.horizontalHover.background);
  }

  style.metrics.activeBarHeight = 0;

  return style;
}

}  // namespace

MenuVisualStyle resolveMenuVisualStyle(const MenuStyleInput& input,
                                       const adqt::theme::ResolvedTheme& resolved) {
  const ThemeMapToken& map = resolved.values;
  MenuVisualStyle style = input.colorScheme == AdNavigationMenu::ColorScheme::Dark
                              ? makeDarkStyle(map, input.baseFont, input.componentTokens)
                              : makeLightStyle(map, input.baseFont, input.componentTokens);

  if (input.componentTokens.metrics.indentation.has_value()) {
    style.metrics.indentation = std::max(0, input.componentTokens.metrics.indentation.value());
  } else if (input.componentTokens.metrics.inlineIndent.has_value()) {
    style.metrics.indentation = std::max(0, input.componentTokens.metrics.inlineIndent.value());
  }

  applySemanticStyles(input.semanticStyles, style);

  return style;
}

MenuVisualStyle resolveMenuVisualStyle(const MenuStyleInput& input) {
  const adqt::theme::ResolvedTheme resolved =
      adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());
  return resolveMenuVisualStyle(input, resolved);
}

}  // namespace adqt::widgets::detail
