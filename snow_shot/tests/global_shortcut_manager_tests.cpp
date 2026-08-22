#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QCoreApplication>
#include <QHash>
#include <QKeyCombination>
#include <QKeySequence>
#include <QSet>
#include <QString>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

namespace shortcuts = snow_shot::presentation;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class FakeGlobalShortcutBackend final : public shortcuts::GlobalShortcutBackend {
  public:
    void setActivationHandler(ActivationHandler handler) override {
        m_handler = std::move(handler);
    }

    shortcuts::GlobalShortcutValidationResult
    validateShortcut(const QString& portableShortcut) const override {
        return {
            portableShortcut,
            !portableShortcut.trimmed().isEmpty(),
            portableShortcut.trimmed().isEmpty()
                ? shortcuts::GlobalShortcutFailureReason::InvalidShortcut
                : shortcuts::GlobalShortcutFailureReason::None,
        };
    }

    shortcuts::GlobalShortcutBackendResult
    registerShortcut(int registrationId, const QString& portableShortcut) override {
        ++registrationAttempts[portableShortcut];
        if (occupiedShortcuts.contains(portableShortcut) ||
            registrations.values().contains(portableShortcut)) {
            return {
                false,
                shortcuts::GlobalShortcutFailureReason::AlreadyInUse,
                1409,
            };
        }

        registrations.insert(registrationId, portableShortcut);
        return {
            true,
            shortcuts::GlobalShortcutFailureReason::None,
            0,
        };
    }

    void unregisterShortcut(int registrationId) override {
        unregisteredIds.insert(registrationId);
        registrations.remove(registrationId);
    }

    void activate(const QString& portableShortcut) {
        const int registrationId = registrations.key(portableShortcut, 0);
        if (registrationId != 0 && m_handler) {
            m_handler(registrationId);
        }
    }

    QSet<QString> occupiedShortcuts;
    QHash<int, QString> registrations;
    QHash<QString, int> registrationAttempts;
    QSet<int> unregisteredIds;

  private:
    ActivationHandler m_handler;
};

struct IsolatedSettings {
    IsolatedSettings() {
        require(directory.isValid(), "temporary shortcut storage should be available");
        static_cast<void>(snow_shot::storage::ApplicationStorage::instance().initialize(
            {directory.path(), directory.path(), 8000}));
        snow_shot::storage::ShortcutSettings().setScreenshot({});
        snow_shot::storage::ShortcutSettings().setOpenSettings({});
        snow_shot::storage::ShortcutSettings().setPinClipboardContent({});
        snow_shot::storage::GlobalShortcutSettings()
            .setDisableOnFocusedFullscreenWindow(false);
    }

    ~IsolatedSettings() {
        snow_shot::storage::ApplicationStorage::instance().shutdown();
    }

    QString organization;
    QString application;
    QTemporaryDir directory;
};

void registrationStatesReflectActualAvailability() {
    IsolatedSettings settings;
    auto backend = std::make_unique<FakeGlobalShortcutBackend>();
    auto* const backendPtr = backend.get();
    backendPtr->occupiedShortcuts.insert(QStringLiteral("Ctrl+Alt+1"));

    shortcuts::GlobalShortcutManager manager(std::move(backend), settings.organization,
                                             settings.application);
    manager.initialize();
    require(manager.state(shortcuts::GlobalShortcutAction::Screenshot).status ==
                shortcuts::GlobalShortcutStatus::Unset,
            "an action without shortcuts should be unset");

    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot,
                         {QStringLiteral("Ctrl+Alt+1"), QStringLiteral("Ctrl+Alt+2")});
    const shortcuts::GlobalShortcutRegistrationState partialState =
        manager.state(shortcuts::GlobalShortcutAction::Screenshot);
    require(partialState.status == shortcuts::GlobalShortcutStatus::PartiallyRegistered,
            "one successful binding should produce a partial state");
    require(partialState.bindings.size() == 2 && !partialState.bindings[0].registered &&
                partialState.bindings[1].registered,
            "partial state should retain each binding result in display order");

    shortcuts::GlobalShortcutAction activatedAction = shortcuts::GlobalShortcutAction::OpenSettings;
    int activationCount = 0;
    QObject::connect(&manager, &shortcuts::GlobalShortcutManager::activated, &manager,
                     [&activatedAction, &activationCount](shortcuts::GlobalShortcutAction action) {
                         activatedAction = action;
                         ++activationCount;
                     });
    backendPtr->activate(QStringLiteral("Ctrl+Alt+2"));
    require(activationCount == 1 && activatedAction == shortcuts::GlobalShortcutAction::Screenshot,
            "native activation should dispatch the owning action");

    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot,
                         {QStringLiteral("Ctrl+Alt+1")});
    require(manager.state(shortcuts::GlobalShortcutAction::Screenshot).status ==
                shortcuts::GlobalShortcutStatus::Failed,
            "an action with no usable bindings should fail");
    require(!backendPtr->unregisteredIds.isEmpty(),
            "removing a successful binding should unregister its native id");

    backendPtr->occupiedShortcuts.clear();
    manager.retryRegistrations();
    require(manager.state(shortcuts::GlobalShortcutAction::Screenshot).status ==
                shortcuts::GlobalShortcutStatus::Registered,
            "retry should register a binding after its conflict is released");

    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot, {});
    require(manager.state(shortcuts::GlobalShortcutAction::Screenshot).status ==
                shortcuts::GlobalShortcutStatus::Unset,
            "clearing all shortcuts should return to unset");

    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot, {
                                                                          QStringLiteral("Ctrl+1"),
                                                                          QStringLiteral("Ctrl+1"),
                                                                          QStringLiteral("Ctrl+2"),
                                                                          QStringLiteral("Ctrl+3"),
                                                                      });
    require(manager.state(shortcuts::GlobalShortcutAction::Screenshot).shortcuts ==
                QStringList{QStringLiteral("Ctrl+1"), QStringLiteral("Ctrl+2")},
            "configuration should keep at most two distinct shortcuts");
}

void persistedDesiredBindingsAreRestored() {
    IsolatedSettings settings;
    {
        auto backend = std::make_unique<FakeGlobalShortcutBackend>();
        backend->occupiedShortcuts.insert(QStringLiteral("Ctrl+,"));
        shortcuts::GlobalShortcutManager manager(std::move(backend), settings.organization,
                                                 settings.application);
        manager.initialize();
        manager.setShortcuts(shortcuts::GlobalShortcutAction::OpenSettings,
                             {QStringLiteral("Ctrl + ,")});
        require(manager.state(shortcuts::GlobalShortcutAction::OpenSettings).status ==
                    shortcuts::GlobalShortcutStatus::Failed,
                "a failed desired binding should still be retained");
    }

    auto restoredBackend = std::make_unique<FakeGlobalShortcutBackend>();
    shortcuts::GlobalShortcutManager restored(std::move(restoredBackend), settings.organization,
                                              settings.application);
    restored.initialize();
    const shortcuts::GlobalShortcutRegistrationState state =
        restored.state(shortcuts::GlobalShortcutAction::OpenSettings);
    require(state.status == shortcuts::GlobalShortcutStatus::Registered &&
                state.shortcuts == QStringList{QStringLiteral("Ctrl+,")},
            "portable shortcut lists, including comma, should survive restart");
}

void releasedApplicationConflictIsReconciled() {
    IsolatedSettings settings;
    auto backend = std::make_unique<FakeGlobalShortcutBackend>();
    shortcuts::GlobalShortcutManager manager(std::move(backend), settings.organization,
                                             settings.application);
    manager.initialize();

    const QString sharedShortcut = QStringLiteral("Ctrl+Shift+P");
    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot, {sharedShortcut});
    manager.setShortcuts(shortcuts::GlobalShortcutAction::OpenSettings, {sharedShortcut});
    require(manager.state(shortcuts::GlobalShortcutAction::Screenshot).status ==
                    shortcuts::GlobalShortcutStatus::Registered &&
                manager.state(shortcuts::GlobalShortcutAction::OpenSettings).status ==
                    shortcuts::GlobalShortcutStatus::Failed,
            "an application-owned shortcut should remain with its first action");
    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot, {});
    require(manager.state(shortcuts::GlobalShortcutAction::OpenSettings).status ==
                shortcuts::GlobalShortcutStatus::Registered,
            "changing another action should retry bindings whose conflict was released");
}

void everyQuickActionHasIndependentPersistenceAndRegistration() {
    IsolatedSettings settings;
    const QVector<QPair<shortcuts::GlobalShortcutAction, QString>> actions = {
        {shortcuts::GlobalShortcutAction::Screenshot, QStringLiteral("Ctrl+Alt+1")},
        {shortcuts::GlobalShortcutAction::ScreenshotDelay, QStringLiteral("Ctrl+Alt+2")},
        {shortcuts::GlobalShortcutAction::ScreenshotFixed, QStringLiteral("Ctrl+Alt+3")},
        {shortcuts::GlobalShortcutAction::ScreenshotOcr, QStringLiteral("Ctrl+Alt+4")},
        {shortcuts::GlobalShortcutAction::ScreenshotTranslation, QStringLiteral("Ctrl+Alt+5")},
        {shortcuts::GlobalShortcutAction::ScreenshotCopy, QStringLiteral("Ctrl+Alt+6")},
        {shortcuts::GlobalShortcutAction::ScreenshotFullScreen, QStringLiteral("Ctrl+Alt+7")},
        {shortcuts::GlobalShortcutAction::ScreenshotFocusedWindow, QStringLiteral("Ctrl+Alt+8")},
        {shortcuts::GlobalShortcutAction::ScreenRecord, QStringLiteral("Ctrl+Alt+9")},
        {shortcuts::GlobalShortcutAction::ScreenRecordCopy, QStringLiteral("Ctrl+Alt+A")},
        {shortcuts::GlobalShortcutAction::OpenCaptureHistory, QStringLiteral("Ctrl+Alt+B")},
        {shortcuts::GlobalShortcutAction::OpenSettings, QStringLiteral("Ctrl+Alt+C")},
        {shortcuts::GlobalShortcutAction::PinClipboardContent,
         QStringLiteral("Ctrl+Alt+D")},
    };

    {
        auto backend = std::make_unique<FakeGlobalShortcutBackend>();
        shortcuts::GlobalShortcutManager manager(std::move(backend), settings.organization,
                                                 settings.application);
        manager.initialize();
        for (const auto& [action, shortcut] : actions) {
            manager.setShortcuts(action, {shortcut});
            const auto state = manager.state(action);
            require(state.status == shortcuts::GlobalShortcutStatus::Registered &&
                        state.shortcuts == QStringList{shortcut},
                    "quick action shortcut did not register independently");
        }
    }

    auto restoredBackend = std::make_unique<FakeGlobalShortcutBackend>();
    auto* const restoredBackendPtr = restoredBackend.get();
    shortcuts::GlobalShortcutManager restored(std::move(restoredBackend), settings.organization,
                                              settings.application);
    restored.initialize();
    for (const auto& [action, shortcut] : actions) {
        const auto state = restored.state(action);
        require(state.status == shortcuts::GlobalShortcutStatus::Registered &&
                    state.shortcuts == QStringList{shortcut},
                "quick action shortcut did not survive manager restart");
    }
    int activationCount = 0;
    shortcuts::GlobalShortcutAction activatedAction = shortcuts::GlobalShortcutAction::Screenshot;
    QObject::connect(&restored, &shortcuts::GlobalShortcutManager::activated, &restored,
                     [&activationCount, &activatedAction](shortcuts::GlobalShortcutAction action) {
                         ++activationCount;
                         activatedAction = action;
                     });
    restoredBackendPtr->activate(QStringLiteral("Ctrl+Alt+7"));
    require(activationCount == 1 &&
                activatedAction == shortcuts::GlobalShortcutAction::ScreenshotFocusedWindow,
            "quick action activation did not dispatch its owning action");
}

void focusedFullscreenSuppressionIsCheckedForEveryActivation() {
    IsolatedSettings settings;
    auto backend = std::make_unique<FakeGlobalShortcutBackend>();
    auto* const backendPtr = backend.get();
    bool focusedFullscreen = true;
    shortcuts::GlobalShortcutManager manager(
        std::move(backend), settings.organization, settings.application, nullptr,
        [&focusedFullscreen]() { return focusedFullscreen; });
    manager.initialize();
    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot,
                         {QStringLiteral("Ctrl+Alt+1")});

    int activationCount = 0;
    QObject::connect(&manager, &shortcuts::GlobalShortcutManager::activated, &manager,
                     [&activationCount](shortcuts::GlobalShortcutAction) {
                         ++activationCount;
                     });
    backendPtr->activate(QStringLiteral("Ctrl+Alt+1"));
    require(activationCount == 1,
            "fullscreen detection should not suppress hotkeys while the setting is disabled");

    require(snow_shot::storage::GlobalShortcutSettings()
                .setDisableOnFocusedFullscreenWindow(true),
            "fullscreen hotkey suppression should be configurable");
    backendPtr->activate(QStringLiteral("Ctrl+Alt+1"));
    require(activationCount == 1,
            "a focused fullscreen window should suppress the activated hotkey");

    focusedFullscreen = false;
    backendPtr->activate(QStringLiteral("Ctrl+Alt+1"));
    require(activationCount == 2,
            "hotkeys should resume as soon as no focused fullscreen window exists");
}

void shortcutFunctionsCanBeDisabledWithoutReconciliation() {
    IsolatedSettings settings;
    auto backend = std::make_unique<FakeGlobalShortcutBackend>();
    auto* const backendPtr = backend.get();
    shortcuts::GlobalShortcutManager manager(std::move(backend), settings.organization,
                                              settings.application);
    manager.initialize();
    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot,
                         {QStringLiteral("Ctrl+Alt+1")});

    int activationCount = 0;
    QObject::connect(&manager, &shortcuts::GlobalShortcutManager::activated, &manager,
                     [&activationCount](shortcuts::GlobalShortcutAction) { ++activationCount; });
    backendPtr->activate(QStringLiteral("Ctrl+Alt+1"));
    require(activationCount == 1 && manager.shortcutFunctionsEnabled(),
            "global shortcuts should activate before the disable gate is enabled");

    manager.setShortcutFunctionsEnabled(false);
    const auto stateWhileDisabled =
        manager.state(shortcuts::GlobalShortcutAction::Screenshot);
    backendPtr->activate(QStringLiteral("Ctrl+Alt+1"));
    require(!manager.shortcutFunctionsEnabled() && activationCount == 1 &&
                stateWhileDisabled.status == shortcuts::GlobalShortcutStatus::Registered,
            "disabling shortcut functions should suppress activation without unregistering keys");

    manager.setShortcutFunctionsEnabled(true);
    backendPtr->activate(QStringLiteral("Ctrl+Alt+1"));
    require(manager.shortcutFunctionsEnabled() && activationCount == 2,
            "re-enabling shortcut functions should resume activation immediately");
}

#ifdef Q_OS_WIN
void nativeWindowsBackendRegistersAndReleases() {
    IsolatedSettings settings;
    shortcuts::GlobalShortcutManager manager(std::unique_ptr<shortcuts::GlobalShortcutBackend>(),
                                             settings.organization, settings.application);
    manager.initialize();

    const auto portableShortcut = [](Qt::KeyboardModifiers modifiers, Qt::Key key) {
        return QKeySequence(QKeyCombination(modifiers, key)).toString(QKeySequence::PortableText);
    };
    const Qt::KeyboardModifiers testModifiers =
        Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
    const QString mainKeyboardOne = portableShortcut(testModifiers, Qt::Key_1);
    const QString numpadOne = portableShortcut(testModifiers | Qt::KeypadModifier, Qt::Key_1);

    require(manager.validateShortcut(QStringLiteral("Ctrl+Alt+Shift+F23")).supported,
            "the Windows backend should accept function keys supported by RegisterHotKey");
    require(manager.validateShortcut(portableShortcut(testModifiers, Qt::Key_Escape)).supported,
            "the Windows backend should accept mapped non-character virtual keys");
    require(!manager.validateShortcut(portableShortcut(testModifiers, Qt::Key_F12)).supported,
            "F12 should be rejected because Windows permanently reserves it for debuggers");
    require(!manager.validateShortcut(QStringLiteral("Ctrl")).supported,
            "modifier-only input should not be accepted as a Windows global shortcut");
    require(!manager.validateShortcut(QStringLiteral("Ctrl+1, Ctrl+2")).supported,
            "multi-chord key sequences should not be accepted by RegisterHotKey");
    require(manager.validateShortcut(numpadOne).supported &&
                numpadOne.contains(QStringLiteral("Num")),
            "a distinguishable numpad digit should retain its keypad modifier");
    require(
        !manager
             .validateShortcut(portableShortcut(testModifiers | Qt::KeypadModifier, Qt::Key_Enter))
             .supported,
        "numpad Enter should be rejected because RegisterHotKey cannot distinguish it");
    require(!manager.validateShortcut(portableShortcut(testModifiers, Qt::Key_F25)).supported,
            "function keys beyond the Win32 F1-F24 range should be rejected");

    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot, {mainKeyboardOne, numpadOne});
    const shortcuts::GlobalShortcutRegistrationState keypadState =
        manager.state(shortcuts::GlobalShortcutAction::Screenshot);
    require(keypadState.status == shortcuts::GlobalShortcutStatus::Registered &&
                keypadState.bindings.size() == 2,
            "main keyboard and numpad digits should register as distinct Windows hotkeys");

    manager.setShortcuts(shortcuts::GlobalShortcutAction::Screenshot,
                         {QStringLiteral("Ctrl+Alt+Shift+F23")});
    const shortcuts::GlobalShortcutRegistrationState state =
        manager.state(shortcuts::GlobalShortcutAction::Screenshot);
    require(state.status == shortcuts::GlobalShortcutStatus::Registered,
            "the Windows backend should register a representative native hotkey");
}
#endif
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    registrationStatesReflectActualAvailability();
    persistedDesiredBindingsAreRestored();
    releasedApplicationConflictIsReconciled();
    everyQuickActionHasIndependentPersistenceAndRegistration();
    focusedFullscreenSuppressionIsCheckedForEveryActivation();
    shortcutFunctionsCanBeDisabledWithoutReconciliation();
#ifdef Q_OS_WIN
    nativeWindowsBackendRegistersAndReleases();
#endif
    return 0;
}
