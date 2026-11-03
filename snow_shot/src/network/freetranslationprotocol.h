#ifndef SNOW_SHOT_NETWORK_FREETRANSLATIONPROTOCOL_H
#define SNOW_SHOT_NETWORK_FREETRANSLATIONPROTOCOL_H

#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/translation/translationlanguagecatalog.h"

#include <QJsonArray>
#include <QJsonObject>

namespace snow_shot::network {
inline const QString freeTranslationModel = QStringLiteral("Index-Translate-35B-A3B");

// Protocol text stays independent of the UI locale. This is the single-user prompt contract
// from Index-Translate's inference/llm/call_api.py, with distinct Chinese script targets.
inline QString freeTranslationLanguage(const QString& code) {
    for (const auto& language : translation::translationLanguages()) {
        if (code == QLatin1StringView(language.code))
            return QString::fromLatin1(language.name);
    }
    return code;
}

inline QJsonObject freeTranslationBody(const SnowShotTranslationRequest& input) {
    const QString source = input.sourceLanguage.isEmpty() || input.sourceLanguage == u"auto"
                               ? QString()
                               : freeTranslationLanguage(input.sourceLanguage) + u' ';
    const QString prompt =
        QStringLiteral("Translate the following %1text into %2. Output only the translation, "
                       "without any explanation. Preserve the original formatting.\n\n%3")
            .arg(source, freeTranslationLanguage(input.targetLanguage), input.text);
    return {{QStringLiteral("model"), freeTranslationModel},
            {QStringLiteral("messages"),
             QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("content"), prompt}}}},
            {QStringLiteral("temperature"), 0},
            {QStringLiteral("max_tokens"), 4096},
            {QStringLiteral("stream"), true},
            {QStringLiteral("chat_template_kwargs"),
             QJsonObject{{QStringLiteral("enable_thinking"), false}}}};
}
} // namespace snow_shot::network

#endif
