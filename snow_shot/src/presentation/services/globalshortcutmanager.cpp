#include "snow_shot/presentation/globalshortcutmanager.h"

#include "globalshortcutbackend_p.h"

#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/platform/focusedfullscreenwindow.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QScopedValueRollback>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace snow_shot::presentation {
namespace {

constexpr int MAX_SHORTCUTS_PER_ACTION = 2;
constexpr int FIRST_REGISTRATION_ID = 0x2200;
constexpr int LAST_REGISTRATION_ID = 0xBFFF;
constexpr std::size_t ACTION_COUNT = 21;

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
    GlobalShortcutAction::OpenScreenRecordingFolder,
    GlobalShortcutAction::OpenCaptureHistory,
    GlobalShortcutAction::OpenPinToScreenManagement,
    GlobalShortcutAction::FullscreenCanvas,
    GlobalShortcutAction::OpenSettings,
    GlobalShortcutAction::PinClipboardContent,
    GlobalShortcutAction::TranslateSelectedText,
    GlobalShortcutAction::PinSelectedFiles,
    GlobalShortcutAction::RestoreLastClosedWindows,
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
    case GlobalShortcutAction::OpenScreenRecordingFolder:
        return settings.openScreenRecordingFolder();
    case GlobalShortcutAction::OpenCaptureHistory:
        return settings.openCaptureHistory();
    case GlobalShortcutAction::OpenPinToScreenManagement:
        return settings.openPinToScreenManagement();
    case GlobalShortcutAction::FullscreenCanvas:
        return settings.fullscreenCanvas();
    case GlobalShortcutAction::OpenSettings:
        return settings.openSettings();
    case GlobalShortcutAction::PinClipboardContent:
        return settings.pinClipboardContent();
    case GlobalShortcutAction::PinSelectedFiles:
        return settings.pinSelectedFiles();
    case GlobalShortcutAction::RestoreLastClosedWindows:
        return settings.restoreLastClosedWindows();
    case GlobalShortcutAction::TranslateSelectedText:
        return settings.translateSelectedText();
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
    case GlobalShortcutAction::ScreenshotTranslation:
        return settings.setScreenshotTranslation(bindings);
    case GlobalShortcutAction::ScreenshotCopy:
        return settings.setScreenshotCopy(bindings);
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
    case GlobalShortcutAction::OpenPinToScreenManagement:
        return settings.setOpenPinToScreenManagement(bindings);
    case GlobalShortcutAction::FullscreenCanvas:
        return settings.setFullscreenCanvas(bindings);
    case GlobalShortcutAction::OpenSettings:
        return settings.setOpenSettings(bindings);
    case GlobalShortcutAction::PinClipboardContent:
        return settings.setPinClipboardContent(bindings);
    case GlobalShortcutAction::PinSelectedFiles:
        return settings.setPinSelectedFiles(bindings);
    case GlobalShortcutAction::RestoreLastClosedWindows:
        return settings.setRestoreLastClosedWindows(bindings);
    case GlobalShortcutAction::TranslateSelectedText:
        return settings.setTranslateSelectedText(bindings);
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
        m_backend->setActivationHandler([this](int registrationId) {
            const QString activeKey = m_registrationKeysById.value(registrationId);
            const auto active = m_activeRegistrations.constFind(activeKey);
            if (active == m_activeRegistrations.cend()) {
                return;
            }
            // Gate-control shortcuts must stay usable under both the session
            // disablement and fullscreen suppression so either can be undone
            // from the keyboard.
            const bool gateControl = controlsGlobalHotkeyGates(active->action);
            if ((!m_globalHotkeysEnabled && !gateControl) ||
                (active->action == GlobalShortcutAction::TranslateSelectedText &&
                 !storage::ExtendedFeaturesSettings().translationPageEnabled())) {
                return;
            }
            if (gateControl ||
                !storage::GlobalShortcutSettings().disableOnFocusedFullscreenWindow() ||
                !m_focusedFullscreenDetector || !m_focusedFullscreenDetector()) {
                emit q.activated(active->action);
            }
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
        QObject::connect(&storage::ApplicationStorage::instance().configuration(),
                         &storage::ConfigurationStore::valueChanged, &q,
                         [this](const QString& key, const QJsonValue&) {
                             if (m_initialized && !m_persistingShortcuts &&
                                 key.startsWith(QStringLiteral("global_shortcuts/"))) {
                                 schedulePersistedShortcutRefresh();
                             }
                             if (m_initialized &&
                                 key ==
                                     QStringLiteral("extended_features/translation_page_enabled")) {
                                 reconcile();
                             }
                         });
        m_initialized = true;
        reconcile();
    }

    void schedulePersistedShortcutRefresh() {
        if (m_persistedRefreshScheduled) {
            return;
        }
        m_persistedRefreshScheduled = true;
        // Configuration observers and stateChanged observers may write another
        // shortcut synchronously. Refresh after that transaction and never enter
        // reconcile recursively while it is publishing its state snapshot.
        QMetaObject::invokeMethod(
            &q,
            [this] {
                m_persistedRefreshScheduled = false;
                const storage::ShortcutSettings settings;
                bool changed = false;
                for (GlobalShortcutAction action : ALL_ACTIONS) {
                    auto& current = m_shortcuts[actionIndex(action)];
                    const auto persisted = canonicalBindings(persistedShortcuts(settings, action));
                    if (current != persisted) {
                        current = persisted;
                        changed = true;
                    }
                }
                if (changed) {
                    reconcile();
                }
            },
            Qt::QueuedConnection);
    }

    bool setShortcuts(GlobalShortcutAction action, const shortcuts::ShortcutBindingList& bindings) {
        if (!m_initialized) {
            initialize();
        }
        const auto canonical = canonicalBindings(bindings);
        bool persisted = false;
        {
            const QScopedValueRollback<bool> persisting(m_persistingShortcuts, true);
            persisted = persistShortcuts(storage::ShortcutSettings(), action, canonical);
        }
        // A synchronous storage observer may have changed a different key while
        // our own write was guarded. The queued refresh also reconciles that edit.
        schedulePersistedShortcutRefresh();
        if (!persisted)
            return false;
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
        if (m_suspensions.isEmpty()) {
            unregisterAll();
        }
        m_suspensions.insert(handle);
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

    void reconcile() {
        if (!m_initialized || !m_suspensions.isEmpty()) {
            return;
        }
        const bool translationEnabled =
            storage::ExtendedFeaturesSettings().translationPageEnabled();
        QHash<QString, QString> winnerByIdentity;
        QSet<QString> desiredOwnerKeys;
        QSet<QString> duplicateOwnerKeys;
        for (GlobalShortcutAction action : ALL_ACTIONS) {
            if (action == GlobalShortcutAction::TranslateSelectedText && !translationEnabled) {
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
            if (action == GlobalShortcutAction::TranslateSelectedText && !translationEnabled) {
                state.status = GlobalShortcutStatus::Unset;
                continue;
            }
            for (const auto& bindingValue : state.shortcuts) {
                GlobalShortcutBindingResult binding;
                binding.binding = bindingValue;
                binding.shortcut = bindingValue.portableText;
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
            state.status = aggregateStatus(state.bindings);
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
    std::array<shortcuts::ShortcutBindingList, ACTION_COUNT> m_shortcuts;
    std::array<GlobalShortcutRegistrationState, ACTION_COUNT> m_states;
    QHash<QString, ActiveRegistration> m_activeRegistrations;
    QHash<int, QString> m_registrationKeysById;
    QSet<RegistrationSuspensionHandle> m_suspensions;
    int m_nextRegistrationId = FIRST_REGISTRATION_ID;
    RegistrationSuspensionHandle m_nextSuspensionHandle = 1;
    bool m_initialized = false;
    bool m_persistingShortcuts = false;
    bool m_persistedRefreshScheduled = false;
    bool m_globalHotkeysEnabled = true;
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

void GlobalShortcutManager::setGlobalHotkeysEnabled(bool enabled) {
    if (m_impl->m_globalHotkeysEnabled == enabled) {
        return;
    }
    m_impl->m_globalHotkeysEnabled = enabled;
    emit globalHotkeysEnabledChanged(enabled);
}

bool GlobalShortcutManager::globalHotkeysEnabled() const {
    return m_impl->m_globalHotkeysEnabled;
}

} // namespace snow_shot::presentation
