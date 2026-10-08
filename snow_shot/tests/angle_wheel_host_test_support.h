#pragma once

#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QWheelEvent>

#include <cstdlib>
#include <functional>
#include <iostream>

namespace angle_wheel_host_test_support {
inline void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

inline void click(SnowCanvasWidget& canvas, QPointF point) {
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, point, canvas.mapToGlobal(point), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    }
    QApplication::processEvents();
}

inline QString label(const QByteArray& documentPayload) {
    const auto entries = QJsonDocument::fromJson(documentPayload)
                             .object()
                             .value(QStringLiteral("document"))
                             .toObject()
                             .value(QStringLiteral("slots"))
                             .toArray();
    for (const auto& slot : entries) {
        const auto text = slot.toObject()
                              .value(QStringLiteral("data"))
                              .toObject()
                              .value(QStringLiteral("Text"))
                              .toObject();
        if (!text.isEmpty())
            return text.value(QStringLiteral("text")).toString();
    }
    return {};
}

inline void exercise(SnowCanvasWidget& canvas,
                     const std::function<QByteArray()>& readDocument = {}) {
    require(canvas.setCanvasTool(SnowCanvasTool::Angle), "activate angle in host canvas");
    auto style = canvas.canvasAngleStyle();
    style.decimalPlaces = 1;
    require(canvas.setCanvasAngleStyle(style), "set visible fine precision for angle host fixture");
    const QPointF vertex(canvas.width() * 0.5, canvas.height() * 0.5);
    click(canvas, {canvas.width() * 0.75, vertex.y()});
    click(canvas, vertex);
    click(canvas, {vertex.x(), canvas.height() * 0.25});
    if (readDocument) {
        const auto createdLabel = label(readDocument());
        if (createdLabel != QStringLiteral("90.0°"))
            std::cerr << "Created angle label: " << createdLabel.toStdString() << '\n';
        require(createdLabel == QStringLiteral("90.0°"), "host creates angle annotations");
    }
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select the host angle for wheel input");
    click(canvas, {canvas.width() * 0.65, vertex.y()});
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedAngle,
            "angle selected through the host's selection tool");
    const auto initialImage = canvas.grab().toImage();
    const auto camera = canvas.canvasToViewTransform();
    const auto wheel = [&](int delta, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                           Qt::ScrollPhase phase = Qt::NoScrollPhase) {
        QWheelEvent event(vertex, canvas.mapToGlobal(vertex), {}, {0, delta}, Qt::NoButton,
                          modifiers, phase, false);
        event.setTimestamp(100);
        QApplication::sendEvent(&canvas, &event);
        QApplication::processEvents();
    };
    wheel(240);
    require(canvas.canvasAngleStyle().strokeWidth == style.strokeWidth &&
                canvas.canvasToViewTransform() == camera && canvas.grab().toImage() != initialImage,
            "host wheel edits the angle while preserving stroke width and navigation");
    if (readDocument)
        require(label(readDocument()) == QStringLiteral("92.0°"),
                "host retains all coalesced wheel steps");
    wheel(120, Qt::ShiftModifier);
    require(canvas.canvasToViewTransform() == camera,
            "shift angle wheel takes precedence over horizontal host navigation");
    if (readDocument)
        require(label(readDocument()) == QStringLiteral("92.1°"),
                "host shift wheel adjusts one tenth degree");
    const auto beforeMomentum = readDocument ? readDocument() : QByteArray();
    wheel(120, Qt::NoModifier, Qt::ScrollMomentum);
    if (readDocument)
        require(readDocument() == beforeMomentum, "host rejects angle wheel momentum");
    wheel(120, Qt::ControlModifier);
    if (readDocument)
        require(label(readDocument()) == QStringLiteral("92.1°"),
                "control wheel preserves navigation without editing angles");
}
} // namespace angle_wheel_host_test_support
