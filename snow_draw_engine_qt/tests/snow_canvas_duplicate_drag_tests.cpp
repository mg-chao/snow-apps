#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const bool move = type == QEvent::MouseMove;
    QMouseEvent event(type, point, point, point, move ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      modifiers);
    QApplication::sendEvent(&canvas, &event);
}
QJsonArray records(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto document = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                              .object()
                              .value(QStringLiteral("document"))
                              .toObject();
    QJsonArray result;
    for (const auto& slot : document.value(QStringLiteral("slots")).toArray()) {
        const auto data = slot.toObject().value(QStringLiteral("data")).toObject();
        if (data.contains(kind)) {
            result.append(data.value(kind));
        }
    }
    return result;
}
void shapeCopyGesture() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(800, 600);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0, 0, 1), "set camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "select shape tool");
    mouse(canvas, QEvent::MouseButtonPress, {300, 200});
    mouse(canvas, QEvent::MouseMove, {500, 400});
    mouse(canvas, QEvent::MouseButtonRelease, {500, 400});
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select move tool");
    mouse(canvas, QEvent::MouseButtonPress, {400, 200});
    mouse(canvas, QEvent::MouseButtonRelease, {400, 200});
    const auto original = records(runtime, QStringLiteral("Rectangle"));
    require(original.size() == 1, "create rectangle");
    mouse(canvas, QEvent::MouseButtonPress, {400, 300}, Qt::AltModifier);
    mouse(canvas, QEvent::MouseButtonRelease, {400, 300}, Qt::AltModifier);
    require(records(runtime, QStringLiteral("Rectangle")) == original, "Alt-click creates no copy");
    mouse(canvas, QEvent::MouseButtonPress, {400, 300}, Qt::AltModifier);
    mouse(canvas, QEvent::MouseMove, {450, 340});
    require(records(runtime, QStringLiteral("Rectangle")) == original,
            "preview leaves document unchanged");
    mouse(canvas, QEvent::MouseButtonRelease, {470, 360});
    const auto copied = records(runtime, QStringLiteral("Rectangle"));
    require(copied.size() == 2 && copied.first() == original.first(), "copy preserves original");
    const auto center = copied.last().toObject().value(QStringLiteral("center")).toObject();
    require(center.value(QStringLiteral("x")).toDouble() == 70 &&
                center.value(QStringLiteral("y")).toDouble() == 60,
            "release position determines final displacement");
    require(canvas.undo(), "undo copy");
    require(records(runtime, QStringLiteral("Rectangle")) == original, "one undo removes copy");
    require(canvas.redo(), "redo copy");
    require(records(runtime, QStringLiteral("Rectangle")) == copied, "redo restores copy");
}
void selectedTextCopiesInsteadOfEditing() {
    for (const auto tool : {SnowCanvasTool::Select, SnowCanvasTool::Text}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(800, 600);
        canvas.show();
        QApplication::processEvents();
        require(canvas.setViewportCamera(0, 0, 1), "set text camera");
        require(canvas.setCanvasTool(SnowCanvasTool::Text), "select text tool");
        mouse(canvas, QEvent::MouseButtonPress, {400, 300});
        mouse(canvas, QEvent::MouseButtonRelease, {400, 300});
        QKeyEvent type(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
                       QStringLiteral("copy this text"));
        QApplication::sendEvent(&canvas, &type);
        require(canvas.setCanvasTool(SnowCanvasTool::Select), "commit text by changing tool");
        require(!canvas.hasActiveTextEditing(), "text committed");
        const auto original = records(runtime, QStringLiteral("Text"));
        require(original.size() == 1, "create text");
        const auto center = original.first().toObject().value(QStringLiteral("center")).toObject();
        const QPointF point = canvas.canvasToViewTransform().map(
            QPointF(center.value(QStringLiteral("x")).toDouble(),
                    center.value(QStringLiteral("y")).toDouble()));
        const auto textRecord = original.first().toObject();
        const QPointF extent(textRecord.value(QStringLiteral("width")).toDouble() / 2 + 30,
                             textRecord.value(QStringLiteral("height")).toDouble() / 2 + 30);
        mouse(canvas, QEvent::MouseButtonPress, point - extent);
        mouse(canvas, QEvent::MouseMove, point + extent);
        mouse(canvas, QEvent::MouseButtonRelease, point + extent);
        require(canvas.setCanvasTool(tool), "set text test tool");
        mouse(canvas, QEvent::MouseButtonPress, point, Qt::AltModifier);
        require(!canvas.hasActiveTextEditing(), "Alt press on selected text bypasses editor");
        mouse(canvas, QEvent::MouseMove, point + QPointF(80, 60), Qt::AltModifier);
        mouse(canvas, QEvent::MouseButtonRelease, point + QPointF(80, 60), Qt::AltModifier);
        const auto copies = records(runtime, QStringLiteral("Text"));
        require(copies.size() == 2 && copies.first() == original.first(),
                "Alt-drag copies selected text");
        require(!canvas.hasActiveTextEditing(), "copy gesture does not activate text editing");
    }
}
} // namespace
int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
    }
#endif
    QApplication app(argc, argv);
    shapeCopyGesture();
    std::cout << "Duplicate drag tests passed\n";
}
