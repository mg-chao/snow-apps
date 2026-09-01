#ifndef ADQT_ICON_RENDERER_H
#define ADQT_ICON_RENDERER_H

#include "adqt_icon_core_global.h"
#include "icon_core_types.h"

#include <QCursor>
#include <QImage>
#include <QIcon>
#include <QList>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QRectF>

#include <memory>

namespace adqt::icons::detail {
struct IconRendererImpl;
}

namespace adqt::icons {

// Thread-safe rendering facade over the immutable descriptor tables of generated icon packs.
class ADQT_ICON_CORE_EXPORT IconRenderer final {
 public:
  static constexpr qint64 kDefaultCacheLimitBytes = 2 * 1024 * 1024;
  static constexpr int kDefaultMaxCacheEntries = 512;
  static constexpr qint64 kDefaultMaxRasterBytes = 256 * 1024;

  IconRenderer();
  ~IconRenderer();
  IconRenderer(const IconRenderer&) = delete;
  IconRenderer& operator=(const IconRenderer&) = delete;

  // Catalogues a generated pack process-wide. The pack must remain alive for the lifetime of any
  // references (generated packs are program-lifetime static data).
  IconPackRegistrationResult registerStaticPack(const IconPack& pack) const;

  IconMetadataView describeIconView(const IconRef& ref) const;
  IconMetadata describeIcon(const IconRef& ref) const;

  QIcon makeIcon(const IconRef& ref, const IconStatePalette& palette = {}) const;
  QImage renderIconImage(const IconRef& ref, const IconRenderRequest& request = {},
                         const IconStatePalette& palette = {}) const;
  QPixmap renderIconPixmap(const IconRef& ref, const IconRenderRequest& request,
                           const IconStatePalette& palette = {}) const;
  void paintIcon(QPainter* painter, const IconRef& ref, const QRectF& rect,
                 const IconRenderRequest& request = {}, const IconStatePalette& palette = {}) const;
  QCursor makeCursor(const IconRef& ref, const QSize& logicalSize, const QPoint& hotSpot,
                     qreal devicePixelRatio = 1.0) const;

  void setPaletteResolver(IconPaletteResolver resolver);
  void clearPaletteResolver();

  void setCacheLimitBytes(qint64 bytes);
  qint64 cacheLimitBytes() const;
  void setCacheLimits(qint64 bytes, int maxEntries = kDefaultMaxCacheEntries,
                      qint64 maxRasterBytes = kDefaultMaxRasterBytes);
  // A negative target trims back to the configured cache limit.
  IconCacheReclaimReport trimCache(qint64 targetBytes = -1);
  void clearCache();
  void prewarm(const QList<IconPixmapRequest>& requests) const;
  IconCacheStatistics cacheStatistics() const;

 private:
  std::shared_ptr<detail::IconRendererImpl> impl_;
};

ADQT_ICON_CORE_EXPORT IconRenderer& defaultRenderer();
ADQT_ICON_CORE_EXPORT IconMetadataView describeIconView(const IconRef& ref);
ADQT_ICON_CORE_EXPORT IconMetadata describeIcon(const IconRef& ref);
ADQT_ICON_CORE_EXPORT QIcon makeIcon(const IconRef& ref, const IconStatePalette& palette = {});
ADQT_ICON_CORE_EXPORT QImage renderIconImage(const IconRef& ref,
                                              const IconRenderRequest& request = {},
                                              const IconStatePalette& palette = {});
ADQT_ICON_CORE_EXPORT QPixmap renderIconPixmap(const IconRef& ref,
                                                const IconRenderRequest& request,
                                                const IconStatePalette& palette = {});
ADQT_ICON_CORE_EXPORT void paintIcon(QPainter* painter, const IconRef& ref, const QRectF& rect,
                                     const IconRenderRequest& request = {},
                                     const IconStatePalette& palette = {});
ADQT_ICON_CORE_EXPORT QCursor makeCursor(const IconRef& ref, const QSize& logicalSize,
                                         const QPoint& hotSpot, qreal devicePixelRatio = 1.0);
ADQT_ICON_CORE_EXPORT void setPaletteResolver(IconPaletteResolver resolver);
ADQT_ICON_CORE_EXPORT void clearPaletteResolver();
ADQT_ICON_CORE_EXPORT void setCacheLimitBytes(qint64 bytes);
ADQT_ICON_CORE_EXPORT void setCacheLimits(qint64 bytes,
                                          int maxEntries = IconRenderer::kDefaultMaxCacheEntries,
                                          qint64 maxRasterBytes =
                                              IconRenderer::kDefaultMaxRasterBytes);
ADQT_ICON_CORE_EXPORT IconCacheReclaimReport trimIconCache(qint64 targetBytes = -1);
ADQT_ICON_CORE_EXPORT void clearCache();
ADQT_ICON_CORE_EXPORT void prewarm(const QList<IconPixmapRequest>& requests);
ADQT_ICON_CORE_EXPORT IconCacheStatistics cacheStatistics();

}  // namespace adqt::icons

#endif  // ADQT_ICON_RENDERER_H
