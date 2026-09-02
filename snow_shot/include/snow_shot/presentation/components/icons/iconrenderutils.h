#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_ICONS_ICONRENDERUTILS_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_ICONS_ICONRENDERUTILS_H

#include "icon_core.h"

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QSize>

namespace snow_shot::presentation::icons {
[[nodiscard]] inline adqt::icons::IconRef withPrimaryColor(const adqt::icons::IconRef& iconRef,
                                                           const QColor& color) {
    if (!adqt::icons::isValid(iconRef) || !color.isValid()) {
        return iconRef;
    }

    return iconRef.withColors(iconRef.colors().withPrimary(color));
}

[[nodiscard]] inline QPixmap renderTintedIconPixmap(const adqt::icons::IconRef& iconRef,
                                                    const QSize& logicalSize,
                                                    qreal devicePixelRatio, const QColor& color,
                                                    QIcon::Mode mode = QIcon::Normal,
                                                    QIcon::State state = QIcon::Off) {
    if (!adqt::icons::isValid(iconRef) || logicalSize.width() <= 0 || logicalSize.height() <= 0) {
        return {};
    }

    return adqt::icons::renderIconPixmap(withPrimaryColor(iconRef, color),
                                         {logicalSize, devicePixelRatio, mode, state});
}
} // namespace snow_shot::presentation::icons

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_ICONS_ICONRENDERUTILS_H
