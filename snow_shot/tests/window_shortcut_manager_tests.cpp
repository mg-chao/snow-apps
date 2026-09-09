#include "snow_shot/presentation/windowshortcutmanager.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QSpinBox>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWidget>
#include <QWindow>

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

void readOnlyTextSurfacesRemainCommandRoutable() {
    QWidget window;
    QSpinBox numericInput(&window);
    require(WindowShortcutManager::focusAcceptsTextInput(&numericInput),
            "a numeric editor owning focus must suppress window shortcuts");
    numericInput.setReadOnly(true);
    require(!WindowShortcutManager::focusAcceptsTextInput(&numericInput),
            "a read-only numeric editor must remain command-routable");
    QLineEdit lineEdit(&window);
    QTextEdit textEdit(&window);
    QWidget textEditChild(&textEdit);
    QPlainTextEdit plainTextEdit(&window);
    QTextBrowser browser(&window);
    QWidget browserChild(&browser);

    require(WindowShortcutManager::focusAcceptsTextInput(&lineEdit),
            "an editable line edit must suppress window shortcuts");
    lineEdit.setReadOnly(true);
    require(!WindowShortcutManager::focusAcceptsTextInput(&lineEdit),
            "a read-only line edit must remain eligible for window shortcuts");

    require(WindowShortcutManager::focusAcceptsTextInput(&textEdit),
            "an editable text edit must suppress window shortcuts");
    require(WindowShortcutManager::focusAcceptsTextInput(&textEditChild),
            "a child inside an editable text edit must suppress window shortcuts");
    textEdit.setReadOnly(true);
    require(!WindowShortcutManager::focusAcceptsTextInput(&textEdit),
            "a read-only text edit must remain eligible for window shortcuts");
    require(!WindowShortcutManager::focusAcceptsTextInput(&textEditChild),
            "a child inside a read-only text edit must remain command-routable");

    require(WindowShortcutManager::focusAcceptsTextInput(&plainTextEdit),
            "an editable plain text edit must suppress window shortcuts");
    plainTextEdit.setReadOnly(true);
    require(!WindowShortcutManager::focusAcceptsTextInput(&plainTextEdit),
            "a read-only plain text edit must remain eligible for window shortcuts");

    require(!WindowShortcutManager::focusAcceptsTextInput(&browser),
            "a read-only text browser must remain eligible for window shortcuts");
    require(!WindowShortcutManager::focusAcceptsTextInput(&browserChild),
            "focus inside a read-only result surface must remain command-routable");
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
    held.cancel = [] {};
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
    declined.cancel = [] {};
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
    destroyed.cancel = [] {};
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
    shiftBinding.cancel = [] {};
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

void inputSuspensionBlocksDispatchAndClearsHeldState() {
    QWidget window;
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);

    int activationCount = 0;
    int releaseCount = 0;
    WindowShortcutManager::Binding binding;
    binding.id = QStringLiteral("suspendable-held");
    binding.keyCombinations = {QKeyCombination(Qt::ControlModifier, Qt::Key_Space)};
    binding.activate = [&activationCount](const auto&) {
        ++activationCount;
        return true;
    };
    binding.cancel = [] {};
    binding.release = [&releaseCount](const auto&) {
        ++releaseCount;
        return true;
    };
    require(manager.addBinding(&window, std::move(binding)) != 0,
            "suspendable binding registration failed");

    require(sendKey(&window, QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier) &&
                activationCount == 1,
            "suspendable binding did not activate before suspension");
    const auto suspension = manager.suspendInput();
    require(suspension != 0, "input suspension did not return a token");
    sendKey(&window, QEvent::ShortcutOverride, Qt::Key_Space, Qt::ControlModifier);
    sendKey(&window, QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier);
    require(activationCount == 1, "input suspension did not block shortcut dispatch");
    sendKey(&window, QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    require(releaseCount == 0,
            "input suspension left a held shortcut armed across the modal interaction");

    manager.resumeInput(suspension);
    require(sendKey(&window, QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier) &&
                activationCount == 2,
            "shortcut dispatch did not resume after suspension");
    sendKey(&window, QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    require(releaseCount == 1, "resumed held shortcut did not release normally");
    manager.resumeInput(suspension);
}

void childWindowOwnershipFallbackKeepsToolbarScope() {
    QWidget overlay;
    QWidget toolbar(&overlay);
    WindowShortcutManager manager;
    manager.addScopeWindow(&overlay);

    int count = 0;
    require(manager.addBinding(&overlay,
                               binding(QStringLiteral("toolbar-child"), Qt::Key_T, 100, [&]() {
                                   ++count;
                                   return true;
                               })) != 0,
            "toolbar-child binding registration failed");
    require(sendKey(&toolbar, QEvent::KeyPress, Qt::Key_T) && count == 1,
            "a toolbar child did not resolve to its overlay scope");
}

void transientToolWindowOwnershipKeepsScope() {
    QWidget overlay(nullptr, Qt::Tool);
    QWidget toolbar(nullptr, Qt::Tool);
    QWidget popup(nullptr, Qt::Tool);
    overlay.show();
    toolbar.show();
    popup.show();
    overlay.winId();
    toolbar.winId();
    popup.winId();
    toolbar.windowHandle()->setTransientParent(overlay.windowHandle());
    popup.windowHandle()->setTransientParent(toolbar.windowHandle());

    WindowShortcutManager manager;
    manager.addScopeWindow(&overlay);
    int count = 0;
    QWidget* resolvedScope = nullptr;
    auto scopedBinding = binding(QStringLiteral("transient-tool"), Qt::Key_P, 100, [&]() {
        ++count;
        return true;
    });
    scopedBinding.activate = [&count, &resolvedScope](const auto& context) {
        ++count;
        resolvedScope = context.scopeWindow;
        return true;
    };
    require(manager.addBinding(&overlay,
                               std::move(scopedBinding)) != 0,
            "transient-tool binding registration failed");
    require(sendKey(&popup, QEvent::KeyPress, Qt::Key_P) && count == 1,
            "nested transient tool window did not resolve to its overlay scope");
    require(resolvedScope == &overlay,
            "transient tool activation did not expose the resolved overlay scope");

    QWidget unrelated(nullptr, Qt::Tool);
    unrelated.show();
    unrelated.winId();
    sendKey(&unrelated, QEvent::KeyPress, Qt::Key_P);
    require(count == 1, "unrelated top-level window escaped the screenshot scope");
}

// A completion shortcut hides the capture UI while the user is still holding
// the key; the release is then routed to whichever window regains the
// foreground and never reaches this process. The platform's pressed-key
// bookkeeping keeps the key recorded, so the next physical press arrives
// flagged as an auto-repeat. The manager must dispatch that press as the fresh
// press it physically is instead of dropping it.
void lostKeyReleaseDoesNotSwallowTheNextPress() {
    QWidget window;
    QWidget child(&window);
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);

    int count = 0;
    require(manager.addBinding(&window,
                               binding(QStringLiteral("completion"), Qt::Key_C, 100, [&]() {
                                   ++count;
                                   return true;
                               })) != 0,
            "completion binding registration failed");

    require(sendKey(&child, QEvent::KeyPress, Qt::Key_C) && count == 1,
            "the first press must dispatch");

    // The capture UI hides while the key is held: the release goes to another
    // application and is never observed here.
    QEvent hide(QEvent::Hide);
    QCoreApplication::sendEvent(&window, &hide);

    // The next physical press arrives mislabeled as an auto-repeat.
    require(sendKey(&child, QEvent::ShortcutOverride, Qt::Key_C, Qt::NoModifier, true),
            "a stale auto-repeat must accept the shortcut override");
    require(sendKey(&child, QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, true) && count == 2,
            "a press whose release was lost must dispatch as a fresh press");

    // Subsequent hardware repeats of the still-held key must not re-fire.
    sendKey(&child, QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, true);
    require(count == 2, "hardware repeats of a held key must not re-fire one-shot bindings");

    // Once the release is observed, the next press is fresh again.
    sendKey(&child, QEvent::KeyRelease, Qt::Key_C);
    require(sendKey(&child, QEvent::KeyPress, Qt::Key_C) && count == 3,
            "a press after an observed release must dispatch");

    // An auto-repeat for a key whose press was never observed is a genuine
    // repeat and stays ignored.
    sendKey(&child, QEvent::KeyPress, Qt::Key_V, Qt::NoModifier, true);
    require(count == 3, "auto-repeat without an observed press must be ignored");
}

// Inactive managers also observe presses dispatched to other windows. If that
// window closes before key-up, they must not keep treating the key as held.
void lostReleaseInAnotherWindowDoesNotSwallowTheNextPress() {
    for (const auto transition : {QEvent::Hide, QEvent::WindowDeactivate}) {
        QWidget firstWindow;
        QWidget nextWindow;
        WindowShortcutManager firstManager;
        WindowShortcutManager nextManager;
        firstManager.addScopeWindow(&firstWindow);
        nextManager.addScopeWindow(&nextWindow);
        int firstCount = 0;
        int nextCount = 0;
        require(firstManager.addBinding(&firstWindow,
                                        binding(QStringLiteral("first-close"), Qt::Key_Escape, 100,
                                                [&]() {
                                                    ++firstCount;
                                                    return true;
                                                })) != 0,
                "first window close binding registration failed");
        require(nextManager.addBinding(&nextWindow,
                                       binding(QStringLiteral("next-close"), Qt::Key_Escape, 100,
                                               [&]() {
                                                   ++nextCount;
                                                   return true;
                                               })) != 0,
                "next window close binding registration failed");
        sendKey(&firstWindow, QEvent::KeyPress, Qt::Key_Escape);
        require(firstCount == 1 && nextCount == 0, "only the first window must handle its press");
        QEvent unreachable(transition);
        QCoreApplication::sendEvent(&firstWindow, &unreachable);
        sendKey(&nextWindow, QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier, true);
        sendKey(&nextWindow, QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, true);
        require(nextCount == 1, "a lost release in another window must not swallow the next press");
        QWidget child(&nextWindow);
        QEvent childHide(QEvent::Hide);
        QCoreApplication::sendEvent(&child, &childHide);
        sendKey(&nextWindow, QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, true);
        require(nextCount == 1, "hardware repeats must remain suppressed after recovery");
    }
}

void lostReleaseSurvivesShortcutManagerRecreation() {
    {
        QWidget window;
        WindowShortcutManager manager;
        manager.addScopeWindow(&window);
        sendKey(&window, QEvent::KeyPress, Qt::Key_Escape);
        QEvent hide(QEvent::Hide);
        QCoreApplication::sendEvent(&window, &hide);
    }
    QWidget window;
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);
    int count = 0;
    require(manager.addBinding(&window, binding(QStringLiteral("new-close"), Qt::Key_Escape, 100,
                                                [&]() {
                                                    ++count;
                                                    return true;
                                                })) != 0,
            "new window close binding registration failed");
    sendKey(&window, QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, true);
    require(count == 1, "lost-release recovery must survive replacement of all window managers");
    sendKey(&window, QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, true);
    require(count == 1, "a replacement manager must still suppress hardware repeats");
    sendKey(&window, QEvent::KeyRelease, Qt::Key_Escape);
}

void interruptedInputLifecycleIsBalanced() {
    for (const auto transition : {QEvent::Hide, QEvent::WindowDeactivate, QEvent::None}) {
        QWidget window;
        WindowShortcutManager manager;
        manager.addScopeWindow(&window);
        bool held = false;
        int releases = 0;
        int cancels = 0;
        auto item = binding(QStringLiteral("interruptible"), Qt::Key_F6, 100, [&] {
            held = true;
            return true;
        });
        item.release = [&](const auto&) {
            held = false;
            ++releases;
            return true;
        };
        item.cancel = [&] {
            held = false;
            ++cancels;
        };
        require(manager.addBinding(&window, std::move(item)) != 0, "held binding must register");
        sendKey(&window, QEvent::KeyPress, Qt::Key_F6);
        if (transition == QEvent::None) {
            const auto first = manager.suspendInput();
            const auto second = manager.suspendInput();
            manager.resumeInput(first);
            require(cancels == 1, "nested suspension must cancel a hold exactly once");
            manager.resumeInput(second);
        } else {
            QEvent event(transition);
            QCoreApplication::sendEvent(&window, &event);
        }
        require(!held && cancels == 1 && releases == 0,
                "interruption must cancel client state without a physical-release action");
        sendKey(&window, QEvent::KeyRelease, Qt::Key_F6);
        require(releases == 0, "late release must not finish a canceled action twice");
        sendKey(&window, QEvent::KeyPress, Qt::Key_F6);
        sendKey(&window, QEvent::KeyRelease, Qt::Key_F6);
        require(releases == 1 && !held, "a later hold must release normally");
    }
}

void finalResumeRecoversKeysObservedDuringSuspension() {
    QWidget window;
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);
    int count = 0;
    require(manager.addBinding(&window, binding(QStringLiteral("resume"), Qt::Key_F7, 100,
                                                [&] {
                                                    ++count;
                                                    return true;
                                                })) != 0,
            "resume binding must register");
    const auto first = manager.suspendInput();
    const auto second = manager.suspendInput();
    sendKey(&window, QEvent::KeyPress, Qt::Key_F7);
    manager.resumeInput(first);
    sendKey(&window, QEvent::KeyPress, Qt::Key_F7, Qt::NoModifier, true);
    require(count == 0, "partial resume must retain suspension");
    manager.resumeInput(second);
    sendKey(&window, QEvent::KeyPress, Qt::Key_F7, Qt::NoModifier, true);
    require(count == 1, "final resume must recover a lost release during suspension");
    manager.resumeInput(second);
    sendKey(&window, QEvent::KeyPress, Qt::Key_F7, Qt::NoModifier, true);
    require(count == 1, "invalid resume must not turn hardware repeats into fresh presses");
    sendKey(&window, QEvent::KeyRelease, Qt::Key_F7);
}

void recoveredPressSurvivesCrossManagerFallthrough() {
    QWidget window;
    WindowShortcutManager fallback;
    WindowShortcutManager declining;
    fallback.addScopeWindow(&window);
    declining.addScopeWindow(&window);
    int count = 0;
    require(fallback.addBinding(&window, binding(QStringLiteral("fallback"), Qt::Key_F8, 100,
                                                 [&] {
                                                     ++count;
                                                     return true;
                                                 })) != 0,
            "fallback must register");
    require(declining.addBinding(&window, binding(QStringLiteral("declining"), Qt::Key_F8, 100,
                                                  [&] {
                                                      sendKey(&window, QEvent::KeyRelease,
                                                              Qt::Key_F10);
                                                      return false;
                                                  })) != 0,
            "declining handler must register");
    sendKey(&window, QEvent::KeyPress, Qt::Key_F8);
    QEvent hide(QEvent::Hide);
    QCoreApplication::sendEvent(&window, &hide);
    sendKey(&window, QEvent::KeyPress, Qt::Key_F8, Qt::NoModifier, true);
    require(count == 2, "a declined recovered press must remain eligible for another manager");
    sendKey(&window, QEvent::KeyPress, Qt::Key_F8, Qt::NoModifier, true);
    require(count == 2, "handled recovery must still suppress hardware repeats");
    sendKey(&window, QEvent::KeyRelease, Qt::Key_F8);
}

void heldBindingsRequireCancellationAndHandleReentrantSuspension() {
    QWidget window;
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);
    auto invalid = binding(QStringLiteral("invalid"), Qt::Key_F9, 100, [] { return true; });
    invalid.release = [](const auto&) { return true; };
    require(manager.addBinding(&window, invalid) == 0,
            "a held binding without cancellation must be rejected");
    invalid.release = {};
    invalid.cancel = [] {};
    require(manager.addBinding(&window, invalid) == 0,
            "a cancellation callback without a held release must be rejected");
    bool held = false;
    int cancels = 0;
    int releases = 0;
    auto item = binding(QStringLiteral("reentrant"), Qt::Key_F9, 100, [&] {
        held = true;
        const auto suspension = manager.suspendInput();
        manager.resumeInput(suspension);
        return true;
    });
    item.cancel = [&] {
        held = false;
        ++cancels;
        const auto nested = manager.suspendInput();
        manager.resumeInput(nested);
    };
    item.release = [&](const auto&) {
        ++releases;
        return true;
    };
    require(manager.addBinding(&window, std::move(item)) != 0, "reentrant binding must register");
    sendKey(&window, QEvent::KeyPress, Qt::Key_F9);
    sendKey(&window, QEvent::KeyRelease, Qt::Key_F9);
    require(!held && cancels == 1 && releases == 0,
            "suspension inside activation must cancel once and must not rearm on return");
}

void cancellationSurvivesBindingRemovalFromCallbacks() {
    QWidget window;
    WindowShortcutManager manager;
    manager.addScopeWindow(&window);
    bool firstHeld = false;
    bool secondHeld = false;
    WindowShortcutManager::BindingHandle secondHandle = 0;
    auto first = binding(QStringLiteral("cancel-first"), Qt::Key_F10, 100, [&] {
        firstHeld = true;
        return true;
    });
    first.release = [&](const auto&) {
        firstHeld = false;
        return true;
    };
    first.cancel = [&] {
        firstHeld = false;
        static_cast<void>(manager.removeBinding(secondHandle));
    };
    auto second = binding(QStringLiteral("cancel-second"), Qt::Key_F11, 100, [&] {
        secondHeld = true;
        return true;
    });
    second.release = [&](const auto&) {
        secondHeld = false;
        return true;
    };
    second.cancel = [&] { secondHeld = false; };
    require(manager.addBinding(&window, std::move(first)) != 0, "first cancellation must register");
    secondHandle = manager.addBinding(&window, std::move(second));
    require(secondHandle != 0, "second cancellation must register");
    sendKey(&window, QEvent::KeyPress, Qt::Key_F10);
    sendKey(&window, QEvent::KeyPress, Qt::Key_F11);
    const auto suspension = manager.suspendInput();
    require(!firstHeld && !secondHeld,
            "unregistering during cancellation must not strand another client");
    manager.resumeInput(suspension);
    sendKey(&window, QEvent::KeyRelease, Qt::Key_F10);
    sendKey(&window, QEvent::KeyRelease, Qt::Key_F11);
}

} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
    cancellationSurvivesBindingRemovalFromCallbacks();
    heldBindingsRequireCancellationAndHandleReentrantSuspension();
    interruptedInputLifecycleIsBalanced();
    finalResumeRecoversKeysObservedDuringSuspension();
    recoveredPressSurvivesCrossManagerFallthrough();
    priorityAndFallthroughAreDeterministic();
    scopeRepeatUpdatesAndLifetimeAreEnforced();
    bindingsCanExplicitlyHandleTransientToolWindows();
    textGuardsLeaveInputUntouched();
    readOnlyTextSurfacesRemainCommandRoutable();
    dispatchSurvivesBindingRemovalFromCallbacks();
    heldBindingsReleaseByTriggerKey();
    heldModifierParsingAndAdditionalModifiersAreScoped();
    inputSuspensionBlocksDispatchAndClearsHeldState();
    childWindowOwnershipFallbackKeepsToolbarScope();
    transientToolWindowOwnershipKeepsScope();
    lostKeyReleaseDoesNotSwallowTheNextPress();
    lostReleaseInAnotherWindowDoesNotSwallowTheNextPress();
    lostReleaseSurvivesShortcutManagerRecreation();
    return 0;
}
