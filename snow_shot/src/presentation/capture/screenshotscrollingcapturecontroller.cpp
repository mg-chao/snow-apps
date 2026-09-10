#include "snow_shot/presentation/screenshotscrollingcapturecontroller.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include "snow_shot/platform/windows/windowchrome.h"
#include <qt_windows.h>
#endif

#include "adaptivescrollingcapturecadence.h"
#include "screenshotscrollingautoscroller.h"
#include "snow_shot/platform/windows/scrollinput.h"
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
            if (active && !exportPaused && !previewReceived) {
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
        shutdownWorker();
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

        canvasSelection = selection;
        restoreOriginalColors = context.restoreOriginalScreenColors();
        mode = requestedMode;
        thumbnailHost = anchorOverlay;
        active = true;
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
        thumbnailHost->beginScrollingThumbnail(
            logicalSelection.translated(-thumbnailHost->geometry().topLeft()), mode);

        logPreparation();
        logScrollingEvent("scrolling.prepared", generation);
        const quint64 requestGeneration = generation;
        const QRect requestSelection = canvasSelection;
        const QRect requestPhysicalSelection =
            canvasSelection.translated(context.geometry.canvasOrigin());
        autoScroller.start(requestPhysicalSelection, mode);
        const AdaptiveScrollCadence::Config requestCadenceConfig = cadenceConfig;
        pipeline->begin(
            requestGeneration, requestSelection.size(), mode,
            nativeScrollingSource(requestPhysicalSelection, restoreOriginalColors, generation),
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
        watchPreview();
        logScrollingEvent("scrolling.mode_changed", generation,
                          {{QStringLiteral("mode"), static_cast<int>(mode)}});
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

        pipeline->begin(
            requestGeneration, requestSelection.size(), mode,
            nativeScrollingSource(requestPhysicalSelection, restoreOriginalColors, generation),
            requestCadenceConfig);
        return true;
    }

    void stop(bool restoreScreenshotPresentation) {
        previewWatchdog.stop();
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
        if (!previewReceived) {
            previewReceived = true;
            previewWatchdog.stop();
            auto fields = thumbnailHost->scrollingDiagnostics();
            fields.insert(QStringLiteral("duration_ms"), previewClock.elapsed());
            fields.insert(QStringLiteral("width"), result.sourceSize.width());
            fields.insert(QStringLiteral("height"), result.sourceSize.height());
            logScrollingEvent("scrolling.first_preview", generation, fields);
        }
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
        logScrollingEvent("scrolling.export_pause", generation,
                          {{QStringLiteral("status"), paused}});
        if (paused)
            previewWatchdog.stop();
        else if (!previewReceived)
            watchPreview();
        autoScroller.setPaused(paused);
        if (paused) {
            pipeline->pause(generation);
        } else {
            pipeline->resume(
                generation, canvasSelection.size(),
                nativeScrollingSource(canvasSelection.translated(context.geometry.canvasOrigin()),
                                      restoreOriginalColors, generation),
                cadenceConfig);
        }
    }

    ScreenshotScrollingCaptureController& owner;
    snow_shot::capture_detail::ScreenshotScrollingAutoScroller autoScroller{
        [this](const QRect& selection, const QPoint& delta) {
            const auto result =
                snow_shot::platform::windows::sendScrollingWheelStep(selection, delta);
            const int status = static_cast<int>(result.status);
            if (status != lastScrollStatus || result.error != lastScrollError) {
                lastScrollStatus = status;
                lastScrollError = result.error;
                logScrollingEvent(
                    "scrolling.wheel_dispatch", generation,
                    {{QStringLiteral("status"), status},
                     {QStringLiteral("code"), static_cast<qint64>(result.error)}},
                    result.status == snow_shot::platform::windows::ScrollInputResult::Status::Posted
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
    ScreenshotScrollingCaptureControllerContext context;
    AdaptiveScrollCadence::Config cadenceConfig;
    bool restoreOriginalColors = false;
    std::unique_ptr<ScreenshotScrollingPipeline> pipeline;
    QPointer<ScreenshotOverlayWindow> thumbnailHost;
    snow_shot::presentation::WindowCaptureExclusion captureExclusion{
#if defined(Q_OS_WIN) || defined(_WIN32)
        [this](QWidget* window, bool excluded) {
            SetLastError(ERROR_SUCCESS);
            const bool succeeded =
                snow_shot::platform::windows::setWindowExcludedFromCapture(window, excluded);
            const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
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
    logScrollingEvent("scrolling.auto_scroll", m_impl->generation,
                      {{QStringLiteral("status"), enabled && m_impl->active}});
    m_impl->lastScrollStatus = -1;
    m_impl->autoScroller.setEnabled(enabled && m_impl->active);
}

void ScreenshotScrollingCaptureController::detachPendingResultRequest() {
    m_impl->detachPendingResultRequest();
}

QRect ScreenshotScrollingCaptureController::canvasSelection() const {
    return m_impl->canvasSelection;
}
