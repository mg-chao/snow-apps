#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "widgets/context_menu.h"

#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QPointer>
#include <QScreen>
#include <QThread>

#include <iostream>
#include <stdexcept>
#include <string>

#include <qt_windows.h>

namespace {
constexpr QRect kSelectionCanvasGeometry(4855, 805, 1298, 737);
constexpr int kObservationDurationMs = 1000;

HWND toNativeHwnd(WId windowId) {
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}

std::string describeRect(const QRect& rect) {
    return std::to_string(rect.x()) + "," + std::to_string(rect.y()) + " " +
           std::to_string(rect.width()) + "x" + std::to_string(rect.height());
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void processEventsFor(int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
}

class CursorPositionRestorer final {
  public:
    CursorPositionRestorer() : m_valid(GetCursorPos(&m_position) != FALSE) {}

    ~CursorPositionRestorer() {
        if (m_valid) {
            SetCursorPos(m_position.x, m_position.y);
        }
    }

  private:
    POINT m_position{};
    bool m_valid = false;
};

class InitialGeometryObserver final : public QObject {
  public:
    InitialGeometryObserver(ScreenshotPinnedWindow& window, const QRect& expectedGeometry)
        : m_window(window), m_expectedGeometry(expectedGeometry) {
        m_window.installEventFilter(this);
    }

    [[nodiscard]] QRect geometryAtShow() const {
        return m_geometryAtShow;
    }

    [[nodiscard]] QRect firstGeometryAfterShow() const {
        return m_firstGeometryAfterShow;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != &m_window || event == nullptr) {
            return false;
        }
        if (event->type() == QEvent::Show) {
            m_showSeen = true;
            m_geometryAtShow = m_window.currentNativeGeometry();
        } else if (m_showSeen && m_firstGeometryAfterShow.isNull() &&
                   (event->type() == QEvent::Move || event->type() == QEvent::Resize) &&
                   m_window.currentNativeGeometry() != m_expectedGeometry) {
            m_firstGeometryAfterShow = m_window.currentNativeGeometry();
        }
        return false;
    }

  private:
    ScreenshotPinnedWindow& m_window;
    QRect m_expectedGeometry;
    QRect m_geometryAtShow;
    QRect m_firstGeometryAfterShow;
    bool m_showSeen = false;
};

QRect nativeGeometryForSelection(const QRect& selection) {
    // Screenshot selections use a zero-based capture canvas. Production pin
    // placement adds the physical virtual-desktop origin to return to HWND coordinates.
    const QPoint virtualDesktopOrigin(GetSystemMetrics(SM_XVIRTUALSCREEN),
                                      GetSystemMetrics(SM_YVIRTUALSCREEN));
    return selection.translated(virtualDesktopOrigin);
}

QRect nativeClientGeometry(HWND hwnd) {
    RECT clientRect{};
    POINT clientTopLeft{};
    if (hwnd == nullptr || GetClientRect(hwnd, &clientRect) == FALSE ||
        ClientToScreen(hwnd, &clientTopLeft) == FALSE) {
        return {};
    }
    return QRect(clientTopLeft.x, clientTopLeft.y,
                 static_cast<int>(clientRect.right - clientRect.left),
                 static_cast<int>(clientRect.bottom - clientRect.top));
}

class NativeGeometryObserver final : public QAbstractNativeEventFilter {
  public:
    explicit NativeGeometryObserver(HWND hwnd) : m_hwnd(hwnd) {}

    void arm(const QRect& expectedGeometry) {
        m_expectedGeometry = expectedGeometry;
        m_firstChangedGeometry = {};
        m_mostDownwardGeometry = expectedGeometry;
        m_armed = true;
    }

    [[nodiscard]] QRect firstChangedGeometry() const {
        return m_firstChangedGeometry;
    }

    [[nodiscard]] QRect mostDownwardGeometry() const {
        return m_mostDownwardGeometry;
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr*) override {
        const bool isWindowsMessage = eventType == QByteArrayLiteral("windows_generic_MSG") ||
                                      eventType == QByteArrayLiteral("windows_dispatcher_MSG");
        if (!m_armed || !isWindowsMessage || message == nullptr) {
            return false;
        }

        const auto* nativeMessage = static_cast<const MSG*>(message);
        if (nativeMessage->hwnd != m_hwnd ||
            (nativeMessage->message != WM_WINDOWPOSCHANGED && nativeMessage->message != WM_MOVE)) {
            return false;
        }

        const QRect currentGeometry = nativeClientGeometry(m_hwnd);
        if (m_firstChangedGeometry.isNull() && currentGeometry != m_expectedGeometry) {
            m_firstChangedGeometry = currentGeometry;
        }
        if (currentGeometry.y() > m_mostDownwardGeometry.y()) {
            m_mostDownwardGeometry = currentGeometry;
        }
        return false;
    }

  private:
    HWND m_hwnd = nullptr;
    QRect m_expectedGeometry;
    QRect m_firstChangedGeometry;
    QRect m_mostDownwardGeometry;
    bool m_armed = false;
};

void settleRealCursorAt(const QPoint& nativePosition) {
    POINT settledCursor{};
    bool cursorSettled = false;
    QElapsedTimer settleTimeout;
    settleTimeout.start();
    while (!cursorSettled && settleTimeout.elapsed() < 1000) {
        require(SetCursorPos(nativePosition.x(), nativePosition.y()) != FALSE,
                "failed to move the real cursor to the pinned window");
        processEventsFor(20);
        cursorSettled = GetCursorPos(&settledCursor) != FALSE &&
                        settledCursor.x == nativePosition.x() &&
                        settledCursor.y == nativePosition.y();
    }
    require(cursorSettled, "the real cursor did not settle at the requested native position");
}

void sendRealLeftClick(const QPoint& nativePosition) {
    settleRealCursorAt(nativePosition);

    INPUT buttonDown{};
    buttonDown.type = INPUT_MOUSE;
    buttonDown.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    require(SendInput(1, &buttonDown, sizeof(INPUT)) == 1,
            "failed to press the real left mouse button");

    INPUT buttonUp{};
    buttonUp.type = INPUT_MOUSE;
    buttonUp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    require(SendInput(1, &buttonUp, sizeof(INPUT)) == 1,
            "failed to release the real left mouse button");
}

void sendRealRightClick(const QPoint& nativePosition) {
    settleRealCursorAt(nativePosition);

    INPUT buttonDown{};
    buttonDown.type = INPUT_MOUSE;
    buttonDown.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    require(SendInput(1, &buttonDown, sizeof(INPUT)) == 1,
            "failed to press the real right mouse button");
    // Let the non-client right-button-down transition settle as it does during a human click.
    processEventsFor(100);

    INPUT buttonUp{};
    buttonUp.type = INPUT_MOUSE;
    buttonUp.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    require(SendInput(1, &buttonUp, sizeof(INPUT)) == 1,
            "failed to release the real right mouse button");
}

void reproducePinnedWindowDownwardShift(SnowCanvasRuntime&) {
    const CursorPositionRestorer restoreCursor;
    const QRect requestedNativeGeometry = nativeGeometryForSelection(kSelectionCanvasGeometry);
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(requestedNativeGeometry);
    require(screen != nullptr, "the pinned selection could not be associated with a screen");

    QImage background(kSelectionCanvasGeometry.size(), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(42, 84, 126));

    auto* pinnedWindow = new ScreenshotPinnedWindow(ScreenshotPinnedWindow::RuntimeMode::NoDocument);
    QPointer<ScreenshotPinnedWindow> guardedWindow(pinnedWindow);
    InitialGeometryObserver initialGeometryObserver(*pinnedWindow, requestedNativeGeometry);
    ScreenshotPinnedWindow::Config config;
    config.nativeGeometry = requestedNativeGeometry;
    config.canvasSourceRect = QRectF(kSelectionCanvasGeometry);
    config.contentCanvasRect = config.canvasSourceRect;
    config.surfaceCanvasRect = config.canvasSourceRect;
    config.backgroundImage = background;
    config.screen = screen;
    config.enableEditing = true;
    require(pinnedWindow->present(config), "failed to present the pinned-window reproduction");
    require(pinnedWindow->currentNativeGeometry() == requestedNativeGeometry,
            "the pinned window left present() at " +
                describeRect(pinnedWindow->currentNativeGeometry()) + " instead of " +
                describeRect(requestedNativeGeometry));
    const QRect geometryAfterPresent = pinnedWindow->currentNativeGeometry();
    processEventsFor(200);

    require(pinnedWindow->currentNativeGeometry() == geometryAfterPresent,
            "the pinned window moved during the 200 ms wait after presentation: before " +
                describeRect(geometryAfterPresent) + ", after " +
                describeRect(pinnedWindow->currentNativeGeometry()));

    require(initialGeometryObserver.geometryAtShow() == requestedNativeGeometry,
            "the pinned window was first shown at " +
                describeRect(initialGeometryObserver.geometryAtShow()) + " instead of " +
                describeRect(requestedNativeGeometry));
    require(initialGeometryObserver.firstGeometryAfterShow().isNull(),
            "the pinned window moved immediately after creation from " +
                describeRect(requestedNativeGeometry) + " to " +
                describeRect(initialGeometryObserver.firstGeometryAfterShow()));

    const HWND pinnedHwnd = toNativeHwnd(pinnedWindow->winId());
    require(pinnedHwnd != nullptr && IsWindowVisible(pinnedHwnd) != FALSE,
            "the pinned-window reproduction did not create a visible HWND");
    require(pinnedWindow->currentNativeGeometry() == requestedNativeGeometry,
            "the pinned window did not start at the requested native geometry: requested " +
                describeRect(requestedNativeGeometry) + ", actual " +
                describeRect(pinnedWindow->currentNativeGeometry()));

    const QPoint clickPosition = requestedNativeGeometry.center();
    require(WindowFromPoint(POINT{clickPosition.x(), clickPosition.y()}) == pinnedHwnd,
            "the pinned HWND is not the topmost window beneath the real click position");
    require(SendMessage(pinnedHwnd, WM_NCHITTEST, 0,
                        MAKELPARAM(static_cast<WORD>(clickPosition.x()),
                                   static_cast<WORD>(clickPosition.y()))) == HTCAPTION,
            "the real click position does not use the pinned window's caption path");

    NativeGeometryObserver nativeGeometryObserver(pinnedHwnd);
    QCoreApplication::instance()->installNativeEventFilter(&nativeGeometryObserver);
    const QRect geometryBeforeLeftClick = pinnedWindow->currentNativeGeometry();
    nativeGeometryObserver.arm(geometryBeforeLeftClick);
    sendRealLeftClick(clickPosition);
    processEventsFor(200);
    require(pinnedWindow->currentNativeGeometry() == geometryBeforeLeftClick,
            "the pinned window moved during the 200 ms wait after a real left-click: before " +
                describeRect(geometryBeforeLeftClick) + ", after " +
                describeRect(pinnedWindow->currentNativeGeometry()));
    require(nativeGeometryObserver.firstChangedGeometry().isNull(),
            "the pinned window transiently moved during a stationary real left-click: before " +
                describeRect(geometryBeforeLeftClick) + ", first changed " +
                describeRect(nativeGeometryObserver.firstChangedGeometry()));

    auto* menu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedContextMenu"));
    auto* scaleMenu = pinnedWindow->findChild<adqt::widgets::AdContextMenu*>(
        QStringLiteral("screenshotPinnedScaleMenu"));
    require(menu != nullptr && scaleMenu != nullptr,
            "the pinned context menu or scale submenu was not found");

    const QRect geometryBeforeScaleTransaction = pinnedWindow->currentNativeGeometry();
    scaleMenu->actions().at(2)->trigger();
    processEventsFor(100);
    const QSize expectedScaledSize(qRound(geometryBeforeScaleTransaction.width() * 0.75),
                                   qRound(geometryBeforeScaleTransaction.height() * 0.75));
    require(pinnedWindow->currentNativeGeometry() ==
                QRect(geometryBeforeScaleTransaction.topLeft(), expectedScaledSize),
            "the stationary real left-click left native geometry transactions blocked");
    scaleMenu->actions().at(3)->trigger();
    processEventsFor(100);
    require(pinnedWindow->currentNativeGeometry() == geometryBeforeScaleTransaction,
            "restoring 100 percent after the stationary click changed the native anchor");

    const QRect geometryBeforeRightClick = pinnedWindow->currentNativeGeometry();
    nativeGeometryObserver.arm(geometryBeforeRightClick);
    sendRealRightClick(geometryBeforeRightClick.center());

    QRect polledFirstChangedGeometry;
    QRect polledMostDownwardGeometry = geometryBeforeRightClick;
    bool menuBecameVisible = false;
    QElapsedTimer observation;
    observation.start();
    while (observation.elapsed() < kObservationDurationMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const QRect currentGeometry = pinnedWindow->currentNativeGeometry();
        menuBecameVisible = menuBecameVisible || menu->isVisible();
        if (polledFirstChangedGeometry.isNull() && currentGeometry != geometryBeforeRightClick) {
            polledFirstChangedGeometry = currentGeometry;
        }
        if (currentGeometry.y() > polledMostDownwardGeometry.y()) {
            polledMostDownwardGeometry = currentGeometry;
        }
        QThread::msleep(1);
    }

    const QRect finalGeometry = pinnedWindow->currentNativeGeometry();
    QCoreApplication::instance()->removeNativeEventFilter(&nativeGeometryObserver);
    const QRect firstChangedGeometry = !nativeGeometryObserver.firstChangedGeometry().isNull()
                                           ? nativeGeometryObserver.firstChangedGeometry()
                                           : polledFirstChangedGeometry;
    const QRect mostDownwardGeometry =
        nativeGeometryObserver.mostDownwardGeometry().y() > polledMostDownwardGeometry.y()
            ? nativeGeometryObserver.mostDownwardGeometry()
            : polledMostDownwardGeometry;
    menu->hide();
    pinnedWindow->close();
    QElapsedTimer deletionWait;
    deletionWait.start();
    while (!guardedWindow.isNull() && deletionWait.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }

    require(menuBecameVisible, "the real right-click did not open the pinned context menu");
    require(firstChangedGeometry.isNull(),
            "reproduced pinned-window movement after a real right-click: before " +
                describeRect(geometryBeforeRightClick) + ", first changed " +
                describeRect(firstChangedGeometry) + ", furthest downward " +
                describeRect(mostDownwardGeometry) + ", final " + describeRect(finalGeometry) +
                ", downward shift " +
                std::to_string(mostDownwardGeometry.y() - geometryBeforeRightClick.y()) + " px");
    require(guardedWindow.isNull(), "the pinned-window reproduction was not deleted");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);

    try {
        SnowCanvasRuntime sourceRuntime;
        require(sourceRuntime.isValid(), "source runtime creation failed");
        reproducePinnedWindowDownwardShift(sourceRuntime);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "The pinned window was created and remained at its exact native geometry "
                 "through the real right-click.\n";
    return 0;
}
