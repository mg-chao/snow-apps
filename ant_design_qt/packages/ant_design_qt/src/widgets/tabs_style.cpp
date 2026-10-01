#include "tabs_style.h"

#include "theme/theme.h"
#include "theme/theme_color_utils.h"

#include <algorithm>
namespace adqt::widgets::detail {

namespace {

QColor colorOr(const std::optional<QColor>& value, const QColor& fallback) {
  return value && value->isValid() ? *value : fallback;
}

int intOr(const std::optional<int>& value, int fallback, int minimum = 0) {
  return std::max(minimum, value.value_or(fallback));
}

}  // namespace

TabsAppearance resolveTabsAppearance(const AdTabs* tabs, const AdTabs::ComponentTokens& tokens) {
  const adqt::theme::ThemeMapToken theme = adqt::theme::ThemeManager::instance().resolveTheme(tabs);

  TabsAppearance result;
  result.item = colorOr(tokens.colors.itemColor, theme.colorText);
  result.selected = colorOr(tokens.colors.itemSelectedColor, theme.colorPrimary);
  result.hover = colorOr(tokens.colors.itemHoverColor, theme.colorPrimaryHover);
  result.active = colorOr(tokens.colors.itemActiveColor, theme.colorPrimaryActive);
  result.disabled = colorOr(tokens.colors.itemDisabledColor, theme.colorTextDisabled);
  result.inkBar = colorOr(tokens.colors.inkBarColor, theme.colorPrimary);
  result.cardBackground = colorOr(tokens.colors.cardBackground, theme.colorFillAlter);
  result.cardActiveBackground = colorOr(tokens.colors.cardActiveBackground, theme.colorBgContainer);
  result.border = colorOr(tokens.colors.borderColor, theme.colorBorderSecondary);
  result.focusOutline = colorOr(tokens.colors.focusOutline, theme.colorPrimaryBorder);
  result.surface = theme.colorBgContainer;

  const bool compact = theme.controlHeight <= 28.0;
  int itemHeight = qRound(theme.controlHeightLG);
  int verticalPadding = qRound(theme.sizeSM);
  int fontPixels = qRound(theme.fontSize);
  if (tabs->controlSize() == AdTabs::ControlSize::Small) {
    itemHeight = qRound(theme.controlHeight);
    verticalPadding = qRound(theme.sizeXS);
  } else if (tabs->controlSize() == AdTabs::ControlSize::Large) {
    itemHeight = qRound(theme.controlHeightLG + theme.sizeXS);
    verticalPadding = qRound(theme.size);
    fontPixels = qRound(theme.fontSizeLG);
  }
  verticalPadding = intOr(tokens.metrics.verticalItemPadding, verticalPadding);
  if (tabs->type() == AdTabs::Type::Line) {
    itemHeight = std::max(itemHeight, qRound(theme.fontHeight) + verticalPadding * 2);
  } else {
    itemHeight = std::max(intOr(tokens.metrics.cardHeight, itemHeight, 24),
                          qRound(theme.fontHeight) + verticalPadding * 2);
  }

  result.metrics.itemHeight = std::max(24, itemHeight);
  result.metrics.horizontalPadding =
      intOr(tokens.metrics.horizontalItemPadding,
            tabs->type() == AdTabs::Type::Line ? 0 : qRound(theme.size));
  result.metrics.verticalPadding = verticalPadding;
  result.metrics.itemGutter = tabs->tabBarGutter() >= 0
                                  ? tabs->tabBarGutter()
                                  : intOr(tokens.metrics.horizontalItemGutter, 32);
  result.metrics.cardGutter = std::max(1, qRound(theme.sizeXXS / 2.0));
  result.metrics.indicatorThickness =
      intOr(tokens.metrics.indicatorThickness, qRound(theme.lineWidthBold), 1);
  result.metrics.borderWidth = std::max(1, qRound(theme.lineWidth));
  result.metrics.borderRadius = intOr(tokens.metrics.borderRadius, qRound(theme.borderRadius));
  result.metrics.iconSize = intOr(tokens.metrics.iconSize, 16, 8);
  result.metrics.iconGap = intOr(tokens.metrics.iconGap, qRound(theme.sizeXS));
  result.metrics.operationExtent = result.metrics.itemHeight;
  result.metrics.focusOutlineWidth = std::max(2, qRound(theme.lineWidthBold));
  result.metrics.focusOutlineOffset = compact ? 1 : 2;
  result.metrics.font = tabs->font();
  result.metrics.font.setPixelSize(std::max(10, fontPixels));
  result.motionDuration = std::max(0, theme.motionDurationMid);
  if (theme.backgroundOpacity != 1.0) {
    result.cardBackground =
        adqt::theme::applyBackgroundOpacity(result.cardBackground, theme.backgroundOpacity);
    result.cardActiveBackground =
        adqt::theme::applyBackgroundOpacity(result.cardActiveBackground, theme.backgroundOpacity);
    result.surface = adqt::theme::applyBackgroundOpacity(result.surface, theme.backgroundOpacity);
  }
  return result;
}

}  // namespace adqt::widgets::detail
