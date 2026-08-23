#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/platform/windows/focusedfullscreenwindow.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QHash>
#include <QKeyCombination>
#include <QKeySequence>
#include <QSet>

#include <algorithm>
#include <array>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace snow_shot::presentation {
namespace {
constexpr int MAX_SHORTCUTS_PER_ACTION = 2;
constexpr int FIRST_REGISTRATION_ID = 0x2200;
constexpr int LAST_REGISTRATION_ID = 0xBFFF;
constexpr std::size_t ACTION_COUNT = 13;

constexpr std::array<GlobalShortcutAction, ACTION_COUNT> ALL_ACTIONS = {
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
    GlobalShortcutAction::OpenCaptureHistory,
    GlobalShortcutAction::OpenSettings,
    GlobalShortcutAction::PinClipboardContent,
};

int actionIndex(GlobalShortcutAction action) {
    switch (action) {
    case GlobalShortcutAction::Screenshot:
        return 0;
    case GlobalShortcutAction::ScreenshotDelay:
        return 1;
    case GlobalShortcutAction::ScreenshotFixed:
        return 2;
    case GlobalShortcutAction::ScreenshotOcr:
        return 3;
    case GlobalShortcutAction::ScreenshotTranslation:
        return 4;
    case GlobalShortcutAction::ScreenshotCopy:
        return 5;
    case GlobalShortcutAction::ScreenshotFullScreen:
        return 6;
    case GlobalShortcutAction::ScreenshotFocusedWindow:
        return 7;
    case GlobalShortcutAction::ScreenRecord:
        return 8;
    case GlobalShortcutAction::ScreenRecordCopy:
        return 9;
    case GlobalShortcutAction::OpenCaptureHistory:
        return 10;
    case GlobalShortcutAction::OpenSettings:
        return 11;
    case GlobalShortcutAction::PinClipboardContent:
        return 12;
    }
    return 0;
}

bool isModifierKey(Qt::Key key) {
    return key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Shift ||
           key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_Super_L ||
           key == Qt::Key_Super_R;
}

QString canonicalShortcut(const QString& shortcut) {
    QString trimmed = shortcut.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    QKeySequence sequence = QKeySequence::fromString(trimmed, QKeySequence::PortableText);
    if (sequence.isEmpty()) {
        sequence = QKeySequence::fromString(trimmed, QKeySequence::NativeText);
    }
    if (sequence.count() != 1) {
        return trimmed;
    }

    const QKeyCombination combination = sequence[0];
    if (combination.key() == Qt::Key_unknown || isModifierKey(combination.key())) {
        return trimmed;
    }

    const QString portable = sequence.toString(QKeySequence::PortableText).trimmed();
    return portable.isEmpty() ? trimmed : portable;
}

QStringList canonicalShortcuts(const QStringList& shortcuts) {
    QStringList result;
    for (const QString& shortcut : shortcuts) {
        const QString canonical = canonicalShortcut(shortcut);
        if (canonical.isEmpty() || result.contains(canonical)) {
            continue;
        }
        result.push_back(canonical);
        if (result.size() >= MAX_SHORTCUTS_PER_ACTION) {
            break;
        }
    }
    return result;
}

bool bindingResultsEqual(const GlobalShortcutBindingResult& lhs,
                         const GlobalShortcutBindingResult& rhs) {
    return lhs.shortcut == rhs.shortcut && lhs.registered == rhs.registered &&
           lhs.failureReason == rhs.failureReason && lhs.nativeErrorCode == rhs.nativeErrorCode;
}

bool statesEqual(const GlobalShortcutRegistrationState& lhs,
                 const GlobalShortcutRegistrationState& rhs) {
    if (lhs.action != rhs.action || lhs.shortcuts != rhs.shortcuts || lhs.status != rhs.status ||
        lhs.bindings.size() != rhs.bindings.size()) {
        return false;
    }
    for (int index = 0; index < lhs.bindings.size(); ++index) {
        if (!bindingResultsEqual(lhs.bindings[index], rhs.bindings[index])) {
            return false;
        }
    }
    return true;
}

GlobalShortcutStatus aggregateStatus(const QVector<GlobalShortcutBindingResult>& bindings) {
    if (bindings.isEmpty()) {
        return GlobalShortcutStatus::Unset;
    }

    const int registeredCount = static_cast<int>(
        std::count_if(bindings.cbegin(), bindings.cend(),
                      [](const GlobalShortcutBindingResult& result) { return result.registered; }));
    if (registeredCount == bindings.size()) {
        return GlobalShortcutStatus::Registered;
    }
    if (registeredCount > 0) {
        return GlobalShortcutStatus::PartiallyRegistered;
    }
    return GlobalShortcutStatus::Failed;
}

QString ownerKey(GlobalShortcutAction action, const QString& shortcut) {
    return QString::number(actionIndex(action)) + QChar(0x1F) + shortcut;
}

class UnsupportedGlobalShortcutBackend final : public GlobalShortcutBackend {
  public:
    void setActivationHandler(ActivationHandler handler) override {
        m_handler = std::move(handler);
    }

    GlobalShortcutValidationResult
    validateShortcut(const QString& portableShortcut) const override {
        return {
            portableShortcut,
            false,
            GlobalShortcutFailureReason::UnsupportedPlatform,
        };
    }

    GlobalShortcutBackendResult registerShortcut(int, const QString&) override {
        return {
            false,
            GlobalShortcutFailureReason::UnsupportedPlatform,
            0,
        };
    }

    void unregisterShortcut(int) override {}

  private:
    ActivationHandler m_handler;
};

#ifdef Q_OS_WIN
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
        // Windows reserves F12 for debuggers, so RegisterHotKey cannot own it.
        if (key == Qt::Key_F12) {
            return 0;
        }
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
        return VK_SNAPSHOT;
    case Qt::Key_Printer:
        return VK_PRINT;
    case Qt::Key_SysReq:
        return VK_SNAPSHOT;
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

bool parseNativeShortcut(const QString& shortcut, NativeShortcut* output) {
    if (output == nullptr) {
        return false;
    }

    const QKeySequence sequence = QKeySequence::fromString(shortcut, QKeySequence::PortableText);
    if (sequence.count() != 1) {
        return false;
    }

    const QKeyCombination combination = sequence[0];
    const Qt::Key key = combination.key();
    if (key == Qt::Key_unknown || isModifierKey(key)) {
        return false;
    }

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
    UINT virtualKey = virtualKeyForQtKey(key, keypad);
    if (virtualKey == 0 && !keypad) {
        const int keyValue = static_cast<int>(key);
        if (keyValue >= 0x20 && keyValue <= 0xFFFF) {
            const SHORT scanResult =
                VkKeyScanExW(static_cast<WCHAR>(keyValue), GetKeyboardLayout(0));
            if (scanResult != -1) {
                virtualKey = static_cast<UINT>(LOBYTE(scanResult));
                const BYTE requiredModifiers = HIBYTE(scanResult);
                if ((requiredModifiers & 0x01U) != 0) {
                    modifiers |= MOD_SHIFT;
                }
                if ((requiredModifiers & 0x02U) != 0) {
                    modifiers |= MOD_CONTROL;
                }
                if ((requiredModifiers & 0x04U) != 0) {
                    modifiers |= MOD_ALT;
                }
            }
        }
    }
    if (virtualKey == 0) {
        return false;
    }

    output->modifiers = modifiers;
    output->virtualKey = virtualKey;
    return true;
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
    validateShortcut(const QString& portableShortcut) const override {
        NativeShortcut nativeShortcut;
        const bool supported = parseNativeShortcut(portableShortcut, &nativeShortcut);
        return {
            portableShortcut,
            supported,
            supported ? GlobalShortcutFailureReason::None
                      : GlobalShortcutFailureReason::InvalidShortcut,
        };
    }

    GlobalShortcutBackendResult registerShortcut(int registrationId,
                                                 const QString& portableShortcut) override {
        NativeShortcut nativeShortcut;
        if (!parseNativeShortcut(portableShortcut, &nativeShortcut)) {
            return {
                false,
                GlobalShortcutFailureReason::InvalidShortcut,
                0,
            };
        }

        SetLastError(ERROR_SUCCESS);
        if (RegisterHotKey(nullptr, registrationId, nativeShortcut.modifiers,
                           nativeShortcut.virtualKey) != FALSE) {
            m_registeredIds.insert(registrationId);
            return {
                true,
                GlobalShortcutFailureReason::None,
                0,
            };
        }

        const DWORD error = GetLastError();
        return {
            false,
            error == ERROR_HOTKEY_ALREADY_REGISTERED ? GlobalShortcutFailureReason::AlreadyInUse
                                                     : GlobalShortcutFailureReason::SystemError,
            static_cast<quint32>(error),
        };
    }

    void unregisterShortcut(int registrationId) override {
        if (!m_registeredIds.remove(registrationId)) {
            return;
        }
        UnregisterHotKey(nullptr, registrationId);
    }

    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
        if (message == nullptr) {
            return false;
        }

        const auto* nativeMessage = static_cast<const MSG*>(message);
        if (nativeMessage->message == WM_HOTKEY && m_handler) {
            m_handler(static_cast<int>(nativeMessage->wParam));
        }
        return false;
    }

  private:
    ActivationHandler m_handler;
    QSet<int> m_registeredIds;
    bool m_filterInstalled = false;
};
#endif

std::unique_ptr<GlobalShortcutBackend> createPlatformBackend() {
#ifdef Q_OS_WIN
    return std::make_unique<WindowsGlobalShortcutBackend>();
#else
    return std::make_unique<UnsupportedGlobalShortcutBackend>();
#endif
}

QStringList persistedShortcuts(const snow_shot::storage::ShortcutSettings& settings,
                               GlobalShortcutAction action) {
    switch (action) {
    case GlobalShortcutAction::Screenshot:
        return settings.screenshot();
    case GlobalShortcutAction::ScreenshotDelay:
        return settings.screenshotDelay();
    case GlobalShortcutAction::ScreenshotFixed:
        return settings.screenshotFixed();
    case GlobalShortcutAction::ScreenshotOcr:
        return settings.screenshotOcr();
    case GlobalShortcutAction::ScreenshotTranslation:
        return settings.screenshotTranslation();
    case GlobalShortcutAction::ScreenshotCopy:
        return settings.screenshotCopy();
    case GlobalShortcutAction::ScreenshotFullScreen:
        return settings.screenshotFullScreen();
    case GlobalShortcutAction::ScreenshotFocusedWindow:
        return settings.screenshotFocusedWindow();
    case GlobalShortcutAction::ScreenRecord:
        return settings.screenRecord();
    case GlobalShortcutAction::ScreenRecordCopy:
        return settings.screenRecordCopy();
    case GlobalShortcutAction::OpenCaptureHistory:
        return settings.openCaptureHistory();
    case GlobalShortcutAction::OpenSettings:
        return settings.openSettings();
    case GlobalShortcutAction::PinClipboardContent:
        return settings.pinClipboardContent();
    }
    return {};
}

bool persistShortcuts(const snow_shot::storage::ShortcutSettings& settings,
                      GlobalShortcutAction action, const QStringList& shortcuts) {
    switch (action) {
    case GlobalShortcutAction::Screenshot:
        return settings.setScreenshot(shortcuts);
    case GlobalShortcutAction::ScreenshotDelay:
        return settings.setScreenshotDelay(shortcuts);
    case GlobalShortcutAction::ScreenshotFixed:
        return settings.setScreenshotFixed(shortcuts);
    case GlobalShortcutAction::ScreenshotOcr:
        return settings.setScreenshotOcr(shortcuts);
    case GlobalShortcutAction::ScreenshotTranslation:
        return settings.setScreenshotTranslation(shortcuts);
    case GlobalShortcutAction::ScreenshotCopy:
        return settings.setScreenshotCopy(shortcuts);
    case GlobalShortcutAction::ScreenshotFullScreen:
        return settings.setScreenshotFullScreen(shortcuts);
    case GlobalShortcutAction::ScreenshotFocusedWindow:
        return settings.setScreenshotFocusedWindow(shortcuts);
    case GlobalShortcutAction::ScreenRecord:
        return settings.setScreenRecord(shortcuts);
    case GlobalShortcutAction::ScreenRecordCopy:
        return settings.setScreenRecordCopy(shortcuts);
    case GlobalShortcutAction::OpenCaptureHistory:
        return settings.setOpenCaptureHistory(shortcuts);
    case GlobalShortcutAction::OpenSettings:
        return settings.setOpenSettings(shortcuts);
    case GlobalShortcutAction::PinClipboardContent:
        return settings.setPinClipboardContent(shortcuts);
    }
    return false;
}
} // namespace

class GlobalShortcutManager::Impl {
  public:
    struct ActiveRegistration {
        GlobalShortcutAction action = GlobalShortcutAction::Screenshot;
        QString shortcut;
        int registrationId = 0;
    };

    Impl(GlobalShortcutManager& owner, std::unique_ptr<GlobalShortcutBackend> backend,
         const QString&, const QString&, std::function<bool()> focusedFullscreenDetector)
        : q(owner), m_backend(backend != nullptr ? std::move(backend) : createPlatformBackend()),
          m_focusedFullscreenDetector(
              focusedFullscreenDetector
                  ? std::move(focusedFullscreenDetector)
                  : std::function<bool()>(
                        &snow_shot::platform::windows::focusedFullscreenWindowExists)) {
        for (GlobalShortcutAction action : ALL_ACTIONS) {
            GlobalShortcutRegistrationState initialState;
            initialState.action = action;
            m_states[actionIndex(action)] = initialState;
        }

        m_backend->setActivationHandler([this](int registrationId) {
            const QString activeKey = m_registrationKeysById.value(registrationId);
            const auto active = m_activeRegistrations.constFind(activeKey);
            if (active != m_activeRegistrations.cend()) {
                if (!m_shortcutFunctionsEnabled) {
                    return;
                }
                const bool suppressionEnabled =
                    snow_shot::storage::GlobalShortcutSettings()
                        .disableOnFocusedFullscreenWindow();
                if (suppressionEnabled && m_focusedFullscreenDetector &&
                    m_focusedFullscreenDetector()) {
                    return;
                }
                emit q.activated(active->action);
            }
        });
    }

    ~Impl() {
        for (const ActiveRegistration& registration : std::as_const(m_activeRegistrations)) {
            m_backend->unregisterShortcut(registration.registrationId);
        }
        m_activeRegistrations.clear();
        m_registrationKeysById.clear();
        m_backend->setActivationHandler({});
    }

    void initialize() {
        if (m_initialized) {
            return;
        }

        const snow_shot::storage::ShortcutSettings settings;
        for (GlobalShortcutAction action : ALL_ACTIONS) {
            m_shortcuts[actionIndex(action)] =
                canonicalShortcuts(persistedShortcuts(settings, action));
        }
        m_initialized = true;
        reconcile();
    }

    void setShortcuts(GlobalShortcutAction action, const QStringList& shortcuts) {
        if (!m_initialized) {
            initialize();
        }

        m_shortcuts[actionIndex(action)] = canonicalShortcuts(shortcuts);
        const snow_shot::storage::ShortcutSettings settings;
        static_cast<void>(persistShortcuts(settings, action, m_shortcuts[actionIndex(action)]));
        reconcile();
    }

    GlobalShortcutRegistrationState state(GlobalShortcutAction action) const {
        return m_states[actionIndex(action)];
    }

    GlobalShortcutValidationResult validateShortcut(const QString& shortcut) const {
        const QString canonical = canonicalShortcut(shortcut);
        if (canonical.isEmpty()) {
            return {
                {},
                false,
                GlobalShortcutFailureReason::InvalidShortcut,
            };
        }

        GlobalShortcutValidationResult result = m_backend->validateShortcut(canonical);
        result.shortcut = canonical;
        return result;
    }

    int allocateRegistrationId() {
        const int capacity = LAST_REGISTRATION_ID - FIRST_REGISTRATION_ID + 1;
        for (int attempt = 0; attempt < capacity; ++attempt) {
            const int candidate = m_nextRegistrationId;
            ++m_nextRegistrationId;
            if (m_nextRegistrationId > LAST_REGISTRATION_ID) {
                m_nextRegistrationId = FIRST_REGISTRATION_ID;
            }
            if (!m_registrationKeysById.contains(candidate)) {
                return candidate;
            }
        }
        return 0;
    }

    void reconcile() {
        QSet<QString> desiredOwnerKeys;
        for (GlobalShortcutAction action : ALL_ACTIONS) {
            for (const QString& shortcut : m_shortcuts[actionIndex(action)]) {
                desiredOwnerKeys.insert(ownerKey(action, shortcut));
            }
        }

        const QList<QString> activeOwnerKeys = m_activeRegistrations.keys();
        for (const QString& activeOwnerKey : activeOwnerKeys) {
            if (desiredOwnerKeys.contains(activeOwnerKey)) {
                continue;
            }
            const ActiveRegistration registration = m_activeRegistrations.take(activeOwnerKey);
            m_registrationKeysById.remove(registration.registrationId);
            m_backend->unregisterShortcut(registration.registrationId);
        }

        std::array<GlobalShortcutRegistrationState, ACTION_COUNT> nextStates;
        for (GlobalShortcutAction action : ALL_ACTIONS) {
            GlobalShortcutRegistrationState nextState;
            nextState.action = action;
            nextState.shortcuts = m_shortcuts[actionIndex(action)];

            for (const QString& shortcut : nextState.shortcuts) {
                GlobalShortcutBindingResult binding;
                binding.shortcut = shortcut;

                const QString activeOwnerKey = ownerKey(action, shortcut);
                if (m_activeRegistrations.contains(activeOwnerKey)) {
                    binding.registered = true;
                    nextState.bindings.push_back(binding);
                    continue;
                }

                const int registrationId = allocateRegistrationId();
                if (registrationId == 0) {
                    binding.failureReason = GlobalShortcutFailureReason::SystemError;
                    nextState.bindings.push_back(binding);
                    continue;
                }

                const GlobalShortcutBackendResult result =
                    m_backend->registerShortcut(registrationId, shortcut);
                binding.registered = result.registered;
                binding.failureReason = result.failureReason;
                binding.nativeErrorCode = result.nativeErrorCode;
                nextState.bindings.push_back(binding);

                if (result.registered) {
                    const ActiveRegistration registration{
                        action,
                        shortcut,
                        registrationId,
                    };
                    m_activeRegistrations.insert(activeOwnerKey, registration);
                    m_registrationKeysById.insert(registrationId, activeOwnerKey);
                }
            }

            nextState.status = aggregateStatus(nextState.bindings);
            nextStates[actionIndex(action)] = nextState;
        }

        for (GlobalShortcutAction action : ALL_ACTIONS) {
            const int index = actionIndex(action);
            const bool changed = !statesEqual(m_states[index], nextStates[index]);
            m_states[index] = nextStates[index];
            if (changed) {
                emit q.stateChanged(action, m_states[index]);
            }
        }
    }

    GlobalShortcutManager& q;
    std::unique_ptr<GlobalShortcutBackend> m_backend;
    std::function<bool()> m_focusedFullscreenDetector;
    std::array<QStringList, ACTION_COUNT> m_shortcuts;
    std::array<GlobalShortcutRegistrationState, ACTION_COUNT> m_states;
    QHash<QString, ActiveRegistration> m_activeRegistrations;
    QHash<int, QString> m_registrationKeysById;
    int m_nextRegistrationId = FIRST_REGISTRATION_ID;
    bool m_initialized = false;
    bool m_shortcutFunctionsEnabled = true;
};

GlobalShortcutManager::GlobalShortcutManager(QObject* parent)
    : GlobalShortcutManager(nullptr, {}, {}, parent, {}) {}

GlobalShortcutManager::GlobalShortcutManager(std::unique_ptr<GlobalShortcutBackend> backend,
                                             const QString& organization,
                                             const QString& application, QObject* parent,
                                             std::function<bool()> focusedFullscreenDetector)
    : QObject(parent),
      m_impl(std::make_unique<Impl>(*this, std::move(backend), organization, application,
                                    std::move(focusedFullscreenDetector))) {}

GlobalShortcutManager::~GlobalShortcutManager() = default;

void GlobalShortcutManager::initialize() {
    m_impl->initialize();
}

bool GlobalShortcutManager::isInitialized() const {
    return m_impl->m_initialized;
}

GlobalShortcutRegistrationState GlobalShortcutManager::state(GlobalShortcutAction action) const {
    return m_impl->state(action);
}

GlobalShortcutValidationResult
GlobalShortcutManager::validateShortcut(const QString& shortcut) const {
    return m_impl->validateShortcut(shortcut);
}

void GlobalShortcutManager::setShortcuts(GlobalShortcutAction action,
                                          const QStringList& shortcuts) {
    m_impl->setShortcuts(action, shortcuts);
}

void GlobalShortcutManager::setShortcutFunctionsEnabled(bool enabled) {
    m_impl->m_shortcutFunctionsEnabled = enabled;
}

bool GlobalShortcutManager::shortcutFunctionsEnabled() const {
    return m_impl->m_shortcutFunctionsEnabled;
}

void GlobalShortcutManager::retryRegistrations() {
    if (!m_impl->m_initialized) {
        m_impl->initialize();
        return;
    }
    m_impl->reconcile();
}
} // namespace snow_shot::presentation
