#pragma once

#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTextCursor>

#include "snow_draw_engine.h"

#include <optional>

namespace snow_canvas_text_edit_geometry {

QPointF topAnchorForItem(const SnowSceneDisplayItem& item);
QPointF topAnchorForCreationPoint(const SnowSceneDisplayItem& item, const QPointF& creationPoint);
QPointF centerForTopAnchor(const SnowSceneDisplayItem& item, const QPointF& anchor);
std::optional<QRectF> normalizedTextEditingBounds(const std::optional<QRectF>& bounds);
std::optional<double> automaticTextWidthLimit(const SnowSceneDisplayItem& item,
                                              const QPointF& anchor,
                                              const std::optional<QRectF>& bounds);

int cursorPositionForViewPoint(const SnowSceneDisplayItem& item, const QFont& baseFont,
                               const QPointF& centerView, double zoom, const QPointF& viewPosition,
                               const QString& text);
int movedCursorPosition(const SnowSceneDisplayItem& item, const QFont& baseFont, double zoom,
                        const QString& text, int cursorPosition,
                        QTextCursor::MoveOperation operation,
                        QTextCursor::MoveMode mode = QTextCursor::MoveAnchor);
QRectF cursorRectForTextPosition(const SnowSceneDisplayItem& item, const QFont& baseFont,
                                 const QPointF& centerView, double zoom, const QString& text,
                                 int cursorPosition);
bool viewPointInsideTextItem(const SnowSceneDisplayItem& item, const QPointF& centerView,
                             double zoom, const QPointF& viewPosition);

} // namespace snow_canvas_text_edit_geometry
