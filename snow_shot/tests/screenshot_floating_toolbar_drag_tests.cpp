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
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QThread>
#include <QVector>
#include <QWindow>

#include <atomic>
#include <cstdint>
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
};

namespace {
std::atomic_bool nativeGeometryWarningEmitted{false};
QtMessageHandler previousMessageHandler = nullptr;

constexpr QRgb kDpiReproIconColor = qRgb(0, 255, 83);

class DpiReproIconEngine final : public QIconEngine {
  public:
    QIconEngine* clone() const override {
        return new DpiReproIconEngine;
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override {
        if (painter != nullptr) {
            painter->fillRect(rect, QColor::fromRgb(kDpiReproIconColor));
        }
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode, QIcon::State) override {
        QPixmap result(size);
        result.fill(QColor::fromRgb(kDpiReproIconColor));
        return result;
    }
};

QImage renderWidgetAtDpr(QWidget& widget, qreal devicePixelRatio) {
    const QSize physicalSize(qRound(widget.width() * devicePixelRatio),
                             qRound(widget.height() * devicePixelRatio));
    QImage image(physicalSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}

QRect dpiReproIconBounds(const QImage& image) {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) != kDpiReproIconColor) {
                continue;
            }
            bounds = bounds.isValid() ? bounds.united(QRect(x, y, 1, 1)) : QRect(x, y, 1, 1);
        }
    }
    return bounds;
}

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

void require(bool condition, const char* message);

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

LONG absoluteMouseCoordinate(LONG value, LONG origin, LONG extent) {
    if (extent <= 1) {
        return 0;
    }
    return static_cast<LONG>((static_cast<double>(value - origin) * 65535.0) /
                             static_cast<double>(extent - 1));
}

void sendRealMouseInput(const QPoint& position, DWORD flags) {
    const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = absoluteMouseCoordinate(position.x(), left, width);
    input.mi.dy = absoluteMouseCoordinate(position.y(), top, height);
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    require(SendInput(1, &input, sizeof(input)) == 1, "SendInput failed");
}

void settleRealMouseInput() {
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
}

class LeftMouseButtonRestorer final {
  public:
    LeftMouseButtonRestorer() = default;

    void arm() {
        m_buttonDown = true;
    }

    void disarm() {
        m_buttonDown = false;
    }

    ~LeftMouseButtonRestorer() {
        if (!m_buttonDown) {
            return;
        }
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &input, sizeof(input));
    }

    LeftMouseButtonRestorer(const LeftMouseButtonRestorer&) = delete;
    LeftMouseButtonRestorer& operator=(const LeftMouseButtonRestorer&) = delete;

  private:
    bool m_buttonDown = false;
};
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

void draggingToolbarFrom100To150PreservesRenderedMetrics() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    std::vector<HardwareMonitor> monitors;
    require(EnumDisplayMonitors(nullptr, nullptr, collectHardwareMonitor,
                                reinterpret_cast<LPARAM>(&monitors)) != FALSE,
            "DPI repro could not enumerate displays");
    require(populateMonitorDpi(&monitors), "DPI repro could not read display scaling");

    const HardwareMonitor* source = nullptr;
    const HardwareMonitor* destination = nullptr;
    for (const HardwareMonitor& monitor : monitors) {
        if (monitor.dpi == 96 && source == nullptr) {
            source = &monitor;
        }
        if (monitor.dpi == 144 && destination == nullptr) {
            destination = &monitor;
        }
    }
    if (source == nullptr || destination == nullptr) {
        std::string detected;
        for (const HardwareMonitor& monitor : monitors) {
            if (!detected.empty()) {
                detected += "; ";
            }
            detected += std::to_string(monitor.dpi) + " DPI at " +
                        std::to_string(monitor.bounds.left) + "," +
                        std::to_string(monitor.bounds.top) + " " +
                        std::to_string(monitor.bounds.right - monitor.bounds.left) + "x" +
                        std::to_string(monitor.bounds.bottom - monitor.bounds.top);
        }
        throw std::runtime_error(
            "DPI repro requires one 100% (96 DPI) and one 150% (144 DPI) display; detected " +
            detected);
    }

    QScreen* sourceScreen = qtScreenForMonitor(*source);
    QScreen* destinationScreen = qtScreenForMonitor(*destination);
    require(sourceScreen != nullptr && destinationScreen != nullptr,
            "DPI repro could not map the displays to QScreen objects");
    const qreal sourceDpr = sourceScreen->devicePixelRatio();
    const qreal destinationDpr = destinationScreen->devicePixelRatio();
    require(qAbs(sourceDpr - 1.0) < 0.01 && qAbs(destinationDpr - 1.5) < 0.01,
            "QScreen did not expose the expected 1.0/1.5 device pixel ratios");

    POINT originalCursor{};
    require(GetCursorPos(&originalCursor) != FALSE, "DPI repro could not save cursor position");
    const CursorPositionRestorer restoreCursor(originalCursor);

    QWidget overlayOwner;
    overlayOwner.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    overlayOwner.setAttribute(Qt::WA_TransparentForMouseEvents, true);
    overlayOwner.setWindowOpacity(0.0);
    overlayOwner.winId();
    require(overlayOwner.windowHandle() != nullptr,
            "DPI repro selection owner did not create a native window");
    overlayOwner.windowHandle()->setScreen(sourceScreen);
    overlayOwner.setGeometry(sourceScreen->geometry());
    overlayOwner.show();
    settleQueuedRefreshes();
    const HWND selectionOwnerWindow = toNativeHwnd(overlayOwner.winId());
    require(MonitorFromWindow(selectionOwnerWindow, MONITOR_DEFAULTTONULL) == source->handle,
            "DPI repro selection owner did not remain on the 100% display");

    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.resetForNewCapture();
    window.setPlacementContext(sourceScreen, sourceScreen->geometry(),
                               monitorPhysicalBounds(*source));
    window.setOwnerWindow(&overlayOwner);
    window.prepareForDisplay();
    window.show();
    window.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    settleQueuedRefreshes();

    auto* sourceGroup =
        window.palette()->findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup"));
    auto* sourceButton = window.palette()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotShapeButton"));
    require(sourceGroup != nullptr && sourceButton != nullptr,
            "DPI repro toolbar did not create the main shape controls");
    const QList<QAbstractButton*> subToolbarButtons = sourceGroup->findChildren<QAbstractButton*>();
    QAbstractButton* sourceSubToolbarButton =
        subToolbarButtons.isEmpty() ? nullptr : subToolbarButtons.constFirst();
    require(sourceSubToolbarButton != nullptr,
            "DPI repro toolbar did not create the sub-toolbar button group");

    const QIcon reproIcon(new DpiReproIconEngine);
    sourceButton->setIconRef({});
    sourceButton->setIcon(reproIcon);
    sourceSubToolbarButton->setIcon(reproIcon);
    settleQueuedRefreshes();

    const QSize sourceGroupSize = sourceGroup->size();
    const QSize sourceButtonSize = sourceButton->size();
    const QSize sourceIconSize = sourceButton->iconSize();
    const QImage sourceButtonImage = renderWidgetAtDpr(*sourceButton, sourceDpr);
    const QImage sourceSubToolbarButtonImage =
        renderWidgetAtDpr(*sourceSubToolbarButton, sourceDpr);
    const QRect sourceButtonIconBounds = dpiReproIconBounds(sourceButtonImage);
    const QRect sourceSubToolbarIconBounds = dpiReproIconBounds(sourceSubToolbarButtonImage);
    const QList<QWidget*> sourceGroupChildren = sourceGroup->findChildren<QWidget*>();
    QVector<QRect> sourceChildGeometries;
    for (QWidget* child : sourceGroupChildren) {
        if (child->parentWidget() == sourceGroup) {
            sourceChildGeometries.append(child->geometry());
        }
    }
    require(sourceGroupSize.isValid() && sourceGroupSize.width() > 0 &&
                sourceButtonIconBounds.isValid() && sourceSubToolbarIconBounds.isValid() &&
                sourceChildGeometries.size() == 3,
            "DPI repro toolbar did not lay out the grouped controls");

    const HWND nativeWindow = toNativeHwnd(window.winId());
    require(IsWindow(nativeWindow) != FALSE, "DPI repro toolbar did not create a native window");
    const QSize preparedPhysicalSize = nativeWindowSize(nativeWindow);
    require(preparedPhysicalSize.isValid() && !preparedPhysicalSize.isEmpty(),
            "DPI repro could not measure the toolbar window");
    QWidget* dragHandle = window.palette()->dragHandle();
    require(dragHandle != nullptr && !dragHandle->rect().isEmpty(),
            "DPI repro toolbar did not expose a drag handle");
    const QPoint sourceContentPosition = sourceScreen->geometry().topLeft() + QPoint(80, 120);
    window.moveContentTo(sourceContentPosition);
    window.raise();
    settleQueuedRefreshes();
    require(MonitorFromWindow(nativeWindow, MONITOR_DEFAULTTONULL) == source->handle,
            "DPI repro could not position the toolbar on the 100% display");

    const QPoint dragHandleInWindow = dragHandle->mapTo(&window, dragHandle->rect().center());
    const QRect sourceNativeGeometry = nativeWindowGeometry(nativeWindow);
    const QPoint dragStart(sourceNativeGeometry.left() + qRound(dragHandleInWindow.x() * sourceDpr),
                           sourceNativeGeometry.top() + qRound(dragHandleInWindow.y() * sourceDpr));
    const POINT dragStartNative{dragStart.x(), dragStart.y()};
    require(MonitorFromPoint(dragStartNative, MONITOR_DEFAULTTONULL) == source->handle,
            "DPI repro drag handle did not land on the 100% display");

    LeftMouseButtonRestorer mouseButton;
    sendRealMouseInput(dragStart, MOUSEEVENTF_MOVE);
    settleRealMouseInput();
    POINT actualDragStart{};
    require(GetCursorPos(&actualDragStart) != FALSE,
            "DPI repro could not read the cursor before pressing the drag handle");
    const POINT nativeHitPoint{actualDragStart.x, actualDragStart.y};
    const HWND hitWindow = WindowFromPoint(nativeHitPoint);
    const HWND hitRoot = hitWindow != nullptr ? GetAncestor(hitWindow, GA_ROOT) : nullptr;
    const QPoint logicalCursorPosition = QCursor::pos();
    if (hitRoot != nativeWindow || !window.containsInteractiveGlobalPoint(logicalCursorPosition)) {
        throw std::runtime_error(
            "DPI repro real cursor did not hit the toolbar drag handle: requested " +
            std::to_string(dragStart.x()) + "," + std::to_string(dragStart.y()) + ", actual " +
            std::to_string(actualDragStart.x) + "," + std::to_string(actualDragStart.y) +
            ", toolbar HWND " + std::to_string(reinterpret_cast<std::uintptr_t>(nativeWindow)) +
            ", hit root HWND " + std::to_string(reinterpret_cast<std::uintptr_t>(hitRoot)) +
            ", Qt global " + std::to_string(logicalCursorPosition.x()) + "," +
            std::to_string(logicalCursorPosition.y()) + ", interactive " +
            (window.containsInteractiveGlobalPoint(logicalCursorPosition) ? "true" : "false"));
    }
    sendRealMouseInput(dragStart, MOUSEEVENTF_LEFTDOWN);
    mouseButton.arm();
    settleRealMouseInput();
    require(ScreenshotFloatingToolPaletteWindowTestAccess::hasPhysicalDragAnchor(window),
            "DPI repro toolbar did not receive the real mouse press on its drag handle");

    const QPoint destinationCenter = monitorCenter(*destination);
    const int distance = qMax(qAbs(destinationCenter.x() - dragStart.x()),
                              qAbs(destinationCenter.y() - dragStart.y()));
    const int steps = qMax(24, distance / 80);
    for (int step = 1; step <= steps; ++step) {
        const QPoint cursor(
            dragStart.x() +
                qRound(static_cast<qreal>(destinationCenter.x() - dragStart.x()) * step / steps),
            dragStart.y() +
                qRound(static_cast<qreal>(destinationCenter.y() - dragStart.y()) * step / steps));
        sendRealMouseInput(cursor, MOUSEEVENTF_MOVE);
        settleRealMouseInput();
    }
    sendRealMouseInput(destinationCenter, MOUSEEVENTF_LEFTUP);
    mouseButton.disarm();
    settleRealMouseInput();

    require(MonitorFromWindow(nativeWindow, MONITOR_DEFAULTTONULL) == destination->handle,
            "DPI repro toolbar did not reach the 150% display through real mouse input");
    require(GetDpiForWindow(nativeWindow) == destination->dpi,
            "DPI repro toolbar did not receive the 150% window DPI transition");
    require(MonitorFromWindow(selectionOwnerWindow, MONITOR_DEFAULTTONULL) == source->handle,
            "DPI repro moved the screenshot selection away from the 100% display");

    auto* destinationGroup =
        window.palette()->findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup"));
    auto* destinationButton = window.palette()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotShapeButton"));
    require(destinationGroup != nullptr && destinationButton != nullptr,
            "DPI repro DPI transition removed the toolbar controls");

    const auto physicalExtent = [](int logical, qreal dpr) { return qRound(logical * dpr); };
    require(qAbs(physicalExtent(destinationGroup->width(), destinationDpr) -
                 physicalExtent(sourceGroupSize.width(), sourceDpr)) <= 2,
            "grouped toolbar physical width changed after a real mixed-DPI drag");
    require(qAbs(physicalExtent(destinationButton->width(), destinationDpr) -
                 physicalExtent(sourceButtonSize.width(), sourceDpr)) <= 2 &&
                qAbs(physicalExtent(destinationButton->height(), destinationDpr) -
                     physicalExtent(sourceButtonSize.height(), sourceDpr)) <= 2,
            "main toolbar button physical size changed after a real mixed-DPI drag");
    require(qAbs(physicalExtent(destinationButton->iconSize().width(), destinationDpr) -
                 physicalExtent(sourceIconSize.width(), sourceDpr)) <= 2 &&
                qAbs(physicalExtent(destinationButton->iconSize().height(), destinationDpr) -
                     physicalExtent(sourceIconSize.height(), sourceDpr)) <= 2,
            "main toolbar icon physical size changed after a real mixed-DPI drag");

    const QImage destinationButtonImage = renderWidgetAtDpr(*destinationButton, destinationDpr);
    const QImage destinationSubToolbarButtonImage =
        renderWidgetAtDpr(*sourceSubToolbarButton, destinationDpr);
    const QRect destinationButtonIconBounds = dpiReproIconBounds(destinationButtonImage);
    const QRect destinationSubToolbarIconBounds =
        dpiReproIconBounds(destinationSubToolbarButtonImage);
    const bool mainButtonSizePreserved = destinationButton->size() == sourceButtonSize;
    const bool mainIconSizePreserved =
        destinationButtonIconBounds.size() == sourceButtonIconBounds.size();
    const bool subToolbarIconPositionPreserved =
        destinationSubToolbarIconBounds.topLeft() == sourceSubToolbarIconBounds.topLeft();
    if (!mainButtonSizePreserved || !mainIconSizePreserved || !subToolbarIconPositionPreserved) {
        const auto describeSize = [](const QSize& size) {
            return std::to_string(size.width()) + "x" + std::to_string(size.height());
        };
        const auto describeRect = [&describeSize](const QRect& rect) {
            return std::to_string(rect.x()) + "," + std::to_string(rect.y()) + " " +
                   describeSize(rect.size());
        };
        throw std::runtime_error(
            "100% to 150% drag changed rendered metrics: main button " +
            describeSize(sourceButtonSize) + " -> " + describeSize(destinationButton->size()) +
            " logical pixels (physical canvas " + describeSize(sourceButtonImage.size()) + " -> " +
            describeSize(destinationButtonImage.size()) + "), main icon " +
            describeRect(sourceButtonIconBounds) + " -> " +
            describeRect(destinationButtonIconBounds) + ", sub-toolbar icon " +
            describeRect(sourceSubToolbarIconBounds) + " -> " +
            describeRect(destinationSubToolbarIconBounds));
    }

    const QList<QWidget*> destinationGroupChildren = destinationGroup->findChildren<QWidget*>();
    QVector<QRect> destinationChildGeometries;
    for (QWidget* child : destinationGroupChildren) {
        if (child->parentWidget() == destinationGroup) {
            destinationChildGeometries.append(child->geometry());
        }
    }
    require(destinationChildGeometries.size() == sourceChildGeometries.size(),
            "grouped toolbar controls changed count after a real mixed-DPI drag");
    for (int index = 0; index < sourceChildGeometries.size(); ++index) {
        const QRect& sourceGeometry = sourceChildGeometries.at(index);
        const QRect& destinationGeometry = destinationChildGeometries.at(index);
        require(qAbs(physicalExtent(destinationGeometry.x(), destinationDpr) -
                     physicalExtent(sourceGeometry.x(), sourceDpr)) <= 4 &&
                    qAbs(physicalExtent(destinationGeometry.width(), destinationDpr) -
                         physicalExtent(sourceGeometry.width(), sourceDpr)) <= 4,
                "grouped toolbar icon positions changed after a real mixed-DPI drag");
    }
#endif
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
            if (left.handle == right.handle || left.dpi == right.dpi) {
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
        require(SetCursorPos(start.x(), start.y()) != FALSE,
                "failed to move the styled toolbar back to the source monitor");
        ScreenshotFloatingToolPaletteWindowTestAccess::updateDrag(window, QCursor::pos());
        settleQueuedRefreshes();
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
        require(MonitorFromWindow(nativeWindow, MONITOR_DEFAULTTONULL) == source->handle,
                "styled toolbar did not return to the selection monitor");
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

        require(palette->actionPanel() != nullptr && palette->stylePanel() != nullptr,
                "the first secondary-tool activation must materialize both reusable panels");
        require(observer.actionPanelShowCount == (expectActionPanel ? 1 : 0) &&
                    observer.stylePanelShowCount == (expectActionPanel ? 0 : 1),
                "the first secondary-tool activation must never expose the other panel type");
        require(palette->actionPanel()->isHidden() != expectActionPanel &&
                    palette->stylePanel()->isHidden() == expectActionPanel,
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
        require(actionPanel != nullptr && stylePanel != nullptr,
                "activating a drawing tool must materialize both secondary panels");

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
        if (app.arguments().contains(QStringLiteral("--dpi-metrics-only"))) {
            draggingToolbarFrom100To150PreservesRenderedMetrics();
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
