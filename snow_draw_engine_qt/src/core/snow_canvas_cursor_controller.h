#pragma once

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QCursor>

#include <optional>

class QWidget;

class SnowCanvasCursorController final {
  public:
    explicit SnowCanvasCursorController(QWidget& widget);

    void setCursor(SnowCanvasCursorLayer layer, const QCursor& cursor);
    void clearCursor(SnowCanvasCursorLayer layer);

  private:
    std::optional<QCursor>& cursorForLayer(SnowCanvasCursorLayer layer);
    void applyResolvedCursor();
    void applyResolvedCursorToWidget(const QCursor& cursor);

    QWidget& m_widget;
    std::optional<QCursor> m_canvasToolCursor;
    std::optional<QCursor> m_hostCursor;
};
