#ifndef SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_CURSOR_H
#define SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_CURSOR_H

#include <QCursor>

// The same cursor is shared by canvas shapes and host-owned selection decorations.
[[nodiscard]] QCursor snowCanvasCornerRadiusCursor(qreal devicePixelRatio = 1.0);

#endif // SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_CURSOR_H
