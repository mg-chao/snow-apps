#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QColor>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}

void key(SnowCanvasWidget& canvas, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         const QString& text = {}) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
    QApplication::sendEvent(&canvas, &event);
}

void wheel(SnowCanvasWidget& canvas, const QPointF& position, int angleDeltaY) {
    QWheelEvent event(position, position, QPoint(0, 0), QPoint(0, angleDeltaY), Qt::NoButton,
                      Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(&canvas, &event);
}

QJsonArray records(const SnowCanvasRuntime& runtime, const QString& kind) {
    const QJsonObject document = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                                     .object()
                                     .value(QStringLiteral("document"))
                                     .toObject();
    QJsonArray result;
    for (const auto& slot : document.value(QStringLiteral("slots")).toArray()) {
        const auto record = slot.toObject();
        if (record.value(QStringLiteral("data")).toObject().contains(kind)) {
            result.append(record);
        }
    }
    return result;
}

QJsonObject payload(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto found = records(runtime, kind);
    require(found.size() == 1, "expected one record of the requested kind");
    return found.first().toObject().value(QStringLiteral("data")).toObject().value(kind).toObject();
}

void clickAndDragLifecycle() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number tool");
    mouse(canvas, QEvent::MouseButtonPress, {100.0, 100.0}, Qt::LeftButton, Qt::LeftButton);
    require(records(runtime, QStringLiteral("SerialNumber")).size() == 1,
            "press creates serial number immediately");
    const auto serial = payload(runtime, QStringLiteral("SerialNumber"));
    mouse(canvas, QEvent::MouseMove, {102.0, 100.0}, Qt::NoButton, Qt::LeftButton);
    require(records(runtime, QStringLiteral("Text")).isEmpty(), "jitter does not attach text");
    mouse(canvas, QEvent::MouseButtonRelease, {102.0, 100.0}, Qt::LeftButton, Qt::NoButton);
    require(!canvas.hasActiveTextEditing(), "click leaves text editing inactive");
    require(payload(runtime, QStringLiteral("SerialNumber")) == serial,
            "click keeps number at press position");

    mouse(canvas, QEvent::MouseButtonPress, {300.0, 180.0}, Qt::LeftButton, Qt::LeftButton);
    require(records(runtime, QStringLiteral("SerialNumber")).size() == 2,
            "next press creates another number");
    mouse(canvas, QEvent::MouseMove, {380.0, 180.0}, Qt::NoButton, Qt::LeftButton);
    require(records(runtime, QStringLiteral("Text")).size() == 1,
            "drag attaches one bound text before release");
    require(!canvas.hasActiveTextEditing(), "drag places text without entering editing");
    mouse(canvas, QEvent::MouseMove, {240.0, 240.0}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {220.0, 260.0}, Qt::LeftButton, Qt::NoButton);
    require(canvas.hasActiveTextEditing(), "release enters text editing");
    require(canvas.testAttribute(Qt::WA_InputMethodEnabled), "release enables text input");
    const auto text = payload(runtime, QStringLiteral("Text"));
    const auto center = text.value(QStringLiteral("center")).toObject();
    const QPointF expected = canvas.canvasToViewTransform().inverted().map(QPointF(220.0, 260.0));
    require(center.value(QStringLiteral("x")).toDouble() == expected.x() &&
                center.value(QStringLiteral("y")).toDouble() == expected.y(),
            "text is placed at the final release position");
    require(text.value(QStringLiteral("width")).toDouble() > 1.0,
            "release persists the host-measured label layout, not the placeholder width");
    key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("Drag label"));
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(!canvas.hasActiveTextEditing(), "commit closes editor");
    require(payload(runtime, QStringLiteral("Text")).value(QStringLiteral("text")).toString() ==
                QStringLiteral("Drag label"),
            "typing after release updates the attached text");
    require(records(runtime, QStringLiteral("Text")).size() == 1,
            "typing does not create another text element");
}

void releaseWithoutMoveAndCancellation() {
    for (int mode = 0; mode < 3; ++mode) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(600, 360);
        canvas.show();
        QApplication::processEvents();
        require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number tool");
        mouse(canvas, QEvent::MouseButtonPress, {100.0, 100.0}, Qt::LeftButton, Qt::LeftButton);
        if (mode != 0) {
            mouse(canvas, QEvent::MouseMove, {250.0, 200.0}, Qt::NoButton, Qt::LeftButton);
            if (mode == 1) {
                key(canvas, Qt::Key_Escape);
            } else {
                QFocusEvent lost(QEvent::FocusOut, Qt::OtherFocusReason);
                QApplication::sendEvent(&canvas, &lost);
            }
        }
        mouse(canvas, QEvent::MouseButtonRelease, {250.0, 200.0}, Qt::LeftButton, Qt::NoButton);
        require(canvas.hasActiveTextEditing() == (mode == 0),
                "only an uncancelled release starts editing, even without move events");
    }
}

bool textRecordHasColor(const QJsonObject& color, int red, int green, int blue, int alpha) {
    return color.value(QStringLiteral("r")).toInt() == red &&
           color.value(QStringLiteral("g")).toInt() == green &&
           color.value(QStringLiteral("b")).toInt() == blue &&
           color.value(QStringLiteral("a")).toInt() == alpha;
}

QJsonObject serialRecordByNumber(const SnowCanvasRuntime& runtime, int number) {
    const auto found = records(runtime, QStringLiteral("SerialNumber"));
    for (const auto& slot : found) {
        const auto record = slot.toObject()
                                .value(QStringLiteral("data"))
                                .toObject()
                                .value(QStringLiteral("SerialNumber"))
                                .toObject();
        if (record.value(QStringLiteral("number")).toInt() == number) {
            return record;
        }
    }
    require(false, "expected a serial number record with the requested number");
    return {};
}

double boundTextFontSize(const SnowCanvasRuntime& runtime) {
    return payload(runtime, QStringLiteral("Text")).value(QStringLiteral("font_size")).toDouble();
}

// Creates badge 1 with a plain click and badge 2 with a drag that attaches a
// bound label, typing label text and leaving the editor session active.
void createBadgeWithEditedBoundText(SnowCanvasWidget& canvas) {
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number tool");
    mouse(canvas, QEvent::MouseButtonPress, {100.0, 100.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {102.0, 100.0}, Qt::LeftButton, Qt::NoButton);
    mouse(canvas, QEvent::MouseButtonPress, {300.0, 180.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {380.0, 180.0}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {380.0, 180.0}, Qt::LeftButton, Qt::NoButton);
    require(canvas.hasActiveTextEditing(), "drag places the bound label into editing");
    // An empty label is deleted on commit, so give the label content up front.
    key(canvas, Qt::Key_W, Qt::NoModifier, QStringLiteral("Wheel label"));
}

void wheelResizesSelectedSerialBoundText() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    createBadgeWithEditedBoundText(canvas);

    const double badgeFontSize =
        serialRecordByNumber(runtime, 2).value(QStringLiteral("font_size")).toDouble();
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(!canvas.hasActiveTextEditing(),
            "commit closes the editor and keeps the label selected");
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedText,
            "committed bound label is the style toolbar source");
    const double initialFontSize = boundTextFontSize(runtime);
    require(initialFontSize == badgeFontSize, "bound label starts at the serial number font size");

    wheel(canvas, {380.0, 180.0}, 120);
    require(boundTextFontSize(runtime) == initialFontSize + 1.0,
            "serial-tool wheel resizes the selected bound text element");
    require(serialRecordByNumber(runtime, 2).value(QStringLiteral("font_size")).toDouble() ==
                badgeFontSize,
            "resizing the selected bound text leaves the badge font size alone");

    wheel(canvas, {380.0, 180.0}, -120);
    require(boundTextFontSize(runtime) == initialFontSize,
            "negative serial-tool wheel delta shrinks the selected bound text element");
}

void wheelResizesEditedSerialBoundText() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    createBadgeWithEditedBoundText(canvas);

    const double initialFontSize = boundTextFontSize(runtime);
    wheel(canvas, {380.0, 180.0}, 120);
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(!canvas.hasActiveTextEditing(), "commit closes the editor");
    require(boundTextFontSize(runtime) == initialFontSize + 1.0,
            "serial-tool wheel resizes the bound text element while it is being edited");
}

void wheelStillResizesSelectedSerialNumberBadge() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();

    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number tool");
    mouse(canvas, QEvent::MouseButtonPress, {100.0, 100.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {102.0, 100.0}, Qt::LeftButton, Qt::NoButton);
    require(records(runtime, QStringLiteral("SerialNumber")).size() == 1,
            "click creates the badge");

    mouse(canvas, QEvent::MouseButtonPress, {100.0, 100.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {100.0, 100.0}, Qt::LeftButton, Qt::NoButton);
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedSerialNumber,
            "selected badge is the style toolbar source");
    const double initialFontSize = payload(runtime, QStringLiteral("SerialNumber"))
                                       .value(QStringLiteral("font_size"))
                                       .toDouble();
    wheel(canvas, {100.0, 100.0}, 120);
    require(payload(runtime, QStringLiteral("SerialNumber"))
                    .value(QStringLiteral("font_size"))
                    .toDouble() == initialFontSize + 1.0,
            "serial-tool wheel still resizes a selected serial number badge");
    require(records(runtime, QStringLiteral("Text")).isEmpty(),
            "badge wheel stepping creates no text element");
}

void wheelResizesTextToolText() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();

    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text tool");
    const double defaultFontSize = canvas.canvasStyleToolbarState().textStyle.fontSize;
    mouse(canvas, QEvent::MouseButtonPress, {200.0, 150.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {200.0, 150.0}, Qt::LeftButton, Qt::NoButton);
    require(canvas.hasActiveTextEditing(), "click starts a new text draft");
    key(canvas, Qt::Key_L, Qt::NoModifier, QStringLiteral("Label"));
    wheel(canvas, {200.0, 150.0}, 120);
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(!canvas.hasActiveTextEditing(), "commit closes the editor");
    require(boundTextFontSize(runtime) == defaultFontSize + 1.0,
            "text-tool wheel resizes the text while it is being edited");

    // Finishing a second edit of the committed text restores its selection,
    // leaving the text selected but no longer edited.
    mouse(canvas, QEvent::MouseButtonPress, {200.0, 150.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {200.0, 150.0}, Qt::LeftButton, Qt::NoButton);
    require(canvas.hasActiveTextEditing(), "click reopens the committed text");
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(!canvas.hasActiveTextEditing(), "commit closes the editor again");
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedText,
            "committed text is the style toolbar source");
    wheel(canvas, {200.0, 150.0}, 120);
    require(boundTextFontSize(runtime) == defaultFontSize + 2.0,
            "text-tool wheel resizes the selected text element");
}

void wheelStepsToolDefaultWithoutSelection() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();

    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number tool");
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::DefaultSerialNumber,
            "serial tool with no selection uses the serial default source");
    const double serialDefault = canvas.canvasStyleToolbarState().serialNumberStyle.fontSize;
    wheel(canvas, {300.0, 180.0}, 120);
    require(canvas.canvasStyleToolbarState().serialNumberStyle.fontSize == serialDefault + 1.0,
            "serial-tool wheel without selection steps the serial number default style");

    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text tool");
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::DefaultText,
            "text tool with no selection uses the text default source");
    const double textDefault = canvas.canvasStyleToolbarState().textStyle.fontSize;
    wheel(canvas, {300.0, 180.0}, 120);
    require(canvas.canvasStyleToolbarState().textStyle.fontSize == textDefault + 1.0,
            "text-tool wheel without selection steps the text default style");
    require(records(runtime, QStringLiteral("Text")).isEmpty() &&
                records(runtime, QStringLiteral("SerialNumber")).isEmpty(),
            "default-style wheel stepping creates no elements");
}

void toolbarCreatedTextKeepsDefaultStyling() {
    // The floating serial toolbar's Create Text button and the serial drag are
    // the two ways to attach a bound label. Both must commit the label with the
    // default text styling intact: the editor session has to begin from the
    // label's styled scene item, or its commit strips the fill, color, and
    // stroke the label was created with.
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();

    SnowCanvasTextStyle style;
    style.color = QColor(0xff, 0xff, 0xff, 0xff);
    style.fill = QColor(0x21, 0x6b, 0xa5, 0xff);
    require(canvas.setCanvasTextStyle(style), "apply default text style");

    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number tool");
    mouse(canvas, QEvent::MouseButtonPress, {100.0, 100.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {102.0, 100.0}, Qt::LeftButton, Qt::NoButton);
    require(records(runtime, QStringLiteral("SerialNumber")).size() == 1,
            "click creates the badge");

    require(canvas.setCanvasTool(SnowCanvasTool::Select), "activate select tool");
    mouse(canvas, QEvent::MouseButtonPress, {100.0, 100.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {102.0, 100.0}, Qt::LeftButton, Qt::NoButton);
    require(canvas.createSerialNumberText(), "toolbar Create Text attaches a label");
    require(canvas.hasActiveTextEditing(), "Create Text starts editing the label");
    key(canvas, Qt::Key_T, Qt::NoModifier, QStringLiteral("Toolbar label"));
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(!canvas.hasActiveTextEditing(), "commit closes the editor");

    const auto toolbarText = payload(runtime, QStringLiteral("Text"));
    require(textRecordHasColor(toolbarText.value(QStringLiteral("fill")).toObject(), 0x21, 0x6b,
                               0xa5, 0xff),
            "toolbar-created label keeps the default fill color");
    require(textRecordHasColor(toolbarText.value(QStringLiteral("color")).toObject(), 0xff, 0xff,
                               0xff, 0xff),
            "toolbar-created label keeps the default text color");
}
} // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
    }
#endif
    QApplication app(argc, argv);
    clickAndDragLifecycle();
    releaseWithoutMoveAndCancellation();
    toolbarCreatedTextKeepsDefaultStyling();
    wheelResizesSelectedSerialBoundText();
    wheelResizesEditedSerialBoundText();
    wheelStillResizesSelectedSerialNumberBadge();
    wheelResizesTextToolText();
    wheelStepsToolDefaultWithoutSelection();
    std::cout << "Serial number drag tests passed\n";
}
