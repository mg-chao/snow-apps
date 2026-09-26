#pragma once

#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTextDocument>
#include <QTextLayout>
#include <QTransform>
#include <QVector>

#include <memory>

#include "snow_draw_engine.h"

namespace snow_canvas_text_layout {

struct FontResolution {
    QFont font;
    double scale = 1.0;
};

struct DocumentLayout {
    double safeZoom = 1.0;
    double itemWidth = 1.0;
    double itemHeight = 1.0;
    double topOffset = 0.0;
    FontResolution resolution;
    std::unique_ptr<QTextDocument> document;

    QTextDocument& textDocument() {
        return *document;
    }
    const QTextDocument& textDocument() const {
        return *document;
    }
};

struct SingleLineLayout {
    FontResolution resolution;
    std::unique_ptr<QTextLayout> layout;
    QRectF layoutBounds;
    QRectF visualBounds;

    QTextLayout& textLayout() {
        return *layout;
    }
    const QTextLayout& textLayout() const {
        return *layout;
    }
};

// Qt is the exact text layout authority. Rust owns layout intent and stores the
// returned rectangle, but all glyph metrics, wrapping, cursor geometry, and
// editor range geometry are resolved here.
FontResolution resolveFont(QFont baseFont, const SnowSceneDisplayItem& item, double zoom);
QFont fontForItem(QFont baseFont, const SnowSceneDisplayItem& item, double zoom);
void applyFontPixelSize(QFont& font, double pixelSize);

// A text measurement: the stored layout rectangle (wrap or natural width plus
// the auto-resize safety margin, document height) together with the painted
// ink box (widest laid-out line, document height). Serial connectors and dirty
// regions anchor to the ink box aligned inside the layout rectangle, so both
// must travel together.
struct TextMeasuredLayout {
    QSizeF layout;
    QSizeF content;
};

QSizeF measureNaturalText(const QString& text, const QFont& baseFont,
                          const SnowSceneDisplayItem& item, double zoom = 1.0);
TextMeasuredLayout measureNaturalTextLayout(const QString& text, const QFont& baseFont,
                                            const SnowSceneDisplayItem& item, double zoom = 1.0);
QSizeF measureWrappedText(const QString& text, const QFont& baseFont,
                          const SnowSceneDisplayItem& item, double width, double zoom = 1.0);
TextMeasuredLayout measureWrappedTextLayout(const QString& text, const QFont& baseFont,
                                            const SnowSceneDisplayItem& item, double width,
                                            double zoom = 1.0);
double measureMinimumWrappedWidth(const QFont& baseFont, const SnowSceneDisplayItem& item,
                                  double zoom = 1.0);

DocumentLayout createDocumentLayout(const SnowSceneDisplayItem& item, const QFont& baseFont,
                                    double zoom, const QString& text, bool substituteEmptyText);
QTransform documentToViewTransform(const SnowSceneDisplayItem& item, const QPointF& centerView,
                                   const DocumentLayout& layout);
QRectF documentContentsRect(const DocumentLayout& layout);
void drawDocument(QPainter& painter, const DocumentLayout& layout);
QRectF documentRectToLocalItemRect(const QRectF& documentRect, const DocumentLayout& layout);
QVector<QRectF> rangeRectsInDocument(const QTextDocument& document, int rangeStart, int rangeEnd);
QRectF cursorRectInDocument(const QTextDocument& document, int cursorPosition);
QRectF caretPaintRectInDocument(const DocumentLayout& layout, int cursorPosition);
SingleLineLayout createSingleLineLayout(const QString& text, const QFont& baseFont,
                                        const SnowSceneDisplayItem& item, double zoom);

} // namespace snow_canvas_text_layout
