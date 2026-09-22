#include "snow_canvas_text_render.h"

#include "snow_canvas_fill_render.h"
#include "snow_canvas_text.h"
#include "snow_canvas_text_layout.h"

#include <QAbstractTextDocumentLayout>
#include <QBrush>
#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextLine>

namespace snow_canvas_text_render {
namespace {

namespace text_layout = snow_canvas_text_layout;

QColor toQColor(const SnowColorRgba8& color) {
    return QColor(color.r, color.g, color.b, color.a);
}

} // namespace

void drawContents(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                  const QRectF& localRect, double zoom) {
    const QString text = snow_canvas_text::textFromSceneItem(item);
    if (text.isEmpty() || item.text_color.a == 0 || item.font_size <= 0.0) {
        return;
    }

    text_layout::DocumentLayout layout =
        text_layout::createDocumentLayout(item, baseFont, zoom, text, false);

    QTextDocument& document = layout.textDocument();
    QTextCursor cursor(&document);
    cursor.select(QTextCursor::Document);
    QTextCharFormat format;
    format.setForeground(QBrush(toQColor(item.text_color)));
    cursor.mergeCharFormat(format);

    painter.save();
    painter.translate(localRect.left(), localRect.top() + layout.topOffset);
    painter.scale(layout.resolution.scale, layout.resolution.scale);
    document.drawContents(&painter, text_layout::documentContentsRect(layout));
    painter.restore();
}

void drawBackground(QPainter& painter, const SnowSceneDisplayItem& item, const QFont& baseFont,
                    const QRectF& localRect, double zoom) {
    if (item.fill.a == 0 || item.font_size <= 0.0) {
        return;
    }

    const QString text = snow_canvas_text::textFromSceneItem(item);
    text_layout::DocumentLayout layout =
        text_layout::createDocumentLayout(item, baseFont, zoom, text, true);

    QTextDocument& document = layout.textDocument();
    // The pill padding comes from the engine's published fill-padding
    // contract. Never measure padding here: a font-metrics line height would
    // drift from the contract edge the dirty regions are derived from.
    const SnowTextPaintOutset fillOutset = snow_scene_text_fill_outset(&item);
    const double canvasToDocument = layout.safeZoom / layout.resolution.scale;
    const double horizontalPadding = fillOutset.x * canvasToDocument;
    const double verticalPadding = fillOutset.y * canvasToDocument;
    const double radius =
        qMax(qMax(item.corner_radii.top_left, item.corner_radii.top_right),
             qMax(item.corner_radii.bottom_right, item.corner_radii.bottom_left)) *
        canvasToDocument;

    painter.save();
    painter.translate(localRect.left(), localRect.top() + layout.topOffset);
    painter.scale(layout.resolution.scale, layout.resolution.scale);
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
    const QString text = snow_canvas_text::textFromSceneItem(item);
    const double strokeWidth = item.stroke_width * zoom;
    if (text.isEmpty() || item.stroke.a == 0 || strokeWidth <= 0.0 || item.font_size <= 0.0) {
        return;
    }

    text_layout::DocumentLayout layout =
        text_layout::createDocumentLayout(item, baseFont, zoom, text, false);

    QTextDocument& document = layout.textDocument();
    QTextCursor cursor(&document);
    cursor.select(QTextCursor::Document);
    QTextCharFormat format;
    QPen outlinePen(toQColor(item.stroke), strokeWidth / layout.resolution.scale, Qt::SolidLine,
                    Qt::RoundCap, Qt::RoundJoin);
    outlinePen.setMiterLimit(2.0);
    format.setTextOutline(outlinePen);
    format.setForeground(QBrush(Qt::transparent));
    cursor.mergeCharFormat(format);

    painter.save();
    painter.translate(localRect.left(), localRect.top() + layout.topOffset);
    painter.scale(layout.resolution.scale, layout.resolution.scale);
    document.drawContents(&painter, text_layout::documentContentsRect(layout));
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

    text_layout::DocumentLayout layout =
        text_layout::createDocumentLayout(item, baseFont, zoom, text, false);
    QTextDocument& document = layout.textDocument();

    painter.save();
    painter.translate(localRect.left(), localRect.top() + layout.topOffset);
    painter.scale(layout.resolution.scale, layout.resolution.scale);
    painter.setPen(QPen(color, strokeWidth / layout.resolution.scale, Qt::SolidLine, Qt::RoundCap,
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
