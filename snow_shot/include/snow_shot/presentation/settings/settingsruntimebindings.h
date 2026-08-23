#ifndef SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSRUNTIMEBINDINGS_H
#define SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSRUNTIMEBINDINGS_H

#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QObject>
#include <QVariant>
#include <QVector>

namespace snow_shot::presentation::settings {

struct SettingsRuntimeOption {
    QVariant value;
    QString label;
};

struct SettingsActionState {
    bool enabled = false;
    bool busy = false;
};

class SettingsRuntimeBindings : public QObject {
    Q_OBJECT

  public:
    explicit SettingsRuntimeBindings(QObject* parent = nullptr) : QObject(parent) {}
    ~SettingsRuntimeBindings() override = default;

    [[nodiscard]] virtual QVariant selectValue(SettingsSelectBinding binding) const = 0;
    [[nodiscard]] QVector<SettingsRuntimeOption>
    virtual dynamicSelectOptions(SettingsSelectBinding binding) const = 0;
    [[nodiscard]] virtual bool applySelectValue(SettingsSelectBinding binding,
                                                const QVariant& value) = 0;

    [[nodiscard]] virtual bool switchValue(SettingsSwitchBinding binding) const = 0;
    [[nodiscard]] virtual bool switchEnabled(SettingsSwitchBinding binding) const {
        Q_UNUSED(binding);
        return true;
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

    [[nodiscard]] virtual QString directoryPathValue(
        SettingsDirectoryPathBinding binding) const = 0;
    [[nodiscard]] virtual bool applyDirectoryPathValue(SettingsDirectoryPathBinding binding,
                                                       const QString& value) = 0;

    [[nodiscard]] virtual QString textValue(SettingsTextBinding binding) const = 0;
    [[nodiscard]] virtual bool applyTextValue(SettingsTextBinding binding,
                                              const QString& value) = 0;

    [[nodiscard]] virtual storage::ScreenshotToolbarLayout toolbarLayout() const = 0;
    [[nodiscard]] virtual bool
    applyToolbarLayout(const storage::ScreenshotToolbarLayout& layout) = 0;

    [[nodiscard]] virtual GlobalShortcutRegistrationState
    shortcutState(GlobalShortcutAction action) const = 0;
    [[nodiscard]] virtual GlobalShortcutValidationResult
    validateShortcut(const QString& shortcut) const = 0;
    [[nodiscard]] virtual bool applyShortcuts(GlobalShortcutAction action,
                                              const QStringList& shortcuts) = 0;
    [[nodiscard]] virtual QStringList localShortcuts(SettingsLocalShortcutScope scope,
                                                     const QString& shortcutId) const = 0;
    [[nodiscard]] virtual GlobalShortcutValidationResult
    validateLocalShortcut(SettingsLocalShortcutScope scope, const QString& shortcutId,
                          const QString& shortcut) const = 0;
    [[nodiscard]] virtual bool applyLocalShortcuts(SettingsLocalShortcutScope scope,
                                                   const QString& shortcutId,
                                                   const QStringList& shortcuts) = 0;

    [[nodiscard]] virtual SettingsActionState
    actionState(SettingsActionBinding binding) const = 0;
    [[nodiscard]] virtual bool triggerAction(SettingsActionBinding binding) = 0;
    [[nodiscard]] virtual storage::StorageStatus storageStatus() const = 0;
    [[nodiscard]] virtual bool resetSection(SettingsSectionReset reset) = 0;

  signals:
    void synchronized();
    void shortcutStateChanged(
        snow_shot::presentation::GlobalShortcutAction action,
        const snow_shot::presentation::GlobalShortcutRegistrationState& state);

};

class BuiltInSettingsRuntimeBindings final : public SettingsRuntimeBindings {
  public:
    explicit BuiltInSettingsRuntimeBindings(GlobalShortcutManager& shortcutManager,
                                            QObject* parent = nullptr);

    [[nodiscard]] QVariant selectValue(SettingsSelectBinding binding) const override;
    [[nodiscard]] QVector<SettingsRuntimeOption>
    dynamicSelectOptions(SettingsSelectBinding binding) const override;
    [[nodiscard]] bool applySelectValue(SettingsSelectBinding binding,
                                        const QVariant& value) override;
    [[nodiscard]] bool switchValue(SettingsSwitchBinding binding) const override;
    [[nodiscard]] bool switchEnabled(SettingsSwitchBinding binding) const override;
    [[nodiscard]] bool applySwitchValue(SettingsSwitchBinding binding, bool value) override;
    [[nodiscard]] QVariantList
    multiSelectValue(SettingsMultiSelectBinding binding) const override;
    [[nodiscard]] bool applyMultiSelectValue(SettingsMultiSelectBinding binding,
                                             const QVariantList& value) override;
    [[nodiscard]] int integerValue(SettingsIntegerBinding binding) const override;
    [[nodiscard]] bool applyIntegerValue(SettingsIntegerBinding binding, int value) override;
    [[nodiscard]] int sliderValue(SettingsSliderBinding binding) const override;
    [[nodiscard]] bool applySliderValue(SettingsSliderBinding binding, int value) override;
    [[nodiscard]] QColor colorValue(SettingsColorBinding binding) const override;
    [[nodiscard]] bool applyColorValue(SettingsColorBinding binding,
                                       const QColor& value) override;
    [[nodiscard]] QVariant radioValue(SettingsRadioBinding binding) const override;
    [[nodiscard]] bool applyRadioValue(SettingsRadioBinding binding,
                                       const QVariant& value) override;
    [[nodiscard]] QString filePathValue(SettingsFilePathBinding binding) const override;
    [[nodiscard]] bool applyFilePathValue(SettingsFilePathBinding binding,
                                          const QString& value) override;
    [[nodiscard]] QString directoryPathValue(
        SettingsDirectoryPathBinding binding) const override;
    [[nodiscard]] bool applyDirectoryPathValue(SettingsDirectoryPathBinding binding,
                                               const QString& value) override;
    [[nodiscard]] QString textValue(SettingsTextBinding binding) const override;
    [[nodiscard]] bool applyTextValue(SettingsTextBinding binding,
                                      const QString& value) override;
    [[nodiscard]] storage::ScreenshotToolbarLayout toolbarLayout() const override;
    [[nodiscard]] bool
    applyToolbarLayout(const storage::ScreenshotToolbarLayout& layout) override;
    [[nodiscard]] GlobalShortcutRegistrationState
    shortcutState(GlobalShortcutAction action) const override;
    [[nodiscard]] GlobalShortcutValidationResult
    validateShortcut(const QString& shortcut) const override;
    [[nodiscard]] bool applyShortcuts(GlobalShortcutAction action,
                                      const QStringList& shortcuts) override;
    [[nodiscard]] QStringList localShortcuts(SettingsLocalShortcutScope scope,
                                             const QString& shortcutId) const override;
    [[nodiscard]] GlobalShortcutValidationResult
    validateLocalShortcut(SettingsLocalShortcutScope scope, const QString& shortcutId,
                          const QString& shortcut) const override;
    [[nodiscard]] bool applyLocalShortcuts(SettingsLocalShortcutScope scope,
                                           const QString& shortcutId,
                                           const QStringList& shortcuts) override;
    [[nodiscard]] SettingsActionState
    actionState(SettingsActionBinding binding) const override;
    [[nodiscard]] bool triggerAction(SettingsActionBinding binding) override;
    [[nodiscard]] storage::StorageStatus storageStatus() const override;
    [[nodiscard]] bool resetSection(SettingsSectionReset reset) override;

  private:
    GlobalShortcutManager& m_shortcutManager;
};

} // namespace snow_shot::presentation::settings

#endif // SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSRUNTIMEBINDINGS_H
