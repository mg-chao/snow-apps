#include "snow_shot/presentation/fullscreencanvascontroller.h"
#include "snow_shot/presentation/fullscreencanvasstylepanel.h"
#include "snow_shot/presentation/fullscreencanvaswindow.h"
#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../src/presentation/pinned/pinnedwindowplatform.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/button.h"
#include "widgets/radio_button_group.h"

#include <QApplication>
#include <QBoxLayout>
#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QWindow>

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
using snow_shot::presentation::FullscreenCanvasController;
using snow_shot::presentation::FullscreenCanvasWindow;
using snow_shot::presentation::PinnedPlacement;
using snow_shot::presentation::PinnedWindowPlatform;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void settle(int milliseconds = 20) {
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    } while (elapsed.elapsed() < milliseconds);
}

#ifdef Q_OS_WIN
bool waitUntil(const std::function<bool()>& condition, int timeoutMs = 1000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeoutMs) {
        settle(5);
    }
    return condition();
}
#endif

struct PlatformState {
    bool attachSucceeds = true;
    bool transparent = false;
    std::deque<bool> transitionResults;
};

class TestPlatform final : public PinnedWindowPlatform {
  public:
    TestPlatform(QWidget* window, std::shared_ptr<PlatformState> state)
        : PinnedWindowPlatform(window, Role::Image), m_state(std::move(state)) {}

    bool attach() override {
        return m_state->attachSucceeds;
    }
    void detach() override {}
    bool applyPlacement(const PinnedPlacement&, QScreen*, GeometryUpdate) override {
        return true;
    }
    std::optional<PinnedPlacement> placement() const override {
        return {};
    }
    bool setInputTransparent(bool transparent) override {
        if (!m_state->transitionResults.empty()) {
            const bool succeeds = m_state->transitionResults.front();
            m_state->transitionResults.pop_front();
            if (!succeeds) {
                return false;
            }
        }
        m_state->transparent = transparent;
        return true;
    }
    bool activate() override {
        return true;
    }

  private:
    std::shared_ptr<PlatformState> m_state;
};

FullscreenCanvasWindow::PlatformFactory testFactory(const std::shared_ptr<PlatformState>& state) {
    return [state](QWidget* window) { return std::make_unique<TestPlatform>(window, state); };
}

QScreen* primaryScreen() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "full-screen canvas tests require a screen");
    return screen;
}

QImage canvasImage(FullscreenCanvasWindow& window) {
    auto* canvas = window.canvas();
    QImage image(canvas->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    canvas->render(&painter);
    return image;
}

qsizetype inkPixels(const QImage& image) {
    qsizetype result = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 16) {
                ++result;
            }
        }
    }
    return result;
}

QRect inkBounds(const QImage& image) {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 16) {
                bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    return bounds;
}

void pressKey(QWidget& receiver, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
              const QString& text = {}) {
    const auto send = [&](QEvent::Type type) {
#ifdef Q_OS_MACOS
        const auto identity = snow_shot::shortcuts::effectiveIdentity(
            {QKeySequence(key).toString(QKeySequence::PortableText)});
        const quint32 nativeKey = identity.physicalKey.value_or(0);
        QKeyEvent event(type, key, modifiers, nativeKey, nativeKey, 0, text);
#else
        QKeyEvent event(type, key, modifiers, text);
#endif
        QApplication::sendEvent(&receiver, &event);
    };
    send(QEvent::KeyPress);
    send(QEvent::KeyRelease);
}

void pressHistoryShortcut(QWidget& receiver, QKeySequence::StandardKey key) {
    const auto bindings = QKeySequence::keyBindings(key);
    require(!bindings.isEmpty() && bindings.front().count() == 1,
            "the current platform must expose a standard history shortcut");
    const auto combination = bindings.front()[0];
    pressKey(receiver, combination.key(), combination.keyboardModifiers());
}

void drawStroke(SnowCanvasWidget& canvas) {
    const auto mouse = [&canvas](QEvent::Type type, const QPointF& point, Qt::MouseButton button,
                                 Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, point, QPointF(canvas.mapToGlobal(point.toPoint())), button,
                          buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    const QPointF start(canvas.width() * 0.60, canvas.height() * 0.55);
    const QPointF end = start + QPointF(95.0, 45.0);
    mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}

void saveVisualQa(FullscreenCanvasWindow& window) {
    if (!qEnvironmentVariableIsSet("SNOW_FULLSCREEN_CANVAS_QA")) {
        return;
    }
    QDir output(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath());
    require(output.cdUp() && output.cdUp() &&
                output.mkpath(QStringLiteral("build/fullscreen-canvas-qa")) &&
                output.cd(QStringLiteral("build/fullscreen-canvas-qa")),
            "full-screen visual QA output directory must be available");
    window.activateTool(SnowCanvasTool::Shape);
    settle(40);
    require(window.grab().save(output.filePath(QStringLiteral("shape.png"))),
            "full-screen shape toolbar screenshot must save");
    window.activateTool(SnowCanvasTool::Text);
    settle(40);
    require(window.grab().save(output.filePath(QStringLiteral("text.png"))),
            "full-screen text toolbar screenshot must save");
    window.activateLaser();
    settle(40);
    require(window.grab().save(output.filePath(QStringLiteral("laser.png"))),
            "full-screen laser toolbar screenshot must save");
    window.activateTool(SnowCanvasTool::Select);
}

void layoutToolsAndUndoableClear() {
    FullscreenCanvasWindow window(primaryScreen(), testFactory(std::make_shared<PlatformState>()));
    require(window.present(), "full-screen canvas must initialize");
    settle();
    auto* canvas = window.canvas();
    require(canvas != nullptr && window.toolbar() != nullptr && window.stylePanel() != nullptr,
            "the canvas must expose its drawing surface and both toolbars");
    require(canvas->canvasTool() == SnowCanvasTool::Select && !window.laserActive() &&
                !canvas->canvasHistoryState().canUndo && !canvas->canvasHistoryState().canRedo &&
                inkPixels(canvasImage(window)) == 0,
            "a fresh canvas must contain no annotations or history");
    require(window.inputSurfaceColor().alpha() > 0 && window.inputSurfaceColor().alpha() <= 2,
            "editing must retain a nearly transparent surface for native hit testing");
    require(window.toolbar()->geometry().top() < window.stylePanel()->geometry().top() &&
                window.stylePanel()->geometry().left() < window.width() / 2 &&
                window.rect().contains(window.toolbar()->geometry()) &&
                window.rect().contains(window.stylePanel()->geometry()),
            "drawing tools belong at the top and style tools at the left inside the screen");
    auto* passthrough = window.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("fullscreenCanvasAction-click-through"));
    QBoxLayout* toolbarLayout = window.toolbar()->contentLayout();
    require(passthrough &&
                toolbarLayout->itemAt(toolbarLayout->count() - 1)->widget() == passthrough,
            "click-through must be the rightmost main-toolbar action");
    saveVisualQa(window);

    constexpr std::array<SnowCanvasTool, 12> tools{
        SnowCanvasTool::Select,       SnowCanvasTool::Shape,     SnowCanvasTool::Arrow,
        SnowCanvasTool::Line,         SnowCanvasTool::FreeDraw,  SnowCanvasTool::RectangleHighlight,
        SnowCanvasTool::PenHighlight, SnowCanvasTool::Text,      SnowCanvasTool::SerialNumber,
        SnowCanvasTool::Eraser,       SnowCanvasTool::Watermark, SnowCanvasTool::Spotlight};
    for (const auto tool : tools) {
        window.activateTool(tool);
        require(canvas->canvasTool() == tool && canvas->interactionEnabled() &&
                    !window.laserActive(),
                "each advertised annotation tool must activate the existing drawing engine");
    }
    window.activateLaser();
    require(window.laserActive() && !canvas->interactionEnabled(),
            "laser must own input without creating engine drawing gestures");
    window.activateTool(SnowCanvasTool::FreeDraw);
    drawStroke(*canvas);
    require(canvas->canvasHistoryState().canUndo && inkPixels(canvasImage(window)) > 0,
            "drawing must create a visible annotation and history entry");
    window.activateTool(SnowCanvasTool::Select);
    window.clearCanvas();
    require(inkPixels(canvasImage(window)) == 0 && canvas->canvasHistoryState().canUndo,
            "Clear must remove annotations while retaining an undo entry");
    require(canvas->undo() && inkPixels(canvasImage(window)) > 0,
            "undoing Clear must restore the annotations");
    require(canvas->redo() && inkPixels(canvasImage(window)) == 0,
            "redoing Clear must remove the annotations again");

    window.resize(420, 440);
    settle();
    auto* strip = window.findChild<QScrollArea*>(QStringLiteral("fullscreenCanvasToolStrip"));
    require(strip && strip->horizontalScrollBar()->maximum() > 0 &&
                window.rect().contains(window.toolbar()->geometry()) &&
                window.rect().contains(window.stylePanel()->geometry()) && passthrough->isVisible(),
            "narrow screens must scroll the tool strip while retaining visible styles and recovery "
            "action");
    const QRect actionRect(passthrough->mapTo(&window, QPoint()), passthrough->size());
    require(window.rect().contains(actionRect), "click-through must remain inside a narrow window");
    window.close();
}

void clickThroughPreservesToolsAndRecovery() {
    const auto state = std::make_shared<PlatformState>();
    FullscreenCanvasWindow window(primaryScreen(), testFactory(state));
    require(window.present(), "click-through fixture must initialize");
    window.activateTool(SnowCanvasTool::FreeDraw);
    drawStroke(*window.canvas());
    window.activateTool(SnowCanvasTool::Arrow);
    require(window.setClickThrough(true), "click-through entry must succeed");
    require(window.clickThrough() && state->transparent && window.toolbar()->isHidden() &&
                window.stylePanel()->isHidden() && !window.canvas()->interactionEnabled() &&
                window.inputSurfaceColor().alpha() == 0,
            "click-through must hide controls, suppress drawing, and clear the input surface");
    require(inkPixels(canvasImage(window)) > 0 && window.canvas()->canvasHistoryState().canUndo,
            "click-through must retain visible annotations and their document history");
    QWidget* recovery = window.recoveryButton();
    require(recovery && recovery->isWindow() && recovery->window() != &window &&
                recovery->isVisible(),
            "click-through requires a visible independent recovery window");
    const QRect usable =
        snow_shot::presentation::pinnedDisplayGeometry(*primaryScreen()).usableBounds.toRect();
    require(recovery->geometry().top() == usable.top() &&
                qAbs(recovery->geometry().center().x() - usable.center().x()) <= 1,
            "recovery must appear centered at the top of the canvas screen");
    window.activateTool(SnowCanvasTool::Text);
    require(window.canvas()->canvasTool() == SnowCanvasTool::Arrow,
            "tool changes must not mutate a click-through canvas");
    auto* recoveryButton = qobject_cast<adqt::widgets::AdButton*>(recovery);
    require(recoveryButton != nullptr, "the recovery surface must be an actionable button");
    recoveryButton->click();
    require(!window.clickThrough() && !state->transparent && recovery->isHidden() &&
                window.toolbar()->isVisible() && window.stylePanel()->isVisible() &&
                window.canvas()->interactionEnabled() &&
                window.canvas()->canvasTool() == SnowCanvasTool::Arrow,
            "recovery must restore the prior tool and editing controls");
    require(inkPixels(canvasImage(window)) > 0,
            "restoring editing must preserve the existing annotations");
    window.activateLaser();
    require(window.setClickThrough(true) && window.setClickThrough(false) && window.laserActive() &&
                !window.canvas()->interactionEnabled(),
            "recovery must preserve transient laser selection without enabling engine drawing");
    require(window.setClickThrough(true), "the canvas must support repeated transitions");
    window.close();
    require(!recovery->isVisible(), "closing a canvas must hide its recovery surface");
}

void transitionFailuresKeepAnEscapeRoute() {
    const auto state = std::make_shared<PlatformState>();
    FullscreenCanvasWindow window(primaryScreen(), testFactory(state));
    int failures = 0;
    QObject::connect(&window, &FullscreenCanvasWindow::operationFailed, &window,
                     [&failures](const QString&, bool) { ++failures; });
    require(window.present(), "failure fixture must initialize");
    state->transitionResults = {false, true};
    require(!window.setClickThrough(true) && !window.clickThrough() &&
                window.toolbar()->isVisible() && !window.recoveryButton()->isVisible() &&
                failures == 1,
            "failed entry with successful rollback must retain normal editing");
    state->transitionResults = {false, false};
    require(!window.setClickThrough(true) && window.clickThrough() &&
                window.recoveryButton()->isVisible() && failures == 2,
            "failed entry and rollback must leave the independent recovery control available");
    state->transitionResults = {false};
    require(!window.setClickThrough(false) && window.clickThrough() &&
                window.recoveryButton()->isVisible() && failures == 3,
            "failed restoration must retain recovery for another attempt");
    require(window.setClickThrough(false) && !window.clickThrough() &&
                window.toolbar()->isVisible(),
            "a later successful restoration must recover the same canvas");
    window.close();

    const auto attachFailure = std::make_shared<PlatformState>();
    attachFailure->attachSucceeds = false;
    FullscreenCanvasWindow failedWindow(primaryScreen(), testFactory(attachFailure));
    require(!failedWindow.present() && !failedWindow.isVisible(),
            "native initialization failure must not leave a visible unusable canvas");
}

void controllerOwnsOneFreshSession() {
    FullscreenCanvasController controller;
    controller.activate();
    QPointer<FullscreenCanvasWindow> first = controller.window();
    require(first && first->isVisible(), "activation must create a canvas on the current screen");
    controller.activate();
    require(controller.window() == first && first->clickThrough(),
            "repeat activation must toggle the existing singleton session");
    controller.activate();
    require(controller.window() == first && !first->clickThrough(),
            "repeat activation must restore the same session");
    first->activateTool(SnowCanvasTool::FreeDraw);
    drawStroke(*first->canvas());
    require(first->canvas()->canvasHistoryState().canUndo,
            "controller fixture must contain drawing");
    first->close();
    require(controller.window() == nullptr, "closing must release the controller's active session");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(first == nullptr, "closing must destroy the old canvas");
    controller.activate();
    QPointer<FullscreenCanvasWindow> second = controller.window();
    require(second && !second->canvas()->canvasHistoryState().canUndo &&
                inkPixels(canvasImage(*second)) == 0,
            "reopening must start a new empty canvas");
    require(second->setClickThrough(true), "shutdown fixture must enter click-through");
    QPointer<QWidget> recovery = second->recoveryButton();
    controller.shutdown();
    require(controller.window() == nullptr && second == nullptr && recovery == nullptr,
            "shutdown must destroy the canvas and its independent recovery window");
}

void displayNotificationsPreserveDrawingAndRecovery() {
    TestPlatform* platform = nullptr;
    const auto state = std::make_shared<PlatformState>();
    QScreen* screen = primaryScreen();
    FullscreenCanvasWindow window(screen, [&](QWidget* widget) {
        auto result = std::make_unique<TestPlatform>(widget, state);
        platform = result.get();
        return result;
    });
    require(window.present() && platform && platform->environmentChanged,
            "display notification fixture must initialize its platform callback");
    window.activateTool(SnowCanvasTool::FreeDraw);
    drawStroke(*window.canvas());
    window.activateTool(SnowCanvasTool::Arrow);
    static_cast<void>(window.canvas()->resetEditingStatePreservingTool());
    const QRect originalInk = inkBounds(canvasImage(window));
    const QPointF originalPoint = window.canvas()->canvasToViewTransform().map(QPointF(320, 240));
    require(!originalInk.isEmpty(),
            "display notification fixture must have persistent annotations");
    const QRect screenRect = screen->geometry();
    const QRect displaced = screenRect.adjusted(25, 30, -25, -30);
    window.setGeometry(displaced);
    platform->environmentChanged(true);
    platform->environmentChanged(false);
    settle();
    require(window.geometry() == screenRect &&
                window.canvas()->canvasToViewTransform().map(QPointF(320, 240)) == originalPoint &&
                inkBounds(canvasImage(window)) == originalInk &&
                window.canvas()->canvasTool() == SnowCanvasTool::Arrow,
            "display refresh must restore screen geometry without rebasing annotations or tools");

    window.setGeometry(displaced);
    require(QMetaObject::invokeMethod(window.windowHandle(), "safeAreaMarginsChanged",
                                      Qt::DirectConnection, Q_ARG(QMargins, QMargins())),
            "the native safe-area notification must be delivered");
    settle();
    require(window.geometry() == screenRect && inkBounds(canvasImage(window)) == originalInk,
            "safe-area changes must refresh anchored layout without rebasing annotations");

    require(window.setClickThrough(true), "display removal fixture must enter click-through");
    QPointer<QWidget> recovery = window.recoveryButton();
    window.setGeometry(displaced);
    // Deliver only the public notification. The platform screen stays registered,
    // so the handler's primary-screen fallback is exercised without changing the desktop.
    require(QMetaObject::invokeMethod(qGuiApp, "screenRemoved", Qt::DirectConnection,
                                      Q_ARG(QScreen*, screen)),
            "the screen-removal notification must be delivered");
    settle();
    const QRect usable =
        snow_shot::presentation::pinnedDisplayGeometry(*primaryScreen()).usableBounds.toRect();
    require(
        window.screen() == primaryScreen() && window.geometry() == primaryScreen()->geometry() &&
            window.clickThrough() && recovery && recovery == window.recoveryButton() &&
            recovery->isVisible() && recovery->geometry().top() == usable.top() &&
            qAbs(recovery->geometry().center().x() - usable.center().x()) <= 1 &&
            inkBounds(canvasImage(window)) == originalInk,
        "screen removal must preserve drawing and click-through recovery on the primary screen");
    platform->environmentChanged(true);
    window.close();
    settle();
    require(!window.isVisible() && recovery && !recovery->isVisible(),
            "a queued display refresh must not reopen a canvas or recovery after close");
}

void transientAndTextInputRespectKeyboardOwnership() {
    const snow_shot::storage::DrawingShortcutSettings settings;
    const auto savedBrush = settings.shortcuts(QStringLiteral("brush"));
    struct RestoreBrushShortcut {
        snow_shot::shortcuts::ShortcutBindingList bindings;
        ~RestoreBrushShortcut() {
            static_cast<void>(snow_shot::storage::DrawingShortcutSettings().setShortcuts(
                QStringLiteral("brush"), bindings));
        }
    } restoreBrush{savedBrush};
    require(settings.setShortcuts(QStringLiteral("brush"), {QStringLiteral("B")}),
            "text-focus fixture must configure an overlapping letter shortcut");
    FullscreenCanvasWindow window(primaryScreen(), testFactory(std::make_shared<PlatformState>()));
    require(window.present(), "keyboard ownership fixture must initialize");
    window.activateWindow();
    auto* canvas = window.canvas();
    canvas->setFocus(Qt::OtherFocusReason);
    settle();
    window.activateLaser();
    drawStroke(*canvas);
    require(inkPixels(canvasImage(window)) > 0, "the laser fixture must contain a visible trail");
    pressKey(*canvas, Qt::Key_Escape);
    require(window.isVisible() && window.laserActive() && inkPixels(canvasImage(window)) == 0 &&
                !canvas->canvasHistoryState().canUndo,
            "Escape must clear a laser trail without closing the canvas or creating history");
    window.activateTool(SnowCanvasTool::FreeDraw);
    drawStroke(*canvas);
    window.activateLaser();
    pressHistoryShortcut(*canvas, QKeySequence::Undo);
    require(window.laserActive() && !canvas->interactionEnabled() &&
                canvas->canvasHistoryState().canRedo && inkPixels(canvasImage(window)) == 0,
            "Undo must affect persistent annotations while laser mode retains input ownership");
    pressHistoryShortcut(*canvas, QKeySequence::Redo);
    require(window.laserActive() && inkPixels(canvasImage(window)) > 0,
            "Redo must restore annotations without leaving laser mode");

    require(canvas->clearDocument(), "text-focus fixture must reset annotation history");
    window.activateTool(SnowCanvasTool::Text);
    const QPointF textPoint(canvas->width() * 0.60, canvas->height() * 0.55);
    QMouseEvent down(QEvent::MouseButtonPress, textPoint, textPoint, textPoint, Qt::LeftButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, textPoint, textPoint, textPoint, Qt::LeftButton,
                   Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &down);
    QApplication::sendEvent(canvas, &up);
    require(canvas->hasActiveTextEditing(), "text drawing must focus an inline draft");
    pressKey(*canvas, Qt::Key_B, Qt::NoModifier, QStringLiteral("b"));
    pressKey(*canvas, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    QInputMethodQueryEvent query(Qt::ImSurroundingText);
    QApplication::sendEvent(canvas, &query);
    require(canvas->hasActiveTextEditing() && canvas->canvasTool() == SnowCanvasTool::Text &&
                query.value(Qt::ImSurroundingText).toString() == QStringLiteral("b "),
            "inline typing must receive shortcut letters and spaces without switching tools");
    pressHistoryShortcut(*canvas, QKeySequence::Undo);
    require(canvas->hasActiveTextEditing() && !canvas->canvasHistoryState().canUndo,
            "text Undo must remain inside the draft instead of invoking document history");
    pressKey(*canvas, Qt::Key_Escape);
    require(window.isVisible() && !canvas->hasActiveTextEditing() &&
                !canvas->canvasHistoryState().canUndo,
            "Escape in text editing must cancel the draft without closing the canvas");
    window.close();
}

void highlightVariantsRemainUsableAcrossPanelRebuilds() {
    FullscreenCanvasWindow window(primaryScreen(), testFactory(std::make_shared<PlatformState>()));
    require(window.present(), "highlight variant fixture must initialize");
    const int rectangleId = static_cast<int>(ScreenshotToolPalette::Tool::RectangleHighlight);
    const int penId = static_cast<int>(ScreenshotToolPalette::Tool::PenHighlight);
    const auto selector = [&window]() {
        auto* container = window.stylePanel()->findChild<QWidget*>(
            QStringLiteral("screenshotHighlightModeSelector"));
        require(container != nullptr, "highlight tools must expose their variant selector");
        auto* group = container->findChild<adqt::widgets::AdRadioButtonGroup*>();
        require(group != nullptr, "highlight selector must contain a radio group");
        return group;
    };
    window.activateTool(SnowCanvasTool::RectangleHighlight);
    auto* rectangleGroup = selector();
    require(rectangleGroup->checkedId() == rectangleId,
            "rectangle highlight must initially select its rectangle radio");
    auto* penButton = rectangleGroup->button(penId);
    require(penButton != nullptr, "rectangle highlight must offer the pen variant");
    penButton->click();
    settle();
    auto* penGroup = selector();
    require(window.canvas()->canvasTool() == SnowCanvasTool::PenHighlight &&
                penGroup->checkedId() == penId,
            "clicking pen must switch the tool and select pen in the rebuilt sidebar");
    auto* rectangleButton = penGroup->button(rectangleId);
    require(rectangleButton != nullptr, "pen highlight must offer the rectangle variant");
    rectangleButton->click();
    settle();
    require(window.canvas()->canvasTool() == SnowCanvasTool::RectangleHighlight &&
                selector()->checkedId() == rectangleId,
            "clicking rectangle must remain usable after both sidebar rebuilds");
    window.close();
}

#ifdef Q_OS_WIN
struct NativeCursorRestorer {
    POINT original{};
    NativeCursorRestorer() {
        require(GetCursorPos(&original) != FALSE,
                "native canvas test must capture cursor position");
    }
    ~NativeCursorRestorer() {
        INPUT release{};
        release.type = INPUT_MOUSE;
        release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &release, sizeof(INPUT));
        SetCursorPos(original.x, original.y);
    }
};

QRect nativeClientRect(QWidget& widget) {
    const HWND handle = reinterpret_cast<HWND>(widget.winId());
    RECT rect{};
    POINT origin{};
    require(GetClientRect(handle, &rect) != FALSE && ClientToScreen(handle, &origin) != FALSE,
            "native canvas test must resolve the client rectangle");
    return QRect(origin.x, origin.y, rect.right - rect.left, rect.bottom - rect.top);
}

void nativeMouse(DWORD flags) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    require(SendInput(1, &input, sizeof(INPUT)) == 1, "native canvas mouse injection failed");
}

void nativeClick(const QPoint& position) {
    require(SetCursorPos(position.x(), position.y()) != FALSE, "native canvas cursor move failed");
    nativeMouse(MOUSEEVENTF_LEFTDOWN);
    nativeMouse(MOUSEEVENTF_LEFTUP);
}

void nativeStroke(const QPoint& start, const QPoint& end) {
    require(SetCursorPos(start.x(), start.y()) != FALSE, "native canvas drag start failed");
    nativeMouse(MOUSEEVENTF_LEFTDOWN);
    settle(30);
    require(SetCursorPos(end.x(), end.y()) != FALSE, "native canvas drag move failed");
    settle(30);
    nativeMouse(MOUSEEVENTF_LEFTUP);
    settle(50);
}

QString nativeWindowDescription(HWND handle) {
    if (handle == nullptr) {
        return QStringLiteral("null HWND");
    }
    wchar_t title[256]{};
    wchar_t className[256]{};
    DWORD processId = 0;
    GetWindowThreadProcessId(handle, &processId);
    GetWindowTextW(handle, title, 256);
    GetClassNameW(handle, className, 256);
    RECT rect{};
    GetWindowRect(handle, &rect);
    return QStringLiteral("HWND=%1 pid=%2 class=%3 title=%4 rect=%5,%6,%7,%8 visible=%9 "
                          "enabled=%10 style=%11 exstyle=%12 owner=%13 previous=%14")
        .arg(reinterpret_cast<quintptr>(handle), 0, 16)
        .arg(processId)
        .arg(QString::fromWCharArray(className), QString::fromWCharArray(title))
        .arg(rect.left)
        .arg(rect.top)
        .arg(rect.right)
        .arg(rect.bottom)
        .arg(IsWindowVisible(handle))
        .arg(IsWindowEnabled(handle))
        .arg(static_cast<qulonglong>(GetWindowLongPtrW(handle, GWL_STYLE)), 0, 16)
        .arg(static_cast<qulonglong>(GetWindowLongPtrW(handle, GWL_EXSTYLE)), 0, 16)
        .arg(reinterpret_cast<quintptr>(GetWindow(handle, GW_OWNER)), 0, 16)
        .arg(reinterpret_cast<quintptr>(GetWindow(handle, GW_HWNDPREV)), 0, 16);
}

QString recoveryHitDescription(QWidget& recovery) {
    const HWND handle = reinterpret_cast<HWND>(recovery.winId());
    const QPoint center = nativeClientRect(recovery).center();
    const POINT point{center.x(), center.y()};
    const HWND hit = WindowFromPoint(point);
    const LRESULT directHit =
        SendMessageW(handle, WM_NCHITTEST, 0,
                     MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y)));
    return QStringLiteral("recovery: %1\ncenter=%2,%3 directHit=%4 foreground=%5\nactual hit: %6")
        .arg(nativeWindowDescription(handle))
        .arg(center.x())
        .arg(center.y())
        .arg(static_cast<qlonglong>(directHit))
        .arg(reinterpret_cast<quintptr>(GetForegroundWindow()), 0, 16)
        .arg(nativeWindowDescription(hit));
}
#endif
} // namespace

void runFullscreenCanvasTests() {
    layoutToolsAndUndoableClear();
    clickThroughPreservesToolsAndRecovery();
    transitionFailuresKeepAnEscapeRoute();
    controllerOwnsOneFreshSession();
    displayNotificationsPreserveDrawingAndRecovery();
    transientAndTextInputRespectKeyboardOwnership();
    highlightVariantsRemainUsableAcrossPanelRebuilds();
}

#ifdef Q_OS_WIN
void runFullscreenCanvasNativeTests() {
    require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "native full-screen canvas tests require Windows QPA");
    NativeCursorRestorer cursorRestorer;
    class MouseProbe final : public QWidget {
      public:
        int presses = 0;

      protected:
        void mousePressEvent(QMouseEvent* event) override {
            ++presses;
            event->accept();
        }
    } lower;
    QScreen* screen = primaryScreen();
    // Model an ordinary underlying application. A competing full-screen topmost
    // window correctly rises above other topmost windows when activated and would
    // cover recovery; that tests Windows z-order policy rather than click-through.
    lower.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    lower.setScreen(screen);
    lower.setGeometry(screen->geometry());
    lower.show();
    lower.raise();
    lower.activateWindow();
    settle(60);
    const HWND lowerHandle = reinterpret_cast<HWND>(lower.winId());
    const QRect lowerBounds = nativeClientRect(lower);
    const POINT lowerPoint{lowerBounds.left() + lowerBounds.width() * 2 / 3,
                           lowerBounds.top() + lowerBounds.height() * 3 / 5};
    const HWND initialLowerHit = WindowFromPoint(lowerPoint);
    if (GetAncestor(initialLowerHit, GA_ROOT) != lowerHandle) {
        qWarning().noquote() << "Underlying probe before presenting canvas:"
                             << nativeWindowDescription(lowerHandle);
        qWarning().noquote() << "Actual probe-point hit:"
                             << nativeWindowDescription(initialLowerHit);
    }
    require(GetAncestor(initialLowerHit, GA_ROOT) == lowerHandle,
            "the underlying application fixture must receive input before presenting canvas");
    FullscreenCanvasWindow window(screen);
    require(window.present(), "native canvas must present");
    settle(80);
    const HWND canvasHandle = reinterpret_cast<HWND>(window.winId());
    const QRect bounds = nativeClientRect(window);
    const QPoint start(bounds.left() + bounds.width() * 2 / 3,
                       bounds.top() + bounds.height() * 3 / 5);
    const QPoint end = start + QPoint(80, 40);
    const POINT hitPoint{start.x(), start.y()};
    require(GetAncestor(WindowFromPoint(hitPoint), GA_ROOT) == canvasHandle,
            "the transparent blank editing surface must receive native mouse input");
    window.activateTool(SnowCanvasTool::FreeDraw);
    nativeStroke(start, end);
    if (lower.presses != 0 || !window.canvas()->canvasHistoryState().canUndo) {
        qWarning() << "Native canvas drag" << "lower presses" << lower.presses << "canUndo"
                   << window.canvas()->canvasHistoryState().canUndo << "tool"
                   << static_cast<int>(window.canvas()->canvasTool());
        qWarning().noquote() << "Canvas:" << nativeWindowDescription(canvasHandle);
        qWarning().noquote() << "Foreground:" << nativeWindowDescription(GetForegroundWindow());
        qWarning().noquote() << "Drag target:"
                             << nativeWindowDescription(WindowFromPoint(hitPoint));
    }
    require(lower.presses == 0 && window.canvas()->canvasHistoryState().canUndo,
            "native dragging on a blank canvas must draw without reaching the lower window");

    require(window.setClickThrough(true), "native click-through entry must succeed");
    settle(50);
    const LONG_PTR passthroughStyles = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    require(reinterpret_cast<HWND>(window.winId()) == canvasHandle &&
                (GetWindowLongPtrW(canvasHandle, GWL_EXSTYLE) & passthroughStyles) ==
                    passthroughStyles,
            "click-through must set native input flags without recreating the canvas HWND");
    require(window.recoveryButton() != nullptr, "native recovery must exist before lower input");
    const QString beforeLowerClick = recoveryHitDescription(*window.recoveryButton());
    const HWND lowerHit = WindowFromPoint(hitPoint);
    const QString beforeLowerInput =
        QStringLiteral("target: %1\nlower: %2\ncanvas: %3\nforeground: %4")
            .arg(nativeWindowDescription(lowerHit), nativeWindowDescription(lowerHandle),
                 nativeWindowDescription(canvasHandle),
                 nativeWindowDescription(GetForegroundWindow()));
    nativeClick(start);
    const bool lowerReceivedClick = waitUntil([&lower] { return lower.presses > 0; });
    if (!lowerReceivedClick || lower.presses != 1) {
        qWarning().noquote() << "Before native click-through input:" << beforeLowerInput;
        qWarning() << "Underlying probe received presses:" << lower.presses;
        qWarning().noquote() << "After native click-through input hit:"
                             << nativeWindowDescription(WindowFromPoint(hitPoint));
    }
    require(lowerReceivedClick && lower.presses == 1,
            "click-through must deliver native input to the lower application");
    QWidget* recovery = window.recoveryButton();
    require(recovery && recovery->isVisible(), "native click-through must expose recovery");
    const HWND recoveryHandle = reinterpret_cast<HWND>(recovery->winId());
    const QPoint recoveryCenter = nativeClientRect(*recovery).center();
    const POINT recoveryPoint{recoveryCenter.x(), recoveryCenter.y()};
    const bool recoveryHitTestable =
        recoveryHandle != canvasHandle &&
        GetAncestor(WindowFromPoint(recoveryPoint), GA_ROOT) == recoveryHandle;
    if (!recoveryHitTestable) {
        qWarning().noquote() << "Recovery before lower activation:" << beforeLowerClick;
        qWarning().noquote() << "Recovery after lower activation:"
                             << recoveryHitDescription(*recovery);
        qWarning().noquote() << "Canvas:" << nativeWindowDescription(canvasHandle);
        qWarning().noquote() << "Lower:"
                             << nativeWindowDescription(reinterpret_cast<HWND>(lower.winId()));
        qWarning() << "Recovery Qt" << recovery->geometry() << recovery->windowOpacity()
                   << recovery->isVisible() << recovery->isEnabled()
                   << recovery->grab().toImage().pixelColor(recovery->rect().center());
    }
    require(recoveryHitTestable, "the separate native recovery HWND must remain hit-testable");
    nativeClick(recoveryCenter);
    require(waitUntil([&window] { return !window.clickThrough(); }) &&
                reinterpret_cast<HWND>(window.winId()) == canvasHandle &&
                (GetWindowLongPtrW(canvasHandle, GWL_EXSTYLE) & passthroughStyles) == 0 &&
                recovery->isHidden() && window.canvas()->canvasTool() == SnowCanvasTool::FreeDraw,
            "native recovery must restore editing and the prior tool on the same HWND");
    require(window.canvas()->clearDocument(), "native recovery fixture must reset its drawing");
    nativeStroke(start, end);
    require(lower.presses == 1 && window.canvas()->canvasHistoryState().canUndo,
            "the restored blank canvas must again capture drawing input");
    window.close();
    lower.close();
}
#endif
