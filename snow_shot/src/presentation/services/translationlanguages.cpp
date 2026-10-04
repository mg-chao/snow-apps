#include "snow_shot/translation/translationlanguages.h"

#include <QCoreApplication>

namespace snow_shot::translation {
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
QString translationModelGroup(const SnowShotChatModel& model) {
    return model.translationMode == QStringLiteral("default")
               ? QCoreApplication::translate("TranslationService", "General Models")
               : QCoreApplication::translate("TranslationService", "Translation Models");
}
} // namespace snow_shot::translation
