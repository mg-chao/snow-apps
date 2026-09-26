#include "snow_canvas_text_edit_geometry.h"

#include "snow_canvas_text_layout.h"

#include <QAbstractTextDocumentLayout>
#include <QTextCursor>
#include <QTextDocument>

#include <cmath>
#include <limits>

namespace snow_canvas_text_edit_geometry {
namespace {

namespace text_layout = snow_canvas_text_layout;

int boundedTextLength(const QString& text) {
    constexpr qsizetype kMaximumPosition = std::numeric_limits<int>::max();
    return static_cast<int>(qMin(text.size(), kMaximumPosition));
}

QPointF rotatedOffset(const SnowSceneDisplayItem& item, const QPointF& offset) {
    return QPointF(std::cos(item.rotation) * offset.x() - std::sin(item.rotation) * offset.y(),
                   std::sin(item.rotation) * offset.x() + std::cos(item.rotation) * offset.y());
}

QPointF topAnchorOffset(const SnowSceneDisplayItem& item) {
    switch (item.text_horizontal_align) {
    case SNOW_TEXT_HORIZONTAL_ALIGN_CENTER:
        return QPointF(0.0, -item.height / 2.0);
    case SNOW_TEXT_HORIZONTAL_ALIGN_RIGHT:
        return QPointF(item.width / 2.0, -item.height / 2.0);
    case SNOW_TEXT_HORIZONTAL_ALIGN_LEFT:
    default:
        return QPointF(-item.width / 2.0, -item.height / 2.0);
    }
}

} // namespace

QPointF topAnchorForItem(const SnowSceneDisplayItem& item) {
    return QPointF(item.center_x, item.center_y) + rotatedOffset(item, topAnchorOffset(item));
}

QPointF topAnchorForCreationPoint(const SnowSceneDisplayItem& item, const QPointF& creationPoint) {
    return QPointF(creationPoint.x(), creationPoint.y() - item.height / 2.0);
}

QPointF centerForTopAnchor(const SnowSceneDisplayItem& item, const QPointF& anchor) {
    return anchor - rotatedOffset(item, topAnchorOffset(item));
}

int cursorPositionForViewPoint(const SnowSceneDisplayItem& item, const QFont& baseFont,
                               const QPointF& centerView, double zoom, const QPointF& viewPosition,
                               const QString& text) {
    text_layout::DocumentLayout layout =
        text_layout::createDocumentLayout(item, baseFont, zoom, text, true);
    const QPointF delta = viewPosition - centerView;
    const double radians = -item.rotation;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const QPointF local(delta.x() * cosine - delta.y() * sine,
                        delta.x() * sine + delta.y() * cosine);
    const QPointF documentPoint((local.x() + layout.itemWidth / 2.0) / layout.resolution.scale,
                                (local.y() + layout.itemHeight / 2.0 - layout.topOffset) /
                                    layout.resolution.scale);

    int cursorPosition =
        layout.textDocument().documentLayout()->hitTest(documentPoint, Qt::FuzzyHit);
    if (cursorPosition < 0) {
        cursorPosition = 0;
    }
    return qBound(0, cursorPosition, boundedTextLength(text));
}

int movedCursorPosition(const SnowSceneDisplayItem& item, const QFont& baseFont, double zoom,
                        const QString& text, int cursorPosition,
                        QTextCursor::MoveOperation operation, QTextCursor::MoveMode mode) {
    text_layout::DocumentLayout layout =
        text_layout::createDocumentLayout(item, baseFont, zoom, text, true);
    QTextCursor cursor(&layout.textDocument());
    const int documentEnd = qMax(0, layout.textDocument().characterCount() - 1);
    cursor.setPosition(qBound(0, cursorPosition, documentEnd));
    cursor.movePosition(operation, mode);
    return qBound(0, cursor.position(), boundedTextLength(text));
}

QRectF cursorRectForTextPosition(const SnowSceneDisplayItem& item, const QFont& baseFont,
                                 const QPointF& centerView, double zoom, const QString& text,
                                 int cursorPosition) {
    text_layout::DocumentLayout layout =
        text_layout::createDocumentLayout(item, baseFont, zoom, text, true);
    if (layout.textDocument().documentLayout() == nullptr) {
        return {};
    }

    const QRectF cursorRect = text_layout::caretPaintRectInDocument(layout, cursorPosition);
    return text_layout::documentToViewTransform(item, centerView, layout).mapRect(cursorRect);
}

bool viewPointInsideTextItem(const SnowSceneDisplayItem& item, const QPointF& centerView,
                             double zoom, const QPointF& viewPosition) {
    const double safeZoom = qMax(0.0001, zoom);
    const double itemWidth = qMax(1.0, item.width * safeZoom);
    const double itemHeight = qMax(1.0, item.height * safeZoom);
    const QPointF delta = viewPosition - centerView;
    const double cosine = std::cos(-item.rotation);
    const double sine = std::sin(-item.rotation);
    const QPointF local(delta.x() * cosine - delta.y() * sine,
                        delta.x() * sine + delta.y() * cosine);
    return std::abs(local.x()) <= itemWidth / 2.0 && std::abs(local.y()) <= itemHeight / 2.0;
}

} // namespace snow_canvas_text_edit_geometry
