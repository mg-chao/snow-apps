#include "snow_canvas_cursor_controller.h"

#include <QWidget>
#include "snow_canvas_input_adapter.h"

#include <algorithm>
#include <cmath>
#include <utility>

SnowCanvasCursorController::SnowCanvasCursorController(QWidget& widget) : m_widget(widget) {}

void SnowCanvasCursorController::setCursor(SnowCanvasCursorLayer layer, const QCursor& cursor) {
    if (layer == SnowCanvasCursorLayer::CanvasTool) {
        m_engineCursorStyle.reset();
    }
    cursorForLayer(layer) = cursor;
    applyResolvedCursor(m_widget.devicePixelRatioF());
}

void SnowCanvasCursorController::clearCursor(SnowCanvasCursorLayer layer) {
    if (layer == SnowCanvasCursorLayer::CanvasTool) {
        m_engineCursorStyle.reset();
        configureSerialNumberCursor(nullptr);
    }
    cursorForLayer(layer).reset();
    applyResolvedCursor(m_widget.devicePixelRatioF());
}

void SnowCanvasCursorController::setCrosshairCursorFactory(CrosshairCursorFactory factory) {
    m_crosshairCursorFactory = std::move(factory);
    m_crosshairCursor.reset();
    applyResolvedCursor(m_widget.devicePixelRatioF());
}

std::optional<QCursor>& SnowCanvasCursorController::cursorForLayer(SnowCanvasCursorLayer layer) {
    switch (layer) {
    case SnowCanvasCursorLayer::Host:
        return m_hostCursor;
    case SnowCanvasCursorLayer::CanvasTool:
    default:
        return m_canvasToolCursor;
    }
}

void SnowCanvasCursorController::applyResolvedCursor(qreal devicePixelRatio) {
    if (m_hostCursor.has_value()) {
        applyResolvedCursorToWidget(*m_hostCursor, devicePixelRatio);
        return;
    }
    if (m_canvasToolCursor.has_value()) {
        applyResolvedCursorToWidget(*m_canvasToolCursor, devicePixelRatio);
        return;
    }
    if (m_widget.testAttribute(Qt::WA_SetCursor)) {
        m_widget.unsetCursor();
    }
}

void SnowCanvasCursorController::applyResolvedCursorToWidget(const QCursor& cursor,
                                                             qreal devicePixelRatio) {
    QCursor resolvedCursor = cursor;
    if (cursor.shape() == Qt::CrossCursor && m_crosshairCursorFactory) {
        const qreal dpr =
            std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
        if (!m_crosshairCursor || dpr != m_crosshairCursorDevicePixelRatio) {
            m_crosshairCursor = m_crosshairCursorFactory(dpr);
            m_crosshairCursorDevicePixelRatio = dpr;
        }
        resolvedCursor = *m_crosshairCursor;
    }
    // QWidget forwards every cursor change to the platform window, and on
    // Windows each changed-shape transition reaches the native sprite
    // immediately. Re-applying the cursor a widget already shows would flash
    // it, so layered updates must resolve to a no-op here.
    if (m_widget.testAttribute(Qt::WA_SetCursor) && m_widget.cursor() == resolvedCursor) {
        return;
    }
    m_widget.setCursor(resolvedCursor);
}

void SnowCanvasCursorController::setEngineCursor(SnowCursorStyle style) {
    setEngineCursor(style, m_widget.devicePixelRatioF());
}

void SnowCanvasCursorController::setEngineCursor(SnowCursorStyle style, qreal dpr) {
    if (dpr != m_cursorDevicePixelRatio) {
        m_cursorDevicePixelRatio = dpr;
        m_strokeCursor.reset();
        m_eraserCursor.reset();
        m_serialNumberCursor.reset();
    }
    if (style == SNOW_CURSOR_STYLE_STROKE || style == SNOW_CURSOR_STYLE_ERASER) {
        auto& cached = style == SNOW_CURSOR_STYLE_STROKE ? m_strokeCursor : m_eraserCursor;
        if (!cached) {
            cached = snow_canvas_input::strokeCursor(
                style == SNOW_CURSOR_STYLE_STROKE ? m_strokeDiameter : 16.0,
                style == SNOW_CURSOR_STYLE_STROKE ? m_strokeColor : std::nullopt,
                style == SNOW_CURSOR_STYLE_STROKE, dpr);
        }
        m_canvasToolCursor = *cached;
    } else if (style == SNOW_CURSOR_STYLE_SERIAL_NUMBER && m_serialNumberPreview) {
        if (!m_serialNumberCursor) {
            m_serialNumberCursor = snow_canvas_input::serialNumberCursor(
                *m_serialNumberPreview, m_widget.font(), m_serialNumberZoom, dpr);
        }
        m_canvasToolCursor = *m_serialNumberCursor;
    } else {
        m_canvasToolCursor = snow_canvas_input::cursorForSnowCursor(style, dpr);
    }
    m_engineCursorStyle = style;
    applyResolvedCursor(dpr);
}

void SnowCanvasCursorController::configureStrokeCursor(double diameter,
                                                       const std::optional<QColor>& color) {
    // Bound native bitmap allocation even at extreme canvas zoom levels.
    diameter = std::isfinite(diameter) ? std::clamp(diameter, 1.0, 1024.0) : 1.0;
    if (m_strokeDiameter == diameter && m_strokeColor == color &&
        m_cursorDevicePixelRatio == m_widget.devicePixelRatioF()) {
        return;
    }
    m_strokeDiameter = diameter;
    m_strokeColor = color;
    m_strokeCursor.reset();
    refreshDevicePixelRatio(m_widget.devicePixelRatioF());
}

void SnowCanvasCursorController::refreshDevicePixelRatio(qreal devicePixelRatio) {
    if (m_engineCursorStyle) {
        setEngineCursor(*m_engineCursorStyle, devicePixelRatio);
    } else {
        applyResolvedCursor(devicePixelRatio);
    }
}

void SnowCanvasCursorController::configureSerialNumberCursor(const SnowCanvasSceneItem* preview,
                                                             double zoom) {
    if (preview == nullptr) {
        m_serialNumberPreview.reset();
        m_serialNumberCursorKey.clear();
        m_serialNumberCursor.reset();
        return;
    }
    const QByteArray key =
        snow_canvas_input::serialNumberCursorKey(*preview, m_widget.font(), zoom);
    if (m_serialNumberPreview && key == m_serialNumberCursorKey) {
        return;
    }
    m_serialNumberPreview = *preview;
    m_serialNumberCursorKey = key;
    m_serialNumberZoom = zoom;
    m_serialNumberCursor.reset();
    refreshDevicePixelRatio(m_widget.devicePixelRatioF());
}
