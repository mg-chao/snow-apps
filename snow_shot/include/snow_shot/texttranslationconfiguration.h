#ifndef SNOW_SHOT_TEXTTRANSLATIONCONFIGURATION_H
#define SNOW_SHOT_TEXTTRANSLATIONCONFIGURATION_H

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <QVector>

namespace snow_shot {
struct TextTranslationConfiguration {
    QString id;
    QString name;
    QString provider = QStringLiteral("deepl");
    QString endpoint;
    QString apiKey;
    QString applicationId;
    int concurrency = 4;

    QString selectionId() const {
        return QStringLiteral("translation:") + id;
    }
    friend bool operator==(const TextTranslationConfiguration&,
                           const TextTranslationConfiguration&) = default;
};
using TextTranslationConfigurations = QVector<TextTranslationConfiguration>;

inline bool validTranslationEndpoint(const QString& endpoint) {
    const QUrl url(endpoint, QUrl::StrictMode);
    return url.isValid() && !url.isRelative() && !url.host().isEmpty() &&
           (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http")) &&
           url.userInfo().isEmpty() && !url.hasFragment();
}

inline QJsonArray textTranslationConfigurationsToJson(const TextTranslationConfigurations& values) {
    QJsonArray result;
    for (const auto& value : values) {
        result.append(QJsonObject{{QStringLiteral("id"), value.id},
                                  {QStringLiteral("name"), value.name},
                                  {QStringLiteral("provider"), value.provider},
                                  {QStringLiteral("endpoint"), value.endpoint},
                                  {QStringLiteral("api_key"), value.apiKey},
                                  {QStringLiteral("application_id"), value.applicationId},
                                  {QStringLiteral("concurrency"), value.concurrency}});
    }
    return result;
}

// Salvage valid records on load; callers performing writes must check valid.
inline TextTranslationConfigurations textTranslationConfigurationsFromJson(const QJsonValue& json,
                                                                           bool* valid = nullptr) {
    TextTranslationConfigurations result;
    bool allValid = json.isArray();
    QSet<QString> ids;
    QSet<QString> names;
    for (const auto& item : json.toArray()) {
        const auto object = item.toObject();
        bool typesValid = item.isObject();
        for (const auto* key : {"id", "name", "provider", "endpoint", "api_key", "application_id"})
            typesValid = typesValid && object.value(QLatin1StringView(key)).isString();
        const auto limit = object.value(QStringLiteral("concurrency"));
        const int concurrency = limit.isUndefined() ? 4 : limit.toInt(-1);
        typesValid = typesValid &&
                     (limit.isUndefined() || (limit.isDouble() && limit.toDouble() == concurrency));
        TextTranslationConfiguration value{
            object.value(QStringLiteral("id")).toString().trimmed(),
            object.value(QStringLiteral("name")).toString().trimmed(),
            object.value(QStringLiteral("provider")).toString(),
            object.value(QStringLiteral("endpoint")).toString().trimmed(),
            object.value(QStringLiteral("api_key")).toString().trimmed(),
            object.value(QStringLiteral("application_id")).toString().trimmed(),
            concurrency};
        const bool providerValid = value.provider == QStringLiteral("deepl") ||
                                   value.provider == QStringLiteral("baidu") ||
                                   value.provider == QStringLiteral("youdao");
        if (!typesValid || !providerValid || QUuid(value.id).isNull() ||
            QUuid(value.id).toString(QUuid::WithoutBraces) != value.id || value.name.isEmpty() ||
            !validTranslationEndpoint(value.endpoint) || concurrency < 1 || concurrency > 16 ||
            value.apiKey.contains(u'\r') || value.apiKey.contains(u'\n') ||
            ids.contains(value.id) || names.contains(value.name.toCaseFolded())) {
            allValid = false;
            continue;
        }
        // Empty credentials are retained for redacted archives and unauthenticated gateways.
        ids.insert(value.id);
        names.insert(value.name.toCaseFolded());
        result.append(value);
    }
    if (valid != nullptr)
        *valid = allValid;
    return result;
}
} // namespace snow_shot
Q_DECLARE_METATYPE(snow_shot::TextTranslationConfigurations)
#endif
