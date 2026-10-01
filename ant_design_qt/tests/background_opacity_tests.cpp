#include "theme/theme_color_utils.h"
#include "theme/theme_manager.h"
#include "theme/theme_types.h"
#include "widgets/button.h"
#include "widgets/button_style.h"
#include "widgets/detail/button_grouping.h"
#include "widgets/input_number.h"
#include "widgets/input_number_style.h"
#include "widgets/input_style.h"
#include "widgets/navigation_menu.h"
#include "widgets/radio.h"
#include "widgets/select_style.h"
#include "widgets/switch.h"
#include "widgets/tag.h"

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QFocusEvent>
#include <QEnterEvent>
#include <QToolButton>
#include <QPainter>
#include <QStandardItemModel>
#include <QTreeView>
#include <QWidget>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

void auxiliaryBackgroundOpacityTests();
void datePickerBackgroundOpacityTests();

namespace {

using adqt::theme::ThemeConfig;
using adqt::theme::ThemeOverride;
using adqt::theme::ThemeScheme;
using namespace adqt::widgets;
using namespace adqt::widgets::detail;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireMasked(const QColor& masked, const QColor& original, double opacity) {
  require(masked.red() == original.red() && masked.green() == original.green() &&
              masked.blue() == original.blue(),
          "background opacity must preserve the resolved fill color");
  require(std::abs(static_cast<qreal>(masked.alphaF()) -
                   static_cast<qreal>(original.alphaF()) * opacity) < 1.0 / 255.0,
          "background opacity must multiply the resolved fill alpha exactly once");
}

void themeOpacityContract() {
  ThemeConfig config = adqt::theme::defaultThemeConfig();
  require(config.backgroundOpacity == 1.0,
          "default themes must preserve opaque component backgrounds");
  ThemeOverride overrideValue;
  overrideValue.backgroundOpacity = 0.4;
  require(!adqt::theme::isEmptyThemeOverride(overrideValue),
          "background opacity must participate in scope override equality");
  const ThemeConfig merged = adqt::theme::mergeThemeConfig(config, overrideValue);
  require(!(merged == config) && merged.backgroundOpacity == 0.4,
          "background opacity must merge into scoped theme config");
  const auto resolved = adqt::theme::makeResolvedTheme(merged);
  require(resolved.theme.metrics.backgroundOpacity == 0.4 &&
              resolved.values.backgroundOpacity == 0.4 &&
              adqt::theme::makeResolvedTheme(resolved.theme).config.backgroundOpacity == 0.4,
          "theme conversion must preserve component background opacity");
  for (const double opacity : {-1.0, 2.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
    config.backgroundOpacity = opacity;
    const double expected = std::isfinite(opacity) ? (opacity < 0 ? 0.0 : 1.0) : 1.0;
    require(adqt::theme::resolveThemeValues(config).backgroundOpacity == expected,
            "resolved background opacity must be finite and clamped to the unit interval");
  }
  require(!adqt::theme::applyBackgroundOpacity(QColor(), 0.4).isValid(),
          "masking an invalid fill must preserve its fallback meaning");
  const QColor translucent(80, 120, 160, 100);
  requireMasked(adqt::theme::applyBackgroundOpacity(translucent, 0.4), translucent, 0.4);
}

void scopedOpacityContract() {
  auto& manager = adqt::theme::ThemeManager::instance();
  const auto originalConfig = manager.config();
  QWidget scope;
  QWidget child(&scope);
  QWidget nested(&child);
  QWidget nestedChild(&nested);
  QWidget dialog(&nested, Qt::Dialog);
  QWidget dialogChild(&dialog);
  QWidget unrelated;
  require(manager.backgroundOpacity() == 1.0 && manager.backgroundOpacity(&child) == 1.0,
          "lightweight queries without scopes must preserve the default opacity");
  ThemeOverride overrideValue;
  overrideValue.backgroundOpacity = 0.4;
  overrideValue.appFont = QFont(QStringLiteral("background-opacity-font-probe"));
  manager.setScopeOverride(&scope, overrideValue);
  require(manager.resolveTheme(&child).backgroundOpacity == 0.4 &&
              manager.backgroundOpacity(&child) == 0.4,
          "controls and custom painters must inherit their parent scope background opacity");
  require(manager.resolveTheme(&unrelated).backgroundOpacity == 1.0 &&
              manager.backgroundOpacity(&unrelated) == 1.0,
          "component background opacity must remain local to its scope");
  ThemeOverride nestedOverride;
  nestedOverride.backgroundOpacity = 0.7;
  manager.setScopeOverride(&nested, nestedOverride);
  require(manager.backgroundOpacity(&nestedChild) == 0.7 &&
              manager.resolveTheme(&nestedChild).backgroundOpacity == 0.7,
          "nested scopes must override parent opacity for custom paints and controls alike");
  require(manager.backgroundOpacity(&dialogChild) == 1.0 &&
              manager.resolveTheme(&dialogChild).backgroundOpacity == 1.0 &&
              manager.resolveTheme(&dialogChild, &nestedChild).backgroundOpacity == 1.0,
          "owned dialogs must not inherit the skin of their owner window");
  require(manager.resolveTheme(&dialogChild).appFont.family() ==
              QStringLiteral("background-opacity-font-probe"),
          "window background boundaries must preserve other scoped theme configuration");
  ThemeOverride dialogOverride;
  dialogOverride.backgroundOpacity = 0.6;
  manager.setScopeOverride(&dialog, dialogOverride);
  require(manager.backgroundOpacity(&dialogChild) == 0.6 &&
              manager.resolveTheme(&dialogChild).backgroundOpacity == 0.6,
          "a dialog must still honor opacity configured for its own surface");
  manager.clearScopeOverride(&dialog);
  manager.clearScopeOverride(&nested);
  require(manager.backgroundOpacity(&nestedChild) == 0.4,
          "clearing a nested scope must restore the parent surface opacity");
  manager.clearScopeOverride(&scope);
  require(manager.resolveTheme(&child).backgroundOpacity == 1.0 &&
              manager.backgroundOpacity(&child) == 1.0,
          "clearing the scope must restore normal component opacity");
  for (const double opacity : {0.3, -1.0, 2.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
    auto config = originalConfig;
    config.backgroundOpacity = opacity;
    manager.setConfig(config);
    require(manager.backgroundOpacity(&child) == manager.resolveTheme(&child).backgroundOpacity,
            "lightweight queries must use the same normalization as resolved themes");
  }
  manager.setConfig(originalConfig);
}

void buttonFills(const adqt::theme::ResolvedTheme& opaque, const adqt::theme::ResolvedTheme& masked,
                 double opacity) {
  for (const auto variant :
       {AdButton::ButtonStyle::Solid, AdButton::ButtonStyle::Outline, AdButton::ButtonStyle::Dashed,
        AdButton::ButtonStyle::Tonal, AdButton::ButtonStyle::Text, AdButton::ButtonStyle::Link,
        AdButton::ButtonStyle::GhostOutline}) {
    for (const bool joined : {false, true}) {
      ButtonStyleInput input;
      input.buttonStyle = variant;
      input.joinsEdges = joined;
      const auto original = resolveButtonVisualStyle(input, opaque);
      const auto result = resolveButtonVisualStyle(input, masked);
      for (const auto member :
           {&ButtonVisualStyle::normal, &ButtonVisualStyle::hover, &ButtonVisualStyle::active,
            &ButtonVisualStyle::checked, &ButtonVisualStyle::disabled}) {
        requireMasked((result.*member).background, (original.*member).background, opacity);
        require((result.*member).text == (original.*member).text &&
                    (result.*member).border == (original.*member).border &&
                    (result.*member).shadow == (original.*member).shadow,
                "button text, borders and shadows must keep their original contrast");
      }
    }
  }
}

void selectFills(const adqt::theme::ResolvedTheme& opaque, const adqt::theme::ResolvedTheme& masked,
                 double opacity) {
  for (const auto variant : {AdSelect::Variant::Outlined, AdSelect::Variant::Filled,
                             AdSelect::Variant::Borderless, AdSelect::Variant::Underlined}) {
    for (const bool disabled : {false, true}) {
      SelectStyleInput input;
      input.variant = variant;
      input.disabled = disabled;
      input.semanticStyles.selector.backgroundColor = QColor(90, 120, 160, 180);
      input.semanticStyles.tag.backgroundColor = QColor(110, 100, 150, 100);
      const auto original = resolveSelectVisualStyle(input, opaque);
      const auto result = resolveSelectVisualStyle(input, masked);
      for (const auto member :
           {&SelectVisualStyle::selectorBg, &SelectVisualStyle::selectorHoverBg,
            &SelectVisualStyle::selectorActiveBg, &SelectVisualStyle::disabledBg,
            &SelectVisualStyle::tagBg, &SelectVisualStyle::clearBg}) {
        requireMasked(result.*member, original.*member, opacity);
      }
      require(result.popupBg == original.popupBg && result.popupBg.alpha() == 255 &&
                  result.optionHoverBg == original.optionHoverBg &&
                  result.optionSelectedBg == original.optionSelectedBg,
              "detached select popup fills must stay opaque and preserve their colors");
      require(result.selectorTextColor == original.selectorTextColor &&
                  result.selectorBorderColor == original.selectorBorderColor &&
                  result.suffixColor == original.suffixColor,
              "select text, borders and icons must keep their original contrast");
    }
  }
}

void inputFills(const adqt::theme::ResolvedTheme& opaque, const adqt::theme::ResolvedTheme& masked,
                double opacity) {
  for (const auto variant : {AdLineEdit::Variant::Outlined, AdLineEdit::Variant::Filled,
                             AdLineEdit::Variant::Borderless, AdLineEdit::Variant::Underlined}) {
    for (const bool disabled : {false, true}) {
      InputStyleInput input;
      input.variant = variant;
      input.disabled = disabled;
      const auto original = resolveInputVisualStyle(input, opaque);
      const auto result = resolveInputVisualStyle(input, masked);
      for (const auto member :
           {&InputVisualStyle::selectorBg, &InputVisualStyle::selectorHoverBg,
            &InputVisualStyle::selectorActiveBg, &InputVisualStyle::disabledBg}) {
        requireMasked(result.*member, original.*member, opacity);
      }
      require(result.selectorTextColor == original.selectorTextColor &&
                  result.selectorBorderColor == original.selectorBorderColor &&
                  result.suffixActionColor == original.suffixActionColor,
              "input text, borders and actions must keep their original contrast");
    }
  }
}

void numberFills(const adqt::theme::ResolvedTheme& opaque, const adqt::theme::ResolvedTheme& masked,
                 double opacity) {
  for (const auto variant :
       {AdInputNumber::Variant::Outlined, AdInputNumber::Variant::Filled,
        AdInputNumber::Variant::Borderless, AdInputNumber::Variant::Underlined}) {
    for (const bool disabled : {false, true}) {
      InputNumberStyleInput input;
      input.variant = variant;
      input.disabled = disabled;
      input.appearanceOverrides.input.backgroundColor = QColor(100, 150, 120, 180);
      input.appearanceOverrides.actions.backgroundColor = QColor(120, 130, 180, 200);
      const auto original = resolveInputNumberVisualStyle(input, opaque);
      const auto result = resolveInputNumberVisualStyle(input, masked);
      for (const auto member :
           {&InputNumberVisualStyle::selectorBg, &InputNumberVisualStyle::selectorHoverBg,
            &InputNumberVisualStyle::selectorActiveBg, &InputNumberVisualStyle::disabledBg,
            &InputNumberVisualStyle::handleBg, &InputNumberVisualStyle::handleActiveBg}) {
        requireMasked(result.*member, original.*member, opacity);
      }
      require(result.selectorTextColor == original.selectorTextColor &&
                  result.selectorBorderColor == original.selectorBorderColor &&
                  result.handleIconColor == original.handleIconColor,
              "number input text, borders and icons must keep their original contrast");
    }
  }
}

class PaintedSurface : public QWidget {
 public:
  QColor background = QColor(30, 90, 150);

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), background);
  }
};

class PaintObserver final : public QObject {
 public:
  int paints = 0;
  int paletteChanges = 0;

  void reset() {
    paints = 0;
    paletteChanges = 0;
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Paint) {
      ++paints;
    } else if (event->type() == QEvent::PaletteChange) {
      ++paletteChanges;
    }
    return QObject::eventFilter(watched, event);
  }
};

void flushWidgetUpdates() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
}

QWidget& prepareObservedControl(QWidget& control) { return control; }

QWidget& prepareObservedControl(AdNavigationMenu& menu) {
  menu.setGeometry(20, 20, 140, 48);
  menu.setMode(AdNavigationMenu::Mode::Inline);
  auto* model = new QStandardItemModel(&menu);
  auto* item = new QStandardItem(QStringLiteral("Dashboard"));
  item->setData(QStringLiteral("dashboard"), AdNavigationMenu::StableIdRole);
  item->setData(static_cast<int>(AdNavigationMenu::NodeKind::Action),
                AdNavigationMenu::NodeKindRole);
  model->appendRow(item);
  menu.setModel(model);
  const QModelIndex index = model->index(0, 0);
  menu.setCurrentIndex(index);
  menu.selectionModel()->select(index,
                                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  auto* view = menu.findChild<QTreeView*>(QStringLiteral("AdNavigationMenu-inline-view"));
  require(view && view->viewport(), "inline navigation menus must have an item viewport");
  view->setFocusPolicy(Qt::NoFocus);
  return *view->viewport();
}

template <typename Control>
bool controlRepaintsForOpacityOnlyChanges(const char* controlName) {
  auto& manager = adqt::theme::ThemeManager::instance();
  ThemeConfig config = adqt::theme::defaultThemeConfig();
  config.backgroundOpacity = 0.3;
  manager.setConfig(config);
  QWidget surface;
  surface.resize(180, 80);
  ThemeOverride overrideValue;
  overrideValue.backgroundOpacity = 0.3;
  manager.setScopeOverride(&surface, overrideValue);
  Control control(&surface);
  control.setGeometry(20, 20, 120, 32);
  control.setFocusPolicy(Qt::NoFocus);
  QWidget& observedControl = prepareObservedControl(control);
  PaintObserver observer;
  observedControl.installEventFilter(&observer);
  surface.show();
  flushWidgetUpdates();
  require(observer.paints > 0, "live opacity regressions require a visible, painted control");
  observer.reset();
  flushWidgetUpdates();
  require(observer.paints == 0, "the control must be idle before changing theme opacity");

  bool passed = true;
  const auto checkRepaint = [&](const char* change, auto mutateTheme, bool paletteMustStayEqual) {
    const QPalette paletteBefore = observedControl.palette();
    observer.reset();
    mutateTheme();
    flushWidgetUpdates();
    if (paletteMustStayEqual) {
      require(observedControl.palette() == paletteBefore && observer.paletteChanges == 0,
              "opacity-only changes must exercise a path without palette notifications");
    }
    if (observer.paints == 0) {
      std::cerr << controlName << " did not repaint after " << change << '\n';
      passed = false;
    }
  };

  // Each control has its own window so another themed sibling cannot repaint it indirectly.
  // Observe delivered paint events: render(), grab(), and update() would hide a missing update.
  for (const double opacity : {0.4, 0.3, 0.4}) {
    checkRepaint(
        "a scoped opacity-only change",
        [&] {
          overrideValue.backgroundOpacity = opacity;
          manager.setScopeOverride(&surface, overrideValue);
        },
        true);
  }
  checkRepaint("clearing the opacity scope", [&] { manager.clearScopeOverride(&surface); }, false);
  require(manager.backgroundOpacity(&control) == 0.3,
          "clearing an opacity scope must restore the global opacity for visible controls");
  for (const double opacity : {0.4, 0.3, 0.4}) {
    checkRepaint(
        "a global opacity-only change",
        [&] {
          config.backgroundOpacity = opacity;
          manager.setConfig(config);
        },
        true);
  }
  return passed;
}

void controlsRepaintForOpacityOnlyThemeChanges() {
  auto& manager = adqt::theme::ThemeManager::instance();
  const ThemeConfig originalConfig = manager.config();
  const bool buttonPassed = controlRepaintsForOpacityOnlyChanges<AdButton>("AdButton");
  const bool radioPassed = controlRepaintsForOpacityOnlyChanges<AdRadio>("AdRadio");
  const bool switchPassed = controlRepaintsForOpacityOnlyChanges<AdSwitch>("AdSwitch");
  const bool tagPassed = controlRepaintsForOpacityOnlyChanges<AdTag>("AdTag");
  const bool menuPassed =
      controlRepaintsForOpacityOnlyChanges<AdNavigationMenu>("AdNavigationMenu viewport");
  manager.setConfig(originalConfig);
  require(buttonPassed && radioPassed && switchPassed && tagPassed && menuPassed,
          "visible controls must repaint when their background opacity changes");
}

void requirePaintedBlend(const QColor& rendered, const QColor& fill, const QColor& surface,
                         const char* context) {
  const double alpha = static_cast<double>(fill.alphaF());
  const bool matches =
      std::abs(rendered.red() - qRound(fill.red() * alpha + surface.red() * (1 - alpha))) <= 1 &&
      std::abs(rendered.green() - qRound(fill.green() * alpha + surface.green() * (1 - alpha))) <=
          1 &&
      std::abs(rendered.blue() - qRound(fill.blue() * alpha + surface.blue() * (1 - alpha))) <= 1;
  if (!matches) {
    std::cerr << context << " rendered " << rendered.name(QColor::HexArgb).toStdString() << " fill "
              << fill.name(QColor::HexArgb).toStdString() << " backdrop "
              << surface.name(QColor::HexArgb).toStdString() << '\n';
  }
  require(matches, "the actual control painter must retain the final masked fill over the skin");
}

void dynamicInputOverridesPreserveOpacity() {
  auto& manager = adqt::theme::ThemeManager::instance();
  PaintedSurface surface;
  surface.resize(180, 80);
  AdLineEdit input(&surface);
  input.setGeometry(20, 20, 120, 32);
  input.setFocusPolicy(Qt::NoFocus);
  const QColor normalFill(210, 80, 50, 170);
  const QColor hoverFill(40, 200, 90, 210);
  const QColor activeFill(60, 90, 230, 190);
  input.setProperty("ad-input-background-color", normalFill);
  input.setProperty("ad-input-hover-background-color", hoverFill);
  input.setProperty("ad-input-active-background-color", activeFill);
  surface.show();
  QApplication::processEvents();

  for (const double opacity : {0.0, 0.4, 1.0}) {
    ThemeOverride overrideValue;
    overrideValue.backgroundOpacity = opacity;
    manager.setScopeOverride(&surface, overrideValue);
    for (const bool disabled : {false, true}) {
      input.setDisabled(disabled);
      for (const int state : {0, 1, 2}) {
        QFocusEvent focusEvent(state == 2 ? QEvent::FocusIn : QEvent::FocusOut);
        QApplication::sendEvent(&input, &focusEvent);
        input.setAttribute(Qt::WA_UnderMouse, state == 1);
        QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        surface.render(&image);
        const QColor source =
            state == 2 ? activeFill : (state == 1 && !disabled ? hoverFill : normalFill);
        requirePaintedBlend(image.pixelColor(80, 24),
                            adqt::theme::applyBackgroundOpacity(source, opacity),
                            surface.background, "input semantic fill");
      }
    }
  }
  manager.clearScopeOverride(&surface);
}

void selectTracksOpacityOnlyThemeChanges() {
  auto& manager = adqt::theme::ThemeManager::instance();
  const ThemeConfig originalConfig = manager.config();
  PaintedSurface surface;
  surface.resize(180, 80);
  AdSelect select(&surface);
  select.setGeometry(20, 20, 120, 32);
  select.setFocusPolicy(Qt::NoFocus);
  auto* editor = select.findChild<QLineEdit*>(QStringLiteral("adselect-input"));
  require(editor != nullptr, "select controls must own an editor");
  editor->setFocusPolicy(Qt::NoFocus);
  surface.show();
  QApplication::processEvents();
  select.setAttribute(Qt::WA_UnderMouse, false);

  for (const auto scheme : {ThemeScheme::Light, ThemeScheme::Dark}) {
    ThemeConfig config = adqt::theme::defaultThemeConfig(scheme);
    manager.setConfig(config);
    for (const auto variant : {AdSelect::Variant::Outlined, AdSelect::Variant::Filled}) {
      select.setVariant(variant);
      SelectStyleInput styleInput;
      styleInput.variant = variant;
      const auto checkFill = [&](const char* context) {
        QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        surface.render(&image);
        const auto style = resolveSelectVisualStyle(styleInput, manager.resolve(&select));
        requirePaintedBlend(image.pixelColor(80, 24), style.selectorBg, surface.background,
                            context);
      };
      for (const double opacity : {0.0, 0.4, 1.0, 0.4, 0.0}) {
        ThemeOverride overrideValue;
        overrideValue.backgroundOpacity = opacity;
        manager.setScopeOverride(&surface, overrideValue);
        checkFill("select repeated scoped opacity");
      }
      manager.clearScopeOverride(&surface);
      for (const double opacity : {0.0, 0.4, 1.0, 0.4, 0.0}) {
        config.backgroundOpacity = opacity;
        manager.setConfig(config);
        checkFill("select repeated global opacity");
      }
      config.backgroundOpacity = 1.0;
      manager.setConfig(config);
    }
  }
  manager.setConfig(originalConfig);
}

void nestedControlFillsDoNotCompound() {
  auto& manager = adqt::theme::ThemeManager::instance();
  PaintedSurface surface;
  surface.resize(240, 140);
  AdInputNumber number(&surface);
  number.setGeometry(20, 20, 180, 32);
  number.setFocusPolicy(Qt::NoFocus);
  auto* numberEditor = number.findChild<QLineEdit*>();
  require(numberEditor != nullptr, "number controls must own an editor");
  numberEditor->setFocusPolicy(Qt::NoFocus);
  AdSelect select(&surface);
  select.setGeometry(20, 80, 180, 32);
  select.setFocusPolicy(Qt::NoFocus);
  AdSelect::Option option;
  option.value = QStringLiteral("selected");
  option.label = QStringLiteral("Selected");
  select.setOptions({option});
  select.setCurrentIndex(0);
  select.setAllowClear(true);
  surface.show();
  QApplication::processEvents();
  number.setAttribute(Qt::WA_UnderMouse, true);
  select.setAttribute(Qt::WA_UnderMouse, true);

  for (const double opacity : {0.0, 0.4, 1.0}) {
    ThemeOverride overrideValue;
    overrideValue.backgroundOpacity = opacity;
    manager.setScopeOverride(&surface, overrideValue);
    for (const auto variant : {AdInputNumber::Variant::Outlined, AdInputNumber::Variant::Filled}) {
      number.setVariant(variant);
      number.setStepButtonsVisible(false);
      number.setStepButtonsVisible(true);
      auto* actions = number.findChild<QWidget*>(QStringLiteral("ad-input-number-actions"));
      require(actions && actions->isVisible(), "hovering a compact number must show step controls");
      QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::transparent);
      surface.render(&image);
      InputNumberStyleInput styleInput;
      styleInput.variant = variant;
      styleInput.hovered = true;
      const auto style = resolveInputNumberVisualStyle(styleInput, manager.resolve(&number));
      requirePaintedBlend(image.pixelColor(actions->mapTo(&surface, QPoint(3, 3))),
                          style.selectorHoverBg, surface.background, "number step area");
    }

    const QPoint local(80, 16);
    QEnterEvent enter(local, local, select.mapToGlobal(local));
    QApplication::sendEvent(&select, &enter);
    auto* clear = select.findChild<QToolButton*>(QStringLiteral("adselect-clear"));
    require(clear && clear->isVisible(), "hovering a populated select must show the clear action");
    clear->setIcon(QIcon());
    QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    surface.render(&image);
    SelectStyleInput styleInput;
    const auto style = resolveSelectVisualStyle(styleInput, manager.resolve(&select));
    requirePaintedBlend(image.pixelColor(clear->mapTo(&surface, clear->rect().center())),
                        style.selectorHoverBg, surface.background, "select clear area");
  }
  manager.clearScopeOverride(&surface);
}

void paintersPreserveFinalOpacity() {
  auto& manager = adqt::theme::ThemeManager::instance();
  PaintedSurface surface;
  surface.resize(180, 120);
  ThemeOverride overrideValue;
  overrideValue.backgroundOpacity = 0.4;
  manager.setScopeOverride(&surface, overrideValue);
  AdInputNumber number(&surface);
  number.setGeometry(20, 20, 120, 32);
  number.setVariant(AdInputNumber::Variant::Filled);
  number.setStepButtonsVisible(false);
  AdButton button(&surface);
  button.setGeometry(20, 65, 120, 32);
  button.setButtonStyle(AdButton::ButtonStyle::Tonal);
  setButtonSegmentPosition(&button, SegmentPosition::Leading);

  QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  surface.render(&image);
  InputNumberStyleInput numberInput;
  numberInput.variant = AdInputNumber::Variant::Filled;
  numberInput.stepButtonsVisible = false;
  ButtonStyleInput buttonInput;
  buttonInput.buttonStyle = AdButton::ButtonStyle::Tonal;
  buttonInput.joinsEdges = true;
  requirePaintedBlend(
      image.pixelColor(80, 24),
      resolveInputNumberVisualStyle(numberInput, manager.resolve(&number)).selectorBg,
      surface.background, "number filled shell");
  requirePaintedBlend(
      image.pixelColor(80, 69),
      resolveButtonVisualStyle(buttonInput, manager.resolve(&button)).normal.background,
      surface.background, "joined tonal button");
  manager.clearScopeOverride(&surface);
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    auto& manager = adqt::theme::ThemeManager::instance();
    manager.setConfig(adqt::theme::defaultThemeConfig());
    manager.applyTo(app);
    themeOpacityContract();
    scopedOpacityContract();
    for (const auto scheme : {ThemeScheme::Light, ThemeScheme::Dark}) {
      const auto config = adqt::theme::defaultThemeConfig(scheme);
      const auto opaque = adqt::theme::makeResolvedTheme(config);
      for (const double opacity : {0.0, 0.4, 1.0}) {
        ThemeConfig maskedConfig = config;
        maskedConfig.backgroundOpacity = opacity;
        const auto masked = adqt::theme::makeResolvedTheme(maskedConfig);
        buttonFills(opaque, masked, opacity);
        selectFills(opaque, masked, opacity);
        inputFills(opaque, masked, opacity);
        numberFills(opaque, masked, opacity);
      }
    }
    paintersPreserveFinalOpacity();
    dynamicInputOverridesPreserveOpacity();
    selectTracksOpacityOnlyThemeChanges();
    controlsRepaintForOpacityOnlyThemeChanges();
    nestedControlFillsDoNotCompound();
    auxiliaryBackgroundOpacityTests();
    datePickerBackgroundOpacityTests();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
