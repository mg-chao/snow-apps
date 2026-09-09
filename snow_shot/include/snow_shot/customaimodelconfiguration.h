#ifndef SNOW_SHOT_CUSTOMAIMODELCONFIGURATION_H
#define SNOW_SHOT_CUSTOMAIMODELCONFIGURATION_H

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QUuid>
#include <QVector>

namespace snow_shot {
struct CustomAiModelConfiguration {
    QString id;
    QString name;
    QString baseUrl;
    QString apiKey;
    QString model;
    bool supportsVision = false;

    [[nodiscard]] QString selectionId() const {
        return QStringLiteral("custom:") + id;
    }
    friend bool operator==(const CustomAiModelConfiguration&,
                           const CustomAiModelConfiguration&) = default;
};
using CustomAiModels = QVector<CustomAiModelConfiguration>;

inline CustomAiModelConfiguration normalizeCustomAiModel(CustomAiModelConfiguration value) {
    value.id = value.id.trimmed();
    value.name = value.name.trimmed();
    value.baseUrl = value.baseUrl.trimmed();
    while (value.baseUrl.endsWith(u'/')) {
        value.baseUrl.chop(1);
    }
    value.apiKey = value.apiKey.trimmed();
    value.model = value.model.trimmed();
    return value;
}

enum class CustomAiModelUrlError { None, InvalidBaseUrl, FullEndpoint };
inline CustomAiModelUrlError customAiModelUrlError(const QString& value) {
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative() || url.host().isEmpty() ||
        (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
        return CustomAiModelUrlError::InvalidBaseUrl;
    }
    if (url.path().endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive)) {
        return CustomAiModelUrlError::FullEndpoint;
    }
    return CustomAiModelUrlError::None;
}

inline bool validCustomAiModel(const CustomAiModelConfiguration& value) {
    return !QUuid(value.id).isNull() &&
           QUuid(value.id).toString(QUuid::WithoutBraces) == value.id && !value.name.isEmpty() &&
           !value.model.isEmpty() && !value.apiKey.contains(u'\r') &&
           !value.apiKey.contains(u'\n') &&
           customAiModelUrlError(value.baseUrl) == CustomAiModelUrlError::None;
}

inline QJsonArray customAiModelsToJson(const CustomAiModels& models) {
    QJsonArray result;
    for (const auto& model : models) {
        result.append(QJsonObject{{QStringLiteral("id"), model.id},
                                  {QStringLiteral("name"), model.name},
                                  {QStringLiteral("base_url"), model.baseUrl},
                                  {QStringLiteral("api_key"), model.apiKey},
                                  {QStringLiteral("model"), model.model},
                                  {QStringLiteral("supports_vision"), model.supportsVision}});
    }
    return result;
}

inline QString customAiModelFingerprint(const CustomAiModelConfiguration& model) {
    const QJsonArray connection{model.id, model.baseUrl, model.apiKey, model.model,
                                model.supportsVision};
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(connection).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
}

// Loading salvages valid records; writers reject the whole collection if any record is invalid.
inline CustomAiModels customAiModelsFromJson(const QJsonValue& value, bool* valid = nullptr) {
    CustomAiModels result;
    bool allValid = value.isArray();
    QSet<QString> ids;
    QSet<QString> names;
    for (const auto& item : value.toArray()) {
        const auto object = item.toObject();
        bool typesValid = item.isObject();
        for (const auto* key : {"id", "name", "base_url", "api_key", "model"}) {
            typesValid = typesValid && object.value(QLatin1StringView(key)).isString();
        }
        typesValid = typesValid && object.value(QStringLiteral("supports_vision")).isBool();
        auto model =
            normalizeCustomAiModel({object.value(QStringLiteral("id")).toString(),
                                    object.value(QStringLiteral("name")).toString(),
                                    object.value(QStringLiteral("base_url")).toString(),
                                    object.value(QStringLiteral("api_key")).toString(),
                                    object.value(QStringLiteral("model")).toString(),
                                    object.value(QStringLiteral("supports_vision")).toBool()});
        if (!typesValid || !validCustomAiModel(model) || ids.contains(model.id) ||
            names.contains(model.name.toCaseFolded())) {
            allValid = false;
            continue;
        }
        ids.insert(model.id);
        names.insert(model.name.toCaseFolded());
        result.push_back(model);
    }
    if (valid != nullptr) {
        *valid = allValid;
    }
    return result;
}
} // namespace snow_shot

Q_DECLARE_METATYPE(snow_shot::CustomAiModels)
#endif
