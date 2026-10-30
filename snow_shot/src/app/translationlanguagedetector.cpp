#include "snow_shot/translation/translationlanguagedetector.h"

#include "snow_shot/translation/translationservice.h"
#include "snow_language_detection.h"

#include <array>
#include <cmath>

namespace snow_shot::translation {
std::optional<DetectedTranslationLanguage> detectTranslationLanguage(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    SnowLanguageDetectionResult result{};
    if (snow_language_detect(reinterpret_cast<const uint8_t*>(utf8.constData()),
                             static_cast<size_t>(utf8.size()), &result) != 1) {
        return std::nullopt;
    }
    return DetectedTranslationLanguage{QString::fromLatin1(result.language, 3), result.confidence};
}

QString resolveTranslationTarget(const TranslationPreferences& preferences,
                                 const std::optional<DetectedTranslationLanguage>& detection) {
    if (preferences.secondaryTargetLanguage.isEmpty() ||
        preferences.secondaryTargetLanguage == preferences.targetLanguage || !detection ||
        !std::isfinite(detection->confidence) ||
        detection->confidence < preferences.languageDetectionConfidenceThreshold / 100.0) {
        return preferences.targetLanguage;
    }
    struct Mapping {
        QLatin1StringView target;
        QLatin1StringView detected;
    };
    static constexpr std::array<Mapping, 16> mappings{{
        {QLatin1StringView("ar"), QLatin1StringView("ara")},
        {QLatin1StringView("de"), QLatin1StringView("deu")},
        {QLatin1StringView("en"), QLatin1StringView("eng")},
        {QLatin1StringView("es"), QLatin1StringView("spa")},
        {QLatin1StringView("fr"), QLatin1StringView("fra")},
        {QLatin1StringView("hi"), QLatin1StringView("hin")},
        {QLatin1StringView("it"), QLatin1StringView("ita")},
        {QLatin1StringView("ja"), QLatin1StringView("jpn")},
        {QLatin1StringView("ko"), QLatin1StringView("kor")},
        {QLatin1StringView("pt"), QLatin1StringView("por")},
        {QLatin1StringView("ru"), QLatin1StringView("rus")},
        {QLatin1StringView("th"), QLatin1StringView("tha")},
        {QLatin1StringView("tr"), QLatin1StringView("tur")},
        {QLatin1StringView("vi"), QLatin1StringView("vie")},
        {QLatin1StringView("zh-Hans"), QLatin1StringView("cmn")},
        {QLatin1StringView("zh-Hant"), QLatin1StringView("cmn")},
    }};
    for (const auto& mapping : mappings) {
        if (preferences.targetLanguage == mapping.target &&
            detection->languageCode == mapping.detected) {
            return preferences.secondaryTargetLanguage;
        }
    }
    return preferences.targetLanguage;
}
} // namespace snow_shot::translation
