#include "snow_shot/presentation/windowshortcutmanager.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
using snow_shot::presentation::WindowShortcutManager;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool sendKey(QObject* receiver, QEvent::Type type, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool autoRepeat = false) {
    QKeyEvent event(type, key, modifiers, QString(), autoRepeat);
    event.setAccepted(false);
    const bool filtered = QCoreApplication::sendEvent(receiver, &event);
    return filtered || event.isAccepted();
}

WindowShortcutManager::Binding binding(const QString& id, Qt::Key key, int priority,
                                       std::function<bool()> action) {
    WindowShortcutManager::Binding result;
    result.id = id;
    result.keyCombinations = {QKeyCombination(Qt::NoModifier, key)};
    result.priority = priority;
    result.activate = [action = std::move(action)](const auto&) { return action(); };
    return result;
}

void priorityAndFallthroughAreDeterministic() {
    QWidget window;
    QWidget child(&window);
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);

    int highCount = 0;
    int lowCount = 0;
    bool highEnabled = true;
    bool highHandles = true;
    auto high = binding(QStringLiteral("high"), Qt::Key_K, 200, [&]() {
        ++highCount;
        return highHandles;
    });
    high.canActivate = [&highEnabled](const auto&) { return highEnabled; };
    require(manager.addBinding(&window, std::move(high)) != 0,
            "high-priority binding registration failed");
    require(manager.addBinding(&window,
                               binding(QStringLiteral("low"), Qt::Key_K, 100, [&]() {
                                   ++lowCount;
                                   return true;
                               })) != 0,
            "low-priority binding registration failed");

    require(sendKey(&child, QEvent::ShortcutOverride, Qt::Key_K),
            "eligible shortcut must accept ShortcutOverride");
    require(sendKey(&child, QEvent::KeyPress, Qt::Key_K) && highCount == 1 && lowCount == 0,
            "highest-priority eligible shortcut must win");

    highEnabled = false;
    require(sendKey(&child, QEvent::KeyPress, Qt::Key_K) && highCount == 1 && lowCount == 1,
            "disabled high-priority shortcut must fall through");

    highEnabled = true;
    highHandles = false;
    require(sendKey(&child, QEvent::KeyPress, Qt::Key_K) && highCount == 2 && lowCount == 2,
            "declining high-priority action must fall through");

    int firstEqualCount = 0;
    int secondEqualCount = 0;
    require(manager.addBinding(&window,
                               binding(QStringLiteral("equal-first"), Qt::Key_J, 150, [&]() {
                                   ++firstEqualCount;
                                   return false;
                               })) != 0 &&
                manager.addBinding(&window,
                                   binding(QStringLiteral("equal-second"), Qt::Key_J, 150, [&]() {
                                       ++secondEqualCount;
                                       return true;
                                   })) != 0,
            "equal-priority binding registration failed");
    require(sendKey(&child, QEvent::KeyPress, Qt::Key_J) && firstEqualCount == 1 &&
                secondEqualCount == 1,
            "equal-priority shortcuts must dispatch in registration order");
}

void scopeRepeatUpdatesAndLifetimeAreEnforced() {
    QWidget scopedWindow;
    QWidget scopedChild(&scopedWindow);
    QWidget otherWindow;
    WindowShortcutManager manager;
    manager.addScopeWindow(&scopedWindow);

    auto* owner = new QObject(&manager);
    int count = 0;
    auto repeatBinding = binding(QStringLiteral("repeat"), Qt::Key_R, 100, [&]() {
        ++count;
        return true;
    });
    const auto handle = manager.addBinding(owner, std::move(repeatBinding));
    require(handle != 0, "repeat binding registration failed");
    sendKey(&otherWindow, QEvent::KeyPress, Qt::Key_R);
    require(count == 0,
            "shortcut must not escape its window scope");
    sendKey(&scopedChild, QEvent::KeyPress, Qt::Key_R, Qt::NoModifier, true);
    require(count == 0,
            "auto-repeat must be disabled by default");

    auto enabledRepeatBinding = binding(QStringLiteral("enabled-repeat"), Qt::Key_U, 100, [&]() {
        ++count;
        return true;
    });
    enabledRepeatBinding.autoRepeat = true;
    require(manager.addBinding(owner, std::move(enabledRepeatBinding)) != 0,
            "enabled auto-repeat binding registration failed");
    require(sendKey(&scopedChild, QEvent::KeyPress, Qt::Key_U, Qt::NoModifier, true) && count == 1,
            "binding with auto-repeat enabled must handle repeated key presses");

    require(manager.setKeyCombinations(handle,
                                       {QKeyCombination(Qt::NoModifier, Qt::Key_T)}),
            "binding update failed");
    sendKey(&scopedChild, QEvent::KeyPress, Qt::Key_R);
    require(count == 1,
            "replaced shortcut must stop matching its old key");
    require(sendKey(&scopedChild, QEvent::KeyPress, Qt::Key_T) && count == 2,
            "updated shortcut must match immediately");

    delete owner;
    sendKey(&scopedChild, QEvent::KeyPress, Qt::Key_T);
    require(count == 2,
            "destroying an owner must unregister its shortcuts");
}

void bindingsCanExplicitlyHandleTransientToolWindows() {
    QWidget scopedWindow;
    QWidget transientToolWindow;
    WindowShortcutManager manager;
    manager.addScopeWindow(&scopedWindow);

    int scopedCount = 0;
    require(manager.addBinding(&scopedWindow, binding(QStringLiteral("scoped"), Qt::Key_K, 200,
                                                      [&scopedCount]() {
                                                          ++scopedCount;
                                                          return true;
                                                      })) != 0,
            "ordinary scoped binding registration failed");

    bool modalInteractionActive = false;
    int modalCount = 0;
    auto modal = binding(QStringLiteral("modal"), Qt::Key_K, 100, [&modalCount]() {
        ++modalCount;
        return true;
    });
    modal.canActivateOutsideScope = [&modalInteractionActive](const auto&) {
        return modalInteractionActive;
    };
    require(manager.addBinding(&scopedWindow, std::move(modal)) != 0,
            "modal binding registration failed");

    sendKey(&transientToolWindow, QEvent::KeyPress, Qt::Key_K);
    require(scopedCount == 0 && modalCount == 0,
            "inactive modal binding escaped normal shortcut scope");

    modalInteractionActive = true;
    require(sendKey(&transientToolWindow, QEvent::ShortcutOverride, Qt::Key_K) &&
                sendKey(&transientToolWindow, QEvent::KeyPress, Qt::Key_K) && scopedCount == 0 &&
                modalCount == 1,
            "active modal binding did not handle its transient tool window");

    require(sendKey(&scopedWindow, QEvent::KeyPress, Qt::Key_K) && scopedCount == 1 &&
                modalCount == 1,
            "out-of-scope eligibility changed normal priority dispatch");
}

void textGuardsLeaveInputUntouched() {
    QWidget window;
    QLineEdit editor(&window);
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);

    int count = 0;
    auto guarded = binding(QStringLiteral("guarded"), Qt::Key_A, 100, [&]() {
        ++count;
        return true;
    });
    guarded.canActivate = [](const WindowShortcutManager::ActivationContext& context) {
        return qobject_cast<QLineEdit*>(context.focusWidget) == nullptr;
    };
    require(manager.addBinding(&window, std::move(guarded)) != 0,
            "guarded binding registration failed");

    window.show();
    editor.setFocus();
    QCoreApplication::processEvents();
    sendKey(&editor, QEvent::ShortcutOverride, Qt::Key_A);
    sendKey(&editor, QEvent::KeyPress, Qt::Key_A);
    require(count == 0, "text guard must prevent shortcut activation");
}

void dispatchSurvivesBindingRemovalFromCallbacks() {
    QWidget window;
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);

    int lowCount = 0;
    require(manager.addBinding(&window,
                               binding(QStringLiteral("removal-low"), Qt::Key_X, 100, [&]() {
                                   ++lowCount;
                                   return true;
                               })) != 0,
            "low-priority removal binding registration failed");

    QObject* transientOwner = new QObject;
    require(manager.addBinding(
                transientOwner,
                binding(QStringLiteral("removal-high"), Qt::Key_X, 200, [&transientOwner]() {
                    QObject* owner = std::exchange(transientOwner, nullptr);
                    delete owner;
                    return false;
                })) != 0,
            "self-removing binding registration failed");

    require(sendKey(&window, QEvent::KeyPress, Qt::Key_X) && lowCount == 1 &&
                transientOwner == nullptr,
            "removing a binding from its callback must safely fall through");
    require(sendKey(&window, QEvent::KeyPress, Qt::Key_X) && lowCount == 2,
            "removed binding must stay unregistered on later dispatches");
}

void heldBindingsReleaseByTriggerKey() {
    QWidget window;
    QWidget otherWindow;
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);

    int activationCount = 0;
    int releaseCount = 0;
    WindowShortcutManager::Binding held;
    held.id = QStringLiteral("held-chord");
    held.keyCombinations = {
        QKeyCombination(Qt::ControlModifier, Qt::Key_Space),
    };
    held.activate = [&activationCount](const auto&) {
        ++activationCount;
        return true;
    };
    held.release = [&releaseCount](const auto&) {
        ++releaseCount;
        return true;
    };
    require(manager.addBinding(&window, std::move(held)) != 0,
            "held chord registration failed");

    require(sendKey(&window, QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier) &&
                activationCount == 1,
            "modified held shortcut did not activate");
    sendKey(&window, QEvent::KeyRelease, Qt::Key_Space, Qt::ControlModifier, true);
    require(releaseCount == 0,
            "an auto-repeat release must not end a physically held shortcut");
    sendKey(&window, QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    require(releaseCount == 0,
            "releasing a chord modifier must not impersonate the physical trigger release");
    require(sendKey(&otherWindow, QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier) &&
                releaseCount == 1,
            "held shortcut did not release after modifier order and focus changed");
    sendKey(&window, QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    require(releaseCount == 1, "a repeated key release invoked the held callback twice");

    int declinedReleaseCount = 0;
    auto declined = binding(QStringLiteral("declined-held"), Qt::Key_D, 100,
                            []() { return false; });
    declined.release = [&declinedReleaseCount](const auto&) {
        ++declinedReleaseCount;
        return true;
    };
    require(manager.addBinding(&window, std::move(declined)) != 0,
            "declined held binding registration failed");
    sendKey(&window, QEvent::KeyPress, Qt::Key_D);
    sendKey(&window, QEvent::KeyRelease, Qt::Key_D);
    require(declinedReleaseCount == 0,
            "a binding that declined activation must not receive a release");

    int destroyedReleaseCount = 0;
    auto* owner = new QObject;
    auto destroyed = binding(QStringLiteral("destroyed-held"), Qt::Key_L, 100,
                             []() { return true; });
    destroyed.release = [&destroyedReleaseCount](const auto&) {
        ++destroyedReleaseCount;
        return true;
    };
    require(manager.addBinding(owner, std::move(destroyed)) != 0,
            "temporary held binding registration failed");
    require(sendKey(&window, QEvent::KeyPress, Qt::Key_L),
            "temporary held binding did not activate");
    delete owner;
    sendKey(&window, QEvent::KeyRelease, Qt::Key_L);
    require(destroyedReleaseCount == 0,
            "destroying a held binding owner must discard its pending release");
}

void heldModifierParsingAndAdditionalModifiersAreScoped() {
    const QList<QKeyCombination> shift =
        WindowShortcutManager::keyCombinationsFromPortableText({QStringLiteral("Shift")});
    require(shift.size() == 1 && shift.constFirst().key() == Qt::Key_Shift &&
                shift.constFirst().keyboardModifiers() == Qt::ShiftModifier,
            "portable bare Shift did not normalize to a modifier-key binding");

    QWidget window;
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);

    int shiftPressCount = 0;
    int shiftReleaseCount = 0;
    WindowShortcutManager::Binding shiftBinding;
    shiftBinding.id = QStringLiteral("bare-shift");
    shiftBinding.keyCombinations = shift;
    shiftBinding.activate = [&shiftPressCount](const auto&) {
        ++shiftPressCount;
        return true;
    };
    shiftBinding.release = [&shiftReleaseCount](const auto&) {
        ++shiftReleaseCount;
        return true;
    };
    require(manager.addBinding(&window, std::move(shiftBinding)) != 0,
            "bare Shift binding registration failed");
    require(sendKey(&window, QEvent::KeyPress, Qt::Key_Shift, Qt::NoModifier) &&
                shiftPressCount == 1,
            "bare Shift key press was not dispatched when Qt omitted its modifier state");
    require(sendKey(&window, QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier) &&
                shiftReleaseCount == 1,
            "bare Shift key release was not dispatched after Qt cleared its modifier flag");

    int moveCount = 0;
    auto move = binding(QStringLiteral("shift-compatible-space"), Qt::Key_Space, 100, [&]() {
        ++moveCount;
        return true;
    });
    move.allowedAdditionalModifiers = Qt::ShiftModifier;
    require(manager.addBinding(&window, std::move(move)) != 0,
            "Shift-compatible Space binding registration failed");
    require(sendKey(&window, QEvent::KeyPress, Qt::Key_Space, Qt::ShiftModifier) &&
                moveCount == 1,
            "Shift followed by Space did not activate the contextual held shortcut");
    sendKey(&window, QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier);
    require(moveCount == 1,
            "a contextual binding accepted an undeclared additional modifier");

    int exactCount = 0;
    require(manager.addBinding(&window,
                               binding(QStringLiteral("exact-key"), Qt::Key_K, 100, [&]() {
                                   ++exactCount;
                                   return true;
                               })) != 0,
            "exact binding registration failed");
    sendKey(&window, QEvent::KeyPress, Qt::Key_K, Qt::ShiftModifier);
    require(exactCount == 0,
            "ordinary bare shortcuts must retain exact modifier matching");
}

} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
    priorityAndFallthroughAreDeterministic();
    scopeRepeatUpdatesAndLifetimeAreEnforced();
    bindingsCanExplicitlyHandleTransientToolWindows();
    textGuardsLeaveInputUntouched();
    dispatchSurvivesBindingRemovalFromCallbacks();
    heldBindingsReleaseByTriggerKey();
    heldModifierParsingAndAdditionalModifiersAreScoped();
    return 0;
}
