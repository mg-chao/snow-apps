#pragma once

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QCursor>
#include <QColor>
#include "snow_draw_engine.h"

#include <optional>

class QWidget;

class SnowCanvasCursorController final {
  public:
    explicit SnowCanvasCursorController(QWidget& widget);

    void setCursor(SnowCanvasCursorLayer layer, const QCursor& cursor);
    void clearCursor(SnowCanvasCursorLayer layer);
    void setEngineCursor(SnowCursorStyle style);
    void configureStrokeCursor(double diameter, const std::optional<QColor>& color);
    void refreshDevicePixelRatio();

  private:
    std::optional<QCursor>& cursorForLayer(SnowCanvasCursorLayer layer);
    void applyResolvedCursor();
    void applyResolvedCursorToWidget(const QCursor& cursor);

    QWidget& m_widget;
    std::optional<SnowCursorStyle> m_engineCursorStyle;
    double m_strokeDiameter = 2.0;
    std::optional<QColor> m_strokeColor;
    qreal m_cursorDevicePixelRatio = 0.0;
    std::optional<QCursor> m_strokeCursor;
    std::optional<QCursor> m_eraserCursor;
    std::optional<QCursor> m_canvasToolCursor;
    std::optional<QCursor> m_hostCursor;
};
