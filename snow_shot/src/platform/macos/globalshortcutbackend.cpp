#include "../../presentation/services/globalshortcutbackend_p.h"

#include <Carbon/Carbon.h>

#include <QHash>
#include <QKeySequence>
#include <QSet>

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
    MacOSGlobalShortcutBackend() {
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
        EventHotKeyRef reference = nullptr;
        const EventHotKeyID id{HOTKEY_SIGNATURE, static_cast<UInt32>(registrationId)};
        const OSStatus status =
            RegisterEventHotKey(native.keyCode, native.modifiers, id, GetApplicationEventTarget(),
                                kEventHotKeyExclusive, &reference);
        if (status != noErr) {
            return {false,
                    status == eventHotKeyExistsErr ? GlobalShortcutFailureReason::AlreadyInUse
                                                   : GlobalShortcutFailureReason::SystemError,
                    static_cast<qint64>(status)};
        }
        m_registered.insert(registrationId, reference);
        return {true, GlobalShortcutFailureReason::None, 0};
    }

    void unregisterShortcut(int registrationId) override {
        EventHotKeyRef reference = m_registered.take(registrationId);
        m_pressed.remove(registrationId);
        if (reference != nullptr) {
            UnregisterEventHotKey(reference);
        }
    }

  private:
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

    ActivationHandler m_activationHandler;
    QHash<int, EventHotKeyRef> m_registered;
    QSet<int> m_pressed;
    EventHandlerRef m_eventHandler = nullptr;
    OSStatus m_handlerStatus = noErr;
};

} // namespace

std::unique_ptr<GlobalShortcutBackend> createMacOSGlobalShortcutBackend() {
    return std::make_unique<MacOSGlobalShortcutBackend>();
}

} // namespace snow_shot::presentation
