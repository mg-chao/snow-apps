#ifndef SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSREGISTRY_H
#define SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSREGISTRY_H

#include "snow_shot/presentation/settings/settingscatalog.h"

#include <QHash>
#include <QJsonValue>
#include <QString>
#include <QVector>

#include <memory>

namespace snow_shot::presentation::settings {

enum class SettingsFieldKind {
    Select,
    Switch,
    Integer,
    MultiSelect,
    Slider,
    Color,
    Radio,
    FilePath,
    DirectoryPath,
    Text,
    ShortcutAction,
    LocalShortcut,
    Action,
    Custom,
};

struct SettingsFieldDescriptor {
    QString id;
    QString pageId;
    QString sectionId;
    QString providerId;
    QString configurationKey;
    SettingsSectionReset reset = SettingsSectionReset::None;
    SettingsFieldKind kind = SettingsFieldKind::Custom;
    const SettingsItemDefinition* definition = nullptr;
    QJsonValue defaultValue;
};

struct SettingsSectionPlan {
    QString id;
    int sectionIndex = -1;
    SettingsSectionReset reset = SettingsSectionReset::None;
    SettingsSectionItemLayout itemLayout = SettingsSectionItemLayout::VerticalList;
    QVector<int> fieldIndexes;
};

struct SettingsPagePlan {
    QString id;
    QString providerId;
    int pageIndex = -1;
    QVector<SettingsSectionPlan> sectionPlans;
    QVector<int> fieldIndexes;
};

struct SettingsPersistenceSpec {
    QString fieldId;
    QString key;
    QJsonValue defaultValue;
};

// Providers are intentionally small: they contribute an immutable catalog
// fragment, while the registry owns validation, indexes, and compiled plans.
// A plugin can implement this interface without depending on any widget code.
class SettingsProvider {
  public:
    virtual ~SettingsProvider() = default;
    [[nodiscard]] virtual QString id() const = 0;
    [[nodiscard]] virtual SettingsCatalog contribute() const = 0;
};

class BuiltInSettingsProvider final : public SettingsProvider {
  public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] SettingsCatalog contribute() const override;
};

class SettingsRegistry;

// Collects immutable provider contributions before they are compiled into a
// registry.  Contributions are copied when added, so a provider may be a
// short-lived plugin object and the resulting registry remains self-contained.
class SettingsRegistryBuilder final {
  public:
    SettingsRegistryBuilder() = default;

    SettingsRegistryBuilder& addProvider(const SettingsProvider& provider);
    SettingsRegistryBuilder& addCatalog(SettingsCatalog catalog,
                                         QString providerId = {});

    // An empty builder is deliberately invalid: the application must make an
    // explicit provider contribution before a registry can drive a settings
    // surface.
    [[nodiscard]] SettingsRegistry build() const;
    [[nodiscard]] QStringList validationErrors() const;

  private:
    struct Contribution {
        SettingsCatalog catalog;
        QString providerId;
    };
    QVector<Contribution> m_contributions;
    QStringList m_validationErrors;
};

class SettingsRegistry final {
  public:
    SettingsRegistry() = default;
    explicit SettingsRegistry(SettingsCatalog catalog, QString providerId = {});
    SettingsRegistry(const SettingsRegistry& other);
    SettingsRegistry& operator=(const SettingsRegistry& other);
    SettingsRegistry(SettingsRegistry&& other);
    SettingsRegistry& operator=(SettingsRegistry&& other);

    [[nodiscard]] const SettingsCatalog& catalog() const;
    [[nodiscard]] const QVector<SettingsFieldDescriptor>& fields() const;
    [[nodiscard]] const SettingsFieldDescriptor* field(const QString& fieldId) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForConfigurationKey(const QString& key) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForShortcut(GlobalShortcutAction action) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForSelect(SettingsSelectBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForSwitch(SettingsSwitchBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForInteger(SettingsIntegerBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForMultiSelect(SettingsMultiSelectBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForSlider(SettingsSliderBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForColor(SettingsColorBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForRadio(SettingsRadioBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForFilePath(SettingsFilePathBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForDirectoryPath(SettingsDirectoryPathBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForText(SettingsTextBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForLocalShortcut(SettingsLocalShortcutScope scope,
                          const QString& shortcutId) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForAction(SettingsActionBinding binding) const;
    [[nodiscard]] const SettingsFieldDescriptor*
    fieldForCustom(SettingsCustomRenderer renderer) const;
    [[nodiscard]] const QVector<SettingsPagePlan>& pagePlans() const;
    [[nodiscard]] const SettingsPagePlan* pagePlan(const QString& pageId) const;
    [[nodiscard]] const QVector<SettingsPersistenceSpec>& persistence() const;
    [[nodiscard]] const QVector<int>& fieldsForReset(SettingsSectionReset reset) const;
    [[nodiscard]] QStringList validationErrors() const;
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] const QStringList& providerIds() const;
    [[nodiscard]] QString providerIdForPage(const QString& pageId) const;

    // Expose the immutable navigation model through the registry so consumers
    // use one compiled source of settings metadata.
    [[nodiscard]] const QVector<SettingsPageDefinition>& pages() const;
    [[nodiscard]] const QVector<SettingsNavigationNode>& navigation() const;
    [[nodiscard]] const SettingsLocation& defaultLocation() const;

    [[nodiscard]] static SettingsRegistry fromCatalog(const SettingsCatalog& catalog,
                                                       QString providerId = {});
    [[nodiscard]] static SettingsRegistry fromProviders(
        const QVector<const SettingsProvider*>& providers);

  private:
    friend class SettingsRegistryBuilder;

    SettingsRegistry(SettingsCatalog catalog, QVector<QString> pageProviderIds,
                     QStringList providerValidationErrors);
    void compile(QString providerId);
    void compile(const QVector<QString>& pageProviderIds, QString fallbackProviderId);

    SettingsCatalog m_catalog;
    QVector<SettingsFieldDescriptor> m_fields;
    QVector<SettingsPagePlan> m_pagePlans;
    QVector<SettingsPersistenceSpec> m_persistence;
    QHash<QString, int> m_fieldIndexById;
    QHash<QString, int> m_fieldIndexByConfigurationKey;
    QHash<int, int> m_fieldIndexByShortcut;
    QHash<int, int> m_fieldIndexBySelect;
    QHash<int, int> m_fieldIndexBySwitch;
    QHash<int, int> m_fieldIndexByInteger;
    QHash<int, int> m_fieldIndexByMultiSelect;
    QHash<int, int> m_fieldIndexBySlider;
    QHash<int, int> m_fieldIndexByColor;
    QHash<int, int> m_fieldIndexByRadio;
    QHash<int, int> m_fieldIndexByFilePath;
    QHash<int, int> m_fieldIndexByDirectoryPath;
    QHash<int, int> m_fieldIndexByText;
    QHash<QString, int> m_fieldIndexByLocalShortcut;
    QHash<int, int> m_fieldIndexByAction;
    QHash<int, int> m_fieldIndexByCustom;
    QHash<QString, int> m_pagePlanIndexById;
    QHash<int, QVector<int>> m_fieldsByReset;
    QStringList m_providerValidationErrors;
    QStringList m_compilationValidationErrors;
    QStringList m_providerIds;
    QVector<QString> m_pageProviderIds;
};

[[nodiscard]] SettingsRegistry buildBuiltInSettingsRegistry();
[[nodiscard]] const SettingsRegistry& builtInSettingsRegistry();

} // namespace snow_shot::presentation::settings

#endif // SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSREGISTRY_H
