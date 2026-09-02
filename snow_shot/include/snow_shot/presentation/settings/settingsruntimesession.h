#ifndef SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSRUNTIMESESSION_H
#define SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSRUNTIMESESSION_H

#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsbackend.h"

#include <QHash>
#include <QSet>
#include <QVariant>

#include <optional>
#include <type_traits>

namespace snow_shot::presentation::settings {

enum class SettingsWritePhase {
    Clean,
    Pending,
    Failed,
    Rejected,
};

struct SettingsFieldState {
    QVariant acceptedValue;
    QVariant draftValue;
    bool dirty = false;
    bool enabled = true;
    bool visible = true;
    bool busy = false;
    bool conflicted = false;
    SettingsWritePhase phase = SettingsWritePhase::Clean;
    QString error;
    quint64 revision = 0;

    friend bool operator==(const SettingsFieldState& first,
                           const SettingsFieldState& second) {
        return first.acceptedValue == second.acceptedValue &&
               first.draftValue == second.draftValue && first.dirty == second.dirty &&
               first.enabled == second.enabled && first.visible == second.visible &&
               first.busy == second.busy && first.conflicted == second.conflicted &&
               first.phase == second.phase && first.error == second.error &&
               first.revision == second.revision;
    }
    friend bool operator!=(const SettingsFieldState& first,
                           const SettingsFieldState& second) {
        return !(first == second);
    }
};

struct SettingsOptions {
    QVector<SettingsRuntimeOption> values;
    bool loading = false;
    QString error;
};

struct SettingsCommandState {
    bool enabled = false;
    bool busy = false;
    QString error;
};

class SettingsRuntimeSession final : public QObject {
    Q_OBJECT

  public:
    SettingsRuntimeSession(const SettingsRegistry& registry,
                           SettingsBackend& backend,
                           QObject* parent = nullptr);
    ~SettingsRuntimeSession() override = default;

    [[nodiscard]] const SettingsRegistry& registry() const;
    [[nodiscard]] SettingsFieldState state(const QString& fieldId) const;
    [[nodiscard]] bool hasDirtyFields() const;
    [[nodiscard]] bool hasPendingWrites() const;
    [[nodiscard]] QStringList dirtyFieldIds() const;

    // Immediate reactive write API.  A rejected/failed write intentionally
    // keeps the attempted draft visible so the UI can offer retry or discard.
    bool submitDraft(const QString& fieldId, const QVariant& value);
    bool retry(const QString& fieldId);
    bool discard(const QString& fieldId);
    bool reset(SettingsSectionReset reset);
    void refreshField(const QString& fieldId);
    void refreshAll();

    // Typed convenience methods translate catalog bindings to the registry's
    // stable field IDs. Widgets never receive the backend and therefore cannot
    // bypass the session's draft, validation, or conflict state.
    [[nodiscard]] QVariant selectValue(SettingsSelectBinding binding) const;
    [[nodiscard]] QVector<SettingsRuntimeOption>
    dynamicSelectOptions(SettingsSelectBinding binding) const;
    [[nodiscard]] bool applySelectValue(SettingsSelectBinding binding,
                                        const QVariant& value);
    [[nodiscard]] bool switchValue(SettingsSwitchBinding binding) const;
    [[nodiscard]] bool switchEnabled(SettingsSwitchBinding binding) const;
    [[nodiscard]] bool applySwitchValue(SettingsSwitchBinding binding, bool value);
    [[nodiscard]] QVariantList
    multiSelectValue(SettingsMultiSelectBinding binding) const;
    [[nodiscard]] bool applyMultiSelectValue(SettingsMultiSelectBinding binding,
                                             const QVariantList& value);
    [[nodiscard]] int integerValue(SettingsIntegerBinding binding) const;
    [[nodiscard]] bool applyIntegerValue(SettingsIntegerBinding binding, int value);
    [[nodiscard]] int sliderValue(SettingsSliderBinding binding) const;
    [[nodiscard]] bool applySliderValue(SettingsSliderBinding binding, int value);
    [[nodiscard]] QColor colorValue(SettingsColorBinding binding) const;
    [[nodiscard]] bool applyColorValue(SettingsColorBinding binding,
                                       const QColor& value);
    [[nodiscard]] QVariant radioValue(SettingsRadioBinding binding) const;
    [[nodiscard]] bool applyRadioValue(SettingsRadioBinding binding,
                                       const QVariant& value);
    [[nodiscard]] QString filePathValue(SettingsFilePathBinding binding) const;
    [[nodiscard]] bool applyFilePathValue(SettingsFilePathBinding binding,
                                          const QString& value);
    [[nodiscard]] QString directoryPathValue(
        SettingsDirectoryPathBinding binding) const;
    [[nodiscard]] bool applyDirectoryPathValue(SettingsDirectoryPathBinding binding,
                                               const QString& value);
    [[nodiscard]] QString textValue(SettingsTextBinding binding) const;
    [[nodiscard]] bool applyTextValue(SettingsTextBinding binding,
                                      const QString& value);
    [[nodiscard]] storage::ScreenshotToolbarLayout toolbarLayout() const;
    [[nodiscard]] bool
    applyToolbarLayout(const storage::ScreenshotToolbarLayout& layout);
    [[nodiscard]] GlobalShortcutRegistrationState
    shortcutState(GlobalShortcutAction action) const;
    [[nodiscard]] GlobalShortcutValidationResult
    validateShortcut(const QString& shortcut) const;
    [[nodiscard]] bool applyShortcuts(GlobalShortcutAction action,
                                      const QStringList& shortcuts);
    [[nodiscard]] QStringList localShortcuts(SettingsLocalShortcutScope scope,
                                             const QString& shortcutId) const;
    [[nodiscard]] GlobalShortcutValidationResult
    validateLocalShortcut(SettingsLocalShortcutScope scope, const QString& shortcutId,
                          const QString& shortcut) const;
    [[nodiscard]] bool applyLocalShortcuts(SettingsLocalShortcutScope scope,
                                           const QString& shortcutId,
                                           const QStringList& shortcuts);
    [[nodiscard]] SettingsActionState
    actionState(SettingsActionBinding binding) const;
    [[nodiscard]] bool triggerAction(SettingsActionBinding binding);
    [[nodiscard]] storage::StorageStatus storageStatus() const;

  signals:
    void fieldChanged(const QString& fieldId,
                      const snow_shot::presentation::settings::SettingsFieldState& state);
    void optionsChanged(const QString& fieldId,
                        const snow_shot::presentation::settings::SettingsOptions& options);
    void commandStateChanged(
        snow_shot::presentation::settings::SettingsCommandKind commandKind,
        const snow_shot::presentation::settings::SettingsCommandState& state);
    void storageStateChanged(const snow_shot::storage::StorageStatus& status);
    void shortcutStateChanged(
        snow_shot::presentation::GlobalShortcutAction action,
        const snow_shot::presentation::GlobalShortcutRegistrationState& state);
    void auxiliaryIntegerChanged(
        snow_shot::presentation::settings::SettingsIntegerBinding binding, int value);
    void refreshed();

  private:
    struct PendingWrite {
        QVariant target;
        QVariant baseline;
        quint64 revision = 0;
        QString fieldError;
        QString configurationError;
        QString historyError;
    };

    struct RetiredWrite {
        QVariant target;
        quint64 revision = 0;
        bool completionObserved = false;
    };

    const SettingsFieldDescriptor* descriptorFor(const QString& fieldId) const;
    const SettingsFieldDescriptor* descriptorForSelect(SettingsSelectBinding binding) const;
    const SettingsFieldDescriptor* descriptorForSwitch(SettingsSwitchBinding binding) const;
    const SettingsFieldDescriptor* descriptorForInteger(SettingsIntegerBinding binding) const;
    const SettingsFieldDescriptor* descriptorForMulti(SettingsMultiSelectBinding binding) const;
    const SettingsFieldDescriptor* descriptorForSlider(SettingsSliderBinding binding) const;
    const SettingsFieldDescriptor* descriptorForColor(SettingsColorBinding binding) const;
    const SettingsFieldDescriptor* descriptorForRadio(SettingsRadioBinding binding) const;
    const SettingsFieldDescriptor* descriptorForFile(SettingsFilePathBinding binding) const;
    const SettingsFieldDescriptor* descriptorForDirectory(
        SettingsDirectoryPathBinding binding) const;
    const SettingsFieldDescriptor* descriptorForText(SettingsTextBinding binding) const;
    const SettingsFieldDescriptor* descriptorForShortcut(GlobalShortcutAction action) const;
    const SettingsFieldDescriptor* descriptorForLocal(SettingsLocalShortcutScope scope,
                                                      const QString& shortcutId) const;
    const SettingsFieldDescriptor* descriptorForAction(SettingsActionBinding binding) const;
    const SettingsFieldDescriptor* descriptorForCustom(SettingsCustomRenderer renderer) const;

    [[nodiscard]] QVariant readValue(const SettingsFieldDescriptor& descriptor) const;
    [[nodiscard]] bool writeValue(const SettingsFieldDescriptor& descriptor,
                                  const QVariant& value);
    [[nodiscard]] bool isPending(const SettingsFieldDescriptor& descriptor) const;
    [[nodiscard]] QString writeError(const SettingsFieldDescriptor& descriptor) const;
    [[nodiscard]] bool valuesEqual(const SettingsFieldDescriptor& descriptor,
                                   const QVariant& first, const QVariant& second) const;
    [[nodiscard]] bool isReadOnly(const SettingsFieldDescriptor& descriptor) const;
    bool submitDraftInternal(const QString& fieldId, const QVariant& value,
                             bool forceWrite);
    void refreshOptions(const SettingsFieldDescriptor& descriptor);
    void refreshCommandStates();
    [[nodiscard]] SettingsOptions buildOptions(const SettingsFieldDescriptor& descriptor) const;
    [[nodiscard]] QString backendError(const SettingsFieldDescriptor& descriptor) const;
    [[nodiscard]] bool operationErrorChanged(const SettingsFieldDescriptor& descriptor,
                                              const PendingWrite& operation) const;
    [[nodiscard]] bool matchesValue(const SettingsFieldDescriptor& descriptor,
                                    const QVariant& first, const QVariant& second) const;
    void retireWrite(const QString& fieldId, const PendingWrite& write);
    void forgetRetiredTarget(const SettingsFieldDescriptor& descriptor,
                             const QVariant& target);
    [[nodiscard]] bool suppressRetiredCompletion(
        const SettingsFieldDescriptor& descriptor, const QVariant& external,
        bool backendPending, const PendingWrite* activeWrite);
    void refreshField(const QString& fieldId, std::optional<quint64> expectedRevision);
    void refreshAuxiliaryInteger(SettingsIntegerBinding binding);
    void updateState(const QString& fieldId, const SettingsFieldState& next);

    const SettingsRegistry& m_registry;
    SettingsBackend& m_backend;
    QHash<QString, SettingsFieldState> m_states;
    QHash<QString, PendingWrite> m_pendingWrites;
    // Reset operations have no user draft, but asynchronous providers still
    // need revision/error tracking distinct from ordinary field writes.
    QHash<QString, PendingWrite> m_pendingResets;
    QHash<QString, QVector<RetiredWrite>> m_retiredWrites;
    mutable QHash<QString, SettingsOptions> m_optionsCache;
    QHash<int, SettingsCommandState> m_commandStateCache;
    QHash<int, int> m_auxiliaryIntegerValues;
    storage::StorageStatus m_lastStorageStatus;
    bool m_hasStorageStatus = false;
};

Q_DECLARE_METATYPE(SettingsWritePhase)
Q_DECLARE_METATYPE(SettingsFieldState)
Q_DECLARE_METATYPE(SettingsOptions)
Q_DECLARE_METATYPE(SettingsCommandState)

} // namespace snow_shot::presentation::settings

#endif // SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSRUNTIMESESSION_H
