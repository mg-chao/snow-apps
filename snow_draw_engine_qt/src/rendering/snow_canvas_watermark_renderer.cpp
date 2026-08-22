#include "snow_canvas_watermark_renderer.h"

#include "snow_canvas_renderer.h"

#include <QBrush>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QRawFont>
#include <QTextLayout>
#include <QTextOption>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace snow_canvas_renderer {
namespace {

constexpr double kMinimumVisibleAlpha = 0.004;
constexpr double kScaleQuantization = 64.0;
constexpr int kChunkLimit = 1024;
constexpr int kRepeatCellDimensionLimit = 4096;
constexpr std::size_t kRepeatCellByteLimit = 4u * 1024u * 1024u;
constexpr std::size_t kCacheEntryLimit = 8;
constexpr std::size_t kCacheByteLimit = 16u * 1024u * 1024u;
constexpr std::size_t kSparseFragmentLimit = 2048;
constexpr double kSparseCoverageLimit = 0.65;

bool isGuiThread() {
    QCoreApplication* application = QCoreApplication::instance();
    return application != nullptr && QThread::currentThread() == application->thread();
}

void releasePixmapsOnGuiThread(QVector<QPixmap>& pixmaps) {
    if (pixmaps.isEmpty() || isGuiThread()) {
        pixmaps.clear();
        return;
    }
    QCoreApplication* application = QCoreApplication::instance();
    if (application == nullptr) {
        return;
    }
    auto deferred = std::make_shared<QVector<QPixmap>>(std::move(pixmaps));
    QMetaObject::invokeMethod(
        application, [deferred]() { deferred->clear(); }, Qt::QueuedConnection);
}

struct ShapeKey {
    QString text;
    QString resolvedFont;
};

struct UnitKey {
    ShapeKey shape;
    int scaleX64 = 64;
    int scaleY64 = 64;
};

struct ShapeData {
    ShapeKey key;
    QFont font;
    QVector<QGlyphRun> glyphRuns;
    QRectF inkBounds;
    Qt::HANDLE creatorThread = nullptr;
};

void deleteShapeData(ShapeData* shape) {
    if (shape == nullptr || isGuiThread()) {
        delete shape;
        return;
    }
    QCoreApplication* application = QCoreApplication::instance();
    if (application == nullptr) {
        delete shape;
        return;
    }
    QMetaObject::invokeMethod(application, [shape]() { delete shape; }, Qt::QueuedConnection);
}

struct UnitChunk {
    QImage alpha;
    int contentStart = 0;
    int contentWidth = 0;
    int sourceX = 0;
};

struct TintedChunk {
    QImage image;
    int contentStart = 0;
    int contentWidth = 0;
    int sourceX = 0;
};

struct UnitEntry {
    ~UnitEntry() {
        releasePixmapsOnGuiThread(pixmaps);
    }

    UnitKey key;
    std::shared_ptr<const ShapeData> shape;
    QVector<UnitChunk> alphaChunks;
    QSize physicalSize;
    bool glyphFallback = false;
    bool segmented = false;
    std::size_t alphaBytes = 0;

    mutable std::mutex resourceMutex;
    QRgb tintKey = 0;
    QVector<TintedChunk> tintedChunks;
    QVector<QPixmap> pixmaps;
    QRgb cellTintKey = 0;
    int cellGap64 = -1;
    QImage repeatCell;
    std::size_t resourceBytes = 0;
};

struct UnitCache {
    std::mutex mutex;
    std::list<std::shared_ptr<UnitEntry>> entries;
    std::size_t bytes = 0;
    std::size_t lifetimeShapeBuilds = 0;
    QCoreApplication* cleanupApplication = nullptr;
};

struct Placement {
    QPointF inkTopLeft;
};

struct Fragment {
    QPointF topLeft;
    QRect source;
    int chunkIndex = 0;
};

struct PlacementWorkspace {
    std::vector<Placement> placements;
    std::vector<Fragment> fragments;
    std::vector<QPainter::PixmapFragment> pixmapFragments;
};

UnitCache g_cache;
thread_local PlacementWorkspace g_workspace;
thread_local WatermarkRenderDiagnostics g_diagnostics;
thread_local std::size_t g_fallbackCount = 0;

void clearWatermarkCacheAtApplicationShutdown() {
    std::list<std::shared_ptr<UnitEntry>> removed;
    {
        std::lock_guard<std::mutex> lock(g_cache.mutex);
        removed.swap(g_cache.entries);
        g_cache.bytes = 0;
        g_cache.cleanupApplication = nullptr;
    }
}

void ensureWatermarkCacheCleanupRegistered() {
    QCoreApplication* application = QCoreApplication::instance();
    if (application == nullptr || !isGuiThread() || QCoreApplication::closingDown()) {
        return;
    }

    bool registerCleanup = false;
    {
        std::lock_guard<std::mutex> lock(g_cache.mutex);
        if (g_cache.cleanupApplication != application) {
            g_cache.cleanupApplication = application;
            registerCleanup = true;
        }
    }
    if (registerCleanup) {
        // Glyph runs own thread-affine QRawFont data and must die before Qt tears down its
        // representation of the GUI thread.
        qAddPostRoutine(&clearWatermarkCacheAtApplicationShutdown);
    }
}

bool shapeKeyEquals(const ShapeKey& left, const ShapeKey& right) {
    return left.text == right.text && left.resolvedFont == right.resolvedFont;
}

bool unitKeyEquals(const UnitKey& left, const UnitKey& right) {
    return shapeKeyEquals(left.shape, right.shape) && left.scaleX64 == right.scaleX64 &&
           left.scaleY64 == right.scaleY64;
}

double elapsedMilliseconds(const QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
}

std::size_t imageBytes(const QImage& image) {
    return image.isNull() ? 0 : static_cast<std::size_t>(image.sizeInBytes());
}

std::size_t pixmapBytes(const QPixmap& pixmap) {
    return pixmap.isNull() ? 0
                           : static_cast<std::size_t>(pixmap.width()) *
                                 static_cast<std::size_t>(pixmap.height()) * 4u;
}

std::size_t entryBytes(const UnitEntry& entry) {
    std::lock_guard<std::mutex> lock(entry.resourceMutex);
    return entry.alphaBytes + entry.resourceBytes;
}

void refreshCacheAccountingAndEvict() {
    std::lock_guard<std::mutex> cacheLock(g_cache.mutex);
    g_cache.bytes = 0;
    for (const std::shared_ptr<UnitEntry>& entry : g_cache.entries) {
        g_cache.bytes += entryBytes(*entry);
    }
    while (!g_cache.entries.empty() &&
           (g_cache.entries.size() > kCacheEntryLimit || g_cache.bytes > kCacheByteLimit)) {
        const std::shared_ptr<UnitEntry> victim = g_cache.entries.back();
        g_cache.bytes -= entryBytes(*victim);
        g_cache.entries.pop_back();
        ++g_diagnostics.cacheEvictionCount;
    }
    g_diagnostics.cacheBytes = g_cache.bytes;
}

QRectF glyphRunInkBounds(const QGlyphRun& run) {
    const QRawFont rawFont = run.rawFont();
    const QList<quint32> glyphIndexes = run.glyphIndexes();
    const QList<QPointF> positions = run.positions();
    const qsizetype count = std::min(glyphIndexes.size(), positions.size());
    QRectF bounds;
    bool hasBounds = false;
    for (qsizetype index = 0; rawFont.isValid() && index < count; ++index) {
        const QRectF glyphBounds =
            rawFont.boundingRect(glyphIndexes.at(index)).translated(positions.at(index));
        if (glyphBounds.isEmpty()) {
            continue;
        }
        bounds = hasBounds ? bounds.united(glyphBounds) : glyphBounds;
        hasBounds = true;
    }
    if (hasBounds) {
        return bounds;
    }
    const QRectF fallback = run.boundingRect();
    return fallback.isEmpty() ? QRectF() : fallback;
}

std::shared_ptr<const ShapeData> buildShape(const ShapeKey& key, const QFont& font) {
    auto shape = std::shared_ptr<ShapeData>(new ShapeData(), &deleteShapeData);
    shape->key = key;
    shape->font = font;
    shape->creatorThread = QThread::currentThreadId();
    QTextLayout layout(key.text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setUseDesignMetrics(true);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(std::numeric_limits<qreal>::max() / 4.0);
        line.setPosition(QPointF(0.0, 0.0));
    }
    layout.endLayout();
    shape->glyphRuns = layout.glyphRuns();
    for (const QGlyphRun& run : shape->glyphRuns) {
        const QRectF runBounds = glyphRunInkBounds(run);
        if (!runBounds.isEmpty()) {
            shape->inkBounds =
                shape->inkBounds.isEmpty() ? runBounds : shape->inkBounds.united(runBounds);
        }
    }
    if (shape->inkBounds.isEmpty()) {
        shape->inkBounds = QFontMetricsF(font).boundingRect(key.text);
    }
    return shape;
}

void drawGlyphRuns(QPainter& painter, const ShapeData& shape, const QPointF& baseline) {
    for (const QGlyphRun& run : shape.glyphRuns) {
        painter.drawGlyphRun(baseline, run);
    }
}

std::shared_ptr<UnitEntry> buildUnit(const UnitKey& key,
                                     const std::shared_ptr<const ShapeData>& shape) {
    auto entry = std::make_shared<UnitEntry>();
    entry->key = key;
    entry->shape = shape;
    const double scaleX = static_cast<double>(key.scaleX64) / kScaleQuantization;
    const double scaleY = static_cast<double>(key.scaleY64) / kScaleQuantization;
    const int width = std::max(1, qCeil(shape->inkBounds.width() * scaleX) + 2);
    const int height = std::max(1, qCeil(shape->inkBounds.height() * scaleY) + 2);
    entry->physicalSize = QSize(width, height);
    bool pathologicalPhysicalGlyph = false;
    for (const QGlyphRun& run : shape->glyphRuns) {
        const QRawFont rawFont = run.rawFont();
        for (quint32 glyph : run.glyphIndexes()) {
            const QRectF glyphBounds = rawFont.boundingRect(glyph);
            if (glyphBounds.width() * scaleX > kChunkLimit ||
                glyphBounds.height() * scaleY > kChunkLimit) {
                pathologicalPhysicalGlyph = true;
            }
        }
    }
    entry->glyphFallback = pathologicalPhysicalGlyph || height > kChunkLimit;
    entry->segmented = !entry->glyphFallback && width > kChunkLimit;
    if (entry->glyphFallback) {
        return entry;
    }

    for (int contentStart = 0; contentStart < width; contentStart += kChunkLimit) {
        const int contentEnd = std::min(width, contentStart + kChunkLimit);
        const int imageStart = std::max(0, contentStart - 1);
        const int imageEnd = std::min(width, contentEnd + 1);
        QImage alpha(QSize(imageEnd - imageStart, height), QImage::Format_ARGB32_Premultiplied);
        alpha.fill(Qt::transparent);
        QPainter unitPainter(&alpha);
        unitPainter.setRenderHint(QPainter::TextAntialiasing, true);
        unitPainter.scale(scaleX, scaleY);
        unitPainter.setPen(Qt::white);
        drawGlyphRuns(unitPainter, *shape,
                      QPointF((1.0 - imageStart) / scaleX - shape->inkBounds.left(),
                              1.0 / scaleY - shape->inkBounds.top()));
        unitPainter.end();
        entry->alphaBytes += imageBytes(alpha);
        entry->alphaChunks.push_back(UnitChunk{
            std::move(alpha),
            contentStart,
            contentEnd - contentStart,
            contentStart - imageStart,
        });
    }
    return entry;
}

int quantizedScale64(double scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        return 64;
    }
    return std::max(1, qRound(scale * kScaleQuantization));
}

double deviceScaleX(const QTransform& transform) {
    return std::hypot(transform.m11(), transform.m12());
}

double deviceScaleY(const QTransform& transform) {
    return std::hypot(transform.m21(), transform.m22());
}

QFont resolveFont(const WatermarkDisplayInfo& configuration, const QFont& base) {
    const int familyLength =
        std::min<int>(configuration.watermark_font_family_len,
                      static_cast<int>(configuration.watermark_font_family.size()));
    QString family =
        QString::fromUtf8(configuration.watermark_font_family.data(), familyLength).trimmed();
    if (family.isEmpty()) {
        family = base.family();
    }
    QFont font(family);
    font.setStyleName(base.styleName());
    font.setWeight(base.weight());
    font.setStyle(base.style());
    font.setStretch(base.stretch());
    font.setKerning(base.kerning());
    font.setHintingPreference(base.hintingPreference());
    font.setPixelSize(
        std::max(1, qRound(std::clamp(configuration.watermark_font_size, 1.0, 10000.0))));
    return font;
}

std::shared_ptr<UnitEntry> findOrBuildUnit(const UnitKey& key, const QFont& font) {
    if (!isGuiThread()) {
        {
            std::lock_guard<std::mutex> lock(g_cache.mutex);
            for (auto iterator = g_cache.entries.begin(); iterator != g_cache.entries.end();
                 ++iterator) {
                if (unitKeyEquals((*iterator)->key, key) && !(*iterator)->glyphFallback) {
                    g_cache.entries.splice(g_cache.entries.begin(), g_cache.entries, iterator);
                    ++g_diagnostics.shapeHitCount;
                    ++g_diagnostics.unitHitCount;
                    g_diagnostics.cacheBytes = g_cache.bytes;
                    return g_cache.entries.front();
                }
            }
        }

        QCoreApplication* application = QCoreApplication::instance();
        if (application == nullptr) {
            return {};
        }
        const QString fontDescription = font.toString();
        std::shared_ptr<UnitEntry> result;
        const bool invoked = QMetaObject::invokeMethod(
            application,
            [&result, &key, &fontDescription]() {
                QFont guiFont;
                guiFont.fromString(fontDescription);
                result = findOrBuildUnit(key, guiFont);
            },
            Qt::BlockingQueuedConnection);
        return invoked && result && !result->glyphFallback ? result : nullptr;
    }

    ensureWatermarkCacheCleanupRegistered();

    std::shared_ptr<const ShapeData> reusableShape;
    {
        std::lock_guard<std::mutex> lock(g_cache.mutex);
        for (auto iterator = g_cache.entries.begin(); iterator != g_cache.entries.end();
             ++iterator) {
            if (unitKeyEquals((*iterator)->key, key) &&
                (!(*iterator)->glyphFallback ||
                 (*iterator)->shape->creatorThread == QThread::currentThreadId())) {
                g_cache.entries.splice(g_cache.entries.begin(), g_cache.entries, iterator);
                ++g_diagnostics.shapeHitCount;
                ++g_diagnostics.unitHitCount;
                g_diagnostics.cacheBytes = g_cache.bytes;
                return g_cache.entries.front();
            }
            if (!reusableShape && shapeKeyEquals((*iterator)->key.shape, key.shape) &&
                (*iterator)->shape->creatorThread == QThread::currentThreadId()) {
                reusableShape = (*iterator)->shape;
            }
        }
    }

    QElapsedTimer shapeTimer;
    shapeTimer.start();
    if (reusableShape) {
        ++g_diagnostics.shapeHitCount;
    } else {
        ++g_diagnostics.shapeMissCount;
        reusableShape = buildShape(key.shape, font);
        std::lock_guard<std::mutex> lock(g_cache.mutex);
        ++g_cache.lifetimeShapeBuilds;
    }
    g_diagnostics.shapeMilliseconds += elapsedMilliseconds(shapeTimer);

    ++g_diagnostics.unitMissCount;
    QElapsedTimer rasterTimer;
    rasterTimer.start();
    std::shared_ptr<UnitEntry> built = buildUnit(key, reusableShape);
    g_diagnostics.rasterMilliseconds += elapsedMilliseconds(rasterTimer);

    {
        std::lock_guard<std::mutex> lock(g_cache.mutex);
        for (auto iterator = g_cache.entries.begin(); iterator != g_cache.entries.end();
             ++iterator) {
            if (unitKeyEquals((*iterator)->key, key) &&
                (!(*iterator)->glyphFallback ||
                 (*iterator)->shape->creatorThread == QThread::currentThreadId())) {
                g_cache.entries.splice(g_cache.entries.begin(), g_cache.entries, iterator);
                return g_cache.entries.front();
            }
        }
        g_cache.entries.push_front(built);
    }
    refreshCacheAccountingAndEvict();
    return built;
}

QVector<TintedChunk> tintedChunksFor(UnitEntry& entry, QRgb tintKey, const QColor& color) {
    {
        std::lock_guard<std::mutex> lock(entry.resourceMutex);
        if (entry.tintKey == tintKey && !entry.tintedChunks.isEmpty()) {
            return entry.tintedChunks;
        }
    }
    QElapsedTimer timer;
    timer.start();
    QVector<TintedChunk> built;
    built.reserve(entry.alphaChunks.size());
    for (const UnitChunk& chunk : entry.alphaChunks) {
        QImage tinted = chunk.alpha.copy();
        QPainter tintPainter(&tinted);
        tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tintPainter.fillRect(tinted.rect(), color);
        tintPainter.end();
        built.push_back(
            TintedChunk{std::move(tinted), chunk.contentStart, chunk.contentWidth, chunk.sourceX});
    }
    g_diagnostics.tintMilliseconds += elapsedMilliseconds(timer);
    ++g_diagnostics.tintBuildCount;
    {
        std::lock_guard<std::mutex> lock(entry.resourceMutex);
        if (entry.tintKey != tintKey || entry.tintedChunks.isEmpty()) {
            entry.tintKey = tintKey;
            entry.tintedChunks = built;
            releasePixmapsOnGuiThread(entry.pixmaps);
            entry.repeatCell = QImage();
            entry.cellGap64 = -1;
            entry.cellTintKey = 0;
            entry.resourceBytes = 0;
            for (const TintedChunk& chunk : entry.tintedChunks) {
                entry.resourceBytes += imageBytes(chunk.image);
            }
        } else {
            built = entry.tintedChunks;
        }
    }
    refreshCacheAccountingAndEvict();
    return built;
}

QRectF inverseRotatedBounds(const QRectF& bounds, const QPointF& center, double angle) {
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.rotate(angle);
    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    return invertible ? inverse.mapRect(bounds) : QRectF();
}

void planPlacements(const UnitEntry& entry, const QRectF& localBounds, double horizontalStep,
                    double verticalStep, double scaleX, double scaleY) {
    g_workspace.placements.clear();
    g_workspace.fragments.clear();
    const qint64 firstRow = static_cast<qint64>(std::floor(localBounds.top() / verticalStep)) - 1;
    const qint64 lastRow = static_cast<qint64>(std::ceil(localBounds.bottom() / verticalStep)) + 1;
    const double topMargin = 1.0 / scaleY;
    for (qint64 row = firstRow; row <= lastRow; ++row) {
        const double offset = (row & 1) != 0 ? horizontalStep / 2.0 : 0.0;
        const qint64 firstColumn =
            static_cast<qint64>(std::floor((localBounds.left() - offset) / horizontalStep)) - 1;
        const qint64 lastColumn =
            static_cast<qint64>(std::ceil((localBounds.right() - offset) / horizontalStep)) + 1;
        for (qint64 column = firstColumn; column <= lastColumn; ++column) {
            const QPointF inkTopLeft(static_cast<double>(column) * horizontalStep + offset,
                                     static_cast<double>(row) * verticalStep);
            const QRectF unitRect(inkTopLeft.x() - 1.0 / scaleX, inkTopLeft.y() - topMargin,
                                  static_cast<double>(entry.physicalSize.width()) / scaleX,
                                  static_cast<double>(entry.physicalSize.height()) / scaleY);
            if (!unitRect.intersects(localBounds)) {
                g_diagnostics.culledFragmentCount += entry.alphaChunks.size();
                continue;
            }
            g_workspace.placements.push_back(Placement{inkTopLeft});
            for (int chunkIndex = 0; chunkIndex < entry.alphaChunks.size(); ++chunkIndex) {
                const UnitChunk& chunk = entry.alphaChunks.at(chunkIndex);
                const QRectF chunkRect(inkTopLeft.x() - 1.0 / scaleX +
                                           static_cast<double>(chunk.contentStart) / scaleX,
                                       inkTopLeft.y() - topMargin,
                                       static_cast<double>(chunk.contentWidth) / scaleX,
                                       static_cast<double>(entry.physicalSize.height()) / scaleY);
                if (!chunkRect.intersects(localBounds)) {
                    ++g_diagnostics.culledFragmentCount;
                    continue;
                }
                g_workspace.fragments.push_back(Fragment{
                    chunkRect.topLeft(),
                    QRect(chunk.sourceX, 0, chunk.contentWidth, entry.physicalSize.height()),
                    chunkIndex,
                });
            }
        }
    }
}

double fragmentCoverage(const QRectF& localBounds, double scaleX, double scaleY) {
    double covered = 0.0;
    for (const Fragment& fragment : g_workspace.fragments) {
        const QRectF destination(fragment.topLeft, QSizeF(fragment.source.width() / scaleX,
                                                          fragment.source.height() / scaleY));
        const QRectF clipped = destination.intersected(localBounds);
        covered += clipped.width() * clipped.height() * scaleX * scaleY;
    }
    const double exposed = localBounds.width() * localBounds.height() * scaleX * scaleY;
    return exposed > 0.0 ? covered / exposed : 0.0;
}

QVector<QPixmap> pixmapsFor(UnitEntry& entry, const QVector<TintedChunk>& chunks) {
    if (!isGuiThread()) {
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(entry.resourceMutex);
        if (entry.pixmaps.size() == chunks.size()) {
            return entry.pixmaps;
        }
    }
    QVector<QPixmap> built;
    built.reserve(chunks.size());
    for (const TintedChunk& chunk : chunks) {
        built.push_back(QPixmap::fromImage(chunk.image));
    }
    {
        std::lock_guard<std::mutex> lock(entry.resourceMutex);
        if (entry.pixmaps.size() != chunks.size()) {
            entry.pixmaps = built;
            for (const QPixmap& pixmap : entry.pixmaps) {
                entry.resourceBytes += pixmapBytes(pixmap);
            }
        } else {
            built = entry.pixmaps;
        }
    }
    refreshCacheAccountingAndEvict();
    return built;
}

void composeSparseImages(QPainter& painter, const QVector<TintedChunk>& chunks, double scaleX,
                         double scaleY) {
    for (const Fragment& fragment : g_workspace.fragments) {
        const QImage& image = chunks.at(fragment.chunkIndex).image;
        if (qFuzzyCompare(scaleX, 1.0) && qFuzzyCompare(scaleY, 1.0) &&
            fragment.source == image.rect()) {
            painter.drawImage(fragment.topLeft, image);
            continue;
        }
        const QRectF destination(fragment.topLeft, QSizeF(fragment.source.width() / scaleX,
                                                          fragment.source.height() / scaleY));
        painter.drawImage(destination, image, fragment.source);
    }
    ++g_diagnostics.sparseBatchCount;
    g_diagnostics.submittedFragmentCount += g_workspace.fragments.size();
}

void composeSparsePixmaps(QPainter& painter, const QVector<QPixmap>& pixmaps, double scaleX,
                          double scaleY) {
    for (int chunkIndex = 0; chunkIndex < pixmaps.size(); ++chunkIndex) {
        g_workspace.pixmapFragments.clear();
        for (const Fragment& fragment : g_workspace.fragments) {
            if (fragment.chunkIndex != chunkIndex) {
                continue;
            }
            const QSizeF destinationSize(fragment.source.width() / scaleX,
                                         fragment.source.height() / scaleY);
            g_workspace.pixmapFragments.push_back(QPainter::PixmapFragment::create(
                fragment.topLeft +
                    QPointF(destinationSize.width() / 2.0, destinationSize.height() / 2.0),
                QRectF(fragment.source), 1.0 / scaleX, 1.0 / scaleY));
        }
        if (!g_workspace.pixmapFragments.empty()) {
            painter.drawPixmapFragments(g_workspace.pixmapFragments.data(),
                                        static_cast<int>(g_workspace.pixmapFragments.size()),
                                        pixmaps.at(chunkIndex));
            ++g_diagnostics.sparseBatchCount;
            g_diagnostics.submittedFragmentCount += g_workspace.pixmapFragments.size();
        }
    }
}

QImage repeatCellFor(UnitEntry& entry, const QVector<TintedChunk>& chunks, QRgb tintKey, int gap64,
                     double horizontalStep, double verticalStep, double scaleX, double scaleY) {
    {
        std::lock_guard<std::mutex> lock(entry.resourceMutex);
        if (entry.cellTintKey == tintKey && entry.cellGap64 == gap64 &&
            !entry.repeatCell.isNull()) {
            return entry.repeatCell;
        }
    }
    const int width = qCeil(horizontalStep * 2.0 * scaleX);
    const int height = qCeil(verticalStep * 2.0 * scaleY);
    const std::size_t bytes = static_cast<std::size_t>(std::max(0, width)) *
                              static_cast<std::size_t>(std::max(0, height)) * 4u;
    if (width <= 0 || height <= 0 || width > kRepeatCellDimensionLimit ||
        height > kRepeatCellDimensionLimit || bytes > kRepeatCellByteLimit) {
        return {};
    }
    QImage cell(QSize(width, height), QImage::Format_ARGB32_Premultiplied);
    cell.fill(Qt::transparent);
    QPainter cellPainter(&cell);
    const auto drawUnit = [&](double x, double y) {
        for (const TintedChunk& chunk : chunks) {
            const QRect source(chunk.sourceX, 0, chunk.contentWidth, entry.physicalSize.height());
            const QRectF destination(x * scaleX - 1.0 + chunk.contentStart, y * scaleY - 1.0,
                                     chunk.contentWidth, entry.physicalSize.height());
            cellPainter.drawImage(destination, chunk.image, source);
        }
    };
    for (int row = -1; row <= 2; ++row) {
        const double offset = (row & 1) != 0 ? horizontalStep / 2.0 : 0.0;
        for (int column = -2; column <= 2; ++column) {
            drawUnit(column * horizontalStep + offset, row * verticalStep);
        }
    }
    cellPainter.end();
    ++g_diagnostics.repeatCellBuildCount;
    {
        std::lock_guard<std::mutex> lock(entry.resourceMutex);
        const std::size_t oldBytes = imageBytes(entry.repeatCell);
        entry.repeatCell = cell;
        entry.cellTintKey = tintKey;
        entry.cellGap64 = gap64;
        entry.resourceBytes = entry.resourceBytes - std::min(entry.resourceBytes, oldBytes) +
                              imageBytes(entry.repeatCell);
    }
    refreshCacheAccountingAndEvict();
    return cell;
}

void composeGlyphFallback(QPainter& painter, const UnitEntry& entry, const QColor& color,
                          const QRectF& localBounds, double horizontalStep, double verticalStep) {
    painter.setPen(color);
    const qint64 firstRow = static_cast<qint64>(std::floor(localBounds.top() / verticalStep)) - 1;
    const qint64 lastRow = static_cast<qint64>(std::ceil(localBounds.bottom() / verticalStep)) + 1;
    for (qint64 row = firstRow; row <= lastRow; ++row) {
        const double offset = (row & 1) != 0 ? horizontalStep / 2.0 : 0.0;
        const qint64 firstColumn =
            static_cast<qint64>(std::floor((localBounds.left() - offset) / horizontalStep)) - 1;
        const qint64 lastColumn =
            static_cast<qint64>(std::ceil((localBounds.right() - offset) / horizontalStep)) + 1;
        for (qint64 column = firstColumn; column <= lastColumn; ++column) {
            const QPointF inkTopLeft(static_cast<double>(column) * horizontalStep + offset,
                                     static_cast<double>(row) * verticalStep);
            const QRectF translatedInk(inkTopLeft, entry.shape->inkBounds.size());
            if (!translatedInk.intersects(localBounds)) {
                ++g_diagnostics.culledFragmentCount;
                continue;
            }
            drawGlyphRuns(painter, *entry.shape, inkTopLeft - entry.shape->inkBounds.topLeft());
            ++g_diagnostics.fallbackGlyphDrawCount;
        }
    }
}

} // namespace

void WatermarkPatternRenderer::render(QPainter& painter, const WatermarkRenderRequest& request) {
    ++g_diagnostics.renderCallCount;
    const WatermarkDisplayInfo& configuration = request.configuration;
    const QRectF surfaceBounds = request.surfaceBounds.normalized();
    const QRectF anchorArea = request.anchorRenderArea.normalized();
    const int textLength = std::min<int>(configuration.watermark_text_len,
                                         static_cast<int>(configuration.watermark_text.size()));
    const QString text = QString::fromUtf8(configuration.watermark_text.data(), textLength)
                             .normalized(QString::NormalizationForm_C);
    const double effectiveAlpha = configuration.watermark_color.a *
                                  std::clamp(configuration.watermark_opacity, 0.0, 1.0) / 255.0;
    if (text.trimmed().isEmpty() || effectiveAlpha < kMinimumVisibleAlpha ||
        surfaceBounds.isEmpty() || anchorArea.isEmpty() ||
        !std::isfinite(configuration.watermark_font_size) ||
        configuration.watermark_font_size <= 0.0) {
        ++g_diagnostics.earlyExitCount;
        return;
    }
    const QRectF visibleArea = surfaceBounds.intersected(anchorArea);
    QRegion exposedRegion = request.exposedRegion.isEmpty() ? QRegion(visibleArea.toAlignedRect())
                                                            : request.exposedRegion;
    exposedRegion &= QRegion(visibleArea.toAlignedRect());
    if (exposedRegion.isEmpty()) {
        ++g_diagnostics.earlyExitCount;
        return;
    }

    const QFont font = resolveFont(configuration, painter.font());
    const int scaleX64 = quantizedScale64(deviceScaleX(request.effectiveDeviceTransform));
    const int scaleY64 = quantizedScale64(deviceScaleY(request.effectiveDeviceTransform));
    const double scaleX = static_cast<double>(scaleX64) / kScaleQuantization;
    const double scaleY = static_cast<double>(scaleY64) / kScaleQuantization;
    const UnitKey key{
        ShapeKey{text, font.toString() + QLatin1Char('|') + font.family()},
        scaleX64,
        scaleY64,
    };
    const std::shared_ptr<UnitEntry> entry = findOrBuildUnit(key, font);
    if (!entry || entry->shape->inkBounds.isEmpty()) {
        ++g_diagnostics.earlyExitCount;
        return;
    }

    QColor color = snow_canvas_renderer::toQColor(configuration.watermark_color);
    color.setAlphaF(static_cast<float>(effectiveAlpha));
    const QRgb tintKey = color.rgba();
    const double gap = std::clamp(configuration.watermark_gap, 10.0, 200.0);
    const double horizontalStep = std::max(1.0, entry->shape->inkBounds.width() + gap);
    const double verticalStep = std::max(1.0, entry->shape->inkBounds.height() + gap);
    const QPointF center = anchorArea.center();
    const QRectF localBounds =
        inverseRotatedBounds(exposedRegion.boundingRect(), center, configuration.watermark_angle);
    if (localBounds.isEmpty()) {
        ++g_diagnostics.earlyExitCount;
        return;
    }
    g_diagnostics.renderedLogicalBounds = exposedRegion.boundingRect();
    g_diagnostics.renderedDeviceBounds =
        request.effectiveDeviceTransform.mapRect(QRectF(exposedRegion.boundingRect()))
            .toAlignedRect();

    QElapsedTimer placementTimer;
    placementTimer.start();
    if (!entry->glyphFallback) {
        planPlacements(*entry, localBounds, horizontalStep, verticalStep, scaleX, scaleY);
        g_diagnostics.fragmentCoverage = fragmentCoverage(localBounds, scaleX, scaleY);
    }
    g_diagnostics.placementMilliseconds += elapsedMilliseconds(placementTimer);

    painter.save();
    painter.setClipRegion(exposedRegion, Qt::IntersectClip);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.translate(center);
    painter.rotate(configuration.watermark_angle);
    QElapsedTimer compositionTimer;
    compositionTimer.start();

    if (entry->glyphFallback) {
        ++g_fallbackCount;
        g_diagnostics.selectedStrategy = WatermarkRenderStrategy::GlyphFallback;
        composeGlyphFallback(painter, *entry, color, localBounds, horizontalStep, verticalStep);
    } else {
        const QVector<TintedChunk> chunks = tintedChunksFor(*entry, tintKey, color);
        const bool sparse = g_workspace.fragments.size() <= kSparseFragmentLimit &&
                            g_diagnostics.fragmentCoverage <= kSparseCoverageLimit;
        bool composed = false;
        if (!sparse) {
            const int gap64 = qRound(gap * 64.0);
            const QImage cell = repeatCellFor(*entry, chunks, tintKey, gap64, horizontalStep,
                                              verticalStep, scaleX, scaleY);
            if (!cell.isNull()) {
                QBrush brush(cell);
                QTransform brushTransform;
                brushTransform.scale(horizontalStep * 2.0 / cell.width(),
                                     verticalStep * 2.0 / cell.height());
                brush.setTransform(brushTransform);
                painter.fillRect(localBounds, brush);
                ++g_diagnostics.denseFillCount;
                g_diagnostics.selectedStrategy = WatermarkRenderStrategy::DenseCell;
                composed = true;
            }
        }
        if (!composed) {
            if (entry->segmented) {
                g_diagnostics.selectedStrategy = WatermarkRenderStrategy::SegmentedSparse;
                g_diagnostics.segmentedChunkCount += g_workspace.fragments.size();
                composeSparseImages(painter, chunks, scaleX, scaleY);
            } else if (request.purpose == WatermarkRenderPurpose::Widget) {
                const QVector<QPixmap> pixmaps = pixmapsFor(*entry, chunks);
                if (!pixmaps.isEmpty()) {
                    g_diagnostics.selectedStrategy = WatermarkRenderStrategy::SparsePixmap;
                    composeSparsePixmaps(painter, pixmaps, scaleX, scaleY);
                } else {
                    g_diagnostics.selectedStrategy = WatermarkRenderStrategy::SparseImage;
                    composeSparseImages(painter, chunks, scaleX, scaleY);
                }
            } else {
                g_diagnostics.selectedStrategy = WatermarkRenderStrategy::SparseImage;
                composeSparseImages(painter, chunks, scaleX, scaleY);
            }
        }
    }
    g_diagnostics.compositionMilliseconds += elapsedMilliseconds(compositionTimer);
    painter.restore();
}

void renderWatermark(QPainter& painter, const WatermarkDisplayInfo& displayInfo) {
    renderWatermark(painter, displayInfo,
                    QRectF(0.0, 0.0, displayInfo.surface_width, displayInfo.surface_height));
}

void renderWatermark(QPainter& painter, const WatermarkDisplayInfo& displayInfo,
                     const QRectF& renderArea) {
    WatermarkPatternRenderer::render(
        painter, WatermarkRenderRequest{
                     displayInfo,
                     QRectF(0.0, 0.0, displayInfo.surface_width, displayInfo.surface_height),
                     renderArea,
                     painter.hasClipping() ? painter.clipRegion()
                                           : QRegion(QRectF(0.0, 0.0, displayInfo.surface_width,
                                                            displayInfo.surface_height)
                                                         .toAlignedRect()),
                     painter.deviceTransform(),
                     WatermarkRenderPurpose::ImageExport,
                 });
}

std::size_t watermarkLayoutCacheBuildCountForCurrentThread() {
    std::lock_guard<std::mutex> lock(g_cache.mutex);
    return g_cache.lifetimeShapeBuilds;
}

std::size_t watermarkDirectFallbackCountForCurrentThread() {
    return g_fallbackCount;
}

std::size_t watermarkPatternCacheEntryCountForCurrentThread() {
    std::lock_guard<std::mutex> lock(g_cache.mutex);
    return g_cache.entries.size();
}

std::size_t watermarkPatternCacheBytesForCurrentThread() {
    std::lock_guard<std::mutex> lock(g_cache.mutex);
    return g_cache.bytes;
}

WatermarkRenderDiagnostics watermarkRenderDiagnosticsForCurrentThread() {
    return g_diagnostics;
}

void resetWatermarkRenderDiagnosticsForCurrentThread() {
    g_diagnostics = {};
}

void resetWatermarkRenderCacheForCurrentThread() {
    std::list<std::shared_ptr<UnitEntry>> removed;
    {
        std::lock_guard<std::mutex> lock(g_cache.mutex);
        removed.swap(g_cache.entries);
        g_cache.bytes = 0;
    }
    g_fallbackCount = 0;
}

} // namespace snow_canvas_renderer
