#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
    QApplication::processEvents();
}

void drag(SnowCanvasWidget& canvas, QPointF start, QPointF end) {
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}

QJsonObject magnifier(const SnowCanvasRuntime& runtime) {
    const auto document = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                              .object()
                              .value(QStringLiteral("document"))
                              .toObject();
    QJsonObject result;
    int count = 0;
    for (const auto& slot : document.value(QStringLiteral("slots")).toArray()) {
        const auto data = slot.toObject().value(QStringLiteral("data")).toObject();
        if (data.contains(QStringLiteral("Magnifier"))) {
            result = data.value(QStringLiteral("Magnifier")).toObject();
            ++count;
        }
    }
    require(count == 1, "a drag creates one compound magnifier annotation");
    return result;
}

QPointF center(const QJsonObject& value, const QString& field) {
    const auto point = value.value(field).toObject();
    return {point.value(QStringLiteral("x")).toDouble(),
            point.value(QStringLiteral("y")).toDouble()};
}

bool near(QPointF a, QPointF b) {
    return std::hypot(a.x() - b.x(), a.y() - b.y()) < 0.001;
}

void prepare(SnowCanvasWidget& canvas) {
    canvas.resize(800, 600);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Magnifier), "activate Magnifier");
}

void compoundDraggingAndDefaultIsolation() {
    for (const auto shape : {SnowCanvasRectangleShape::Rectangle, SnowCanvasRectangleShape::Ellipse,
                             SnowCanvasRectangleShape::Diamond}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        prepare(canvas);
        auto style = canvas.canvasMagnifierStyle();
        style.shape = shape;
        require(canvas.setCanvasMagnifierStyle(style), "set magnifier creation shape");
        drag(canvas, {220, 160}, {300, 220});
        const auto created = magnifier(runtime);
        const auto source = center(created, QStringLiteral("source_center"));
        const auto lens = center(created, QStringLiteral("magnified_center"));
        require(near(source, lens) && created.value(QStringLiteral("width")).toDouble() == 80 &&
                    created.value(QStringLiteral("height")).toDouble() == 60 &&
                    created.value(QStringLiteral("factor")).toDouble() == 2,
                "creation retains the entire source and initially aligns both regions");

        const QPointF delta(30, 15);
        const auto sourceView = canvas.canvasToViewTransform().map(source);
        drag(canvas, sourceView, sourceView + delta);
        const auto moved = magnifier(runtime);
        require(near(center(moved, QStringLiteral("source_center")), source + delta) &&
                    near(center(moved, QStringLiteral("magnified_center")), lens + delta),
                "the source wins overlap hit testing and moves both regions");
        require(canvas.undo() && magnifier(runtime) == created,
                "one undo restores the complete source drag");
        require(canvas.redo(), "redo restores the source drag");

        const auto movedLens = center(moved, QStringLiteral("magnified_center"));
        const auto lensView = canvas.canvasToViewTransform().map(movedLens + QPointF(60, 0));
        drag(canvas, lensView, lensView + QPointF(180, 0));
        const auto separated = magnifier(runtime);
        require(near(center(separated, QStringLiteral("source_center")), source + delta) &&
                    near(center(separated, QStringLiteral("magnified_center")),
                         movedLens + QPointF(180, 0)),
                "dragging the exposed magnified region preserves the source");

        const auto resizeHandle =
            canvas.canvasToViewTransform().map(source + delta + QPointF(40, 30));
        drag(canvas, resizeHandle, resizeHandle + QPointF(20, 10));
        const auto resized = magnifier(runtime);
        require(resized.value(QStringLiteral("width")).toDouble() == 100 &&
                    resized.value(QStringLiteral("height")).toDouble() == 70 &&
                    near(center(resized, QStringLiteral("magnified_center")),
                         center(separated, QStringLiteral("magnified_center"))) &&
                    resized.value(QStringLiteral("factor")).toDouble() == 2,
                "source resize handles preserve the magnified center and factor");
        require(canvas.undo() && magnifier(runtime) == separated,
                "source resizing is independently undoable");

        style.factor = 6;
        require(canvas.applyStyleEdit(
                    SnowCanvasMagnifierEdit{style, SnowCanvasMagnifierStylePropertyFactor, true}),
                "creation defaults can be replicated while an element is selected");
        require(magnifier(runtime).value(QStringLiteral("factor")).toDouble() == 2,
                "default replication cannot mutate a selected magnifier");
        style.factor = 3;
        require(canvas.commitStyleEdit(
                    SnowCanvasMagnifierEdit{style, SnowCanvasMagnifierStylePropertyFactor, false}),
                "selected magnification can be edited");
        require(magnifier(runtime).value(QStringLiteral("factor")).toDouble() == 3 &&
                    canvas.undo() && magnifier(runtime) == separated,
                "selected styles are undoable independently of creation defaults");
    }
}

void coincidentOneTimesLensGrip() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    auto style = canvas.canvasMagnifierStyle();
    style.factor = 1;
    require(canvas.setCanvasMagnifierStyle(style), "set one-times creation default");
    drag(canvas, {220, 160}, {300, 220});
    const auto original = magnifier(runtime);
    const auto source = center(original, QStringLiteral("source_center"));
    const auto sourceView = canvas.canvasToViewTransform().map(source);
    mouse(canvas, QEvent::MouseButtonPress, sourceView, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, sourceView, Qt::LeftButton, Qt::NoButton);
    const auto grip = canvas.canvasToViewTransform().map(source + QPointF(0, 54));
    drag(canvas, grip, grip + QPointF(150, 0));
    const auto moved = magnifier(runtime);
    require(near(center(moved, QStringLiteral("source_center")), source) &&
                near(center(moved, QStringLiteral("magnified_center")), source + QPointF(150, 0)),
            "the external move grip separates a coincident one-times lens");
    require(canvas.undo() && magnifier(runtime) == original,
            "lens grip dragging remains a single undoable operation");
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    compoundDraggingAndDefaultIsolation();
    coincidentOneTimesLensGrip();
    return EXIT_SUCCESS;
}
