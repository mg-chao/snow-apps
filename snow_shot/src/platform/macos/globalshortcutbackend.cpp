#include <Carbon/Carbon.h>

#include "snow_shot/platform/macos/globalshortcutbackend.h"

#include <QCoreApplication>
#include <QHash>
#include <QKeySequence>

#include <array>
#include <optional>
#include <utility>

namespace snow_shot::platform::macos {
namespace {
using namespace snow_shot::presentation;
constexpr OSType kShortcutSignature = 0x534e4f57;

std::optional<UInt32> keyCode(Qt::Key key) {
    static constexpr std::array<UInt32, 20> functionKeys = {
        kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,  kVK_F8,  kVK_F9,  kVK_F10,
        kVK_F11, kVK_F12, kVK_F13, kVK_F14, kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20,
    };
    if (key >= Qt::Key_F1 && key <= Qt::Key_F20) {
        return functionKeys[static_cast<std::size_t>(key - Qt::Key_F1)];
    }
    switch (key) {
    case Qt::Key_Return:
        return kVK_Return;
    case Qt::Key_Enter:
        return kVK_ANSI_KeypadEnter;
    case Qt::Key_Tab:
        return kVK_Tab;
    case Qt::Key_Space:
        return kVK_Space;
    case Qt::Key_Backspace:
        return kVK_Delete;
    case Qt::Key_Delete:
        return kVK_ForwardDelete;
    case Qt::Key_Escape:
        return kVK_Escape;
    case Qt::Key_Left:
        return kVK_LeftArrow;
    case Qt::Key_Right:
        return kVK_RightArrow;
    case Qt::Key_Up:
        return kVK_UpArrow;
    case Qt::Key_Down:
        return kVK_DownArrow;
    case Qt::Key_Home:
        return kVK_Home;
    case Qt::Key_End:
        return kVK_End;
    case Qt::Key_PageUp:
        return kVK_PageUp;
    case Qt::Key_PageDown:
        return kVK_PageDown;
    default:
        break;
    }
    if (key < Qt::Key_Exclam || key > Qt::Key_ydiaeresis) {
        return std::nullopt;
    }
    // Resolve printable keys in the active layout rather than assuming a US keyboard.
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (source == nullptr) {
        return std::nullopt;
    }
    auto* data =
        static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    std::optional<UInt32> result;
    if (data != nullptr) {
        const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        for (UInt16 code = 0; code < 128; ++code) {
            UInt32 deadState = 0;
            UniChar characters[4]{};
            UniCharCount length = 0;
            const OSStatus status = UCKeyTranslate(
                layout, code, kUCKeyActionDown, 0, static_cast<UInt32>(LMGetKbdType()),
                kUCKeyTranslateNoDeadKeysBit, &deadState, 4, &length, characters);
            if (status == noErr && length == 1 &&
                QChar(characters[0]).toUpper().unicode() == static_cast<ushort>(key)) {
                result = code;
                break;
            }
        }
    }
    CFRelease(source);
    return result;
}

class MacOsGlobalShortcutBackend final : public GlobalShortcutBackend {
  public:
    MacOsGlobalShortcutBackend() {
        const EventTypeSpec eventType{kEventClassKeyboard, kEventHotKeyPressed};
        m_installStatus =
            InstallApplicationEventHandler(&handleEvent, 1, &eventType, this, &m_eventHandler);
    }

    ~MacOsGlobalShortcutBackend() override {
        for (EventHotKeyRef key : std::as_const(m_keys)) {
            UnregisterEventHotKey(key);
        }
        if (m_eventHandler != nullptr) {
            RemoveEventHandler(m_eventHandler);
        }
    }

    void setActivationHandler(ActivationHandler handler) override {
        m_handler = std::move(handler);
    }

    GlobalShortcutValidationResult validateShortcut(const QString& shortcut) const override {
        const bool valid = nativeShortcut(shortcut).valid;
        return {shortcut, valid,
                valid ? GlobalShortcutFailureReason::None
                      : GlobalShortcutFailureReason::InvalidShortcut};
    }

    GlobalShortcutBackendResult registerShortcut(int registrationId,
                                                 const QString& shortcut) override {
        const NativeShortcut native = nativeShortcut(shortcut);
        if (!native.valid) {
            return {false, GlobalShortcutFailureReason::InvalidShortcut, 0};
        }
        if (m_installStatus != noErr) {
            return {false, GlobalShortcutFailureReason::SystemError,
                    static_cast<quint32>(m_installStatus)};
        }
        unregisterShortcut(registrationId);
        EventHotKeyRef reference = nullptr;
        const EventHotKeyID id{kShortcutSignature, static_cast<UInt32>(registrationId)};
        const OSStatus status = RegisterEventHotKey(native.keyCode, native.modifiers, id,
                                                    GetApplicationEventTarget(), 0, &reference);
        if (status != noErr) {
            return {false,
                    status == eventHotKeyExistsErr ? GlobalShortcutFailureReason::AlreadyInUse
                                                   : GlobalShortcutFailureReason::SystemError,
                    static_cast<quint32>(status)};
        }
        m_keys.insert(registrationId, reference);
        return {true, GlobalShortcutFailureReason::None, 0};
    }

    void unregisterShortcut(int registrationId) override {
        const EventHotKeyRef key = m_keys.take(registrationId);
        if (key != nullptr) {
            UnregisterEventHotKey(key);
        }
    }

  private:
    static OSStatus handleEvent(EventHandlerCallRef, EventRef event, void* context) {
        EventHotKeyID id{};
        if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                              sizeof(id), nullptr, &id) != noErr ||
            id.signature != kShortcutSignature) {
            return eventNotHandledErr;
        }
        auto* backend = static_cast<MacOsGlobalShortcutBackend*>(context);
        const auto handler = backend->m_handler;
        if (handler && backend->m_keys.contains(static_cast<int>(id.id))) {
            handler(static_cast<int>(id.id));
        }
        return noErr;
    }

    ActivationHandler m_handler;
    QHash<int, EventHotKeyRef> m_keys;
    EventHandlerRef m_eventHandler = nullptr;
    OSStatus m_installStatus = noErr;
};
} // namespace

NativeShortcut nativeShortcut(const QString& portableShortcut) {
    const QKeySequence sequence =
        QKeySequence::fromString(portableShortcut, QKeySequence::PortableText);
    if (sequence.count() != 1) {
        return {};
    }
    const QKeyCombination combination = sequence[0];
    const auto code = keyCode(combination.key());
    if (!code.has_value()) {
        return {};
    }
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    if (modifiers.testFlag(Qt::KeypadModifier) || modifiers.testFlag(Qt::GroupSwitchModifier)) {
        return {};
    }
    UInt32 nativeModifiers = 0;
    const bool swap = !QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    if (modifiers.testFlag(Qt::ControlModifier))
        nativeModifiers |= swap ? cmdKey : controlKey;
    if (modifiers.testFlag(Qt::MetaModifier))
        nativeModifiers |= swap ? controlKey : cmdKey;
    if (modifiers.testFlag(Qt::AltModifier))
        nativeModifiers |= optionKey;
    if (modifiers.testFlag(Qt::ShiftModifier))
        nativeModifiers |= shiftKey;
    return {*code, nativeModifiers, true};
}

std::unique_ptr<presentation::GlobalShortcutBackend> createGlobalShortcutBackend() {
    return std::make_unique<MacOsGlobalShortcutBackend>();
}
} // namespace snow_shot::platform::macos
