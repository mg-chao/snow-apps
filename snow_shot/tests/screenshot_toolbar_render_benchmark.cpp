#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "../src/presentation/tools/screenshottoolbarperfinstrumentation.h"
#include "widgets/button.h"
#include "widgets/control_scale.h"
#include "widgets/dpi_stable_window_controller.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QPaintEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QSet>
#include <QSysInfo>
#include <QThread>
#include <QTemporaryDir>
#include <QToolTip>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <dwmapi.h>
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#endif

namespace toolbar_perf = snow_shot::presentation::toolbar_perf;

namespace {
#if defined(Q_OS_WIN) || defined(_WIN32)
HWND toNativeHwnd(WId windowId) {
    // Qt transports the native HWND through its integer-valued WId type.
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}
#endif

constexpr int kReportSchemaVersion = 2;
constexpr int kScenarioManifestVersion = 4;
constexpr int kCrossMonitorDragSteps = 240;
constexpr double kRegressionPercent = 15.0;
constexpr double kRegressionAbsoluteMilliseconds = 0.5;

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
        previousMessageHandler = qInstallMessageHandler(captureNativeGeometryWarning);
    }

    ~NativeGeometryWarningScope() {
        qInstallMessageHandler(previousMessageHandler);
        previousMessageHandler = nullptr;
    }
};

struct Aggregate {
    qint64 count = 0;
    qint64 totalNanoseconds = 0;
    qint64 maximumNanoseconds = 0;
};

struct Sample {
    double actionMilliseconds = 0.0;
    double settleMilliseconds = 0.0;
    double compositorMilliseconds = 0.0;
    double totalMilliseconds = 0.0;
    qint64 widgetDirtyPixels = 0;
    qint64 surfaceDirtyPixels = 0;
    qint64 surfacePresentations = 0;
    qint64 auxiliarySurfaceDirtyPixels = 0;
    qint64 auxiliarySurfacePresentations = 0;
    qint64 toolbarPixels = 0;
    double maximumEventGapMilliseconds = 0.0;
    double maximumPaintGapMilliseconds = 0.0;
    QMap<QString, qint64> eventCounts;
    QMap<QString, Aggregate> paintReceivers;
    QMap<QString, Aggregate> traceScopes;
    QMap<QString, qint64> counters;
};

class Collector final : public toolbar_perf::Sink {
  public:
    void setSample(Sample* sample) {
        m_sample = sample;
    }

    void recordScope(const char* name, qint64 elapsedNanoseconds) override {
        if (m_sample == nullptr) {
            return;
        }
        Aggregate& aggregate = m_sample->traceScopes[QString::fromLatin1(name)];
        ++aggregate.count;
        aggregate.totalNanoseconds += elapsedNanoseconds;
        aggregate.maximumNanoseconds = std::max(aggregate.maximumNanoseconds, elapsedNanoseconds);
    }

    void recordCounter(const char* name, qint64 value) override {
        if (m_sample != nullptr) {
            m_sample->counters[QString::fromLatin1(name)] += value;
        }
    }

  private:
    Sample* m_sample = nullptr;
};

QString eventName(QEvent::Type type) {
    switch (type) {
    case QEvent::Paint:
        return QStringLiteral("paint");
    case QEvent::UpdateRequest:
        return QStringLiteral("update_request");
    case QEvent::LayoutRequest:
        return QStringLiteral("layout_request");
    case QEvent::PolishRequest:
        return QStringLiteral("polish_request");
    case QEvent::Show:
        return QStringLiteral("show");
    case QEvent::Hide:
        return QStringLiteral("hide");
    case QEvent::Move:
        return QStringLiteral("move");
    case QEvent::Resize:
        return QStringLiteral("resize");
    case QEvent::Enter:
        return QStringLiteral("enter");
    case QEvent::Leave:
        return QStringLiteral("leave");
    case QEvent::HoverEnter:
        return QStringLiteral("hover_enter");
    case QEvent::HoverMove:
        return QStringLiteral("hover_move");
    case QEvent::HoverLeave:
        return QStringLiteral("hover_leave");
    case QEvent::MouseButtonPress:
        return QStringLiteral("mouse_press");
    case QEvent::MouseButtonRelease:
        return QStringLiteral("mouse_release");
    case QEvent::MouseMove:
        return QStringLiteral("mouse_move");
    case QEvent::Wheel:
        return QStringLiteral("wheel");
    default:
        return {};
    }
}

QString widgetKey(const QWidget& widget) {
    QString identity = widget.objectName();
    if (identity.isEmpty()) {
        identity = widget.accessibleName();
    }
    if (identity.isEmpty()) {
        identity = widget.toolTip();
    }
    if (identity.size() > 80) {
        identity.truncate(80);
    }
    return QStringLiteral("%1:%2").arg(QString::fromLatin1(widget.metaObject()->className()),
                                       identity);
}

class BenchmarkApplication final : public QApplication {
  public:
    BenchmarkApplication(int& argc, char** argv) : QApplication(argc, argv) {
        m_eventClock.start();
    }

    void monitor(ScreenshotToolbarWindow* toolbar, QWidget* owner) {
        m_toolbar = toolbar;
        m_owner = owner;
    }

    void setSample(Sample* sample) {
        m_sample = sample;
        m_previousObservedEventNanoseconds = 0;
        m_previousPaintNanoseconds = 0;
    }

    quint64 observedEventRevision() const {
        return m_observedEventRevision;
    }

    bool notify(QObject* receiver, QEvent* event) override {
        QWidget* widget = qobject_cast<QWidget*>(receiver);
        const bool observed = widget != nullptr && event != nullptr && isObserved(*widget);
        if (!observed || m_sample == nullptr) {
            return QApplication::notify(receiver, event);
        }

        const QString name = eventName(event->type());
        QElapsedTimer timer;
        timer.start();
        const bool handled = QApplication::notify(receiver, event);
        const qint64 elapsed = timer.nsecsElapsed();
        if (!name.isEmpty()) {
            const qint64 now = m_eventClock.nsecsElapsed();
            if (m_previousObservedEventNanoseconds > 0) {
                m_sample->maximumEventGapMilliseconds = std::max(
                    m_sample->maximumEventGapMilliseconds,
                    static_cast<double>(now - m_previousObservedEventNanoseconds) / 1'000'000.0);
            }
            m_previousObservedEventNanoseconds = now;
            ++m_sample->eventCounts[name];
            ++m_observedEventRevision;
        }
        if (event->type() == QEvent::Resize) {
            const auto* resizeEvent = static_cast<QResizeEvent*>(event);
            if (resizeEvent->oldSize() == resizeEvent->size()) {
                ++m_sample->eventCounts[QStringLiteral("resize_unchanged")];
            }
        }
        if (event->type() == QEvent::Paint) {
            const qint64 now = m_eventClock.nsecsElapsed();
            if (m_previousPaintNanoseconds > 0) {
                m_sample->maximumPaintGapMilliseconds =
                    std::max(m_sample->maximumPaintGapMilliseconds,
                             static_cast<double>(now - m_previousPaintNanoseconds) / 1'000'000.0);
            }
            m_previousPaintNanoseconds = now;
            Aggregate& aggregate = m_sample->paintReceivers[widgetKey(*widget)];
            ++aggregate.count;
            aggregate.totalNanoseconds += elapsed;
            aggregate.maximumNanoseconds = std::max(aggregate.maximumNanoseconds, elapsed);
            const auto* paintEvent = static_cast<QPaintEvent*>(event);
            for (const QRect& rect : paintEvent->region()) {
                const qint64 pixels = static_cast<qint64>(rect.width()) * rect.height();
                m_sample->widgetDirtyPixels += pixels;
                if (widget->isWindow()) {
                    if (widget == m_toolbar) {
                        m_sample->surfaceDirtyPixels += pixels;
                    } else {
                        m_sample->auxiliarySurfaceDirtyPixels += pixels;
                    }
                }
            }
            if (widget->isWindow()) {
                if (widget == m_toolbar) {
                    ++m_sample->surfacePresentations;
                } else {
                    ++m_sample->auxiliarySurfacePresentations;
                }
            }
            if (m_toolbar != nullptr) {
                m_sample->toolbarPixels =
                    static_cast<qint64>(m_toolbar->width()) * m_toolbar->height();
            }
        }
        return handled;
    }

  private:
    bool isObserved(const QWidget& widget) const {
        if (m_toolbar == nullptr || &widget == m_owner) {
            return false;
        }
        if (&widget == m_toolbar || m_toolbar->isAncestorOf(&widget)) {
            return true;
        }
        if (!widget.isWindow()) {
            return isObserved(*widget.window());
        }
        const Qt::WindowType type = static_cast<Qt::WindowType>(
            widget.windowFlags().toInt() & static_cast<int>(Qt::WindowType_Mask));
        return widget.isVisible() && (type == Qt::ToolTip || type == Qt::Popup || type == Qt::Tool);
    }

    ScreenshotToolbarWindow* m_toolbar = nullptr;
    QWidget* m_owner = nullptr;
    Sample* m_sample = nullptr;
    quint64 m_observedEventRevision = 0;
    QElapsedTimer m_eventClock;
    qint64 m_previousObservedEventNanoseconds = 0;
    qint64 m_previousPaintNanoseconds = 0;
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
    void repositionToolbarForContentChange() override {}
    void hideColorPickersForScreenshotUi() override {}
};

void flushCompositor() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    static_cast<void>(DwmFlush());
#endif
}

void settleEvents(BenchmarkApplication& application) {
    quint64 previousRevision = application.observedEventRevision();
    int stableTurns = 0;
    for (int turn = 0; turn < 40 && stableTurns < 2; ++turn) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const quint64 revision = application.observedEventRevision();
        if (revision == previousRevision) {
            ++stableTurns;
        } else {
            stableTurns = 0;
            previousRevision = revision;
        }
    }
}

void waitWithEvents(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

#if defined(Q_OS_WIN) || defined(_WIN32)
void sendMouseButton(bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    static_cast<void>(SendInput(1, &input, sizeof(input)));
}

void sendMouseWheel(int delta) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);
    static_cast<void>(SendInput(1, &input, sizeof(input)));
}

void moveNativeCursor(const QPoint& position) {
    QCursor::setPos(position);
    waitWithEvents(4);
}

void movePhysicalCursor(const QPoint& position) {
    static_cast<void>(SetCursorPos(position.x(), position.y()));
    waitWithEvents(4);
}

QRect nativeScreenPhysicalBounds(QScreen* screen) {
    auto* nativeScreen =
        screen != nullptr ? screen->nativeInterface<QNativeInterface::QWindowsScreen>() : nullptr;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (nativeScreen == nullptr || nativeScreen->handle() == nullptr ||
        GetMonitorInfo(nativeScreen->handle(), &info) == FALSE) {
        return {};
    }
    return QRect(info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right - info.rcMonitor.left,
                 info.rcMonitor.bottom - info.rcMonitor.top);
}
#else
void sendMouseButton(bool) {}
void moveNativeCursor(const QPoint& position) {
    QCursor::setPos(position);
}
void movePhysicalCursor(const QPoint& position) {
    QCursor::setPos(position);
}
void sendMouseWheel(int) {}
QRect nativeScreenPhysicalBounds(QScreen* screen) {
    return screen != nullptr ? screen->geometry() : QRect();
}
#endif

void parkCursorOutsideToolbar(QScreen* screen) {
    const QRect bounds = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1600, 900);
    moveNativeCursor(bounds.topLeft() + QPoint(4, 4));
    QToolTip::hideText();
}

bool perMonitorDpiAware() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) ==
           DPI_AWARENESS_PER_MONITOR_AWARE;
#else
    return true;
#endif
}

QSize nativeWindowSize(const QWidget& window) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    RECT bounds{};
    const HWND hwnd = toNativeHwnd(window.winId());
    if (hwnd == nullptr || GetWindowRect(hwnd, &bounds) == FALSE) {
        return {};
    }
    return QSize(bounds.right - bounds.left, bounds.bottom - bounds.top);
#else
    return window.size() * window.devicePixelRatioF();
#endif
}

bool crossMonitorDragKeepsDprConstant(QScreen* first, QScreen* second) {
    if (!perMonitorDpiAware() || first == nullptr || second == nullptr || first == second ||
        !qFuzzyCompare(first->devicePixelRatio() + 1.0, second->devicePixelRatio() + 1.0)) {
        return false;
    }
    const qreal dpr = first->devicePixelRatio();
    const QPoint start = first->availableGeometry().center();
    const QPoint finish = second->availableGeometry().center();
    for (int step = 0; step <= kCrossMonitorDragSteps; ++step) {
        QScreen* pathScreen =
            QGuiApplication::screenAt(start + (finish - start) * step / kCrossMonitorDragSteps);
        if (pathScreen != nullptr &&
            !qFuzzyCompare(pathScreen->devicePixelRatio() + 1.0, dpr + 1.0)) {
            return false;
        }
    }
    return true;
}

struct Fixture {
    explicit Fixture(BenchmarkApplication& application, QScreen* targetScreen)
        : app(application), screen(targetScreen) {
        owner = std::make_unique<QWidget>(nullptr, Qt::FramelessWindowHint | Qt::Tool |
                                                       Qt::WindowStaysOnTopHint);
        owner->setObjectName(QStringLiteral("toolbarPerfOwner"));
        owner->setAttribute(Qt::WA_TranslucentBackground, true);
        toolbar = std::make_unique<ScreenshotToolbarWindow>(commands);
        toolbar->setObjectName(QStringLiteral("toolbarPerfWindow"));
        toolbar->setOwnerWindow(owner.get());
        const QRect bounds =
            screen != nullptr ? screen->availableGeometry() : QRect(100, 100, 1600, 900);
        owner->setGeometry(bounds);
        owner->show();
        toolbar->setPlacementContext(screen, bounds, nativeScreenPhysicalBounds(screen));
        toolbar->prepareForDisplay();
        toolbar->moveContentTo(bounds.center() - QPoint(toolbar->contentSizeHint().width() / 2,
                                                        toolbar->contentSizeHint().height() / 2));
        toolbar->show();
        toolbar->raise();
        app.monitor(toolbar.get(), owner.get());
        settleEvents(app);
        flushCompositor();
    }

    ~Fixture() {
        QToolTip::hideText();
        if (toolbar != nullptr) {
            toolbar->hide();
        }
        if (owner != nullptr) {
            owner->hide();
        }
        app.monitor(nullptr, nullptr);
        settleEvents(app);
    }

    void reset() {
        app.monitor(toolbar.get(), owner.get());
        prewarmedToolbar.reset();
        QToolTip::hideText();
        for (QWidget* topLevel : QApplication::topLevelWidgets()) {
            if (topLevel != toolbar.get() && topLevel != owner.get() && topLevel->isVisible()) {
                topLevel->hide();
            }
        }
        toolbar->resetForNewCapture();
        toolbar->setScrollingScreenshotMode(false);
        toolbar->setActiveTool(ScreenshotToolPalette::Tool::Move);
        toolbar->show();
        settleEvents(app);
        flushCompositor();
    }

    BenchmarkApplication& app;
    QScreen* screen = nullptr;
    NoOpToolbarCommands commands;
    std::unique_ptr<QWidget> owner;
    std::unique_ptr<ScreenshotToolbarWindow> toolbar;
    std::unique_ptr<ScreenshotToolbarWindow> prewarmedToolbar;
};

class TransitionEventAudit final : public QObject {
  public:
    explicit TransitionEventAudit(ScreenshotToolbarWindow* toolbar) : m_toolbar(toolbar) {
        if (m_toolbar == nullptr) {
            return;
        }
        m_toolbar->installEventFilter(this);
        m_watched.append(m_toolbar);
        for (QWidget* widget : m_toolbar->findChildren<QWidget*>()) {
            widget->installEventFilter(this);
            m_watched.append(widget);
            if (dynamic_cast<adqt::widgets::AdControlScaleParticipant*>(widget) != nullptr) {
                m_participants.insert(widget);
            }
        }
        m_commitConnection = QObject::connect(
            m_toolbar, &ScreenshotFloatingToolPaletteWindow::dpiScaleCommitCompleted,
            [this]() { m_commitObserved = true; });
    }

    ~TransitionEventAudit() override {
        QObject::disconnect(m_commitConnection);
        for (const QPointer<QObject>& watched : std::as_const(m_watched)) {
            if (watched != nullptr) {
                watched->removeEventFilter(this);
            }
        }
    }

    int postCommitTopLevelPaints() const {
        return m_postCommitTopLevelPaints;
    }

    int layoutRequests() const {
        return m_layoutRequests;
    }

    int maximumParticipantResizeCount() const {
        int maximum = 0;
        for (auto it = m_participantResizeCounts.cbegin(); it != m_participantResizeCounts.cend();
             ++it) {
            maximum = std::max(maximum, it.value());
        }
        return maximum;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event != nullptr) {
            if (event->type() == QEvent::LayoutRequest) {
                ++m_layoutRequests;
            } else if (event->type() == QEvent::Resize && m_participants.contains(watched)) {
                ++m_participantResizeCounts[watched];
            } else if (event->type() == QEvent::Paint && m_commitObserved && watched == m_toolbar) {
                ++m_postCommitTopLevelPaints;
            }
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    QPointer<ScreenshotToolbarWindow> m_toolbar;
    QList<QPointer<QObject>> m_watched;
    QSet<QObject*> m_participants;
    QHash<QObject*, int> m_participantResizeCounts;
    QMetaObject::Connection m_commitConnection;
    bool m_commitObserved = false;
    int m_postCommitTopLevelPaints = 0;
    int m_layoutRequests = 0;
};

quint64 busyIndicatorFrameCount(const ScreenshotToolbarWindow& toolbar) {
    quint64 count = 0;
    const auto buttons = toolbar.findChildren<adqt::widgets::AdButton*>();
    for (const adqt::widgets::AdButton* button : buttons) {
        count += button->busyIndicatorFrameCount();
    }
    return count;
}

void prepareToolbarOnScreen(Fixture& fixture, QScreen* screen) {
    if (screen == nullptr) {
        return;
    }
    const QRect bounds = screen->availableGeometry();
    const QRect physicalBounds = nativeScreenPhysicalBounds(screen);

    fixture.app.monitor(nullptr, nullptr);
    if (fixture.toolbar != nullptr) {
        fixture.toolbar->hide();
    }
    fixture.toolbar = std::make_unique<ScreenshotToolbarWindow>(fixture.commands);
    fixture.toolbar->setObjectName(QStringLiteral("toolbarPerfWindow"));
    fixture.toolbar->setOwnerWindow(fixture.owner.get());
    fixture.toolbar->resetForNewCapture();
    fixture.screen = screen;
    if (fixture.owner->windowHandle() != nullptr) {
        fixture.owner->windowHandle()->setScreen(screen);
    }
    fixture.owner->setGeometry(bounds);
    fixture.owner->show();
    fixture.toolbar->setPlacementContext(screen, bounds, physicalBounds);
    fixture.toolbar->prepareForDisplay();
    fixture.toolbar->show();
    fixture.toolbar->raise();
    fixture.app.monitor(fixture.toolbar.get(), fixture.owner.get());
    settleEvents(fixture.app);
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = toNativeHwnd(fixture.toolbar->winId());
    RECT windowBounds{};
    if (hwnd != nullptr && GetWindowRect(hwnd, &windowBounds) != FALSE &&
        !physicalBounds.isEmpty()) {
        const QSize physicalSize(windowBounds.right - windowBounds.left,
                                 windowBounds.bottom - windowBounds.top);
        const QPoint targetTopLeft =
            physicalBounds.center() - QPoint(physicalSize.width() / 2, physicalSize.height() / 2);
        static_cast<void>(SetWindowPos(hwnd, nullptr, targetTopLeft.x(), targetTopLeft.y(), 0, 0,
                                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
        settleEvents(fixture.app);
        flushCompositor();
    }
#else
    fixture.toolbar->moveContentTo(bounds.center() -
                                   QPoint(fixture.toolbar->contentSizeHint().width() / 2,
                                          fixture.toolbar->contentSizeHint().height() / 2));
#endif
    fixture.toolbar->prepareForDisplay();
    settleEvents(fixture.app);
    flushCompositor();
}

void smoothlyDragToolbarToScreen(Fixture& fixture, QScreen* destination) {
    QWidget* handle =
        fixture.toolbar->palette() != nullptr ? fixture.toolbar->palette()->dragHandle() : nullptr;
    if (handle == nullptr || destination == nullptr) {
        return;
    }

    const QSize stablePhysicalSize = nativeWindowSize(*fixture.toolbar);
    TransitionEventAudit transitionAudit(fixture.toolbar.get());
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND toolbarHwnd = toNativeHwnd(fixture.toolbar->winId());
    RECT toolbarBounds{};
    GetWindowRect(toolbarHwnd, &toolbarBounds);
    const UINT sourceDpi = toolbarHwnd != nullptr ? GetDpiForWindow(toolbarHwnd) : 0;
    const qreal sourceDpr = sourceDpi > 0 ? sourceDpi / 96.0 : 1.0;
    const QPoint handleCenter = handle->mapTo(fixture.toolbar.get(), handle->rect().center());
    const QPoint start(toolbarBounds.left + qRound(handleCenter.x() * sourceDpr),
                       toolbarBounds.top + qRound(handleCenter.y() * sourceDpr));
    const QPoint finish = nativeScreenPhysicalBounds(destination).center();
    const quint64 transitionCountBefore =
        fixture.toolbar->dpiTransitionDiagnostics().transitionCount;
#else
    const QPoint start = handle->mapToGlobal(handle->rect().center());
    const QPoint finish = destination->availableGeometry().center();
#endif
    int physicalSizeChangedSteps = 0;
    int maximumPhysicalWidthDelta = 0;
    int maximumPhysicalHeightDelta = 0;
    toolbar_perf::counter("layout.commit", 0);
    toolbar_perf::counter("window.geometry_committed", 0);
    toolbar_perf::counter("window.geometry_request_coalesced", 0);
    toolbar_perf::counter("window.resize_reanchor", 0);
    movePhysicalCursor(start);
    const bool handleContainsCursor =
        handle->isVisible() && handle->rect().contains(handle->mapFromGlobal(QCursor::pos()));
    sendMouseButton(true);
    waitWithEvents(4);
    const bool physicalDragBecameActive = fixture.toolbar->physicalDragActive();
    for (int step = 1; step <= kCrossMonitorDragSteps; ++step) {
        movePhysicalCursor(start + (finish - start) * step / kCrossMonitorDragSteps);
        const QSize currentPhysicalSize = nativeWindowSize(*fixture.toolbar);
        if (stablePhysicalSize.isValid() && currentPhysicalSize.isValid() &&
            currentPhysicalSize != stablePhysicalSize) {
            ++physicalSizeChangedSteps;
            maximumPhysicalWidthDelta =
                std::max(maximumPhysicalWidthDelta,
                         qAbs(currentPhysicalSize.width() - stablePhysicalSize.width()));
            maximumPhysicalHeightDelta =
                std::max(maximumPhysicalHeightDelta,
                         qAbs(currentPhysicalSize.height() - stablePhysicalSize.height()));
        }
    }
    sendMouseButton(false);
    waitWithEvents(10);
    fixture.toolbar->cancelDrag();
#if defined(Q_OS_WIN) || defined(_WIN32)
    auto* nativeDestination = destination->nativeInterface<QNativeInterface::QWindowsScreen>();
    const HMONITOR destinationMonitor =
        nativeDestination != nullptr ? nativeDestination->handle() : nullptr;
    const bool reachedDestination =
        toolbarHwnd != nullptr &&
        MonitorFromWindow(toolbarHwnd, MONITOR_DEFAULTTONULL) == destinationMonitor;
    const bool dprChanged =
        toolbarHwnd != nullptr && sourceDpi != 0 && GetDpiForWindow(toolbarHwnd) != sourceDpi;
    const quint64 transitionCountAfter =
        fixture.toolbar->dpiTransitionDiagnostics().transitionCount;
    const bool exactlyOneCommit = transitionCountAfter == transitionCountBefore + 1;
#else
    const bool reachedDestination = false;
    const bool dprChanged = false;
    const bool exactlyOneCommit = false;
#endif
    toolbar_perf::counter("drag.handle_received_press", handleContainsCursor);
    toolbar_perf::counter("drag.physical_drag_became_active", physicalDragBecameActive);
    toolbar_perf::counter("drag.hwnd_reached_destination", reachedDestination);
    toolbar_perf::counter("drag.window_dpr_changed", dprChanged);
    toolbar_perf::counter("drag.logical_transition_commits", exactlyOneCommit);
    toolbar_perf::counter("drag.post_commit_top_level_paints",
                          transitionAudit.postCommitTopLevelPaints());
    toolbar_perf::counter("drag.layout_requests", transitionAudit.layoutRequests());
    toolbar_perf::counter("drag.maximum_participant_resize_count",
                          transitionAudit.maximumParticipantResizeCount());
    const bool transitionWorkIsBatched = transitionAudit.postCommitTopLevelPaints() == 1 &&
                                         transitionAudit.layoutRequests() <= 2 &&
                                         transitionAudit.maximumParticipantResizeCount() <= 1;
    toolbar_perf::counter("drag.sample_valid", handleContainsCursor && physicalDragBecameActive &&
                                                   reachedDestination && dprChanged &&
                                                   exactlyOneCommit && transitionWorkIsBatched);
    toolbar_perf::counter("window.physical_size_changed_steps", physicalSizeChangedSteps);
    toolbar_perf::counter("window.maximum_physical_width_delta_px", maximumPhysicalWidthDelta);
    toolbar_perf::counter("window.maximum_physical_height_delta_px", maximumPhysicalHeightDelta);
}

struct ToolCase {
    const char* id;
    ScreenshotToolPalette::Tool tool;
};

constexpr ToolCase kTools[] = {
    {"move", ScreenshotToolPalette::Tool::Move},
    {"select", ScreenshotToolPalette::Tool::Select},
    {"shape", ScreenshotToolPalette::Tool::Shape},
    {"arrow", ScreenshotToolPalette::Tool::Arrow},
    {"line", ScreenshotToolPalette::Tool::Line},
    {"free_draw", ScreenshotToolPalette::Tool::FreeDraw},
    {"rectangle_highlight", ScreenshotToolPalette::Tool::RectangleHighlight},
    {"pen_highlight", ScreenshotToolPalette::Tool::PenHighlight},
    {"eraser", ScreenshotToolPalette::Tool::Eraser},
    {"filter", ScreenshotToolPalette::Tool::Filter},
    {"watermark", ScreenshotToolPalette::Tool::Watermark},
    {"text", ScreenshotToolPalette::Tool::Text},
    {"serial_number", ScreenshotToolPalette::Tool::SerialNumber},
    {"ocr", ScreenshotToolPalette::Tool::Ocr},
    {"scrolling_screenshot", ScreenshotToolPalette::Tool::ScrollingScreenshot},
};

struct SourceCase {
    const char* id;
    SnowCanvasStyleToolbarSource source;
    ScreenshotToolPalette::Tool tool;
};

constexpr SourceCase kSources[] = {
    {"default_rectangle", SnowCanvasStyleToolbarSource::DefaultRectangle,
     ScreenshotToolPalette::Tool::Shape},
    {"selected_rectangle", SnowCanvasStyleToolbarSource::SelectedRectangle,
     ScreenshotToolPalette::Tool::Shape},
    {"default_arrow", SnowCanvasStyleToolbarSource::DefaultArrow,
     ScreenshotToolPalette::Tool::Arrow},
    {"selected_arrow", SnowCanvasStyleToolbarSource::SelectedArrow,
     ScreenshotToolPalette::Tool::Arrow},
    {"default_line", SnowCanvasStyleToolbarSource::DefaultLine, ScreenshotToolPalette::Tool::Line},
    {"selected_line", SnowCanvasStyleToolbarSource::SelectedLine,
     ScreenshotToolPalette::Tool::Line},
    {"default_free_draw", SnowCanvasStyleToolbarSource::DefaultFreeDraw,
     ScreenshotToolPalette::Tool::FreeDraw},
    {"selected_free_draw", SnowCanvasStyleToolbarSource::SelectedFreeDraw,
     ScreenshotToolPalette::Tool::FreeDraw},
    {"default_rectangle_highlight", SnowCanvasStyleToolbarSource::DefaultRectangleHighlight,
     ScreenshotToolPalette::Tool::RectangleHighlight},
    {"selected_rectangle_highlight", SnowCanvasStyleToolbarSource::SelectedRectangleHighlight,
     ScreenshotToolPalette::Tool::RectangleHighlight},
    {"default_pen_highlight", SnowCanvasStyleToolbarSource::DefaultPenHighlight,
     ScreenshotToolPalette::Tool::PenHighlight},
    {"selected_pen_highlight", SnowCanvasStyleToolbarSource::SelectedPenHighlight,
     ScreenshotToolPalette::Tool::PenHighlight},
    {"eraser", SnowCanvasStyleToolbarSource::Eraser, ScreenshotToolPalette::Tool::Eraser},
    {"default_filter", SnowCanvasStyleToolbarSource::DefaultFilter,
     ScreenshotToolPalette::Tool::Filter},
    {"selected_filter", SnowCanvasStyleToolbarSource::SelectedFilter,
     ScreenshotToolPalette::Tool::Filter},
    {"watermark", SnowCanvasStyleToolbarSource::Watermark, ScreenshotToolPalette::Tool::Watermark},
    {"default_text", SnowCanvasStyleToolbarSource::DefaultText, ScreenshotToolPalette::Tool::Text},
    {"selected_text", SnowCanvasStyleToolbarSource::SelectedText,
     ScreenshotToolPalette::Tool::Text},
    {"default_serial_number", SnowCanvasStyleToolbarSource::DefaultSerialNumber,
     ScreenshotToolPalette::Tool::SerialNumber},
    {"selected_serial_number", SnowCanvasStyleToolbarSource::SelectedSerialNumber,
     ScreenshotToolPalette::Tool::SerialNumber},
};

SnowCanvasStyleToolbarState stateForSource(SnowCanvasStyleToolbarSource source, int variant,
                                           bool mixed) {
    SnowCanvasStyleToolbarState state;
    state.source = source;
    state.shapeStyle.strokeWidth = 2.0 + variant;
    state.shapeStyle.stroke = QColor(30 + variant * 7, 90, 180);
    state.shapeStyle.fill = QColor(200, 50 + variant * 5, 70, 180);
    state.shapeStyle.opacity = variant % 2 == 0 ? 0.7 : 0.9;
    state.textStyle.color = QColor(30, 100 + variant * 3, 180);
    state.textStyle.fontSize = 18.0 + variant;
    state.textStyle.fontFamily = QStringLiteral("Segoe UI");
    state.textStyle.opacity = 0.75;
    state.serialNumberStyle.number = variant + 1;
    state.serialNumberStyle.fontSize = 20.0 + variant;
    state.serialNumberStyle.fontFamily = QStringLiteral("Segoe UI");
    state.serialNumberStyle.opacity = 0.8;
    state.filterStyle.strength = 0.25 + (variant % 5) * 0.1;
    state.filterStyle.opacity = 0.8;
    if (mixed) {
        state.shapeStyleMixed = 0xffffffffu;
        state.textStyleMixed = 0xffffffffu;
        state.serialNumberStyleMixed = 0xffffffffu;
        state.filterStyleMixed = 0xffffffffu;
    }
    return state;
}

struct Scenario {
    QString id;
    QString category;
    QString description;
    std::function<void(Fixture&)> prepare;
    std::function<void(Fixture&)> action;
    int sampleOverride = 0;
    int operationsPerSample = 1;
    bool requiresValidDragSample = false;
};

QVector<QWidget*> visibleInteractiveWidgets(QWidget* root) {
    QVector<QWidget*> widgets;
    if (root == nullptr) {
        return widgets;
    }
    for (QWidget* widget : root->findChildren<QWidget*>()) {
        if (widget != nullptr && widget->isVisible() && widget->isEnabled() &&
            (qobject_cast<QAbstractButton*>(widget) != nullptr ||
             qobject_cast<QAbstractSlider*>(widget) != nullptr)) {
            widgets.push_back(widget);
        }
    }
    return widgets;
}

void sweepVisibleControls(Fixture& fixture, bool click) {
    const QVector<QWidget*> widgets = visibleInteractiveWidgets(fixture.toolbar.get());
    for (QWidget* widget : widgets) {
        if (widget == nullptr || !widget->isVisible()) {
            continue;
        }
        const QPoint center = widget->mapToGlobal(widget->rect().center());
        moveNativeCursor(center);
        if (click && qobject_cast<QAbstractButton*>(widget) != nullptr) {
            sendMouseButton(true);
            waitWithEvents(2);
            sendMouseButton(false);
            waitWithEvents(3);
#if defined(Q_OS_WIN) || defined(_WIN32)
            keybd_event(VK_ESCAPE, 0, 0, 0);
            keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
#endif
        }
    }
}

void dragVisibleSliders(Fixture& fixture) {
    for (QWidget* widget : visibleInteractiveWidgets(fixture.toolbar.get())) {
        auto* slider = qobject_cast<QAbstractSlider*>(widget);
        if (slider == nullptr || slider->width() < 8) {
            continue;
        }
        const QPoint start = slider->mapToGlobal(QPoint(3, slider->height() / 2));
        const QPoint finish =
            slider->mapToGlobal(QPoint(slider->width() - 4, slider->height() / 2));
        moveNativeCursor(start);
        sendMouseButton(true);
        for (int step = 1; step <= 20; ++step) {
            moveNativeCursor(start + (finish - start) * step / 20);
        }
        sendMouseButton(false);
        waitWithEvents(5);
    }
}

QVector<Scenario> createScenarios() {
    QVector<Scenario> scenarios;
    scenarios.push_back({QStringLiteral("lifecycle.reset"),
                         QStringLiteral("lifecycle"),
                         QStringLiteral("Reset a visible production toolbar for a new capture"),
                         {},
                         [](Fixture& f) { f.toolbar->resetForNewCapture(); }});
    scenarios.push_back({QStringLiteral("lifecycle.hide_show"),
                         QStringLiteral("lifecycle"),
                         QStringLiteral("Hide and show the native toolbar window"),
                         {},
                         [](Fixture& f) {
                             f.toolbar->hide();
                             f.toolbar->show();
                             f.toolbar->raise();
                         }});
    scenarios.push_back({QStringLiteral("lifecycle.prepare_unchanged"),
                         QStringLiteral("lifecycle"),
                         QStringLiteral("Prepare an already prepared toolbar"),
                         {},
                         [](Fixture& f) { f.toolbar->prepareForDisplay(); }});
    scenarios.push_back(
        {QStringLiteral("lifecycle.fresh_instance_show"),
         QStringLiteral("lifecycle"),
         QStringLiteral("Construct, prepare, show, present, and destroy a fresh toolbar instance"),
         {},
         [](Fixture& f) {
             ScreenshotToolbarWindow fresh(f.commands);
             fresh.setOwnerWindow(f.owner.get());
             const QRect bounds = f.screen->availableGeometry();
             fresh.setPlacementContext(f.screen, bounds, nativeScreenPhysicalBounds(f.screen));
             fresh.prepareForDisplay();
             fresh.moveContentTo(bounds.center());
             fresh.show();
             fresh.raise();
             waitWithEvents(20);
             flushCompositor();
             fresh.hide();
         },
         10});
    scenarios.push_back(
        {QStringLiteral("lifecycle.prewarmed_first_show"), QStringLiteral("lifecycle"),
         QStringLiteral(
             "Attach and show a hidden toolbar whose layout, style, and icons were prewarmed"),
         [](Fixture& f) {
             f.prewarmedToolbar = std::make_unique<ScreenshotToolbarWindow>(f.commands);
             f.prewarmedToolbar->prewarmForScreen(f.screen);
             f.app.monitor(f.prewarmedToolbar.get(), f.owner.get());
         },
         [](Fixture& f) {
             ScreenshotToolbarWindow& toolbar = *f.prewarmedToolbar;
             toolbar.setOwnerWindow(f.owner.get());
             const QRect bounds = f.screen->availableGeometry();
             toolbar.setPlacementContext(f.screen, bounds, nativeScreenPhysicalBounds(f.screen));
             toolbar.moveContentTo(bounds.center() -
                                   QPoint(toolbar.contentSizeHint().width() / 2,
                                          toolbar.contentSizeHint().height() / 2));
             toolbar.show();
             toolbar.raise();
         },
         20});
    scenarios.push_back({QStringLiteral("placement.reanchor"),
                         QStringLiteral("placement"),
                         QStringLiteral("Move content without changing the preset window size"),
                         {},
                         [](Fixture& f) {
                             f.toolbar->moveContentTo(f.toolbar->contentPosition() + QPoint(3, 2));
                         }});
    scenarios.push_back(
        {QStringLiteral("placement.row_above"), QStringLiteral("placement"),
         QStringLiteral("Move the secondary row above the main toolbar"),
         [](Fixture& f) { f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Shape); },
         [](Fixture& f) { f.toolbar->setStyleToolbarAboveMain(true); }});
    scenarios.push_back({QStringLiteral("placement.row_below"), QStringLiteral("placement"),
                         QStringLiteral("Move the secondary row below the main toolbar"),
                         [](Fixture& f) {
                             f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Shape);
                             f.toolbar->setStyleToolbarAboveMain(true);
                         },
                         [](Fixture& f) { f.toolbar->setStyleToolbarAboveMain(false); }});
    scenarios.push_back(
        {QStringLiteral("placement.movement_bounds"),
         QStringLiteral("placement"),
         QStringLiteral("Apply movement bounds and clamp the current content position"),
         {},
         [](Fixture& f) {
             const QRect bounds = f.screen->availableGeometry().adjusted(20, 20, -20, -20);
             f.toolbar->setMovementBounds(bounds, bounds);
         }});

    for (const ToolCase& tool : kTools) {
        const QString toolId = QString::fromLatin1(tool.id);
        scenarios.push_back({QStringLiteral("tool.activate.") + toolId,
                             QStringLiteral("tool"),
                             QStringLiteral("Activate %1 from Move").arg(toolId),
                             {},
                             [tool](Fixture& f) { f.toolbar->setActiveTool(tool.tool); }});
        scenarios.push_back({QStringLiteral("tool.repeat.") + toolId, QStringLiteral("no_op"),
                             QStringLiteral("Replay the already active %1 tool").arg(toolId),
                             [tool](Fixture& f) { f.toolbar->setActiveTool(tool.tool); },
                             [tool](Fixture& f) { f.toolbar->setActiveTool(tool.tool); }});
        scenarios.push_back(
            {QStringLiteral("interaction.hover_sweep.") + toolId, QStringLiteral("interaction"),
             QStringLiteral("Use the real cursor to hover every visible %1 control").arg(toolId),
             [tool](Fixture& f) { f.toolbar->setActiveTool(tool.tool); },
             [](Fixture& f) { sweepVisibleControls(f, false); }, 1, 32});
        scenarios.push_back(
            {QStringLiteral("interaction.press_sweep.") + toolId, QStringLiteral("interaction"),
             QStringLiteral("Press every visible %1 button and dismiss its popup").arg(toolId),
             [tool](Fixture& f) { f.toolbar->setActiveTool(tool.tool); },
             [](Fixture& f) { sweepVisibleControls(f, true); }, 1, 32});
        scenarios.push_back({QStringLiteral("interaction.slider_sweep.") + toolId,
                             QStringLiteral("continuous_input"),
                             QStringLiteral("Drag every visible %1 slider").arg(toolId),
                             [tool](Fixture& f) { f.toolbar->setActiveTool(tool.tool); },
                             [](Fixture& f) { dragVisibleSliders(f); }, 1, 32});
    }

    int sourceVariant = 0;
    for (const SourceCase& source : kSources) {
        const QString sourceId = QString::fromLatin1(source.id);
        const int variant = sourceVariant++;
        scenarios.push_back(
            {QStringLiteral("style.apply.") + sourceId, QStringLiteral("style_state"),
             QStringLiteral("Apply the %1 style source").arg(sourceId),
             [source](Fixture& f) { f.toolbar->setActiveTool(source.tool); },
             [source, variant](Fixture& f) {
                 f.toolbar->setStyleToolbarState(stateForSource(source.source, variant, false));
             }});
        scenarios.push_back(
            {QStringLiteral("style.value_change.") + sourceId, QStringLiteral("style_state"),
             QStringLiteral("Change values without changing the %1 source").arg(sourceId),
             [source, variant](Fixture& f) {
                 f.toolbar->setActiveTool(source.tool);
                 f.toolbar->setStyleToolbarState(stateForSource(source.source, variant, false));
             },
             [source, variant](Fixture& f) {
                 f.toolbar->setStyleToolbarState(stateForSource(source.source, variant + 1, false));
             }});
        scenarios.push_back(
            {QStringLiteral("style.identical.") + sourceId, QStringLiteral("no_op"),
             QStringLiteral("Replay an identical %1 source state").arg(sourceId),
             [source, variant](Fixture& f) {
                 f.toolbar->setActiveTool(source.tool);
                 f.toolbar->setStyleToolbarState(stateForSource(source.source, variant, false));
             },
             [source, variant](Fixture& f) {
                 f.toolbar->setStyleToolbarState(stateForSource(source.source, variant, false));
             }});
        scenarios.push_back(
            {QStringLiteral("style.mixed.") + sourceId, QStringLiteral("style_state"),
             QStringLiteral("Change %1 from uniform to fully mixed").arg(sourceId),
             [source, variant](Fixture& f) {
                 f.toolbar->setActiveTool(source.tool);
                 f.toolbar->setStyleToolbarState(stateForSource(source.source, variant, false));
             },
             [source, variant](Fixture& f) {
                 f.toolbar->setStyleToolbarState(stateForSource(source.source, variant, true));
             }});
    }

    scenarios.push_back(
        {QStringLiteral("selection.enable_uniform"), QStringLiteral("selection_action"),
         QStringLiteral("Enable the selection-action row with a uniform selected shape"),
         [](Fixture& f) { f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Select); },
         [](Fixture& f) {
             f.toolbar->setStyleToolbarState(
                 stateForSource(SnowCanvasStyleToolbarSource::SelectedRectangle, 2, false));
         }});
    scenarios.push_back(
        {QStringLiteral("selection.enable_mixed"), QStringLiteral("selection_action"),
         QStringLiteral("Render fully mixed selected-element controls"),
         [](Fixture& f) { f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Select); },
         [](Fixture& f) {
             f.toolbar->setStyleToolbarState(
                 stateForSource(SnowCanvasStyleToolbarSource::SelectedText, 3, true));
         }});
    scenarios.push_back(
        {QStringLiteral("selection.disable"), QStringLiteral("selection_action"),
         QStringLiteral("Disable selection actions after returning to a default style source"),
         [](Fixture& f) {
             f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Select);
             f.toolbar->setStyleToolbarState(
                 stateForSource(SnowCanvasStyleToolbarSource::SelectedRectangle, 2, false));
         },
         [](Fixture& f) {
             f.toolbar->setStyleToolbarState(
                 stateForSource(SnowCanvasStyleToolbarSource::DefaultRectangle, 2, false));
         }});
    scenarios.push_back(
        {QStringLiteral("selection.cross_type_text"), QStringLiteral("selection_action"),
         QStringLiteral(
             "Switch style controls to selected text while another creation tool is active"),
         [](Fixture& f) { f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::SerialNumber); },
         [](Fixture& f) {
             f.toolbar->setStyleToolbarState(
                 stateForSource(SnowCanvasStyleToolbarSource::SelectedText, 4, false));
         }});

    const auto watermarkConfig = [](int variant) {
        SnowCanvasWatermarkConfig config;
        config.color = QColor(20 + variant * 10, 40, 80, 210);
        config.text = QStringLiteral("Snow Shot benchmark %1").arg(variant);
        config.fontSize = 14.0 + variant;
        config.fontFamily = variant % 2 == 0 ? QStringLiteral("Segoe UI") : QStringLiteral("Arial");
        config.angle = -30.0 + variant * 5.0;
        config.gap = 20.0 + variant * 3.0;
        config.opacity = 0.4 + variant * 0.05;
        return config;
    };
    scenarios.push_back(
        {QStringLiteral("watermark.config_all_fields"), QStringLiteral("watermark"),
         QStringLiteral("Synchronize all seven watermark configuration fields"),
         [watermarkConfig](Fixture& f) {
             f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Watermark);
             f.toolbar->setWatermarkConfig(watermarkConfig(1));
         },
         [watermarkConfig](Fixture& f) { f.toolbar->setWatermarkConfig(watermarkConfig(2)); }});
    scenarios.push_back(
        {QStringLiteral("watermark.config_identical"), QStringLiteral("no_op"),
         QStringLiteral("Replay an identical seven-field watermark configuration"),
         [watermarkConfig](Fixture& f) {
             f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Watermark);
             f.toolbar->setWatermarkConfig(watermarkConfig(2));
         },
         [watermarkConfig](Fixture& f) { f.toolbar->setWatermarkConfig(watermarkConfig(2)); }});

    scenarios.push_back({QStringLiteral("mode.scrolling_enable"),
                         QStringLiteral("mode"),
                         QStringLiteral("Enable scrolling screenshot mode"),
                         {},
                         [](Fixture& f) { f.toolbar->setScrollingScreenshotMode(true); }});
    scenarios.push_back({QStringLiteral("mode.scrolling_disable"), QStringLiteral("mode"),
                         QStringLiteral("Disable scrolling screenshot mode"),
                         [](Fixture& f) { f.toolbar->setScrollingScreenshotMode(true); },
                         [](Fixture& f) { f.toolbar->setScrollingScreenshotMode(false); }});
    scenarios.push_back(
        {QStringLiteral("interaction.main_tooltip"), QStringLiteral("popup"),
         QStringLiteral("Show the native tooltip for a main toolbar button using the real pointer"),
         [](Fixture& f) { f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Move); },
         [](Fixture& f) {
             for (QAbstractButton* button : f.toolbar->findChildren<QAbstractButton*>()) {
                 if (button->isVisible() && !button->toolTip().isEmpty()) {
                     moveNativeCursor(button->mapToGlobal(button->rect().center()));
                     waitWithEvents(800);
                     break;
                 }
             }
         },
         3});
    scenarios.push_back(
        {QStringLiteral("interaction.shape_wheel"), QStringLiteral("continuous_input"),
         QStringLiteral("Adjust shape stroke width through a native wheel event"),
         [](Fixture& f) { f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Shape); },
         [](Fixture& f) {
             QWidget* panel = f.toolbar->palette()->stylePanel();
             moveNativeCursor(panel->mapToGlobal(panel->rect().center()));
             sendMouseWheel(WHEEL_DELTA);
             waitWithEvents(20);
         },
         5});
    scenarios.push_back({QStringLiteral("stress.identical_style_1000"), QStringLiteral("stress"),
                         QStringLiteral("Replay one style state 1000 times"),
                         [](Fixture& f) {
                             f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Shape);
                             f.toolbar->setStyleToolbarState(stateForSource(
                                 SnowCanvasStyleToolbarSource::DefaultRectangle, 1, false));
                         },
                         [](Fixture& f) {
                             const SnowCanvasStyleToolbarState state = stateForSource(
                                 SnowCanvasStyleToolbarSource::DefaultRectangle, 1, false);
                             for (int i = 0; i < 1000; ++i)
                                 f.toolbar->setStyleToolbarState(state);
                         },
                         5, 1000});
    scenarios.push_back(
        {QStringLiteral("stress.changing_style_1000"), QStringLiteral("stress"),
         QStringLiteral("Apply 1000 changing style states"),
         [](Fixture& f) { f.toolbar->setActiveTool(ScreenshotToolPalette::Tool::Shape); },
         [](Fixture& f) {
             for (int i = 0; i < 1000; ++i) {
                 f.toolbar->setStyleToolbarState(
                     stateForSource(SnowCanvasStyleToolbarSource::DefaultRectangle, i % 17, false));
             }
         },
         5, 1000});
    scenarios.push_back({QStringLiteral("stress.tool_cycle"),
                         QStringLiteral("stress"),
                         QStringLiteral("Cycle every tool 100 times"),
                         {},
                         [](Fixture& f) {
                             for (int round = 0; round < 100; ++round)
                                 for (const ToolCase& tool : kTools)
                                     f.toolbar->setActiveTool(tool.tool);
                         },
                         5,
                         static_cast<int>(std::size(kTools)) * 100});
    scenarios.push_back({QStringLiteral("movement.native_drag_240"),
                         QStringLiteral("movement"),
                         QStringLiteral("Move the native toolbar through a 240-point drag path"),
                         {},
                         [](Fixture& f) {
                             QWidget* handle = f.toolbar->palette() != nullptr
                                                   ? f.toolbar->palette()->dragHandle()
                                                   : nullptr;
                             if (handle == nullptr) {
                                 return;
                             }
                             const QPoint start = handle->mapToGlobal(handle->rect().center());
                             moveNativeCursor(start);
                             sendMouseButton(true);
                             for (int step = 1; step <= 240; ++step) {
                                 const QPoint offset(qRound(std::sin(step / 18.0) * 100.0),
                                                     qRound(std::cos(step / 23.0) * 45.0));
                                 moveNativeCursor(start + offset);
                             }
                             sendMouseButton(false);
                             waitWithEvents(10);
                         },
                         5,
                         240});
    return scenarios;
}

struct Distribution {
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double standardDeviation = 0.0;
    double median = 0.0;
    double mad = 0.0;
    double p90 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

double percentile(const std::vector<double>& sorted, double quantile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = quantile * static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = static_cast<size_t>(std::ceil(position));
    if (lower == upper) {
        return sorted[lower];
    }
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

Distribution distribution(std::vector<double> values) {
    Distribution result;
    if (values.empty()) {
        return result;
    }
    std::sort(values.begin(), values.end());
    result.minimum = values.front();
    result.maximum = values.back();
    result.mean =
        std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    double squaredDifference = 0.0;
    for (double value : values) {
        const double difference = value - result.mean;
        squaredDifference += difference * difference;
    }
    result.standardDeviation = std::sqrt(squaredDifference / static_cast<double>(values.size()));
    result.median = percentile(values, 0.5);
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (double value : values) {
        deviations.push_back(std::abs(value - result.median));
    }
    std::sort(deviations.begin(), deviations.end());
    result.mad = percentile(deviations, 0.5);
    result.p90 = percentile(values, 0.90);
    result.p95 = percentile(values, 0.95);
    result.p99 = percentile(values, 0.99);
    return result;
}

QJsonObject distributionJson(const Distribution& value) {
    return {
        {QStringLiteral("min"), value.minimum},
        {QStringLiteral("max"), value.maximum},
        {QStringLiteral("mean"), value.mean},
        {QStringLiteral("stddev"), value.standardDeviation},
        {QStringLiteral("median"), value.median},
        {QStringLiteral("mad"), value.mad},
        {QStringLiteral("p90"), value.p90},
        {QStringLiteral("p95"), value.p95},
        {QStringLiteral("p99"), value.p99},
    };
}

QJsonObject aggregateJson(const Aggregate& aggregate) {
    return {
        {QStringLiteral("count"), static_cast<double>(aggregate.count)},
        {QStringLiteral("total_ms"), static_cast<double>(aggregate.totalNanoseconds) / 1'000'000.0},
        {QStringLiteral("max_ms"), static_cast<double>(aggregate.maximumNanoseconds) / 1'000'000.0},
    };
}

QJsonObject sampleJson(const Sample& sample) {
    QJsonObject events;
    for (auto it = sample.eventCounts.cbegin(); it != sample.eventCounts.cend(); ++it) {
        events.insert(it.key(), static_cast<double>(it.value()));
    }
    QJsonObject painters;
    for (auto it = sample.paintReceivers.cbegin(); it != sample.paintReceivers.cend(); ++it) {
        painters.insert(it.key(), aggregateJson(it.value()));
    }
    QJsonObject traces;
    for (auto it = sample.traceScopes.cbegin(); it != sample.traceScopes.cend(); ++it) {
        traces.insert(it.key(), aggregateJson(it.value()));
    }
    QJsonObject counters;
    for (auto it = sample.counters.cbegin(); it != sample.counters.cend(); ++it) {
        counters.insert(it.key(), static_cast<double>(it.value()));
    }
    return {
        {QStringLiteral("action_ms"), sample.actionMilliseconds},
        {QStringLiteral("settle_ms"), sample.settleMilliseconds},
        {QStringLiteral("compositor_ms"), sample.compositorMilliseconds},
        {QStringLiteral("total_ms"), sample.totalMilliseconds},
        {QStringLiteral("widget_dirty_pixels"), static_cast<double>(sample.widgetDirtyPixels)},
        {QStringLiteral("surface_dirty_pixels"), static_cast<double>(sample.surfaceDirtyPixels)},
        {QStringLiteral("surface_presentations"), static_cast<double>(sample.surfacePresentations)},
        {QStringLiteral("auxiliary_surface_dirty_pixels"),
         static_cast<double>(sample.auxiliarySurfaceDirtyPixels)},
        {QStringLiteral("auxiliary_surface_presentations"),
         static_cast<double>(sample.auxiliarySurfacePresentations)},
        {QStringLiteral("toolbar_pixels"), static_cast<double>(sample.toolbarPixels)},
        {QStringLiteral("maximum_event_gap_ms"), sample.maximumEventGapMilliseconds},
        {QStringLiteral("maximum_paint_gap_ms"), sample.maximumPaintGapMilliseconds},
        {QStringLiteral("events"), events},
        {QStringLiteral("paint_receivers"), painters},
        {QStringLiteral("trace_scopes"), traces},
        {QStringLiteral("counters"), counters},
    };
}

QJsonObject summarizeScenario(const Scenario& scenario, const QVector<Sample>& samples) {
    std::vector<double> actionValues;
    std::vector<double> settleValues;
    std::vector<double> compositorValues;
    std::vector<double> totalValues;
    std::vector<double> widgetDirtyRatios;
    std::vector<double> surfaceDirtyRatios;
    std::vector<double> eventGaps;
    std::vector<double> paintGaps;
    std::vector<double> paintEvents;
    std::vector<double> surfacePresentations;
    std::vector<double> auxiliarySurfaceDirtyRatios;
    std::vector<double> auxiliarySurfacePresentations;
    std::vector<double> layoutRequestEvents;
    std::vector<double> resizeEvents;
    std::vector<double> unchangedResizeEvents;
    QMap<QString, Aggregate> painters;
    QMap<QString, Aggregate> traces;
    QMap<QString, qint64> events;
    QMap<QString, std::vector<double>> counters;
    QJsonArray rawSamples;
    for (const Sample& sample : samples) {
        actionValues.push_back(sample.actionMilliseconds);
        settleValues.push_back(sample.settleMilliseconds);
        compositorValues.push_back(sample.compositorMilliseconds);
        totalValues.push_back(sample.totalMilliseconds);
        widgetDirtyRatios.push_back(sample.toolbarPixels > 0
                                        ? static_cast<double>(sample.widgetDirtyPixels) /
                                              static_cast<double>(sample.toolbarPixels)
                                        : 0.0);
        surfaceDirtyRatios.push_back(sample.toolbarPixels > 0
                                         ? static_cast<double>(sample.surfaceDirtyPixels) /
                                               static_cast<double>(sample.toolbarPixels)
                                         : 0.0);
        eventGaps.push_back(sample.maximumEventGapMilliseconds);
        paintGaps.push_back(sample.maximumPaintGapMilliseconds);
        paintEvents.push_back(
            static_cast<double>(sample.eventCounts.value(QStringLiteral("paint"))));
        surfacePresentations.push_back(static_cast<double>(sample.surfacePresentations));
        auxiliarySurfaceDirtyRatios.push_back(
            sample.toolbarPixels > 0 ? static_cast<double>(sample.auxiliarySurfaceDirtyPixels) /
                                           static_cast<double>(sample.toolbarPixels)
                                     : 0.0);
        auxiliarySurfacePresentations.push_back(
            static_cast<double>(sample.auxiliarySurfacePresentations));
        layoutRequestEvents.push_back(
            static_cast<double>(sample.eventCounts.value(QStringLiteral("layout_request"))));
        resizeEvents.push_back(
            static_cast<double>(sample.eventCounts.value(QStringLiteral("resize"))));
        unchangedResizeEvents.push_back(
            static_cast<double>(sample.eventCounts.value(QStringLiteral("resize_unchanged"))));
        for (auto it = sample.eventCounts.cbegin(); it != sample.eventCounts.cend(); ++it)
            events[it.key()] += it.value();
        for (auto it = sample.paintReceivers.cbegin(); it != sample.paintReceivers.cend(); ++it) {
            Aggregate& target = painters[it.key()];
            target.count += it.value().count;
            target.totalNanoseconds += it.value().totalNanoseconds;
            target.maximumNanoseconds =
                std::max(target.maximumNanoseconds, it.value().maximumNanoseconds);
        }
        for (auto it = sample.traceScopes.cbegin(); it != sample.traceScopes.cend(); ++it) {
            Aggregate& target = traces[it.key()];
            target.count += it.value().count;
            target.totalNanoseconds += it.value().totalNanoseconds;
            target.maximumNanoseconds =
                std::max(target.maximumNanoseconds, it.value().maximumNanoseconds);
        }
        QSet<QString> counterNames;
        for (auto it = sample.counters.cbegin(); it != sample.counters.cend(); ++it) {
            std::vector<double>& values = counters[it.key()];
            while (values.size() < static_cast<size_t>(rawSamples.size())) {
                values.push_back(0.0);
            }
            values.push_back(static_cast<double>(it.value()));
            counterNames.insert(it.key());
        }
        for (auto it = counters.begin(); it != counters.end(); ++it) {
            if (!counterNames.contains(it.key()) &&
                it.value().size() < static_cast<size_t>(rawSamples.size() + 1)) {
                it.value().push_back(0.0);
            }
        }
        rawSamples.push_back(sampleJson(sample));
    }
    QJsonObject metrics{
        {QStringLiteral("action_ms"), distributionJson(distribution(actionValues))},
        {QStringLiteral("settle_ms"), distributionJson(distribution(settleValues))},
        {QStringLiteral("compositor_ms"), distributionJson(distribution(compositorValues))},
        {QStringLiteral("total_ms"), distributionJson(distribution(totalValues))},
        {QStringLiteral("widget_dirty_area_ratio"),
         distributionJson(distribution(widgetDirtyRatios))},
        {QStringLiteral("surface_dirty_area_ratio"),
         distributionJson(distribution(surfaceDirtyRatios))},
        {QStringLiteral("maximum_event_gap_ms"), distributionJson(distribution(eventGaps))},
        {QStringLiteral("maximum_paint_gap_ms"), distributionJson(distribution(paintGaps))},
        {QStringLiteral("paint_events"), distributionJson(distribution(paintEvents))},
        {QStringLiteral("surface_presentations"),
         distributionJson(distribution(surfacePresentations))},
        {QStringLiteral("auxiliary_surface_dirty_area_ratio"),
         distributionJson(distribution(auxiliarySurfaceDirtyRatios))},
        {QStringLiteral("auxiliary_surface_presentations"),
         distributionJson(distribution(auxiliarySurfacePresentations))},
        {QStringLiteral("layout_request_events"),
         distributionJson(distribution(layoutRequestEvents))},
        {QStringLiteral("resize_events"), distributionJson(distribution(resizeEvents))},
        {QStringLiteral("unchanged_resize_events"),
         distributionJson(distribution(unchangedResizeEvents))},
    };
    QJsonObject eventObject;
    for (auto it = events.cbegin(); it != events.cend(); ++it)
        eventObject.insert(it.key(), static_cast<double>(it.value()));
    QJsonObject painterObject;
    for (auto it = painters.cbegin(); it != painters.cend(); ++it)
        painterObject.insert(it.key(), aggregateJson(it.value()));
    QJsonObject traceObject;
    for (auto it = traces.cbegin(); it != traces.cend(); ++it)
        traceObject.insert(it.key(), aggregateJson(it.value()));
    QJsonObject counterObject;
    QJsonObject normalizedCounterObject;
    for (auto it = counters.cbegin(); it != counters.cend(); ++it) {
        counterObject.insert(it.key(), distributionJson(distribution(it.value())));
        std::vector<double> normalized = it.value();
        const double operations = std::max(1, scenario.operationsPerSample);
        for (double& value : normalized) {
            value /= operations;
        }
        normalizedCounterObject.insert(it.key(),
                                       distributionJson(distribution(std::move(normalized))));
    }
    return {
        {QStringLiteral("id"), scenario.id},
        {QStringLiteral("category"), scenario.category},
        {QStringLiteral("description"), scenario.description},
        {QStringLiteral("status"), QStringLiteral("measured")},
        {QStringLiteral("sample_count"), samples.size()},
        {QStringLiteral("operations_per_sample"), scenario.operationsPerSample},
        {QStringLiteral("metrics"), metrics},
        {QStringLiteral("event_totals"), eventObject},
        {QStringLiteral("paint_receivers"), painterObject},
        {QStringLiteral("trace_scopes"), traceObject},
        {QStringLiteral("structural_counters"), counterObject},
        {QStringLiteral("structural_counters_per_operation"), normalizedCounterObject},
        {QStringLiteral("samples"), rawSamples},
    };
}

QString compilerName() {
#if defined(_MSC_VER)
    return QStringLiteral("MSVC %1").arg(_MSC_VER);
#elif defined(__clang__)
    return QStringLiteral("Clang %1").arg(QString::fromLatin1(__clang_version__));
#elif defined(__GNUC__)
    return QStringLiteral("GCC %1").arg(QString::fromLatin1(__VERSION__));
#else
    return QStringLiteral("unknown");
#endif
}

QString gpuDescription() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    QStringList devices;
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0) != FALSE; ++index) {
        if ((device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) == 0) {
            devices.push_back(QString::fromWCharArray(device.DeviceString));
        }
        device = DISPLAY_DEVICEW{};
        device.cb = sizeof(device);
    }
    devices.removeDuplicates();
    return devices.join(QStringLiteral(" | "));
#else
    return {};
#endif
}

QJsonObject environmentMetadata() {
    QJsonArray screens;
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect geometry = screen->geometry();
        screens.push_back(QJsonObject{
            {QStringLiteral("name"), screen->name()},
            {QStringLiteral("x"), geometry.x()},
            {QStringLiteral("y"), geometry.y()},
            {QStringLiteral("width"), geometry.width()},
            {QStringLiteral("height"), geometry.height()},
            {QStringLiteral("dpr"), screen->devicePixelRatio()},
            {QStringLiteral("logical_dpi"), screen->logicalDotsPerInch()},
            {QStringLiteral("physical_dpi"), screen->physicalDotsPerInch()},
            {QStringLiteral("refresh_hz"), screen->refreshRate()},
        });
    }
    QString cpu;
#if defined(Q_OS_WIN) || defined(_WIN32)
    cpu = QSysInfo::currentCpuArchitecture();
    BOOL dwmEnabled = FALSE;
    const bool compositionEnabled =
        SUCCEEDED(DwmIsCompositionEnabled(&dwmEnabled)) && dwmEnabled != FALSE;
    SYSTEM_POWER_STATUS power{};
    const bool hasPowerStatus = GetSystemPowerStatus(&power) != FALSE;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    const bool hasMemoryStatus = GlobalMemoryStatusEx(&memory) != FALSE;
#else
    const bool compositionEnabled = false;
    const bool hasPowerStatus = false;
    const bool hasMemoryStatus = false;
#endif
    const QString executablePath = QCoreApplication::applicationFilePath();
    QFile executable(executablePath);
    QString executableHash;
    if (executable.open(QIODevice::ReadOnly)) {
        executableHash = QString::fromLatin1(
            QCryptographicHash::hash(executable.readAll(), QCryptographicHash::Sha256).toHex());
    }
    QJsonObject environment{
        {QStringLiteral("timestamp_utc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("git_commit"), qEnvironmentVariable("SNOW_SHOT_PERF_GIT_COMMIT")},
        {QStringLiteral("git_dirty"),
         qEnvironmentVariableIntValue("SNOW_SHOT_PERF_GIT_DIRTY") != 0},
        {QStringLiteral("executable_sha256"), executableHash},
        {QStringLiteral("build_type"), QStringLiteral("Release")},
        {QStringLiteral("compiler"), compilerName()},
        {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
        {QStringLiteral("qpa_platform"), QGuiApplication::platformName()},
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("kernel"), QSysInfo::kernelVersion()},
        {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("cpu"), cpu},
        {QStringLiteral("logical_threads"), QThread::idealThreadCount()},
        {QStringLiteral("gpu"), gpuDescription()},
        {QStringLiteral("gpu_driver"), qEnvironmentVariable("SNOW_SHOT_PERF_GPU_DRIVER")},
        {QStringLiteral("power_plan"), qEnvironmentVariable("SNOW_SHOT_PERF_POWER_PLAN")},
        {QStringLiteral("dwm_composition"), compositionEnabled},
        {QStringLiteral("power_status_available"), hasPowerStatus},
        {QStringLiteral("memory_status_available"), hasMemoryStatus},
        {QStringLiteral("screens"), screens},
    };
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (hasPowerStatus) {
        environment.insert(QStringLiteral("on_ac_power"), power.ACLineStatus == 1);
        environment.insert(QStringLiteral("battery_percent"),
                           power.BatteryLifePercent <= 100 ? power.BatteryLifePercent : -1);
    }
    if (hasMemoryStatus) {
        environment.insert(QStringLiteral("physical_memory_bytes"),
                           static_cast<double>(memory.ullTotalPhys));
        environment.insert(QStringLiteral("available_memory_bytes"),
                           static_cast<double>(memory.ullAvailPhys));
    }
#endif
    QJsonObject fingerprintInput{
        {QStringLiteral("schema"), kReportSchemaVersion},
        {QStringLiteral("manifest"), kScenarioManifestVersion},
        {QStringLiteral("qt"), environment.value(QStringLiteral("qt_version"))},
        {QStringLiteral("os"), environment.value(QStringLiteral("os"))},
        {QStringLiteral("kernel"), environment.value(QStringLiteral("kernel"))},
        {QStringLiteral("architecture"), environment.value(QStringLiteral("architecture"))},
        {QStringLiteral("cpu"), environment.value(QStringLiteral("cpu"))},
        {QStringLiteral("gpu"), environment.value(QStringLiteral("gpu"))},
        {QStringLiteral("gpu_driver"), environment.value(QStringLiteral("gpu_driver"))},
        {QStringLiteral("screens"), screens},
    };
    environment.insert(
        QStringLiteral("fingerprint"),
        QString::fromLatin1(
            QCryptographicHash::hash(QJsonDocument(fingerprintInput).toJson(QJsonDocument::Compact),
                                     QCryptographicHash::Sha256)
                .toHex()));
    return environment;
}

double jsonP95(const QJsonObject& scenario, const QString& metric) {
    return scenario.value(QStringLiteral("metrics"))
        .toObject()
        .value(metric)
        .toObject()
        .value(QStringLiteral("p95"))
        .toDouble();
}

QJsonObject compareWithBaseline(QJsonArray* scenarios, const QJsonObject& environment,
                                const QString& baselinePath, bool forceCompare) {
    QJsonObject comparison{
        {QStringLiteral("requested"), !baselinePath.isEmpty()},
        {QStringLiteral("compatible"), false},
        {QStringLiteral("forced"), forceCompare},
        {QStringLiteral("timing_threshold_percent"), kRegressionPercent},
        {QStringLiteral("timing_threshold_absolute_ms"), kRegressionAbsoluteMilliseconds},
        {QStringLiteral("regression_count"), 0},
    };
    if (baselinePath.isEmpty()) {
        comparison.insert(QStringLiteral("status"), QStringLiteral("no_baseline"));
        return comparison;
    }
    QFile file(baselinePath);
    if (!file.open(QIODevice::ReadOnly)) {
        comparison.insert(QStringLiteral("status"), QStringLiteral("baseline_unreadable"));
        comparison.insert(QStringLiteral("message"), file.errorString());
        return comparison;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        comparison.insert(QStringLiteral("status"), QStringLiteral("baseline_invalid"));
        comparison.insert(QStringLiteral("message"), error.errorString());
        return comparison;
    }
    const QJsonObject baseline = document.object();
    const QJsonObject baselineEnvironment =
        baseline.value(QStringLiteral("environment")).toObject();
    const bool compatible =
        baseline.value(QStringLiteral("schema_version")).toInt() == kReportSchemaVersion &&
        baseline.value(QStringLiteral("scenario_manifest_version")).toInt() ==
            kScenarioManifestVersion &&
        baselineEnvironment.value(QStringLiteral("fingerprint")).toString() ==
            environment.value(QStringLiteral("fingerprint")).toString();
    comparison.insert(QStringLiteral("compatible"), compatible);
    comparison.insert(QStringLiteral("status"), compatible ? QStringLiteral("comparable")
                                                : forceCompare
                                                    ? QStringLiteral("forced_incompatible")
                                                    : QStringLiteral("incompatible"));
    comparison.insert(QStringLiteral("baseline_fingerprint"),
                      baselineEnvironment.value(QStringLiteral("fingerprint")));
    comparison.insert(QStringLiteral("current_fingerprint"),
                      environment.value(QStringLiteral("fingerprint")));
    if (!compatible && !forceCompare) {
        return comparison;
    }

    QMap<QString, QJsonObject> baselineById;
    for (const auto& value : baseline.value(QStringLiteral("scenarios")).toArray()) {
        const QJsonObject object = value.toObject();
        baselineById.insert(object.value(QStringLiteral("id")).toString(), object);
    }
    int regressionCount = 0;
    int comparedCount = 0;
    int newCount = 0;
    for (qsizetype index = 0; index < scenarios->size(); ++index) {
        QJsonObject current = scenarios->at(index).toObject();
        const QString id = current.value(QStringLiteral("id")).toString();
        const auto baselineIt = baselineById.constFind(id);
        QJsonObject result;
        if (baselineIt == baselineById.cend()) {
            result.insert(QStringLiteral("status"), QStringLiteral("new"));
            ++newCount;
            current.insert(QStringLiteral("baseline"), result);
            scenarios->replace(index, current);
            continue;
        }
        ++comparedCount;
        const double currentP95 = jsonP95(current, QStringLiteral("total_ms"));
        const double baselineP95 = jsonP95(*baselineIt, QStringLiteral("total_ms"));
        const double deltaMs = currentP95 - baselineP95;
        const double deltaPercent = baselineP95 > 0.0 ? deltaMs / baselineP95 * 100.0 : 0.0;
        bool structuralRegression = false;
        QJsonArray structuralChanges;
        const QJsonObject currentCounters =
            current.value(QStringLiteral("structural_counters_per_operation")).toObject();
        const QJsonObject baselineCounters =
            baselineIt->value(QStringLiteral("structural_counters_per_operation")).toObject();
        for (auto counterIt = currentCounters.begin(); counterIt != currentCounters.end();
             ++counterIt) {
            if (!baselineCounters.contains(counterIt.key())) {
                continue;
            }
            const double currentCounterP95 =
                counterIt.value().toObject().value(QStringLiteral("p95")).toDouble();
            const double baselineCounterP95 = baselineCounters.value(counterIt.key())
                                                  .toObject()
                                                  .value(QStringLiteral("p95"))
                                                  .toDouble();
            if (currentCounterP95 > baselineCounterP95 + 0.001) {
                structuralRegression = true;
                structuralChanges.push_back(QJsonObject{
                    {QStringLiteral("counter"), counterIt.key()},
                    {QStringLiteral("baseline_p95"), baselineCounterP95},
                    {QStringLiteral("current_p95"), currentCounterP95},
                });
            }
        }
        const bool timingRegression =
            deltaMs > kRegressionAbsoluteMilliseconds && deltaPercent > kRegressionPercent;
        const bool regression = timingRegression || structuralRegression;
        if (regression) {
            ++regressionCount;
        }
        result = {
            {QStringLiteral("status"),
             regression ? QStringLiteral("regression") : QStringLiteral("pass")},
            {QStringLiteral("baseline_total_p95_ms"), baselineP95},
            {QStringLiteral("current_total_p95_ms"), currentP95},
            {QStringLiteral("delta_ms"), deltaMs},
            {QStringLiteral("delta_percent"), deltaPercent},
            {QStringLiteral("timing_regression"), timingRegression},
            {QStringLiteral("structural_regression"), structuralRegression},
            {QStringLiteral("structural_changes"), structuralChanges},
        };
        current.insert(QStringLiteral("baseline"), result);
        scenarios->replace(index, current);
    }
    comparison.insert(QStringLiteral("regression_count"), regressionCount);
    comparison.insert(QStringLiteral("compared_scenario_count"), comparedCount);
    comparison.insert(QStringLiteral("new_scenario_count"), newCount);
    return comparison;
}

QJsonArray recommendationsFor(const QJsonArray& scenarios) {
    QJsonArray recommendations;
    for (const auto& value : scenarios) {
        const QJsonObject scenario = value.toObject();
        const QString id = scenario.value(QStringLiteral("id")).toString();
        const QString category = scenario.value(QStringLiteral("category")).toString();
        const double operations =
            std::max(1.0, scenario.value(QStringLiteral("operations_per_sample")).toDouble(1.0));
        const QJsonObject events = scenario.value(QStringLiteral("event_totals")).toObject();
        const QJsonObject counters =
            scenario.value(QStringLiteral("structural_counters_per_operation")).toObject();
        const double layoutP95 = counters.value(QStringLiteral("layout.commit"))
                                     .toObject()
                                     .value(QStringLiteral("p95"))
                                     .toDouble();
        const double resizeP95 = counters.value(QStringLiteral("window.resize_reanchor"))
                                     .toObject()
                                     .value(QStringLiteral("p95"))
                                     .toDouble();
        const double physicalSizeChangesP95 =
            counters.value(QStringLiteral("window.physical_size_changed_steps"))
                .toObject()
                .value(QStringLiteral("p95"))
                .toDouble();
        const double physicalWidthDeltaP95 =
            counters.value(QStringLiteral("window.maximum_physical_width_delta_px"))
                .toObject()
                .value(QStringLiteral("p95"))
                .toDouble();
        const double physicalHeightDeltaP95 =
            counters.value(QStringLiteral("window.maximum_physical_height_delta_px"))
                .toObject()
                .value(QStringLiteral("p95"))
                .toDouble();
        const QJsonObject metrics = scenario.value(QStringLiteral("metrics")).toObject();
        const double dirtyP95 = metrics.value(QStringLiteral("surface_dirty_area_ratio"))
                                    .toObject()
                                    .value(QStringLiteral("p95"))
                                    .toDouble();
        const double paintGapP95 = metrics.value(QStringLiteral("maximum_paint_gap_ms"))
                                       .toObject()
                                       .value(QStringLiteral("p95"))
                                       .toDouble();
        const double paintEventsP95 = metrics.value(QStringLiteral("paint_events"))
                                          .toObject()
                                          .value(QStringLiteral("p95"))
                                          .toDouble();
        const double layoutRequestP95 = metrics.value(QStringLiteral("layout_request_events"))
                                            .toObject()
                                            .value(QStringLiteral("p95"))
                                            .toDouble();
        const double resizeEventP95 = metrics.value(QStringLiteral("resize_events"))
                                          .toObject()
                                          .value(QStringLiteral("p95"))
                                          .toDouble();
        if (layoutP95 > 1.0 && id != QStringLiteral("lifecycle.fresh_instance_show")) {
            recommendations.push_back(QJsonObject{
                {QStringLiteral("scenario"), id},
                {QStringLiteral("kind"), QStringLiteral("layout_amplification")},
                {QStringLiteral("message"),
                 QStringLiteral(
                     "One logical update commits the toolbar layout more than once; inspect "
                     "dirty-state propagation and repeated synchronous geometry queries")},
                {QStringLiteral("evidence"),
                 QStringLiteral("layout.commit p95 = %1").arg(layoutP95)},
            });
        }
        if (category == QStringLiteral("no_op") && paintEventsP95 > 0.0) {
            recommendations.push_back(QJsonObject{
                {QStringLiteral("scenario"), id},
                {QStringLiteral("kind"), QStringLiteral("unchanged_state_repaint")},
                {QStringLiteral("message"),
                 QStringLiteral("An unchanged state replay still paints widgets; add equality "
                                "guards before visual setters or update requests")},
                {QStringLiteral("evidence"),
                 QStringLiteral("paint-event p95 = %1").arg(paintEventsP95)},
            });
        }
        if (category == QStringLiteral("movement") &&
            !id.contains(QStringLiteral("cross_monitor")) &&
            (layoutP95 > 0.0 || resizeP95 > 0.0 || layoutRequestP95 > 0.0 ||
             resizeEventP95 > 0.0)) {
            recommendations.push_back(QJsonObject{
                {QStringLiteral("scenario"), id},
                {QStringLiteral("kind"), QStringLiteral("drag_structural_work")},
                {QStringLiteral("message"),
                 QStringLiteral("Position-only drag performs layout or resize work; keep drag "
                                "updates on the native move-only path")},
                {QStringLiteral("evidence"),
                 QStringLiteral("layout commits p95 %1, resize/reanchors p95 %2, layout requests "
                                "p95 %3, resize events p95 %4")
                     .arg(layoutP95)
                     .arg(resizeP95)
                     .arg(layoutRequestP95)
                     .arg(resizeEventP95)},
            });
        }
        if (physicalSizeChangesP95 > 0.0) {
            recommendations.push_back(QJsonObject{
                {QStringLiteral("scenario"), id},
                {QStringLiteral("kind"), QStringLiteral("physical_size_instability")},
                {QStringLiteral("message"),
                 QStringLiteral(
                     "The toolbar HWND changes physical pixel size during the drag; preserve the "
                     "prepared GetWindowRect size across every monitor transition")},
                {QStringLiteral("evidence"),
                 QStringLiteral("changed steps p95 %1, maximum width delta p95 %2 px, maximum "
                                "height delta p95 %3 px")
                     .arg(physicalSizeChangesP95)
                     .arg(physicalWidthDeltaP95)
                     .arg(physicalHeightDeltaP95)},
            });
        }
        if (dirtyP95 / operations > 2.0 && events.value(QStringLiteral("paint")).toDouble() > 0.0) {
            recommendations.push_back(QJsonObject{
                {QStringLiteral("scenario"), id},
                {QStringLiteral("kind"), QStringLiteral("paint_amplification")},
                {QStringLiteral("message"),
                 QStringLiteral(
                     "The accumulated dirty area exceeds two full toolbar areas per sample; narrow "
                     "update regions and avoid repainting unaffected controls")},
                {QStringLiteral("evidence"),
                 QStringLiteral("dirty-area p95 = %1x toolbar per operation")
                     .arg(dirtyP95 / operations, 0, 'f', 2)},
            });
        }
        if (category == QStringLiteral("animation") && paintGapP95 > 50.0) {
            recommendations.push_back(QJsonObject{
                {QStringLiteral("scenario"), id},
                {QStringLiteral("kind"), QStringLiteral("animation_frame_gap")},
                {QStringLiteral("message"),
                 QStringLiteral("Animation paint gaps exceed 50 ms; inspect main-thread stalls and "
                                "animation update cadence")},
                {QStringLiteral("evidence"),
                 QStringLiteral("maximum paint-gap p95 = %1 ms").arg(paintGapP95, 0, 'f', 2)},
            });
        }
    }
    return recommendations;
}

QString htmlEscape(QString value) {
    value.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    value.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    value.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    value.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return value;
}

QString formatNumber(double value) {
    return QString::number(value, 'f', value >= 100.0 ? 1 : 3);
}

bool writeHtmlReport(const QString& path, const QJsonObject& report, QString* error) {
    const QJsonObject environment = report.value(QStringLiteral("environment")).toObject();
    const QJsonObject comparison = report.value(QStringLiteral("comparison")).toObject();
    const QJsonArray scenarios = report.value(QStringLiteral("scenarios")).toArray();
    const QJsonArray recommendations = report.value(QStringLiteral("recommendations")).toArray();
    QString rows;
    for (const auto& value : scenarios) {
        const QJsonObject scenario = value.toObject();
        const QJsonObject baseline = scenario.value(QStringLiteral("baseline")).toObject();
        const QString status =
            baseline.value(QStringLiteral("status")).toString(QStringLiteral("uncompared"));
        const double total = jsonP95(scenario, QStringLiteral("total_ms"));
        const double action = jsonP95(scenario, QStringLiteral("action_ms"));
        const double compositor = jsonP95(scenario, QStringLiteral("compositor_ms"));
        const QJsonObject metrics = scenario.value(QStringLiteral("metrics")).toObject();
        const double surfaceRatio = metrics.value(QStringLiteral("surface_dirty_area_ratio"))
                                        .toObject()
                                        .value(QStringLiteral("p95"))
                                        .toDouble();
        const double surfacePresentations = metrics.value(QStringLiteral("surface_presentations"))
                                                .toObject()
                                                .value(QStringLiteral("p95"))
                                                .toDouble();
        const double paintCount = metrics.value(QStringLiteral("paint_events"))
                                      .toObject()
                                      .value(QStringLiteral("p95"))
                                      .toDouble();
        const double layout = scenario.value(QStringLiteral("structural_counters_per_operation"))
                                  .toObject()
                                  .value(QStringLiteral("layout.commit"))
                                  .toObject()
                                  .value(QStringLiteral("p95"))
                                  .toDouble();
        rows += QStringLiteral("<tr data-status=\"%1\"><td><code>%2</code><small>%3</small></td>"
                               "<td>%4</td><td data-n=\"%5\">%5</td><td data-n=\"%6\">%6</td>"
                               "<td data-n=\"%7\">%7</td><td data-n=\"%8\">%8</td>"
                               "<td data-n=\"%9\">%9</td><td data-n=\"%10\">%10</td>"
                               "<td data-n=\"%11\">%11</td><td class=\"status %1\">%1</td></tr>")
                    .arg(htmlEscape(status),
                         htmlEscape(scenario.value(QStringLiteral("id")).toString()),
                         htmlEscape(scenario.value(QStringLiteral("description")).toString()),
                         htmlEscape(scenario.value(QStringLiteral("category")).toString()),
                         formatNumber(total), formatNumber(action), formatNumber(compositor),
                         formatNumber(surfacePresentations), formatNumber(surfaceRatio),
                         formatNumber(paintCount), formatNumber(layout));
    }
    QString recommendationRows;
    for (const auto& value : recommendations) {
        const QJsonObject item = value.toObject();
        recommendationRows +=
            QStringLiteral(
                "<article><h3>%1</h3><code>%2</code><p>%3</p><strong>%4</strong></article>")
                .arg(htmlEscape(item.value(QStringLiteral("kind")).toString()),
                     htmlEscape(item.value(QStringLiteral("scenario")).toString()),
                     htmlEscape(item.value(QStringLiteral("message")).toString()),
                     htmlEscape(item.value(QStringLiteral("evidence")).toString()));
    }
    QByteArray embedded = QJsonDocument(report).toJson(QJsonDocument::Compact);
    embedded.replace("<", "\\u003c");
    const QString html =
        QStringLiteral(R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Screenshot Toolbar Performance</title>
<style>
:root{font-family:"Segoe UI",sans-serif;color:#202124;background:#f5f7f8}body{margin:0}header{background:#fff;border-bottom:1px solid #dfe3e6;padding:24px 32px}main{padding:24px 32px;max-width:1600px;margin:auto}.meta{display:flex;gap:24px;flex-wrap:wrap;color:#53606a}.band{background:#fff;border:1px solid #dfe3e6;border-radius:6px;padding:18px;margin-bottom:20px}h1{margin:0 0 10px;font-size:26px}h2{font-size:18px;margin-top:0}h3{font-size:14px;margin:0 0 6px}table{border-collapse:collapse;width:100%;font-size:12px}th,td{border-bottom:1px solid #e5e8ea;padding:8px;text-align:right}th{position:sticky;top:0;background:#f8fafb;cursor:pointer}th:first-child,td:first-child,th:nth-child(2),td:nth-child(2){text-align:left}td small{display:block;color:#69757d;max-width:470px}.status.regression{color:#b42318;font-weight:700}.status.pass{color:#067647}.status.new,.status.uncompared{color:#6941c6}article{border-left:3px solid #d92d20;padding:10px 14px;margin:10px 0;background:#fff8f7}code{font-family:Consolas,monospace}details{margin-top:20px}pre{overflow:auto;max-height:480px;background:#111827;color:#d1d5db;padding:16px}input{padding:8px 10px;width:320px;border:1px solid #b8c1c7;border-radius:4px}
</style></head><body><header><h1>Screenshot Toolbar Rendering Performance</h1><div class="meta"><span>%1</span><span>Qt %2</span><span>%3</span><span>Baseline: %4</span><span>Regressions: %5</span></div></header><main>
<section class="band"><h2>Measured Scenarios</h2><p>Times and per-sample presentation metrics are p95. Structural counters are normalized per operation.</p><input id="filter" placeholder="Filter scenarios"><div style="overflow:auto;max-height:720px"><table id="scenarios"><thead><tr><th>Scenario</th><th>Category</th><th>Total ms</th><th>Action ms</th><th>DWM ms</th><th>Surface presents</th><th>Surface dirty ratio</th><th>Paint events</th><th>Layout/op</th><th>Status</th></tr></thead><tbody>%6</tbody></table></div></section>
<section class="band"><h2>Tuning Evidence</h2>%7</section>
<section class="band"><h2>Environment</h2><pre>%8</pre></section>
<details class="band"><summary>Complete report JSON</summary><pre id="raw"></pre></details>
</main><script id="data" type="application/json">%9</script><script>
const table=document.querySelector('#scenarios'),body=table.tBodies[0];document.querySelector('#filter').addEventListener('input',e=>{const q=e.target.value.toLowerCase();for(const r of body.rows)r.hidden=!r.textContent.toLowerCase().includes(q)});for(const [i,h] of [...table.tHead.rows[0].cells].entries())h.onclick=()=>{const rows=[...body.rows],num=i>=2&&i<=8;rows.sort((a,b)=>num?(parseFloat(a.cells[i].dataset.n||a.cells[i].textContent)-parseFloat(b.cells[i].dataset.n||b.cells[i].textContent)):a.cells[i].textContent.localeCompare(b.cells[i].textContent));rows.forEach(r=>body.appendChild(r))};document.querySelector('#raw').textContent=JSON.stringify(JSON.parse(document.querySelector('#data').textContent),null,2);
</script></body></html>)HTML")
            .arg(htmlEscape(environment.value(QStringLiteral("timestamp_utc")).toString()),
                 htmlEscape(environment.value(QStringLiteral("qt_version")).toString()),
                 htmlEscape(environment.value(QStringLiteral("cpu")).toString()),
                 htmlEscape(comparison.value(QStringLiteral("status")).toString()),
                 QString::number(comparison.value(QStringLiteral("regression_count")).toInt()),
                 rows,
                 recommendationRows.isEmpty()
                     ? QStringLiteral("<p>No rule-based tuning findings were triggered.</p>")
                     : recommendationRows,
                 htmlEscape(
                     QString::fromUtf8(QJsonDocument(environment).toJson(QJsonDocument::Indented))),
                 QString::fromUtf8(embedded));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr)
            *error = file.errorString();
        return false;
    }
    file.write(html.toUtf8());
    return true;
}

bool wildcardMatches(const QString& pattern, const QString& value) {
    if (pattern.isEmpty() || pattern == QStringLiteral("*")) {
        return true;
    }
    const QRegularExpression expression(QRegularExpression::wildcardToRegularExpression(pattern),
                                        QRegularExpression::CaseInsensitiveOption);
    return expression.match(value).hasMatch();
}

bool runSelfTests() {
    const auto require = [](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "toolbar benchmark self-test failed: " << message << '\n';
        }
        return condition;
    };
    bool passed = true;

    NoOpToolbarCommands commands;
    ScreenshotToolbarWindow toolbar(commands);
    ScreenshotToolPalette* palette = toolbar.palette();
    QWidget* retainedMainPanel = palette != nullptr ? palette->mainPanel() : nullptr;
    passed &= require(palette != nullptr && retainedMainPanel != nullptr,
                      "lazy toolbar retained core");
    passed &= require(palette->stylePanel() == nullptr && palette->actionPanel() == nullptr,
                      "lazy toolbar cold secondary rows");

    SnowCanvasStyleToolbarState retainedState;
    retainedState.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    retainedState.shapeStyle =
        snow_shot::presentation::screenshotCanvasStyleDefaults().rectangle;
    retainedState.shapeStyle.strokeWidth = 17.0;
    toolbar.setStyleToolbarState(retainedState);
    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    QPointer<QWidget> firstStylePanel = palette->stylePanel();
    passed &= require(firstStylePanel != nullptr && palette->styleToolbarVisible(),
                      "style row lazy materialization");
    passed &= require(qFuzzyCompare(palette->rectangleStyle().strokeWidth + 1.0, 18.0),
                      "pre-materialization state application");

    toolbar.releaseIdleResources();
    passed &= require(toolbar.palette() == palette && palette->mainPanel() == retainedMainPanel,
                      "idle release retained object core");
    passed &= require(firstStylePanel == nullptr && palette->stylePanel() == nullptr &&
                          palette->actionPanel() == nullptr,
                      "idle release secondary eviction");

    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    passed &= require(toolbar.palette() == palette && palette->mainPanel() == retainedMainPanel &&
                          palette->stylePanel() != nullptr && palette->styleToolbarVisible(),
                      "secondary rematerialization without palette rebuild");
    passed &= require(qFuzzyCompare(palette->rectangleStyle().strokeWidth + 1.0, 18.0),
                      "style state retained across eviction");
    toolbar.releaseIdleResources();
    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Select);
    passed &= require(palette->actionPanel() != nullptr && palette->actionToolbarVisible() &&
                          !palette->styleToolbarVisible(),
                      "action row lazy materialization");

    std::vector<double> values;
    for (int value = 1; value <= 100; ++value) {
        values.push_back(static_cast<double>(value));
    }
    const Distribution stats = distribution(values);
    passed &= require(qFuzzyCompare(stats.median + 1.0, 51.5), "median");
    passed &= require(std::abs(stats.p95 - 95.05) < 0.001, "p95 interpolation");
    passed &=
        require(wildcardMatches(QStringLiteral("style.*"), QStringLiteral("style.apply.shape")),
                "wildcard positive match");
    passed &=
        require(!wildcardMatches(QStringLiteral("tool.*"), QStringLiteral("style.apply.shape")),
                "wildcard negative match");

    const auto syntheticScenario = [](double totalP95, double layoutP95) {
        const QJsonObject totalDistribution{{QStringLiteral("p95"), totalP95}};
        const QJsonObject zeroDistribution{{QStringLiteral("p95"), 0.0}};
        return QJsonObject{
            {QStringLiteral("id"), QStringLiteral("synthetic")},
            {QStringLiteral("category"), QStringLiteral("self_test")},
            {QStringLiteral("description"), QStringLiteral("Synthetic scenario")},
            {QStringLiteral("metrics"),
             QJsonObject{
                 {QStringLiteral("total_ms"), totalDistribution},
                 {QStringLiteral("action_ms"), zeroDistribution},
                 {QStringLiteral("compositor_ms"), zeroDistribution},
                 {QStringLiteral("widget_dirty_area_ratio"), zeroDistribution},
                 {QStringLiteral("surface_dirty_area_ratio"), zeroDistribution},
                 {QStringLiteral("paint_events"), zeroDistribution},
                 {QStringLiteral("surface_presentations"), zeroDistribution},
             }},
            {QStringLiteral("event_totals"), QJsonObject{}},
            {QStringLiteral("structural_counters"),
             QJsonObject{{QStringLiteral("layout.commit"),
                          QJsonObject{{QStringLiteral("p95"), layoutP95}}}}},
            {QStringLiteral("structural_counters_per_operation"),
             QJsonObject{{QStringLiteral("layout.commit"),
                          QJsonObject{{QStringLiteral("p95"), layoutP95}}}}},
            {QStringLiteral("samples"), QJsonArray{}},
        };
    };
    QTemporaryDir temporaryDirectory;
    passed &= require(temporaryDirectory.isValid(), "temporary directory");
    const QString baselinePath = temporaryDirectory.filePath(QStringLiteral("baseline.json"));
    QFile baselineFile(baselinePath);
    passed &= require(baselineFile.open(QIODevice::WriteOnly), "baseline file open");
    const QJsonObject baselineReport{
        {QStringLiteral("schema_version"), kReportSchemaVersion},
        {QStringLiteral("scenario_manifest_version"), kScenarioManifestVersion},
        {QStringLiteral("environment"),
         QJsonObject{{QStringLiteral("fingerprint"), QStringLiteral("self-test")}}},
        {QStringLiteral("scenarios"), QJsonArray{syntheticScenario(10.0, 0.0)}},
    };
    baselineFile.write(QJsonDocument(baselineReport).toJson(QJsonDocument::Compact));
    baselineFile.close();
    QJsonArray current{syntheticScenario(12.0, 0.0)};
    QJsonObject comparison = compareWithBaseline(
        &current, QJsonObject{{QStringLiteral("fingerprint"), QStringLiteral("self-test")}},
        baselinePath, false);
    passed &=
        require(comparison.value(QStringLiteral("compatible")).toBool(), "compatible baseline");
    passed &= require(comparison.value(QStringLiteral("regression_count")).toInt() == 1,
                      "dual-threshold timing regression");
    passed &= require(current.first()
                          .toObject()
                          .value(QStringLiteral("baseline"))
                          .toObject()
                          .value(QStringLiteral("timing_regression"))
                          .toBool(),
                      "scenario timing result");
    QJsonArray incompatible{syntheticScenario(12.0, 0.0)};
    comparison = compareWithBaseline(
        &incompatible,
        QJsonObject{{QStringLiteral("fingerprint"), QStringLiteral("other-machine")}}, baselinePath,
        false);
    passed &= require(comparison.value(QStringLiteral("status")).toString() ==
                          QStringLiteral("incompatible"),
                      "incompatible baseline protection");

    QJsonObject htmlReport{
        {QStringLiteral("environment"),
         QJsonObject{{QStringLiteral("timestamp_utc"), QStringLiteral("self-test")},
                     {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
                     {QStringLiteral("cpu"), QStringLiteral("synthetic")}}},
        {QStringLiteral("comparison"),
         QJsonObject{{QStringLiteral("status"), QStringLiteral("self-test")},
                     {QStringLiteral("regression_count"), 0}}},
        {QStringLiteral("scenarios"), QJsonArray{syntheticScenario(10.0, 0.0)}},
        {QStringLiteral("recommendations"), QJsonArray{}},
    };
    const QString htmlPath = temporaryDirectory.filePath(QStringLiteral("report.html"));
    QString htmlError;
    passed &= require(writeHtmlReport(htmlPath, htmlReport, &htmlError), "HTML writer");
    QFile htmlFile(htmlPath);
    passed &= require(htmlFile.open(QIODevice::ReadOnly), "HTML file open");
    const QByteArray html = htmlFile.readAll();
    passed &= require(html.contains("Measured Scenarios") && html.contains("application/json") &&
                          html.contains("synthetic"),
                      "HTML report content");
    if (passed) {
        std::cout << "toolbar benchmark self-tests passed\n";
    }
    return passed;
}

QJsonObject coverageJson(const QVector<Scenario>& scenarios) {
    QJsonArray tools;
    for (const ToolCase& tool : kTools)
        tools.push_back(QString::fromLatin1(tool.id));
    QJsonArray sources;
    for (const SourceCase& source : kSources)
        sources.push_back(QString::fromLatin1(source.id));
    QJsonArray scenarioIds;
    for (const Scenario& scenario : scenarios)
        scenarioIds.push_back(scenario.id);
    const QJsonArray entryPoints{
        QStringLiteral("resetForNewCapture"),  QStringLiteral("setScrollingScreenshotMode"),
        QStringLiteral("setActiveTool"),       QStringLiteral("setStyleToolbarState"),
        QStringLiteral("setWatermarkConfig"),
        QStringLiteral("setPlacementContext"), QStringLiteral("setPlacementScreen"),
        QStringLiteral("setMovementBounds"),   QStringLiteral("setStyleToolbarAboveMain"),
        QStringLiteral("prepareForDisplay"),   QStringLiteral("moveContentTo"),
        QStringLiteral("show/hide"),           QStringLiteral("native hover/press/drag/wheel"),
    };
    return {
        {QStringLiteral("toolbar_scope"), QStringLiteral("ScreenshotToolbarWindow")},
        {QStringLiteral("tool_enum_count"), static_cast<int>(std::size(kTools))},
        {QStringLiteral("style_source_count"), static_cast<int>(std::size(kSources))},
        {QStringLiteral("scenario_count"), static_cast<int>(scenarios.size())},
        {QStringLiteral("tools"), tools},
        {QStringLiteral("style_sources"), sources},
        {QStringLiteral("entry_points"), entryPoints},
        {QStringLiteral("scenario_ids"), scenarioIds},
    };
}

QJsonObject instrumentationCalibration(BenchmarkApplication& application, Collector& collector,
                                       Fixture& fixture) {
    constexpr int iterations = 10'000;
    Sample scopeSample;
    collector.setSample(&scopeSample);
    toolbar_perf::setSink(&collector);
    QElapsedTimer scopeTimer;
    scopeTimer.start();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        SNOW_SHOT_TOOLBAR_PERF_SCOPE("calibration.scope");
    }
    const qint64 scopeNanoseconds = scopeTimer.nsecsElapsed();
    collector.setSample(nullptr);

    QWidget* receiver = fixture.toolbar->palette();
    QElapsedTimer inactiveTimer;
    inactiveTimer.start();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        QEvent event(QEvent::User);
        QCoreApplication::sendEvent(receiver, &event);
    }
    const qint64 inactiveNanoseconds = inactiveTimer.nsecsElapsed();
    Sample notifySample;
    application.setSample(&notifySample);
    QElapsedTimer activeTimer;
    activeTimer.start();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        QEvent event(QEvent::User);
        QCoreApplication::sendEvent(receiver, &event);
    }
    const qint64 activeNanoseconds = activeTimer.nsecsElapsed();
    application.setSample(nullptr);
    toolbar_perf::setSink(nullptr);
    return {
        {QStringLiteral("iterations"), iterations},
        {QStringLiteral("scope_recording_ns_per_call"),
         static_cast<double>(scopeNanoseconds) / iterations},
        {QStringLiteral("notify_observation_ns_per_event"),
         static_cast<double>(std::max<qint64>(0, activeNanoseconds - inactiveNanoseconds)) /
             iterations},
    };
}

class CursorRestore final {
  public:
    CursorRestore() : m_position(QCursor::pos()) {}
    ~CursorRestore() {
        QCursor::setPos(m_position);
    }

  private:
    QPoint m_position;
};

int runMain(BenchmarkApplication& application) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Interactive native Windows screenshot-toolbar rendering benchmark"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("output"), QStringLiteral("Artifact directory"),
                      QStringLiteral("directory"), QStringLiteral("build/toolbar-perf/latest")});
    parser.addOption({QStringLiteral("baseline"), QStringLiteral("Baseline report.json"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("warmups"), QStringLiteral("Warmups per discrete scenario"),
                      QStringLiteral("count"), QStringLiteral("8")});
    parser.addOption({QStringLiteral("samples"), QStringLiteral("Samples per discrete scenario"),
                      QStringLiteral("count"), QStringLiteral("40")});
    parser.addOption({QStringLiteral("scenario"), QStringLiteral("Scenario wildcard"),
                      QStringLiteral("glob"), QStringLiteral("*")});
    parser.addOption({QStringLiteral("screen-index"), QStringLiteral("Initial QScreen index"),
                      QStringLiteral("index"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("force-compare"),
                      QStringLiteral("Compare mismatched environment fingerprints")});
    parser.addOption(
        {QStringLiteral("no-gate"), QStringLiteral("Never fail for baseline regressions")});
    parser.addOption(
        {QStringLiteral("list-scenarios"), QStringLiteral("Print the scenario manifest and exit")});
    parser.addOption(
        {QStringLiteral("self-test"), QStringLiteral("Run report/statistics self-tests and exit")});
    parser.process(application);

    if (parser.isSet(QStringLiteral("self-test"))) {
        return runSelfTests() ? 0 : 1;
    }

    QVector<Scenario> scenarios = createScenarios();
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (qsizetype index = 0; index < screens.size(); ++index) {
        QPointer<QScreen> screen = screens.at(index);
        scenarios.push_back(
            {QStringLiteral("placement.screen_%1").arg(index),
             QStringLiteral("monitor"),
             QStringLiteral("Apply placement and physical scaling for screen %1").arg(index),
             {},
             [screen](Fixture& f) {
                 if (screen == nullptr)
                     return;
                 const QRect bounds = screen->availableGeometry();
                 f.toolbar->setPlacementContext(screen, bounds, nativeScreenPhysicalBounds(screen));
                 f.toolbar->moveContentTo(bounds.center());
                 f.toolbar->prepareForDisplay();
             }});
    }
    QScreen* firstDprScreen = nullptr;
    QScreen* secondDprScreen = nullptr;
    for (QScreen* lhs : screens) {
        for (QScreen* rhs : screens) {
            if (!qFuzzyCompare(lhs->devicePixelRatio() + 1.0, rhs->devicePixelRatio() + 1.0)) {
                firstDprScreen = lhs;
                secondDprScreen = rhs;
                break;
            }
        }
        if (firstDprScreen != nullptr)
            break;
    }
    if (firstDprScreen != nullptr && secondDprScreen != nullptr) {
        QPointer<QScreen> first = firstDprScreen;
        QPointer<QScreen> second = secondDprScreen;
        scenarios.push_back(
            {QStringLiteral("placement.cross_dpi_transition"), QStringLiteral("monitor"),
             QStringLiteral("Move the native toolbar between screens with distinct DPR values"),
             [first](Fixture& f) {
                 if (first == nullptr)
                     return;
                 const QRect bounds = first->availableGeometry();
                 f.toolbar->setPlacementContext(first, bounds, nativeScreenPhysicalBounds(first));
                 f.toolbar->moveContentTo(bounds.center());
                 f.toolbar->prepareForDisplay();
             },
             [second](Fixture& f) {
                 if (second == nullptr)
                     return;
                 const QRect bounds = second->availableGeometry();
                 f.toolbar->setPlacementContext(second, bounds, nativeScreenPhysicalBounds(second));
                 f.toolbar->moveContentTo(bounds.center());
                 f.toolbar->prepareForDisplay();
             }});
    }
    QScreen* firstSameDprScreen = nullptr;
    QScreen* secondSameDprScreen = nullptr;
    for (QScreen* lhs : screens) {
        for (QScreen* rhs : screens) {
            if (crossMonitorDragKeepsDprConstant(lhs, rhs)) {
                firstSameDprScreen = lhs;
                secondSameDprScreen = rhs;
                break;
            }
        }
        if (firstSameDprScreen != nullptr)
            break;
    }
    if (firstSameDprScreen != nullptr && secondSameDprScreen != nullptr) {
        QPointer<QScreen> first = firstSameDprScreen;
        QPointer<QScreen> second = secondSameDprScreen;
        scenarios.push_back(
            {QStringLiteral("movement.cross_monitor_same_dpi_drag"), QStringLiteral("movement"),
             QStringLiteral(
                 "Smoothly drag the native toolbar between screens while keeping DPR constant"),
             [first](Fixture& f) { prepareToolbarOnScreen(f, first); },
             [first, second](Fixture& f) {
                 if (first == nullptr || second == nullptr ||
                     !qFuzzyCompare(first->devicePixelRatio() + 1.0,
                                    second->devicePixelRatio() + 1.0)) {
                     return;
                 }
                 smoothlyDragToolbarToScreen(f, second);
             },
             5, kCrossMonitorDragSteps});
    }
    if (perMonitorDpiAware() && firstDprScreen != nullptr && secondDprScreen != nullptr) {
        QPointer<QScreen> first = firstDprScreen;
        QPointer<QScreen> second = secondDprScreen;
        scenarios.push_back(
            {QStringLiteral("movement.cross_monitor_physical_size_drag"),
             QStringLiteral("movement"),
             QStringLiteral("Smoothly drag the production toolbar across real per-monitor DPI "
                            "boundaries while preserving its prepared HWND pixel size"),
             [first](Fixture& f) { prepareToolbarOnScreen(f, first); },
             [second](Fixture& f) { smoothlyDragToolbarToScreen(f, second); }, 0,
             kCrossMonitorDragSteps, true});
    }
    if (parser.isSet(QStringLiteral("list-scenarios"))) {
        std::cout
            << QJsonDocument(coverageJson(scenarios)).toJson(QJsonDocument::Indented).constData();
        return 0;
    }

    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        std::cerr << "The benchmark requires QT_QPA_PLATFORM=windows.\n";
        return 3;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    BOOL compositionEnabled = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&compositionEnabled)) || !compositionEnabled) {
        std::cerr << "Desktop Window Manager composition is required.\n";
        return 3;
    }
#endif
    bool warmupsValid = false;
    bool samplesValid = false;
    const int warmups = parser.value(QStringLiteral("warmups")).toInt(&warmupsValid);
    const int sampleCount = parser.value(QStringLiteral("samples")).toInt(&samplesValid);
    if (!warmupsValid || !samplesValid || warmups < 0 || sampleCount < 1) {
        std::cerr << "Warmups must be nonnegative and samples must be positive.\n";
        return 1;
    }
    bool screenValid = false;
    const int screenIndex = parser.value(QStringLiteral("screen-index")).toInt(&screenValid);
    if (!screenValid || screenIndex < 0 || screenIndex >= screens.size()) {
        std::cerr << "The selected screen index is unavailable.\n";
        return 3;
    }
    const QString outputDirectory = QDir::cleanPath(parser.value(QStringLiteral("output")));
    if (!QDir().mkpath(outputDirectory)) {
        std::cerr << "Could not create the output directory.\n";
        return 1;
    }
    QFile rawFile(QDir(outputDirectory).filePath(QStringLiteral("raw.jsonl")));
    if (!rawFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::cerr << "Could not create raw.jsonl.\n";
        return 1;
    }

    CursorRestore restoreCursor;
    Collector collector;
    toolbar_perf::setSink(&collector);
    const NativeGeometryWarningScope geometryWarningScope;
    QJsonArray scenarioReports;
    Fixture fixture(application, screens.at(screenIndex));
    const QString scenarioPattern = parser.value(QStringLiteral("scenario"));
    int executedCount = 0;
    for (const Scenario& scenario : std::as_const(scenarios)) {
        if (!wildcardMatches(scenarioPattern, scenario.id)) {
            continue;
        }
        ++executedCount;
        const int measuredSamples =
            scenario.sampleOverride > 0 ? scenario.sampleOverride : sampleCount;
        const int scenarioWarmups = scenario.sampleOverride > 0 ? 0 : warmups;
        std::cout << "[toolbar-perf] " << scenario.id.toStdString() << " (" << measuredSamples
                  << " samples)\n";
        QVector<Sample> measured;
        measured.reserve(measuredSamples);
        int warmupsRemaining = scenarioWarmups;
        int attemptCount = 0;
        const int maximumAttempts = scenarioWarmups + measuredSamples * 5;
        while ((warmupsRemaining > 0 || measured.size() < measuredSamples) &&
               attemptCount < maximumAttempts) {
            ++attemptCount;
            application.setSample(nullptr);
            collector.setSample(nullptr);
            parkCursorOutsideToolbar(fixture.screen);
            settleEvents(application);
            fixture.reset();
            if (scenario.prepare)
                scenario.prepare(fixture);
            settleEvents(application);
            flushCompositor();

            Sample sample;
            const quint64 busyFramesBefore = busyIndicatorFrameCount(*fixture.toolbar);
            application.setSample(&sample);
            collector.setSample(&sample);
            QElapsedTimer totalTimer;
            totalTimer.start();
            QElapsedTimer actionTimer;
            actionTimer.start();
            nativeGeometryWarningEmitted.store(false, std::memory_order_relaxed);
            scenario.action(fixture);
            sample.actionMilliseconds =
                static_cast<double>(actionTimer.nsecsElapsed()) / 1'000'000.0;
            QElapsedTimer settleTimer;
            settleTimer.start();
            settleEvents(application);
            sample.settleMilliseconds =
                static_cast<double>(settleTimer.nsecsElapsed()) / 1'000'000.0;
            QElapsedTimer compositorTimer;
            compositorTimer.start();
            flushCompositor();
            sample.compositorMilliseconds =
                static_cast<double>(compositorTimer.nsecsElapsed()) / 1'000'000.0;
            sample.totalMilliseconds = static_cast<double>(totalTimer.nsecsElapsed()) / 1'000'000.0;
            const quint64 busyFramesAfter = busyIndicatorFrameCount(*fixture.toolbar);
            sample.counters[QStringLiteral("spinner.isolated_frames")] +=
                static_cast<qint64>(busyFramesAfter - busyFramesBefore);
            application.setSample(nullptr);
            collector.setSample(nullptr);
            const bool geometryWarning =
                nativeGeometryWarningEmitted.load(std::memory_order_relaxed);
            sample.counters[QStringLiteral("window.native_geometry_warning")] +=
                geometryWarning ? 1 : 0;
            const bool sampleValid =
                !scenario.requiresValidDragSample ||
                (sample.counters.value(QStringLiteral("drag.sample_valid")) == 1 &&
                 !geometryWarning);
            if (!sampleValid) {
                std::cerr
                    << "[toolbar-perf] rejected drag attempt: handle="
                    << sample.counters.value(QStringLiteral("drag.handle_received_press"))
                    << " active="
                    << sample.counters.value(QStringLiteral("drag.physical_drag_became_active"))
                    << " destination="
                    << sample.counters.value(QStringLiteral("drag.hwnd_reached_destination"))
                    << " dpr=" << sample.counters.value(QStringLiteral("drag.window_dpr_changed"))
                    << " commit="
                    << sample.counters.value(QStringLiteral("drag.logical_transition_commits"))
                    << " post_commit_paints="
                    << sample.counters.value(QStringLiteral("drag.post_commit_top_level_paints"))
                    << " layouts=" << sample.counters.value(QStringLiteral("drag.layout_requests"))
                    << " max_resizes="
                    << sample.counters.value(
                           QStringLiteral("drag.maximum_participant_resize_count"))
                    << " geometry_warning=" << geometryWarning << '\n';
            }
            if (warmupsRemaining > 0) {
                if (sampleValid) {
                    --warmupsRemaining;
                }
            } else if (sampleValid) {
                measured.push_back(std::move(sample));
            }
        }
        if (measured.size() != measuredSamples) {
            std::cerr << "[toolbar-perf] rejected too many invalid samples for "
                      << scenario.id.toStdString() << ": accepted " << measured.size() << " of "
                      << measuredSamples << '\n';
            toolbar_perf::setSink(nullptr);
            return 4;
        }
        QJsonObject summary = summarizeScenario(scenario, measured);
        scenarioReports.push_back(summary);
        QJsonObject rawRecord{
            {QStringLiteral("schema_version"), kReportSchemaVersion},
            {QStringLiteral("scenario"), summary},
        };
        rawFile.write(QJsonDocument(rawRecord).toJson(QJsonDocument::Compact));
        rawFile.write("\n");
        rawFile.flush();
    }
    toolbar_perf::setSink(nullptr);
    if (executedCount == 0) {
        std::cerr << "The scenario filter did not match any scenarios.\n";
        return 1;
    }

    const QJsonObject calibration = instrumentationCalibration(application, collector, fixture);
    const QJsonObject environment = environmentMetadata();
    const bool forceCompare = parser.isSet(QStringLiteral("force-compare"));
    const QJsonObject comparison = compareWithBaseline(
        &scenarioReports, environment, parser.value(QStringLiteral("baseline")), forceCompare);
    QJsonArray skipped;
    bool distinctDpr = false;
    bool sameDprScreenPair = false;
    for (QScreen* lhs : screens)
        for (QScreen* rhs : screens) {
            distinctDpr = distinctDpr || !qFuzzyCompare(lhs->devicePixelRatio() + 1.0,
                                                        rhs->devicePixelRatio() + 1.0);
            sameDprScreenPair = sameDprScreenPair || crossMonitorDragKeepsDprConstant(lhs, rhs);
        }
    if (!distinctDpr) {
        skipped.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("placement.cross_dpi_transition")},
            {QStringLiteral("reason"),
             QStringLiteral("No screens with distinct DPR values are available")},
        });
    }
    if (!sameDprScreenPair) {
        const QString reason =
            !perMonitorDpiAware()
                ? QStringLiteral("The process is not per-monitor DPI aware, so Qt DPR values do "
                                 "not prove equal native monitor scaling")
                : QStringLiteral("No two distinct screens with matching DPR values and a "
                                 "constant-DPR drag route are available");
        skipped.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("movement.cross_monitor_same_dpi_drag")},
            {QStringLiteral("reason"), reason},
        });
    }
    if (!perMonitorDpiAware() || screens.size() < 2) {
        skipped.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("movement.cross_monitor_physical_size_drag")},
            {QStringLiteral("reason"),
             !perMonitorDpiAware() ? QStringLiteral("The process is not per-monitor DPI aware")
                                   : QStringLiteral("Fewer than two active screens are available")},
        });
    }
    QJsonObject report{
        {QStringLiteral("schema_version"), kReportSchemaVersion},
        {QStringLiteral("scenario_manifest_version"), kScenarioManifestVersion},
        {QStringLiteral("environment"), environment},
        {QStringLiteral("configuration"),
         QJsonObject{
             {QStringLiteral("warmups"), warmups},
             {QStringLiteral("samples"), sampleCount},
             {QStringLiteral("scenario_filter"), scenarioPattern},
             {QStringLiteral("screen_index"), screenIndex},
             {QStringLiteral("interactive"), true},
             {QStringLiteral("native_windows"), true},
             {QStringLiteral("dwm_flush_is_presentation_proxy"), true},
             {QStringLiteral("instrumentation_calibration"), calibration},
         }},
        {QStringLiteral("coverage"), coverageJson(scenarios)},
        {QStringLiteral("skipped"), skipped},

        {QStringLiteral("comparison"), comparison},
        {QStringLiteral("scenarios"), scenarioReports},
    };
    report.insert(QStringLiteral("recommendations"), recommendationsFor(scenarioReports));
    const QString jsonPath = QDir(outputDirectory).filePath(QStringLiteral("report.json"));
    QFile reportFile(jsonPath);

    if (!reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::cerr << "Could not create report.json.\n";
        return 1;
    }
    reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    reportFile.close();
    QString htmlError;
    const QString htmlPath = QDir(outputDirectory).filePath(QStringLiteral("report.html"));
    if (!writeHtmlReport(htmlPath, report, &htmlError)) {
        std::cerr << "Could not create report.html: " << htmlError.toStdString() << '\n';
        return 1;
    }
    std::cout << "[toolbar-perf] JSON: " << jsonPath.toStdString() << '\n'
              << "[toolbar-perf] HTML: " << htmlPath.toStdString() << '\n';
    const int regressions = comparison.value(QStringLiteral("regression_count")).toInt();
    return regressions > 0 && !parser.isSet(QStringLiteral("no-gate")) ? 2 : 0;
}
} // namespace

int main(int argc, char** argv) {
    BenchmarkApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("snow-shot-toolbar-perf"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1"));
    return runMain(application);
}
