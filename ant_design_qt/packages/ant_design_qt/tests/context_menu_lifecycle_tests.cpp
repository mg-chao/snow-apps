#include "widgets/context_menu.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPointer>

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
}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    destroyedMenuTreesReleaseIcons(true);
    destroyedMenuTreesReleaseIcons(false);
    destructionPreservesSharedActionsAndSubmenus();
    destructionCancelsPendingPopup();
    resettingBackgroundPreservesMenuTree();
    std::cout << "Context menu lifecycle tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
