#include "snow_canvas_text_render.h"

#include "snow_canvas_fill_render.h"
#include "snow_canvas_text.h"
#include "snow_canvas_text_layout.h"

#include <QAbstractTextDocumentLayout>
#include <QBrush>
#include <QColor>
#include <QGuiApplication>
#include <QHashFunctions>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextLine>

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace snow_canvas_text_render {
namespace {

namespace text_layout = snow_canvas_text_layout;

enum class RenderPhase { Background, Contents, Stroke, Geometry };

struct LayoutKey {
    QString text;
    QFont font;
    double scale = 1.0;
    double zoom = 1.0;
    double width = 1.0;
    double height = 1.0;
    SnowTextHorizontalAlign horizontalAlignment = SNOW_TEXT_HORIZONTAL_ALIGN_LEFT;
    SnowTextVerticalAlign verticalAlignment = SNOW_TEXT_VERTICAL_ALIGN_TOP;
    RenderPhase phase = RenderPhase::Geometry;
    QRgb color = 0;
    double strokeWidth = 0.0;
    qreal devicePixelRatio = 1.0;
    int logicalDpiX = 0;
    int logicalDpiY = 0;

    bool operator==(const LayoutKey& other) const {
        return text == other.text && font == other.font && scale == other.scale &&
               zoom == other.zoom && width == other.width && height == other.height &&
               horizontalAlignment == other.horizontalAlignment &&
               verticalAlignment == other.verticalAlignment && phase == other.phase &&
               color == other.color && strokeWidth == other.strokeWidth &&
               devicePixelRatio == other.devicePixelRatio && logicalDpiX == other.logicalDpiX &&
               logicalDpiY == other.logicalDpiY;
    }
};

struct LayoutKeyHash {
    std::size_t operator()(const LayoutKey& key) const {
        return qHashMulti(0, key.text, key.font, key.scale, key.zoom, key.width, key.height,
                          static_cast<int>(key.horizontalAlignment),
                          static_cast<int>(key.verticalAlignment), static_cast<int>(key.phase),
                          key.color, key.strokeWidth, key.devicePixelRatio, key.logicalDpiX,
                          key.logicalDpiY);
    }
};

struct RetainedLayout {
    LayoutKey key;
    std::shared_ptr<const text_layout::DocumentLayout> layout;
    std::size_t characters = 0;
    std::size_t estimatedBytes = 0;
};

struct RenderCache {
    std::list<RetainedLayout> layouts;
    std::unordered_map<LayoutKey, std::list<RetainedLayout>::iterator, LayoutKeyHash> entries;
    RenderCacheDiagnostics diagnostics;
    std::uint64_t fontDatabaseRevision = 0;
};

std::atomic<std::uint64_t> g_fontDatabaseRevision{1};

RenderCache& renderCache() {
    // Each QTextDocument is created, painted, and destroyed on its rendering thread.
    thread_local RenderCache cache;
    return cache;
}

void synchronizeFontDatabase(RenderCache& cache) {
    static std::once_flag connectionOnce;
    if (qGuiApp != nullptr) {
        std::call_once(connectionOnce, [] {
            QObject::connect(
                qGuiApp, &QGuiApplication::fontDatabaseChanged, qGuiApp,
                [] { g_fontDatabaseRevision.fetch_add(1, std::memory_order_relaxed); },
                Qt::DirectConnection);
        });
    }
    const auto revision = g_fontDatabaseRevision.load(std::memory_order_relaxed);
    if (cache.fontDatabaseRevision != revision) {
        cache.entries.clear();
        cache.layouts.clear();
        cache.diagnostics = {};
        cache.fontDatabaseRevision = revision;
    }
}

QColor toQColor(const SnowColorRgba8& color) {
    return QColor(color.r, color.g, color.b, color.a);
}

std::shared_ptr<const text_layout::DocumentLayout>
preparedLayout(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
               double zoom, const QString& text, RenderPhase phase) {
    const double safeZoom = qMax(0.0001, zoom);
    const auto resolution = text_layout::resolveFont(baseFont, item, safeZoom);
    LayoutKey key;
    key.text = text;
    key.font = resolution.font;
    key.scale = resolution.scale;
    key.zoom = safeZoom;
    key.width = qMax(1.0, item.width * safeZoom);
    key.height = qMax(1.0, item.height * safeZoom);
    key.horizontalAlignment = item.text_horizontal_align;
    key.verticalAlignment = item.text_vertical_align;
    key.phase = phase;
    if (phase == RenderPhase::Stroke) {
        key.color = toQColor(item.stroke).rgba();
        key.strokeWidth = item.stroke_width * zoom / resolution.scale;
    }
    if (painter.device() != nullptr) {
        key.devicePixelRatio = painter.device()->devicePixelRatioF();
        key.logicalDpiX = painter.device()->logicalDpiX();
        key.logicalDpiY = painter.device()->logicalDpiY();
    }

    auto& cache = renderCache();
    synchronizeFontDatabase(cache);
    const auto found = cache.entries.find(key);
    if (found != cache.entries.end()) {
        ++cache.diagnostics.hits;
        cache.layouts.splice(cache.layouts.begin(), cache.layouts, found->second);
        return found->second->layout;
    }

    auto layout = std::make_shared<text_layout::DocumentLayout>(text_layout::createDocumentLayout(
        item, baseFont, safeZoom, text, phase == RenderPhase::Background));
    ++cache.diagnostics.builds;
    QTextDocument& document = layout->textDocument();
    document.setUndoRedoEnabled(false);
    if (phase == RenderPhase::Stroke) {
        QTextCursor cursor(&document);
        cursor.select(QTextCursor::Document);
        QTextCharFormat format;
        QPen outlinePen(QColor::fromRgba(key.color), key.strokeWidth, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin);
        outlinePen.setMiterLimit(2.0);
        format.setTextOutline(outlinePen);
        format.setForeground(QBrush(Qt::transparent));
        cursor.mergeCharFormat(format);
    }
    const auto characters = static_cast<std::size_t>(document.characterCount());
    // Conservatively account for the document, blocks, glyph runs, key, and cache nodes.
    const auto estimatedBytes = 4096u + sizeof(RetainedLayout) + sizeof(LayoutKey) +
                                static_cast<std::size_t>(text.size()) * (64u + sizeof(QChar)) +
                                static_cast<std::size_t>(key.font.key().size()) * sizeof(QChar) +
                                static_cast<std::size_t>(document.blockCount()) * 256u;
    if (characters > kRenderCacheCharacterLimit || estimatedBytes > kRenderCacheByteLimit) {
        return layout;
    }
    while (cache.layouts.size() >= kRenderCacheEntryLimit ||
           cache.diagnostics.retainedCharacters + characters > kRenderCacheCharacterLimit ||
           cache.diagnostics.estimatedBytes + estimatedBytes > kRenderCacheByteLimit) {
        const auto& oldest = cache.layouts.back();
        cache.diagnostics.retainedCharacters -= oldest.characters;
        cache.diagnostics.estimatedBytes -= oldest.estimatedBytes;
        cache.entries.erase(oldest.key);
        cache.layouts.pop_back();
        ++cache.diagnostics.evictions;
    }
    cache.layouts.push_front({std::move(key), layout, characters, estimatedBytes});
    try {
        cache.entries.emplace(cache.layouts.front().key, cache.layouts.begin());
    } catch (...) {
        cache.layouts.pop_front();
        throw;
    }
    cache.diagnostics.retainedCharacters += characters;
    cache.diagnostics.estimatedBytes += estimatedBytes;
    cache.diagnostics.entries = cache.layouts.size();
    return layout;
}

} // namespace

RenderCacheDiagnostics renderCacheDiagnosticsForCurrentThread() {
    auto& cache = renderCache();
    synchronizeFontDatabase(cache);
    return cache.diagnostics;
}

void clearRenderCacheForCurrentThread() {
    auto& cache = renderCache();
    cache.entries.clear();
    cache.layouts.clear();
    cache.diagnostics = {};
    cache.fontDatabaseRevision = g_fontDatabaseRevision.load(std::memory_order_relaxed);
}

void resetLayoutCacheForCurrentThread() {
    clearRenderCacheForCurrentThread();
}

LayoutCacheStats layoutCacheStatsForCurrentThread() {
    const auto diagnostics = renderCacheDiagnosticsForCurrentThread();
    return {static_cast<qsizetype>(diagnostics.entries),
            static_cast<qsizetype>(diagnostics.estimatedBytes), diagnostics.builds};
}

void drawContents(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                  const QRectF& localRect, double zoom) {
    if (item.text_color.a == 0 || item.font_size <= 0.0) {
        return;
    }
    const QString text = snow_canvas_text::textFromSceneItem(item);
    if (text.isEmpty()) {
        return;
    }

    const auto layout = preparedLayout(painter, item, baseFont, zoom, text, RenderPhase::Contents);

    painter.save();
    painter.translate(localRect.left(), localRect.top() + layout->topOffset);
    painter.scale(layout->resolution.scale, layout->resolution.scale);
    // Foreground is paint state, so color changes can reuse the immutable layout.
    text_layout::drawDocument(painter, *layout, toQColor(item.text_color));
    painter.restore();
}

void drawBackground(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                    const QRectF& localRect, double zoom) {
    if (item.fill.a == 0 || item.font_size <= 0.0) {
        return;
    }

    const QString text = snow_canvas_text::textFromSceneItem(item);
    const auto layout =
        preparedLayout(painter, item, baseFont, zoom, text, RenderPhase::Background);
    const QTextDocument& document = layout->textDocument();
    // The pill padding comes from the engine's published fill-padding
    // contract. Never measure padding here: a font-metrics line height would
    // drift from the contract edge the dirty regions are derived from.
    const SnowTextPaintOutset fillOutset = snow_scene_text_fill_outset(&item);
    const double canvasToDocument = layout->safeZoom / layout->resolution.scale;
    const double horizontalPadding = fillOutset.x * canvasToDocument;
    const double verticalPadding = fillOutset.y * canvasToDocument;
    const double radius =
        qMax(qMax(item.corner_radii.top_left, item.corner_radii.top_right),
             qMax(item.corner_radii.bottom_right, item.corner_radii.bottom_left)) *
        canvasToDocument;

    painter.save();
    painter.translate(localRect.left(), localRect.top() + layout->topOffset);
    painter.scale(layout->resolution.scale, layout->resolution.scale);
    painter.setPen(Qt::NoPen);

    const double fillCoordinateScale = canvasToDocument;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextLayout* blockLayout = block.layout();
        if (blockLayout == nullptr) {
            continue;
        }
        const QRectF blockRect = document.documentLayout()->blockBoundingRect(block);
        for (int index = 0; index < blockLayout->lineCount(); ++index) {
            const QTextLine line = blockLayout->lineAt(index);
            if (!line.isValid()) {
                continue;
            }
            // x() is the layout box origin; naturalTextRect() also includes
            // Qt's horizontal alignment offset within that box.
            QRectF lineRect = line.naturalTextRect().translated(blockRect.topLeft());
            lineRect.setWidth(qMax(1.0, lineRect.width()));
            lineRect.setHeight(qMax(1.0, lineRect.height()));
            lineRect.adjust(-horizontalPadding, -verticalPadding, horizontalPadding,
                            verticalPadding);
            const double clampedRadius =
                qMin(radius, qMin(lineRect.width(), lineRect.height()) / 2.0);
            QPainterPath backgroundPath;
            if (clampedRadius > 0.0) {
                backgroundPath.addRoundedRect(lineRect, clampedRadius, clampedRadius);
            } else {
                backgroundPath.addRect(lineRect);
            }
            snow_canvas_fill_render::drawTextBackgroundFill(painter, backgroundPath, item.fill,
                                                            item.fill_style, item.font_size,
                                                            fillCoordinateScale);
        }
    }

    painter.restore();
}

void drawStroke(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                const QRectF& localRect, double zoom) {
    const double strokeWidth = item.stroke_width * zoom;
    if (item.stroke.a == 0 || strokeWidth <= 0.0 || item.font_size <= 0.0) {
        return;
    }
    const QString text = snow_canvas_text::textFromSceneItem(item);
    if (text.isEmpty()) {
        return;
    }

    const auto layout = preparedLayout(painter, item, baseFont, zoom, text, RenderPhase::Stroke);

    painter.save();
    painter.translate(localRect.left(), localRect.top() + layout->topOffset);
    painter.scale(layout->resolution.scale, layout->resolution.scale);
    text_layout::drawDocument(painter, *layout);
    painter.restore();
}

void drawHoverUnderlines(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                         const QRectF& localRect, double zoom, const QColor& color,
                         double strokeWidth) {
    const QString text = snow_canvas_text::textFromSceneItem(item);
    if (text.isEmpty() || !color.isValid() || color.alpha() == 0 || strokeWidth <= 0.0 ||
        item.font_size <= 0.0) {
        return;
    }

    const auto layout = preparedLayout(painter, item, baseFont, zoom, text, RenderPhase::Geometry);
    const QTextDocument& document = layout->textDocument();

    painter.save();
    painter.translate(localRect.left(), localRect.top() + layout->topOffset);
    painter.scale(layout->resolution.scale, layout->resolution.scale);
    painter.setPen(QPen(color, strokeWidth / layout->resolution.scale, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextLayout* blockLayout = block.layout();
        if (blockLayout == nullptr) {
            continue;
        }
        const QRectF blockRect = document.documentLayout()->blockBoundingRect(block);
        for (int index = 0; index < blockLayout->lineCount(); ++index) {
            const QTextLine line = blockLayout->lineAt(index);
            if (!line.isValid()) {
                continue;
            }
            const QRectF lineRect = line.naturalTextRect().translated(blockRect.topLeft());
            if (lineRect.width() <= 0.0) {
                continue;
            }
            painter.drawLine(lineRect.bottomLeft(), lineRect.bottomRight());
        }
    }
    painter.restore();
}

} // namespace snow_canvas_text_render
