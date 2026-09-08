#include "snow_shot/presentation/screenshotscrollingcapturecontroller.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include "snow_shot/platform/windows/windowchrome.h"
#endif

#include "adaptivescrollingcapturecadence.h"
#include "screenshotscrollingautoscroller.h"
#include "snow_shot/platform/windows/scrollinput.h"
#include "screenshotscrollingpipeline.h"
#include "windowcaptureexclusion.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace {
using snow_shot::capture_detail::nativeScrollingSource;
using snow_shot::capture_detail::ScreenshotScrollingPipeline;
using snow_shot::capture_detail::ScrollingPipelineFrame;
using AdaptiveScrollCadence = snow_shot::capture_detail::AdaptiveScrollingCaptureCadence;
QRect logicalSelectionRect(const ScreenshotGeometryMapper& geometry,
                           const CapturedDisplayModel& display, const QRect& canvasSelection) {
    const ScreenshotHalfOpenRect selection = ScreenshotHalfOpenRect::fromRect(canvasSelection);
    const QPointF topLeft = geometry.logicalPositionForCanvasPoint(display, selection.topLeft());
    const QPointF bottomRight =
        geometry.logicalPositionForCanvasPoint(display, selection.bottomRight());
    return ScreenshotHalfOpenRect::fromEdges(topLeft.x(), topLeft.y(), bottomRight.x(),
                                             bottomRight.y())
        .toAlignedQRect();
}
} // namespace

struct ScreenshotScrollingCaptureController::Impl {
    Impl(ScreenshotScrollingCaptureController& ownerValue,
         ScreenshotScrollingCaptureControllerContext contextValue)
        : owner(ownerValue), context(contextValue) {}

    ~Impl() {
        stop(false);
        shutdownWorker();
    }

    bool start(QRect selection, ScreenshotScrollingRecognitionMode requestedMode) {
        if (selection.width() < 1 || selection.height() < 1) {
            return false;
        }

        const CapturedDisplayModel* anchorDisplay = context.geometry.displayForCanvasPoint(
            context.displaySession, ScreenshotHalfOpenRect::fromRect(selection).center());
        if (anchorDisplay == nullptr) {
            anchorDisplay =
                context.geometry.displayForCanvasRect(context.displaySession, QRectF(selection));
        }
        ScreenshotOverlayWindow* anchorOverlay =
            context.displaySession.overlayForDisplay(anchorDisplay);
        if (anchorDisplay == nullptr || anchorOverlay == nullptr) {
            return false;
        }

        if (active) {
            stop(false);
        }
        ensureWorker();
        if (pipeline == nullptr) {
            return false;
        }

        if (!excludeScrollingWindowsFromCapture(anchorOverlay)) {
            return false;
        }

        canvasSelection = selection;
        restoreOriginalColors = context.restoreOriginalScreenColors();
        mode = requestedMode;
        thumbnailHost = anchorOverlay;
        active = true;
        ++generation;
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.scrolling"), QStringLiteral("scrolling.started"),
            {{QStringLiteral("operation"), QString::number(generation)}});
        pendingResultRequestId.reset();
        if (pipeline)
            pipeline->reset(generation);

        context.overlayCoordinator.setScrollingCaptureMode(context.displaySession,
                                                           QRectF(canvasSelection), true);

        const QRect logicalSelection =
            logicalSelectionRect(context.geometry, *anchorDisplay, canvasSelection);
        thumbnailHost->beginScrollingThumbnail(
            logicalSelection.translated(-thumbnailHost->geometry().topLeft()), mode);

        const quint64 requestGeneration = generation;
        const QRect requestSelection = canvasSelection;
        const QRect requestPhysicalSelection =
            canvasSelection.translated(context.geometry.canvasOrigin());
        autoScroller.start(requestPhysicalSelection, mode);
        const AdaptiveScrollCadence::Config requestCadenceConfig = cadenceConfig;
        pipeline->begin(requestGeneration, requestSelection.size(), mode,
                        nativeScrollingSource(requestPhysicalSelection, restoreOriginalColors),
                        requestCadenceConfig);
        return true;
    }

    bool switchMode(ScreenshotScrollingRecognitionMode requestedMode) {
        if (!active || mode == requestedMode || canvasSelection.isEmpty() ||
            thumbnailHost == nullptr) {
            return false;
        }

        const CapturedDisplayModel* anchorDisplay = context.geometry.displayForCanvasPoint(
            context.displaySession, ScreenshotHalfOpenRect::fromRect(canvasSelection).center());
        if (anchorDisplay == nullptr) {
            anchorDisplay = context.geometry.displayForCanvasRect(context.displaySession,
                                                                  QRectF(canvasSelection));
        }
        if (anchorDisplay == nullptr) {
            return false;
        }

        // A direction change invalidates the axis-specific capture and stitch state, but the
        // selection, overlay presentation, and window exclusion must remain active. Advancing
        // the generation drops work from the previous axis without briefly restoring the canvas.
        mode = requestedMode;
        ++generation;
        autoScroller.setMode(mode);
        pendingResultRequestId.reset();
        if (pipeline)
            pipeline->reset(generation);
        latestOutputSize = {};
        cachedSnapshot = {};
        cachedSnapshotTop = -1;
        cachedSnapshotBottom = -1;
        cachedSnapshotGeneration = 0;

        const quint64 requestGeneration = generation;
        const QRect requestSelection = canvasSelection;
        const QRect requestPhysicalSelection =
            canvasSelection.translated(context.geometry.canvasOrigin());
        const AdaptiveScrollCadence::Config requestCadenceConfig = cadenceConfig;
        const QRect logicalSelection =
            logicalSelectionRect(context.geometry, *anchorDisplay, canvasSelection);
        thumbnailHost->beginScrollingThumbnail(
            logicalSelection.translated(-thumbnailHost->geometry().topLeft()), mode);

        pipeline->begin(requestGeneration, requestSelection.size(), mode,
                        nativeScrollingSource(requestPhysicalSelection, restoreOriginalColors),
                        requestCadenceConfig);
        return true;
    }

    void stop(bool restoreScreenshotPresentation) {
        autoScroller.stop();
        const bool wasActive = active;
        if (wasActive) {
            snow_shot::diagnostics::logEvent(
                QStringLiteral("snow_shot.scrolling"), QStringLiteral("scrolling.stopped"),
                {{QStringLiteral("operation"), QString::number(generation)}});
        }
        active = false;
        exportPaused = false;
        ++generation;
        pendingResultRequestId.reset();
        if (pipeline)
            pipeline->reset(generation);
        latestOutputSize = {};
        cachedSnapshot = {};
        cachedSnapshotTop = -1;
        cachedSnapshotBottom = -1;
        cachedSnapshotGeneration = 0;
        canvasSelection = {};

        if (wasActive) {
            context.overlayCoordinator.setScrollingCaptureMode(context.displaySession, QRectF(),
                                                               false);
            if (restoreScreenshotPresentation) {
                context.overlayCoordinator.applyDisplayModels(context.displaySession);
            }
        } else if (thumbnailHost != nullptr) {
            thumbnailHost->clearScrollingThumbnail();
        }
        restoreScrollingWindowsCaptureVisibility();
        thumbnailHost = nullptr;

        // Scrolling workers are session-scoped.  Tear them down after invalidating all
        // in-flight work so the stitch session and its native resources are released between
        // captures; the next start() recreates them on demand.
        shutdownWorker();
    }

    void detachPendingResultRequest() {
        if (pendingResultRequestId.has_value()) {
            detachedResultRequestIds.insert(*pendingResultRequestId);
            pendingResultRequestId.reset();
        }
    }

    bool excludeScrollingWindowsFromCapture(ScreenshotOverlayWindow* overlay) {
#if defined(Q_OS_WIN) || defined(_WIN32)
        ScreenshotToolbarWindow* const toolbar = context.overlayCoordinator.toolbar();
        if (QCoreApplication::arguments().contains(QStringLiteral("--e2e-allow-overlay-capture"))) {
            return overlay != nullptr && toolbar != nullptr;
        }
        if (overlay == nullptr || toolbar == nullptr) {
            return false;
        }
        captureExclusion.exclude(overlay);
        captureExclusion.exclude(toolbar);
#else
        Q_UNUSED(overlay);
#endif
        return true;
    }

    void restoreScrollingWindowsCaptureVisibility() {
        captureExclusion.restore();
    }

    void ensureWorker() {
        if (pipeline)
            return;
        pipeline = std::make_unique<ScreenshotScrollingPipeline>(
            [this](ScrollingPipelineFrame frame) { handleFrame(std::move(frame)); },
            [this](quint64 value, QString error) { handleCaptureError(value, std::move(error)); });
    }

    void shutdownWorker() {
        pipeline.reset();
    }

    void handleCaptureError(quint64 value, QString error) {
        if (!active || exportPaused || value != generation)
            return;
        qWarning("Scrolling capture stream failed: %s", qUtf8Printable(error));
        autoScroller.setPaused(true);
        ++generation;
        pendingResultRequestId.reset();
        pipeline->reset(generation);
    }

    void handleFrame(ScrollingPipelineFrame result) {
        if (!active || exportPaused || result.generation != generation)
            return;
        if (result.fatalError) {
            handleCaptureError(result.generation, QStringLiteral("scrolling stitching failed"));
            return;
        }
        if (!result.changed || result.sourceSize.isEmpty() || thumbnailHost == nullptr)
            return;
        thumbnailHost->updateScrollingThumbnail(result.previewImage, result.sourceSize,
                                                result.change, result.addedRows,
                                                result.previewReplaced, result.replacedPreviewRows);
        latestOutputSize = result.sourceSize;
        cachedSnapshot = {};
        cachedSnapshotTop = -1;
        cachedSnapshotBottom = -1;
        cachedSnapshotGeneration = 0;
    }

    QSize trimmedSize() const {
        if (!active || thumbnailHost == nullptr || latestOutputSize.isEmpty()) {
            return {};
        }
        const ScreenshotScrollingTrimRange trim = thumbnailHost->scrollingThumbnailTrim();
        if (!trim.isValid()) {
            return {};
        }
        const int extent = mode == ScreenshotScrollingRecognitionMode::Horizontal
                               ? latestOutputSize.width()
                               : latestOutputSize.height();
        const int top = std::clamp(trim.top, 0, extent - 1);
        const int bottom = std::clamp(trim.bottom, top + 1, extent);
        return mode == ScreenshotScrollingRecognitionMode::Horizontal
                   ? QSize(bottom - top, latestOutputSize.height())
                   : QSize(latestOutputSize.width(), bottom - top);
    }

    bool
    requestTrimmedSnapshot(ScreenshotScrollingCaptureController::SnapshotResultCallback callback) {
        if (!active || pipeline == nullptr || thumbnailHost == nullptr ||
            latestOutputSize.isEmpty() || !callback || pendingResultRequestId.has_value()) {
            return false;
        }
        const ScreenshotScrollingTrimRange trim = thumbnailHost->scrollingThumbnailTrim();
        if (!trim.isValid()) {
            return false;
        }
        if (cachedSnapshot.isValid() && cachedSnapshotGeneration == generation &&
            cachedSnapshotTop == trim.top && cachedSnapshotBottom == trim.bottom) {
            SNOW_SHOT_PIN_PERF_COUNTER("scrolling.snapshot_cache_hit", 1);
            const QPointer<ScreenshotScrollingCaptureController> receiver(&owner);
            QTimer::singleShot(
                0, &owner,
                [receiver, cached = cachedSnapshot, callback = std::move(callback)]() mutable {
                    if (!receiver.isNull() && receiver->m_impl != nullptr &&
                        receiver->m_impl->active) {
                        callback(std::move(cached));
                    }
                });
            return true;
        }
        const quint64 requestGeneration = generation;
        const quint64 requestId = ++nextResultRequestId;
        pendingResultRequestId = requestId;
        const QPointer<ScreenshotScrollingCaptureController> receiver(&owner);
        const bool invoked = pipeline->requestSnapshot(
            trim.top, trim.bottom, &owner,
            [receiver, requestId, requestGeneration, trim,
             callback = std::move(callback)](ScreenshotScrollingSnapshot result) mutable {
                if (receiver.isNull() || receiver->m_impl == nullptr ||
                    (receiver->m_impl->pendingResultRequestId != requestId &&
                     !receiver->m_impl->detachedResultRequestIds.contains(requestId))) {
                    return;
                }
                const bool detached =
                    receiver->m_impl->detachedResultRequestIds.contains(requestId);
                if (detached) {
                    receiver->m_impl->detachedResultRequestIds.remove(requestId);
                } else {
                    receiver->m_impl->pendingResultRequestId.reset();
                }
                if (!detached && (!receiver->m_impl->active ||
                                  receiver->m_impl->generation != requestGeneration)) {
                    return;
                }
                if (result.isValid()) {
                    receiver->m_impl->cachedSnapshot = result;
                    receiver->m_impl->cachedSnapshotTop = trim.top;
                    receiver->m_impl->cachedSnapshotBottom = trim.bottom;
                    receiver->m_impl->cachedSnapshotGeneration = requestGeneration;
                }
                callback(std::move(result));
            });
        if (!invoked && pendingResultRequestId == requestId) {
            pendingResultRequestId.reset();
        }
        return invoked;
    }

    void setExportPaused(bool paused) {
        if (!active || exportPaused == paused)
            return;
        exportPaused = paused;
        autoScroller.setPaused(paused);
        if (paused) {
            pipeline->pause(generation);
        } else {
            pipeline->resume(
                generation, canvasSelection.size(),
                nativeScrollingSource(canvasSelection.translated(context.geometry.canvasOrigin()),
                                      restoreOriginalColors),
                cadenceConfig);
        }
    }

    ScreenshotScrollingCaptureController& owner;
    snow_shot::capture_detail::ScreenshotScrollingAutoScroller autoScroller{
        snow_shot::platform::windows::sendScrollingWheelStep};
    bool exportPaused = false;
    ScreenshotScrollingCaptureControllerContext context;
    AdaptiveScrollCadence::Config cadenceConfig;
    bool restoreOriginalColors = false;
    std::unique_ptr<ScreenshotScrollingPipeline> pipeline;
    QPointer<ScreenshotOverlayWindow> thumbnailHost;
    snow_shot::presentation::WindowCaptureExclusion captureExclusion{
#if defined(Q_OS_WIN) || defined(_WIN32)
        snow_shot::platform::windows::setWindowExcludedFromCapture
#endif
    };
    QSize latestOutputSize;
    ScreenshotScrollingSnapshot cachedSnapshot;
    int cachedSnapshotTop = -1;
    int cachedSnapshotBottom = -1;
    quint64 cachedSnapshotGeneration = 0;
    std::optional<quint64> pendingResultRequestId;
    QSet<quint64> detachedResultRequestIds;
    QRect canvasSelection;
    quint64 generation = 0;
    quint64 nextResultRequestId = 0;
    bool active = false;
    ScreenshotScrollingRecognitionMode mode = ScreenshotScrollingRecognitionMode::Vertical;
};

ScreenshotScrollingCaptureController::ScreenshotScrollingCaptureController(
    ScreenshotScrollingCaptureControllerContext context, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, context)) {}

ScreenshotScrollingCaptureController::~ScreenshotScrollingCaptureController() = default;

bool ScreenshotScrollingCaptureController::start(const QRect& canvasSelection,
                                                 ScreenshotScrollingRecognitionMode mode) {
    return m_impl->start(canvasSelection, mode);
}

bool ScreenshotScrollingCaptureController::setRecognitionMode(
    ScreenshotScrollingRecognitionMode mode) {
    if (m_impl->mode == mode) {
        return false;
    }
    if (!m_impl->active) {
        m_impl->mode = mode;
        return true;
    }
    return m_impl->switchMode(mode);
}

ScreenshotScrollingRecognitionMode ScreenshotScrollingCaptureController::recognitionMode() const {
    return m_impl->mode;
}

void ScreenshotScrollingCaptureController::stop(bool restoreScreenshotPresentation) {
    m_impl->stop(restoreScreenshotPresentation);
}

bool ScreenshotScrollingCaptureController::active() const {
    return m_impl->active;
}

QSize ScreenshotScrollingCaptureController::trimmedSize() const {
    return m_impl->trimmedSize();
}

bool ScreenshotScrollingCaptureController::requestTrimmedSnapshot(SnapshotResultCallback callback) {
    return m_impl->requestTrimmedSnapshot(std::move(callback));
}

void ScreenshotScrollingCaptureController::setExportPaused(bool paused) {
    m_impl->setExportPaused(paused);
}

void ScreenshotScrollingCaptureController::setAutoScroll(bool enabled) {
    m_impl->autoScroller.setEnabled(enabled && m_impl->active);
}

void ScreenshotScrollingCaptureController::detachPendingResultRequest() {
    m_impl->detachPendingResultRequest();
}

QRect ScreenshotScrollingCaptureController::canvasSelection() const {
    return m_impl->canvasSelection;
}
