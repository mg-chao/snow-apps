#pragma once

#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QCursor>
#include <QColor>
#include "snow_draw_engine.h"
#include "snow_canvas_display_item.h"

#include <optional>
#include <functional>

class QWidget;

class SnowCanvasCursorController final {
  public:
    explicit SnowCanvasCursorController(QWidget& widget);

    void setCursor(SnowCanvasCursorLayer layer, const QCursor& cursor);
    void clearCursor(SnowCanvasCursorLayer layer);
    using CrosshairCursorFactory = std::function<QCursor(qreal devicePixelRatio)>;
    void setCrosshairCursorFactory(CrosshairCursorFactory factory);
    void setEngineCursor(SnowCursorStyle style);
    void configureStrokeCursor(double diameter, const std::optional<QColor>& color);
    void configureSerialNumberCursor(const SnowCanvasSceneItem* preview, double zoom = 1.0);
    void refreshDevicePixelRatio(qreal devicePixelRatio);

  private:
    std::optional<QCursor>& cursorForLayer(SnowCanvasCursorLayer layer);
    void applyResolvedCursor(qreal devicePixelRatio);
    void applyResolvedCursorToWidget(const QCursor& cursor, qreal devicePixelRatio);
    void setEngineCursor(SnowCursorStyle style, qreal devicePixelRatio);

    QWidget& m_widget;
    std::optional<SnowCursorStyle> m_engineCursorStyle;
    double m_strokeDiameter = 2.0;
    std::optional<QColor> m_strokeColor;
    qreal m_cursorDevicePixelRatio = 0.0;
    std::optional<QCursor> m_strokeCursor;
    std::optional<QCursor> m_eraserCursor;
    std::optional<SnowCanvasSceneItem> m_serialNumberPreview;
    QByteArray m_serialNumberCursorKey;
    double m_serialNumberZoom = 1.0;
    std::optional<QCursor> m_serialNumberCursor;
    std::optional<QCursor> m_canvasToolCursor;
    std::optional<QCursor> m_hostCursor;
    CrosshairCursorFactory m_crosshairCursorFactory;
    std::optional<QCursor> m_crosshairCursor;
    qreal m_crosshairCursorDevicePixelRatio = 0.0;
};
