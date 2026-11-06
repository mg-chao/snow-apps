#include "canvas_automatic_selection_test_support.h"
#include "angle_wheel_host_test_support.h"
#include "canvas_text_editing_preferences_test_support.h"
#include "../../test-support/canvas_quick_selection_test_support.h"
#include "physical_key_test_support.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/crosshaircursor.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "../src/presentation/recording/recordingcountdownoverlay.h"
#include "../src/presentation/recording/recordingregiondraghandle.h"
#include "../src/presentation/recording/recordingregioninputrouter.h"
#include "../src/presentation/recording/recordingwebcamcontrol.h"
#include <QToolButton>
#include <algorithm>
#include "../src/presentation/recording/screenrecordinggeometry.h"

#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QDialog>
#include <QSemaphore>
#include <QtMath>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QTranslator>
#include "theme/theme_manager.h"
#include <QWheelEvent>
#include <QTimer>
#include <QWindow>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <QScopeGuard>
#ifdef Q_OS_MACOS
#include "macos_native_input.h"
#include "safe_area_test_support.h"
#include <stdexcept>
#endif
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <cstdlib>
#include <atomic>
#include <iostream>
#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

class ScreenRecordingAreaWindowTestAccess {
  public:
    static QMargins insets(const ScreenRecordingAreaWindow& area) {
        return area.m_physicalInsets.toMargins();
    }
    static bool editable(const ScreenRecordingAreaWindow& area) {
        return area.regionEditingEnabled();
    }
    static bool observing(const ScreenRecordingAreaWindow& area) {
        return area.m_regionInputRouter != nullptr;
    }
    static Qt::Edges edges(const ScreenRecordingAreaWindow& area, QPointF position) {
        return area.resizeEdgesAt(position);
    }
    static QRegion inputRegion(const ScreenRecordingAreaWindow& area) {
        return area.regionInteractionRegion();
    }
    static void beginDrag(ScreenRecordingAreaWindow& area, QPoint point, Qt::Edges edges) {
        area.beginRegionDrag(point, edges);
    }
    static void drag(ScreenRecordingAreaWindow& area, QPoint point) {
        area.updateRegionDrag(point);
    }
    static void cancel(ScreenRecordingAreaWindow& area) {
        area.cancelRegionInteraction();
    }
    static bool dragging(const ScreenRecordingAreaWindow& area) {
        return area.m_regionDragActive;
    }
    static void begin(ScreenRecordingAreaWindow& area) {
        area.beginRegionDrag(area.recordingRegion().topLeft(), Qt::LeftEdge);
    }
    static void finish(ScreenRecordingAreaWindow& area) {
        area.finishRegionInteraction();
    }
    static QByteArray history(const ScreenRecordingAreaWindow& area) {
        return area.m_canvasRuntime->serializeDocumentHistory();
    }
};

namespace {
class RegionMoveTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) == "RecordingRegionDragHandle" &&
            QByteArray(source) == "Move screen recording area")
            return QStringLiteral("Translated recording move");
        return {};
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

#ifdef Q_OS_WIN
struct RegionMouseInput {
    static inline RegionMouseInput* instance = nullptr;
    static constexpr ULONG_PTR nativeMarker = 0x534E4F57;
    QSemaphore installed;
    QSemaphore observed;
    std::atomic<int> forwarded{0};
    std::atomic<int> removals{0};
    DWORD installingThread = 0;
    DWORD removingThread = 0;
    bool failInstall = false;
    bool native = false;
    bool initialPositionOnly = false;
    QPoint initialPosition{-80, 120};

    static HHOOK WINAPI install(int kind, HOOKPROC callback, HINSTANCE, DWORD) {
        instance->installingThread = GetCurrentThreadId();
        require(kind == WH_MOUSE_LL, "region observation must use the mouse hook");
        if (instance->native) {
            const HHOOK hook = SetWindowsHookExW(kind, callback, GetModuleHandleW(nullptr), 0);
            require(hook != nullptr, "the native regression hook must install");
            instance->installed.release();
            return hook;
        }
        if (instance->failInstall) {
            instance->observed.release();
            SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }
        if (instance->initialPositionOnly) {
            QTimer::singleShot(0, QAbstractEventDispatcher::instance(),
                               [] { instance->observed.release(); });
            return reinterpret_cast<HHOOK>(static_cast<INT_PTR>(1));
        }
        QTimer::singleShot(0, QAbstractEventDispatcher::instance(), [callback] {
            require(callback(-1, WM_MOUSEMOVE, 0) == 17,
                    "unhandled mouse input must retain the next hook's result");
            for (const POINT point :
                 {POINT{20, 30}, POINT{-40, -50}, POINT{-2147483647, 2147483647}}) {
                MSLLHOOKSTRUCT input{};
                input.pt = point;
                require(callback(HC_ACTION, WM_MOUSEMOVE, reinterpret_cast<LPARAM>(&input)) == 17,
                        "observing movement must never consume system input");
            }
            instance->observed.release();
        });
        return reinterpret_cast<HHOOK>(static_cast<INT_PTR>(1));
    }

    static BOOL WINAPI remove(HHOOK hook) {
        instance->removingThread = GetCurrentThreadId();
        ++instance->removals;
        return instance->native ? UnhookWindowsHookEx(hook) : TRUE;
    }

    static LRESULT WINAPI next(HHOOK hook, int code, WPARAM message, LPARAM data) {
        ++instance->forwarded;
        if (instance->native) {
            if (code == HC_ACTION && message == WM_MOUSEMOVE &&
                reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->dwExtraInfo == nativeMarker)
                instance->observed.release();
            return CallNextHookEx(hook, code, message, data);
        }
        return 17;
    }

    static BOOL WINAPI cursorPosition(LPPOINT point) {
        if (instance->native)
            return GetCursorPos(point);
        point->x = instance->initialPosition.x();
        point->y = instance->initialPosition.y();
        return TRUE;
    }

    RecordingRegionMouseApi api() {
        instance = this;
        return {install, remove, next, [] { return true; }, cursorPosition};
    }
};

void regionObservationNeverWaitsForTheGui() {
    const DWORD guiThread = GetCurrentThreadId();
    for (const bool retireBeforeDelivery : {false, true}) {
        RegionMouseInput input;
        input.initialPositionOnly = true;
        int deliveries = 0;
        auto router = std::make_unique<RecordingRegionInputRouter>(
            [&](const QPoint& point) {
                require(GetCurrentThreadId() == guiThread && point == input.initialPosition,
                        "observer startup must publish the current pointer on the GUI thread");
                ++deliveries;
            },
            input.api());
        require(input.observed.tryAcquire(1, 1000),
                "observer startup must complete without awaiting movement");
        require(deliveries == 0, "startup observation must not call widgets from the worker");
        if (retireBeforeDelivery)
            router.reset();
        QCoreApplication::processEvents();
        require(deliveries == (retireBeforeDelivery ? 0 : 1),
                "initial routing must deliver once and retire with its observer");
    }
    for (const bool retireBeforeDelivery : {false, true}) {
        RegionMouseInput input;
        int deliveries = 0;
        auto router = std::make_unique<RecordingRegionInputRouter>(
            [&](const QPoint& point) {
                require(GetCurrentThreadId() == guiThread,
                        "routing must update widgets on the GUI thread");
                require(point == QPoint(-2147483647, 2147483647),
                        "coalesced routing must retain the newest signed desktop coordinates");
                ++deliveries;
            },
            input.api());
        // Deliberately do not pump the GUI: this represents lazy settings creation
        // or any other synchronous UI work while the recording region is editable.
        require(input.observed.tryAcquire(1, 1000),
                "all mouse input must be forwarded even while the GUI is blocked");
        require(input.installingThread != guiThread && input.forwarded == 4 && deliveries == 0,
                "system input observation must be isolated from widget updates");
        if (retireBeforeDelivery)
            router.reset();
        QCoreApplication::processEvents();
        require(deliveries == (retireBeforeDelivery ? 0 : 1),
                "routing must coalesce movement and discard updates when observation ends");
        router.reset();
        require(input.removals == 1 && input.removingThread == input.installingThread,
                "the owning worker must retire its hook before destruction");
    }
    RegionMouseInput failed;
    failed.failInstall = true;
    {
        RecordingRegionInputRouter router(
            [](const QPoint&) {
                require(false, "failed hook installation must not publish input");
            },
            failed.api());
        require(failed.observed.tryAcquire(1, 1000), "failed hook startup must complete");
    }
    require(failed.removals == 0, "failed startup must not unhook an invalid handle");
    RegionMouseInput::instance = nullptr;
}

void nativeRegionObservationNeverWaitsForTheGui() {
    RegionMouseInput input;
    input.native = true;
    POINT saved{};
    require(GetCursorPos(&saved), "the native regression must save the pointer position");
    bool observed = false;
    {
        RecordingRegionInputRouter router([](const QPoint&) {}, input.api());
        require(input.installed.tryAcquire(1, 2000), "native mouse observation must start");
        INPUT movement{};
        movement.type = INPUT_MOUSE;
        movement.mi.dx = 1;
        movement.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
        movement.mi.dwExtraInfo = RegionMouseInput::nativeMarker;
        require(SendInput(1, &movement, sizeof(movement)) == 1,
                "the native regression must inject a pointer event");
        observed = input.observed.tryAcquire(1, 250);
        if (!observed) {
            qInfo() << "Native forwarding while GUI blocked:" << input.forwarded.load();
            QCoreApplication::processEvents();
            qInfo() << "Native forwarding after GUI resumed:" << input.forwarded.load();
        }
    }
    RegionMouseInput::instance = nullptr;
    SetCursorPos(saved.x, saved.y);
    require(observed, "native system mouse input must pass without pumping the GUI event loop");
}
#endif

QRect testRecordingRegion();

void modalsSuspendRecordingInputAndRestoreTheRequestedMode() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    for (const auto mode : {ScreenRecordingAreaWindow::InputMode::RegionEditing,
                            ScreenRecordingAreaWindow::InputMode::Drawing}) {
        area.setInputMode(mode);
        QCoreApplication::processEvents();
        if (mode == ScreenRecordingAreaWindow::InputMode::RegionEditing)
            ScreenRecordingAreaWindowTestAccess::begin(area);
        QDialog modal(&area);
        modal.setWindowModality(Qt::ApplicationModal);
        modal.show();
        QCoreApplication::processEvents();
        require(!ScreenRecordingAreaWindowTestAccess::editable(area) &&
                    !ScreenRecordingAreaWindowTestAccess::observing(area) &&
                    !ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                    ScreenRecordingAreaWindowTestAccess::inputRegion(area).isEmpty() &&
                    !area.canvas()->interactionEnabled() && area.inputSurfaceColor().alpha() == 0,
                "modal opening must cancel drags, retire observation and suspend drawing and "
                "resize input");
        require(area.inputMode() == mode, "modal blocking must preserve the requested input mode");
        modal.hide();
        QCoreApplication::processEvents();
        require(ScreenRecordingAreaWindowTestAccess::editable(area) ==
                        (mode == ScreenRecordingAreaWindow::InputMode::RegionEditing) &&
                    area.canvas()->interactionEnabled() ==
                        (mode == ScreenRecordingAreaWindow::InputMode::Drawing),
                "modal closing must restore the requested recording interaction");
    }
    area.setTrimming(true);
    QDialog modal(&area);
    modal.setWindowModality(Qt::ApplicationModal);
    modal.show();
    QCoreApplication::processEvents();
    require(ScreenRecordingAreaWindowTestAccess::inputRegion(area).isEmpty(),
            "trimming must also relinquish its input surface while a modal is open");
    area.setDrawingBlocked(true);
    area.setTrimming(false);
    modal.hide();
    QCoreApplication::processEvents();
    require(!area.canvas()->interactionEnabled(),
            "closing a modal must not clear another input block");
}

void sendMouseEvent(SnowCanvasWidget& canvas, QEvent::Type type, const QPointF& position,
                    Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &event);
}

void drawRectangle(SnowCanvasWidget& canvas, const QPointF& start, const QPointF& end) {
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "recording canvas should accept the shape tool");
    sendMouseEvent(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
}

bool imageHasVisiblePixel(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() != 0) {
                return true;
            }
        }
    }
    return false;
}

QImage renderWidget(QWidget& widget, qreal scale = 1.0) {
    QImage image(QSize(qCeil(widget.width() * scale), qCeil(widget.height() * scale)),
                 QImage::Format_RGBA8888);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}

QRect testRecordingRegion() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "recording area tests require an offscreen primary screen");
#ifdef Q_OS_MACOS
    const QRect bounds = screen->geometry();
#else
    const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
#endif
    const int width = qMin(320, qMax(80, bounds.width() / 2));
    const int height = qMin(240, qMax(60, bounds.height() / 2));
    return QRect(bounds.left() + qMax(0, (bounds.width() - width) / 4),
                 bounds.top() + qMax(0, (bounds.height() - height) / 4), width, height);
}

#ifdef Q_OS_MACOS
void recordingBorderInput(bool native) {
    ScreenRecordingAreaWindow area;
    const QRect initial(QGuiApplication::primaryScreen()->availableGeometry().center() -
                            QPoint(120, 80),
                        QSize(240, 160));
    area.setRecordingRegion(initial);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.show();
    QCoreApplication::processEvents();
    if (native)
        macActivateApplication();
    int starts = 0, finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted,
                     [&] { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished,
                     [&] { ++finishes; });
    const auto check = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    for (const QPoint direction : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1),
                                   QPoint(-1, -1), QPoint(1, -1), QPoint(-1, 1), QPoint(1, 1)}) {
        area.setRecordingRegion(initial);
        QCoreApplication::processEvents();
        // Hit the painted outer border, where AppKit can intercept input before Qt.
        const QPoint start(direction.x() < 0   ? initial.x() - 2
                           : direction.x() > 0 ? initial.x() + initial.width() + 1
                                               : initial.center().x(),
                           direction.y() < 0   ? initial.y() - 2
                           : direction.y() > 0 ? initial.y() + initial.height() + 1
                                               : initial.center().y());
        const auto send = [&](QEvent::Type type, QPoint global) {
            const QPoint local = area.mapFromGlobal(global);
            QMouseEvent event(type, local, local, global,
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(&area, &event);
            QCoreApplication::processEvents();
        };
        std::unique_ptr<MacMouseDrag> drag;
        if (native) {
            // WindowServer commits the preceding frame change asynchronously.
            QElapsedTimer timer;
            timer.start();
            while (!macWindowReceivesPoint(&area, start) && timer.elapsed() < 1000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            check(macWindowReceivesPoint(&area, start), "recording border must receive input");
            drag = std::make_unique<MacMouseDrag>(start);
        } else {
            send(QEvent::MouseButtonPress, start);
        }
        check(ScreenRecordingAreaWindowTestAccess::dragging(area),
              "border input must begin the controlled resize");
        for (int extent : {1, -40, 1, 60}) {
            const QPoint end = start - QPoint(direction.x() * (initial.width() - extent),
                                              direction.y() * (initial.height() - extent));
            if (native)
                drag->moveTo(end);
            else
                send(QEvent::MouseMove, end);
            const int minimum = snow_shot::presentation::recording::screenRecordingMinimumExtent(
                area.devicePixelRatioF());
            const int size = qMax(minimum, qAbs(extent));
            QRect expected = initial;
            if (direction.x()) {
                const int anchor = direction.x() > 0 ? initial.x() : initial.x() + initial.width();
                expected.setRect(anchor - (direction.x() * extent < 0 ? size : 0), expected.y(),
                                 size, expected.height());
            }
            if (direction.y()) {
                const int anchor = direction.y() > 0 ? initial.y() : initial.y() + initial.height();
                expected.setRect(expected.x(), anchor - (direction.y() * extent < 0 ? size : 0),
                                 expected.width(), size);
            }
            check(area.recordingRegion() == expected,
                  "each border must retain its anchor, enforce the minimum and flip both ways");
            check(area.selectionRect().size().toSize() == expected.size(),
                  "visible selection must match the committed recording region");
        }
        if (native)
            drag->finish();
        else
            send(QEvent::MouseButtonRelease,
                 start - QPoint(direction.x() * (initial.width() - 60),
                                direction.y() * (initial.height() - 60)));
        check(!ScreenRecordingAreaWindowTestAccess::dragging(area) && starts == finishes,
              "border release must finish exactly one controlled interaction");
    }
    check(starts == 8 && finishes == 8, "every edge and corner must complete one resize");
}

void nativeLogicalDrag() {
    ScreenRecordingAreaWindow area;
    QScreen* primary = QGuiApplication::primaryScreen();
    area.setRecordingRegion(
        QRect(primary->availableGeometry().topLeft() + QPoint(200, 200), QSize(321, 241)));
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.show();
    QCoreApplication::processEvents();
    macActivateApplication();
    for (QScreen* destination : QGuiApplication::screens()) {
        if (destination == primary)
            continue;
        for (QScreen* target : {destination, primary}) {
            const QRect origin = area.recordingRegion();
            QWidget* button =
                area.findChild<QWidget*>(QStringLiteral("screenRecordingRegionDragHandle"));
            const QPoint start = button->mapToGlobal(button->rect().center());
            const QPoint delta = target->availableGeometry().center() - origin.center();
            const int steps = qMax(1, qMax(qAbs(delta.x()), qAbs(delta.y())) / 40);
            MacMouseDrag drag(start);
            for (int step = 1; step <= steps; ++step) {
                const QPoint offset = (QPointF(delta) * step / steps).toPoint();
                drag.moveTo(start + offset);
                if (area.recordingRegion() != origin.translated(offset)) {
                    qWarning() << "Recording drag" << "expected" << origin.translated(offset)
                               << "actual" << area.recordingRegion() << "pointer" << start + offset;
                    throw std::runtime_error("recording drag must preserve the cursor anchor and "
                                             "logical size across displays");
                }
            }
            drag.finish();
            require(area.recordingRegion() == origin.translated(delta) && area.screen() == target,
                    "recording region must remain on the destination display after release");
        }
    }
}

void logicalRegionDragAndResize() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(-231, -119, 321, 241));
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.show();
    QCoreApplication::processEvents();
    const QRect initial = area.recordingRegion();
    require(initial == QRect(-231, -119, 321, 241),
            "odd logical region must survive native layout unchanged");
    const auto send = [&area](QEvent::Type type, QPoint global, Qt::MouseButton button,
                              Qt::MouseButtons buttons, QWidget* target = nullptr) {
        if (target == nullptr)
            target = &area;
        const QPoint local = target->mapFromGlobal(global);
        QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &event);
        QCoreApplication::processEvents();
    };
    // A retained drawing-tool cursor must not mask the host's region cursor.
    area.canvas()->setCursorForLayer(SnowCanvasCursorLayer::CanvasTool, QCursor(Qt::CrossCursor));
    const auto hover = [&](QPoint global, Qt::CursorShape expected) {
        const QPoint local = area.canvas()->mapFromGlobal(global);
        QMouseEvent event(QEvent::MouseMove, local, local, global, Qt::NoButton, Qt::NoButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(area.canvas(), &event);
        require(area.cursor().shape() == expected && area.canvas()->cursor().shape() == expected &&
                    area.windowHandle()->cursor().shape() == expected,
                "region hover must own the widget, canvas and native window cursors");
    };
    hover(initial.topLeft(), Qt::SizeFDiagCursor);
    hover(QPoint(initial.left(), initial.center().y()), Qt::SizeHorCursor);
    QWidget* moveButton =
        area.findChild<QWidget*>(QStringLiteral("screenRecordingRegionDragHandle"));
    const QPoint center = moveButton->mapToGlobal(moveButton->rect().center());
    send(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton, moveButton);
    send(QEvent::MouseMove, center + QPoint(43, 27), Qt::NoButton, Qt::LeftButton, moveButton);
    send(QEvent::MouseButtonRelease, center + QPoint(43, 27), Qt::LeftButton, Qt::NoButton,
         moveButton);
    require(area.recordingRegion() == initial.translated(43, 27),
            "drag deltas must be logical pixels");
    const QRect moved = area.recordingRegion();
    const QPoint corner = moved.bottomRight();
    send(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, corner + QPoint(19, 13), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, corner + QPoint(19, 13), Qt::LeftButton, Qt::NoButton);
    require(area.recordingRegion() == QRect(moved.topLeft(), moved.size() + QSize(19, 13)),
            "corner resize must preserve the logical origin and exact dimensions");
    QMouseEvent interiorHover(QEvent::MouseMove, area.canvas()->rect().center(),
                              area.canvas()->mapToGlobal(area.canvas()->rect().center()),
                              Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(area.canvas(), &interiorHover);
    require(area.cursor().shape() == Qt::ArrowCursor,
            "the recording interior must not advertise movement");
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    require(area.canvas()->cursor() ==
                snow_shot::presentation::crosshairCursor(area.canvas()->devicePixelRatioF()),
            "leaving region editing must release the host cursor to the drawing tool");
}
#endif

#ifdef Q_OS_MACOS
void recordingSafeAreaChangesPreserveCaptureGeometry() {
    const QRect display = QGuiApplication::primaryScreen()->geometry();
    for (const QRect region :
         {display, QRect(display.topLeft() - QPoint(20, 10), QSize(321, 181))}) {
        for (const bool trimming : {false, true}) {
            ScreenRecordingAreaWindow area;
            area.setRecordingRegion(region);
            area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
            area.show();
            QCoreApplication::processEvents();
            area.setTrimming(trimming);
            const QRect geometry = area.geometry();
            const QRect canvasGeometry = area.canvasGeometry();
            const QTransform camera = area.canvas()->canvasToViewTransform();
            const QRegion inputRegion = ScreenRecordingAreaWindowTestAccess::inputRegion(area);
            int regionChanges = 0;
            QObject::connect(&area, &ScreenRecordingAreaWindow::recordingRegionChanged, &area,
                             [&](const QRect&) { ++regionChanges; });
            safe_area_tests::exercise(*area.windowHandle(), [&](QMargins margins) {
                require(area.windowHandle()->safeAreaMargins() == margins,
                        "recording fixture must expose logical safe-area margins");
                require(area.geometry() == geometry && area.canvasGeometry() == canvasGeometry &&
                            area.canvas()->geometry() == canvasGeometry,
                        "safe-area changes must retain recording canvas and border alignment");
                require(area.recordingRegion() == region && regionChanges == 0,
                        "safe-area changes must not crop or move the recording region");
                require(area.canvas()->canvasToViewTransform() == camera &&
                            ScreenRecordingAreaWindowTestAccess::inputRegion(area) == inputRegion,
                        "safe-area changes must preserve annotation and input coordinates");
            });
        }
    }
}
#endif

void geometryAndTransparentCanvasFollowThePhysicalSelection() {
    ScreenRecordingAreaWindow area;
    const QRect physical = testRecordingRegion();
    area.setRecordingRegion(physical);
    area.show();
    QCoreApplication::processEvents();

#ifdef Q_OS_MACOS
    const auto expected =
        snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(QRectF(physical), 1.0);
#else
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(physical);
    const QRectF logical = ScreenshotGeometryMapper::logicalRectFForPhysicalRect(physical, screen);
    const auto expected = snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(
        logical, screen != nullptr ? screen->devicePixelRatio() : 1.0);
#endif
    require(area.geometry() == expected.windowGeometry &&
                area.canvasGeometry() == expected.selectionRect.toAlignedRect(),
            "recording canvas should exactly cover the logical capture selection");
    require(area.canvas() != nullptr && !area.canvas()->clearBackgroundEnabled() &&
                !area.canvas()->wheelZoomEnabled() && area.canvas()->canvasContentVisible(),
            "recording canvas should render transparently without viewport wheel transforms");
    require(!imageHasVisiblePixel(renderWidget(*area.canvas())),
            "an empty recording canvas should remain fully transparent");
}

void inputModesOwnOnlyDrawingInputAndRestoreRequestedState() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    QCoreApplication::processEvents();
    SnowCanvasWidget* canvas = area.canvas();
    require(canvas != nullptr &&
                area.inputMode() == ScreenRecordingAreaWindow::InputMode::PassThrough &&
                area.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !canvas->interactionEnabled(),
            "pass-through must disable both movement and resizing");

    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    require(!area.testAttribute(Qt::WA_TransparentForMouseEvents) && canvas->interactionEnabled(),
            "drawing mode should enable canvas input without replacing the window");
    const WId nativeId = area.winId();
    area.setDrawingBlocked(true);
    require(area.inputMode() == ScreenRecordingAreaWindow::InputMode::Drawing &&
                area.drawingBlocked() && area.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !canvas->interactionEnabled(),
            "busy transitions should block input while retaining the requested drawing mode");
    area.setDrawingBlocked(false);
    require(area.winId() == nativeId && canvas->interactionEnabled() &&
                !area.testAttribute(Qt::WA_TransparentForMouseEvents),
            "leaving a busy transition should restore drawing without recreating the window");

    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    require(area.winId() == nativeId && area.testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !canvas->interactionEnabled(),
            "deactivation must release both movement and resize input");
    area.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
    require(area.testAttribute(Qt::WA_TransparentForMouseEvents),
            "recording pass-through should release the whole window's input");
}

void drawingSurfaceFollowsEffectiveInputMode() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();

    const auto requireSurface = [&](bool drawing) {
        QCoreApplication::processEvents();
        const QImage image = renderWidget(area);
        const QRect interior = area.canvasGeometry().adjusted(8, 8, -8, -8);
        for (int y = interior.top(); y <= interior.bottom(); ++y) {
            for (int x = interior.left(); x <= interior.right(); ++x) {
                const int alpha = image.pixelColor(x, y).alpha();
                if (drawing ? alpha <= 0 || alpha > 2 : alpha != 0) {
                    std::cerr << "drawing=" << drawing << " pixel=" << x << ',' << y
                              << " alpha=" << alpha << '\n';
                }
                require(drawing ? alpha > 0 && alpha <= 2 : alpha == 0,
                        "empty drawing pixels must receive layered-window hits only while drawing");
            }
        }
        // The window origin can lie on the visible border. Sample the physical
        // pixel immediately outside the capture instead, inside its transparent gap.
        const qreal scale = area.devicePixelRatioF();
        const QImage physicalImage = renderWidget(area, scale);
        const QPoint padding(qRound(area.selectionRect().left() * scale) - 1,
                             qRound(area.selectionRect().center().y() * scale));
        require(physicalImage.rect().contains(padding),
                "the recording frame must reserve a pixel outside its capture");
        require(physicalImage.pixelColor(padding).alpha() == 0,
                "drawing input must not fill the padding outside the recording region");
    };

    requireSurface(false);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    for (const auto state : {ScreenshotToolPalette::RecordingState::Idle,
                             ScreenshotToolPalette::RecordingState::Recording,
                             ScreenshotToolPalette::RecordingState::Paused}) {
        area.setRecordingState(state);
        requireSurface(true);
        area.setDrawingBlocked(true);
        requireSurface(false);
        area.setDrawingBlocked(false);
        requireSurface(true);
    }
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    requireSurface(false);
}

void wheelAndEscapeRespectDrawingOwnership() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    SnowCanvasWidget* canvas = area.canvas();
    require(canvas != nullptr, "recording area should own a canvas");

    int wheelRequests = 0;
    int wheelDirection = 0;
    int deactivationRequests = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::drawingWheelRequested, &area,
                     [&](int direction) {
                         ++wheelRequests;
                         wheelDirection = direction;
                     });
    QObject::connect(&area, &ScreenRecordingAreaWindow::drawingDeactivationRequested, &area,
                     [&]() { ++deactivationRequests; });

    QWheelEvent wheel(QPointF(20, 20), QPointF(20, 20), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &wheel);
    require(wheel.isAccepted() && wheelRequests == 1 && wheelDirection == 1,
            "drawing mode should consume the wheel and request a positive tool adjustment");

    sendMouseEvent(*canvas, QEvent::MouseButtonPress, QPointF(16, 16), Qt::LeftButton,
                   Qt::LeftButton);
    PhysicalKeyEvent cancelGesture(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &cancelGesture);
    require(deactivationRequests == 0,
            "the first Escape should cancel an in-progress gesture before deactivating drawing");
    PhysicalKeyEvent deactivate(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &deactivate);
    require(deactivationRequests == 1 && deactivate.isAccepted(),
            "Escape should request pass-through after transient canvas work is canceled");

    require(canvas->setCanvasTool(SnowCanvasTool::Text), "activate recording text tool");
    sendMouseEvent(*canvas, QEvent::MouseButtonPress, QPointF(40, 40), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(*canvas, QEvent::MouseButtonRelease, QPointF(40, 40), Qt::LeftButton,
                   Qt::NoButton);
    PhysicalKeyEvent insert(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
                            QStringLiteral("Preserved text"));
    QCoreApplication::sendEvent(canvas, &insert);
    require(canvas->hasActiveTextEditing(), "recording text draft should be active");
    PhysicalKeyEvent preserveText(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &preserveText);
    require(preserveText.isAccepted() && !canvas->hasActiveTextEditing() &&
                !canvas->testAttribute(Qt::WA_InputMethodEnabled) && deactivationRequests == 1 &&
                canvas->canvasHistoryState().canUndo,
            "Escape must commit recording text and end editing while preserving drawing mode");
    const QByteArray committedHistory = ScreenRecordingAreaWindowTestAccess::history(area);
    require(committedHistory.contains("Preserved text"), "Escape must retain recording text");
    PhysicalKeyEvent releaseText(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &releaseText);
    require(ScreenRecordingAreaWindowTestAccess::history(area) == committedHistory &&
                deactivationRequests == 1,
            "releasing Escape must preserve committed text and drawing mode");

    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    QWheelEvent passThroughWheel(QPointF(20, 20), QPointF(20, 20), QPoint(), QPoint(0, -120),
                                 Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &passThroughWheel);
    require(wheelRequests == 1,
            "pass-through mode should not claim wheel input for drawing adjustments");
}

void annotationsPersistAcrossStatesAndClearOnlyForANewRegion() {
    ScreenRecordingAreaWindow area;
    const QRect firstRegion = testRecordingRegion();
    area.setRecordingRegion(firstRegion);
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    SnowCanvasWidget* canvas = area.canvas();
    drawRectangle(*canvas, QPointF(20, 20), QPointF(70, 55));
    require(canvas->canvasHistoryState().canUndo && imageHasVisiblePixel(renderWidget(*canvas)),
            "drawing while idle should create visible recording annotations");

    for (const auto state : {ScreenshotToolPalette::RecordingState::Recording,
                             ScreenshotToolPalette::RecordingState::Paused,
                             ScreenshotToolPalette::RecordingState::Idle}) {
        area.setRecordingState(state);
        QCoreApplication::processEvents();
        require(canvas->canvasHistoryState().canUndo,
                "annotations should survive recording, pause, resume, stop, and idle states");
    }
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
    require(canvas->canvasHistoryState().canUndo && imageHasVisiblePixel(renderWidget(*canvas)),
            "completed annotations should remain visible while input passes through");

    area.setRecordingRegion(firstRegion);
    require(canvas->canvasHistoryState().canUndo,
            "reapplying the same recording region should preserve annotations");
    const QRect secondRegion = firstRegion.translated(8, 6);
    area.setRecordingRegion(secondRegion);
    QCoreApplication::processEvents();
    require(!canvas->canvasHistoryState().canUndo,
            "opening a different recording region should clear the previous annotations once");
}

void geometryEditingIsOneCapability() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    for (const auto mode : {ScreenRecordingAreaWindow::InputMode::PassThrough,
                            ScreenRecordingAreaWindow::InputMode::Drawing,
                            ScreenRecordingAreaWindow::InputMode::RegionEditing}) {
        for (const auto state : {ScreenshotToolPalette::RecordingState::Idle,
                                 ScreenshotToolPalette::RecordingState::Recording,
                                 ScreenshotToolPalette::RecordingState::Paused}) {
            for (const bool busy : {false, true}) {
                area.setInputMode(mode);
                area.setRecordingState(state);
                area.setDrawingBlocked(busy);
                const bool editable = mode == ScreenRecordingAreaWindow::InputMode::RegionEditing &&
                                      state == ScreenshotToolPalette::RecordingState::Idle && !busy;
                require(ScreenRecordingAreaWindowTestAccess::editable(area) == editable,
                        "moving and resizing must share Export Settings idle eligibility");
                require((ScreenRecordingAreaWindowTestAccess::edges(area, {1, 1}) != Qt::Edges()) ==
                            editable,
                        "resize hit targets must never outlive movement eligibility");
                QWidget* button =
                    area.findChild<QWidget*>(QStringLiteral("screenRecordingRegionDragHandle"));
                require((button && button->isVisible()) == editable,
                        "region controls must share Export Settings idle eligibility");
            }
        }
    }
}

void childDragHandleOwnsCaptureAndPreservesAnnotations() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    drawRectangle(*area.canvas(), {20, 20}, {70, 55});
    const QByteArray history = ScreenRecordingAreaWindowTestAccess::history(area);
    const auto windows = QApplication::topLevelWidgets();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    QCoreApplication::processEvents();
    auto* button = area.findChild<RecordingRegionDragHandle*>(
        QStringLiteral("screenRecordingRegionDragHandle"));
    require(button && button->isVisible(), "export settings must show the region move button");
    require(button->parentWidget() == &area && !button->isWindow() && button->window() == &area &&
                QApplication::topLevelWidgets().size() == windows.size(),
            "the recording move button must be a child widget without an auxiliary window");
    require(button->size() == QSize(32, 32) && button->iconSize() == QSize(18, 18) &&
                button->cursor().shape() == Qt::SizeAllCursor &&
                button->accessibleName() == QStringLiteral("Move screen recording area"),
            "the move control must use the pinned window's size, icon and drag affordance");
    RegionMoveTranslator translator;
    require(QCoreApplication::installTranslator(&translator),
            "the recording move translation fixture must install");
    QCoreApplication::processEvents();
    require(button->toolTip() == QStringLiteral("Translated recording move") &&
                button->accessibleName() == button->toolTip(),
            "the recording move control must retranslate its tooltip and accessible name");
    QCoreApplication::removeTranslator(&translator);
    QCoreApplication::processEvents();
    int closeRequests = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::closeRequested, [&] { ++closeRequests; });
    QCloseEvent close;
    QCoreApplication::sendEvent(&area, &close);
    require(closeRequests == 1 && !close.isAccepted() && area.isVisible(),
            "closing the recording region must request closure through its controller");
    require(area.selectionRect().contains(QRectF(button->geometry())) &&
                button->geometry().right() == area.selectionRect().toRect().right() - 8 &&
                button->geometry().bottom() == area.selectionRect().toRect().bottom() - 8,
            "the move button must sit inside the recording region's bottom-right corner");
    const auto requireInputRegion = [&] {
        const QRegion input = ScreenRecordingAreaWindowTestAccess::inputRegion(area);
        for (int y = 0; y < area.height(); ++y) {
            for (int x = 0; x < area.width(); ++x) {
                const QPoint point(x, y);
                const bool target = ScreenRecordingAreaWindowTestAccess::edges(area, point) ||
                                    button->geometry().contains(point);
                require(input.contains(point) == target,
                        "only resize edges and the move button may own region input");
            }
        }
        require(input.contains(button->geometry().center()) && area.mask().isEmpty(),
                "the move button must receive input without clipping recording annotations");
    };
    requireInputRegion();
    require(area.inputSurfaceColor().alpha() == 0 &&
                imageHasVisiblePixel(renderWidget(*area.canvas())),
            "export settings must release the interior without clipping annotations");
    const QPoint interior = area.selectionRect().center().toPoint();
    require(!ScreenRecordingAreaWindowTestAccess::inputRegion(area).contains(interior) &&
                renderWidget(area).pixelColor(interior).alpha() == 0,
            "the recording interior must remain transparent outside annotations and controls");
    int starts = 0, finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted,
                     [&] { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished,
                     [&] { ++finishes; });
    const auto send = [](QWidget& target, QEvent::Type type, QPoint global, Qt::MouseButton changed,
                         Qt::MouseButtons held) {
        const QPoint local = target.mapFromGlobal(global);
        QMouseEvent event(type, local, local, global, changed, held, Qt::NoModifier);
        QCoreApplication::sendEvent(&target, &event);
    };
    const QRect original = area.recordingRegion();
    const QPoint center = area.mapToGlobal(interior);
    send(area, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    send(area, QEvent::MouseMove, center + QPoint(24, 16), Qt::NoButton, Qt::LeftButton);
    send(area, QEvent::MouseButtonRelease, center + QPoint(24, 16), Qt::LeftButton, Qt::NoButton);
    require(starts == 0 && area.recordingRegion() == original,
            "interior clicks and drags must not start movement");
    const QPoint origin = button->mapToGlobal(button->rect().center());
    send(*button, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    require(ScreenRecordingAreaWindowTestAccess::dragging(area) && button->isDown() &&
                QWidget::mouseGrabber() == button,
            "pressing the child drag handle must capture mouse input in that widget");
    send(*button, QEvent::MouseMove, origin + QPoint(24, 16), Qt::NoButton, Qt::LeftButton);
#ifdef Q_OS_MACOS
    const QPoint delta(24, 16);
#else
    const QPoint delta = (QPointF(24, 16) * area.devicePixelRatioF()).toPoint();
#endif
    require(area.recordingRegion() == original.translated(delta),
            "button dragging must preserve the region's size and pointer anchor");
    send(*button, QEvent::MouseButtonRelease, origin + QPoint(24, 16), Qt::LeftButton,
         Qt::NoButton);
    require(!ScreenRecordingAreaWindowTestAccess::dragging(area) && !button->isDown() &&
                starts == 1 && finishes == 1 &&
                ScreenRecordingAreaWindowTestAccess::history(area) == history,
            "releasing outside the button must commit once and preserve annotations");
    require(area.selectionRect().contains(QRectF(button->geometry())) && button->window() == &area,
            "the child move button must stay inside the recording region during drag");
    const QPoint nextOrigin = button->mapToGlobal(button->rect().center());
    send(*button, QEvent::MouseButtonPress, nextOrigin, Qt::LeftButton, Qt::LeftButton);
    send(*button, QEvent::MouseMove, nextOrigin + QPoint(24, 16), Qt::NoButton, Qt::LeftButton);
    area.setDrawingBlocked(true);
    require(!button->isVisible() && !ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                starts == 2 && finishes == 2 &&
                area.recordingRegion() == original.translated(delta),
            "blocking input during a button drag must roll back and hide controls once");
    area.setDrawingBlocked(false);
    require(button->isVisible(), "clearing a busy state must restore region controls");
    area.setRecordingRegion(QRect(original.topLeft(), QSize(10, 10)));
    requireInputRegion();
    require(button->isVisible() && button->width() > 0 &&
                area.selectionRect().contains(QRectF(button->geometry())),
            "minimum-size recording regions must keep their move button inside the region");
    area.setRecordingRegion(testRecordingRegion());
    area.move(area.x(), area.screen()->availableGeometry().bottom() - area.height() + 1);
    QCoreApplication::processEvents();
    require(
        area.screen()->availableGeometry().contains(
            QRect(button->mapToGlobal(QPoint()), button->size())),
        "the move button must stay reachable when the recording region touches the screen bottom");
    requireInputRegion();
#ifdef Q_OS_MACOS
    const QRect fullScreen = area.screen()->geometry();
#else
    const QRect fullScreen = ScreenshotGeometryMapper::physicalRectForScreen(*area.screen());
#endif
    area.setRecordingRegion(fullScreen);
    QCoreApplication::processEvents();
    require(
        area.screen()->geometry().contains(QRect(button->mapToGlobal(QPoint()), button->size())),
        "full-screen regions must keep the whole child move control inside the display");
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    require(!button->isVisible() && area.canvas()->interactionEnabled(),
            "drawing mode must hide region controls and restore the canvas");
}

void interactionBoundariesAreIdempotentAndCancelOnStateChanges() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    int starts = 0;
    int finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted, &area,
                     [&]() { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished, &area,
                     [&]() { ++finishes; });
    ScreenRecordingAreaWindowTestAccess::begin(area);
    ScreenRecordingAreaWindowTestAccess::begin(area);
    require(starts == 1 && finishes == 0, "drag entry must emit one start");
    area.setDrawingBlocked(true);
    require(finishes == 1, "busy transition must end an active operation");
    ScreenRecordingAreaWindowTestAccess::begin(area);
    require(starts == 1, "blocked input must not start an operation");
    area.setDrawingBlocked(false);
    ScreenRecordingAreaWindowTestAccess::begin(area);
    area.hide();
    ScreenRecordingAreaWindowTestAccess::finish(area);
    require(starts == 2 && finishes == 2,
            "hide and duplicate drag completion must finish only once");
    area.show();
    area.setRecordingRegion({40, 40, 2, 2});
    require(ScreenRecordingAreaWindowTestAccess::edges(
                area, QRectF(area.canvasGeometry()).center()) == Qt::Edges(),
            "even a minimum-size region must retain an interior drag target");
}

void dragHandleUsesWidgetEventsAndCancelsOnInterruption() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    auto* handle = area.findChild<RecordingRegionDragHandle*>(
        QStringLiteral("screenRecordingRegionDragHandle"));
    require(handle != nullptr, "region editing must provide a drag handle widget");
    const QRect original = area.recordingRegion();
    int starts = 0, finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted,
                     [&] { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished,
                     [&] { ++finishes; });
    const auto send = [](QWidget& target, QEvent::Type type, QPoint global, Qt::MouseButton button,
                         Qt::MouseButtons buttons) {
        const QPoint local = target.mapFromGlobal(global);
        QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&target, &event);
    };
    const QPoint origin = handle->mapToGlobal(handle->rect().center());
    send(*handle, QEvent::MouseButtonPress, origin, Qt::RightButton, Qt::RightButton);
    require(!handle->isDragging() && starts == 0, "the drag handle must accept only left presses");
    send(*handle, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    QWidget unrelated;
    send(unrelated, QEvent::MouseMove, origin + QPoint(80, 60), Qt::NoButton, Qt::LeftButton);
    send(unrelated, QEvent::MouseButtonRelease, origin + QPoint(80, 60), Qt::LeftButton,
         Qt::NoButton);
    require(handle->isDragging() && QWidget::mouseGrabber() == handle &&
                area.recordingRegion() == original && finishes == 0,
            "events sent to unrelated widgets must not drive or finish the handle's drag");
    send(*handle, QEvent::MouseMove, origin + QPoint(80, 60), Qt::NoButton, Qt::LeftButton);
    require(area.recordingRegion() != original,
            "the captured handle must continue dragging outside its own bounds");
    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(handle, &ungrab);
    require(!handle->isDragging() && !handle->isDown() && QWidget::mouseGrabber() != handle &&
                area.recordingRegion() == original && starts == 1 && finishes == 1,
            "losing widget capture must roll back and finish the drag once");
    send(*handle, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    send(*handle, QEvent::MouseMove, origin + QPoint(80, 60), Qt::NoButton, Qt::LeftButton);
    handle->setEnabled(false);
    require(!handle->isDragging() && area.recordingRegion() == original && starts == 2 &&
                finishes == 2 &&
                !ScreenRecordingAreaWindowTestAccess::inputRegion(area).contains(
                    handle->geometry().center()),
            "disabling the handle must cancel its drag and release its input bounds");
    handle->setEnabled(true);
    send(*handle, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    send(*handle, QEvent::MouseMove, origin + QPoint(80, 60), Qt::NoButton, Qt::LeftButton);
    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    PhysicalKeyEvent escapeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&area, &escape);
    QCoreApplication::sendEvent(&area, &escapeRelease);
    require(area.isVisible() && !handle->isDragging() && area.recordingRegion() == original &&
                starts == 3 && finishes == 3,
            "Escape in the recording window must cancel the child drag without closing it");
}

void controlledBordersCrossAndCancelWithoutClearingAnnotations() {
    ScreenRecordingAreaWindow area;
    const QRect initial = testRecordingRegion();
    area.setRecordingRegion(initial);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    area.show();
    QCoreApplication::processEvents();
    drawRectangle(*area.canvas(), {20, 20}, {60, 50});
    const QByteArray history = ScreenRecordingAreaWindowTestAccess::history(area);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    int starts = 0, finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted,
                     [&] { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished,
                     [&] { ++finishes; });
    const QPoint pressed = initial.topLeft() + QPoint(initial.width(), initial.height());
    ScreenRecordingAreaWindowTestAccess::beginDrag(area, pressed, Qt::RightEdge | Qt::BottomEdge);
    ScreenRecordingAreaWindowTestAccess::drag(area, initial.topLeft() - QPoint(80, 60));
    require(area.recordingRegion().width() >= 80 && area.recordingRegion().height() >= 60 &&
                area.recordingRegion().x() < initial.x() &&
                area.recordingRegion().y() < initial.y(),
            "recording corner must cross both fixed boundaries");
    require(ScreenRecordingAreaWindowTestAccess::history(area) == history,
            "crossing borders must retain annotation history");
    ScreenRecordingAreaWindowTestAccess::drag(area, initial.topLeft() - QPoint(1, 1));
#ifdef Q_OS_MACOS
    const int minimum =
        snow_shot::presentation::recording::screenRecordingMinimumExtent(area.devicePixelRatioF());
#else
    const int minimum = 10;
#endif
    require(area.recordingRegion().width() >= minimum && area.recordingRegion().height() >= minimum,
            "recording minimum must hold immediately past the fixed corner");
    ScreenRecordingAreaWindowTestAccess::cancel(area);
    require(area.recordingRegion() == initial && starts == 1 && finishes == 1 &&
                !ScreenRecordingAreaWindowTestAccess::dragging(area),
            "cancellation must restore the starting region and finish once");
    for (const auto reason : {QEvent::UngrabMouse, QEvent::WindowDeactivate, QEvent::Hide}) {
        ScreenRecordingAreaWindowTestAccess::beginDrag(area, pressed, Qt::RightEdge);
        ScreenRecordingAreaWindowTestAccess::drag(area, initial.topLeft() - QPoint(20, 0));
        QEvent interruption(reason);
        QCoreApplication::sendEvent(&area, &interruption);
        require(!ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                    area.recordingRegion() == initial,
                "interruption must roll back a crossed region");
    }
    ScreenRecordingAreaWindowTestAccess::beginDrag(area, pressed, Qt::RightEdge);
    ScreenRecordingAreaWindowTestAccess::drag(area, initial.topLeft() - QPoint(20, 0));
    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    PhysicalKeyEvent escapeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&area, &escape);
    QCoreApplication::sendEvent(&area, &escapeRelease);
    require(area.isVisible() && area.recordingRegion() == initial &&
                !ScreenRecordingAreaWindowTestAccess::dragging(area),
            "Escape must cancel the resize without closing the recording region");
    ScreenRecordingAreaWindowTestAccess::beginDrag(area, pressed, Qt::RightEdge);
    const QPoint end = initial.topLeft() - QPoint(80, 0);
#ifdef Q_OS_MACOS
    const QPointF logical(end);
#else
    const QPointF logical = ScreenshotGeometryMapper::logicalRectFForPhysicalRect(
                                QRect(end, QSize(1, 1)), area.screen())
                                .topLeft();
#endif
    QMouseEvent release(QEvent::MouseButtonRelease, area.mapFromGlobal(logical), logical,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&area, &release);
    require(!ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                area.recordingRegion().x() < initial.x() && starts == finishes,
            "release must apply the final position and commit once");
    require(ScreenRecordingAreaWindowTestAccess::history(area) == history,
            "committing a crossing must preserve annotations");
}

void drawingAcrossFrameEdgesRetainsDrawingOwnership() {
    ScreenRecordingAreaWindow area;
    const QRect initial = testRecordingRegion();
    area.setRecordingRegion(initial);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    area.show();
    QCoreApplication::processEvents();
    drawRectangle(*area.canvas(), {20, 20},
                  {static_cast<qreal>(area.canvas()->width() - 2),
                   static_cast<qreal>(area.canvas()->height() - 2)});
    require(
        area.recordingRegion() == initial && area.canvas()->canvasHistoryState().canUndo,
        "drawing through frame hit regions must complete the annotation without editing geometry");
}

void countdownHelpersFollowTheSecondBoundaries() {
    namespace recording = snow_shot::presentation::recording;
    require(recording::screenRecordingCountdownRemainingSeconds(3000) == 3 &&
                recording::screenRecordingCountdownRemainingSeconds(2999) == 3 &&
                recording::screenRecordingCountdownRemainingSeconds(2001) == 3 &&
                recording::screenRecordingCountdownRemainingSeconds(2000) == 2 &&
                recording::screenRecordingCountdownRemainingSeconds(1000) == 1 &&
                recording::screenRecordingCountdownRemainingSeconds(1) == 1 &&
                recording::screenRecordingCountdownRemainingSeconds(0) == 1,
            "remaining seconds must count whole seconds and hold the final one");
    const auto progress = recording::screenRecordingCountdownProgress;
    require(progress(3000, 3000) == 1.0 && progress(3000, 1500) == 0.5 &&
                progress(3000, 0) == 0.0 && progress(3000, 4000) == 1.0 && progress(0, 0) == 0.0,
            "the progress ring must drain linearly and stay clamped to the full circle");
    const auto entrance = recording::screenRecordingCountdownDigitEntrance;
    require(entrance(3000, 3000) == 0.0 && entrance(3000, 2000) == 0.0 &&
                entrance(3000, 3000 - recording::screenRecordingCountdownDigitPopDurationMs) ==
                    1.0 &&
                entrance(3000, 1001) == 1.0,
            "each digit must pop in at its second boundary and settle within the second");
}

void countdownUsesThePausedBorderAndCentersItsIndicator() {
    namespace recording = snow_shot::presentation::recording;
    ScreenRecordingAreaWindow area;
    const QRect physical = testRecordingRegion();
    area.setRecordingRegion(physical);
    area.show();
    QCoreApplication::processEvents();

#ifdef Q_OS_MACOS
    const QRectF logical(physical);
    const qreal scale = 1.0;
#else
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(physical);
    const QRectF logical = ScreenshotGeometryMapper::logicalRectFForPhysicalRect(physical, screen);
    const qreal scale = screen != nullptr ? screen->devicePixelRatio() : 1.0;
#endif
    const auto frame = recording::screenRecordingAreaFrameGeometry(logical, scale);
    const auto border = recording::screenRecordingAreaBorderGeometry(
        frame.frameRect, frame.selectionRect, frame.paddingWidth);
    const auto borderColor = [&area](const QPointF& position) {
        const QImage image = renderWidget(area);
        return image.pixelColor(qRound(position.x()), qRound(position.y()));
    };
    const QColor kIdle(0x40, 0x96, 0xff);
    const QColor kPaused(0xfa, 0xad, 0x14);
    const QPointF sample = border.top.center();

    require(!area.countdownActive(), "the countdown must start inactive");
    require(borderColor(sample) == kIdle, "an idle area must paint the blue border");
    area.startCountdown(3);
    require(area.countdownActive(), "a started countdown must report active");
    QCoreApplication::processEvents();
    require(borderColor(sample) == kPaused,
            "the countdown must repaint the border in the paused colour");

    auto* overlay = area.findChild<recording::RecordingCountdownOverlay*>();
    require(overlay != nullptr && overlay->isVisible(),
            "the countdown indicator must be a visible child of the recording area");
    require(overlay->remainingSeconds() == 3,
            "the indicator must start by showing the whole delay");
    const QRectF selection = area.selectionRect();
    const QRect geometry = overlay->geometry();
    require(geometry.width() == geometry.height() &&
                geometry.width() == recording::screenRecordingCountdownIndicatorSize,
            "the countdown indicator must be a square of the documented size");
    require(qAbs(QRectF(geometry).center().x() - selection.center().x()) <= 1.0 &&
                qAbs(QRectF(geometry).center().y() - selection.center().y()) <= 1.0,
            "the countdown indicator must center on the recording selection");

    area.setRecordingRegion(physical.translated(24, 16));
    QCoreApplication::processEvents();
    const QRect moved = overlay->geometry();
    require(qAbs(QRectF(moved).center().x() - area.selectionRect().center().x()) <= 1.0,
            "the indicator must follow selection layout changes");

    overlay->setRemainingMilliseconds(2500);
    const QImage midSecond = renderWidget(*overlay);
    const QPoint backdropSample(12, midSecond.height() / 2);
    require(midSecond.pixelColor(backdropSample).alpha() > 150,
            "the backdrop must stay opaque; the indicator must never fade as a whole");
    require(midSecond.pixelColor(3, 3).alpha() == 0 &&
                midSecond.pixelColor(midSecond.width() - 4, 3).alpha() == 0 &&
                midSecond.pixelColor(3, midSecond.height() - 4).alpha() == 0 &&
                midSecond.pixelColor(midSecond.width() - 4, midSecond.height() - 4).alpha() == 0,
            "the indicator backdrop must be circular, leaving the corners transparent");
    const QColor ring = midSecond.pixelColor(midSecond.width() / 2, 5);
    require(ring.red() > 200 && ring.green() > 120 && ring.blue() < 100,
            "a draining accent ring must paint the countdown progress");
    bool digitVisible = false;
    for (int y = 24; y < 64 && !digitVisible; ++y) {
        for (int x = 24; x < 64 && !digitVisible; ++x) {
            digitVisible = midSecond.pixelColor(x, y) == QColor(Qt::white);
        }
    }
    require(digitVisible, "the countdown digit must paint solid white between second boundaries");
    overlay->setRemainingMilliseconds(2000);
    require(renderWidget(*overlay).pixelColor(backdropSample).alpha() > 150,
            "second boundaries must pop only the digit, never the backdrop");

    area.clearCountdown();
    require(!area.countdownActive() && !overlay->isVisible(),
            "clearing must hide the countdown indicator");
    QCoreApplication::processEvents();
    require(borderColor(sample) == kIdle, "clearing must restore the idle border");
}

void quickSelectionFollowsTheDrawingSetting() {
    const snow_shot::storage::DrawingSettings drawingSettings;
    const QStringList originalDisabledTools = drawingSettings.quickSelectionDisabledTools();
    require(drawingSettings.setQuickSelectionDisabledTools({QStringLiteral("free-draw")}),
            "disabling free-draw quick selection should persist");

    // The window is constructed after the setting changed, so the very first
    // click already exercises the initial application of the policy.
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    QCoreApplication::processEvents();
    SnowCanvasWidget* canvas = area.canvas();
    require(canvas != nullptr, "recording area should own a canvas");

    require(canvas->setCanvasTool(SnowCanvasTool::FreeDraw),
            "recording canvas should accept the free-draw tool");
    sendMouseEvent(*canvas, QEvent::MouseButtonPress, QPointF(20, 20), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(*canvas, QEvent::MouseMove, QPointF(70, 20), Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(*canvas, QEvent::MouseMove, QPointF(120, 20), Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(*canvas, QEvent::MouseButtonRelease, QPointF(120, 20), Qt::LeftButton,
                   Qt::NoButton);
    QCoreApplication::processEvents();
    require(canvas->canvasHistoryState().canUndo, "the free-draw stroke should be drawn");

    const auto clickStroke = [&]() {
        require(canvas->resetEditingState() && canvas->setCanvasTool(SnowCanvasTool::FreeDraw),
                "the fixture should restart free-draw creation without a selection");
        require(canvas->canvasStyleToolbarState().source ==
                    SnowCanvasStyleToolbarSource::DefaultFreeDraw,
                "the fixture should start from the creation style state");
        sendMouseEvent(*canvas, QEvent::MouseButtonPress, QPointF(70, 20), Qt::LeftButton,
                       Qt::LeftButton);
        sendMouseEvent(*canvas, QEvent::MouseButtonRelease, QPointF(70, 20), Qt::LeftButton,
                       Qt::NoButton);
        QCoreApplication::processEvents();
    };

    clickStroke();
    require(canvas->canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::DefaultFreeDraw,
            "the recording canvas must not select free-draw elements once disabled");

    // The disabled click creates a dot. Remove it before clicking again: its
    // endpoint would intentionally start stroke continuation before selection.
    require(canvas->undo(), "the disabled-selection dot should be undone");
    require(drawingSettings.setQuickSelectionDisabledTools({}),
            "enabling free-draw quick selection should persist");
    clickStroke();
    require(canvas->canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedFreeDraw,
            "quick selection should select the stroke while the setting keeps it enabled");

    require(drawingSettings.setQuickSelectionDisabledTools({QStringLiteral("free-draw")}),
            "re-disabling free-draw quick selection should persist");
    clickStroke();
    require(canvas->canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::DefaultFreeDraw,
            "the recording canvas must follow live quick-selection changes");

    require(drawingSettings.setQuickSelectionDisabledTools(originalDisabledTools),
            "restoring the quick-selection setting should persist");
}

void ordinaryWindowGeometryPreservesAnnotations() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    drawRectangle(*area.canvas(), {20, 20}, {70, 55});
    const QByteArray history = ScreenRecordingAreaWindowTestAccess::history(area);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    const QRect before = area.recordingRegion();
    int changes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::recordingRegionChanged, &area,
                     [&](const QRect& region) {
                         ++changes;
                         require(region == area.recordingRegion(),
                                 "notification must report committed geometry");
                     });
    // Normal window events, including positions outside the starting screen, are authoritative.
    area.move(-200, -100);
    QCoreApplication::processEvents();
    const QRect moved = area.recordingRegion();
    require(moved.topLeft() != before.topLeft() && moved.size() == before.size() && changes > 0,
            "ordinary movement must synchronize physical coordinates without clamping");
    const QSize originalSize = area.size();
    area.resize(originalSize - QSize(20, 10));
    QCoreApplication::processEvents();
    require(area.recordingRegion().width() < moved.width() &&
                area.recordingRegion().height() < moved.height(),
            "ordinary resize must synchronize physical dimensions");
    area.resize(originalSize);
    QCoreApplication::processEvents();
    require(area.recordingRegion() == moved,
            "shrinking and expanding must restore the same physical rectangle");
    require(ScreenRecordingAreaWindowTestAccess::history(area) == history,
            "window geometry changes must preserve annotation coordinates and history");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
template <typename Ready> bool waitForNativeRegionInput(Ready ready) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (ready())
            return true;
        QThread::msleep(1);
    } while (timer.elapsed() < 1000);
    return false;
}

void pressNativeRegionControl(QWidget& target) {
    const QPoint global = QCursor::pos();
    const QPoint local = target.mapFromGlobal(global);
    QMouseEvent press(QEvent::MouseButtonPress, local, local, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&target, &press);
}

INPUT nativeRegionMovement(QPoint point) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    input.mi.dx = qRound((point.x() - GetSystemMetrics(SM_XVIRTUALSCREEN) + 0.5) * 65536.0 /
                         GetSystemMetrics(SM_CXVIRTUALSCREEN));
    input.mi.dy = qRound((point.y() - GetSystemMetrics(SM_YVIRTUALSCREEN) + 0.5) * 65536.0 /
                         GetSystemMetrics(SM_CYVIRTUALSCREEN));
    return input;
}

void moveNativeRegionPointer(QPoint point) {
    INPUT input = nativeRegionMovement(point);
    require(SendInput(1, &input, sizeof(input)) == 1,
            "native recording input must deliver a pointer move");
    require(waitForNativeRegionInput([&] {
                POINT actual{};
                return GetCursorPos(&actual) && QPoint(actual.x, actual.y) == point;
            }),
            "native recording input must reach the requested physical point");
}

int nativeWebcamGestures() {
    POINT saved{};
    if (!GetCursorPos(&saved) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        qWarning("Skipping native webcam test: the interactive pointer is unavailable or busy");
        return 77;
    }
    const HWND foreground = GetForegroundWindow();
    ScreenRecordingAreaWindow area;
    const QRect region(
        ScreenshotGeometryMapper::physicalRectForScreen(*QGuiApplication::primaryScreen())
                .topLeft() +
            QPoint(100, 100),
        QSize(600, 400));
    area.setRecordingRegion(region);
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.show();
    QCoreApplication::processEvents();
    const qreal scale = area.devicePixelRatioF();
    int size = 150;
    QPoint offset(48, 48);
    int commits = 0;
    const auto present = [&] {
        area.setWebcamOverlay(
            QRectF(area.selectionRect().topLeft() +
                       QPointF(offset.x() / scale, (400 - offset.y() - size) / scale),
                   QSizeF(size / scale, size / scale)),
            QSize(600, 400), size, offset, offset, true);
    };
    QObject::connect(&area, &ScreenRecordingAreaWindow::webcamLayoutPreviewed, &area,
                     [&](int next, const QPoint& position) {
                         size = next;
                         offset = position;
                         present();
                     });
    QObject::connect(&area, &ScreenRecordingAreaWindow::webcamLayoutCommitted, &area,
                     [&](int, const QPoint&) { ++commits; });
    present();
    auto* control = area.findChild<RecordingWebcamControl*>();
    auto* button = area.findChild<QToolButton*>(QStringLiteral("screenRecordingWebcamResize"));
    const HWND hwnd = reinterpret_cast<HWND>(area.winId());
    const auto physicalPoint = [&](QWidget& widget) {
        POINT origin{};
        ClientToScreen(hwnd, &origin);
        const QPointF local = widget.mapTo(&area, QPointF(widget.rect().center()));
        return QPoint(origin.x + qRound(local.x() * scale), origin.y + qRound(local.y() * scale));
    };
    bool pressed = false;
    const auto click = [&](DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        const bool sent = SendInput(1, &input, sizeof(input)) == 1;
        if (sent)
            pressed = flags == MOUSEEVENTF_LEFTDOWN;
        return sent;
    };
    const auto cleanup = qScopeGuard([&] {
        area.cancelWebcamInteraction();
        area.hide();
        if (pressed)
            click(MOUSEEVENTF_LEFTUP);
        SetCursorPos(saved.x, saved.y);
        if (foreground && IsWindow(foreground))
            SetForegroundWindow(foreground);
    });
    const auto pointerAt = [](QPoint point) {
        POINT actual{};
        const bool readable = GetCursorPos(&actual);
        const bool matches = readable && QPoint(actual.x, actual.y) == point;
        if (!matches)
            qInfo() << "Native webcam pointer mismatch" << "expected" << point << "actual"
                    << QPoint(actual.x, actual.y) << "readable" << readable;
        return matches;
    };
    const auto position = [&](QPoint point) {
        INPUT movement = nativeRegionMovement(point);
        if (SendInput(1, &movement, sizeof(movement)) != 1)
            return false;
        const bool positioned = waitForNativeRegionInput([&] {
            POINT actual{};
            return GetCursorPos(&actual) && QPoint(actual.x, actual.y) == point;
        });
        if (!positioned)
            static_cast<void>(pointerAt(point));
        return positioned;
    };
    const auto unavailable = [] {
        qWarning("Skipping native webcam test: pointer injection is unavailable or interrupted");
        return 77;
    };
    const auto waitAt = [&](QPoint point, const auto& ready, const char* message) {
        const bool completed =
            waitForNativeRegionInput([&] { return !pointerAt(point) || ready(); });
        if (!pointerAt(point))
            return unavailable();
        if (!completed) {
            qWarning("%s", message);
            POINT nativeOrigin{};
            ClientToScreen(hwnd, &nativeOrigin);
            const QPointF local =
                QPointF(point - QPoint(nativeOrigin.x, nativeOrigin.y)) / area.devicePixelRatioF();
            qInfo() << "Native webcam routing" << "point" << point << "local" << local
                    << "initial DPR" << scale << "current DPR" << area.devicePixelRatioF()
                    << "window" << area.geometry() << "region" << area.recordingRegion()
                    << "selection" << area.selectionRect() << "control" << control->geometry()
                    << "visible" << control->isVisible() << "input contains"
                    << ScreenRecordingAreaWindowTestAccess::inputRegion(area).contains(
                           local.toPoint())
                    << "observer" << ScreenRecordingAreaWindowTestAccess::observing(area)
                    << "transparent"
                    << bool(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT);
            return 1;
        }
        return 0;
    };
    for (bool resize : {false, true}) {
        const QPoint origin = physicalPoint(resize ? static_cast<QWidget&>(*button) : *control);
        if (!position(origin))
            return unavailable();
        int result = waitAt(
            origin, [&] { return !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT); },
            "webcam hover must enable the native input surface");
        if (result)
            return result;
        if (!click(MOUSEEVENTF_LEFTDOWN))
            return unavailable();
        result = waitAt(
            origin, [&] { return control->interacting() && GetCapture() == hwnd; },
            "native webcam press must capture the pointer");
        if (result)
            return result;
        const QPoint destination = origin + QPoint(40, -40);
        if (!position(destination))
            return unavailable();
        result = waitAt(
            destination, [&] { return resize ? size == 190 : offset == QPoint(88, 88); },
            "native webcam drag must preserve physical-pixel geometry");
        if (result)
            return result;
        if (!click(MOUSEEVENTF_LEFTUP))
            return unavailable();
        result = waitAt(
            destination, [&] { return !control->interacting(); },
            "native release finishes webcam editing");
        if (result)
            return result;
    }
    if (commits != 2 || area.recordingRegion() != region) {
        qWarning("Native webcam edits must commit without moving the recording region");
        return 1;
    }
    return 0;
}

void nativeRecordingBordersCross() {
    POINT saved{};
    require(GetCursorPos(&saved) && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000),
            "native recording input requires an available interactive pointer");
    const HWND foreground = GetForegroundWindow();
    ScreenRecordingAreaWindow* active = nullptr;
    bool pressed = false;
    const auto sendButton = [&](DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        const bool sent = SendInput(1, &input, sizeof(input)) == 1;
        if (sent)
            pressed = flags == MOUSEEVENTF_LEFTDOWN;
        return sent;
    };
    const auto cleanup = [&] {
        if (active) {
            ScreenRecordingAreaWindowTestAccess::cancel(*active);
            active->hide();
        }
        if (pressed)
            sendButton(MOUSEEVENTF_LEFTUP);
        SetCursorPos(saved.x, saved.y);
        if (foreground && IsWindow(foreground))
            SetForegroundWindow(foreground);
    };
    const auto cleanupOnExit = qScopeGuard(cleanup);
    const auto check = [&](bool condition, const char* message) {
        if (!condition)
            cleanup();
        require(condition, message);
    };
    const auto setPointer = [&](const QPoint& position) {
        INPUT input = nativeRegionMovement(position);
        check(SendInput(1, &input, sizeof(input)) == 1,
              "native recording input must deliver a pointer move");
        check(waitForNativeRegionInput([&] {
                  POINT actual{};
                  return GetCursorPos(&actual) && QPoint(actual.x, actual.y) == position;
              }),
              "native recording test could not position the physical pointer");
    };
    const auto matchesNativeRegion = [](ScreenRecordingAreaWindow& area, const QRect& expected) {
        const HWND hwnd = reinterpret_cast<HWND>(area.winId());
        RECT client{};
        POINT origin{};
        return GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &origin) &&
               area.recordingRegion() == expected &&
               QRect(origin.x, origin.y, client.right - client.left, client.bottom - client.top)
                       .marginsRemoved(ScreenRecordingAreaWindowTestAccess::insets(area)) ==
                   expected;
    };
    for (QScreen* display : QGuiApplication::screens()) {
        ScreenRecordingAreaWindow area;
        active = &area;
        const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*display);
        const QRect original(bounds.topLeft() + QPoint(250, 200), QSize(240, 120));
        area.setRecordingRegion(original);
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
        area.show();
        const HWND hwnd = reinterpret_cast<HWND>(area.winId());
        check(waitForNativeRegionInput(
                  [&] { return IsWindowVisible(hwnd) && matchesNativeRegion(area, original); }),
              "native recording fixture must publish its initial physical region");
        const QPoint start = original.topLeft() + QPoint(original.width(), original.height());
        const auto pressBorder = [&] {
            setPointer(start);
            check(waitForNativeRegionInput([&] {
                      return !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) &&
                             WindowFromPoint(POINT{start.x(), start.y()}) == hwnd;
                  }),
                  "native recording border must receive the next pointer press");
            check(sendButton(MOUSEEVENTF_LEFTDOWN),
                  "native recording input must deliver a border press");
            check(waitForNativeRegionInput([&] {
                      return ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                             GetCapture() == hwnd;
                  }),
                  "native recording border must own mouse capture");
        };
        pressBorder();
        const QPoint crossed = original.topLeft() - QPoint(60, 30);
        setPointer(crossed);
        const bool crossedExactly = waitForNativeRegionInput(
            [&] { return matchesNativeRegion(area, QRect(crossed, QSize(60, 30))); });
        if (area.recordingRegion() != QRect(crossed, QSize(60, 30))) {
            POINT observed{};
            GetCursorPos(&observed);
            qInfo() << "Crossing" << original << "start" << start << "end" << crossed << "pointer"
                    << QPoint(observed.x, observed.y) << "actual" << area.recordingRegion()
                    << "active" << ScreenRecordingAreaWindowTestAccess::dragging(area);
        }
        check(crossedExactly, "native recording crossing must preserve exact physical dimensions");
        for (QScreen* destination : QGuiApplication::screens()) {
            const QPoint end =
                ScreenshotGeometryMapper::physicalRectForScreen(*destination).center();
            const QRect before = area.recordingRegion();
            setPointer(end);
            check(waitForNativeRegionInput([&] {
                      return area.recordingRegion() != before &&
                             area.recordingRegion().width() >= 10 &&
                             area.recordingRegion().height() >= 10 &&
                             ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                             GetCapture() == hwnd &&
                             matchesNativeRegion(area, area.recordingRegion());
                  }),
                  "cross-display recording resize must retain capture and its minimum");
        }
        SendMessageW(hwnd, WM_CANCELMODE, 0, 0);
        check(sendButton(MOUSEEVENTF_LEFTUP),
              "native recording input must release the cancelled drag");
        check(waitForNativeRegionInput([&] {
                  return matchesNativeRegion(area, original) &&
                         !ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                         !(GetAsyncKeyState(VK_LBUTTON) & 0x8000);
              }),
              "native recording cancellation must restore exact geometry");
        const QPoint end = original.topLeft() - QPoint(1, 1);
        pressBorder();
        setPointer(end);
        check(sendButton(MOUSEEVENTF_LEFTUP),
              "native recording input must deliver the final border release");
        const bool releasedMinimum = waitForNativeRegionInput([&] {
            return matchesNativeRegion(area,
                                       QRect(original.topLeft() - QPoint(10, 10), QSize(10, 10))) &&
                   !ScreenRecordingAreaWindowTestAccess::dragging(area);
        });
        if (area.recordingRegion() != QRect(original.topLeft() - QPoint(10, 10), QSize(10, 10)))
            qInfo() << "Native minimum release" << "expected"
                    << QRect(original.topLeft() - QPoint(10, 10), QSize(10, 10)) << "actual"
                    << area.recordingRegion() << "pointer" << QCursor::pos() << "input mode"
                    << int(area.inputMode());
        check(releasedMinimum, "native release must commit the crossed minimum-sized region");
        auto* button = area.findChild<QWidget*>(QStringLiteral("screenRecordingRegionDragHandle"));
        check(button && button->isVisible() &&
                  area.selectionRect().contains(QRectF(button->geometry())),
              "minimum-sized regions must keep the child handle inside at the display scale");
        area.hide();
        active = nullptr;
    }
}

void nativeWindowsGeometryAndInteraction() {
    require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "native test requires Windows QPA");
    ScreenRecordingAreaWindow area;
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    QWidget underlying(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    int starts = 0;
    int finishes = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionStarted, &area,
                     [&]() { ++starts; });
    QObject::connect(&area, &ScreenRecordingAreaWindow::regionInteractionFinished, &area,
                     [&]() { ++finishes; });
    std::cout << "Native display count: " << QGuiApplication::screens().size() << '\n';
    for (QScreen* display : QGuiApplication::screens()) {
        const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*display);
        const QRect selected(bounds.topLeft() + QPoint(40, 40), QSize(321, 241));
        area.setRecordingRegion(selected);
        area.show();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        const HWND handle = reinterpret_cast<HWND>(area.winId());
        qInfo() << display->name() << display->devicePixelRatio() << "expected" << selected
                << "observed" << area.recordingRegion() << "logical" << area.geometry();
        require(area.recordingRegion() == selected,
                "opening on each display must preserve exact physical selection");
        const auto verifyClient = [&]() {
            RECT client{};
            POINT origin{};
            require(GetClientRect(handle, &client) && ClientToScreen(handle, &origin),
                    "native client geometry must be readable");
            const QRect clientGeometry(origin.x, origin.y, client.right - client.left,
                                       client.bottom - client.top);
            const QMargins currentInsets = ScreenRecordingAreaWindowTestAccess::insets(area);
            if (area.recordingRegion() != clientGeometry.marginsRemoved(currentInsets)) {
                qInfo() << "Geometry mismatch" << "client" << clientGeometry << "capture"
                        << area.recordingRegion() << "insets" << currentInsets << "logical"
                        << area.geometry();
            }
            require(area.recordingRegion() == clientGeometry.marginsRemoved(currentInsets),
                    "native events must synchronize capture to actual client pixels");
        };
        verifyClient();
        const QRect next(bounds.topLeft() + QPoint(60, 50), QSize(409, 307));
        require(SetWindowPos(handle, nullptr, next.x(), next.y(), next.width(), next.height(),
                             SWP_NOZORDER | SWP_NOACTIVATE) != 0,
                "native resize must succeed");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        verifyClient();
        // Physical changes can round to the same QWidget size at fractional DPI.
        for (const int nativeWidth : {410, 411, 409}) {
            SetWindowPos(handle, nullptr, 0, 0, nativeWidth, next.height(),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            verifyClient();
        }
        const QRect physical = area.recordingRegion();
        const int x = physical.center().x();
        const int y = physical.center().y();
        const auto hit = [&](QPoint position) {
            return SendMessageW(handle, WM_NCHITTEST, 0, MAKELPARAM(position.x(), position.y()));
        };
        require(hit({x, y}) == HTTRANSPARENT && hit(physical.topLeft()) == HTCLIENT,
                "Export Settings must accept native input only over controls and resize edges");
        underlying.setGeometry(area.geometry());
        underlying.show();
        require(SetWindowPos(reinterpret_cast<HWND>(underlying.winId()), handle, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE,
                "the click-through fixture must sit immediately below the recording area");
        POINT saved{};
        GetCursorPos(&saved);
        moveNativeRegionPointer({x, y});
        area.repaint();
        QCoreApplication::processEvents();
        require((GetWindowLongPtrW(handle, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0,
                "recording interior pixels must pass input to other applications");
        require(WindowFromPoint(POINT{x, y}) == reinterpret_cast<HWND>(underlying.winId()),
                "native interior hits must reach the window under the recording region");
        const QPoint targets[] = {{physical.left(), y},  {physical.right(), y},
                                  {x, physical.top()},   {x, physical.bottom()},
                                  physical.topLeft(),    physical.topRight(),
                                  physical.bottomLeft(), physical.bottomRight()};
        for (const QPoint point : targets) {
            moveNativeRegionPointer(point);
            if (WindowFromPoint(POINT{point.x(), point.y()}) != handle) {
                POINT cursor{};
                GetCursorPos(&cursor);
                qInfo() << "Border routing" << point << "cursor" << QPoint(cursor.x, cursor.y)
                        << "hit" << hit(point) << "style" << GetWindowLongPtrW(handle, GWL_EXSTYLE)
                        << "actual" << WindowFromPoint(POINT{point.x(), point.y()}) << "expected"
                        << handle;
            }
            require(WindowFromPoint(POINT{point.x(), point.y()}) == handle,
                    "native border hits must reach the recording region window");
            const int previousStarts = starts;
            const int previousFinishes = finishes;
            const QRect beforeInteraction = area.recordingRegion();
            pressNativeRegionControl(area);
            require(ScreenRecordingAreaWindowTestAccess::dragging(area) && GetCapture() == handle,
                    "the recording border must capture its controlled native resize");
            SendMessageW(handle, WM_CANCELMODE, 0, 0);
            require(starts == previousStarts + 1 && finishes == previousFinishes + 1,
                    "controlled border resizing must have one balanced interaction lifecycle");
            require(area.recordingRegion() == beforeInteraction,
                    "cancelling a no-motion border drag must preserve the exact region");
            verifyClient();
        }
        auto* button = area.findChild<QWidget*>(QStringLiteral("screenRecordingRegionDragHandle"));
        require(button && !button->isWindow() && button->parentWidget() == &area,
                "the native move handle must remain a child widget");
        POINT controlOrigin{};
        require(ClientToScreen(handle, &controlOrigin), "the native control origin must exist");
        const auto physicalPoint = [&](QPoint local) {
            return QPoint(controlOrigin.x, controlOrigin.y) +
                   (QPointF(local) * area.devicePixelRatioF()).toPoint();
        };
        const QPoint buttonPoint = physicalPoint(button->geometry().center());
        moveNativeRegionPointer(buttonPoint);
        require(WindowFromPoint(POINT{buttonPoint.x(), buttonPoint.y()}) == handle,
                "the child move button must receive native input through the recording window");
        const int buttonStarts = starts;
        const int buttonFinishes = finishes;
        INPUT click{};
        click.type = INPUT_MOUSE;
        click.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        moveNativeRegionPointer({x, y});
        INPUT moveAndPress[] = {nativeRegionMovement(buttonPoint), click};
        require(SendInput(2, moveAndPress, sizeof(INPUT)) == 2,
                "a move and immediate handle press must be delivered in one input batch");
        const bool buttonCaptured = waitForNativeRegionInput([&] {
            return ScreenRecordingAreaWindowTestAccess::dragging(area) &&
                   QWidget::mouseGrabber() == button && GetCapture() == handle;
        });
        click.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        require(SendInput(1, &click, sizeof(click)) == 1,
                "the native handle release must be delivered");
        const bool buttonReleased = waitForNativeRegionInput(
            [&] { return !ScreenRecordingAreaWindowTestAccess::dragging(area); });
        require(buttonCaptured && buttonReleased && starts == buttonStarts + 1 &&
                    finishes == buttonFinishes + 1 && area.recordingRegion() == physical,
                "native clicks must reach the child move button and complete one interaction");
        SetCursorPos(saved.x, saved.y);
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
        require(hit(physical.topLeft()) == HTCLIENT && hit({x, y}) == HTCLIENT,
                "drawing must disable both native resize and move targets");
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
        require(hit(physical.topLeft()) == HTTRANSPARENT && hit({x, y}) == HTTRANSPARENT,
                "pass-through must release both native resize and move targets");
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
        drawRectangle(*area.canvas(), {20, 20}, {70, 55});
        const QByteArray history = ScreenRecordingAreaWindowTestAccess::history(area);
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
        const QImage annotations = renderWidget(*area.canvas());
        QPoint annotationLocal;
        bool foundAnnotation = false;
        for (int row = 0; row < annotations.height() && !foundAnnotation; ++row) {
            for (int column = 0; column < annotations.width(); ++column) {
                if (annotations.pixelColor(column, row).alpha() > 0) {
                    annotationLocal = QPoint(column, row);
                    foundAnnotation = true;
                    break;
                }
            }
        }
        require(foundAnnotation, "the native click-through test must cover a visible annotation");
        const QPoint annotationPoint = physicalPoint(area.canvas()->mapTo(&area, annotationLocal));
        moveNativeRegionPointer(annotationPoint);
        require(WindowFromPoint(POINT{annotationPoint.x(), annotationPoint.y()}) ==
                        reinterpret_cast<HWND>(underlying.winId()) &&
                    ScreenRecordingAreaWindowTestAccess::history(area) == history,
                "visible annotations must preserve their pixels and pass interior input through");
        SetCursorPos(saved.x, saved.y);
        for (QScreen* destination : QGuiApplication::screens()) {
            if (destination == display) {
                continue;
            }
            const QRect destinationBounds =
                ScreenshotGeometryMapper::physicalRectForScreen(*destination);
            SetWindowPos(handle, nullptr, destinationBounds.x() + 60, destinationBounds.y() + 60, 0,
                         0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            verifyClient();
            require(area.screen() == destination &&
                        area.recordingRegion().intersects(destinationBounds),
                    "an existing native window must move onto another display without clamping");
            require(ScreenRecordingAreaWindowTestAccess::history(area) == history,
                    "native display transitions must preserve annotations");
        }
    }
}
#endif

} // namespace

void trimmingFreezesSizeAndMakesThePreviewDraggable() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(QRect(100, 100, 320, 240));
    area.setTrimming(true);
    area.show();
    QCoreApplication::processEvents();
    auto* handle = area.findChild<QWidget*>(QStringLiteral("screenRecordingRegionDragHandle"));
    require(handle == nullptr || !handle->isVisible(),
            "trim preview must not show the region drag handle");
    const QSize original = area.recordingRegion().size();
    const QSize windowSize = area.size();
    area.resize(windowSize + QSize(50, 50));
    require(area.size() == windowSize && area.minimumSize() == area.maximumSize(),
            "native and programmatic resize constraints freeze the preview window");
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const auto hwnd = reinterpret_cast<HWND>(area.winId());
        RECT rectangle{};
        GetWindowRect(hwnd, &rectangle);
        require(SendMessageW(hwnd, WM_NCHITTEST, 0,
                             MAKELPARAM(rectangle.left + 1, rectangle.top + 1)) == HTCLIENT,
                "native trim corners remain draggable client pixels rather than resize borders");
    }
#endif
    require(ScreenRecordingAreaWindowTestAccess::inputRegion(area) == QRegion(area.rect()),
            "preview owns its whole area for dragging");
    for (auto point : {area.rect().topLeft(), area.rect().bottomRight(), area.rect().center()})
        require(!ScreenRecordingAreaWindowTestAccess::edges(area, point),
                "trim preview has no resize hit targets");
    const QPoint from = area.recordingRegion().center();
    ScreenRecordingAreaWindowTestAccess::beginDrag(area, from, {});
    ScreenRecordingAreaWindowTestAccess::drag(area, from + QPoint(30, 20));
    ScreenRecordingAreaWindowTestAccess::finish(area);
    require(area.recordingRegion().size() == original, "preview dragging preserves its size");
    require(!area.canvas()->isVisible(), "drawing surface is hidden during preview");
    QImage frame(32, 24, QImage::Format_RGBA8888);
    frame.fill(Qt::red);
    area.setPreviewFrame(frame);
    require(area.grab().toImage().pixelColor(area.rect().center()).red() > 200,
            "preview frame paints inside the recording area");
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
    area.setTrimming(false);
    handle = area.findChild<QWidget*>(QStringLiteral("screenRecordingRegionDragHandle"));
    require(handle != nullptr && handle->isVisible(),
            "leaving trim preview restores the region editing drag handle");
    area.setTrimming(true);
    require(!handle->isVisible(), "entering trim preview hides an existing region drag handle");
    area.hide();
    area.show();
    QCoreApplication::processEvents();
    require(!handle->isVisible(), "showing trim preview again must keep the drag handle hidden");
}

namespace {
void recordingRightQuickSelection() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    QApplication::processEvents();
    auto* canvas = area.canvas();
    canvas_quick_selection_test::drawStroke(*canvas);
    canvas_quick_selection_test::selectAndDragStroke(*canvas);
    int deactivations = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::drawingDeactivationRequested, &area,
                     [&]() { ++deactivations; });
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonPress, {120, 135},
                                       Qt::RightButton, Qt::RightButton);
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseMove, {120, 160}, Qt::NoButton,
                                       Qt::RightButton);
    canvas_quick_selection_test::mouse(*canvas, QEvent::MouseButtonRelease, {120, 160},
                                       Qt::LeftButton, Qt::RightButton);
    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escape);
    require(deactivations == 0 && canvas->hasQuickSelectionTargetAt({120, 135}, Qt::RightButton),
            "Escape cancels right drag without leaving drawing mode");
    PhysicalKeyEvent secondEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &secondEscape);
    require(deactivations == 1, "next Escape can leave drawing mode");
}

void recordingAngleWheel() {
    ScreenRecordingAreaWindow area;
    area.setRecordingRegion(testRecordingRegion());
    area.show();
    area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
    QApplication::processEvents();
    int widthWheelRequests = 0;
    QObject::connect(&area, &ScreenRecordingAreaWindow::drawingWheelRequested, &area,
                     [&](int) { ++widthWheelRequests; });
    angle_wheel_host_test_support::exercise(
        *area.canvas(), [&] { return ScreenRecordingAreaWindowTestAccess::history(area); });
    require(widthWheelRequests == 0, "recording angle wheel bypasses ordinary style adjustment");
}

void webcamOrientationToolbarGeometryAndAppearance() {
    QWidget host;
    host.setGeometry(100, 100, 400, 300);
    host.show();
    RecordingWebcamControl control(&host);
    control.present(QRectF(100, 100, 150, 150), QSize(400, 300), 150, {}, {}, false);
    control.show();
    QApplication::processEvents();
    auto* toolbar =
        control.findChild<QWidget*>(QStringLiteral("screenRecordingWebcamOrientationToolbar"));
    auto* action = toolbar->findChild<QAbstractButton*>(
        QStringLiteral("screenRecordingWebcamRotateClockwise"));
    const qreal aboveGap = control.mapToGlobal(QPointF()).y() - (toolbar->y() + toolbar->height());
    require(aboveGap >= 6 && aboveGap < 7,
            "orientation row preserves six DIP clearance when aligned to whole widget coordinates");
    const QPoint initial = toolbar->pos();
    host.move(host.pos() + QPoint(20, 10));
    QApplication::processEvents();
    require(toolbar->pos() == initial + QPoint(20, 10), "floating controls follow their owner");
    int requests = 0;
    QObject::connect(&control, &RecordingWebcamControl::orientationRequested, &control,
                     [&](int) { ++requests; });
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(action, &press);
    QCoreApplication::sendEvent(action, &release);
    require(requests == 1, "keyboard activation invokes orientation once");
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    host.move(available.topLeft());
    control.present(QRectF(0, 0, 20, 20), QSize(20, 20), 150, {}, {}, true);
    QApplication::processEvents();
    const qreal belowGap = toolbar->y() - control.mapToGlobal(QPointF(0, 20)).y();
    require(available.contains(toolbar->geometry()) && belowGap >= 6 && belowGap < 7,
            "tiny top-edge webcams put a usable row below the image");
    host.move(available.bottomRight() - QPoint(10, 10));
    QApplication::processEvents();
    require(available.contains(toolbar->geometry()), "bottom-right placement clamps to the screen");
    for (const char* screenSignal : {"geometryChanged", "availableGeometryChanged"}) {
        toolbar->move(available.bottomRight() + QPoint(100, 100));
        require(QMetaObject::invokeMethod(QGuiApplication::primaryScreen(), screenSignal,
                                          Qt::DirectConnection, Q_ARG(QRect, available)),
                "notify a screen geometry change");
        require(available.contains(toolbar->geometry()),
                "display changes restore the floating row to its current screen bounds");
    }

    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalConfig = themes.config();
    const QString output = qEnvironmentVariable("SNOW_WEBCAM_ORIENTATION_PREVIEW_DIR");
    if (!output.isEmpty())
        require(QDir().mkpath(output), "create webcam preview output directory");
    for (const bool dark : {false, true}) {
        themes.setPreset(dark ? adqt::theme::ThemeScheme::Dark : adqt::theme::ThemeScheme::Light);
        QApplication::processEvents();
        for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
            QImage image((toolbar->size() * dpr).expandedTo(QSize(1, 1)),
                         QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::transparent);
            toolbar->render(&image);
            require(imageHasVisiblePixel(image),
                    "toolbar renders in both themes at fractional DPI");
            if (!output.isEmpty())
                require(image.save(QDir(output).filePath(
                            QStringLiteral("webcam-%1-%2.png")
                                .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                                .arg(dpr))),
                        "save webcam orientation visual fixture");
        }
    }
    themes.setConfig(originalConfig);
    host.hide();
    require(!toolbar->isVisible(), "hiding the owner retires its floating toolbar");
}

void webcamSetupGestures() {
    for (qreal scale : {1.0, 1.25, 1.5, 2.0}) {
        ScreenRecordingAreaWindow area;
        area.setRecordingRegion(QRect(-200, 40, 800, 600));
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
        area.show();
        QApplication::processEvents();
        const QRect region = area.recordingRegion();
        int size = 150;
        QPoint offset(48, 48);
        int commits = 0;
        const QSize physical(800, 600);
        const auto present = [&] {
            const int side = std::min({size, physical.width(), physical.height()});
            const QPoint fitted(std::min(offset.x(), physical.width() - side),
                                std::min(offset.y(), physical.height() - side));
            area.setWebcamOverlay(QRectF(30 + fitted.x() / scale,
                                         30 + (physical.height() - fitted.y() - side) / scale,
                                         side / scale, side / scale),
                                  physical, size, offset, fitted, false);
        };
        QObject::connect(&area, &ScreenRecordingAreaWindow::webcamLayoutPreviewed, &area,
                         [&](int next, const QPoint& position) {
                             size = next;
                             offset = position;
                             present();
                         });
        QObject::connect(&area, &ScreenRecordingAreaWindow::webcamLayoutCommitted, &area,
                         [&](int, const QPoint&) { ++commits; });
        present();
        auto* control = area.findChild<RecordingWebcamControl*>();
        auto* button = area.findChild<QToolButton*>(QStringLiteral("screenRecordingWebcamResize"));
        auto* toolbar =
            control->findChild<QWidget*>(QStringLiteral("screenRecordingWebcamOrientationToolbar"));
        require(toolbar && toolbar->isWindow() && toolbar->isVisible(),
                "orientation toolbar floats above the webcam without capture-region clipping");
        const std::array<const char*, 4> actionNames{
            "screenRecordingWebcamRotateClockwise", "screenRecordingWebcamRotateCounterclockwise",
            "screenRecordingWebcamFlipHorizontal", "screenRecordingWebcamFlipVertical"};
        const std::array<QString, 4> actionLabels{
            QStringLiteral("Rotate clockwise"), QStringLiteral("Rotate counterclockwise"),
            QStringLiteral("Flip horizontally"), QStringLiteral("Flip vertically")};
        int orientationAction = -1;
        QObject::connect(&area, &ScreenRecordingAreaWindow::webcamOrientationRequested, &area,
                         [&](int action) { orientationAction = action; });
        std::array<QAbstractButton*, 4> actions{};
        for (std::size_t i = 0; i < actions.size(); ++i) {
            actions[i] = toolbar->findChild<QAbstractButton*>(QString::fromLatin1(actionNames[i]));
            require(actions[i] && actions[i]->toolTip() == actionLabels[i] &&
                        actions[i]->accessibleName() == actionLabels[i] &&
                        actions[i]->focusPolicy() == Qt::StrongFocus,
                    "orientation actions have translated labels and keyboard focus");
            actions[i]->click();
            require(orientationAction == static_cast<int>(i), "each button dispatches its action");
        }
        require(QGuiApplication::primaryScreen()->availableGeometry().contains(toolbar->geometry()),
                "orientation row fits on screen even when capture begins off screen");
        class WebcamTranslator final : public QTranslator {
          public:
            QString translate(const char* context, const char* source, const char*,
                              int) const override {
                if (QByteArray(context) == "RecordingWebcamControl")
                    return QStringLiteral("translated:") + QString::fromUtf8(source);
                return {};
            }
        } translator;
        QCoreApplication::installTranslator(&translator);
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(control, &languageChange);
        require(actions[0]->toolTip() == QStringLiteral("translated:Rotate clockwise") &&
                    actions[3]->accessibleName() == QStringLiteral("translated:Flip vertically"),
                "visible orientation actions retranslate without recreating the camera");
        QCoreApplication::removeTranslator(&translator);
        QCoreApplication::sendEvent(control, &languageChange);
        require(control && control->isVisible() && button && button->width() == 32 &&
                    control->rect().contains(button->geometry()),
                "webcam resize button fits its top-right corner");
        require(ScreenRecordingAreaWindowTestAccess::inputRegion(area).contains(
                    control->geometry().center()),
                "webcam body must own native input");
        const auto send = [](QWidget& target, QEvent::Type type, QPointF global,
                             Qt::MouseButton changed, Qt::MouseButtons held) {
            const QPointF local = target.mapFromGlobal(global);
            QMouseEvent event(type, local, local, global, changed, held, Qt::NoModifier);
            QCoreApplication::sendEvent(&target, &event);
        };
        QPointF origin = control->mapToGlobal(QPointF(control->rect().center()));
        send(*control, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
        require(control->interacting() && QWidget::mouseGrabber() == control,
                "webcam dragging retains mouse capture");
        require(!toolbar->isEnabled(), "orientation actions are disabled during webcam gestures");
        send(*control, QEvent::MouseMove, origin + QPointF(40 / scale, -20 / scale), Qt::NoButton,
             Qt::LeftButton);
        require(
            offset == QPoint(88, 68) && size == 150 && area.recordingRegion() == region,
            "dragging changes bottom-left offsets in physical pixels without moving the region");
        send(*control, QEvent::MouseButtonRelease, origin + QPointF(40 / scale, -20 / scale),
             Qt::LeftButton, Qt::NoButton);
        require(commits == 1 && !control->interacting(), "release commits one webcam layout");
        require(toolbar->isEnabled(), "orientation actions resume after a gesture");
        origin = button->mapToGlobal(QPointF(button->rect().center()));
        send(*button, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
        send(*control, QEvent::MouseMove, origin + QPointF(80 / scale, -40 / scale), Qt::NoButton,
             Qt::LeftButton);
        require(size == 210 && offset == QPoint(88, 68) && control->width() == control->height(),
                "diagonal projection resizes proportionally around the bottom-left anchor");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(&area, &escape);
        require(size == 150 && commits == 1 && !control->interacting() && area.isVisible(),
                "Escape rolls back the webcam gesture without closing recording setup");
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(&area, &release);
        offset = QPoint(750, 550);
        present();
        origin = button->mapToGlobal(QPointF(button->rect().center()));
        send(*button, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
        send(*control, QEvent::MouseMove, origin + QPointF(40 / scale, 40 / scale), Qt::NoButton,
             Qt::LeftButton);
        require(size == 150 && offset == QPoint(750, 550),
                "movement perpendicular to the resize diagonal preserves fitted preferences");
        send(*control, QEvent::MouseMove, origin + QPointF(-50 / scale, 50 / scale), Qt::NoButton,
             Qt::LeftButton);
        require(size == 100 && offset == QPoint(650, 450),
                "shrinking a fitted webcam keeps its visible bottom-left anchor fixed");
        send(*control, QEvent::MouseMove, origin + QPointF(-10000, 10000), Qt::NoButton,
             Qt::LeftButton);
        require(size == 50, "resizing preserves the minimum preferred webcam size");
        send(*control, QEvent::MouseMove, origin + QPointF(10000, -10000), Qt::NoButton,
             Qt::LeftButton);
        require(size == 500, "resizing preserves the maximum preferred webcam size");
        area.cancelWebcamInteraction();
        require(size == 150 && offset == QPoint(750, 550) && commits == 1,
                "cancelling a fitted resize restores the original preferences");
        offset = QPoint(88, 68);
        present();
        for (int interruption = 0; interruption < 5; ++interruption) {
            origin = control->mapToGlobal(QPointF(control->rect().center()));
            send(*control, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
            send(*control, QEvent::MouseMove, origin + QPointF(-10000, 10000), Qt::NoButton,
                 Qt::LeftButton);
            require(offset == QPoint(0, 0), "webcam drag must clamp at the bottom-left corner");
            send(*control, QEvent::MouseMove, origin + QPointF(10000, -10000), Qt::NoButton,
                 Qt::LeftButton);
            require(offset == QPoint(650, 450), "webcam drag must clamp to the region");
            if (interruption == 0)
                area.setDrawingBlocked(true);
            if (interruption == 1)
                area.startCountdown(3);
            if (interruption == 2) {
                QEvent lost(QEvent::UngrabMouse);
                QCoreApplication::sendEvent(control, &lost);
            }
            if (interruption == 3) {
                QEvent blocked(QEvent::WindowBlocked);
                QCoreApplication::sendEvent(&area, &blocked);
            }
            if (interruption == 4)
                area.hide();
            require(offset == QPoint(88, 68) && commits == 1 && !control->interacting(),
                    "interruption restores the original layout without saving");
            area.setDrawingBlocked(false);
            area.clearCountdown();
            QEvent unblocked(QEvent::WindowUnblocked);
            QCoreApplication::sendEvent(&area, &unblocked);
            area.show();
            present();
        }
        area.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
        require(!control->isVisible(), "recording hides webcam editing controls");
        require(!toolbar->isVisible(), "recording hides the floating orientation row");
        orientationAction = -1;
        QMetaObject::invokeMethod(actions[0], "clicked", Qt::DirectConnection, Q_ARG(bool, false));
        require(orientationAction == -1, "a delayed click cannot escape setup eligibility");
        area.setRecordingState(ScreenshotToolPalette::RecordingState::Paused);
        require(!control->isVisible(), "paused recordings keep webcam editing locked");
        area.setRecordingState(ScreenshotToolPalette::RecordingState::Idle);
        area.setTrimming(true);
        require(!control->isVisible(), "playback hides webcam editing controls");
        area.setTrimming(false);
        area.setWebcamOverlay(QRectF(50, 50, 20, 20), QSize(20, 20), 150, QPoint(48, 48), {}, true);
        require(button->width() == 20 && control->rect().contains(button->geometry()),
                "tiny circular previews keep the resize control inside their bounds");
        // Clicking a fitted preview must not overwrite the larger preferred layout.
        origin = control->mapToGlobal(QPointF(10, 15));
        send(*control, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
        send(*control, QEvent::MouseButtonRelease, origin, Qt::LeftButton, Qt::NoButton);
        require(commits == 1, "a click without movement must not persist fitted geometry");
        area.setRecordingRegion(QRect(-200, -100, 20, 20));
        area.setWebcamOverlay(area.selectionRect(), QSize(20, 20), 150, QPoint(48, 48), {}, true);
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
        require(area.childAt(button->mapTo(&area, button->rect().center())) == button,
                "the resize button takes input priority over region controls in tiny regions");
        area.setWebcamOverlay({});
        require(!control->isVisible(), "disabled webcam releases its input surface");
        require(!toolbar->isVisible(), "camera disable also hides its floating controls");
    }
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "failed to create recording area test storage directory");
    const QString executableDirectory =
        QDir(storageDirectory.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executableDirectory),
            "failed to create recording area test executable directory");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({executableDirectory, storageDirectory.path(), 60000})
                .success,
            "failed to initialize isolated recording area test storage");
    if (application.arguments().contains(QStringLiteral("--crosshair-only"))) {
        ScreenRecordingAreaWindow area;
        area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
        require(area.canvas()->setCanvasTool(SnowCanvasTool::Shape), "activate a recording tool");
        require(area.canvas()->cursor() ==
                    snow_shot::presentation::crosshairCursor(area.canvas()->devicePixelRatioF()),
                "recording annotation tools must use Snow Shot's crosshair");
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--webcam-only"))) {
        webcamOrientationToolbarGeometryAndAppearance();
        webcamSetupGestures();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--angle-wheel-only"))) {
        recordingAngleWheel();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--right-quick-selection-only"))) {
        recordingRightQuickSelection();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--automatic-selection-only"))) {
        canvas_automatic_selection_test::exercise([](auto verify) {
            ScreenRecordingAreaWindow area;
            area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
            verify(*area.canvas());
        });
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--delete-text-after-cursor-only"))) {
        canvas_text_editing_preferences_test::exercise([](auto verify) {
            ScreenRecordingAreaWindow area;
            area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
            verify(*area.canvas());
        });
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--trim-only"))) {
        trimmingFreezesSizeAndMakesThePreviewDraggable();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (application.arguments().contains(QStringLiteral("--webcam-native-only"))) {
        const int result = nativeWebcamGestures();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return result;
    }
    if (application.arguments().contains(QStringLiteral("--native-geometry-only"))) {
        nativeRegionObservationNeverWaitsForTheGui();
        modalsSuspendRecordingInputAndRestoreTheRequestedMode();
        nativeRecordingBordersCross();
        nativeWindowsGeometryAndInteraction();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
#endif
#ifdef Q_OS_MACOS
    if (application.arguments().contains(QStringLiteral("--safe-area-only"))) {
        recordingSafeAreaChangesPreserveCaptureGeometry();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--native-border-input-only"))) {
        int result = 0;
        if (!macCanPostMouseEvents()) {
            result = 77;
        } else {
            try {
                recordingBorderInput(true);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                result = 1;
            }
        }
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return result;
    }
    if (application.arguments().contains(QStringLiteral("--native-logical-drag-only"))) {
        int result = 0;
        if (QGuiApplication::screens().size() < 2 || !macCanPostMouseEvents()) {
            result = 77;
        } else {
            try {
                nativeLogicalDrag();
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                result = 1;
            }
        }
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return result;
    }
    recordingBorderInput(false);
    logicalRegionDragAndResize();
#endif
    modalsSuspendRecordingInputAndRestoreTheRequestedMode();
#ifdef Q_OS_WIN
    regionObservationNeverWaitsForTheGui();
#endif
    trimmingFreezesSizeAndMakesThePreviewDraggable();
    controlledBordersCrossAndCancelWithoutClearingAnnotations();
    geometryAndTransparentCanvasFollowThePhysicalSelection();
    inputModesOwnOnlyDrawingInputAndRestoreRequestedState();
    drawingSurfaceFollowsEffectiveInputMode();
    wheelAndEscapeRespectDrawingOwnership();
    annotationsPersistAcrossStatesAndClearOnlyForANewRegion();
    quickSelectionFollowsTheDrawingSetting();
    geometryEditingIsOneCapability();
    childDragHandleOwnsCaptureAndPreservesAnnotations();
    interactionBoundariesAreIdempotentAndCancelOnStateChanges();
    dragHandleUsesWidgetEventsAndCancelsOnInterruption();
    ordinaryWindowGeometryPreservesAnnotations();
    drawingAcrossFrameEdgesRetainsDrawingOwnership();
    countdownHelpersFollowTheSecondBoundaries();
    countdownUsesThePausedBorderAndCentersItsIndicator();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
