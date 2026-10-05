#ifndef SNOW_SHOT_CLOUDUPLOADCONFIGURATION_H
#define SNOW_SHOT_CLOUDUPLOADCONFIGURATION_H

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <QVector>
#include <algorithm>

namespace snow_shot {
struct CloudUploadConfiguration {
    QString id;
    QString name;
    QString protocol = QStringLiteral("s3");
    QString endpoint;
    QString region = QStringLiteral("us-east-1");
    QString bucket;
    QString accessKeyId;
    QString secretAccessKey;
    QString sessionToken;
    QString keyPrefix;
    QString addressingStyle = QStringLiteral("path");
    QString publicBaseUrl;
    friend bool operator==(const CloudUploadConfiguration&,
                           const CloudUploadConfiguration&) = default;
};
struct CloudUploadSettings {
    QVector<CloudUploadConfiguration> configurations;
    QString defaultId;
    friend bool operator==(const CloudUploadSettings&, const CloudUploadSettings&) = default;
    const CloudUploadConfiguration* selected() const {
        for (const auto& value : configurations)
            if (value.id == defaultId)
                return &value;
        return nullptr;
    }
};
inline bool validCloudUploadUrl(const QString& value) {
    const QUrl url(value, QUrl::StrictMode);
    return url.isValid() && !url.isRelative() && !url.host().isEmpty() &&
           (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https")) &&
           url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}
inline bool cloudUploadConfigurationValid(const CloudUploadConfiguration& value) {
    if (QUuid(value.id).isNull() || QUuid(value.id).toString(QUuid::WithoutBraces) != value.id ||
        value.name.trimmed().isEmpty() || value.protocol != QStringLiteral("s3") ||
        !validCloudUploadUrl(value.endpoint) || value.region.trimmed().isEmpty() ||
        value.bucket.trimmed().isEmpty() || value.bucket.contains(u'/') ||
        value.bucket == QStringLiteral(".") || value.bucket == QStringLiteral("..") ||
        (value.addressingStyle != QStringLiteral("path") &&
         value.addressingStyle != QStringLiteral("virtual")) ||
        (!value.publicBaseUrl.isEmpty() && !validCloudUploadUrl(value.publicBaseUrl)))
        return false;
    for (const auto& field : {value.region, value.bucket, value.accessKeyId, value.secretAccessKey,
                              value.sessionToken, value.keyPrefix})
        if (field.contains(u'\r') || field.contains(u'\n') || field.contains(QChar(0)))
            return false;
    if (value.region.contains(u'/') ||
        std::any_of(value.region.cbegin(), value.region.cend(),
                    [](QChar c) { return c.isSpace(); }) ||
        value.accessKeyId.contains(u'/') ||
        std::any_of(value.accessKeyId.cbegin(), value.accessKeyId.cend(),
                    [](QChar c) { return c.isSpace(); }))
        return false;
    if (value.addressingStyle == QStringLiteral("virtual")) {
        const auto host = QUrl(value.endpoint).host();
        if (host.contains(u':') || host.isEmpty())
            return false;
        for (const auto& label : value.bucket.split(u'.')) {
            if (label.isEmpty() || label.startsWith(u'-') || label.endsWith(u'-') ||
                label.size() > 63 || !std::all_of(label.cbegin(), label.cend(), [](QChar c) {
                    return (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9') || c == u'-';
                }))
                return false;
        }
    }
    return true;
}
inline bool cloudUploadConfigurationUsable(const CloudUploadConfiguration& value) {
    return cloudUploadConfigurationValid(value) && !value.accessKeyId.isEmpty() &&
           !value.secretAccessKey.isEmpty();
}
inline QJsonObject cloudUploadSettingsToJson(const CloudUploadSettings& settings) {
    QJsonArray values;
    for (const auto& value : settings.configurations)
        values.append(QJsonObject{{QStringLiteral("id"), value.id},
                                  {QStringLiteral("name"), value.name},
                                  {QStringLiteral("protocol"), value.protocol},
                                  {QStringLiteral("endpoint"), value.endpoint},
                                  {QStringLiteral("region"), value.region},
                                  {QStringLiteral("bucket"), value.bucket},
                                  {QStringLiteral("access_key_id"), value.accessKeyId},
                                  {QStringLiteral("secret_access_key"), value.secretAccessKey},
                                  {QStringLiteral("session_token"), value.sessionToken},
                                  {QStringLiteral("key_prefix"), value.keyPrefix},
                                  {QStringLiteral("addressing_style"), value.addressingStyle},
                                  {QStringLiteral("public_base_url"), value.publicBaseUrl}});
    return {{QStringLiteral("configurations"), values},
            {QStringLiteral("default_id"), settings.defaultId}};
}
// Preserve valid records and redacted credentials on load; reject invalid writes atomically.
inline CloudUploadSettings cloudUploadSettingsFromJson(const QJsonValue& json,
                                                       bool* valid = nullptr) {
    CloudUploadSettings result;
    const auto root = json.toObject();
    bool allValid = json.isObject() && root.value(QStringLiteral("configurations")).isArray() &&
                    root.value(QStringLiteral("default_id")).isString();
    QSet<QString> ids;
    QSet<QString> names;
    for (const auto& item : root.value(QStringLiteral("configurations")).toArray()) {
        const auto object = item.toObject();
        bool typesValid = item.isObject();
        for (const auto* key : {"id", "name", "protocol", "endpoint", "region", "bucket",
                                "access_key_id", "secret_access_key", "session_token", "key_prefix",
                                "addressing_style", "public_base_url"})
            typesValid = typesValid && object.value(QLatin1StringView(key)).isString();
        CloudUploadConfiguration value{
            object.value(QStringLiteral("id")).toString().trimmed(),
            object.value(QStringLiteral("name")).toString().trimmed(),
            object.value(QStringLiteral("protocol")).toString(),
            object.value(QStringLiteral("endpoint")).toString().trimmed(),
            object.value(QStringLiteral("region")).toString().trimmed(),
            object.value(QStringLiteral("bucket")).toString().trimmed(),
            object.value(QStringLiteral("access_key_id")).toString().trimmed(),
            object.value(QStringLiteral("secret_access_key")).toString(),
            object.value(QStringLiteral("session_token")).toString(),
            object.value(QStringLiteral("key_prefix")).toString(),
            object.value(QStringLiteral("addressing_style")).toString(),
            object.value(QStringLiteral("public_base_url")).toString().trimmed()};
        if (!typesValid || !cloudUploadConfigurationValid(value) || ids.contains(value.id) ||
            names.contains(value.name.toCaseFolded())) {
            allValid = false;
            continue;
        }
        ids.insert(value.id);
        names.insert(value.name.toCaseFolded());
        result.configurations.push_back(value);
    }
    result.defaultId = root.value(QStringLiteral("default_id")).toString();
    if (!result.defaultId.isEmpty() && !ids.contains(result.defaultId)) {
        allValid = false;
        result.defaultId.clear();
    }
    if (valid)
        *valid = allValid;
    return result;
}
} // namespace snow_shot
Q_DECLARE_METATYPE(snow_shot::CloudUploadSettings)
#endif
