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
#include "scrollingstepbarrier.h"
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
#include <QSet>
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
        : owner(ownerValue), context(contextValue) {
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

        exclusionGeneration = generation + 1;
        if (!excludeScrollingWindowsFromCapture(anchorOverlay)) {
            return false;
        }

        const auto renderSpec = screenshotSelectionRenderSpec(context.displaySession, selection);
        if (!renderSpec.isValid())
            return false;
        viewportPixelSize = renderSpec.pixelSize;
        sourceScale = renderSpec.scale;
        canvasSelection = selection;
        restoreOriginalColors = context.restoreOriginalScreenColors();
        mode = requestedMode;
        thumbnailHost = anchorOverlay;
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
        pendingResultRequestId.reset();
        if (pipeline)
            pipeline->reset(generation);

        context.overlayCoordinator.setScrollingCaptureMode(context.displaySession,
                                                           QRectF(canvasSelection), true);

        const QRect logicalSelection =
            logicalSelectionRect(context.geometry, *anchorDisplay, canvasSelection);
        if (!context.presentationSuppressed())
            thumbnailHost->beginScrollingThumbnail(
                logicalSelection.translated(-thumbnailHost->geometry().topLeft()), mode);

        logPreparation();
        logScrollingEvent("scrolling.prepared", generation);
        const quint64 requestGeneration = generation;
        const QRect requestPhysicalSelection =
            canvasSelection.translated(context.geometry.canvasOrigin());
        autoScroller.start(requestPhysicalSelection, mode);
        const AdaptiveScrollCadence::Config requestCadenceConfig = cadenceConfig;
        pipeline->begin(requestGeneration, viewportPixelSize, mode,
                        nativeScrollingSource(requestPhysicalSelection, restoreOriginalColors,
                                              exclusionWindowIds, generation),
                        requestCadenceConfig);
        if (exportPaused)
            updatePausedState();
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
        mode = requestedMode;
        ++generation;
        watchPreview();
        logScrollingEvent("scrolling.mode_changed", generation,
                          {{QStringLiteral("mode"), static_cast<int>(mode)}});
        autoScroller.setMode(mode);
        autoScroller.setPaused(exportPaused);
        pendingResultRequestId.reset();
        if (pipeline)
            pipeline->reset(generation);
        latestOutputSize = {};
        emit owner.stateChanged();
        *trimRange = {};
        cachedSnapshot = {};
        cachedSnapshotTop = -1;
        cachedSnapshotBottom = -1;
        cachedSnapshotGeneration = 0;

        const quint64 requestGeneration = generation;
        const QRect requestPhysicalSelection =
            canvasSelection.translated(context.geometry.canvasOrigin());
        const AdaptiveScrollCadence::Config requestCadenceConfig = cadenceConfig;
        const QRect logicalSelection =
            logicalSelectionRect(context.geometry, *anchorDisplay, canvasSelection);
        if (!context.presentationSuppressed())
            thumbnailHost->beginScrollingThumbnail(
                logicalSelection.translated(-thumbnailHost->geometry().topLeft()), mode);

        pipeline->begin(requestGeneration, viewportPixelSize, mode,
                        nativeScrollingSource(requestPhysicalSelection, restoreOriginalColors,
                                              exclusionWindowIds, generation),
                        requestCadenceConfig);
        if (exportPaused)
            updatePausedState();
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
        emit owner.stateChanged();
        movement.end();
        exportPaused = false;
        ++generation;
        pendingResultRequestId.reset();
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
        if (pendingResultRequestId.has_value()) {
            detachedResultRequestIds.insert(*pendingResultRequestId);
            pendingResultRequestId.reset();
        }
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
        pendingResultRequestId.reset();
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
        latestOutputSize = result.sourceSize;
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
            latestOutputSize.isEmpty() || !callback || pendingResultRequestId.has_value()) {
            return false;
        }
        const ScreenshotScrollingTrimRange trim = currentTrim();
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

    bool beginSelectionMove(ScreenshotScrollingRecognitionMode axis, QPoint pointer) {
        if (!active || exportPaused || !movement.begin(axis, mode, canvasSelection, pointer))
            return false;
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
                        .translated(-thumbnailHost->geometry().topLeft()));
            }
        });
    }

    void endSelectionMove() {
        if (!movement.active())
            return;
        movement.end();
        if (active)
            updatePausedState();
    }

    void setExportPaused(bool paused) {
        if (!active || exportPaused == paused)
            return;
        exportPaused = paused;
        logScrollingEvent("scrolling.export_pause", generation,
                          {{QStringLiteral("status"), paused}});
        updatePausedState();
    }

    void updatePausedState() {
        const bool paused = exportPaused || movement.active();
        if (paused)
            previewWatchdog.stop();
        else if (!previewReceived)
            watchPreview();
        autoScroller.setPaused(paused);
        if (paused) {
            pipeline->pause(generation);
        } else {
            pipeline->resume(
                generation, viewportPixelSize,
                nativeScrollingSource(canvasSelection.translated(context.geometry.canvasOrigin()),
                                      restoreOriginalColors, exclusionWindowIds, generation),
                cadenceConfig);
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
    std::shared_ptr<ScreenshotScrollingTrimRange> trimRange =
        std::make_shared<ScreenshotScrollingTrimRange>();
    quint64 contentRevision = 0;
    quint64 stepGeneration = 0;
    StepCompletion stepCompletion;
    QTimer* stepTimer = nullptr;
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
    std::optional<quint64> pendingResultRequestId;
    QSet<quint64> detachedResultRequestIds;
    QRect canvasSelection;
    QSize viewportPixelSize;
    qreal sourceScale = 1.;
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

void ScreenshotScrollingCaptureController::setAutoScroll(bool enabled) {
    logScrollingEvent("scrolling.auto_scroll", m_impl->generation,
                      {{QStringLiteral("status"), enabled && m_impl->active}});
    m_impl->lastScrollStatus = -1;
    m_impl->autoScrollEnabled = enabled && m_impl->active;
    m_impl->autoScroller.setEnabled(m_impl->autoScrollEnabled);
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
    if (!s.active || s.exportPaused || s.stepCompletion || start < 0 || end <= start ||
        end > extent)
        return false;
    *s.trimRange = {start, end};
    ++s.contentRevision;
    if (s.thumbnailHost)
        s.thumbnailHost->setScrollingTrimModel(s.trimRange);
    return true;
}
bool ScreenshotScrollingCaptureController::moveSelection(QPoint offset) {
    auto& s = *m_impl;
    if (s.stepCompletion || !s.beginSelectionMove(s.mode, {}))
        return false;
    s.updateSelectionMove(offset);
    s.endSelectionMove();
    return true;
}
void ScreenshotScrollingCaptureController::cancelScrollOnce() {
    auto& s = *m_impl;
    ++s.stepGeneration;
    if (s.stepTimer) {
        s.stepTimer->stop();
        s.stepTimer->deleteLater();
        s.stepTimer = nullptr;
    }
    auto completion = std::exchange(s.stepCompletion, {});
    if (s.active && s.exportPaused)
        s.setExportPaused(false);
    if (completion)
        completion({}, QStringLiteral("canceled"));
}
void ScreenshotScrollingCaptureController::scrollOnce(const QString& direction,
                                                      StepCompletion completion) {
    auto& s = *m_impl;
    const bool vertical = direction == QStringLiteral("up") || direction == QStringLiteral("down");
    const bool horizontal =
        direction == QStringLiteral("left") || direction == QStringLiteral("right");
    if ((!vertical && !horizontal) ||
        vertical != (s.mode == ScreenshotScrollingRecognitionMode::Vertical)) {
        completion({}, QStringLiteral("invalid_direction"));
        return;
    }
    if (!s.active || s.latestOutputSize.isEmpty()) {
        completion({}, QStringLiteral("scrolling_not_ready"));
        return;
    }
    if (s.stepCompletion || s.autoScrollEnabled || s.exportPaused || s.movement.active()) {
        completion({}, QStringLiteral("busy"));
        return;
    }
    if (!snow_shot::platform::screenshotScrollPermission()) {
        completion({}, QStringLiteral("permission_required"));
        return;
    }
    s.stepCompletion = std::move(completion);
    const auto generation = ++s.stepGeneration;
    const auto revision = s.contentRevision;
    const QPointer<ScreenshotScrollingCaptureController> guard(this);
    // Stop and join the source before dispatch, removing every pre-dispatch native buffer.
    s.pipeline->finishInput([this, guard, generation, revision, direction] {
        if (!guard || generation != m_impl->stepGeneration)
            return;
        auto& s = *m_impl;
        s.stepTimer = new QTimer(this);
        s.stepTimer->setInterval(10);
        const auto started = std::make_shared<QElapsedTimer>();
        started->start();
        const auto dispatched = std::make_shared<bool>(false);
        const auto dispatchTime =
            std::make_shared<snow_shot::capture_detail::ScrollClock::time_point>();
        connect(
            s.stepTimer, &QTimer::timeout, this,
            [this, generation, revision, direction, started, dispatched, dispatchTime] {
                auto& s = *m_impl;
                if (generation != s.stepGeneration || !s.stepCompletion)
                    return;
                const auto finish = [this](QJsonObject result, QString error) {
                    auto callback = std::exchange(m_impl->stepCompletion, {});
                    if (m_impl->stepTimer) {
                        m_impl->stepTimer->stop();
                        m_impl->stepTimer->deleteLater();
                        m_impl->stepTimer = nullptr;
                    }
                    if (m_impl->active && m_impl->exportPaused)
                        m_impl->setExportPaused(false);
                    if (callback)
                        callback(std::move(result), std::move(error));
                };
                if (!s.active) {
                    finish({}, QStringLiteral("capture_unavailable"));
                    return;
                }
                if (started->elapsed() >= 2000) {
                    if (!*dispatched)
                        s.updatePausedState();
                    finish({}, QStringLiteral("timeout"));
                    return;
                }
                if (!*dispatched) {
                    if (!s.pipeline->idle())
                        return;
                    const QPoint delta = *snow_shot::capture_detail::scrollingStepDelta(direction);
                    const auto input = snow_shot::platform::sendScreenshotScroll(
                        s.canvasSelection.translated(s.context.geometry.canvasOrigin()), delta);
                    if (input.status != snow_shot::platform::ScrollInputResult::Status::Posted) {
                        s.updatePausedState();
                        finish(
                            {},
                            input.status ==
                                    snow_shot::platform::ScrollInputResult::Status::TargetNotFound
                                ? QStringLiteral("target_not_found")
                                : QStringLiteral("scroll_dispatch_failed"));
                        return;
                    }
                    *dispatchTime = snow_shot::capture_detail::ScrollClock::now();
                    *dispatched = true;
                    s.updatePausedState();
                    return;
                }
                const auto observation = s.pipeline->observation();
                const snow_shot::capture_detail::ScrollingStepBarrier barrier{*dispatchTime};
                if (!barrier.settled(observation.observedAt, observation.changedAt,
                                     s.pipeline->idle()))
                    return;
                s.stepTimer->stop();
                s.setExportPaused(true);
                const auto processedSequence = observation.sequence;
                if (!s.requestTrimmedSnapshot([this, generation, revision, direction,
                                               processedSequence,
                                               finish](ScreenshotScrollingSnapshot snapshot) {
                        if (generation != m_impl->stepGeneration)
                            return;
                        if (!snapshot.isValid()) {
                            finish({}, QStringLiteral("output_failed"));
                            return;
                        }
                        auto result = state();
                        result.insert(QStringLiteral("direction"), direction);
                        result.insert(QStringLiteral("dispatch_status"), QStringLiteral("posted"));
                        result.insert(QStringLiteral("changed"),
                                      m_impl->contentRevision != revision);
                        result.insert(QStringLiteral("processed_frame_sequence"),
                                      static_cast<qint64>(processedSequence));
                        finish(result, {});
                    }))
                    finish({}, QStringLiteral("output_failed"));
            });
        s.stepTimer->start();
    });
}
