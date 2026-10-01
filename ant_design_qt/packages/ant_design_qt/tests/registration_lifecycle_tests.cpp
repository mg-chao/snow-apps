#include "theme/theme_manager.h"
#include "widgets/navigation_menu.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QStyledItemDelegate>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Base>
class DestructionProbe final : public Base {
 public:
  using Base::Base;

  int destroyedCallbacks() const { return this->receivers(SIGNAL(destroyed(QObject*))); }
};

class ReentrantScopeWidget final : public QWidget {
 public:
  std::function<void()> paletteChanged;

  int destroyedCallbacks() const { return receivers(SIGNAL(destroyed(QObject*))); }

 protected:
  bool event(QEvent* event) override {
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::PaletteChange && paletteChanged) {
      paletteChanged();
    }
    return handled;
  }
};

void scopeOverridesReleaseTheirRegistration() {
  auto& manager = adqt::theme::ThemeManager::instance();
  DestructionProbe<QObject> scope;
  QObject observer;
  bool destroyed = false;
  QObject::connect(&scope, &QObject::destroyed, &observer, [&] { destroyed = true; });
  const int baseline = scope.destroyedCallbacks();
  adqt::theme::ThemeOverride overrideValue;
  overrideValue.scheme = adqt::theme::ThemeScheme::Dark;
  for (int cycle = 0; cycle < 256; ++cycle) {
    manager.setScopeOverride(&scope, overrideValue);
    require(scope.destroyedCallbacks() == baseline + 1,
            "a scope override must own exactly one destruction callback");
    overrideValue.scheme = adqt::theme::ThemeScheme::Light;
    manager.setScopeOverride(&scope, overrideValue);
    require(scope.destroyedCallbacks() == baseline + 1,
            "updating a scope override must reuse its destruction callback");
    if (cycle % 2 == 0) {
      manager.clearScopeOverride(&scope);
    } else {
      manager.setScopeOverride(&scope, {});
    }
    require(scope.destroyedCallbacks() == baseline,
            "clearing a scope override must release only its destruction callback");
    require(adqt::theme::isEmptyThemeOverride(manager.scopeOverride(&scope)),
            "clearing a scope override must discard its theme state");
    overrideValue.scheme = adqt::theme::ThemeScheme::Dark;
  }
  require(!destroyed, "clearing scope registrations must not destroy their scope");

  auto active = std::make_unique<DestructionProbe<QObject>>();
  QObject* identity = active.get();
  manager.setScopeOverride(identity, overrideValue);
  active.reset();
  require(adqt::theme::isEmptyThemeOverride(manager.scopeOverride(identity)),
          "scope destruction must discard its active override");
}

void widgetScopeCleanupAllowsReentrantClear() {
  auto& manager = adqt::theme::ThemeManager::instance();
  ReentrantScopeWidget scope;
  QPalette palette = scope.palette();
  palette.setColor(QPalette::Window, QColor(QStringLiteral("#123456")));
  scope.setPalette(palette);
  QFont font = scope.font();
  font.setPixelSize(19);
  scope.setFont(font);
  const int baseline = scope.destroyedCallbacks();
  adqt::theme::ThemeOverride overrideValue;
  overrideValue.scheme = adqt::theme::ThemeScheme::Dark;
  overrideValue.appFont = font;
  overrideValue.appFont->setPixelSize(25);
  manager.setScopeOverride(&scope, overrideValue);
  int restorationEvents = 0;
  scope.paletteChanged = [&] {
    ++restorationEvents;
    manager.clearScopeOverride(&scope);
  };
  manager.clearScopeOverride(&scope);
  require(restorationEvents > 0, "widget scope cleanup must exercise synchronous palette events");
  require(scope.palette() == palette && scope.font() == font,
          "scope cleanup must restore the widget's explicit palette and font");
  require(scope.destroyedCallbacks() == baseline &&
              adqt::theme::isEmptyThemeOverride(manager.scopeOverride(&scope)),
          "reentrant scope cleanup must leave no registration or override behind");
}

void delegateRegistrationsFollowTheActiveProvider() {
  using adqt::widgets::AdNavigationMenu;
  QObject owner;
  DestructionProbe<QStyledItemDelegate> first(&owner);
  auto second = std::make_unique<DestructionProbe<QStyledItemDelegate>>(&owner);
  QObject observer;
  int unrelatedDestructions = 0;
  QObject::connect(second.get(), &QObject::destroyed, &observer, [&] { ++unrelatedDestructions; });
  const int firstBaseline = first.destroyedCallbacks();
  const int secondBaseline = second->destroyedCallbacks();
  AdNavigationMenu menu;
  auto* fallback = menu.itemDelegate();
  int notifications = 0;
  QObject::connect(&menu, &AdNavigationMenu::itemDelegateChanged, &observer,
                   [&](QAbstractItemDelegate*) { ++notifications; });

  for (int cycle = 0; cycle < 256; ++cycle) {
    menu.setItemDelegate(&first);
    menu.setItemDelegate(second.get());
    require(first.destroyedCallbacks() == firstBaseline,
            "replacing an item delegate must release the previous destruction callback");
    menu.setItemDelegate(nullptr);
    require(second->destroyedCallbacks() == secondBaseline,
            "clearing an item delegate must release only its destruction callback");
    require(menu.itemDelegate() == fallback, "clearing an item delegate must restore the default");
  }

  menu.setItemDelegate(second.get());
  const int beforeDestruction = notifications;
  second.reset();
  require(unrelatedDestructions == 1, "delegate cleanup must preserve unrelated subscriptions");
  require(notifications == beforeDestruction + 1,
          "active delegate destruction must notify the default delegate exactly once");
  require(menu.itemDelegate() == fallback, "active delegate destruction must restore the default");
  for (const QString& name : {QStringLiteral("AdNavigationMenu-inline-view"),
                              QStringLiteral("AdNavigationMenu-vertical-view"),
                              QStringLiteral("AdNavigationMenu-bar-view")}) {
    auto* view = menu.findChild<QAbstractItemView*>(name);
    require(view != nullptr, "the menu must provide all root item views");
    require(view->itemDelegate() == fallback,
            "active delegate destruction must restore the default on every root item view");
  }

  auto retired = std::make_unique<DestructionProbe<QStyledItemDelegate>>(&owner);
  menu.setItemDelegate(retired.get());
  menu.setItemDelegate(&first);
  const int beforeRetiredDestruction = notifications;
  retired.reset();
  require(notifications == beforeRetiredDestruction && menu.itemDelegate() == &first,
          "destroying a replaced delegate must not disturb the active delegate");
}

void popupFactoryRegistrationsFollowTheActiveProvider() {
  using adqt::widgets::AdNavigationMenu;
  using adqt::widgets::AdNavigationMenuPopupFactory;
  QObject owner;
  DestructionProbe<AdNavigationMenuPopupFactory> first(&owner);
  auto second = std::make_unique<DestructionProbe<AdNavigationMenuPopupFactory>>(&owner);
  QObject observer;
  int unrelatedDestructions = 0;
  QObject::connect(second.get(), &QObject::destroyed, &observer, [&] { ++unrelatedDestructions; });
  const int firstBaseline = first.destroyedCallbacks();
  const int secondBaseline = second->destroyedCallbacks();
  AdNavigationMenu menu;
  int notifications = 0;
  QObject::connect(&menu, &AdNavigationMenu::popupFactoryChanged, &observer,
                   [&](AdNavigationMenuPopupFactory*) { ++notifications; });

  for (int cycle = 0; cycle < 256; ++cycle) {
    menu.setPopupFactory(&first);
    require(first.destroyedCallbacks() == firstBaseline + 1,
            "a popup factory must own exactly one destruction callback");
    menu.setPopupFactory(second.get());
    require(first.destroyedCallbacks() == firstBaseline,
            "replacing a popup factory must release the previous destruction callback");
    menu.setPopupFactory(nullptr);
    require(second->destroyedCallbacks() == secondBaseline,
            "clearing a popup factory must release only its destruction callback");
  }

  menu.setPopupFactory(second.get());
  const int beforeDestruction = notifications;
  second.reset();
  require(unrelatedDestructions == 1, "factory cleanup must preserve unrelated subscriptions");
  require(notifications == beforeDestruction + 1 && menu.popupFactory() == nullptr,
          "active popup factory destruction must notify the default factory exactly once");

  auto retired = std::make_unique<DestructionProbe<AdNavigationMenuPopupFactory>>(&owner);
  menu.setPopupFactory(retired.get());
  menu.setPopupFactory(&first);
  const int beforeRetiredDestruction = notifications;
  retired.reset();
  require(notifications == beforeRetiredDestruction && menu.popupFactory() == &first,
          "destroying a replaced factory must not disturb the active factory");
}

void menuDestructionReleasesExternalProviders() {
  using adqt::widgets::AdNavigationMenu;
  using adqt::widgets::AdNavigationMenuPopupFactory;
  QObject owner;
  DestructionProbe<QStyledItemDelegate> delegate(&owner);
  DestructionProbe<AdNavigationMenuPopupFactory> factory(&owner);
  const int delegateBaseline = delegate.destroyedCallbacks();
  const int factoryBaseline = factory.destroyedCallbacks();
  {
    AdNavigationMenu menu;
    menu.setItemDelegate(&delegate);
    menu.setPopupFactory(&factory);
  }
  require(delegate.destroyedCallbacks() == delegateBaseline &&
              factory.destroyedCallbacks() == factoryBaseline,
          "destroying a menu must release all external provider destruction callbacks");
}

void delegateNotificationsMayDestroyTheMenu() {
  using adqt::widgets::AdNavigationMenu;
  QObject owner;
  QObject observer;
  QStyledItemDelegate delegate(&owner);
  QPointer<AdNavigationMenu> menu = new AdNavigationMenu;
  QObject::connect(menu, &AdNavigationMenu::itemDelegateChanged, &observer,
                   [menu](QAbstractItemDelegate*) { delete menu.data(); });
  menu->setItemDelegate(&delegate);
  require(menu.isNull(), "setting an item delegate may destroy the menu from its notification");

  auto activeDelegate = std::make_unique<QStyledItemDelegate>(&owner);
  menu = new AdNavigationMenu;
  menu->setItemDelegate(activeDelegate.get());
  QObject::connect(menu, &AdNavigationMenu::itemDelegateChanged, &observer,
                   [menu](QAbstractItemDelegate*) { delete menu.data(); });
  activeDelegate.reset();
  require(menu.isNull(),
          "restoring the default delegate may destroy the menu from its notification");
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  int failures = 0;
  for (auto test :
       {scopeOverridesReleaseTheirRegistration, widgetScopeCleanupAllowsReentrantClear,
        delegateRegistrationsFollowTheActiveProvider,
        popupFactoryRegistrationsFollowTheActiveProvider, menuDestructionReleasesExternalProviders,
        delegateNotificationsMayDestroyTheMenu}) {
    try {
      test();
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
