#include "snow_shot/presentation/globalshortcutmanager.h"

#include "globalshortcutbackend_p.h"

#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/platform/focusedfullscreenwindow.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace snow_shot::presentation {
namespace {

constexpr int MAX_SHORTCUTS_PER_ACTION = 2;
constexpr int FIRST_REGISTRATION_ID = 0x2200;
constexpr int LAST_REGISTRATION_ID = 0xBFFF;
constexpr std::size_t ACTION_COUNT = 29;

constexpr std::array<GlobalShortcutAction, ACTION_COUNT> ALL_ACTIONS = {
    GlobalShortcutAction::Screenshot,
    GlobalShortcutAction::ScreenshotDelay,
    GlobalShortcutAction::ScreenshotFixed,
    GlobalShortcutAction::ScreenshotOcr,
    GlobalShortcutAction::ScreenshotOcrCopy,
    GlobalShortcutAction::ScreenshotTranslation,
    GlobalShortcutAction::ScreenshotCopy,
    GlobalShortcutAction::ScreenshotSave,
    GlobalShortcutAction::ScreenshotQuickSave,
    GlobalShortcutAction::ScreenshotFullScreen,
    GlobalShortcutAction::ScreenshotFocusedWindow,
    GlobalShortcutAction::ScreenRecord,
    GlobalShortcutAction::ScreenRecordCopy,
    GlobalShortcutAction::OpenScreenRecordingFolder,
    GlobalShortcutAction::OpenCaptureHistory,
    GlobalShortcutAction::OpenPinToScreenManagement,
    GlobalShortcutAction::GlobalCanvas,
    GlobalShortcutAction::StitchSelectedImages,
    GlobalShortcutAction::SplitSelectedImages,
    GlobalShortcutAction::SwitchWindowGroup,
    GlobalShortcutAction::OpenSettings,
    GlobalShortcutAction::PinClipboardContent,
    GlobalShortcutAction::TranslateSelectedText,
    GlobalShortcutAction::ToggleWindowUnderCursorAlwaysOnTop,
    GlobalShortcutAction::PinSelectedFiles,
    GlobalShortcutAction::RestoreLastClosedWindows,
    GlobalShortcutAction::HideShowAllWindows,
    GlobalShortcutAction::ToggleGlobalHotkeys,
    GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow,
};

std::size_t actionIndex(GlobalShortcutAction action) {
    const auto found = std::find(ALL_ACTIONS.cbegin(), ALL_ACTIONS.cend(), action);
    return found == ALL_ACTIONS.cend()
               ? 0
               : static_cast<std::size_t>(std::distance(ALL_ACTIONS.cbegin(), found));
}

shortcuts::ShortcutBindingList canonicalBindings(const shortcuts::ShortcutBindingList& bindings) {
    shortcuts::ShortcutBindingList result;
    for (const auto& binding : bindings) {
        const auto canonical = shortcuts::canonicalBinding(binding);
        const bool duplicate =
            std::any_of(result.cbegin(), result.cend(), [&canonical](const auto& existing) {
                return shortcuts::bindingsConflict(existing, canonical);
            });
        if (canonical.portableText.isEmpty() || duplicate) {
            continue;
        }
        result.push_back(canonical);
        if (result.size() >= MAX_SHORTCUTS_PER_ACTION) {
            break;
        }
    }
    return result;
}

QString runtimeIdentityKey(const shortcuts::ShortcutBinding& binding) {
    const shortcuts::ShortcutIdentity identity = shortcuts::effectiveIdentity(binding);
    const QString key = identity.physicalKey.has_value()
                            ? QStringLiteral("p:%1").arg(*identity.physicalKey)
                            : QStringLiteral("q:%1").arg(static_cast<int>(identity.key));
    return key + QStringLiteral(":m:%1").arg(static_cast<int>(identity.modifiers));
}

QString ownerKey(GlobalShortcutAction action, const shortcuts::ShortcutBinding& binding) {
    const QByteArray serialized =
        QJsonDocument(shortcuts::shortcutBindingToJson(binding)).toJson(QJsonDocument::Compact);
    return QString::number(actionIndex(action)) + QChar(0x1F) + QString::fromUtf8(serialized);
}

bool bindingResultsEqual(const GlobalShortcutBindingResult& first,
                         const GlobalShortcutBindingResult& second) {
    return first.binding == second.binding && first.shortcut == second.shortcut &&
           first.registered == second.registered && first.failureReason == second.failureReason &&
           first.nativeErrorCode == second.nativeErrorCode;
}

bool statesEqual(const GlobalShortcutRegistrationState& first,
                 const GlobalShortcutRegistrationState& second) {
    if (first.action != second.action || first.shortcuts != second.shortcuts ||
        first.status != second.status || first.bindings.size() != second.bindings.size()) {
        return false;
    }
    for (int index = 0; index < first.bindings.size(); ++index) {
        if (!bindingResultsEqual(first.bindings[index], second.bindings[index])) {
            return false;
        }
    }
    return true;
}

GlobalShortcutStatus aggregateStatus(const QVector<GlobalShortcutBindingResult>& bindings) {
    if (bindings.isEmpty()) {
        return GlobalShortcutStatus::Unset;
    }
    const int registered = static_cast<int>(std::count_if(
        bindings.cbegin(), bindings.cend(), [](const auto& item) { return item.registered; }));
    if (registered == bindings.size()) {
        return GlobalShortcutStatus::Registered;
    }
    return registered > 0 ? GlobalShortcutStatus::PartiallyRegistered
                          : GlobalShortcutStatus::Failed;
}

shortcuts::ShortcutBindingList persistedShortcuts(const storage::ShortcutSettings& settings,
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
    case GlobalShortcutAction::ScreenshotOcrCopy:
        return settings.screenshotOcrCopy();
    case GlobalShortcutAction::ScreenshotTranslation:
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        return settings.screenshotTranslation();
#else
        return {};
#endif
    case GlobalShortcutAction::ScreenshotCopy:
        return settings.screenshotCopy();
    case GlobalShortcutAction::ScreenshotSave:
        return settings.screenshotSave();
    case GlobalShortcutAction::ScreenshotQuickSave:
        return settings.screenshotQuickSave();
    case GlobalShortcutAction::ScreenshotFullScreen:
        return settings.screenshotFullScreen();
    case GlobalShortcutAction::ScreenshotFocusedWindow:
        return settings.screenshotFocusedWindow();
    case GlobalShortcutAction::ScreenRecord:
        return settings.screenRecord();
    case GlobalShortcutAction::ScreenRecordCopy:
        return settings.screenRecordCopy();
    case GlobalShortcutAction::OpenScreenRecordingFolder:
        return settings.openScreenRecordingFolder();
    case GlobalShortcutAction::OpenCaptureHistory:
        return settings.openCaptureHistory();
    case GlobalShortcutAction::SwitchWindowGroup:
        return settings.switchWindowGroup();
    case GlobalShortcutAction::GlobalCanvas:
        return settings.globalCanvas();
    case GlobalShortcutAction::StitchSelectedImages:
        return settings.stitchSelectedImages();
    case GlobalShortcutAction::SplitSelectedImages:
        return settings.splitSelectedImages();
    case GlobalShortcutAction::OpenPinToScreenManagement:
        return settings.openPinToScreenManagement();
    case GlobalShortcutAction::OpenSettings:
        return settings.openSettings();
    case GlobalShortcutAction::PinClipboardContent:
        return settings.pinClipboardContent();
    case GlobalShortcutAction::PinSelectedFiles:
        return settings.pinSelectedFiles();
    case GlobalShortcutAction::RestoreLastClosedWindows:
        return settings.restoreLastClosedWindows();
    case GlobalShortcutAction::HideShowAllWindows:
        return settings.hideShowAllWindows();
    case GlobalShortcutAction::TranslateSelectedText:
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        return settings.translateSelectedText();
#else
        return {};
#endif
    case GlobalShortcutAction::ToggleWindowUnderCursorAlwaysOnTop:
        return settings.toggleWindowUnderCursorAlwaysOnTop();
    case GlobalShortcutAction::ToggleGlobalHotkeys:
        return settings.toggleGlobalHotkeys();
    case GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow:
        return settings.toggleDisableOnFocusedFullscreenWindow();
    }
    return {};
}

bool persistShortcuts(const storage::ShortcutSettings& settings, GlobalShortcutAction action,
                      const shortcuts::ShortcutBindingList& bindings) {
    switch (action) {
    case GlobalShortcutAction::Screenshot:
        return settings.setScreenshot(bindings);
    case GlobalShortcutAction::ScreenshotDelay:
        return settings.setScreenshotDelay(bindings);
    case GlobalShortcutAction::ScreenshotFixed:
        return settings.setScreenshotFixed(bindings);
    case GlobalShortcutAction::ScreenshotOcr:
        return settings.setScreenshotOcr(bindings);
    case GlobalShortcutAction::ScreenshotOcrCopy:
        return settings.setScreenshotOcrCopy(bindings);
    case GlobalShortcutAction::ScreenshotTranslation:
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        return settings.setScreenshotTranslation(bindings);
#else
        return false;
#endif
    case GlobalShortcutAction::ScreenshotCopy:
        return settings.setScreenshotCopy(bindings);
    case GlobalShortcutAction::ScreenshotSave:
        return settings.setScreenshotSave(bindings);
    case GlobalShortcutAction::ScreenshotQuickSave:
        return settings.setScreenshotQuickSave(bindings);
    case GlobalShortcutAction::ScreenshotFullScreen:
        return settings.setScreenshotFullScreen(bindings);
    case GlobalShortcutAction::ScreenshotFocusedWindow:
        return settings.setScreenshotFocusedWindow(bindings);
    case GlobalShortcutAction::ScreenRecord:
        return settings.setScreenRecord(bindings);
    case GlobalShortcutAction::ScreenRecordCopy:
        return settings.setScreenRecordCopy(bindings);
    case GlobalShortcutAction::OpenScreenRecordingFolder:
        return settings.setOpenScreenRecordingFolder(bindings);
    case GlobalShortcutAction::OpenCaptureHistory:
        return settings.setOpenCaptureHistory(bindings);
    case GlobalShortcutAction::SwitchWindowGroup:
        return settings.setSwitchWindowGroup(bindings);
    case GlobalShortcutAction::GlobalCanvas:
        return settings.setGlobalCanvas(bindings);
    case GlobalShortcutAction::StitchSelectedImages:
        return settings.setStitchSelectedImages(bindings);
    case GlobalShortcutAction::SplitSelectedImages:
        return settings.setSplitSelectedImages(bindings);
    case GlobalShortcutAction::OpenPinToScreenManagement:
        return settings.setOpenPinToScreenManagement(bindings);
    case GlobalShortcutAction::OpenSettings:
        return settings.setOpenSettings(bindings);
    case GlobalShortcutAction::PinClipboardContent:
        return settings.setPinClipboardContent(bindings);
    case GlobalShortcutAction::PinSelectedFiles:
        return settings.setPinSelectedFiles(bindings);
    case GlobalShortcutAction::RestoreLastClosedWindows:
        return settings.setRestoreLastClosedWindows(bindings);
    case GlobalShortcutAction::HideShowAllWindows:
        return settings.setHideShowAllWindows(bindings);
    case GlobalShortcutAction::TranslateSelectedText:
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        return settings.setTranslateSelectedText(bindings);
#else
        return false;
#endif
    case GlobalShortcutAction::ToggleWindowUnderCursorAlwaysOnTop:
        return settings.setToggleWindowUnderCursorAlwaysOnTop(bindings);
    case GlobalShortcutAction::ToggleGlobalHotkeys:
        return settings.setToggleGlobalHotkeys(bindings);
    case GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow:
        return settings.setToggleDisableOnFocusedFullscreenWindow(bindings);
    }
    return false;
}

GlobalShortcutValidationResult invalidValidation(const shortcuts::ShortcutBinding& binding,
                                                 GlobalShortcutFailureReason reason) {
    GlobalShortcutValidationResult result;
    result.binding = binding;
    result.shortcut = binding.portableText;
    result.supported = false;
    result.failureReason = reason;
    return result;
}

} // namespace

class GlobalShortcutManager::Impl {
  public:
    struct ActiveRegistration {
        GlobalShortcutAction action = GlobalShortcutAction::Screenshot;
        shortcuts::ShortcutBinding binding;
        int registrationId = 0;
    };

    Impl(GlobalShortcutManager& owner, std::unique_ptr<GlobalShortcutBackend> backend,
         std::function<bool()> fullscreenDetector)
        : q(owner), m_backend(backend != nullptr ? std::move(backend)
                                                 : createPlatformGlobalShortcutBackend()),
          m_focusedFullscreenDetector(
              fullscreenDetector
                  ? std::move(fullscreenDetector)
                  : std::function<bool()>(&platform::focusedFullscreenWindowExists)) {
        for (GlobalShortcutAction action : ALL_ACTIONS) {
            m_states[actionIndex(action)].action = action;
        }
        m_fullscreenTimer.setInterval(1000);
        m_fullscreenTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_fullscreenTimer, &QTimer::timeout, &q,
                         &GlobalShortcutManager::refreshFocusedFullscreenState);
        m_backend->setActivationHandler([this](int registrationId) {
            // Focus can change between timer ticks. Refresh before looking up
            // the ID because reconciliation can release this registration.
            refreshFocusedFullscreenState();
            const QString activeKey = m_registrationKeysById.value(registrationId);
            const auto active = m_activeRegistrations.constFind(activeKey);
            if (active == m_activeRegistrations.cend() || !registrationEligible(active->action)) {
                return;
            }
            const auto action = active->action;
            emit q.bindingActivated(action, registrationId);
            emit q.activated(action);
        });
        m_backend->setAvailabilityChangedHandler([this](const QList<int>& invalidatedIds) {
            for (int id : invalidatedIds) {
                const auto entry = m_registrationKeysById.find(id);
                if (entry == m_registrationKeysById.end()) {
                    continue;
                }
                m_activeRegistrations.remove(*entry);
                m_registrationKeysById.erase(entry);
                m_backend->unregisterShortcut(id);
            }
            reconcile();
        });
    }

    ~Impl() {
        m_fullscreenTimer.stop();
        m_backend->setAvailabilityChangedHandler({});
        unregisterAll();
        m_backend->setActivationHandler({});
    }

    void initialize() {
        if (m_initialized) {
            return;
        }
        const storage::ShortcutSettings settings;
        for (GlobalShortcutAction action : ALL_ACTIONS) {
            m_shortcuts[actionIndex(action)] =
                canonicalBindings(persistedShortcuts(settings, action));
        }
        QObject::connect(
            &storage::ApplicationStorage::instance().configuration(),
            &storage::ConfigurationStore::valueChanged, &q,
            [this](const QString& key, const QJsonValue&) {
                if (m_initialized &&
                    (key == QStringLiteral("extended_features/translation_page_enabled") ||
                     key ==
                         QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window"))) {
                    reconcile();
                }
            });
        m_initialized = true;
        reconcile();
    }

    bool setShortcuts(GlobalShortcutAction action, const shortcuts::ShortcutBindingList& bindings) {
        if (!m_initialized) {
            initialize();
        }
        const auto canonical = canonicalBindings(bindings);
        if (!persistShortcuts(storage::ShortcutSettings(), action, canonical)) {
            return false;
        }
        m_shortcuts[actionIndex(action)] = canonical;
        reconcile();
        return true;
    }

    GlobalShortcutValidationResult
    validateShortcut(std::optional<GlobalShortcutAction> owner,
                     const shortcuts::ShortcutBinding& requested) const {
        const auto binding = shortcuts::canonicalBinding(requested);
        if (binding.portableText.isEmpty()) {
            return invalidValidation(binding, GlobalShortcutFailureReason::InvalidShortcut);
        }
        GlobalShortcutValidationResult validation = m_backend->validateShortcut(binding);
        validation.binding = binding;
        validation.shortcut = binding.portableText;
        if (!validation.supported) {
            return validation;
        }
        if (owner.has_value()) {
            for (GlobalShortcutAction action : ALL_ACTIONS) {
                if (action == *owner) {
                    continue;
                }
                // Runtime ownership is per binding, not per action. A
                // partially registered action can contain both a live binding
                // and a failed one; only the live binding may veto another
                // action. Registration states remain intact while recorders
                // temporarily suspend the native backend, so this also keeps
                // validation stable throughout editing.
                const auto& state = m_states[actionIndex(action)];
                if (std::any_of(state.bindings.cbegin(), state.bindings.cend(),
                                [&binding](const auto& existing) {
                                    return existing.registered &&
                                           shortcuts::bindingsConflict(existing.binding, binding);
                                })) {
                    return invalidValidation(binding, GlobalShortcutFailureReason::AlreadyInUse);
                }
            }
        }
        return validation;
    }

    GlobalShortcutRegistrationState state(GlobalShortcutAction action) const {
        return m_states[actionIndex(action)];
    }

    RegistrationSuspensionHandle suspendRegistrations() {
        const RegistrationSuspensionHandle handle = m_nextSuspensionHandle++;
        const bool firstSuspension = m_suspensions.isEmpty();
        m_suspensions.insert(handle);
        if (firstSuspension) {
            unregisterAll();
            emit q.registrationsSuspended();
        }
        return handle;
    }

    void resumeRegistrations(RegistrationSuspensionHandle handle) {
        if (!m_suspensions.remove(handle) || !m_suspensions.isEmpty()) {
            return;
        }
        reconcile();
    }

    int allocateRegistrationId() {
        const int capacity = LAST_REGISTRATION_ID - FIRST_REGISTRATION_ID + 1;
        for (int attempt = 0; attempt < capacity; ++attempt) {
            const int candidate = m_nextRegistrationId++;
            if (m_nextRegistrationId > LAST_REGISTRATION_ID) {
                m_nextRegistrationId = FIRST_REGISTRATION_ID;
            }
            if (!m_registrationKeysById.contains(candidate)) {
                return candidate;
            }
        }
        return 0;
    }

    void unregisterAll() {
        for (const auto& registration : std::as_const(m_activeRegistrations)) {
            m_backend->unregisterShortcut(registration.registrationId);
        }
        m_activeRegistrations.clear();
        m_registrationKeysById.clear();
    }

    bool sampleFocusedFullscreenState() {
        if (!m_initialized) {
            return false;
        }
        const bool suppressionEnabled =
            storage::GlobalShortcutSettings().disableOnFocusedFullscreenWindow();
        const bool fullscreen =
            suppressionEnabled && m_focusedFullscreenDetector && m_focusedFullscreenDetector();
        const bool fullscreenChanged = fullscreen != m_focusedFullscreen;
        const bool changed =
            suppressionEnabled != m_fullscreenSuppressionEnabled || fullscreenChanged;
        m_fullscreenSuppressionEnabled = suppressionEnabled;
        m_focusedFullscreen = fullscreen;
        if (suppressionEnabled && !m_fullscreenTimer.isActive()) {
            m_fullscreenTimer.start();
        } else if (!suppressionEnabled) {
            m_fullscreenTimer.stop();
        }
        if (fullscreenChanged) {
            emit q.focusedFullscreenSuppressionChanged(fullscreen);
        }
        return changed;
    }

    void refreshFocusedFullscreenState() {
        if (sampleFocusedFullscreenState()) {
            reconcile(false);
        }
    }

    bool actionAvailable(GlobalShortcutAction action) const {
#ifndef Q_OS_WIN
        if (action == GlobalShortcutAction::ToggleWindowUnderCursorAlwaysOnTop)
            return false;
#endif
        if (action != GlobalShortcutAction::TranslateSelectedText) {
            return true;
        }
#if SNOW_SHOT_ENABLE_EXTENDED_FEATURES
        return storage::ExtendedFeaturesSettings().translationPageEnabled();
#else
        return false;
#endif
    }

    bool registrationEligible(GlobalShortcutAction action) const {
        // Keep both gate controls usable so disablement can be undone from
        // the keyboard. An active canvas must also remain dismissible.
        return actionAvailable(action) &&
               (controlsGlobalHotkeyGates(action) ||
                (m_globalHotkeysEnabled &&
                 (!m_focusedFullscreen ||
                  (action == GlobalShortcutAction::GlobalCanvas && m_globalCanvasActive))));
    }

    void reconcile(bool refreshFullscreen = true) {
        if (!m_initialized) {
            return;
        }
        if (refreshFullscreen) {
            sampleFocusedFullscreenState();
        }
        if (!m_suspensions.isEmpty()) {
            return;
        }
        QHash<QString, QString> winnerByIdentity;
        QSet<QString> desiredOwnerKeys;
        QSet<QString> duplicateOwnerKeys;
        for (GlobalShortcutAction action : ALL_ACTIONS) {
            if (!registrationEligible(action)) {
                continue;
            }
            for (const auto& binding : m_shortcuts[actionIndex(action)]) {
                const QString activeOwner = ownerKey(action, binding);
                const QString identity = runtimeIdentityKey(binding);
                if (winnerByIdentity.contains(identity)) {
                    duplicateOwnerKeys.insert(activeOwner);
                } else {
                    winnerByIdentity.insert(identity, activeOwner);
                    desiredOwnerKeys.insert(activeOwner);
                }
            }
        }

        const QList<QString> activeKeys = m_activeRegistrations.keys();
        for (const QString& key : activeKeys) {
            if (!desiredOwnerKeys.contains(key)) {
                const ActiveRegistration registration = m_activeRegistrations.take(key);
                m_registrationKeysById.remove(registration.registrationId);
                m_backend->unregisterShortcut(registration.registrationId);
            }
        }

        std::array<GlobalShortcutRegistrationState, ACTION_COUNT> nextStates;
        for (GlobalShortcutAction action : ALL_ACTIONS) {
            auto& state = nextStates[actionIndex(action)];
            state.action = action;
            state.shortcuts = m_shortcuts[actionIndex(action)];
            if (!actionAvailable(action)) {
                state.status = GlobalShortcutStatus::Unset;
                continue;
            }
            const bool eligible = registrationEligible(action);
            for (const auto& bindingValue : state.shortcuts) {
                GlobalShortcutBindingResult binding;
                binding.binding = bindingValue;
                binding.shortcut = bindingValue.portableText;
                if (!eligible) {
                    state.bindings.push_back(binding);
                    continue;
                }
                const QString activeOwner = ownerKey(action, bindingValue);
                if (duplicateOwnerKeys.contains(activeOwner)) {
                    binding.failureReason = GlobalShortcutFailureReason::AlreadyInUse;
                    state.bindings.push_back(binding);
                    continue;
                }
                if (m_activeRegistrations.contains(activeOwner)) {
                    binding.registered = true;
                    state.bindings.push_back(binding);
                    continue;
                }
                const int registrationId = allocateRegistrationId();
                if (registrationId == 0) {
                    binding.failureReason = GlobalShortcutFailureReason::SystemError;
                    state.bindings.push_back(binding);
                    continue;
                }
                const auto result = m_backend->registerShortcut(registrationId, bindingValue);
                binding.registered = result.registered;
                binding.failureReason = result.failureReason;
                binding.nativeErrorCode = result.nativeErrorCode;
                diagnostics::logEvent(
                    QStringLiteral("snow_shot.shortcuts"), QStringLiteral("shortcut.registration"),
                    {{QStringLiteral("operation"), QString::number(registrationId)},
                     {QStringLiteral("code"), result.nativeErrorCode},
                     {QStringLiteral("outcome"),
                      result.registered ? QStringLiteral("registered") : QStringLiteral("failed")}},
                    result.registered ? QtInfoMsg : QtWarningMsg);
                state.bindings.push_back(binding);
                if (result.registered) {
                    m_activeRegistrations.insert(
                        activeOwner, ActiveRegistration{action, bindingValue, registrationId});
                    m_registrationKeysById.insert(registrationId, activeOwner);
                }
            }
            state.status = !eligible && !state.bindings.isEmpty() ? GlobalShortcutStatus::Disabled
                                                                  : aggregateStatus(state.bindings);
        }

        for (GlobalShortcutAction action : ALL_ACTIONS) {
            const std::size_t index = actionIndex(action);
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
    QTimer m_fullscreenTimer;
    std::array<shortcuts::ShortcutBindingList, ACTION_COUNT> m_shortcuts;
    std::array<GlobalShortcutRegistrationState, ACTION_COUNT> m_states;
    QHash<QString, ActiveRegistration> m_activeRegistrations;
    QHash<int, QString> m_registrationKeysById;
    QSet<RegistrationSuspensionHandle> m_suspensions;
    int m_nextRegistrationId = FIRST_REGISTRATION_ID;
    RegistrationSuspensionHandle m_nextSuspensionHandle = 1;
    bool m_initialized = false;
    bool m_globalHotkeysEnabled = true;
    bool m_globalCanvasActive = false;
    bool m_fullscreenSuppressionEnabled = false;
    bool m_focusedFullscreen = false;
};

GlobalShortcutManager::GlobalShortcutManager(QObject* parent)
    : GlobalShortcutManager(nullptr, parent, {}) {}

GlobalShortcutManager::GlobalShortcutManager(std::unique_ptr<GlobalShortcutBackend> backend,
                                             QObject* parent,
                                             std::function<bool()> fullscreenDetector)
    : QObject(parent),
      m_impl(std::make_unique<Impl>(*this, std::move(backend), std::move(fullscreenDetector))) {}

GlobalShortcutManager::~GlobalShortcutManager() = default;

void GlobalShortcutManager::initialize() {
    m_impl->initialize();
}

GlobalShortcutRegistrationState GlobalShortcutManager::state(GlobalShortcutAction action) const {
    return m_impl->state(action);
}

GlobalShortcutValidationResult
GlobalShortcutManager::validateShortcut(GlobalShortcutAction action,
                                        const shortcuts::ShortcutBinding& shortcut) const {
    return m_impl->validateShortcut(action, shortcut);
}

GlobalShortcutValidationResult
GlobalShortcutManager::validateShortcut(const QString& shortcut) const {
    return m_impl->validateShortcut(std::nullopt, shortcuts::ShortcutBinding{shortcut});
}

bool GlobalShortcutManager::setShortcuts(GlobalShortcutAction action,
                                         const shortcuts::ShortcutBindingList& bindings) {
    return m_impl->setShortcuts(action, bindings);
}

GlobalShortcutManager::RegistrationSuspensionHandle GlobalShortcutManager::suspendRegistrations() {
    return m_impl->suspendRegistrations();
}

void GlobalShortcutManager::resumeRegistrations(RegistrationSuspensionHandle handle) {
    m_impl->resumeRegistrations(handle);
}

void GlobalShortcutManager::setGlobalCanvasActive(bool active) {
    if (m_impl->m_globalCanvasActive == active) {
        return;
    }
    m_impl->m_globalCanvasActive = active;
    m_impl->reconcile();
}

void GlobalShortcutManager::setGlobalHotkeysEnabled(bool enabled) {
    if (m_impl->m_globalHotkeysEnabled == enabled) {
        return;
    }
    m_impl->m_globalHotkeysEnabled = enabled;
    m_impl->reconcile();
    emit globalHotkeysEnabledChanged(enabled);
}

void GlobalShortcutManager::refreshFocusedFullscreenState() {
    m_impl->refreshFocusedFullscreenState();
}

std::optional<GlobalShortcutInputState>
GlobalShortcutManager::inputState(int registrationId) const {
    if (registrationId != 0 && (!m_impl->m_globalHotkeysEnabled ||
                                !m_impl->m_registrationKeysById.contains(registrationId))) {
        return std::nullopt;
    }
    return m_impl->m_backend->inputState(registrationId);
}

bool GlobalShortcutManager::globalHotkeysEnabled() const {
    return m_impl->m_globalHotkeysEnabled;
}

bool GlobalShortcutManager::focusedFullscreenSuppressionActive() const {
    return m_impl->m_focusedFullscreen;
}

} // namespace snow_shot::presentation
