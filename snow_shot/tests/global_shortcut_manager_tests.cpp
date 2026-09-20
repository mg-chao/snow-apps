#include "snow_shot/platform/focusedfullscreenwindow.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QCoreApplication>
#include <QHash>
#include <QTemporaryDir>

#ifdef Q_OS_MACOS
#include <Carbon/Carbon.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <array>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
using namespace snow_shot::presentation;
namespace shortcuts = snow_shot::shortcuts;

constexpr std::array ALL_ACTIONS{
    GlobalShortcutAction::Screenshot,
    GlobalShortcutAction::ScreenshotDelay,
    GlobalShortcutAction::ScreenshotFixed,
    GlobalShortcutAction::ScreenshotOcr,
    GlobalShortcutAction::ScreenshotTranslation,
    GlobalShortcutAction::ScreenshotCopy,
    GlobalShortcutAction::ScreenshotFullScreen,
    GlobalShortcutAction::ScreenshotFocusedWindow,
    GlobalShortcutAction::ScreenRecord,
    GlobalShortcutAction::ScreenRecordCopy,
    GlobalShortcutAction::OpenScreenRecordingFolder,
    GlobalShortcutAction::OpenCaptureHistory,
    GlobalShortcutAction::OpenSettings,
    GlobalShortcutAction::PinClipboardContent,
    GlobalShortcutAction::TranslateSelectedText,
    GlobalShortcutAction::PinSelectedFiles,
    GlobalShortcutAction::ToggleGlobalHotkeys,
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class FakeBackend final : public GlobalShortcutBackend {
  public:
    void setActivationHandler(ActivationHandler value) override {
        handler = std::move(value);
    }

    GlobalShortcutValidationResult
    validateShortcut(const shortcuts::ShortcutBinding& binding) const override {
        const auto canonical = shortcuts::canonicalBinding(binding);
        return {canonical.portableText, !canonical.portableText.isEmpty(),
                canonical.portableText.isEmpty() ? GlobalShortcutFailureReason::InvalidShortcut
                                                 : GlobalShortcutFailureReason::None,
                canonical};
    }

    GlobalShortcutBackendResult
    registerShortcut(int id, const shortcuts::ShortcutBinding& binding) override {
        ++registerCalls;
        if (const auto failure = failures.constFind(binding.portableText);
            failure != failures.cend()) {
            return *failure;
        }
        registrations.insert(id, binding);
        return {true, GlobalShortcutFailureReason::None, 0};
    }

    void unregisterShortcut(int id) override {
        ++unregisterCalls;
        registrations.remove(id);
    }

    ActivationHandler handler;
    QHash<int, shortcuts::ShortcutBinding> registrations;
    QHash<QString, GlobalShortcutBackendResult> failures;
    int registerCalls = 0;
    int unregisterCalls = 0;
};

void clearAll(GlobalShortcutManager& manager) {
    for (const GlobalShortcutAction action : ALL_ACTIONS) {
        require(manager.setShortcuts(action, {}), "clear global shortcut fixture");
    }
}

void validationCoversSupportedAndRejectedKeys() {
    GlobalShortcutManager manager;
#ifdef Q_OS_MACOS
    constexpr int maximumFunctionKey = 20;
#else
    constexpr int maximumFunctionKey = 24;
#endif
    for (int number = 1; number <= maximumFunctionKey; ++number) {
        const QString shortcut = QStringLiteral("Ctrl+Shift+F%1").arg(number);
        require(manager.validateShortcut(shortcut).supported,
                "supported function key rejected before native registration");
    }
#ifdef Q_OS_MACOS
    for (const QString& shortcut :
         {QStringLiteral("Ctrl+A"), QStringLiteral("Meta+Left"), QStringLiteral("Alt+Delete"),
          QStringLiteral("Shift+Num+1"), QStringLiteral("Ctrl+PgDown")}) {
        require(manager.validateShortcut(shortcut).supported,
                "printable, navigation, editing, or keypad Carbon mapping was rejected");
    }
#endif
    for (const QString& shortcut : {QStringLiteral("Ctrl"), QStringLiteral("Ctrl+K, Ctrl+C"),
                                    QStringLiteral("Num+F12"), QStringLiteral("F25")}) {
        require(!manager.validateShortcut(shortcut).supported,
                "unmappable or multi-stroke shortcut accepted");
    }
#ifdef Q_OS_MACOS
    shortcuts::ShortcutBinding media{QStringLiteral("Volume Up")};
    media.physicalKeys.insert(shortcuts::ShortcutPlatform::MacOS, 72);
    require(!manager.validateShortcut(GlobalShortcutAction::Screenshot, media).supported,
            "physical metadata must not make consumer/media keys registerable");
#endif
}

void fullscreenClassificationUsesTheFocusedLayerZeroWindow() {
    using snow_shot::platform::focusedWindowCoversDisplay;
    using snow_shot::platform::FocusedWindowSnapshot;

    const QVector<QRectF> displays{QRectF(0.0, 0.0, 1728.0, 1117.0),
                                   QRectF(1728.0, 0.0, 1920.0, 1080.0)};
    require(focusedWindowCoversDisplay(41,
                                       {{22, 0, 1.0, displays.first()},
                                        {41, 8, 1.0, displays.first()},
                                        {41, 0, 1.0, QRectF(1727.5, 0.5, 1920.0, 1079.0)}},
                                       displays),
            "the visible frontmost layer-zero window may match any complete display");
    require(!focusedWindowCoversDisplay(
                41,
                {{41, 0, 1.0, QRectF(0.0, 0.0, 1600.0, 1000.0)}, {41, 0, 1.0, displays.first()}},
                displays),
            "the first ordered frontmost layer-zero window must be authoritative");
    require(!focusedWindowCoversDisplay(41,
                                        {{22, 0, 1.0, displays.first()},
                                         {41, 3, 1.0, displays.first()},
                                         {41, 0, 0.0, displays.first()}},
                                        displays),
            "wrong-owner, nonzero-layer, and invisible windows must be ignored");
    require(
        !focusedWindowCoversDisplay(41, {{41, 0, 1.0, QRectF(0.0, 0.0, 1726.0, 1117.0)}}, displays),
        "a partial window outside the one-point tolerance must not be fullscreen");
    require(!focusedWindowCoversDisplay(0, {{41, 0, 1.0, displays.first()}}, displays) &&
                !focusedWindowCoversDisplay(41, {}, displays) &&
                !focusedWindowCoversDisplay(41, {{41, 0, 1.0, displays.first()}}, {}),
            "missing process, window, or display snapshots must fail closed");
}

void deterministicOwnershipPartialFailureAndSuspension() {
    auto native = std::make_unique<FakeBackend>();
    FakeBackend* input = native.get();
    GlobalShortcutManager manager(std::move(native), nullptr, [] { return false; });
    manager.initialize();
    clearAll(manager);
    input->registerCalls = 0;
    input->unregisterCalls = 0;

    const shortcuts::ShortcutBinding shared{QStringLiteral("Ctrl+F12")};
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot, {shared}) &&
                manager.setShortcuts(GlobalShortcutAction::ScreenshotCopy, {shared}),
            "configure colliding actions");
    require(
        manager.state(GlobalShortcutAction::Screenshot).status ==
                GlobalShortcutStatus::Registered &&
            manager.state(GlobalShortcutAction::ScreenshotCopy).status ==
                GlobalShortcutStatus::Failed &&
            manager.state(GlobalShortcutAction::ScreenshotCopy).bindings.first().failureReason ==
                GlobalShortcutFailureReason::AlreadyInUse &&
            input->registrations.size() == 1,
        "earlier action must deterministically own a duplicate runtime identity");
    require(manager.validateShortcut(GlobalShortcutAction::Screenshot, shared).supported &&
                !manager.validateShortcut(GlobalShortcutAction::ScreenshotCopy, shared).supported,
            "validation must allow the owning action and reject other Snow Shot owners");

    input->failures.insert(QStringLiteral("Ctrl+F11"),
                           {false, GlobalShortcutFailureReason::SystemError, -9876});
    require(manager.setShortcuts(GlobalShortcutAction::ScreenshotCopy, {}) &&
                manager.setShortcuts(GlobalShortcutAction::Screenshot,
                                     {QStringLiteral("Ctrl+F10"), QStringLiteral("Ctrl+F11")}),
            "configure partial registration fixture");
    const auto partial = manager.state(GlobalShortcutAction::Screenshot);
    require(partial.status == GlobalShortcutStatus::PartiallyRegistered &&
                partial.bindings.size() == 2 && partial.bindings.at(0).registered &&
                !partial.bindings.at(1).registered &&
                partial.bindings.at(1).nativeErrorCode == -9876,
            "partial state and signed native errors must be preserved");
    require(!manager.validateShortcut(GlobalShortcutAction::ScreenshotCopy,
                                      shortcuts::ShortcutBinding{QStringLiteral("Ctrl+F10")})
                    .supported &&
                manager
                    .validateShortcut(GlobalShortcutAction::ScreenshotCopy,
                                      shortcuts::ShortcutBinding{QStringLiteral("Ctrl+F11")})
                    .supported,
            "collision validation must reject the registered member and ignore the failed member "
            "of a partially registered action");

    const auto firstSuspension = manager.suspendRegistrations();
    const int unregistered = input->unregisterCalls;
    const auto secondSuspension = manager.suspendRegistrations();
    require(input->registrations.isEmpty() && input->unregisterCalls == unregistered,
            "nested suspension must unregister native bindings only once");
    const int registrationsBeforeDraft = input->registerCalls;
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot, {QStringLiteral("Ctrl+F9")}) &&
                input->registerCalls == registrationsBeforeDraft,
            "editing while suspended must update desired state without native registration");
    manager.resumeRegistrations(firstSuspension);
    require(input->registrations.isEmpty(),
            "resuming an inner token must not end another suspension");
    manager.resumeRegistrations(secondSuspension);
    require(input->registrations.size() == 1 &&
                input->registrations.cbegin()->portableText == QStringLiteral("Ctrl+F9"),
            "final resume must perform one reconciliation of the latest desired bindings");

    int activations = 0;
    QObject::connect(&manager, &GlobalShortcutManager::activated, &manager,
                     [&activations](GlobalShortcutAction action) {
                         if (action == GlobalShortcutAction::Screenshot) {
                             ++activations;
                         }
                     });
    input->handler(input->registrations.cbegin().key());
    require(activations == 1, "registered backend activation must dispatch its action");
}

void toggleShortcutSurvivesGlobalHotkeyDisablement() {
    auto native = std::make_unique<FakeBackend>();
    FakeBackend* input = native.get();
    GlobalShortcutManager manager(std::move(native), nullptr, [] { return false; });
    manager.initialize();
    clearAll(manager);
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot, {QStringLiteral("Ctrl+F10")}) &&
                manager.setShortcuts(GlobalShortcutAction::ToggleGlobalHotkeys,
                                     {QStringLiteral("Ctrl+F12")}),
            "configure the toggle and a regular shortcut");

    int screenshotActivations = 0;
    int toggleActivations = 0;
    QObject::connect(&manager, &GlobalShortcutManager::activated, &manager,
                     [&screenshotActivations, &toggleActivations](GlobalShortcutAction action) {
                         if (action == GlobalShortcutAction::Screenshot) {
                             ++screenshotActivations;
                         } else if (action == GlobalShortcutAction::ToggleGlobalHotkeys) {
                             ++toggleActivations;
                         }
                     });
    int enabledNotifications = 0;
    bool lastEnabledState = true;
    QObject::connect(&manager, &GlobalShortcutManager::globalHotkeysEnabledChanged, &manager,
                     [&enabledNotifications, &lastEnabledState](bool enabled) {
                         ++enabledNotifications;
                         lastEnabledState = enabled;
                     });
    int screenshotRegistrationId = 0;
    int toggleRegistrationId = 0;
    for (auto it = input->registrations.cbegin(); it != input->registrations.cend(); ++it) {
        if (it.value().portableText == QStringLiteral("Ctrl+F10")) {
            screenshotRegistrationId = it.key();
        } else if (it.value().portableText == QStringLiteral("Ctrl+F12")) {
            toggleRegistrationId = it.key();
        }
    }
    require(screenshotRegistrationId != 0 && toggleRegistrationId != 0,
            "both bindings must be registered before the disablement check");

    manager.setGlobalHotkeysEnabled(false);
    manager.setGlobalHotkeysEnabled(false);
    require(enabledNotifications == 1 && !lastEnabledState,
            "redundant disable requests must announce the change exactly once");
    input->handler(screenshotRegistrationId);
    input->handler(toggleRegistrationId);
    require(screenshotActivations == 0 && toggleActivations == 1,
            "disabled hotkeys must stay silent except for the toggle shortcut");

    manager.setGlobalHotkeysEnabled(true);
    require(enabledNotifications == 2 && lastEnabledState && manager.globalHotkeysEnabled(),
            "re-enabling global hotkeys must announce the restored state");
    input->handler(screenshotRegistrationId);
    input->handler(toggleRegistrationId);
    require(screenshotActivations == 1 && toggleActivations == 2,
            "re-enabling global hotkeys must restore every activation");
}

void nativeRegistrationProbe() {
#ifdef Q_OS_MACOS
    GlobalShortcutManager manager;
    manager.initialize();
    clearAll(manager);
    EventHotKeyRef probe = nullptr;
    const EventHotKeyID id{0x53536854, 0x6A01}; // "SShT"
    const OSStatus probeStatus = RegisterEventHotKey(
        90, cmdKey | shiftKey, id, GetApplicationEventTarget(), kEventHotKeyExclusive, &probe);
    require(probeStatus == noErr && probe != nullptr,
            "permission-free Carbon hotkey probe must register");
    require(
        manager.setShortcuts(GlobalShortcutAction::Screenshot, {QStringLiteral("Ctrl+Shift+F20")}),
        "persist native conflict probe binding");
    const auto conflict = manager.state(GlobalShortcutAction::Screenshot).bindings.first();
    require(!conflict.registered &&
                conflict.failureReason == GlobalShortcutFailureReason::AlreadyInUse &&
                conflict.nativeErrorCode == static_cast<qint64>(eventHotKeyExistsErr),
            "Carbon exclusive conflict and signed OSStatus must be reported");
    require(UnregisterEventHotKey(probe) == noErr, "release direct Carbon probe");
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot, {}) &&
                manager.setShortcuts(GlobalShortcutAction::Screenshot,
                                     {QStringLiteral("Ctrl+Shift+F20")}) &&
                manager.state(GlobalShortcutAction::Screenshot).status ==
                    GlobalShortcutStatus::Registered,
            "Carbon binding must unregister and re-register after a conflict disappears");
#elif defined(Q_OS_WIN)
    GlobalShortcutManager manager;
    manager.initialize();
    clearAll(manager);
    constexpr int probeId = 0x6A01;
    const bool available =
        RegisterHotKey(nullptr, probeId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12) != FALSE;
    const DWORD error = GetLastError();
    require(
        manager.setShortcuts(GlobalShortcutAction::Screenshot, {QStringLiteral("Ctrl+Shift+F12")}),
        "persist Windows native probe binding");
    const auto result = manager.state(GlobalShortcutAction::Screenshot).bindings.first();
    require(result.registered != available &&
                result.failureReason == GlobalShortcutFailureReason::AlreadyInUse,
            "Windows native registration must observe the direct probe");
    if (available) {
        require(UnregisterHotKey(nullptr, probeId) != FALSE, "release Windows direct probe");
    } else {
        std::cout << "Direct Windows probe unavailable: " << error << '\n';
    }
#endif
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "test storage directory must be available");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage
                .initialize({.executableDirectory = temporary.filePath(QStringLiteral("bin")),
                             .appDataDirectory = temporary.path()})
                .success,
            "initialize shortcut test storage");
    validationCoversSupportedAndRejectedKeys();
    fullscreenClassificationUsesTheFocusedLayerZeroWindow();
    deterministicOwnershipPartialFailureAndSuspension();
    toggleShortcutSurvivesGlobalHotkeyDisablement();
    if (application.arguments().contains(QStringLiteral("--native-registration-smoke"))) {
        nativeRegistrationProbe();
    }
    storage.shutdown();
    return 0;
}
