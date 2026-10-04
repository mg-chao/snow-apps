#ifndef SNOW_SHOT_TRANSLATION_TRANSLATIONLANGUAGECATALOG_H
#define SNOW_SHOT_TRANSLATION_TRANSLATIONLANGUAGECATALOG_H

#include <QCoreApplication>
#include <QVector>

namespace snow_shot::translation {
struct TranslationLanguage {
    const char* code;
    const char* name;
};

inline const QVector<TranslationLanguage>& translationLanguages() {
    static const QVector<TranslationLanguage> languages{
        {"ar", QT_TRANSLATE_NOOP("TranslationLanguages", "Arabic")},
        {"de", QT_TRANSLATE_NOOP("TranslationLanguages", "German")},
        {"en", QT_TRANSLATE_NOOP("TranslationLanguages", "English")},
        {"es", QT_TRANSLATE_NOOP("TranslationLanguages", "Spanish")},
        {"fr", QT_TRANSLATE_NOOP("TranslationLanguages", "French")},
        {"it", QT_TRANSLATE_NOOP("TranslationLanguages", "Italian")},
        {"ja", QT_TRANSLATE_NOOP("TranslationLanguages", "Japanese")},
        {"ko", QT_TRANSLATE_NOOP("TranslationLanguages", "Korean")},
        {"pt", QT_TRANSLATE_NOOP("TranslationLanguages", "Portuguese")},
        {"ru", QT_TRANSLATE_NOOP("TranslationLanguages", "Russian")},
        {"tr", QT_TRANSLATE_NOOP("TranslationLanguages", "Turkish")},
        {"zh-Hans", QT_TRANSLATE_NOOP("TranslationLanguages", "Simplified Chinese")},
        {"zh-Hant", QT_TRANSLATE_NOOP("TranslationLanguages", "Traditional Chinese")},
    };
    return languages;
}
} // namespace snow_shot::translation
#endif
