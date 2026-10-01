#include "theme/theme.h"
#include "widgets/alert_style.h"
#include "widgets/checkbox_style.h"
#include "widgets/color_picker_style.h"
#include "widgets/descriptions_style.h"
#include "widgets/menu_style.h"
#include "widgets/pagination_style.h"
#include "widgets/radio_style.h"
#include "widgets/slider_style.h"
#include "widgets/spin_style.h"
#include "widgets/switch_style.h"
#include "widgets/tabs_style.h"
#include "widgets/tag_style.h"

#include <QLinearGradient>
#include <QApplication>
#include <QImage>
#include <QLayout>
#include <QPainter>

#include <cmath>
#include <stdexcept>

namespace {

using namespace adqt::widgets::detail;

constexpr qreal kBackgroundOpacity = 0.35;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireBackground(const QColor& actual, const QColor& base, const char* message,
                       qreal opacity = kBackgroundOpacity) {
  require(actual.isValid() == base.isValid(), message);
  if (!base.isValid()) {
    return;
  }
  require(actual.red() == base.red() && actual.green() == base.green() &&
              actual.blue() == base.blue() &&
              std::abs(static_cast<qreal>(actual.alphaF()) -
                       static_cast<qreal>(base.alphaF()) * opacity) < 0.001,
          message);
}

void inlineNavigationRespectsOpacity(const adqt::theme::ResolvedTheme& base) {
  using adqt::widgets::AdNavigationMenu;
  MenuStyleInput input;
  input.mode = AdNavigationMenu::Mode::Inline;
  input.colorScheme = base.config.scheme == adqt::theme::ThemeScheme::Dark
                          ? AdNavigationMenu::ColorScheme::Dark
                          : AdNavigationMenu::ColorScheme::Light;
  input.semanticStyles.item.backgroundColor = QColor(50, 80, 120, 170);
  input.semanticStyles.itemContent.backgroundColor = QColor(70, 100, 140, 180);
  input.semanticStyles.subMenuItem.backgroundColor = QColor(90, 120, 150, 190);
  input.semanticStyles.popup.backgroundColor = QColor(20, 40, 70);
  const MenuVisualStyle inlineBase = resolveMenuVisualStyle(input, base);
  for (qreal opacity : {0.0, 0.4, 1.0}) {
    adqt::theme::ResolvedTheme masked = base;
    masked.values.backgroundOpacity = opacity;
    const MenuVisualStyle inlineMasked = resolveMenuVisualStyle(input, masked);
    for (MenuStateStyle MenuVisualStyle::* state :
         {&MenuVisualStyle::hover, &MenuVisualStyle::active, &MenuVisualStyle::selected,
          &MenuVisualStyle::dangerHover, &MenuVisualStyle::dangerActive,
          &MenuVisualStyle::dangerSelected}) {
      requireBackground((inlineMasked.*state).background, (inlineBase.*state).background,
                        "inline navigation interactive fills must use final background opacity",
                        opacity);
      require((inlineMasked.*state).text == (inlineBase.*state).text,
              "inline navigation ink must retain its contrast");
    }
    require(inlineMasked.menuBackground == inlineBase.menuBackground &&
                inlineMasked.normal.background == inlineBase.normal.background &&
                inlineMasked.disabled.background == inlineBase.disabled.background &&
                inlineMasked.danger.background == inlineBase.danger.background &&
                inlineMasked.subMenuBackground == inlineBase.subMenuBackground &&
                inlineMasked.popupBackground == inlineBase.popupBackground &&
                inlineMasked.borderColor == inlineBase.borderColor &&
                inlineMasked.popupBorderColor == inlineBase.popupBorderColor,
            "inline navigation source surfaces and detached popup fills must stay unchanged");
    MenuStyleInput popupInput = input;
    popupInput.mode = AdNavigationMenu::Mode::Vertical;
    const MenuVisualStyle popupBase = resolveMenuVisualStyle(popupInput, base);
    const MenuVisualStyle popupMasked = resolveMenuVisualStyle(popupInput, masked);
    for (MenuStateStyle MenuVisualStyle::* state :
         {&MenuVisualStyle::normal, &MenuVisualStyle::hover, &MenuVisualStyle::active,
          &MenuVisualStyle::selected, &MenuVisualStyle::dangerHover, &MenuVisualStyle::dangerActive,
          &MenuVisualStyle::dangerSelected}) {
      require((popupMasked.*state).background == (popupBase.*state).background &&
                  (popupMasked.*state).text == (popupBase.*state).text,
              "detached vertical menu item surfaces and ink must keep their original appearance");
    }
  }
}

void indicatorsRespectOpacity(const adqt::theme::ResolvedTheme& base,
                              const adqt::theme::ResolvedTheme& masked) {
  const QColor customFill(30, 70, 120, 180);
  RadioStyleInput radioInput;
  radioInput.componentTokens.colors.indicatorFillColor = customFill;
  const RadioVisualStyle radioBase = resolveRadioVisualStyle(radioInput, base);
  const RadioVisualStyle radioMasked = resolveRadioVisualStyle(radioInput, masked);
  for (const RadioDotStateStyle* state :
       {&radioMasked.normal, &radioMasked.hover, &radioMasked.active, &radioMasked.checked,
        &radioMasked.checkedHover, &radioMasked.disabled, &radioMasked.checkedDisabled}) {
    requireBackground(state->backgroundColor, customFill,
                      "every radio indicator state must apply opacity after token overrides");
  }
  require(radioMasked.checked.dotColor == radioBase.checked.dotColor &&
              radioMasked.normal.labelColor == radioBase.normal.labelColor &&
              radioMasked.checked.borderColor == radioBase.checked.borderColor,
          "radio foreground and borders must retain their contrast");

  RadioButtonStyleInput buttonInput;
  buttonInput.buttonStyle = adqt::widgets::AdRadio::ButtonStyle::Solid;
  const RadioButtonVisualStyle buttonBase = resolveRadioButtonVisualStyle(buttonInput, base);
  const RadioButtonVisualStyle buttonMasked = resolveRadioButtonVisualStyle(buttonInput, masked);
  requireBackground(buttonMasked.normal.backgroundColor, buttonBase.normal.backgroundColor,
                    "unselected radio buttons must use background opacity");
  requireBackground(buttonMasked.checkedActive.backgroundColor,
                    buttonBase.checkedActive.backgroundColor,
                    "pressed solid radio buttons must use background opacity");
  requireBackground(buttonMasked.checkedDisabled.backgroundColor,
                    buttonBase.checkedDisabled.backgroundColor,
                    "disabled radio buttons must use background opacity");
  require(buttonMasked.checked.textColor == buttonBase.checked.textColor &&
              buttonMasked.checked.borderColor == buttonBase.checked.borderColor,
          "radio button text and borders must retain their contrast");

  CheckboxStyleInput checkboxInput;
  checkboxInput.componentTokens.colors.indicatorFillColor = customFill;
  const CheckboxVisualStyle checkboxBase = resolveCheckboxVisualStyle(checkboxInput, base);
  const CheckboxVisualStyle checkboxMasked = resolveCheckboxVisualStyle(checkboxInput, masked);
  for (const CheckboxStateStyle* state :
       {&checkboxMasked.normal, &checkboxMasked.hover, &checkboxMasked.checked,
        &checkboxMasked.checkedHover, &checkboxMasked.indeterminate,
        &checkboxMasked.indeterminateHover, &checkboxMasked.disabled,
        &checkboxMasked.checkedDisabled, &checkboxMasked.indeterminateDisabled}) {
    requireBackground(state->backgroundColor, customFill,
                      "every checkbox state must apply opacity after token overrides");
  }
  require(checkboxMasked.checked.markColor == checkboxBase.checked.markColor &&
              checkboxMasked.checked.labelColor == checkboxBase.checked.labelColor &&
              checkboxMasked.checked.borderColor == checkboxBase.checked.borderColor,
          "checkbox foreground and borders must retain their contrast");
}

void switchAndSliderRespectOpacity(const adqt::theme::ResolvedTheme& base,
                                   const adqt::theme::ResolvedTheme& masked) {
  SwitchAppearanceInput switchInput;
  switchInput.componentTokens.colors.checkedTrack = QColor(40, 100, 210, 180);
  for (bool disabled : {false, true}) {
    switchInput.disabled = disabled;
    const SwitchAppearance switchBase = resolveSwitchAppearance(switchInput, base);
    const SwitchAppearance switchMasked = resolveSwitchAppearance(switchInput, masked);
    requireBackground(switchMasked.uncheckedTrackColor, switchBase.uncheckedTrackColor,
                      "unchecked switch tracks must use opacity in enabled and disabled states");
    requireBackground(switchMasked.checkedTrackHoverColor, switchBase.checkedTrackHoverColor,
                      "checked switch tracks must compose skin opacity with disabled opacity");
    require(switchMasked.thumbColor == switchBase.thumbColor &&
                switchMasked.contentColor == switchBase.contentColor &&
                switchMasked.loadingIndicatorColor == switchBase.loadingIndicatorColor,
            "switch thumbs, labels and loading indicators must retain their contrast");
  }

  SliderStyleInput sliderInput;
  sliderInput.deferSemanticStyles = true;
  sliderInput.componentTokens.railBg = QColor(20, 80, 140, 170);
  sliderInput.componentTokens.trackBgDisabled = QColor(90, 120, 140, 150);
  const SliderVisualStyle sliderBase = resolveSliderVisualStyle(sliderInput, base);
  const SliderVisualStyle sliderRaw = resolveSliderVisualStyle(sliderInput, masked);
  adqt::widgets::AdMultiSlider::SemanticStyles semantic;
  semantic.root.backgroundColor = QColor(220, 230, 240, 200);
  semantic.track.backgroundColor = QColor(60, 110, 170, 180);
  semantic.rail.brush = QBrush(QColor(10, 40, 80, 190));
  const SliderVisualStyle sliderMasked = applySliderSemanticStyles(sliderRaw, semantic, false);
  requireBackground(sliderMasked.rootBg, *semantic.root.backgroundColor,
                    "slider root semantic overrides must use opacity");
  requireBackground(sliderMasked.railHoverBg, sliderBase.railHoverBg,
                    "hovered slider rails must use opacity");
  requireBackground(sliderMasked.trackBg, *semantic.track.backgroundColor,
                    "slider track semantic overrides must use opacity");
  requireBackground(sliderMasked.railBrush.color(), semantic.rail.brush->color(),
                    "solid slider semantic brushes must use opacity");
  require(sliderMasked.surfaceBg == sliderBase.surfaceBg &&
              sliderMasked.handleColor == sliderBase.handleColor &&
              sliderMasked.tooltipBg == sliderBase.tooltipBg,
          "slider handles and tooltips must retain their contrast");
  const SliderVisualStyle repeated = applySliderSemanticStyles(sliderRaw, semantic, false);
  require(repeated.railBg == sliderMasked.railBg && repeated.trackBg == sliderMasked.trackBg,
          "refreshing cached slider semantic styles must not multiply opacity again");
  const SliderVisualStyle disabled = applySliderSemanticStyles(sliderRaw, semantic, true);
  requireBackground(disabled.trackBg, sliderBase.trackBgDisabled,
                    "disabled slider tracks must apply background opacity once");

  QLinearGradient gradient(0, 0, 1, 0);
  gradient.setColorAt(0, Qt::red);
  gradient.setColorAt(1, Qt::blue);
  semantic.rail.brush = QBrush(gradient);
  const SliderVisualStyle content = applySliderSemanticStyles(sliderRaw, semantic, false);
  require(content.railBrush == *semantic.rail.brush,
          "color-channel gradient content must preserve its actual colors");
}

void statusAndPickerRespectOpacity(const adqt::theme::ResolvedTheme& base,
                                   const adqt::theme::ResolvedTheme& masked) {
  TagStyleInput tagInput;
  tagInput.semanticStyles.root.backgroundColor = QColor(50, 90, 130, 170);
  tagInput.semanticStyles.closeIcon.backgroundColor = QColor(80, 100, 140, 190);
  const TagVisualStyle tagBase = resolveTagVisualStyle(tagInput, base);
  const TagVisualStyle tagMasked = resolveTagVisualStyle(tagInput, masked);
  requireBackground(tagMasked.backgroundColor, tagBase.backgroundColor,
                    "tag root overrides must use background opacity");
  requireBackground(tagMasked.closeHoverBackground, tagBase.closeHoverBackground,
                    "tag close hover overrides must use background opacity");
  require(tagMasked.contentColor == tagBase.contentColor &&
              tagMasked.borderColor == tagBase.borderColor,
          "tag text and borders must retain their contrast");

  AlertStyleInput alertInput;
  alertInput.hasPaletteOverride = true;
  alertInput.palette.setColor(QPalette::Window, QColor(80, 110, 150, 170));
  alertInput.palette.setColor(QPalette::AlternateBase, QColor(60, 90, 130, 150));
  const AlertVisualStyle alertBase = resolveAlertVisualStyle(alertInput, base);
  const AlertVisualStyle alertMasked = resolveAlertVisualStyle(alertInput, masked);
  requireBackground(alertMasked.background, alertBase.background,
                    "alert palette overrides must use background opacity");
  requireBackground(alertMasked.closeButtonHoverBackground, alertBase.closeButtonHoverBackground,
                    "alert close hover backgrounds must use background opacity");
  require(alertMasked.textColor == alertBase.textColor &&
              alertMasked.iconColor == alertBase.iconColor &&
              alertMasked.border == alertBase.border,
          "alert text, icons and borders must retain their contrast");

  ColorPickerStyleInput pickerInput;
  pickerInput.componentTokens.triggerBackground = QColor(210, 220, 230, 170);
  const ColorPickerVisualStyle pickerBase = resolveColorPickerVisualStyle(pickerInput, base);
  const ColorPickerVisualStyle pickerMasked = resolveColorPickerVisualStyle(pickerInput, masked);
  requireBackground(pickerMasked.triggerBackground, pickerBase.triggerBackground,
                    "color picker trigger overrides must use background opacity");
  requireBackground(pickerMasked.triggerBackgroundDisabled, pickerBase.triggerBackgroundDisabled,
                    "disabled color picker triggers must use background opacity");
  require(pickerMasked.triggerText == pickerBase.triggerText &&
              pickerMasked.triggerBorder == pickerBase.triggerBorder &&
              pickerMasked.panelBackground == pickerBase.panelBackground &&
              pickerMasked.segmentedItemBackground == pickerBase.segmentedItemBackground &&
              pickerMasked.transparentCellA == pickerBase.transparentCellA &&
              pickerMasked.transparentCellB == pickerBase.transparentCellB &&
              pickerMasked.invalidSwatchFill == pickerBase.invalidSwatchFill,
          "color picker text, popup surfaces and color swatches must keep their actual colors");
}

void paginationAndSpinRespectOpacity(const adqt::theme::ResolvedTheme& base,
                                     const adqt::theme::ResolvedTheme& masked) {
  PaginationStyleInput paginationInput;
  paginationInput.semanticStyles.item.backgroundColor = QColor(60, 100, 150, 170);
  const PaginationVisualStyle paginationBase = resolvePaginationVisualStyle(paginationInput, base);
  const PaginationVisualStyle paginationMasked =
      resolvePaginationVisualStyle(paginationInput, masked);
  requireBackground(paginationMasked.itemBackground, paginationBase.itemBackground,
                    "pagination item overrides must use background opacity");
  requireBackground(paginationMasked.itemHoverBackground, paginationBase.itemHoverBackground,
                    "pagination hover backgrounds must use background opacity");
  requireBackground(paginationMasked.itemPressedBackground, paginationBase.itemPressedBackground,
                    "pagination pressed backgrounds must use background opacity");
  requireBackground(paginationMasked.activeDisabledBackground,
                    paginationBase.activeDisabledBackground,
                    "disabled pagination backgrounds must use background opacity");
  require(paginationMasked.text == paginationBase.text &&
              paginationMasked.activeBorder == paginationBase.activeBorder,
          "pagination text and borders must retain their contrast");

  SpinStyleInput spinInput;
  spinInput.semanticStyles.section.backgroundColor = QColor(30, 60, 100, 180);
  const SpinVisualStyle spinBase = resolveSpinVisualStyle(spinInput, base);
  const SpinVisualStyle spinMasked = resolveSpinVisualStyle(spinInput, masked);
  requireBackground(spinMasked.sectionBackground, spinBase.sectionBackground,
                    "spinner section overrides must use background opacity");
  requireBackground(spinMasked.containerOverlay, spinBase.containerOverlay,
                    "spinner content overlays must use background opacity");
  requireBackground(spinMasked.progressTrack, spinBase.progressTrack,
                    "spinner progress tracks must use background opacity");
  require(spinMasked.indicator == spinBase.indicator &&
              spinMasked.description == spinBase.description &&
              spinMasked.fullscreenMask == spinBase.fullscreenMask,
          "spinner indicators, text and fullscreen masks must retain their contrast");
}

void layoutComponentsRespectScopedOpacity() {
  QWidget host;
  QPalette inheritedPalette = host.palette();
  const QColor inheritedBackground(50, 80, 110, 230);
  inheritedPalette.setColor(QPalette::Window, inheritedBackground);
  host.setPalette(inheritedPalette);
  adqt::widgets::AdTabs tabs(&host);
  adqt::widgets::AdDescriptions descriptions(&host);
  require(
      resolveDescriptionsAppearance(&descriptions, {}, {}).rootBackground == inheritedBackground,
      "unmasked descriptions must preserve an inherited caller palette");
  const QColor customBackground(60, 90, 120, 220);
  QPalette descriptionPalette = descriptions.palette();
  descriptionPalette.setColor(QPalette::Window, customBackground);
  descriptions.setPalette(descriptionPalette);
  adqt::widgets::AdTabs::ComponentTokens tabTokens;
  tabTokens.colors.cardBackground = QColor(80, 120, 160, 180);
  adqt::widgets::AdDescriptions::ComponentTokens descriptionTokens;
  descriptionTokens.colors.labelBackground = QColor(70, 110, 150, 160);
  const TabsAppearance tabsBase = resolveTabsAppearance(&tabs, tabTokens);
  const DescriptionsAppearance descriptionsBase =
      resolveDescriptionsAppearance(&descriptions, descriptionTokens, {});
  require(descriptionsBase.rootBackground == customBackground,
          "unmasked descriptions must preserve their explicit caller palette");

  adqt::theme::ThemeOverride scope;
  scope.backgroundOpacity = kBackgroundOpacity;
  adqt::theme::ThemeManager::instance().setScopeOverride(&host, scope);
  const TabsAppearance tabsMasked = resolveTabsAppearance(&tabs, tabTokens);
  const DescriptionsAppearance descriptionsMasked =
      resolveDescriptionsAppearance(&descriptions, descriptionTokens, {});
  requireBackground(tabsMasked.cardBackground, tabsBase.cardBackground,
                    "tab card overrides must use scoped background opacity");
  requireBackground(tabsMasked.cardActiveBackground, tabsBase.cardActiveBackground,
                    "selected tab backgrounds must use scoped background opacity");
  require(tabsMasked.selected == tabsBase.selected && tabsMasked.border == tabsBase.border,
          "tab text and borders must retain their contrast");
  requireBackground(descriptionsMasked.rootBackground, descriptionsBase.rootBackground,
                    "description surfaces must use scoped background opacity");
  requireBackground(descriptionsMasked.labelBackground, descriptionsBase.labelBackground,
                    "description label overrides must use scoped background opacity");
  descriptions.setColumn(1);
  descriptions.setColumn(2);
  const DescriptionsAppearance descriptionsRepeated =
      resolveDescriptionsAppearance(&descriptions, descriptionTokens, {});
  require(descriptionsRepeated.rootBackground == descriptionsMasked.rootBackground,
          "description rebuilds must not multiply the palette background opacity again");
  require(descriptions.palette().color(QPalette::Window) == customBackground,
          "description rebuilds must not overwrite the caller's raw palette");
  require(descriptionsMasked.labelColor == descriptionsBase.labelColor &&
              descriptionsMasked.borderColor == descriptionsBase.borderColor,
          "description text and borders must retain their contrast");

  adqt::widgets::AdDescriptions::SemanticStyles semantic;
  const QColor semanticBackground(200, 210, 220);
  semantic.root.backgroundColor = semanticBackground;
  descriptions.setSemanticStyles(semantic);
  descriptions.addItem(QString(), QString());
  descriptions.resize(180, 90);
  descriptions.ensurePolished();
  descriptions.layout()->activate();
  QImage image(descriptions.size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::black);
  QPainter painter(&image);
  descriptions.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
  painter.end();
  const QColor painted = image.pixelColor(140, 40);
  require(std::abs(painted.red() - semanticBackground.red() * kBackgroundOpacity) <= 2.0 &&
              std::abs(painted.green() - semanticBackground.green() * kBackgroundOpacity) <= 2.0 &&
              std::abs(painted.blue() - semanticBackground.blue() * kBackgroundOpacity) <= 2.0,
          "description root, surface and inherited cell backgrounds must composite only once");
  require(descriptions.palette().color(QPalette::Window) == customBackground,
          "description semantic backgrounds must preserve the caller's raw palette");
  descriptions.resetSemanticStyles();
  adqt::theme::ThemeManager::instance().clearScopeOverride(&host);
  require(resolveDescriptionsAppearance(&descriptions, {}, {}).rootBackground == customBackground,
          "clearing scoped opacity must restore the caller's original description background");
}

class AuxiliaryPaintedSurface final : public QWidget {
 public:
  const QColor background = QColor(25, 50, 85);

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), background);
  }
};

void requirePaintedBackground(const QColor& pixel, const QColor& fill, const QColor& background,
                              qreal opacity, const char* message) {
  const qreal alpha = static_cast<qreal>(fill.alphaF()) * opacity;
  require(
      std::abs(pixel.red() - (fill.red() * alpha + background.red() * (1.0 - alpha))) <= 2.0 &&
          std::abs(pixel.green() - (fill.green() * alpha + background.green() * (1.0 - alpha))) <=
              2.0 &&
          std::abs(pixel.blue() - (fill.blue() * alpha + background.blue() * (1.0 - alpha))) <= 2.0,
      message);
}

void cachedAuxiliaryPaintersFollowOpacityChanges() {
  using namespace adqt::widgets;
  auto& manager = adqt::theme::ThemeManager::instance();
  struct RestoreTheme {
    adqt::theme::ThemeConfig config = adqt::theme::ThemeManager::instance().config();
    ~RestoreTheme() { adqt::theme::ThemeManager::instance().setConfig(config); }
  } restoreTheme;
  auto config = restoreTheme.config;
  config.backgroundOpacity = 1.0;
  config.motion = false;
  manager.setConfig(config);

  AuxiliaryPaintedSurface surface;
  surface.resize(260, 180);
  const QColor fill(195, 165, 125, 190);
  AdAlert alert(&surface);
  alert.setIconMode(AdAlert::IconMode::Hidden);
  QPalette alertPalette = alert.palette();
  alertPalette.setColor(QPalette::Window, fill);
  alertPalette.setColor(QPalette::Mid, Qt::transparent);
  alert.setPalette(alertPalette);
  alert.setGeometry(20, 20, 180, 40);

  AdSlider slider(&surface);
  AdMultiSlider::ComponentTokens sliderTokens;
  sliderTokens.railSize = 12;
  sliderTokens.railBg = fill;
  sliderTokens.railHoverBg = fill;
  slider.setComponentTokens(sliderTokens);
  slider.setTooltipEnabled(false);
  slider.setValue(slider.minimum());
  slider.setGeometry(20, 75, 180, 40);

  AdColorPicker picker(&surface);
  AdColorPicker::ComponentTokens pickerTokens;
  pickerTokens.triggerMinWidth = 80;
  pickerTokens.triggerRadius = 0;
  pickerTokens.swatchSize = 10;
  pickerTokens.triggerBackground = fill;
  pickerTokens.triggerBorderColor = Qt::transparent;
  pickerTokens.triggerBorderHoverColor = Qt::transparent;
  picker.setComponentTokens(pickerTokens);
  picker.setGeometry(20, 130, 100, 32);
  auto* trigger = picker.findChild<QWidget*>(QStringLiteral("ad-color-picker-trigger-frame"));
  require(trigger != nullptr, "color picker trigger must exist for the live opacity check");

  const auto verify = [&](qreal opacity) {
    QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    surface.render(&image);
    requirePaintedBackground(image.pixelColor(alert.mapTo(&surface, QPoint(140, 20))), fill,
                             surface.background, opacity,
                             "cached alert paint must follow live opacity-only theme changes");
    requirePaintedBackground(image.pixelColor(slider.mapTo(&surface, QPoint(140, 20))), fill,
                             surface.background, opacity,
                             "cached slider rails must follow live opacity-only theme changes");
    requirePaintedBackground(
        image.pixelColor(trigger->mapTo(&surface, QPoint(trigger->width() / 2, 2))), fill,
        surface.background, opacity,
        "cached color picker triggers must follow live opacity-only theme changes");
  };

  verify(1.0);
  adqt::theme::ThemeOverride scope;
  for (qreal opacity : {0.0, 0.4, 1.0}) {
    scope.backgroundOpacity = opacity;
    manager.setScopeOverride(&surface, scope);
    verify(opacity);
  }
  scope.backgroundOpacity = 0.4;
  manager.setScopeOverride(&surface, scope);
  verify(0.4);
  manager.clearScopeOverride(&surface);
  verify(1.0);

  for (qreal opacity : {0.0, 0.4, 1.0}) {
    config.backgroundOpacity = opacity;
    manager.setConfig(config);
    verify(opacity);
  }
}

}  // namespace

void auxiliaryBackgroundOpacityTests() {
  for (adqt::theme::ThemeScheme scheme :
       {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
    const adqt::theme::ResolvedTheme base =
        adqt::theme::makeResolvedTheme(adqt::theme::makeTheme(scheme));
    adqt::theme::ResolvedTheme masked = base;
    masked.values.backgroundOpacity = kBackgroundOpacity;
    indicatorsRespectOpacity(base, masked);
    switchAndSliderRespectOpacity(base, masked);
    statusAndPickerRespectOpacity(base, masked);
    paginationAndSpinRespectOpacity(base, masked);
    inlineNavigationRespectsOpacity(base);
  }
  layoutComponentsRespectScopedOpacity();
  cachedAuxiliaryPaintersFollowOpacityChanges();
}
