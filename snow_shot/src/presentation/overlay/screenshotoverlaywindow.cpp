#include "snow_shot/presentation/screenshotoverlaywindow.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"
#include "screenshotoverlayframepresenter.h"
#include "snow_shot/presentation/screenshotmessageservice.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QEvent>
#include "snow_shot/platform/windows/windowchrome.h"
#ifdef Q_OS_MACOS
#include "snow_shot/platform/screenshotnative.h"
#endif
#include <QGuiApplication>
#include "../capture/screenshotscrollingdiagnostics.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRegion>
#include "snow_shot/presentation/screenshotregiontypecontrol.h"
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <optional>
#include <utility>

namespace {
constexpr int kScrollingThumbnailGap = 8;
constexpr int kScrollingThumbnailMargin = 8;
constexpr auto kHistoryLoadingMessageKey = "screenshot-history-loading";

#if defined(SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION)
qint64 paintRegionArea(const QRegion& region) {
    qint64 area = 0;
    for (const QRect& rect : region) {
        area += static_cast<qint64>(rect.width()) * static_cast<qint64>(rect.height());
    }
    return area;
}
#endif
} // namespace

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

ScreenshotOverlayWindow::ScreenshotOverlayWindow(ScreenshotOverlayEventSink& eventSink,
                                                 SnowCanvasWidget* canvas, QWidget* parent)
    : QWidget(parent), m_eventSink(eventSink), m_canvas(canvas) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_canvas);

    m_regionTypeControl = new ScreenshotRegionTypeControl(this, true);
    m_regionTypeControl->hide();
    m_framePresenter = std::make_unique<ScreenshotOverlayFramePresenter>(*this);

    if (m_canvas != nullptr) {
        m_screenshotRenderer = std::make_unique<ScreenshotCanvasRenderer>(*m_canvas);
        m_canvas->setCustomRenderer(m_screenshotRenderer.get());
        m_canvas->setWatermarkRenderArea(QRectF());
        m_canvas->setSpotlightRenderArea(QRectF());
        m_canvas->setWheelZoomEnabled(false);
        m_canvas->setInteractionEnabled(false);
        m_canvas->installEventFilter(this);
        m_canvas->setFocusPolicy(Qt::StrongFocus);
        m_canvas->setMouseTracking(true);
        connect(m_canvas, &SnowCanvasWidget::unhandledLeftDoubleClick, this,
                [this]() { m_eventSink.handleUnhandledLeftDoubleClick(); });
        connect(m_canvas, &SnowCanvasWidget::unhandledMiddleClick, this,
                [this]() { m_eventSink.handleUnhandledMiddleClick(); });
    }

    initializeScreenshotSurface();
    setCanvasClearBackgroundEnabled(false);
}

ScreenshotOverlayWindow::~ScreenshotOverlayWindow() {
    setScrollingCaptureMode(false);
    if (m_canvas != nullptr) {
        m_canvas->clearWatermarkRenderArea();
        m_canvas->clearSpotlightRenderArea();
    }
    if (m_canvas != nullptr && m_canvas->customRenderer() == m_screenshotRenderer.get()) {
        m_canvas->setCustomRenderer(nullptr);
    }
}

SnowCanvasWidget* ScreenshotOverlayWindow::canvas() const {
    return m_canvas;
}

void ScreenshotOverlayWindow::setCaptureGeometry(const QRect& displayGeometry) {
#ifdef Q_OS_MACOS
    // Cocoa's upward Y axis excludes NSMaxY(frame) from WindowServer hit testing.
    // Put the display's top row inside the native frame, while the canvas still
    // covers exactly the captured display. This is a logical point, not a pixel.
    m_captureFrameMargins = QMargins(0, 1, 0, 0);
#endif
    layout()->setContentsMargins(m_captureFrameMargins);
    const QRect frame = displayGeometry.marginsAdded(m_captureFrameMargins);
    if (geometry() != frame)
        setGeometry(frame);
    layout()->activate();
}

QRect ScreenshotOverlayWindow::captureGeometry() const {
    return geometry().marginsRemoved(m_captureFrameMargins);
}

QPoint ScreenshotOverlayWindow::canvasLocalPosition(const QPoint& globalPosition) const {
    return globalPosition - captureGeometry().topLeft();
}

void ScreenshotOverlayWindow::setScreenshotImage(QImage image, const QRectF& canvasRect) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setImage(std::move(image), canvasRect);
    }
}

void ScreenshotOverlayWindow::setScreenshotImageSource(ScreenshotImageSource source) {
    if (m_screenshotRenderer)
        m_screenshotRenderer->setImageSource(std::move(source));
}

void ScreenshotOverlayWindow::setScreenshotMaskVisible(bool visible) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setMaskVisible(visible);
    }
}

void ScreenshotOverlayWindow::setScreenshotSelectionBorderColor(const QColor& color) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setSelectionBorderColor(color);
    }
}

void ScreenshotOverlayWindow::setScreenshotMaskColor(const QColor& color) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setMaskColor(color);
    }
}

void ScreenshotOverlayWindow::setScreenshotGuideLines(const QPointF& cursorPosition,
                                                      const QColor& cursorColor,
                                                      const QColor& monitorCenterColor) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setGuideLines(cursorPosition, cursorColor, monitorCenterColor);
    }
}

void ScreenshotOverlayWindow::clearScreenshotGuideLines() {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->clearGuideLines();
    }
}

void ScreenshotOverlayWindow::setScreenshotSelection(const QRectF& selection, bool handlesVisible,
                                                     int cornerRadius, int shadowWidth,
                                                     const QColor& shadowColor,
                                                     bool selectionToolbarHovered) {
    const QRectF normalizedSelection = selection.normalized();
    ScreenshotSelectionVisualState state;
    state.bounds = normalizedSelection;
    state.present = normalizedSelection.isValid() && !normalizedSelection.isEmpty();
    state.handlesVisible = handlesVisible;
    state.cornerRadius = cornerRadius;
    state.shadowWidth = shadowWidth;
    state.shadowColor = shadowColor;
    state.toolbarHovered = selectionToolbarHovered;
    setScreenshotSelectionState(state);
}

void ScreenshotOverlayWindow::setScreenshotSelectionState(
    const ScreenshotSelectionVisualState& requestedState) {
    if (m_canvas != nullptr) {
        const QRectF normalizedSelection = requestedState.bounds.normalized();
        const QRectF configuredArea = requestedState.present && normalizedSelection.isValid() &&
                                              !normalizedSelection.isEmpty()
                                          ? normalizedSelection
                                          : QRectF();
        m_canvas->setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
            std::optional<QRectF>(configuredArea),
            std::optional<QRectF>(configuredArea),
        });
    }
    if (m_screenshotRenderer != nullptr) {
        ScreenshotSelectionVisualState state = requestedState;
        state.borderVisible = m_screenshotRenderer->selectionBorderVisible();
        m_screenshotRenderer->applySelectionState(state);
    }
}

void ScreenshotOverlayWindow::setScreenshotSelectionRegion(
    const ScreenshotRegionGeometry& region, const ScreenshotRegionGeometry& confirmed,
    const QRectF& marquee, bool subtracting, const QColor& danger) {
    if (m_screenshotRenderer)
        m_screenshotRenderer->setSelectionRegion(region, confirmed, marquee, subtracting, danger);
}

void ScreenshotOverlayWindow::clearScreenshotSelection() {
    if (m_canvas != nullptr) {
        m_canvas->setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
            std::optional<QRectF>(QRectF()),
            std::optional<QRectF>(QRectF()),
        });
    }
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->clearSelection();
    }
}

bool ScreenshotOverlayWindow::hasScreenshotSelection() const {
    return m_screenshotRenderer != nullptr && m_screenshotRenderer->hasSelection();
}

bool ScreenshotOverlayWindow::screenshotSelectionHandlesVisible() const {
    return m_screenshotRenderer != nullptr && m_screenshotRenderer->selectionHandlesVisible();
}

void ScreenshotOverlayWindow::setScreenshotSelectionBorderVisible(bool visible) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setSelectionBorderVisible(visible);
    }
}

bool ScreenshotOverlayWindow::screenshotSelectionBorderVisible() const {
    return m_screenshotRenderer == nullptr || m_screenshotRenderer->selectionBorderVisible();
}

void ScreenshotOverlayWindow::setScreenshotOcrBackground(
    std::shared_ptr<ScreenshotOcrPresentation> presentation) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setOcrPresentation(
            std::move(presentation), ScreenshotCanvasRenderer::OcrPresentationMode::BackgroundOnly);
    }
}

void ScreenshotOverlayWindow::setScreenshotOcrFilteredImage(QImage image,
                                                            const QRectF& canvasRect) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setOcrFilteredImage(std::move(image), canvasRect);
    }
}

void ScreenshotOverlayWindow::setScreenshotOcrVisible(bool visible) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setOcrVisible(visible);
    }
}

void ScreenshotOverlayWindow::clearScreenshotOcrBackground() {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->clearOcrPresentation();
    }
}

void ScreenshotOverlayWindow::setHistoryLoadingVisible(bool visible) {
    const QString key = QString::fromLatin1(kHistoryLoadingMessageKey);
    if (!visible) {
        ScreenshotMessageService::destroyFor(this, key);
        return;
    }
    ScreenshotMessageService::loadingFor(this, key, tr("Loading screenshot history"));
}

void ScreenshotOverlayWindow::resetScreenshotRendering() {
    setScrollingCaptureMode(false);
    if (m_canvas != nullptr) {
        m_canvas->setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
            std::optional<QRectF>(QRectF()),
            std::optional<QRectF>(QRectF()),
        });
    }
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->reset();
    }
}

void ScreenshotOverlayWindow::commitInitialSelectionCursor() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HCURSOR crossCursor = LoadCursorW(nullptr, IDC_CROSS);
    if (crossCursor != nullptr) {
        SetCursor(crossCursor);
    }
#endif
}

void ScreenshotOverlayWindow::setCanvasClearBackgroundEnabled(bool enabled) {
    if (m_canvas == nullptr) {
        return;
    }

    m_canvas->setClearBackgroundEnabled(enabled);
}

QJsonObject ScreenshotOverlayWindow::scrollingDiagnostics() const {
    using snow_shot::capture_detail::scrollingRect;
    const QRect hole =
        m_scrollingVisualHole.translated(m_captureFrameMargins.left(), m_captureFrameMargins.top())
            .intersected(rect());
    QJsonObject fields{{QStringLiteral("overlay_rect"), scrollingRect(geometry())},
                       {QStringLiteral("hole_rect"), scrollingRect(hole)},
                       {QStringLiteral("full_hole"),
                        !hole.isEmpty() && hole == rect().marginsRemoved(m_captureFrameMargins)},
                       {QStringLiteral("mask_empty"), mask().isEmpty()},
                       {QStringLiteral("dpr"), devicePixelRatioF()},
                       {QStringLiteral("thumbnail_visible"),
                        m_scrollingThumbnail != nullptr && m_scrollingThumbnail->isVisible()}};
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() == QStringLiteral("windows") && internalWinId() != 0) {
        const auto hwnd = reinterpret_cast<HWND>(internalWinId());
        const HRGN region = CreateRectRgn(0, 0, 0, 0);
        if (region != nullptr) {
            const int type = GetWindowRgn(hwnd, region);
            fields.insert(QStringLiteral("native_region_type"), type);
            if (type != ERROR && !hole.isEmpty()) {
                const QPoint center = hole.center();
                fields.insert(QStringLiteral("native_hole_contains_center"),
                              PtInRegion(region, qRound(center.x() * devicePixelRatioF()),
                                         qRound(center.y() * devicePixelRatioF())) != FALSE);
            }
            DeleteObject(region);
        }
        DWORD affinity = 0;
        if (GetWindowDisplayAffinity(hwnd, &affinity))
            fields.insert(QStringLiteral("code"), static_cast<qint64>(affinity));
        DWORD captureProcess = 0;
        GetWindowThreadProcessId(GetCapture(), &captureProcess);
        fields.insert(QStringLiteral("capture_is_self"), captureProcess == GetCurrentProcessId());
        UINT routing = 0;
        if (SystemParametersInfoW(SPI_GETMOUSEWHEELROUTING, 0, &routing, 0))
            fields.insert(QStringLiteral("inactive_scroll_enabled"),
                          routing == MOUSEWHEEL_ROUTING_MOUSE_POS);
    }
#endif
    return fields;
}

void ScreenshotOverlayWindow::setScrollingVisualHole(const QRect& localRect) {
    m_scrollingVisualHole = localRect;
    updateWindowMask();
}

void ScreenshotOverlayWindow::clearScrollingVisualHole() {
    m_scrollingVisualHole = {};
    updateWindowMask();
}

void ScreenshotOverlayWindow::setScrollingCaptureMode(bool enabled) {
    if (!enabled) {
        // Standard rendering draws into the scrolling visual hole, so
        // restore the full window surface before its synchronous repaint.
        clearScrollingVisualHole();
        clearScrollingThumbnail();
    }

    if (m_scrollingCaptureMode == enabled) {
        return;
    }

    if (enabled) {
        if (m_canvas != nullptr) {
            m_canvasContentWasVisible = m_canvas->canvasContentVisible();
            m_canvasClearBackgroundWasEnabled = m_canvas->clearBackgroundEnabled();
            m_canvasInteractionWasEnabled = m_canvas->interactionEnabled();

            m_canvas->setInteractionEnabled(false);
            m_canvas->setClearBackgroundEnabled(false);
            m_canvas->setCanvasContentVisible(false);
        }
        if (m_screenshotRenderer != nullptr) {
            m_screenshotRenderer->setRenderMode(
                ScreenshotCanvasRenderer::RenderMode::ScrollingCapture);
        }
    } else {
        if (m_screenshotRenderer != nullptr) {
            m_screenshotRenderer->setRenderMode(ScreenshotCanvasRenderer::RenderMode::Standard);
        }
        if (m_canvas != nullptr) {
            m_canvas->setCanvasContentVisible(m_canvasContentWasVisible);
            m_canvas->setClearBackgroundEnabled(m_canvasClearBackgroundWasEnabled);
            m_canvas->setInteractionEnabled(m_canvasInteractionWasEnabled);
        }
    }

    m_scrollingCaptureMode = enabled;
    updateScrollingInputTransparency();
    if (m_canvas != nullptr && m_canvas->isVisible() && m_canvas->updatesEnabled()) {
        m_canvas->repaint();
    } else if (m_canvas != nullptr) {
        m_canvas->update();
    }
}

void ScreenshotOverlayWindow::beginScrollingThumbnail(const QRect& localSelection,
                                                      ScreenshotScrollingRecognitionMode mode) {
    if (m_scrollingThumbnail == nullptr) {
        m_scrollingThumbnail = new ScreenshotScrollingThumbnailWidget(*this);
        // Configure once, before creating the native surface. Reparenting or changing
        // QWidget window flags during capture would hide/recreate visible windows.
        m_scrollingThumbnail->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                                             Qt::WindowStaysOnTopHint |
                                             Qt::WindowDoesNotAcceptFocus);
        m_scrollingThumbnail->setAttribute(Qt::WA_ShowWithoutActivating);
        m_scrollingThumbnail->setAttribute(Qt::WA_TranslucentBackground);
        m_scrollingThumbnail->hide();
    }

    m_scrollingThumbnailAnchor = localSelection.normalized();
    m_scrollingThumbnailMode = mode;
    m_scrollingThumbnail->setRecognitionMode(mode);
    m_scrollingThumbnail->reset();
    m_scrollingThumbnail->hide();
    layoutScrollingThumbnail();
}

void ScreenshotOverlayWindow::updateScrollingThumbnail(const QImage& previewImage,
                                                       const QSize& sourceSize,
                                                       ScreenshotScrollingStitchChange change,
                                                       int addedRows, bool replacePreview,
                                                       int replacedPreviewRows) {
    if (m_scrollingThumbnail == nullptr) {
        return;
    }

    m_scrollingThumbnail->setStitchedImage(previewImage, sourceSize, change, addedRows,
                                           replacePreview, replacedPreviewRows);
    layoutScrollingThumbnail();
    if (isVisible() && !m_scrollingThumbnail->isVisible()) {
#ifdef Q_OS_MACOS
        snow_shot::platform::configureScreenshotToolbarWindow(m_scrollingThumbnail);
#endif
        m_scrollingThumbnail->show();
    }
}

void ScreenshotOverlayWindow::reanchorScrollingThumbnail(const QRect& localSelection) {
    m_scrollingThumbnailAnchor = localSelection;
    layoutScrollingThumbnail();
}

void ScreenshotOverlayWindow::clearScrollingThumbnail() {
    delete std::exchange(m_scrollingThumbnail, nullptr);
    m_scrollingThumbnailAnchor = {};
    m_scrollingThumbnailMode = ScreenshotScrollingRecognitionMode::Vertical;
}

QWidget* ScreenshotOverlayWindow::scrollingThumbnailWindow() const {
    return m_scrollingThumbnail;
}

void ScreenshotOverlayWindow::updateScrollingInputTransparency() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    // Mutate only WS_EX_TRANSPARENT on the existing layered HWND. In particular,
    // do not hide/show the overlay or ask Qt to rebuild its window flags.
    static_cast<void>(
        snow_shot::platform::windows::setWindowInputTransparent(this, m_scrollingCaptureMode));
#elif defined(Q_OS_MACOS)
    snow_shot::platform::setScreenshotInputTransparent(this, m_scrollingCaptureMode);
#endif
}

ScreenshotScrollingTrimRange ScreenshotOverlayWindow::scrollingThumbnailTrim() const {
    if (m_scrollingThumbnail == nullptr) {
        return {};
    }
    return {
        m_scrollingThumbnail->trimTop(),
        m_scrollingThumbnail->trimBottom(),
    };
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
quint64 ScreenshotOverlayWindow::windowMaskApplicationCountForTesting() const {
    return m_windowMaskApplicationCount;
}

quint64 ScreenshotOverlayWindow::transparentClearCountForTesting() const {
    return m_transparentClearCount;
}

ScreenshotCanvasRenderer* ScreenshotOverlayWindow::screenshotRendererForTesting() const {
    return m_screenshotRenderer.get();
}
#endif

void ScreenshotOverlayWindow::warmPresentationSurface() {
    if (m_framePresenter != nullptr) {
        m_framePresenter->warmPresentationSurface();
    }
}

void ScreenshotOverlayWindow::showPreparedFrame(bool deferFirstPaint) {
    if (m_framePresenter != nullptr) {
        m_framePresenter->presentPreparedFrame(deferFirstPaint);
    }
}

void ScreenshotOverlayWindow::releaseNativeSurface() {
    hide();
    setUpdatesEnabled(false);
    clearMask();
    m_scrollingVisualHole = {};
    m_appliedWindowMask = QRegion();
    m_windowMaskInitialized = false;
    if (m_canvas != nullptr) {
        m_canvas->setInteractionEnabled(false);
        m_canvas->setUpdatesEnabled(false);
    }

    // A child top-level QWidget is not necessarily WA_NativeWindow, so destroy()
    // does not recursively release it. Release the preview before its owner;
    // otherwise it survives with a null transient parent when this overlay is reused.
    if (m_scrollingThumbnail != nullptr) {
        m_scrollingThumbnail->releaseNativeSurface();
    }

    // QWidget::destroy() keeps this QObject and its renderer/model alive while
    // releasing the native window, child native windows, and backing store.
    // Calling the Qt API also keeps QWidget's internal platform state coherent.
    destroy(true, true);
}

void ScreenshotOverlayWindow::restoreNativeSurface() {
    setUpdatesEnabled(true);
    if (m_canvas != nullptr) {
        m_canvas->setUpdatesEnabled(true);
    }

    initializeScreenshotSurface();
    static_cast<void>(winId());
    hide();
}

void ScreenshotOverlayWindow::initializeScreenshotSurface() {
    // Capture coordinates cover the full display, including the macOS menu bar
    // and notch band. Qt's default safe-area margins would inset the canvas and
    // break its alignment with the captured pixels and pointer coordinates.
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);

    // Keep the native surface mode stable after winId/show. Runtime toggling of
    // WA_TranslucentBackground is unreliable for top-level layered windows on Windows.
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);

    if (m_canvas == nullptr) {
        return;
    }

    m_canvas->setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_canvas->setAttribute(Qt::WA_TranslucentBackground, true);
    m_canvas->setAttribute(Qt::WA_NoSystemBackground, true);
    m_canvas->setAttribute(Qt::WA_TransparentForMouseEvents, false);
}

bool ScreenshotOverlayWindow::event(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::Show) {
        updateScrollingInputTransparency();
    }
    if (event != nullptr && m_scrollingThumbnail != nullptr) {
        if (event->type() == QEvent::Hide) {
            m_scrollingThumbnail->hide();
        } else if (event->type() == QEvent::Move) {
            layoutScrollingThumbnail();
        } else if (event->type() == QEvent::Show) {
            layoutScrollingThumbnail();
            if (m_scrollingThumbnail->hasPreview()) {
#ifdef Q_OS_MACOS
                snow_shot::platform::configureScreenshotToolbarWindow(m_scrollingThumbnail);
#endif
                m_scrollingThumbnail->show();
            }
        }
    }
    if (event != nullptr &&
        (event->type() == QEvent::SafeAreaMarginsChange || event->type() == QEvent::Show)) {
        layoutRegionTypeControl();
    }
#ifdef Q_OS_MACOS
    if (event != nullptr && event->type() == QEvent::Show) {
        const bool handled = QWidget::event(event);
        snow_shot::platform::configureScreenshotOverlayWindow(this);
        return handled;
    }
#endif
    if (event == nullptr || event->type() != QEvent::UpdateRequest) {
        return QWidget::event(event);
    }

    SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.update_request");
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.update_request_events", 1);
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.update_request_begin");
    const bool handled = QWidget::event(event);
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.update_request_end");
    return handled;
}

bool ScreenshotOverlayWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_canvas && event != nullptr && event->type() == QEvent::Paint) {
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.canvas.paint_dispatches", 1);
#if defined(SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION)
        const auto* paintEvent = static_cast<QPaintEvent*>(event);
        const QRegion region = paintEvent->region().intersected(m_canvas->rect());
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.canvas.dispatch_rects",
                                       region.rectCount());
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.canvas.dispatch_logical_pixels",
                                       paintRegionArea(region));
#endif
    }
    if (watched == m_canvas && handleCanvasEvent(event)) {
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

void ScreenshotOverlayWindow::keyPressEvent(QKeyEvent* event) {
    if (m_eventSink.shouldBlockUnhandledOverlayKeyInput()) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool ScreenshotOverlayWindow::nativeEvent(const QByteArray& eventType, void* message,
                                          qintptr* result) {
    if (m_mouseReleaseAction.handleNativeEvent(message, result)) {
        return true;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    Q_UNUSED(eventType);
    if (message == nullptr || result == nullptr) {
        return QWidget::nativeEvent(eventType, message, result);
    }

    const auto* msg = static_cast<const MSG*>(message);
    if (msg->message == WM_PAINT) {
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.native.wm_paint", 1);
    } else if (msg->message == WM_SYNCPAINT) {
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.native.wm_syncpaint", 1);
    }
    if (msg->message == WM_NCHITTEST) {
        *result = HTCLIENT;
        return true;
    }
#else
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif

    return QWidget::nativeEvent(eventType, message, result);
}

void ScreenshotOverlayWindow::paintEvent(QPaintEvent* event) {
    SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.paint_event");
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.paint_events", 1);
    const QRegion paintRegion =
        event != nullptr ? event->region().intersected(rect()) : QRegion(rect());
#if defined(SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION)
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.paint_rects", paintRegion.rectCount());
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.paint_logical_pixels",
                                   paintRegionArea(paintRegion));
    if (paintRegion.contains(rect())) {
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.full_paint_events", 1);
    }
#endif
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.paint_begin");
    const QRect targetRect = paintRegion.boundingRect();
    const bool rendererCoversTarget =
        m_canvas != nullptr && m_screenshotRenderer != nullptr && m_canvas->isVisible() &&
        !targetRect.isEmpty() && m_canvas->geometry().contains(targetRect) &&
        m_screenshotRenderer->coversWidgetRect(targetRect.translated(-m_canvas->pos()));
    if (testAttribute(Qt::WA_TranslucentBackground) && !targetRect.isEmpty() &&
        !rendererCoversTarget) {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(targetRect, Qt::transparent);
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        ++m_transparentClearCount;
#endif
    }

    QWidget::paintEvent(event);
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.paint_end");
}

void ScreenshotOverlayWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutScrollingThumbnail();
    layoutRegionTypeControl();
    updateWindowMask();
}

void ScreenshotOverlayWindow::layoutScrollingThumbnail() {
    if (m_scrollingThumbnail == nullptr) {
        return;
    }

    const QRect bounds(QPoint(), captureGeometry().size());
    const QPoint displayOrigin = captureGeometry().topLeft();
    if (m_scrollingThumbnailMode == ScreenshotScrollingRecognitionMode::Horizontal) {
        const int availableWidth = std::max(1, bounds.width() - kScrollingThumbnailMargin * 2);
        m_scrollingThumbnail->setMaximumPreviewExtent(availableWidth);
        const int thumbnailWidth = m_scrollingThumbnail->width();
        const int thumbnailHeight = m_scrollingThumbnail->height();
        const int aboveY =
            m_scrollingThumbnailAnchor.top() - kScrollingThumbnailGap - thumbnailHeight;
        const bool fitsAbove = aboveY >= kScrollingThumbnailMargin;
        const int belowY = m_scrollingThumbnailAnchor.bottom() + 1 + kScrollingThumbnailGap;
        const int maximumY = std::max(kScrollingThumbnailMargin, bounds.height() - thumbnailHeight -
                                                                     kScrollingThumbnailMargin);
        const int preferredY = fitsAbove ? aboveY : belowY;
        const int y = std::clamp(preferredY, kScrollingThumbnailMargin, maximumY);
        const int maximumX = std::max(kScrollingThumbnailMargin,
                                      bounds.width() - thumbnailWidth - kScrollingThumbnailMargin);
        const int x =
            std::clamp(m_scrollingThumbnailAnchor.x(), kScrollingThumbnailMargin, maximumX);
        m_scrollingThumbnail->move(QPoint(x, y) + displayOrigin);
        return;
    }

    const int availableHeight = std::max(1, bounds.height() - kScrollingThumbnailMargin * 2);
    m_scrollingThumbnail->setMaximumPreviewHeight(
        std::min(std::max(1, m_scrollingThumbnailAnchor.height()), availableHeight));

    const int thumbnailWidth = m_scrollingThumbnail->width();
    const int thumbnailHeight = m_scrollingThumbnail->height();
    const int rightX = m_scrollingThumbnailAnchor.x() + m_scrollingThumbnailAnchor.width() +
                       kScrollingThumbnailGap;
    const bool fitsRight =
        rightX + thumbnailWidth <= bounds.right() - kScrollingThumbnailMargin + 1;
    const int leftX = m_scrollingThumbnailAnchor.x() - kScrollingThumbnailGap - thumbnailWidth;
    const int preferredX = fitsRight ? rightX : leftX;
    const int maximumX = std::max(kScrollingThumbnailMargin,
                                  bounds.width() - thumbnailWidth - kScrollingThumbnailMargin);
    // The overlay is scoped to the display that owns the capture selection.
    // Keep the thumbnail inside that display even when neither side has room.
    const int x = std::clamp(preferredX, kScrollingThumbnailMargin, maximumX);
    const int maximumY = std::max(kScrollingThumbnailMargin,
                                  bounds.height() - thumbnailHeight - kScrollingThumbnailMargin);
    const int y = std::clamp(m_scrollingThumbnailAnchor.y(), bounds.top(), maximumY);
    m_scrollingThumbnail->move(QPoint(x, y) + displayOrigin);
}

void ScreenshotOverlayWindow::updateWindowMask() {
    const QRect hole =
        m_scrollingVisualHole.translated(m_captureFrameMargins.left(), m_captureFrameMargins.top())
            .intersected(rect());
    QRegion visibleRegion;
    if (hole.isEmpty()) {
        visibleRegion = {};
    } else {
        visibleRegion = QRegion(rect()).subtracted(QRegion(hole));
    }
    if (m_windowMaskInitialized && visibleRegion == m_appliedWindowMask) {
        return;
    }
    m_windowMaskInitialized = true;
    m_appliedWindowMask = visibleRegion;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    ++m_windowMaskApplicationCount;
#endif
    if (visibleRegion.isEmpty()) {
        clearMask();
    } else {
        setMask(visibleRegion);
    }
}

bool ScreenshotOverlayWindow::handleCanvasEvent(QEvent* event) {
    if (event == nullptr) {
        return false;
    }

    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::Wheel) &&
        !m_eventSink.acceptOverlayInput(event->spontaneous())) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress) {
        return handleCanvasKeyPress(static_cast<QKeyEvent*>(event));
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseButtonDblClick) {
        return handleCanvasMouseEvent(static_cast<QMouseEvent*>(event));
    }
    if (event->type() == QEvent::Wheel) {
        return handleCanvasWheel(static_cast<QWheelEvent*>(event));
    }

    return false;
}

bool ScreenshotOverlayWindow::handleCanvasKeyPress(QKeyEvent* event) {
    if (event == nullptr) {
        return false;
    }
    if (m_canvas != nullptr && m_canvas->hasActiveTextEditing()) {
        return false;
    }
    if (m_eventSink.shouldBlockUnhandledOverlayKeyInput()) {
        event->accept();
        return true;
    }
    return false;
}

bool ScreenshotOverlayWindow::handleCanvasMouseEvent(QMouseEvent* event) {
    if (event == nullptr) {
        return false;
    }

    if (event->type() == QEvent::MouseButtonDblClick && event->button() == Qt::LeftButton &&
        m_eventSink.handleRegionDoubleClick(this, event->position())) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress && event->button() == Qt::RightButton) {
        const auto action = m_eventSink.handleOverlayRightClick(this, event->position());
        if (action == ScreenshotOverlayRightClickResult::CancelCapture) {
            static_cast<void>(m_mouseReleaseAction.arm(
                this, Qt::RightButton, [this] { m_eventSink.completeRightClickCancellation(); }));
        }
        if (action != ScreenshotOverlayRightClickResult::Ignored) {
            event->accept();
            return true;
        }
    }

    // A drawing gesture owns the pointer until the canvas releases its grab. The
    // selection border may cross that gesture, but cannot take over its moves or release.
    if (m_canvas != nullptr && QWidget::mouseGrabber() == m_canvas &&
        (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)) {
        return false;
    }

    if (event->type() == QEvent::MouseMove && !event->buttons().testFlag(Qt::LeftButton)) {
        m_eventSink.handleOverlayMouseMove(this, event->position());
    }

    const bool leftButtonActive = event->type() == QEvent::MouseButtonPress
                                      ? event->button() == Qt::LeftButton
                                      : event->buttons().testFlag(Qt::LeftButton);
    if (!m_eventSink.shouldHandleOverlayMouseEvent(this, event->position(), leftButtonActive)) {
        if (event->type() == QEvent::MouseButtonPress) {
            m_eventSink.raiseToolbarForCanvasInteraction();
        }
        return false;
    }
    return dispatchHandledMouseEvent(event);
}

bool ScreenshotOverlayWindow::handleCanvasWheel(QWheelEvent* event) {
    if (event == nullptr) {
        return false;
    }
    if (m_canvas != nullptr && m_canvas->hasActiveTextEditing()) {
        return false;
    }
    if (!m_eventSink.handleOverlayWheel(this, event->position(), event->angleDelta(),
                                        event->pixelDelta())) {
        return false;
    }

    event->accept();
    return true;
}

bool ScreenshotOverlayWindow::dispatchHandledMouseEvent(QMouseEvent* event) {
    if (event == nullptr) {
        return false;
    }

    if (event->type() == QEvent::MouseButtonPress && event->button() == Qt::LeftButton) {
        m_eventSink.handleOverlayMousePress(this, event->position());
        event->accept();
        return true;
    }
    if (event->type() == QEvent::MouseMove && event->buttons().testFlag(Qt::LeftButton)) {
        m_eventSink.handleOverlayMouseMove(this, event->position());
        event->accept();
        return true;
    }
    if (event->type() == QEvent::MouseMove) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease && event->button() == Qt::LeftButton) {
        m_eventSink.handleOverlayMouseRelease(this, event->position());
        event->accept();
        return true;
    }

    return false;
}

void ScreenshotOverlayWindow::setRegionTypeControlVisible(bool visible, ScreenshotRegionType type,
                                                          const QRectF& selectionGlobal,
                                                          const QPointF& cursorGlobal) {
    m_regionTypeControl->setType(type);
    if (visible) {
        layoutRegionTypeControl();
    }
    m_regionTypeControl->setPresentationVisible(visible, selectionGlobal, cursorGlobal);
}

void ScreenshotOverlayWindow::layoutRegionTypeControl() {
    if (m_regionTypeControl == nullptr) {
        return;
    }
    const int maximumWidth = std::max(1, width() - 16);
    if (m_regionTypeControl->maximumWidth() != maximumWidth) {
        m_regionTypeControl->setMaximumWidth(maximumWidth);
        m_regionTypeControl->adjustSize();
    }
    // Keep the capture canvas full-screen, but place controls below the notch.
    // QWindow reports logical margins, matching QWidget coordinates at every DPR.
    const int safeTop = windowHandle() != nullptr ? windowHandle()->safeAreaMargins().top() : 0;
    const QPoint position(
        std::max(0, (width() - m_regionTypeControl->width()) / 2),
        std::max(0, std::min(safeTop + 12, height() - m_regionTypeControl->height())));
    if (m_regionTypeControl->pos() != position)
        m_regionTypeControl->move(position);
}

void ScreenshotOverlayWindow::setSelectionDraft(const QPainterPath& path,
                                                const QVector<QPointF>& vertices) {
    m_screenshotRenderer->setSelectionDraft(path, vertices);
}

void ScreenshotOverlayWindow::setScrollingTrimModel(
    std::shared_ptr<ScreenshotScrollingTrimRange> trim) {
    if (m_scrollingThumbnail)
        m_scrollingThumbnail->setTrimModel(std::move(trim));
}
