#include "snow_shot/presentation/screenshotscrollingcapturecontroller.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"

#if defined(Q_OS_WIN) || defined(_WIN32) || defined(Q_OS_MACOS)
#include "snow_shot/platform/windowcaptureexclusion.h"
#if defined(Q_OS_WIN) || defined(_WIN32)
#include "snow_shot/platform/windows/windowchrome.h"
#include <qt_windows.h>
#endif
#endif

#include "adaptivescrollingcapturecadence.h"
#include "screenshotscrollingautoscroller.h"
#include "scrollingselectionmovement.h"
#include "scrollingstepinput.h"
#include "scrollingsnapshotrequest.h"
#include "scrollinghoverpreview.h"
#include "snow_shot/platform/screenshotnative.h"
#include "screenshotscrollingpipeline.h"
#include "screenshotscrollingdiagnostics.h"
#include <QElapsedTimer>
#include "windowcaptureexclusion.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QJsonArray>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace {
using snow_shot::capture_detail::logScrollingEvent;
using snow_shot::capture_detail::nativeScrollingSource;
using snow_shot::capture_detail::ScreenshotScrollingPipeline;
using snow_shot::capture_detail::ScrollingPipelineFrame;
using snow_shot::capture_detail::scrollingRect;
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
        : owner(ownerValue), context(contextValue),
          hoverPreview({
              [this](std::function<void()> acknowledged) {
                  hoverPaused = true;
                  updatePausedState(std::move(acknowledged));
              },
              [this](const QRect& rect, std::function<void(QImage)> completed) {
                  if (!active || !pipeline || rect.size() != viewportPixelSize)
                      return false;
                  const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
                  if ((horizontal ? rect.y() : rect.x()) != 0)
                      return false;
                  const int start = horizontal ? rect.x() : rect.y();
                  const int extent = horizontal ? rect.width() : rect.height();
                  return pipeline->requestViewportPreview(start, start + extent, &owner,
                                                          std::move(completed));
              },
              [this](const QImage& image, bool cropping) {
                  const std::optional<Qt::Orientation> cropGuide =
                      cropping
                          ? std::optional(mode == ScreenshotScrollingRecognitionMode::Horizontal
                                              ? Qt::Vertical
                                              : Qt::Horizontal)
                          : std::nullopt;
                  context.overlayCoordinator.setScrollingResultPreview(
                      context.displaySession, image, QRectF(canvasSelection), cropGuide);
                  hoverPreviewVisible = true;
              },
              [this] { clearHoverPresentation(); },
              [this] {
                  hoverPaused = false;
                  updatePausedState();
              },
          }) {
        previewWatchdog.setSingleShot(true);
        QObject::connect(&previewWatchdog, &QTimer::timeout, &owner, [this] {
            if (active && !exportPaused && !movement.active() && !previewReceived) {
                logScrollingEvent(
                    "scrolling.preview_timeout", generation,
                    {{QStringLiteral("duration_ms"), previewClock.elapsed()},
                     {QStringLiteral("stage"), QStringLiteral("waiting_for_first_preview")}},
                    QtWarningMsg);
                logPreparation();
            }
        });
    }

    void logPreparation() const {
#if defined(Q_OS_WIN) || defined(_WIN32)
        const QPoint center = canvasSelection.translated(context.geometry.canvasOrigin()).center();
        const HWND target = WindowFromPoint(POINT{center.x(), center.y()});
        DWORD targetProcess = 0;
        GetWindowThreadProcessId(target, &targetProcess);
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        logScrollingEvent(
            "scrolling.input_target", generation,
            {{QStringLiteral("target_found"), target != nullptr},
             {QStringLiteral("target_is_self"), targetProcess == GetCurrentProcessId()},
             {QStringLiteral("foreground_is_self"), foregroundProcess == GetCurrentProcessId()}});
#endif
        context.displaySession.forEachActiveOverlay(
            [this](qsizetype index, const CapturedDisplayModel&, ScreenshotOverlayWindow* overlay) {
                if (overlay == nullptr)
                    return;
                auto fields = overlay->scrollingDiagnostics();
                fields.insert(QStringLiteral("display_index"), static_cast<qint64>(index));
                logScrollingEvent("scrolling.input_state", generation, fields);
            });
    }

    void watchPreview() {
        previewReceived = false;
        previewClock.start();
        previewWatchdog.start(5000);
    }

    ~Impl() {
        stop(false);
    }

    bool start(QRect selection, ScreenshotScrollingRecognitionMode requestedMode) {
        if (selection.width() < 1 || selection.height() < 1) {
            logScrollingEvent("scrolling.start_rejected", generation,
                              {{QStringLiteral("reason"), QStringLiteral("empty_selection")}},
                              QtWarningMsg);
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
            logScrollingEvent(
                "scrolling.start_rejected", generation,
                {{QStringLiteral("reason"), QStringLiteral("missing_display_or_overlay")}},
                QtWarningMsg);
            return false;
        }

        if (active) {
            stop(false);
        }
        ensureWorker();
        if (pipeline == nullptr) {
            return false;
        }

        const auto renderSpec = screenshotSelectionRenderSpec(context.displaySession, selection);
        if (!renderSpec.isValid())
            return false;
        const QRect logicalSelection =
            logicalSelectionRect(context.geometry, *anchorDisplay, selection);
        if (!context.presentationSuppressed())
            anchorOverlay->beginScrollingThumbnail(
                logicalSelection.translated(-anchorOverlay->captureGeometry().topLeft()),
                requestedMode, renderSpec.pixelSize);

        exclusionGeneration = generation + 1;
        if (!excludeScrollingWindowsFromCapture(anchorOverlay)) {
            anchorOverlay->clearScrollingThumbnail();
            return false;
        }

        viewportPixelSize = renderSpec.pixelSize;
        sourceScale = renderSpec.scale;
        canvasSelection = selection;
        restoreOriginalColors = context.restoreOriginalScreenColors();
        mode = requestedMode;
        thumbnailHost = anchorOverlay;
        QObject::disconnect(hoverConnection);
        hoverConnection = QObject::connect(thumbnailHost,
                                           &ScreenshotOverlayWindow::scrollingThumbnailHoverChanged,
                                           &owner, [this](const QRect& rect, bool cropping) {
                                               hoverPreview.setHoverRect(rect, cropping);
                                           });
        thumbnailHost->setScrollingTrimModel(trimRange);
        active = true;
        emit owner.stateChanged();
        ++generation;
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.scrolling"), QStringLiteral("scrolling.started"),
            {{QStringLiteral("operation"), QString::number(generation)}});
        watchPreview();
        logScrollingEvent("scrolling.preparing", generation,
                          {{QStringLiteral("selection"), scrollingRect(selection)},
                           {QStringLiteral("physical_selection"),
                            scrollingRect(selection.translated(context.geometry.canvasOrigin()))},
                           {QStringLiteral("mode"), static_cast<int>(mode)},
                           {QStringLiteral("restore_colors"), restoreOriginalColors}});
        snapshotRequest.cancel();
        if (pipeline)
            pipeline->reset(generation);
        capturePaused = false;
        pipelineInitialized = false;

        context.overlayCoordinator.setScrollingCaptureMode(context.displaySession,
                                                           QRectF(canvasSelection), true);

        logPreparation();
        logScrollingEvent("scrolling.prepared", generation);
        const QRect requestPhysicalSelection =
            canvasSelection.translated(context.geometry.canvasOrigin());
        autoScroller.start(requestPhysicalSelection, mode);
        beginPipelineAfterPresentationClear();
        refreshHoverEligibility();
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
        movement.end();
        hoverPreview.reset();
        mode = requestedMode;
        ++generation;
        watchPreview();
        logScrollingEvent("scrolling.mode_changed", generation,
                          {{QStringLiteral("mode"), static_cast<int>(mode)}});
        autoScroller.setMode(mode);
        autoScroller.setPaused(exportPaused);
        snapshotRequest.cancel();
        if (pipeline)
            pipeline->reset(generation);
        capturePaused = false;
        pipelineInitialized = false;
        latestOutputSize = {};
        emit owner.stateChanged();
        *trimRange = {};
        cachedSnapshot = {};
        cachedSnapshotTop = -1;
        cachedSnapshotBottom = -1;
        cachedSnapshotGeneration = 0;

        const QRect logicalSelection =
            logicalSelectionRect(context.geometry, *anchorDisplay, canvasSelection);
        if (!context.presentationSuppressed())
            thumbnailHost->beginScrollingThumbnail(
                logicalSelection.translated(-thumbnailHost->captureGeometry().topLeft()), mode,
                viewportPixelSize);

        beginPipelineAfterPresentationClear();
        refreshHoverEligibility();
        return true;
    }

    void stop(bool restoreScreenshotPresentation) {
        previewWatchdog.stop();
        autoScrollEnabled = false;
        autoScroller.stop();
        const bool wasActive = active;
        if (wasActive) {
            snow_shot::diagnostics::logEvent(
                QStringLiteral("snow_shot.scrolling"), QStringLiteral("scrolling.stopped"),
                {{QStringLiteral("operation"), QString::number(generation)},
                 {QStringLiteral("preview_received"), previewReceived},
                 {QStringLiteral("duration_ms"),
                  previewClock.isValid() ? previewClock.elapsed() : 0},
                 {QStringLiteral("width"), latestOutputSize.width()},
                 {QStringLiteral("height"), latestOutputSize.height()},
                 {QStringLiteral("restore_presentation"), restoreScreenshotPresentation}});
        }
        active = false;
        QObject::disconnect(hoverConnection);
        hoverPreview.reset();
        clearHoverPresentation();
        ++pauseTransition;
        emit owner.stateChanged();
        movement.end();
        exportPaused = false;
        capturePaused = false;
        pipelineInitialized = false;
        ++generation;
        snapshotRequest.cancel();
        if (pipeline)
            pipeline->reset(generation);
        latestOutputSize = {};
        *trimRange = {};
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
        thumbnailHost = nullptr;

        // Scrolling workers are session-scoped.  Tear them down after invalidating all
        // in-flight work so the stitch session and its native resources are released between
        // captures; the next start() recreates them on demand.
        shutdownWorker();
        restoreScrollingWindowsCaptureVisibility();
    }

    void detachPendingResultRequest() {
        snapshotRequest.detach();
    }

    bool excludeScrollingWindowsFromCapture(ScreenshotOverlayWindow* overlay) {
#if defined(Q_OS_WIN) || defined(_WIN32) || defined(Q_OS_MACOS)
        ScreenshotToolbarWindow* const toolbar = context.overlayCoordinator.toolbar();
        if (context.captureUiInScrollingScreenshot() ||
            QCoreApplication::arguments().contains(QStringLiteral("--e2e-allow-overlay-capture"))) {
            return overlay != nullptr && toolbar != nullptr;
        }
        if (overlay == nullptr || toolbar == nullptr) {
            return false;
        }
        captureExclusion.exclude(overlay);
        captureExclusion.exclude(overlay->scrollingThumbnailWindow());
        captureExclusion.exclude(toolbar);
        exclusionWindowIds = captureExclusion.windowIds(snow_shot::platform::captureWindowId);
#else
        Q_UNUSED(overlay);
#endif
        return true;
    }

    void restoreScrollingWindowsCaptureVisibility() {
        captureExclusion.restore();
        exclusionWindowIds.clear();
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
        previewWatchdog.stop();
        logScrollingEvent("scrolling.failed", generation,
                          {{QStringLiteral("stage"), QStringLiteral("capture_or_stitch")},
                           {QStringLiteral("preview_received"), previewReceived}},
                          QtWarningMsg);
        qWarning("Scrolling capture stream failed: %s", qUtf8Printable(error));
        autoScroller.setPaused(true);
        ++generation;
        snapshotRequest.cancel();
        pipeline->reset(generation);
        // Leave the pipeline's error callback before destroying it. stop() joins
        // capture before restoring native sharing policies, including failed starts.
        QMetaObject::invokeMethod(
            &owner,
            [this, failedGeneration = generation]() {
                if (active && generation == failedGeneration) {
                    stop(true);
                    if (context.captureFailed)
                        context.captureFailed();
                }
            },
            Qt::QueuedConnection);
    }

    void handleFrame(ScrollingPipelineFrame result) {
        if (!active || result.generation != generation)
            return;
        if (result.fatalError) {
            handleCaptureError(result.generation, QStringLiteral("scrolling stitching failed"));
            return;
        }
        if (!result.changed || result.sourceSize.isEmpty())
            return;
        ++contentRevision;
        latestOutputSize = result.sourceSize;
        hoverPreview.contentChanged();
        const int extent = mode == ScreenshotScrollingRecognitionMode::Horizontal
                               ? result.sourceSize.width()
                               : result.sourceSize.height();
        if (context.presentationSuppressed()) {
            if (!trimRange->isValid() ||
                result.change == ScreenshotScrollingStitchChange::Replaced ||
                result.change == ScreenshotScrollingStitchChange::Initial)
                *trimRange = {0, extent};
            else if (result.change == ScreenshotScrollingStitchChange::PrependedUp ||
                     result.change == ScreenshotScrollingStitchChange::PrependedLeft)
                *trimRange = {0,
                              std::min(extent, trimRange->bottom + std::max(0, result.addedRows))};
            else
                trimRange->bottom = extent;
        }
        if (thumbnailHost && !context.presentationSuppressed())
            thumbnailHost->updateScrollingThumbnail(
                result.previewImage, result.sourceSize, result.change, result.addedRows,
                result.previewReplaced, result.replacedPreviewRows);
        if (!previewReceived) {
            previewReceived = true;
            previewWatchdog.stop();
            auto fields = thumbnailHost ? thumbnailHost->scrollingDiagnostics() : QJsonObject{};
            fields.insert(QStringLiteral("duration_ms"), previewClock.elapsed());
            fields.insert(QStringLiteral("width"), result.sourceSize.width());
            fields.insert(QStringLiteral("height"), result.sourceSize.height());
            logScrollingEvent("scrolling.first_preview", generation, fields);
        }
        refreshHoverEligibility();
        emit owner.stateChanged();
        cachedSnapshot = {};
        cachedSnapshotTop = -1;
        cachedSnapshotBottom = -1;
        cachedSnapshotGeneration = 0;
    }

    ScreenshotScrollingTrimRange currentTrim() const {
        return *trimRange;
    }
    QSize trimmedSize() const {
        if (!active || thumbnailHost == nullptr || latestOutputSize.isEmpty()) {
            return {};
        }
        const ScreenshotScrollingTrimRange trim = currentTrim();
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
            latestOutputSize.isEmpty() || !callback || snapshotRequest.pending()) {
            return false;
        }
        const ScreenshotScrollingTrimRange trim = currentTrim();
        if (!trim.isValid()) {
            return false;
        }
        const quint64 requestGeneration = generation;
        const QPointer<ScreenshotScrollingCaptureController> receiver(&owner);
        auto completion = snapshotRequest.begin(
            [receiver, requestGeneration, trim,
             callback = std::move(callback)](ScreenshotScrollingSnapshot result) mutable {
                if (receiver.isNull() || receiver->m_impl == nullptr)
                    return;
                auto& impl = *receiver->m_impl;
                if (result.isValid() && impl.active && impl.generation == requestGeneration) {
                    impl.cachedSnapshot = result;
                    impl.cachedSnapshotTop = trim.top;
                    impl.cachedSnapshotBottom = trim.bottom;
                    impl.cachedSnapshotGeneration = requestGeneration;
                }
                callback(std::move(result));
            });
        if (cachedSnapshot.isValid() && cachedSnapshotGeneration == generation &&
            cachedSnapshotTop == trim.top && cachedSnapshotBottom == trim.bottom) {
            SNOW_SHOT_PIN_PERF_COUNTER("scrolling.snapshot_cache_hit", 1);
            QTimer::singleShot(
                0, &owner, [cached = cachedSnapshot, completion = std::move(completion)]() mutable {
                    completion(std::move(cached));
                });
            return true;
        }
        const bool invoked =
            pipeline->requestSnapshot(trim.top, trim.bottom, &owner, std::move(completion));
        if (!invoked)
            snapshotRequest.cancel();
        return invoked;
    }

    bool beginSelectionMove(ScreenshotScrollingRecognitionMode axis, QPoint pointer) {
        if (!active || exportPaused || !movement.begin(axis, mode, canvasSelection, pointer))
            return false;
        refreshHoverEligibility();
        updatePausedState();
        return true;
    }

    void updateSelectionMove(QPoint pointer) {
        if (!active || !movement.active())
            return;
        canvasSelection = movement.update(pointer, context.geometry.canvasBounds().toAlignedRect());
        context.overlayCoordinator.setScrollingCaptureMode(context.displaySession, canvasSelection,
                                                           true);
        autoScroller.setSelection(canvasSelection.translated(context.geometry.canvasOrigin()));
        context.displaySession.forEachActiveOverlay([this](qsizetype,
                                                           const CapturedDisplayModel& display,
                                                           ScreenshotOverlayWindow* overlay) {
            if (overlay == thumbnailHost) {
                thumbnailHost->reanchorScrollingThumbnail(
                    logicalSelectionRect(context.geometry, display, canvasSelection)
                        .translated(-thumbnailHost->captureGeometry().topLeft()));
            }
        });
    }

    void endSelectionMove() {
        if (!movement.active())
            return;
        movement.end();
        if (active) {
            refreshHoverEligibility();
            updatePausedState();
        }
    }

    void setExportPaused(bool paused) {
        if (!active || exportPaused == paused)
            return;
        exportPaused = paused;
        refreshHoverEligibility();
        logScrollingEvent("scrolling.export_pause", generation,
                          {{QStringLiteral("status"), paused}});
        updatePausedState();
    }

    void refreshHoverEligibility() {
        hoverPreview.setEnabled(active && !exportPaused && !movement.active() &&
                                    !latestOutputSize.isEmpty() &&
                                    !context.presentationSuppressed(),
                                !autoScrollEnabled);
    }

    void clearHoverPresentation() {
        if (!hoverPreviewVisible)
            return;
        context.overlayCoordinator.clearScrollingResultPreview(context.displaySession);
        hoverPreviewVisible = false;
        presentationFlushPending = true;
    }

    void afterPresentationClear(std::function<void()> action, std::function<bool()> valid,
                                int attempt = 0) {
        if (!valid())
            return;
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (presentationFlushPending && !snow_shot::platform::windows::flushWindowComposition()) {
            if (attempt == 0)
                qWarning("Waiting for scrolling result preview removal before capture resumes");
            // Keep the stitched result and source stopped until native presentation catches up.
            // Back off without blocking input; a new hover or session cancels this continuation.
            QTimer::singleShot(
                std::min(1000, 50 * (1 << std::min(attempt, 5))), &owner,
                [this, action = std::move(action), valid = std::move(valid), attempt]() mutable {
                    afterPresentationClear(std::move(action), std::move(valid), attempt + 1);
                });
            return;
        }
#else
        Q_UNUSED(attempt);
#endif
        presentationFlushPending = false;
        action();
    }

    void beginPipelineAfterPresentationClear() {
        autoScroller.setPaused(true);
        afterPresentationClear(
            [this] {
                // Input may move the selection or change pause reasons while composition catches
                // up. Initialize once from the current selection, then reconcile every reason.
                pipeline->begin(generation, viewportPixelSize, mode,
                                nativeScrollingSource(
                                    canvasSelection.translated(context.geometry.canvasOrigin()),
                                    restoreOriginalColors, exclusionWindowIds, generation),
                                cadenceConfig);
                pipelineInitialized = true;
                capturePaused = false;
                updatePausedState();
                if (!capturePaused)
                    autoScroller.setPaused(false);
            },
            [this, session = generation] {
                return active && generation == session && !pipelineInitialized;
            });
    }

    void updatePausedState(std::function<void()> acknowledged = {}) {
        if (!active || !pipeline)
            return;
        if (!pipelineInitialized) {
            autoScroller.setPaused(true);
            return;
        }
        const bool paused = exportPaused || movement.active() || hoverPaused;
        if (paused == capturePaused && !acknowledged)
            return;
        capturePaused = paused;
        const quint64 transition = ++pauseTransition;
        if (paused)
            previewWatchdog.stop();
        else if (!previewReceived)
            watchPreview();
        autoScroller.setPaused(true);
        if (paused) {
            pipeline->pause(generation, std::move(acknowledged));
        } else {
            QTimer::singleShot(0, &owner, [this, transition, session = generation] {
                afterPresentationClear(
                    [this] {
                        pipeline->resume(
                            generation, viewportPixelSize,
                            nativeScrollingSource(
                                canvasSelection.translated(context.geometry.canvasOrigin()),
                                restoreOriginalColors, exclusionWindowIds, generation),
                            cadenceConfig);
                        autoScroller.setPaused(false);
                    },
                    [this, transition, session] {
                        return active && generation == session && pauseTransition == transition &&
                               !exportPaused && !movement.active() && !hoverPaused;
                    });
            });
        }
    }

    snow_shot::capture_detail::ScrollingSelectionMovement movement;
    ScreenshotScrollingCaptureController& owner;
    snow_shot::capture_detail::ScreenshotScrollingAutoScroller autoScroller{
        [this](const QRect& selection, const QPoint& delta) {
            if (!snow_shot::platform::screenshotScrollPermission()) {
                autoScroller.setEnabled(false);
                return;
            }
            const auto result = snow_shot::platform::sendScreenshotScroll(selection, delta);
#ifdef Q_OS_MACOS
            if (result.status != snow_shot::platform::ScrollInputResult::Status::Posted) {
                handleCaptureError(generation, QStringLiteral("automatic scroll dispatch failed"));
            }
#endif
            const int status = static_cast<int>(result.status);
            if (status != lastScrollStatus || result.error != lastScrollError) {
                lastScrollStatus = status;
                lastScrollError = result.error;
                logScrollingEvent("scrolling.wheel_dispatch", generation,
                                  {{QStringLiteral("status"), status},
                                   {QStringLiteral("code"), static_cast<qint64>(result.error)}},
                                  result.status ==
                                          snow_shot::platform::ScrollInputResult::Status::Posted
                                      ? QtInfoMsg
                                      : QtWarningMsg);
            }
        }};
    int lastScrollStatus = -1;
    quint32 lastScrollError = 0;
    QTimer previewWatchdog;
    QElapsedTimer previewClock;
    bool previewReceived = false;
    bool exportPaused = false;
    bool autoScrollEnabled = false;
    bool hoverPaused = false;
    bool capturePaused = false;
    bool pipelineInitialized = false;
    bool hoverPreviewVisible = false;
    bool presentationFlushPending = false;
    quint64 pauseTransition = 0;
    QMetaObject::Connection hoverConnection;
    std::shared_ptr<ScreenshotScrollingTrimRange> trimRange =
        std::make_shared<ScreenshotScrollingTrimRange>();
    quint64 contentRevision = 0;
    ScreenshotScrollingCaptureControllerContext context;
    AdaptiveScrollCadence::Config cadenceConfig;
    bool restoreOriginalColors = false;
    std::unique_ptr<ScreenshotScrollingPipeline> pipeline;
    QPointer<ScreenshotOverlayWindow> thumbnailHost;
    QVector<std::uint32_t> exclusionWindowIds;
    snow_shot::presentation::WindowCaptureExclusion captureExclusion{
#if defined(Q_OS_WIN) || defined(_WIN32) || defined(Q_OS_MACOS)
        [this](QWidget* window, bool excluded) {
#if defined(Q_OS_WIN) || defined(_WIN32)
            SetLastError(ERROR_SUCCESS);
            const bool succeeded =
                snow_shot::platform::setWindowExcludedFromCapture(window, excluded);
            const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
#else
            const bool succeeded =
                snow_shot::platform::setWindowExcludedFromCapture(window, excluded);
            const qint64 error = 0;
#endif
            logScrollingEvent("scrolling.window_exclusion", exclusionGeneration,
                              {{QStringLiteral("status"), excluded},
                               {QStringLiteral("outcome"),
                                succeeded ? QStringLiteral("succeeded") : QStringLiteral("failed")},
                               {QStringLiteral("code"), static_cast<qint64>(error)}},
                              succeeded ? QtInfoMsg : QtWarningMsg);
            return succeeded;
        }
#endif
    };
    quint64 exclusionGeneration = 0;
    QSize latestOutputSize;
    ScreenshotScrollingSnapshot cachedSnapshot;
    int cachedSnapshotTop = -1;
    int cachedSnapshotBottom = -1;
    quint64 cachedSnapshotGeneration = 0;
    snow_shot::capture_detail::ScrollingSnapshotRequest snapshotRequest;
    QRect canvasSelection;
    QSize viewportPixelSize;
    qreal sourceScale = 1.;
    quint64 generation = 0;
    bool active = false;
    ScreenshotScrollingRecognitionMode mode = ScreenshotScrollingRecognitionMode::Vertical;
    snow_shot::capture_detail::ScrollingHoverPreview hoverPreview;
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

qreal ScreenshotScrollingCaptureController::sourceScale() const {
    return m_impl->sourceScale;
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

void ScreenshotScrollingCaptureController::setAutoScrollIntervalMs(int milliseconds) {
    m_impl->autoScroller.setIntervalMs(milliseconds);
}

void ScreenshotScrollingCaptureController::setAutoScroll(bool enabled) {
    logScrollingEvent("scrolling.auto_scroll", m_impl->generation,
                      {{QStringLiteral("status"), enabled && m_impl->active}});
    m_impl->lastScrollStatus = -1;
    m_impl->autoScrollEnabled = enabled && m_impl->active;
    m_impl->autoScroller.setEnabled(m_impl->autoScrollEnabled);
    m_impl->refreshHoverEligibility();
}

void ScreenshotScrollingCaptureController::detachPendingResultRequest() {
    m_impl->detachPendingResultRequest();
}

QRect ScreenshotScrollingCaptureController::canvasSelection() const {
    return m_impl->canvasSelection;
}

bool ScreenshotScrollingCaptureController::beginSelectionMove(
    ScreenshotScrollingRecognitionMode axis, QPoint physicalPointer) {
    return m_impl->beginSelectionMove(axis, physicalPointer);
}
void ScreenshotScrollingCaptureController::updateSelectionMove(QPoint physicalPointer) {
    m_impl->updateSelectionMove(physicalPointer);
}
void ScreenshotScrollingCaptureController::endSelectionMove() {
    m_impl->endSelectionMove();
}
bool ScreenshotScrollingCaptureController::movingSelection() const {
    return m_impl->movement.active();
}

QJsonObject ScreenshotScrollingCaptureController::state() const {
    const auto& s = *m_impl;
    const auto trim = s.currentTrim();
    return {{QStringLiteral("active"), s.active},
            {QStringLiteral("axis"), s.mode == ScreenshotScrollingRecognitionMode::Horizontal
                                         ? QStringLiteral("horizontal")
                                         : QStringLiteral("vertical")},
            {QStringLiteral("auto_scroll"), s.autoScrollEnabled},
            {QStringLiteral("ready"), !s.latestOutputSize.isEmpty()},
            {QStringLiteral("content_revision"), static_cast<qint64>(s.contentRevision)},
            {QStringLiteral("width"), s.latestOutputSize.width()},
            {QStringLiteral("height"), s.latestOutputSize.height()},
            {QStringLiteral("trim"), QJsonArray{trim.top, trim.bottom}}};
}
bool ScreenshotScrollingCaptureController::setTrimRange(int start, int end) {
    auto& s = *m_impl;
    const int extent = s.mode == ScreenshotScrollingRecognitionMode::Horizontal
                           ? s.latestOutputSize.width()
                           : s.latestOutputSize.height();
    if (!s.active || s.exportPaused || start < 0 || end <= start || end > extent)
        return false;
    *s.trimRange = {start, end};
    ++s.contentRevision;
    if (s.thumbnailHost)
        s.thumbnailHost->setScrollingTrimModel(s.trimRange);
    return true;
}
bool ScreenshotScrollingCaptureController::moveSelection(QPoint offset) {
    auto& s = *m_impl;
    if (!s.beginSelectionMove(s.mode, {}))
        return false;
    s.updateSelectionMove(offset);
    s.endSelectionMove();
    return true;
}
QJsonObject ScreenshotScrollingCaptureController::scrollOnce(const QString& direction,
                                                             QString* error) {
    const auto& s = *m_impl;
    const auto delta = snow_shot::capture_detail::scrollingStepDelta(direction);
    if (!delta || (delta->y() != 0) != (s.mode == ScreenshotScrollingRecognitionMode::Vertical)) {
        *error = QStringLiteral("invalid_direction");
        return {};
    }
    if (!s.active || s.latestOutputSize.isEmpty()) {
        *error = QStringLiteral("scrolling_not_ready");
        return {};
    }
    if (s.autoScrollEnabled || s.exportPaused || s.movement.active() || s.hoverPaused) {
        *error = QStringLiteral("busy");
        return {};
    }
    if (!snow_shot::platform::screenshotScrollPermission()) {
        *error = QStringLiteral("permission_required");
        return {};
    }
    const auto input = snow_shot::platform::sendScreenshotScroll(
        s.canvasSelection.translated(s.context.geometry.canvasOrigin()), *delta);
    if (input.status != snow_shot::platform::ScrollInputResult::Status::Posted) {
        *error = input.status == snow_shot::platform::ScrollInputResult::Status::TargetNotFound
                     ? QStringLiteral("target_not_found")
                     : QStringLiteral("scroll_dispatch_failed");
        return {};
    }
    error->clear();
    return {{QStringLiteral("direction"), direction},
            {QStringLiteral("dispatch_status"), QStringLiteral("posted")}};
}
