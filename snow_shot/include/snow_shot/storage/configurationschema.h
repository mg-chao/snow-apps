#ifndef SNOW_SHOT_STORAGE_CONFIGURATIONSCHEMA_H
#define SNOW_SHOT_STORAGE_CONFIGURATIONSCHEMA_H

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace snow_shot::storage {
enum class ConfigurationValueKind {
    Boolean,
    Integer,
    String,
    StringList,
    ShortcutList,
    Structured,
};

struct ConfigurationIntegerRange {
    int minimum = 0;
    int maximum = 0;
    int step = 1;
};

struct ConfigurationSchemaEntry {
    QString key;
    QJsonValue defaultValue;
    ConfigurationValueKind valueKind = ConfigurationValueKind::Structured;
    std::optional<ConfigurationIntegerRange> integerRange = std::nullopt;
    QStringList allowedStringValues{};
    int maximumListItems = -1;
};

struct ConfigurationNormalization {
    QJsonValue value;
    bool valid = false;
    bool changed = false;
};

class ConfigurationSchema final {
  public:
    [[nodiscard]] static const QVector<ConfigurationSchemaEntry>& entries();
    [[nodiscard]] static const ConfigurationSchemaEntry* entry(const QString& key);
    [[nodiscard]] static bool contains(const QString& key);
    [[nodiscard]] static QJsonValue defaultValue(const QString& key);
    [[nodiscard]] static ConfigurationNormalization normalize(const QString& key,
                                                              const QJsonValue& value);
    [[nodiscard]] static QJsonObject completeDefaultDocument();
};
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_CONFIGURATIONSCHEMA_H
