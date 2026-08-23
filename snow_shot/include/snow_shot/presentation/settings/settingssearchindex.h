#ifndef SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSSEARCHINDEX_H
#define SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSSEARCHINDEX_H

#include "snow_shot/presentation/settings/settingscatalog.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace snow_shot::presentation::settings {

enum class SettingsSearchNodeKind {
    Page,
    Section,
    Item,
};

struct SettingsSearchEntry {
    QString id;
    SettingsSearchNodeKind kind = SettingsSearchNodeKind::Item;
    SettingsLocation location;
    QString title;
    QString description;
    QString path;
    QStringList aliases;
    QStringList optionLabels;
    int catalogOrder = 0;
};

struct SettingsSearchRuntimeValues {
    int screenshotDelaySeconds = 3;
};

class SettingsSearchIndex final {
  public:
    explicit SettingsSearchIndex(
        const SettingsCatalog& catalog,
        SettingsSearchRuntimeValues runtimeValues = {});

    void rebuild();
    void setRuntimeValues(SettingsSearchRuntimeValues runtimeValues);
    [[nodiscard]] const QVector<SettingsSearchEntry>& entries() const;
    [[nodiscard]] const QVector<SettingsSearchEntry>& pageEntries() const;
    [[nodiscard]] QVector<SettingsSearchEntry> search(const QString& query) const;

  public:
    struct NormalizedFields {
        QString title;
        QString description;
        QString path;
        QStringList aliases;
        QStringList optionLabels;
    };

  private:
    void rebuildPageEntries();
    void buildDetailedIndex() const;
    void ensureDetailedIndex() const;

    const SettingsCatalog& m_catalog;
    SettingsSearchRuntimeValues m_runtimeValues;
    QVector<SettingsSearchEntry> m_pageEntries;
    mutable QVector<SettingsSearchEntry> m_entries;
    mutable QVector<NormalizedFields> m_normalizedEntries;
    mutable bool m_detailedIndexBuilt = false;
};

} // namespace snow_shot::presentation::settings

#endif // SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSSEARCHINDEX_H
