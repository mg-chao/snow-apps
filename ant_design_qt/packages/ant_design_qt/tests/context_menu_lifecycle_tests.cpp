#include "widgets/context_menu.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIconEngine>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using adqt::widgets::AdContextMenu;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class LifetimeIconEngine final : public QIconEngine {
 public:
  explicit LifetimeIconEngine(std::shared_ptr<int> lifetime) : lifetime_(std::move(lifetime)) {}

  QIconEngine* clone() const override { return new LifetimeIconEngine(lifetime_); }

  void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override {
    painter->fillRect(rect, Qt::green);
  }

  QPixmap pixmap(const QSize& size, QIcon::Mode, QIcon::State) override {
    QPixmap result(size);
    result.fill(Qt::green);
    return result;
  }

 private:
  std::shared_ptr<int> lifetime_;
};

std::weak_ptr<int> trackIcon(QAction* action) {
  auto lifetime = std::make_shared<int>(0);
  action->setIcon(QIcon(new LifetimeIconEngine(lifetime)));
  action->setIconVisibleInMenu(true);
  return lifetime;
}

void destroyedMenuTreesReleaseIcons(bool native) {
  for (int cycle = 0; cycle < 24; ++cycle) {
    QWidget owner;
    auto* menu = new AdContextMenu(&owner);
    menu->setNativeMenuEnabled(native);
    QPixmap background(120, 80);
    background.fill(Qt::blue);
    menu->setBackgroundFrame({background, QRectF(0.0, 0.0, 1.0, 1.0), 1.0, 0.8});
    std::vector<std::weak_ptr<int>> icons;
    for (int item = 0; item < 10; ++item) {
      icons.push_back(trackIcon(menu->addItem(QStringLiteral("Root item"))));
    }
    auto* submenu = menu->addSubMenu(QStringLiteral("Submenu"));
    submenu->setBackgroundFrame({background, QRectF(0.0, 0.0, 1.0, 1.0), 1.0, 0.8});
    icons.push_back(trackIcon(submenu->menuAction()));
    for (int item = 0; item < 10; ++item) {
      icons.push_back(trackIcon(submenu->addItem(QStringLiteral("Child item"))));
    }
    auto* nested = submenu->addSubMenu(QStringLiteral("Nested submenu"));
    nested->setBackgroundFrame({background, QRectF(0.0, 0.0, 1.0, 1.0), 1.0, 0.8});
    icons.push_back(trackIcon(nested->menuAction()));
    icons.push_back(trackIcon(nested->addItem(QStringLiteral("Nested item"))));
    QPointer<AdContextMenu> child = submenu;
    QPointer<AdContextMenu> grandchild = nested;
    delete menu;
    require(!child && !grandchild, "destroying a menu must destroy its owned submenus");
    for (const auto& icon : icons) {
      require(icon.expired(), "destroyed menus must release native item icon ownership");
    }
  }
}

void destructionPreservesSharedActionsAndSubmenus() {
  QObject actionOwner;
  auto* sharedAction = new QAction(&actionOwner);
  sharedAction->setText(QStringLiteral("Shared action"));
  const auto sharedIcon = trackIcon(sharedAction);
  AdContextMenu sharedSubmenu;
  const auto childIcon = trackIcon(sharedSubmenu.addItem(QStringLiteral("Shared submenu item")));
  auto* menu = new AdContextMenu;
  menu->addAction(sharedAction);
  menu->addMenu(&sharedSubmenu);
  QPointer<QAction> action = sharedAction;
  delete menu;
  require(action && !action->icon().isNull(),
          "menu cleanup must preserve externally owned actions");
  require(action->associatedObjects().isEmpty(),
          "a shared action must detach from a destroyed menu");
  require(sharedSubmenu.actions().size() == 1 && !childIcon.expired(),
          "menu cleanup must preserve externally owned submenu contents");
  sharedAction->setIcon({});
  require(sharedIcon.expired(), "a destroyed menu must not keep an external action's old icon");
  sharedSubmenu.clear();
  require(childIcon.expired(), "a shared submenu must still release its own native items");
}

void destructionCancelsPendingPopup() {
  auto* menu = new AdContextMenu;
  const auto icon = trackIcon(menu->addItem(QStringLiteral("Pending popup")));
  menu->popupAt(QPoint(100, 100));
  menu->dismissPopup();
  delete menu;
  QCoreApplication::processEvents();
  require(icon.expired(), "a cancelled queued popup must release its icon on destruction");
}

void destructionNotifiesObserversBeforeMenuTeardown(bool native) {
  QWidget owner;
  owner.show();
  for (int state = 0; state < 3; ++state) {
    QPointer<AdContextMenu> menu = new AdContextMenu(&owner);
    menu->setNativeMenuEnabled(native);
    QPointer<QAction> action = menu->addItem(QStringLiteral("Observed action"));
    if (state > 0) menu->popupAt(QPoint(100, 100));
    if (state == 2) menu->dismissPopup();
    int notifications = 0;
    bool observedLiveState = false;
    QObject::connect(menu, &AdContextMenu::aboutToDestroy, &owner, [&]() {
      ++notifications;
      observedLiveState = menu && action && menu->actions().contains(action) &&
                          menu->nativeMenuEnabled() == native && !menu->isRetiring();
      (void)menu->isPopupVisible();
    });
    delete menu.data();
    require(notifications == 1 && observedLiveState && !menu && !action,
            "observers must be notified once while menu APIs are valid, before Qt teardown");
    QCoreApplication::processEvents();
  }
}

void resettingBackgroundPreservesMenuTree() {
  AdContextMenu menu;
  menu.setNativeMenuEnabled(false);
  QAction* action = menu.addItem(QStringLiteral("Root"));
  AdContextMenu* submenu = menu.addSubMenu(QStringLiteral("Child"));
  QAction* child = submenu->addItem(QStringLiteral("Child item"));
  QPixmap background(120, 80);
  background.fill(Qt::blue);
  menu.setBackgroundFrame({background, QRectF(0.0, 0.0, 1.0, 1.0), 1.0, 0.8});
  submenu->setBackgroundFrame({background, QRectF(0.0, 0.0, 0.5, 1.0), 1.0, 0.8});
  menu.resetBackgroundFrame();
  require(menu.backgroundFrame().image.isNull(), "reset must release the stored background raster");
  require(!submenu->backgroundFrame().image.isNull(),
          "independent submenu viewports must retain their own background frames");
  require(menu.actions().contains(action) && submenu->actions().contains(child),
          "background reset must preserve actions and submenu ownership");
  submenu->resetBackgroundFrame();
  require(submenu->backgroundFrame().image.isNull(), "submenu reset must release its raster");
}

void drainRetiredPopups() {
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void transientPopupsReleaseTheirEntireTree() {
  QWidget owner;
  owner.show();
  QPointer<AdContextMenu> menu = new AdContextMenu(&owner);
  menu->setNativeMenuEnabled(false);
  menu->setDeleteOnHide();
  const auto icon = trackIcon(menu->addItem(QStringLiteral("Root")));
  int openings = 0;
  auto* entry = menu->addLazySubMenu(QStringLiteral("Child"), [&openings](auto* child) {
    ++openings;
    child->addItem(QStringLiteral("Child item"));
  });
  QPointer<QMenu> child = entry->menu();
  require(openings == 0 && child->actions().isEmpty() && child->windowHandle() == nullptr,
          "a submenu must have no content or popup surface before it opens");
  menu->popupAt(QPoint(100, 100));
  menu->dismissPopup();
  drainRetiredPopups();
  require(!menu && !child && icon.expired() && openings == 0,
          "hiding a root must release its actions, icons and unopened submenu shells");
}

void lazySubmenusRetireAndReopenWithFreshContent() {
  AdContextMenu menu;
  menu.setNativeMenuEnabled(false);
  int openings = 0;
  int nestedOpenings = 0;
  std::weak_ptr<int> icon;
  auto* entry = menu.addLazySubMenu(QStringLiteral("Child"), [&](auto* child) {
    ++openings;
    icon = trackIcon(child->addItem(QString::number(openings)));
    child->addLazySubMenu(QStringLiteral("Nested"), [&](auto* nested) {
      ++nestedOpenings;
      nested->addItem(QStringLiteral("Nested item"));
    });
  });
  menu.popupAt(QPoint(100, 100));
  auto* child = qobject_cast<AdContextMenu*>(entry->menu());
  child->popupAt(QPoint(200, 100));
  QPointer<AdContextMenu> previous = child;
  QPointer<QMenu> nested = child->actions().last()->menu();
  require(openings == 1 && nestedOpenings == 0,
          "opening one submenu must not materialize its descendants");
  child->dismissPopup();
  drainRetiredPopups();
  require(!previous && !nested && icon.expired() && entry->menu() != nullptr &&
              entry->menu()->actions().isEmpty() && menu.isVisible(),
          "a hidden submenu must retire its entire tree while its parent remains usable");
  child = qobject_cast<AdContextMenu*>(entry->menu());
  child->popupAt(QPoint(200, 100));
  require(openings == 2 && child->actions().first()->text() == QStringLiteral("2"),
          "reopening a submenu must use a fresh snapshot");
  previous = child;
  menu.clear();
  require(!previous, "clearing a menu must also destroy its lazy submenu bindings and shells");
  menu.dismissPopup();
}

void lazySubmenusOpenThroughKeyboardNavigation() {
  AdContextMenu menu;
  menu.setNativeMenuEnabled(false);
  int openings = 0;
  int nestedOpenings = 0;
  auto* entry = menu.addLazySubMenu(QStringLiteral("Child"), [&](auto* child) {
    ++openings;
    child->addLazySubMenu(QStringLiteral("Nested"), [&](auto* nested) {
      ++nestedOpenings;
      nested->addItem(QStringLiteral("Command"));
    });
  });
  menu.popupAt(QPoint(100, 100));
  menu.setActiveAction(entry);
  QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
  QApplication::sendEvent(&menu, &right);
  auto* child = qobject_cast<AdContextMenu*>(entry->menu());
  require(child && child->isVisible() && openings == 1 && nestedOpenings == 0,
          "Qt keyboard navigation must open an initially empty lazy submenu");
  auto* nestedEntry = child->actions().first();
  child->setActiveAction(nestedEntry);
  QApplication::sendEvent(child, &right);
  QPointer<QMenu> nested = nestedEntry->menu();
  require(nested && nested->isVisible() && nestedOpenings == 1,
          "keyboard navigation materializes only the requested submenu level");
  QPointer<AdContextMenu> previous = child;
  menu.dismissPopup();
  drainRetiredPopups();
  require(!previous && !nested, "closing the menu retires all navigated submenu levels");
}

void persistentParentsRestoreLazySubmenus(bool drainBeforeReopening) {
  AdContextMenu menu;
  menu.setNativeMenuEnabled(false);
  int openings = 0;
  int activations = 0;
  auto* entry = menu.addLazySubMenu(QStringLiteral("Child"), [&](auto* child) {
    ++openings;
    auto* command = child->addItem(QString::number(openings));
    QObject::connect(command, &QAction::triggered, &menu, [&]() { ++activations; });
  });
  QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QPointer<QMenu> previous;
  QPointer<QAction> previousCommand;
  for (int opening = 1; opening <= 3; ++opening) {
    menu.popupAt(QPoint(100, 100));
    QPointer<QMenu> shell = entry->menu();
    require(shell && shell->actions().isEmpty(),
            "reopening a persistent parent must restore an empty lazy submenu shell");
    drainRetiredPopups();
    require(!previous && !previousCommand && shell && entry->menu() == shell && menu.isVisible(),
            "retirement of an older shell must not replace the current shell");
    menu.setActiveAction(entry);
    QApplication::sendEvent(&menu, &right);
    require(shell->isVisible() && openings == opening &&
                shell->actions().first()->text() == QString::number(opening),
            "keyboard reopening must populate fresh submenu content exactly once");
    QPointer<QAction> command = shell->actions().first();
    shell->setActiveAction(command);
    QApplication::sendEvent(shell, &enter);
    require(activations == opening && !menu.isVisible(),
            "the restored submenu must deliver its command and close the menu tree");
    previous = shell;
    previousCommand = command;
    if (drainBeforeReopening) {
      drainRetiredPopups();
      require(!shell && !command, "dismissal must release populated submenu content");
    }
  }
  drainRetiredPopups();
}

void retirementWaitsForActionDeliveryAndCancelsPendingPopups() {
  QPointer<AdContextMenu> menu = new AdContextMenu;
  menu->setNativeMenuEnabled(false);
  menu->setDeleteOnHide();
  QPointer<QAction> action = menu->addItem(QStringLiteral("Execute"));
  bool triggered = false;
  QObject::connect(action, &QAction::triggered, [&]() { triggered = true; });
  QTimer::singleShot(0, menu, [&]() {
    menu->hide();
    drainRetiredPopups();
    require(menu && action, "an executing popup must survive until selected actions are delivered");
    action->trigger();
  });
  menu->execAt(QPoint(100, 100));
  drainRetiredPopups();
  require(triggered && !menu && !action, "action delivery must finish before the popup is retired");

  menu = new AdContextMenu;
  menu->setDeleteOnHide();
  action = menu->addItem(QStringLiteral("Cancelled"));
  menu->popupAt(QPoint(100, 100));
  menu->dismissPopup();
  drainRetiredPopups();
  require(!menu && !action,
          "cancelling a queued or visible popup must release the complete session");
}
}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    destroyedMenuTreesReleaseIcons(true);
    destroyedMenuTreesReleaseIcons(false);
    destructionPreservesSharedActionsAndSubmenus();
    destructionCancelsPendingPopup();
    destructionNotifiesObserversBeforeMenuTeardown(false);
#ifdef Q_OS_MACOS
    destructionNotifiesObserversBeforeMenuTeardown(true);
#endif
    resettingBackgroundPreservesMenuTree();
    transientPopupsReleaseTheirEntireTree();
    lazySubmenusRetireAndReopenWithFreshContent();
    lazySubmenusOpenThroughKeyboardNavigation();
    persistentParentsRestoreLazySubmenus(true);
    persistentParentsRestoreLazySubmenus(false);
    retirementWaitsForActionDeliveryAndCancelsPendingPopups();
    std::cout << "Context menu lifecycle tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
