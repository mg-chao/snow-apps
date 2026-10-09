#include "snow_shot/presentation/components/actionpopupmenu.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPointer>
#include <QPushButton>

#include <cstdlib>
#include <iostream>

namespace {
using adqt::widgets::AdContextMenu;
using snow_shot::presentation::ActionPopupMenu;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void drainRetiredMenus() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void deletingAnOpenMenuEndsTheControllerSession() {
    QWidget owner;
    QPushButton trigger(QStringLiteral("More"), &owner);
    owner.show();
    QPointer<AdContextMenu> menu;
    int creations = 0;
    ActionPopupMenu controller(
        &trigger,
        [&]() {
            ++creations;
            menu = new AdContextMenu(&owner);
            menu->addItem(QStringLiteral("Action"));
            return menu.data();
        },
        ActionPopupMenu::Placement::BottomLeft, ActionPopupMenu::Surface::Widget);
    controller.open();
    require(menu && menu->isPopupVisible(), "the action menu must open");
    QPointer<AdContextMenu> previous = menu;
    // A popup's destruction can synchronously deliver events to its trigger.
    // Reopen before hiding, while the old menu still reports visible, to verify
    // that the controller has relinquished that session before teardown starts.
    QObject::connect(previous, &AdContextMenu::aboutToDestroy, &owner, [&]() {
        require(previous && previous->isPopupVisible(),
                "destruction notification must precede menu state teardown");
        controller.open();
        require(creations == 2 && menu != previous,
                "a destroying menu must no longer own the controller's active session");
    });
    delete previous.data();
    require(!previous && menu && menu->isPopupVisible(),
            "destroying an old session must preserve the newly opened menu");
    menu->dismissPopup();
    drainRetiredMenus();
    require(!menu, "the replacement session must retire normally");
}

void closingAndReopeningMenusPreservesActionDelivery() {
    QWidget owner;
    QPushButton trigger(QStringLiteral("More"), &owner);
    owner.show();
    QPointer<AdContextMenu> menu;
    QPointer<QAction> action;
    int creations = 0;
    int activations = 0;
    ActionPopupMenu controller(
        &trigger,
        [&]() {
            ++creations;
            menu = new AdContextMenu(&owner);
            action = menu->addItem(QStringLiteral("Action"));
            QObject::connect(action, &QAction::triggered, &owner, [&]() { ++activations; });
            return menu.data();
        },
        ActionPopupMenu::Placement::BottomLeft, ActionPopupMenu::Surface::Widget);
    for (int cycle = 1; cycle <= 4; ++cycle) {
        controller.open(true);
        require(menu && menu->activeAction() == action,
                "keyboard opening must select the current session's action");
        QPointer<AdContextMenu> previous = menu;
        QPointer<QAction> previousAction = action;
        menu->dismissPopup();
        // QMenu delivers its selected action after aboutToHide.
        previousAction->trigger();
        require(activations == cycle, "retirement must preserve action delivery");
        controller.open();
        require(menu && menu != previous && creations == 2 * cycle,
                "reopening before deferred deletion must create a fresh session");
        drainRetiredMenus();
        require(!previous && !previousAction && menu && menu->isPopupVisible(),
                "retiring an older menu must leave the new session usable");
        menu->dismissPopup();
        drainRetiredMenus();
        require(!menu && !action, "dismissal must release the menu and its actions");
    }
}

void destroyingTheTriggerDismissesItsMenu() {
    QWidget owner;
    owner.show();
    auto* trigger = new QPushButton(QStringLiteral("More"), &owner);
    trigger->show();
    QPointer<AdContextMenu> menu;
    new ActionPopupMenu(
        trigger,
        [&]() {
            menu = new AdContextMenu(&owner);
            menu->addItem(QStringLiteral("Action"));
            return menu.data();
        },
        ActionPopupMenu::Placement::BottomLeft, ActionPopupMenu::Surface::Widget);
    trigger->click();
    require(menu && menu->isPopupVisible(), "the trigger must open its owned controller's menu");
    delete trigger;
    drainRetiredMenus();
    require(!menu, "trigger destruction must dismiss and release its popup session");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    deletingAnOpenMenuEndsTheControllerSession();
    closingAndReopeningMenusPreservesActionDelivery();
    destroyingTheTriggerDismissesItsMenu();
    std::cout << "Action popup menu tests passed\n";
    return 0;
}
