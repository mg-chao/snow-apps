#pragma once

#include <QFont>
#include <QColor>
#include <QRectF>
#include <cstdint>

#include <cstddef>

#include "snow_draw_engine.h"

class QPainter;

namespace snow_canvas_text_render {

constexpr std::size_t kRenderCacheEntryLimit = 512;
constexpr std::size_t kRenderCacheCharacterLimit = 256u * 1024u;
constexpr std::size_t kRenderCacheByteLimit = 4u * 1024u * 1024u;

struct RenderCacheDiagnostics {
    std::size_t hits = 0;
    std::size_t builds = 0;
    std::size_t evictions = 0;
    std::size_t entries = 0;
    // Document characters, including the terminal paragraph character; this is not a byte count.
    std::size_t retainedCharacters = 0;
    std::size_t estimatedBytes = 0;
};

RenderCacheDiagnostics renderCacheDiagnosticsForCurrentThread();
void clearRenderCacheForCurrentThread();

struct LayoutCacheStats {
    qsizetype entries = 0;
    qsizetype estimatedBytes = 0;
    std::uint64_t builds = 0;
};

void resetLayoutCacheForCurrentThread();
LayoutCacheStats layoutCacheStatsForCurrentThread();

void drawContents(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                  const QRectF& localRect, double zoom);
void drawBackground(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                    const QRectF& localRect, double zoom);
void drawStroke(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                const QRectF& localRect, double zoom);
void drawHoverUnderlines(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                         const QRectF& localRect, double zoom, const QColor& color,
                         double strokeWidth);

} // namespace snow_canvas_text_render
