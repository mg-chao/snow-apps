#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>

#include <cstdlib>
#include <cmath>
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
void drawTemplateRoundTrip() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(800, 600);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0, 0, 1), "set draw-template camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "select draw-template shape tool");
    mouse(canvas, QEvent::MouseButtonPress, {300, 200});
    mouse(canvas, QEvent::MouseMove, {500, 400});
    mouse(canvas, QEvent::MouseButtonRelease, {500, 400});
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select draw-template move tool");
    mouse(canvas, QEvent::MouseButtonPress, {400, 200});
    mouse(canvas, QEvent::MouseButtonRelease, {400, 200});

    const QByteArray payload = runtime.serializeSelectedDrawTemplate();
    require(!payload.isEmpty(), "export selected elements through the Qt and C APIs");
    const QJsonArray original = records(runtime, QStringLiteral("Rectangle"));
    require(original.size() == 1, "draw-template fixture should contain one rectangle");
    require(canvas.insertDrawTemplate(payload, QPointF(100, 120)),
            "insert draw template through the Qt and C APIs");
    const QJsonArray inserted = records(runtime, QStringLiteral("Rectangle"));
    require(inserted.size() == 2 && inserted.first() == original.first(),
            "draw-template insertion should preserve the original element");
    const QJsonObject center =
        inserted.last().toObject().value(QStringLiteral("center")).toObject();
    require(center.value(QStringLiteral("x")).toDouble() == 100 &&
                center.value(QStringLiteral("y")).toDouble() == 120,
            "draw-template insertion should use the requested center");
    const auto selected = QJsonDocument::fromJson(runtime.serializeSelectedDrawTemplate()).object();
    require(selected.value(QStringLiteral("selectedIds")).toArray().size() == 1,
            "draw-template insertion should select its new element");
    require(!canvas.insertDrawTemplate(QByteArrayLiteral("invalid"), QPointF(0, 0)) &&
                records(runtime, QStringLiteral("Rectangle")) == inserted,
            "malformed draw templates should leave the document unchanged");
    require(canvas.undo(), "undo draw-template insertion");
    require(records(runtime, QStringLiteral("Rectangle")) == original,
            "one undo should remove the inserted draw template");
    require(canvas.redo(), "redo draw-template insertion");
    require(records(runtime, QStringLiteral("Rectangle")) == inserted,
            "redo should restore the inserted draw template");
}
enum class TextGesture { Copy, Resize, Rotate };

struct TextGestureCase {
    bool editText = true;
    bool changeText = false;
    bool drag = true;
    bool expandText = false;
    double borderOffset = 0.0;
    double zoom = 1.0;
    TextGesture gesture = TextGesture::Copy;
};

void selectedTextGesture(const TextGestureCase& testCase) {
    const auto& [editText, changeText, drag, expandText, borderOffset, zoom, gesture] = testCase;
    for (const auto tool : {SnowCanvasTool::Select, SnowCanvasTool::Text}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(800, 600);
        canvas.show();
        QApplication::processEvents();
        require(canvas.setViewportCamera(0, 0, zoom), "set text camera");
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
        QPointF point = canvas.canvasToViewTransform().map(
            QPointF(center.value(QStringLiteral("x")).toDouble(),
                    center.value(QStringLiteral("y")).toDouble()));
        const auto textRecord = original.first().toObject();
        const QPointF extent(textRecord.value(QStringLiteral("width")).toDouble() * zoom / 2 + 30,
                             textRecord.value(QStringLiteral("height")).toDouble() * zoom / 2 + 30);
        mouse(canvas, QEvent::MouseButtonPress, point - extent);
        mouse(canvas, QEvent::MouseMove, point + extent);
        mouse(canvas, QEvent::MouseButtonRelease, point + extent);
        require(canvas.setCanvasTool(tool), "set text test tool");
        if (tool == SnowCanvasTool::Text) {
            // Switching tools clears selection; Shift-click selects without entering editing.
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::ShiftModifier);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::ShiftModifier);
        }
        const QString replacement = expandText
                                        ? QStringLiteral("updated text with a much wider draft")
                                        : QStringLiteral("updated text");
        if (editText) {
            mouse(canvas, QEvent::MouseButtonPress, point);
            mouse(canvas, QEvent::MouseButtonRelease, point);
            require(canvas.hasActiveTextEditing(), "ordinary click enters text editing");
            if (changeText) {
                QKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
                QApplication::sendEvent(&canvas, &selectAll);
                QKeyEvent replace(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, replacement);
                QApplication::sendEvent(&canvas, &replace);
            }
        }
        if (expandText) {
            // Start inside the expanded draft, beyond the committed element's old bounds.
            point.rx() += (textRecord.value(QStringLiteral("width")).toDouble() / 2 + 30) * zoom;
        }
        if (borderOffset > 0.0) {
            // Stay away from the corner resize handles and the centered rotation handle.
            point.rx() -= textRecord.value(QStringLiteral("width")).toDouble() * zoom / 4;
            point.ry() -=
                textRecord.value(QStringLiteral("height")).toDouble() * zoom / 2 + borderOffset;
        }
        if (gesture == TextGesture::Rotate) {
            // The rotation control is 20 view pixels above the 14-pixel padded frame.
            point.ry() -= textRecord.value(QStringLiteral("height")).toDouble() * zoom / 2 + 34;
        }
        mouse(canvas, QEvent::MouseButtonPress, point, Qt::AltModifier);
        if (gesture != TextGesture::Copy) {
            require(canvas.hasActiveTextEditing(), "Alt handle press preserves the active draft");
            const QPointF delta = gesture == TextGesture::Rotate ? QPointF(40, 0) : QPointF(0, -40);
            mouse(canvas, QEvent::MouseMove, point + delta, Qt::AltModifier);
            mouse(canvas, QEvent::MouseButtonRelease, point + delta, Qt::AltModifier);
            require(canvas.hasActiveTextEditing(), "Alt handle drag preserves text editing");
            require(canvas.resetEditingState(), "commit transformed text");
            const auto resized = records(runtime, QStringLiteral("Text"));
            require(resized.size() == 1, "Alt handle drag must not duplicate text");
            const QString property = gesture == TextGesture::Rotate ? QStringLiteral("rotation")
                                                                    : QStringLiteral("height");
            require(resized.first().toObject().value(property) != textRecord.value(property),
                    "Alt handle drag must perform the requested transform");
            require(resized.first().toObject().value(QStringLiteral("text")) ==
                        original.first().toObject().value(QStringLiteral("text")),
                    "Alt handle drag preserves text content");
            if (gesture == TextGesture::Rotate) {
                const auto rotated = resized.first().toObject();
                const auto rotatedCenter = rotated.value(QStringLiteral("center")).toObject();
                point = canvas.canvasToViewTransform().map(
                    QPointF(rotatedCenter.value(QStringLiteral("x")).toDouble(),
                            rotatedCenter.value(QStringLiteral("y")).toDouble()));
                require(canvas.setCanvasTool(tool), "restore tool for rotated text copy");
                mouse(canvas, QEvent::MouseButtonPress, point, Qt::ShiftModifier);
                mouse(canvas, QEvent::MouseButtonRelease, point, Qt::ShiftModifier);
                mouse(canvas, QEvent::MouseButtonPress, point);
                mouse(canvas, QEvent::MouseButtonRelease, point);
                require(canvas.hasActiveTextEditing(), "edit rotated text before copying");
                mouse(canvas, QEvent::MouseButtonPress, point, Qt::AltModifier);
                require(!canvas.hasActiveTextEditing(), "rotated text copy commits its draft");
                mouse(canvas, QEvent::MouseMove, point + QPointF(80, 60), Qt::AltModifier);
                mouse(canvas, QEvent::MouseButtonRelease, point + QPointF(80, 60), Qt::AltModifier);
                const auto copies = records(runtime, QStringLiteral("Text"));
                require(copies.size() == 2 && copies.first() == resized.first(),
                        "rotated text copy must preserve the original");
                require(copies.last().toObject().value(QStringLiteral("rotation")) ==
                            rotated.value(QStringLiteral("rotation")),
                        "rotated text copy must preserve rotation");
                require(canvas.undo(), "undo rotated text copy");
                require(records(runtime, QStringLiteral("Text")) == resized,
                        "undo rotated copy must preserve the earlier rotation");
            }
            continue;
        }
        require(!canvas.hasActiveTextEditing(), "Alt press on selected text bypasses editor");
        const auto committed = records(runtime, QStringLiteral("Text"));
        require(committed.size() == 1, "Alt press creates no copy");
        if (changeText) {
            require(committed.first().toObject().value(QStringLiteral("text")).toString() ==
                        replacement,
                    "copy gesture commits pending text edits");
        } else {
            require(committed == original, "copy press preserves unchanged text");
        }
        if (!drag) {
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::AltModifier);
            require(records(runtime, QStringLiteral("Text")) == committed,
                    "Alt-click on edited text creates no copy");
            continue;
        }
        mouse(canvas, QEvent::MouseMove, point + QPointF(80, 60), Qt::AltModifier);
        require(records(runtime, QStringLiteral("Text")) == committed,
                "text copy preview leaves committed document unchanged");
        mouse(canvas, QEvent::MouseButtonRelease, point + QPointF(80, 60), Qt::AltModifier);
        const auto copies = records(runtime, QStringLiteral("Text"));
        require(copies.size() == 2 && copies.first() == committed.first(),
                "Alt-drag copies selected text");
        auto expectedCopy = committed.first().toObject();
        auto expectedCenter = expectedCopy.value(QStringLiteral("center")).toObject();
        expectedCenter.insert(QStringLiteral("x"),
                              expectedCenter.value(QStringLiteral("x")).toDouble() + 80 / zoom);
        expectedCenter.insert(QStringLiteral("y"),
                              expectedCenter.value(QStringLiteral("y")).toDouble() + 60 / zoom);
        auto actualCopy = copies.last().toObject();
        const auto actualCenter = actualCopy.value(QStringLiteral("center")).toObject();
        require(std::abs(actualCenter.value(QStringLiteral("x")).toDouble() -
                         expectedCenter.value(QStringLiteral("x")).toDouble()) < 1e-8 &&
                    std::abs(actualCenter.value(QStringLiteral("y")).toDouble() -
                             expectedCenter.value(QStringLiteral("y")).toDouble()) < 1e-8,
                "copy is positioned at the drag offset");
        actualCopy.remove(QStringLiteral("center"));
        expectedCopy.remove(QStringLiteral("center"));
        require(actualCopy == expectedCopy, "copy preserves all text and style properties");
        require(!canvas.hasActiveTextEditing(), "copy gesture does not activate text editing");
        require(canvas.undo(), "undo text copy");
        require(records(runtime, QStringLiteral("Text")) == committed,
                "one undo removes only the copy, preserving text edits");
        require(canvas.redo(), "redo text copy");
        require(records(runtime, QStringLiteral("Text")) == copies, "redo restores text copy");
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
    drawTemplateRoundTrip();
    TextGestureCase testCase;
    testCase.editText = false;
    selectedTextGesture(testCase);
    selectedTextGesture({});
    testCase = {};
    testCase.changeText = true;
    selectedTextGesture(testCase);
    testCase.drag = false;
    selectedTextGesture(testCase);
    testCase.drag = true;
    testCase.expandText = true;
    selectedTextGesture(testCase);
    testCase = {};
    testCase.borderOffset = 11.0;
    selectedTextGesture(testCase);
    testCase.drag = false;
    selectedTextGesture(testCase);
    testCase = {};
    testCase.changeText = true;
    testCase.borderOffset = 7.0;
    selectedTextGesture(testCase);
    for (double zoom : {0.75, 1.0, 2.0}) {
        testCase = {};
        testCase.zoom = zoom;
        testCase.changeText = true;
        testCase.expandText = true;
        selectedTextGesture(testCase);
        testCase = {};
        testCase.zoom = zoom;
        testCase.borderOffset = 11.0;
        selectedTextGesture(testCase);
        // Two pixels outside the padded frame is within the resize hit tolerance.
        testCase.borderOffset = 16.0;
        testCase.gesture = TextGesture::Resize;
        selectedTextGesture(testCase);
        testCase.borderOffset = 0.0;
        testCase.gesture = TextGesture::Rotate;
        selectedTextGesture(testCase);
    }
    std::cout << "Duplicate drag tests passed\n";
}
