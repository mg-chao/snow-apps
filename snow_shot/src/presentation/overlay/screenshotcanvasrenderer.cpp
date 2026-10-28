#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotimagerendering.h"

#include "snow_shot/presentation/screenshotguidelinerendering.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtextlayer.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "theme/theme_manager.h"
#include "widgets/checkerboard.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QColorSpace>
#include <QEvent>
#if defined(SNOW_SHOT_BENCH_INTERNALS)
#include <QElapsedTimer>
#endif
#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QGraphicsItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPaintEngine>
#include <QPainterStateGuard>
#include <QPen>
#include <QPointF>
#include <QRawFont>
#include <QRect>
#include <QRegion>
#include <QSizeF>
#include <QStyleOptionGraphicsItem>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <QTransform>
#include <QVector>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <list>
#include <memory>
#include <utility>
#include <vector>

// Keep glyph rasters local to the recognition surface. Qt's graphics-item device cache shares
// QPixmapCache with the rest of the application and evicts dense pages while they are being drawn.
class ScreenshotOcrGlyphRasterCache final {
  public:
    struct Entry {
        QImage image;
        std::array<QImage, 3> edges;
        QPoint imageOffset;
        QRect solidInterior;
        QColor interiorColor;
        QTransform deviceTransform;
        QRect pixels;
        QColor highlight;
        QColor selectedText;
        ScreenshotOcrTextRange selection;
        QPainter::RenderHints hints;
        bool selectionOnly = false;
        bool retained = false;
        bool previewRaster = false;
        bool backgroundRaster = false;
        quint64 lastUse = 0;
        std::list<Entry*>::iterator position;

        [[nodiscard]] qsizetype bytes() const {
            qsizetype result = image.sizeInBytes();
            for (const auto& edge : edges) {
                result += edge.sizeInBytes();
            }
            return result;
        }
    };

    static constexpr qsizetype byteLimit = 64 * 1024 * 1024;
    static constexpr qsizetype entryByteLimit = 8 * 1024 * 1024;

    void discard(Entry& entry) {
        if (entry.retained) {
            m_bytes -= entry.bytes();
            if (entry.previewRaster) {
                m_previewBytes -= entry.bytes();
            }
            if (entry.backgroundRaster) {
                m_backgroundBytes -= entry.bytes();
            }
            m_entries.erase(entry.position);
            entry.retained = false;
        }
        entry.image = {};
        entry.edges = {};
        entry.imageOffset = {};
        entry.solidInterior = {};
    }

    void touch(Entry& entry) {
        entry.lastUse = ++m_clock;
        m_entries.splice(m_entries.end(), m_entries, entry.position);
    }

    bool allocate(Entry& entry, const QSize& size, bool previewRaster = false,
                  bool backgroundRaster = false, const Entry* paintedGlyph = nullptr) {
        const qint64 bytes = qint64(size.width()) * size.height() * 4;
        if (size.isEmpty() || bytes > entryByteLimit || bytes > m_byteBudget) {
            return false;
        }
        discard(entry);
        entry.previewRaster = previewRaster;
        entry.backgroundRaster = backgroundRaster;
        while (m_bytes + bytes > m_byteBudget && !m_entries.empty()) {
            const auto victim =
                std::find_if(m_entries.begin(), m_entries.end(),
                             [paintedGlyph](const auto* cached) { return cached != paintedGlyph; });
            if (victim == m_entries.end()) {
                return false;
            }
            discard(**victim);
        }
        entry.image = snowCanvasAllocateImage(size, QImage::Format_ARGB32_Premultiplied);
        if (entry.image.isNull()) {
            return false;
        }
        m_entries.push_back(&entry);
        entry.position = std::prev(m_entries.end());
        entry.retained = true;
        entry.lastUse = ++m_clock;
        m_bytes += entry.image.sizeInBytes();
        if (previewRaster) {
            m_previewBytes += entry.image.sizeInBytes();
        }
        if (backgroundRaster) {
            m_backgroundBytes += entry.image.sizeInBytes();
        }
        return true;
    }

    void compactRectangleInterior(Entry& entry, const QRect& interior, const QColor& color) {
        if (!entry.previewRaster || !entry.retained || interior.isEmpty() || interior.top() <= 0 ||
            !entry.image.rect().contains(interior)) {
            return;
        }
        const QRgb expected = qPremultiply(color.rgba());
        for (int y = interior.top(); y <= interior.bottom(); ++y) {
            const auto* pixels = reinterpret_cast<const QRgb*>(entry.image.constScanLine(y));
            if (std::any_of(pixels + interior.left(), pixels + interior.right() + 1,
                            [expected](QRgb pixel) { return pixel != expected; })) {
                return;
            }
        }
        const QSize size = entry.image.size();
        QImage top = entry.image.copy(QRect(0, 0, size.width(), interior.top()));
        std::array<QImage, 3> edges = {
            entry.image.copy(QRect(0, interior.bottom() + 1, size.width(),
                                   size.height() - interior.bottom() - 1)),
            entry.image.copy(QRect(0, interior.top(), interior.left(), interior.height())),
            entry.image.copy(QRect(interior.right() + 1, interior.top(),
                                   size.width() - interior.right() - 1, interior.height()))};
        if (top.isNull() || std::any_of(edges.begin(), edges.end(),
                                        [](const QImage& edge) { return edge.isNull(); })) {
            return;
        }
        const qsizetype previous = entry.bytes();
        entry.image = std::move(top);
        entry.edges = std::move(edges);
        entry.solidInterior = interior;
        entry.interiorColor = color;
        const qsizetype released = previous - entry.bytes();
        m_bytes -= released;
        m_previewBytes -= released;
    }

    void trimTransparentMargins(Entry& entry) {
        if (!m_marginTrimmingEnabled || !entry.retained || entry.previewRaster ||
            entry.image.isNull()) {
            return;
        }
        const QSize size = entry.image.size();
        int left = size.width();
        int right = -1;
        int top = size.height();
        int bottom = -1;
        for (int y = 0; y < size.height(); ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(entry.image.constScanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                if (qAlpha(row[x]) == 0) {
                    continue;
                }
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
        // Keep the complete device bounds and phase for later background construction and
        // clipping. Only the copied image extent changes, by an exact integer pixel offset.
        const QRect content =
            right >= left ? QRect(QPoint(left, top), QPoint(right, bottom)) : QRect(0, 0, 1, 1);
        if (content == entry.image.rect()) {
            return;
        }
        QImage trimmed = entry.image.copy(content);
        if (trimmed.isNull()) {
            return;
        }
        const qsizetype previous = entry.bytes();
        entry.image = std::move(trimmed);
        entry.imageOffset += content.topLeft();
        const qsizetype released = previous - entry.bytes();
        m_bytes -= released;
        if (entry.backgroundRaster) {
            m_backgroundBytes -= released;
        }
    }

    bool enabled = true;
    bool backgroundEnabled = true;
    quint64 builds = 0;
    quint64 hits = 0;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    quint64 directPaints = 0;
    quint64 backgroundPaints = 0;
#endif
    quint64 previewBuilds = 0;
    quint64 previewHits = 0;
    quint64 backgroundBuilds = 0;
    quint64 backgroundHits = 0;
    [[nodiscard]] qsizetype bytes() const {
        return m_bytes;
    }
    [[nodiscard]] qsizetype previewBytes() const {
        return m_previewBytes;
    }
    [[nodiscard]] qsizetype backgroundBytes() const {
        return m_backgroundBytes;
    }
    void setByteBudget(qsizetype bytes) {
        m_byteBudget = std::clamp<qsizetype>(bytes, 0, byteLimit);
        while (m_bytes > m_byteBudget && !m_entries.empty()) {
            discard(*m_entries.front());
        }
    }
    void setMarginTrimmingEnabled(bool trimmingEnabled) {
        if (m_marginTrimmingEnabled == trimmingEnabled) {
            return;
        }
        while (!m_entries.empty()) {
            discard(*m_entries.front());
        }
        m_marginTrimmingEnabled = trimmingEnabled;
    }

  private:
    std::list<Entry*> m_entries;
    qsizetype m_bytes = 0;
    qsizetype m_previewBytes = 0;
    qsizetype m_backgroundBytes = 0;
    qsizetype m_byteBudget = byteLimit;
    quint64 m_clock = 0;
    bool m_marginTrimmingEnabled = true;
};

// Preview state changes are sparse, but a page-wide drag may affect thousands of blocks. Keep
// their backgrounds in one scene item and rebuild the two draw paths lazily when painting.
class ScreenshotOcrSelectionPreviewItem final : public QGraphicsItem {
  public:
    ScreenshotOcrSelectionPreviewItem(ScreenshotOcrGlyphRasterCache& cache, std::size_t count)
        : m_cache(cache), m_quads(count) {
        setZValue(-1.0);
        for (auto& indices : m_active) {
            indices.reserve(count);
        }
        m_geometryOrder.reserve(count);
        m_sweep.reserve(count);
        m_uncached.reserve(count);
        for (auto& rectangles : m_rectangles) {
            rectangles.reserve(count);
        }
        for (auto& indices : m_separated) {
            indices.reserve(count);
        }
    }

    ~ScreenshotOcrSelectionPreviewItem() override {
        for (auto& quad : m_quads) {
            for (auto& raster : quad.rasters) {
                m_cache.discard(raster);
            }
        }
    }

    void setBounds(const QRectF& bounds) {
        if (m_bounds == bounds) {
            return;
        }
        prepareGeometryChange();
        m_bounds = bounds;
    }

    void setQuad(int index, QPolygonF polygon) {
        Quad& quad = m_quads.at(static_cast<std::size_t>(index));
        if (polygon.size() != 4 ||
            std::any_of(polygon.begin(), polygon.end(), [](const QPointF& point) {
                return !std::isfinite(point.x()) || !std::isfinite(point.y());
            })) {
            polygon.clear();
        }
        if (quad.polygon == polygon) {
            return;
        }
        if (quad.preview != ScreenshotOcrLineSelectionPreview::None) {
            addDamage(quad.bounds);
        }
        // Winding fill retains coverage where blocks overlap, even when an OCR provider returns
        // reversed corners. Normalize once with the geometry instead of during each gesture.
        qreal area = 0.0;
        for (qsizetype point = 0; point < polygon.size(); ++point) {
            const QPointF& a = polygon[point];
            const QPointF& b = polygon[(point + 1) % polygon.size()];
            area += a.x() * b.y() - b.x() * a.y();
        }
        if (area < 0.0) {
            std::reverse(polygon.begin(), polygon.end());
        }
        quad.polygon = std::move(polygon);
        for (auto& raster : quad.rasters) {
            m_cache.discard(raster);
        }
        quad.shapeBounds = quad.polygon.boundingRect();
        quad.rasterBounds = {};
        if (!quad.polygon.isEmpty()) {
            QPainterPath outline;
            outline.addPolygon(quad.polygon);
            outline.closeSubpath();
            QPainterPathStroker stroker;
            stroker.setWidth(1.0);
            stroker.setJoinStyle(Qt::MiterJoin);
            stroker.setMiterLimit(2.0);
            quad.rasterBounds =
                stroker.createStroke(outline).boundingRect().united(quad.shapeBounds);
        }
        quad.bounds =
            quad.polygon.isEmpty() ? QRectF{} : quad.shapeBounds.adjusted(-2.0, -2.0, 2.0, 2.0);
        quad.rectangle = quad.polygon.size() == 4;
        for (qsizetype point = 0; point < quad.polygon.size(); ++point) {
            const QPointF& a = quad.polygon[point];
            const QPointF& b = quad.polygon[(point + 1) % quad.polygon.size()];
            quad.rectangle &= a.x() == b.x() || a.y() == b.y();
        }
        m_geometryDirty = true;
        if (quad.preview != ScreenshotOcrLineSelectionPreview::None) {
            addDamage(quad.bounds);
            m_pathsDirty = true;
        }
    }

    bool setPreview(int index, ScreenshotOcrLineSelectionPreview preview) {
        Quad& quad = m_quads.at(static_cast<std::size_t>(index));
        if (quad.preview == preview) {
            return false;
        }
        if (quad.preview != ScreenshotOcrLineSelectionPreview::None) {
            auto& indices = m_active[actionIndex(quad.preview)];
            const int replacement = indices.back();
            indices[static_cast<std::size_t>(quad.slot)] = replacement;
            m_quads[static_cast<std::size_t>(replacement)].slot = quad.slot;
            indices.pop_back();
        }
        quad.preview = preview;
        quad.slot = -1;
        if (preview != ScreenshotOcrLineSelectionPreview::None) {
            auto& indices = m_active[actionIndex(preview)];
            quad.slot = static_cast<int>(indices.size());
            indices.push_back(index);
        }
        addDamage(quad.bounds);
        m_pathsDirty = true;
        return true;
    }

    [[nodiscard]] QRegion takeDamage() {
        QRegion damage = m_damageCount > sparseDamageLimit ? QRegion(m_damageBounds) : m_damage;
        m_damage = {};
        m_damageBounds = {};
        m_damageCount = 0;
        return damage;
    }

    [[nodiscard]] const QPainterPath& path(ScreenshotOcrLineSelectionPreview preview) {
        rebuildPaths();
        return m_paths[actionIndex(preview)];
    }

    [[nodiscard]] QRectF boundingRect() const override {
        return m_bounds;
    }

#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] quint64 paintNanoseconds() const {
        return m_paintNanoseconds;
    }
#endif

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        QElapsedTimer timer;
        timer.start();
#endif
        rebuildPaths();
        painter->save();
        for (std::size_t action = 0; action < m_paths.size(); ++action) {
            if (m_paths[action].isEmpty()) {
                continue;
            }
            const QColor color = action == 0 ? QColor(0x52, 0xc4, 0x1a) : QColor(0xff, 0x4d, 0x4f);
            QPen pen(color, 1.0, Qt::SolidLine);
            pen.setJoinStyle(Qt::MiterJoin);
            painter->setPen(pen);
            QColor fill = color;
            fill.setAlpha(0x33);
            painter->setBrush(fill);
            paintSeparated(painter, action);
            if (!m_overlapPaths[action].isEmpty()) {
                painter->drawPath(m_overlapPaths[action]);
            }
        }
        painter->restore();
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        m_paintNanoseconds += static_cast<quint64>(timer.nsecsElapsed());
#endif
    }

  private:
    struct Quad {
        QPolygonF polygon;
        QRectF shapeBounds;
        QRectF rasterBounds;
        QRectF bounds;
        ScreenshotOcrLineSelectionPreview preview = ScreenshotOcrLineSelectionPreview::None;
        int slot = -1;
        bool rectangle = false;
        bool overlaps = false;
        std::array<ScreenshotOcrGlyphRasterCache::Entry, 2> rasters;
    };

    [[nodiscard]] static std::size_t actionIndex(ScreenshotOcrLineSelectionPreview preview) {
        return preview == ScreenshotOcrLineSelectionPreview::Select ? 0U : 1U;
    }

    void paintSeparated(QPainter* painter, std::size_t action) {
        const QTransform device = painter->deviceTransform();
        if (!device.isAffine() || !device.isInvertible() ||
            painter->paintEngine()->type() != QPaintEngine::Raster || painter->opacity() != 1.0 ||
            painter->compositionMode() != QPainter::CompositionMode_SourceOver) {
            if (!m_rectangles[action].empty()) {
                painter->drawRects(m_rectangles[action].data(),
                                   static_cast<int>(m_rectangles[action].size()));
            }
            if (!m_complexPaths[action].isEmpty()) {
                painter->drawPath(m_complexPaths[action]);
            }
            return;
        }
        const QPen pen = painter->pen();
        const QBrush brush = painter->brush();
        const auto hints = painter->renderHints();
        painter->save();
        painter->setWorldTransform(QTransform());
        painter->setViewTransformEnabled(false);
        painter->setWorldTransform(painter->deviceTransform().inverted());
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter->setRenderHint(QPainter::Antialiasing, false);
        m_uncached.clear();
        for (const int index : m_separated[action]) {
            Quad& quad = m_quads[static_cast<std::size_t>(index)];
            auto& raster = quad.rasters[action];
            if (!raster.image.isNull() && raster.deviceTransform == device &&
                raster.hints == hints) {
                m_cache.touch(raster);
                ++m_cache.previewHits;
            } else {
                const QRectF physical = device.mapRect(quad.rasterBounds);
                constexpr qreal coordinateLimit = std::numeric_limits<int>::max() / 4.0;
                if (!std::isfinite(physical.left()) || !std::isfinite(physical.top()) ||
                    !std::isfinite(physical.right()) || !std::isfinite(physical.bottom()) ||
                    std::abs(physical.left()) > coordinateLimit ||
                    std::abs(physical.top()) > coordinateLimit ||
                    std::abs(physical.right()) > coordinateLimit ||
                    std::abs(physical.bottom()) > coordinateLimit) {
                    m_uncached.push_back(index);
                    continue;
                }
                const QRect pixels = physical.adjusted(-1.0, -1.0, 1.0, 1.0).toAlignedRect();
                if (!m_cache.allocate(raster, pixels.size(), true)) {
                    m_uncached.push_back(index);
                    continue;
                }
                raster.deviceTransform = device;
                raster.pixels = pixels;
                raster.hints = hints;
                raster.image.fill(Qt::transparent);
                QPainter background(&raster.image);
                background.setRenderHints(hints);
                background.setWorldTransform(device *
                                             QTransform::fromTranslate(-pixels.x(), -pixels.y()));
                background.setPen(pen);
                background.setBrush(brush);
                background.drawPolygon(quad.polygon, Qt::WindingFill);
                background.end();
                if (quad.rectangle && device.type() <= QTransform::TxScale) {
                    const QRectF fill = device.mapRect(quad.shapeBounds);
                    const qreal xPadding = std::abs(device.m11()) * pen.widthF() * 0.5 + 1.0;
                    const qreal yPadding = std::abs(device.m22()) * pen.widthF() * 0.5 + 1.0;
                    const QRect interior(
                        QPoint(qCeil(fill.left() + xPadding), qCeil(fill.top() + yPadding)),
                        QPoint(qFloor(fill.right() - xPadding) - 1,
                               qFloor(fill.bottom() - yPadding) - 1));
                    if (qint64(interior.width()) * interior.height() >=
                        qint64(pixels.width()) * pixels.height() / 4) {
                        // Retain every AA/outline pixel exactly, but avoid storing and reading
                        // thousands of identical translucent interior pixels on dense pages.
                        m_cache.compactRectangleInterior(
                            raster, interior.translated(-pixels.topLeft()), brush.color());
                    }
                }
                ++m_cache.previewBuilds;
            }
            if (!raster.solidInterior.isEmpty()) {
                const QRect interior = raster.solidInterior.translated(raster.pixels.topLeft());
                painter->fillRect(interior, raster.interiorColor);
                painter->drawImage(QPoint(raster.pixels.x(), interior.bottom() + 1),
                                   raster.edges[0]);
                painter->drawImage(QPoint(raster.pixels.x(), interior.y()), raster.edges[1]);
                painter->drawImage(QPoint(interior.right() + 1, interior.y()), raster.edges[2]);
            }
            painter->drawImage(raster.pixels.topLeft(), raster.image);
        }
        painter->restore();
        for (const int index : m_uncached) {
            painter->drawPolygon(m_quads[static_cast<std::size_t>(index)].polygon, Qt::WindingFill);
        }
    }

    void addDamage(const QRectF& bounds) {
        const QRect rectangle = bounds.intersected(m_bounds).toAlignedRect();
        if (rectangle.isEmpty()) {
            return;
        }
        m_damageBounds = m_damageBounds.united(rectangle);
        if (++m_damageCount <= sparseDamageLimit) {
            m_damage += rectangle;
        }
    }

    void updateOverlapFlags() {
        if (!m_geometryDirty) {
            return;
        }
        m_geometryOrder.clear();
        for (std::size_t index = 0; index < m_quads.size(); ++index) {
            m_quads[index].overlaps = false;
            if (m_quads[index].polygon.size() == 4) {
                m_geometryOrder.push_back(static_cast<int>(index));
            }
        }
        std::sort(m_geometryOrder.begin(), m_geometryOrder.end(), [this](int a, int b) {
            return m_quads[static_cast<std::size_t>(a)].shapeBounds.top() <
                   m_quads[static_cast<std::size_t>(b)].shapeBounds.top();
        });
        m_sweep.clear();
        for (const int index : m_geometryOrder) {
            Quad& quad = m_quads[static_cast<std::size_t>(index)];
            const QRectF bounds = quad.shapeBounds.adjusted(-1.0, -1.0, 1.0, 1.0);
            m_sweep.erase(
                std::remove_if(
                    m_sweep.begin(), m_sweep.end(),
                    [this, &bounds](int other) {
                        return m_quads[static_cast<std::size_t>(other)].shapeBounds.bottom() +
                                   1.0 <=
                               bounds.top();
                    }),
                m_sweep.end());
            for (const int other : m_sweep) {
                Quad& candidate = m_quads[static_cast<std::size_t>(other)];
                if ((!quad.overlaps || !candidate.overlaps) &&
                    bounds.intersects(candidate.shapeBounds.adjusted(-1.0, -1.0, 1.0, 1.0))) {
                    quad.overlaps = true;
                    candidate.overlaps = true;
                }
            }
            m_sweep.push_back(index);
        }
        m_geometryDirty = false;
    }

    void rebuildPaths() {
        if (!m_pathsDirty) {
            return;
        }
        updateOverlapFlags();
        for (std::size_t action = 0; action < m_paths.size(); ++action) {
            auto& path = m_paths[action];
            path = {};
            path.setFillRule(Qt::WindingFill);
            path.reserve(static_cast<int>(m_active[action].size()) * 5);
            auto& complex = m_complexPaths[action];
            complex = {};
            complex.setFillRule(Qt::WindingFill);
            complex.reserve(static_cast<int>(m_active[action].size()) * 5);
            auto& rectangles = m_rectangles[action];
            rectangles.clear();
            auto& overlap = m_overlapPaths[action];
            overlap = {};
            overlap.setFillRule(Qt::WindingFill);
            overlap.reserve(static_cast<int>(m_active[action].size()) * 5);
            m_separated[action].clear();
            for (const int index : m_active[action]) {
                const Quad& quad = m_quads[static_cast<std::size_t>(index)];
                path.addPolygon(quad.polygon);
                path.closeSubpath();
                if (quad.overlaps) {
                    overlap.addPolygon(quad.polygon);
                    overlap.closeSubpath();
                } else {
                    m_separated[action].push_back(index);
                }
                if (!quad.overlaps && quad.rectangle) {
                    rectangles.push_back(quad.shapeBounds);
                } else if (!quad.overlaps) {
                    complex.addPolygon(quad.polygon);
                    complex.closeSubpath();
                }
            }
        }
        m_pathsDirty = false;
    }

    static constexpr int sparseDamageLimit = 64;
    ScreenshotOcrGlyphRasterCache& m_cache;
    std::vector<Quad> m_quads;
    std::array<std::vector<int>, 2> m_active;
    std::array<QPainterPath, 2> m_paths;
    std::array<QPainterPath, 2> m_complexPaths;
    std::array<QPainterPath, 2> m_overlapPaths;
    std::array<std::vector<QRectF>, 2> m_rectangles;
    std::array<std::vector<int>, 2> m_separated;
    std::vector<int> m_uncached;
    std::vector<int> m_geometryOrder;
    std::vector<int> m_sweep;
    bool m_geometryDirty = true;
    bool m_pathsDirty = true;
    QRectF m_bounds;
    QRegion m_damage;
    QRect m_damageBounds;
    int m_damageCount = 0;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    quint64 m_paintNanoseconds = 0;
#endif
};

class ScreenshotOcrSelectionMarqueeItem final : public QGraphicsItem {
  public:
    ScreenshotOcrSelectionMarqueeItem() {
        setZValue(1.0);
        setAcceptedMouseButtons(Qt::NoButton);
    }

    void setBounds(const QRectF& bounds) {
        if (m_bounds != bounds) {
            prepareGeometryChange();
            m_bounds = bounds;
        }
    }

    void setPen(const QPen& pen) {
        m_pen = pen;
    }

    void setBrush(const QBrush& brush) {
        m_brush = brush;
    }

    [[nodiscard]] QRectF rect() const {
        return m_rect;
    }

    [[nodiscard]] QRegion setRect(const QRectF& rectangle) {
        const QRectF next = rectangle.isEmpty() || !std::isfinite(rectangle.left()) ||
                                    !std::isfinite(rectangle.top()) ||
                                    !std::isfinite(rectangle.right()) ||
                                    !std::isfinite(rectangle.bottom())
                                ? QRectF{}
                                : rectangle;
        if (m_rect == next) {
            return {};
        }
        const auto clippedRegion = [this](const QRectF& bounds) {
            return QRegion(bounds.intersected(m_bounds).toAlignedRect());
        };
        const auto border = [&](const QRectF& bounds) {
            if (bounds.isEmpty()) {
                return QRegion{};
            }
            constexpr qreal padding = 2.0;
            QRegion band = clippedRegion(bounds.adjusted(-padding, -padding, padding, padding));
            if (bounds.width() > padding * 2 && bounds.height() > padding * 2) {
                const QRectF interior =
                    bounds.adjusted(padding, padding, -padding, -padding).intersected(m_bounds);
                if (!interior.isEmpty()) {
                    const int left = qCeil(interior.left());
                    const int top = qCeil(interior.top());
                    const QRect pixels(left, top, std::max(0, qFloor(interior.right()) - left),
                                       std::max(0, qFloor(interior.bottom()) - top));
                    band -= pixels;
                }
            }
            return band;
        };
        QRegion damage = clippedRegion(m_rect).xored(clippedRegion(next));
        damage += border(m_rect);
        damage += border(next);
        // The filled intersection is unchanged. Keep scene geometry fixed and let the layer
        // invalidate just these fill differences and old/new antialiased border bands.
        m_rect = next;
        return damage;
    }

    [[nodiscard]] QRectF boundingRect() const override {
        return m_bounds;
    }

#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] quint64 paintNanoseconds() const {
        return m_paintNanoseconds;
    }
#endif

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        if (m_rect.isEmpty()) {
            return;
        }
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        QElapsedTimer timer;
        timer.start();
#endif
        painter->save();
        painter->setPen(m_pen);
        painter->setBrush(m_brush);
        painter->drawRect(m_rect);
        painter->restore();
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        m_paintNanoseconds += static_cast<quint64>(timer.nsecsElapsed());
#endif
    }

  private:
    QRectF m_bounds;
    QRectF m_rect;
    QPen m_pen;
    QBrush m_brush;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    quint64 m_paintNanoseconds = 0;
#endif
};

class ScreenshotOcrGraphicsScene final : public QGraphicsScene {
  public:
    using QGraphicsScene::QGraphicsScene;

    void setTextHighlighting(const QColor& color, const QPainterPath& mask,
                             const std::optional<QTransform>& imageToScene = std::nullopt,
                             const QRectF& imageBounds = {}) {
        if (m_maskColor == color && m_mask == mask && m_imageToScene == imageToScene &&
            m_imageBounds == imageBounds) {
            return;
        }
        m_maskColor = color;
        m_mask = mask;
        m_imageToScene = imageToScene;
        m_imageBounds = imageBounds;
        invalidate(sceneRect(), QGraphicsScene::BackgroundLayer);
    }

    void clearTextHighlighting() {
        setTextHighlighting(QColor(Qt::transparent), {});
        m_raster = {};
        m_rasterMask = {};
    }

#if defined(SNOW_SHOT_BENCH_INTERNALS)
    quint64 rasterBuilds = 0;
    QImage rasterForTesting() const {
        return m_raster;
    }
#endif

  protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override {
        QGraphicsScene::drawBackground(painter, rect);
        if (m_maskColor.alpha() == 0 || (m_mask.isEmpty() && !m_imageToScene)) {
            return;
        }
        // Paint one shared complement so overlapping text regions stay clear and mask
        // opacity is applied once. Export and print render layouts without this scene.
        const QPainterStateGuard guard(painter);
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (m_imageToScene) {
            painter->setClipRect(sceneRect(), Qt::IntersectClip);
            painter->setWorldTransform(*m_imageToScene, true);
            // Panning can expose canvas outside the recognized image. Fill that exterior
            // separately so it does not enlarge or invalidate the image-resolution cache.
            QPainterPath exterior;
            exterior.addRect(m_imageToScene->inverted().mapRect(sceneRect()));
            QPainterPath image;
            image.addRect(m_imageBounds);
            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->fillPath(exterior.subtracted(image), m_maskColor);
            painter->setClipRect(m_imageBounds, Qt::IntersectClip);
            painter->setRenderHint(QPainter::Antialiasing, true);
        }
        if (!drawCachedMask(*painter)) {
            painter->fillPath(m_mask, m_maskColor);
        }
    }

  private:
    bool drawCachedMask(QPainter& painter) {
        // Small contours are cheaper to fill directly than to blit a viewport-sized image.
        // Complex contours amortize rasterization across repaints. The actual device
        // transform owns pixel alignment, including fractional DPR and redirected renders.
        // Pinned OCR keeps one pixel per image coordinate (100% zoom), independent of
        // viewport size, pan, DPR and redirected paint devices.
        const QTransform device = m_imageToScene ? QTransform() : painter.deviceTransform();
        if ((!m_imageToScene && m_mask.elementCount() < 128) ||
            device.type() > QTransform::TxScale || !std::isfinite(device.m11()) ||
            device.m11() <= 0 || !qFuzzyCompare(device.m11(), device.m22())) {
            return false;
        }
        const qreal scale = device.m11();
        const QRect pixels =
            m_imageToScene
                ? m_imageBounds.toAlignedRect()
                : device.mapRect(m_mask.boundingRect().adjusted(-2, -2, 2, 2)).toAlignedRect();
        constexpr qint64 byteLimit = 64 * 1024 * 1024;
        // Device-sized masks can grow without bound while zooming. The native cache is
        // bounded by the source image instead and uses the canvas image allocator's policy.
        if (pixels.isEmpty() ||
            (!m_imageToScene &&
             (pixels.width() > byteLimit / 4 || pixels.height() > byteLimit / 4 ||
              qint64(pixels.width()) * pixels.height() * 4 > byteLimit))) {
            return false;
        }
        const QPointF origin = device.inverted().map(QPointF(pixels.topLeft()));
        if (m_raster.isNull() || m_rasterMask != m_mask || m_rasterColor != m_maskColor ||
            m_rasterOrigin != origin || m_rasterScale != scale ||
            m_raster.size() != pixels.size()) {
            if (m_raster.size() != pixels.size()) {
                m_raster = {};
                m_raster =
                    snowCanvasAllocateImage(pixels.size(), QImage::Format_ARGB32_Premultiplied);
            }
            if (m_raster.isNull()) {
                return false;
            }
            m_raster.setDevicePixelRatio(scale);
            m_raster.fill(Qt::transparent);
            {
                QPainter raster(&m_raster);
                raster.setRenderHint(QPainter::Antialiasing, true);
                raster.translate(-origin);
                raster.fillPath(m_mask, m_maskColor);
            }
            m_rasterMask = m_mask;
            m_rasterColor = m_maskColor;
            m_rasterOrigin = origin;
            m_rasterScale = scale;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
            ++rasterBuilds;
#endif
        }
        if (m_imageToScene)
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(origin, m_raster);
        return true;
    }

    QColor m_maskColor = QColor(Qt::transparent);
    QPainterPath m_mask;
    std::optional<QTransform> m_imageToScene;
    QRectF m_imageBounds;
    QImage m_raster;
    QPainterPath m_rasterMask;
    QColor m_rasterColor;
    QPointF m_rasterOrigin;
    qreal m_rasterScale = 0;
};

class ScreenshotOcrGraphicsTextItem final : public QGraphicsItem {
  public:
    ScreenshotOcrGraphicsTextItem(ScreenshotOcrGlyphRasterCache& cache, const QWidget& appearance)
        : m_cache(cache), m_appearance(appearance) {}
    ~ScreenshotOcrGraphicsTextItem() override {
        for (auto& entry : m_rasters) {
            m_cache.discard(entry);
        }
        for (auto& entry : m_backgroundRasters) {
            m_cache.discard(entry);
        }
    }
    ScreenshotOcrTextLayout layout;
    bool selectionOnly = false;

    void setSelection(const ScreenshotOcrTextRange& selection, bool scheduleUpdate = true) {
        if (m_selection.start == selection.start && m_selection.length == selection.length) {
            return;
        }
        m_selection = selection;
        layout.setSelection(selection);
        if (scheduleUpdate) {
            update();
        }
    }
    [[nodiscard]] int cursorPositionAt(const QPointF& position) const {
        return layout.cursorPositionAt(position);
    }
    [[nodiscard]] QRectF boundingRect() const override {
        return layout.boundingRect();
    }
    bool configure(const ScreenshotOcrLine& line, const QTransform& canvasToView,
                   const QColor& color, const ScreenshotOcrTextRange& selection,
                   QTransform* transform) {
        for (auto& entry : m_rasters) {
            m_cache.discard(entry);
        }
        for (auto& entry : m_backgroundRasters) {
            m_cache.discard(entry);
        }
        prepareGeometryChange();
        const bool valid = configureScreenshotOcrTextLayout(
            layout, line, canvasToView, QApplication::font(), color, selection, transform);
        m_selection = selection;
        update();
        return valid;
    }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        if (selectionOnly && m_selection.empty()) {
            return;
        }
        // The scroll-area viewport may retain its own style palette. Resolve selection colors
        // from the recognition surface consistently for widget paints and direct scene renders.
        const QPalette palette = m_appearance.palette();
        QColor highlight = palette.highlight().color();
        if (selectionOnly) {
            highlight.setAlphaF(0.4F);
        }
        const QColor selectedText = palette.highlightedText().color();
        if (!paintRaster(painter, highlight, selectedText)) {
#if defined(SNOW_SHOT_BENCH_INTERNALS)
            ++m_cache.directPaints;
#endif
            layout.paint(painter, highlight, selectedText,
                         selectionOnly ? ScreenshotOcrTextLayout::PaintMode::SelectionOnly
                                       : ScreenshotOcrTextLayout::PaintMode::TextAndSelection);
        }
    }

  private:
    bool paintRaster(QPainter* painter, const QColor& highlight, const QColor& selectedText) {
        if (!m_cache.enabled || (!selectionOnly && !layout.glyphRasterCacheable()) ||
            painter->paintEngine()->type() != QPaintEngine::Raster || painter->opacity() != 1.0 ||
            painter->compositionMode() != QPainter::CompositionMode_SourceOver) {
            return false;
        }
        // Physical coordinates include DPR and fractional translation. Rasterizing with this
        // exact transform and blitting at integer device pixels preserves glyph phase and fitting.
        const QTransform device = painter->deviceTransform();
        // Perspective text uses device-dependent clipping inside QTextLayout. A translated
        // intermediate surface can change those clipped edge pixels, so retain direct painting.
        if (!device.isAffine()) {
            return false;
        }
        const QRectF bounds =
            device.mapRect(selectionOnly ? layout.boundingRect() : layout.glyphRasterBounds());
        constexpr qreal coordinateLimit = std::numeric_limits<int>::max() / 4.0;
        if (!device.isInvertible() || !std::isfinite(bounds.left()) ||
            !std::isfinite(bounds.top()) || !std::isfinite(bounds.right()) ||
            !std::isfinite(bounds.bottom()) || std::abs(bounds.left()) > coordinateLimit ||
            std::abs(bounds.top()) > coordinateLimit ||
            std::abs(bounds.right()) > coordinateLimit ||
            std::abs(bounds.bottom()) > coordinateLimit) {
            return false;
        }
        // QFixed truncates towards zero. Keep negative device baselines on the original
        // painter, since moving them into a positive cache could change their subpixel bucket.
        if (!selectionOnly && (bounds.left() < 0.0 || bounds.top() < 0.0)) {
            return false;
        }
        const QRect pixels = bounds.adjusted(-1.0, -1.0, 1.0, 1.0).toAlignedRect();
        const QTransform localDevice = device * QTransform::fromTranslate(-pixels.x(), -pixels.y());
        if (device.type() <= QTransform::TxScale) {
            // Qt rounds the original clip corners before building its raster clip. Re-centering
            // a cache can change floating-point addition around a half pixel and clip a whole
            // selection row. Preserve direct painting when that integer clip would differ.
            const QRectF clip = layout.boundingRect();
            if (device.map(clip.topLeft()).toPoint() - pixels.topLeft() !=
                    localDevice.map(clip.topLeft()).toPoint() ||
                device.map(clip.bottomRight()).toPoint() - pixels.topLeft() !=
                    localDevice.map(clip.bottomRight()).toPoint()) {
                return false;
            }
        }
        ScreenshotOcrGlyphRasterCache::Entry* raster = nullptr;
        for (auto& entry : m_rasters) {
            if (!entry.image.isNull() && entry.deviceTransform == device &&
                entry.selection.start == m_selection.start &&
                entry.selection.length == m_selection.length && entry.highlight == highlight &&
                entry.selectedText == selectedText && entry.selectionOnly == selectionOnly &&
                entry.hints == painter->renderHints()) {
                raster = &entry;
                m_cache.touch(entry);
                ++m_cache.hits;
                break;
            }
        }
        if (raster == nullptr) {
            raster =
                m_rasters[0].lastUse <= m_rasters[1].lastUse ? m_rasters.data() : &m_rasters[1];
            if (!m_cache.allocate(*raster, pixels.size())) {
                return false;
            }
            raster->deviceTransform = device;
            raster->pixels = pixels;
            raster->selection = m_selection;
            raster->highlight = highlight;
            raster->selectedText = selectedText;
            raster->selectionOnly = selectionOnly;
            raster->hints = painter->renderHints();
            raster->image.fill(Qt::transparent);
            QPainter glyphs(&raster->image);
            glyphs.setRenderHints(painter->renderHints());
            glyphs.setWorldTransform(localDevice);
            layout.paint(&glyphs, highlight, selectedText,
                         selectionOnly ? ScreenshotOcrTextLayout::PaintMode::SelectionOnly
                                       : ScreenshotOcrTextLayout::PaintMode::TextOnly);
            glyphs.end();
            m_cache.trimTransparentMargins(*raster);
            ++m_cache.builds;
        }
        if (!selectionOnly && !m_selection.empty()) {
            // Keep the original selection AA and clipping on the destination. Combining an AA
            // background and glyph in one transparent raster adds another rounding step where
            // their edges overlap. A foreground-only raster retains one glyph AA operation.
            paintBackground(painter, highlight, selectedText, *raster);
#if defined(SNOW_SHOT_BENCH_INTERNALS)
            ++m_cache.backgroundPaints;
#endif
        }
        painter->save();
        // Cancel the item and view transforms explicitly. Multiplying an inverse projective
        // transform into the current matrix can leave tiny perspective terms, which makes Qt
        // resample an otherwise integer-aligned image instead of taking its pixel-copy path.
        painter->setWorldTransform(QTransform());
        painter->setViewTransformEnabled(false);
        painter->setWorldTransform(painter->deviceTransform().inverted());
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter->drawImage(raster->pixels.topLeft() + raster->imageOffset, raster->image);
        painter->restore();
        return true;
    }

    void paintBackground(QPainter* painter, const QColor& highlight, const QColor& selectedText,
                         const ScreenshotOcrGlyphRasterCache::Entry& paintedGlyph) {
        if (!m_cache.backgroundEnabled) {
            layout.paint(painter, highlight, selectedText,
                         ScreenshotOcrTextLayout::PaintMode::SelectionOnly);
            return;
        }
        ScreenshotOcrGlyphRasterCache::Entry* background = nullptr;
        for (auto& entry : m_backgroundRasters) {
            if (entry.retained && entry.selection.start == m_selection.start &&
                entry.selection.length == m_selection.length && entry.highlight == highlight &&
                entry.hints == painter->renderHints() &&
                entry.deviceTransform == paintedGlyph.deviceTransform) {
                background = &entry;
                m_cache.touch(entry);
                ++m_cache.backgroundHits;
                break;
            }
        }
        if (background == nullptr) {
            auto& entry = m_backgroundRasters[0].lastUse <= m_backgroundRasters[1].lastUse
                              ? m_backgroundRasters[0]
                              : m_backgroundRasters[1];
            ++m_cache.backgroundBuilds;
            if (!m_cache.allocate(entry, paintedGlyph.pixels.size(), false, true, &paintedGlyph)) {
                layout.paint(painter, highlight, selectedText,
                             ScreenshotOcrTextLayout::PaintMode::SelectionOnly);
                return;
            }
            entry.selection = m_selection;
            entry.highlight = highlight;
            entry.hints = painter->renderHints();
            entry.deviceTransform = paintedGlyph.deviceTransform;
            entry.pixels = paintedGlyph.pixels;
            entry.image.fill(Qt::transparent);
            // Prepare the whole background independently of exposure and keep foreground
            // blending separate. QTextLayout's otherwise empty text passes install many
            // expensive clips even when NoPen suppresses their glyphs.
            QPainter recorder(&entry.image);
            recorder.setRenderHints(entry.hints);
            recorder.setWorldTransform(
                entry.deviceTransform *
                QTransform::fromTranslate(-entry.pixels.x(), -entry.pixels.y()));
            layout.paint(&recorder, highlight, selectedText,
                         ScreenshotOcrTextLayout::PaintMode::SelectionOnly);
            recorder.end();
            m_cache.trimTransparentMargins(entry);
            background = &entry;
        }
        painter->save();
        painter->setWorldTransform(QTransform());
        painter->setViewTransformEnabled(false);
        painter->setWorldTransform(painter->deviceTransform().inverted());
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter->drawImage(background->pixels.topLeft() + background->imageOffset,
                           background->image);
        painter->restore();
    }

    ScreenshotOcrTextRange m_selection;
    ScreenshotOcrGlyphRasterCache& m_cache;
    const QWidget& m_appearance;
    std::array<ScreenshotOcrGlyphRasterCache::Entry, 2> m_rasters;
    std::array<ScreenshotOcrGlyphRasterCache::Entry, 2> m_backgroundRasters;
};

namespace {
constexpr double kSelectionBorderWidth = 2.0;
constexpr double kSelectionHandleRadius = 4.0;
constexpr double kSelectionHandleStrokeWidth = 1.5;
constexpr double kShowEndHandlesMinSize = 32.0;
constexpr double kShowMidHandlesMinSize = 64.0;
constexpr int kSelectionBorderUpdatePadding = 3;
constexpr int kSelectionHandleUpdatePadding = 6;
constexpr int kGuideLineUpdatePadding = 1;
constexpr int kOcrSelectionPreviewAlpha = 0x33;

QPen ocrSelectionPreviewPen(const QColor& color) {
    QPen pen(color, 1.0, Qt::SolidLine);
    pen.setJoinStyle(Qt::MiterJoin);
    return pen;
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
thread_local QRegion g_selectionDamageRegion;
thread_local std::size_t g_selectionDamagePathFallbacks = 0;
thread_local std::size_t g_selectionBackgroundTileBuilds = 0;
thread_local std::size_t g_selectionPreviewMaskBuilds = 0;
thread_local std::size_t g_selectionGeometryBuilds = 0;
thread_local QRegion g_guideLineDamageRegion;
thread_local std::size_t g_guideLineUpdateRequests = 0;

std::size_t regionPixelCount(const QRegion& region) {
    std::size_t pixels = 0;
    for (const QRect& rectangle : region) {
        pixels += static_cast<std::size_t>(rectangle.width()) *
                  static_cast<std::size_t>(rectangle.height());
    }
    return pixels;
}
#endif

QPainterPath selectionShapePath(const QRectF& selection, int cornerRadius,
                                const QTransform& canvasToViewTransform, qreal inset = 0.0) {
    QRectF viewRect = canvasToViewTransform.mapRect(selection.normalized());
    viewRect.adjust(inset, inset, -inset, -inset);

    QPainterPath path;
    if (!viewRect.isValid() || viewRect.isEmpty()) {
        return path;
    }

    const qreal canvasRadius = std::min<qreal>(
        std::max(0, cornerRadius), std::min(selection.width(), selection.height()) / 2.0);
    if (canvasRadius <= 0.0) {
        path.addRect(viewRect);
        return path;
    }

    const qreal horizontalScale =
        std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12());
    const qreal verticalScale =
        std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22());
    const qreal horizontalRadius =
        std::clamp(canvasRadius * horizontalScale - inset, 0.0, viewRect.width() / 2.0);
    const qreal verticalRadius =
        std::clamp(canvasRadius * verticalScale - inset, 0.0, viewRect.height() / 2.0);
    path.addRoundedRect(viewRect, horizontalRadius, verticalRadius, Qt::AbsoluteSize);
    return path;
}

bool rectFCovers(const QRectF& outer, const QRect& inner) {
    if (!inner.isValid() || inner.isEmpty() || !outer.isValid() || outer.isEmpty()) {
        return false;
    }
    const QRectF innerBounds(inner);
    return outer.left() <= innerBounds.left() && outer.top() <= innerBounds.top() &&
           outer.right() >= innerBounds.right() && outer.bottom() >= innerBounds.bottom();
}

bool paintPixelAlignedRectangularMask(QPainter& painter, const QRectF& viewport,
                                      const QRectF& selection, const QColor& color) {
    const QTransform device = painter.deviceTransform();
    if (device.type() > QTransform::TxScale || device.m11() <= 0 || device.m22() <= 0 ||
        !viewport.contains(selection))
        return false;
    const auto pixelAligned = [&](const QRectF& rect) {
        const QRectF pixels = device.mapRect(rect);
        const std::array edges{pixels.left(), pixels.top(), pixels.right(), pixels.bottom()};
        return std::all_of(edges.begin(), edges.end(), [](qreal edge) {
            return std::isfinite(edge) && std::abs(edge) < std::numeric_limits<int>::max() &&
                   std::abs(edge - std::round(edge)) <= 1e-8;
        });
    };
    if (!pixelAligned(viewport) || !pixelAligned(selection))
        return false;

    // Adjacent antialiased strips would blend twice at fractional joins. Only
    // integral device edges use this path; fractional coverage uses the original
    // odd-even rasterizer below, retaining its exact edge samples.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    const std::array strips{
        QRectF(viewport.left(), viewport.top(), viewport.width(), selection.top() - viewport.top()),
        QRectF(viewport.left(), selection.bottom(), viewport.width(),
               viewport.bottom() - selection.bottom()),
        QRectF(viewport.left(), selection.top(), selection.left() - viewport.left(),
               selection.height()),
        QRectF(selection.right(), selection.top(), viewport.right() - selection.right(),
               selection.height())};
    for (const QRectF& strip : strips) {
        if (!strip.isEmpty())
            painter.fillRect(strip, color);
    }
    painter.restore();
    return true;
}

QRect interiorPixelRect(const QRectF& rect) {
    // Rounding outwards can incorrectly claim a pixel across a fractional seam
    // or display gap. Only complete logical pixels contribute to the union.
    const int left = qCeil(rect.left());
    const int top = qCeil(rect.top());
    return QRect(left, top, qFloor(rect.right()) - left, qFloor(rect.bottom()) - top);
}

bool hasSelectionBounds(const ScreenshotSelectionVisualState& state) {
    return state.present && state.bounds.isValid() && !state.bounds.isEmpty();
}

bool selectionEffectsPreviewVisible(const ScreenshotSelectionVisualState& state) {
    return hasSelectionBounds(state) && (state.toolbarHovered || state.effectPreviewVisible) &&
           (state.region || state.cornerRadius > 0 || state.effectWidth > 0);
}

QRectF selectionPreviewBounds(const ScreenshotSelectionVisualState& state,
                              const QTransform& canvasToViewTransform) {
    const qreal padding = std::max(0, state.effectWidth);
    return canvasToViewTransform.mapRect(
        state.bounds.adjusted(-padding, -padding, padding, padding));
}

QRectF mappedSelectionBounds(const ScreenshotSelectionVisualState& state,
                             const QTransform& canvasToViewTransform) {
    return hasSelectionBounds(state) ? canvasToViewTransform.mapRect(state.bounds.normalized())
                                     : QRectF();
}

qreal viewScale(const QTransform& canvasToViewTransform) {
    return std::max<qreal>(
        1.0e-6, std::min(std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12()),
                         std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22())));
}

QRegion selectionStateHandleRegion(const ScreenshotSelectionVisualState& state,
                                   const QRect& viewportRect,
                                   const QTransform& canvasToViewTransform) {
    if (!state.present || state.toolbarHovered || !state.handlesVisible) {
        return {};
    }
    const QRectF selectionBounds = mappedSelectionBounds(state, canvasToViewTransform);
    if (!selectionBounds.isValid() || selectionBounds.isEmpty()) {
        return {};
    }
    const double minSide = std::min(selectionBounds.width(), selectionBounds.height());
    std::array<QPointF, 8> handles{};
    std::size_t handleCount = 0;
    if (state.cornerRadius <= 0 && minSide > kShowEndHandlesMinSize) {
        handles[handleCount++] = selectionBounds.topLeft();
        handles[handleCount++] = selectionBounds.topRight();
        handles[handleCount++] = selectionBounds.bottomRight();
        handles[handleCount++] = selectionBounds.bottomLeft();
    }
    if (minSide > kShowMidHandlesMinSize) {
        handles[handleCount++] = QPointF(selectionBounds.center().x(), selectionBounds.top());
        handles[handleCount++] = QPointF(selectionBounds.right(), selectionBounds.center().y());
        handles[handleCount++] = QPointF(selectionBounds.center().x(), selectionBounds.bottom());
        handles[handleCount++] = QPointF(selectionBounds.left(), selectionBounds.center().y());
    }
    QRegion damage;
    for (std::size_t index = 0; index < handleCount; ++index) {
        damage += QRectF(handles[index].x() - kSelectionHandleUpdatePadding,
                         handles[index].y() - kSelectionHandleUpdatePadding,
                         kSelectionHandleUpdatePadding * 2, kSelectionHandleUpdatePadding * 2)
                      .toAlignedRect()
                      .intersected(viewportRect);
    }
    return damage;
}

QRegion selectionEffectEditorRegion(const ScreenshotSelectionVisualState& state,
                                    const QRect& viewportRect, const QTransform& canvasToView) {
    if (!state.present || !state.effectEditorsVisible || state.toolbarHovered)
        return {};
    const auto layout = screenshotSelectionEffectLayout(state.bounds, state.cornerRadius,
                                                        canvasToView, viewportRect);
    if (!layout.available)
        return {};
    const qreal shadowPadding = 9.0 * kScreenshotSelectionShadowControlScale;
    QRegion damage(QRectF(layout.shadowAnchor, layout.shadow)
                       .normalized()
                       .adjusted(-shadowPadding, -shadowPadding, shadowPadding, shadowPadding)
                       .toAlignedRect());
    const auto handle = screenshotSelectionRadiusHandle(state.activeEffectHandle)
                            ? state.activeEffectHandle
                            : state.hoveredEffectHandle;
    if (screenshotSelectionRadiusHandle(handle))
        damage += QRectF(layout.position(handle) - QPointF(8, 8), QSizeF(16, 16)).toAlignedRect();
    return damage.intersected(viewportRect);
}

QRegion selectionStateDecorationRegion(const ScreenshotSelectionVisualState& state,
                                       const QRect& viewportRect,
                                       const QTransform& canvasToViewTransform) {
    const QRectF selectionBounds = mappedSelectionBounds(state, canvasToViewTransform);
    if (!selectionBounds.isValid() || selectionBounds.isEmpty()) {
        return {};
    }
    const qreal scale = viewScale(canvasToViewTransform);
    const qreal shadow =
        selectionEffectsPreviewVisible(state) ? std::max(0, state.effectWidth) * scale : 0.0;
    const qreal padding = shadow + kSelectionBorderUpdatePadding;
    QRegion decoration(selectionBounds.adjusted(-padding, -padding, padding, padding)
                           .toAlignedRect()
                           .intersected(viewportRect));
    const QRect stableInterior =
        selectionBounds
            .adjusted(kSelectionBorderUpdatePadding, kSelectionBorderUpdatePadding,
                      -kSelectionBorderUpdatePadding, -kSelectionBorderUpdatePadding)
            .toAlignedRect();
    if (!stableInterior.isEmpty()) {
        decoration -= QRegion(stableInterior.intersected(viewportRect));
    }

    decoration += selectionStateHandleRegion(state, viewportRect, canvasToViewTransform);
    decoration += selectionEffectEditorRegion(state, viewportRect, canvasToViewTransform);
    return decoration;
}

QRegion selectionStateMaskRegion(const ScreenshotSelectionVisualState& state,
                                 const QRect& viewportRect,
                                 const QTransform& canvasToViewTransform) {
    if (!hasSelectionBounds(state)) {
        return {};
    }
    const QRectF mapped = canvasToViewTransform.mapRect(state.bounds.normalized());
    return QRegion(mapped.adjusted(-1.0, -1.0, 1.0, 1.0).toAlignedRect().intersected(viewportRect));
}

QRegion selectionStateRoundedCornerRegion(const ScreenshotSelectionVisualState& state,
                                          const QRect& viewportRect,
                                          const QTransform& canvasToViewTransform) {
    if (!hasSelectionBounds(state) || state.cornerRadius <= 0) {
        return {};
    }

    const QRectF mapped = mappedSelectionBounds(state, canvasToViewTransform);
    if (!mapped.isValid() || mapped.isEmpty()) {
        return {};
    }

    const qreal canvasRadius = std::min<qreal>(
        state.cornerRadius, std::min(state.bounds.width(), state.bounds.height()) / 2.0);
    const qreal horizontalScale =
        std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12());
    const qreal verticalScale =
        std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22());
    const qreal radiusX = std::min(mapped.width() / 2.0, canvasRadius * horizontalScale);
    const qreal radiusY = std::min(mapped.height() / 2.0, canvasRadius * verticalScale);
    constexpr qreal padding = 2.0;
    QRegion corners;
    corners += QRectF(mapped.left() - padding, mapped.top() - padding, radiusX + padding * 2.0,
                      radiusY + padding * 2.0)
                   .toAlignedRect();
    corners += QRectF(mapped.right() - radiusX - padding, mapped.top() - padding,
                      radiusX + padding * 2.0, radiusY + padding * 2.0)
                   .toAlignedRect();
    corners += QRectF(mapped.left() - padding, mapped.bottom() - radiusY - padding,
                      radiusX + padding * 2.0, radiusY + padding * 2.0)
                   .toAlignedRect();
    corners += QRectF(mapped.right() - radiusX - padding, mapped.bottom() - radiusY - padding,
                      radiusX + padding * 2.0, radiusY + padding * 2.0)
                   .toAlignedRect();
    return corners.intersected(viewportRect);
}

QColor normalizedGuideLineColor(const QColor& color) {
    return color.isValid() && color.alpha() > 0 ? color : QColor(0, 0, 0, 0);
}

QPoint guideLinePixelPosition(const QPointF& position) {
    return QPoint(qFloor(position.x()), qFloor(position.y()));
}

QRegion guideLineVerticalRegion(const QRect& viewportRect, int x) {
    if (viewportRect.isEmpty()) {
        return {};
    }

    return QRegion(QRect(x - kGuideLineUpdatePadding, viewportRect.top(),
                         kGuideLineUpdatePadding * 2 + 1, viewportRect.height()))
        .intersected(viewportRect);
}

QRegion guideLineHorizontalRegion(const QRect& viewportRect, int y) {
    if (viewportRect.isEmpty()) {
        return {};
    }

    return QRegion(QRect(viewportRect.left(), y - kGuideLineUpdatePadding, viewportRect.width(),
                         kGuideLineUpdatePadding * 2 + 1))
        .intersected(viewportRect);
}

QRegion guideLineCrosshairRegion(const QRect& viewportRect, const QPoint& center) {
    return guideLineVerticalRegion(viewportRect, center.x()) +
           guideLineHorizontalRegion(viewportRect, center.y());
}

QPoint monitorCenterGuideLinePosition(const QRect& viewportRect) {
    const QPointF center = QRectF(viewportRect).center();
    return guideLinePixelPosition(center);
}

std::optional<QPoint> selectionCenterGuideLinePosition(const ScreenshotSelectionVisualState& state,
                                                       const QTransform& canvasToViewTransform) {
    if (!state.present || state.bounds.isEmpty()) {
        return std::nullopt;
    }
    return guideLinePixelPosition(canvasToViewTransform.map(state.bounds.center()));
}

QRegion planCrosshairDamage(const QRect& viewportRect, const std::optional<QPoint>& previous,
                            const std::optional<QPoint>& next, bool colorChanged = false) {
    QRegion dirtyRegion;
    const bool visibilityChanged = previous.has_value() != next.has_value();
    if (colorChanged || visibilityChanged || (previous && next && previous->x() != next->x())) {
        if (previous) {
            dirtyRegion += guideLineVerticalRegion(viewportRect, previous->x());
        }
        if (next) {
            dirtyRegion += guideLineVerticalRegion(viewportRect, next->x());
        }
    }
    if (colorChanged || visibilityChanged || (previous && next && previous->y() != next->y())) {
        if (previous) {
            dirtyRegion += guideLineHorizontalRegion(viewportRect, previous->y());
        }
        if (next) {
            dirtyRegion += guideLineHorizontalRegion(viewportRect, next->y());
        }
    }
    return dirtyRegion;
}

QRegion planGuideLineDamage(const QRect& viewportRect, const QPoint& previousCursorPosition,
                            const QColor& previousCursorColor,
                            const QColor& previousMonitorCenterColor,
                            const QPoint& nextCursorPosition, const QColor& nextCursorColor,
                            const QColor& nextMonitorCenterColor) {
    QRegion dirtyRegion = planCrosshairDamage(
        viewportRect,
        previousCursorColor.alpha() > 0 ? std::optional(previousCursorPosition) : std::nullopt,
        nextCursorColor.alpha() > 0 ? std::optional(nextCursorPosition) : std::nullopt,
        previousCursorColor != nextCursorColor);
    if (previousMonitorCenterColor != nextMonitorCenterColor &&
        (previousMonitorCenterColor.alpha() > 0 || nextMonitorCenterColor.alpha() > 0)) {
        dirtyRegion +=
            guideLineCrosshairRegion(viewportRect, monitorCenterGuideLinePosition(viewportRect));
    }
    return dirtyRegion;
}

QRegion mappedRegionPixels(const ScreenshotRegionGeometry& geometry,
                           const QTransform& canvasToViewTransform, const QRect& viewportRect) {
    QRegion pixels;
    for (const QRect& rectangle : geometry.rectangles()) {
        pixels += canvasToViewTransform.mapRect(QRectF(rectangle))
                      .toAlignedRect()
                      .intersected(viewportRect);
    }
    return pixels;
}

QRegion shapedPreviewBackgroundPixels(const ScreenshotSelectionVisualState& state,
                                      const QRegion& selectedPixels,
                                      const QTransform& canvasToViewTransform,
                                      const QRect& viewportRect) {
    if (!state.region || !selectionEffectsPreviewVisible(state))
        return {};
    const QRect preview = selectionPreviewBounds(state, canvasToViewTransform)
                              .toAlignedRect()
                              .intersected(viewportRect);
    return QRegion(preview).subtracted(selectedPixels);
}

QRegion regionEdgeDamage(const QRegion& region, int padding, const QRect& viewportRect,
                         int cornerRadius = 0) {
    QRegion edges;
    for (const QRect& rectangle : region) {
        QRegion band(rectangle.adjusted(-padding, -padding, padding, padding));
        const QRect interior = rectangle.adjusted(padding, padding, -padding, -padding);
        if (!interior.isEmpty()) {
            band -= QRegion(interior);
        }
        if (cornerRadius > padding) {
            const int extent = cornerRadius + padding;
            const int side = 2 * extent;
            for (const QPoint& corner : {rectangle.topLeft(), rectangle.topRight(),
                                         rectangle.bottomLeft(), rectangle.bottomRight()}) {
                // Concave corners can round into holes as well as into the
                // selected region, so cover both sides of each vertex.
                const int x = corner.x() + (corner.x() == rectangle.right() ? 1 : 0) - extent;
                const int y = corner.y() + (corner.y() == rectangle.bottom() ? 1 : 0) - extent;
                band += QRect(x, y, side, side);
            }
        }
        edges += band;
    }
    return edges.intersected(viewportRect);
}

QRegion marqueeEdgeDamage(const QRectF& marquee, const QTransform& canvasToViewTransform,
                          const QRect& viewportRect) {
    if (marquee.isEmpty()) {
        return {};
    }
    return regionEdgeDamage(QRegion(canvasToViewTransform.mapRect(marquee).toAlignedRect()), 3,
                            viewportRect);
}

QRegion boundedSelectionDamage(const QRegion& damage, const QRect& viewport) {
    if (damage.rectCount() <= 128)
        return damage.intersected(viewport);
    // Hundreds of small contour bands make source blits and raster clips more
    // expensive than moderate overdraw. Coalesce them on a fixed repaint grid.
    constexpr int tileSide = 64;
    QRegion tiles;
    for (const auto& rect : damage) {
        const int left = qFloor(qreal(rect.left()) / tileSide) * tileSide;
        const int top = qFloor(qreal(rect.top()) / tileSide) * tileSide;
        const int right = qCeil(qreal(rect.right() + 1) / tileSide) * tileSide;
        const int bottom = qCeil(qreal(rect.bottom() + 1) / tileSide) * tileSide;
        tiles += QRect(left, top, right - left, bottom - top);
    }
    return tiles.intersected(viewport);
}

QPainterPath exposedSelectionContours(const QPainterPath& path, const QRegion& exposure) {
    const QRectF area = QRectF(exposure.boundingRect()).adjusted(-2, -2, 2, 2);
    if (path.elementCount() < 64 ||
        (exposure.rectCount() == 1 && area.contains(path.controlPointRect())))
        return path;
    // Qt strokes all subpaths before clipping. Reject distant contours first,
    // retaining the exact lines/cubics and every potentially intersecting hole.
    QPainterPath visible;
    visible.setFillRule(path.fillRule());
    for (int first = 0; first < path.elementCount();) {
        const auto start = path.elementAt(first);
        qreal left = start.x, right = start.x, top = start.y, bottom = start.y;
        int end = first + 1;
        while (end < path.elementCount() && !path.elementAt(end).isMoveTo()) {
            const auto point = path.elementAt(end++);
            left = std::min(left, point.x);
            right = std::max(right, point.x);
            top = std::min(top, point.y);
            bottom = std::max(bottom, point.y);
        }
        if (right >= area.left() && left <= area.right() && bottom >= area.top() &&
            top <= area.bottom() &&
            exposure.intersects(QRectF(QPointF(left, top), QPointF(right, bottom))
                                    .adjusted(-2, -2, 2, 2)
                                    .toAlignedRect())) {
            visible.moveTo(start.x, start.y);
            for (int i = first + 1; i < end; ++i) {
                const auto point = path.elementAt(i);
                if (point.isLineTo()) {
                    visible.lineTo(point.x, point.y);
                } else if (point.isCurveTo()) {
                    const auto control = path.elementAt(++i);
                    const auto target = path.elementAt(++i);
                    visible.cubicTo(point.x, point.y, control.x, control.y, target.x, target.y);
                }
            }
        }
        first = end;
    }
    return visible;
}

} // namespace

QRegion planScreenshotGuideLineDamage(const QRect& viewportRect,
                                      const QPoint& previousCursorPosition,
                                      const QColor& previousCursorColor,
                                      const QColor& previousMonitorCenterColor,
                                      const QPoint& nextCursorPosition,
                                      const QColor& nextCursorColor,
                                      const QColor& nextMonitorCenterColor) {
    return planGuideLineDamage(viewportRect, previousCursorPosition, previousCursorColor,
                               previousMonitorCenterColor, nextCursorPosition, nextCursorColor,
                               nextMonitorCenterColor);
}

QRegion planScreenshotSelectionDamage(const ScreenshotSelectionVisualState& previous,
                                      const ScreenshotSelectionVisualState& next,
                                      const QRect& viewportRect,
                                      const QTransform& canvasToViewTransform, bool maskVisible) {
    if (previous == next || viewportRect.isEmpty()) {
        return {};
    }
    if (!previous.snapGuides.isEmpty() || !next.snapGuides.isEmpty()) {
        auto before = previous;
        auto after = next;
        before.snapGuides.clear();
        after.snapGuides.clear();
        QRegion damage = planScreenshotSelectionDamage(before, after, viewportRect,
                                                       canvasToViewTransform, maskVisible);
        if (previous.snapGuides != next.snapGuides) {
            damage += snowCanvasSnapGuideDamage(previous.snapGuides, canvasToViewTransform);
            damage += snowCanvasSnapGuideDamage(next.snapGuides, canvasToViewTransform);
        }
        return damage.intersected(viewportRect);
    }
    if (previous.backgroundOpacityPercent != next.backgroundOpacityPercent) {
        QRegion damage;
        for (const auto* state : {&previous, &next}) {
            if (!selectionEffectsPreviewVisible(*state))
                continue;
            QRegion background(selectionPreviewBounds(*state, canvasToViewTransform)
                                   .toAlignedRect()
                                   .adjusted(-3, -3, 3, 3));
            // Background opacity never changes the selected interior, including
            // translucent capture pixels. Keep a conservative band for rounded
            // corners, fractional pixel coverage and concave region edges.
            if (canvasToViewTransform.type() <= QTransform::TxScale) {
                const qreal scale = std::max(std::abs(canvasToViewTransform.m11()),
                                             std::abs(canvasToViewTransform.m22()));
                const int padding = qCeil(state->cornerRadius * scale) + 3;
                if (!state->region) {
                    background -=
                        interiorPixelRect(canvasToViewTransform.mapRect(state->bounds)
                                              .adjusted(padding, padding, -padding, -padding));
                } else if (!state->region->custom() && state->region->rectCount() <= 512) {
                    for (const QRect& rectangle : state->region->rectangles())
                        background -=
                            interiorPixelRect(canvasToViewTransform.mapRect(QRectF(rectangle))
                                                  .adjusted(padding, padding, -padding, -padding));
                }
            }
            damage += background;
        }
        // Presets can change opacity and geometry together. Preserve the damage
        // for every other state change, including handles outside these bounds.
        auto previousWithNextOpacity = previous;
        previousWithNextOpacity.backgroundOpacityPercent = next.backgroundOpacityPercent;
        damage += planScreenshotSelectionDamage(previousWithNextOpacity, next, viewportRect,
                                                canvasToViewTransform, maskVisible);
        return damage.intersected(viewportRect);
    }
    if (previous.stationaryRegions && next.stationaryRegions && previous.highlightedRegion &&
        next.highlightedRegion && !previous.toolbarHovered && !next.toolbarHovered &&
        !previous.effectPreviewVisible && !next.effectPreviewVisible &&
        canvasToViewTransform.isInvertible()) {
        auto unchanged = previous;
        unchanged.bounds = next.bounds;
        unchanged.region = next.region;
        unchanged.confirmedRegion = next.confirmedRegion;
        unchanged.highlightedRegion = next.highlightedRegion;
        if (unchanged == next) {
            // Only the moving shape can alter the mask or hide/reveal another contour.
            // Repaint its two neighborhoods without touching distant stationary regions.
            const qreal scale =
                std::max(std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12()),
                         std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22()));
            // Nearby edges can constrain a rounded corner within twice its radius;
            // include the corner's own extent as well as the antialiased border.
            const int padding = qCeil(scale * next.cornerRadius * 3) + 3;
            QRegion damage;
            for (const auto& region : {*previous.highlightedRegion, *next.highlightedRegion})
                damage += canvasToViewTransform.mapRect(QRectF(region.boundingRect()))
                              .toAlignedRect()
                              .adjusted(-padding, -padding, padding, padding);
            return boundedSelectionDamage(damage.intersected(viewportRect), viewportRect);
        }
    }
    if (previous.highlightedRegion || next.highlightedRegion) {
        auto before = previous;
        auto after = next;
        before.highlightedRegion.reset();
        after.highlightedRegion.reset();
        QRegion damage = planScreenshotSelectionDamage(before, after, viewportRect,
                                                       canvasToViewTransform, maskVisible);
        if (previous.highlightedRegion != next.highlightedRegion ||
            previous.toolbarHovered != next.toolbarHovered ||
            previous.borderVisible != next.borderVisible) {
            for (const auto& region : {previous.highlightedRegion, next.highlightedRegion}) {
                if (region)
                    damage += canvasToViewTransform.mapRect(QRectF(region->boundingRect()))
                                  .toAlignedRect()
                                  .adjusted(-3, -3, 3, 3);
            }
        }
        return boundedSelectionDamage(damage.intersected(viewportRect), viewportRect);
    }
    if (previous.region || next.region) {
        if (!canvasToViewTransform.isInvertible()) {
            return QRegion(viewportRect);
        }
        const bool rasterRegions = previous.present && next.present && previous.region &&
                                   next.region && !previous.region->custom() &&
                                   !next.region->custom() && previous.region->rectCount() <= 512 &&
                                   next.region->rectCount() <= 512 &&
                                   canvasToViewTransform.type() <= QTransform::TxScale;
        if (rasterRegions) {
            const QRegion previousPixels =
                mappedRegionPixels(*previous.region, canvasToViewTransform, viewportRect);
            const QRegion nextPixels =
                mappedRegionPixels(*next.region, canvasToViewTransform, viewportRect);
            const qreal scale = std::max(std::abs(canvasToViewTransform.m11()),
                                         std::abs(canvasToViewTransform.m22()));
            // Rounded contours differ from their integer region only in a band
            // around its edges. Include that band without repainting the gaps.
            const int padding =
                qCeil(scale *
                      std::max(selectionEffectsPreviewVisible(previous) ? previous.effectWidth : 0,
                               selectionEffectsPreviewVisible(next) ? next.effectWidth : 0)) +
                3;
            const int radius = qCeil(scale * std::max(previous.cornerRadius, next.cornerRadius));
            QRegion dirtyRegion = previousPixels.xored(nextPixels);
            dirtyRegion += regionEdgeDamage(previousPixels, padding, viewportRect, radius);
            dirtyRegion += regionEdgeDamage(nextPixels, padding, viewportRect, radius);
            // Both preview triggers replace the mask in holes, gaps and shadow
            // padding. Those pixels can be far from every selected contour.
            dirtyRegion += shapedPreviewBackgroundPixels(previous, previousPixels,
                                                         canvasToViewTransform, viewportRect)
                               .xored(shapedPreviewBackgroundPixels(
                                   next, nextPixels, canvasToViewTransform, viewportRect));
            // Integer coverage can overlap while the fractional preview clip
            // changes edge coverage, especially when shadow padding expands.
            for (const auto* state : {&previous, &next}) {
                if (selectionEffectsPreviewVisible(*state))
                    dirtyRegion += regionEdgeDamage(
                        QRegion(
                            selectionPreviewBounds(*state, canvasToViewTransform).toAlignedRect()),
                        3, viewportRect);
            }
            if (previous.confirmedRegion != next.confirmedRegion ||
                previous.borderVisible != next.borderVisible ||
                previous.cornerRadius != next.cornerRadius) {
                dirtyRegion +=
                    regionEdgeDamage(mappedRegionPixels(previous.confirmedRegion,
                                                        canvasToViewTransform, viewportRect),
                                     3, viewportRect, radius);
                dirtyRegion += regionEdgeDamage(
                    mappedRegionPixels(next.confirmedRegion, canvasToViewTransform, viewportRect),
                    3, viewportRect, radius);
            }
            dirtyRegion += marqueeEdgeDamage(previous.marquee, canvasToViewTransform, viewportRect);
            dirtyRegion += marqueeEdgeDamage(next.marquee, canvasToViewTransform, viewportRect);
            if (previous.draftPath != next.draftPath ||
                previous.draftVertices != next.draftVertices ||
                previous.subtracting != next.subtracting ||
                previous.dangerColor != next.dangerColor) {
                const QRectF draftBounds =
                    previous.draftPath.boundingRect().united(next.draftPath.boundingRect());
                if (!draftBounds.isEmpty()) {
                    dirtyRegion += QRegion(canvasToViewTransform.mapRect(draftBounds)
                                               .toAlignedRect()
                                               .adjusted(-5, -5, 5, 5));
                }
                for (const QPointF& vertex : previous.draftVertices + next.draftVertices) {
                    const QPointF view = canvasToViewTransform.map(vertex);
                    dirtyRegion += QRectF(view - QPointF(5, 5), QSizeF(10, 10)).toAlignedRect();
                }
            }
            dirtyRegion +=
                selectionStateHandleRegion(previous, viewportRect, canvasToViewTransform);
            dirtyRegion += selectionStateHandleRegion(next, viewportRect, canvasToViewTransform);
            return boundedSelectionDamage(dirtyRegion, viewportRect);
        }
        const QRectF affected = previous.bounds.united(next.bounds)
                                    .united(previous.marquee)
                                    .united(next.marquee)
                                    .united(previous.draftPath.boundingRect())
                                    .united(next.draftPath.boundingRect())
                                    .united(QRectF(previous.confirmedRegion.boundingRect()))
                                    .united(QRectF(next.confirmedRegion.boundingRect()));
        QRectF affectedView = canvasToViewTransform.mapRect(affected);
        for (const auto* state : {&previous, &next}) {
            if (selectionEffectsPreviewVisible(*state))
                affectedView =
                    affectedView.united(selectionPreviewBounds(*state, canvasToViewTransform));
        }
        QRegion dirtyRegion =
            QRegion(affectedView.toAlignedRect().adjusted(-3, -3, 3, 3)).intersected(viewportRect);
        dirtyRegion += selectionStateHandleRegion(previous, viewportRect, canvasToViewTransform);
        dirtyRegion += selectionStateHandleRegion(next, viewportRect, canvasToViewTransform);
        return dirtyRegion;
    }
    if (!canvasToViewTransform.isInvertible()) {
        return QRegion(viewportRect);
    }
    auto decorationOnly = previous;
    decorationOnly.effectEditorsVisible = next.effectEditorsVisible;
    decorationOnly.hoveredEffectHandle = next.hoveredEffectHandle;
    decorationOnly.activeEffectHandle = next.activeEffectHandle;
    if (decorationOnly == next) {
        return selectionEffectEditorRegion(previous, viewportRect, canvasToViewTransform) |
               selectionEffectEditorRegion(next, viewportRect, canvasToViewTransform);
    }
    QRegion dirtyRegion =
        selectionStateDecorationRegion(previous, viewportRect, canvasToViewTransform);
    dirtyRegion += selectionStateDecorationRegion(next, viewportRect, canvasToViewTransform);
    // Shadow changes also repaint the transparent rounded corner squares.
    const bool hoveredShadowChanged =
        (previous.toolbarHovered || next.toolbarHovered || previous.effectPreviewVisible ||
         next.effectPreviewVisible) &&
        (previous.borderType != next.borderType || previous.effectWidth != next.effectWidth ||
         previous.effectColor != next.effectColor);
    if (previous.bounds != next.bounds || previous.cornerRadius != next.cornerRadius ||
        previous.toolbarHovered != next.toolbarHovered ||
        previous.effectPreviewVisible != next.effectPreviewVisible || hoveredShadowChanged) {
        dirtyRegion +=
            selectionStateRoundedCornerRegion(previous, viewportRect, canvasToViewTransform);
        dirtyRegion += selectionStateRoundedCornerRegion(next, viewportRect, canvasToViewTransform);
    }
    if (maskVisible) {
        dirtyRegion +=
            selectionStateMaskRegion(previous, viewportRect, canvasToViewTransform)
                .xored(selectionStateMaskRegion(next, viewportRect, canvasToViewTransform));
    }
    if (previous.draftPath != next.draftPath || previous.draftVertices != next.draftVertices ||
        previous.subtracting != next.subtracting || previous.dangerColor != next.dangerColor) {
        const QRectF draftBounds =
            previous.draftPath.boundingRect().united(next.draftPath.boundingRect());
        if (!draftBounds.isEmpty()) {
            dirtyRegion += QRegion(
                canvasToViewTransform.mapRect(draftBounds).toAlignedRect().adjusted(-5, -5, 5, 5));
        }
    }
    return dirtyRegion.intersected(viewportRect);
}

namespace {
QRectF sourcePixelsForImageLayer(const ScreenshotImageLayer& layer) {
    if (!layer.isValid()) {
        return {};
    }
    const qreal scaleX = layer.image.width() / layer.imageCanvasRect.width();
    const qreal scaleY = layer.image.height() / layer.imageCanvasRect.height();
    return QRectF((layer.destinationCanvasRect.left() - layer.imageCanvasRect.left()) * scaleX,
                  (layer.destinationCanvasRect.top() - layer.imageCanvasRect.top()) * scaleY,
                  layer.destinationCanvasRect.width() * scaleX,
                  layer.destinationCanvasRect.height() * scaleY);
}

bool finiteRect(const QRectF& rect) {
    return std::isfinite(rect.left()) && std::isfinite(rect.top()) && std::isfinite(rect.width()) &&
           std::isfinite(rect.height());
}

void paintImageLayer(QPainter& painter, const ScreenshotImageLayer& layer,
                     const QTransform& canvasToTarget, const QRegion* exposedRegion = nullptr) {
    const QRectF sourcePixels = sourcePixelsForImageLayer(layer);
    if (sourcePixels.isEmpty()) {
        return;
    }
    const QRectF targetRect = canvasToTarget.mapRect(layer.destinationCanvasRect);
    if (exposedRegion != nullptr) {
        paintExposedScreenshotImage(painter, targetRect, layer.image, sourcePixels, *exposedRegion);
    } else {
        painter.drawImage(targetRect, layer.image, sourcePixels);
    }
}

// A pinned result drawn 1:1 in device pixels must stay pixel-exact. Both minification and
// magnification use linear filtering to blend neighboring source pixels.
bool pinnedResultUsesLinearFiltering(const SnowCanvasRenderContext& context,
                                     const QRectF& targetRect, const QSize& sourceSize) {
    if (context.devicePixelRatio <= 0.0) {
        return false;
    }
    const QSize deviceSize(qRound(targetRect.width() * context.devicePixelRatio),
                           qRound(targetRect.height() * context.devicePixelRatio));
    return deviceSize != sourceSize;
}
} // namespace

ScreenshotOcrTextLayer::ScreenshotOcrTextLayer(QWidget* parent,
                                               HighlightingResolution highlightingResolution)
    : QGraphicsView(parent), m_scene(new ScreenshotOcrGraphicsScene(this)),
      m_glyphRasterCache(std::make_unique<ScreenshotOcrGlyphRasterCache>()),
      m_highlightingResolution(highlightingResolution) {
    setObjectName(QStringLiteral("snowShotOcrTextLayer"));
    setScene(m_scene);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setBackgroundBrush(Qt::NoBrush);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    viewport()->setAttribute(Qt::WA_NoSystemBackground, true);
    viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
    viewport()->setAutoFillBackground(false);
    setOptimizationFlag(QGraphicsView::DontSavePainterState, true);
    setRenderHint(QPainter::Antialiasing, true);
    setStyleSheet(QStringLiteral("QGraphicsView#snowShotOcrTextLayer {"
                                 " background: transparent; border: none;"
                                 "}"));
    QObject::connect(&adqt::theme::ThemeManager::instance(),
                     &adqt::theme::ThemeManager::themeChanged, this,
                     [this]() { refreshTextAppearance(); });
    hide();
}

ScreenshotOcrTextLayer::~ScreenshotOcrTextLayer() {
    // Items release their local cache entries while the layer's budget still exists.
    m_textItems.clear();
    m_scene->clear();
}

void ScreenshotOcrTextLayer::changeEvent(QEvent* event) {
    QGraphicsView::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange) {
        refreshTextAppearance();
    } else if (event->type() == QEvent::PaletteChange) {
        viewport()->update();
    }
}

void ScreenshotOcrTextLayer::refreshTextAppearance() {
    const QColor color = adqt::theme::ThemeManager::instance().resolveTheme(this).colorText;
    const QFont font = QApplication::font();
    if (color == m_textColor && font == m_layoutFont) {
        return;
    }
    m_textColor = color;
    m_layoutFont = font;
    if (!m_synchronized || m_presentation == nullptr) {
        return;
    }
    for (TextItem& item : m_textItems) {
        synchronizeTextItem(item, m_canvasToViewTransform);
    }
    m_textHighlightGeometryDirty = true;
    updateTextHighlighting();
    viewport()->update();
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
quint64 ScreenshotOcrTextLayer::highlightingRasterBuildCount() const {
    return m_scene->rasterBuilds;
}

QImage ScreenshotOcrTextLayer::highlightingRasterForTesting() const {
    return m_scene->rasterForTesting();
}

quint64 ScreenshotOcrTextLayer::glyphRasterBuildCount() const {
    return m_glyphRasterCache->builds;
}

quint64 ScreenshotOcrTextLayer::glyphRasterHitCount() const {
    return m_glyphRasterCache->hits;
}

quint64 ScreenshotOcrTextLayer::glyphDirectPaintCount() const {
    return m_glyphRasterCache->directPaints;
}

quint64 ScreenshotOcrTextLayer::glyphBackgroundPaintCount() const {
    return m_glyphRasterCache->backgroundPaints;
}

qsizetype ScreenshotOcrTextLayer::glyphRasterBytes() const {
    return m_glyphRasterCache->bytes() - m_glyphRasterCache->previewBytes() -
           m_glyphRasterCache->backgroundBytes();
}

quint64 ScreenshotOcrTextLayer::backgroundRasterBuildCount() const {
    return m_glyphRasterCache->backgroundBuilds;
}

quint64 ScreenshotOcrTextLayer::backgroundRasterHitCount() const {
    return m_glyphRasterCache->backgroundHits;
}

qsizetype ScreenshotOcrTextLayer::backgroundRasterBytes() const {
    return m_glyphRasterCache->backgroundBytes();
}

quint64 ScreenshotOcrTextLayer::previewRasterBuildCount() const {
    return m_glyphRasterCache->previewBuilds;
}

quint64 ScreenshotOcrTextLayer::previewRasterHitCount() const {
    return m_glyphRasterCache->previewHits;
}

qsizetype ScreenshotOcrTextLayer::previewRasterBytes() const {
    return m_glyphRasterCache->previewBytes();
}

quint64 ScreenshotOcrTextLayer::previewPaintNanoseconds() const {
    return m_selectionPreview != nullptr ? m_selectionPreview->paintNanoseconds() : 0;
}

quint64 ScreenshotOcrTextLayer::marqueePaintNanoseconds() const {
    return m_selectionMarquee != nullptr ? m_selectionMarquee->paintNanoseconds() : 0;
}

QRectF ScreenshotOcrTextLayer::marqueeRectangleForTesting() const {
    return m_selectionMarquee != nullptr ? m_selectionMarquee->rect() : QRectF{};
}

QRegion ScreenshotOcrTextLayer::takeSelectionDamageForTesting() {
    return std::exchange(m_selectionDamage, QRegion{});
}

QPainterPath ScreenshotOcrTextLayer::selectionPreviewPathForTesting(
    ScreenshotOcrLineSelectionPreview preview) const {
    return m_selectionPreview != nullptr && preview != ScreenshotOcrLineSelectionPreview::None
               ? m_selectionPreview->path(preview)
               : QPainterPath{};
}

void ScreenshotOcrTextLayer::setGlyphCachingEnabledForTesting(bool enabled) {
    m_glyphRasterCache->enabled = enabled;
    viewport()->update();
}

void ScreenshotOcrTextLayer::setBackgroundRasterCachingEnabledForTesting(bool enabled) {
    m_glyphRasterCache->backgroundEnabled = enabled;
    viewport()->update();
}

void ScreenshotOcrTextLayer::setRasterMarginTrimmingEnabledForTesting(bool enabled) {
    m_glyphRasterCache->setMarginTrimmingEnabled(enabled);
    viewport()->update();
}

void ScreenshotOcrTextLayer::setRasterCacheBudgetForTesting(qsizetype bytes) {
    m_glyphRasterCache->setByteBudget(bytes);
    viewport()->update();
}
#endif

void ScreenshotOcrTextLayer::setPresentation(
    std::shared_ptr<ScreenshotOcrPresentation> presentation, RenderingMode mode) {
    m_renderingMode = mode;
    m_textItems.clear();
    m_scene->clear();
    m_selectionMarquee = nullptr;
    m_selectionPreview = nullptr;
    m_scene->clearTextHighlighting();
    m_textHighlightMask = {};
    m_textHighlightGeometryDirty = true;
    m_presentation = std::move(presentation);
    const adqt::theme::ThemeMapToken theme =
        adqt::theme::ThemeManager::instance().resolveTheme(this);
    m_textColor = theme.colorText;
    m_layoutFont = QApplication::font();
    if (m_presentation != nullptr) {
        m_presentation->prepareForRendering();
    }
    m_viewportRect = {};
    m_selectionRevision = std::numeric_limits<quint64>::max();
    m_synchronized = false;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    m_selectionDamage = {};
#endif
    rebuildTextItems();
    hide();
}

void ScreenshotOcrTextLayer::clearPresentation() {
    m_textItems.clear();
    m_scene->clear();
    m_selectionMarquee = nullptr;
    m_selectionPreview = nullptr;
    m_scene->clearTextHighlighting();
    m_textHighlightMask = {};
    m_textHighlightGeometryDirty = true;
    m_presentation.reset();
    m_textColor = {};
    m_viewportRect = {};
    m_selectionRevision = std::numeric_limits<quint64>::max();
    m_synchronized = false;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    m_selectionDamage = {};
#endif
    hide();
}

void ScreenshotOcrTextLayer::setTextHighlighting(bool enabled, const QColor& maskColor) {
    const QColor normalized = maskColor.isValid() ? maskColor : QColor(0, 0, 0, 128);
    // Keep only the effective paint color, so disabled/transparent style changes are no-ops.
    const QColor next = enabled && normalized.alpha() > 0 ? normalized : QColor(Qt::transparent);
    if (m_textHighlightColor == next) {
        return;
    }
    m_textHighlightColor = next;
    updateTextHighlighting();
}

void ScreenshotOcrTextLayer::updateTextHighlighting() {
    if (!m_synchronized || m_renderingMode == RenderingMode::SelectionOnly ||
        m_textHighlightColor.alpha() == 0) {
        if (m_highlightingResolution == HighlightingResolution::Image)
            m_scene->clearTextHighlighting();
        else
            m_scene->setTextHighlighting(QColor(Qt::transparent), {});
        return;
    }
    const bool imageResolution = m_highlightingResolution == HighlightingResolution::Image;
    if (m_textHighlightGeometryDirty) {
        QPainterPath textRegions;
        textRegions.setFillRule(Qt::WindingFill);
        for (const TextItem& item : m_textItems) {
            QPolygonF polygon;
            if (imageResolution) {
                // Prepare fitted holes at 100%, including labels currently outside the
                // viewport. Zooming/panning must never rebuild this geometry or raster.
                ScreenshotOcrTextLayout layout;
                QTransform textToImage;
                if (!configureScreenshotOcrTextLayout(
                        layout, m_presentation->lines.at(item.lineIndex), QTransform(),
                        m_layoutFont, m_textColor, {}, &textToImage)) {
                    continue;
                }
                polygon = textToImage.map(QPolygonF(layout.boundingRect()));
            } else {
                if (!item.graphicsText->isVisible()) {
                    continue;
                }
                polygon = item.graphicsText->mapToScene(item.graphicsText->boundingRect());
            }
            // Consistent winding gives union semantics even when mirrored labels overlap.
            qreal area = 0;
            for (qsizetype index = 0; index < polygon.size(); ++index) {
                const QPointF& next = polygon[(index + 1) % polygon.size()];
                area += polygon[index].x() * next.y() - next.x() * polygon[index].y();
            }
            if (area < 0) {
                std::reverse(polygon.begin(), polygon.end());
            }
            textRegions.addPolygon(polygon);
            textRegions.closeSubpath();
        }
        QPainterPath viewport;
        viewport.addRect(imageResolution ? QRectF(m_presentation->selection) : sceneRect());
        m_textHighlightMask = viewport.subtracted(textRegions);
        m_textHighlightGeometryDirty = false;
    }
    m_scene->setTextHighlighting(
        m_textHighlightColor, m_textHighlightMask,
        imageResolution ? std::optional<QTransform>(m_canvasToViewTransform) : std::nullopt,
        imageResolution ? QRectF(m_presentation->selection) : QRectF());
}

void ScreenshotOcrTextLayer::synchronize(const QTransform& canvasToViewTransform,
                                         const QRect& viewportRect) {
    if (m_presentation == nullptr || m_textItems.empty() || viewportRect.isEmpty()) {
        hide();
        return;
    }

    const bool geometryChanged = !m_synchronized ||
                                 m_canvasToViewTransform != canvasToViewTransform ||
                                 m_viewportRect != viewportRect;
    if (geometryChanged && geometry() != viewportRect) {
        setGeometry(viewportRect);
    }
    if (geometryChanged) {
        setSceneRect(QRectF(QPointF(0.0, 0.0), viewportRect.size()));
        m_selectionPreview->setBounds(sceneRect());
        m_selectionMarquee->setBounds(sceneRect());
        // The background mask can damage the entire viewport, beyond the text item bounds.
        m_scene->setSceneRect(sceneRect());
        m_canvasToViewTransform = canvasToViewTransform;
        m_viewportRect = viewportRect;
        m_synchronized = true;
        for (TextItem& item : m_textItems) {
            synchronizeTextItem(item, canvasToViewTransform);
        }
        updateSelectionOverlay();
        if (m_highlightingResolution == HighlightingResolution::Device)
            m_textHighlightGeometryDirty = true;
        updateTextHighlighting();
    }
    updateSelection();
    show();
    raise();
    if (geometryChanged) {
        viewport()->update();
    }
}

void ScreenshotOcrTextLayer::updateLineText(int lineIndex) {
    if (!m_synchronized || lineIndex < 0 || lineIndex >= static_cast<int>(m_textItems.size())) {
        return;
    }
    synchronizeTextItem(m_textItems.at(static_cast<std::size_t>(lineIndex)),
                        m_canvasToViewTransform);
    m_textHighlightGeometryDirty = true;
    updateTextHighlighting();
    updateSelection();
}

void ScreenshotOcrTextLayer::updateSelection() {
    if (m_presentation == nullptr) {
        return;
    }
    if (m_selectionRevision == m_presentation->selectionRevision()) {
        flushSelectionPreviewDamage();
        return;
    }
    const QVector<int>* changedLines = m_presentation->selectionChangesSince(m_selectionRevision);
    if (changedLines != nullptr) {
        for (const int lineIndex : *changedLines) {
            if (lineIndex >= 0 && lineIndex < static_cast<int>(m_textItems.size())) {
                updateSelectionItem(m_textItems.at(static_cast<std::size_t>(lineIndex)));
            }
        }
    } else {
        for (TextItem& item : m_textItems) {
            updateSelectionItem(item);
        }
    }
    updateSelectionOverlay();
    flushSelectionPreviewDamage();
    m_selectionRevision = m_presentation->selectionRevision();
}

void ScreenshotOcrTextLayer::updateSelectionItem(TextItem& item) {
    const quint64 revision = m_presentation->lineSelectionRevision(item.lineIndex);
    if (item.selectionRevision == revision || item.graphicsText == nullptr) {
        return;
    }
    item.selectionRevision = revision;
    const auto preview = m_presentation->lineSelectionPreview(item.lineIndex);
    const bool previewChanged = m_selectionPreview->setPreview(
        item.lineIndex,
        item.graphicsText->isVisible() ? preview : ScreenshotOcrLineSelectionPreview::None);
    // Preview transitions already invalidate the whole quad, including its fitted glyphs.
    // Avoid scheduling a second scene dirty update for the same pixels. Ordinary character
    // selection still schedules the glyph item because no preview damage covers that change.
    item.graphicsText->setSelection(preview == ScreenshotOcrLineSelectionPreview::None
                                        ? m_presentation->textSelectionForLine(item.lineIndex)
                                        : ScreenshotOcrTextRange{},
                                    !previewChanged);
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    ++m_selectionItemUpdateCount;
#endif
}

void ScreenshotOcrTextLayer::flushSelectionPreviewDamage() {
    if (m_selectionPreview != nullptr) {
        const QRegion damage = m_selectionPreview->takeDamage();
        invalidateSelectionRegion(damage);
    }
}

void ScreenshotOcrTextLayer::invalidateSelectionRegion(const QRegion& damage) {
    if (damage.isEmpty()) {
        return;
    }
    // Graphics-item subrectangle updates coalesce into one large dirty rectangle. Preserve
    // sparse quad damage and marquee border bands by invalidating the viewport directly.
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    m_selectionDamage += damage;
#endif
    viewport()->update(damage);
}

void ScreenshotOcrTextLayer::updateSelectionOverlay() {
    const QRectF mapped =
        m_presentation->lineBoxSelectionActive()
            ? m_canvasToViewTransform.mapRect(m_presentation->lineBoxSelectionRect())
            : QRectF{};
    const QRectF rectangle = mapped.isEmpty() ? QRectF{} : mapped;
    if (m_selectionMarquee == nullptr || m_selectionMarquee->rect() == rectangle) {
        return;
    }
    invalidateSelectionRegion(m_selectionMarquee->setRect(rectangle));
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    ++m_overlayUpdateCount;
#endif
}

ScreenshotOcrTextPosition ScreenshotOcrTextLayer::textPositionAt(const QPointF& canvasPosition,
                                                                 bool useClosestLine) const {
    if (m_presentation == nullptr) {
        return {};
    }

    ScreenshotOcrTextPosition position =
        m_presentation->textPositionAt(canvasPosition, useClosestLine);
    if (!position.valid() || !m_synchronized ||
        position.lineIndex >= static_cast<int>(m_textItems.size())) {
        return position;
    }

    const TextItem& item = m_textItems.at(static_cast<std::size_t>(position.lineIndex));
    if (item.graphicsText == nullptr || !item.graphicsText->isVisible()) {
        return position;
    }

    const QPointF viewPosition = m_canvasToViewTransform.map(canvasPosition);
    const QPointF itemPosition = item.graphicsText->mapFromScene(viewPosition);
    position.characterIndex = item.graphicsText->cursorPositionAt(itemPosition);
    return position;
}

void ScreenshotOcrTextLayer::rebuildTextItems() {
    if (m_presentation == nullptr) {
        return;
    }

    m_textItems.reserve(static_cast<std::size_t>(m_presentation->lines.size()));
    m_selectionPreview = new ScreenshotOcrSelectionPreviewItem(
        *m_glyphRasterCache, static_cast<std::size_t>(m_presentation->lines.size()));
    m_scene->addItem(m_selectionPreview);
    m_selectionMarquee = new ScreenshotOcrSelectionMarqueeItem;
    m_selectionMarquee->setPen(ocrSelectionPreviewPen(QColor(0x40, 0x96, 0xff)));
    m_selectionMarquee->setBrush(QColor(0x40, 0x96, 0xff, kOcrSelectionPreviewAlpha));
    m_scene->addItem(m_selectionMarquee);

    const auto appendTextItem = [this](int lineIndex) {
        auto* graphicsText = new ScreenshotOcrGraphicsTextItem(*m_glyphRasterCache, *this);
        graphicsText->selectionOnly = m_renderingMode == RenderingMode::SelectionOnly;
        m_scene->addItem(graphicsText);
        TextItem item;
        item.lineIndex = lineIndex;
        item.graphicsText = graphicsText;
        m_textItems.push_back(std::move(item));
    };

    for (int lineIndex = 0; lineIndex < m_presentation->lines.size(); ++lineIndex) {
        appendTextItem(lineIndex);
    }
}

void ScreenshotOcrTextLayer::synchronizeTextItem(TextItem& item,
                                                 const QTransform& canvasToViewTransform) {
    if (m_presentation == nullptr || item.graphicsText == nullptr || item.lineIndex < 0 ||
        item.lineIndex >= m_presentation->lines.size()) {
        return;
    }

    const ScreenshotOcrLine& line = m_presentation->lines.at(item.lineIndex);
    const QString& text = line.text;
    item.viewQuad = canvasToViewTransform.map(line.quad);
    item.selectionRevision = std::numeric_limits<quint64>::max();
    m_selectionPreview->setQuad(item.lineIndex, item.viewQuad);
    if (text.isEmpty() || item.viewQuad.size() != 4 ||
        std::any_of(item.viewQuad.begin(), item.viewQuad.end(),
                    [](const QPointF& point) {
                        return !std::isfinite(point.x()) || !std::isfinite(point.y());
                    }) ||
        !item.viewQuad.boundingRect().intersects(sceneRect())) {
        item.graphicsText->hide();
        m_selectionPreview->setPreview(item.lineIndex, ScreenshotOcrLineSelectionPreview::None);
        return;
    }

    QTransform transform;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    ++m_geometrySynchronizationCount;
#endif
    if (!item.graphicsText->configure(line, canvasToViewTransform, m_textColor,
                                      m_presentation->lineSelectionPreview(item.lineIndex) ==
                                              ScreenshotOcrLineSelectionPreview::None
                                          ? m_presentation->textSelectionForLine(item.lineIndex)
                                          : ScreenshotOcrTextRange{},
                                      &transform)) {
        item.graphicsText->hide();
        m_selectionPreview->setPreview(item.lineIndex, ScreenshotOcrLineSelectionPreview::None);
        return;
    }
    item.graphicsText->setPos(0.0, 0.0);
    item.graphicsText->setTransform(transform);
    item.graphicsText->show();
    updateSelectionItem(item);
}

bool ScreenshotCanvasRenderer::PathRasterCache::draw(QPainter& painter, const QPainterPath& path,
                                                     const QColor& color, bool exterior,
                                                     const QRect& viewport) {
    // QWidget::render can redirect painting to a differently scaled device.
    // Its actual device transform, including pixel phase, owns raster alignment.
    const auto device = painter.deviceTransform();
    if (device.type() > QTransform::TxScale || device.m11() <= 0 ||
        !qFuzzyCompare(device.m11(), device.m22()))
        return false;
    const qreal scale = device.m11();
    const QRect pixels = device.mapRect(path.boundingRect().adjusted(-2, -2, 2, 2)).toAlignedRect();
    constexpr qsizetype byteLimit = 64 * 1024 * 1024;
    const qint64 imageBytes = qint64(pixels.width()) * pixels.height() * 4;
    if (pixels.isEmpty() || imageBytes > byteLimit)
        return false;
    const QPointF origin = device.inverted().map(QPointF(pixels.topLeft()));
    const auto matchesView = [&](const Entry& entry) {
        return !entry.image.isNull() && entry.viewPath == path && entry.origin == origin &&
               entry.scale == scale && entry.color == color && entry.image.size() == pixels.size();
    };
    if (!matchesView(entries[0])) {
        // A stationary selection shares its cached view path. Normalize only
        // after geometry or device alignment changes, retaining translated reuse.
        const QPainterPath localPath = path.translated(-origin);
        const auto matches = [&](const Entry& entry) {
            return !entry.image.isNull() && entry.path == localPath && entry.scale == scale &&
                   entry.color == color && entry.image.size() == pixels.size();
        };
        if (!matches(entries[0])) {
            if (matches(entries[1])) {
                std::swap(entries[0], entries[1]);
            } else {
                if (imageBytes + entries[0].image.sizeInBytes() <= byteLimit)
                    entries[1] = std::move(entries[0]);
                else
                    entries[1] = {};
                entries[0] = {};
                auto& entry = entries[0];
                entry.path = localPath;
                entry.color = color;
                entry.scale = scale;
                entry.image =
                    snowCanvasAllocateImage(pixels.size(), QImage::Format_ARGB32_Premultiplied);
                if (entry.image.isNull())
                    return false;
                entry.image.setDevicePixelRatio(scale);
                entry.image.fill(Qt::transparent);
                QPainter raster(&entry.image);
                raster.setRenderHint(QPainter::Antialiasing);
                if (exterior) {
                    ScreenshotResultCompositor::fillOutsideSelection(
                        raster, QRectF(QPointF(), QSizeF(pixels.size()) / scale), localPath, color);
                } else {
                    raster.setPen(QPen(color, kSelectionBorderWidth));
                    raster.setBrush(Qt::NoBrush);
                    raster.drawPath(localPath);
                }
            }
        }
        entries[0].viewPath = path;
        entries[0].origin = origin;
    }
    if (exterior) {
        // The cached image covers only the shape bounds. Fill the rest of the
        // viewport on exact physical-pixel boundaries, without overlapping it.
        const QRectF bounds(origin, QSizeF(pixels.size()) / scale);
        const QRectF view(viewport);
        painter.fillRect(QRectF(view.left(), view.top(), view.width(),
                                std::max(qreal(0), bounds.top() - view.top())),
                         color);
        painter.fillRect(QRectF(view.left(), std::max(view.top(), bounds.bottom()), view.width(),
                                std::max(qreal(0), view.bottom() - bounds.bottom())),
                         color);
        const qreal top = std::max(view.top(), bounds.top());
        const qreal height = std::max(qreal(0), std::min(view.bottom(), bounds.bottom()) - top);
        painter.fillRect(
            QRectF(view.left(), top, std::max(qreal(0), bounds.left() - view.left()), height),
            color);
        painter.fillRect(QRectF(std::max(view.left(), bounds.right()), top,
                                std::max(qreal(0), view.right() - bounds.right()), height),
                         color);
    }
    painter.drawImage(origin, entries[0].image);
    return true;
}

ScreenshotCanvasRenderer::ScreenshotCanvasRenderer(SnowCanvasWidget& canvas)
    : m_canvas(canvas), m_canvasGuard(&canvas) {
    m_themeConnection = QObject::connect(&adqt::theme::ThemeManager::instance(),
                                         &adqt::theme::ThemeManager::themeChanged, &canvas,
                                         [&canvas]() { canvas.update(); });
}

ScreenshotCanvasRenderer::~ScreenshotCanvasRenderer() {
    if (m_canvasGuard && m_canvasGuard->customRenderer() == this)
        m_canvasGuard->setCustomRenderer(nullptr);
    QObject::disconnect(m_themeConnection);
    delete m_ocrTextLayer.data();
}

void ScreenshotCanvasRenderer::setRenderMode(RenderMode mode) {
    if (m_renderMode == mode) {
        return;
    }
    m_renderMode = mode;
    if (mode != RenderMode::ScrollingCapture) {
        m_scrollingResultPreviewImage = {};
        m_scrollingResultPreviewCanvasRect = {};
        m_scrollingCropGuide.reset();
    }
    refreshBaseImageSources();
    invalidateCachedContent();
    if (m_renderMode == RenderMode::ScrollingCapture && m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->hide();
    }
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setImage(QImage image, const QRectF& canvasRect) {
    setImageSource(ScreenshotImageSource::fromImage(std::move(image), canvasRect));
}

void ScreenshotCanvasRenderer::setScrollingResultPreview(QImage image, const QRectF& canvasRect,
                                                         std::optional<Qt::Orientation> cropGuide) {
    const QRectF target = canvasRect.normalized();
    if (m_renderMode != RenderMode::ScrollingCapture || image.isNull() || !finiteRect(target) ||
        !target.isValid() || target.isEmpty()) {
        clearScrollingResultPreview();
        return;
    }
    if (m_scrollingResultPreviewImage.cacheKey() == image.cacheKey() &&
        m_scrollingResultPreviewCanvasRect == target && m_scrollingCropGuide == cropGuide) {
        return;
    }
    const QRegion damage = canvasImageDamageRegion(m_scrollingResultPreviewCanvasRect) +
                           canvasImageDamageRegion(target);
    const bool originalChanged = m_scrollingResultPreviewImage.cacheKey() != image.cacheKey() ||
                                 m_scrollingResultPreviewCanvasRect != target;
    if (image.devicePixelRatio() != 1.0) {
        if (!snowCanvasDetachImage(image))
            return;
        image.setDevicePixelRatio(1.0);
    }
    m_scrollingResultPreviewImage = std::move(image);
    m_scrollingResultPreviewCanvasRect = target;
    m_scrollingCropGuide = cropGuide;
    refreshBaseImageSources();
    invalidateCachedContent(originalChanged);
    if (originalChanged)
        m_canvas.updateBaseImageContent(damage);
    else if (!damage.isEmpty())
        m_canvas.update(damage);
}

void ScreenshotCanvasRenderer::clearScrollingResultPreview() {
    if (m_scrollingResultPreviewImage.isNull()) {
        return;
    }
    const QRegion damage = canvasImageDamageRegion(m_scrollingResultPreviewCanvasRect);
    m_scrollingResultPreviewImage = {};
    m_scrollingResultPreviewCanvasRect = {};
    m_scrollingCropGuide.reset();
    refreshBaseImageSources();
    invalidateCachedContent();
    m_canvas.updateBaseImageContent(damage);
}

bool ScreenshotCanvasRenderer::hasScrollingResultPreview() const {
    return !m_scrollingResultPreviewImage.isNull();
}

void ScreenshotCanvasRenderer::setImageSource(ScreenshotImageSource source, const QRectF& damage,
                                              const ScreenshotImageSource& samplingSource) {
    QList<QRectF> opaqueRects;
    // DPR normalization can detach the QImage. Check the shared source key first
    // so every overlay reuses the answer for the same captured pixel buffer.
    if (source.isMaterialized()) {
        if (source.imageIsOpaque(source.materializedImage))
            opaqueRects.append(source.materializedCanvasRect);
    } else if (source.isLayered()) {
        for (const ScreenshotImageLayer& layer : source.layers) {
            if (source.imageIsOpaque(layer.image))
                opaqueRects.append(layer.destinationCanvasRect);
        }
    }
    if (source.isMaterialized() && source.materializedImage.devicePixelRatio() != 1.0) {
        if (!snowCanvasDetachImage(source.materializedImage))
            return;
        source.materializedImage.setDevicePixelRatio(1.0);
    }
    m_imageSource = std::move(source);
    m_samplingImageSource = samplingSource.isValid() ? samplingSource : m_imageSource;
    refreshBaseImageSources();
    m_opaqueImageCanvasRects = std::move(opaqueRects);
    m_opaqueCoverageValid = false;
    QList<SnowCanvasBaseImageSource> baseSources;
    if (m_imageSource.isMaterialized()) {
        baseSources.push_back(
            {m_imageSource.materializedImage, m_imageSource.materializedCanvasRect, {}});
    } else {
        for (const auto& layer : m_imageSource.layers) {
            if (!layer.smartEraseSource)
                continue;
            // An empty coverage means the entire image canvas rect. Keep the
            // same reconstruction key when a plain image gains a cursor layer.
            const QRectF coverage = layer.destinationCanvasRect == layer.imageCanvasRect
                                        ? QRectF{}
                                        : layer.destinationCanvasRect;
            baseSources.push_back({layer.image, layer.imageCanvasRect, coverage});
        }
    }
    if (m_imageSource.isMaterialized()) {
        const QRectF& rect = m_imageSource.materializedCanvasRect;
        static_cast<void>(m_canvas.setDistanceCreationPixelScale(
            QSizeF(m_imageSource.materializedImage.width() / rect.width(),
                   m_imageSource.materializedImage.height() / rect.height())));
    }
    if (damage.isEmpty())
        m_canvas.setBaseImageSources(baseSources);
    else
        m_canvas.setBaseImageSources(baseSources, canvasImageDamageRegion(damage));
    clearOcrFilteredImage();
    invalidateCachedContent();
    if (damage.isEmpty())
        m_canvas.update();
    else
        m_canvas.update(canvasImageDamageRegion(damage));
}

void ScreenshotCanvasRenderer::setImageViewportPhysicalSize(const QSize& size) {
    const QSize normalized = size.isValid() && !size.isEmpty() ? size : QSize();
    if (m_imageViewportPhysicalSize == normalized) {
        return;
    }
    m_imageViewportPhysicalSize = normalized;
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setPinnedAnimationFrame(const QImage& image,
                                                       const QRectF& canvasRect) {
    m_imageSource = ScreenshotImageSource::fromImage(image, canvasRect);
    m_samplingImageSource = m_imageSource;
    m_opaqueImageCanvasRects.clear();
    m_opaqueCoverageValid = false;
    invalidateCachedContent();
    m_canvas.update(canvasImageDamageRegion(canvasRect));
}

void ScreenshotCanvasRenderer::setPinnedViewportRect(const QRectF& rect) {
    const std::optional<QRectF> normalized =
        rect.isValid() && !rect.isEmpty() ? std::optional<QRectF>(rect.normalized()) : std::nullopt;
    if (m_pinnedViewportRect == normalized)
        return;
    m_pinnedViewportRect = normalized;
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setPinnedResultSurface(const QRectF& contentCanvasRect,
                                                      const QRectF& surfaceCanvasRect,
                                                      const ScreenshotResultStyle& style) {
    const auto normalizedStyle = ScreenshotResultCompositor::normalizedStyle(style);
    const bool changed = m_pinnedContentCanvasRect != contentCanvasRect.normalized() ||
                         m_pinnedSurfaceCanvasRect != surfaceCanvasRect.normalized() ||
                         m_pinnedResultStyle.cornerRadius != normalizedStyle.cornerRadius ||
                         m_pinnedResultStyle.borderType != normalizedStyle.borderType ||
                         m_pinnedResultStyle.borderWidth != normalizedStyle.borderWidth ||
                         m_pinnedResultStyle.borderColor != normalizedStyle.borderColor ||
                         m_pinnedResultStyle.shadowWidth != normalizedStyle.shadowWidth ||
                         m_pinnedResultStyle.shadowColor != normalizedStyle.shadowColor ||
                         m_pinnedResultStyle.region != normalizedStyle.region ||
                         m_pinnedResultStyle.regionScale != normalizedStyle.regionScale;
    m_pinnedContentCanvasRect = contentCanvasRect.normalized();
    m_pinnedSurfaceCanvasRect = surfaceCanvasRect.normalized();
    m_pinnedResultStyle = normalizedStyle;
    setRenderMode(RenderMode::PinnedResult);
    if (changed) {
        invalidateCachedContent();
        m_canvas.update();
    }
}

void ScreenshotCanvasRenderer::setPinnedBackgroundColor(const QColor& color) {
    const QColor normalized = color.isValid() ? color : QColor();
    if (m_pinnedBackgroundColor == normalized) {
        return;
    }
    m_pinnedBackgroundColor = normalized;
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setPinnedCheckerboardEnabled(bool enabled) {
    if (m_pinnedCheckerboardEnabled == enabled)
        return;
    m_pinnedCheckerboardEnabled = enabled;
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setMaskVisible(bool visible) {
    if (m_maskVisible == visible) {
        return;
    }
    m_maskVisible = visible;
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setSelectionBorderColor(const QColor& color) {
    const QColor next = color.isValid() ? color : QColor(0x40, 0x96, 0xff);
    if (m_selectionBorderColor == next) {
        return;
    }
    m_selectionBorderColor = next;
    if (m_selectionState.present) {
        m_canvas.update();
    }
}

void ScreenshotCanvasRenderer::setMaskColor(const QColor& color) {
    const QColor next = color.isValid() ? color : QColor(0, 0, 0, 128);
    if (m_maskColor == next) {
        return;
    }
    m_maskColor = next;
    if (m_maskVisible) {
        m_canvas.update();
    }
}

void ScreenshotCanvasRenderer::setGuideLines(const QPointF& cursorPosition,
                                             const QColor& cursorColor,
                                             const QColor& monitorCenterColor) {
    const QColor nextCursorColor = normalizedGuideLineColor(cursorColor);
    const QColor nextMonitorColor = normalizedGuideLineColor(monitorCenterColor);
    const QPoint nextCursorPosition =
        nextCursorColor.alpha() > 0 ? guideLinePixelPosition(cursorPosition) : QPoint();
    const bool nextVisible = nextCursorColor.alpha() > 0 || nextMonitorColor.alpha() > 0 ||
                             m_selectionCenterGuideLineColor.alpha() > 0;
    if (m_guideLineCursorPosition == nextCursorPosition &&
        m_cursorGuideLineColor == nextCursorColor &&
        m_monitorCenterGuideLineColor == nextMonitorColor && m_guideLinesVisible == nextVisible) {
        return;
    }
    const QRegion dirtyRegion = planScreenshotGuideLineDamage(
        m_canvas.rect(), m_guideLineCursorPosition, m_cursorGuideLineColor,
        m_monitorCenterGuideLineColor, nextCursorPosition, nextCursorColor, nextMonitorColor);
    m_guideLineCursorPosition = nextCursorPosition;
    m_cursorGuideLineColor = nextCursorColor;
    m_monitorCenterGuideLineColor = nextMonitorColor;
    m_guideLinesVisible = nextVisible;
    if (!dirtyRegion.isEmpty()) {
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        g_guideLineDamageRegion += dirtyRegion;
        ++g_guideLineUpdateRequests;
#endif
        m_canvas.update(dirtyRegion);
    }
}

void ScreenshotCanvasRenderer::setGuideCursorPosition(const QPointF& cursorPosition) {
    if (m_cursorGuideLineColor.alpha() > 0) {
        setGuideLines(cursorPosition, m_cursorGuideLineColor, m_monitorCenterGuideLineColor);
    }
}

void ScreenshotCanvasRenderer::setSelectionCenterGuideLineColor(const QColor& color) {
    const QColor nextColor = normalizedGuideLineColor(color);
    if (m_selectionCenterGuideLineColor == nextColor) {
        return;
    }
    const auto center =
        selectionCenterGuideLinePosition(m_selectionState, m_canvas.canvasToViewTransform());
    if (center.has_value()) {
        const QRegion dirty = guideLineCrosshairRegion(m_canvas.rect(), *center);
        if (!dirty.isEmpty()) {
            m_canvas.update(dirty);
        }
    }
    m_selectionCenterGuideLineColor = nextColor;
    m_guideLinesVisible = m_cursorGuideLineColor.alpha() > 0 ||
                          m_monitorCenterGuideLineColor.alpha() > 0 || nextColor.alpha() > 0;
}

void ScreenshotCanvasRenderer::clearGuideLines() {
    setGuideLines({}, Qt::transparent, Qt::transparent);
    setSelectionCenterGuideLineColor(Qt::transparent);
}

void ScreenshotCanvasRenderer::setSelection(const QRectF& selection, bool handlesVisible,
                                            int cornerRadius, int shadowWidth,
                                            const QColor& shadowColor) {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.region.reset();
    next.confirmedRegion = {};
    next.highlightedRegion.reset();
    next.stationaryRegions.reset();
    next.snapGuides.clear();
    next.marquee = {};
    next.bounds = selection.normalized();
    next.present = next.bounds.isValid() && !next.bounds.isEmpty();
    next.handlesVisible = handlesVisible;
    next.cornerRadius = next.present ? std::max(0, cornerRadius) : 0;
    next.borderType = ScreenshotSelectionBorderType::Shadow;
    next.effectWidth = next.present ? std::max(0, shadowWidth) : 0;
    next.effectColor = shadowColor.isValid() ? shadowColor : QColor(0x33, 0x33, 0x33);
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::setSelectionRegion(const ScreenshotRegionGeometry& region,
                                                  const ScreenshotRegionGeometry& confirmed,
                                                  const QRectF& marquee, bool subtracting,
                                                  const QColor& danger) {
    auto state = m_selectionState;
    state.highlightedRegion.reset();
    state.stationaryRegions.reset();
    state.snapGuides.clear();
    state.region = region;
    state.confirmedRegion = confirmed;
    state.marquee = marquee;
    state.subtracting = subtracting;
    state.dangerColor = danger;
    state.bounds =
        QRectF(region.boundingRect()).united(QRectF(confirmed.boundingRect())).united(marquee);
    state.present = !state.bounds.isEmpty();
    state.handlesVisible = false;
    applySelectionState(state);
}

void ScreenshotCanvasRenderer::applySelectionState(const ScreenshotSelectionVisualState& state) {
    ScreenshotSelectionVisualState next = state;
    next.bounds = next.bounds.normalized();
    next.present = next.present && next.bounds.isValid() && !next.bounds.isEmpty();
    next.cornerRadius = next.present ? std::max(0, next.cornerRadius) : 0;
    next.effectWidth = next.present ? std::max(0, next.effectWidth) : 0;
    next.backgroundOpacityPercent = std::clamp(next.backgroundOpacityPercent, 0, 100);
    if (!next.effectColor.isValid()) {
        next.effectColor = QColor(0x33, 0x33, 0x33);
    }
    if (next == m_selectionState) {
        return;
    }
    const ScreenshotSelectionVisualState previous = m_selectionState;
    m_selectionState = next;
    // Compound previews now form the scene backdrop. Refresh retained filter
    // tiles when it changes, while keeping pristine capture/eraser sources stable.
    if (m_renderMode == RenderMode::Standard &&
        ((previous.region && selectionEffectsPreviewVisible(previous)) ||
         (next.region && selectionEffectsPreviewVisible(next))))
        invalidateCachedContent(false);
    // Appearance and editor decorations do not change the cached selection paths.
    if (previous.bounds != next.bounds || previous.present != next.present ||
        previous.cornerRadius != next.cornerRadius || previous.region != next.region ||
        previous.confirmedRegion != next.confirmedRegion ||
        previous.stationaryRegions != next.stationaryRegions ||
        previous.draftPath != next.draftPath || previous.subtracting != next.subtracting)
        m_selectionViewGeometry = {};
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    const QTransform canvasToViewTransform = m_canvas.canvasToViewTransform();
    if (!canvasToViewTransform.isInvertible()) {
        ++g_selectionDamagePathFallbacks;
    }
#else
    const QTransform canvasToViewTransform = m_canvas.canvasToViewTransform();
#endif
    QRegion dirtyRegion = planScreenshotSelectionDamage(previous, m_selectionState, m_canvas.rect(),
                                                        canvasToViewTransform, m_maskVisible);
    if (m_selectionCenterGuideLineColor.alpha() > 0) {
        dirtyRegion += planCrosshairDamage(
            m_canvas.rect(), selectionCenterGuideLinePosition(previous, canvasToViewTransform),
            selectionCenterGuideLinePosition(m_selectionState, canvasToViewTransform));
    }
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    g_selectionDamageRegion += dirtyRegion;
#endif
    if (!dirtyRegion.isEmpty()) {
        m_canvas.update(dirtyRegion);
    }
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
ScreenshotSelectionRenderDiagnostics selectionRenderDiagnosticsForCurrentThread() {
    return ScreenshotSelectionRenderDiagnostics{
        regionPixelCount(g_selectionDamageRegion),
        g_selectionDamagePathFallbacks,
        g_selectionBackgroundTileBuilds,
        g_selectionPreviewMaskBuilds,
        g_selectionGeometryBuilds,
    };
}

void resetSelectionRenderDiagnosticsForCurrentThread() {
    g_selectionDamageRegion = {};
    g_selectionDamagePathFallbacks = 0;
    g_selectionBackgroundTileBuilds = 0;
    g_selectionPreviewMaskBuilds = 0;
    g_selectionGeometryBuilds = 0;
}

ScreenshotGuideLineRenderDiagnostics guideLineRenderDiagnosticsForCurrentThread() {
    return ScreenshotGuideLineRenderDiagnostics{
        regionPixelCount(g_guideLineDamageRegion),
        g_guideLineUpdateRequests,
    };
}

void resetGuideLineRenderDiagnosticsForCurrentThread() {
    g_guideLineDamageRegion = {};
    g_guideLineUpdateRequests = 0;
}
#endif

void ScreenshotCanvasRenderer::setSelectionToolbarHovered(bool hovered) {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.toolbarHovered = hovered;
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::setSelectionBorderVisible(bool visible) {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.borderVisible = visible;
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::clearSelection() {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.region.reset();
    next.confirmedRegion = {};
    next.marquee = {};
    next.bounds = QRectF();
    next.present = false;
    next.handlesVisible = true;
    next.cornerRadius = 0;
    next.borderType = ScreenshotSelectionBorderType::Shadow;
    next.effectWidth = 0;
    next.effectColor = QColor(0x33, 0x33, 0x33);
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::setOcrVisible(bool visible) {
    if (m_ocrVisible == visible) {
        return;
    }
    m_ocrVisible = visible;
    if (m_ocrTextLayer != nullptr) {
        if (visible && m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText) {
            m_ocrTextLayer->setPresentation(m_ocrPresentation);
        } else {
            m_ocrTextLayer->clearPresentation();
        }
    }
    invalidateCachedContent(false);
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setOcrPresentation(
    std::shared_ptr<ScreenshotOcrPresentation> presentation, OcrPresentationMode mode) {
    const bool presentationChanged = m_ocrPresentation != presentation;
    const bool hadPresentation = m_ocrPresentation != nullptr;
    const bool modeChanged = m_ocrPresentationMode != mode;
    const QRectF clearedFilteredCanvasRect = m_ocrFilteredCanvasRect;
    const bool hadFilteredImage = !m_ocrFilteredImage.isNull();
    m_ocrPresentation = std::move(presentation);
    if (presentationChanged) {
        clearOcrFilteredImage();
    }
    m_ocrPresentationMode = mode;
    m_ocrBackgroundColor = {};
    if (m_ocrPresentation != nullptr) {
        const adqt::theme::ThemeMapToken theme =
            adqt::theme::ThemeManager::instance().resolveTheme(&m_canvas);
        m_ocrBackgroundColor =
            theme.colorBgContainer.isValid() ? theme.colorBgContainer : QColor(Qt::white);
    }
    invalidateCachedContent(false);
    if (m_ocrVisible && m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText) {
        ensureOcrTextLayer()->setPresentation(m_ocrPresentation);
    } else if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->clearPresentation();
    }
    // The presentation alone repaints pixels only when the text layer relayouts
    // (BackgroundAndText) or when the selection decorations toggle with its
    // presence; otherwise the damage comes from the filtered image, which is set
    // separately with its own targeted update.
    if (m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText || modeChanged ||
        hadPresentation != (m_ocrPresentation != nullptr)) {
        m_canvas.update();
    } else if (hadFilteredImage) {
        const QRegion dirtyRegion = canvasImageDamageRegion(clearedFilteredCanvasRect);
        if (!dirtyRegion.isEmpty()) {
            m_canvas.update(dirtyRegion);
        }
    }
}

QRegion ScreenshotCanvasRenderer::canvasImageDamageRegion(const QRectF& canvasRect) const {
    if (!canvasRect.isValid() || canvasRect.isEmpty()) {
        return {};
    }
    if (!m_canvas.canvasToViewTransform().isInvertible()) {
        // The display cache is not synchronized; repaint everything rather than
        // risk stale content.
        return QRegion(m_canvas.rect());
    }
    // One pixel of padding covers view-space rounding at the rect edges.
    const QRect viewRect = m_canvas.viewRectForCanvasRect(canvasRect, 1);
    if (viewRect.isEmpty()) {
        return {};
    }
    // Preserve offscreen originals until the canvas expands lens/filter dependencies.
    // Direct QWidget updates and the final dependency region are clipped by the canvas.
    return QRegion(viewRect);
}

void ScreenshotCanvasRenderer::setOcrFilteredImage(QImage image, const QRectF& canvasRect) {
    const QRectF previousCanvasRect = m_ocrFilteredCanvasRect;
    QRegion dirtyRegion;
    if (!image.isNull() && canvasRect.isValid() && !canvasRect.isEmpty()) {
        if (image.devicePixelRatio() != 1.0 && !snowCanvasDetachImage(image))
            return;
        image.setDevicePixelRatio(1.0);
        m_ocrFilteredImage = std::move(image);
        m_ocrFilteredCanvasRect = canvasRect.normalized();
        dirtyRegion = canvasImageDamageRegion(previousCanvasRect) +
                      canvasImageDamageRegion(m_ocrFilteredCanvasRect);
    } else {
        dirtyRegion = canvasImageDamageRegion(previousCanvasRect);
        m_ocrFilteredImage = {};
        m_ocrFilteredCanvasRect = {};
    }
    invalidateCachedContent(false);
    if (!dirtyRegion.isEmpty()) {
        m_canvas.update(dirtyRegion);
    }
}

void ScreenshotCanvasRenderer::clearOcrFilteredImage() {
    if (m_ocrFilteredImage.isNull() && !m_ocrFilteredCanvasRect.isValid()) {
        return;
    }
    m_ocrFilteredImage = {};
    m_ocrFilteredCanvasRect = {};
    invalidateCachedContent(false);
}

void ScreenshotCanvasRenderer::updateOcrSelection() {
    if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->updateSelection();
    }
}

ScreenshotOcrTextPosition ScreenshotCanvasRenderer::ocrTextPositionAt(const QPointF& canvasPosition,
                                                                      bool useClosestLine) const {
    if (m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText &&
        m_ocrTextLayer != nullptr) {
        return m_ocrTextLayer->textPositionAt(canvasPosition, useClosestLine);
    }
    return m_ocrVisible && m_ocrPresentation != nullptr
               ? m_ocrPresentation->textPositionAt(canvasPosition, useClosestLine)
               : ScreenshotOcrTextPosition{};
}

void ScreenshotCanvasRenderer::clearOcrPresentation() {
    if (m_ocrPresentation == nullptr && m_ocrFilteredImage.isNull()) {
        return;
    }
    m_ocrPresentation.reset();
    m_ocrFilteredImage = {};
    m_ocrFilteredCanvasRect = {};
    m_ocrBackgroundColor = {};
    m_ocrPresentationMode = OcrPresentationMode::BackgroundAndText;
    invalidateCachedContent(false);
    if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->clearPresentation();
    }
    m_canvas.update();
}

void ScreenshotCanvasRenderer::reset() {
    // Pinned/export snapshots can share geometry with the ending capture.
    // Release only derived data, leaving those snapshots fully usable.
    clearRenderState();
    setOcrVisible(true);
    const bool hadCachedContent =
        m_imageSource.isValid() || !m_imageViewportPhysicalSize.isEmpty() ||
        m_renderMode != RenderMode::Standard || m_ocrPresentation != nullptr ||
        !m_ocrFilteredImage.isNull() || hasScrollingResultPreview();
    const bool hadState = m_imageSource.isValid() || !m_imageViewportPhysicalSize.isEmpty() ||
                          m_renderMode != RenderMode::Standard || m_maskVisible ||
                          m_selectionState.present || m_selectionState.effectWidth > 0 ||
                          m_selectionState.toolbarHovered || !m_selectionState.borderVisible ||
                          m_ocrPresentation != nullptr || !m_ocrFilteredImage.isNull() ||
                          m_guideLinesVisible;
    m_imageSource = {};
    m_samplingImageSource = {};
    m_baseImageSources.clear();
    m_opaqueImageCanvasRects.clear();
    m_opaqueImageViewCoverage = {};
    m_opaqueCoverageValid = false;
    m_scrollingResultPreviewImage = {};
    m_scrollingResultPreviewCanvasRect = {};
    m_scrollingCropGuide.reset();
    m_canvas.setBaseImageSources({});
    m_imageViewportPhysicalSize = QSize();
    m_pinnedViewportRect.reset();
    m_pinnedContentCanvasRect = {};
    m_pinnedSurfaceCanvasRect = {};
    m_pinnedResultStyle = {};
    m_pinnedBackgroundColor = {};
    m_pinnedCheckerboardEnabled = false;
    m_selectionState = ScreenshotSelectionVisualState{};
    m_renderMode = RenderMode::Standard;
    m_maskVisible = false;
    m_guideLineCursorPosition = {};
    m_cursorGuideLineColor = QColor(0, 0, 0, 0);
    m_selectionCenterGuideLineColor = QColor(0, 0, 0, 0);
    m_monitorCenterGuideLineColor = QColor(0, 0, 0, 0);
    m_guideLinesVisible = false;
    m_ocrPresentation.reset();
    m_ocrFilteredImage = {};
    m_ocrFilteredCanvasRect = {};
    m_ocrBackgroundColor = {};
    m_ocrPresentationMode = OcrPresentationMode::BackgroundAndText;
    if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->clearPresentation();
    }
    if (hadCachedContent) {
        invalidateCachedContent();
    }
    if (hadState) {
        m_canvas.update();
    }
}

void ScreenshotCanvasRenderer::clearRenderState() {
    m_selectionBackgroundTiles.clear();
    m_selectionCoverage.clear();
    m_selectionBackgroundCachePath = {};
    m_selectionBackgroundDevicePath = {};
    m_selectionBackgroundInteriorTiles.clear();
    m_selectionBackgroundCacheTransform = {};
    m_selectionBackgroundCacheViewport = {};
    m_selectionBackgroundCacheDpr = 0;
    if (m_selectionState.region)
        m_selectionState.region->clearDerivedCache();
    m_selectionState.confirmedRegion.clearDerivedCache();
    if (m_pinnedResultStyle.region)
        m_pinnedResultStyle.region->clearDerivedCache();
    m_regionShadowRenderer.reset();
    m_outlineCache = {};
    m_maskCache = {};
    m_selectionViewGeometry = {};
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
}

std::uint64_t ScreenshotCanvasRenderer::contentRevision() const {
    return m_contentRevision;
}

std::uint64_t ScreenshotCanvasRenderer::originalBackgroundRevision() const {
    return m_originalBackgroundRevision;
}

QList<SnowCanvasBaseImageSource> ScreenshotCanvasRenderer::baseImageSources() const {
    return m_baseImageSources;
}

void ScreenshotCanvasRenderer::refreshBaseImageSources() {
    m_baseImageSources.clear();
    if (m_renderMode == RenderMode::ScrollingCapture) {
        if (hasScrollingResultPreview())
            m_baseImageSources.append(
                {m_scrollingResultPreviewImage, m_scrollingResultPreviewCanvasRect, {}});
    } else if (m_samplingImageSource.isMaterialized()) {
        m_baseImageSources.append({m_samplingImageSource.materializedImage,
                                   m_samplingImageSource.materializedCanvasRect,
                                   {}});
    } else if (m_samplingImageSource.isLayered()) {
        m_baseImageSources.reserve(m_samplingImageSource.layers.size());
        for (const auto& layer : m_samplingImageSource.layers) {
            if (layer.isValid())
                m_baseImageSources.append(
                    {layer.image, layer.imageCanvasRect, layer.destinationCanvasRect});
        }
    }
}

std::optional<SnowCanvasFilterRenderReference>
ScreenshotCanvasRenderer::filterRenderReference() const {
    if (m_renderMode != RenderMode::PinnedResult || !m_imageSource.isMaterialized() ||
        !m_pinnedSurfaceCanvasRect.isValid()) {
        return std::nullopt;
    }
    return SnowCanvasFilterRenderReference{m_pinnedSurfaceCanvasRect,
                                           m_imageSource.materializedImage.width() /
                                               m_imageSource.materializedCanvasRect.width()};
}

ScreenshotCanvasRenderer::RenderMode ScreenshotCanvasRenderer::renderMode() const {
    return m_renderMode;
}

bool ScreenshotCanvasRenderer::maskVisible() const {
    return m_maskVisible;
}

QColor ScreenshotCanvasRenderer::maskColor() const {
    return m_maskColor;
}

bool ScreenshotCanvasRenderer::guideLinesVisible() const {
    return m_guideLinesVisible;
}

bool ScreenshotCanvasRenderer::hasSelection() const {
    return m_selectionState.present;
}

bool ScreenshotCanvasRenderer::selectionHandlesVisible() const {
    return m_selectionState.handlesVisible;
}

int ScreenshotCanvasRenderer::selectionCornerRadius() const {
    return m_selectionState.cornerRadius;
}

int ScreenshotCanvasRenderer::selectionEffectWidth() const {
    return m_selectionState.effectWidth;
}

bool ScreenshotCanvasRenderer::selectionToolbarHovered() const {
    return m_selectionState.toolbarHovered;
}

bool ScreenshotCanvasRenderer::selectionBorderVisible() const {
    return m_selectionState.borderVisible;
}

QRectF ScreenshotCanvasRenderer::selection() const {
    return m_selectionState.bounds;
}

bool ScreenshotCanvasRenderer::coversWidgetRect(const QRect& widgetRect) const {
    if (!widgetRect.isValid() || widgetRect.isEmpty() || !m_canvas.rect().contains(widgetRect)) {
        return false;
    }

    if (m_renderMode == RenderMode::ScrollingCapture || m_renderMode == RenderMode::PinnedResult) {
        return true;
    }

    if (m_opaqueImageCanvasRects.isEmpty()) {
        return false;
    }

    const QRect canvasRect = m_canvas.rect();
    const qreal devicePixelRatio = m_canvas.devicePixelRatioF();
    if (m_imageViewportPhysicalSize.isValid() && !m_imageViewportPhysicalSize.isEmpty() &&
        devicePixelRatio > 0.0 && m_imageSource.isMaterialized()) {
        const QRectF targetRect(QPointF(canvasRect.topLeft()),
                                QSizeF(m_imageViewportPhysicalSize.width() / devicePixelRatio,
                                       m_imageViewportPhysicalSize.height() / devicePixelRatio));
        return rectFCovers(targetRect, widgetRect);
    }

    const QTransform canvasToView = m_canvas.canvasToViewTransform();
    if (!canvasToView.isInvertible() || canvasToView.type() > QTransform::TxScale) {
        return false;
    }

    if (!m_opaqueCoverageValid || m_opaqueCoverageTransform != canvasToView ||
        m_opaqueCoverageViewport != canvasRect) {
        m_opaqueImageViewCoverage = {};
        for (const QRectF& rect : m_opaqueImageCanvasRects) {
            const QRectF mappedRect = canvasToView.mapRect(rect);
            if (finiteRect(mappedRect)) {
                m_opaqueImageViewCoverage +=
                    interiorPixelRect(mappedRect.intersected(QRectF(canvasRect)));
            }
        }
        m_opaqueCoverageTransform = canvasToView;
        m_opaqueCoverageViewport = canvasRect;
        m_opaqueCoverageValid = true;
    }
    if (m_opaqueImageViewCoverage.rectCount() == 1) {
        return m_opaqueImageViewCoverage.boundingRect().contains(widgetRect);
    }
    // QRegion::contains(QRect) also returns true for a partial intersection.
    return QRegion(widgetRect).subtracted(m_opaqueImageViewCoverage).isEmpty();
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
quint64 ScreenshotCanvasRenderer::ocrGeometrySynchronizationCountForTesting() const {
    return m_ocrTextLayer != nullptr ? m_ocrTextLayer->geometrySynchronizationCount() : 0;
}

#endif

void ScreenshotCanvasRenderer::invalidateCachedContent(bool originalChanged) {
    ++m_contentRevision;
    if (originalChanged) {
        m_selectionBackgroundTiles.clear();
        ++m_originalBackgroundRevision;
    }
}

ScreenshotOcrTextLayer* ScreenshotCanvasRenderer::ensureOcrTextLayer() {
    if (m_ocrTextLayer == nullptr) {
        m_ocrTextLayer = new ScreenshotOcrTextLayer(&m_canvas);
    }
    return m_ocrTextLayer;
}

void ScreenshotCanvasRenderer::renderBeforeCanvas(QPainter& painter,
                                                  const SnowCanvasRenderContext& context) {
    paintBackground(painter, context, false);
    if (context.purpose == SnowCanvasRenderContext::Purpose::ContentSnapshot)
        return;
    if (m_renderMode == RenderMode::PinnedResult && m_pinnedResultStyle.region) {
        const auto content = context.canvasToViewTransform.mapRect(m_pinnedContentCanvasRect);
        const qreal scale =
            std::hypot(context.canvasToViewTransform.m11(), context.canvasToViewTransform.m12());
        const auto path = screenshotSelectionPath(content, m_pinnedResultStyle.region,
                                                  m_pinnedResultStyle.cornerRadius * scale,
                                                  m_pinnedResultStyle.regionScale * scale);
        ScreenshotResultCompositor::maskLiveSurface(
            painter, pinnedViewportForContext(context).intersected(context.viewportRect), path);
        paintPinnedRegionEffects(painter, context);
    }
    if (m_renderMode == RenderMode::Standard && m_selectionState.region &&
        selectionEffectsPreviewVisible(m_selectionState)) {
        selectionViewGeometry(context, m_selectionState.cornerRadius,
                              m_ocrPresentation == nullptr ? m_selectionState.cornerRadius : 0);
        paintSelectionPreview(painter, context);
    }
}

QRectF
ScreenshotCanvasRenderer::pinnedViewportForContext(const SnowCanvasRenderContext& context) const {
    if (!m_pinnedViewportRect)
        return QRectF(context.viewportRect);
    const QTransform canvasToView = m_canvas.canvasToViewTransform();
    if (context.canvasToViewTransform == canvasToView || !canvasToView.isInvertible())
        return *m_pinnedViewportRect;
    // Pristine/filter tiles use a source-pixel view rather than the widget's view. Project the
    // host clip through canvas space so it does not accidentally trim their offscreen backdrop.
    return context.canvasToViewTransform.mapRect(
        canvasToView.inverted().mapRect(*m_pinnedViewportRect));
}

void ScreenshotCanvasRenderer::renderOriginalBackground(QPainter& painter,
                                                        const SnowCanvasRenderContext& context) {
    paintBackground(painter, context, true);
}

void ScreenshotCanvasRenderer::paintSelectionBackground(QPainter& painter,
                                                        const SnowCanvasRenderContext& context,
                                                        const QRectF& bounds) {
    // Flatten overlapping layers (notably the cursor patch) before applying
    // opacity. Per-layer alpha would otherwise reveal the covered capture twice.
    const QRect visible = bounds.toAlignedRect().intersected(context.viewportRect);
    if (visible.isEmpty())
        return;
    auto selectionPath = m_selectionViewGeometry.effectivePath;
    if (m_selectionViewGeometry.rectangular)
        selectionPath.addRect(m_selectionViewGeometry.effectiveRect);
    if (m_selectionBackgroundCachePath != selectionPath ||
        m_selectionBackgroundCacheTransform != context.canvasToViewTransform ||
        m_selectionBackgroundCacheDpr != context.devicePixelRatio ||
        m_selectionBackgroundCacheViewport != context.viewportRect) {
        m_selectionBackgroundTiles.clear();
        m_selectionBackgroundInteriorTiles.clear();
        m_selectionBackgroundDevicePath =
            QTransform::fromScale(context.devicePixelRatio, context.devicePixelRatio)
                .map(selectionPath);
        m_selectionBackgroundCachePath = selectionPath;
        m_selectionBackgroundCacheTransform = context.canvasToViewTransform;
        m_selectionBackgroundCacheDpr = context.devicePixelRatio;
        m_selectionBackgroundCacheViewport = context.viewportRect;
    }
    const qreal dpr = context.devicePixelRatio;
    const auto logicalRect = [dpr](const QRect& pixels) {
        return QRectF(pixels.x() / dpr, pixels.y() / dpr, pixels.width() / dpr,
                      pixels.height() / dpr);
    };
    const auto render = [&](const QRect& pixels) {
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        ++g_selectionBackgroundTileBuilds;
#endif
        auto image =
            snowCanvasAllocateZeroedImage(pixels.size(), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            return image;
        QPainter layerPainter(&image);
        const QTransform toTile(dpr, 0, 0, dpr, -pixels.x(), -pixels.y());
        layerPainter.setTransform(toTile);
        auto full = context;
        full.exposedRegion = logicalRect(pixels).toAlignedRect();
        paintBackground(layerPainter, full, true);
        layerPainter.end();
        image = ScreenshotResultCompositor::maskBackgroundForSelection(image, image,
                                                                       toTile.map(selectionPath));
        image.setDevicePixelRatio(dpr);
        return image;
    };
    // A fixed viewport grid survives changes in shadow padding. Cache only
    // exposed tiles, so large high-DPI previews retain their active working set
    // instead of disabling reuse when a full preview would exceed the byte cap.
    // Align tile joins in device pixels, including non-integral display scales.
    const auto toPixels = QTransform::fromScale(dpr, dpr);
    const QRect viewportPixels = toPixels.mapRect(QRectF(context.viewportRect)).toAlignedRect();
    // Larger tiles reduce draw overhead when the entire viewport fits. Otherwise
    // use smaller tiles to avoid retaining too much unexposed edge padding.
    const int side = qint64(viewportPixels.width()) * viewportPixels.height() * 4 <=
                             m_selectionBackgroundTiles.maxCost()
                         ? 256
                         : 128;
    const QRect visiblePixels = toPixels.mapRect(QRectF(visible)).toAlignedRect();
    const QPoint origin = viewportPixels.topLeft();
    const int firstX = origin.x() + (visiblePixels.left() - origin.x()) / side * side;
    const int firstY = origin.y() + (visiblePixels.top() - origin.y()) / side * side;
    for (int y = firstY; y <= visiblePixels.bottom(); y += side) {
        for (int x = firstX; x <= visiblePixels.right(); x += side) {
            const QRect tile = QRect(x, y, side, side).intersected(viewportPixels);
            const QRectF destination = logicalRect(tile);
            if (!context.exposedRegion.intersects(destination.toAlignedRect().intersected(visible)))
                continue;
            auto interior = m_selectionBackgroundInteriorTiles.constFind(tile.topLeft());
            if (interior == m_selectionBackgroundInteriorTiles.cend())
                interior = m_selectionBackgroundInteriorTiles.insert(
                    tile.topLeft(),
                    m_selectionBackgroundDevicePath.contains(QRectF(tile).adjusted(-1, -1, 1, 1)));
            // Background opacity affects only uncovered pixels. Opaque capture
            // content covers the selected interior; translucent source pixels
            // have their retained background removed by maskBackgroundForSelection.
            // Neither case needs an interior backdrop tile.
            if (*interior)
                continue;
            QImage image;
            if (const auto* cached = m_selectionBackgroundTiles.object(tile.topLeft())) {
                image = *cached;
            } else {
                image = render(tile);
                if (!image.isNull() && image.sizeInBytes() <= m_selectionBackgroundTiles.maxCost())
                    m_selectionBackgroundTiles.insert(tile.topLeft(), new QImage(image),
                                                      image.sizeInBytes());
            }
            painter.drawImage(destination.topLeft(), image);
        }
    }
}

void ScreenshotCanvasRenderer::maskSelectionPreview(QPainter& painter,
                                                    const SnowCanvasRenderContext& context,
                                                    const QRectF& bounds) {
    auto& geometry = m_selectionViewGeometry;
    const auto device = painter.deviceTransform();
    const auto inverse = device.inverted();
    const QRect previewPixels = device.mapRect(bounds).toAlignedRect().intersected(
        device.mapRect(QRectF(context.viewportRect)).toAlignedRect());
    if (previewPixels.isEmpty())
        return;
    auto path = geometry.effectivePath;
    if (geometry.rectangular)
        path.addRect(geometry.effectiveRect);
    // Coverage depends on the selection, not on its shadow padding. Clear the
    // outside separately so width sweeps reuse the same positive-coverage mask.
    const QRect pixels =
        device.mapRect(path.boundingRect())
            .toAlignedRect()
            .intersected(device.mapRect(QRectF(context.viewportRect)).toAlignedRect());
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const QRect& outside : QRegion(previewPixels).subtracted(pixels))
        painter.fillRect(inverse.mapRect(QRectF(outside)), Qt::transparent);
    painter.restore();
    if (pixels.isEmpty())
        return;
    const auto render = [&](const QRect& rect) {
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        ++g_selectionPreviewMaskBuilds;
#endif
        auto mask = snowCanvasAllocateZeroedImage(rect.size(), QImage::Format_Alpha8);
        if (mask.isNull())
            return mask;
        QPainter raster(&mask);
        raster.setRenderHint(QPainter::Antialiasing);
        raster.translate(-rect.topLeft());
        raster.setTransform(device, true);
        raster.fillPath(path, Qt::white);
        return mask;
    };
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    // Rasterize positive coverage exactly as the result compositor does.
    // Filling an inverse path rounds antialiased edge coverage differently.
    auto& coverage = m_selectionCoverage;
    if (coverage.path != path || coverage.pixels != pixels || coverage.transform != device) {
        coverage.clear();
        coverage.path = path;
        coverage.pixels = pixels;
        coverage.transform = device;
        coverage.devicePath = device.map(path);
    }
    constexpr qint64 limit = 16 * 1024 * 1024;
    const qint64 alignedRowBytes = (qint64(pixels.width()) + 3) / 4 * 4;
    if (alignedRowBytes * pixels.height() <= limit) {
        if (coverage.mask.isNull())
            coverage.mask = render(pixels);
        painter.drawImage(inverse.mapRect(QRectF(pixels)), coverage.mask);
    } else {
        // Retain only boundary coverage. Interior pixels are already unchanged by
        // DestinationIn; empty tiles can be cleared directly. Caching either kind
        // needlessly evicts boundary masks when a large preview spans many tiles.
        constexpr int side = 256;
        painter.save();
        // Coverage is already antialiased in physical pixels. Draw its tiles at
        // 1:1 device coordinates, without antialiasing their rectangular joins.
        painter.setWorldTransform(inverse, true);
        painter.setRenderHint(QPainter::Antialiasing, false);
        for (int y = pixels.top(); y <= pixels.bottom(); y += side) {
            for (int x = pixels.left(); x <= pixels.right(); x += side) {
                const QRect tile = QRect(x, y, side, side).intersected(pixels);
                const auto destination = inverse.mapRect(QRectF(tile));
                if (!context.exposedRegion.intersects(destination.toAlignedRect()))
                    continue;
                auto kind = coverage.tileKinds.constFind(tile.topLeft());
                if (kind == coverage.tileKinds.cend()) {
                    const QRectF neighborhood = QRectF(tile).adjusted(-1, -1, 1, 1);
                    const auto classification = coverage.devicePath.contains(neighborhood)
                                                    ? SelectionCoverageCache::TileKind::Interior
                                                : coverage.devicePath.intersects(neighborhood)
                                                    ? SelectionCoverageCache::TileKind::Boundary
                                                    : SelectionCoverageCache::TileKind::Empty;
                    kind = coverage.tileKinds.insert(tile.topLeft(), classification);
                }
                if (*kind == SelectionCoverageCache::TileKind::Interior)
                    continue;
                if (*kind == SelectionCoverageCache::TileKind::Empty) {
                    painter.fillRect(tile, Qt::transparent);
                    continue;
                }
                QImage mask;
                if (const auto* cached = coverage.tiles.object(tile.topLeft())) {
                    mask = *cached;
                } else {
                    mask = render(tile);
                    // Once a frame's boundary working set exceeds the byte cap,
                    // keep the admitted tiles instead of evicting them in scan order.
                    if (!mask.isNull() &&
                        mask.sizeInBytes() <= coverage.tiles.maxCost() - coverage.tiles.totalCost())
                        coverage.tiles.insert(tile.topLeft(), new QImage(mask), mask.sizeInBytes());
                }
                painter.drawImage(tile.topLeft(), mask);
            }
        }
        painter.restore();
    }
}

void ScreenshotCanvasRenderer::paintBackground(QPainter& painter,
                                               const SnowCanvasRenderContext& context,
                                               bool originalOnly) {
    if (m_renderMode == RenderMode::ScrollingCapture || m_renderMode == RenderMode::PinnedResult) {
        painter.save();
        // Replace every covered device pixel, including fractional-DPI edges.
        // Antialiasing a clear leaves residual canvas/parent background alpha.
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(context.viewportRect, QColor(Qt::transparent));
        painter.restore();
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (m_renderMode == RenderMode::ScrollingCapture) {
            if (hasScrollingResultPreview()) {
                const QRectF targetRect =
                    context.canvasToViewTransform.mapRect(m_scrollingResultPreviewCanvasRect);
                if (context.exposedRegion.intersects(targetRect.toAlignedRect())) {
                    painter.save();
                    painter.setRenderHint(
                        QPainter::SmoothPixmapTransform,
                        m_scrollingResultPreviewImage.size() !=
                            QSize(qRound(targetRect.width() * context.devicePixelRatio),
                                  qRound(targetRect.height() * context.devicePixelRatio)));
                    paintExposedScreenshotImage(painter, targetRect, m_scrollingResultPreviewImage,
                                                QRectF(m_scrollingResultPreviewImage.rect()),
                                                context.exposedRegion);
                    if (!originalOnly && m_scrollingCropGuide) {
                        painter.setClipRegion(context.exposedRegion, Qt::IntersectClip);
                        painter.setClipRect(targetRect, Qt::IntersectClip);
                        painter.setRenderHint(QPainter::Antialiasing, false);
                        QPen pen(Qt::red, 1.0);
                        pen.setCosmetic(true);
                        painter.setPen(pen);
                        if (*m_scrollingCropGuide == Qt::Horizontal) {
                            painter.drawLine(QPointF(targetRect.left(), targetRect.center().y()),
                                             QPointF(targetRect.right(), targetRect.center().y()));
                        } else {
                            painter.drawLine(QPointF(targetRect.center().x(), targetRect.top()),
                                             QPointF(targetRect.center().x(), targetRect.bottom()));
                        }
                    }
                    painter.restore();
                }
            }
            return;
        }
    }

    // The engine restores painter state around each host callback; this local clip constrains
    // background rendering. renderAfterCanvas also erases scene/editor pixels outside this clip.
    std::optional<QPainterStateGuard> viewportGuard;
    if (m_renderMode == RenderMode::PinnedResult && m_pinnedViewportRect) {
        viewportGuard.emplace(&painter);
        painter.setClipRect(pinnedViewportForContext(context), Qt::IntersectClip);
    }

    if (!m_imageSource.isValid()) {
        return;
    }

    const bool physicalViewport =
        m_imageViewportPhysicalSize.isValid() && !m_imageViewportPhysicalSize.isEmpty() &&
        context.devicePixelRatio > 0.0 && m_imageSource.isMaterialized() &&
        (!m_pinnedViewportRect ||
         context.canvasToViewTransform == m_canvas.canvasToViewTransform());
    if (physicalViewport) {
        const QRectF targetRect(
            m_renderMode == RenderMode::PinnedResult && m_pinnedViewportRect
                ? m_pinnedViewportRect->topLeft()
                : QPointF(context.viewportRect.topLeft()),
            QSizeF(m_imageViewportPhysicalSize.width() / context.devicePixelRatio,
                   m_imageViewportPhysicalSize.height() / context.devicePixelRatio));
        if (!context.exposedRegion.intersects(targetRect.toAlignedRect())) {
            return;
        }
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              m_imageSource.materializedImage.size() !=
                                  m_imageViewportPhysicalSize);
        paintExposedScreenshotImage(painter, targetRect, m_imageSource.materializedImage,
                                    QRectF(m_imageSource.materializedImage.rect()),
                                    context.exposedRegion);
        painter.restore();
    } else if (m_imageSource.isMaterialized()) {
        const QRectF targetRect =
            context.canvasToViewTransform.mapRect(m_imageSource.materializedCanvasRect);
        if (context.exposedRegion.intersects(targetRect.toAlignedRect())) {
            painter.save();
            if (m_renderMode == RenderMode::PinnedResult) {
                painter.setRenderHint(
                    QPainter::SmoothPixmapTransform,
                    pinnedResultUsesLinearFiltering(context, targetRect,
                                                    m_imageSource.materializedImage.size()));
            }
            paintExposedScreenshotImage(painter, targetRect, m_imageSource.materializedImage,
                                        QRectF(m_imageSource.materializedImage.rect()),
                                        context.exposedRegion);
            painter.restore();
        }
    } else {
        for (const ScreenshotImageLayer& layer : m_imageSource.layers) {
            const QRectF targetRect =
                context.canvasToViewTransform.mapRect(layer.destinationCanvasRect);
            if (context.exposedRegion.intersects(targetRect.toAlignedRect())) {
                paintImageLayer(painter, layer, context.canvasToViewTransform,
                                &context.exposedRegion);
            }
        }
    }
    if (!originalOnly && m_ocrVisible && m_ocrPresentation != nullptr &&
        !m_ocrFilteredImage.isNull()) {
        const QRectF canvasRect = m_ocrFilteredCanvasRect.isValid()
                                      ? m_ocrFilteredCanvasRect
                                      : QRectF(m_ocrPresentation->selection).normalized();
        const QRectF targetRect = context.canvasToViewTransform.mapRect(canvasRect);
        if (!canvasRect.isEmpty() && targetRect.isValid() && !targetRect.isEmpty() &&
            context.exposedRegion.intersects(targetRect.toAlignedRect())) {
            painter.save();
            if (m_renderMode == RenderMode::PinnedResult) {
                painter.setRenderHint(QPainter::SmoothPixmapTransform,
                                      pinnedResultUsesLinearFiltering(context, targetRect,
                                                                      m_ocrFilteredImage.size()));
            }
            painter.setClipRegion(context.exposedRegion, Qt::IntersectClip);
            painter.setClipRect(targetRect, Qt::IntersectClip);
            painter.drawImage(targetRect, m_ocrFilteredImage);
            painter.restore();
        }
    }
}

ScreenshotCanvasRenderer::SelectionViewGeometry&
ScreenshotCanvasRenderer::selectionViewGeometry(const SnowCanvasRenderContext& context,
                                                int cornerRadius, int borderCornerRadius) {
    auto& geometry = m_selectionViewGeometry;
    if (geometry.valid && geometry.canvasToViewTransform == context.canvasToViewTransform &&
        geometry.viewportRect == context.viewportRect && geometry.cornerRadius == cornerRadius &&
        geometry.borderCornerRadius == borderCornerRadius) {
        return geometry;
    }

    const bool shaped = m_selectionState.region.has_value();
    geometry = {};
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    ++g_selectionGeometryBuilds;
#endif
    geometry.canvasToViewTransform = context.canvasToViewTransform;
    geometry.viewportRect = context.viewportRect;
    geometry.cornerRadius = cornerRadius;
    geometry.borderCornerRadius = borderCornerRadius;
    geometry.rectangular = !shaped && m_selectionState.draftPath.isEmpty() && cornerRadius == 0 &&
                           borderCornerRadius == 0;
    geometry.effectiveRect = context.canvasToViewTransform.mapRect(m_selectionState.bounds);
    geometry.outlineRect = geometry.effectiveRect.adjusted(0.5, 0.5, -0.5, -0.5);
    geometry.effectivePath = geometry.rectangular ? QPainterPath{}
                             : shaped ? context.canvasToViewTransform.map(screenshotRegionPath(
                                            *m_selectionState.region, cornerRadius))
                                      : selectionShapePath(m_selectionState.bounds, cornerRadius,
                                                           context.canvasToViewTransform);
    geometry.outlinePath =
        geometry.rectangular ? QPainterPath{}
        : shaped
            ? context.canvasToViewTransform.map(screenshotRegionPath(
                  m_selectionState.confirmedRegion.isEmpty() ? *m_selectionState.region
                                                             : m_selectionState.confirmedRegion,
                  borderCornerRadius))
            : selectionShapePath(m_selectionState.bounds, borderCornerRadius,
                                 context.canvasToViewTransform, 0.5);
    geometry.draftPath = context.canvasToViewTransform.map(m_selectionState.draftPath);
    geometry.maskPath = {};
    if (!geometry.rectangular && geometry.effectivePath.fillRule() == Qt::OddEvenFill) {
        geometry.maskPath.setFillRule(Qt::OddEvenFill);
        geometry.maskPath.addRect(
            QRectF(context.viewportRect).united(geometry.effectivePath.boundingRect()));
        if (m_selectionState.present)
            geometry.maskPath.addPath(geometry.effectivePath);
    }
    // Cache the geometry decisions along with the paths. Cursor motion changes
    // none of these inputs, including potentially large confirmed contours.
    geometry.maskCacheEligible =
        shaped && m_selectionState.present && !m_selectionState.stationaryRegions &&
        m_selectionState.draftPath.isEmpty() && geometry.effectivePath.elementCount() > 32 &&
        *m_selectionState.region == m_selectionState.confirmedRegion;
    geometry.sharedDraftOutline = !m_selectionState.subtracting && !geometry.draftPath.isEmpty() &&
                                  geometry.draftPath == geometry.outlinePath;
    geometry.valid = true;
    return geometry;
}

void ScreenshotCanvasRenderer::paintSelectionPreview(QPainter& painter,
                                                     const SnowCanvasRenderContext& context) {
    const QRectF previewBounds =
        selectionPreviewBounds(m_selectionState, context.canvasToViewTransform);
    const QRectF selectionView = context.canvasToViewTransform.mapRect(m_selectionState.bounds);
    painter.save();
    painter.setClipRect(previewBounds, Qt::IntersectClip);
    maskSelectionPreview(painter, context, previewBounds);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    if (m_selectionState.borderType == ScreenshotSelectionBorderType::Border) {
        const auto path =
            screenshotSelectionPath(m_selectionState.region ? QRectF() : m_selectionState.bounds,
                                    m_selectionState.region, m_selectionState.cornerRadius);
        painter.save();
        painter.setWorldTransform(context.canvasToViewTransform, true);
        ScreenshotSelectionShadowRenderer::renderBorder(painter, path, m_selectionState.effectWidth,
                                                        m_selectionState.effectColor);
        painter.restore();
    } else if (m_selectionState.region) {
        m_regionShadowRenderer.render(painter, *m_selectionState.region,
                                      m_selectionState.cornerRadius, m_selectionState.effectWidth,
                                      m_selectionState.effectColor, context.canvasToViewTransform,
                                      context.exposedRegion);
    } else {
        const qreal scale =
            std::max<qreal>(1.0e-6, std::min(std::hypot(context.canvasToViewTransform.m11(),
                                                        context.canvasToViewTransform.m12()),
                                             std::hypot(context.canvasToViewTransform.m21(),
                                                        context.canvasToViewTransform.m22())));
        ScreenshotSelectionShadowRenderer::renderResultShadow(
            painter, selectionView, m_selectionState.cornerRadius * scale,
            m_selectionState.effectWidth * scale, m_selectionState.effectColor,
            context.devicePixelRatio);
    }
    if (m_selectionState.backgroundOpacityPercent > 0) {
        painter.setOpacity(m_selectionState.backgroundOpacityPercent / 100.0);
        paintSelectionBackground(painter, context, previewBounds);
        painter.setOpacity(1.0);
    }
    painter.restore();
}

void ScreenshotCanvasRenderer::paintPinnedRegionEffects(QPainter& painter,
                                                        const SnowCanvasRenderContext& context) {
    const auto& style = m_pinnedResultStyle;
    if (style.effectWidth() <= 0)
        return;
    const auto path =
        screenshotSelectionPath(QRectF(), style.region, style.cornerRadius, style.regionScale);
    QTransform transform = context.canvasToViewTransform;
    transform.translate(m_pinnedContentCanvasRect.x(), m_pinnedContentCanvasRect.y());
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    if (style.borderType == ScreenshotSelectionBorderType::Border) {
        painter.setWorldTransform(transform, true);
        ScreenshotSelectionShadowRenderer::renderBorder(painter, path, style.borderWidth,
                                                        style.borderColor);
    } else {
        m_regionShadowRenderer.render(
            painter, ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve), 0,
            style.shadowWidth, style.shadowColor, transform, context.exposedRegion);
    }
    painter.restore();
}

void ScreenshotCanvasRenderer::renderAfterCanvas(QPainter& painter,
                                                 const SnowCanvasRenderContext& context) {
    if (context.purpose == SnowCanvasRenderContext::Purpose::ContentSnapshot)
        return;
    if (m_renderMode == RenderMode::PinnedResult) {
        const QRectF viewport = pinnedViewportForContext(context).intersected(context.viewportRect);
        if (m_pinnedViewportRect) {
            painter.save();
            // Erase four margin rectangles after scene and editor rendering. Integral physical
            // edges must be cleared without antialiasing to avoid stale alpha on fractional DPR.
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            const QRectF outer(context.viewportRect);
            painter.fillRect(
                QRectF(outer.left(), outer.top(), outer.width(), viewport.top() - outer.top()),
                Qt::transparent);
            painter.fillRect(QRectF(outer.left(), viewport.bottom(), outer.width(),
                                    outer.bottom() - viewport.bottom()),
                             Qt::transparent);
            painter.fillRect(QRectF(outer.left(), viewport.top(), viewport.left() - outer.left(),
                                    viewport.height()),
                             Qt::transparent);
            painter.fillRect(QRectF(viewport.right(), viewport.top(),
                                    outer.right() - viewport.right(), viewport.height()),
                             Qt::transparent);
            painter.restore();
        }
        QPainterStateGuard viewportGuard(&painter);
        painter.setClipRect(viewport, Qt::IntersectClip);
        const QRectF contentView = context.canvasToViewTransform.mapRect(m_pinnedContentCanvasRect);
        if (m_pinnedResultStyle.region) {
            // The capture mask and interior effects were painted before annotations.
            // Trim document overflow to its bounding rectangle, then restore only
            // the exterior effect margin that this trim erased.
            QPainterPath contentBounds;
            contentBounds.addRect(contentView);
            ScreenshotResultCompositor::maskLiveSurface(painter, viewport, contentBounds);
            QPainterPath margin;
            margin.setFillRule(Qt::OddEvenFill);
            margin.addRect(viewport.united(contentView));
            margin.addRect(contentView);
            painter.save();
            painter.setClipPath(margin, Qt::IntersectClip);
            paintPinnedRegionEffects(painter, context);
            painter.restore();
        } else {
            ScreenshotResultCompositor::finishLiveSurface(
                painter, viewport, contentView, m_pinnedResultStyle, context.devicePixelRatio,
                std::hypot(context.canvasToViewTransform.m11(),
                           context.canvasToViewTransform.m12()));
        }
        // Fill behind the composed image and annotations. The compositor has already
        // cleared the exterior of rounded and shaped results, so an earlier fill
        // would be removed along with those pixels.
        if (m_pinnedCheckerboardEnabled || m_pinnedBackgroundColor.isValid()) {
            painter.save();
            painter.setClipRegion(context.exposedRegion, Qt::IntersectClip);
            painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
            if (m_pinnedCheckerboardEnabled) {
                const QRectF checkerBounds =
                    m_pinnedBackgroundColor.isValid()
                        ? context.canvasToViewTransform.mapRect(m_pinnedSurfaceCanvasRect)
                        : viewport;
                painter.fillRect(checkerBounds.intersected(viewport),
                                 adqt::widgets::themedCheckerboardBrush(&m_canvas));
            }
            if (m_pinnedBackgroundColor.isValid())
                painter.fillRect(viewport, m_pinnedBackgroundColor);
            painter.restore();
        }
        if (m_ocrVisible && m_ocrPresentation != nullptr &&
            m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText &&
            m_ocrTextLayer != nullptr) {
            const QRect textViewport(
                QPoint(qCeil(viewport.left()), qCeil(viewport.top())),
                QPoint(qFloor(viewport.right()) - 1, qFloor(viewport.bottom()) - 1));
            const QTransform textTransform =
                context.canvasToViewTransform *
                QTransform::fromTranslate(-textViewport.x(), -textViewport.y());
            m_ocrTextLayer->synchronize(textTransform, textViewport);
        }
        return;
    }
    if (!m_maskVisible && !m_selectionState.present && m_ocrPresentation == nullptr &&
        !m_guideLinesVisible) {
        return;
    }

    painter.save();
    const int visibleCornerRadius =
        m_renderMode == RenderMode::Standard ? m_selectionState.cornerRadius : 0;
    const int selectionBorderCornerRadius = m_ocrPresentation == nullptr ? visibleCornerRadius : 0;
    const bool shaped = m_selectionState.region.has_value();
    auto& geometry =
        selectionViewGeometry(context, visibleCornerRadius, selectionBorderCornerRadius);
    const auto& effectivePath = geometry.effectivePath;
    const auto& outlinePath = geometry.outlinePath;
    if (visibleCornerRadius > 0 || shaped || !m_selectionState.draftPath.isEmpty()) {
        painter.setRenderHint(QPainter::Antialiasing, true);
    }

    const bool effectsPreview =
        m_renderMode == RenderMode::Standard && selectionEffectsPreviewVisible(m_selectionState);
    const QRectF previewBounds =
        selectionPreviewBounds(m_selectionState, context.canvasToViewTransform);
    painter.save();
    if (effectsPreview) {
        // Preserve the source at curve edges: the preview supplies its own matte.
        QPainterPath outside;
        outside.addRect(context.viewportRect);
        outside.addRect(previewBounds);
        outside.setFillRule(Qt::OddEvenFill);
        painter.setClipPath(outside, Qt::IntersectClip);
    }
    const bool cachedMask =
        m_maskVisible && geometry.maskCacheEligible &&
        // Small masks are cheaper to fill than to fetch from a selection-sized
        // texture. Cache contours whose edge processing can amortize that blit.
        m_maskCache.draw(painter, effectivePath, m_maskColor, true, context.viewportRect);
    if (m_maskVisible && !cachedMask) {
        const bool rectangularMask =
            geometry.rectangular && m_selectionState.present &&
            paintPixelAlignedRectangularMask(painter, QRectF(context.viewportRect),
                                             geometry.effectiveRect, m_maskColor);
        if (!rectangularMask && m_selectionState.present &&
            effectivePath.fillRule() == Qt::WindingFill) {
            ScreenshotResultCompositor::fillOutsideSelection(painter, context.viewportRect,
                                                             effectivePath, m_maskColor);
        } else if (!rectangularMask) {
            if (geometry.maskPath.isEmpty()) {
                geometry.maskPath.setFillRule(Qt::OddEvenFill);
                geometry.maskPath.addRect(QRectF(context.viewportRect));
                if (m_selectionState.present)
                    geometry.maskPath.addRect(geometry.effectiveRect);
            }
            painter.fillPath(
                m_selectionState.stationaryRegions
                    ? exposedSelectionContours(geometry.maskPath, context.exposedRegion)
                    : geometry.maskPath,
                m_maskColor);
        }
    }
    painter.restore();
    if (m_renderMode == RenderMode::Standard && m_guideLinesVisible) {
        const QRectF viewport(context.viewportRect);
        paintScreenshotGuideLines(painter, viewport, QPointF(m_guideLineCursorPosition),
                                  m_cursorGuideLineColor, m_monitorCenterGuideLineColor,
                                  &context.exposedRegion);
        if (m_selectionCenterGuideLineColor.alpha() > 0 && m_selectionState.present) {
            paintScreenshotGuideLineCrosshair(
                painter, viewport,
                context.canvasToViewTransform.map(m_selectionState.bounds.center()),
                m_selectionCenterGuideLineColor, false, &context.exposedRegion);
        }
    }
    const auto& draftViewPath = geometry.draftPath;
    const bool sharedDraftOutline = geometry.sharedDraftOutline;
    if (m_renderMode == RenderMode::Standard && !m_selectionState.draftPath.isEmpty()) {
        const auto color =
            m_selectionState.subtracting ? m_selectionState.dangerColor : m_selectionBorderColor;
        QPen pen(color, kSelectionBorderWidth);
        if (m_selectionState.subtracting)
            pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        // Replacement drafts and their selection outline can be the same curve.
        // Preserve both paint layers, but rasterize that curve only once.
        if (!sharedDraftOutline ||
            !m_outlineCache.draw(painter, draftViewPath, color, false, context.viewportRect))
            painter.drawPath(draftViewPath);
    }
    if (m_renderMode == RenderMode::Standard && m_selectionState.present) {
        const QColor selectionAccent = m_selectionBorderColor;
        const QRectF selectionView = context.canvasToViewTransform.mapRect(m_selectionState.bounds);
        if (effectsPreview) {
            if (!shaped) {
                paintSelectionPreview(painter, context);
            } else if (m_selectionState.effectWidth > 0) {
                // Annotations belong to the overall content bounds, not the
                // added shadow margin. Restore only that margin after drawing.
                painter.save();
                QPainterPath margin;
                margin.addRect(previewBounds);
                margin.addRect(selectionView);
                margin.setFillRule(Qt::OddEvenFill);
                painter.setClipPath(margin, Qt::IntersectClip);
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.fillRect(previewBounds, Qt::transparent);
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                paintBackground(painter, context, false);
                paintSelectionPreview(painter, context);
                painter.restore();
            }
            // Checkerboard is a display aid, never a filter's sampling backdrop.
            painter.save();
            painter.setClipRect(previewBounds, Qt::IntersectClip);
            painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
            painter.fillRect(previewBounds, adqt::widgets::themedCheckerboardBrush(&m_canvas));
            painter.restore();
        }
        if (!m_selectionState.toolbarHovered && m_selectionState.borderVisible) {
            painter.setPen(QPen(selectionAccent, kSelectionBorderWidth));
            painter.setBrush(Qt::NoBrush);
            // Cache in local physical-pixel coordinates. Integer-pixel moves
            // reuse the raster; fractional moves retain their exact sample phase.
            if (geometry.rectangular) {
                if (geometry.outlineRect.isValid() && !geometry.outlineRect.isEmpty())
                    painter.drawRect(geometry.outlineRect);
            } else if (!shaped || m_selectionState.stationaryRegions ||
                       (m_selectionState.confirmedRegion.isEmpty() && !sharedDraftOutline) ||
                       !m_outlineCache.draw(painter, outlinePath, selectionAccent, false,
                                            context.viewportRect)) {
                painter.drawPath(m_selectionState.stationaryRegions
                                     ? exposedSelectionContours(outlinePath, context.exposedRegion)
                                     : outlinePath);
            }
            if (m_selectionState.highlightedRegion) {
                QPen pen(selectionAccent, kSelectionBorderWidth);
                pen.setStyle(Qt::DashLine);
                painter.setPen(pen);
                painter.drawPath(
                    context.canvasToViewTransform.map(m_selectionState.highlightedRegion->path()));
            }
            if (!m_selectionState.marquee.isEmpty()) {
                QPen pen(m_selectionState.subtracting ? m_selectionState.dangerColor
                                                      : selectionAccent,
                         kSelectionBorderWidth);
                if (m_selectionState.subtracting)
                    pen.setStyle(Qt::DashLine);
                painter.setPen(pen);
                painter.drawRect(context.canvasToViewTransform.mapRect(m_selectionState.marquee));
            }
        }

        const double minSide = std::min(selectionView.width(), selectionView.height());
        std::array<QPointF, 8> handles{};
        std::size_t handleCount = 0;
        if (!m_selectionState.toolbarHovered && m_selectionState.handlesVisible &&
            visibleCornerRadius <= 0 && minSide > kShowEndHandlesMinSize) {
            handles[handleCount++] = selectionView.topLeft();
            handles[handleCount++] = selectionView.topRight();
            handles[handleCount++] = selectionView.bottomRight();
            handles[handleCount++] = selectionView.bottomLeft();
        }
        if (!m_selectionState.toolbarHovered && m_selectionState.handlesVisible &&
            minSide > kShowMidHandlesMinSize) {
            handles[handleCount++] = QPointF(selectionView.center().x(), selectionView.top());
            handles[handleCount++] = QPointF(selectionView.right(), selectionView.center().y());
            handles[handleCount++] = QPointF(selectionView.center().x(), selectionView.bottom());
            handles[handleCount++] = QPointF(selectionView.left(), selectionView.center().y());
        }
        if (handleCount != 0) {
            painter.setBrush(selectionAccent);
            painter.setPen(QPen(Qt::white, kSelectionHandleStrokeWidth));
            for (std::size_t index = 0; index < handleCount; ++index) {
                painter.drawEllipse(handles[index], kSelectionHandleRadius, kSelectionHandleRadius);
            }
        }
        if (m_selectionState.effectEditorsVisible && !m_selectionState.toolbarHovered) {
            const auto layout = screenshotSelectionEffectLayout(
                m_selectionState.bounds, visibleCornerRadius, context.canvasToViewTransform,
                context.viewportRect);
            if (layout.available) {
                painter.setRenderHint(QPainter::Antialiasing);
                painter.save();
                if (m_selectionState.hoveredEffectHandle !=
                        ScreenshotSelectionEffectHandle::Width &&
                    m_selectionState.activeEffectHandle != ScreenshotSelectionEffectHandle::Width)
                    painter.setOpacity(painter.opacity() * 0.5);
                painter.setPen(QPen(selectionAccent, 1.5 * kScreenshotSelectionShadowControlScale));
                painter.drawLine(layout.shadowAnchor, layout.shadow);
                painter.translate(layout.shadow);
                painter.scale(kScreenshotSelectionShadowControlScale,
                              kScreenshotSelectionShadowControlScale);
                painter.setPen(QPen(Qt::white, 1.5));
                painter.setBrush(selectionAccent);
                painter.drawRoundedRect(QRectF(-6, -6, 12, 12), 3, 3);
                painter.setPen(QPen(Qt::white, 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(QRectF(-2, -1, 5, 4), 1, 1);
                painter.drawRoundedRect(QRectF(-3, -3, 5, 4), 1, 1);
                painter.restore();
                const auto handle =
                    screenshotSelectionRadiusHandle(m_selectionState.activeEffectHandle)
                        ? m_selectionState.activeEffectHandle
                        : m_selectionState.hoveredEffectHandle;
                if (screenshotSelectionRadiusHandle(handle)) {
                    painter.setBrush(selectionAccent);
                    painter.setPen(QPen(Qt::white, 1.5));
                    painter.drawEllipse(layout.position(handle), 4, 4);
                }
            }
        }
    }
    snowCanvasPaintSnapGuides(painter, m_selectionState.snapGuides, context.canvasToViewTransform,
                              context.exposedRegion);
    if (m_ocrVisible && m_renderMode == RenderMode::Standard && m_ocrPresentation != nullptr &&
        m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText) {
        if (m_ocrTextLayer != nullptr) {
            m_ocrTextLayer->synchronize(context.canvasToViewTransform, context.viewportRect);
        }
    }
    painter.restore();
}

void ScreenshotCanvasRenderer::setSelectionDraft(const QPainterPath& path,
                                                 const QVector<QPointF>& vertices) {
    auto next = m_selectionState;
    next.draftPath = path;
    next.draftVertices = vertices;
    if (next == m_selectionState)
        return;
    const QRectF before = m_selectionState.draftPath.boundingRect();
    applySelectionState(next);
    QRegion damage(m_canvas.canvasToViewTransform()
                       .mapRect(before.united(path.boundingRect()))
                       .adjusted(-5, -5, 5, 5)
                       .toAlignedRect());
    m_canvas.update(damage);
}
