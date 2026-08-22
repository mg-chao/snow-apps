#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "widgets/button.h"
#include "widgets/dpi_stable_window_controller.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QThread>
#include <QWheelEvent>
#include <QWindow>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#ifndef WM_GETDPISCALEDSIZE
#define WM_GETDPISCALEDSIZE 0x02E4
#endif
#endif

class ScreenshotFloatingToolPaletteWindowTestAccess {
  public:
    static void beginLogicalDrag(ScreenshotFloatingToolPaletteWindow& window,
                                 const QPoint& globalPosition) {
        window.m_draggingPalette = true;
        window.m_dragPhysicalAnchorValid = false;
        window.m_lastDragPosition = QPointF(globalPosition);
        window.m_dragContentPosition = QPointF(window.contentPosition());
    }

    static void beginPhysicalDrag(ScreenshotFloatingToolPaletteWindow& window,
                                  const QPoint& globalPosition) {
        window.beginPaletteDrag(globalPosition);
    }

    static void updateDrag(ScreenshotFloatingToolPaletteWindow& window,
                           const QPoint& globalPosition) {
        window.updatePaletteDrag(globalPosition);
    }

    static void finishDrag(ScreenshotFloatingToolPaletteWindow& window) {
        window.finishPaletteDrag(false);
    }

    static bool hasPhysicalDragAnchor(const ScreenshotFloatingToolPaletteWindow& window) {
        return window.m_dragPhysicalAnchorValid;
    }

    static quint64 geometryRefreshCount(const ScreenshotFloatingToolPaletteWindow& window) {
        return window.m_paletteGeometryRefreshCount;
    }

    static quint64 committedGeometryPassCount(const ScreenshotFloatingToolPaletteWindow& window) {
        return window.m_committedGeometryPassCount;
    }

    static quint64
    coalescedGeometryRequestCount(const ScreenshotFloatingToolPaletteWindow& window) {
        return window.m_coalescedGeometryRequestCount;
    }

    static QRect nativeGeometryForPhysicalDrag(const QPointF& physicalCursorPosition,
                                               const QPointF& physicalCursorToWindowOffset,
                                               const QSize& stablePhysicalWindowSize) {
        return ScreenshotFloatingToolPaletteWindow::nativeWindowGeometryForPhysicalDrag(
            physicalCursorPosition, physicalCursorToWindowOffset, stablePhysicalWindowSize);
    }

    static bool handleNativeHitTest(const ScreenshotFloatingToolPaletteWindow& window,
                                    void* message, qintptr* result) {
        return window.handleNativeHitTest(message, result);
    }
};

namespace {
std::atomic_bool nativeGeometryWarningEmitted{false};
QtMessageHandler previousMessageHandler = nullptr;

void captureNativeGeometryWarning(QtMsgType type, const QMessageLogContext& context,
                                  const QString& message) {
    if (type == QtWarningMsg && message.contains(QStringLiteral("QWindowsWindow::setGeometry"))) {
        nativeGeometryWarningEmitted.store(true, std::memory_order_relaxed);
    }

    if (previousMessageHandler != nullptr) {
        previousMessageHandler(type, context, message);
    } else {
        std::cerr << message.toLocal8Bit().constData() << '\n';
    }
}

class NativeGeometryWarningScope final {
  public:
    NativeGeometryWarningScope() {
        nativeGeometryWarningEmitted.store(false, std::memory_order_relaxed);
        previousMessageHandler = qInstallMessageHandler(captureNativeGeometryWarning);
    }

    ~NativeGeometryWarningScope() {
        qInstallMessageHandler(previousMessageHandler);
        previousMessageHandler = nullptr;
    }

    bool emitted() const {
        return nativeGeometryWarningEmitted.load(std::memory_order_relaxed);
    }

    NativeGeometryWarningScope(const NativeGeometryWarningScope&) = delete;
    NativeGeometryWarningScope& operator=(const NativeGeometryWarningScope&) = delete;
};

class SecondaryToolbarShowObserver final : public QObject {
  public:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::Show) {
            return false;
        }

        const QString objectName = watched->objectName();
        if (objectName == QStringLiteral("screenshotSelectActionPanel")) {
            ++actionPanelShowCount;
        } else if (objectName == QStringLiteral("screenshotRectangleStylePanel")) {
            ++stylePanelShowCount;
        }
        return false;
    }

    int actionPanelShowCount = 0;
    int stylePanelShowCount = 0;
};

class WheelEventObserver final : public QWidget {
  public:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event != nullptr && event->type() == QEvent::Wheel) {
            ++wheelEventCount;
        }
        return false;
    }

    int wheelEventCount = 0;
};

class NoOpToolbarCommands final : public ScreenshotToolbarCommandSink {
  public:
    void setMoveTool() override {}
    void setSelectTool() override {}
    void setShapeTool() override {}
    void setArrowTool() override {}
    void setLineTool() override {}
    void setFreeDrawTool() override {}
    void setHighlightTool() override {}
    void setPenHighlightTool() override {}
    void setEraserTool() override {}
    void setFilterTool() override {}
    void setWatermarkTool() override {}
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle&, quint32) override {}
    void setTextTool() override {}
    void setSerialNumberTool() override {}
    void setOcrTool() override {}
    void setTextTranslationTool() override {
        ++textTranslationToolCount;
    }
    void toggleTextTranslation() override {
        ++textTranslationToggleCount;
    }
    void startScrollingScreenshot() override {}
    void pinSelectionToScreen() override {}
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&) override {}
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSerialNumbers() override {}
    void incrementSelectedSerialNumbers() override {}
    void createTextForSelectedSerialNumber() override {}
    void repositionToolbarForContentChange() override {
        ++repositionCount;
    }
    void hideColorPickersForScreenshotUi() override {}

    int repositionCount = 0;
    int textTranslationToolCount = 0;
    int textTranslationToggleCount = 0;
};

#if defined(Q_OS_WIN) || defined(_WIN32)
HWND toNativeHwnd(WId windowId) {
    // Qt transports the native HWND through its integer-valued WId type.
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}

template <typename T> T* pointerFromLParam(LPARAM value) {
    // Windows transports callback context pointers through LPARAM.
    return reinterpret_cast<T*>(value); // NOLINT(performance-no-int-to-ptr)
}

struct HardwareMonitor {
    HMONITOR handle = nullptr;
    RECT bounds{};
    UINT dpi = 0;
};

class CursorPositionRestorer final {
  public:
    explicit CursorPositionRestorer(const POINT& position) : m_position(position) {}

    ~CursorPositionRestorer() {
        SetCursorPos(m_position.x, m_position.y);
    }

    CursorPositionRestorer(const CursorPositionRestorer&) = delete;
    CursorPositionRestorer& operator=(const CursorPositionRestorer&) = delete;

  private:
    POINT m_position{};
};

BOOL CALLBACK collectHardwareMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto* monitors = pointerFromLParam<std::vector<HardwareMonitor>>(context);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) != FALSE) {
        monitors->push_back(HardwareMonitor{monitor, info.rcMonitor, 0});
    }
    return TRUE;
}

bool populateMonitorDpi(std::vector<HardwareMonitor>* monitors) {
    using GetDpiForMonitorFunction = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore == nullptr) {
        return false;
    }
    const auto getDpiForMonitor =
        reinterpret_cast<GetDpiForMonitorFunction>(GetProcAddress(shcore, "GetDpiForMonitor"));
    if (getDpiForMonitor == nullptr) {
        FreeLibrary(shcore);
        return false;
    }

    bool populated = true;
    for (HardwareMonitor& monitor : *monitors) {
        UINT dpiX = 0;
        UINT dpiY = 0;
        if (FAILED(getDpiForMonitor(monitor.handle, 0, &dpiX, &dpiY)) || dpiX == 0 || dpiY == 0) {
            populated = false;
            break;
        }
        monitor.dpi = dpiX;
    }
    FreeLibrary(shcore);
    return populated;
}

QPoint monitorCenter(const HardwareMonitor& monitor) {
    return QPoint(monitor.bounds.left + (monitor.bounds.right - monitor.bounds.left) / 2,
                  monitor.bounds.top + (monitor.bounds.bottom - monitor.bounds.top) / 2);
}

QScreen* qtScreenForMonitor(const HardwareMonitor& monitor) {
    for (QScreen* screen : QGuiApplication::screens()) {
        auto* nativeScreen = screen != nullptr
                                 ? screen->nativeInterface<QNativeInterface::QWindowsScreen>()
                                 : nullptr;
        if (nativeScreen != nullptr && nativeScreen->handle() == monitor.handle) {
            return screen;
        }
    }
    return nullptr;
}

QRect monitorPhysicalBounds(const HardwareMonitor& monitor) {
    return QRect(monitor.bounds.left, monitor.bounds.top,
                 monitor.bounds.right - monitor.bounds.left,
                 monitor.bounds.bottom - monitor.bounds.top);
}

QRect nativeWindowGeometry(HWND window) {
    RECT bounds{};
    if (GetWindowRect(window, &bounds) == FALSE) {
        return QRect();
    }
    return QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
}

QSize nativeWindowSize(HWND window) {
    return nativeWindowGeometry(window).size();
}

QSize physicalSizeForDpi(const QSize& logicalSize, UINT dpi) {
    return QSize(qRound(static_cast<qreal>(logicalSize.width()) * dpi / 96.0),
                 qRound(static_cast<qreal>(logicalSize.height()) * dpi / 96.0));
}

bool sizesMatchWithinDpiRounding(const QSize& left, const QSize& right) {
    constexpr int kDpiRoundingTolerance = 2;
    return qAbs(left.width() - right.width()) <= kDpiRoundingTolerance &&
           qAbs(left.height() - right.height()) <= kDpiRoundingTolerance;
}

bool positionsMatchWithinDpiRounding(const QPoint& actual, const QPoint& expected) {
    return qAbs(actual.x() - expected.x()) <= 1 && qAbs(actual.y() - expected.y()) <= 1;
}

bool waitForNativePosition(HWND window, const QPoint& expectedPosition,
                           int timeoutMilliseconds = 1000) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (positionsMatchWithinDpiRounding(nativeWindowGeometry(window).topLeft(),
                                            expectedPosition)) {
            return true;
        }
        QThread::msleep(1);
    } while (timer.elapsed() < timeoutMilliseconds);
    return false;
}
#endif

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ScreenshotToolPalette::Options testToolbarOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showMoveTool = true;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.enableStyleToolbar = false;
    return options;
}

void settleQueuedRefreshes() {
    for (int iteration = 0; iteration < 4; ++iteration) {
        QCoreApplication::processEvents();
    }
}

void logicalDragMovesWithoutRefreshingGeometry() {
    ScreenshotFloatingToolPaletteWindow window(testToolbarOptions());
    window.prepareForDisplay();
    window.moveContentTo(QPoint(100, 120));
    settleQueuedRefreshes();

    const QPoint dragStart(320, 240);
    const QPoint initialContentPosition = window.contentPosition();
    const QSize initialWindowSize = window.size();
    ScreenshotFloatingToolPaletteWindowTestAccess::beginLogicalDrag(window, dragStart);
    const quint64 initialRefreshCount =
        ScreenshotFloatingToolPaletteWindowTestAccess::geometryRefreshCount(window);

    for (int step = 1; step <= 24; ++step) {
        ScreenshotFloatingToolPaletteWindowTestAccess::updateDrag(
            window, dragStart + QPoint(step, step / 2));
    }

    require(window.contentPosition() == initialContentPosition + QPoint(24, 12),
            "logical drag should track the pointer delta");
    require(window.size() == initialWindowSize,
            "same-screen drag should preserve the prepared window size");
    require(ScreenshotFloatingToolPaletteWindowTestAccess::geometryRefreshCount(window) ==
                initialRefreshCount,
            "same-screen logical drag must not refresh palette geometry");
    ScreenshotFloatingToolPaletteWindowTestAccess::finishDrag(window);
}

void physicalDragMovesWithoutRefreshingGeometry() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    ScreenshotFloatingToolPaletteWindow window(testToolbarOptions());
    window.prepareForDisplay();
    window.moveContentTo(QPoint(160, 180));
    settleQueuedRefreshes();

    const QPoint cursorPosition = QCursor::pos();
    ScreenshotFloatingToolPaletteWindowTestAccess::beginPhysicalDrag(window, cursorPosition);
    settleQueuedRefreshes();
    const quint64 initialRefreshCount =
        ScreenshotFloatingToolPaletteWindowTestAccess::geometryRefreshCount(window);

    ScreenshotFloatingToolPaletteWindowTestAccess::updateDrag(window, cursorPosition);

    require(ScreenshotFloatingToolPaletteWindowTestAccess::geometryRefreshCount(window) ==
                initialRefreshCount,
            "same-screen physical drag must not refresh palette geometry");
    ScreenshotFloatingToolPaletteWindowTestAccess::finishDrag(window);
#endif
}

void physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const NativeGeometryWarningScope geometryWarningScope;
    std::vector<HardwareMonitor> monitors;
    require(EnumDisplayMonitors(nullptr, nullptr, collectHardwareMonitor,
                                reinterpret_cast<LPARAM>(&monitors)) != FALSE,
            "failed to enumerate the hardware monitors");
    require(monitors.size() >= 2, "hardware test requires at least two active monitors");
    require(populateMonitorDpi(&monitors), "hardware test could not read effective monitor DPI");

    const HardwareMonitor* source = nullptr;
    const HardwareMonitor* destination = nullptr;
    qint64 shortestDistanceSquared = std::numeric_limits<qint64>::max();
    for (const HardwareMonitor& left : monitors) {
        for (const HardwareMonitor& right : monitors) {
            // Exercise the reported direction: the destination needs a larger logical client
            // rect even though the toolbar keeps the same physical frame.
            if (left.handle == right.handle || left.dpi <= right.dpi) {
                continue;
            }
            const QPoint delta = monitorCenter(right) - monitorCenter(left);
            const qint64 distanceSquared = static_cast<qint64>(delta.x()) * delta.x() +
                                           static_cast<qint64>(delta.y()) * delta.y();
            if (distanceSquared < shortestDistanceSquared) {
                source = &left;
                destination = &right;
                shortestDistanceSquared = distanceSquared;
            }
        }
    }
    require(source != nullptr && destination != nullptr,
            "hardware test requires two monitors with different effective DPI values");

    POINT originalCursor{};
    require(GetCursorPos(&originalCursor) != FALSE, "failed to save cursor position");
    const CursorPositionRestorer restoreCursor(originalCursor);

    QScreen* sourceScreen = qtScreenForMonitor(*source);
    require(sourceScreen != nullptr, "could not map the source monitor to QScreen");
    QScreen* destinationScreen = qtScreenForMonitor(*destination);
    require(destinationScreen != nullptr, "could not map the destination monitor to QScreen");
    QWidget overlayOwner;
    overlayOwner.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    overlayOwner.setAttribute(Qt::WA_TransparentForMouseEvents, true);
    overlayOwner.setWindowOpacity(0.0);
    overlayOwner.winId();
    require(overlayOwner.windowHandle() != nullptr, "test overlay did not create a native window");
    overlayOwner.windowHandle()->setScreen(sourceScreen);
    overlayOwner.setGeometry(sourceScreen->geometry());
    overlayOwner.show();
    settleQueuedRefreshes();

    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.resetForNewCapture();
    window.setPlacementContext(sourceScreen, sourceScreen->geometry(),
                               monitorPhysicalBounds(*source));
    window.setOwnerWindow(&overlayOwner);
    window.prepareForDisplay();
    window.show();
    settleQueuedRefreshes();
    const auto shapeColorTriggerTop = [&window](const QString& name) {
        QWidget* shapeControls = window.palette()->findChild<QWidget*>(
            QStringLiteral("screenshotRectangleStyleControls"));
        if (shapeControls == nullptr) {
            return -1;
        }
        for (QWidget* widget : window.palette()->findChildren<QWidget*>()) {
            if (widget->accessibleName() == name) {
                return widget->mapTo(shapeControls, QPoint()).y();
            }
        }
        return -1;
    };
    const HWND nativeWindow = toNativeHwnd(window.winId());
    require(IsWindow(nativeWindow) != FALSE, "toolbar did not create a native HWND");

    const QPoint start = monitorCenter(*source);
    const QPoint finish = monitorCenter(*destination);
    const QPoint cursorOffset(24, 16);
    const QSize preparedPhysicalSize = nativeWindowSize(nativeWindow);
    require(preparedPhysicalSize.isValid(), "failed to measure the toolbar HWND");
    require(SetWindowPos(nativeWindow, nullptr, start.x() - cursorOffset.x(),
                         start.y() - cursorOffset.y(), preparedPhysicalSize.width(),
                         preparedPhysicalSize.height(), SWP_NOACTIVATE | SWP_NOZORDER) != FALSE,
            "failed to position the toolbar on the source monitor");
    require(SetCursorPos(start.x(), start.y()) != FALSE,
            "failed to position the cursor on the source monitor");
    settleQueuedRefreshes();

    const QSize stablePhysicalSize = nativeWindowSize(nativeWindow);
    const UINT sourceWindowDpi = GetDpiForWindow(nativeWindow);
    require(sourceWindowDpi > destination->dpi,
            "toolbar HWND did not adopt the higher-DPI source monitor");

    // Exercise the native hit-test conversion with a real lower-DPI HWND while the toolbar's Qt
    // window still reports the source monitor DPR. This models the event turn after
    // WM_DPICHANGED, when the HWND and QWindow can temporarily disagree about their DPI.
    QWidget destinationDpiProbe;
    destinationDpiProbe.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    destinationDpiProbe.setAttribute(Qt::WA_TransparentForMouseEvents, true);
    destinationDpiProbe.setWindowOpacity(0.0);
    destinationDpiProbe.winId();
    require(destinationDpiProbe.windowHandle() != nullptr,
            "DPI probe did not create a native window");
    destinationDpiProbe.windowHandle()->setScreen(destinationScreen);
    destinationDpiProbe.resize(window.size());
    destinationDpiProbe.move(destinationScreen->geometry().center() -
                             QPoint(destinationDpiProbe.width() / 2,
                                    destinationDpiProbe.height() / 2));
    destinationDpiProbe.show();
    settleQueuedRefreshes();

    const HWND destinationProbeWindow = toNativeHwnd(destinationDpiProbe.winId());
    const UINT destinationProbeDpi = GetDpiForWindow(destinationProbeWindow);
    require(destinationProbeDpi > 0 && destinationProbeDpi < sourceWindowDpi,
            "DPI probe did not adopt the lower-DPI destination monitor");
    RECT destinationProbeRect{};
    require(GetWindowRect(destinationProbeWindow, &destinationProbeRect) != FALSE,
            "failed to read the DPI probe geometry");

    const QRegion interactiveRegion = window.paletteHost()->interactiveHostRegion().translated(
        window.paletteHost()->pos());
    const qreal sourceScale = static_cast<qreal>(sourceWindowDpi) / 96.0;
    const qreal destinationScale = static_cast<qreal>(destinationProbeDpi) / 96.0;
    QPoint hitTestPhysicalOffset;
    bool foundDpiSensitivePoint = false;
    const QRect interactiveBounds = interactiveRegion.boundingRect().intersected(window.rect());
    for (int y = interactiveBounds.top(); y <= interactiveBounds.bottom() &&
                                              !foundDpiSensitivePoint;
         ++y) {
        for (int x = interactiveBounds.left(); x <= interactiveBounds.right(); ++x) {
            const QPoint physicalOffset(qFloor((x + 0.5) * destinationScale),
                                        qFloor((y + 0.5) * destinationScale));
            const QPoint mappedWithDestinationDpi(
                qFloor(static_cast<qreal>(physicalOffset.x()) / destinationScale),
                qFloor(static_cast<qreal>(physicalOffset.y()) / destinationScale));
            const QPoint mappedWithStaleSourceDpi(
                qFloor(static_cast<qreal>(physicalOffset.x()) / sourceScale),
                qFloor(static_cast<qreal>(physicalOffset.y()) / sourceScale));
            if (window.rect().contains(mappedWithDestinationDpi) &&
                interactiveRegion.contains(mappedWithDestinationDpi) &&
                (!window.rect().contains(mappedWithStaleSourceDpi) ||
                 !interactiveRegion.contains(mappedWithStaleSourceDpi))) {
                hitTestPhysicalOffset = physicalOffset;
                foundDpiSensitivePoint = true;
                break;
            }
        }
    }
    require(foundDpiSensitivePoint,
            "toolbar did not expose a point that distinguishes destination and stale source DPI");

    const QPoint nativeHitTestPoint(destinationProbeRect.left + hitTestPhysicalOffset.x(),
                                    destinationProbeRect.top + hitTestPhysicalOffset.y());
    require(nativeHitTestPoint.x() >= std::numeric_limits<short>::min() &&
                nativeHitTestPoint.x() <= std::numeric_limits<short>::max() &&
                nativeHitTestPoint.y() >= std::numeric_limits<short>::min() &&
                nativeHitTestPoint.y() <= std::numeric_limits<short>::max(),
            "native hit-test point exceeds the WM_NCHITTEST coordinate range");
    MSG hitTestMessage{};
    hitTestMessage.hwnd = destinationProbeWindow;
    hitTestMessage.message = WM_NCHITTEST;
    hitTestMessage.lParam = MAKELPARAM(nativeHitTestPoint.x(), nativeHitTestPoint.y());
    qintptr hitTestResult = HTERROR;
    require(!ScreenshotFloatingToolPaletteWindowTestAccess::handleNativeHitTest(
                window, &hitTestMessage, &hitTestResult) &&
                hitTestResult == HTERROR,
            "native hit testing used the stale source-monitor DPR for a destination HWND");

    const QRect initialNativeGeometry = nativeWindowGeometry(nativeWindow);
    const QPoint physicalCursorToWindowOffset = start - initialNativeGeometry.topLeft();
    const QSize stableVisualPhysicalSize =
        physicalSizeForDpi(window.visualContentRect().size(), sourceWindowDpi);
    ScreenshotFloatingToolPaletteWindowTestAccess::beginPhysicalDrag(window, QCursor::pos());
    require(ScreenshotFloatingToolPaletteWindowTestAccess::hasPhysicalDragAnchor(window),
            "toolbar did not start a native physical drag");

    bool reachedDestination = false;
    bool observedDpiTransition = false;
    std::string failure;
    const int distance = qMax(qAbs(finish.x() - start.x()), qAbs(finish.y() - start.y()));
    const int steps = qMax(1, distance / 2);
    QPoint expectedFinalTopLeft;
    for (int step = 1; step <= steps; ++step) {
        const QPoint cursor(
            start.x() + qRound(static_cast<qreal>(finish.x() - start.x()) * step / steps),
            start.y() + qRound(static_cast<qreal>(finish.y() - start.y()) * step / steps));
        if (SetCursorPos(cursor.x(), cursor.y()) == FALSE) {
            failure = "failed to move the hardware cursor between monitors";
            break;
        }
        ScreenshotFloatingToolPaletteWindowTestAccess::updateDrag(window, QCursor::pos());
        settleQueuedRefreshes();

        POINT actualCursor{};
        if (GetCursorPos(&actualCursor) == FALSE) {
            failure = "failed to read the physical cursor during the monitor move";
            break;
        }
        const QPoint expectedTopLeft(actualCursor.x - physicalCursorToWindowOffset.x(),
                                     actualCursor.y - physicalCursorToWindowOffset.y());
        expectedFinalTopLeft = expectedTopLeft;
        waitForNativePosition(nativeWindow, expectedTopLeft, 10);
        const QRect settledNativeGeometry = nativeWindowGeometry(nativeWindow);
        if (settledNativeGeometry.size() != stablePhysicalSize) {
            failure = "toolbar physical pixel size changed from " +
                      std::to_string(stablePhysicalSize.width()) + "x" +
                      std::to_string(stablePhysicalSize.height()) + " to " +
                      std::to_string(settledNativeGeometry.width()) + "x" +
                      std::to_string(settledNativeGeometry.height()) +
                      " during the monitor move at DPI " +
                      std::to_string(GetDpiForWindow(nativeWindow));
            break;
        }
        const UINT currentWindowDpi = GetDpiForWindow(nativeWindow);
        const QSize visualPhysicalSize =
            physicalSizeForDpi(window.visualContentRect().size(), currentWindowDpi);
        if (!sizesMatchWithinDpiRounding(visualPhysicalSize, stableVisualPhysicalSize)) {
            failure = "visible toolbar physical pixel size changed from " +
                      std::to_string(stableVisualPhysicalSize.width()) + "x" +
                      std::to_string(stableVisualPhysicalSize.height()) + " to " +
                      std::to_string(visualPhysicalSize.width()) + "x" +
                      std::to_string(visualPhysicalSize.height()) + " during the monitor move";
            break;
        }
        const HMONITOR windowMonitor = MonitorFromWindow(nativeWindow, MONITOR_DEFAULTTONULL);
        reachedDestination = reachedDestination || windowMonitor == destination->handle;
        observedDpiTransition = observedDpiTransition || currentWindowDpi != sourceWindowDpi;
    }

    if (failure.empty() && !waitForNativePosition(nativeWindow, expectedFinalTopLeft, 3000)) {
        const QPoint actualFinalTopLeft = nativeWindowGeometry(nativeWindow).topLeft();
        failure = "toolbar HWND did not finish at the requested destination position: expected " +
                  std::to_string(expectedFinalTopLeft.x()) + "," +
                  std::to_string(expectedFinalTopLeft.y()) + " but reached " +
                  std::to_string(actualFinalTopLeft.x()) + "," +
                  std::to_string(actualFinalTopLeft.y());
    }
    ScreenshotFloatingToolPaletteWindowTestAccess::finishDrag(window);
    if (failure.empty()) {
        require(nativeWindowSize(nativeWindow) == stablePhysicalSize,
                "toolbar physical pixel size changed after crossing monitors");
        window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
        settleQueuedRefreshes();

        const qreal expectedDestinationScale =
            sourceScreen->devicePixelRatio() / destinationScreen->devicePixelRatio();
        require(qFuzzyCompare(window.paletteHost()->physicalScale() + 1.0,
                              expectedDestinationScale + 1.0),
                "style toolbar should retain the selection display as its scale reference");
        const QSize styledVisualPhysicalSize =
            physicalSizeForDpi(window.visualContentRect().size(), GetDpiForWindow(nativeWindow));
        const QSize styledPhysicalWindowSize = nativeWindowSize(nativeWindow);

        ScreenshotFloatingToolPaletteWindowTestAccess::beginPhysicalDrag(window, QCursor::pos());
        require(ScreenshotFloatingToolPaletteWindowTestAccess::hasPhysicalDragAnchor(window),
                "toolbar did not restart a native physical drag after style changes");
        const QPoint styledReturnStart = QCursor::pos();
        const int returnDistance =
            qMax(qAbs(start.x() - styledReturnStart.x()),
                 qAbs(start.y() - styledReturnStart.y()));
        const int returnSteps = qMax(1, returnDistance / 2);
        for (int step = 1; step <= returnSteps; ++step) {
            const QPoint cursor(
                styledReturnStart.x() +
                    qRound(static_cast<qreal>(start.x() - styledReturnStart.x()) * step /
                           returnSteps),
                styledReturnStart.y() +
                    qRound(static_cast<qreal>(start.y() - styledReturnStart.y()) * step /
                           returnSteps));
            require(SetCursorPos(cursor.x(), cursor.y()) != FALSE,
                    "failed to move the styled toolbar back to the source monitor");
            POINT actualCursor{};
            require(GetCursorPos(&actualCursor) != FALSE,
                    "failed to read the cursor during the styled return move");
            ScreenshotFloatingToolPaletteWindowTestAccess::updateDrag(
                window, QPoint(actualCursor.x, actualCursor.y));
            settleQueuedRefreshes();
        }
        const QWidget* shapeControls = window.palette()->findChild<QWidget*>(
            QStringLiteral("screenshotRectangleStyleControls"));
        require(shapeControls != nullptr,
                "shape style controls should exist after activating the shape tool");
        const int rowTop = shapeControls->rect().top();
        require(shapeColorTriggerTop(QStringLiteral("Stroke color")) == rowTop &&
                    shapeColorTriggerTop(QStringLiteral("Fill color")) == rowTop,
                "shape color editor triggers should stay aligned after a native DPI transition");
        window.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
        settleQueuedRefreshes();
        window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
        settleQueuedRefreshes();
        require(shapeColorTriggerTop(QStringLiteral("Stroke color")) == rowTop &&
                    shapeColorTriggerTop(QStringLiteral("Fill color")) == rowTop,
                "shape color editor triggers should remain aligned after a tool toggle");
        const QRect returnedNativeGeometry = nativeWindowGeometry(nativeWindow);
        require(MonitorFromWindow(nativeWindow, MONITOR_DEFAULTTONULL) == source->handle,
                ("styled toolbar did not return to the selection monitor; native geometry is " +
                 std::to_string(returnedNativeGeometry.x()) + "," +
                 std::to_string(returnedNativeGeometry.y()) + " " +
                 std::to_string(returnedNativeGeometry.width()) + "x" +
                 std::to_string(returnedNativeGeometry.height()) + ", expected frame size " +
                 std::to_string(styledPhysicalWindowSize.width()) + "x" +
                 std::to_string(styledPhysicalWindowSize.height()))
                    .c_str());
        require(qFuzzyCompare(window.paletteHost()->physicalScale() + 1.0, 2.0),
                "styled toolbar should use the selection monitor's scale after returning");
        const QSize returnedStyledVisualPhysicalSize =
            physicalSizeForDpi(window.visualContentRect().size(), GetDpiForWindow(nativeWindow));
        require(
            sizesMatchWithinDpiRounding(returnedStyledVisualPhysicalSize, styledVisualPhysicalSize),
            "styled toolbar physical content size changed when returning to the selection monitor");
        require(
            nativeWindowSize(nativeWindow) == styledPhysicalWindowSize,
            "styled toolbar physical frame size changed when returning to the selection monitor");
        ScreenshotFloatingToolPaletteWindowTestAccess::finishDrag(window);
    }
    window.hide();
    settleQueuedRefreshes();

    require(failure.empty(), failure.c_str());
    require(reachedDestination, "toolbar HWND never reached the destination monitor");
    require(observedDpiTransition, "toolbar HWND did not receive the destination monitor DPI");
    require(!geometryWarningScope.emitted(),
            "mixed-DPI drag emitted QWindowsWindow::setGeometry warning");
#endif
}

void dpiScaledSizeMessagePreservesThePhysicalWindowSize() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    ScreenshotFloatingToolPaletteWindow window(testToolbarOptions());
    window.prepareForDisplay();
    const QSize stablePhysicalWindowSize(240, 56);
    const WId testWindowId = static_cast<WId>(1);
    SIZE requestedSize{1, 1};
    MSG message{};
    message.hwnd = toNativeHwnd(testWindowId);
    message.message = WM_GETDPISCALEDSIZE;
    message.wParam = MAKELPARAM(192, 192);
    message.lParam = reinterpret_cast<LPARAM>(&requestedSize);
    qintptr result = 0;
    const bool handled =
        adqt::widgets::AdDpiStableWindowController::enforceStablePhysicalSizeForMessage(
            &message, testWindowId, stablePhysicalWindowSize, true, &result);

    require(handled && result == TRUE &&
                QSize(requestedSize.cx, requestedSize.cy) == stablePhysicalWindowSize,
            "WM_GETDPISCALEDSIZE should preserve the toolbar physical size");

    WINDOWPOS requestedPosition{};
    requestedPosition.hwnd = toNativeHwnd(testWindowId);
    requestedPosition.cx = 480;
    requestedPosition.cy = 112;
    message.message = WM_WINDOWPOSCHANGING;
    message.lParam = reinterpret_cast<LPARAM>(&requestedPosition);
    result = 0;
    const bool positionHandled =
        adqt::widgets::AdDpiStableWindowController::enforceStablePhysicalSizeForMessage(
            &message, testWindowId, stablePhysicalWindowSize, true, &result);
    require(!positionHandled &&
                QSize(requestedPosition.cx, requestedPosition.cy) == stablePhysicalWindowSize,
            "WM_WINDOWPOSCHANGING should reject Qt's destination-scaled size");
#endif
}

void logicalMetricsIgnorePhysicalPlacementBounds() {
    ScreenshotFloatingToolPaletteWindow window(testToolbarOptions());
    const QRect logicalBounds(0, 0, 1920, 1080);
    window.setPlacementContext(nullptr, logicalBounds, logicalBounds);
    window.prepareForDisplay();
    const QSize initialContentSize = window.contentSizeHint();
    const QSize initialWindowSize = window.windowSizeHint();
    const quint64 geometryCommits =
        ScreenshotFloatingToolPaletteWindowTestAccess::committedGeometryPassCount(window);

    window.setPlacementContext(nullptr, logicalBounds,
                               QRect(0, 0, logicalBounds.width() * 2, logicalBounds.height() * 2));

    require(window.contentSizeHint() == initialContentSize,
            "physical capture bounds must not change toolbar logical content metrics");
    require(window.windowSizeHint() == initialWindowSize,
            "physical capture bounds must not change toolbar logical window metrics");
    require(ScreenshotFloatingToolPaletteWindowTestAccess::committedGeometryPassCount(window) ==
                geometryCommits,
            "physical-only placement changes should not commit toolbar geometry");
}

void styleToolChangesKeepThePresetWindowSize() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showMoveTool = true;
    options.showSelectTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showOcrTool = true;
    options.showScrollingScreenshotTool = true;
    options.showScreenRecordButton = true;
    options.separatorBeforeShape = true;
    options.separatorAfterArrow = true;
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;
    ScreenshotFloatingToolPaletteWindow window(options);
    window.prepareForDisplay();
    const QSize presetWindowSize = window.size();

    constexpr ScreenshotToolPalette::Tool tools[] = {
        ScreenshotToolPalette::Tool::Select,       ScreenshotToolPalette::Tool::Shape,
        ScreenshotToolPalette::Tool::Arrow,        ScreenshotToolPalette::Tool::Text,
        ScreenshotToolPalette::Tool::SerialNumber,
    };
    for (const ScreenshotToolPalette::Tool tool : tools) {
        window.palette()->setActiveTool(tool);
        settleQueuedRefreshes();
        require(window.size() == presetWindowSize && window.windowSizeHint() == presetWindowSize,
                "style tool changes must stay within the preset toolbar window");
    }
}

void placementRectsTrackTheDisplayedStyleToolbar() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.prepareForDisplay();
    const QSize presetWindowSize = window.windowSizeHint();
    ScreenshotToolPalette* palette = window.palette();
    require(palette != nullptr, "screenshot toolbar should own a palette");

    const auto expectedVisibleRect = [palette]() {
        return palette->mainToolbarContentRect().united(
            palette->stylePanel()->geometry().translated(-palette->contentOffset()));
    };

    window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    settleQueuedRefreshes();
    const QRect shapePlacementRect = window.bottomPlacementContentRect();
    require(shapePlacementRect == expectedVisibleRect(),
            "bottom placement should use the displayed shape style toolbar size");

    window.setActiveTool(ScreenshotToolPalette::Tool::Text);
    settleQueuedRefreshes();
    const QRect textPlacementRect = window.bottomPlacementContentRect();
    require(textPlacementRect == expectedVisibleRect(),
            "bottom placement should use the displayed text style toolbar size");
    require(shapePlacementRect != window.fullContentRect() ||
                textPlacementRect != window.fullContentRect(),
            "placement should not always use the maximum reserved toolbar extent");

    window.setStyleToolbarAboveMain(true);
    settleQueuedRefreshes();
    require(window.topPlacementContentRect() == expectedVisibleRect(),
            "top placement should use the displayed style toolbar size");
    require(window.windowSizeHint() == presetWindowSize,
            ("actual placement extents must not resize the preset toolbar window; preset " +
             std::to_string(presetWindowSize.width()) + "x" +
             std::to_string(presetWindowSize.height()) + ", actual " +
             std::to_string(window.windowSizeHint().width()) + "x" +
             std::to_string(window.windowSizeHint().height()))
                .c_str());
}

void toolChangesRepositionOnlyBeforeManualDrag() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.prepareForDisplay();

    window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    require(commands.repositionCount == 1,
            "an automatically placed toolbar should reposition after a tool change");

    window.paletteHost()->dragStarted(QPoint(10, 10));
    window.setActiveTool(ScreenshotToolPalette::Tool::Text);
    require(commands.repositionCount == 1,
            "a manually dragged toolbar should retain its position after a tool change");

    window.resetPositionForSelection(window.contentPosition(), window.windowSizeHint());
    window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    require(commands.repositionCount == 2,
            "resetting the toolbar after a selection change should restore "
            "automatic tool-change repositioning");

    window.resetForNewCapture();
    window.setActiveTool(ScreenshotToolPalette::Tool::Text);
    require(commands.repositionCount == 3,
            "resetting a capture should restore automatic tool-change repositioning");
}

void unchangedShadowMarginsAreNoOps() {
    ScreenshotFloatingToolPaletteWindow window(testToolbarOptions());
    ScreenshotToolPalette* palette = window.palette();
    require(palette != nullptr, "floating toolbar should own a palette");
    require(!palette->setShadowMargins(ScreenshotToolPaletteHost::defaultShadowMargins()),
            "setting the current shadow margins should be a no-op");
}

void screenshotToolbarSizeMultiplierSurvivesCaptureReset() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.setToolbarSize(QStringLiteral("small"));
    window.prepareForDisplay();
    require(qFuzzyCompare(window.paletteHost()->physicalScale() + 1.0, 1.8),
            "small screenshot toolbar should apply the 0.8 palette multiplier");
    window.resetForNewCapture();
    require(qFuzzyCompare(window.paletteHost()->physicalScale() + 1.0, 1.8),
            "capture reset should preserve the configured small toolbar multiplier");
    window.setToolbarSize(QStringLiteral("normal"));
    window.prepareForDisplay();
    require(qFuzzyCompare(window.paletteHost()->physicalScale() + 1.0, 2.0),
            "normal screenshot toolbar should restore the unmodified DPI scale");
}

void translateButtonRoutesEveryClickThroughTheToggleCommand() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.setActiveTool(ScreenshotToolPalette::Tool::Ocr);
    window.setTextEditingState(true, false);
    window.setTextTranslationState(true, false, false);

    auto* translate = window.findChild<QAbstractButton*>(
        QStringLiteral("screenshotOcrTextTranslateButton"));
    require(translate != nullptr && translate->isEnabled(),
            "Translate should be available for a completed OCR result");
    translate->click();
    require(commands.textTranslationToggleCount == 1,
            "the first Translate click should enter through the toggle command");

    window.setTextTranslationState(true, true, true);
    require(translate->isEnabled(),
            "active Translate should stay clickable while translation is streaming");
    translate->click();
    require(commands.textTranslationToggleCount == 2,
            "clicking active Translate should exit through the same toggle command");
}

void mainTextTranslationButtonUsesTranslationPresentation() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    auto* translation = window.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotTextTranslationButton"));
    auto* recognition = window.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotTextRecognitionButton"));
    require(translation != nullptr, "the main Text Translation control should be present");

    translation->click();
    require(commands.textTranslationToolCount == 1 &&
                window.palette()->activeToolForTests() ==
                    ScreenshotToolPalette::Tool::TextTranslation,
            "the main Text Translation control should activate the translation presentation");
    window.setOcrBusy(true);
    require(translation->busy() && (recognition == nullptr || !recognition->busy()),
            "recognition for Text Translation should load on the translation control");
    window.setOcrBusy(false);
    window.setTextTranslationState(true, true, true);
    require(translation->busy(),
            "streaming translation should load on the main translation control");
    window.setTextTranslationState(true, true, false);
    require(!translation->busy(),
            "the main translation control should stop loading when streaming completes");
}

void nativeSurfaceReleaseKeepsTheToolbarObjectUsableForDeferredDeletion() {
    ScreenshotFloatingToolPaletteWindow window(testToolbarOptions());
    window.show();
    QApplication::processEvents();
    static_cast<void>(window.winId());
    require(window.internalWinId() != 0 && window.testAttribute(Qt::WA_WState_Created),
            "the toolbar teardown test must begin with a live native window");

    window.releaseNativeSurface();
    require(window.internalWinId() == 0 && !window.testAttribute(Qt::WA_WState_Created),
            "toolbar retirement must synchronously release its native window");
    require(!window.isVisible(),
            "native surface release must leave the still-live toolbar object hidden");
}

void firstSecondaryToolbarOpeningNeverExposesBothPanelTypes() {
    const auto verifyFirstActivation = [](ScreenshotToolPalette::Tool tool,
                                          bool expectActionPanel) {
        NoOpToolbarCommands commands;
        ScreenshotToolbarWindow window(commands);
        ScreenshotToolPalette* palette = window.palette();
        require(palette != nullptr && palette->actionPanel() == nullptr &&
                    palette->stylePanel() == nullptr,
                "the screenshot toolbar must begin with lazy secondary resources");

        window.show();
        QApplication::processEvents();

        SecondaryToolbarShowObserver observer;
        QApplication::instance()->installEventFilter(&observer);
        window.setActiveTool(tool);
        QApplication::processEvents();
        QApplication::instance()->removeEventFilter(&observer);

        require((expectActionPanel ? palette->actionPanel() != nullptr
                                   : palette->stylePanel() != nullptr) &&
                    (expectActionPanel ? palette->stylePanel() == nullptr
                                       : palette->actionPanel() == nullptr),
                "the first secondary-tool activation must materialize only the requested panel");
        require(observer.actionPanelShowCount == (expectActionPanel ? 1 : 0) &&
                    observer.stylePanelShowCount == (expectActionPanel ? 0 : 1),
                "the first secondary-tool activation must never expose the other panel type");
        const QWidget* requestedPanel =
            expectActionPanel ? palette->actionPanel() : palette->stylePanel();
        require(requestedPanel != nullptr && !requestedPanel->isHidden(),
                "only the requested secondary panel may remain visible after first activation");
    };

    verifyFirstActivation(ScreenshotToolPalette::Tool::Shape, false);
    verifyFirstActivation(ScreenshotToolPalette::Tool::Select, true);
}

void secondaryToolbarEvictionIsSafeAndReusable() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    ScreenshotToolPalette* palette = window.palette();
    require(palette != nullptr && palette->mainPanel() != nullptr,
            "the reusable screenshot toolbar must retain its main panel");
    QWidget* const mainPanel = palette->mainPanel();

    for (int cycle = 0; cycle < 2; ++cycle) {
        window.show();
        QApplication::processEvents();
        window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
        QApplication::processEvents();

        QPointer<QWidget> actionPanel = palette->actionPanel();
        QPointer<QWidget> stylePanel = palette->stylePanel();
        require(stylePanel != nullptr && actionPanel == nullptr,
                "activating a drawing tool must materialize only its style panel");

        window.releaseIdleResources();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();

        require(actionPanel.isNull() && stylePanel.isNull(),
                "ending a capture must destroy the materialized secondary panels");
        require(palette->actionPanel() == nullptr && palette->stylePanel() == nullptr &&
                    palette->mainPanel() == mainPanel,
                "secondary eviction must not leave exposed stale panels or replace the main row");

        // Capture-state callbacks can still arrive while the reusable toolbar is idle.
        // They must update retained state without touching controls from the retired rows.
        window.setTextEditingState(true, false, true, true);
        window.setTextTranslationState(true, false, false, false, false, false);
        window.setTableEditingState(true, true, true, true, true, true);
        window.setTextTransformSelections(QStringLiteral("keep"), QStringLiteral("half"));
        SnowCanvasSpotlightConfig spotlight;
        spotlight.opacity = 0.42;
        window.setSpotlightConfig(spotlight);
        window.resetForNewCapture();
    }
}

void lazySecondaryControlsInheritWheelEventFilters() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    ScreenshotToolPalette* palette = window.palette();
    require(palette != nullptr && palette->stylePanel() == nullptr,
            "the wheel-filter test must begin before lazy style controls exist");

    WheelEventObserver observer;
    palette->installWheelFilters(&observer);
    window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QWidget* controls =
        palette->findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
    require(controls != nullptr,
            "activating Shape must materialize controls for the wheel-filter test");

    const QPoint localPosition = controls->rect().center();
    QWheelEvent event(QPointF(localPosition), controls->mapToGlobal(localPosition), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(controls, &event);
    require(observer.wheelEventCount == 1,
            "controls materialized after filter registration must inherit the wheel filter");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        if (app.arguments().contains(QStringLiteral("--ocr-translation-toggle-only"))) {
            translateButtonRoutesEveryClickThroughTheToggleCommand();
            mainTextTranslationButtonUsesTranslationPresentation();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--toolbar-size-only"))) {
            screenshotToolbarSizeMultiplierSurvivesCaptureReset();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--native-surface-release-only"))) {
            nativeSurfaceReleaseKeepsTheToolbarObjectUsableForDeferredDeletion();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--secondary-toolbar-eviction-only"))) {
            secondaryToolbarEvictionIsSafeAndReusable();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--secondary-toolbar-first-open-only"))) {
            firstSecondaryToolbarOpeningNeverExposesBothPanelTypes();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--mixed-dpi-only"))) {
            physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable();
            return 0;
        }
        logicalDragMovesWithoutRefreshingGeometry();
        physicalDragMovesWithoutRefreshingGeometry();
        physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable();
        dpiScaledSizeMessagePreservesThePhysicalWindowSize();
        logicalMetricsIgnorePhysicalPlacementBounds();
        styleToolChangesKeepThePresetWindowSize();
        placementRectsTrackTheDisplayedStyleToolbar();
        toolChangesRepositionOnlyBeforeManualDrag();
        unchangedShadowMarginsAreNoOps();
        screenshotToolbarSizeMultiplierSurvivesCaptureReset();
        translateButtonRoutesEveryClickThroughTheToggleCommand();
        mainTextTranslationButtonUsesTranslationPresentation();
        firstSecondaryToolbarOpeningNeverExposesBothPanelTypes();
        secondaryToolbarEvictionIsSafeAndReusable();
        lazySecondaryControlsInheritWheelEventFilters();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
