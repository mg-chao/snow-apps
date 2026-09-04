#include "snow_shot/presentation/settings/settingsruntimesession.h"

#include "antd_icons.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QSet>

#include <cstdlib>
#include <iostream>

namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;
namespace presentation = snow_shot::presentation;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

settings::TranslatableText text(const char* source) {
    return {"SettingsRuntimeSessionTests", source};
}

enum class WriteMode {
    Immediate,
    Reject,
    MutateThenReject,
    Pending,
};

class FakeSettingsBackend final : public settings::SettingsBackend {
  public:
    FakeSettingsBackend() {
        m_status.writeAvailable = true;
        m_status.effectiveMode = storage::StorageMode::ApplicationData;
        m_status.effectiveDirectory = QStringLiteral("C:/settings-runtime-tests");
        m_status.historyUsage.entryCount = 1;
        m_status.historyUsage.totalBytes = 128;
    }

    QVariant selectValue(settings::SettingsSelectBinding binding) const override {
        if (binding == settings::SettingsSelectBinding::Theme) {
            return m_theme;
        }
        return {};
    }

    QVector<settings::SettingsRuntimeOption>
    dynamicSelectOptions(settings::SettingsSelectBinding binding) const override {
        if (binding == settings::SettingsSelectBinding::Language) {
            return {{QStringLiteral("en_US"), QStringLiteral("English")}};
        }
        return {};
    }

    bool applySelectValue(settings::SettingsSelectBinding binding,
                          const QVariant& value) override {
        if (binding != settings::SettingsSelectBinding::Theme) {
            return false;
        }
        return applyField(QStringLiteral("theme"), value,
                          [this](const QVariant& next) { m_theme = next.toString(); });
    }

    bool switchValue(settings::SettingsSwitchBinding binding) const override {
        if (binding == settings::SettingsSwitchBinding::TrayEnabled) {
            return m_trayEnabled;
        }
        return false;
    }

    bool switchEnabled(settings::SettingsSwitchBinding) const override { return true; }

    bool applySwitchValue(settings::SettingsSwitchBinding binding, bool value) override {
        if (binding != settings::SettingsSwitchBinding::TrayEnabled) {
            return false;
        }
        return applyField(QStringLiteral("tray-enabled"), value,
                          [this](const QVariant& next) { m_trayEnabled = next.toBool(); });
    }

    QVariantList multiSelectValue(settings::SettingsMultiSelectBinding binding) const override {
        if (binding == settings::SettingsMultiSelectBinding::TrayMenuOptions) {
            return m_trayOptions;
        }
        return {};
    }

    bool applyMultiSelectValue(settings::SettingsMultiSelectBinding binding,
                               const QVariantList& value) override {
        if (binding != settings::SettingsMultiSelectBinding::TrayMenuOptions) {
            return false;
        }
        return applyField(QStringLiteral("tray-options"), value,
                          [this](const QVariant& next) { m_trayOptions = next.toList(); });
    }

    int integerValue(settings::SettingsIntegerBinding binding) const override {
        if (binding == settings::SettingsIntegerBinding::ScreenshotDelaySeconds) {
            return m_delay;
        }
        return 0;
    }

    bool applyIntegerValue(settings::SettingsIntegerBinding binding, int value) override {
        if (binding != settings::SettingsIntegerBinding::ScreenshotDelaySeconds) {
            return false;
        }
        return applyField(QStringLiteral("delay"), value,
                          [this](const QVariant& next) { m_delay = next.toInt(); });
    }

    int sliderValue(settings::SettingsSliderBinding) const override { return 0; }
    bool applySliderValue(settings::SettingsSliderBinding, int) override { return false; }

    QColor colorValue(settings::SettingsColorBinding) const override { return {}; }
    bool applyColorValue(settings::SettingsColorBinding, const QColor&) override { return false; }

    QVariant radioValue(settings::SettingsRadioBinding) const override { return {}; }
    bool applyRadioValue(settings::SettingsRadioBinding, const QVariant&) override { return false; }

    QString filePathValue(settings::SettingsFilePathBinding) const override { return {}; }
    bool applyFilePathValue(settings::SettingsFilePathBinding, const QString&) override {
        return false;
    }

    QString directoryPathValue(settings::SettingsDirectoryPathBinding) const override { return {}; }
    bool applyDirectoryPathValue(settings::SettingsDirectoryPathBinding,
                                 const QString&) override {
        return false;
    }

    QString textValue(settings::SettingsTextBinding) const override { return {}; }
    bool applyTextValue(settings::SettingsTextBinding, const QString&) override { return false; }

    storage::ScreenshotToolbarLayout toolbarLayout() const override { return m_toolbar; }

    bool applyToolbarLayout(const storage::ScreenshotToolbarLayout& layout) override {
        return applyField(QStringLiteral("toolbar"), QVariant::fromValue(layout),
                          [this](const QVariant& next) {
                              m_toolbar = next.value<storage::ScreenshotToolbarLayout>();
                          });
    }

    presentation::GlobalShortcutRegistrationState
    shortcutState(presentation::GlobalShortcutAction action) const override {
        presentation::GlobalShortcutRegistrationState result;
        result.action = action;
        return result;
    }

    presentation::GlobalShortcutValidationResult
    validateShortcut(const QString& shortcut) const override {
        return {shortcut, true, presentation::GlobalShortcutFailureReason::None};
    }

    bool applyShortcuts(presentation::GlobalShortcutAction,
                        const QStringList&) override {
        return false;
    }

    QStringList localShortcuts(settings::SettingsLocalShortcutScope,
                               const QString&) const override {
        return {};
    }

    presentation::GlobalShortcutValidationResult
    validateLocalShortcut(settings::SettingsLocalShortcutScope, const QString&,
                          const QString& shortcut) const override {
        return {shortcut, true, presentation::GlobalShortcutFailureReason::None};
    }

    bool applyLocalShortcuts(settings::SettingsLocalShortcutScope, const QString&,
                             const QStringList&) override {
        return false;
    }

    settings::SettingsActionState actionState(settings::SettingsActionBinding) const override {
        return {true, false};
    }

    bool triggerAction(settings::SettingsActionBinding) override { return true; }

    storage::StorageStatus storageStatus() const override { return m_status; }

    void refreshStorageStatus() override { ++m_refreshCount; }

    int refreshCount() const { return m_refreshCount; }

    void setAppUsage(const storage::AppStorageUsage& usage) {
        m_status.appUsage = usage;
        emit synchronized();
    }

    bool resetSection(settings::SettingsSectionReset) override {
        if (!m_resetAccepted) {
            m_status.lastConfigurationError = QStringLiteral("reset rejected");
            emit synchronized();
            return false;
        }
        if (m_resetHistoryPending) {
            m_status.historyPolicyUpdating = true;
        }
        return true;
    }

    QString fieldError(const QString& fieldId) const override { return m_fieldErrors.value(fieldId); }

    bool fieldPending(const QString& fieldId) const override {
        const auto found = m_pending.constFind(fieldId);
        return (found != m_pending.cend() && !found->isEmpty()) ||
               (m_status.historyPolicyUpdating && fieldId == QStringLiteral("theme"));
    }

    void setMode(const QString& fieldId, WriteMode mode) { m_modes.insert(fieldId, mode); }

    void setResetAccepted(bool accepted) { m_resetAccepted = accepted; }

    void setResetHistoryPending(bool pending) { m_resetHistoryPending = pending; }

    void completeHistoryReset() {
        m_status.historyPolicyUpdating = false;
        emit synchronized();
    }

    int applyCount(const QString& fieldId) const { return m_applyCounts.value(fieldId); }

    void setConfigurationError(const QString& error) {
        m_status.lastConfigurationError = error;
        emit synchronized();
    }

    void notify() { emit synchronized(); }

    void setExternal(const QString& fieldId, const QVariant& value) {
        setFieldValue(fieldId, value);
        emit synchronized();
    }

    void complete(const QString& fieldId, int index = 0, bool accepted = true) {
        auto found = m_pending.find(fieldId);
        if (found == m_pending.end() || index < 0 || index >= found->size()) {
            return;
        }
        const QVariant value = found->takeAt(index);
        if (accepted) {
            setFieldValue(fieldId, value);
            m_fieldErrors.remove(fieldId);
        } else {
            m_fieldErrors.insert(fieldId, QStringLiteral("async rejection"));
        }
        if (found->isEmpty()) {
            m_pending.erase(found);
        }
        emit synchronized();
    }

    QString theme() const { return m_theme; }
    bool trayEnabled() const { return m_trayEnabled; }
    int delay() const { return m_delay; }

  private:
    template <typename Setter>
    bool applyField(const QString& fieldId, const QVariant& value, Setter setter) {
        ++m_applyCounts[fieldId];
        const WriteMode mode = m_modes.value(fieldId, WriteMode::Immediate);
        if (mode == WriteMode::Reject) {
            m_fieldErrors.insert(fieldId, QStringLiteral("rejected"));
            emit synchronized();
            return false;
        }
        if (mode == WriteMode::MutateThenReject) {
            setter(value);
            m_fieldErrors.insert(fieldId, QStringLiteral("persistence failed"));
            emit synchronized();
            return false;
        }
        if (mode == WriteMode::Pending) {
            m_pending[fieldId].push_back(value);
            return true;
        }
        setter(value);
        m_fieldErrors.remove(fieldId);
        emit synchronized();
        return true;
    }

    void setFieldValue(const QString& fieldId, const QVariant& value) {
        if (fieldId == QStringLiteral("theme")) {
            m_theme = value.toString();
        } else if (fieldId == QStringLiteral("tray-enabled")) {
            m_trayEnabled = value.toBool();
        } else if (fieldId == QStringLiteral("delay")) {
            m_delay = value.toInt();
        } else if (fieldId == QStringLiteral("tray-options")) {
            m_trayOptions = value.toList();
        } else if (fieldId == QStringLiteral("toolbar")) {
            m_toolbar = value.value<storage::ScreenshotToolbarLayout>();
        }
    }

    QString m_theme = QStringLiteral("system");
    bool m_trayEnabled = true;
    int m_delay = 3;
    QVariantList m_trayOptions{QStringLiteral("quick.screenshot")};
    storage::ScreenshotToolbarLayout m_toolbar{
        {{QStringLiteral("select")}}, {QStringLiteral("eraser")}};
    storage::StorageStatus m_status;
    QHash<QString, WriteMode> m_modes;
    QHash<QString, int> m_applyCounts;
    QHash<QString, QString> m_fieldErrors;
    QHash<QString, QVector<QVariant>> m_pending;
    bool m_resetAccepted = true;
    bool m_resetHistoryPending = false;
    int m_refreshCount = 0;
};

settings::SettingsRegistry testRegistry(
    settings::SettingsSectionReset reset = settings::SettingsSectionReset::None) {
    settings::SettingsSelectDefinition theme;
    theme.binding = settings::SettingsSelectBinding::Theme;
    theme.options = {{QStringLiteral("system"), text("Follow system")},
                     {QStringLiteral("light"), text("Light")},
                     {QStringLiteral("dark"), text("Dark")}};

    settings::SettingsSwitchDefinition trayEnabled;
    trayEnabled.binding = settings::SettingsSwitchBinding::TrayEnabled;

    settings::SettingsIntegerDefinition delay;
    delay.binding = settings::SettingsIntegerBinding::ScreenshotDelaySeconds;
    delay.suffix = text("s");

    settings::SettingsCustomDefinition trayOptions;
    trayOptions.renderer = settings::SettingsCustomRenderer::TrayMenuOptions;

    settings::SettingsCustomDefinition toolbar;
    toolbar.renderer = settings::SettingsCustomRenderer::DrawingToolbarEditor;

    settings::SettingsCustomDefinition storageStatus;
    storageStatus.renderer = settings::SettingsCustomRenderer::StorageStatus;

    settings::SettingsSectionDefinition section{
        QStringLiteral("general"), text("General"), text("General settings"),
        reset,
        {{QStringLiteral("theme"), text("Theme"), text("Theme"), {},
          QStringLiteral("interface/theme_mode"), theme},
         {QStringLiteral("tray-enabled"), text("Tray enabled"), text("Tray enabled"), {},
          QStringLiteral("tray/enabled"), trayEnabled},
         {QStringLiteral("delay"), text("Delay"), text("Delay"), {},
          QStringLiteral("screenshot/delay_seconds"), delay},
         {QStringLiteral("tray-options"), text("Tray options"), text("Tray options"), {},
          QStringLiteral("tray/menu_options"), trayOptions},
         {QStringLiteral("toolbar"), text("Toolbar"), text("Toolbar"), {},
          QStringLiteral("screenshot_toolbar/layout"), toolbar},
         {QStringLiteral("storage-status"), text("Storage status"), text("Storage status"), {},
          {}, storageStatus}}};

    settings::SettingsPageDefinition page{QStringLiteral("test-page"), QStringLiteral("/test"),
                                          text("Test"), text("Test settings"), {section}};
    settings::SettingsNavigationPageDefinition navigation{
        QStringLiteral("nav.test"), page.id, []() { return adqt::icons::antd::outlined::Appstore(); }};
    settings::SettingsCatalog catalog({page}, {navigation}, {page.id, section.id, {}});
    return settings::SettingsRegistry::fromCatalog(catalog, QStringLiteral("test-provider"));
}

settings::SettingsRegistry registryWithoutStandaloneDelay() {
    settings::SettingsSelectDefinition theme;
    theme.binding = settings::SettingsSelectBinding::Theme;
    settings::SettingsSectionDefinition section{
        QStringLiteral("general"), text("General"), text("General settings"),
        settings::SettingsSectionReset::None,
        {{QStringLiteral("theme"), text("Theme"), text("Theme"), {},
          QStringLiteral("interface/theme_mode"), theme}}};
    settings::SettingsPageDefinition page{QStringLiteral("test-page"), QStringLiteral("/test"),
                                          text("Test"), text("Test settings"), {section}};
    settings::SettingsNavigationPageDefinition navigation{
        QStringLiteral("nav.test"), page.id,
        []() { return adqt::icons::antd::outlined::Appstore(); }};
    return settings::SettingsRegistry::fromCatalog(
        settings::SettingsCatalog({page}, {navigation}, {page.id, section.id, {}}),
        QStringLiteral("test-provider"));
}

void initialStateAndNoOp() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);

    const settings::SettingsFieldState initial = session.state(QStringLiteral("theme"));
    require(initial.acceptedValue == QStringLiteral("system") &&
                initial.draftValue == QStringLiteral("system") && !initial.dirty &&
                !initial.busy && initial.phase == settings::SettingsWritePhase::Clean &&
                initial.revision == 0,
            "initial runtime state must be clean and revision zero");
    const quint64 revision = initial.revision;
    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("system")),
            "a no-op draft must be accepted");
    require(backend.applyCount(QStringLiteral("theme")) == 0 &&
                session.state(QStringLiteral("theme")).revision == revision,
            "a no-op draft must not write or increment its revision");
    require(!session.submitDraft(QStringLiteral("missing"), QStringLiteral("value")),
            "unknown fields must be rejected");
    require(!session.submitDraft(QStringLiteral("storage-status"), QStringLiteral("value")),
            "read-only fields must be rejected");
    const settings::SettingsFieldState unknown = session.state(QStringLiteral("missing"));
    require(!unknown.enabled && !unknown.visible && unknown.acceptedValue.isNull() &&
                unknown.draftValue.isNull(),
            "unknown fields must be disabled and invisible");
}

void storageUsagePropagation() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);

    session.refreshStorageStatus();
    require(backend.refreshCount() == 1,
            "a storage status refresh must be forwarded to the backend");

    int statusChanges = 0;
    storage::StorageStatus latest;
    QObject::connect(&session, &settings::SettingsRuntimeSession::storageStateChanged, &session,
                     [&statusChanges, &latest](const storage::StorageStatus& status) {
                         ++statusChanges;
                         latest = status;
                     });

    storage::AppStorageUsage usage;
    usage.thumbnailCacheBytes = 2048;
    usage.recordingTempBytes = 4096;
    backend.setAppUsage(usage);
    flushEvents();
    require(statusChanges >= 1,
            "an app usage change must emit storageStateChanged");
    require(latest.appUsage.thumbnailCacheBytes == 2048 &&
                latest.appUsage.recordingTempBytes == 4096 &&
                latest.appUsage.totalBytes() == 6144,
            "the emitted status must carry the updated app usage");

    backend.notify();
    flushEvents();
    require(statusChanges == 1, "an unchanged app usage must not re-emit storageStateChanged");
}

void synchronousWriteAndFieldSignals() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    QHash<QString, int> signalCounts;
    QObject::connect(&session, &settings::SettingsRuntimeSession::fieldChanged, &session,
                     [&signalCounts](const QString& fieldId,
                                     const settings::SettingsFieldState&) {
                         ++signalCounts[fieldId];
                     });

    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "synchronous write must be accepted");
    const settings::SettingsFieldState state = session.state(QStringLiteral("theme"));
    require(state.acceptedValue == QStringLiteral("dark") &&
                state.draftValue == QStringLiteral("dark") && !state.dirty && !state.busy &&
                state.phase == settings::SettingsWritePhase::Clean && signalCounts.value("theme") > 0,
            "synchronous write must settle the field and emit its field signal");
    require(signalCounts.value(QStringLiteral("tray-enabled")) == 0 &&
                signalCounts.value(QStringLiteral("delay")) == 0,
            "a field write must not refresh unrelated field subscribers");
    require(session.selectValue(settings::SettingsSelectBinding::Theme) ==
                QStringLiteral("dark"),
            "compatibility reads must expose the accepted draft");
}

void rejectedWriteRetainsDraftAndCanRetry() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    backend.setMode(QStringLiteral("theme"), WriteMode::Reject);
    settings::SettingsRuntimeSession session(registry, backend);

    require(!session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "a rejected backend write must report failure");
    settings::SettingsFieldState rejected = session.state(QStringLiteral("theme"));
    require(rejected.acceptedValue == QStringLiteral("system") &&
                rejected.draftValue == QStringLiteral("dark") && rejected.dirty &&
                !rejected.busy && rejected.phase == settings::SettingsWritePhase::Rejected &&
                rejected.error == QStringLiteral("rejected"),
            "rejected writes must retain the attempted draft and error");

    backend.setMode(QStringLiteral("theme"), WriteMode::Immediate);
    require(session.retry(QStringLiteral("theme")), "a rejected draft must be retryable");
    const settings::SettingsFieldState retried = session.state(QStringLiteral("theme"));
    require(retried.acceptedValue == QStringLiteral("dark") && !retried.dirty &&
                retried.phase == settings::SettingsWritePhase::Clean && retried.error.isEmpty() &&
                backend.applyCount(QStringLiteral("theme")) == 2,
            "a successful retry must commit and clear the rejected state");
}

void mutatedRejectedWriteDoesNotSelfHeal() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    backend.setMode(QStringLiteral("theme"), WriteMode::MutateThenReject);
    settings::SettingsRuntimeSession session(registry, backend);

    require(!session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "a write that mutates the backend before failing must report failure");
    const settings::SettingsFieldState failed = session.state(QStringLiteral("theme"));
    require(failed.acceptedValue == QStringLiteral("system") &&
                failed.draftValue == QStringLiteral("dark") && failed.dirty &&
                !failed.busy && failed.phase == settings::SettingsWritePhase::Rejected &&
                failed.error == QStringLiteral("persistence failed"),
            "a mutated rejected write must retain its retryable error and draft");

    backend.setMode(QStringLiteral("theme"), WriteMode::Immediate);
    backend.notify();
    flushEvents();
    const settings::SettingsFieldState afterNotification = session.state(QStringLiteral("theme"));
    require(afterNotification.phase == settings::SettingsWritePhase::Rejected &&
                afterNotification.dirty && afterNotification.draftValue == QStringLiteral("dark"),
            "a synchronization matching a rejected target must not self-heal the failure");

    require(session.retry(QStringLiteral("theme")),
            "a mutated rejected write must remain retryable");
    const settings::SettingsFieldState retried = session.state(QStringLiteral("theme"));
    require(retried.phase == settings::SettingsWritePhase::Clean && !retried.dirty &&
                retried.acceptedValue == QStringLiteral("dark") &&
                backend.applyCount(QStringLiteral("theme")) == 2,
            "a successful retry must settle a previously mutated rejected write");
}

void pendingWriteDiscardAndCompletionShield() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    backend.setMode(QStringLiteral("tray-enabled"), WriteMode::Pending);
    settings::SettingsRuntimeSession session(registry, backend);

    require(session.submitDraft(QStringLiteral("tray-enabled"), false),
            "pending write must be accepted for asynchronous processing");
    const settings::SettingsFieldState pending = session.state(QStringLiteral("tray-enabled"));
    require(pending.acceptedValue == true && pending.draftValue == false && pending.dirty &&
                pending.busy && pending.phase == settings::SettingsWritePhase::Pending &&
                session.hasPendingWrites(),
            "pending writes must expose baseline, draft, and busy state");

    const quint64 pendingRevision = pending.revision;
    require(session.discard(QStringLiteral("tray-enabled")), "pending drafts must be discardable");
    const settings::SettingsFieldState discarded = session.state(QStringLiteral("tray-enabled"));
    require(discarded.acceptedValue == true && discarded.draftValue == true && !discarded.dirty &&
                !discarded.busy && discarded.revision > pendingRevision,
            "discard must restore the accepted baseline and advance the revision");

    backend.complete(QStringLiteral("tray-enabled"));
    flushEvents();
    const settings::SettingsFieldState afterCompletion =
        session.state(QStringLiteral("tray-enabled"));
    require(afterCompletion.acceptedValue == true && afterCompletion.draftValue == true &&
                !afterCompletion.dirty && !afterCompletion.busy,
            "a late completion must not resurrect a discarded draft");

    backend.notify();
    backend.notify();
    flushEvents();
    require(session.state(QStringLiteral("tray-enabled")) == afterCompletion,
            "repeated notifications for a discarded completion must be no-ops");

    backend.setExternal(QStringLiteral("tray-enabled"), true);
    flushEvents();
    backend.setExternal(QStringLiteral("tray-enabled"), false);
    flushEvents();
    require(session.state(QStringLiteral("tray-enabled")).acceptedValue == false,
            "a later external transition back to a discarded target must be observable");
}

void newerRevisionWinsOverStaleCompletion() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    backend.setMode(QStringLiteral("theme"), WriteMode::Pending);
    settings::SettingsRuntimeSession session(registry, backend);

    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "first asynchronous write must be accepted");
    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("light")),
            "a newer draft must supersede the first asynchronous write");
    require(backend.applyCount(QStringLiteral("theme")) == 2 &&
                session.state(QStringLiteral("theme")).draftValue == QStringLiteral("light"),
            "superseding a pending write must retain the newest draft");

    backend.complete(QStringLiteral("theme"), 0);
    flushEvents();
    settings::SettingsFieldState afterStale = session.state(QStringLiteral("theme"));
    require(afterStale.draftValue == QStringLiteral("light") && afterStale.dirty &&
                afterStale.busy && afterStale.phase == settings::SettingsWritePhase::Pending,
            "a stale completion must not settle a newer revision");

    backend.notify();
    backend.notify();
    flushEvents();
    require(session.state(QStringLiteral("theme")) == afterStale,
            "repeated stale-completion notifications must not alter the newer revision");

    backend.complete(QStringLiteral("theme"), 0);
    flushEvents();
    const settings::SettingsFieldState settled = session.state(QStringLiteral("theme"));
    require(settled.acceptedValue == QStringLiteral("light") &&
                settled.draftValue == QStringLiteral("light") && !settled.dirty &&
                !settled.busy && settled.phase == settings::SettingsWritePhase::Clean,
            "the newest completion must settle the current revision");
}

void conflictAndScopedErrorSnapshots() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    backend.setConfigurationError(QStringLiteral("previous operation failed"));
    settings::SettingsRuntimeSession session(registry, backend);

    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "a stale global error must not reject a new write");
    require(session.state(QStringLiteral("theme")).phase == settings::SettingsWritePhase::Clean,
            "a stale global error must not turn a successful write into a failure");

    backend.setMode(QStringLiteral("delay"), WriteMode::Pending);
    require(session.submitDraft(QStringLiteral("delay"), 7),
            "the delayed field must enter pending state");
    backend.setConfigurationError(QStringLiteral("new configuration failure"));
    flushEvents();
    const settings::SettingsFieldState failed = session.state(QStringLiteral("delay"));
    require(failed.dirty && !failed.busy && failed.phase == settings::SettingsWritePhase::Failed &&
                failed.error == QStringLiteral("new configuration failure") &&
                failed.draftValue == 7,
            "a new scoped backend error must fail only the affected pending write and retain its draft");

    require(session.discard(QStringLiteral("delay")),
            "a failed asynchronous write must be discardable");
    require(session.state(QStringLiteral("delay")).draftValue == 3 &&
                !session.state(QStringLiteral("delay")).dirty,
            "discard after failure must restore the accepted baseline");
    backend.complete(QStringLiteral("delay"));
    flushEvents();
    require(session.state(QStringLiteral("delay")).acceptedValue == 3,
            "a completion that arrives after a failed draft was discarded must stay quarantined");
}

void asynchronousFailureCanRetryAndExternalUpdatesConflict() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    backend.setMode(QStringLiteral("theme"), WriteMode::Pending);
    settings::SettingsRuntimeSession session(registry, backend);

    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "the asynchronous write must start");
    backend.complete(QStringLiteral("theme"), 0, false);
    flushEvents();
    settings::SettingsFieldState failed = session.state(QStringLiteral("theme"));
    require(failed.phase == settings::SettingsWritePhase::Failed && failed.dirty &&
                !failed.busy && failed.draftValue == QStringLiteral("dark") &&
                failed.error == QStringLiteral("async rejection"),
            "an asynchronous rejection must retain a retryable draft");

    require(session.retry(QStringLiteral("theme")),
            "an asynchronously failed write must be retryable");
    backend.complete(QStringLiteral("theme"));
    flushEvents();
    require(session.state(QStringLiteral("theme")).phase ==
                    settings::SettingsWritePhase::Clean &&
                session.state(QStringLiteral("theme")).acceptedValue ==
                    QStringLiteral("dark"),
            "a successful asynchronous retry must settle cleanly");

    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("light")),
            "a second pending write must start");
    backend.setExternal(QStringLiteral("theme"), QStringLiteral("system"));
    flushEvents();
    settings::SettingsFieldState conflicted = session.state(QStringLiteral("theme"));
    require(conflicted.busy && conflicted.dirty && conflicted.conflicted &&
                conflicted.acceptedValue == QStringLiteral("system") &&
                conflicted.draftValue == QStringLiteral("light"),
            "an external update during a pending write must expose a conflict without losing the draft");
    backend.complete(QStringLiteral("theme"));
    flushEvents();
    require(!session.state(QStringLiteral("theme")).conflicted &&
                session.state(QStringLiteral("theme")).acceptedValue ==
                    QStringLiteral("light"),
            "the requested completion must resolve its temporary external conflict");
}

void submittingAcceptedBaselineSupersedesPendingWrite() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    backend.setMode(QStringLiteral("theme"), WriteMode::Pending);
    settings::SettingsRuntimeSession session(registry, backend);

    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "the first write must be pending");
    const quint64 pendingRevision = session.state(QStringLiteral("theme")).revision;
    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("system")),
            "submitting the accepted baseline must cancel the local pending draft");
    const settings::SettingsFieldState restored = session.state(QStringLiteral("theme"));
    require(!restored.dirty && !restored.busy &&
                restored.phase == settings::SettingsWritePhase::Clean &&
                restored.revision > pendingRevision &&
                backend.applyCount(QStringLiteral("theme")) == 1,
            "restoring the accepted baseline must advance the revision without another backend write");

    backend.complete(QStringLiteral("theme"));
    flushEvents();
    backend.notify();
    flushEvents();
    require(session.state(QStringLiteral("theme")).acceptedValue ==
                QStringLiteral("system"),
            "the superseded completion must not overwrite the restored baseline");
}

void deterministicDirtyOrderAndCustomValues() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);

    backend.setMode(QStringLiteral("delay"), WriteMode::Pending);
    backend.setMode(QStringLiteral("theme"), WriteMode::Pending);
    require(session.submitDraft(QStringLiteral("delay"), 8), "integer draft must be accepted");
    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "select draft must be accepted");
    require(session.dirtyFieldIds() ==
                QStringList{QStringLiteral("theme"), QStringLiteral("delay")},
            "dirty fields must follow catalog order even when submitted in reverse order");
    backend.complete(QStringLiteral("theme"));
    backend.complete(QStringLiteral("delay"));
    flushEvents();
    require(session.dirtyFieldIds().isEmpty(),
            "completed writes must leave no dirty fields");

    backend.setMode(QStringLiteral("tray-options"), WriteMode::Pending);
    const QVariantList options{QStringLiteral("quick.screenshot"), QStringLiteral("tray.exit")};
    require(session.applyMultiSelectValue(settings::SettingsMultiSelectBinding::TrayMenuOptions,
                                          options),
            "custom tray values must use the indexed custom descriptor");
    require(session.multiSelectValue(settings::SettingsMultiSelectBinding::TrayMenuOptions) ==
                options,
            "custom tray compatibility reads must expose the draft");
    require(session.dirtyFieldIds() == QStringList{QStringLiteral("tray-options")},
            "dirty fields must be reported in catalog order");
    backend.complete(QStringLiteral("tray-options"));
    flushEvents();
    require(!session.hasDirtyFields(), "custom tray completion must clear its dirty state");

    storage::ScreenshotToolbarLayout layout;
    layout.positions = {{QStringLiteral("brush")}};
    layout.hidden = {QStringLiteral("text")};
    require(session.applyToolbarLayout(layout), "custom toolbar values must be writable");
    require(session.toolbarLayout() == layout,
            "custom toolbar compatibility reads must expose the accepted value");
}

void acceptedResetClearsDraftAndQuarantinesLateCompletion() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    backend.setMode(QStringLiteral("theme"), WriteMode::Pending);
    settings::SettingsRuntimeSession session(registry, backend);

    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "reset fixture must start with a pending draft");
    require(session.reset(settings::SettingsSectionReset::None),
            "an accepted reset must be reported to the caller");
    const settings::SettingsFieldState resetState = session.state(QStringLiteral("theme"));
    require(!resetState.dirty && !resetState.busy &&
                resetState.phase == settings::SettingsWritePhase::Clean &&
                resetState.acceptedValue == QStringLiteral("system"),
            "an accepted reset must clear pending draft state to the backend value");

    backend.complete(QStringLiteral("theme"));
    flushEvents();
    const settings::SettingsFieldState afterCompletion = session.state(QStringLiteral("theme"));
    require(afterCompletion.acceptedValue == QStringLiteral("system") &&
                afterCompletion.draftValue == QStringLiteral("system") &&
                !afterCompletion.dirty && !afterCompletion.busy,
            "a late completion from before reset must remain quarantined");
}

void asynchronousResetTracksPendingStateAndLateUserEdits() {
    const settings::SettingsRegistry registry =
        testRegistry(settings::SettingsSectionReset::HistoryPolicy);
    FakeSettingsBackend backend;
    backend.setResetHistoryPending(true);
    settings::SettingsRuntimeSession session(registry, backend);

    require(session.reset(settings::SettingsSectionReset::HistoryPolicy),
            "an accepted asynchronous reset must be reported to the caller");
    const settings::SettingsFieldState pending = session.state(QStringLiteral("theme"));
    require(!pending.dirty && pending.busy &&
                pending.phase == settings::SettingsWritePhase::Pending,
            "an asynchronous reset must remain pending until its provider finishes");

    backend.setMode(QStringLiteral("theme"), WriteMode::Pending);
    require(session.submitDraft(QStringLiteral("theme"), QStringLiteral("dark")),
            "a user edit must supersede an asynchronous reset generation");
    const settings::SettingsFieldState edited = session.state(QStringLiteral("theme"));
    require(edited.dirty && edited.busy && edited.draftValue == QStringLiteral("dark"),
            "a user edit after reset must become the active field generation");

    backend.completeHistoryReset();
    flushEvents();
    const settings::SettingsFieldState afterResetCompletion =
        session.state(QStringLiteral("theme"));
    require(afterResetCompletion.dirty && afterResetCompletion.draftValue == QStringLiteral("dark") &&
                afterResetCompletion.busy,
            "a late reset completion must not overwrite a newer user edit");
}

void discardingAsynchronousResetQuarantinesLateCompletion() {
    const settings::SettingsRegistry registry =
        testRegistry(settings::SettingsSectionReset::HistoryPolicy);
    FakeSettingsBackend backend;
    backend.setResetHistoryPending(true);
    settings::SettingsRuntimeSession session(registry, backend);

    require(session.reset(settings::SettingsSectionReset::HistoryPolicy),
            "the reset must start before it can be discarded");
    const quint64 pendingRevision = session.state(QStringLiteral("theme")).revision;
    require(session.discard(QStringLiteral("theme")),
            "an asynchronous reset must be discardable while pending");
    const settings::SettingsFieldState discarded = session.state(QStringLiteral("theme"));
    require(!discarded.dirty && !discarded.busy &&
                discarded.phase == settings::SettingsWritePhase::Clean &&
                discarded.revision > pendingRevision,
            "discarding a reset must retire its active generation");

    backend.completeHistoryReset();
    flushEvents();
    const settings::SettingsFieldState afterCompletion =
        session.state(QStringLiteral("theme"));
    require(afterCompletion.acceptedValue == discarded.acceptedValue &&
                afterCompletion.draftValue == discarded.draftValue &&
                afterCompletion.dirty == discarded.dirty && !afterCompletion.busy &&
                afterCompletion.phase == discarded.phase &&
                afterCompletion.revision == discarded.revision && afterCompletion.enabled,
            "a late completion must leave the discarded generation inert and restore enablement");
}

void rejectedResetRetainsStateAndErrorUntilDiscarded() {
    const settings::SettingsRegistry registry = testRegistry();
    FakeSettingsBackend backend;
    backend.setResetAccepted(false);
    settings::SettingsRuntimeSession session(registry, backend);

    require(!session.reset(settings::SettingsSectionReset::None),
            "a rejected reset must report failure");
    const settings::SettingsFieldState rejected = session.state(QStringLiteral("theme"));
    require(!rejected.dirty && !rejected.busy &&
                rejected.phase == settings::SettingsWritePhase::Rejected &&
                rejected.error == QStringLiteral("reset rejected"),
            "a rejected reset must retain a visible error without fabricating a draft");

    backend.notify();
    flushEvents();
    require(session.state(QStringLiteral("theme")).phase ==
                settings::SettingsWritePhase::Rejected,
            "a repeated synchronization must not clear a rejected reset");
    require(session.discard(QStringLiteral("theme")),
            "a rejected reset state must be dismissible");
    require(session.state(QStringLiteral("theme")).phase == settings::SettingsWritePhase::Clean &&
                session.state(QStringLiteral("theme")).error.isEmpty(),
            "discard must clear a rejected reset state");
}

void auxiliaryIntegerValuesRemainReactiveWithoutSyntheticFields() {
    const settings::SettingsRegistry registry = registryWithoutStandaloneDelay();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    require(registry.fieldForInteger(settings::SettingsIntegerBinding::ScreenshotDelaySeconds) ==
                nullptr &&
                session.integerValue(settings::SettingsIntegerBinding::ScreenshotDelaySeconds) ==
                    3,
            "auxiliary runtime values must not require synthetic catalog fields");

    int changedValue = 0;
    int changeCount = 0;
    QObject::connect(
        &session, &settings::SettingsRuntimeSession::auxiliaryIntegerChanged, &session,
        [&changedValue, &changeCount](settings::SettingsIntegerBinding binding, int value) {
            if (binding == settings::SettingsIntegerBinding::ScreenshotDelaySeconds) {
                changedValue = value;
                ++changeCount;
            }
        });
    require(session.applyIntegerValue(settings::SettingsIntegerBinding::ScreenshotDelaySeconds, 5) &&
                session.integerValue(settings::SettingsIntegerBinding::ScreenshotDelaySeconds) ==
                    5 &&
                changedValue == 5 && changeCount == 1,
            "auxiliary integer writes must update session consumers immediately");

    backend.setExternal(QStringLiteral("delay"), 7);
    flushEvents();
    require(session.integerValue(settings::SettingsIntegerBinding::ScreenshotDelaySeconds) == 7 &&
                changedValue == 7 && changeCount == 2,
            "backend synchronization must refresh auxiliary integer consumers exactly once");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    initialStateAndNoOp();
    storageUsagePropagation();
    synchronousWriteAndFieldSignals();
    rejectedWriteRetainsDraftAndCanRetry();
    mutatedRejectedWriteDoesNotSelfHeal();
    pendingWriteDiscardAndCompletionShield();
    newerRevisionWinsOverStaleCompletion();
    conflictAndScopedErrorSnapshots();
    asynchronousFailureCanRetryAndExternalUpdatesConflict();
    submittingAcceptedBaselineSupersedesPendingWrite();
    deterministicDirtyOrderAndCustomValues();
    acceptedResetClearsDraftAndQuarantinesLateCompletion();
    asynchronousResetTracksPendingStateAndLateUserEdits();
    discardingAsynchronousResetQuarantinesLateCompletion();
    rejectedResetRetainsStateAndErrorUntilDiscarded();
    auxiliaryIntegerValuesRemainReactiveWithoutSyntheticFields();
    return 0;
}
