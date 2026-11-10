#ifndef SNOW_SHOT_TRANSLATION_FREETRANSLATION_H
#define SNOW_SHOT_TRANSLATION_FREETRANSLATION_H

#include <QCoreApplication>
#include <QString>

namespace snow_shot::translation {
inline const QString freeTranslationId = QStringLiteral("free-translation");

inline QString freeTranslationErrorText() {
    return QCoreApplication::translate(
        "FreeTranslation",
        "The free translation service may no longer be available. Please update the app or report "
        "the issue");
}
} // namespace snow_shot::translation

#endif
