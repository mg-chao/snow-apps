#include "../../presentation/services/globalshortcutbackend_p.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QKeySequence>
#include <QSet>

#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace snow_shot::presentation {
namespace {

bool modifierKey(Qt::Key key) {
    return key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Shift ||
           key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_Super_L ||
           key == Qt::Key_Super_R;
}

UINT virtualKeyForQtKey(Qt::Key key, bool keypad) {
    const int value = static_cast<int>(key);
    if (keypad) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            return static_cast<UINT>(VK_NUMPAD0 + value - static_cast<int>(Qt::Key_0));
        }
        switch (key) {
        case Qt::Key_Plus:
            return VK_ADD;
        case Qt::Key_Minus:
            return VK_SUBTRACT;
        case Qt::Key_Asterisk:
            return VK_MULTIPLY;
        case Qt::Key_Slash:
            return VK_DIVIDE;
        case Qt::Key_Period:
        case Qt::Key_Comma:
            return VK_DECIMAL;
        case Qt::Key_NumLock:
            return VK_NUMLOCK;
        default:
            return 0;
        }
    }
    if ((key >= Qt::Key_0 && key <= Qt::Key_9) || (key >= Qt::Key_A && key <= Qt::Key_Z)) {
        return static_cast<UINT>(value);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return static_cast<UINT>(VK_F1 + value - static_cast<int>(Qt::Key_F1));
    }
    switch (key) {
    case Qt::Key_Cancel:
        return VK_CANCEL;
    case Qt::Key_Backspace:
        return VK_BACK;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return VK_TAB;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return VK_RETURN;
    case Qt::Key_Escape:
        return VK_ESCAPE;
    case Qt::Key_Insert:
        return VK_INSERT;
    case Qt::Key_Delete:
        return VK_DELETE;
    case Qt::Key_Pause:
        return VK_PAUSE;
    case Qt::Key_Print:
    case Qt::Key_SysReq:
        return VK_SNAPSHOT;
    case Qt::Key_Printer:
        return VK_PRINT;
    case Qt::Key_Clear:
        return VK_CLEAR;
    case Qt::Key_Home:
        return VK_HOME;
    case Qt::Key_End:
        return VK_END;
    case Qt::Key_Left:
        return VK_LEFT;
    case Qt::Key_Up:
        return VK_UP;
    case Qt::Key_Right:
        return VK_RIGHT;
    case Qt::Key_Down:
        return VK_DOWN;
    case Qt::Key_PageUp:
        return VK_PRIOR;
    case Qt::Key_PageDown:
        return VK_NEXT;
    case Qt::Key_CapsLock:
        return VK_CAPITAL;
    case Qt::Key_NumLock:
        return VK_NUMLOCK;
    case Qt::Key_ScrollLock:
        return VK_SCROLL;
    case Qt::Key_Menu:
        return VK_APPS;
    case Qt::Key_Help:
        return VK_HELP;
    case Qt::Key_Select:
        return VK_SELECT;
    case Qt::Key_Execute:
        return VK_EXECUTE;
    case Qt::Key_Sleep:
    case Qt::Key_Standby:
        return VK_SLEEP;
    case Qt::Key_Space:
        return VK_SPACE;
    case Qt::Key_Comma:
        return VK_OEM_COMMA;
    case Qt::Key_Period:
        return VK_OEM_PERIOD;
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        return VK_OEM_MINUS;
    case Qt::Key_Equal:
    case Qt::Key_Plus:
        return VK_OEM_PLUS;
    case Qt::Key_Semicolon:
    case Qt::Key_Colon:
        return VK_OEM_1;
    case Qt::Key_Slash:
    case Qt::Key_Question:
        return VK_OEM_2;
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde:
        return VK_OEM_3;
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft:
        return VK_OEM_4;
    case Qt::Key_Backslash:
    case Qt::Key_Bar:
        return VK_OEM_5;
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight:
        return VK_OEM_6;
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl:
        return VK_OEM_7;
    case Qt::Key_VolumeMute:
        return VK_VOLUME_MUTE;
    case Qt::Key_VolumeDown:
        return VK_VOLUME_DOWN;
    case Qt::Key_VolumeUp:
        return VK_VOLUME_UP;
    case Qt::Key_Back:
        return VK_BROWSER_BACK;
    case Qt::Key_Forward:
        return VK_BROWSER_FORWARD;
    case Qt::Key_Refresh:
        return VK_BROWSER_REFRESH;
    case Qt::Key_Stop:
        return VK_BROWSER_STOP;
    case Qt::Key_Search:
        return VK_BROWSER_SEARCH;
    case Qt::Key_Favorites:
        return VK_BROWSER_FAVORITES;
    case Qt::Key_HomePage:
        return VK_BROWSER_HOME;
    case Qt::Key_MediaNext:
        return VK_MEDIA_NEXT_TRACK;
    case Qt::Key_MediaPrevious:
        return VK_MEDIA_PREV_TRACK;
    case Qt::Key_MediaStop:
        return VK_MEDIA_STOP;
    case Qt::Key_MediaPlay:
    case Qt::Key_MediaPause:
    case Qt::Key_MediaTogglePlayPause:
        return VK_MEDIA_PLAY_PAUSE;
    case Qt::Key_LaunchMail:
        return VK_LAUNCH_MAIL;
    case Qt::Key_LaunchMedia:
        return VK_LAUNCH_MEDIA_SELECT;
    case Qt::Key_Launch0:
        return VK_LAUNCH_APP1;
    case Qt::Key_Launch1:
        return VK_LAUNCH_APP2;
    case Qt::Key_Play:
        return VK_PLAY;
    case Qt::Key_Zoom:
        return VK_ZOOM;
    default:
        return 0;
    }
}

struct NativeShortcut {
    UINT modifiers = 0;
    UINT virtualKey = 0;
};

bool parseNativeShortcut(const shortcuts::ShortcutBinding& binding, NativeShortcut* output) {
    const QKeySequence sequence =
        QKeySequence::fromString(binding.portableText, QKeySequence::PortableText);
    if (output == nullptr || sequence.count() != 1 || modifierKey(sequence[0].key())) {
        return false;
    }
    const QKeyCombination combination = sequence[0];
    UINT modifiers = MOD_NOREPEAT;
    const Qt::KeyboardModifiers qtModifiers = combination.keyboardModifiers();
    if (qtModifiers.testFlag(Qt::ControlModifier)) {
        modifiers |= MOD_CONTROL;
    }
    if (qtModifiers.testFlag(Qt::AltModifier)) {
        modifiers |= MOD_ALT;
    }
    if (qtModifiers.testFlag(Qt::ShiftModifier)) {
        modifiers |= MOD_SHIFT;
    }
    if (qtModifiers.testFlag(Qt::MetaModifier)) {
        modifiers |= MOD_WIN;
    }
    const bool keypad = qtModifiers.testFlag(Qt::KeypadModifier);
    UINT virtualKey = virtualKeyForQtKey(combination.key(), keypad);
    if (virtualKey == 0 && !keypad) {
        const int keyValue = static_cast<int>(combination.key());
        if (keyValue >= 0x20 && keyValue <= 0xFFFF) {
            const SHORT scanResult =
                VkKeyScanExW(static_cast<WCHAR>(keyValue), GetKeyboardLayout(0));
            if (scanResult != -1) {
                virtualKey = static_cast<UINT>(LOBYTE(scanResult));
                const BYTE required = HIBYTE(scanResult);
                modifiers |= (required & 0x01U) != 0 ? MOD_SHIFT : 0;
                modifiers |= (required & 0x02U) != 0 ? MOD_CONTROL : 0;
                modifiers |= (required & 0x04U) != 0 ? MOD_ALT : 0;
            }
        }
    }
    if (virtualKey == 0) {
        return false;
    }
    *output = {modifiers, virtualKey};
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

class WindowsGlobalShortcutBackend final : public GlobalShortcutBackend,
                                           public QAbstractNativeEventFilter {
  public:
    WindowsGlobalShortcutBackend() {
        if (QCoreApplication::instance() != nullptr) {
            QCoreApplication::instance()->installNativeEventFilter(this);
            m_filterInstalled = true;
        }
    }
    ~WindowsGlobalShortcutBackend() override {
        for (int registrationId : std::as_const(m_registeredIds)) {
            UnregisterHotKey(nullptr, registrationId);
        }
        if (m_filterInstalled && QCoreApplication::instance() != nullptr) {
            QCoreApplication::instance()->removeNativeEventFilter(this);
        }
    }
    void setActivationHandler(ActivationHandler handler) override {
        m_handler = std::move(handler);
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
        SetLastError(ERROR_SUCCESS);
        if (RegisterHotKey(nullptr, registrationId, native.modifiers, native.virtualKey) != FALSE) {
            m_registeredIds.insert(registrationId);
            return {true, GlobalShortcutFailureReason::None, 0};
        }
        const DWORD error = GetLastError();
        return {false,
                error == ERROR_HOTKEY_ALREADY_REGISTERED ? GlobalShortcutFailureReason::AlreadyInUse
                                                         : GlobalShortcutFailureReason::SystemError,
                static_cast<qint64>(error)};
    }
    void unregisterShortcut(int registrationId) override {
        if (m_registeredIds.remove(registrationId)) {
            UnregisterHotKey(nullptr, registrationId);
        }
    }
    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
        if (message != nullptr) {
            const auto* native = static_cast<const MSG*>(message);
            if (native->message == WM_HOTKEY && m_handler) {
                m_handler(static_cast<int>(native->wParam));
            }
        }
        return false;
    }

  private:
    ActivationHandler m_handler;
    QSet<int> m_registeredIds;
    bool m_filterInstalled = false;
};

} // namespace

std::unique_ptr<GlobalShortcutBackend> createWindowsGlobalShortcutBackend() {
    return std::make_unique<WindowsGlobalShortcutBackend>();
}

} // namespace snow_shot::presentation
