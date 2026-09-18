#ifndef SNOW_SHOT_STORAGE_PERSISTEDWINDOWGEOMETRY_H
#define SNOW_SHOT_STORAGE_PERSISTEDWINDOWGEOMETRY_H

#include <QJsonObject>
#include <QList>
#include <QRect>
#include <QSize>

#include <optional>

namespace snow_shot::storage {

// Window geometry is persisted in Qt logical coordinates; screen-layout clamping
// happens at restore time, so a layout or DPI change can never strand a window.
struct PersistedWindowGeometry {
    QRect normalGeometry;
    bool maximized = false;
};

[[nodiscard]] std::optional<PersistedWindowGeometry> parseWindowGeometry(const QJsonObject& value);
[[nodiscard]] QJsonObject windowGeometryToJson(const QRect& normalGeometry, bool maximized);

[[nodiscard]] std::optional<QSize> parseWindowSize(const QJsonObject& value);
[[nodiscard]] QJsonObject windowSizeToJson(const QSize& size);

// If the geometry intersects any screen it is kept; otherwise it is moved fully inside
// the first entry, which callers supply from the primary screen. Sizes larger than that
// fallback screen are clamped to it.
[[nodiscard]] QRect clampWindowGeometryToScreens(const QRect& geometry,
                                                 const QList<QRect>& availableGeometries);
[[nodiscard]] QSize clampWindowSize(const QSize& size, const QSize& minimum, const QSize& maximum);

// Raises below-minimum sizes, caps to the largest available screen, then applies
// clampWindowGeometryToScreens. Empty screen lists only enforce the minimum size.
[[nodiscard]] PersistedWindowGeometry
fitPersistedWindowGeometry(const PersistedWindowGeometry& saved, const QSize& minimumSize,
                           const QList<QRect>& availableGeometries);

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_PERSISTEDWINDOWGEOMETRY_H
