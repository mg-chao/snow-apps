#include "snow_shot/presentation/globalcanvascontroller.h"
#include "../src/presentation/globalcanvas/globalcanvasplatform.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/shortcutdisplaytext.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "widgets/button.h"
#include "widgets/select.h"
#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include "physical_key_test_support.h"
#include <QMouseEvent>
#include <QScreen>
#include <QTemporaryDir>
#include <QTranslator>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>
#include <cstdlib>
#include <iostream>
#include <QElapsedTimer>
#include <QThread>
#include <QWindow>
#include <QWheelEvent>
#ifdef Q_OS_MACOS
#include "../src/platform/macos/capturewindowlayers_p.h"
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace snow_shot;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
void mouse(QWidget* widget, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, widget->mapToGlobal(point), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
class CanvasTestScreen final : public QPlatformScreen {
  public:
    QRect bounds{-1600, -100, 1600, 1000};
    QRect geometry() const override {
        return bounds;
    }
    QRect availableGeometry() const override {
        return bounds.adjusted(0, 0, 0, -40);
    }
    int depth() const override {
        return 32;
    }
    QImage::Format format() const override {
        return QImage::Format_ARGB32_Premultiplied;
    }
    qreal devicePixelRatio() const override {
        return 1.5;
    }
    QString name() const override {
        return QStringLiteral("GlobalCanvasTestDisplay");
    }
};
void displayLifecycle() {
    auto* native = new CanvasTestScreen;
    QWindowSystemInterface::handleScreenAdded(native);
    QScreen* selected = native->screen();
    presentation::GlobalCanvasController controller(
        nullptr, {[&]() { return selected; }, [](QWidget*, bool) { return true; }});
    controller.activate();
    QApplication::processEvents();
    require(controller.window()->geometry() == selected->geometry(),
            "create on injected pointer display at negative coordinates");
    auto* window = controller.window();
    auto* canvas = controller.canvas();
    const QRectF annotation(60, 80, 30, 20);
    const QRect originalMapping = canvas->viewRectForCanvasRect(annotation);
    native->bounds = QRect(-1900, -200, 1900, 1200);
    QWindowSystemInterface::handleScreenGeometryChange(selected, native->geometry(),
                                                       native->availableGeometry());
    QApplication::processEvents();
    require(window->geometry() == selected->geometry() &&
                canvas->viewRectForCanvasRect(annotation) == originalMapping,
            "display resize preserves document coordinates");
    selected = QGuiApplication::primaryScreen();
    controller.activate();
    require(controller.window() == window && window->geometry() == native->screen()->geometry(),
            "moving pointer to another display toggles the original singleton");
    QWindowSystemInterface::handleScreenRemoved(native);
    QApplication::processEvents();
    require(controller.active() && controller.clickThrough() && controller.window() == window &&
                window->isVisible() && controller.toolbar()->isVisible() &&
                window->geometry() == selected->geometry(),
            "removed display migrates the session to primary");
    controller.shutdown();
}

class CanvasTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QString::fromLatin1(context) == QStringLiteral("ScreenshotToolPalette") &&
            (QString::fromLatin1(source) == QStringLiteral("Click-through") ||
             QString::fromLatin1(source) == QStringLiteral("Exit")))
            return QStringLiteral("Translated ") + QString::fromLatin1(source);
        return {};
    }
};
#ifdef Q_OS_WIN
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 150) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}
class InputProbe final : public QWidget {
  public:
    InputProbe() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint) {}
    int presses = 0;
    void mousePressEvent(QMouseEvent*) override {
        ++presses;
    }
};
void nativeInput() {
    POINT oldCursor{};
    GetCursorPos(&oldCursor);
    const HWND foreground = GetForegroundWindow();
    for (QScreen* screen : QGuiApplication::screens()) {
        InputProbe underlay;
        underlay.setGeometry(QRect(screen->geometry().topLeft() + QPoint(40, 40), QSize(260, 220)));
        underlay.show();
        underlay.raise();
        settle();
        presentation::GlobalCanvasController controller(nullptr,
                                                        {[screen]() { return screen; }, {}});
        controller.activate();
        settle();
        const HWND canvasHandle = reinterpret_cast<HWND>(controller.window()->winId());
        const HWND toolbarHandle = reinterpret_cast<HWND>(controller.toolbar()->winId());
        const HWND probeHandle = reinterpret_cast<HWND>(underlay.winId());
        RECT bounds{};
        GetWindowRect(canvasHandle, &bounds);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromWindow(canvasHandle, MONITOR_DEFAULTTONULL), &info);
        require(EqualRect(&bounds, &info.rcMonitor), "native canvas covers exact monitor pixels");
        POINT point{80, 80};
        ClientToScreen(probeHandle, &point);
        require(WindowFromPoint(point) == canvasHandle,
                "blank editing pixels intercept native input");
        require(SetCursorPos(point.x, point.y) != FALSE, "position pointer over input probe");
        INPUT click[2]{};
        click[0].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].type = INPUT_MOUSE;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        require(SendInput(2, click, sizeof(INPUT)) == 2, "send canvas click");
        settle();
        require(underlay.presses == 0, "editing canvas blocks underlying input");
        controller.activate();
        settle();
        require((GetWindowLongPtr(canvasHandle, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0,
                "native click-through flag enabled");
        require(WindowFromPoint(point) == probeHandle,
                "click-through reaches only the test underlay");
        require(SendInput(2, click, sizeof(INPUT)) == 2, "send click-through click");
        settle();
        require(underlay.presses == 1, "underlying window receives native click");
        auto* dragHandle = controller.toolbar()->palette()->dragHandle();
        const QPoint gripLocal =
            dragHandle->mapTo(controller.toolbar(), dragHandle->rect().center());
        const qreal dragScale = controller.toolbar()->devicePixelRatioF();
        POINT gripPoint{qRound(gripLocal.x() * dragScale), qRound(gripLocal.y() * dragScale)};
        ClientToScreen(toolbarHandle, &gripPoint);
        require(GetAncestor(WindowFromPoint(gripPoint), GA_ROOT) == toolbarHandle,
                "drag handle is an interactive native target");
        const QPoint beforeDrag = controller.toolbar()->contentPosition();
        require(SetCursorPos(gripPoint.x, gripPoint.y) != FALSE,
                "position pointer over drag handle");
        require(SendInput(1, &click[0], sizeof(INPUT)) == 1, "press drag handle");
        settle();
        require(SetCursorPos(gripPoint.x - 40, gripPoint.y - 25) != FALSE, "move toolbar pointer");
        settle();
        require(SendInput(1, &click[1], sizeof(INPUT)) == 1, "release drag handle");
        settle();
        require(controller.toolbar()->contentPosition() != beforeDrag,
                "native toolbar drag changes placement during click-through");
        auto* toggle = controller.toolbar()->palette()->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("globalCanvasClickThroughButton"));
        RECT toolbarBounds{};
        GetClientRect(toolbarHandle, &toolbarBounds);
        const QPoint toggleLocal = toggle->mapTo(controller.toolbar(), toggle->rect().center());
        const qreal scale = controller.toolbar()->devicePixelRatioF();
        POINT togglePoint{qRound(toggleLocal.x() * scale), qRound(toggleLocal.y() * scale)};
        ClientToScreen(toolbarHandle, &togglePoint);
        require(GetAncestor(WindowFromPoint(togglePoint), GA_ROOT) == toolbarHandle,
                "toolbar remains a native input target during click-through");
        require(SetCursorPos(togglePoint.x, togglePoint.y) != FALSE,
                "position pointer over toolbar");
        require(SendInput(2, click, sizeof(INPUT)) == 2, "click toolbar toggle");
        settle();
        require(!controller.clickThrough() && GetForegroundWindow() == canvasHandle,
                "toolbar restores native canvas focus");
        require(WindowFromPoint(point) == canvasHandle, "restored editing intercepts blank pixels");
        controller.shutdown();
    }
    SetCursorPos(oldCursor.x, oldCursor.y);
    if (IsWindow(foreground))
        SetForegroundWindow(foreground);
}
#endif

#ifdef Q_OS_MACOS
void canvasCollectionBehavior() {
    using namespace platform::detail;
    const auto canvasLevel = captureWindowLevel({CaptureFamily::GlobalCanvas, kOverlayLayer});
    require(canvasLevel > CGWindowLevelForKey(kCGMainMenuWindowLevelKey) &&
                canvasLevel > CGWindowLevelForKey(kCGDockWindowLevelKey),
            "canvas covers system chrome");
    require(canvasLevel < captureWindowLevel({CaptureFamily::GlobalCanvas, kToolbarLayer}) &&
                captureWindowLevel({CaptureFamily::GlobalCanvas, 1000}) <
                    captureWindowLevel({CaptureFamily::Screenshot, 0}) &&
                pinnedWindowLevel() < captureWindowLevel({CaptureFamily::Recording, 0}) &&
                captureWindowLevel({CaptureFamily::Recording, 1000}) <
                    captureWindowLevel({CaptureFamily::Screenshot, 0}),
            "canvas tools remain above the canvas and below screenshot windows");
    for (int layer : {kOverlayLayer, kToolbarLayer, kPopupLayer, 1000})
        require(captureWindowLevel({CaptureFamily::GlobalCanvas, layer}) ==
                    captureWindowLevel({CaptureFamily::Recording, layer}),
                "canvas and recording share native levels including nested tools");
    const auto screenshotLevel = CGWindowLevelForKey(kCGScreenSaverWindowLevelKey);
    require(captureWindowLevel({CaptureFamily::Screenshot, 0}) == screenshotLevel &&
                captureWindowLevel({CaptureFamily::Recording, 0}) ==
                    screenshotLevel - kRecordingBandSize &&
                pinnedWindowLevel() == screenshotLevel - kRecordingBandSize - 1,
            "original screenshot, recording, and pin levels remain unchanged");
    QWindow canvas;
    canvas.setProperty(kScreenshotLayer, kOverlayLayer);
    canvas.setProperty(kCaptureFamily, static_cast<int>(CaptureFamily::GlobalCanvas));
    QWindow toolbar;
    toolbar.setTransientParent(&canvas);
    QWindow popup;
    popup.setTransientParent(&toolbar);
    require(captureLayer(&toolbar).family == CaptureFamily::GlobalCanvas &&
                captureWindowLevel(captureLayer(&toolbar)) > canvasLevel &&
                captureWindowLevel(captureLayer(&popup)) >
                    captureWindowLevel(captureLayer(&toolbar)),
            "transient canvas tools and popups inherit the canvas band");
    constexpr NSWindowCollectionBehavior unrelated =
        NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle |
        NSWindowCollectionBehaviorFullScreenDisallowsTiling;
    for (const auto space :
         {NSWindowCollectionBehaviorDefault, NSWindowCollectionBehaviorMoveToActiveSpace,
          NSWindowCollectionBehaviorCanJoinAllSpaces}) {
        for (const auto role :
             {NSWindowCollectionBehaviorDefault, NSWindowCollectionBehaviorFullScreenPrimary,
              NSWindowCollectionBehaviorFullScreenAuxiliary,
              NSWindowCollectionBehaviorFullScreenNone}) {
            const auto result =
                presentation::globalCanvasCollectionBehavior(unrelated | space | role);
            require(result == (unrelated | NSWindowCollectionBehaviorCanJoinAllSpaces |
                               NSWindowCollectionBehaviorFullScreenAuxiliary),
                    "canvas replaces conflicting policies and preserves unrelated window behavior");
            require(presentation::globalCanvasCollectionBehavior(result) == result,
                    "reapplying canvas policy is idempotent");
        }
    }
}

void nativeCanvasLifecycle() {
    for (int session = 0; session < 2; ++session) {
        presentation::GlobalCanvasController controller;
        controller.activate();
        QApplication::processEvents();
        require(controller.active(), "native canvas opens");
        NSWindow* native = reinterpret_cast<NSView*>(controller.window()->winId()).window;
        require(native != nil, "canvas has a native window");
        const auto verify = [&](bool transparent) {
            using namespace platform::detail;
            NSWindow* toolbar = reinterpret_cast<NSView*>(controller.toolbar()->winId()).window;
            require(native.level == captureWindowLevel({CaptureFamily::Recording, 0}) &&
                        toolbar.level > native.level &&
                        toolbar.level < captureWindowLevel({CaptureFamily::Screenshot, 0}),
                    "canvas uses the recording level with its toolbar above it");
            require(NSEqualRects(native.frame, native.screen.frame),
                    "canvas covers the display including the menu bar and notch area");
            const NSRect notchFrame = NSMakeRect(-1920, -80, 1920, 1200);
            require(NSEqualRects([native constrainFrameRect:notchFrame toScreen:native.screen],
                                 notchFrame),
                    "canvas frame bypasses AppKit visible-frame constraints");
            const auto behavior = native.collectionBehavior;
            require((behavior & NSWindowCollectionBehaviorCanJoinAllSpaces) != 0 &&
                        (behavior & NSWindowCollectionBehaviorFullScreenAuxiliary) != 0 &&
                        (behavior & NSWindowCollectionBehaviorMoveToActiveSpace) == 0 &&
                        (behavior & NSWindowCollectionBehaviorFullScreenPrimary) == 0 &&
                        (behavior & NSWindowCollectionBehaviorFullScreenNone) == 0,
                    "native canvas has compatible Space and fullscreen policies");
            require(native.ignoresMouseEvents == transparent && !native.hidesOnDeactivate,
                    "native canvas input mode matches the controller");
        };
        verify(false);
        controller.activate();
        QApplication::processEvents();
        verify(true);
        require(controller.clickThrough() && controller.toolbar()->isVisible(),
                "click-through retains the toolbar");
        controller.activate();
        QApplication::processEvents();
        verify(false);
        native.level = NSFloatingWindowLevel;
        controller.toolbar()->raise();
        QApplication::processEvents();
        verify(false);
        native.collectionBehavior =
            NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenNone;
        QEvent surfaceChanged(QEvent::WinIdChange);
        QApplication::sendEvent(controller.window(), &surfaceChanged);
        QApplication::processEvents();
        verify(false);
        controller.window()->close();
        QApplication::processEvents();
        require(!controller.active(), "native canvas closes");
    }
}
#endif

void toolbarPlacement(QApplication& app) {
    presentation::GlobalCanvasController controller(
        nullptr, {[&]() { return app.primaryScreen(); }, [](QWidget*, bool) { return true; }});
    controller.activate();
    auto* palette = controller.toolbar()->palette();
    palette->freeDrawRequested();
    const auto requireStyleBelowMain = [&]() {
        app.processEvents();
        QWidget* main = palette->mainPanel();
        QWidget* style = palette->stylePanel();
        require(style != nullptr && style->isVisible(), "canvas style toolbar is visible");
        require(style->mapToGlobal(QPoint()).y() >
                    main->mapToGlobal(QPoint(0, main->height() - 1)).y(),
                "canvas style toolbar stays below the main toolbar");
    };
    requireStyleBelowMain();
    controller.activate();
    controller.activate();
    requireStyleBelowMain();
    palette->rectangleFilterRequested();
    requireStyleBelowMain();
    controller.window()->close();
    app.processEvents();
}

void canvasNavigation(QApplication& app) {
    presentation::GlobalCanvasController controller(
        nullptr, {[&]() { return app.primaryScreen(); }, [](QWidget*, bool) { return true; }});
    controller.activate();
    app.processEvents();
    auto* canvas = controller.canvas();
    const QPointF pointer(170, 130);
    const auto wheel = [&](Qt::KeyboardModifiers modifiers, QPoint angle, QPoint pixel = {}) {
        QWheelEvent event(pointer, canvas->mapToGlobal(pointer), pixel, angle, Qt::NoButton,
                          modifiers, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &event);
    };
    const auto closePoint = [](QPointF a, QPointF b) { return QLineF(a, b).length() < 0.001; };
    const QTransform initial = canvas->canvasToViewTransform();
    wheel(Qt::NoModifier, QPoint(0, 120));
    require(canvas->canvasToViewTransform() == initial, "plain wheel does not navigate");
    const QPointF anchor = initial.inverted().map(pointer);
    wheel(Qt::ControlModifier, QPoint(0, 120));
    QTransform zoomed = canvas->canvasToViewTransform();
    require(zoomed.m11() > initial.m11() && closePoint(zoomed.map(anchor), pointer),
            "Ctrl wheel zooms around the pointer");
    wheel(Qt::ControlModifier, QPoint(0, -120));
    require(closePoint(canvas->canvasToViewTransform().map(anchor), initial.map(anchor)) &&
                qAbs(canvas->canvasToViewTransform().m11() - initial.m11()) < 0.001,
            "reverse Ctrl wheel restores zoom");
    canvas->setCanvasTool(SnowCanvasTool::Text);
    const double fontSize = canvas->canvasStyleToolbarState().textStyle.fontSize;
    wheel(Qt::ControlModifier, QPoint(0, 120));
    require(canvas->canvasStyleToolbarState().textStyle.fontSize == fontSize &&
                canvas->canvasToViewTransform().m11() > initial.m11(),
            "fullscreen navigation takes precedence over text font wheel shortcuts");
    zoomed = canvas->canvasToViewTransform();
    wheel(Qt::ShiftModifier, QPoint(0, 120));
    require(closePoint(canvas->canvasToViewTransform().map(anchor),
                       zoomed.map(anchor) + QPointF(40, 0)),
            "Shift wheel pans horizontally in view pixels at any zoom");
    zoomed = canvas->canvasToViewTransform();
    wheel(Qt::ControlModifier | Qt::ShiftModifier, QPoint(), QPoint(0, -17));
    require(closePoint(canvas->canvasToViewTransform().map(anchor),
                       zoomed.map(anchor) + QPointF(0, -17)),
            "Ctrl Shift precise wheel pans vertically without changing zoom");
    zoomed = canvas->canvasToViewTransform();
    mouse(canvas, QEvent::MouseButtonPress, pointer, Qt::MiddleButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseMove, pointer + QPointF(23, 31), Qt::NoButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseButtonRelease, pointer + QPointF(29, 37), Qt::MiddleButton,
          Qt::NoButton);
    require(closePoint(canvas->canvasToViewTransform().map(anchor),
                       zoomed.map(anchor) + QPointF(29, 37)) &&
                !canvas->canvasHistoryState().canUndo &&
                canvas->canvasTool() == SnowCanvasTool::Text,
            "middle drag pans without drawing or changing the tool");
    zoomed = canvas->canvasToViewTransform();
    controller.window()->resize(controller.window()->size() + QSize(20, 30));
    app.processEvents();
    require(closePoint(canvas->canvasToViewTransform().map(anchor), zoomed.map(anchor)) &&
                qAbs(canvas->canvasToViewTransform().m11() - zoomed.m11()) < 0.001,
            "resize preserves pan and zoom");
    mouse(canvas, QEvent::MouseButtonPress, pointer, Qt::MiddleButton, Qt::MiddleButton);
    controller.activate();
    zoomed = canvas->canvasToViewTransform();
    wheel(Qt::ControlModifier, QPoint(0, 120));
    mouse(canvas, QEvent::MouseMove, pointer + QPointF(50, 50), Qt::NoButton, Qt::MiddleButton);
    require(canvas->canvasToViewTransform() == zoomed,
            "click-through disables navigation and cancels dragging");
    controller.activate();
    mouse(canvas, QEvent::MouseMove, pointer + QPointF(60, 60), Qt::NoButton, Qt::NoButton);
    require(canvas->canvasToViewTransform() == zoomed, "restoring input does not resume old drag");
    controller.shutdown();
}

void textEscapePreservesAnnotations(QApplication& app) {
    presentation::GlobalCanvasController controller(
        nullptr, {[&]() { return app.primaryScreen(); }, [](QWidget*, bool) { return true; }});
    controller.activate();
    app.processEvents();
    auto* canvas = controller.canvas();
    canvas->setCanvasTool(SnowCanvasTool::FreeDraw);
    mouse(canvas, QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {180, 140}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {180, 140}, Qt::LeftButton, Qt::NoButton);
    require(canvas->canvasHistoryState().canUndo, "create annotation before text editing");
    canvas->setCanvasTool(SnowCanvasTool::Text);
    mouse(canvas, QEvent::MouseButtonPress, {220, 180}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {220, 180}, Qt::LeftButton, Qt::NoButton);
    require(canvas->hasActiveTextEditing(), "begin global canvas text draft");
    QKeyEvent text(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QApplication::sendEvent(canvas, &text);
    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escape);
    app.processEvents();
    require(controller.active(), "Escape cancels text without destroying annotations");
    require(!canvas->hasActiveTextEditing() && canvas->canvasHistoryState().canUndo,
            "Escape ends the draft and retains annotation history");
    require(canvas->undo() && !canvas->canvasHistoryState().canUndo,
            "cancelled text adds no history entry");
    PhysicalKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    PhysicalKeyEvent exit(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &exit);
    app.processEvents();
    require(!controller.active(), "Escape still closes canvas after text cancellation");
}

void savedToolbarLayout(QApplication& app) {
    const storage::ScreenshotToolbarSettings settings;
    const auto kind = storage::ScreenshotToolbarLayoutKind::DrawingTools;
    const auto original = settings.layout(kind);
    auto visible = original;
    for (auto& position : visible.positions)
        position.removeAll(QStringLiteral("arrow"));
    visible.positions.prepend({QStringLiteral("arrow")});
    visible.hidden.removeAll(QStringLiteral("arrow"));
    auto hidden = visible;
    for (auto& position : hidden.positions)
        position.removeAll(QStringLiteral("arrow"));
    hidden.hidden.append(QStringLiteral("arrow"));
    require(settings.setLayout(kind, visible), "save standalone arrow tool");
    presentation::GlobalCanvasController controller(
        nullptr, {[&]() { return app.primaryScreen(); }, [](QWidget*, bool) { return true; }});
    controller.activate();
    app.processEvents();
    auto* palette = controller.toolbar()->palette();
    auto* arrow =
        palette->findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotArrowButton"));
    require(arrow && arrow->isVisible(), "canvas loads saved drawing tool layout");
    require(settings.setLayout(kind, hidden), "hide arrow tool");
    app.processEvents();
    require(!arrow->isVisible(), "open canvas hides tools removed from the layout");
    require(settings.setLayout(kind, visible), "restore visible arrow tool");
    app.processEvents();
    require(arrow->isVisible(), "open canvas follows drawing layout changes");
    for (const auto* name : {"globalCanvasClickThroughButton", "globalCanvasExitButton"}) {
        auto* button = palette->findChild<adqt::widgets::AdButton*>(QString::fromLatin1(name));
        require(button && button->isVisible(), "layout changes retain canvas actions");
    }
    require(settings.setLayout(kind, original), "restore drawing layout");
}

void templateInsertionAfterNavigation(QApplication& app) {
    SnowCanvasRuntime source;
    SnowCanvasWidget sourceCanvas(source);
    sourceCanvas.resize(400, 300);
    sourceCanvas.show();
    app.processEvents();
    sourceCanvas.setCanvasTool(SnowCanvasTool::Shape);
    mouse(&sourceCanvas, QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(&sourceCanvas, QEvent::MouseMove, {180, 140}, Qt::NoButton, Qt::LeftButton);
    mouse(&sourceCanvas, QEvent::MouseButtonRelease, {180, 140}, Qt::LeftButton, Qt::NoButton);
    sourceCanvas.setCanvasTool(SnowCanvasTool::Select);
    mouse(&sourceCanvas, QEvent::MouseButtonPress, {140, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(&sourceCanvas, QEvent::MouseButtonRelease, {140, 100}, Qt::LeftButton, Qt::NoButton);
    const QByteArray payload = source.serializeSelectedDrawTemplate();
    require(!payload.isEmpty(), "serialize a real drawing template");
    const storage::DrawTemplateSettings settings;
    const auto original = settings.templates();
    require(settings.setTemplates({{QStringLiteral("Rectangle"), payload}}), "save test template");
    presentation::GlobalCanvasController controller(
        nullptr, {[&]() { return app.primaryScreen(); }, [](QWidget*, bool) { return true; }});
    controller.activate();
    app.processEvents();
    auto* canvas = controller.canvas();
    auto* select = controller.toolbar()->palette()->findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotDrawTemplateSelect"));
    require(select != nullptr, "canvas exposes saved templates");
    for (const double zoom : {0.5, 1.0, 2.0}) {
        canvas->clearDocument();
        canvas->setViewportCamera(5000, -3000, zoom);
        select->selected(QStringLiteral("draw-template:0"), QStringLiteral("Rectangle"));
        require(canvas->canvasHistoryState().canUndo, "template insertion creates history");
        canvas->resetEditingState();
        const QImage image = canvas->grab().toImage();
        QRect painted;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y).alpha() > 10)
                    painted = painted.united(QRect(x, y, 1, 1));
        require(!painted.isEmpty() &&
                    QLineF(QRectF(painted).center(), QRectF(image.rect()).center()).length() < 4,
                "templates remain centered in the visible viewport after pan and zoom");
    }
    require(settings.setTemplates(original), "restore template library");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
#ifdef Q_OS_WIN
    // Static Qt's offscreen font database does not discover Windows fonts.
    require(
        QFontDatabase::addApplicationFont(
            QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/segoeui.ttf"))) >=
            0,
        "load a font for inline text editing");
#endif
#ifdef Q_OS_MACOS
    canvasCollectionBehavior();
    if (app.arguments().contains(QStringLiteral("--platform-only")))
        return 0;
#endif
    QTemporaryDir directory;
    auto& storage = storage::ApplicationStorage::instance();
    require(storage
                .initialize({directory.filePath(QStringLiteral("bin")),
                             directory.filePath(QStringLiteral("data")), 60000})
                .success,
            "initialize storage");
#ifdef Q_OS_MACOS
    if (app.arguments().contains(QStringLiteral("--native-only"))) {
        nativeCanvasLifecycle();
        storage.shutdown();
        return 0;
    }
#endif
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--native-only"))) {
        nativeInput();
        storage.shutdown();
        return 0;
    }
#endif
    if (app.arguments().contains(QStringLiteral("--text-escape-only"))) {
        textEscapePreservesAnnotations(app);
        storage.shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--saved-layout-only"))) {
        savedToolbarLayout(app);
        storage.shutdown();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--template-navigation-only"))) {
        templateInsertionAfterNavigation(app);
        storage.shutdown();
        return 0;
    }
    textEscapePreservesAnnotations(app);
    savedToolbarLayout(app);
    templateInsertionAfterNavigation(app);
    toolbarPlacement(app);
    canvasNavigation(app);
    if (app.arguments().contains(QStringLiteral("--toolbar-placement-only"))) {
        storage.shutdown();
        return 0;
    }
    bool nativeTransparent = false;
    bool failTransition = false;
    int pointerReads = 0;
    presentation::GlobalCanvasController controller(nullptr, {[&]() {
                                                                  ++pointerReads;
                                                                  return app.primaryScreen();
                                                              },
                                                              [&](QWidget*, bool enabled) {
                                                                  if (failTransition)
                                                                      return false;
                                                                  nativeTransparent = enabled;
                                                                  return true;
                                                              }});
    int errors = 0;
    QObject::connect(&controller, &presentation::GlobalCanvasController::errorOccurred, &app,
                     [&](const QString&) { ++errors; });
    controller.activate();
    app.processEvents();
    require(controller.active() && !controller.clickThrough() && pointerReads == 1,
            "create editing canvas once");
    auto* canvas = controller.canvas();
    auto* window = controller.window();
    auto* toolbar = controller.toolbar();
    auto* palette = toolbar->palette();
    require(window->geometry() == app.primaryScreen()->geometry(), "canvas covers pointer display");
    require(canvas->canvasTool() == SnowCanvasTool::Select, "initial selection tool");
    auto* toggle = palette->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("globalCanvasClickThroughButton"));
    auto* exit =
        palette->findChild<adqt::widgets::AdButton*>(QStringLiteral("globalCanvasExitButton"));
    require(toggle && exit && toggle->isVisible() && exit->isVisible(), "canvas actions visible");
    require(toggle->mapToGlobal(QPoint()).x() < exit->mapToGlobal(QPoint()).x(),
            "exit follows click-through");
    require(toggle->toolTip() == QStringLiteral("Click-through"), "unset shortcut has no hint");
    const QString escapeHint = presentation::formatShortcutListDisplayText({QStringLiteral("Esc")});
    require(exit->toolTip() == QStringLiteral("Exit (%1)").arg(escapeHint),
            "exit tooltip displays the fixed Escape shortcut");
    require(storage::ShortcutSettings().setGlobalCanvas({QStringLiteral("Ctrl+F8")}),
            "set shortcut");
    require(toggle->toolTip().contains(presentation::formatShortcutListDisplayText(
                storage::ShortcutSettings().globalCanvas())),
            "shortcut hint updates immediately");
    CanvasTranslator translator;
    app.installTranslator(&translator);
    app.processEvents();
    require(toggle->toolTip().startsWith(QStringLiteral("Translated Click-through")) &&
                exit->accessibleName() == QStringLiteral("Translated Exit") &&
                exit->toolTip() == QStringLiteral("Translated Exit (%1)").arg(escapeHint),
            "live toolbar retranslation");
    app.removeTranslator(&translator);
    app.processEvents();
    const QImage emptyCanvas = canvas->grab().toImage();
    require(emptyCanvas.pixelColor(10, 10).alpha() <= 2,
            "canvas background is transparent, not a screenshot");
    const QPoint moved = toolbar->constrainedContentPosition(QPoint(70, 90));
    toolbar->moveContentTo(moved);
    QWidget* handle = palette->dragHandle();
    require(handle != nullptr, "drawing toolbar provides a drag handle");
    // The native test exercises dragging: Windows placement reads the physical
    // cursor rather than synthetic mouse-event positions.
    palette->freeDrawRequested();
    const double widthBefore = canvas->canvasStyleToolbarState().shapeStyle.strokeWidth;
    require(palette->stepStrokeWidth(1) &&
                canvas->canvasStyleToolbarState().shapeStyle.strokeWidth > widthBefore,
            "toolbar style edits reach the canvas");
    mouse(canvas, QEvent::MouseButtonPress, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, QPointF(180, 140), Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(200, 160), Qt::LeftButton, Qt::NoButton);
    require(canvas->canvasHistoryState().canUndo, "drawing commits a stroke");
    const QPoint beforeToggle = toolbar->contentPosition();
    toggle->click();
    require(controller.clickThrough() && nativeTransparent && !canvas->interactionEnabled(),
            "button enables native click-through");
    require(toolbar->isVisible() && !toolbar->testAttribute(Qt::WA_TransparentForMouseEvents),
            "toolbar remains interactive");
    require(canvas->canvasHistoryState().canUndo, "toggle preserves annotations");
    require(canvas->grab().toImage().pixelColor(10, 10).alpha() == 0,
            "click-through removes the minimal input surface");
    controller.activate();
    require(!controller.clickThrough() && !nativeTransparent && canvas->interactionEnabled(),
            "hotkey restores drawing");
    require(pointerReads == 1 && controller.window() == window &&
                toolbar->contentPosition() == beforeToggle,
            "hotkey retains singleton, display and toolbar placement");
    failTransition = true;
    toggle->click();
    require(!controller.clickThrough() && !toggle->isChecked() && canvas->interactionEnabled() &&
                errors == 1,
            "native failure preserves state and reports error");
    failTransition = false;
    canvas->setCanvasTool(SnowCanvasTool::Select);
    require(palette->activeTool() == ScreenshotToolPalette::Tool::Select,
            "canvas keyboard tool changes synchronize toolbar selection");
    palette->undoRequested();
    require(canvas->canvasHistoryState().canRedo, "undo available");
    palette->redoRequested();
    require(canvas->canvasHistoryState().canUndo, "redo available");
    palette->rectangleFilterRequested();
    require(canvas->canvasTool() == SnowCanvasTool::RectangleFilter, "filter tool reaches canvas");
    mouse(canvas, QEvent::MouseButtonPress, QPointF(500, 300), Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, QPointF(600, 400), Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(600, 400), Qt::LeftButton, Qt::NoButton);
    const QImage filtered = canvas->grab().toImage();
    const QPoint sample = (QPointF(550, 350) * canvas->devicePixelRatioF()).toPoint();
    require(filtered.pixelColor(sample).alpha() <= 2,
            "filtering an empty area preserves transparency and does not capture desktop pixels");
    palette->setToolbarLayout(storage::ScreenshotToolbarSettings().layout(
        storage::ScreenshotToolbarLayoutKind::DrawingTools));
    require(toggle->isVisible() && exit->isVisible(), "layout rebuild retains canvas actions");
    exit->click();
    app.processEvents();
    require(!controller.active(), "exit destroys session");
    controller.activate();
    require(pointerReads == 2 && !controller.clickThrough() &&
                !controller.canvas()->canvasHistoryState().canUndo,
            "reopen creates empty editing session on current display");
    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(controller.canvas(), &escape);
    app.processEvents();
    require(!controller.active(), "Escape on the canvas exits the session");
    controller.activate();
    controller.activate();
    require(controller.clickThrough(), "enable click-through before toolbar Escape");
    PhysicalKeyEvent toolbarEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(controller.toolbar()->palette(), &toolbarEscape);
    app.processEvents();
    require(!controller.active(), "Escape on the toolbar exits a click-through session");
#ifdef Q_OS_MACOS
    for (bool clickThrough : {false, true}) {
        controller.activate();
        if (clickThrough)
            controller.activate();
        QWidget* receiver = clickThrough ? static_cast<QWidget*>(controller.toolbar()->palette())
                                         : static_cast<QWidget*>(controller.canvas());
        PhysicalKeyEvent plainW(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
        QApplication::sendEvent(receiver, &plainW);
        PhysicalKeyEvent plainWRelease(QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier);
        QApplication::sendEvent(receiver, &plainWRelease);
        require(controller.active(), "unmodified W does not close the canvas");
        // Qt maps the macOS Command key to ControlModifier.
        PhysicalKeyEvent closePress(QEvent::KeyPress, Qt::Key_W, Qt::ControlModifier);
        QApplication::sendEvent(receiver, &closePress);
        require(controller.active(), "Command+W waits for release before closing");
        PhysicalKeyEvent closeRelease(QEvent::KeyRelease, Qt::Key_W, Qt::ControlModifier);
        QApplication::sendEvent(receiver, &closeRelease);
        app.processEvents();
        require(!controller.active(),
                "Command+W exits from the canvas or the click-through toolbar");
    }
#endif
    presentation::GlobalCanvasController noDisplay(nullptr,
                                                   {[]() -> QScreen* { return nullptr; }, {}});
    noDisplay.activate();
    require(!noDisplay.active(), "missing display does not create a broken session");
    displayLifecycle();
    ScreenshotToolPalette ordinary(ScreenshotToolPalette::Options{});
    require(!ordinary.findChild<adqt::widgets::AdButton*>(QStringLiteral("globalCanvasExitButton")),
            "existing palettes unchanged");
    storage.shutdown();
    return 0;
}
