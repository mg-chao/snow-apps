#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/button.h"
#include "widgets/radio_button_group.h"
#include "widgets/popover.h"
#include "widgets/detail/overlay_popup_surface.h"
#include <QPushButton>
#include "widgets/dpi_stable_window_controller.h"

#include <QAbstractButton>
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QLayout>
#include <QLineEdit>
#include <QLabel>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QThread>
#include <QTemporaryDir>
#include <QWindow>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#ifndef WM_GETDPISCALEDSIZE
#define WM_GETDPISCALEDSIZE 0x02E4
#endif
#endif

#if defined(Q_OS_MACOS)
#include "macos_native_input.h"
#include "snow_shot/platform/screenshotnative.h"
#endif

class ScreenshotFloatingToolPaletteWindowTestAccess {
  public:
    static void simulateDpr(ScreenshotFloatingToolPaletteWindow& window, qreal dpr) {
        window.m_testWindowDevicePixelRatio = dpr;
        QEvent change(QEvent::DevicePixelRatioChange);
        QApplication::sendEvent(&window, &change);
    }

    static bool hasPhysicalBaseline(const ScreenshotFloatingToolPaletteWindow& window) {
        return window.m_referenceDevicePixelRatio > 0.0 ||
               (window.m_dpiController != nullptr && window.m_dpiController->hasBaseline());
    }

    static void beginKeyboardFocus(ScreenshotFloatingToolPaletteWindow& window, QWidget* editor) {
        window.beginKeyboardFocusInteraction(editor);
    }

    static void endKeyboardFocus(ScreenshotFloatingToolPaletteWindow& window, QWidget* editor) {
        window.endKeyboardFocusInteraction(editor);
    }

    static bool keyboardFocusActive(const ScreenshotFloatingToolPaletteWindow& window,
                                    const QWidget* editor) {
        return window.m_keyboardFocusInteractionActive &&
               (editor == nullptr || window.m_keyboardFocusEditor == editor);
    }

    static void beginLogicalDrag(ScreenshotFloatingToolPaletteWindow& window,
                                 const QPoint& globalPosition) {
        window.m_draggingPalette = true;
        if (window.m_dpiController != nullptr)
            window.m_dpiController->endPhysicalDrag();
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
        return window.physicalDragActive();
    }

    static quint64 geometryRefreshCount(const ScreenshotFloatingToolPaletteWindow& window) {
        return window.m_paletteGeometryRefreshCount;
    }

    static quint64 committedGeometryPassCount(const ScreenshotFloatingToolPaletteWindow& window) {
        return window.m_committedGeometryPassCount;
    }

    static bool isPointInInteractiveContent(const ScreenshotFloatingToolPaletteWindow& window,
                                            const QPoint& localPosition) {
        return window.isPointInInteractiveContent(localPosition);
    }
};

namespace {
#if defined(Q_OS_WIN) || defined(_WIN32)
class LayeredSurfaceMonitor final : public QAbstractNativeEventFilter {
  public:
    explicit LayeredSurfaceMonitor(WId id) : m_id(id) {
        qApp->installNativeEventFilter(this);
    }
    ~LayeredSurfaceMonitor() override {
        qApp->removeNativeEventFilter(this);
    }
    int resets = 0;

    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
        const auto* msg = static_cast<MSG*>(message);
        if (msg->hwnd == reinterpret_cast<HWND>(m_id) && msg->message == WM_STYLECHANGED &&
            msg->wParam == static_cast<WPARAM>(GWL_EXSTYLE)) {
            const auto* style = reinterpret_cast<const STYLESTRUCT*>(msg->lParam);
            if ((style->styleOld & WS_EX_LAYERED) != 0 && (style->styleNew & WS_EX_LAYERED) == 0) {
                ++resets;
            }
        }
        return false;
    }

  private:
    WId m_id;
};
#endif
std::atomic_bool nativeGeometryWarningEmitted{false};
std::atomic_bool nonFocusableActivationWarningEmitted{false};
QtMessageHandler previousMessageHandler = nullptr;

void captureNativeGeometryWarning(QtMsgType type, const QMessageLogContext& context,
                                  const QString& message) {
    if (type == QtWarningMsg && message.contains(QStringLiteral("QWindowsWindow::setGeometry"))) {
        nativeGeometryWarningEmitted.store(true, std::memory_order_relaxed);
    }
    if (type == QtWarningMsg && message.contains(QStringLiteral("WindowDoesNotAcceptFocus")))
        nonFocusableActivationWarningEmitted.store(true, std::memory_order_relaxed);

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
        nonFocusableActivationWarningEmitted.store(false, std::memory_order_relaxed);
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
    int selectionUnitCommands = 0;
    void setSelectionDisplayUnit(ScreenshotSelectionDisplayUnit unit) override {
        ++selectionUnitCommands;
        static_cast<void>(snow_shot::storage::ScreenshotUiSettings().setSelectionDisplayUnit(
            screenshotSelectionDisplayUnitId(unit)));
    }
    int quickSaveCount = 0;
    void quickSaveSelection() override {
        ++quickSaveCount;
    }

    void setMoveTool() override {}
    void setSelectTool() override {}
    void deleteAllElements() override {
        ++deleteAllElementsCount;
    }
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
    void jumpToTranslationPage() override {
        ++jumpToTranslationPageCount;
        // Mirrors the real sink: the command ends the capture, whose teardown
        // resets the toolbar and evicts the secondary contents synchronously.
        if (jumpToTranslationPageResetTarget != nullptr) {
            jumpToTranslationPageResetTarget->resetForNewCapture();
        }
    }
    void startScrollingScreenshot() override {}
    void pinSelectionToScreen() override {
        ++pinSelectionCount;
    }
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&, quint32) override {}
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSerialNumbers() override {}
    void incrementSelectedSerialNumbers() override {}
    void createTextForSelectedSerialNumber() override {}
    void repositionToolbarForContentChange() override {
        ++repositionCount;
    }
    void repositionToolbarForPresentationChange() override {
        ++presentationRepositionCount;
    }
    void hideColorPickersForScreenshotUi() override {}

    int repositionCount = 0;
    int pinSelectionCount = 0;
    int deleteAllElementsCount = 0;
    int presentationRepositionCount = 0;
    int textTranslationToolCount = 0;
    int textTranslationToggleCount = 0;
    int jumpToTranslationPageCount = 0;
    ScreenshotToolbarWindow* jumpToTranslationPageResetTarget = nullptr;
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

// Locates the mixed-DPI bench layout the hardware drag scenarios need: a 150%
// monitor A whose left edge is the right edge of a vertically overlapping 100%
// monitor B. Returns false when the machine does not provide that layout; the
// callers then skip instead of failing, because the bench is a lab-machine
// prerequisite rather than a property of the code under test.
bool findMixedDpiMonitorBench(const std::vector<HardwareMonitor>& monitors,
                              const HardwareMonitor** monitorA, const HardwareMonitor** monitorB) {
    constexpr UINT kMonitorADpi = 144;
    constexpr UINT kMonitorBDpi = 96;
    for (const HardwareMonitor& candidateA : monitors) {
        if (candidateA.dpi != kMonitorADpi) {
            continue;
        }
        for (const HardwareMonitor& candidateB : monitors) {
            const bool verticallyOverlaps = candidateB.bounds.top < candidateA.bounds.bottom &&
                                            candidateB.bounds.bottom > candidateA.bounds.top;
            if (candidateB.dpi == kMonitorBDpi &&
                candidateB.bounds.right == candidateA.bounds.left && verticallyOverlaps) {
                *monitorA = &candidateA;
                *monitorB = &candidateB;
                return true;
            }
        }
    }
    return false;
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
    return qAbs(left.width() - right.width()) <= 1 && qAbs(left.height() - right.height()) <= 1;
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
    require(palette != nullptr && mainPanel != nullptr,
            "toolbar size snapshot lacks its main panel");

    ToolbarSizeSnapshot snapshot;
    snapshot.visualContentSize = snapshotPhysicalSize(window.visualContentRect().size(), dpi);
    snapshot.mainToolbarContentSize =
        snapshotPhysicalSize(palette->mainToolbarContentRect().size(), dpi);
    snapshot.mainPanelSize = snapshotPhysicalSize(mainPanel->size(), dpi);
    snapshot.secondaryPanelSize =
        secondaryPanel != nullptr ? snapshotPhysicalSize(secondaryPanel->size(), dpi) : QSize();
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
    message << stateDescription << " " << component.toStdString() << " physical size changed from "
            << expectedSize.width() << "x" << expectedSize.height() << " to " << actualSize.width()
            << "x" << actualSize.height() << " (more than 1px)";
    failures->push_back(message.str());
}

void appendMainToolbarSizeFailures(const ToolbarSizeSnapshot& expected,
                                   const ToolbarSizeSnapshot& actual, const char* stateDescription,
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
                               const ToolbarSizeSnapshot& actual, const char* stateDescription,
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

void settleQueuedRefreshes();

void recordingExportSettingsParticipateInNativeHitTesting() {
    ScreenshotToolPalette::Options options = recordingToolbarOptionsForPresetTest();
    options.showShapeTool = true;
    options.recordingDrawingMode = true;
    options.enableStyleToolbar = true;

    ScreenshotFloatingToolPaletteWindow window(options);
    window.prepareForDisplay();
    window.show();
    settleQueuedRefreshes();

    ScreenshotToolPalette* palette = window.palette();
    auto* exportButton = palette != nullptr ? palette->findChild<adqt::widgets::AdButton*>(
                                                  QStringLiteral("screenRecordingExportSettings"))
                                            : nullptr;
    require(exportButton != nullptr, "recording hit-test fixture should expose Export Settings");
    exportButton->click();
    settleQueuedRefreshes();
    require(palette->recordingExportSettingsVisible() &&
                palette->recordingExportSettingsPanel()->isVisible(),
            "clicking active Export Settings must retain the floating secondary toolbar");
    exportButton->click();
    settleQueuedRefreshes();

    QWidget* exportPanel = palette->recordingExportSettingsPanel();
    auto* format =
        exportPanel != nullptr
            ? exportPanel->findChild<QWidget*>(QStringLiteral("screenRecordingOutputFormat"))
            : nullptr;
    auto* cursor =
        exportPanel != nullptr
            ? exportPanel->findChild<QWidget*>(QStringLiteral("screenRecordingShowCursor"))
            : nullptr;
    require(exportPanel != nullptr && exportPanel->isVisible() &&
                palette->recordingExportSettingsVisible() && format != nullptr && cursor != nullptr,
            "opening Export Settings should expose its complete secondary toolbar");

    const QPoint formatCenter =
        format->mapTo(&window, QPoint(format->width() / 2, format->height() / 2));
    const QPoint cursorCenter =
        cursor->mapTo(&window, QPoint(cursor->width() / 2, cursor->height() / 2));
    require(ScreenshotFloatingToolPaletteWindowTestAccess::isPointInInteractiveContent(
                window, formatCenter) &&
                ScreenshotFloatingToolPaletteWindowTestAccess::isPointInInteractiveContent(
                    window, cursorCenter),
            "native hit-testing should retain clicks over every Export Settings control");
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

void physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable(
    [[maybe_unused]] bool reverseDirection) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const NativeGeometryWarningScope geometryWarningScope;
    std::vector<HardwareMonitor> monitors;
    require(EnumDisplayMonitors(nullptr, nullptr, collectHardwareMonitor,
                                reinterpret_cast<LPARAM>(&monitors)) != FALSE,
            "failed to enumerate the hardware monitors");
    require(populateMonitorDpi(&monitors), "hardware test could not read effective monitor DPI");

    const HardwareMonitor* monitorA = nullptr;
    const HardwareMonitor* monitorB = nullptr;
    if (!findMixedDpiMonitorBench(monitors, &monitorA, &monitorB)) {
        std::cout << "SKIP: hardware drag scenario requires a 150% monitor immediately right of "
                     "a 100% monitor; this machine does not provide that bench layout\n";
        return;
    }

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
    auto* controller = window.findChild<adqt::widgets::AdDpiStableWindowController*>();
    require(controller != nullptr, "toolbar has no native geometry controller");
    const QSize referencePhysicalSize = controller->stablePhysicalFrameSize();
    const auto physicalFrameMatchesReference = [&](const QSize& actual) {
        // The reference must never be recaptured from a rounded drag frame.
        if (controller->stablePhysicalFrameSize() != referencePhysicalSize)
            return false;
        const qreal dpr = GetDpiForWindow(nativeWindow) / 96.0;
        const auto matches = [dpr](int value, int reference) {
            // Windows can preserve the exact native extent; a complete Qt layered
            // repaint can instead round through its integer logical client extent.
            // Exactly representable dimensions still require exact equality.
            return value == reference || value == qRound(qRound(reference / dpr) * dpr);
        };
        return matches(actual.width(), referencePhysicalSize.width()) &&
               matches(actual.height(), referencePhysicalSize.height());
    };
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
        if (!physicalFrameMatchesReference(settledNativeGeometry.size())) {
            failures.push_back(
                "toolbar physical pixel size changed from " +
                std::to_string(stablePhysicalSize.width()) + "x" +
                std::to_string(stablePhysicalSize.height()) + " to " +
                std::to_string(settledNativeGeometry.width()) + "x" +
                std::to_string(settledNativeGeometry.height()) +
                " during the monitor move at DPI " + std::to_string(GetDpiForWindow(nativeWindow)) +
                " drag=" + std::to_string(window.physicalDragActive()) +
                " baseline=" + std::to_string(controller->stablePhysicalFrameSize().height()));
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
        failures.push_back(
            "toolbar HWND did not finish at the requested destination position: expected " +
            std::to_string(expectedFinalTopLeft.x()) + "," +
            std::to_string(expectedFinalTopLeft.y()) + " but reached " +
            std::to_string(actualFinalTopLeft.x()) + "," + std::to_string(actualFinalTopLeft.y()));
    }
    ScreenshotFloatingToolPaletteWindowTestAccess::finishDrag(window);
    if (!physicalFrameMatchesReference(nativeWindowSize(nativeWindow))) {
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
    if (!physicalFrameMatchesReference(nativeWindowSize(nativeWindow))) {
        failures.push_back("toolbar physical frame size changed after activating Shape");
    }
    const QWidget* shapeControls =
        window.palette()->findChild<QWidget*>(QStringLiteral("screenshotRectangleStyleControls"));
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
    if (!physicalFrameMatchesReference(nativeWindowSize(nativeWindow))) {
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
        failures.push_back(
            "toolbar did not restart a native physical drag after switching to Select");
    }

    const int returnDistance =
        qMax(qAbs(returnFinish.x() - returnStart.x()), qAbs(returnFinish.y() - returnStart.y()));
    const int returnSteps = qMax(1, returnDistance / 2);
    QPoint expectedReturnTopLeft;
    for (int step = 1; step <= returnSteps; ++step) {
        const QPoint cursor(
            returnStart.x() +
                qRound(static_cast<qreal>(returnFinish.x() - returnStart.x()) * step / returnSteps),
            returnStart.y() + qRound(static_cast<qreal>(returnFinish.y() - returnStart.y()) * step /
                                     returnSteps));
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
        if (!physicalFrameMatchesReference(nativeWindowSize(nativeWindow))) {
            failures.push_back(
                "toolbar physical frame size changed during the return monitor move");
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

[[maybe_unused]] void physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable() {
    physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable(false);
}

void physicalDragFromDestinationMonitorAndBackKeepsPhysicalGeometryStable() {
    physicalDragAcrossHardwareMonitorsKeepsPhysicalGeometryStable(true);
}

// Regression scenario for a toolbar captured on the 150% monitor A whose frame is dragged
// across the seam shared with the 100% monitor B and back. The toolbar stays straddling the
// seam the whole time, so the majority of its frame - and with it the window DPI - flips on
// every crossing while the capture display remains the palette's scale reference. After the
// round trip the content must still render once at 150%, not at 150% * 150%.
void slowSeamStraddlingDragKeepsToolbarContentUnmagnified() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const NativeGeometryWarningScope geometryWarningScope;
    std::vector<HardwareMonitor> monitors;
    require(EnumDisplayMonitors(nullptr, nullptr, collectHardwareMonitor,
                                reinterpret_cast<LPARAM>(&monitors)) != FALSE,
            "failed to enumerate the hardware monitors");
    require(populateMonitorDpi(&monitors), "hardware test could not read effective monitor DPI");

    constexpr UINT kMonitorADpi = 144;
    constexpr UINT kMonitorBDpi = 96;
    const HardwareMonitor* monitorA = nullptr;
    const HardwareMonitor* monitorB = nullptr;
    if (!findMixedDpiMonitorBench(monitors, &monitorA, &monitorB)) {
        std::cout << "SKIP: seam-straddle scenario requires a 150% monitor immediately right of "
                     "a 100% monitor; this machine does not provide that bench layout\n";
        return;
    }

    const int seamX = monitorA->bounds.left;
    const int seamTop = qMax(monitorA->bounds.top, monitorB->bounds.top);
    const int seamBottom = qMin(monitorA->bounds.bottom, monitorB->bounds.bottom);
    require(seamBottom > seamTop, "hardware test requires the mixed-DPI monitors to overlap");
    const int seamY = seamTop + (seamBottom - seamTop) / 2;

    POINT originalCursor{};
    require(GetCursorPos(&originalCursor) != FALSE, "failed to save cursor position");
    const CursorPositionRestorer restoreCursor(originalCursor);

    QScreen* screenA = qtScreenForMonitor(*monitorA);
    require(screenA != nullptr, "could not map the capture display monitor to QScreen");

    QWidget overlayOwner;
    overlayOwner.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    overlayOwner.setAttribute(Qt::WA_TransparentForMouseEvents, true);
    overlayOwner.setWindowOpacity(0.0);
    overlayOwner.winId();
    require(overlayOwner.windowHandle() != nullptr, "test overlay did not create a native window");
    overlayOwner.windowHandle()->setScreen(screenA);
    overlayOwner.setGeometry(screenA->geometry());
    overlayOwner.show();
    settleQueuedRefreshes();

    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.resetForNewCapture();
    window.setPlacementContext(screenA, screenA->geometry(), monitorPhysicalBounds(*monitorA));
    window.setOwnerWindow(&overlayOwner);
    window.prepareForDisplay();
    window.show();
    settleQueuedRefreshes();
    window.resetForNewCapture();
    settleQueuedRefreshes();
    const HWND nativeWindow = toNativeHwnd(window.winId());
    require(IsWindow(nativeWindow) != FALSE, "toolbar did not create a native HWND");

    const QSize stablePhysicalSize = nativeWindowSize(nativeWindow);
    require(stablePhysicalSize.isValid() && !stablePhysicalSize.isEmpty(),
            "failed to measure the prepared toolbar HWND");
    require(GetDpiForWindow(nativeWindow) == kMonitorADpi,
            "seam test must start with the toolbar on the 150% monitor");
    require(qFuzzyCompare(window.paletteHost()->physicalScale() + 1.0, 2.0),
            "seam test must start with the toolbar at unit physical scale");
    const ToolbarSizeSnapshot initialSizes =
        captureToolbarSizeSnapshot(window, nullptr, kMonitorADpi);

    // Park the toolbar with its midpoint 20 physical pixels right of the monitor seam.
    const QPoint seamStraddleCursor(seamX + 20, seamY);
    require(SetCursorPos(seamStraddleCursor.x(), seamStraddleCursor.y()) != FALSE,
            "failed to park the cursor at the seam");
    require(SetWindowPos(nativeWindow, nullptr,
                         seamStraddleCursor.x() - stablePhysicalSize.width() / 2,
                         seamStraddleCursor.y() - stablePhysicalSize.height() / 2,
                         stablePhysicalSize.width(), stablePhysicalSize.height(),
                         SWP_NOACTIVATE | SWP_NOZORDER) != FALSE,
            "failed to park the toolbar across the seam");
    settleQueuedRefreshes();
    require(MonitorFromWindow(nativeWindow, MONITOR_DEFAULTTONULL) == monitorA->handle,
            "a seam-straddling toolbar should keep its majority on the 150% monitor");

    std::vector<std::string> failures;
    const auto slowlyDragCursorTo = [&](const QPoint& physicalTarget) {
        POINT current{};
        if (GetCursorPos(&current) == FALSE) {
            failures.push_back("failed to read the physical cursor during the seam drag");
            return;
        }
        const QPoint start(current.x, current.y);
        const int steps = qMax(1, qAbs(physicalTarget.x() - start.x()) / 2);
        for (int step = 1; step <= steps; ++step) {
            const QPoint cursor(
                start.x() +
                    qRound(static_cast<qreal>(physicalTarget.x() - start.x()) * step / steps),
                start.y() +
                    qRound(static_cast<qreal>(physicalTarget.y() - start.y()) * step / steps));
            if (SetCursorPos(cursor.x(), cursor.y()) == FALSE) {
                failures.push_back("failed to move the hardware cursor across the seam");
                return;
            }
            ScreenshotFloatingToolPaletteWindowTestAccess::updateDrag(window, QCursor::pos());
            settleQueuedRefreshes();
        }
    };

    // Slowly drag the toolbar midpoint to the left of the seam (onto the 100% monitor).
    ScreenshotFloatingToolPaletteWindowTestAccess::beginPhysicalDrag(window, QCursor::pos());
    require(ScreenshotFloatingToolPaletteWindowTestAccess::hasPhysicalDragAnchor(window),
            "toolbar did not start a native physical drag at the seam");
    slowlyDragCursorTo(QPoint(seamX - 40, seamY));
    ScreenshotFloatingToolPaletteWindowTestAccess::finishDrag(window);
    settleQueuedRefreshes();

    // Left of the seam the toolbar must keep the 150% capture display as its scale
    // reference: its content stretches by 150% in logical coordinates, keeping the
    // physical content size unchanged.
    if (GetDpiForWindow(nativeWindow) != kMonitorBDpi) {
        failures.push_back("toolbar did not adopt the 100% monitor DPI after crossing the seam");
    }
    if (!qFuzzyCompare(window.paletteHost()->physicalScale() + 1.0, 2.5)) {
        std::ostringstream message;
        message << "toolbar lost the 150% capture display as its scale reference left of the seam"
                << " (physical scale " << window.paletteHost()->physicalScale()
                << ", expected 1.5)";
        failures.push_back(message.str());
    }

    // Slowly drag the toolbar midpoint back to the right of the seam (onto the 150% monitor).
    ScreenshotFloatingToolPaletteWindowTestAccess::beginPhysicalDrag(window, QCursor::pos());
    if (!ScreenshotFloatingToolPaletteWindowTestAccess::hasPhysicalDragAnchor(window)) {
        failures.push_back("toolbar did not restart a native physical drag left of the seam");
    }
    slowlyDragCursorTo(seamStraddleCursor);
    ScreenshotFloatingToolPaletteWindowTestAccess::finishDrag(window);
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }

    if (MonitorFromWindow(nativeWindow, MONITOR_DEFAULTTONULL) != monitorA->handle) {
        failures.push_back("toolbar did not return its majority to the 150% monitor");
    }
    const UINT monitorAWindowDpi = GetDpiForWindow(nativeWindow);
    if (monitorAWindowDpi != kMonitorADpi) {
        failures.push_back("toolbar window did not readopt the 150% monitor DPI after returning");
    }
    if (nativeWindowSize(nativeWindow) != stablePhysicalSize) {
        const QSize finalSize = nativeWindowSize(nativeWindow);
        std::ostringstream message;
        message << "toolbar physical frame size changed from " << stablePhysicalSize.width() << "x"
                << stablePhysicalSize.height() << " to " << finalSize.width() << "x"
                << finalSize.height() << " across the seam round trip";
        failures.push_back(message.str());
    }

    // The reported defect: after the round trip the toolbar content renders at another 150%
    // on top of the 150% monitor scale and gets clipped by the fixed toolbar frame.
    const qreal finalPhysicalScale = window.paletteHost()->physicalScale();
    if (!qFuzzyCompare(finalPhysicalScale + 1.0, 2.0)) {
        std::ostringstream message;
        message << "toolbar content was magnified across the seam round trip: physical scale "
                << finalPhysicalScale << " instead of 1.0 (an extra 150% is still applied)";
        failures.push_back(message.str());
    }
    if (window.windowSizeHint() != window.size()) {
        std::ostringstream message;
        message << "toolbar frame no longer matches its fixed preset: committed window is "
                << window.size().width() << "x" << window.size().height() << " instead of "
                << window.windowSizeHint().width() << "x" << window.windowSizeHint().height();
        failures.push_back(message.str());
    }
    if (!window.rect().contains(QRect(QPoint(0, 0), window.paletteHost()->size()))) {
        std::ostringstream message;
        message << "toolbar content is clipped: palette host is "
                << window.paletteHost()->size().width() << "x"
                << window.paletteHost()->size().height() << " inside a " << window.rect().width()
                << "x" << window.rect().height() << " frame";
        failures.push_back(message.str());
    }
    const ToolbarSizeSnapshot finalSizes =
        captureToolbarSizeSnapshot(window, nullptr, monitorAWindowDpi);
    appendToolbarSizeFailures(initialSizes, finalSizes, "Seam round trip toolbar", &failures);
    if (geometryWarningScope.emitted()) {
        failures.push_back("seam drag emitted QWindowsWindow::setGeometry warning");
    }
    window.hide();
    settleQueuedRefreshes();

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

class ToolbarPaintExtentMonitor final : public QObject {
  public:
    explicit ToolbarPaintExtentMonitor(ScreenshotFloatingToolPaletteWindow& window)
        : m_window(window) {
        window.installEventFilter(this);
    }
    bool painted = false;
    bool clipped = false;

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            painted = true;
            clipped |= !m_window.rect().contains(m_window.paletteHost()->geometry());
        }
        return false;
    }

  private:
    ScreenshotFloatingToolPaletteWindow& m_window;
};

class ToolbarGeometryEventMonitor final : public QObject {
  public:
    int moves = 0;
    int resizes = 0;

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        moves += event->type() == QEvent::Move;
        resizes += event->type() == QEvent::Resize;
        return false;
    }
};

void toolSwitchPreservesNativeFrame() {
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "stable toolbar frame tests require isolated settings");
    {
        NoOpToolbarCommands commands;
        ScreenshotToolbarWindow window(commands);
        window.show();
        using Tool = ScreenshotToolPalette::Tool;
        for (const QString& size : {QStringLiteral("normal"), QStringLiteral("small")}) {
            window.setToolbarSize(size);
            for (bool above : {false, true}) {
                window.setActiveTool(Tool::Select);
                window.setStyleToolbarAboveMain(above);
                window.moveContentTo(QPoint(300, 300));
                settleQueuedRefreshes();
                const QRect frame = window.geometry();
                const QPoint mainPosition = window.palette()->mainPanel()->mapToGlobal(QPoint());
                ToolbarGeometryEventMonitor monitor;
                window.installEventFilter(&monitor);
                for (Tool tool : {Tool::Qr, Tool::Shape, Tool::Qr, Tool::Select, Tool::Qr}) {
                    window.setActiveTool(tool);
                    settleQueuedRefreshes();
                    require(window.geometry() == frame &&
                                window.palette()->mainPanel()->mapToGlobal(QPoint()) ==
                                    mainPosition,
                            "switching secondary rows must preserve the native frame and main row");
                    require(monitor.moves == 0 && monitor.resizes == 0,
                            "tool switches must not expose intermediate native moves or resizes");
                }
            }
        }
    }
    storage.shutdown();
}

void dpiCommitReconcilesTheActualFrameBeforePainting() {
#if !defined(Q_OS_MACOS)
    ScreenshotFloatingToolPaletteWindow window(testToolbarOptions());
    window.prepareForDisplay();
    window.show();
    settleQueuedRefreshes();
    auto* controller = window.findChild<adqt::widgets::AdDpiStableWindowController*>();
    require(controller != nullptr, "toolbar must have a DPI controller");
    ToolbarPaintExtentMonitor monitor(window);
    const QSize expected = window.windowSizeHint();
    // Model a native transition retaining an older, smaller frame while the
    // pooled toolbar has already prepared the new capture's content extent.
    window.resize(expected.width() / 2, expected.height());
    controller->requestScaleCommit();
    settleQueuedRefreshes();
    require(window.size() == expected && window.rect().contains(window.paletteHost()->geometry()),
            "a DPI commit must reconcile the actual frame with the prepared content");
    require(monitor.painted && !monitor.clipped,
            "the first repaint after a DPI commit must contain the complete toolbar");
#endif
}

void dpiCommitPresentsContentWhenUpdatesResume() {
#if !defined(Q_OS_MACOS)
    ScreenshotFloatingToolPaletteWindow window(testToolbarOptions());
    window.prepareForDisplay();
    window.show();
    settleQueuedRefreshes();
    auto* controller = window.findChild<adqt::widgets::AdDpiStableWindowController*>();
    require(controller != nullptr, "toolbar must have a DPI controller");
    ToolbarPaintExtentMonitor monitor(window);
    for (const bool hiddenDuringCommit : {false, true}) {
        if (hiddenDuringCommit) {
            window.hide();
        }
        monitor.painted = false;
        controller->requestScaleCommit();
        if (hiddenDuringCommit) {
            window.show();
        }
        // The frame is unchanged, as when logical toolbar dimensions stay the
        // same across monitors. A resize must not be needed to refresh its pixels.
        settleQueuedRefreshes();
        require(window.updatesEnabled() && monitor.painted && !monitor.clipped,
                "resuming updates after a DPI commit must present the complete toolbar "
                "even when its logical frame is unchanged");
    }
#endif
}

void reusedToolbarFitsOnFirstShowAcrossScreens() {
    QScreen* screenA = nullptr;
    QScreen* screenB = nullptr;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (qFuzzyCompare(screen->devicePixelRatio(), 1.5)) {
            screenA = screen;
        } else if (qFuzzyCompare(screen->devicePixelRatio(), 1.0)) {
            screenB = screen;
        }
    }
    require(screenA != nullptr && screenB != nullptr, "requires 150% and 100% screens");
    require(screenB->geometry().right() + 1 == screenA->geometry().left(),
            "requires 100% monitor B immediately left of 150% monitor A");
    class Owner : public QWidget {
      public:
        void retire() {
            hide();
            destroy(true, true);
        }
    };
    Owner ownerA;
    Owner ownerB;
    for (auto pair : {std::make_pair(&ownerA, screenA), std::make_pair(&ownerB, screenB)}) {
        pair.first->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        pair.first->setWindowOpacity(0.0);
        pair.first->winId();
        pair.first->windowHandle()->setScreen(pair.second);
        pair.first->setGeometry(pair.second->geometry());
        pair.first->show();
    }
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    ToolbarPaintExtentMonitor monitor(window);
    for (auto pair : {std::make_pair(&ownerB, screenB), std::make_pair(&ownerA, screenA),
                      std::make_pair(&ownerB, screenB), std::make_pair(&ownerA, screenA)}) {
        window.hide();
        ownerA.retire();
        ownerB.retire();
        window.releaseNativeSurface();
        for (auto ownerPair :
             {std::make_pair(&ownerA, screenA), std::make_pair(&ownerB, screenB)}) {
            ownerPair.first->winId();
            ownerPair.first->windowHandle()->setScreen(ownerPair.second);
            ownerPair.first->setGeometry(ownerPair.second->geometry());
            ownerPair.first->show();
        }
        window.restoreNativeSurface();
        window.resetForNewCapture();
        // Capture preparation uses the first overlay before the selection's
        // actual monitor is known. Omitting this step misses the regression.
        window.setOwnerWindow(&ownerA);
        window.prepareForDisplay();
        settleQueuedRefreshes();
        window.setPlacementContext(pair.second, pair.second->geometry(), QRect());
        window.prepareForDisplay();
        window.setOwnerWindow(pair.first);
        window.setStyleToolbarAboveMain(false);
        window.resetPositionForSelection(pair.second->geometry().topLeft() + QPoint(0, 300));
        window.prepareForDisplay();
        monitor.painted = false;
        monitor.clipped = false;
        window.show();
        settleQueuedRefreshes();
        require(window.rect().contains(window.paletteHost()->geometry()),
                "reused toolbar must contain its host on the first show on another screen");
        require(window.size() == window.windowSizeHint(),
                "reused toolbar frame must match its preset on first show");
        require(monitor.painted && !monitor.clipped,
                "reused toolbar must not paint a clipped frame on its first show");
    }
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
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;
    ScreenshotFloatingToolPaletteWindow window(options);
    constexpr ScreenshotToolPalette::Tool tools[] = {
        ScreenshotToolPalette::Tool::Select,       ScreenshotToolPalette::Tool::Shape,
        ScreenshotToolPalette::Tool::Arrow,        ScreenshotToolPalette::Tool::Text,
        ScreenshotToolPalette::Tool::SerialNumber,
    };
    require(window.palette()->ensureActionFamily(ScreenshotToolPalette::ActionFamily::Selection),
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
    // The Move tool keeps the selection-action row as its secondary: the
    // placement reserves that row even though every style row stays hidden.
    require(movePlacementRect == window.fullContentRect() &&
                movePlacementRect != palette->mainToolbarContentRect(),
            "an editorless tool should reserve its action row without the style rows");

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

void screenshotSelectionUnitSurvivesWindowAndCaptureReset() {
    using Unit = ScreenshotSelectionDisplayUnit;
    const snow_shot::storage::ScreenshotUiSettings settings;
    const QString saved = settings.selectionDisplayUnit();
    require(settings.setSelectionDisplayUnit(QStringLiteral("logical_pixels")),
            "initialize unit preference");
    NoOpToolbarCommands commands;
    {
        ScreenshotToolbarWindow window(commands);
        auto* palette = window.palette();
        palette->setActiveTool(ScreenshotToolPalette::Tool::Move);
        const auto group = [&] {
            return palette->findChild<adqt::widgets::AdRadioButtonGroup*>(
                QStringLiteral("screenshotSelectionDisplayUnitButtonGroup"));
        };
        require(group() && group()->checkedId() == int(Unit::LogicalPixels),
                "toolbar construction must load the saved selection unit");
        group()->button(int(Unit::PhysicalPixels))->click();
        require(commands.selectionUnitCommands == 1 &&
                    settings.selectionDisplayUnit() == QStringLiteral("physical_pixels"),
                "unit click must route exactly once through the toolbar command sink");
        window.resetForNewCapture();
        palette->setActiveTool(ScreenshotToolPalette::Tool::Move);
        require(group() && group()->checkedId() == int(Unit::PhysicalPixels),
                "capture reset must preserve the unit");
        require(settings.setSelectionDisplayUnit(QStringLiteral("logical_pixels")),
                "change unit externally");
        require(group()->checkedId() == int(Unit::LogicalPixels) &&
                    commands.selectionUnitCommands == 1,
                "live preference changes must synchronize without command loops");
    }
    ScreenshotToolbarWindow recreated(commands);
    recreated.palette()->setActiveTool(ScreenshotToolPalette::Tool::Move);
    const auto* group = recreated.palette()->findChild<adqt::widgets::AdRadioButtonGroup*>(
        QStringLiteral("screenshotSelectionDisplayUnitButtonGroup"));
    require(group && group->checkedId() == int(Unit::LogicalPixels),
            "recreated windows must retain the persisted unit");
    require(settings.setSelectionDisplayUnit(saved), "restore unit preference");
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
        // Unmaterialized panels keep degenerate placeholder geometry; only
        // displayed content has to fit the preset frame.
        if (widget == nullptr || widget->size().isEmpty() || !widget->isVisible()) {
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

int actionToolbarButtonCount(const ScreenshotToolPalette& palette) {
    const QWidget* mainPanel = palette.mainPanel();
    const QLayout* layout = mainPanel != nullptr ? mainPanel->layout() : nullptr;
    if (layout == nullptr) {
        return 0;
    }

    const QStringList actionIds = snow_shot::presentation::toolbar_layout::defaultOrder(
        snow_shot::storage::ScreenshotToolbarLayoutKind::ActionTools);
    int count = 0;
    for (int index = 0; index < layout->count(); ++index) {
        const auto* button =
            qobject_cast<const adqt::widgets::AdButton*>(layout->itemAt(index)->widget());
        if (button == nullptr) {
            continue;
        }
        const QStringList positionItems =
            button->property("screenshotToolbarPositionItems").toStringList();
        for (const QString& itemId : positionItems) {
            if (actionIds.contains(itemId)) {
                ++count;
                break;
            }
        }
    }
    return count;
}

void keyboardFocusTransitionsKeepQtAndNativeStateConsistent() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create keyboard-focus test storage");
    const QString executableDirectory = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executableDirectory), "failed to create test executable directory");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({executableDirectory, temporary.path(), 60000}).success,
            "failed to initialize keyboard-focus test storage");
    {
        const NativeGeometryWarningScope warningScope;
        NoOpToolbarCommands commands;
        ScreenshotToolbarWindow screenshotToolbar(commands);
        ScreenshotFloatingToolPaletteWindow genericToolbar(ScreenshotToolPalette::Options{});
        for (ScreenshotFloatingToolPaletteWindow* toolbar :
             {static_cast<ScreenshotFloatingToolPaletteWindow*>(&screenshotToolbar),
              &genericToolbar}) {
            const WId originalId = toolbar->winId();
            QWindow* handle = toolbar->windowHandle();
            const QRect originalGeometry = handle->geometry();
            const Qt::WindowFlags originalFlags = handle->flags();
            QLineEdit first(toolbar);
            QLineEdit second(toolbar);
            const auto checkState = [&](bool enabled) {
                require(toolbar->winId() == originalId,
                        "keyboard focus transitions must preserve the native surface");
                require(handle->geometry() == originalGeometry &&
                            (handle->flags() & ~Qt::WindowDoesNotAcceptFocus) ==
                                (originalFlags & ~Qt::WindowDoesNotAcceptFocus),
                        "keyboard focus must preserve toolbar geometry and other window flags");
                require(handle->flags().testFlag(Qt::WindowDoesNotAcceptFocus) != enabled,
                        "Qt focus policy must follow the active keyboard interaction");
#if defined(Q_OS_WIN) || defined(_WIN32)
                if (QGuiApplication::platformName() == QStringLiteral("windows")) {
                    const auto style =
                        GetWindowLongPtrW(reinterpret_cast<HWND>(originalId), GWL_EXSTYLE);
                    require(((style & WS_EX_NOACTIVATE) == 0) == enabled,
                            "Windows focus policy must agree with Qt focus policy");
                }
#endif
            };
            checkState(!originalFlags.testFlag(Qt::WindowDoesNotAcceptFocus));
            ScreenshotFloatingToolPaletteWindowTestAccess::beginKeyboardFocus(*toolbar, &first);
            handle->requestActivate();
            require(
                !nonFocusableActivationWarningEmitted.load(std::memory_order_relaxed),
                "an active keyboard editor must not request activation of a non-focusable window");
            checkState(true);
            ScreenshotFloatingToolPaletteWindowTestAccess::beginKeyboardFocus(*toolbar, &second);
            ScreenshotFloatingToolPaletteWindowTestAccess::endKeyboardFocus(*toolbar, &first);
            checkState(true);
            ScreenshotFloatingToolPaletteWindowTestAccess::endKeyboardFocus(*toolbar, &second);
            checkState(!originalFlags.testFlag(Qt::WindowDoesNotAcceptFocus));
            ScreenshotFloatingToolPaletteWindowTestAccess::endKeyboardFocus(*toolbar, nullptr);
            checkState(!originalFlags.testFlag(Qt::WindowDoesNotAcceptFocus));
            require(!toolbar->isVisible(), "focus transitions must not show a hidden toolbar");
        }
        require(!warningScope.emitted(),
                "focus transitions must not cause native geometry warnings");
    }
    storage.shutdown();
}

void qtFocusFlagChangeStillRequiresLayeredSurfacePreservation() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        return;
    }
    // Sentinel for Qt's applyWindowFlags()/initialize() layer reset. If Qt
    // starts preserving the surface itself, retire the nativeEvent workaround.
    QWidget window(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window.setAttribute(Qt::WA_TranslucentBackground);
    window.resize(80, 40);
    window.show();
    QCoreApplication::processEvents();
    require((GetWindowLongPtr(reinterpret_cast<HWND>(window.winId()), GWL_EXSTYLE) &
             WS_EX_LAYERED) != 0,
            "Qt sentinel requires a layered window");
    LayeredSurfaceMonitor surface(window.winId());
    window.windowHandle()->setFlag(Qt::WindowDoesNotAcceptFocus, false);
    require(surface.resets > 0,
            "Qt now preserves layered surfaces: review and retire the focus-policy workaround");
#endif
}

void screenshotActionLayoutReloadIsWindowScopedAndFitsThePreset() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create isolated action-layout test storage");
    const QString executableDirectory = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executableDirectory),
            "failed to create the action-layout test executable directory");

    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    require(applicationStorage.initialize({executableDirectory, temporary.path(), 60000}).success,
            "failed to initialize isolated action-layout test storage");

    const QStringList actionIds = snow_shot::presentation::toolbar_layout::defaultOrder(
        snow_shot::storage::ScreenshotToolbarLayoutKind::ActionTools);
    const int expectedActionButtonCount = static_cast<int>(actionIds.size());
    constexpr int expectedDefaultActionButtonCount = 7;
    const snow_shot::storage::ScreenshotToolbarSettings toolbarSettings;
    require(toolbarSettings.setLayout(snow_shot::storage::ScreenshotToolbarLayoutKind::ActionTools,
                                      snow_shot::storage::ScreenshotToolbarLayout{{}, actionIds}),
            "failed to persist the all-hidden screenshot action layout");

    {
        NoOpToolbarCommands commands;
        ScreenshotToolbarWindow screenshotToolbar(commands);
        screenshotToolbar.prepareForDisplay();
        require(actionToolbarButtonCount(*screenshotToolbar.palette()) == 0,
                "an all-hidden screenshot action layout should remove every configurable action");
        require(screenshotToolbar.palette()->findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotUndoButton")) != nullptr &&
                    screenshotToolbar.palette()->findChild<adqt::widgets::AdButton*>(
                        QStringLiteral("screenshotRedoButton")) != nullptr,
                "an all-hidden screenshot action layout should retain history controls");

        bool hasCancel = false;
        bool hasCopy = false;
        for (const adqt::widgets::AdButton* button :
             screenshotToolbar.palette()->findChildren<adqt::widgets::AdButton*>()) {
            hasCancel =
                hasCancel || button->accessibleName() == QStringLiteral("Cancel screenshot");
            hasCopy = hasCopy || button->accessibleName() == QStringLiteral("Copy to clipboard");
        }
        require(hasCancel && hasCopy,
                "an all-hidden screenshot action layout should retain fixed result actions");

        ScreenshotToolPalette::Options genericOptions;
        genericOptions.showSelectTool = false;
        genericOptions.showShapeTool = false;
        genericOptions.showTableTool = true;
        genericOptions.showQrTool = true;
        genericOptions.showScreenRecordButton = true;
        genericOptions.enableStyleToolbar = false;
        ScreenshotFloatingToolPaletteWindow genericToolbar(genericOptions);
        genericToolbar.prepareForDisplay();
        require(actionToolbarButtonCount(*genericToolbar.palette()) == 2,
                "a generic palette should retain its fixed Table/Barcode and Record slots");

        snow_shot::storage::ScreenshotToolbarLayout unstackedLayout;
        for (const QString& itemId : actionIds) {
            unstackedLayout.positions.push_back({itemId});
        }
        require(toolbarSettings.setLayout(
                    snow_shot::storage::ScreenshotToolbarLayoutKind::ActionTools, unstackedLayout),
                "failed to persist the widest screenshot action layout");
        settleQueuedRefreshes();
        require(actionToolbarButtonCount(*screenshotToolbar.palette()) ==
                        expectedActionButtonCount &&
                    screenshotToolbar.palette()->findChild<adqt::widgets::AdButton*>(
                        QStringLiteral("screenshotPinToScreenButton")) != nullptr &&
                    screenshotToolbar.palette()->findChild<adqt::widgets::AdButton*>(
                        QStringLiteral("screenshotQrRecognitionButton")) != nullptr &&
                    screenshotToolbar.palette()->findChild<adqt::widgets::AdButton*>(
                        QStringLiteral("screenshotTableRecognitionButton")) != nullptr,
                "the screenshot window should live-reload every independent action slot");
        require(actionToolbarButtonCount(*genericToolbar.palette()) == 2,
                "screenshot action layout reloads must not modify generic palettes");
        requireDynamicToolbarContentFits(
            screenshotToolbar,
            "the widest screenshot action layout must fit the fixed window preset");

        require(toolbarSettings.setLayout(
                    snow_shot::storage::ScreenshotToolbarLayoutKind::ActionTools, {}),
                "failed to restore the default screenshot action layout");
        settleQueuedRefreshes();
        require(
            actionToolbarButtonCount(*screenshotToolbar.palette()) ==
                    expectedDefaultActionButtonCount &&
                screenshotToolbar.palette()->findChild<adqt::widgets::AdButton*>(
                    QStringLiteral("screenshotTableQrButton")) != nullptr,
            "restoring the default layout should restore the shared recognition/conversion slot");
        require(actionToolbarButtonCount(*genericToolbar.palette()) == 2,
                "restoring the screenshot layout must not modify generic palettes");
    }

    applicationStorage.shutdown();
}

void floatingToolbarsUseTheFixedWindowPreset() {
    constexpr QSize normalPreset(1242, 142);
    constexpr QSize smallPreset(994, 114);

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
    requireDynamicToolbarContentFits(screenshotToolbar,
                                     "screenshot toolbar content must fit the fixed normal preset");
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
    requireDynamicToolbarContentFits(screenshotToolbar,
                                     "screenshot toolbar content must fit the fixed small preset");

    ScreenshotFloatingToolPaletteWindow recordingToolbar(recordingToolbarOptionsForPresetTest());
    recordingToolbar.prepareForDisplay();
    require(recordingToolbar.windowSizeHint() == normalPreset &&
                recordingToolbar.size() == normalPreset,
            "screen recording toolbar should use the fixed normal window preset");
    requireDynamicToolbarContentFits(recordingToolbar,
                                     "screen recording toolbar content must fit the fixed preset");

    ScreenshotFloatingToolPaletteWindow pinnedToolbar(testToolbarOptions());
    pinnedToolbar.prepareForDisplay();
    require(pinnedToolbar.windowSizeHint() == normalPreset && pinnedToolbar.size() == normalPreset,
            "pinned toolbar should use the fixed normal window preset");
    requireDynamicToolbarContentFits(pinnedToolbar,
                                     "pinned toolbar content must fit the fixed preset");
}

void scrollingModeRequestsPresentationReposition() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow screenshotToolbar(commands);

    screenshotToolbar.setScrollingScreenshotMode(true);
    require(commands.presentationRepositionCount == 1,
            "entering scrolling mode should recompute toolbar placement after the sub-toolbar "
            "appears");

    screenshotToolbar.setScrollingScreenshotMode(true);
    require(commands.presentationRepositionCount == 1,
            "reapplying scrolling mode should not trigger a redundant placement pass");

    screenshotToolbar.setScrollingScreenshotMode(false);
    require(commands.presentationRepositionCount == 2,
            "leaving scrolling mode should recompute toolbar placement after the sub-toolbar "
            "hides");
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
    require(window.paletteHost()->pos() == QPoint(0, 0) && window.palette()->y() == topPlacementY,
            "top placement should anchor the palette to the fixed frame bottom");

    window.resetForNewCapture();
    settleQueuedRefreshes();

    const QMargins shadowMargins = ScreenshotToolPaletteHost::defaultShadowMargins();
    require(window.paletteHost()->pos() == QPoint(0, 0) &&
                window.paletteHost()->size() == frameSize,
            "capture reset should restore the host to the fixed frame origin");
    const ScreenshotToolbarPlacementSnapshot snapshot = window.placementSnapshot();
    const QRect mainRect =
        snapshot.bottom.mainToolbarContentRect.translated(snapshot.contentOffset);
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
    require(window.internalWinId() == 0, "retiring an already retired toolbar must be idempotent");

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

void prewarmedToolbarSurfaceStaysHiddenUntilShown() {
    NoOpToolbarCommands commands;
    QWidget owner;
    owner.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    owner.resize(640, 360);
    settleQueuedRefreshes();

    ScreenshotToolbarWindow window(commands);
    window.restoreNativeSurface();
    window.setOwnerWindow(&owner);
    window.prepareForDisplay();
    settleQueuedRefreshes();

    require(window.internalWinId() != 0 && window.testAttribute(Qt::WA_WState_Created),
            "a prewarmed toolbar must hold a live native surface");
    require(!window.isVisible(),
            "attaching and preparing a hidden toolbar must keep it hidden until it is shown");
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(GetWindow(toNativeHwnd(window.internalWinId()), GW_OWNER) ==
                toNativeHwnd(owner.internalWinId()),
            "a prewarmed toolbar must be natively owned by its overlay while hidden");
#endif

    owner.show();
    settleQueuedRefreshes();
    window.show();
    settleQueuedRefreshes();
    require(window.isVisible(),
            "a prewarmed toolbar must still show normally when the presenter reveals it");
}

void translateButtonRoutesEveryClickThroughTheToggleCommand() {
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.setActiveTool(ScreenshotToolPalette::Tool::Ocr);
    window.setTextEditingState(true, false);
    window.setTextTranslationState(true, false, false);

    auto* translate =
        window.findChild<QAbstractButton*>(QStringLiteral("screenshotOcrTextTranslateButton"));
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

void jumpToTranslationPageFollowsLiveSettingsAndOcrAvailability() {
    QTemporaryDir directory;
    require(directory.isValid(), "jump toolbar test requires isolated storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 60000}).success,
            "initialize jump toolbar test storage");

    const snow_shot::storage::ExtendedFeaturesSettings settings;
    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.setActiveTool(ScreenshotToolPalette::Tool::Ocr);
    auto* jump = window.findChild<QAbstractButton*>(
        QStringLiteral("screenshotOcrJumpToTranslationPageButton"));
    auto* translate =
        window.findChild<QAbstractButton*>(QStringLiteral("screenshotOcrTextTranslateButton"));
    auto* formatting =
        window.findChild<QWidget*>(QStringLiteral("screenshotOcrTextFormattingSelect"));
    require(jump != nullptr && translate != nullptr && formatting != nullptr && jump->isHidden(),
            "default-off setting must hide the OCR jump action");
    QLayout* row = jump->parentWidget()->layout();
    require(row != nullptr, "the OCR jump action must live in a laid-out action row");
    // The whole-window hint only grows when the action row is the widest row, which
    // depends on unrelated toolbar content; the row width is the real contract.
    const int hiddenRowWidth = row->sizeHint().width();

    require(settings.setJumpToTranslationPage(true), "enable preserved child preference");
    QCoreApplication::processEvents();
    require(jump->isHidden(), "master-off setting must keep the OCR jump action hidden");
    require(settings.setTranslationPageEnabled(true), "enable Translation page master");
    QCoreApplication::processEvents();
    require(!jump->isHidden() && !jump->isEnabled() && row->sizeHint().width() > hiddenRowWidth,
            "both settings must reveal a result-gated OCR jump action and expand the row");

    require(row->indexOf(translate) < row->indexOf(jump) &&
                row->indexOf(jump) < row->indexOf(formatting),
            "OCR jump action must follow Text translation and precede formatting");
    window.setTextEditingState(true, false);
    window.setTextTranslationState(true, false, false);
    require(jump->isEnabled(), "completed OCR must enable the jump action");
    jump->click();
    require(commands.jumpToTranslationPageCount == 1,
            "OCR jump action must dispatch exactly one toolbar command");

    require(settings.setTranslationPageEnabled(false), "disable Translation page master");
    QCoreApplication::processEvents();
    require(jump->isHidden() && settings.jumpToTranslationPage() &&
                row->sizeHint().width() == hiddenRowWidth,
            "master-off must hide the action, restore row width, and preserve child preference");
    storage.shutdown();
}

void jumpToTranslationPageCommandMustOutliveItsClickDispatch() {
    QTemporaryDir directory;
    require(directory.isValid(), "jump teardown test requires isolated storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 60000}).success,
            "initialize jump teardown test storage");

    const snow_shot::storage::ExtendedFeaturesSettings settings;
    require(settings.setTranslationPageEnabled(true) && settings.setJumpToTranslationPage(true),
            "reveal the OCR jump action for the teardown test");
    QCoreApplication::processEvents();

    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    commands.jumpToTranslationPageResetTarget = &window;
    window.setActiveTool(ScreenshotToolPalette::Tool::Ocr);
    QCoreApplication::processEvents();
    QPointer<QAbstractButton> jump = window.findChild<QAbstractButton*>(
        QStringLiteral("screenshotOcrJumpToTranslationPageButton"));
    require(jump != nullptr && !jump->isHidden(), "OCR jump action must be visible");
    window.setTextEditingState(true, false);
    window.setTextTranslationState(true, false, false);
    require(jump->isEnabled(), "completed OCR must enable the jump action");

    // Release through the real mouse path: the command resets the toolbar for a new
    // capture, which synchronously evicts the secondary toolbar contents from the
    // dispatching button's own mouseReleaseEvent. The eviction must detach the
    // widgets but defer their destruction, so the button survives its release
    // event and the event loop reaps it afterwards.
    const QPointF local = QPointF(jump->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, local, QPointF(jump->mapToGlobal(local.toPoint())),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(jump.data(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local,
                        QPointF(jump->mapToGlobal(local.toPoint())), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(jump.data(), &release);
    require(commands.jumpToTranslationPageCount == 1,
            "the jump command must dispatch synchronously from the click");
    require(!jump.isNull(),
            "eviction must not destroy the dispatching button inside its own mouseReleaseEvent");
    require(window.palette()->activeToolForTests() == ScreenshotToolPalette::Tool::Move,
            "the jump command must still reset the toolbar for the next capture");
    QCoreApplication::processEvents();
    require(jump.isNull(),
            "evicted secondary contents must be destroyed once control returns to the event loop");

    require(settings.setTranslationPageEnabled(false) && settings.setJumpToTranslationPage(false),
            "restore jump teardown test settings");
    storage.shutdown();
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

void floatingToolbarInputsAcquireKeyboardFocus() {
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "keyboard focus tests require isolated storage");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({storageDirectory.path(), storageDirectory.path(), 60000})
                .success,
            "keyboard focus tests should initialize isolated storage");
    QWidget owner;
    owner.show();
    ScreenshotToolPalette::Options options;
    options.showSerialNumberTool = true;
    ScreenshotFloatingToolPaletteWindow window(options);
    window.setTransientOwnerWindow(&owner);
    window.show();
    auto* palette = window.palette();
    const auto click = [](QWidget* target) {
        const QPoint local = target->rect().center();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(local),
                          QPointF(target->mapToGlobal(local)), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(target, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local),
                            QPointF(target->mapToGlobal(local)), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(target, &release);
    };
    const auto requireNativeFocus = [&window](QWidget* input) {
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            const auto hwnd = reinterpret_cast<HWND>(window.winId());
            require((GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) == 0,
                    "editing should temporarily allow native toolbar activation");
            require(input->hasFocus() && QApplication::focusWidget() == input,
                    "clicking the floating input must give it actual keyboard focus");
        }
#elif defined(Q_OS_MACOS)
        if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
            settleQueuedRefreshes();
            require(!window.windowHandle()->flags().testFlag(Qt::WindowDoesNotAcceptFocus) &&
                        input->hasFocus() && QApplication::focusWidget() == input,
                    "editing a Cocoa toolbar must give the input actual keyboard focus");
        }
#else
        Q_UNUSED(window);
        Q_UNUSED(input);
#endif
    };
    for (const auto tool :
         {ScreenshotToolPalette::Tool::Watermark, ScreenshotToolPalette::Tool::SerialNumber}) {
        palette->setActiveTool(tool);
        QCoreApplication::processEvents();
        QLineEdit* input = nullptr;
        for (auto* candidate : palette->findChildren<QLineEdit*>()) {
            if (candidate->isVisible() &&
                (candidate->objectName() == QStringLiteral("screenshotWatermarkTextEdit") ||
                 candidate->accessibleName() ==
                     QStringLiteral("Sequence number (scroll to adjust)"))) {
                input = candidate;
                break;
            }
        }
        require(input != nullptr, "floating toolbar should expose the active text input");
#if defined(Q_OS_WIN) || defined(_WIN32)
        const bool nativeWindows = QGuiApplication::platformName() == QStringLiteral("windows");
        const auto hwnd = reinterpret_cast<HWND>(window.winId());
        LayeredSurfaceMonitor surface(window.winId());
        if (nativeWindows) {
            require((GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0,
                    "visible transparent toolbar must start with layered rendering");
        }
#endif
        click(input);
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (nativeWindows) {
            require(surface.resets == 0,
                    "focusing a toolbar input must not discard the layered surface");
            require((GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0,
                    "focusing a toolbar input must preserve layered rendering before repaint");
        }
#endif
        QCoreApplication::processEvents();
        require(ScreenshotFloatingToolPaletteWindowTestAccess::keyboardFocusActive(window, input),
                "clicking any floating toolbar input must enable its keyboard focus interaction");
        requireNativeFocus(input);
        for (int repeat = 0; repeat < 3; ++repeat) {
            click(input);
            QCoreApplication::processEvents();
            requireNativeFocus(input);
        }
        input->selectAll();
        QKeyEvent key(QEvent::KeyPress, Qt::Key_4, Qt::NoModifier, QStringLiteral("42"));
        QApplication::sendEvent(input, &key);
        require(input->text() == QStringLiteral("42"), "the focused input should accept typing");
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(input, &enter);
        QCoreApplication::processEvents();
        require(!ScreenshotFloatingToolPaletteWindowTestAccess::keyboardFocusActive(window, input),
                "finishing input should release the temporary keyboard focus interaction");
        if (auto* prefix = input->findChild<QLabel*>(QStringLiteral("ad-input-prefix-icon"));
            prefix != nullptr && prefix->isVisible()) {
            click(prefix);
        } else {
            click(input);
        }
        QCoreApplication::processEvents();
        require(ScreenshotFloatingToolPaletteWindowTestAccess::keyboardFocusActive(window, input),
                "clicking the input prefix should also start keyboard editing");
        requireNativeFocus(input);
        input->clearFocus();
        QCoreApplication::processEvents();
        require(!ScreenshotFloatingToolPaletteWindowTestAccess::keyboardFocusActive(window, input),
                "focus loss should restore non-activating toolbar behavior");
        click(input);
        palette->setActiveTool(ScreenshotToolPalette::Tool::Select);
        // Switching tools retires the input row through staged content
        // changes; let those settle before judging the keyboard state.
        settleQueuedRefreshes();
        require(
            !ScreenshotFloatingToolPaletteWindowTestAccess::keyboardFocusActive(window, nullptr),
            "destroying an active input should not leave keyboard interaction enabled");
#if defined(Q_OS_WIN) || defined(_WIN32)
        require(surface.resets == 0,
                "editing, repeated clicks, focus loss and input removal must preserve the surface");
#endif
    }
    snow_shot::storage::ApplicationStorage::instance().shutdown();
}

void interruptedToolbarDragStopsMoving() {
    for (const bool trailing : {false, true}) {
        for (const auto interruption :
             {QEvent::UngrabMouse, QEvent::WindowDeactivate, QEvent::MouseMove, QEvent::Hide}) {
            auto options = testToolbarOptions();
            options.showTrailingDragHandle = true;
            ScreenshotFloatingToolPaletteWindow window(options);
            window.show();
            QApplication::processEvents();
            auto* handle =
                trailing ? window.palette()->trailingDragHandle() : window.palette()->dragHandle();
            require(handle != nullptr, "toolbar drag handle must exist");
            const QPoint local = handle->rect().center();
            const QPoint global = handle->mapToGlobal(local);
            QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(handle, &press);
            require(QWidget::mouseGrabber() == handle, "toolbar drag must own mouse capture");
            if (interruption != QEvent::MouseMove) {
                QEvent event(interruption);
                QApplication::sendEvent(interruption == QEvent::WindowDeactivate
                                            ? static_cast<QWidget*>(&window)
                                            : handle,
                                        &event);
            }
            const QPoint previous = window.contentPosition();
            QMouseEvent move(QEvent::MouseMove, local, global + QPoint(80, 40), Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(handle, &move);
            if (QWidget::mouseGrabber() == handle || window.contentPosition() != previous ||
                window.physicalDragActive()) {
                std::cerr << "interruption=" << interruption
                          << " grabbed=" << (QWidget::mouseGrabber() == handle)
                          << " physical=" << window.physicalDragActive()
                          << " before=" << previous.x() << ',' << previous.y()
                          << " after=" << window.contentPosition().x() << ','
                          << window.contentPosition().y() << '\n';
            }
            require(QWidget::mouseGrabber() != handle && window.contentPosition() == previous &&
                        !window.physicalDragActive(),
                    "lost capture, deactivation and missing releases must end toolbar dragging");
            QMouseEvent nextPress(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(handle, &nextPress);
            require(QWidget::mouseGrabber() == handle,
                    "an interrupted toolbar drag must allow a fresh drag");
            int dragMoves = 0;
            QObject::connect(window.paletteHost(), &ScreenshotToolPaletteHost::dragMoved, &window,
                             [&]() { ++dragMoves; });
            QMouseEvent nextMove(QEvent::MouseMove, local, global + QPoint(20, 10), Qt::NoButton,
                                 Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(handle, &nextMove);
            // Synthetic events do not move the Windows physical cursor used by
            // native positioning. Verify that the new drag dispatches movement.
            require(dragMoves == 1,
                    "a fresh toolbar drag must dispatch movement after interruption");
            QMouseEvent release(QEvent::MouseButtonRelease, local, global + QPoint(20, 10),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(handle, &release);
            require(QWidget::mouseGrabber() != handle && !window.physicalDragActive(),
                    "a fresh toolbar drag must finish normally");
        }
    }
    snow_shot::storage::ApplicationStorage::instance().shutdown();
}

#if defined(Q_OS_MACOS)
void macosToolbarCrossDisplayDrag() {
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "native drag tests require isolated settings");
    {
        NoOpToolbarCommands commands;
        QWidget owner;
        QScreen* primary = QGuiApplication::primaryScreen();
        owner.setGeometry(primary->geometry());
        owner.show();
        ScreenshotToolbarWindow window(commands);
        window.setOwnerWindow(&owner);
        window.setPlacementContext(primary, primary->geometry(), primary->geometry());
        window.moveContentTo(primary->availableGeometry().topLeft() + QPoint(300, 200));
        window.show();
        settleQueuedRefreshes();
        snow_shot::platform::configureScreenshotOverlayWindow(&owner);
        snow_shot::platform::configureScreenshotToolbarWindow(&window);
        macActivateApplication();
        for (QScreen* destination : QGuiApplication::screens()) {
            if (destination == primary)
                continue;
            for (QScreen* target : {destination, primary}) {
                const QPoint start = window.contentPosition();
                QWidget* handle = window.palette()->dragHandle();
                const QPoint pointerStart = handle->mapToGlobal(handle->rect().center());
                const QPoint end = target->availableGeometry().center() - window.rect().center() +
                                   (window.contentPosition() - window.pos());
                const QPoint delta = end - start;
                const int steps = qMax(1, qMax(qAbs(delta.x()), qAbs(delta.y())) / 40);
                const QSize size = window.size();
                MacMouseDrag drag(pointerStart);
                for (int step = 1; step <= steps; ++step) {
                    const QPoint offset = (QPointF(delta) * step / steps).toPoint();
                    drag.moveTo(pointerStart + offset);
                    if (window.contentPosition() != start + offset || window.size() != size)
                        qWarning() << "Toolbar drag" << "expected" << start + offset << "actual"
                                   << window.contentPosition() << "size" << window.size() << size
                                   << "pointer" << pointerStart + offset;
                    require(window.contentPosition() == start + offset && window.size() == size,
                            "toolbar must follow the cursor across displays without shifting or "
                            "scaling");
                }
                drag.finish();
                require(window.screen() == target && window.contentPosition() == end,
                        "released toolbar must remain on its destination display");
            }
        }
    }
    storage.shutdown();
}

void macosToolbarShadowClickThrough() {
    require(macCanPostMouseEvents(),
            "native click test requires Accessibility event-posting access");
    MacCursorRestore restoreCursor;
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "native toolbar input tests require isolated settings");
    {
        QWidget owner;
        owner.setGeometry(
            QApplication::primaryScreen()->availableGeometry().adjusted(50, 100, -50, -100));
        QPushButton underlying(QStringLiteral("Underlying canvas"), &owner);
        underlying.setGeometry(owner.rect());
        int canvasClicks = 0;
        QObject::connect(&underlying, &QPushButton::clicked, [&] { ++canvasClicks; });
        owner.show();
        snow_shot::platform::configureScreenshotOverlayWindow(&owner);
        macActivateApplication();
        owner.activateWindow();
        NoOpToolbarCommands commands;
        ScreenshotToolbarWindow window(commands);
        window.setOwnerWindow(&owner);
        window.moveContentTo(owner.mapToGlobal(QPoint(300, 300)));
        window.show();
        settleQueuedRefreshes();
        snow_shot::platform::configureScreenshotToolbarWindow(&window);
        for (const auto& size : {QStringLiteral("normal"), QStringLiteral("small")}) {
            window.setToolbarSize(size);
            window.prepareForDisplay();
            settleQueuedRefreshes();
            for (const auto& name : {QStringLiteral("screenshotArrowLineButton"),
                                     QStringLiteral("screenshotHighlightButton")}) {
                auto* trigger = window.findChild<adqt::widgets::AdButton*>(name);
                require(trigger && trigger->isVisible(), "drawing group trigger missing");
                int triggerClicks = 0;
                const auto connection = QObject::connect(trigger, &adqt::widgets::AdButton::clicked,
                                                         [&] { ++triggerClicks; });
                const QPoint center = trigger->mapToGlobal(trigger->rect().center());
                macPostMove(center);
                QElapsedTimer wait;
                wait.start();
                adqt::widgets::detail::OverlayPopupSurface* popup = nullptr;
                while (wait.elapsed() < 2000 && !popup) {
                    QCoreApplication::processEvents();
                    QThread::msleep(10);
                    for (auto* widget : QApplication::topLevelWidgets()) {
                        if (widget->isVisible() &&
                            widget->objectName() == QStringLiteral("adpopover-surface")) {
                            popup =
                                dynamic_cast<adqt::widgets::detail::OverlayPopupSurface*>(widget);
                            break;
                        }
                    }
                }
                require(popup, "real hover must open the drawing group popover");
                // Keep the popup's painted body and arrow clear of its trigger; the
                // toolbar reserve is covered separately by native click-through tests.
                const QRect triggerRect(trigger->mapToGlobal(QPoint()), trigger->size());
                require(!popup->frameGeometry().intersects(triggerRect),
                        "popover native input frame must not overlap its trigger");
                require(popup->shadowMargins().isNull(),
                        "macOS popovers must not reserve native input space for shadows");
                for (const int y : {1, trigger->height() / 2, trigger->height() - 2}) {
                    const int before = triggerClicks;
                    const QPoint point = trigger->mapToGlobal(QPoint(trigger->width() / 2, y));
                    macPostClick(point);
                    require(triggerClicks == before + 1,
                            "popover shadow must pass a complete click to its toolbar trigger");
                }
                QObject::disconnect(connection);
                for (auto* popover : window.findChildren<adqt::widgets::AdPopover*>())
                    popover->hide();
                settleQueuedRefreshes();
            }
            for (int cycle = 0; cycle < 2; ++cycle) {
                using Tool = ScreenshotToolPalette::Tool;
                for (bool above : {false, true}) {
                    window.setStyleToolbarAboveMain(above);
                    for (Tool tool : {Tool::Select, Tool::Qr, Tool::Shape, Tool::Qr}) {
                        window.setActiveTool(tool);
                        settleQueuedRefreshes();
                        require(
                            macWindowHasShadow(&window),
                            "fixed-frame click-through must work with the native shadow enabled");
                        const QRect canvasInWindow(
                            window.mapFromGlobal(underlying.mapToGlobal(QPoint())),
                            underlying.size());
                        const QRegion reserve =
                            (QRegion(window.rect()) - window.mask())
                                .intersected(canvasInWindow.adjusted(2, 2, -2, -2));
                        QRect target;
                        for (const QRect& rect : reserve) {
                            if (rect.width() > 16 && rect.height() > 16 &&
                                rect.width() * rect.height() > target.width() * target.height()) {
                                target = rect;
                            }
                        }
                        require(!target.isEmpty(),
                                "fixed toolbar frame must expose a testable transparent reserve");
                        const int before = canvasClicks;
                        macPostClick(window.mapToGlobal(target.center()));
                        require(canvasClicks == before + 1,
                                "unused native frame must pass a complete click to the canvas");
                        auto* pin = window.palette()->findChild<adqt::widgets::AdButton*>(
                            QStringLiteral("screenshotPinToScreenButton"));
                        require(pin != nullptr, "toolbar Pin button missing");
                        const int pinBefore = commands.pinSelectionCount;
                        macPostClick(pin->mapToGlobal(pin->rect().center()));
                        require(commands.pinSelectionCount == pinBefore + 1 &&
                                    canvasClicks == before + 1,
                                "panel mask must retain button input after each row transition");
                    }
                }
                const auto* panel = window.palette()->mainPanel();
                for (const QPoint point : {QPoint(panel->width() / 2, panel->height() + 4),
                                           QPoint(-4, panel->height() / 2), QPoint(0, 0)}) {
                    const int before = canvasClicks;
                    macPostClick(panel->mapToGlobal(point));
                    require(canvasClicks == before + 1, "toolbar shadow and rounded corner must "
                                                        "pass a complete click to the canvas");
                }
                window.releaseNativeSurface();
                window.restoreNativeSurface();
                window.show();
                settleQueuedRefreshes();
                snow_shot::platform::configureScreenshotToolbarWindow(&window);
            }
        }
    }
    storage.shutdown();
}

void macosToolbarUsesLogicalGeometry() {
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "macOS toolbar tests require isolated settings");
    {
        QWidget owner;
        owner.resize(640, 360);
        owner.show();
        NoOpToolbarCommands commands;
        ScreenshotToolbarWindow window(commands);
        auto* pinButton = window.palette()->findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotPinToScreenButton"));
        require(pinButton != nullptr,
                "macOS screenshot toolbar must expose the supported Pin to Screen action");
        pinButton->click();
        require(commands.pinSelectionCount == 1,
                "macOS Pin to Screen action must dispatch exactly one toolbar command");
        window.setTransientOwnerWindow(&owner);
        window.setPlacementContext(nullptr, QRect(-2000, -1000, 6000, 4000),
                                   QRect(8000, 9000, 12000, 8000));
        window.moveContentTo(QPoint(200, 200));
        window.show();
        settleQueuedRefreshes();
        require(window.findChild<adqt::widgets::AdDpiStableWindowController*>() == nullptr,
                "macOS toolbar must not install a physical-size controller");
        require(window.testAttribute(Qt::WA_MacAlwaysShowToolWindow),
                "application deactivation must not hide an active macOS tool window");
        require(window.windowHandle()->transientParent() == owner.windowHandle(),
                "macOS toolbar must retain its Qt transient owner");

        const auto checkMask = [&]() {
            const QRegion panels = window.paletteHost()->interactiveHostRegion().translated(
                window.paletteHost()->pos());
            require(window.mask() == window.paletteHost()->surfaceHostRegion(),
                    "macOS native mask must follow the panels inside the fixed frame");
            require(window.palette()->mainPanel()->graphicsEffect() == nullptr,
                    "toolbar shadows must not be painted into the native input backing image");
            require(!window.windowFlags().testFlag(Qt::NoDropShadowWindowHint),
                    "native shadow visibility must be owned by the Qt window flags");
            require(!window.mask().isEmpty() &&
                        (panels.intersected(window.rect()) - window.mask()).isEmpty(),
                    "window mask must include every displayed panel");
            require(!QRegion(window.rect()).subtracted(window.mask()).isEmpty(),
                    "unused backing-window space must be excluded from the mask");
            const QRect main =
                window.palette()->mainPanel()->geometry().translated(window.palette()->pos());
            require(!window.mask().contains(main.topLeft()),
                    "fully transparent rounded corners must be excluded from native input");
            require(!window.mask().contains(main.topLeft() - QPoint(1, 1)),
                    "toolbar shadows must be outside the native input mask");
            QWidget* panel = window.palette()->mainPanel();
            for (const qreal renderDpr : {1.0, 1.5, 2.0}) {
                QImage image(
                    QSize(qRound(panel->width() * renderDpr), qRound(panel->height() * renderDpr)),
                    QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(renderDpr);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                panel->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
                painter.end();
                int blendedPixels = 0;
                for (int y = 0; y < qRound(8 * renderDpr); ++y) {
                    for (int x = 0; x < qRound(8 * renderDpr); ++x) {
                        const int alpha = image.pixelColor(x, y).alpha();
                        if (alpha > 0 && alpha < 255) {
                            ++blendedPixels;
                            const QPoint point = panel->mapTo(
                                &window, QPoint(qFloor(x / renderDpr), qFloor(y / renderDpr)));
                            require(window.mask().contains(point),
                                    "native mask must retain every antialiased corner pixel");
                        }
                    }
                }
                require(blendedPixels > 0, "toolbar corners must have fractional alpha coverage");
                require(image.pixelColor(image.width() / 2, 0) ==
                            image.pixelColor(image.width() / 2, 2),
                        "toolbar surface edge must not have a border stroke");
            }
        };
        for (const qreal multiplier : {1.0, 0.8}) {
            window.setToolbarSize(multiplier == 1.0 ? QStringLiteral("normal")
                                                    : QStringLiteral("small"));
            window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
            window.prepareForDisplay();
            settleQueuedRefreshes();
            const QSize logicalSize = window.size();
            require(logicalSize == QSize(qRound(1242 * multiplier), qRound(142 * multiplier)),
                    "macOS must share the fixed normal and small toolbar frame presets");
            const QSize contentSize = window.contentSizeHint();
            const QPoint anchor = window.contentPosition();
            for (const qreal dpr : {1.0, 2.0, 1.0, 2.0}) {
                ScreenshotFloatingToolPaletteWindowTestAccess::simulateDpr(window, dpr);
                settleQueuedRefreshes();
                const auto context = adqt::widgets::controlScaleContextFor(window.palette());
                require(qFuzzyCompare(context.currentDpr, dpr) &&
                            qFuzzyCompare(context.referenceDpr, dpr) &&
                            qFuzzyCompare(context.logicalScale, multiplier),
                        "DPR changes must update rendering resolution without layout compensation");
                require(window.size() == logicalSize && window.contentSizeHint() == contentSize &&
                            window.contentPosition() == anchor,
                        "display transitions must preserve logical dimensions and the anchor");
                require(!ScreenshotFloatingToolPaletteWindowTestAccess::hasPhysicalBaseline(window),
                        "macOS must not capture physical dimensions or a reference display DPR");
                checkMask();
            }

            // Materialize another editor only after its size and DPR are set.
            window.setActiveTool(ScreenshotToolPalette::Tool::Text);
            settleQueuedRefreshes();
            const auto lazyContext =
                adqt::widgets::controlScaleContextFor(window.palette()->stylePanel());
            require(qFuzzyCompare(lazyContext.logicalScale, multiplier),
                    "lazy style controls must inherit the user's logical size setting");
            checkMask();
            const QPoint mainAnchor =
                window.contentPosition() + window.paletteHost()->mainToolbarContentRect().topLeft();
            window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
            settleQueuedRefreshes();
            require(window.contentPosition() +
                            window.paletteHost()->mainToolbarContentRect().topLeft() ==
                        mainAnchor,
                    "materializing a style row must preserve the main-row anchor");
            window.setStyleToolbarAboveMain(true);
            settleQueuedRefreshes();
            checkMask();
            window.setStyleToolbarAboveMain(false);

            const QPoint beforeDrag = window.contentPosition();
            window.paletteHost()->dragStarted(QPoint(400, 300));
            window.paletteHost()->dragMoved(QPoint(420, 310));
            require(window.contentPosition() == beforeDrag + QPoint(20, 10) &&
                        !window.physicalDragActive(),
                    "macOS drag deltas must use logical coordinates despite physical bounds");
            ScreenshotFloatingToolPaletteWindowTestAccess::simulateDpr(window, 1.0);
            window.paletteHost()->dragMoved(QPoint(430, 315));
            require(window.contentPosition() == beforeDrag + QPoint(30, 15),
                    "changing DPR during a drag must not move the cursor anchor");
            window.paletteHost()->dragFinished(QPoint(430, 315));

            window.resetForNewCapture();
            window.prepareForDisplay();
            require(qFuzzyCompare(window.paletteHost()->physicalScale(), multiplier),
                    "capture reset must preserve the configured logical toolbar size");
            window.releaseNativeSurface();
            window.restoreNativeSurface();
            require(!window.isVisible() &&
                        window.windowHandle()->transientParent() == owner.windowHandle(),
                    "surface recreation must restore ownership without showing the toolbar");
            window.show();
            settleQueuedRefreshes();
            require(window.size() == window.windowSizeHint() &&
                        window.testAttribute(Qt::WA_MacAlwaysShowToolWindow),
                    "surface recreation must retain logical sizing and macOS attributes");
            checkMask();
        }
    }
    storage.shutdown();
}
#endif

void borderCursorSurvivesToolRestoration() {
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "border cursor tests require isolated settings");
    // Match the application's non-native canvas children.
    const bool nativeSiblingsDisabled =
        QCoreApplication::testAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    using Tool = ScreenshotToolPalette::Tool;
    for (const auto tool : {Tool::FreeDraw, Tool::Arrow, Tool::Shape, Tool::Select}) {
        QWidget overlay(nullptr, Qt::Tool | Qt::FramelessWindowHint);
        overlay.resize(800, 600);
        SnowCanvasWidget canvas(&overlay);
        canvas.setGeometry(overlay.rect());
        overlay.show();
        NoOpToolbarCommands commands;
        ScreenshotToolbarWindow toolbar(commands);
        toolbar.setOwnerWindow(&overlay);
        toolbar.prepareForDisplay();
        ScreenshotToolPalette& palette = *toolbar.palette();
        toolbar.setActiveTool(tool);
        toolbar.show();
        const auto canvasTool = tool == Tool::FreeDraw ? SnowCanvasTool::FreeDraw
                                : tool == Tool::Arrow  ? SnowCanvasTool::Arrow
                                : tool == Tool::Shape  ? SnowCanvasTool::Shape
                                                       : SnowCanvasTool::Select;
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                         [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
        require(canvas.setCanvasTool(canvasTool), "activate drawing tool");
        QCoreApplication::processEvents();

        for (int resize = 0; resize < 2; ++resize) {
            // The real border-resize path resets the engine before switching the toolbar to
            // Move. Free Draw and Arrow consequently materialize a temporary Shape row.
            require(canvas.resetEditingState(), "reset canvas for border drag");
            canvas.setInteractionEnabled(false);
            QVector<QPointer<QWidget>> controls;
            for (QWidget* control : palette.findChildren<QWidget*>()) {
                controls.push_back(control);
            }
            toolbar.setActiveTool(Tool::Move);
            toolbar.hide();
            QVector<QPointer<QWidget>> retiredControls;
            for (const QPointer<QWidget>& control : controls) {
                if (control && control->parentWidget() == nullptr) {
                    retiredControls.push_back(control);
                    require(control->isHidden(), "retired controls must initially be hidden");
                }
            }
            require(!retiredControls.isEmpty(), "resize must retire the previous toolbar controls");

            canvas.setInteractionEnabled(true);
            require(canvas.setCanvasTool(canvasTool), "restore drawing tool");
            toolbar.setActiveTool(tool);
            toolbar.show();
            canvas.setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::SizeHorCursor));

            // Layout insertion queues QWidget's _q_showIfNotHidden. Deliver those callbacks
            // before deferred deletion, as the application event loop does after mouse-up.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            for (const QPointer<QWidget>& control : retiredControls) {
                require(control && control->isHidden(),
                        "queued layout callbacks must not reopen retired controls as windows");
                require(control->windowHandle() == nullptr,
                        "retired controls must not acquire a native window and steal cursor focus");
            }
            require(canvas.cursor().shape() == Qt::SizeHorCursor,
                    "restored drawing tool must preserve the border cursor");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            for (const QPointer<QWidget>& control : retiredControls) {
                require(control.isNull(), "retired controls must still be deleted asynchronously");
            }
            QCoreApplication::processEvents();
        }
    }
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, nativeSiblingsDisabled);
    storage.shutdown();
}

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    try {
        if (app.arguments().contains(QStringLiteral("--stable-tool-frame-only"))) {
            toolSwitchPreservesNativeFrame();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--border-cursor-only"))) {
            borderCursorSurvivesToolRestoration();
            return 0;
        }
#if defined(Q_OS_MACOS)
        if (app.arguments().contains(QStringLiteral("--macos-cross-display-only"))) {
            if (QGuiApplication::screens().size() < 2 || !macCanPostMouseEvents())
                return 77;
            macosToolbarCrossDisplayDrag();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--macos-shadow-input-only"))) {
            macosToolbarShadowClickThrough();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--macos-logical-only"))) {
            toolSwitchPreservesNativeFrame();
            macosToolbarUsesLogicalGeometry();
            return 0;
        }
#endif
        if (app.arguments().contains(QStringLiteral("--interrupted-drag-only"))) {
            interruptedToolbarDragStopsMoving();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--dpi-frame-reconcile-only"))) {
            dpiCommitReconcilesTheActualFrameBeforePainting();
            dpiCommitPresentsContentWhenUpdatesResume();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--capture-screen-switch-only"))) {
            reusedToolbarFitsOnFirstShowAcrossScreens();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--quick-save-only"))) {
            NoOpToolbarCommands commands;
            ScreenshotToolbarWindow window(commands);
            require(window.palette(), "screenshot toolbar palette unavailable");
            window.palette()->quickSaveRequested();
            require(commands.quickSaveCount == 1,
                    "screenshot Quick save must forward exactly one command");
            window.palette()->saveRequested();
            require(commands.quickSaveCount == 1,
                    "manual Save must not dispatch the Quick save command");
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--keyboard-focus-only"))) {
            qtFocusFlagChangeStillRequiresLayeredSurfacePreservation();
            keyboardFocusTransitionsKeepQtAndNativeStateConsistent();
            floatingToolbarInputsAcquireKeyboardFocus();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--selection-reset-only"))) {
            NoOpToolbarCommands commands;
            ScreenshotToolbarWindow window(commands);
            auto* palette = window.palette();
            require(palette != nullptr, "screenshot toolbar should expose its palette");
            palette->setActiveTool(ScreenshotToolPalette::Tool::Select);
            auto* reset = palette->findChild<adqt::widgets::AdButton*>(
                QStringLiteral("screenshotResetCanvasButton"));
            require(reset != nullptr && reset->isEnabled(),
                    "screenshot reset should be enabled without selection");
            reset->click();
            require(commands.deleteAllElementsCount == 1,
                    "screenshot reset should forward exactly one canvas command");
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--ocr-translation-toggle-only"))) {
            translateButtonRoutesEveryClickThroughTheToggleCommand();
            mainTextTranslationButtonUsesTranslationPresentation();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--jump-to-translation-page-only"))) {
            jumpToTranslationPageFollowsLiveSettingsAndOcrAvailability();
            jumpToTranslationPageCommandMustOutliveItsClickDispatch();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--selection-unit-only"))) {
            screenshotSelectionUnitSurvivesWindowAndCaptureReset();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--toolbar-size-only"))) {
            screenshotToolbarSizeMultiplierSurvivesCaptureReset();
            floatingToolbarsUseTheFixedWindowPreset();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--action-layout-only"))) {
            screenshotActionLayoutReloadIsWindowScopedAndFitsThePreset();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--recording-hit-test-only"))) {
            recordingExportSettingsParticipateInNativeHitTesting();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--native-surface-lifecycle-only"))) {
            toolbarNativeSurfaceCanBeRetiredAndRestored();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--prewarm-lifecycle-only"))) {
            prewarmedToolbarSurfaceStaysHiddenUntilShown();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--reverse-hardware-drag-only"))) {
            physicalDragFromDestinationMonitorAndBackKeepsPhysicalGeometryStable();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--seam-straddle-drag-only"))) {
            slowSeamStraddlingDragKeepsToolbarContentUnmagnified();
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
        try {
            slowSeamStraddlingDragKeepsToolbarContentUnmagnified();
        } catch (const std::exception& error) {
            hardwareDragFailures.push_back(std::string("seam straddle: ") + error.what());
        }
        if (!hardwareDragFailures.empty()) {
            std::ostringstream message;
            for (std::size_t index = 0; index < hardwareDragFailures.size(); ++index) {
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
        scrollingModeRequestsPresentationReposition();
        firstDisplayUsesThePreparedToolbarGeometry();
        captureResetRestoresTheNormalFrameAnchor();
        toolbarNativeSurfaceCanBeRetiredAndRestored();
        prewarmedToolbarSurfaceStaysHiddenUntilShown();
        translateButtonRoutesEveryClickThroughTheToggleCommand();
        mainTextTranslationButtonUsesTranslationPresentation();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
