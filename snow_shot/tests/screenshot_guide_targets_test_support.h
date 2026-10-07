#ifndef SNOW_SHOT_TESTS_SCREENSHOT_GUIDE_TARGETS_TEST_SUPPORT_H
#define SNOW_SHOT_TESTS_SCREENSHOT_GUIDE_TARGETS_TEST_SUPPORT_H

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotoverlaycanvaspresenter.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace screenshot_guide_targets_tests {

inline void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

inline void verifyRectangleEnd(SnowCanvasWidget& canvas, SnowCanvasRuntime& runtime,
                               const QPointF& guide, bool snaps) {
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate presenter guide shape tool");
    const QTransform transform = canvas.canvasToViewTransform();
    const QPointF end = transform.map(guide) - QPointF(3.0, 3.0);
    // Start inside a small viewport even when a grabbed drag ends beyond its edge.
    const QPointF start = transform.map(guide) - QPointF(250.0, 240.0);
    const auto send = [&canvas](QEvent::Type type, const QPointF& point) {
        const bool moving = type == QEvent::MouseMove;
        QMouseEvent event(type, point, point, point, moving ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    send(QEvent::MouseButtonPress, start);
    send(QEvent::MouseMove, end);
    send(QEvent::MouseButtonRelease, end);
    const auto document = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                              .object()
                              .value(QStringLiteral("document"))
                              .toObject();
    QJsonObject rectangle;
    for (const auto& slot : document.value(QStringLiteral("slots")).toArray()) {
        const auto data = slot.toObject().value(QStringLiteral("data")).toObject();
        if (data.contains(QStringLiteral("Rectangle"))) {
            rectangle = data.value(QStringLiteral("Rectangle")).toObject();
        }
    }
    require(!rectangle.isEmpty(), "presenter guide test must create a rectangle");
    const auto center = rectangle.value(QStringLiteral("center")).toObject();
    const QPointF actual(center.value(QStringLiteral("x")).toDouble() +
                             rectangle.value(QStringLiteral("width")).toDouble() / 2.0,
                         center.value(QStringLiteral("y")).toDouble() +
                             rectangle.value(QStringLiteral("height")).toDouble() / 2.0);
    const QPointF expected = snaps ? guide : transform.inverted().map(end);
    require(std::abs(actual.x() - expected.x()) < 1e-9 &&
                std::abs(actual.y() - expected.y()) < 1e-9,
            "presenter guide targets must follow selection, viewport, ownership, and visibility");
    require(canvas.undo(), "undo presenter guide test rectangle");
}

template <typename EventSink> void run() {
    EventSink eventSink;
    SnowCanvasRuntime firstRuntime;
    SnowCanvasRuntime secondRuntime;
    auto* firstCanvas = new SnowCanvasWidget(firstRuntime);
    auto* secondCanvas = new SnowCanvasWidget(secondRuntime);
    ScreenshotOverlayWindow firstOverlay(eventSink, firstCanvas);
    ScreenshotOverlayWindow secondOverlay(eventSink, secondCanvas);
    firstOverlay.resize(800, 600);
    secondOverlay.resize(800, 600);
    firstOverlay.show();
    secondOverlay.show();
    QApplication::processEvents();
    firstCanvas->setInteractionEnabled(true);
    secondCanvas->setInteractionEnabled(true);
    require(firstCanvas->setViewportCamera(0.0, 0.0, 1.0) &&
                secondCanvas->setViewportCamera(0.0, 0.0, 1.0),
            "initialize presenter guide cameras");

    CapturedDisplayModel display;
    display.active = true;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display, &firstOverlay);
    displays.appendDisplay(display, &secondOverlay);
    ScreenshotOverlayCanvasPresenter presenter({});
    const QPointF cursor(10.0, 12.0);
    const auto selectionGuides = [&] {
        presenter.updateGuideLines(displays, &firstOverlay, cursor, true, Qt::transparent,
                                   Qt::transparent, Qt::green);
    };
    firstOverlay.setScreenshotSelection(QRectF(50.0, 40.0, 100.0, 80.0), false, 0);
    selectionGuides();
    selectionGuides();
    verifyRectangleEnd(*firstCanvas, firstRuntime, QPointF(100.0, 80.0), true);
    firstOverlay.setScreenshotSelection(QRectF(150.0, 140.0, 100.0, 80.0), false, 0);
    selectionGuides();
    verifyRectangleEnd(*firstCanvas, firstRuntime, QPointF(200.0, 180.0), true);

    require(firstCanvas->setViewportCamera(100.0, 100.0, 2.0), "change presenter guide camera");
    selectionGuides();
    verifyRectangleEnd(*firstCanvas, firstRuntime, QPointF(200.0, 180.0), true);
    require(firstCanvas->setViewportCamera(0.0, 0.0, 1.0), "restore presenter guide camera");
    firstOverlay.setScreenshotSelection(QRectF(250.0, 160.0, 100.0, 80.0), false, 0);
    selectionGuides();
    verifyRectangleEnd(*firstCanvas, firstRuntime, QPointF(300.0, 200.0), true);
    firstOverlay.resize(400, 300);
    QApplication::processEvents();
    selectionGuides();
    verifyRectangleEnd(*firstCanvas, firstRuntime, QPointF(300.0, 200.0), false);
    firstOverlay.resize(800, 600);
    QApplication::processEvents();
    selectionGuides();
    verifyRectangleEnd(*firstCanvas, firstRuntime, QPointF(300.0, 200.0), true);

    require(firstCanvas->setViewportCamera(60.0, 40.0, 2.0), "move monitor guide camera");
    presenter.updateGuideLines(displays, &firstOverlay, cursor, true, Qt::transparent, Qt::blue);
    verifyRectangleEnd(*firstCanvas, firstRuntime, QPointF(60.0, 40.0), true);
    presenter.updateGuideLines(displays, &secondOverlay, cursor, true, Qt::transparent, Qt::blue);
    verifyRectangleEnd(*firstCanvas, firstRuntime, QPointF(60.0, 40.0), false);
    verifyRectangleEnd(*secondCanvas, secondRuntime, QPointF(), true);
    presenter.updateGuideLines(displays, &secondOverlay, cursor, false, Qt::transparent, Qt::blue);
    verifyRectangleEnd(*secondCanvas, secondRuntime, QPointF(), false);
    presenter.updateGuideLines(displays, &secondOverlay, cursor, true, Qt::transparent, Qt::blue);
    presenter.clearGuideLines(displays);
    verifyRectangleEnd(*secondCanvas, secondRuntime, QPointF(), false);
}

} // namespace screenshot_guide_targets_tests

#endif // SNOW_SHOT_TESTS_SCREENSHOT_GUIDE_TARGETS_TEST_SUPPORT_H
