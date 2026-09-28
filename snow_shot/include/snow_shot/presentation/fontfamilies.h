#ifndef SNOW_SHOT_PRESENTATION_FONTFAMILIES_H
#define SNOW_SHOT_PRESENTATION_FONTFAMILIES_H

#include <QFontDatabase>
#include <QStringList>

namespace snow_shot::presentation {
// Enumerate once: font selectors are rebuilt when pages and toolbar editors change.
inline const QStringList& applicationFontFamilies() {
    static const QStringList families = [] {
        QStringList result;
        for (const QString& family : QFontDatabase::families()) {
            const QString trimmed = family.trimmed();
            if (!trimmed.isEmpty()) {
                result.append(trimmed);
            }
        }
        result.removeDuplicates();
        result.sort(Qt::CaseInsensitive);
        return result;
    }();
    return families;
}
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_FONTFAMILIES_H
