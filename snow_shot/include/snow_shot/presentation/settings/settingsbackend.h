#ifndef SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSBACKEND_H
#define SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSBACKEND_H

#include "snow_shot/presentation/globalshortcuttypes.h"
#include "snow_shot/presentation/apppermissionservice.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_shot/platform/macos/loginitemservice.h"

#include <QObject>
#include <QVariant>
#include <QVector>
#include <future>

namespace snow_shot::presentation {
class GlobalShortcutManager;
class GlobalMouseManager;
namespace settings {

struct SettingsRuntimeOption {
    QVariant value;
    QString label{};

    friend bool operator==(const SettingsRuntimeOption& first,
                           const SettingsRuntimeOption& second) {
        return first.value == second.value && first.label == second.label;
    }
    friend bool operator!=(const SettingsRuntimeOption& first,
                           const SettingsRuntimeOption& second) {
        return !(first == second);
    }
};

struct SettingsActionState {
    bool enabled = false;
    bool busy = false;
    QString label{};
    QString hint{};
    bool successAccent = false;
};

class SettingsBackend : public QObject {
    Q_OBJECT

  public:
    explicit SettingsBackend(QObject* parent = nullptr) : QObject(parent) {}
    ~SettingsBackend() override = default;

    [[nodiscard]] virtual QVariant selectValue(SettingsSelectBinding binding) const = 0;
    [[nodiscard]] QVector<SettingsRuntimeOption> virtual dynamicSelectOptions(
        SettingsSelectBinding binding) const = 0;
    [[nodiscard]] virtual bool applySelectValue(SettingsSelectBinding binding,
                                                const QVariant& value) = 0;

    [[nodiscard]] virtual bool switchValue(SettingsSwitchBinding binding) const = 0;
    [[nodiscard]] virtual bool switchEnabled(SettingsSwitchBinding binding) const {
        Q_UNUSED(binding);
        return true;
    }
    virtual QString switchHint(SettingsSwitchBinding) const {
        return {};
    }
    [[nodiscard]] virtual bool applySwitchValue(SettingsSwitchBinding binding, bool value) = 0;

    [[nodiscard]] virtual QVariantList
    multiSelectValue(SettingsMultiSelectBinding binding) const = 0;
    [[nodiscard]] virtual bool applyMultiSelectValue(SettingsMultiSelectBinding binding,
                                                     const QVariantList& value) = 0;

    [[nodiscard]] virtual int integerValue(SettingsIntegerBinding binding) const = 0;
    [[nodiscard]] virtual bool applyIntegerValue(SettingsIntegerBinding binding, int value) = 0;

    [[nodiscard]] virtual int sliderValue(SettingsSliderBinding binding) const = 0;
    [[nodiscard]] virtual bool applySliderValue(SettingsSliderBinding binding, int value) = 0;

    [[nodiscard]] virtual QColor colorValue(SettingsColorBinding binding) const = 0;
    [[nodiscard]] virtual bool applyColorValue(SettingsColorBinding binding,
                                               const QColor& value) = 0;

    [[nodiscard]] virtual QVariant radioValue(SettingsRadioBinding binding) const = 0;
    [[nodiscard]] virtual bool applyRadioValue(SettingsRadioBinding binding,
                                               const QVariant& value) = 0;

    [[nodiscard]] virtual QString filePathValue(SettingsFilePathBinding binding) const = 0;
    [[nodiscard]] virtual bool applyFilePathValue(SettingsFilePathBinding binding,
                                                  const QString& value) = 0;

    [[nodiscard]] virtual QString
    directoryPathValue(SettingsDirectoryPathBinding binding) const = 0;
    [[nodiscard]] virtual bool applyDirectoryPathValue(SettingsDirectoryPathBinding binding,
                                                       const QString& value) = 0;

    [[nodiscard]] virtual QString textValue(SettingsTextBinding binding) const = 0;
    [[nodiscard]] virtual bool applyTextValue(SettingsTextBinding binding,
                                              const QString& value) = 0;

    [[nodiscard]] virtual storage::ScreenshotToolbarLayout
    toolbarLayout(storage::ScreenshotToolbarLayoutKind kind) const = 0;
    [[nodiscard]] virtual bool
    applyToolbarLayout(storage::ScreenshotToolbarLayoutKind kind,
                       const storage::ScreenshotToolbarLayout& layout) = 0;

    [[nodiscard]] virtual CustomAiModels customAiModels() const {
        return {};
    }
    virtual bool applyCustomAiModels(const CustomAiModels&) {
        return false;
    }
    virtual bool
    importConfigurationSnapshot(const QMap<QString, QJsonValue>&, int,
                                std::shared_future<storage::StorageResult>* completion = nullptr) {
        if (completion)
            *completion = {};
        return false;
    }

    [[nodiscard]] virtual GlobalShortcutRegistrationState
    shortcutState(GlobalShortcutAction action) const = 0;
    [[nodiscard]] virtual GlobalShortcutValidationResult
    validateShortcut(GlobalShortcutAction action,
                     const shortcuts::ShortcutBinding& shortcut) const = 0;
    [[nodiscard]] virtual bool applyShortcuts(GlobalShortcutAction action,
                                              const shortcuts::ShortcutBindingList& shortcuts) = 0;
    [[nodiscard]] virtual shortcuts::ShortcutBindingList
    localShortcuts(SettingsLocalShortcutScope scope, const QString& shortcutId) const = 0;
    [[nodiscard]] virtual GlobalShortcutValidationResult
    validateLocalShortcut(SettingsLocalShortcutScope scope, const QString& shortcutId,
                          const shortcuts::ShortcutBinding& shortcut) const = 0;
    [[nodiscard]] virtual bool
    applyLocalShortcuts(SettingsLocalShortcutScope scope, const QString& shortcutId,
                        const shortcuts::ShortcutBindingList& shortcuts) = 0;
    [[nodiscard]] virtual quint64 suspendGlobalShortcuts() {
        return 0;
    }
    virtual void resumeGlobalShortcuts(quint64) {}

    [[nodiscard]] virtual SettingsGlobalMouseCombination
    globalMouseCombination(SettingsGlobalMouseAction action) const {
        Q_UNUSED(action);
        return {};
    }
    [[nodiscard]] virtual bool
    applyGlobalMouseCombination(SettingsGlobalMouseAction action,
                                const SettingsGlobalMouseCombination& combination) {
        Q_UNUSED(action);
        Q_UNUSED(combination);
        return false;
    }

    virtual AppPermissionService* appPermissions() const {
        return nullptr;
    }

    virtual GlobalMousePermissionState globalMousePermissionState() const {
        return {GlobalMousePermissionState::Status::Ready, true, true, true};
    }
    virtual void requestGlobalMousePermission() {}
    virtual void openGlobalMousePermissionSettings() {}
    virtual void refreshGlobalMousePermission() {}

    [[nodiscard]] virtual SettingsActionState actionState(SettingsActionBinding binding) const = 0;
    [[nodiscard]] virtual bool triggerAction(SettingsActionBinding binding,
                                             const QString& filePath = {}) = 0;
    [[nodiscard]] virtual storage::StorageStatus storageStatus() const = 0;
    virtual void refreshPlatformSettings() {}
    virtual void refreshStorageStatus() {}
    // Show-event path; backends may throttle repeated refreshes.  Defaults to
    // the unthrottled refresh so simple backends only need that override.
    virtual void refreshStorageStatusIfStale() {
        refreshStorageStatus();
    }
    [[nodiscard]] virtual bool resetSection(SettingsSectionReset reset) = 0;
    [[nodiscard]] virtual QString fieldError(const QString& fieldId) const {
        Q_UNUSED(fieldId);
        return {};
    }
    [[nodiscard]] virtual bool fieldPending(const QString& fieldId) const {
        Q_UNUSED(fieldId);
        return false;
    }

  signals:
    void operationMessage(const QString& message, bool warning);
    void synchronized();
    void globalMousePermissionChanged();
    void actionFinished(snow_shot::presentation::settings::SettingsActionBinding action,
                        bool success, const QString& error);
    void
    shortcutStateChanged(snow_shot::presentation::GlobalShortcutAction action,
                         const snow_shot::presentation::GlobalShortcutRegistrationState& state);
};

class BuiltInSettingsBackend final : public SettingsBackend {
  public:
    explicit BuiltInSettingsBackend(
        ::snow_shot::presentation::GlobalShortcutManager& shortcutManager,
        QObject* parent = nullptr, GlobalMouseManager* mouseManager = nullptr,
        AppPermissionService* permissions = nullptr,
        platform::macos::LoginItemService* loginItems = nullptr);
    AppPermissionService* appPermissions() const override {
        return m_permissions;
    }

    GlobalMousePermissionState globalMousePermissionState() const override;
    void requestGlobalMousePermission() override;
    void openGlobalMousePermissionSettings() override;
    void refreshGlobalMousePermission() override;

    [[nodiscard]] QVariant selectValue(SettingsSelectBinding binding) const override;
    [[nodiscard]] QVector<SettingsRuntimeOption>
    dynamicSelectOptions(SettingsSelectBinding binding) const override;
    [[nodiscard]] bool applySelectValue(SettingsSelectBinding binding,
                                        const QVariant& value) override;
    [[nodiscard]] bool switchValue(SettingsSwitchBinding binding) const override;
    [[nodiscard]] bool switchEnabled(SettingsSwitchBinding binding) const override;
    QString switchHint(SettingsSwitchBinding binding) const override;
    bool fieldPending(const QString& fieldId) const override;
    [[nodiscard]] bool applySwitchValue(SettingsSwitchBinding binding, bool value) override;
    [[nodiscard]] QVariantList multiSelectValue(SettingsMultiSelectBinding binding) const override;
    [[nodiscard]] bool applyMultiSelectValue(SettingsMultiSelectBinding binding,
                                             const QVariantList& value) override;
    [[nodiscard]] int integerValue(SettingsIntegerBinding binding) const override;
    [[nodiscard]] bool applyIntegerValue(SettingsIntegerBinding binding, int value) override;
    [[nodiscard]] int sliderValue(SettingsSliderBinding binding) const override;
    [[nodiscard]] bool applySliderValue(SettingsSliderBinding binding, int value) override;
    [[nodiscard]] QColor colorValue(SettingsColorBinding binding) const override;
    [[nodiscard]] bool applyColorValue(SettingsColorBinding binding, const QColor& value) override;
    [[nodiscard]] QVariant radioValue(SettingsRadioBinding binding) const override;
    [[nodiscard]] bool applyRadioValue(SettingsRadioBinding binding,
                                       const QVariant& value) override;
    [[nodiscard]] QString filePathValue(SettingsFilePathBinding binding) const override;
    [[nodiscard]] bool applyFilePathValue(SettingsFilePathBinding binding,
                                          const QString& value) override;
    [[nodiscard]] QString directoryPathValue(SettingsDirectoryPathBinding binding) const override;
    [[nodiscard]] bool applyDirectoryPathValue(SettingsDirectoryPathBinding binding,
                                               const QString& value) override;
    [[nodiscard]] QString textValue(SettingsTextBinding binding) const override;
    [[nodiscard]] bool applyTextValue(SettingsTextBinding binding, const QString& value) override;
    [[nodiscard]] storage::ScreenshotToolbarLayout
    toolbarLayout(storage::ScreenshotToolbarLayoutKind kind) const override;
    [[nodiscard]] bool applyToolbarLayout(storage::ScreenshotToolbarLayoutKind kind,
                                          const storage::ScreenshotToolbarLayout& layout) override;
    [[nodiscard]] GlobalShortcutRegistrationState
    shortcutState(GlobalShortcutAction action) const override;
    [[nodiscard]] GlobalShortcutValidationResult
    validateShortcut(GlobalShortcutAction action,
                     const shortcuts::ShortcutBinding& shortcut) const override;
    [[nodiscard]] bool applyShortcuts(GlobalShortcutAction action,
                                      const shortcuts::ShortcutBindingList& shortcuts) override;
    [[nodiscard]] shortcuts::ShortcutBindingList
    localShortcuts(SettingsLocalShortcutScope scope, const QString& shortcutId) const override;
    [[nodiscard]] GlobalShortcutValidationResult
    validateLocalShortcut(SettingsLocalShortcutScope scope, const QString& shortcutId,
                          const shortcuts::ShortcutBinding& shortcut) const override;
    [[nodiscard]] bool
    applyLocalShortcuts(SettingsLocalShortcutScope scope, const QString& shortcutId,
                        const shortcuts::ShortcutBindingList& shortcuts) override;
    [[nodiscard]] quint64 suspendGlobalShortcuts() override;
    void resumeGlobalShortcuts(quint64 handle) override;
    [[nodiscard]] SettingsGlobalMouseCombination
    globalMouseCombination(SettingsGlobalMouseAction action) const override;
    [[nodiscard]] bool
    applyGlobalMouseCombination(SettingsGlobalMouseAction action,
                                const SettingsGlobalMouseCombination& combination) override;
    [[nodiscard]] SettingsActionState actionState(SettingsActionBinding binding) const override;
    [[nodiscard]] bool triggerAction(SettingsActionBinding binding,
                                     const QString& filePath = {}) override;
    [[nodiscard]] CustomAiModels customAiModels() const override;
    bool applyCustomAiModels(const CustomAiModels& models) override;
    bool importConfigurationSnapshot(
        const QMap<QString, QJsonValue>& values, int schemaVersion,
        std::shared_future<storage::StorageResult>* completion = nullptr) override;
    [[nodiscard]] storage::StorageStatus storageStatus() const override;
    void refreshPlatformSettings() override;
    void refreshStorageStatus() override;
    void refreshStorageStatusIfStale() override;
    [[nodiscard]] bool resetSection(SettingsSectionReset reset) override;

  private:
    ::snow_shot::presentation::GlobalShortcutManager& m_shortcutManager;
    AppPermissionService* m_permissions = nullptr;
    GlobalMouseManager* m_mouseManager = nullptr;
    platform::macos::LoginItemService* m_loginItems = nullptr;
    bool m_copyLogBusy = false;
    bool m_configurationBusy = false;
};

} // namespace settings
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSBACKEND_H
