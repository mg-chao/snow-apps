#include "globalshortcutbackend_p.h"

#include <Carbon/Carbon.h>

#include <QHash>
#include <QKeySequence>
#include <QSet>
#include <QTimer>

#include <utility>

namespace snow_shot::presentation {
namespace {

constexpr OSType HOTKEY_SIGNATURE = (static_cast<OSType>('S') << 24U) |
                                    (static_cast<OSType>('n') << 16U) |
                                    (static_cast<OSType>('S') << 8U) | static_cast<OSType>('h');

struct NativeShortcut {
    UInt32 keyCode = 0;
    UInt32 modifiers = 0;
};

quint64 nativeIdentity(const NativeShortcut& native) {
    return (static_cast<quint64>(native.modifiers) << 32U) | native.keyCode;
}

struct ReservationPolicy {
    OSStatus status = noErr;
    OptionBits options = kEventHotKeyExclusive;
};

struct SystemReservations {
    OSStatus status = noErr;
    QHash<quint64, bool> enabledByIdentity;

    bool operator==(const SystemReservations&) const = default;

    ReservationPolicy policyFor(const NativeShortcut& native) const {
        if (status != noErr) {
            return {status, kEventHotKeyExclusive};
        }
        const auto entry = enabledByIdentity.constFind(nativeIdentity(native));
        if (entry == enabledByIdentity.cend()) {
            return {};
        }
        // Disabled symbolic hotkeys retain exclusive reservations. Non-exclusive
        // registrants receive events only while the system owner is disabled.
        return {*entry ? static_cast<OSStatus>(eventHotKeyExistsErr) : static_cast<OSStatus>(noErr),
                kEventHotKeyNoOptions};
    }
};

SystemReservations readSystemReservations(const MacOSHotKeyApi& api) {
    SystemReservations result;
    CFArrayRef symbolicKeys = nullptr;
    result.status = api.copySymbolicHotKeys(&symbolicKeys);
    if (result.status == noErr && symbolicKeys != nullptr) {
        for (CFIndex index = 0; index < CFArrayGetCount(symbolicKeys); ++index) {
            const auto entry =
                static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(symbolicKeys, index));
            const auto code =
                static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kHISymbolicHotKeyCode));
            const auto modifiers =
                static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kHISymbolicHotKeyModifiers));
            SInt32 keyCode = 0;
            SInt32 keyModifiers = 0;
            if (code == nullptr || modifiers == nullptr ||
                !CFNumberGetValue(code, kCFNumberSInt32Type, &keyCode) ||
                !CFNumberGetValue(modifiers, kCFNumberSInt32Type, &keyModifiers)) {
                continue;
            }
            const quint64 identity =
                nativeIdentity({static_cast<UInt32>(keyCode), static_cast<UInt32>(keyModifiers)});
            const bool enabled =
                CFDictionaryGetValue(entry, kHISymbolicHotKeyEnabled) == kCFBooleanTrue;
            // Multiple symbolic actions can share a binding; any enabled owner wins.
            result.enabledByIdentity[identity] =
                result.enabledByIdentity.value(identity) || enabled;
        }
    }
    if (symbolicKeys != nullptr) {
        CFRelease(symbolicKeys);
    }
    return result;
}

bool parseNativeShortcut(const shortcuts::ShortcutBinding& binding, NativeShortcut* output) {
    const QKeySequence sequence =
        QKeySequence::fromString(binding.portableText, QKeySequence::PortableText);
    const auto keyCode = shortcuts::macVirtualKeyForBinding(binding);
    if (output == nullptr || sequence.count() != 1 || !keyCode.has_value()) {
        return false;
    }
    const QKeyCombination combination = sequence[0];
    if (combination.key() == Qt::Key_unknown || combination.key() == Qt::Key_Control ||
        combination.key() == Qt::Key_Alt || combination.key() == Qt::Key_Shift ||
        combination.key() == Qt::Key_Meta || combination.key() == Qt::Key_AltGr) {
        return false;
    }
    // A saved physical key may only disambiguate the position of a keyboard
    // key. It must not turn media/consumer keys into Carbon registrations.
    const Qt::Key logicalKey = combination.key();
    const bool printable = static_cast<int>(logicalKey) >= 0x20 &&
                           static_cast<int>(logicalKey) < static_cast<int>(Qt::Key_Escape);
    shortcuts::ShortcutBinding portableOnly{binding.portableText};
    if (!printable && !shortcuts::macVirtualKeyForBinding(portableOnly).has_value()) {
        return false;
    }
    UInt32 modifiers = 0;
    const Qt::KeyboardModifiers qtModifiers = combination.keyboardModifiers();
    if (qtModifiers.testFlag(Qt::ControlModifier)) {
        modifiers |= cmdKey;
    }
    if (qtModifiers.testFlag(Qt::MetaModifier)) {
        modifiers |= controlKey;
    }
    if (qtModifiers.testFlag(Qt::AltModifier)) {
        modifiers |= optionKey;
    }
    if (qtModifiers.testFlag(Qt::ShiftModifier)) {
        modifiers |= shiftKey;
    }
    if (qtModifiers.testFlag(Qt::GroupSwitchModifier)) {
        return false;
    }
    *output = {*keyCode, modifiers};
    return true;
}

GlobalShortcutValidationResult validation(const shortcuts::ShortcutBinding& binding,
                                          bool supported) {
    GlobalShortcutValidationResult result;
    result.binding = binding;
    result.shortcut = binding.portableText;
    result.supported = supported;
    result.failureReason = supported ? GlobalShortcutFailureReason::None
                                     : GlobalShortcutFailureReason::InvalidShortcut;
    return result;
}

class MacOSGlobalShortcutBackend final : public GlobalShortcutBackend {
  public:
    explicit MacOSGlobalShortcutBackend(MacOSHotKeyApi api)
        : m_api(api), m_reservations(readSystemReservations(api)) {
        // Carbon provides a snapshot but no public symbolic-hotkey change event.
        // Compare normalized snapshots at low frequency, including while all
        // bindings are conflicted, so disabling a system owner can recover them.
        m_availabilityTimer.setInterval(2000);
        m_availabilityTimer.setTimerType(Qt::VeryCoarseTimer);
        QObject::connect(&m_availabilityTimer, &QTimer::timeout, &m_availabilityTimer,
                         [this] { refreshAvailability(); });
        const EventTypeSpec eventTypes[] = {
            {kEventClassKeyboard, kEventHotKeyPressed},
            {kEventClassKeyboard, kEventHotKeyReleased},
        };
        m_handlerStatus = InstallEventHandler(GetApplicationEventTarget(), eventHandler,
                                              static_cast<UInt32>(std::size(eventTypes)),
                                              eventTypes, this, &m_eventHandler);
    }

    ~MacOSGlobalShortcutBackend() override {
        const QList<int> ids = m_registered.keys();
        for (int id : ids) {
            unregisterShortcut(id);
        }
        if (m_eventHandler != nullptr) {
            RemoveEventHandler(m_eventHandler);
        }
    }

    void setActivationHandler(ActivationHandler handler) override {
        m_activationHandler = std::move(handler);
    }

    void setAvailabilityChangedHandler(AvailabilityChangedHandler handler) override {
        m_availabilityChangedHandler = std::move(handler);
        if (m_availabilityChangedHandler) {
            m_availabilityTimer.start();
        } else {
            m_availabilityTimer.stop();
        }
    }

    void refreshAvailability() override {
        updateSystemReservations();
        if (!m_availabilityChanged) {
            return;
        }
        m_availabilityChanged = false;
        QList<int> invalidatedIds;
        for (auto entry = m_registered.cbegin(); entry != m_registered.cend(); ++entry) {
            const auto policy = m_reservations.policyFor(entry->native);
            if (policy.status != noErr || policy.options != entry->options) {
                invalidatedIds.append(entry.key());
            }
        }
        // The manager owns release/re-registration and publishes aggregate UI
        // state. Notify after iteration because the callback can mutate the map.
        if (m_availabilityChangedHandler) {
            m_availabilityChangedHandler(invalidatedIds);
        }
    }

    GlobalShortcutValidationResult
    validateShortcut(const shortcuts::ShortcutBinding& binding) const override {
        NativeShortcut native;
        return validation(binding, parseNativeShortcut(binding, &native));
    }

    GlobalShortcutBackendResult
    registerShortcut(int registrationId, const shortcuts::ShortcutBinding& binding) override {
        NativeShortcut native;
        if (!parseNativeShortcut(binding, &native)) {
            return {false, GlobalShortcutFailureReason::InvalidShortcut, 0};
        }
        if (m_handlerStatus != noErr) {
            return {false, GlobalShortcutFailureReason::SystemError,
                    static_cast<qint64>(m_handlerStatus)};
        }
        // Read afresh for explicit edits/resume as well as periodic changes.
        updateSystemReservations();
        const auto policy = m_reservations.policyFor(native);
        if (policy.status != noErr) {
            return {false,
                    policy.status == eventHotKeyExistsErr
                        ? GlobalShortcutFailureReason::AlreadyInUse
                        : GlobalShortcutFailureReason::SystemError,
                    static_cast<qint64>(policy.status)};
        }
        EventHotKeyRef reference = nullptr;
        const EventHotKeyID id{HOTKEY_SIGNATURE, static_cast<UInt32>(registrationId)};
        const OSStatus status =
            m_api.registerEventHotKey(native.keyCode, native.modifiers, id,
                                      GetApplicationEventTarget(), policy.options, &reference);
        if (status != noErr) {
            return {false,
                    status == eventHotKeyExistsErr ? GlobalShortcutFailureReason::AlreadyInUse
                                                   : GlobalShortcutFailureReason::SystemError,
                    static_cast<qint64>(status)};
        }
        m_registered.insert(registrationId, {reference, native, policy.options});
        return {true, GlobalShortcutFailureReason::None, 0};
    }

    void unregisterShortcut(int registrationId) override {
        EventHotKeyRef reference = m_registered.take(registrationId).reference;
        m_pressed.remove(registrationId);
        if (reference != nullptr) {
            m_api.unregisterEventHotKey(reference);
        }
    }

  private:
    void updateSystemReservations() {
        auto reservations = readSystemReservations(m_api);
        if (reservations != m_reservations) {
            m_reservations = std::move(reservations);
            // Explicit registration can observe changes between timer ticks.
            // Retain that observation even if settings revert before the next
            // tick, so a binding rejected in the meantime is retried.
            m_availabilityChanged = true;
        }
    }

    static OSStatus eventHandler(EventHandlerCallRef, EventRef event, void* context) {
        auto& self = *static_cast<MacOSGlobalShortcutBackend*>(context);
        EventHotKeyID id{};
        const OSStatus parameterStatus = GetEventParameter(
            event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(id), nullptr, &id);
        if (parameterStatus != noErr || id.signature != HOTKEY_SIGNATURE) {
            return eventNotHandledErr;
        }
        const int registrationId = static_cast<int>(id.id);
        if (!self.m_registered.contains(registrationId)) {
            return eventNotHandledErr;
        }
        if (GetEventKind(event) == kEventHotKeyReleased) {
            self.m_pressed.remove(registrationId);
            return noErr;
        }
        if (!self.m_pressed.contains(registrationId)) {
            self.m_pressed.insert(registrationId);
            if (self.m_activationHandler) {
                self.m_activationHandler(registrationId);
            }
        }
        return noErr;
    }

    struct Registration {
        EventHotKeyRef reference = nullptr;
        NativeShortcut native;
        OptionBits options = kEventHotKeyExclusive;
    };

    MacOSHotKeyApi m_api;
    SystemReservations m_reservations;
    bool m_availabilityChanged = false;
    QTimer m_availabilityTimer;
    AvailabilityChangedHandler m_availabilityChangedHandler;
    ActivationHandler m_activationHandler;
    QHash<int, Registration> m_registered;
    QSet<int> m_pressed;
    EventHandlerRef m_eventHandler = nullptr;
    OSStatus m_handlerStatus = noErr;
};

} // namespace

std::unique_ptr<GlobalShortcutBackend> createMacOSGlobalShortcutBackend() {
    return createMacOSGlobalShortcutBackend(MacOSHotKeyApi{});
}

std::unique_ptr<GlobalShortcutBackend> createMacOSGlobalShortcutBackend(MacOSHotKeyApi api) {
    return std::make_unique<MacOSGlobalShortcutBackend>(api);
}

} // namespace snow_shot::presentation
