#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/crosshaircursor.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"
#include "screenshotoverlaycursor.h"
#include "screenshotoverlayframepresenter.h"
#include "snow_shot/presentation/screenshotmessageservice.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/canvasstatusreadout.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QEvent>
#include <QBackingStore>
#include <QCoreApplication>
#include <QCursor>
#include "snow_shot/platform/windows/windowchrome.h"
#ifdef Q_OS_MACOS
#include "snow_shot/platform/screenshotnative.h"
#endif
#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include "../capture/screenshotscrollingdiagnostics.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRegion>
#include <QScopedValueRollback>
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
constexpr qsizetype kBackingStorePreparationSliceBytes = 1024 * 1024;
// A conservative stride also covers platforms with larger (for example, 16 KiB) pages.
constexpr qsizetype kBackingStorePreparationPageBytes = 4096;

QEvent::Type backingStorePreparationEventType() {
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
}

class BackingStorePreparationEvent final : public QEvent {
  public:
    explicit BackingStorePreparationEvent(quint64 preparationGeneration)
        : QEvent(backingStorePreparationEventType()), generation(preparationGeneration) {}

    const quint64 generation;
};

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
#ifdef Q_OS_MACOS
    setWindowFlag(Qt::NoDropShadowWindowHint);
#endif
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
    m_regionTypeControl->installEventFilter(this);
    m_regionTypeControl->hide();
    m_framePresenter = std::make_unique<ScreenshotOverlayFramePresenter>(*this);

    if (m_canvas != nullptr) {
        snow_shot::presentation::configureCanvasCrosshairCursor(*m_canvas);
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
                [this](const QPointF& position) {
                    m_eventSink.handleUnhandledMiddleClick(this, m_canvas->mapTo(this, position));
                });
    }

    initializeScreenshotSurface();
    setCanvasClearBackgroundEnabled(false);
}

ScreenshotOverlayWindow::~ScreenshotOverlayWindow() {
    cancelBackingStorePreparation();
    setScrollingCaptureMode(false);
    if (m_canvas != nullptr) {
        m_canvas->setTextEditingBounds(std::nullopt);
        m_canvas->clearWatermarkRenderArea();
        m_canvas->clearSpotlightRenderArea();
    }
    if (m_canvas != nullptr && m_canvas->customRenderer() == m_screenshotRenderer.get()) {
        m_canvas->setCustomRenderer(nullptr);
    }
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
    if (geometry() != frame) {
        cancelBackingStorePreparation();
        setGeometry(frame);
    }
    layout()->activate();
}

QRect ScreenshotOverlayWindow::captureGeometry() const {
    return geometry().marginsRemoved(m_captureFrameMargins);
}

QPoint ScreenshotOverlayWindow::canvasLocalPosition(const QPoint& globalPosition) const {
    return globalPosition - captureGeometry().topLeft();
}

void ScreenshotOverlayWindow::setScreenshotImage(QImage image, const QRectF& canvasRect) {
    cancelBackingStorePreparation();
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setImage(std::move(image), canvasRect);
    }
}

void ScreenshotOverlayWindow::setScreenshotImageSource(ScreenshotImageSource source,
                                                       const QRectF& damage) {
    setScreenshotImageSource(std::move(source), damage, {});
}

void ScreenshotOverlayWindow::setScreenshotImageSource(
    ScreenshotImageSource source, const QRectF& damage,
    const ScreenshotImageSource& samplingSource) {
    cancelBackingStorePreparation();
    if (m_screenshotRenderer)
        m_screenshotRenderer->setImageSource(std::move(source), damage, samplingSource);
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

void ScreenshotOverlayWindow::setSelectionCenterGuideLineColor(const QColor& color) {
    if (m_screenshotRenderer != nullptr) {
        m_screenshotRenderer->setSelectionCenterGuideLineColor(color);
    }
}

QRectF ScreenshotOverlayWindow::screenshotSelection() const {
    return m_screenshotRenderer != nullptr ? m_screenshotRenderer->selection() : QRectF();
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
    state.effectWidth = shadowWidth;
    state.effectColor = shadowColor;
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
        m_canvas->setTextEditingBounds(
            configuredArea.isEmpty() ? std::nullopt : std::optional<QRectF>(configuredArea));
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
        m_canvas->setTextEditingBounds(std::nullopt);
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
    retirePresentation();
    setScrollingCaptureMode(false);
    if (m_canvas != nullptr) {
        m_canvas->setTextEditingBounds(std::nullopt);
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
    snow_shot::presentation::detail::commitOverlayCursor(
        captureGeometry(), QCursor::pos(), [this]() {
            static_cast<void>(snow_shot::presentation::applyNativeCrosshairCursor(
                m_canvas ? m_canvas->devicePixelRatioF() : devicePixelRatioF()));
        });
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
    QJsonObject fields{{QStringLiteral("overlay_rect"), scrollingRect(geometry())},
                       {QStringLiteral("dpr"), devicePixelRatioF()},
                       {QStringLiteral("thumbnail_visible"),
                        m_scrollingThumbnail != nullptr && m_scrollingThumbnail->isVisible()}};
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() == QStringLiteral("windows") && internalWinId() != 0) {
        const auto hwnd = reinterpret_cast<HWND>(internalWinId());
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

void ScreenshotOverlayWindow::setScrollingCaptureMode(bool enabled) {
    if (!enabled) {
        clearScrollingResultPreview();
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
                                                      ScreenshotScrollingRecognitionMode mode,
                                                      const QSize& captureViewportSize) {
    clearScrollingResultPreview();
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
        connect(m_scrollingThumbnail, &ScreenshotScrollingThumbnailWidget::hoverSourceRectChanged,
                this, &ScreenshotOverlayWindow::scrollingThumbnailHoverChanged);
    }

    m_scrollingThumbnailAnchor = localSelection.normalized();
    m_scrollingThumbnailMode = mode;
    m_scrollingThumbnail->setRecognitionMode(mode);
    m_scrollingThumbnail->reset();
    m_scrollingThumbnail->setCaptureViewportSize(captureViewportSize);
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
    if (m_scrollingThumbnailAnchor != localSelection) {
        clearScrollingResultPreview();
        if (m_scrollingThumbnail != nullptr) {
            m_scrollingThumbnail->clearHover();
        }
    }
    m_scrollingThumbnailAnchor = localSelection;
    layoutScrollingThumbnail();
}

void ScreenshotOverlayWindow::clearScrollingThumbnail() {
    clearScrollingResultPreview();
    delete std::exchange(m_scrollingThumbnail, nullptr);
    m_scrollingThumbnailAnchor = {};
    m_scrollingThumbnailMode = ScreenshotScrollingRecognitionMode::Vertical;
}

void ScreenshotOverlayWindow::setScrollingResultPreview(const QImage& image,
                                                        const QRectF& canvasRect, bool showStatus,
                                                        std::optional<Qt::Orientation> cropGuide) {
    if (!m_scrollingCaptureMode || m_screenshotRenderer == nullptr) {
        clearScrollingResultPreview();
        return;
    }
    m_screenshotRenderer->setScrollingResultPreview(image, canvasRect, cropGuide);
    if (!m_screenshotRenderer->hasScrollingResultPreview()) {
        clearScrollingResultPreview();
        return;
    }
    m_scrollingResultPreviewCanvasRect = canvasRect.normalized();
    m_scrollingResultPreviewStatusVisible = showStatus;
    updateScrollingResultPreviewReadout();
}

void ScreenshotOverlayWindow::clearScrollingResultPreview() {
    if (m_screenshotRenderer == nullptr || (!m_screenshotRenderer->hasScrollingResultPreview() &&
                                            m_scrollingResultPreviewCanvasRect.isEmpty())) {
        return;
    }
    const QRect damage =
        m_canvas != nullptr ? m_canvas->viewRectForCanvasRect(m_scrollingResultPreviewCanvasRect, 1)
                                  .intersected(m_canvas->rect())
                            : QRect();
    m_screenshotRenderer->clearScrollingResultPreview();
    m_scrollingResultPreviewCanvasRect = {};
    m_scrollingResultPreviewStatusVisible = false;
    if (m_scrollingResultPreviewReadout != nullptr) {
        m_scrollingResultPreviewReadout->dismiss();
    }
    if (m_canvas != nullptr && isVisible() && updatesEnabled() && m_canvas->updatesEnabled()) {
        // A queued update can outlive hover exit and be captured as a new stitch frame.
        // Present the transparent restoration synchronously, before source restart.
        const QRect canvasDamage = damage.isEmpty() ? m_canvas->rect() : damage;
        repaint(canvasDamage.translated(m_canvas->pos()));
        m_canvas->repaint(canvasDamage);
    }
}

void ScreenshotOverlayWindow::updateScrollingResultPreviewReadout() {
    if (!m_scrollingResultPreviewStatusVisible || m_screenshotRenderer == nullptr ||
        !m_screenshotRenderer->hasScrollingResultPreview() || m_canvas == nullptr) {
        if (m_scrollingResultPreviewReadout != nullptr) {
            m_scrollingResultPreviewReadout->dismiss();
        }
        return;
    }
    if (m_scrollingResultPreviewReadout == nullptr) {
        m_scrollingResultPreviewReadout = new CanvasStatusReadout(this);
        m_scrollingResultPreviewReadout->setObjectName(
            QStringLiteral("scrollingScreenshotResultPreviewLabel"));
        m_scrollingResultPreviewReadout->setMessage(CanvasStatusReadout::Message::ResultPreview);
    }
    const QRectF anchor = m_canvas->canvasToViewTransform()
                              .mapRect(m_scrollingResultPreviewCanvasRect)
                              .intersected(QRectF(m_canvas->rect()))
                              .translated(m_canvas->pos());
    if (anchor.isEmpty()) {
        m_scrollingResultPreviewReadout->dismiss();
        return;
    }
    m_scrollingResultPreviewReadout->layoutIn(anchor);
    m_scrollingResultPreviewReadout->showPersistent();
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
quint64 ScreenshotOverlayWindow::transparentClearCountForTesting() const {
    return m_transparentClearCount;
}

ScreenshotCanvasRenderer* ScreenshotOverlayWindow::screenshotRendererForTesting() const {
    return m_screenshotRenderer.get();
}

void ScreenshotOverlayWindow::setBackingStorePreparationModeForTesting(
    ScreenshotBackingStorePreparationMode mode) {
    cancelBackingStorePreparation();
    m_backingStorePreparationMode = mode;
}

void ScreenshotOverlayWindow::resetBackingStorePreparationCountersForTesting() {
    m_backingStorePreparationCounters = {};
}

ScreenshotBackingStorePreparationCounters
ScreenshotOverlayWindow::backingStorePreparationCountersForTesting() const {
    return m_backingStorePreparationCounters;
}

bool ScreenshotOverlayWindow::isBackingStorePreparationPendingForTesting() const {
    return m_backingStorePreparationActive;
}
#endif

void ScreenshotOverlayWindow::prepareBackingStore() {
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    if (m_backingStorePreparationMode == ScreenshotBackingStorePreparationMode::Disabled)
        return;
#endif
    if (isVisible() || size().isEmpty() || internalWinId() == 0)
        return;

    const qreal dpr = devicePixelRatioF();
    if ((m_backingStorePreparationActive || m_backingStorePreparationComplete) &&
        m_backingStorePreparationSize == size() && m_backingStorePreparationScreen == screen() &&
        qFuzzyCompare(m_backingStorePreparationDevicePixelRatio, dpr) &&
        m_backingStorePreparationNativeId == internalWinId()) {
        return;
    }

    cancelBackingStorePreparation();
    m_backingStorePreparationSize = size();
    m_backingStorePreparationScreen = screen();
    m_backingStorePreparationDevicePixelRatio = dpr;
    m_backingStorePreparationNativeId = internalWinId();
    m_backingStorePreparationActive = true;
    postBackingStorePreparation();
}

void ScreenshotOverlayWindow::cancelBackingStorePreparation() {
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    if (m_backingStorePreparationActive)
        ++m_backingStorePreparationCounters.canceledPreparations;
#endif
    ++m_backingStorePreparationGeneration;
    m_backingStorePreparationActive = false;
    m_backingStorePreparationAllocated = false;
    m_backingStorePreparationComplete = false;
    m_backingStorePreparationNextByte = 0;
    m_backingStorePreparationSize = {};
    m_backingStorePreparationScreen = nullptr;
    m_backingStorePreparationDevicePixelRatio = 0.0;
    m_backingStorePreparationNativeId = 0;
}

void ScreenshotOverlayWindow::postBackingStorePreparation() {
    // Ready capture results and input outrank speculative page preparation.
    QCoreApplication::postEvent(
        this, new BackingStorePreparationEvent(m_backingStorePreparationGeneration),
        Qt::LowEventPriority);
}

void ScreenshotOverlayWindow::continueBackingStorePreparation(quint64 generation) {
    if (!m_backingStorePreparationActive || generation != m_backingStorePreparationGeneration)
        return;
    if (isVisible() || internalWinId() == 0 ||
        m_backingStorePreparationNativeId != internalWinId() ||
        m_backingStorePreparationSize != size() || m_backingStorePreparationScreen != screen() ||
        !qFuzzyCompare(m_backingStorePreparationDevicePixelRatio, devicePixelRatioF())) {
        cancelBackingStorePreparation();
        return;
    }

    SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.backing_store_prepare_slice");
    QBackingStore* store = backingStore();
    if (store == nullptr) {
        cancelBackingStorePreparation();
        return;
    }
    if (!m_backingStorePreparationAllocated) {
        store->resize(size());
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        ++m_backingStorePreparationCounters.allocationRequests;
#endif
    }

    // Cocoa allocates its IOSurface in beginPaint, rather than resize. An empty
    // region avoids clearing a desktop-sized alpha surface in each slice and
    // leaves its dirty state intact for Qt's complete first visible paint.
    store->beginPaint(QRegion());
    m_backingStorePreparationAllocated = true;
    bool preparePages = true;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    preparePages =
        m_backingStorePreparationMode != ScreenshotBackingStorePreparationMode::AllocationOnly;
#endif
    QPaintDevice* device = store->paintDevice();
    if (preparePages && device != nullptr && device->devType() == QInternal::Image) {
        auto* image = static_cast<QImage*>(device);
        const qsizetype totalBytes = image->sizeInBytes();
        const qsizetype sliceEnd = std::min(totalBytes, m_backingStorePreparationNextByte +
                                                            kBackingStorePreparationSliceBytes);
        const qsizetype sliceBytes = sliceEnd - m_backingStorePreparationNextByte;
        volatile uchar* pixels = image->bits();
        if (pixels != nullptr && sliceBytes > 0) {
            quint64 pageWrites = 0;
            for (qsizetype offset = m_backingStorePreparationNextByte; offset < sliceEnd;
                 offset += kBackingStorePreparationPageBytes) {
                pixels[offset] = 0;
                ++pageWrites;
            }
            // The final allocation page can extend beyond the last stride, including
            // when the image's address is not aligned to a system page boundary.
            pixels[sliceEnd - 1] = 0;
            ++pageWrites;
            m_backingStorePreparationNextByte = sliceEnd;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
            ++m_backingStorePreparationCounters.pageTouchSlices;
            m_backingStorePreparationCounters.touchedBytes += static_cast<quint64>(sliceBytes);
            m_backingStorePreparationCounters.pageWrites += pageWrites;
            m_backingStorePreparationCounters.maximumSliceBytes =
                std::max(m_backingStorePreparationCounters.maximumSliceBytes,
                         static_cast<quint64>(sliceBytes));
#else
            Q_UNUSED(pageWrites);
#endif
            preparePages = sliceEnd < totalBytes;
        } else {
            preparePages = false;
        }
    } else {
        preparePages = false;
    }
    store->endPaint();

    if (preparePages) {
        postBackingStorePreparation();
    } else {
        m_backingStorePreparationActive = false;
        m_backingStorePreparationComplete = true;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        ++m_backingStorePreparationCounters.completedPreparations;
#endif
    }
}

void ScreenshotOverlayWindow::warmPresentationSurface() {
    retirePresentation();
    if (m_framePresenter != nullptr) {
        m_framePresenter->warmPresentationSurface();
    }
}

void ScreenshotOverlayWindow::showPreparedFrame(bool deferFirstPaint) {
    static_cast<void>(showPreparedFrameForPresentation(beginPresentation(), deferFirstPaint));
}

quint64 ScreenshotOverlayWindow::beginPresentation() {
    cancelBackingStorePreparation();
    ++m_presentationGeneration;
    m_presentationActive = true;
    return m_presentationGeneration;
}

quint64 ScreenshotOverlayWindow::presentationGeneration() const {
    return m_presentationGeneration;
}

bool ScreenshotOverlayWindow::isPresentationCurrent(quint64 generation) const {
    return m_presentationActive && generation == m_presentationGeneration;
}

void ScreenshotOverlayWindow::retirePresentation() {
    cancelBackingStorePreparation();
    ++m_presentationGeneration;
    m_presentationActive = false;
}

bool ScreenshotOverlayWindow::showPreparedFrameForPresentation(quint64 generation,
                                                               bool deferFirstPaint) {
    if (!isPresentationCurrent(generation)) {
        return false;
    }
    // The presenter's internal show() publishes this reservation. A separate public
    // show() starts a new presentation even when the pooled widget remains visible.
    const QScopedValueRollback<bool> showingPreparedFrame(m_showingPreparedFrame, !isVisible());
    if (m_framePresenter != nullptr) {
        m_framePresenter->presentPreparedFrame(deferFirstPaint);
    }
    return isPresentationCurrent(generation) && isVisible();
}

void ScreenshotOverlayWindow::setVisible(bool visible) {
    // Consume the prepared show's reservation before Qt dispatches Show/focus events;
    // a reentrant public show() must acquire its own generation.
    const bool showingPreparedFrame = visible && std::exchange(m_showingPreparedFrame, false);
    if (!visible) {
        // hide() can cancel a pending reveal while the widget is already hidden;
        // QWidget does not emit a Hide event in that case.
        retirePresentation();
    } else if (!showingPreparedFrame) {
        static_cast<void>(beginPresentation());
    }
    QWidget::setVisible(visible);
}

void ScreenshotOverlayWindow::hideForNativeSurfaceRelease() {
    retirePresentation();
    setUpdatesEnabled(false);
    if (m_canvas != nullptr) {
        m_canvas->setInteractionEnabled(false);
        m_canvas->setUpdatesEnabled(false);
    }
#ifdef Q_OS_MACOS
    snow_shot::platform::retireScreenshotWindowPresentation(this);
#endif
    hide();
}

void ScreenshotOverlayWindow::releaseNativeSurface() {
    hideForNativeSurfaceRelease();
    clearScrollingResultPreview();

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
    retirePresentation();
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
    // The canvas and its event filter own pointer input. Ignored canvas events
    // must not bubble back to the window's canvas-input forwarding path.
    m_canvas->setAttribute(Qt::WA_NoMousePropagation, true);
}

bool ScreenshotOverlayWindow::event(QEvent* event) {
    if (event != nullptr && forwardCanvasInputEvent(event)) {
        return true;
    }
    if (event != nullptr && event->type() == backingStorePreparationEventType()) {
        continueBackingStorePreparation(
            static_cast<BackingStorePreparationEvent*>(event)->generation);
        return true;
    }
    if (event != nullptr &&
        (event->type() == QEvent::Resize || event->type() == QEvent::ScreenChangeInternal ||
         event->type() == QEvent::DevicePixelRatioChange ||
         event->type() == QEvent::PlatformSurface)) {
        cancelBackingStorePreparation();
    }
    if (event != nullptr &&
        (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate)) {
        m_eventSink.clearOverlaySnapping();
        m_eventSink.cancelEffectDrag();
        m_eventSink.leaveEffectEditors();
    }
    if (event != nullptr && event->type() == QEvent::Hide) {
        retirePresentation();
        clearScrollingResultPreview();
    }
    if (event != nullptr && (event->type() == QEvent::DevicePixelRatioChange ||
                             event->type() == QEvent::ScreenChangeInternal)) {
        updateScrollingResultPreviewReadout();
    }
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
    if (watched == m_regionTypeControl && event != nullptr && event->type() == QEvent::Resize)
        layoutRegionTypeControl();
    if (watched == m_canvas && event != nullptr) {
        if (event->type() == QEvent::Leave)
            m_eventSink.leaveEffectEditors();
        if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide)
            m_eventSink.cancelEffectDrag();
    }
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
    updateScrollingResultPreviewReadout();
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

bool ScreenshotOverlayWindow::forwardCanvasInputEvent(QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        break;
    default:
        return false;
    }
    if (m_canvas == nullptr || !m_canvas->isVisible() || !m_canvas->isEnabled() ||
        m_canvas->testAttribute(Qt::WA_TransparentForMouseEvents)) {
        return false;
    }

    const auto* pointer = static_cast<QSinglePointEvent*>(event);
    const QPointF position = m_canvas->mapFrom(this, pointer->position());
    const bool continuingGesture =
        event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::TabletRelease ||
        ((event->type() == QEvent::MouseMove || event->type() == QEvent::TabletMove) &&
         pointer->buttons() != Qt::NoButton);
    if (!continuingGesture) {
        if (position.x() < 0 || position.y() < 0 || position.x() >= m_canvas->width() ||
            position.y() >= m_canvas->height()) {
            return false;
        }
        if (const QWidget* child = childAt(pointer->position());
            child != nullptr && child != m_canvas) {
            return false;
        }
    }

    // Qt's QWidget child hit test excludes the fractional strip beyond width-1
    // and height-1. Those valid display coordinates land on this window instead
    // of its canvas. Route them through the same canvas path, including drawing
    // tools and the remainder of Qt's implicit grab, without changing geometry.
    std::unique_ptr<QSinglePointEvent> mapped;
    if (event->type() == QEvent::Wheel) {
        const auto* wheel = static_cast<QWheelEvent*>(event);
        mapped = std::make_unique<QWheelEvent>(
            position, wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(),
            wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted(),
            wheel->source(), wheel->pointingDevice());
    } else if (event->type() == QEvent::TabletPress || event->type() == QEvent::TabletMove ||
               event->type() == QEvent::TabletRelease) {
        const auto* tablet = static_cast<QTabletEvent*>(event);
        mapped = std::make_unique<QTabletEvent>(
            tablet->type(), tablet->pointingDevice(), position, tablet->globalPosition(),
            tablet->pressure(), tablet->xTilt(), tablet->yTilt(), tablet->tangentialPressure(),
            tablet->rotation(), tablet->z(), tablet->modifiers(), tablet->button(),
            tablet->buttons());
    } else {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        mapped = std::make_unique<QMouseEvent>(mouse->type(), position, mouse->scenePosition(),
                                               mouse->globalPosition(), mouse->button(),
                                               mouse->buttons(), mouse->modifiers(),
                                               mouse->source(), mouse->pointingDevice());
    }
    mapped->setTimestamp(pointer->timestamp());
    // sendEvent marks the remapped event synthetic. Preserve its actual origin
    // for the startup gate, scoped to this exact event rather than nested input.
    const QScopedValueRollback forwardedEvent(m_forwardedCanvasInputEvent,
                                              static_cast<const QEvent*>(mapped.get()));
    const QScopedValueRollback genuine(m_forwardedCanvasInputGenuine, event->spontaneous());
    QCoreApplication::sendEvent(m_canvas, mapped.get());
    event->setAccepted(mapped->isAccepted());
    return true;
}

bool ScreenshotOverlayWindow::handleCanvasEvent(QEvent* event) {
    if (event == nullptr) {
        return false;
    }
    if (event->type() == QEvent::FocusOut)
        m_eventSink.clearOverlaySnapping();

    const bool tablet = event->type() == QEvent::TabletPress ||
                        event->type() == QEvent::TabletMove ||
                        event->type() == QEvent::TabletRelease;
    if ((tablet || event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease ||
         event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove ||
         event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::Wheel) &&
        !m_eventSink.acceptOverlayInput(event == m_forwardedCanvasInputEvent
                                            ? m_forwardedCanvasInputGenuine
                                            : event->spontaneous())) {
        event->accept();
        return true;
    }
    if (tablet) {
        auto* tabletEvent = static_cast<QTabletEvent*>(event);
        if (event->type() == QEvent::TabletMove && m_screenshotRenderer) {
            if (!m_eventSink.presentOverlayPointer(this, tabletEvent->position())) {
                m_screenshotRenderer->setGuideCursorPosition(tabletEvent->position());
            }
        }
        const bool barrelButtonChange =
            m_canvas != nullptr && m_canvas->canvasTool() == SnowCanvasTool::FreeDraw &&
            QWidget::mouseGrabber() == m_canvas && tabletEvent->button() != Qt::LeftButton &&
            tabletEvent->buttons().testFlag(Qt::LeftButton) && event->type() != QEvent::TabletMove;
        const auto type = barrelButtonChange                       ? QEvent::MouseMove
                          : event->type() == QEvent::TabletPress   ? QEvent::MouseButtonPress
                          : event->type() == QEvent::TabletRelease ? QEvent::MouseButtonRelease
                                                                   : QEvent::MouseMove;
        QMouseEvent pointer(type, tabletEvent->position(), tabletEvent->globalPosition(),
                            barrelButtonChange ? Qt::NoButton : tabletEvent->button(),
                            tabletEvent->buttons(), tabletEvent->modifiers(),
                            tabletEvent->pointingDevice());
        if (handleCanvasMouseEvent(&pointer)) {
            event->accept();
            return true;
        }
        return false;
    }
    if (event->type() == QEvent::MouseMove && m_screenshotRenderer) {
        const QPointF position = static_cast<QMouseEvent*>(event)->position();
        if (!m_eventSink.presentOverlayPointer(this, position))
            m_screenshotRenderer->setGuideCursorPosition(position);
    }
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Control && !key->isAutoRepeat()) {
            auto modifiers = key->modifiers();
            modifiers.setFlag(Qt::ControlModifier, event->type() == QEvent::KeyPress);
            m_eventSink.updateOverlayModifiers(modifiers, true);
        }
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
    m_eventSink.updateOverlayModifiers(event->modifiers(), false);

    if (event->type() == QEvent::MouseButtonDblClick && event->button() == Qt::LeftButton &&
        m_eventSink.handleEffectDoubleClick(this, event->position())) {
        event->accept();
        return true;
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
        !m_eventSink.effectDragActive() &&
        (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)) {
        return false;
    }

    if (event->type() == QEvent::MouseMove && !event->buttons().testFlag(Qt::LeftButton)) {
        m_eventSink.cancelEffectDrag();
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
    if (!m_eventSink.handleOverlayWheel(this, *event)) {
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
