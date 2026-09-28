#ifndef SNOW_SHOT_TEXTTRANSLATIONPROTOCOL_H
#define SNOW_SHOT_TEXTTRANSLATIONPROTOCOL_H

#include "snow_shot/network/snowshotapiclient.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <optional>

namespace snow_shot::text_translation {
inline std::optional<QString> language(const QString& provider, const QString& code, bool target) {
    const QStringList codes{
        QStringLiteral("auto"),   QStringLiteral("ar"), QStringLiteral("de"),
        QStringLiteral("en"),     QStringLiteral("es"), QStringLiteral("fr"),
        QStringLiteral("it"),     QStringLiteral("ja"), QStringLiteral("pt"),
        QStringLiteral("ru"),     QStringLiteral("tr"), QStringLiteral("zh-Hans"),
        QStringLiteral("zh-Hant")};
    const auto index = codes.indexOf(code);
    if (index < 0 || (target && index == 0))
        return std::nullopt;
    if (provider == QStringLiteral("deepl")) {
        if (index == 0)
            return QString();
        if (code.startsWith(QStringLiteral("zh-")))
            return target ? (code == QStringLiteral("zh-Hant") ? QStringLiteral("ZH-HANT")
                                                               : QStringLiteral("ZH-HANS"))
                          : QStringLiteral("ZH");
        return code.toUpper();
    }
    if (provider == QStringLiteral("baidu")) {
        const QStringList mapped{
            QStringLiteral("auto"), QStringLiteral("ara"), QStringLiteral("de"),
            QStringLiteral("en"),   QStringLiteral("spa"), QStringLiteral("fra"),
            QStringLiteral("it"),   QStringLiteral("jp"),  QStringLiteral("pt"),
            QStringLiteral("ru"),   QStringLiteral("tr"),  QStringLiteral("zh"),
            QStringLiteral("cht")};
        return mapped[index];
    }
    if (code == QStringLiteral("zh-Hans"))
        return QStringLiteral("zh-CHS");
    if (code == QStringLiteral("zh-Hant"))
        return QStringLiteral("zh-CHT");
    return code;
}
inline QByteArray digest(const QString& value, QCryptographicHash::Algorithm algorithm) {
    return QCryptographicHash::hash(value.toUtf8(), algorithm).toHex();
}
inline QByteArray body(const TextTranslationConfiguration& config,
                       const SnowShotTranslationRequest& request, const QString& salt,
                       const QString& timestamp) {
    const auto source = language(config.provider, request.sourceLanguage, false);
    const auto target = language(config.provider, request.targetLanguage, true);
    if (!source || !target)
        return {};
    if (config.provider == QStringLiteral("deepl")) {
        QJsonObject object{{QStringLiteral("text"), QJsonArray{request.text}},
                           {QStringLiteral("target_lang"), *target}};
        if (!source->isEmpty())
            object.insert(QStringLiteral("source_lang"), *source);
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }
    QList<QPair<QString, QString>> fields{{QStringLiteral("q"), request.text},
                                          {QStringLiteral("from"), *source},
                                          {QStringLiteral("to"), *target},
                                          {QStringLiteral("salt"), salt}};
    if (config.provider == QStringLiteral("baidu")) {
        fields.append({QStringLiteral("appid"), config.applicationId});
        fields.append(
            {QStringLiteral("sign"),
             QString::fromLatin1(digest(config.applicationId + request.text + salt + config.apiKey,
                                        QCryptographicHash::Md5))});
    } else {
        // Count Unicode code points, not UTF-8 bytes or UTF-16 surrogate halves.
        const auto points = request.text.toStdU32String();
        const QString input = points.size() <= 20
                                  ? request.text
                                  : QString::fromUcs4(points.data(), 10) +
                                        QString::number(static_cast<qulonglong>(points.size())) +
                                        QString::fromUcs4(points.data() + points.size() - 10, 10);
        fields.append({QStringLiteral("appKey"), config.applicationId});
        fields.append({QStringLiteral("signType"), QStringLiteral("v3")});
        fields.append({QStringLiteral("curtime"), timestamp});
        fields.append({QStringLiteral("strict"), QStringLiteral("true")});
        fields.append({QStringLiteral("sign"),
                       QString::fromLatin1(
                           digest(config.applicationId + input + salt + timestamp + config.apiKey,
                                  QCryptographicHash::Sha256))});
    }
    QByteArray encoded;
    for (const auto& field : fields) {
        if (!encoded.isEmpty())
            encoded += '&';
        encoded +=
            QUrl::toPercentEncoding(field.first) + '=' + QUrl::toPercentEncoding(field.second);
    }
    return encoded;
}
struct Response {
    QString text;
    QString code;
    bool valid = false;
    bool throttled = false;
};
inline Response parse(const QString& provider, const QByteArray& bytes) {
    Response result;
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        return result;
    const auto object = document.object();
    const auto error =
        object.value(provider == QStringLiteral("youdao") ? QStringLiteral("errorCode")
                                                          : QStringLiteral("error_code"));
    if (!error.isUndefined()) {
        result.code = error.isString() ? error.toString() : QString::number(error.toInt(-1));
        if (result.code != QStringLiteral("0") &&
            !(provider == QStringLiteral("baidu") && result.code == QStringLiteral("52000"))) {
            result.throttled =
                (provider == QStringLiteral("baidu") && result.code == QStringLiteral("54003")) ||
                (provider == QStringLiteral("youdao") &&
                 (result.code == QStringLiteral("411") || result.code == QStringLiteral("412")));
            return result;
        }
        result.code.clear();
    }
    const auto entries =
        object.value(provider == QStringLiteral("deepl")   ? QStringLiteral("translations")
                     : provider == QStringLiteral("baidu") ? QStringLiteral("trans_result")
                                                           : QStringLiteral("translation"));
    if (!entries.isArray() || entries.toArray().isEmpty())
        return result;
    QStringList texts;
    for (const auto& entry : entries.toArray()) {
        const auto text = provider == QStringLiteral("youdao")
                              ? entry
                              : entry.toObject().value(provider == QStringLiteral("deepl")
                                                           ? QStringLiteral("text")
                                                           : QStringLiteral("dst"));
        if (!text.isString())
            return result;
        texts.append(text.toString());
    }
    result.text = texts.join(u'\n');
    result.valid = !result.text.trimmed().isEmpty();
    return result;
}
} // namespace snow_shot::text_translation
#endif
