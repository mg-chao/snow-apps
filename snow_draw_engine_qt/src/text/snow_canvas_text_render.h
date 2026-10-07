#pragma once

#include <QFont>
#include <QColor>
#include <QRectF>

#include <cstddef>

#include "snow_draw_engine.h"

class QPainter;

namespace snow_canvas_text_render {

constexpr std::size_t kRenderCacheEntryLimit = 512;
constexpr std::size_t kRenderCacheCharacterLimit = 256u * 1024u;

struct RenderCacheDiagnostics {
    std::size_t hits = 0;
    std::size_t builds = 0;
    std::size_t evictions = 0;
    std::size_t entries = 0;
    // Document characters, including the terminal paragraph character; this is not a byte count.
    std::size_t retainedCharacters = 0;
};

RenderCacheDiagnostics renderCacheDiagnosticsForCurrentThread();
void clearRenderCacheForCurrentThread();

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
