#include "snow_shot/presentation/settings/settingsregistry.h"

#include "snow_shot/storage/configurationschema.h"

#include <QCoreApplication>

#include <type_traits>
#include <utility>

namespace snow_shot::presentation::settings {
namespace {
SettingsFieldKind fieldKind(const SettingsItemPayload& payload) {
    return std::visit(
        [](const auto& value) -> SettingsFieldKind {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, SettingsSelectDefinition>) {
                return SettingsFieldKind::Select;
            } else if constexpr (std::is_same_v<Value, SettingsSwitchDefinition>) {
                return SettingsFieldKind::Switch;
            } else if constexpr (std::is_same_v<Value, SettingsIntegerDefinition>) {
                return SettingsFieldKind::Integer;
            } else if constexpr (std::is_same_v<Value, SettingsMultiSelectDefinition>) {
                return SettingsFieldKind::MultiSelect;
            } else if constexpr (std::is_same_v<Value, SettingsSliderDefinition>) {
                return SettingsFieldKind::Slider;
            } else if constexpr (std::is_same_v<Value, SettingsColorDefinition>) {
                return SettingsFieldKind::Color;
            } else if constexpr (std::is_same_v<Value, SettingsRadioDefinition>) {
                return SettingsFieldKind::Radio;
            } else if constexpr (std::is_same_v<Value, SettingsFilePathDefinition>) {
                return SettingsFieldKind::FilePath;
            } else if constexpr (std::is_same_v<Value, SettingsDirectoryPathDefinition>) {
                return SettingsFieldKind::DirectoryPath;
            } else if constexpr (std::is_same_v<Value, SettingsTextDefinition>) {
                return SettingsFieldKind::Text;
            } else if constexpr (std::is_same_v<Value, SettingsShortcutActionDefinition>) {
                return SettingsFieldKind::ShortcutAction;
            } else if constexpr (std::is_same_v<Value, SettingsLocalShortcutDefinition>) {
                return SettingsFieldKind::LocalShortcut;
            } else if constexpr (std::is_same_v<Value, SettingsActionDefinition>) {
                return SettingsFieldKind::Action;
            } else {
                return SettingsFieldKind::Custom;
            }
        },
        payload);
}

QString localShortcutKey(SettingsLocalShortcutScope scope, const QString& shortcutId) {
    return QString::number(static_cast<int>(scope)) + QLatin1Char('\x1f') + shortcutId;
}

QString providerName(const QString& providerId, int index) {
    const QString trimmed = providerId.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("provider-%1").arg(index) : trimmed;
}
} // namespace

QString BuiltInSettingsProvider::id() const {
    return QStringLiteral("built-in");
}

SettingsCatalog BuiltInSettingsProvider::contribute() const {
    return buildBuiltInSettingsCatalog();
}

SettingsRegistryBuilder& SettingsRegistryBuilder::addProvider(const SettingsProvider& provider) {
    QString providerId = provider.id().trimmed();
    return addCatalog(provider.contribute(), std::move(providerId));
}

SettingsRegistryBuilder& SettingsRegistryBuilder::addCatalog(SettingsCatalog catalog,
                                                               QString providerId) {
    const int index = m_contributions.size();
    const QString normalizedId = providerName(providerId, index);
    if (providerId.trimmed().isEmpty()) {
        m_validationErrors.push_back(QStringLiteral("settings provider id must not be empty"));
    }
    if (normalizedId.contains(QChar(0x1f))) {
        m_validationErrors.push_back(
            QStringLiteral("settings provider id contains the reserved settings index delimiter: %1")
                .arg(normalizedId));
    }
    for (const Contribution& contribution : m_contributions) {
        if (contribution.providerId == normalizedId) {
            m_validationErrors.push_back(
                QStringLiteral("duplicate settings provider id: %1").arg(normalizedId));
            break;
        }
    }
    m_contributions.push_back({std::move(catalog), normalizedId});
    return *this;
}

SettingsRegistry SettingsRegistryBuilder::build() const {
    QVector<SettingsPageDefinition> pages;
    QVector<SettingsNavigationNode> navigation;
    QVector<QString> pageProviderIds;
    SettingsLocation defaultLocation;
    bool hasDefault = false;

    for (const Contribution& contribution : m_contributions) {
        const SettingsCatalog& catalog = contribution.catalog;
        if (!hasDefault && !catalog.defaultLocation().isEmpty()) {
            defaultLocation = catalog.defaultLocation();
            hasDefault = true;
        }
        for (const SettingsPageDefinition& page : catalog.pages()) {
            pages.push_back(page);
            pageProviderIds.push_back(contribution.providerId);
        }
        for (const SettingsNavigationNode& node : catalog.navigation()) {
            navigation.push_back(node);
        }
    }

    QStringList validationErrors = m_validationErrors;
    if (m_contributions.isEmpty()) {
        validationErrors.push_back(
            QStringLiteral("settings registry requires at least one provider"));
    }
    return SettingsRegistry(SettingsCatalog(std::move(pages), std::move(navigation),
                                            std::move(defaultLocation)),
                            std::move(pageProviderIds), std::move(validationErrors));
}

QStringList SettingsRegistryBuilder::validationErrors() const {
    QStringList errors = m_validationErrors;
    if (m_contributions.isEmpty()) {
        errors.push_back(QStringLiteral("settings registry requires at least one provider"));
    }
    return errors;
}

SettingsRegistry::SettingsRegistry(SettingsCatalog catalog, QString providerId)
    : m_catalog(std::move(catalog)) {
    compile(std::move(providerId));
}

SettingsRegistry::SettingsRegistry(SettingsCatalog catalog, QVector<QString> pageProviderIds,
                                   QStringList providerValidationErrors)
    : m_catalog(std::move(catalog)),
      m_providerValidationErrors(std::move(providerValidationErrors)),
      m_pageProviderIds(std::move(pageProviderIds)) {
    compile(m_pageProviderIds, {});
}

SettingsRegistry::SettingsRegistry(const QVector<const SettingsProvider*>& providers) {
    *this = fromProviders(providers);
}

SettingsRegistry::SettingsRegistry(const SettingsRegistry& other)
    : m_catalog(other.m_catalog),
      m_providerValidationErrors(other.m_providerValidationErrors),
      m_compilationValidationErrors(other.m_compilationValidationErrors),
      m_pageProviderIds(other.m_pageProviderIds) {
    compile(m_pageProviderIds, {});
}

SettingsRegistry& SettingsRegistry::operator=(const SettingsRegistry& other) {
    if (this == &other) {
        return *this;
    }
    m_catalog = other.m_catalog;
    m_providerValidationErrors = other.m_providerValidationErrors;
    m_compilationValidationErrors = other.m_compilationValidationErrors;
    m_pageProviderIds = other.m_pageProviderIds;
    compile(m_pageProviderIds, {});
    return *this;
}

SettingsRegistry::SettingsRegistry(SettingsRegistry&& other)
    : m_catalog(std::move(other.m_catalog)),
      m_providerValidationErrors(std::move(other.m_providerValidationErrors)),
      m_compilationValidationErrors(std::move(other.m_compilationValidationErrors)),
      m_pageProviderIds(std::move(other.m_pageProviderIds)) {
    compile(m_pageProviderIds, {});
}

SettingsRegistry& SettingsRegistry::operator=(SettingsRegistry&& other) {
    if (this == &other) {
        return *this;
    }
    m_catalog = std::move(other.m_catalog);
    m_providerValidationErrors = std::move(other.m_providerValidationErrors);
    m_compilationValidationErrors = std::move(other.m_compilationValidationErrors);
    m_pageProviderIds = std::move(other.m_pageProviderIds);
    compile(m_pageProviderIds, {});
    return *this;
}

void SettingsRegistry::compile(QString providerId) {
    QVector<QString> pageProviderIds;
    pageProviderIds.fill(providerId, m_catalog.pages().size());
    compile(pageProviderIds, std::move(providerId));
}

void SettingsRegistry::compile(const QVector<QString>& pageProviderIds,
                               QString fallbackProviderId) {
    m_compilationValidationErrors.clear();
    m_fields.clear();
    m_pagePlans.clear();
    m_persistence.clear();
    m_fieldIndexById.clear();
    m_fieldIndexByConfigurationKey.clear();
    m_fieldIndexByShortcut.clear();
    m_fieldIndexBySelect.clear();
    m_fieldIndexBySwitch.clear();
    m_fieldIndexByInteger.clear();
    m_fieldIndexByMultiSelect.clear();
    m_fieldIndexBySlider.clear();
    m_fieldIndexByColor.clear();
    m_fieldIndexByRadio.clear();
    m_fieldIndexByFilePath.clear();
    m_fieldIndexByDirectoryPath.clear();
    m_fieldIndexByText.clear();
    m_fieldIndexByLocalShortcut.clear();
    m_fieldIndexByAction.clear();
    m_fieldIndexByCustom.clear();
    m_pagePlanIndexById.clear();
    m_fieldsByReset.clear();
    m_providerIds.clear();
    m_pageProviderIds = pageProviderIds;

    const auto rememberProvider = [this](const QString& id) {
        if (!id.isEmpty() && !m_providerIds.contains(id)) {
            m_providerIds.push_back(id);
        }
    };
    const auto duplicate = [this](const QString& kind, const QString& value) {
        m_compilationValidationErrors.push_back(
            QStringLiteral("duplicate %1: %2").arg(kind, value));
    };
    const auto insertStringIndex = [this, &duplicate](QHash<QString, int>& index,
                                                       const QString& key, int value,
                                                       const QString& kind) {
        if (key.isEmpty()) {
            return;
        }
        if (index.contains(key)) {
            duplicate(kind, key);
            return;
        }
        index.insert(key, value);
    };
    const auto insertIntegerIndex = [this, &duplicate](QHash<int, int>& index, int key, int value,
                                                        const QString& kind) {
        if (index.contains(key)) {
            duplicate(kind, QString::number(key));
            return;
        }
        index.insert(key, value);
    };

    int fieldIndex = 0;
    for (int pageIndex = 0; pageIndex < m_catalog.pages().size(); ++pageIndex) {
        const SettingsPageDefinition& page = m_catalog.pages().at(pageIndex);
        const QString providerId = pageIndex < m_pageProviderIds.size()
                                       ? m_pageProviderIds.at(pageIndex)
                                       : fallbackProviderId;
        rememberProvider(providerId);

        SettingsPagePlan plan;
        plan.id = page.id;
        plan.route = page.route;
        plan.providerId = providerId;
        plan.pageIndex = pageIndex;
        if (m_pagePlanIndexById.contains(plan.id)) {
            duplicate(QStringLiteral("settings page plan id"), plan.id);
        } else {
            m_pagePlanIndexById.insert(plan.id, m_pagePlans.size());
        }

        for (int sectionIndex = 0; sectionIndex < page.sections.size(); ++sectionIndex) {
            const SettingsSectionDefinition& section = page.sections.at(sectionIndex);
            SettingsSectionPlan sectionPlan;
            sectionPlan.id = section.id;
            sectionPlan.sectionIndex = sectionIndex;
            sectionPlan.reset = section.reset;
            sectionPlan.itemLayout = section.itemLayout;

            for (const SettingsItemDefinition& item : section.items) {
                SettingsFieldDescriptor descriptor;
                descriptor.id = item.id;
                descriptor.pageId = page.id;
                descriptor.sectionId = section.id;
                descriptor.providerId = providerId;
                descriptor.configurationKey = item.configurationKey;
                descriptor.reset = section.reset;
                descriptor.kind = fieldKind(item.payload);
                descriptor.definition = &item;
                if (!descriptor.configurationKey.isEmpty()) {
                    descriptor.defaultValue =
                        storage::ConfigurationSchema::defaultValue(descriptor.configurationKey);
                }

                const int currentIndex = fieldIndex;
                insertStringIndex(m_fieldIndexById, descriptor.id, currentIndex,
                                  QStringLiteral("settings field id"));
                insertStringIndex(m_fieldIndexByConfigurationKey, descriptor.configurationKey,
                                  currentIndex, QStringLiteral("settings persistence key"));

                std::visit(
                    [&](const auto& payload) {
                        using Payload = std::decay_t<decltype(payload)>;
                        if constexpr (std::is_same_v<Payload, SettingsSelectDefinition>) {
                            insertIntegerIndex(m_fieldIndexBySelect, static_cast<int>(payload.binding),
                                                currentIndex, QStringLiteral("select binding"));
                        } else if constexpr (std::is_same_v<Payload, SettingsSwitchDefinition>) {
                            insertIntegerIndex(m_fieldIndexBySwitch, static_cast<int>(payload.binding),
                                                currentIndex, QStringLiteral("switch binding"));
                        } else if constexpr (std::is_same_v<Payload, SettingsIntegerDefinition>) {
                            insertIntegerIndex(m_fieldIndexByInteger,
                                                static_cast<int>(payload.binding), currentIndex,
                                                QStringLiteral("integer binding"));
                        } else if constexpr (std::is_same_v<Payload,
                                                             SettingsMultiSelectDefinition>) {
                            insertIntegerIndex(m_fieldIndexByMultiSelect,
                                                static_cast<int>(payload.binding), currentIndex,
                                                QStringLiteral("multi-select binding"));
                        } else if constexpr (std::is_same_v<Payload, SettingsSliderDefinition>) {
                            insertIntegerIndex(m_fieldIndexBySlider, static_cast<int>(payload.binding),
                                                currentIndex, QStringLiteral("slider binding"));
                        } else if constexpr (std::is_same_v<Payload, SettingsColorDefinition>) {
                            insertIntegerIndex(m_fieldIndexByColor, static_cast<int>(payload.binding),
                                                currentIndex, QStringLiteral("color binding"));
                        } else if constexpr (std::is_same_v<Payload, SettingsRadioDefinition>) {
                            insertIntegerIndex(m_fieldIndexByRadio, static_cast<int>(payload.binding),
                                                currentIndex, QStringLiteral("radio binding"));
                        } else if constexpr (std::is_same_v<Payload, SettingsFilePathDefinition>) {
                            insertIntegerIndex(m_fieldIndexByFilePath,
                                                static_cast<int>(payload.binding), currentIndex,
                                                QStringLiteral("file path binding"));
                        } else if constexpr (std::is_same_v<Payload,
                                                             SettingsDirectoryPathDefinition>) {
                            insertIntegerIndex(m_fieldIndexByDirectoryPath,
                                                static_cast<int>(payload.binding), currentIndex,
                                                QStringLiteral("directory path binding"));
                        } else if constexpr (std::is_same_v<Payload, SettingsTextDefinition>) {
                            insertIntegerIndex(m_fieldIndexByText, static_cast<int>(payload.binding),
                                                currentIndex, QStringLiteral("text binding"));
                        } else if constexpr (std::is_same_v<Payload,
                                                             SettingsShortcutActionDefinition>) {
                            insertIntegerIndex(m_fieldIndexByShortcut,
                                                static_cast<int>(payload.shortcutAction), currentIndex,
                                                QStringLiteral("shortcut action"));
                        } else if constexpr (std::is_same_v<Payload,
                                                             SettingsLocalShortcutDefinition>) {
                            insertStringIndex(m_fieldIndexByLocalShortcut,
                                              localShortcutKey(payload.scope, payload.shortcutId),
                                              currentIndex, QStringLiteral("local shortcut"));
                        } else if constexpr (std::is_same_v<Payload, SettingsActionDefinition>) {
                            insertIntegerIndex(m_fieldIndexByAction, static_cast<int>(payload.binding),
                                                currentIndex, QStringLiteral("action binding"));
                        } else if constexpr (std::is_same_v<Payload, SettingsCustomDefinition>) {
                            insertIntegerIndex(m_fieldIndexByCustom,
                                                static_cast<int>(payload.renderer), currentIndex,
                                                QStringLiteral("custom renderer"));
                        }
                    },
                    item.payload);

                m_fieldsByReset[static_cast<int>(descriptor.reset)].push_back(currentIndex);
                if (!descriptor.configurationKey.isEmpty()) {
                    m_persistence.push_back(
                        {descriptor.id, descriptor.configurationKey, descriptor.defaultValue});
                }
                sectionPlan.fieldIndexes.push_back(currentIndex);
                plan.fieldIndexes.push_back(currentIndex);
                m_fields.push_back(std::move(descriptor));
                ++fieldIndex;
            }
            plan.sectionPlans.push_back(std::move(sectionPlan));
        }
        m_pagePlans.push_back(std::move(plan));
    }
}

const SettingsCatalog& SettingsRegistry::catalog() const {
    return m_catalog;
}

const QVector<SettingsFieldDescriptor>& SettingsRegistry::fields() const {
    return m_fields;
}

const SettingsFieldDescriptor* SettingsRegistry::field(const QString& fieldId) const {
    const auto found = m_fieldIndexById.constFind(fieldId);
    return found == m_fieldIndexById.cend() ? nullptr : &m_fields.at(found.value());
}

const SettingsFieldDescriptor*
SettingsRegistry::fieldForConfigurationKey(const QString& key) const {
    const auto found = m_fieldIndexByConfigurationKey.constFind(key);
    return found == m_fieldIndexByConfigurationKey.cend() ? nullptr
                                                           : &m_fields.at(found.value());
}

const SettingsFieldDescriptor*
SettingsRegistry::fieldForShortcut(GlobalShortcutAction action) const {
    const auto found = m_fieldIndexByShortcut.constFind(static_cast<int>(action));
    return found == m_fieldIndexByShortcut.cend() ? nullptr : &m_fields.at(found.value());
}

#define REGISTRY_INTEGER_LOOKUP(name, member, type)                                    \
    const SettingsFieldDescriptor* SettingsRegistry::name(type value) const {          \
        const auto found = member.constFind(static_cast<int>(value));                 \
        return found == member.cend() ? nullptr : &m_fields.at(found.value());         \
    }

REGISTRY_INTEGER_LOOKUP(fieldForSelect, m_fieldIndexBySelect, SettingsSelectBinding)
REGISTRY_INTEGER_LOOKUP(fieldForSwitch, m_fieldIndexBySwitch, SettingsSwitchBinding)
REGISTRY_INTEGER_LOOKUP(fieldForInteger, m_fieldIndexByInteger, SettingsIntegerBinding)
REGISTRY_INTEGER_LOOKUP(fieldForMultiSelect, m_fieldIndexByMultiSelect,
                        SettingsMultiSelectBinding)
REGISTRY_INTEGER_LOOKUP(fieldForSlider, m_fieldIndexBySlider, SettingsSliderBinding)
REGISTRY_INTEGER_LOOKUP(fieldForColor, m_fieldIndexByColor, SettingsColorBinding)
REGISTRY_INTEGER_LOOKUP(fieldForRadio, m_fieldIndexByRadio, SettingsRadioBinding)
REGISTRY_INTEGER_LOOKUP(fieldForFilePath, m_fieldIndexByFilePath, SettingsFilePathBinding)
REGISTRY_INTEGER_LOOKUP(fieldForDirectoryPath, m_fieldIndexByDirectoryPath,
                        SettingsDirectoryPathBinding)
REGISTRY_INTEGER_LOOKUP(fieldForText, m_fieldIndexByText, SettingsTextBinding)
REGISTRY_INTEGER_LOOKUP(fieldForAction, m_fieldIndexByAction, SettingsActionBinding)
REGISTRY_INTEGER_LOOKUP(fieldForCustom, m_fieldIndexByCustom, SettingsCustomRenderer)

#undef REGISTRY_INTEGER_LOOKUP

const SettingsFieldDescriptor* SettingsRegistry::fieldForLocalShortcut(
    SettingsLocalShortcutScope scope, const QString& shortcutId) const {
    const auto found = m_fieldIndexByLocalShortcut.constFind(localShortcutKey(scope, shortcutId));
    return found == m_fieldIndexByLocalShortcut.cend() ? nullptr : &m_fields.at(found.value());
}

const QVector<SettingsPagePlan>& SettingsRegistry::pagePlans() const {
    return m_pagePlans;
}

const SettingsPagePlan* SettingsRegistry::pagePlan(const QString& pageId) const {
    const auto found = m_pagePlanIndexById.constFind(pageId);
    return found == m_pagePlanIndexById.cend() ? nullptr : &m_pagePlans.at(found.value());
}

const QVector<SettingsPersistenceSpec>& SettingsRegistry::persistence() const {
    return m_persistence;
}

const QVector<int>& SettingsRegistry::fieldsForReset(SettingsSectionReset reset) const {
    static const QVector<int> empty;
    const auto found = m_fieldsByReset.constFind(static_cast<int>(reset));
    return found == m_fieldsByReset.cend() ? empty : found.value();
}

QStringList SettingsRegistry::validationErrors() const {
    QStringList errors = m_providerValidationErrors;
    errors.append(m_compilationValidationErrors);
    errors.append(m_catalog.validationErrors());
    return errors;
}

bool SettingsRegistry::isValid() const {
    return validationErrors().isEmpty();
}

const QStringList& SettingsRegistry::providerIds() const {
    return m_providerIds;
}

QString SettingsRegistry::providerIdForPage(const QString& pageId) const {
    const SettingsPagePlan* plan = pagePlan(pageId);
    if (plan == nullptr || plan->pageIndex < 0 ||
        plan->pageIndex >= m_pageProviderIds.size()) {
        return {};
    }
    return m_pageProviderIds.at(plan->pageIndex);
}

const QVector<SettingsPageDefinition>& SettingsRegistry::pages() const {
    return m_catalog.pages();
}

const QVector<SettingsNavigationNode>& SettingsRegistry::navigation() const {
    return m_catalog.navigation();
}

const SettingsLocation& SettingsRegistry::defaultLocation() const {
    return m_catalog.defaultLocation();
}

SettingsRegistry SettingsRegistry::fromCatalog(const SettingsCatalog& catalog,
                                                QString providerId) {
    return SettingsRegistry(SettingsCatalog(catalog.pages(), catalog.navigation(),
                                            catalog.defaultLocation()),
                            std::move(providerId));
}

SettingsRegistry SettingsRegistry::fromProviders(
    const QVector<const SettingsProvider*>& providers) {
    SettingsRegistryBuilder builder;
    QStringList nullProviderErrors;
    for (const SettingsProvider* provider : providers) {
        if (provider == nullptr) {
            nullProviderErrors.push_back(QStringLiteral("null settings provider"));
            continue;
        }
        builder.addProvider(*provider);
    }
    SettingsRegistry result = builder.build();
    for (auto it = nullProviderErrors.crbegin(); it != nullProviderErrors.crend(); ++it) {
        result.m_providerValidationErrors.prepend(*it);
    }
    return result;
}

SettingsRegistry buildBuiltInSettingsRegistry() {
    BuiltInSettingsProvider provider;
    return SettingsRegistry::fromProviders({&provider});
}

const SettingsRegistry& builtInSettingsRegistry() {
    static const SettingsRegistry registry = buildBuiltInSettingsRegistry();
    static const bool validated = []() {
        const QStringList errors = registry.validationErrors();
        if (!errors.isEmpty()) {
            qFatal("Invalid built-in settings registry:\n%s", qPrintable(errors.join(u'\n')));
        }
        return true;
    }();
    Q_UNUSED(validated)
    return registry;
}

} // namespace snow_shot::presentation::settings
