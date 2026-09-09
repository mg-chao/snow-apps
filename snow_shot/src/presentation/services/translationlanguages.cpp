#include "snow_shot/presentation/translationlanguages.h"

#include <QCoreApplication>

namespace snow_shot::presentation {
const QVector<TranslationLanguage>& translationLanguages() {
    static const QVector<TranslationLanguage> languages{
        {"ar", QT_TRANSLATE_NOOP("TranslationLanguages", "Arabic")},
        {"de", QT_TRANSLATE_NOOP("TranslationLanguages", "German")},
        {"en", QT_TRANSLATE_NOOP("TranslationLanguages", "English")},
        {"es", QT_TRANSLATE_NOOP("TranslationLanguages", "Spanish")},
        {"fr", QT_TRANSLATE_NOOP("TranslationLanguages", "French")},
        {"it", QT_TRANSLATE_NOOP("TranslationLanguages", "Italian")},
        {"ja", QT_TRANSLATE_NOOP("TranslationLanguages", "Japanese")},
        {"pt", QT_TRANSLATE_NOOP("TranslationLanguages", "Portuguese")},
        {"ru", QT_TRANSLATE_NOOP("TranslationLanguages", "Russian")},
        {"tr", QT_TRANSLATE_NOOP("TranslationLanguages", "Turkish")},
        {"zh-Hans", QT_TRANSLATE_NOOP("TranslationLanguages", "Simplified Chinese")},
        {"zh-Hant", QT_TRANSLATE_NOOP("TranslationLanguages", "Traditional Chinese")},
    };
    return languages;
}

QString translationLanguageName(const QString& code) {
    if (code == QStringLiteral("auto")) {
        return QCoreApplication::translate("TranslationLanguages", "Auto-detect language");
    }
    for (const auto& language : translationLanguages()) {
        if (code == QLatin1StringView(language.code)) {
            return QCoreApplication::translate("TranslationLanguages", language.name);
        }
    }
    return code;
}

QString defaultTranslationTargetLanguage(const QLocale& locale) {
    if (locale.language() == QLocale::Chinese) {
        return locale.script() == QLocale::TraditionalHanScript ? QStringLiteral("zh-Hant")
                                                                : QStringLiteral("zh-Hans");
    }
    const QString code = locale.name().section(u'_', 0, 0);
    for (const auto& language : translationLanguages()) {
        if (code == QLatin1StringView(language.code)) {
            return code;
        }
    }
    return QStringLiteral("en");
}

int translationModelIndex(const QVector<SnowShotChatModel>& models, const QString& preferredId) {
    int first = -1;
    int general = -1;
    for (int index = 0; index < models.size(); ++index) {
        const auto& model = models.at(index);
        if (!model.supportsTranslation()) {
            continue;
        }
        if (model.id == preferredId) {
            return index;
        }
        if (first < 0) {
            first = index;
        }
        if (general < 0 && model.translationMode == QStringLiteral("default")) {
            general = index;
        }
    }
    return general >= 0 ? general : first;
}
} // namespace snow_shot::presentation
