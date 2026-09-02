#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
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
#include <QLayout>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QThread>
#include <QWindow>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <sstream>
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

bool sizesMatchWithinOnePhysicalPixel(const QSize& left, const QSize& right) {
    return qAbs(left.width() - right.width()) <= 1 &&
           qAbs(left.height() - right.height()) <= 1;
}

void require(bool condition, const char* message);

struct MainToolbarButtonSizeSnapshot {
    const adqt::widgets::AdButton* button = nullptr;
    QString description;
    QSize size;
    QSize iconSize;
};

struct ToolbarSizeSnapshot {
    QSize visualContentSize;
    QSize mainToolbarContentSize;
    QSize mainPanelSize;
    QSize secondaryPanelSize;
    QVector<MainToolbarButtonSizeSnapshot> buttons;
};

QSize snapshotPhysicalSize(const QSize& logicalSize, qreal dpi) {
    return QSize(qRound(static_cast<qreal>(logicalSize.width()) * dpi / 96.0),
                 qRound(static_cast<qreal>(logicalSize.height()) * dpi / 96.0));
}

QString describeButton(const adqt::widgets::AdButton* button, int index) {
    if (button == nullptr) {
        return QStringLiteral("main toolbar button #%1").arg(index);
    }
    if (!button->objectName().isEmpty()) {
        return QStringLiteral("main toolbar button '%1'").arg(button->objectName());
    }
    if (!button->toolTip().isEmpty()) {
        return QStringLiteral("main toolbar button '%1'").arg(button->toolTip());
    }
    return QStringLiteral("main toolbar button #%1").arg(index);
}

ToolbarSizeSnapshot captureToolbarSizeSnapshot(const ScreenshotToolbarWindow& window,
                                               const QWidget* secondaryPanel, qreal dpi) {
    const ScreenshotToolPalette* palette = window.palette();
    const QWidget* mainPanel = palette != nullptr ? palette->mainPanel() : nullptr;
    require(palette != nullptr && mainPanel != nullptr, "toolbar size snapshot lacks its main panel");

    ToolbarSizeSnapshot snapshot;
    snapshot.visualContentSize = snapshotPhysicalSize(window.visualContentRect().size(), dpi);
    snapshot.mainToolbarContentSize =
        snapshotPhysicalSize(palette->mainToolbarContentRect().size(), dpi);
    snapshot.mainPanelSize = snapshotPhysicalSize(mainPanel->size(), dpi);
    snapshot.secondaryPanelSize = secondaryPanel != nullptr
                                      ? snapshotPhysicalSize(secondaryPanel->size(), dpi)
                                      : QSize();
    const QList<adqt::widgets::AdButton*> buttons =
        mainPanel->findChildren<adqt::widgets::AdButton*>();
    snapshot.buttons.reserve(buttons.size());
    for (int index = 0; index < buttons.size(); ++index) {
        const adqt::widgets::AdButton* button = buttons.at(index);
        snapshot.buttons.push_back(MainToolbarButtonSizeSnapshot{
            button,
            describeButton(button, index),
            snapshotPhysicalSize(button->size(), dpi),
            snapshotPhysicalSize(button->iconSize(), dpi),
        });
    }
    return snapshot;
}

void appendPhysicalSizeFailure(const QSize& expectedSize, const QSize& actualSize,
                               const QString& component, const char* stateDescription,
                               std::vector<std::string>* failures) {
    if (sizesMatchWithinOnePhysicalPixel(expectedSize, actualSize)) {
        return;
    }
    std::ostringstream message;
    message << stateDescription << " " << component.toStdString()
            << " physical size changed from " << expectedSize.width() << "x"
            << expectedSize.height() << " to " << actualSize.width() << "x"
            << actualSize.height() << " (more than 1px)";
    failures->push_back(message.str());
}

void appendMainToolbarSizeFailures(const ToolbarSizeSnapshot& expected,
                                   const ToolbarSizeSnapshot& actual,
                                   const char* stateDescription,
                                   std::vector<std::string>* failures) {
    appendPhysicalSizeFailure(expected.mainToolbarContentSize, actual.mainToolbarContentSize,
                              QStringLiteral("main toolbar content"), stateDescription, failures);
    appendPhysicalSizeFailure(expected.mainPanelSize, actual.mainPanelSize,
                              QStringLiteral("main toolbar panel"), stateDescription, failures);
    if (actual.buttons.size() != expected.buttons.size()) {
        failures->push_back("main toolbar button count changed during a display transition");
        return;
    }
    for (int index = 0; index < expected.buttons.size(); ++index) {
        const MainToolbarButtonSizeSnapshot& expectedButton = expected.buttons.at(index);
        const MainToolbarButtonSizeSnapshot& actualButton = actual.buttons.at(index);
        if (expectedButton.button != actualButton.button) {
            failures->push_back("main toolbar button identity changed during a display transition");
            continue;
        }
        appendPhysicalSizeFailure(expectedButton.size, actualButton.size,
                                  expectedButton.description + QStringLiteral(" size"),
                                  stateDescription, failures);
        appendPhysicalSizeFailure(expectedButton.iconSize, actualButton.iconSize,
                                  expectedButton.description + QStringLiteral(" icon size"),
                                  stateDescription, failures);
    }
}

void appendToolbarSizeFailures(const ToolbarSizeSnapshot& expected,
                               const ToolbarSizeSnapshot& actual,
                               const char* stateDescription,
                               std::vector<std::string>* failures) {
    appendMainToolbarSizeFailures(expected, actual, stateDescription, failures);
    appendPhysicalSizeFailure(expected.visualContentSize, actual.visualContentSize,
                              QStringLiteral("visible toolbar content"), stateDescription,
                              failures);
    appendPhysicalSizeFailure(expected.secondaryPanelSize, actual.secondaryPanelSize,
                              QStringLiteral("secondary toolbar panel"), stateDescription,
                              failures);
}

void appendSecondaryPanelLayoutFailure(QWidget* panel, qreal dpi, const char* stateDescription,
                                       std::vector<std::string>* failures) {
    if (panel == nullptr || panel->layout() == nullptr) {
        failures->push_back(std::string(stateDescription) +
                            " secondary toolbar does not have a layout");
        return;
    }
    panel->layout()->activate();
    appendPhysicalSizeFailure(snapshotPhysicalSize(panel->layout()->sizeHint(), dpi),
                              snapshotPhysicalSize(panel->size(), dpi),
                              QStringLiteral("secondary toolbar layout"), stateDescription,
                              failures);
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

ScreenshotToolPalette::Options recordingToolbarOptionsForPresetTest() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showRecordingControls = true;
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

void physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable(bool reverseDirection) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const NativeGeometryWarningScope geometryWarningScope;
    std::vector<HardwareMonitor> monitors;
    require(EnumDisplayMonitors(nullptr, nullptr, collectHardwareMonitor,
                                reinterpret_cast<LPARAM>(&monitors)) != FALSE,
            "failed to enumerate the hardware monitors");
    require(monitors.size() >= 2, "hardware test requires at least two active monitors");
    require(populateMonitorDpi(&monitors), "hardware test could not read effective monitor DPI");

    constexpr UINT kMonitorADpi = 144;
    constexpr UINT kMonitorBDpi = 96;
    const HardwareMonitor* monitorA = nullptr;
    const HardwareMonitor* monitorB = nullptr;
    for (const HardwareMonitor& candidateA : monitors) {
        if (candidateA.dpi != kMonitorADpi) {
            continue;
        }
        for (const HardwareMonitor& candidateB : monitors) {
            const bool verticallyOverlaps =
                candidateB.bounds.top < candidateA.bounds.bottom &&
                candidateB.bounds.bottom > candidateA.bounds.top;
            if (candidateB.dpi == kMonitorBDpi &&
                candidateB.bounds.right == candidateA.bounds.left && verticallyOverlaps) {
                monitorA = &candidateA;
                monitorB = &candidateB;
                break;
            }
        }
        if (monitorA != nullptr) {
            break;
        }
    }
    require(monitorA != nullptr && monitorB != nullptr,
            "hardware test requires 150% monitor A immediately right of 100% monitor B");

    const HardwareMonitor* source = monitorA;
    const HardwareMonitor* destination = monitorB;
    if (reverseDirection) {
        std::swap(source, destination);
    }

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

    window.resetForNewCapture();
    settleQueuedRefreshes();
    const QPoint start = monitorCenter(*source);
    const QPoint finish = monitorCenter(*destination);
    const QPoint cursorOffset(24, 16);

    QWidget* shapeStylePanel = window.palette()->stylePanel();
    QWidget* selectActionPanel = window.palette()->actionPanel();
    require(shapeStylePanel != nullptr && selectActionPanel != nullptr,
            "toolbar did not create both secondary toolbar panels");
    std::vector<std::string> failures;

    const UINT sourceWindowDpi = GetDpiForWindow(nativeWindow);
    window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    settleQueuedRefreshes();
    if (!window.palette()->styleToolbarVisible() || window.palette()->actionToolbarVisible()) {
        failures.push_back("Shape should show only the style toolbar on the source display");
    }
    appendSecondaryPanelLayoutFailure(shapeStylePanel, sourceWindowDpi,
                                      "Shape toolbar on source display", &failures);
    const ToolbarSizeSnapshot sourceShapeSizes =
        captureToolbarSizeSnapshot(window, shapeStylePanel, sourceWindowDpi);
    window.setActiveTool(ScreenshotToolPalette::Tool::Select);
    settleQueuedRefreshes();
    if (!window.palette()->actionToolbarVisible() || window.palette()->styleToolbarVisible()) {
        failures.push_back("Select should show only the action toolbar on the source display");
    }
    appendSecondaryPanelLayoutFailure(selectActionPanel, sourceWindowDpi,
                                      "Select toolbar on source display", &failures);
    const ToolbarSizeSnapshot sourceSelectSizes =
        captureToolbarSizeSnapshot(window, selectActionPanel, sourceWindowDpi);
    appendMainToolbarSizeFailures(sourceShapeSizes, sourceSelectSizes,
                                  "when switching from Shape to Select on the source display",
                                  &failures);
    window.setActiveTool(ScreenshotToolPalette::Tool::Move);
    settleQueuedRefreshes();

    // Place the native frame only after the final tool/layout refresh. A placement
    // performed before tool changes can be replayed by Qt's logical geometry update,
    // leaving the cursor far from the toolbar when the source monitor is B.
    const QSize preparedPhysicalSize = nativeWindowSize(nativeWindow);
    require(preparedPhysicalSize.isValid(), "failed to measure the toolbar HWND");
    require(SetCursorPos(start.x(), start.y()) != FALSE,
            "failed to position the cursor on the source monitor");
    require(SetWindowPos(nativeWindow, nullptr, start.x() - cursorOffset.x(),
                         start.y() - cursorOffset.y(), preparedPhysicalSize.width(),
                         preparedPhysicalSize.height(), SWP_NOACTIVATE | SWP_NOZORDER) != FALSE,
            "failed to position the toolbar on the source monitor");

    const QSize stablePhysicalSize = nativeWindowSize(nativeWindow);
    const QRect initialNativeGeometry = nativeWindowGeometry(nativeWindow);
    const QPoint physicalCursorToWindowOffset = start - initialNativeGeometry.topLeft();
    ScreenshotFloatingToolPaletteWindowTestAccess::beginPhysicalDrag(window, QCursor::pos());
    require(ScreenshotFloatingToolPaletteWindowTestAccess::hasPhysicalDragAnchor(window),
            "toolbar did not start a native physical drag");

    bool reachedDestination = false;
    bool observedDpiTransition = false;
    const QSize stableMoveVisualSize =
        snapshotPhysicalSize(window.visualContentRect().size(), sourceWindowDpi);
    const int distance = qMax(qAbs(finish.x() - start.x()), qAbs(finish.y() - start.y()));
    const int steps = qMax(1, distance / 2);
    QPoint expectedFinalTopLeft;
    for (int step = 1; step <= steps; ++step) {
        const QPoint cursor(
            start.x() + qRound(static_cast<qreal>(finish.x() - start.x()) * step / steps),
            start.y() + qRound(static_cast<qreal>(finish.y() - start.y()) * step / steps));
        if (SetCursorPos(cursor.x(), cursor.y()) == FALSE) {
            failures.push_back("failed to move the hardware cursor between monitors");
            break;
        }
        ScreenshotFloatingToolPaletteWindowTestAccess::updateDrag(window, QCursor::pos());
        settleQueuedRefreshes();

        POINT actualCursor{};
        if (GetCursorPos(&actualCursor) == FALSE) {
            failures.push_back("failed to read the physical cursor during the monitor move");
            break;
        }
        const QPoint expectedTopLeft(actualCursor.x - physicalCursorToWindowOffset.x(),
                                     actualCursor.y - physicalCursorToWindowOffset.y());
        expectedFinalTopLeft = expectedTopLeft;
        waitForNativePosition(nativeWindow, expectedTopLeft, 10);
        const QRect settledNativeGeometry = nativeWindowGeometry(nativeWindow);
        if (settledNativeGeometry.size() != stablePhysicalSize) {
            failures.push_back("toolbar physical pixel size changed from " +
                               std::to_string(stablePhysicalSize.width()) + "x" +
                               std::to_string(stablePhysicalSize.height()) + " to " +
                               std::to_string(settledNativeGeometry.width()) + "x" +
                               std::to_string(settledNativeGeometry.height()) +
                               " during the monitor move at DPI " +
                               std::to_string(GetDpiForWindow(nativeWindow)));
            break;
        }
        const UINT currentWindowDpi = GetDpiForWindow(nativeWindow);
        if (!sizesMatchWithinOnePhysicalPixel(
                stableMoveVisualSize,
                snapshotPhysicalSize(window.visualContentRect().size(), currentWindowDpi))) {
            failures.push_back(
                "visible toolbar content changed physical size during the monitor move");
            break;
        }
        const HMONITOR windowMonitor = MonitorFromWindow(nativeWindow, MONITOR_DEFAULTTONULL);
        reachedDestination = reachedDestination || windowMonitor == destination->handle;
        observedDpiTransition = observedDpiTransition || currentWindowDpi != sourceWindowDpi;
    }

    if (!expectedFinalTopLeft.isNull() &&
        !waitForNativePosition(nativeWindow, expectedFinalTopLeft, 3000)) {
        const QPoint actualFinalTopLeft = nativeWindowGeometry(nativeWindow).topLeft();
        failures.push_back("toolbar HWND did not finish at the requested destination position: expected " +
                           std::to_string(expectedFinalTopLeft.x()) + "," +
                           std::to_string(expectedFinalTopLeft.y()) + " but reached " +
                           std::to_string(actualFinalTopLeft.x()) + "," +
                           std::to_string(actualFinalTopLeft.y()));
    }
    ScreenshotFloatingToolPaletteWindowTestAccess::finishDrag(window);
    if (nativeWindowSize(nativeWindow) != stablePhysicalSize) {
        failures.push_back("toolbar physical pixel size changed after crossing monitors");
    }
    window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    settleQueuedRefreshes();

    const qreal expectedDestinationScale =
        sourceScreen->devicePixelRatio() / destinationScreen->devicePixelRatio();
    if (!qFuzzyCompare(window.paletteHost()->physicalScale() + 1.0,
                       expectedDestinationScale + 1.0)) {
        failures.push_back(
            "style toolbar did not retain the selection display as its scale reference");
    }
    if (!window.palette()->styleToolbarVisible() || window.palette()->actionToolbarVisible()) {
        failures.push_back("Shape should show only the style toolbar on the destination display");
    }
    if (nativeWindowSize(nativeWindow) != stablePhysicalSize) {
        failures.push_back("toolbar physical frame size changed after activating Shape");
    }
    const QWidget* shapeControls = window.palette()->findChild<QWidget*>(
        QStringLiteral("screenshotRectangleStyleControls"));
    if (shapeControls == nullptr) {
        failures.push_back("shape style controls should exist after activating the shape tool");
    } else {
        const int rowTop = shapeControls->rect().top();
        if (shapeColorTriggerTop(QStringLiteral("Stroke color")) != rowTop ||
            shapeColorTriggerTop(QStringLiteral("Fill color")) != rowTop) {
            failures.push_back(
                "shape color editor triggers should stay aligned after activating Shape");
        }
    }
    const qreal destinationWindowDpi = GetDpiForWindow(nativeWindow);
    appendSecondaryPanelLayoutFailure(shapeStylePanel, destinationWindowDpi,
                                      "Shape toolbar on destination display", &failures);
    const ToolbarSizeSnapshot destinationShapeSizes =
        captureToolbarSizeSnapshot(window, shapeStylePanel, destinationWindowDpi);
    appendToolbarSizeFailures(sourceShapeSizes, destinationShapeSizes,
                              "Shape toolbar on destination display", &failures);

    window.setActiveTool(ScreenshotToolPalette::Tool::Select);
    settleQueuedRefreshes();
    if (window.palette()->activeToolForTests() != ScreenshotToolPalette::Tool::Select) {
        failures.push_back("toolbar did not switch to the Select tool on the destination display");
    }
    if (!window.palette()->actionToolbarVisible() || window.palette()->styleToolbarVisible()) {
        failures.push_back("Select should show only the action toolbar on the destination display");
    }
    if (nativeWindowSize(nativeWindow) != stablePhysicalSize) {
        failures.push_back("toolbar physical frame size changed after activating Select");
    }
    appendSecondaryPanelLayoutFailure(selectActionPanel, destinationWindowDpi,
                                      "Select toolbar on destination display", &failures);
    const ToolbarSizeSnapshot destinationSelectSizes =
        captureToolbarSizeSnapshot(window, selectActionPanel, destinationWindowDpi);
    appendToolbarSizeFailures(sourceSelectSizes, destinationSelectSizes,
                              "Select toolbar on destination display", &failures);
    appendMainToolbarSizeFailures(destinationShapeSizes, destinationSelectSizes,
                                  "when switching from Shape to Select on the destination display",
                                  &failures);

    const QPoint returnStart = finish;
    const QPoint returnFinish = start;
    const QRect returnInitialGeometry = nativeWindowGeometry(nativeWindow);
    const QPoint returnCursorToWindowOffset = returnStart - returnInitialGeometry.topLeft();
    const QSize stableSelectVisualSize = destinationSelectSizes.visualContentSize;
    if (SetCursorPos(returnStart.x(), returnStart.y()) == FALSE) {
        failures.push_back("failed to position the cursor before returning to the source monitor");
    }
    ScreenshotFloatingToolPaletteWindowTestAccess::beginPhysicalDrag(window, QCursor::pos());
    if (!ScreenshotFloatingToolPaletteWindowTestAccess::hasPhysicalDragAnchor(window)) {
        failures.push_back("toolbar did not restart a native physical drag after switching to Select");
    }

    const int returnDistance =
        qMax(qAbs(returnFinish.x() - returnStart.x()), qAbs(returnFinish.y() - returnStart.y()));
    const int returnSteps = qMax(1, returnDistance / 2);
    QPoint expectedReturnTopLeft;
    for (int step = 1; step <= returnSteps; ++step) {
        const QPoint cursor(
            returnStart.x() +
                qRound(static_cast<qreal>(returnFinish.x() - returnStart.x()) * step / returnSteps),
            returnStart.y() +
                qRound(static_cast<qreal>(returnFinish.y() - returnStart.y()) * step / returnSteps));
        if (SetCursorPos(cursor.x(), cursor.y()) == FALSE) {
            failures.push_back("failed to move the hardware cursor back to the source monitor");
            break;
        }
        ScreenshotFloatingToolPaletteWindowTestAccess::updateDrag(window, QCursor::pos());
        settleQueuedRefreshes();

        POINT actualCursor{};
        if (GetCursorPos(&actualCursor) == FALSE) {
            failures.push_back("failed to read the physical cursor during the return move");
            break;
        }
        expectedReturnTopLeft = QPoint(actualCursor.x - returnCursorToWindowOffset.x(),
                                       actualCursor.y - returnCursorToWindowOffset.y());
        waitForNativePosition(nativeWindow, expectedReturnTopLeft, 10);
        if (nativeWindowSize(nativeWindow) != stablePhysicalSize) {
            failures.push_back("toolbar physical frame size changed during the return monitor move");
        }
        const qreal returnWindowDpi = GetDpiForWindow(nativeWindow);
        if (!sizesMatchWithinOnePhysicalPixel(
                stableSelectVisualSize,
                snapshotPhysicalSize(window.visualContentRect().size(), returnWindowDpi))) {
            failures.push_back(
                "Select visible content changed physical size during the return monitor move");
            break;
        }
    }
    if (!expectedReturnTopLeft.isNull() &&
        !waitForNativePosition(nativeWindow, expectedReturnTopLeft, 3000)) {
        failures.push_back("toolbar HWND did not finish at the source position after returning");
    }
    ScreenshotFloatingToolPaletteWindowTestAccess::finishDrag(window);
    settleQueuedRefreshes();
    if (MonitorFromWindow(nativeWindow, MONITOR_DEFAULTTONULL) != source->handle) {
        failures.push_back("Select toolbar did not return to the source monitor");
    }
    if (!qFuzzyCompare(window.paletteHost()->physicalScale() + 1.0, 2.0)) {
        failures.push_back("Select toolbar did not restore its reference scale after returning");
    }
    if (!window.palette()->actionToolbarVisible() || window.palette()->styleToolbarVisible()) {
        failures.push_back("Select should show only the action toolbar after returning");
    }
    const qreal returnedWindowDpi = GetDpiForWindow(nativeWindow);
    appendSecondaryPanelLayoutFailure(selectActionPanel, returnedWindowDpi,
                                      "Select toolbar on source display after return", &failures);
    const ToolbarSizeSnapshot returnedSelectSizes =
        captureToolbarSizeSnapshot(window, selectActionPanel, returnedWindowDpi);
    appendToolbarSizeFailures(sourceSelectSizes, returnedSelectSizes,
                              "Select toolbar on source display after return", &failures);
    window.hide();
    settleQueuedRefreshes();

    if (!reachedDestination) {
        failures.push_back("toolbar HWND never reached the destination monitor");
    }
    if (!observedDpiTransition) {
        failures.push_back("toolbar HWND did not receive the destination monitor DPI");
    }
    if (geometryWarningScope.emitted()) {
        failures.push_back("mixed-DPI drag emitted QWindowsWindow::setGeometry warning");
    }
    if (!failures.empty()) {
        std::ostringstream message;
        for (int index = 0; index < static_cast<int>(failures.size()); ++index) {
            if (index != 0) {
                message << "\n";
            }
            message << failures.at(index);
        }
        throw std::runtime_error(message.str());
    }
#endif
}

void physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable() {
    physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable(false);
}

void physicalDragFromDestinationMonitorAndBackKeepsPhysicalGeometryStable() {
    physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable(true);
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
    constexpr ScreenshotToolPalette::Tool tools[] = {
        ScreenshotToolPalette::Tool::Select,       ScreenshotToolPalette::Tool::Shape,
        ScreenshotToolPalette::Tool::Arrow,        ScreenshotToolPalette::Tool::Text,
        ScreenshotToolPalette::Tool::SerialNumber,
    };
    require(window.palette()->ensureActionFamily(
                ScreenshotToolPalette::ActionFamily::Selection),
            "preset test should materialize the inspected selection actions");
    for (const ScreenshotToolPalette::Tool tool : tools) {
        if (tool != ScreenshotToolPalette::Tool::Select) {
            require(window.palette()->ensureStyleFamily(tool),
                    "preset test should materialize every inspected style family");
        }
    }
    window.prepareForDisplay();
    const QSize presetWindowSize = window.size();

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
    constexpr ScreenshotToolPalette::Tool referenceFamilies[] = {
        ScreenshotToolPalette::Tool::Shape,
        ScreenshotToolPalette::Tool::Arrow,
        ScreenshotToolPalette::Tool::RectangleHighlight,
        ScreenshotToolPalette::Tool::PenHighlight,
        ScreenshotToolPalette::Tool::Spotlight,
        ScreenshotToolPalette::Tool::Text,
        ScreenshotToolPalette::Tool::SerialNumber,
        ScreenshotToolPalette::Tool::RectangleFilter,
        ScreenshotToolPalette::Tool::PenFilter,
        ScreenshotToolPalette::Tool::Watermark,
    };
    for (const ScreenshotToolPalette::Tool tool : referenceFamilies) {
        require(window.palette()->ensureStyleFamily(tool),
                "placement test should materialize its reference style families");
    }
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

    window.setActiveTool(ScreenshotToolPalette::Tool::Move);
    settleQueuedRefreshes();
        const QRect movePlacementRect = window.bottomPlacementContentRect();
        require(movePlacementRect == palette->mainToolbarContentRect() &&
                    movePlacementRect != window.fullContentRect(),
                "an editorless tool should exclude hidden secondary rows from placement");

    window.setActiveTool(ScreenshotToolPalette::Tool::Text);
    settleQueuedRefreshes();
    window.setStyleToolbarAboveMain(true);
    settleQueuedRefreshes();
    require(window.topPlacementContentRect() == expectedVisibleRect(),
            "top placement should use the displayed style toolbar size");
    require(window.windowSizeHint() == presetWindowSize,
            "actual placement extents must not resize the preset toolbar window");
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

    window.resetPositionForSelection(window.contentPosition());
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

void requireDynamicToolbarContentFits(ScreenshotFloatingToolPaletteWindow& window,
                                      const char* description) {
    const QRect outerRect = window.rect();
    const auto requireWidgetFits = [&](const QWidget* widget) {
        if (widget == nullptr || widget->size().isEmpty()) {
            return;
        }
        const QRect widgetRect(widget->mapTo(&window, QPoint(0, 0)), widget->size());
        require(outerRect.contains(widgetRect.topLeft()) &&
                    outerRect.contains(widgetRect.bottomRight()),
                description);
    };

    requireWidgetFits(window.paletteHost());
    if (ScreenshotToolPalette* palette = window.palette()) {
        requireWidgetFits(palette->mainPanel());
        requireWidgetFits(palette->actionPanel());
        requireWidgetFits(palette->stylePanel());
    }
}

void floatingToolbarsUseTheFixedWindowPreset() {
    constexpr QSize normalPreset(1042, 142);
    constexpr QSize smallPreset(834, 114);

    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow screenshotToolbar(commands);
    screenshotToolbar.setToolbarSize(QStringLiteral("normal"));
    screenshotToolbar.prepareForDisplay();
    require(screenshotToolbar.windowSizeHint() == normalPreset &&
                screenshotToolbar.size() == normalPreset,
            "screenshot toolbar should use the fixed normal window preset");
    require(screenshotToolbar.palette()->findChild<QWidget*>(
                QStringLiteral("screenshotStyleToolbarReserve")) == nullptr,
            "screenshot toolbar should not create a placeholder reserve control");
    requireDynamicToolbarContentFits(
        screenshotToolbar, "screenshot toolbar content must fit the fixed normal preset");
    const QRect bounds(0, 0, 1920, 1080);
    screenshotToolbar.setPlacementContext(nullptr, bounds, bounds);
    const QPoint anchor(1400, 1060);
    const ScreenshotToolbarPlacementSnapshot initialSnapshot =
        screenshotToolbar.placementSnapshot();
    const auto initialPlacement = ScreenshotGeometryMapper::anchoredToolbarPlacement(
        anchor, QPoint(anchor.x(), 800), initialSnapshot.bottom, initialSnapshot.top, bounds, 4);
    screenshotToolbar.setStyleToolbarAboveMain(initialPlacement.usesTopRightPlacement);
    screenshotToolbar.resetPositionForSelection(initialPlacement.contentPosition);
    screenshotToolbar.palette()->setActiveTool(ScreenshotToolPalette::Tool::Shape);
    settleQueuedRefreshes();

    const QMargins shadowMargins = ScreenshotToolPaletteHost::defaultShadowMargins();
    screenshotToolbar.setStyleToolbarAboveMain(false);
    settleQueuedRefreshes();
    ScreenshotToolbarPlacementSnapshot snapshot = screenshotToolbar.placementSnapshot();
    const QRect bottomMain =
        snapshot.bottom.mainToolbarContentRect.translated(snapshot.contentOffset);
    require(bottomMain.right() == normalPreset.width() - shadowMargins.right() - 1 &&
                bottomMain.top() == shadowMargins.top(),
            "the normal arrangement should anchor the main row to the frame top-right");

    screenshotToolbar.setStyleToolbarAboveMain(true);
    settleQueuedRefreshes();
    snapshot = screenshotToolbar.placementSnapshot();
    const QRect topMain = snapshot.top.mainToolbarContentRect.translated(snapshot.contentOffset);
    const QRect topSecondary =
        snapshot.top.secondaryToolbarContentRect.translated(snapshot.contentOffset);
    require(topMain.right() == normalPreset.width() - shadowMargins.right() - 1 &&
                topMain.bottom() == normalPreset.height() - shadowMargins.bottom() - 1,
            "the top arrangement should anchor the main row to the frame bottom-right");
    require(topSecondary.right() == topMain.right() &&
                topMain.top() - topSecondary.bottom() - 1 == 6,
            "the top arrangement should place the secondary row above the main row");

    screenshotToolbar.setToolbarSize(QStringLiteral("small"));
    screenshotToolbar.prepareForDisplay();
    require(screenshotToolbar.windowSizeHint() == smallPreset &&
                screenshotToolbar.size() == smallPreset,
            "screenshot toolbar should use the rounded small window preset");
    requireDynamicToolbarContentFits(
        screenshotToolbar, "screenshot toolbar content must fit the fixed small preset");

    ScreenshotFloatingToolPaletteWindow recordingToolbar(recordingToolbarOptionsForPresetTest());
    recordingToolbar.prepareForDisplay();
    require(recordingToolbar.windowSizeHint() == normalPreset &&
                recordingToolbar.size() == normalPreset,
            "screen recording toolbar should use the fixed normal window preset");
    requireDynamicToolbarContentFits(
        recordingToolbar, "screen recording toolbar content must fit the fixed preset");

    ScreenshotFloatingToolPaletteWindow pinnedToolbar(testToolbarOptions());
    pinnedToolbar.prepareForDisplay();
    require(pinnedToolbar.windowSizeHint() == normalPreset &&
                pinnedToolbar.size() == normalPreset,
            "pinned toolbar should use the fixed normal window preset");
    requireDynamicToolbarContentFits(pinnedToolbar,
                                     "pinned toolbar content must fit the fixed preset");
}

void firstDisplayUsesThePreparedToolbarGeometry() {
    constexpr int toolbarGap = 4;
    const QRect bounds(0, 0, 1920, 1080);
    const QPoint bottomRightAnchor(1400, 900);
    const QPoint topRightAnchor(bottomRightAnchor.x(), 700);

    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.prepareForDisplay();

    require(window.palette()->findChild<QWidget*>(
                QStringLiteral("screenshotStyleToolbarReserve")) == nullptr,
            "first-display toolbar should not create a reserve control");
    window.setPlacementContext(nullptr, bounds, bounds);
    const ScreenshotToolbarPlacementSnapshot preparedSnapshot = window.placementSnapshot();
    const auto placement = ScreenshotGeometryMapper::anchoredToolbarPlacement(
        bottomRightAnchor, topRightAnchor, preparedSnapshot.bottom, preparedSnapshot.top, bounds,
        toolbarGap);
    require(!placement.usesTopRightPlacement,
            "first-display regression should use the bottom-right arrangement");

    window.setStyleToolbarAboveMain(placement.usesTopRightPlacement);
    window.resetPositionForSelection(placement.contentPosition);
    const QRect expectedMain =
        preparedSnapshot.bottom.mainToolbarContentRect.translated(placement.contentPosition);
    require(window.paletteHost()->pos() == QPoint(0, 0) &&
                window.paletteHost()->size() == window.windowSizeHint(),
            "first-display toolbar host should occupy the fixed frame at the origin");
    require(expectedMain.top() - bottomRightAnchor.y() - 1 == toolbarGap,
            "first-display toolbar should preserve the requested bottom gap");

    window.show();
    settleQueuedRefreshes();

    const ScreenshotToolbarPlacementSnapshot displayedSnapshot = window.placementSnapshot();
    const QRect displayedMain =
        displayedSnapshot.bottom.mainToolbarContentRect.translated(window.contentPosition());
    const QRect actualMain(window.palette()->mainPanel()->mapToGlobal(QPoint(0, 0)),
                           window.palette()->mainPanel()->size());
    require(actualMain == expectedMain,
            "first display should place the main toolbar at the prepared global rectangle");
    require(displayedMain == expectedMain,
            "showing the toolbar should not change its prepared content geometry");
    require(displayedMain.top() - bottomRightAnchor.y() - 1 == toolbarGap,
            "first display should retain the requested bottom gap");
    window.hide();
}

void captureResetRestoresTheNormalFrameAnchor() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.prepareForDisplay();
    window.setActiveTool(ScreenshotToolPalette::Tool::Text);
    settleQueuedRefreshes();
    window.setStyleToolbarAboveMain(true);
    settleQueuedRefreshes();

    const QSize frameSize = window.windowSizeHint();
    const int topPlacementY = frameSize.height() - window.palette()->height();
    require(window.paletteHost()->pos() == QPoint(0, 0) &&
                window.palette()->y() == topPlacementY,
            "top placement should anchor the palette to the fixed frame bottom");

    window.resetForNewCapture();
    settleQueuedRefreshes();

    const QMargins shadowMargins = ScreenshotToolPaletteHost::defaultShadowMargins();
    require(window.paletteHost()->pos() == QPoint(0, 0) &&
                window.paletteHost()->size() == frameSize,
            "capture reset should restore the host to the fixed frame origin");
    const ScreenshotToolbarPlacementSnapshot snapshot = window.placementSnapshot();
    const QRect mainRect = snapshot.bottom.mainToolbarContentRect.translated(snapshot.contentOffset);
    require(mainRect.top() == shadowMargins.top() &&
                mainRect.right() == frameSize.width() - shadowMargins.right() - 1,
            "capture reset should leave the main toolbar flush with the frame's normal anchor");
}

void toolbarNativeSurfaceCanBeRetiredAndRestored() {
    NoOpToolbarCommands commands;
    QWidget owner;
    owner.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    owner.resize(640, 360);
    owner.show();
    settleQueuedRefreshes();

    ScreenshotToolbarWindow window(commands);
    window.setOwnerWindow(&owner);
    window.prepareForDisplay();
    window.show();
    settleQueuedRefreshes();
    require(window.internalWinId() != 0 && window.testAttribute(Qt::WA_WState_Created),
            "toolbar lifecycle test must begin with a live native surface");

    ScreenshotToolPalette* const palette = window.palette();
    window.releaseNativeSurface();
    require(window.palette() == palette,
            "retiring a toolbar surface must retain the palette object graph");
    require(window.internalWinId() == 0 && !window.testAttribute(Qt::WA_WState_Created),
            "retiring a toolbar must synchronously release its native surface");
    window.releaseNativeSurface();
    require(window.internalWinId() == 0,
            "retiring an already retired toolbar must be idempotent");

    window.restoreNativeSurface();
    window.restoreNativeSurface();
    require(window.internalWinId() != 0 && window.testAttribute(Qt::WA_WState_Created) &&
                !window.isVisible(),
            "restoring a toolbar must recreate a hidden native surface");
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(GetWindow(toNativeHwnd(window.internalWinId()), GW_OWNER) ==
                toNativeHwnd(owner.internalWinId()),
            "restoring a toolbar must restore its native owner");
#endif
    window.show();
    settleQueuedRefreshes();
    require(window.isVisible() && window.palette() == palette,
            "a restored toolbar must show with its retained palette");
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
    require(translation != nullptr, "the main Text translation control should be present");

    translation->click();
    require(commands.textTranslationToolCount == 1 &&
                window.palette()->activeToolForTests() ==
                    ScreenshotToolPalette::Tool::TextTranslation,
            "the main Text translation control should activate the translation presentation");
    window.setOcrBusy(true);
    require(translation->busy() && (recognition == nullptr || !recognition->busy()),
            "recognition for Text translation should load on the translation control");
    window.setOcrBusy(false);
    window.setTextTranslationState(true, true, true);
    require(translation->busy(),
            "streaming translation should load on the main translation control");
    window.setTextTranslationState(true, true, false);
    require(!translation->busy(),
            "the main translation control should stop loading when streaming completes");
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
        if (app.arguments().contains(QStringLiteral("--native-surface-lifecycle-only"))) {
            toolbarNativeSurfaceCanBeRetiredAndRestored();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--reverse-hardware-drag-only"))) {
            physicalDragFromDestinationMonitorAndBackKeepsPhysicalGeometryStable();
            return 0;
        }
        logicalDragMovesWithoutRefreshingGeometry();
        physicalDragMovesWithoutRefreshingGeometry();
        std::vector<std::string> hardwareDragFailures;
        for (const bool reverseDirection : {false, true}) {
            try {
                physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable(reverseDirection);
            } catch (const std::exception& error) {
                hardwareDragFailures.push_back(
                    std::string(reverseDirection ? "B-to-A-to-B: " : "A-to-B-to-A: ") +
                    error.what());
            }
        }
        if (!hardwareDragFailures.empty()) {
            std::ostringstream message;
            for (int index = 0; index < static_cast<int>(hardwareDragFailures.size()); ++index) {
                if (index != 0) {
                    message << "\n";
                }
                message << hardwareDragFailures.at(index);
            }
            throw std::runtime_error(message.str());
        }
        dpiScaledSizeMessagePreservesThePhysicalWindowSize();
        logicalMetricsIgnorePhysicalPlacementBounds();
        styleToolChangesKeepThePresetWindowSize();
        placementRectsTrackTheDisplayedStyleToolbar();
        toolChangesRepositionOnlyBeforeManualDrag();
        unchangedShadowMarginsAreNoOps();
        screenshotToolbarSizeMultiplierSurvivesCaptureReset();
        floatingToolbarsUseTheFixedWindowPreset();
        firstDisplayUsesThePreparedToolbarGeometry();
        captureResetRestoresTheNormalFrameAnchor();
        toolbarNativeSurfaceCanBeRetiredAndRestored();
        translateButtonRoutesEveryClickThroughTheToggleCommand();
        mainTextTranslationButtonUsesTranslationPresentation();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
