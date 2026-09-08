#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenrecordingshortcutcontroller.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

class ScreenshotFloatingToolPaletteWindowTestAccess {
  public:
    static void edit(ScreenshotFloatingToolPaletteWindow& toolbar, QWidget& editor) {
        toolbar.beginKeyboardFocusInteraction(&editor);
        toolbar.endKeyboardFocusInteraction(&editor);
    }
};

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void press(QWidget& receiver, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
           bool autoRepeat = false) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), autoRepeat);
    QCoreApplication::sendEvent(&receiver, &event);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QCoreApplication::sendEvent(&receiver, &release);
}

void focus(QWidget& widget) {
    widget.window()->activateWindow();
    widget.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
}

void drawRectangle(SnowCanvasWidget& canvas) {
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "shape should activate");
    const auto mouse = [&canvas](QEvent::Type type, QPointF position, Qt::MouseButton button,
                                 Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &event);
    };
    mouse(QEvent::MouseButtonPress, {30, 30}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, {100, 80}, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {100, 80}, Qt::LeftButton, Qt::NoButton);
    require(canvas.canvasHistoryState().canUndo, "drawing should create undo history");
}

void requireFocusPolicy(QWidget& widget, bool acceptsFocus) {
    require(widget.focusPolicy() == (acceptsFocus ? Qt::StrongFocus : Qt::NoFocus) &&
                widget.testAttribute(Qt::WA_ShowWithoutActivating) != acceptsFocus,
            "window focus policy must survive preparation and text editing");
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const auto style = GetWindowLongPtrW(reinterpret_cast<HWND>(widget.winId()), GWL_EXSTYLE);
        require(((style & WS_EX_NOACTIVATE) == 0) == acceptsFocus,
                "native activation policy must match recording keyboard availability");
    }
#endif
}

void recordingToolbarKeepsFocusAfterEditingAndSurfaceRestoration() {
    ScreenRecordingToolbarWindow recording;
    ScreenshotFloatingToolPaletteWindow screenshot{ScreenshotToolPalette::Options{}};
    for (auto* toolbar :
         {static_cast<ScreenshotFloatingToolPaletteWindow*>(&recording), &screenshot}) {
        const bool acceptsFocus = toolbar == &recording;
        toolbar->prepareForDisplay();
        toolbar->show();
        QCoreApplication::processEvents();
        requireFocusPolicy(*toolbar, acceptsFocus);
        QLineEdit editor(toolbar);
        editor.show();
        ScreenshotFloatingToolPaletteWindowTestAccess::edit(*toolbar, editor);
        requireFocusPolicy(*toolbar, acceptsFocus);
        toolbar->releaseNativeSurface();
        toolbar->restoreNativeSurface();
        toolbar->show();
        QCoreApplication::processEvents();
        requireFocusPolicy(*toolbar, acceptsFocus);
    }
}

void recordingShortcutsFollowBothWindowsAndConfiguredKeys() {
    const snow_shot::storage::DrawingShortcutSettings drawing;
    const snow_shot::storage::ScreenshotShortcutSettings history;
    require(drawing.setShape({QStringLiteral("F6")}) && drawing.setArrow({QStringLiteral("F7")}) &&
                drawing.setShortcuts(QStringLiteral("highlight"), {QStringLiteral("F8")}) &&
                drawing.setShortcuts(QStringLiteral("filter"), {QStringLiteral("F9")}) &&
                history.setShortcuts(QStringLiteral("undo"), {QStringLiteral("Ctrl+Z")}) &&
                history.setShortcuts(QStringLiteral("redo"), {QStringLiteral("Ctrl+Y")}),
            "test shortcut settings should be accepted");

    ScreenRecordingAreaWindow area;
    ScreenRecordingToolbarWindow toolbar;
    area.setPhysicalRegion(
        ScreenshotGeometryMapper::physicalRectForScreen(*QGuiApplication::primaryScreen())
            .adjusted(20, 20, -20, -20));
    auto* palette = toolbar.palette();
    auto* canvas = area.canvas();
    int shapes = 0;
    int arrows = 0;
    int undos = 0;
    int redos = 0;
    QObject::connect(palette, &ScreenshotToolPalette::shapeRequested, &area, [&]() {
        ++shapes;
        static_cast<void>(canvas->setCanvasTool(SnowCanvasTool::Shape));
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    });
    QObject::connect(palette, &ScreenshotToolPalette::arrowRequested, &area, [&]() {
        ++arrows;
        static_cast<void>(canvas->setCanvasTool(SnowCanvasTool::Arrow));
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    });
    QObject::connect(palette, &ScreenshotToolPalette::undoRequested, &area, [&]() {
        ++undos;
        static_cast<void>(canvas->undo());
    });
    QObject::connect(palette, &ScreenshotToolPalette::redoRequested, &area, [&]() {
        ++redos;
        static_cast<void>(canvas->redo());
    });

    auto shortcuts = std::make_unique<ScreenRecordingShortcutController>(area, toolbar);
    area.show();
    toolbar.show();
    focus(toolbar);
    require(!toolbar.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus) &&
                !toolbar.testAttribute(Qt::WA_ShowWithoutActivating) &&
                toolbar.focusPolicy() == Qt::StrongFocus && toolbar.hasFocus(),
            "recording toolbar must accept focus before selecting any drawing tool");
    require(area.focusPolicy() == Qt::StrongFocus &&
                !area.testAttribute(Qt::WA_ShowWithoutActivating),
            "idle recording area must accept keyboard input for region editing");

    for (QWidget* receiver : {static_cast<QWidget*>(&toolbar), static_cast<QWidget*>(&area),
                              static_cast<QWidget*>(canvas), palette->mainPanel()}) {
        for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                                ScreenRecordingAreaWindow::InputMode::RegionEditing,
                                ScreenRecordingAreaWindow::InputMode::Drawing}) {
            area.setInputMode(mode);
            focus(toolbar);
            const int before = shapes;
            press(*receiver, Qt::Key_F6);
            require(shapes == before + 1 &&
                        area.inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing,
                    "each recording scope must activate the first drawing tool exactly once");
        }
    }
    for (const auto state : {ScreenshotToolPalette::RecordingState::Recording,
                             ScreenshotToolPalette::RecordingState::Paused}) {
        area.setRecordingState(state);
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
        require(area.focusPolicy() == Qt::NoFocus &&
                    area.testAttribute(Qt::WA_TransparentForMouseEvents),
                "recording pass-through must retain mouse and keyboard pass-through");
        focus(toolbar);
        const int before = arrows;
        press(toolbar, Qt::Key_F7);
        require(arrows == before + 1, "toolbar shortcuts should enter drawing while recording");
    }
    area.setRecordingState(ScreenshotToolPalette::RecordingState::Idle);

    const auto previousTool = canvas->canvasTool();
    press(toolbar, Qt::Key_F8);
    press(area, Qt::Key_F9);
    require(canvas->canvasTool() == previousTool,
            "recording shortcuts must not activate unavailable highlight or filter tools");

    drawRectangle(*canvas);
    focus(toolbar);
    press(toolbar, Qt::Key_Z, Qt::ControlModifier);
    require(undos == 1 && canvas->canvasHistoryState().canRedo,
            "toolbar undo should operate on recording canvas history");
    press(area, Qt::Key_Y, Qt::ControlModifier);
    require(redos == 1 && canvas->canvasHistoryState().canUndo,
            "area redo should operate on the same recording canvas history");
    press(*canvas, Qt::Key_Z, Qt::ControlModifier);
    press(toolbar, Qt::Key_Z, Qt::ControlModifier);
    require(undos == 2, "unavailable history actions must not emit toolbar commands");
    require(history.setShortcuts(QStringLiteral("redo"), {QStringLiteral("Ctrl+F10")}),
            "redo shortcut should be reconfigurable while recording UI is open");
    press(toolbar, Qt::Key_Y, Qt::ControlModifier);
    require(redos == 1, "old history binding must be removed immediately");
    press(toolbar, Qt::Key_F10, Qt::ControlModifier);
    require(redos == 2, "updated history shortcut should work immediately");

    require(history.setShortcuts(QStringLiteral("undo"), {QStringLiteral("F11")}) &&
                drawing.setArrow({QStringLiteral("F11")}),
            "drawing and history shortcut collision should be configurable");
    const int arrowsBeforeUndo = arrows;
    press(toolbar, Qt::Key_F11);
    require(undos == 3 && arrows == arrowsBeforeUndo,
            "available history action must take priority over drawing tools");
    press(toolbar, Qt::Key_F11);
    require(undos == 3 && arrows == arrowsBeforeUndo + 1,
            "unavailable history action must fall through to a matching drawing shortcut");
    require(history.setShortcuts(QStringLiteral("undo"), {QStringLiteral("Ctrl+Z")}),
            "undo shortcut should restore after testing priority");

    require(drawing.setShape({QStringLiteral("F10")}), "drawing shortcut should be configurable");
    int before = shapes;
    press(toolbar, Qt::Key_F6);
    require(shapes == before, "old drawing binding must be removed immediately");
    press(toolbar, Qt::Key_F10);
    require(shapes == ++before, "updated drawing shortcut should work immediately");
    press(toolbar, Qt::Key_F10, Qt::NoModifier, true);
    require(shapes == before, "held drawing keys must not repeatedly activate tools");

    area.setDrawingBlocked(true);
    press(toolbar, Qt::Key_F10);
    press(area, Qt::Key_Z, Qt::ControlModifier);
    require(shapes == before && undos == 3, "busy recording operations must block shortcuts");
    area.setDrawingBlocked(false);

    QLineEdit editor(&toolbar);
    editor.show();
    focus(editor);
    require(editor.hasFocus(), "editable toolbar control should own keyboard focus");
    press(editor, Qt::Key_F10);
    press(editor, Qt::Key_Z, Qt::ControlModifier);
    require(shapes == before && undos == 3, "text editors must retain shortcut input");
    editor.setReadOnly(true);
    press(editor, Qt::Key_F10);
    require(shapes == ++before, "read-only text controls should allow toolbar shortcuts");
    editor.hide();
    focus(toolbar);

    require(canvas->setCanvasTool(SnowCanvasTool::Text), "text tool should activate");
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    const QPointF textPosition(160, 100);
    QMouseEvent textPress(QEvent::MouseButtonPress, textPosition, textPosition, textPosition,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent textRelease(QEvent::MouseButtonRelease, textPosition, textPosition, textPosition,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &textPress);
    QCoreApplication::sendEvent(canvas, &textRelease);
    require(canvas->hasActiveTextEditing(), "canvas should begin text editing");
    press(*canvas, Qt::Key_F10);
    require(shapes == before, "active canvas text editing must retain drawing shortcut input");
    static_cast<void>(canvas->cancelActiveTextEditing());
    focus(toolbar);

    QWidget unrelated;
    unrelated.show();
    press(unrelated, Qt::Key_F10);
    require(shapes == before, "unrelated windows must not dispatch recording shortcuts");
    QWidget popup(&toolbar, Qt::Tool);
    emit palette->materializedScope(&popup);
    popup.show();
    press(popup, Qt::Key_F10);
    require(shapes == ++before, "materialized toolbar scopes should share recording shortcuts");
    popup.hide();
    press(popup, Qt::Key_F10);
    require(shapes == before, "hidden popups must not dispatch recording shortcuts");
    toolbar.hide();
    press(area, Qt::Key_F10);
    require(shapes == before, "hidden recording UI must not dispatch shortcuts");
    toolbar.show();
    focus(toolbar);
    shortcuts.reset();
    press(toolbar, Qt::Key_F10);
    require(shapes == before, "destroyed shortcut controller must release all bindings");
    shortcuts = std::make_unique<ScreenRecordingShortcutController>(area, toolbar);
    press(toolbar, Qt::Key_F10);
    require(shapes == ++before, "recreated shortcut controller must dispatch exactly once");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated test storage must be available");
    const QString executableDirectory = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executableDirectory), "test executable directory must exist");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({executableDirectory, temporary.path(), 60000}).success,
            "isolated recording shortcut settings must initialize");
    recordingToolbarKeepsFocusAfterEditingAndSurfaceRestoration();
    recordingShortcutsFollowBothWindowsAndConfiguredKeys();
    storage.shutdown();
    return 0;
}
