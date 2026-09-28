#include "physical_key_test_support.h"
#include "snow_shot/presentation/screenshotqrcontroller.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/button.h"
#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QScreen>
#include "theme/theme_manager.h"
#include <QClipboard>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTextEdit>
#include <QThread>
#include <QWindow>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#ifdef Q_OS_WIN
// Native z-order assertions need real HWNDs, but must not mix synthetic hover
// with the input desktop's unrelated Enter/Leave events. Keep the user's desktop
// active and create only this test's windows on a private desktop. All Qt objects
// are constructed on the dedicated GUI thread. Exiting it also releases system
// windows (e.g. IME/OLE) that can outlive QApplication and prevent desktop detachment.
int runOnNativeStackingDesktop(const std::function<int()>& run) {
    const std::wstring name = L"SnowQrStacking-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    require(desktop != nullptr, "create isolated native test desktop");
    int result = EXIT_FAILURE;
    std::thread guiThread([&] {
        require(SetThreadDesktop(desktop), "attach native test GUI thread to its desktop");
        result = run();
    });
    guiThread.join();
    require(CloseDesktop(desktop), "release native test desktop");
    return result;
}
#endif

void until(const std::function<bool()>& ready,
           const char* message = "asynchronous operation timed out") {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), message);
}
void advance(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
void windowMouse(QWidget* target, QEvent::Type type, const QPoint& position, Qt::MouseButton button,
                 Qt::MouseButtons buttons) {
    auto* window = target->window()->windowHandle();
    const QPoint local = target->mapTo(target->window(), position);
    QMouseEvent event(type, local, target->mapToGlobal(position), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(window, &event);
}
void windowClick(QWidget* target) {
    const QPoint point = target->rect().center();
    windowMouse(target, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    windowMouse(target, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
}
QPolygonF quad(qreal x, qreal y, qreal size = 30) {
    return {QPointF(x, y), QPointF(x + size, y), QPointF(x + size, y + size), QPointF(x, y + size)};
}
class Decoder final : public ScreenshotQrRecognitionPort {
  public:
    RequestToken recognize(QImage image, QObject*, Completion completion,
                           ScreenshotQrRecognitionMode mode) override {
        require(mode == ScreenshotQrRecognitionMode::QrOnly,
                "automatic scan must skip 1D barcodes");
        pixels = image.size();
        callbacks.append(std::move(completion));
        return quint64(callbacks.size());
    }
    void cancel(RequestToken token) override {
        cancelled.append(token);
    }
    QSize pixels;
    QList<Completion> callbacks;
    QList<RequestToken> cancelled;
};
struct Fixture {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas{runtime};
    Decoder decoder;
    int opened = 0;
    int closed = 0;
    bool openSucceeds = true;
    ScreenshotQrController controller{decoder,
                                      {[this](const QUrl&) {
                                           ++opened;
                                           return openSucceeds;
                                       },
                                       [this] { ++closed; }}};
    ScreenshotQrController::Snapshot snapshot;
    explicit Fixture(Qt::WindowFlags flags = {}) {
        canvas.setWindowFlags(flags);
        canvas.setAttribute(Qt::WA_ShowWithoutActivating,
                            flags.testFlag(Qt::WindowDoesNotAcceptFocus));
        canvas.resize(400, 300);
        canvas.show();
        QImage image(400, 300, QImage::Format_RGB32);
        image.fill(Qt::white);
        snapshot.bounds = QRectF(0, 0, 400, 300);
        snapshot.pixelSize = image.size();
        snapshot.selection.addRect(snapshot.bounds);
        snapshot.layers.append({image, snapshot.bounds, snapshot.bounds});
        controller.attachCanvas(&canvas);
    }
    void start() {
        const auto count = decoder.callbacks.size();
        controller.recognize(snapshot);
        controller.synchronize(snapshot.selection, true, true);
        until([&] { return decoder.callbacks.size() == count + 1; },
              "QR image preparation must reach the recognition port");
    }
    void complete(QList<ScreenshotQrDetection> detections) {
        decoder.callbacks.last()({{}, {}, std::move(detections)});
        QApplication::processEvents();
    }
    QList<QAbstractButton*> markers() {
        return canvas.findChildren<QAbstractButton*>(QStringLiteral("screenshotQrMarker"));
    }
    QAbstractButton* marker(int index) {
        for (auto* button : markers())
            if (button->property("qrDetectionIndex").toInt() == index)
                return button;
        require(false, "marker index exists");
        return nullptr;
    }
    QWidget* popover() {
        return canvas.findChild<QWidget*>(QStringLiteral("screenshotQrPopover"));
    }
};

void geometryVisibilityAndLifecycle() {
    Fixture f;
    const QByteArray document = f.runtime.serializeDocumentSession();
    const QImage exported = f.runtime.renderToImage(f.snapshot.bounds, f.snapshot.pixelSize, {});
    f.start();
    require(f.controller.busy() && !f.controller.available(), "pending scan is unavailable");
    f.complete({{QStringLiteral("same"), quad(40, 50)}, {QStringLiteral("same"), quad(180, 130)}});
    require(f.controller.available() && f.markers().size() == 2,
            "identical payloads at different positions must retain separate markers");
    const QPoint center = f.canvas.canvasToViewTransform().map(QPointF(55, 65)).toPoint();
    require(f.marker(0)->pos() + QPoint(12, 12) == center, "marker follows canvas transform");
    require(f.marker(0)->size() == QSize(24, 24), "marker uses constant logical dimensions");
    require(f.marker(0)->mask().isEmpty(), "antialiased marker has no binary clipping mask");
    require(f.runtime.serializeDocumentSession() == document,
            "markers cannot enter exported document");
    require(f.runtime.renderToImage(f.snapshot.bounds, f.snapshot.pixelSize, {}) == exported,
            "exported pixels exclude marker child widgets");
    f.controller.synchronize(f.snapshot.selection, true, false);
    require(!f.marker(0)->isVisible(), "markers are hidden outside Move");
    f.controller.synchronize(f.snapshot.selection, true, true);
    require(f.marker(0)->isVisible(), "returning to Move restores markers");
    f.controller.setMarkersVisible(false);
    require(!f.marker(0)->isVisible(), "visibility toggle hides markers");
    f.controller.synchronize(f.snapshot.selection, true, false);
    f.controller.synchronize(f.snapshot.selection, true, true);
    require(!f.marker(0)->isVisible(), "tool changes preserve hidden preference");
    f.start();
    f.complete({{QStringLiteral("new"), quad(60, 70)}});
    require(f.controller.markersVisible() && f.marker(0)->isVisible(),
            "recapture resets visibility");
    f.controller.setSuspended(true);
    require(!f.marker(0)->isVisible(), "recapture preparation hides markers");
    f.controller.setSuspended(false);
    require(f.marker(0)->isVisible(), "failed recapture can restore unchanged results");
    QPainterPath changed;
    changed.addRect(QRectF(0, 0, 350, 250));
    f.controller.synchronize(changed, true, true);
    require(f.controller.available() && f.marker(0)->isVisible(),
            "contained result survives resize");
    f.marker(0)->click();
    QPainterPath excluded;
    excluded.addRect(QRectF(80, 75, 200, 150));
    f.controller.synchronize(excluded, true, true);
    require(f.controller.available() && !f.marker(0)->isVisible() && !f.popover()->isVisible(),
            "excluded QR center hides marker and popover without losing results");
    QPainterPath centerOnly;
    centerOnly.addRect(QRectF(70, 80, 10, 10));
    f.controller.synchronize(centerOnly, true, true);
    require(f.marker(0)->isVisible(), "including only the QR center restores the marker");
    f.marker(0)->click();
    require(f.popover()->isVisible(), "restored marker can reopen the popover");
    f.controller.synchronize(changed, true, true);
    require(f.marker(0)->isVisible(), "expanding selection restores result");
    QPainterPath hole;
    hole.addRect(QRectF(62, 72, 5, 5));
    f.controller.synchronize(changed.subtracted(hole), true, true);
    require(f.marker(0)->isVisible(), "hole away from QR center preserves marker");
    f.controller.synchronize(changed.subtracted(centerOnly), true, true);
    require(!f.marker(0)->isVisible() && !f.popover()->isVisible(),
            "hole containing QR center hides marker and popover");
    f.controller.synchronize(changed, true, true);
    require(f.decoder.callbacks.size() == 2, "region edits never rescan");
    f.start();
    f.controller.synchronize(excluded, true, true);
    f.complete({{QStringLiteral("pending"), quad(60, 70)}});
    require(f.controller.available() && !f.marker(0)->isVisible(),
            "pending result survives selection change");
    f.controller.synchronize(centerOnly, true, true);
    require(f.marker(0)->isVisible(), "pending result retains source coordinates");
    f.start();
    f.controller.synchronize(centerOnly, true, true);
    f.complete({{QStringLiteral("pending center"), quad(60, 70)}});
    require(f.controller.available() && f.marker(0)->isVisible(),
            "pending result appears when only its center remains selected");
    f.start();
    f.controller.invalidate();
    require(!f.decoder.cancelled.isEmpty(), "invalidate cancels decoder request");
    f.complete({{QStringLiteral("stale"), quad(40, 50)}});
    require(!f.controller.available() && f.markers().isEmpty(),
            "late completion cannot resurrect markers");
}

void selectionDragPreservesRecognition() {
    Fixture f;
    f.start();
    f.complete({{QStringLiteral("dragged selection"), quad(60, 70)}});
    f.marker(0)->click();
    // The overlay input handler leaves Editing for OverlayVisible at drag start.
    f.controller.synchronize(f.snapshot.selection, false, false);
    require(f.controller.available() && !f.marker(0)->isVisible() && !f.popover()->isVisible(),
            "drag start hides QR UI without discarding recognized results");
    QPainterPath resized;
    resized.addRect(QRectF(70, 80, 10, 10));
    f.controller.synchronize(resized, false, false);
    f.controller.synchronize(resized, true, true);
    require(f.marker(0)->isVisible() && f.decoder.callbacks.size() == 1,
            "drag release restores the selected QR center without rescanning");

    f.start();
    f.controller.synchronize(resized, false, false);
    require(f.controller.busy() && f.decoder.cancelled.isEmpty(),
            "drag start does not cancel pending recognition");
    f.complete({{QStringLiteral("completed during drag"), quad(60, 70)}});
    require(f.controller.available() && !f.marker(0)->isVisible(),
            "results arriving during a drag are retained but hidden");
    f.controller.synchronize(resized, true, true);
    require(f.marker(0)->isVisible(), "drag release shows results completed during the drag");
    f.controller.invalidate();
    f.controller.synchronize(resized, true, true);
    require(!f.controller.available() && f.markers().isEmpty(),
            "session invalidation prevents later presentation from restoring old results");
}

void sourceMaskAndValidation() {
    Fixture f;
    QPainterPath hole;
    hole.addRect(QRectF(110, 100, 40, 40));
    f.snapshot.selection = f.snapshot.selection.subtracted(hole);
    f.snapshot.layers[0].image.fill(Qt::black);
    const auto prepared = ScreenshotQrController::prepareImage(f.snapshot, std::atomic_bool(false));
    require(prepared.pixelColor(120, 110) == Qt::white && prepared.pixelColor(50, 50) == Qt::black,
            "preparation masks excluded pixels without adding screenshot decoration");
    f.start();
    f.complete({{QStringLiteral("inside"), quad(20, 20)},
                {QStringLiteral("hole"), quad(110, 100)},
                {QStringLiteral("crossing"), quad(90, 90)},
                {QStringLiteral("outside"), quad(390, 290)},
                {QStringLiteral("bad"), {QPointF(10, 10)}}});
    require(f.markers().size() == 1, "entire quadrilateral must lie within selection");
    f.start();
    f.decoder.callbacks.last()({{}, QStringLiteral("detector failure"), {}});
    require(!f.controller.available() && !f.controller.error().isEmpty(),
            "failure is exposed without modal UI");
    f.start();
    f.complete({});
    require(!f.controller.available() && f.controller.error().isEmpty(),
            "empty scan is not a failure");
    f.snapshot.pixelSize = QSize(7680, 4320);
    const auto large = ScreenshotQrController::prepareImage(f.snapshot, std::atomic_bool(false));
    require(large.width() <= 2560 && qint64(large.width()) * large.height() <= 1920 * 1080,
            "preparation bounds memory before composing large captures");
    require(ScreenshotQrController::prepareImage(f.snapshot, std::atomic_bool(true)).isNull(),
            "cancelled preparation returns no image");
}

void transformsRegionsAndSupersededRequests() {
    Fixture f;
    f.snapshot.pixelSize *= 2;
    QPainterPath regions;
    regions.addRect(QRectF(0, 0, 100, 100));
    regions.addRect(QRectF(200, 100, 100, 100));
    f.snapshot.selection = regions;
    f.start();
    f.complete({{QStringLiteral("left"), quad(40, 40, 60)},
                {QStringLiteral("right"), quad(420, 220, 60)},
                {QStringLiteral("gap"), quad(260, 220, 60)}});
    require(f.markers().size() == 2, "disjoint selected regions exclude intervening pixels");
    require(f.marker(0)->pos() + QPoint(12, 12) ==
                f.canvas.canvasToViewTransform().map(QPointF(35, 35)).toPoint(),
            "physical source pixels map into logical canvas coordinates");
    require(f.canvas.setViewportCamera(100, 100, 1.5), "set zoomed camera");
    QApplication::processEvents();
    require(f.marker(0)->pos() + QPoint(12, 12) ==
                    f.canvas.canvasToViewTransform().map(QPointF(35, 35)).toPoint() &&
                f.marker(0)->size() == QSize(24, 24),
            "camera changes reposition markers without scaling their hit area");
    f.start();
    const auto stale = f.decoder.callbacks.last();
    f.start();
    stale({{}, {}, {{QStringLiteral("old"), quad(40, 40)}}});
    require(f.controller.busy() && !f.controller.available(),
            "old result cannot end newer request");
    f.complete({{QStringLiteral("current"), quad(40, 40)}});
    require(f.controller.available(), "current result is accepted");
    f.controller.synchronize(f.snapshot.selection, false, true);
    require(f.controller.available() && !f.marker(0)->isVisible(),
            "leaving Editing hides ephemeral UI while retaining session results");
    f.controller.invalidate();
    require(!f.controller.available() && f.markers().isEmpty(),
            "invalidating the capture session clears ephemeral UI");
}

void separateDisplayCanvases() {
    Fixture f;
    f.canvas.resize(200, 300);
    require(f.canvas.setViewportCamera(100, 150, 1), "first display camera");
    SnowCanvasWidget second(f.runtime);
    second.resize(200, 300);
    second.show();
    require(second.setViewportCamera(300, 150, 1), "second display camera");
    f.controller.attachCanvas(&second);
    f.start();
    f.complete({{QStringLiteral("left"), quad(30, 50)}, {QStringLiteral("right"), quad(230, 50)}});
    require(f.marker(0)->isVisible() && !f.marker(1)->isVisible(),
            "first display clips foreign markers");
    const auto rightMarkers =
        second.findChildren<QAbstractButton*>(QStringLiteral("screenshotQrMarker"));
    int visibleCount = 0;
    for (auto* marker : rightMarkers) {
        if (marker->isVisible()) {
            ++visibleCount;
            require(marker->property("qrDetectionIndex").toInt() == 1,
                    "second display shows its own QR");
            marker->click();
            auto* popup = second.findChild<QWidget*>(QStringLiteral("screenshotQrPopover"));
            require(popup && popup->isVisible(), "popover belongs to marker's display window");
        }
    }
    require(visibleCount == 1, "one visible QR marker per display");
}

void hoverPopoverPreservesOwnerStacking() {
    Fixture f(Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    f.start();
    f.complete({{QStringLiteral("stacking"), quad(50, 60)}});
    QWidget unrelated(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    unrelated.setAttribute(Qt::WA_ShowWithoutActivating);
    unrelated.setGeometry(600, 50, 100, 100);
    unrelated.show();
    const auto verify = [&] {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            const auto above = [](QWidget* first, QWidget* second) {
                for (HWND window = GetTopWindow(nullptr); window;
                     window = GetWindow(window, GW_HWNDNEXT)) {
                    if (window == reinterpret_cast<HWND>(first->internalWinId()))
                        return true;
                    if (window == reinterpret_cast<HWND>(second->internalWinId()))
                        return false;
                }
                return false;
            };
            require(above(&unrelated, &f.canvas), "QR hover must not raise the canvas owner");
            if (f.popover() && f.popover()->isVisible())
                require(above(&unrelated, f.popover()) && above(f.popover(), &f.canvas),
                        "QR hover must stay above its owner and below unrelated topmosts");
        }
#endif
    };
    for (int cycle = 0; cycle < 3; ++cycle) {
        unrelated.raise();
        QApplication::processEvents();
        verify();
        auto* marker = f.marker(0);
        QEnterEvent enter(QPointF(12, 12), QPointF(12, 12), marker->mapToGlobal(QPoint(12, 12)));
        QApplication::sendEvent(marker, &enter);
        until([&] { return f.popover() && f.popover()->isVisible(); },
              "QR hover must reveal the popover");
        require(marker->internalWinId() == 0,
                "native QR hover must preserve the marker's non-native surface");
        verify();
        f.canvas.resize(f.canvas.size() + QSize(1, 1));
        QApplication::processEvents();
        verify();
        f.controller.setSuspended(true);
        QApplication::processEvents();
        require(!f.popover()->isVisible(), "suspending must hide QR hover");
        verify();
        f.controller.setSuspended(false);
    }
}

void enterMarker(QWidget* marker) {
    const QPoint center = marker->rect().center();
    QEnterEvent enter(center, marker->mapTo(marker->window(), center), marker->mapToGlobal(center));
    QApplication::sendEvent(marker, &enter);
}

void popoverCreationPreservesCanvasChildren() {
    for (const bool staysOnTop : {false, true}) {
        Fixture f(Qt::Window | (staysOnTop ? Qt::WindowStaysOnTopHint : Qt::WindowFlags{}));
        f.start();
        f.complete({{QStringLiteral("surface"), quad(50, 60)}});
        auto* marker = f.marker(0);
        require(marker->internalWinId() == 0, "QR marker starts without a native surface");
        for (int cycle = 0; cycle < 3; ++cycle) {
            marker->click();
            require(f.popover() && f.popover()->isVisible(), "click opens QR popover");
            require(marker->internalWinId() == 0 && !marker->testAttribute(Qt::WA_NativeWindow),
                    "opening a QR popover must not make canvas children native");
            require(f.popover()->windowHandle()->transientParent() == f.canvas.windowHandle(),
                    "QR native surface retains its canvas owner");
            require(f.popover()->windowFlags().testFlag(Qt::WindowStaysOnTopHint) == staysOnTop,
                    "QR popover inherits its owner's stacking band");
            f.controller.dismissPopover();
        }
    }
}

void popoverRelayoutPreservesPendingHover() {
    Fixture f;
    f.start();
    f.complete(
        {{QStringLiteral("first"), quad(50, 60)}, {QStringLiteral("second"), quad(150, 60)}});
    enterMarker(f.marker(0));
    until([&] { return f.popover() && f.popover()->isVisible(); });
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(f.marker(0), &leave);
    enterMarker(f.marker(1));
    f.canvas.move(f.canvas.pos() + QPoint(12, 8));
    until(
        [&] {
            return f.popover()->findChild<QTextEdit*>()->toPlainText() == QStringLiteral("second");
        },
        "relayout must preserve a pending hover on another QR marker");
}

void popoverRelayoutPreservesDismissal() {
    Fixture f;
    f.start();
    f.complete({{QStringLiteral("dismiss"), quad(50, 60)}});
    enterMarker(f.marker(0));
    until([&] { return f.popover() && f.popover()->isVisible(); });
    auto* text = f.popover()->findChild<QTextEdit*>();
    text->setFocus();
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(f.marker(0), &leave);
    QApplication::sendEvent(f.popover(), &leave);
    f.canvas.resize(f.canvas.size() + QSize(1, 1));
    advance(300);
    require(f.popover()->isVisible(), "focused QR text retains its popover after pointer exit");
    text->clearFocus();
    QApplication::sendEvent(f.popover(), &leave);
    f.canvas.resize(f.canvas.size() + QSize(1, 1));
    until([&] { return !f.popover()->isVisible(); },
          "relayout must preserve dismissal after leaving a QR popover");
}

void visiblePopoverRelayoutDoesNotRaise() {
    Fixture f;
    f.start();
    f.complete({{QStringLiteral("relayout"), quad(50, 60)}});
    f.marker(0)->click();
    QApplication::processEvents();
    class StackingEvents final : public QObject {
      public:
        int count = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::ZOrderChange)
                ++count;
            return false;
        }
    } events;
    f.popover()->installEventFilter(&events);
    const QPoint previousPosition = f.popover()->pos();
    f.canvas.move(f.canvas.pos() + QPoint(12, 8));
    QApplication::processEvents();
    require(f.popover()->isVisible() && f.popover()->pos() != previousPosition,
            "visible QR popover must follow its moved canvas");
    require(events.count == 0, "QR popover geometry updates must not raise its native window");
}

void popoverActionsAndHover() {
    Fixture f;
    f.start();
    const QString payload = QStringLiteral("  <b>Text</b>\n二维码\n  ");
    f.complete({{payload, quad(50, 60)}});
    auto* marker = f.marker(0);
    QEnterEvent enter(QPointF(12, 12), QPointF(12, 12), marker->mapToGlobal(QPoint(12, 12)));
    QApplication::sendEvent(marker, &enter);
    require(!f.popover(), "hover opening is delayed");
    advance(175);
    require(f.popover() && f.popover()->isVisible(), "hover opens tool popover");
    require(f.popover()->windowType() == Qt::Tool, "popover is a Qt Tool window");
    auto* text = f.popover()->findChild<QTextEdit*>();
    auto* copy = f.popover()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotQrCopyTextButton"));
    auto* open = f.popover()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotQrOpenUrlButton"));
    require(text->isReadOnly() && text->toPlainText() == payload,
            "payload is literal selectable text");
    require(copy->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid &&
                !open->isVisible(),
            "plain text has one primary Copy Text action");
    windowClick(copy);
    require(QApplication::clipboard()->text() == payload && f.closed == 1 &&
                !f.popover()->isVisible(),
            "copy preserves exact payload and finishes editor");
    marker->click();
    QTextCursor start = text->textCursor();
    start.setPosition(0);
    QTextCursor end = start;
    end.setPosition(7);
    const QPoint from = text->cursorRect(start).center();
    const QPoint to = text->cursorRect(end).center();
    windowMouse(text->viewport(), QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    windowMouse(text->viewport(), QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    windowMouse(text->viewport(), QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    require(f.popover()->isVisible() && text->textCursor().selectedText() == payload.left(7),
            "window mouse drag selects text without dismissing popover");
    windowClick(copy);
    require(QApplication::clipboard()->text() == payload.left(7),
            "Copy Text copies only the mouse selection");
    require(!f.popover()->isVisible(), "copy dismisses popover immediately");
    marker->click();
    start.setPosition(2);
    start.setPosition(static_cast<int>(payload.size()) - 2, QTextCursor::KeepAnchor);
    text->setTextCursor(start);
    windowClick(copy);
    require(QApplication::clipboard()->text() == payload.mid(2, payload.size() - 4),
            "selected multiline copy preserves newlines");
    require(!f.popover()->isVisible(), "copy dismisses popover immediately");
    marker->click();
    start.clearSelection();
    text->setTextCursor(start);
    windowClick(copy);
    require(QApplication::clipboard()->text() == payload,
            "copy without a selection restores the complete payload");
    require(!f.popover()->isVisible(), "copy dismisses popover immediately");
    marker->click();
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(marker, &leave);
    advance(100);
    QApplication::sendEvent(f.popover(), &enter);
    advance(200);
    require(f.popover()->isVisible(), "pointer may cross the marker-popover gap");
    windowClick(marker);
    require(f.popover()->isVisible(), "pressing the active marker preserves its popover");
    windowClick(&f.canvas);
    require(!f.popover()->isVisible(), "outside canvas press dismisses the popover");
    windowClick(marker);
    require(f.popover()->isVisible(), "window mouse click reopens the marker popover");
    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(text, &escape);
    require(!f.popover()->isVisible() && f.closed == 4, "Escape dismisses popover before editor");
    f.start();
    f.complete({{QStringLiteral(" https://example.com/a?q=b "), quad(50, 60)}});
    f.marker(0)->click();
    require(f.controller.ownsInput(text), "read-only popover text owns screenshot shortcuts");
    text->selectAll();
    PhysicalKeyEvent copyKey(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(text, &copyKey);
    require(QApplication::clipboard()->text() == QStringLiteral(" https://example.com/a?q=b "),
            "Ctrl+C copies selected decoded text");
    require(open->isVisible() &&
                copy->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Outline,
            "web URL offers regular copy and primary open");
    f.openSucceeds = false;
    windowClick(open);
    require(f.closed == 4 && f.popover()->isVisible(), "failed opening preserves editor");
    f.openSucceeds = true;
    windowClick(open);
    require(f.opened == 2 && f.closed == 5, "successful opening closes editor");
}

void antialiasedMarkerAndPopoverArrow() {
    Fixture f;
    require(f.canvas.setViewportCamera(200, 150, 1), "place marker at known screen coordinates");
    f.start();
    f.complete({{QStringLiteral("arrow"), quad(50, 60)}});
    auto* marker = f.marker(0);
    QImage rendered(marker->size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    marker->render(&rendered);
    bool smoothEdge = false;
    for (int y = 0; y < rendered.height(); ++y)
        for (int x = 0; x < rendered.width(); ++x) {
            const int alpha = rendered.pixelColor(x, y).alpha();
            smoothEdge = smoothEdge || (alpha > 0 && alpha < 255);
        }
    require(rendered.pixelColor(0, 0).alpha() == 0 && smoothEdge,
            "marker has transparent corners and fractional coverage at circular edge");
    const QRect screen = f.canvas.screen()->availableGeometry();
    for (const bool right : {false, true}) {
        f.canvas.move(right ? screen.right() - 150 : screen.left(), screen.top());
        marker->click();
        QApplication::processEvents();
        auto* bubble = f.popover()->findChild<QWidget*>(QStringLiteral("screenshotQrPopoverBody"));
        const QImage image = bubble->grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        const int y = marker->mapToGlobal(marker->rect().center()).y() - f.popover()->y();
        const int x = right ? bubble->width() - 7 : 6;
        require(image.pixelColor(qRound(x * dpr), qRound(y * dpr)).alpha() > 220,
                "popover arrow points toward marker on either side");
        require(image.pixelColor(qRound(x * dpr), qRound((y + 20) * dpr)).alpha() < 160,
                "arrow projects beyond the bubble body");
        require(screen.contains(f.popover()->geometry()), "arrow popover fits screen");
        f.controller.dismissPopover();
    }
}

void renderPreviews(const QString& directory) {
    require(QDir().mkpath(directory), "preview directory");
    Fixture f;
    f.canvas.resize(900, 600);
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        adqt::theme::ThemeManager::instance().setColorScheme(theme);
        const QString prefix = theme == adqt::theme::ThemeScheme::Light ? QStringLiteral("light")
                                                                        : QStringLiteral("dark");
        for (bool link : {false, true}) {
            f.start();
            const auto text =
                link ? QStringLiteral(
                           "https://snowshot.example/documentation?topic=screenshots&language=en")
                     : QStringLiteral("Meeting notes\nRoom 302 · 14:30\nBring your "
                                      "laptop.\n\n二维码 / QR Code");
            f.complete({{text, quad(140, 140)}});
            f.marker(0)->click();
            advance(100);
            const QString base = directory + QLatin1Char('/') + prefix +
                                 (link ? QStringLiteral("-url") : QStringLiteral("-text"));
            require(f.popover()->grab().save(base + QStringLiteral(".png")),
                    "save popover preview");
            require(f.marker(0)->grab().save(base + QStringLiteral("-marker.png")),
                    "save marker preview");
            require(f.popover()->screen()->availableGeometry().contains(f.popover()->geometry()),
                    "popover fits screen");
            f.controller.dismissPopover();
        }
    }
}

void disablingRecognitionCancelsAndRejectsLateResults() {
    Fixture f;
    f.controller.setEnabled(false);
    f.controller.recognize(f.snapshot);
    require(!f.controller.busy() && !f.controller.available() && f.decoder.callbacks.isEmpty(),
            "disabled recognition must not submit work or enable the toolbar action");
    f.controller.setEnabled(true);
    f.start();
    f.controller.setEnabled(false);
    require(f.decoder.cancelled.size() == 1 && !f.controller.busy(),
            "disabling recognition must cancel the pending request");
    f.decoder.callbacks.last()(
        {{QStringLiteral("late")}, {}, {{QStringLiteral("late"), quad(10, 10)}}});
    require(!f.controller.available(), "late completion must not restore QR availability");
    f.controller.setEnabled(true);
    f.start();
    require(f.decoder.callbacks.size() == 2, "re-enabling must allow recognition again");
    f.complete({{QStringLiteral("ready"), quad(10, 10)}});
    require(f.controller.available() && !f.markers().isEmpty(), "recognized codes enable markers");
    f.controller.setEnabled(false);
    require(!f.controller.available() && f.markers().isEmpty(),
            "disabling recognition must remove markers and disable the toolbar action");
}

void urlClassification() {
    for (const auto& text :
         {QStringLiteral("https://example.com"), QStringLiteral(" HTTP://example.com/a "),
          QStringLiteral("https://example.com/a%20b")})
        require(!ScreenshotQrController::webUrl(text).isEmpty(), "valid web URL");
    for (const auto& text :
         {QStringLiteral("www.example.com"), QStringLiteral("mailto:a@example.com"),
          QStringLiteral("file:///tmp/a"), QStringLiteral("https://"),
          QStringLiteral("read https://example.com"), QStringLiteral("https://example.com a"),
          QStringLiteral("https://example.com/%zz")})
        require(ScreenshotQrController::webUrl(text).isEmpty(),
                "non-web or malformed payload is plain text");
}
} // namespace

int runTests(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "load Segoe UI for offscreen visual checks");
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) >= 0,
            "load Chinese glyphs for offscreen visual checks");
    app.setFont(QFont(QStringLiteral("Segoe UI")));
#endif
    if (app.arguments().contains(QStringLiteral("--preview-dir"))) {
        const qsizetype index = app.arguments().indexOf(QStringLiteral("--preview-dir"));
        require(index + 1 < app.arguments().size(), "preview output directory required");
        renderPreviews(app.arguments()[index + 1]);
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--stacking-only"))) {
        hoverPopoverPreservesOwnerStacking();
        return 0;
    }
    hoverPopoverPreservesOwnerStacking();
    antialiasedMarkerAndPopoverArrow();
    urlClassification();
    disablingRecognitionCancelsAndRejectsLateResults();
    geometryVisibilityAndLifecycle();
    selectionDragPreservesRecognition();
    sourceMaskAndValidation();
    transformsRegionsAndSupersededRequests();
    separateDisplayCanvases();
    visiblePopoverRelayoutDoesNotRaise();
    popoverCreationPreservesCanvasChildren();
    popoverRelayoutPreservesPendingHover();
    popoverRelayoutPreservesDismissal();
    popoverActionsAndHover();
    return 0;
}

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stacking-only") == 0)
            return runOnNativeStackingDesktop([&] { return runTests(argc, argv); });
    }
#endif
    return runTests(argc, argv);
}
