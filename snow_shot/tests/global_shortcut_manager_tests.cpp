#include "snow_shot/platform/focusedfullscreenwindow.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QCoreApplication>
#include <QHash>
#include <QTemporaryDir>

#ifdef Q_OS_MACOS
#include "../src/platform/macos/globalshortcutbackend_p.h"
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
    GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow,
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

    void setAvailabilityChangedHandler(AvailabilityChangedHandler value) override {
        availabilityHandler = std::move(value);
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
    AvailabilityChangedHandler availabilityHandler;
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

void backendAvailabilityInvalidatesOwnershipAndRecovers() {
    auto backend = std::make_unique<FakeBackend>();
    auto* input = backend.get();
    GlobalShortcutManager manager(std::move(backend), nullptr, [] { return false; });
    manager.initialize();
    clearAll(manager);
    const QString binding = QStringLiteral("Ctrl+F12");
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot, {{binding}}),
            "configure availability fixture");
    const int oldId = input->registrations.constBegin().key();
    int activations = 0;
    QObject::connect(&manager, &GlobalShortcutManager::activated, &manager,
                     [&](GlobalShortcutAction) { ++activations; });
    require(static_cast<bool>(input->availabilityHandler),
            "manager must subscribe to backend availability changes");
    input->failures.insert(binding, {false, GlobalShortcutFailureReason::AlreadyInUse, 123});
    input->availabilityHandler({oldId});
    input->handler(oldId);
    require(input->registrations.isEmpty() && activations == 0 &&
                manager.state(GlobalShortcutAction::Screenshot).status ==
                    GlobalShortcutStatus::Failed,
            "invalidated IDs must lose ownership and reject already-queued activation events");
    const int releases = input->unregisterCalls;
    input->failures.clear();
    input->availabilityHandler({});
    require(input->registrations.size() == 1 && input->registrations.constBegin().key() != oldId &&
                manager.state(GlobalShortcutAction::Screenshot).status ==
                    GlobalShortcutStatus::Registered,
            "availability recovery with no active IDs must retry failed bindings");
    const int newId = input->registrations.constBegin().key();
    input->availabilityHandler({oldId});
    input->handler(oldId);
    input->handler(newId);
    require(input->unregisterCalls == releases && activations == 1 &&
                input->registrations.contains(newId),
            "stale invalidations must not remove or activate a replacement registration");
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

void fullscreenToggleSurvivesFullscreenSuppression() {
    auto native = std::make_unique<FakeBackend>();
    FakeBackend* input = native.get();
    bool focusedFullscreen = false;
    GlobalShortcutManager manager(std::move(native), nullptr,
                                  [&focusedFullscreen] { return focusedFullscreen; });
    manager.initialize();
    clearAll(manager);
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot, {QStringLiteral("Ctrl+F10")}) &&
                manager.setShortcuts(GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow,
                                     {QStringLiteral("Ctrl+F11")}),
            "configure the fullscreen toggle and a regular shortcut");

    auto& store = snow_shot::storage::ApplicationStorage::instance().configuration();
    const QString suppressionKey =
        QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window");
    const auto previousSuppression = store.value(suppressionKey);
    require(store.setValue(suppressionKey, true), "enable fullscreen suppression");

    int screenshotActivations = 0;
    int toggleActivations = 0;
    QObject::connect(&manager, &GlobalShortcutManager::activated, &manager,
                     [&screenshotActivations, &toggleActivations](GlobalShortcutAction action) {
                         if (action == GlobalShortcutAction::Screenshot) {
                             ++screenshotActivations;
                         } else if (action ==
                                    GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow) {
                             ++toggleActivations;
                         }
                     });
    int screenshotRegistrationId = 0;
    int toggleRegistrationId = 0;
    for (auto it = input->registrations.cbegin(); it != input->registrations.cend(); ++it) {
        if (it.value().portableText == QStringLiteral("Ctrl+F10")) {
            screenshotRegistrationId = it.key();
        } else if (it.value().portableText == QStringLiteral("Ctrl+F11")) {
            toggleRegistrationId = it.key();
        }
    }
    require(screenshotRegistrationId != 0 && toggleRegistrationId != 0,
            "both bindings must be registered before the suppression check");

    focusedFullscreen = true;
    input->handler(screenshotRegistrationId);
    input->handler(toggleRegistrationId);
    require(screenshotActivations == 0 && toggleActivations == 1,
            "fullscreen suppression must stay silent except for the fullscreen toggle shortcut");

    focusedFullscreen = false;
    input->handler(screenshotRegistrationId);
    input->handler(toggleRegistrationId);
    require(screenshotActivations == 1 && toggleActivations == 2,
            "leaving fullscreen must restore every activation");

    require(store.setValue(suppressionKey, previousSuppression),
            "restore the fullscreen suppression preference");
}

void gateControlShortcutsSurviveBothHotkeyGates() {
    for (const GlobalShortcutAction action : ALL_ACTIONS) {
        const bool expected =
            action == GlobalShortcutAction::ToggleGlobalHotkeys ||
            action == GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow;
        require(controlsGlobalHotkeyGates(action) == expected,
                "only the hotkey-gate toggles may bypass activation gates");
    }

    auto native = std::make_unique<FakeBackend>();
    FakeBackend* input = native.get();
    bool focusedFullscreen = false;
    GlobalShortcutManager manager(std::move(native), nullptr,
                                  [&focusedFullscreen] { return focusedFullscreen; });
    manager.initialize();
    clearAll(manager);
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot, {QStringLiteral("Ctrl+F10")}) &&
                manager.setShortcuts(GlobalShortcutAction::ToggleGlobalHotkeys,
                                     {QStringLiteral("Ctrl+F12")}) &&
                manager.setShortcuts(GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow,
                                     {QStringLiteral("Ctrl+F11")}),
            "configure both gate-control shortcuts and a regular shortcut");

    auto& store = snow_shot::storage::ApplicationStorage::instance().configuration();
    const QString suppressionKey =
        QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window");
    const auto previousSuppression = store.value(suppressionKey);
    require(store.setValue(suppressionKey, true), "enable fullscreen suppression");

    int screenshotActivations = 0;
    int globalToggleActivations = 0;
    int fullscreenToggleActivations = 0;
    QObject::connect(&manager, &GlobalShortcutManager::activated, &manager,
                     [&screenshotActivations, &globalToggleActivations,
                      &fullscreenToggleActivations](GlobalShortcutAction action) {
                         if (action == GlobalShortcutAction::Screenshot) {
                             ++screenshotActivations;
                         } else if (action == GlobalShortcutAction::ToggleGlobalHotkeys) {
                             ++globalToggleActivations;
                         } else if (action ==
                                    GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow) {
                             ++fullscreenToggleActivations;
                         }
                     });
    int screenshotId = 0;
    int globalToggleId = 0;
    int fullscreenToggleId = 0;
    for (auto it = input->registrations.cbegin(); it != input->registrations.cend(); ++it) {
        if (it.value().portableText == QStringLiteral("Ctrl+F10")) {
            screenshotId = it.key();
        } else if (it.value().portableText == QStringLiteral("Ctrl+F11")) {
            fullscreenToggleId = it.key();
        } else if (it.value().portableText == QStringLiteral("Ctrl+F12")) {
            globalToggleId = it.key();
        }
    }
    require(screenshotId != 0 && globalToggleId != 0 && fullscreenToggleId != 0,
            "all three bindings must be registered before the gate check");

    const auto fireAll = [&]() {
        input->handler(screenshotId);
        input->handler(globalToggleId);
        input->handler(fullscreenToggleId);
    };

    manager.setGlobalHotkeysEnabled(false);
    focusedFullscreen = true;
    fireAll();
    require(screenshotActivations == 0 && globalToggleActivations == 1 &&
                fullscreenToggleActivations == 1,
            "both gate-control shortcuts must stay usable when every hotkey gate is closed");

    manager.setGlobalHotkeysEnabled(true);
    fireAll();
    require(screenshotActivations == 0 && globalToggleActivations == 2 &&
                fullscreenToggleActivations == 2,
            "re-enabling the session gate must not restore regular shortcuts while fullscreen "
            "suppression still applies");

    focusedFullscreen = false;
    manager.setGlobalHotkeysEnabled(false);
    fireAll();
    require(screenshotActivations == 0 && globalToggleActivations == 3 &&
                fullscreenToggleActivations == 3,
            "closing only the session gate must still silence regular shortcuts");

    manager.setGlobalHotkeysEnabled(true);
    fireAll();
    require(screenshotActivations == 1 && globalToggleActivations == 4 &&
                fullscreenToggleActivations == 4,
            "opening both gates must restore every activation");

    require(store.setValue(suppressionKey, previousSuppression),
            "restore the fullscreen suppression preference");
}

#ifdef Q_OS_MACOS
struct SymbolicHotKeyFixture {
    SInt32 keyCode = kVK_ANSI_1;
    SInt32 modifiers = controlKey;
    bool enabled = false;
};

QList<SymbolicHotKeyFixture> symbolicHotKeys;
OSStatus symbolicLookupStatus = noErr;
OSStatus nativeRegistrationStatus = noErr;
int nativeRegistrationCalls = 0;
int nativeUnregistrationCalls = 0;
UInt32 lastNativeKey = 0;
UInt32 lastNativeModifiers = 0;
OptionBits lastNativeOptions = 0;

OSStatus copySymbolicHotKeyFixture(CFArrayRef* output) {
    auto array = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (const auto& entry : symbolicHotKeys) {
        auto dictionary = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
        const auto code = CFNumberCreate(nullptr, kCFNumberSInt32Type, &entry.keyCode);
        const auto modifiers = CFNumberCreate(nullptr, kCFNumberSInt32Type, &entry.modifiers);
        CFDictionarySetValue(dictionary, kHISymbolicHotKeyCode, code);
        CFDictionarySetValue(dictionary, kHISymbolicHotKeyModifiers, modifiers);
        CFDictionarySetValue(dictionary, kHISymbolicHotKeyEnabled,
                             entry.enabled ? kCFBooleanTrue : kCFBooleanFalse);
        CFArrayAppendValue(array, dictionary);
        CFRelease(code);
        CFRelease(modifiers);
        CFRelease(dictionary);
    }
    *output = array;
    return symbolicLookupStatus;
}

OSStatus registerHotKeyFixture(UInt32 code, UInt32 modifiers, EventHotKeyID, EventTargetRef,
                               OptionBits options, EventHotKeyRef* output) {
    ++nativeRegistrationCalls;
    lastNativeKey = code;
    lastNativeModifiers = modifiers;
    lastNativeOptions = options;
    if (nativeRegistrationStatus != noErr) {
        return nativeRegistrationStatus;
    }
    for (const auto& entry : symbolicHotKeys) {
        if (static_cast<UInt32>(entry.keyCode) == code &&
            static_cast<UInt32>(entry.modifiers) == modifiers && options == kEventHotKeyExclusive) {
            return eventHotKeyExistsErr;
        }
    }
    *output = reinterpret_cast<EventHotKeyRef>(&nativeRegistrationCalls);
    return noErr;
}

OSStatus unregisterHotKeyFixture(EventHotKeyRef) {
    ++nativeUnregistrationCalls;
    return noErr;
}

void disabledMacOSSystemReservationsRemainUsable() {
    auto backend = createMacOSGlobalShortcutBackend(
        {copySymbolicHotKeyFixture, registerHotKeyFixture, unregisterHotKeyFixture});
    // Qt PortableText Meta represents physical Control on macOS.
    const shortcuts::ShortcutBinding controlOne{QStringLiteral("Meta+1")};
    symbolicHotKeys = {{kVK_ANSI_1, controlKey, false}};
    require(backend->registerShortcut(1, controlOne).registered && lastNativeKey == kVK_ANSI_1 &&
                lastNativeModifiers == controlKey && lastNativeOptions == kEventHotKeyNoOptions,
            "disabled Control+1 system reservation must allow normal Carbon registration");
    backend->unregisterShortcut(1);
    require(nativeUnregistrationCalls == 1, "normal Carbon registration must be released");

    symbolicHotKeys.append({kVK_ANSI_1, controlKey, true});
    const int callsBeforeConflict = nativeRegistrationCalls;
    const auto conflict = backend->registerShortcut(2, controlOne);
    require(!conflict.registered &&
                conflict.failureReason == GlobalShortcutFailureReason::AlreadyInUse &&
                conflict.nativeErrorCode == eventHotKeyExistsErr &&
                nativeRegistrationCalls == callsBeforeConflict,
            "an enabled system owner must win even if a disabled entry also matches");

    symbolicHotKeys = {{kVK_ANSI_2, controlKey | shiftKey, false}};
    require(backend->registerShortcut(3, {QStringLiteral("Meta+Shift+2")}).registered &&
                lastNativeKey == kVK_ANSI_2 && lastNativeModifiers == (controlKey | shiftKey) &&
                lastNativeOptions == kEventHotKeyNoOptions,
            "disabled system reservations must work for other keys and modifier combinations");
    backend->unregisterShortcut(3);

    symbolicHotKeys = {{kVK_ANSI_1, controlKey, true}};
    require(backend->registerShortcut(4, {QStringLiteral("Ctrl+1")}).registered &&
                lastNativeModifiers == cmdKey && lastNativeOptions == kEventHotKeyExclusive,
            "Command+1 must remain distinct from the system's Control+1 reservation");
    backend->unregisterShortcut(4);

    symbolicHotKeys.clear();
    require(backend->registerShortcut(5, controlOne).registered &&
                lastNativeOptions == kEventHotKeyExclusive,
            "unreserved shortcuts must retain exclusive registration");
    backend->unregisterShortcut(5);
    nativeRegistrationStatus = eventHotKeyExistsErr;
    const auto nativeConflict = backend->registerShortcut(6, controlOne);
    require(!nativeConflict.registered &&
                nativeConflict.failureReason == GlobalShortcutFailureReason::AlreadyInUse &&
                nativeConflict.nativeErrorCode == eventHotKeyExistsErr,
            "real native registration conflicts must remain visible");
    nativeRegistrationStatus = noErr;
    symbolicLookupStatus = paramErr;
    const int callsBeforeLookupFailure = nativeRegistrationCalls;
    const auto lookupFailure = backend->registerShortcut(7, controlOne);
    require(!lookupFailure.registered &&
                lookupFailure.failureReason == GlobalShortcutFailureReason::SystemError &&
                lookupFailure.nativeErrorCode == paramErr &&
                nativeRegistrationCalls == callsBeforeLookupFailure,
            "failed system lookup must preserve the error without attempting registration");
    symbolicLookupStatus = noErr;
}

void macOSSystemReservationChangesReconcileLiveBindings() {
    symbolicHotKeys = {{kVK_ANSI_1, controlKey, false}};
    auto backend = createMacOSGlobalShortcutBackend(
        {copySymbolicHotKeyFixture, registerHotKeyFixture, unregisterHotKeyFixture});
    auto* input = backend.get();
    GlobalShortcutManager manager(std::move(backend), nullptr, [] { return false; });
    manager.initialize();
    clearAll(manager);
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot,
                                 {{QStringLiteral("Meta+1")}, {QStringLiteral("Ctrl+1")}}),
            "configure a system-reserved binding and an independent binding");
    require(manager.state(GlobalShortcutAction::Screenshot).status ==
                GlobalShortcutStatus::Registered,
            "disabled system reservation must initially be usable");
    int stateChanges = 0;
    QObject::connect(&manager, &GlobalShortcutManager::stateChanged, &manager,
                     [&](GlobalShortcutAction action, const GlobalShortcutRegistrationState&) {
                         if (action == GlobalShortcutAction::Screenshot) {
                             ++stateChanges;
                         }
                     });
    const int registrationsBefore = nativeRegistrationCalls;
    const int unregistrationsBefore = nativeUnregistrationCalls;
    symbolicHotKeys.first().enabled = true;
    input->refreshAvailability();
    const auto conflict = manager.state(GlobalShortcutAction::Screenshot);
    require(
        conflict.status == GlobalShortcutStatus::PartiallyRegistered &&
            conflict.bindings.first().failureReason == GlobalShortcutFailureReason::AlreadyInUse &&
            conflict.bindings.first().nativeErrorCode == eventHotKeyExistsErr &&
            conflict.bindings.last().registered && stateChanges == 1 &&
            nativeRegistrationCalls == registrationsBefore &&
            nativeUnregistrationCalls == unregistrationsBefore + 1,
        "enabling a system shortcut must invalidate only its live binding and publish conflict");
    input->refreshAvailability();
    require(stateChanges == 1 && nativeRegistrationCalls == registrationsBefore &&
                nativeUnregistrationCalls == unregistrationsBefore + 1,
            "unchanged reservations must not churn registrations or state notifications");
    symbolicHotKeys.first().enabled = false;
    input->refreshAvailability();
    require(manager.state(GlobalShortcutAction::Screenshot).status ==
                    GlobalShortcutStatus::Registered &&
                stateChanges == 2 && nativeRegistrationCalls == registrationsBefore + 1 &&
                lastNativeOptions == kEventHotKeyNoOptions,
            "disabling the system shortcut must automatically recover the failed binding");
    symbolicHotKeys.clear();
    input->refreshAvailability();
    require(manager.state(GlobalShortcutAction::Screenshot).status ==
                    GlobalShortcutStatus::Registered &&
                stateChanges == 2 && nativeRegistrationCalls == registrationsBefore + 2 &&
                nativeUnregistrationCalls == unregistrationsBefore + 2 &&
                lastNativeOptions == kEventHotKeyExclusive,
            "removing a reservation must restore exclusive ownership without changing UI state");
    symbolicHotKeys = {{kVK_ANSI_1, controlKey, false}};
    input->refreshAvailability();
    require(lastNativeOptions == kEventHotKeyNoOptions &&
                nativeRegistrationCalls == registrationsBefore + 3,
            "new disabled reservations must downgrade only the matching registration");
    const auto suspension = manager.suspendRegistrations();
    const int suspendedCalls = nativeRegistrationCalls;
    symbolicHotKeys.first().enabled = true;
    input->refreshAvailability();
    symbolicHotKeys.first().enabled = false;
    input->refreshAvailability();
    require(nativeRegistrationCalls == suspendedCalls,
            "system changes must not register shortcuts while capture has suspended them");
    manager.resumeRegistrations(suspension);
    require(manager.state(GlobalShortcutAction::Screenshot).status ==
                GlobalShortcutStatus::Registered,
            "resume must use current system reservations");
    symbolicLookupStatus = paramErr;
    input->refreshAvailability();
    const auto failedLookup = manager.state(GlobalShortcutAction::Screenshot);
    require(failedLookup.status == GlobalShortcutStatus::Failed &&
                failedLookup.bindings.first().failureReason ==
                    GlobalShortcutFailureReason::SystemError &&
                failedLookup.bindings.first().nativeErrorCode == paramErr,
            "reservation lookup errors must not leave stale success states");
    symbolicLookupStatus = noErr;
    input->refreshAvailability();
    require(manager.state(GlobalShortcutAction::Screenshot).status ==
                GlobalShortcutStatus::Registered,
            "successful lookup after an error must recover live bindings");
    // Equivalent ordering and duplicate entries must not trigger a reconcile.
    const int callsBeforeEquivalent = nativeRegistrationCalls;
    const int releasesBeforeEquivalent = nativeUnregistrationCalls;
    symbolicHotKeys.append(symbolicHotKeys.first());
    input->refreshAvailability();
    require(nativeRegistrationCalls == callsBeforeEquivalent &&
                nativeUnregistrationCalls == releasesBeforeEquivalent,
            "equivalent reservation snapshots must not churn native registrations");

    // Registration itself may observe an enabled owner between polling ticks.
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot, {}), "remove live bindings");
    symbolicHotKeys = {{kVK_ANSI_1, controlKey, true}};
    require(manager.setShortcuts(GlobalShortcutAction::Screenshot, {{QStringLiteral("Meta+1")}}) &&
                manager.state(GlobalShortcutAction::Screenshot).status ==
                    GlobalShortcutStatus::Failed,
            "an explicit edit must read current reservations before the next timer tick");
    symbolicHotKeys.first().enabled = false;
    input->refreshAvailability();
    require(
        manager.state(GlobalShortcutAction::Screenshot).status == GlobalShortcutStatus::Registered,
        "a failed edit between ticks must recover even when settings revert to the last snapshot");
    symbolicHotKeys.clear();
}
#endif

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
    backendAvailabilityInvalidatesOwnershipAndRecovers();
#ifdef Q_OS_MACOS
    disabledMacOSSystemReservationsRemainUsable();
    macOSSystemReservationChangesReconcileLiveBindings();
#endif
    fullscreenClassificationUsesTheFocusedLayerZeroWindow();
    deterministicOwnershipPartialFailureAndSuspension();
    toggleShortcutSurvivesGlobalHotkeyDisablement();
    fullscreenToggleSurvivesFullscreenSuppression();
    gateControlShortcutsSurviveBothHotkeyGates();
    if (application.arguments().contains(QStringLiteral("--native-registration-smoke"))) {
        nativeRegistrationProbe();
    }
    storage.shutdown();
    return 0;
}
