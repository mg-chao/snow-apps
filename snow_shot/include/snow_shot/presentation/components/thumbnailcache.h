#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_THUMBNAILCACHE_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_THUMBNAILCACHE_H

#include <QImage>
#include <QSize>
#include <QString>

namespace snow_shot::presentation::components::thumbnail_cache {

struct CachedThumbnail final {
    QImage image;
    QSize naturalSize;
};

[[nodiscard]] QString pathForKey(const QString& key, const QString& directory = {});
[[nodiscard]] CachedThumbnail load(const QString& path);
void persist(const QString& path, QImage image, QSize naturalSize = {});

} // namespace snow_shot::presentation::components::thumbnail_cache

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_THUMBNAILCACHE_H
