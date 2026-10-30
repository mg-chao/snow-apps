#include "snow_shot/platform/screenshotwindownames.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "recordingtrimsession.h"
#include "snow_shot/platform/applicationqos.h"
#include "recordingeffectpreview.h"
#include "recordingaudiogainpopover.h"
#include "recordingpopupexclusions.h"
#include "widgets/popover.h"
#include "widgets/popup_surface_guard.h"
#include "snow_shot/storage/applicationstorage.h"
#include "recordingcolorsampler.h"
#include "recordingrenderjob.h"
#include "recordingrecovery.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/app/mcp/recordingoptioncontracts.h"
#include "snow_shot/platform/filemanager.h"
#include "widgets/message.h"
#include "recordingeffectstyle.h"
#include "screenrecordingperfinstrumentation.h"
#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QUuid>
#include <QElapsedTimer>

#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/components/screenrecordingsettingsdialog.h"
#include "widgets/modal.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenrecordingshortcutcontroller.h"
#include "screenrecordinggeometry.h"
#include "screenrecordingselection.h"
#include "../capture/windowcaptureexclusion.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/screenrecordingfolder.h"

#if defined(Q_OS_WIN) || defined(_WIN32) || defined(Q_OS_MACOS)
#include "snow_shot/platform/windowcaptureexclusion.h"
#if defined(Q_OS_WIN) || defined(_WIN32)
#include "snow_shot/platform/windows/windowchrome.h"
#endif
#endif

#include "snow_capture.h"
#include "snow_recording.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QScopedValueRollback>
#include "snow_shot/presentation/automationrevision.h"
#include <cmath>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QMimeData>
#include <QTimer>
#include <QWindow>
#include <QScreen>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <thread>
#include <memory>
#include <utility>

namespace {
constexpr int kDurationTickMilliseconds = 100;
constexpr int kCountdownTickMilliseconds = 16;

uint32_t recordingCaptureBackendPreference(const snow_shot::storage::RecordingSettings& settings) {
#ifdef Q_OS_MACOS
    Q_UNUSED(settings)
    return SNOW_CAPTURE_BACKEND_AUTO;
#else
    const QString mode = settings.apiMode();
    if (mode == QStringLiteral("wgc")) {
        return SNOW_CAPTURE_BACKEND_WGC;
    }
    if (mode == QStringLiteral("gdi")) {
        return SNOW_CAPTURE_BACKEND_GDI;
    }
    return SNOW_CAPTURE_BACKEND_DXGI;
#endif
}

QColor progressBarColorFromString(const QString& value) {
    const QString normalized = value.trimmed();
    return normalized.size() == 9 && normalized.startsWith(u'#')
               ? snow_shot::storage::colorFromRgbaString(normalized)
               : QColor(normalized);
}

struct DirectRecordingSettings {
    SnowRecordingOutputFormat format = SNOW_RECORDING_OUTPUT_FORMAT_MP4;
    SnowCaptureVideoCodec codec = SNOW_CAPTURE_VIDEO_CODEC_H264;
    SnowCaptureVideoEncodingPreset preset = SNOW_CAPTURE_VIDEO_ENCODING_PRESET_VERYFAST;
    uint32_t quality = 80;
    bool useHardwareEncoder = false;
    QSize maximumSize{1920, 1080};
    uint32_t targetFps = 30;
    QString extension = QStringLiteral("mp4");
};

int validRecordingFrameRate(int frameRate) {
    return frameRate > 0 ? frameRate : 30;
}

int validAnimatedImageFrameRate(int frameRate) {
    return frameRate > 0 ? frameRate : 10;
}

SnowCaptureVideoCodec videoCodec(const QString& encoder) {
    return encoder == QStringLiteral("h265") || encoder == QStringLiteral("h265_hw")
               ? SNOW_CAPTURE_VIDEO_CODEC_H265
               : SNOW_CAPTURE_VIDEO_CODEC_H264;
}

SnowCaptureVideoEncodingPreset videoEncodingPreset(const QString& preset) {
    if (preset == QStringLiteral("ultrafast")) {
        return SNOW_CAPTURE_VIDEO_ENCODING_PRESET_ULTRAFAST;
    }
    if (preset == QStringLiteral("medium")) {
        return SNOW_CAPTURE_VIDEO_ENCODING_PRESET_MEDIUM;
    }
    if (preset == QStringLiteral("veryslow")) {
        return SNOW_CAPTURE_VIDEO_ENCODING_PRESET_VERYSLOW;
    }
    if (preset == QStringLiteral("placebo")) {
        return SNOW_CAPTURE_VIDEO_ENCODING_PRESET_PLACEBO;
    }
    return SNOW_CAPTURE_VIDEO_ENCODING_PRESET_VERYFAST;
}

DirectRecordingSettings directRecordingSettings(const QString& outputFormat,
                                                const QSize& captureSize,
                                                const QJsonObject& overrides = {}) {
    const snow_shot::storage::RecordingSettings settings;
    DirectRecordingSettings result;
    const QString encoder = overrides.value(QStringLiteral("encoder")).toString(settings.encoder());
    result.codec = videoCodec(encoder);
    result.preset = videoEncodingPreset(
        overrides.value(QStringLiteral("encoding_preset")).toString(settings.encodingPreset()));
    result.quality = static_cast<uint32_t>(
        qBound(0, overrides.value(QStringLiteral("quality")).toInt(settings.videoQuality()), 100));
    result.useHardwareEncoder =
        encoder == QStringLiteral("h264_hw") || encoder == QStringLiteral("h265_hw");

    if (outputFormat == QStringLiteral("mp4")) {
        result
            .maximumSize = snow_shot::presentation::recording::screenRecordingMaximumSizeForClarity(
            overrides.value(QStringLiteral("clarity")).toString(settings.screenRecordingClarity()));
        result.maximumSize = snow_shot::presentation::recording::screenRecordingOrientedMaximumSize(
            result.maximumSize, captureSize);
        result.targetFps = static_cast<uint32_t>(validRecordingFrameRate(
            overrides.value(QStringLiteral("frame_rate")).toInt(settings.frameRate())));
        return result;
    }

    result.maximumSize = snow_shot::presentation::recording::screenRecordingMaximumSizeForClarity(
        overrides.value(QStringLiteral("animated_clarity"))
            .toString(settings.animatedImageClarity()));
    result.maximumSize = snow_shot::presentation::recording::screenRecordingOrientedMaximumSize(
        result.maximumSize, captureSize);
    result.targetFps = static_cast<uint32_t>(
        validAnimatedImageFrameRate(overrides.value(QStringLiteral("animated_frame_rate"))
                                        .toInt(settings.animatedImageFrameRate())));
    if (outputFormat == QStringLiteral("apng")) {
        result.format = SNOW_RECORDING_OUTPUT_FORMAT_APNG;
        result.extension = QStringLiteral("apng");
    } else if (outputFormat == QStringLiteral("webp")) {
        result.format = SNOW_RECORDING_OUTPUT_FORMAT_WEBP;
        result.extension = QStringLiteral("webp");
    } else {
        result.format = SNOW_RECORDING_OUTPUT_FORMAT_GIF;
        result.extension = QStringLiteral("gif");
    }
    return result;
}

// Owns the native recording session (capture pipeline, input hooks while
// running, and worker threads) through its destroy entry point on every path.
struct RecordingSessionDeleter {
    mutable snow_shot::runtime::RuntimeActivityLease activity;
    void operator()(SnowRecordingSession* session) const {
        snow_recording_session_destroy(session);
        activity = {};
    }
};
using RecordingSessionHandle = std::unique_ptr<SnowRecordingSession, RecordingSessionDeleter>;

struct FinalizationResult {
    bool ok = false;
    QString error;
    SnowRecordingSource* source = nullptr;
};

struct StartAttemptResult {
    RecordingSessionHandle session;
    QString outputPath;
    QString error;
};

// The start worker only publishes success and queues a GUI-thread commit. Keeping
// this independent of the controller also covers a window retired during startup.
struct RecordingAreaHistoryCommit {
    QRect rectangle;
    QPointer<snow_shot::storage::ConfigurationStore> configuration;
    std::atomic_bool started{false};
    bool committed = false; // GUI thread only.
    bool saved = false;     // GUI thread only.

    void commit() {
        if (committed || !started.load() || !configuration)
            return;
        committed = true;
        saved = snow_shot::storage::RecordingAreaHistorySettings::append(*configuration, rectangle);
    }
};

QString captureError() {
    const char* error = snow_recording_last_error_message();
    const QString message = QString::fromUtf8(error != nullptr ? error : "");
    const QString recoveryMarker = QStringLiteral("recoverable media is retained in ");
    if (message.contains(recoveryMarker)) {
        return QCoreApplication::translate(
                   "ScreenRecordingController",
                   "The recording could not be finalized. Recoverable media and its timeline "
                   "were saved in:\n%1\n\nKeep this folder to recover the recording.")
            .arg(message.section(recoveryMarker, 1).trimmed());
    }
    if (message.contains(QStringLiteral("keyboard recording:"))) {
        return QCoreApplication::translate("ScreenRecordingController",
                                           "Keyboard recording failed: %1")
            .arg(message.section(QStringLiteral("keyboard recording:"), 1).trimmed());
    }
    return message.isEmpty()
               ? QCoreApplication::translate("ScreenRecordingController", "Unknown recording error")
               : message;
}

uint32_t packedRgba(const QColor& color) {
    return (static_cast<uint32_t>(color.red()) << 24U) |
           (static_cast<uint32_t>(color.green()) << 16U) |
           (static_cast<uint32_t>(color.blue()) << 8U) | static_cast<uint32_t>(color.alpha());
}

void copyFileToClipboard(const QString& filePath) {
    auto* mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile(filePath)});
    QApplication::clipboard()->setMimeData(mimeData);
}
} // namespace

namespace {
// UI resources and connection contexts exist only for an open selection. Backend
// finalization belongs to the controller and can finish after this object is retired.
struct RecordingUiSession final : QObject {
    explicit RecordingUiSession(QObject* parent) : QObject(parent) {}
    ~RecordingUiSession() override {
        if (settingsModal)
            settingsModal->close();
    }
    QPointer<adqt::widgets::AdModal> settingsModal;
    std::unique_ptr<ScreenRecordingAreaWindow> area = std::make_unique<ScreenRecordingAreaWindow>();
    std::unique_ptr<ScreenRecordingToolbarWindow> toolbar =
        std::make_unique<ScreenRecordingToolbarWindow>();
    std::unique_ptr<QObject> connections = std::make_unique<QObject>();
    std::unique_ptr<ScreenRecordingShortcutController> shortcuts;
    std::unique_ptr<RecordingEffectPreview> preview;
    std::unique_ptr<snow_shot::presentation::recording::RecordingColorSampler> colorSampler =
        std::make_unique<snow_shot::presentation::recording::RecordingColorSampler>(*area,
                                                                                    *toolbar);
};
} // namespace

struct ScreenRecordingController::Impl {
    explicit Impl(ScreenRecordingController& owner,
                  ScreenRecordingController::EffectsSourceFactory factory = {})
        : owner(owner), effectsSourceFactory(std::move(factory)) {
        const snow_shot::storage::RecordingSettings settings;
        microphoneEnabled = settings.microphoneEnabled();
        systemAudioEnabled = settings.systemAudioEnabled();
        microphoneGainDb = settings.microphoneGainDb();
        systemAudioGainDb = settings.systemAudioGainDb();
        outputFormat = settings.outputFormat();
        mouseTrailColor = settings.mouseTrailColor();
        mouseTrailDurationMs = settings.mouseTrailDurationMs();
        keyboardSize = settings.keyboardSize();
        webcamEnabled = settings.webcamEnabled();
        webcamSize = settings.webcamSize();
        webcamOffset = QPoint(settings.webcamOffsetLeft(), settings.webcamOffsetBottom());
        webcamShape = settings.webcamShape();
        webcamOrientation = settings.webcamOrientation();
        keyboardBackgroundColor = settings.keyboardBackgroundColor();
        keyboardForegroundColor = settings.keyboardForegroundColor();
        mouseClickColor = settings.mouseClickColor();
        mouseHighlightEnabled = settings.mouseHighlightEnabled();
        recordMouseClicks = settings.recordMouseClicks();
        recordMouseScrolling = settings.recordMouseScrolling();
        mouseHighlightColor = settings.mouseHighlightColor();
        mouseHighlightRadius = settings.mouseHighlightRadius();
        showCursor = settings.showCursor();
        showKeyboard = settings.showKeyboard();
        startDelaySeconds = settings.startDelaySeconds();
        postProcessingEnabled = settings.postProcessingEnabled();
        postProcessingEffect = settings.postProcessingEffect();
        progressBarColor = settings.progressBarColor();
        QObject::connect(&snow_shot::storage::ApplicationStorage::instance().configuration(),
                         &snow_shot::storage::ConfigurationStore::valueChanged, &owner,
                         [this](const QString& key, const QJsonValue&) {
                             if (key == QStringLiteral("screen_recording/area_history")) {
                                 areaHistoryIndex = -1;
                                 return;
                             }
                             if (key == QStringLiteral("screen_recording/show_status_prompts")) {
                                 syncPreview();
                                 return;
                             }
                             if (key == QStringLiteral("screen_recording/enable_webcam") ||
                                 key == QStringLiteral("screen_recording/webcam_size") ||
                                 key == QStringLiteral("screen_recording/webcam_shape") ||
                                 key == QStringLiteral("screen_recording/webcam_offset_left") ||
                                 key == QStringLiteral("screen_recording/webcam_offset_bottom") ||
                                 key == QStringLiteral("screen_recording/webcam_orientation")) {
                                 // Saved preferences belong to the next editable session. Keep
                                 // the refresh pending while recording or automation owns options.
                                 webcamSettingsRefreshPending = true;
                                 if (!automationOwned && recordingOptionsEditable())
                                     syncUi();
                                 return;
                             }
                             // Automation owns isolated options; saved preferences still apply
                             // when a later manual recording starts. Active jobs keep snapshots.
                             if (automationOwned)
                                 return;
                             const snow_shot::storage::RecordingSettings saved;
                             if (key == QStringLiteral("screen_recording/post_processing_enabled"))
                                 setOption(postProcessingEnabled, saved.postProcessingEnabled());
                             else if (key ==
                                      QStringLiteral("screen_recording/post_processing_effect"))
                                 setOption(postProcessingEffect, saved.postProcessingEffect());
                             else if (key == QStringLiteral("screen_recording/progress_bar_color"))
                                 setOption(progressBarColor, saved.progressBarColor());
                             else
                                 return;
                             syncUi();
                         });
#ifdef Q_OS_MACOS
        dimensionsPollTimer.setInterval(50);
        QObject::connect(&dimensionsPollTimer, &QTimer::timeout, &owner, [this] {
            if (!dimensionsFuture.valid() || dimensionsFuture.wait_for(std::chrono::milliseconds(
                                                 0)) != std::future_status::ready)
                return;
            const auto [generation, size] = dimensionsFuture.get();
            dimensionsPollTimer.stop();
            if (generation == dimensionsGeneration) {
                previewOutput = size;
                dimensionsResolved = true;
            }
            syncPreview();
        });
#endif
        webcamTimer.setInterval(100);
        QObject::connect(&webcamTimer, &QTimer::timeout, &owner, [this] { syncWebcam(); });
        audioMeterTimer.setInterval(33);
        audioMeterTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&audioMeterTimer, &QTimer::timeout, &owner, [this] { pollAudioMeter(); });
        audioMeterClock.start();
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            QObject::connect(
                &storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged,
                &owner, [this](const QString& key, const QJsonValue&) {
                    if (key != QStringLiteral("screen_recording/system_audio_gain_db") &&
                        key != QStringLiteral("screen_recording/microphone_gain_db"))
                        return;
                    audioSettingsRefreshPending = true;
                    if (audioSettingsRefreshQueued)
                        return;
                    audioSettingsRefreshQueued = true;
                    QTimer::singleShot(0, &this->owner, [this] {
                        audioSettingsRefreshQueued = false;
                        refreshAudioGains();
                    });
                });
        }
        exclusionPollTimer.setInterval(33);
        QObject::connect(&exclusionPollTimer, &QTimer::timeout, &owner,
                         [this] { pollToolbarExclusions(); });
        durationTimer.setInterval(kDurationTickMilliseconds);
        durationTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&durationTimer, &QTimer::timeout, &owner, [this]() {
            if (pollSessionLiveness()) {
                return;
            }
            if (sessionStatus.state() != ScreenshotToolPalette::RecordingState::Recording) {
                return;
            }
            durationMilliseconds += kDurationTickMilliseconds;
            syncUi();
        });
        finalizationPollTimer.setInterval(50);
        QObject::connect(&finalizationPollTimer, &QTimer::timeout, &owner, [this]() {
            pollDiscard();
            pollFinalization();
        });
        startPollTimer.setInterval(50);
        QObject::connect(&startPollTimer, &QTimer::timeout, &owner, [this]() { pollStart(); });
        // The countdown shares one clock with the overlay digits: every tick
        // pushes the same elapsed reading that decides when recording starts.
        countdownTimer.setInterval(kCountdownTickMilliseconds);
        countdownTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&countdownTimer, &QTimer::timeout, &owner, [this]() { tickCountdown(); });
    }

    void shutdown() {
        stopAudioMeter();
        exclusionPollTimer.stop();
        durationTimer.stop();
        finalizationPollTimer.stop();
        startPollTimer.stop();
        countdownTimer.stop();
        std::future<std::pair<quint64, QSize>> pendingDimensions;
#ifdef Q_OS_MACOS
        dimensionsPollTimer.stop();
        pendingDimensions = std::move(dimensionsFuture);
#endif
        // Native acquisition/teardown and compressed-source finalization can
        // block. Transfer every pending operation and its session into one
        // independent owner so controller destruction never joins on the GUI.
        if (startFuture.valid() || finalizationFuture.valid() || discardFuture.valid() ||
            recordingSession != nullptr || pendingDimensions.valid()) {
            std::thread(
                snow_shot::runtime::trackRuntimeWork(
                    [start = std::move(startFuture), finish = std::move(finalizationFuture),
                     discard = std::move(discardFuture), session = std::move(recordingSession),
                     dimensions = std::move(pendingDimensions)]() mutable {
                        snow_shot::platform::applyApplicationQoSToCurrentThread();
                        if (start.valid()) {
                            StartAttemptResult result = start.get();
                            if (result.session != nullptr && session == nullptr)
                                session = std::move(result.session);
                        }
                        if (finish.valid()) {
                            const auto result = finish.get();
                            if (result.source)
                                snow_recording_source_destroy(result.source);
                        }
                        if (discard.valid())
                            discard.wait();
                        session.reset();
                        if (dimensions.valid())
                            dimensions.wait();
                    }))
                .detach();
        }
        if (renderJob) {
            delete renderJob;
            renderJob = nullptr;
        }
        destroyUi();
    }

    void setCaptureActivity(bool active) {
        if (desktopCaptureActive == active)
            return;
        desktopCaptureActive = active;
        emit owner.captureActivityChanged(active);
    }
    void open(const QRect& requestedRegion) {
        if (sessionEnding)
            return;
        const QRect region =
            snow_shot::presentation::recording::screenRecordingNormalizedRegion(requestedRegion);
        if (!region.isValid() || region.isEmpty() || (trimRequested && uiSession) ||
            sessionStatus.busy() ||
            sessionStatus.state() != ScreenshotToolPalette::RecordingState::Idle) {
            return;
        }
        setCaptureActivity(true);
        cancelPendingStart();
        areaHistoryIndex = -1;
        refreshWebcamSettings();
        if (!automationOwned && !automationNextStart) {
            const snow_shot::storage::RecordingSettings settings;
            microphoneGainDb = settings.microphoneGainDb();
            systemAudioGainDb = settings.systemAudioGainDb();
        }
        if (uiSession != nullptr) {
            setOption(recordingRegion, region);
            updateCaptureRegion();
            // Match the screenshot capture flow, which refreshes persisted
            // creation styles on every capture, so style edits made elsewhere
            // since the last session are picked up on reopen.
            const SnowCanvasStyleDefaults defaults =
                snow_shot::presentation::screenshotCanvasToolStyleDefaults();
            if (areaWindow->canvas() != nullptr) {
                snow_shot::presentation::applyScreenshotCanvasToolStyles(*areaWindow->canvas(),
                                                                         defaults);
            }
            if (ScreenshotToolPalette* palette = toolbarWindow->palette()) {
                palette->setCreationStyleDefaults(defaults);
            }
            areaWindow->setRecordingRegion(region);
            syncUi();
            toolbarWindow->placeForRecordingRegion(region);
            areaWindow->show();
            areaWindow->raise();
            showRecordingControls();
            return;
        }

        trimRequested = false;
        recordingRegion = region;
        updateCaptureRegion();
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.before_ui_session");
        uiSession = new RecordingUiSession(&owner);
        webcamFailed = false;
        snow_webcam_retry(webcam.get());
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.ui_session_constructed");
        areaWindow = uiSession->area.get();
        QObject::connect(areaWindow->canvas(), &SnowCanvasWidget::historyStateChanged,
                         uiSession->connections.get(), [this] {
                             automationRevision = snow_shot::presentation::nextAutomationRevision();
                         });
        QObject::connect(areaWindow->canvas(), &SnowCanvasWidget::styleToolbarStateChanged,
                         uiSession->connections.get(), [this] {
                             automationRevision = snow_shot::presentation::nextAutomationRevision();
                         });
        uiSession->preview = std::make_unique<RecordingEffectPreview>(
            *areaWindow, effectsSourceFactory ? effectsSourceFactory() : nullptr);
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.preview_created");
        uiSession->preview->reportError = [this](const QString& error) {
            showRecordingWarning(QStringLiteral("screen-recording-preview-error"),
                                 tr("Motion preview unavailable: %1").arg(error));
        };
        toolbarWindow = uiSession->toolbar.get();
        // Keep the toolbar above the area even when drawing or resizing activates the area.
        toolbarWindow->setTransientOwnerWindow(areaWindow);
        toolbarWindow->setAutoHideControlOwner(areaWindow);
        toolbarWindow->setAutoHideControlShowGuard(
            [this](QWidget* surface) { return guardToolbarSurface(surface); });
        QObject::connect(toolbarWindow, &ScreenRecordingToolbarWindow::autoHideControlRequested,
                         uiSession->connections.get(),
                         [this](QWidget*) { toolbarExclusions.retry(); });
        areaWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        toolbarWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        areaWindow->setRecordingRegion(region);
        toolbarWindow->placeForRecordingRegion(region);
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.region_applied");
        connectToolbar();
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.toolbar_connected");
        uiSession->shortcuts =
            std::make_unique<ScreenRecordingShortcutController>(*areaWindow, *toolbarWindow);
        uiSession->shortcuts->setAreaHistoryActions(
            [this] { return canNavigateAreaHistory(); },
            [this](bool previous) { navigateAreaHistory(previous); });
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.shortcuts_created");

        sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::idle();

        automationRevision = snow_shot::presentation::nextAutomationRevision();
        durationMilliseconds = 0;
        syncUi();
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.ui_synced");

        areaWindow->show();
        areaWindow->raise();
        showRecordingControls();
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.show_returned");
    }

    void showRecordingControls() {
        toolbarWindow->showWithoutActivating();
        if (!areaWindow->activateInput()) {
            toolbarWindow->showAndActivate();
        }
    }

    bool isOpen() const {
        return areaWindow != nullptr && toolbarWindow != nullptr &&
               (areaWindow->isVisible() || toolbarWindow->recordingUiPresented());
    }

    template <typename T> void setOption(T& option, const T& value) {
        // Track mutations where they happen, including change-then-restore, without
        // serializing automation state on the duration/preview timer.
        if (option == value)
            return;
        option = value;
        automationRevision = snow_shot::presentation::nextAutomationRevision();
    }

    void connectToolbar() {
        ScreenshotToolPalette* palette =
            toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
        if (palette == nullptr) {
            return;
        }
        palette->setRecordingSettingsOwnerWindow(areaWindow);
        QObject::connect(
            palette, &ScreenshotToolPalette::recordingSettingsRequested,
            uiSession->connections.get(), [this] {
                if (sessionStatus.state() != ScreenshotToolPalette::RecordingState::Idle ||
                    sessionStatus.busy())
                    return;
                if (!uiSession->settingsModal) {
                    auto* modal = snow_shot::presentation::createScreenRecordingSettingsDialog(
                        areaWindow, uiSession);
                    uiSession->settingsModal = modal;
                    toolbarWindow->setAutoHideBlocked(modal, true);
                    QObject::connect(modal, &adqt::widgets::AdModal::finished,
                                     uiSession->connections.get(), [this, modal] {
                                         toolbarWindow->setAutoHideBlocked(modal, false);
                                         uiSession->settingsModal = nullptr;
                                         syncPreview();
                                     });
                }
                uiSession->settingsModal->open();
            });
        QObject::connect(palette, &ScreenshotToolPalette::recordingExportSettingsVisibleChanged,
                         uiSession->connections.get(), [this](bool visible) {
                             if (!visible && uiSession->settingsModal)
                                 uiSession->settingsModal->close();
                         });
        connectDrawingToolbar(*palette);
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::webcamLayoutPreviewed,
                         uiSession->connections.get(), [this](int size, const QPoint& offset) {
                             setOption(webcamSize, size);
                             setOption(webcamOffset, offset);
                             toolbarWindow->palette()->setRecordingWebcamSize(size);
                             syncPreview();
                         });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::webcamLayoutCommitted,
                         uiSession->connections.get(), [this](int size, const QPoint& offset) {
                             if (recordingOptionsEditable())
                                 snow_shot::storage::RecordingSettings().setWebcamLayout(size,
                                                                                         offset);
                         });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::webcamOrientationRequested,
                         uiSession->connections.get(), [this](int action) {
                             if (!recordingOptionsEditable())
                                 return;
                             const int quarterTurns = webcamOrientation & 3;
                             const int mirror = webcamOrientation & 4;
                             int next = webcamOrientation;
                             switch (action) {
                             case 0:
                                 next = ((quarterTurns + 1) & 3) | mirror;
                                 break;
                             case 1:
                                 next = ((quarterTurns + 3) & 3) | mirror;
                                 break;
                             case 2:
                                 next = ((4 - quarterTurns) & 3) | (mirror ^ 4);
                                 break;
                             case 3:
                                 next = ((6 - quarterTurns) & 3) | (mirror ^ 4);
                                 break;
                             default:
                                 return;
                             }
                             setOption(webcamOrientation, next);
                             snow_shot::storage::RecordingSettings().setWebcamOrientation(next);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingKeyboardSizeChanged,
                         uiSession->connections.get(), [this](int value) {
                             setOption(keyboardSize, value);
                             snow_shot::storage::RecordingSettings().setKeyboardSize(value);
                             syncPreview();
                         });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::recordingRegionChanged,
                         uiSession->connections.get(), [this](const QRect& region) {
                             if (trimRequested) {
                                 toolbarWindow->placeForRecordingRegion(region);
                                 return;
                             }
                             if (sessionStatus.state() !=
                                     ScreenshotToolPalette::RecordingState::Idle ||
                                 sessionStatus.busy()) {
                                 return;
                             }
                             setOption(recordingRegion, region);
                             if (!restoringAreaHistory)
                                 areaHistoryIndex = -1;
                             updateCaptureRegion();
                             syncPreview();
                             toolbarWindow->placeForRecordingRegion(region);
                         });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::regionInteractionStarted,
                         uiSession->connections.get(),
                         [this]() { toolbarWindow->beginRegionInteraction(); });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::regionInteractionFinished,
                         uiSession->connections.get(), [this]() {
                             if (areaWindow->isVisible()) {
                                 toolbarWindow->endRegionInteraction(areaWindow->recordingRegion());
                                 areaWindow->activateInput();
                             }
                         });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::closeRequested,
                         uiSession->connections.get(), [this]() { close(); });
        QObject::connect(toolbarWindow, &ScreenRecordingToolbarWindow::closeRequested,
                         uiSession->connections.get(), [this]() { close(); });
        if (palette->recordingExportSettingsVisible()) {
            areaWindow->setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
        }
        QObject::connect(palette, &ScreenshotToolPalette::recordingStartRequested,
                         uiSession->connections.get(), [this]() { start(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingStopRequested,
                         uiSession->connections.get(), [this]() { stop(false); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingPauseRequested,
                         uiSession->connections.get(), [this]() { pause(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingResumeRequested,
                         uiSession->connections.get(), [this]() { resume(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMicrophoneToggled,
                         uiSession->connections.get(), [this](bool enabled) {
                             setOption(microphoneEnabled, enabled);
                             snow_shot::storage::RecordingSettings().setMicrophoneEnabled(enabled);
                             syncPreview();
                             syncAudioMeter();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingSystemAudioToggled,
                         uiSession->connections.get(), [this](bool enabled) {
                             setOption(systemAudioEnabled, enabled);
                             snow_shot::storage::RecordingSettings().setSystemAudioEnabled(enabled);
                             syncPreview();
                             syncAudioMeter();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMicrophoneGainChanged,
                         uiSession->connections.get(),
                         [this](int gainDb) { changeAudioGain(true, gainDb); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingSystemAudioGainChanged,
                         uiSession->connections.get(),
                         [this](int gainDb) { changeAudioGain(false, gainDb); });
        for (bool microphone : {false, true}) {
            auto* popover = palette->recordingAudioGainPopover(microphone);
            QObject::connect(popover, &RecordingAudioGainPopover::visibleChanged,
                             uiSession->connections.get(), [this, microphone](bool visible) {
                                 if (visible) {
                                     audioMeterSource = microphone ? 1 : 0;
                                     displayedAudioPeak = 0;
                                     audioClipDeadline = 0;
                                     lastAudioMeterTick = audioMeterClock.elapsed();
                                 }
                                 syncAudioMeter();
                             });
            QObject::connect(popover, &RecordingAudioGainPopover::surfaceVisibilityChanged,
                             uiSession->connections.get(), [this](bool) { syncAudioMeter(); });
        }
        QObject::connect(palette, &ScreenshotToolPalette::recordingOpenFolderRequested,
                         uiSession->connections.get(), [this]() { openFolder(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCloseRequested,
                         uiSession->connections.get(), [this]() { close(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCopyRequested,
                         uiSession->connections.get(), [this]() {
                             if (trimSession)
                                 trimSession->exportClip(false);
                             else
                                 stop(true);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingTrimRequested,
                         uiSession->connections.get(), [this] {
                             if (trimRequested)
                                 exitTrim();
                             else
                                 beginTrim(false);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingSaveRequested,
                         uiSession->connections.get(), [this] {
                             if (trimSession)
                                 trimSession->exportClip(true);
                             else
                                 beginTrim(true);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingOutputFormatChanged,
                         uiSession->connections.get(), [this](const QString& format) {
                             setOption(outputFormat, format);
                             snow_shot::storage::RecordingSettings().setOutputFormat(format);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingPostProcessingEnabledChanged,
                         uiSession->connections.get(), [this](bool value) {
                             setOption(postProcessingEnabled, value);
                             snow_shot::storage::RecordingSettings().setPostProcessingEnabled(
                                 value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingPostProcessingEffectChanged,
                         uiSession->connections.get(), [this](const QString& value) {
                             setOption(postProcessingEffect, value);
                             snow_shot::storage::RecordingSettings().setPostProcessingEffect(value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingProgressBarColorChanged,
                         uiSession->connections.get(), [this](const QColor& value) {
                             setOption(progressBarColor, value);
                             snow_shot::storage::RecordingSettings().setProgressBarColor(value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingStartDelaySecondsChanged,
                         uiSession->connections.get(), [this](int seconds) {
                             setOption(startDelaySeconds, seconds);
                             snow_shot::storage::RecordingSettings().setStartDelaySeconds(seconds);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseTrailDurationMsChanged,
                         uiSession->connections.get(), [this](int value) {
                             setOption(mouseTrailDurationMs, value);
                             snow_shot::storage::RecordingSettings().setMouseTrailDurationMs(value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingKeyboardBackgroundColorChanged,
                         uiSession->connections.get(), [this](const QColor& value) {
                             setOption(keyboardBackgroundColor, value);
                             snow_shot::storage::RecordingSettings().setKeyboardBackgroundColor(
                                 value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingKeyboardForegroundColorChanged,
                         uiSession->connections.get(), [this](const QColor& value) {
                             setOption(keyboardForegroundColor, value);
                             snow_shot::storage::RecordingSettings().setKeyboardForegroundColor(
                                 value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseTrailColorChanged,
                         uiSession->connections.get(), [this](const QColor& color) {
                             setOption(mouseTrailColor, color);
                             snow_shot::storage::RecordingSettings().setMouseTrailColor(color);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseClickColorChanged,
                         uiSession->connections.get(), [this](const QColor& color) {
                             setOption(mouseClickColor, color);
                             snow_shot::storage::RecordingSettings().setMouseClickColor(color);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingKeyboardVisibleChanged,
                         uiSession->connections.get(), [this](bool visible) {
                             setOption(showKeyboard, visible);
                             snow_shot::storage::RecordingSettings().setShowKeyboard(visible);
                             syncUi();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingWebcamEnabledChanged,
                         uiSession->connections.get(), [this](bool enabled) {
                             setOption(webcamEnabled, enabled);
                             webcamFailed = false;
                             if (enabled)
                                 snow_webcam_retry(webcam.get());
                             snow_shot::storage::RecordingSettings().setWebcamEnabled(enabled);
                             syncUi();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingWebcamSizeChanged,
                         uiSession->connections.get(), [this](int value) {
                             setOption(webcamSize, value);
                             snow_shot::storage::RecordingSettings().setWebcamSize(value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingWebcamShapeChanged,
                         uiSession->connections.get(), [this](const QString& value) {
                             setOption(webcamShape, value);
                             snow_shot::storage::RecordingSettings().setWebcamShape(value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseHighlightEnabledChanged,
                         uiSession->connections.get(), [this](bool value) {
                             setOption(mouseHighlightEnabled, value);
                             snow_shot::storage::RecordingSettings().setMouseHighlightEnabled(
                                 value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingRecordMouseClicksChanged,
                         uiSession->connections.get(), [this](bool value) {
                             setOption(recordMouseClicks, value);
                             snow_shot::storage::RecordingSettings().setRecordMouseClicks(value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingRecordMouseScrollingChanged,
                         uiSession->connections.get(), [this](bool value) {
                             setOption(recordMouseScrolling, value);
                             snow_shot::storage::RecordingSettings().setRecordMouseScrolling(value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseHighlightColorChanged,
                         uiSession->connections.get(), [this](const QColor& value) {
                             setOption(mouseHighlightColor, value);
                             snow_shot::storage::RecordingSettings().setMouseHighlightColor(value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseHighlightRadiusChanged,
                         uiSession->connections.get(), [this](int value) {
                             setOption(mouseHighlightRadius, value);
                             snow_shot::storage::RecordingSettings().setMouseHighlightRadius(value);
                             syncPreview();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCursorVisibleChanged,
                         uiSession->connections.get(), [this](bool visible) {
                             setOption(showCursor, visible);
                             snow_shot::storage::RecordingSettings().setShowCursor(visible);
                             syncPreview();
                         });
    }

    void connectDrawingToolbar(ScreenshotToolPalette& palette) {
        SnowCanvasWidget* canvas = areaWindow != nullptr ? areaWindow->canvas() : nullptr;
        if (canvas == nullptr) {
            return;
        }
        const auto activate = [this, canvas](SnowCanvasTool tool) {
            canvas->setCanvasTool(tool);
            areaWindow->setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
        };
        snow_shot::presentation::recording::connectScreenRecordingSelection(
            palette, *areaWindow, *uiSession->connections);
        QObject::connect(&palette, &ScreenshotToolPalette::speechBubbleRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::SpeechBubble); });
        QObject::connect(&palette, &ScreenshotToolPalette::shapeRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Shape); });
        QObject::connect(&palette, &ScreenshotToolPalette::distanceToolRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Distance); });
        QObject::connect(&palette, &ScreenshotToolPalette::angleToolRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Angle); });
        QObject::connect(&palette, &ScreenshotToolPalette::bracketToolRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Bracket); });
        QObject::connect(&palette, &ScreenshotToolPalette::arrowRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Arrow); });
        QObject::connect(&palette, &ScreenshotToolPalette::lineRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Line); });
        QObject::connect(&palette, &ScreenshotToolPalette::freeDrawRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::FreeDraw); });
        QObject::connect(&palette, &ScreenshotToolPalette::highlightRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::RectangleHighlight); });
        QObject::connect(&palette, &ScreenshotToolPalette::penHighlightRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::PenHighlight); });
        QObject::connect(&palette, &ScreenshotToolPalette::rectangleFilterRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::RectangleFilter); });
        QObject::connect(&palette, &ScreenshotToolPalette::penFilterRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::PenFilter); });
        QObject::connect(&palette, &ScreenshotToolPalette::autoFilterRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::AutoFilter); });
        QObject::connect(&palette, &ScreenshotToolPalette::spotlightRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Spotlight); });
        QObject::connect(&palette, &ScreenshotToolPalette::rectangleEraserRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::RectangleEraser); });
        QObject::connect(&palette, &ScreenshotToolPalette::brushEraserRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::BrushEraser); });
        QObject::connect(&palette, &ScreenshotToolPalette::eraserRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Eraser); });
        QObject::connect(&palette, &ScreenshotToolPalette::watermarkRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Watermark); });
        QObject::connect(&palette, &ScreenshotToolPalette::textRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Text); });
        QObject::connect(&palette, &ScreenshotToolPalette::serialNumberRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::SerialNumber); });
        QObject::connect(&palette, &ScreenshotToolPalette::undoRequested,
                         uiSession->connections.get(),
                         [canvas]() { static_cast<void>(canvas->undo()); });
        QObject::connect(&palette, &ScreenshotToolPalette::redoRequested,
                         uiSession->connections.get(),
                         [canvas]() { static_cast<void>(canvas->redo()); });

        QObject::connect(&palette, &ScreenshotToolPalette::watermarkPreviewChanged,
                         uiSession->connections.get(),
                         [canvas](const SnowCanvasWatermarkConfig& config) {
                             canvas->previewCanvasWatermarkConfig(config);
                         });

        QObject::connect(&palette, &ScreenshotToolPalette::spotlightPreviewChanged,
                         uiSession->connections.get(),
                         [canvas](const SnowCanvasSpotlightConfig& config) {
                             canvas->previewCanvasSpotlightConfig(config);
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::textStylePopupInteractionBegan,
                         uiSession->connections.get(),
                         [canvas]() { canvas->beginTextStylePopupInteraction(); });
        QObject::connect(&palette, &ScreenshotToolPalette::textStylePopupInteractionEnded,
                         uiSession->connections.get(),
                         [canvas, this]() { canvas->endTextStylePopupInteraction(toolbarWindow); });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::drawingDeactivationRequested,
                         uiSession->connections.get(), [this, &palette]() {
                             palette.clearActiveTool();
                             areaWindow->setInputMode(
                                 ScreenRecordingAreaWindow::InputMode::PassThrough);
                         });
        new snow_shot::presentation::ScreenshotStyleBinding(palette, *canvas,
                                                            uiSession->connections.get());
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::drawingWheelRequested,
                         uiSession->connections.get(), [canvas, &palette](int direction) {
                             static_cast<void>(snow_shot::presentation::stepScreenshotStyle(
                                 palette, *canvas, direction));
                         });
        snow_shot::presentation::applyScreenshotCanvasToolStyles(
            *canvas, snow_shot::presentation::screenshotCanvasToolStyleDefaults());
        palette.setCreationStyleDefaults(
            snow_shot::presentation::screenshotCanvasToolStyleDefaults());
        palette.setHistoryState(canvas->canvasHistoryState());
        palette.setStyleToolbarState(canvas->canvasStyleToolbarState());
        palette.setWatermarkConfig(canvas->canvasWatermarkConfig());
        palette.setSpotlightConfig(canvas->canvasSpotlightConfig());
    }

    void cancelPendingStart() {
        ++startGeneration;
        startScheduled = false;
        countdownTimer.stop();
        if (sessionStatus.busyOperation() ==
            ScreenshotToolPalette::RecordingBusyOperation::CountingDown) {
            if (areaWindow != nullptr) {
                areaWindow->clearCountdown();
            }
            sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::idle();
            automationRevision = snow_shot::presentation::nextAutomationRevision();
        }
    }

    void start() {
        if (sessionEnding)
            return;
        if (trimRequested && !exitTrim())
            return;
        if (!isOpen() || !recordingOptionsEditable()) {
            return;
        }
        areaWindow->cancelWebcamInteraction();
        // UI starts after an automated session use current saved preferences.
        if (automationOwned && !automationNextStart) {
            automationRevision = snow_shot::presentation::nextAutomationRevision();
            automationOptions = {};
            automationOwned = false;
            webcamSettingsRefreshPending = true;
            const snow_shot::storage::RecordingSettings settings;
            microphoneEnabled = settings.microphoneEnabled();
            systemAudioEnabled = settings.systemAudioEnabled();
            microphoneGainDb = settings.microphoneGainDb();
            systemAudioGainDb = settings.systemAudioGainDb();
            outputFormat = settings.outputFormat();
            mouseTrailColor = settings.mouseTrailColor();
            mouseTrailDurationMs = settings.mouseTrailDurationMs();
            keyboardSize = settings.keyboardSize();
            keyboardBackgroundColor = settings.keyboardBackgroundColor();
            keyboardForegroundColor = settings.keyboardForegroundColor();
            mouseClickColor = settings.mouseClickColor();
            mouseHighlightEnabled = settings.mouseHighlightEnabled();
            recordMouseClicks = settings.recordMouseClicks();
            recordMouseScrolling = settings.recordMouseScrolling();
            mouseHighlightColor = settings.mouseHighlightColor();
            mouseHighlightRadius = settings.mouseHighlightRadius();
            showCursor = settings.showCursor();
            showKeyboard = settings.showKeyboard();
            startDelaySeconds = settings.startDelaySeconds();
            postProcessingEnabled = settings.postProcessingEnabled();
            postProcessingEffect = settings.postProcessingEffect();
            progressBarColor = settings.progressBarColor();
        }
        automationNextStart = false;
        refreshWebcamSettings();
        if (!allowRecording(true)) {
            setCaptureActivity(false);
            return;
        }
        setCaptureActivity(true);
        // A new accepted recording owns the state even during its countdown.
        // Earlier retained sources remain on disk; earlier exports cease being
        // eligible for this recording's Copy action.
        retainedSourcePath.clear();
        sessionWindowNames = {};
        finalizedOutputPath.clear();
        finalizedDurationMilliseconds = 0;
        automationError.clear();
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        if (startDelaySeconds > 0) {
            beginCountdown();
            return;
        }
        scheduleStart();
    }

    void beginCountdown() {
        const int seconds = std::clamp(startDelaySeconds, 1, 10);
        sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::countingDown();
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        // The busy status stops the motion preview and blocks recording-area
        // interactions before the countdown overlay appears.
        syncUi();
        // One clock drives both the overlay and the actual start, so the
        // displayed seconds can never drift away from the recording start.
        countdownTotalMilliseconds = static_cast<qint64>(seconds) * 1000;
        countdownElapsed.start();
        if (areaWindow != nullptr) {
            areaWindow->startCountdown(seconds);
        }
        countdownTimer.start();
    }

    void tickCountdown() {
        if (sessionStatus.busyOperation() !=
            ScreenshotToolPalette::RecordingBusyOperation::CountingDown) {
            countdownTimer.stop();
            return;
        }
        const qint64 remaining = countdownTotalMilliseconds - countdownElapsed.elapsed();
        if (remaining > 0) {
            if (areaWindow != nullptr) {
                areaWindow->updateCountdown(remaining);
            }
            return;
        }
        countdownTimer.stop();
        if (areaWindow != nullptr) {
            areaWindow->clearCountdown();
        }
        sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::idle();
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        scheduleStart();
    }

    bool allowRecording(bool notify) {
        const bool input = showKeyboard || recordMouseClicks || recordMouseScrolling ||
                           mouseTrailColor.alpha() != 0 || mouseClickColor.alpha() != 0;
        return !permissionCheck ||
               permissionCheck(outputFormat == QStringLiteral("mp4") && microphoneEnabled, input,
                               notify);
    }

    bool canNavigateAreaHistory() const {
        return recordingOptionsEditable() && !sessionEnding && !trimRequested && !trimSession &&
               uiSession && areaWindow->canRestoreRecordingRegion() &&
               toolbarWindow->palette()->recordingExportSettingsVisible();
    }

    void navigateAreaHistory(bool previous) {
        if (!canNavigateAreaHistory())
            return;
        const auto entries = snow_shot::storage::RecordingAreaHistorySettings().entries();
        if (areaHistoryIndex >= entries.size())
            areaHistoryIndex = -1;
        if (areaHistoryIndex < 0 && !previous)
            return;
        const qsizetype step = previous ? -1 : 1;
        qsizetype index = areaHistoryIndex < 0 ? entries.size() - 1 : areaHistoryIndex + step;
        for (; index >= 0 && index < entries.size(); index += step) {
            const auto& entry = entries.at(index);
            if (entry.coordinateSpace != snow_shot::storage::recordingAreaCoordinateSpace())
                continue;
            bool onDesktop = false;
            for (auto* screen : QGuiApplication::screens()) {
#ifdef Q_OS_MACOS
                const QRect bounds = screen->geometry();
#else
                const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
#endif
                onDesktop = onDesktop || bounds.intersects(entry.rectangle);
            }
            if (!onDesktop)
                continue;
            const QScopedValueRollback<bool> restoring(restoringAreaHistory, true);
            if (areaWindow->restoreRecordingRegion(entry.rectangle))
                areaHistoryIndex = index;
            return;
        }
    }

    void scheduleStart() {
        if (webcamFailed) {
            snow_webcam_retry(webcam.get());
            webcamFailed = false;
        }
        if (!allowRecording(true)) {
            setCaptureActivity(false);
            return;
        }
        setCaptureActivity(true);
        startScheduled = true;
        syncPreview();
        uiSession->preview->stopAndClear(true);
        operation = QUuid::createUuid().toString(QUuid::Id128);
        operationTimer.start();
        const quint64 generation = startGeneration;
        QTimer::singleShot(0, &owner, [this, generation]() {
            // An old callback must neither start a replacement session nor
            // clear the pending flag of a newer request.
            if (generation != startGeneration) {
                return;
            }
            startScheduled = false;
            if (sessionStatus.state() != ScreenshotToolPalette::RecordingState::Idle ||
                sessionStatus.busy() || recordingSession != nullptr || !isOpen()) {
                return;
            }
            sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::starting();
            pendingAreaHistory = std::make_shared<RecordingAreaHistoryCommit>();
            pendingAreaHistory->rectangle = recordingRegion;
            pendingAreaHistory->configuration =
                &snow_shot::storage::ApplicationStorage::instance().configuration();
            discardPendingStart = false;
            automationRevision = snow_shot::presentation::nextAutomationRevision();
            syncUi();

            // Snapshot every UI and storage value on the GUI thread; the worker
            // below must not touch either.
            const snow_shot::storage::RecordingSettings settings;
            sessionOutputSettings =
                directRecordingSettings(outputFormat, captureRegion.size(), automationOptions);
#ifdef Q_OS_MACOS
            sessionCaptureBackendPreference = QStringLiteral("auto");
#else
            sessionCaptureBackendPreference = settings.apiMode();
#endif
            sessionDeferred = postProcessingEnabled;
            const uint32_t overlay = postProcessingEffect == QStringLiteral("playback_time")
                                         ? SNOW_RECORDING_PLAYBACK_OVERLAY_PLAYBACK_TIME
                                         : SNOW_RECORDING_PLAYBACK_OVERLAY_PROGRESS_BAR;
            const uint32_t barColor = packedRgba(progressBarColor);
            sessionMouseTrailColor = mouseTrailColor;
            sessionMouseClickColor = mouseClickColor;
            sessionShowCursor = showCursor;
            const bool audioSupported = outputFormat == QStringLiteral("mp4");
            const RecordingKeyboardFont keyboardFont;
            const RecordingKeyboardTheme keyboardTheme(keyboardBackgroundColor,
                                                       keyboardForegroundColor);
            QVector<std::uint32_t> excludedWindowIds;
            if (!automationOptions.value(QStringLiteral("capture_toolbar"))
                     .toBool(settings.captureToolbarInRecording())) {
                excludeToolbarFromCapture();
                excludedWindowIds =
                    captureExclusion.windowIds(snow_shot::platform::captureWindowId);
            }
            const auto previewRetirement = audioPreviewRetirement;
            SnowCaptureDirectRecordingConfig config{
                SNOW_CAPTURE_DIRECT_RECORDING_CONFIG_VERSION,
                sizeof(SnowCaptureDirectRecordingConfig),
                captureRegion.x(),
                captureRegion.y(),
                static_cast<uint32_t>(captureRegion.width()),
                static_cast<uint32_t>(captureRegion.height()),
                recordingCaptureBackendPreference(settings),
                // Bound on the worker thread together with the keyboard labels.
                nullptr,
                static_cast<uint32_t>(sessionOutputSettings.format),
                static_cast<uint32_t>(
                    validRecordingFrameRate(automationOptions.value(QStringLiteral("frame_rate"))
                                                .toInt(settings.frameRate()))),
                sessionOutputSettings.targetFps,
                static_cast<uint32_t>(sessionOutputSettings.maximumSize.width()),
                static_cast<uint32_t>(sessionOutputSettings.maximumSize.height()),
                static_cast<uint32_t>(sessionOutputSettings.codec),
                static_cast<uint32_t>(sessionOutputSettings.preset),
                static_cast<uint32_t>(sessionOutputSettings.useHardwareEncoder
                                          ? SNOW_CAPTURE_ENCODER_PREFERENCE_HARDWARE
                                          : SNOW_CAPTURE_ENCODER_PREFERENCE_SOFTWARE),
                static_cast<uint8_t>(audioSupported && microphoneEnabled),
                static_cast<uint8_t>(audioSupported && systemAudioEnabled),
                static_cast<uint8_t>(sessionShowCursor),
                0,
                packedRgba(sessionMouseTrailColor),
                packedRgba(sessionMouseClickColor),
                {},
                static_cast<uint32_t>(showKeyboard),
                packedRgba(keyboardTheme.background),
                packedRgba(keyboardTheme.text),
                packedRgba(keyboardTheme.border),
                nullptr,
                0,
                static_cast<uint32_t>(mouseTrailDurationMs),
                static_cast<uint32_t>(keyboardSize),
                static_cast<uint32_t>(automationOptions.value(QStringLiteral("loop"))
                                          .toBool(settings.loopAnimatedImages())),
                {},
                mouseHighlightEnabled ? packedRgba(mouseHighlightColor) : 0u,
                static_cast<uint32_t>(recordMouseClicks),
                nullptr,
                nullptr,
                0u,
                sessionOutputSettings.quality,
                static_cast<uint32_t>(
                    audioSupported &&
                            automationOptions.value(QStringLiteral("separate_audio_tracks"))
                                .toBool(settings.separateAudioTracks())
                        ? SNOW_CAPTURE_RECORDING_AUDIO_SEPARATE
                        : SNOW_CAPTURE_RECORDING_AUDIO_MIXED),
                systemAudioGainDb,
                microphoneGainDb,
                static_cast<uint32_t>(mouseHighlightRadius),
                webcamEnabled ? webcam.get() : nullptr,
                static_cast<uint32_t>(webcamSize),
                webcamShape == QStringLiteral("circle") ? 1u : 0u,
                static_cast<uint32_t>(webcamOffset.x()),
                static_cast<uint32_t>(webcamOffset.y()),
                static_cast<uint32_t>(recordMouseScrolling),
                static_cast<uint32_t>(webcamOrientation),
            };
            sessionLoopAnimatedImages = config.loop_animated_images != 0;
            const QDateTime outputTimestamp = QDateTime::currentDateTime();
            sessionWindowNames =
                exportWindowNames(settings.videoSaveDirectory(), settings.videoFilenameFormat());
            const QString baseName = snow_shot::storage::expandScreenshotExportVariables(
                settings.videoFilenameFormat(), outputTimestamp, sessionWindowNames);
            const QStringList directories =
                snow_shot::presentation::recording::screenRecordingDirectories(outputTimestamp,
                                                                               sessionWindowNames);
            const QString extension = sessionOutputSettings.extension;
            const bool keyboard = showKeyboard || recordMouseClicks || recordMouseScrolling;
            const QString requestedPath =
                automationOptions.value(QStringLiteral("path")).toString();
            startupRecovery =
                std::make_shared<snow_shot::presentation::recording::StartupRecovery>();
            // Session creation blocks on capture, audio, hooks, and encoder
            // initialization; keep it off the GUI thread so the busy state can
            // paint. The FFI error string is thread-local, so it is read here.
            startFuture = std::async(
                std::launch::async,
                snow_shot::runtime::trackRuntimeWork(
                    [config, excludedWindowIds, directories, baseName, extension, keyboard,
                     keyboardFont, requestedPath, previewRetirement, deferred = sessionDeferred,
                     overlay, barColor, recovery = startupRecovery, webcamLease = webcam,
                     areaHistory = pendingAreaHistory]() mutable -> StartAttemptResult {
                        Q_UNUSED(webcamLease);
                        if (previewRetirement.valid())
                            previewRetirement.wait();
                        snow_shot::platform::applyApplicationQoSToCurrentThread();
                        StartAttemptResult result;
                        result.session.get_deleter().activity =
                            snow_shot::runtime::RuntimeActivityTracker::shared().acquire();
                        result.outputPath =
                            requestedPath.isEmpty()
                                ? snow_shot::presentation::recording::screenRecordingOutputPath(
                                      directories, baseName, extension)
                                : requestedPath;
                        // The recording exporter owns its private staging file and
                        // publishes only after finalization. Reserving the destination
                        // here would make that publication reject our own empty file.
                        if (result.outputPath.isEmpty()) {
                            result.error = QCoreApplication::translate(
                                "ScreenRecordingController",
                                "Unable to create the recording directory");
                            return result;
                        }
                        static_cast<void>(recovery->publishOutput(result.outputPath));
                        const QByteArray outputUtf8 =
                            QDir::toNativeSeparators(result.outputPath).toUtf8();
                        const RecordingKeyboardLabels labels(keyboard);
                        keyboardFont.applyTo(config);
                        config.output_file_utf8 = outputUtf8.constData();
                        config.keyboard_labels = labels.entries.constData();
                        config.keyboard_label_count = static_cast<uint32_t>(labels.entries.size());
                        config.exclusions.windows = excludedWindowIds.constData();
                        config.exclusions.window_count =
                            static_cast<size_t>(excludedWindowIds.size());
                        SnowRecordingSession* created = nullptr;
                        const SnowRecordingDeferredOptions deferredOptions{
                            SNOW_RECORDING_DEFERRED_OPTIONS_VERSION,
                            sizeof(SnowRecordingDeferredOptions), overlay, barColor, nullptr};
                        const SnowRecordingResult createResult =
                            deferred ? snow_recording_session_create_deferred(
                                           &config, &deferredOptions, &created)
                                     : snow_recording_session_create_direct(&config, &created);
                        result.session.reset(created);
                        if (createResult != SNOW_RECORDING_RESULT_OK || result.session == nullptr ||
                            snow_recording_session_start(result.session.get()) == 0) {
                            result.error = captureError();
                            return result;
                        }
                        areaHistory->started.store(true);
                        if (areaHistory->configuration) {
                            QMetaObject::invokeMethod(
                                areaHistory->configuration,
                                [areaHistory] { areaHistory->commit(); }, Qt::QueuedConnection);
                        }
                        return result;
                    }));
            startPollTimer.start();
        });
    }

    void pollStart() {
        if (!startFuture.valid() ||
            startFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            return;
        }
        startPollTimer.stop();
        StartAttemptResult result = startFuture.get();
        if (pendingAreaHistory) {
            pendingAreaHistory->commit();
            if (pendingAreaHistory->saved)
                areaHistoryIndex = -1;
        }
        if (discardPendingStart) {
            discardPendingStart = false;
            recordingSession = std::move(result.session);
            if (recordingSession != nullptr) {
                discard();
            } else {
                sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::idle();
                automationRevision = snow_shot::presentation::nextAutomationRevision();
                restoreToolbarCaptureVisibility();
            }
            return;
        }
        if (uiSession == nullptr) {
            // The UI was retired while the backend was starting. A session that
            // did start is shut down through the regular finalization path; any
            // other attempt is destroyed when this result is dropped.
            if (result.session != nullptr && result.error.isEmpty()) {
                recordingSession = std::move(result.session);
                pendingOutputPath = result.outputPath;
                sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::recording();
                automationRevision = snow_shot::presentation::nextAutomationRevision();
                stop(false);
                return;
            }
            result.session.reset();
            restoreToolbarCaptureVisibility();
            sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::idle();
            automationRevision = snow_shot::presentation::nextAutomationRevision();
            if (!result.error.isEmpty()) {
                report(QStringLiteral("recording.failed"), QtWarningMsg, result.error);
            }
            return;
        }
        if (result.session == nullptr || !result.error.isEmpty()) {
            result.session.reset();
            restoreToolbarCaptureVisibility();
            sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::idle();
            automationRevision = snow_shot::presentation::nextAutomationRevision();
            syncUi();
            if (areaWindow != nullptr) {
                areaWindow->show();
                areaWindow->raise();
            }
            if (toolbarWindow != nullptr) {
                toolbarWindow->show();
                toolbarWindow->raise();
            }
            showError(result.error);
            return;
        }

        recordingSession = std::move(result.session);
        pendingOutputPath = result.outputPath;
        durationMilliseconds = 0;
        sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::recording();
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        syncUi();
        if (areaWindow != nullptr) {
            areaWindow->show();
            areaWindow->raise();
        }
        if (toolbarWindow != nullptr) {
            showRecordingControls();
        }
        durationTimer.start();
        if (toolbarSurfaceGuard)
            toolbarSurfaceGuard->refresh();
        if (toolbarWindow)
            toolbarWindow->refreshAutoHideControl();
        report(QStringLiteral("recording.started"));
    }

    void pause() {
        if (recordingSession == nullptr ||
            sessionStatus.state() != ScreenshotToolPalette::RecordingState::Recording ||
            sessionStatus.busy()) {
            return;
        }
        if (snow_recording_session_pause(recordingSession.get()) == 0) {
            showError(captureError());
            return;
        }
        sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::paused();
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        report(QStringLiteral("recording.paused"));
        syncUi();
    }

    void resume() {
        if (recordingSession == nullptr ||
            sessionStatus.state() != ScreenshotToolPalette::RecordingState::Paused ||
            sessionStatus.busy()) {
            return;
        }
        if (snow_recording_session_resume(recordingSession.get()) == 0) {
            showError(captureError());
            return;
        }
        sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::recording();
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        report(QStringLiteral("recording.resumed"));
        durationTimer.start();
        syncUi();
    }

    bool exitTrim() {
        if (!trimSession || !trimSession->ready() || !uiSession)
            return false;
        trimSession->detach();
        trimSession = nullptr;
        trimRequested = false;
        trimSaveWhenReady = false;
        areaWindow->setTrimming(false);
        areaWindow->setRecordingRegion(recordingRegion);
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        syncUi();
        return true;
    }

    void beginTrim(bool save) {
        if (trimRequested || sessionStatus.busy() || !recordingSession || !uiSession)
            return;
        trimRequested = true;
        trimSaveWhenReady = save;
        stop(false);
        uiSession->preview->stopAndClear();
        trimSession = new RecordingTrimSession(areaWindow, toolbarWindow, &owner);
        trimSession->reportError = [this](const QString& error) { showError(error); };
        trimSession->exported = [this, session = trimSession](const QString& path) {
            if (trimSession == session)
                autoExitAfterRecordingEnds();
            notifyExportCompleted(path);
        };
    }

    void stop(bool copyToClipboard) {
        if (sessionStatus.busy()) {
            return;
        }
        if (sessionStatus.state() == ScreenshotToolPalette::RecordingState::Idle ||
            recordingSession == nullptr) {
            return;
        }

        // Freeze the media endpoint before UI teardown or worker scheduling.
        static_cast<void>(snow_recording_session_request_stop(recordingSession.get()));
        setCaptureActivity(false);
        stopAudioMeter();
        exclusionPollTimer.stop();
        durationTimer.stop();
        // Keep the final duration on screen while the file is finalized;
        // pollFinalization resets it once the operation completes.
        sessionStatus = sessionStatus.finishing(copyToClipboard);
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        syncUi();
        // The member keeps ownership; pollFinalization destroys the session
        // only after the asynchronous stop has joined the worker.
        SnowRecordingSession* session = recordingSession.get();
        finalizationFuture = std::async(
            std::launch::async,
            snow_shot::runtime::trackRuntimeWork([session, deferred = sessionDeferred] {
                snow_shot::platform::applyApplicationQoSToCurrentThread();
                FinalizationResult result;
                result.ok =
                    (deferred ? snow_recording_session_finalize_deferred(session, &result.source)
                              : snow_recording_session_stop(session)) == SNOW_RECORDING_RESULT_OK;
                if (!result.ok)
                    result.error = captureError();
                return result;
            }));
        finalizationPollTimer.start();
    }

    void pollFinalization() {
        if (!finalizationFuture.valid() || finalizationFuture.wait_for(std::chrono::milliseconds(
                                               0)) != std::future_status::ready) {
            return;
        }
        finalizationPollTimer.stop();
        const FinalizationResult result = finalizationFuture.get();
        recordingSession.reset();
        restoreToolbarCaptureVisibility();
        if (trimRequested) {
            sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::idle();
            QString sourcePath = pendingOutputPath;
            if (result.source) {
                const size_t length = snow_recording_source_path(result.source, nullptr, 0);
                if (length > 0 && length < 1024 * 1024) {
                    QByteArray path(static_cast<qsizetype>(length), '\0');
                    snow_recording_source_path(result.source, path.data(), length);
                    sourcePath = QString::fromUtf8(path.constData());
                    retainedSourcePath = sourcePath;
                }
                snow_recording_source_destroy(result.source);
            }
            syncUi();
            if (!result.ok) {
                if (trimSession)
                    trimSession->detach();
                trimSession = nullptr;
                showError(result.error);
                return;
            }
            if (trimSession) {
                const auto& settings = sessionOutputSettings;
                const SnowRecordingClipOptions options{1,
                                                       sizeof(SnowRecordingClipOptions),
                                                       static_cast<uint32_t>(settings.format),
                                                       static_cast<uint32_t>(settings.codec),
                                                       static_cast<uint32_t>(settings.preset),
                                                       settings.quality,
                                                       settings.targetFps,
                                                       settings.useHardwareEncoder ? 1u : 0u,
                                                       sessionLoopAnimatedImages ? 1u : 0u};
                trimSession->open(sourcePath, pendingOutputPath, options, sessionDeferred,
                                  trimSaveWhenReady, sessionWindowNames);
            }
            return;
        }
        if (result.ok && result.source) {
            QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(recordingRegion);
            // Keep placement independent of the selection windows, which may already be closed.
            QRect anchorGeometry;
            if (areaWindow) {
                anchorGeometry = areaWindow->frameGeometry();
            } else {
#ifdef Q_OS_MACOS
                anchorGeometry = recordingRegion;
#else
                anchorGeometry =
                    ScreenshotGeometryMapper::logicalRectForPhysicalRect(recordingRegion, screen);
#endif
            }
            renderJob = new RecordingRenderJob(result.source, !automationOwned, screen, &owner,
                                               anchorGeometry, areaWindow);
            renderJob->changed = [this, job = renderJob] {
                if (renderJob != job)
                    return;
                automationError = renderJob->error();
                automationRevision = snow_shot::presentation::nextAutomationRevision();
            };
            renderJob->finished = [this, job = renderJob](RecordingRenderJob::Outcome outcome) {
                if (renderJob != job)
                    return;
                auto* completed = renderJob;
                const auto state = completed->state();
                retainedSourcePath = outcome == RecordingRenderJob::Outcome::Kept
                                         ? completed->sourcePath()
                                         : QString();
                const QString error = completed->error();
                const qint64 renderedDuration =
                    state.value(QStringLiteral("render_duration_ms")).toInteger();
                renderJob = nullptr;
                completed->deleteLater();
                finishExport(outcome == RecordingRenderJob::Outcome::Succeeded, error,
                             renderedDuration, false);
            };
            renderJob->start();
            return;
        }
        if (result.source)
            snow_recording_source_destroy(result.source);
        finishExport(result.ok, result.error, 0, true);
    }

    void finishExport(bool ok, const QString& error, qint64 renderedDuration, bool notifyError) {
        const QString completedPath = pendingOutputPath;
        const bool shouldCopy =
            sessionStatus.busyOperation() == ScreenshotToolPalette::RecordingBusyOperation::Copying;
        sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::idle();
        automationError = error;
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        if (ok) {
            report(QStringLiteral("recording.export_finished"));
            finalizedDurationMilliseconds =
                renderedDuration > 0 ? renderedDuration : durationMilliseconds;
        }
        durationMilliseconds = 0;
        syncUi();
        if (!ok) {
            if (notifyError)
                showError(error);
            return;
        }
        finalizedOutputPath = completedPath;
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        autoExitAfterRecordingEnds();
        // Finalized observers can destroy the controller or start a new session.
        // Keep the original Copy intent and its path independent of that callback.
        const QPointer<ScreenRecordingController> guard(&owner);
        notifyExportCompleted(completedPath);
        if (guard)
            emit guard->finalized();
        if (shouldCopy)
            copyFileToClipboard(completedPath);
    }

    void notifyExportCompleted(const QString& path) {
        if (snow_shot::storage::RecordingSettings().notifyAfterExportCompletes())
            emit owner.exportNotificationRequested(path);
    }

    void autoExitAfterRecordingEnds() {
        if (snow_shot::storage::RecordingSettings().autoExitAfterRecordingEnds())
            destroyUi();
    }

    bool pollSessionLiveness() {
        if (recordingSession == nullptr || sessionStatus.busy() ||
            sessionStatus.state() == ScreenshotToolPalette::RecordingState::Idle) {
            return false;
        }
        SnowRecordingState nativeState = SNOW_RECORDING_STATE_CREATED;
        if (snow_recording_session_state(recordingSession.get(), &nativeState) == 0 ||
            nativeState != SNOW_RECORDING_STATE_STOPPED) {
            return false;
        }
        stop(false);
        return true;
    }

    void openFolder() {
        static_cast<void>(
            snow_shot::presentation::recording::openScreenRecordingFolder(sessionWindowNames));
    }

    void offerRecovery() {
        using namespace snow_shot::presentation::recording;
        if (recoveryOffered || sessionEnding)
            return;
        recoveryOffered = true;
        const QString directory = snow_shot::app::distribution::writableDataDirectory();
        if (directory.isEmpty())
            return;
        const QString marker = QDir(directory).filePath(QStringLiteral("recording-recovery.json"));
        auto record = loadRecovery(marker);
        if (!record)
            return;
        *record = refreshRecoveryStaging(std::move(*record));
        const QString existing = QFileInfo(record->sourcePath).isFile()   ? record->sourcePath
                                 : QFileInfo(record->outputPath).isFile() ? record->outputPath
                                                                          : QString();
        const QString folder = recoveryDirectory(*record);
        const QString openTarget = !existing.isEmpty()                ? existing
                                   : record->stagingPaths.size() == 1 ? record->stagingPaths.front()
                                                                      : folder;
        if (existing.isEmpty() && !QFileInfo(folder).isDir())
            return;
        auto* notice =
            new QMessageBox(QMessageBox::Information, tr("Recording recovery"),
                            tr("A recording was interrupted when Windows ended the "
                               "session. Recover the saved recording or open its folder."),
                            QMessageBox::NoButton, QApplication::activeWindow());
        notice->setAttribute(Qt::WA_DeleteOnClose);
        QStringList details{record->sourcePath, record->outputPath, folder};
        details.append(record->stagingPaths);
        details.removeAll(QString());
        details.removeDuplicates();
        notice->setDetailedText(details.join(QLatin1Char('\n')));
        QAbstractButton* recover = nullptr;
        if (QFileInfo(record->sourcePath).isFile() &&
            record->sourcePath.endsWith(QStringLiteral(".snowrec"), Qt::CaseInsensitive))
            recover = notice->addButton(tr("Recover"), QMessageBox::AcceptRole);
        auto* openFolder = notice->addButton(tr("Open folder"), QMessageBox::ActionRole);
        notice->addButton(QMessageBox::Close);
        QObject::connect(
            notice, &QMessageBox::buttonClicked, &owner,
            [this, record = *record, marker, openTarget, recover,
             openFolder](QAbstractButton* button) {
                if (button == openFolder) {
                    static_cast<void>(snow_shot::platform::revealInFileManager(openTarget));
                    return;
                }
                if (button != recover || sessionEnding || isOpen())
                    return;
                auto* screen = QGuiApplication::primaryScreen();
                if (!screen)
                    return;
#ifdef Q_OS_MACOS
                const QRect region = screen->availableGeometry();
#else
                const QRect region = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
#endif
                open(region);
                if (!uiSession)
                    return;
                trimRequested = true;
                retainedSourcePath = record.sourcePath;
                pendingOutputPath =
                    record.outputPath.isEmpty()
                        ? QFileInfo(record.sourcePath)
                              .dir()
                              .filePath(QFileInfo(record.sourcePath).completeBaseName() +
                                        QStringLiteral(".mp4"))
                        : record.outputPath;
                uiSession->preview->stopAndClear();
                trimSession = new RecordingTrimSession(areaWindow, toolbarWindow, &owner);
                trimSession->reportError = [this](const QString& error) { showError(error); };
                trimSession->exported = [this, marker, record](const QString& path) {
                    static_cast<void>(clearRecovery(marker, record));
                    notifyExportCompleted(path);
                };
                trimSession->open(record.sourcePath, pendingOutputPath, {}, true, false, {});
            });
        notice->open();
    }

    snow_shot::storage::ScreenshotWindowNames exportWindowNames(const QString& directory,
                                                                const QString& filename) const {
        const auto fields = snow_shot::storage::screenshotWindowNameFields(directory, filename);
        if (!fields.any())
            return {};
        // Naming ignores recording UI even when those surfaces are included in the video.
        // Inspect existing identities only; lookup must not materialize hidden controls.
        QVector<std::uintptr_t> excluded;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            bool recordingSurface = false;
            for (QWidget* parent = widget; parent; parent = parent->parentWidget())
                recordingSurface |= parent == areaWindow || parent == toolbarWindow;
            for (QWindow* window = widget->windowHandle(); window;
                 window = window->transientParent())
                recordingSurface |= (areaWindow && window == areaWindow->windowHandle()) ||
                                    (toolbarWindow && window == toolbarWindow->windowHandle());
            if (!recordingSurface || !widget->internalWinId())
                continue;
#ifdef Q_OS_MACOS
            if (const auto id = snow_shot::platform::captureWindowId(widget))
                excluded.push_back(*id);
#else
            excluded.push_back(static_cast<std::uintptr_t>(widget->internalWinId()));
#endif
        }
        // recordingRegion already uses native desktop units on both supported platforms.
        return snow_shot::platform::screenshotWindowNamesAtPoint(
            QRectF(recordingRegion).center(),
            {excluded.constData(), static_cast<size_t>(excluded.size())}, fields);
    }

    void discard() {
        setCaptureActivity(false);
        stopAudioMeter();
        exclusionPollTimer.stop();
        durationTimer.stop();
        sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::stopping();
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        syncUi();
        // Destroy cancels capture and removes only the session's private staging
        // files. It can join native workers, so retain ownership off the GUI thread.
        discardFuture = std::async(
            std::launch::async,
            snow_shot::runtime::trackRuntimeWork([session = std::move(recordingSession)]() mutable {
                snow_shot::platform::applyApplicationQoSToCurrentThread();
                session.reset();
            }));
        finalizationPollTimer.start();
    }

    void pollDiscard() {
        if (!discardFuture.valid() ||
            discardFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return;
        finalizationPollTimer.stop();
        discardFuture.get();
        pendingOutputPath.clear();
        durationMilliseconds = 0;
        sessionStatus = ScreenshotToolPalette::RecordingSessionStatus::idle();
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        restoreToolbarCaptureVisibility();
        syncUi();
    }

    void close() {
        if (sessionEnding)
            return;
        // Snapshot the preference for an in-flight start while its UI still
        // exists. Repeated Close commands must not change that decision.
        if (startFuture.valid() && uiSession != nullptr) {
            discardPendingStart =
                snow_shot::storage::RecordingSettings().deleteFilesOnRecordingClose();
            if (discardPendingStart)
                setCaptureActivity(false);
        }
        if (recordingSession != nullptr && !sessionStatus.busy() &&
            snow_shot::storage::RecordingSettings().deleteFilesOnRecordingClose()) {
            discard();
        } else {
            stop(false);
        }
        destroyUi();
    }

    void destroyUi() {
        if (!recordingSession)
            setCaptureActivity(false);
        if (trimSession) {
            trimSession->detach();
            trimSession = nullptr;
        }
        stopAudioMeter();
        if (toolbarWindow) {
            toolbarWindow->dismissAutoHide();
            toolbarWindow->setAutoHideControlShowGuard({});
            toolbarWindow->palette()->closeRecordingAudioGainPopovers();
        }
        cancelPendingStart();
        if (uiSession == nullptr) {
            restoreToolbarCaptureVisibility();
            return;
        }
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        auto* retiring = uiSession;
        if (retiring->settingsModal)
            retiring->settingsModal->close();
        if (renderJob)
            renderJob->detachWindowOwner();
        webcamTimer.stop();
        if (webcam)
            snow_webcam_set_active(webcam.get(), 0);
        uiSession = nullptr;
        areaWindow = nullptr;
        toolbarWindow = nullptr;
        // Disconnect before hiding: hide/focus events can emit canvas and geometry signals.
        retiring->preview->setEligible(false);
        retiring->preview->stopAndClear();
        retiring->connections.reset();
        retiring->colorSampler.reset();
        retiring->shortcuts.reset();
        retiring->toolbar->hide();
        retiring->area->hide();
        // Capture may still be joining on its worker. Hide every retained surface
        // before clearing the exclusion policies and native show guards.
        restoreToolbarCaptureVisibility();
        // Close can originate in a toolbar button's event handler.
        retiring->deleteLater();
    }

    bool excludeToolbarFromCapture() {
        // Reveal an already-collapsed control before changing capture policy.
        // Restoration also refreshes pending transitions, so inhibit auto-hide
        // until both retained windows have their new exclusion policy applied.
        QObject policyTransition;
        if (toolbarWindow)
            toolbarWindow->setAutoHideBlocked(&policyTransition, true);
        restoreToolbarCaptureVisibility();
        toolbarPopupsExcluded = true;
        const bool excluded = captureExclusion.exclude(toolbarWindow);
        if (toolbarWindow) {
            // Create the retained native surface before collecting the capture
            // configuration, even when auto-hide is currently disabled.
            static_cast<void>(captureExclusion.exclude(toolbarWindow->autoHideControlWindow()));
            toolbarSurfaceGuard = std::make_unique<adqt::widgets::AdPopupSurfaceGuard>(
                toolbarWindow, [this](QWidget* surface) { return guardToolbarSurface(surface); });
            QObject::connect(toolbarSurfaceGuard.get(),
                             &adqt::widgets::AdPopupSurfaceGuard::surfaceRequested, &owner,
                             [this](QWidget*) { toolbarExclusions.retry(); });
        }
        if (toolbarSurfaceGuard)
            toolbarSurfaceGuard->refresh();
        return excluded;
    }

    void restoreToolbarCaptureVisibility() {
        exclusionPollTimer.stop();
        toolbarPopupsExcluded = false;
        toolbarExclusionErrorReported = false;
        toolbarExclusions = {};
        captureExclusion.restore();
        toolbarSurfaceGuard.reset();
        if (toolbarWindow)
            toolbarWindow->refreshAutoHideControl();
    }

    QJsonObject currentAutomationOptions() const {
        return {
            {QStringLiteral("region"),
             QJsonArray{recordingRegion.x(), recordingRegion.y(), recordingRegion.width(),
                        recordingRegion.height()}},
            {QStringLiteral("format"), outputFormat},
            {QStringLiteral("post_processing"), postProcessingEnabled},
            {QStringLiteral("post_processing_effect"), postProcessingEffect},
            {QStringLiteral("progress_bar_color"),
             snow_shot::storage::colorToRgbaString(progressBarColor)},
            {QStringLiteral("microphone"), microphoneEnabled},
            {QStringLiteral("system_audio"), systemAudioEnabled},
            {QStringLiteral("system_audio_gain_db"), systemAudioGainDb},
            {QStringLiteral("microphone_gain_db"), microphoneGainDb},
            {QStringLiteral("show_cursor"), showCursor},
            {QStringLiteral("show_keyboard"), showKeyboard},
            {QStringLiteral("webcam"), webcamEnabled},
            {QStringLiteral("webcam_size"), webcamSize},
            {QStringLiteral("webcam_offset_left"), webcamOffset.x()},
            {QStringLiteral("webcam_offset_bottom"), webcamOffset.y()},
            {QStringLiteral("webcam_shape"), webcamShape},
            {QStringLiteral("webcam_orientation"), webcamOrientation},
            {QStringLiteral("record_mouse_clicks"), recordMouseClicks},
            {QStringLiteral("record_mouse_scrolling"), recordMouseScrolling},
            {QStringLiteral("mouse_highlight"), mouseHighlightEnabled},
            {QStringLiteral("start_delay_seconds"), startDelaySeconds},
            {QStringLiteral("keyboard_size"), keyboardSize},
            {QStringLiteral("mouse_trail_duration_ms"), mouseTrailDurationMs},
            {QStringLiteral("mouse_trail"), mouseTrailColor.name(QColor::HexArgb)},
            {QStringLiteral("mouse_click"), mouseClickColor.name(QColor::HexArgb)},
            {QStringLiteral("mouse_highlight_color"), mouseHighlightColor.name(QColor::HexArgb)},
            {QStringLiteral("mouse_highlight_radius"), mouseHighlightRadius},
            {QStringLiteral("keyboard_background"), keyboardBackgroundColor.name(QColor::HexArgb)},
            {QStringLiteral("keyboard_foreground"), keyboardForegroundColor.name(QColor::HexArgb)}};
    }

    void retireAudioPreview() {
        if (!audioPreview)
            return;
        auto* retiring = std::exchange(audioPreview, nullptr);
        audioPreviewSource = -1;
        snow_recording_audio_monitor_cancel(retiring);
        std::promise<void> completion;
        audioPreviewRetirement = completion.get_future().share();
        std::thread(snow_shot::runtime::trackRuntimeWork([retiring, completion = std::move(
                                                                        completion)]() mutable {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            snow_recording_audio_monitor_destroy(retiring);
            completion.set_value();
        })).detach();
    }

    void stopAudioMeter() {
        audioMeterTimer.stop();
        if (recordingSession && !sessionStatus.busy())
            static_cast<void>(snow_recording_session_set_audio_metering(recordingSession.get(), 0));
        retireAudioPreview();
    }

    void syncAudioMeter() {
        auto* palette = toolbarWindow ? toolbarWindow->palette() : nullptr;
        int visibleSource = -1;
        if (palette && !sessionStatus.busy() && !startScheduled &&
            outputFormat == QStringLiteral("mp4")) {
            for (bool microphone : {false, true}) {
                auto* popup = palette->recordingAudioGainPopover(microphone);
                if (popup && popup->popover()->isVisible() && popup->popover()->surfaceWidget() &&
                    popup->popover()->surfaceWidget()->isVisible())
                    visibleSource = microphone ? 1 : 0;
            }
        }
        if (visibleSource < 0) {
            stopAudioMeter();
            audioMeterSource = -1;
            return;
        }
        audioMeterSource = visibleSource;
        const bool enabled = visibleSource == 1 ? microphoneEnabled : systemAudioEnabled;
        if (audioPreview && (recordingSession || audioPreviewSource != visibleSource || !enabled))
            retireAudioPreview();
        if (recordingSession)
            static_cast<void>(snow_recording_session_set_audio_metering(
                recordingSession.get(), enabled ? (1u << visibleSource) : 0u));
        if (!enabled) {
            palette->recordingAudioGainPopover(visibleSource == 1)
                ->setLevel(0, false, RecordingAudioGainPopover::LevelStatus::AudioOff);
            audioMeterTimer.stop();
        } else {
            if (!audioMeterTimer.isActive()) {
                audioMeterTimer.start();
                pollAudioMeter();
            }
        }
    }

    void applyAudioGain(bool microphone, int gainDb) {
        setOption(microphone ? microphoneGainDb : systemAudioGainDb, gainDb);
        if (recordingSession)
            static_cast<void>(snow_recording_session_set_audio_gain(
                recordingSession.get(),
                microphone ? SNOW_RECORDING_AUDIO_MICROPHONE : SNOW_RECORDING_AUDIO_SYSTEM,
                gainDb));
        if (audioPreview && audioPreviewSource == (microphone ? 1 : 0))
            static_cast<void>(snow_recording_audio_monitor_set_gain(audioPreview, gainDb));
    }

    void refreshAudioGains() {
        if (!audioSettingsRefreshPending || sessionStatus.busy() || automationOwned ||
            automationNextStart)
            return;
        audioSettingsRefreshPending = false;
        const snow_shot::storage::RecordingSettings settings;
        if (microphoneGainDb != settings.microphoneGainDb())
            applyAudioGain(true, settings.microphoneGainDb());
        if (systemAudioGainDb != settings.systemAudioGainDb())
            applyAudioGain(false, settings.systemAudioGainDb());
        if (toolbarWindow) {
            toolbarWindow->palette()->setRecordingMicrophoneGainDb(microphoneGainDb);
            toolbarWindow->palette()->setRecordingSystemAudioGainDb(systemAudioGainDb);
        }
    }

    void changeAudioGain(bool microphone, int gainDb) {
        if (gainDb < -24 || gainDb > 24 || sessionStatus.busy())
            return;
        applyAudioGain(microphone, gainDb);
        if (!automationOwned) {
            const snow_shot::storage::RecordingSettings settings;
            if (microphone)
                settings.setMicrophoneGainDb(gainDb);
            else
                settings.setSystemAudioGainDb(gainDb);
        }
    }

    void pollAudioMeter() {
        if (audioMeterSource < 0 || !toolbarWindow || sessionStatus.busy())
            return;
        auto* control = toolbarWindow->palette()->recordingAudioGainPopover(audioMeterSource == 1);
        if (!control || !control->popover()->isVisible() || !control->popover()->surfaceWidget() ||
            !control->popover()->surfaceWidget()->isVisible()) {
            stopAudioMeter();
            return;
        }
        SnowRecordingAudioLevels levels{};
        bool available = false;
        if (recordingSession)
            available =
                snow_recording_session_take_audio_levels(recordingSession.get(), &levels) != 0;
        else {
            if (!audioPreview) {
                if (audioPreviewRetirement.valid() &&
                    audioPreviewRetirement.wait_for(std::chrono::milliseconds(0)) !=
                        std::future_status::ready) {
                    control->setLevel(0, false, RecordingAudioGainPopover::LevelStatus::Starting);
                    return;
                }
                const auto source = audioMeterSource == 1 ? SNOW_RECORDING_AUDIO_MICROPHONE
                                                          : SNOW_RECORDING_AUDIO_SYSTEM;
                const int gain = audioMeterSource == 1 ? microphoneGainDb : systemAudioGainDb;
                if (snow_recording_audio_monitor_create(source, gain, &audioPreview) !=
                    SNOW_RECORDING_RESULT_OK) {
                    control->setLevel(0, false,
                                      RecordingAudioGainPopover::LevelStatus::Unavailable);
                    audioMeterTimer.stop();
                    return;
                }
                audioPreviewSource = audioMeterSource;
                static_cast<void>(snow_recording_audio_monitor_set_metering(audioPreview, 1));
            }
            available = snow_recording_audio_monitor_take_levels(audioPreview, &levels) != 0;
        }
        const auto& level = audioMeterSource == 1 ? levels.microphone : levels.system_audio;
        const qint64 now = audioMeterClock.elapsed();
        const double elapsed = static_cast<double>(std::max<qint64>(0, now - lastAudioMeterTick));
        lastAudioMeterTick = now;
        const bool ready = available && level.status == SNOW_RECORDING_AUDIO_READY;
        if (!ready || level.age_ms > 200)
            displayedAudioPeak = 0;
        else
            displayedAudioPeak = std::max(static_cast<double>(level.peak),
                                          displayedAudioPeak * std::exp(-elapsed / 200.0));
        if (ready && level.clipped)
            audioClipDeadline = now + 1000;
        auto status = RecordingAudioGainPopover::LevelStatus::Live;
        if (!available || level.status == SNOW_RECORDING_AUDIO_UNAVAILABLE ||
            level.status == SNOW_RECORDING_AUDIO_STOPPED)
            status = RecordingAudioGainPopover::LevelStatus::Unavailable;
        else if (level.status == SNOW_RECORDING_AUDIO_PERMISSION_DENIED)
            status = RecordingAudioGainPopover::LevelStatus::PermissionRequired;
        else if (level.status == SNOW_RECORDING_AUDIO_STARTING ||
                 level.status == SNOW_RECORDING_AUDIO_RECONNECTING)
            status = RecordingAudioGainPopover::LevelStatus::Starting;
        else if (level.status == SNOW_RECORDING_AUDIO_DISABLED)
            status = RecordingAudioGainPopover::LevelStatus::AudioOff;
        control->setLevel(displayedAudioPeak, ready && now < audioClipDeadline, status);
    }

    bool guardToolbarSurface(QWidget* surface) {
        if (!toolbarPopupsExcluded)
            return true;
        if (!surface || !snow_shot::platform::setWindowExcludedFromCapture(surface, true)) {
            reportToolbarExclusionError();
            return false;
        }
        static_cast<void>(captureExclusion.exclude(surface));
#ifdef Q_OS_MACOS
        if (!recordingSession || sessionStatus.busy())
            return false;
        const auto ids = captureExclusion.windowIds(snow_shot::platform::captureWindowId);
        const auto windowId = snow_shot::platform::captureWindowId(surface);
        if (!windowId) {
            reportToolbarExclusionError();
            return false;
        }
        const bool canShow = toolbarExclusions.canShow(*windowId, ids, [this](const auto& windows) {
            SnowCaptureExclusions exclusions{windows.constData(),
                                             static_cast<size_t>(windows.size()), nullptr, 0};
            uint64_t generation = 0;
            if (!snow_recording_session_request_exclusions(
                    recordingSession.get(), &exclusions, windows.constData(),
                    static_cast<uint32_t>(windows.size()), &generation)) {
                reportToolbarExclusionError();
                return uint64_t{0};
            }
            return generation;
        });
        if (toolbarExclusions.pending() && !exclusionPollTimer.isActive())
            exclusionPollTimer.start();
        return canShow;
#else
        return true;
#endif
    }

    void showRecordingWarning(const QString& key, const QString& content) {
        if (!areaWindow)
            return;
        adqt::widgets::AdMessage::Request request;
        request.key = key;
        request.content = content;
        adqt::widgets::AdMessageService::warning(std::move(request), areaWindow);
    }

    void reportToolbarExclusionError() {
        if (toolbarExclusionErrorReported || !areaWindow)
            return;
        toolbarExclusionErrorReported = true;
        showRecordingWarning(
            QStringLiteral("recording-toolbar-exclusion-error"),
            QCoreApplication::translate("ScreenRecordingController",
                                        "Unable to exclude toolbar controls from recording"));
    }

    void pollToolbarExclusions() {
#ifdef Q_OS_MACOS
        if (!recordingSession || sessionStatus.busy() || !toolbarExclusions.pending()) {
            exclusionPollTimer.stop();
            return;
        }
        SnowRecordingExclusionStatus status{};
        if (!snow_recording_session_exclusion_status(recordingSession.get(), &status))
            return;
        const auto completion = toolbarExclusions.complete(
            status.requested_generation, status.applied_generation, status.status);
        if (completion == snow_shot::presentation::RecordingPopupExclusions::Completion::Pending)
            return;
        exclusionPollTimer.stop();
        if (completion == snow_shot::presentation::RecordingPopupExclusions::Completion::Failed)
            reportToolbarExclusionError();
        // Refresh on failure too: a native identity may have changed while the
        // failed request was in flight. Unchanged failed snapshots stay blocked.
        if (toolbarSurfaceGuard)
            toolbarSurfaceGuard->refresh();
        if (toolbarWindow)
            toolbarWindow->refreshAutoHideControl();
#endif
    }

    bool recordingOptionsEditable() const {
        return sessionStatus.state() == ScreenshotToolPalette::RecordingState::Idle &&
               !sessionStatus.busy() && !startScheduled && !recordingSession;
    }

    void refreshWebcamSettings() {
        if (!webcamSettingsRefreshPending || automationOwned || automationNextStart ||
            !recordingOptionsEditable())
            return;
        webcamSettingsRefreshPending = false;
        if (areaWindow)
            areaWindow->cancelWebcamInteraction();
        const snow_shot::storage::RecordingSettings saved;
        const bool enabledChanged = webcamEnabled != saved.webcamEnabled();
        setOption(webcamEnabled, saved.webcamEnabled());
        setOption(webcamSize, saved.webcamSize());
        setOption(webcamOffset, QPoint(saved.webcamOffsetLeft(), saved.webcamOffsetBottom()));
        setOption(webcamShape, saved.webcamShape());
        setOption(webcamOrientation, saved.webcamOrientation());
        if (enabledChanged) {
            webcamFailed = false;
            if (webcamEnabled)
                snow_webcam_retry(webcam.get());
        }
    }

    void syncWebcam() {
        if (!webcamEnabled || !uiSession) {
            if (webcam)
                snow_webcam_set_active(webcam.get(), 0);
            webcamTimer.stop();
            return;
        }
        if (!webcamTimer.isActive())
            webcamTimer.start();
        if (webcamFailed)
            return;
        // Preview must receive a stable source even before the area is shown. The source
        // starts inactive; window visibility controls device capture, not handle creation.
        if (!webcam) {
            webcam = std::shared_ptr<SnowWebcam>(
                snow_webcam_create(static_cast<uint32_t>(webcamSize), 30), snow_webcam_destroy);
            if (!webcam) {
                webcamFailed = true;
                notifyWebcamError(tr("The webcam could not be initialized."));
                return;
            }
        }
        char error[1024]{};
        if (snow_webcam_error(webcam.get(), error, sizeof(error)) != 0) {
            webcamFailed = true;
            notifyWebcamError(QString::fromUtf8(error));
            return;
        }
        // Once startup begins, the recording worker owns capture and pause/resume.
        if (!recordingOptionsEditable())
            return;
        const bool preview = !trimRequested && areaWindow && areaWindow->isVisible() &&
                             !QApplication::activeModalWidget();
        static_cast<void>(
            snow_webcam_configure(webcam.get(), static_cast<uint32_t>(webcamSize), 30));
        snow_webcam_set_active(webcam.get(), static_cast<uint8_t>(preview));
    }

    void notifyWebcamError(const QString& error) {
        if (areaWindow)
            areaWindow->setWebcamOverlay({});
        showRecordingWarning(QStringLiteral("screen-recording-webcam-error"),
                             tr("Webcam unavailable: %1").arg(error));
    }

    void syncPreview() {
        syncWebcam();
        if (uiSession == nullptr) {
            return;
        }
        const bool eligible = !trimRequested && recordingOptionsEditable();
        if (!eligible) {
            uiSession->preview->setEligible(false);
            return;
        }
        const auto output =
            directRecordingSettings(outputFormat, captureRegion.size(), automationOptions);
#ifdef Q_OS_MACOS
        if (!allowRecording(false)) {
            uiSession->preview->setEligible(false);
            return;
        }
        if (dimensionsRegion != captureRegion || dimensionsMaximum != output.maximumSize ||
            dimensionsFormat != output.format) {
            dimensionsRegion = captureRegion;
            dimensionsMaximum = output.maximumSize;
            dimensionsFormat = output.format;
            ++dimensionsGeneration;
            dimensionsResolved = false;
            previewOutput = {};
        }
        if (!dimensionsResolved) {
            uiSession->preview->setEligible(false);
            if (!dimensionsFuture.valid()) {
                const QRect region = captureRegion;
                const QSize maximum = output.maximumSize;
                const auto format = output.format;
                const auto generation = dimensionsGeneration;
                dimensionsFuture = std::async(
                    std::launch::async,
                    snow_shot::runtime::trackRuntimeWork([region, maximum, format, generation] {
                        snow_shot::platform::applyApplicationQoSToCurrentThread();
                        uint32_t width = 0, height = 0;
                        const bool ok =
                            snow_recording_region_output_dimensions(
                                region.x(), region.y(), static_cast<uint32_t>(region.width()),
                                static_cast<uint32_t>(region.height()),
                                static_cast<uint32_t>(maximum.width()),
                                static_cast<uint32_t>(maximum.height()),
                                static_cast<uint32_t>(format), &width, &height) != 0;
                        return std::make_pair(generation, ok ? QSize(static_cast<int>(width),
                                                                     static_cast<int>(height))
                                                             : QSize());
                    }));
                dimensionsPollTimer.start();
            }
            return;
        }
        if (previewOutput.isEmpty()) {
            uiSession->preview->setEligible(false);
            return;
        }
        const auto width = previewOutput.width();
        const auto height = previewOutput.height();
#else
        uint32_t width = 0;
        uint32_t height = 0;
        if (snow_recording_output_dimensions(static_cast<uint32_t>(captureRegion.width()),
                                             static_cast<uint32_t>(captureRegion.height()),
                                             static_cast<uint32_t>(output.maximumSize.width()),
                                             static_cast<uint32_t>(output.maximumSize.height()),
                                             static_cast<uint32_t>(output.format), &width,
                                             &height) == 0) {
            uiSession->preview->setEligible(false);
            return;
        }
#endif
        uiSession->preview->configure(
            captureRegion, QSize(static_cast<int>(width), static_cast<int>(height)),
            mouseTrailColor, mouseClickColor, showKeyboard, mouseTrailDurationMs,
            keyboardBackgroundColor, keyboardForegroundColor, keyboardSize,
            mouseHighlightEnabled && showCursor ? mouseHighlightColor : QColor(0, 0, 0, 0),
            recordMouseClicks, mouseHighlightRadius,
            !postProcessingEnabled ? SNOW_RECORDING_PLAYBACK_OVERLAY_NONE
            : postProcessingEffect == QStringLiteral("playback_time")
                ? SNOW_RECORDING_PLAYBACK_OVERLAY_PLAYBACK_TIME
                : SNOW_RECORDING_PLAYBACK_OVERLAY_PROGRESS_BAR,
            progressBarColor, webcamEnabled && !webcamFailed ? webcam.get() : nullptr, webcamSize,
            webcamShape == QStringLiteral("circle") ? 1u : 0u, webcamOffset, recordMouseScrolling,
            static_cast<uint32_t>(webcamOrientation));
        const bool audioSupported = outputFormat == QStringLiteral("mp4");
        uiSession->preview->setStatusPrompts(
            snow_shot::storage::RecordingSettings().showStatusPrompts(), outputFormat,
            audioSupported && microphoneEnabled, audioSupported && systemAudioEnabled);
        uiSession->preview->setEligible(true);
    }

    void syncUi() {
        refreshAudioGains();
        refreshWebcamSettings();
        if (uiSession != nullptr && sessionStatus.busy())
            uiSession->colorSampler->cancel();
        if (uiSession != nullptr && uiSession->settingsModal &&
            (sessionStatus.state() != ScreenshotToolPalette::RecordingState::Idle ||
             sessionStatus.busy()))
            uiSession->settingsModal->close();
        syncPreview();
        if (areaWindow != nullptr) {
            areaWindow->setRecordingState(sessionStatus.state());
            areaWindow->setDrawingBlocked(sessionStatus.busy());
        }
        ScreenshotToolPalette* palette =
            toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
        if (palette != nullptr) {
            palette->setRecordingSession(sessionStatus);
            palette->setRecordingDuration(durationMilliseconds);
            palette->setRecordingMicrophoneEnabled(microphoneEnabled);
            palette->setRecordingSystemAudioEnabled(systemAudioEnabled);
            palette->setRecordingMicrophoneGainDb(microphoneGainDb);
            palette->setRecordingSystemAudioGainDb(systemAudioGainDb);
            palette->setRecordingOutputFormat(outputFormat);
            palette->setRecordingPostProcessingEnabled(postProcessingEnabled);
            palette->setRecordingPostProcessingEffect(postProcessingEffect);
            palette->setRecordingProgressBarColor(progressBarColor);
            palette->setRecordingMouseTrailColor(mouseTrailColor);
            palette->setRecordingMouseTrailDurationMs(mouseTrailDurationMs);
            palette->setRecordingKeyboardSize(keyboardSize);
            palette->setRecordingWebcamEnabled(webcamEnabled);
            palette->setRecordingWebcamSize(webcamSize);
            palette->setRecordingWebcamShape(webcamShape);
            palette->setRecordingKeyboardBackgroundColor(keyboardBackgroundColor);
            palette->setRecordingKeyboardForegroundColor(keyboardForegroundColor);
            palette->setRecordingMouseClickColor(mouseClickColor);
            palette->setRecordingMouseHighlightEnabled(mouseHighlightEnabled);
            palette->setRecordingRecordMouseClicks(recordMouseClicks);
            palette->setRecordingRecordMouseScrolling(recordMouseScrolling);
            palette->setRecordingMouseHighlightColor(mouseHighlightColor);
            palette->setRecordingMouseHighlightRadius(mouseHighlightRadius);
            palette->setRecordingStartDelaySeconds(startDelaySeconds);
            palette->setRecordingCursorVisible(showCursor);
            palette->setRecordingKeyboardVisible(showKeyboard);
        }
        syncAudioMeter();
    }

    void updateCaptureRegion() {
#ifdef Q_OS_MACOS
        captureRegion = recordingRegion;
#else
        QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(recordingRegion);
        const QRect bounds =
            screen != nullptr ? ScreenshotGeometryMapper::physicalRectForScreen(*screen) : QRect();
        captureRegion = snow_shot::presentation::recording::screenRecordingCompatibleCaptureRegion(
            recordingRegion, bounds);
#endif
    }

    void showError(const QString& message) {
        if (!recordingSession)
            setCaptureActivity(false);
        report(QStringLiteral("recording.failed"), QtWarningMsg, message);
        automationError = message;
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        if (automationOwned || automationCommand)
            return;
        QMessageBox::critical(toolbarWindow, tr("Screen recording"),
                              message.isEmpty() ? tr("The recording operation failed") : message);
    }

    PermissionCheck permissionCheck;
    bool sessionEnding = false;
    bool recoveryOffered = false;
    QJsonObject automationOptions;
    QString automationError;
    QString finalizedOutputPath;
    qint64 finalizedDurationMilliseconds = 0;
    bool automationOwned = false;
    bool automationCommand = false;
    quint64 automationRevision = snow_shot::presentation::nextAutomationRevision();
    bool automationNextStart = false;
#ifdef Q_OS_MACOS
    QTimer dimensionsPollTimer;
    std::future<std::pair<quint64, QSize>> dimensionsFuture;
    quint64 dimensionsGeneration = 0;
    QRect dimensionsRegion;
    QSize dimensionsMaximum;
    SnowRecordingOutputFormat dimensionsFormat = SNOW_RECORDING_OUTPUT_FORMAT_MP4;
    QSize previewOutput;
    bool dimensionsResolved = false;
#endif
    ScreenRecordingController& owner;
    ScreenRecordingController::EffectsSourceFactory effectsSourceFactory;
    RecordingUiSession* uiSession = nullptr;
    ScreenRecordingAreaWindow* areaWindow = nullptr;
    ScreenRecordingToolbarWindow* toolbarWindow = nullptr;
    RecordingSessionHandle recordingSession;
    QRect recordingRegion;
    QRect captureRegion;
    QTimer durationTimer;
    QTimer finalizationPollTimer;
    QTimer startPollTimer;
    QTimer countdownTimer;
    QElapsedTimer countdownElapsed;
    qint64 countdownTotalMilliseconds = 0;
    std::future<FinalizationResult> finalizationFuture;
    std::future<void> discardFuture;
    bool discardPendingStart = false;
    RecordingRenderJob* renderJob = nullptr;
    QPointer<RecordingTrimSession> trimSession;
    bool trimRequested = false;
    bool trimSaveWhenReady = false;
    bool sessionLoopAnimatedImages = false;
    QString retainedSourcePath;
    bool sessionDeferred = false;
    bool postProcessingEnabled = false;
    QString postProcessingEffect = QStringLiteral("progress_bar");
    QColor progressBarColor{22, 119, 255};
    std::future<StartAttemptResult> startFuture;
    std::shared_ptr<RecordingAreaHistoryCommit> pendingAreaHistory;
    qsizetype areaHistoryIndex = -1;
    bool restoringAreaHistory = false;
    std::shared_ptr<snow_shot::presentation::recording::StartupRecovery> startupRecovery =
        std::make_shared<snow_shot::presentation::recording::StartupRecovery>();
    ScreenshotToolPalette::RecordingSessionStatus sessionStatus =
        ScreenshotToolPalette::RecordingSessionStatus::idle();
    qint64 durationMilliseconds = 0;
    QString pendingOutputPath;
    snow_shot::storage::ScreenshotWindowNames sessionWindowNames;
    bool webcamEnabled = false;
    int webcamSize = 150;
    QPoint webcamOffset{48, 48};
    QString webcamShape = QStringLiteral("square");
    int webcamOrientation = 0;
    bool webcamSettingsRefreshPending = false;
    std::shared_ptr<SnowWebcam> webcam;
    bool webcamFailed = false;
    QTimer webcamTimer;
    bool microphoneEnabled = false;
    bool systemAudioEnabled = true;
    int microphoneGainDb = 0;
    int systemAudioGainDb = 0;
    bool audioSettingsRefreshPending = false;
    bool audioSettingsRefreshQueued = false;
    QTimer audioMeterTimer;
    QElapsedTimer audioMeterClock;
    qint64 lastAudioMeterTick = 0;
    qint64 audioClipDeadline = 0;
    double displayedAudioPeak = 0;
    int audioMeterSource = -1;
    SnowRecordingAudioMonitor* audioPreview = nullptr;
    int audioPreviewSource = -1;
    std::shared_future<void> audioPreviewRetirement;
    QTimer exclusionPollTimer;
    bool toolbarPopupsExcluded = false;
    bool toolbarExclusionErrorReported = false;
    snow_shot::presentation::RecordingPopupExclusions toolbarExclusions;
    std::unique_ptr<adqt::widgets::AdPopupSurfaceGuard> toolbarSurfaceGuard;
    QString outputFormat = QStringLiteral("mp4");
    int startDelaySeconds = 0;
    int mouseTrailDurationMs = 500;
    int keyboardSize = 64;
    QColor keyboardBackgroundColor{0, 0, 0, 204};
    QColor keyboardForegroundColor{Qt::white};
    QColor mouseTrailColor{0, 0, 0, 0};
    QColor mouseClickColor{0, 0, 0, 0};
    bool mouseHighlightEnabled = false;
    bool recordMouseClicks = false;
    bool recordMouseScrolling = false;
    QColor mouseHighlightColor{255, 255, 0, 128};
    int mouseHighlightRadius = 41;
    bool showCursor = true;
    bool showKeyboard = false;
    DirectRecordingSettings sessionOutputSettings;
    QColor sessionMouseTrailColor{0, 0, 0, 0};
    QColor sessionMouseClickColor{0, 0, 0, 0};
    bool sessionShowCursor = true;
    QString sessionCaptureBackendPreference = QStringLiteral("auto");
    void report(const QString& event, QtMsgType level = QtInfoMsg,
                const QString& message = {}) const {
        snow_shot::diagnostics::DiagnosticsService::instance().record(
            level, QStringLiteral("snow_shot.recording"), event, message,
            {{QStringLiteral("operation"), operation},
             {QStringLiteral("backend_preference"), sessionCaptureBackendPreference},
             {QStringLiteral("format"), sessionOutputSettings.extension},
             {QStringLiteral("codec"), static_cast<int>(sessionOutputSettings.codec)},
             {QStringLiteral("hardware"), sessionOutputSettings.useHardwareEncoder},
             {QStringLiteral("fps"), static_cast<int>(sessionOutputSettings.targetFps)},
             {QStringLiteral("quality"), static_cast<int>(sessionOutputSettings.quality)},
             {QStringLiteral("encoding_preset"), static_cast<int>(sessionOutputSettings.preset)},
             {QStringLiteral("deferred"), sessionDeferred},
             {QStringLiteral("width"), recordingRegion.width()},
             {QStringLiteral("height"), recordingRegion.height()},
             {QStringLiteral("duration_ms"),
              operationTimer.isValid() ? operationTimer.elapsed() : 0},
             {QStringLiteral("backend"),
#ifdef Q_OS_MACOS
              QStringLiteral("screencapturekit")
#else
              QStringLiteral("auto")
#endif
             }});
    }
    QString operation;
    QElapsedTimer operationTimer;
    bool desktopCaptureActive = false;
    bool startScheduled = false;
    quint64 startGeneration = 0;
    snow_shot::presentation::WindowCaptureExclusion captureExclusion{
#if defined(Q_OS_WIN) || defined(_WIN32) || defined(Q_OS_MACOS)
        snow_shot::platform::setWindowExcludedFromCapture
#endif
    };
};

ScreenRecordingController::ScreenRecordingController(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}

ScreenRecordingController::ScreenRecordingController(EffectsSourceFactory factory, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(factory))) {}

ScreenRecordingController::~ScreenRecordingController() {
    // Capture-activity observers can query the controller during final UI retirement.
    // Keep m_impl available until those notifications and worker transfers have completed.
    if (m_impl->sessionEnding) {
        // Process termination owns the remaining native/preview resources. Ordinary shutdown
        // cancellation could remove recoverable staging or join an unfinished native worker.
        static_cast<void>(m_impl.release());
    } else {
        m_impl->shutdown();
    }
}

void ScreenRecordingController::setPermissionCheck(PermissionCheck check) {
    m_impl->permissionCheck = std::move(check);
}

void ScreenRecordingController::open(const QRect& recordingRegion) {
    m_impl->open(recordingRegion);
}

bool ScreenRecordingController::isOpen() const {
    return m_impl->isOpen();
}

bool ScreenRecordingController::isRecording() const {
    return m_impl->sessionStatus.state() != ScreenshotToolPalette::RecordingState::Idle;
}

bool ScreenRecordingController::blocksMemoryTrimming() const {
    const auto& state = *m_impl;
    return state.isOpen() || isRecording() || state.recordingSession || state.startFuture.valid() ||
           state.finalizationFuture.valid() || state.discardFuture.valid() || state.renderJob ||
           state.trimSession || state.audioPreview ||
           (state.audioPreviewRetirement.valid() &&
            state.audioPreviewRetirement.wait_for(std::chrono::milliseconds(0)) !=
                std::future_status::ready);
}

void ScreenRecordingController::startRecording() {
    m_impl->start();
}

void ScreenRecordingController::stopRecordingAndCopy() {
    if (m_impl->sessionEnding)
        return;
    if (m_impl->trimSession)
        m_impl->trimSession->exportClip(false);
    else
        m_impl->stop(true);
}

void ScreenRecordingController::openRecordingFolder() {
    m_impl->openFolder();
}

void ScreenRecordingController::prepareForSessionEnd(int timeoutMs) {
    auto& s = *m_impl;
    if (s.sessionEnding)
        return;
    s.sessionEnding = true;
    const bool hasUnfinishedRecording = s.recordingSession || s.startFuture.valid() ||
                                        s.finalizationFuture.valid() || s.renderJob ||
                                        s.trimSession;
    s.durationTimer.stop();
    s.finalizationPollTimer.stop();
    s.startPollTimer.stop();
    s.countdownTimer.stop();
    s.exclusionPollTimer.stop();
    s.audioMeterTimer.stop();
    s.webcamTimer.stop();
#ifdef Q_OS_MACOS
    s.dimensionsPollTimer.stop();
#endif
    s.startScheduled = false;
    if (s.uiSession) {
        // QObject's ordinary child destruction would join preview workers even though Impl is
        // retained. Retain its UI owners too, and prevent callbacks into the retiring controller.
        s.uiSession->setParent(nullptr);
        s.uiSession->connections.reset();
        s.uiSession->shortcuts.reset();
        s.uiSession->preview->prepareForSessionEnd();
    }
    if (!hasUnfinishedRecording)
        return;
    const QString directory = snow_shot::app::distribution::writableDataDirectory();
    const QString marker =
        directory.isEmpty() ? QString()
                            : QDir(directory).filePath(QStringLiteral("recording-recovery.json"));
    QString sourcePath = s.retainedSourcePath;
    QString outputPath = s.pendingOutputPath;
    if (s.trimSession) {
        if (!s.trimSession->sourcePath().isEmpty())
            sourcePath = s.trimSession->sourcePath();
        if (!s.trimSession->outputPath().isEmpty())
            outputPath = s.trimSession->outputPath();
        s.trimSession->prepareForSessionEnd();
        s.trimSession = nullptr;
    }
    if (s.renderJob) {
        sourcePath = s.renderJob->sourcePath();
        s.renderJob->prepareForSessionEnd();
        s.renderJob = nullptr;
    }
    using namespace snow_shot::presentation::recording;
    // Publish known durable locations before waiting for native finalization. On a forced
    // deadline the worker retains every borrowed handle; the GUI never destroys its future.
    // Keep native owners in the terminal Impl until the worker starts. Allocating its task or
    // completion state can fail; unwinding a closure that already owns futures could join here.
    auto* terminal = &s;
    static_cast<void>(runBoundedPreservation(
        [terminal, deferred = s.sessionDeferred, marker, sourcePath, outputPath,
         recovery = s.startupRecovery]() mutable {
            auto start = std::move(terminal->startFuture);
            auto finish = std::move(terminal->finalizationFuture);
            auto discard = std::move(terminal->discardFuture);
            auto session = std::move(terminal->recordingSession);
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            struct PreserveSessionOnFailure {
                RecordingSessionHandle& session;
                ~PreserveSessionOnFailure() {
                    static_cast<void>(session.release());
                }
            } preserveOnFailure{session};
            static_cast<void>(recovery->beginPreservation(marker, {sourcePath, outputPath, {}}));
            if (start.valid()) {
                auto started = start.get();
                if (!started.outputPath.isEmpty())
                    outputPath = started.outputPath;
                if (started.session && !session)
                    session = std::move(started.session);
                static_cast<void>(
                    recovery->beginPreservation(marker, {sourcePath, outputPath, {}}));
            }
            FinalizationResult result;
            if (finish.valid()) {
                result = finish.get();
            } else if (session) {
                static_cast<void>(snow_recording_session_request_stop(session.get()));
                result.ok =
                    (deferred
                         ? snow_recording_session_finalize_deferred(session.get(), &result.source)
                         : snow_recording_session_stop(session.get())) == SNOW_RECORDING_RESULT_OK;
            }
            if (result.source) {
                const auto length = snow_recording_source_path(result.source, nullptr, 0);
                if (length > 0 && length < 1024 * 1024) {
                    QByteArray path(static_cast<qsizetype>(length), '\0');
                    snow_recording_source_path(result.source, path.data(), length);
                    sourcePath = QString::fromUtf8(path.constData());
                }
                // Dropping a finalized source preserves its self-contained .snowrec bundle.
                snow_recording_source_destroy(result.source);
            }
            static_cast<void>(recovery->beginPreservation(marker, {sourcePath, outputPath, {}}));
            if (discard.valid())
                discard.wait();
            if (result.ok)
                session.reset();
        },
        timeoutMs));
    // Session-end preservation may block the GUI event loop, so drain the
    // successful-start commit directly before application storage is flushed.
    if (s.pendingAreaHistory)
        s.pendingAreaHistory->commit();
}

void ScreenRecordingController::offerRecordingRecovery() {
    QTimer::singleShot(0, this, [this] { m_impl->offerRecovery(); });
}

QJsonObject ScreenRecordingController::automationState() const {
    const auto& s = *m_impl;
    auto options = s.automationOptions;
    const auto currentOptions = s.currentAutomationOptions();
    for (auto field = currentOptions.begin(); field != currentOptions.end(); ++field)
        options.insert(field.key(), field.value());
    const QStringList states{QStringLiteral("idle"), QStringLiteral("recording"),
                             QStringLiteral("paused")};
    const QStringList busy{QStringLiteral("none"), QStringLiteral("starting"),
                           QStringLiteral("stopping"), QStringLiteral("copying"),
                           QStringLiteral("counting_down")};
    QJsonObject result{
        {QStringLiteral("revision"), static_cast<qint64>(s.automationRevision)},
        {QStringLiteral("automated"), s.automationOwned},
        {QStringLiteral("options"), options},
        {QStringLiteral("effective_frame_rate"),
         static_cast<int>(s.sessionOutputSettings.targetFps)},
        {QStringLiteral("maximum_output_size"),
         QJsonArray{s.sessionOutputSettings.maximumSize.width(),
                    s.sessionOutputSettings.maximumSize.height()}},
        {QStringLiteral("canvas_revision"),
         s.areaWindow ? static_cast<qint64>(s.areaWindow->canvasRuntime().documentRevision()) : 0},
        {QStringLiteral("open"), s.isOpen()},
        {QStringLiteral("state"), s.trimSession
                                      ? s.trimSession->phase()
                                      : states.at(static_cast<int>(s.sessionStatus.state()))},
        {QStringLiteral("operation"), busy.at(static_cast<int>(s.sessionStatus.busyOperation()))},
        {QStringLiteral("busy"),
         s.sessionStatus.busy() || s.startScheduled || (s.trimSession && !s.trimSession->ready())},
        {QStringLiteral("duration_ms"), s.finalizedOutputPath.isEmpty()
                                            ? s.durationMilliseconds
                                            : s.finalizedDurationMilliseconds},
        {QStringLiteral("region"),
         QJsonArray{s.recordingRegion.x(), s.recordingRegion.y(), s.recordingRegion.width(),
                    s.recordingRegion.height()}},
        {QStringLiteral("format"), s.outputFormat},
        {QStringLiteral("path"), s.finalizedOutputPath},
        {QStringLiteral("finalized"), !s.finalizedOutputPath.isEmpty()},
        {QStringLiteral("error"), s.automationError},
        {QStringLiteral("render_mode"),
         s.sessionDeferred ? QStringLiteral("post_recording") : QStringLiteral("realtime")},
        {QStringLiteral("source_path"), s.retainedSourcePath},
        {QStringLiteral("source_retained"), !s.retainedSourcePath.isEmpty()},
        {QStringLiteral("render_phase"), QStringLiteral("none")},
        {QStringLiteral("render_progress"), 0}};
    if (s.renderJob) {
        const auto progress = s.renderJob->state();
        for (auto field = progress.begin(); field != progress.end(); ++field)
            result.insert(field.key(), field.value());
    }
    return result;
}

bool ScreenRecordingController::startAutomation(const QRect& region, const QJsonObject& options,
                                                QString* error) {
    auto fail = [error](const char* code) {
        if (error)
            *error = QString::fromLatin1(code);
        return false;
    };
    auto& s = *m_impl;
    if (s.sessionEnding || s.isOpen() || s.sessionStatus.busy() || s.recordingSession ||
        s.startScheduled)
        return fail("busy");
    if (!region.isValid() || region.width() > 32768 || region.height() > 32768)
        return fail("invalid_region");
    const auto& contracts = snow_shot::app::mcp::recordingAutomationOptionContracts();
    for (auto it = options.begin(); it != options.end(); ++it) {
        const QJsonValue descriptor = contracts.value(it.key());
        if (!descriptor.isObject())
            return fail("invalid_parameters");
        const QJsonObject contract = descriptor.toObject();
        const QString format = contract.value(QStringLiteral("format")).toString();
        if (format == QStringLiteral("output-path")) {
            if (!it->isString() || !QFileInfo(it->toString()).isAbsolute() ||
                QFileInfo::exists(it->toString()) ||
                !QFileInfo(QFileInfo(it->toString()).absolutePath()).isDir())
                return fail("output_path_unavailable");
        }
        const QString type = contract.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("boolean")) {
            if (!it->isBool())
                return fail("invalid_parameters");
        } else if (type == QStringLiteral("string")) {
            if (!it->isString())
                return fail("invalid_parameters");
        } else if (type == QStringLiteral("integer")) {
            const double number = it->toDouble();
            if (!it->isDouble() || !std::isfinite(number) || std::floor(number) != number)
                return fail("invalid_parameters");
            const QJsonValue minimum = contract.value(QStringLiteral("minimum"));
            const QJsonValue maximum = contract.value(QStringLiteral("maximum"));
            if ((minimum.isDouble() && number < minimum.toDouble()) ||
                (maximum.isDouble() && number > maximum.toDouble()))
                return fail("invalid_parameters");
        } else {
            return fail("invalid_parameters");
        }
        const QJsonValue choices = contract.value(QStringLiteral("enum"));
        if (choices.isArray() && !choices.toArray().contains(it.value()))
            return fail("invalid_parameters");
        if (format == QStringLiteral("qt-color")) {
            const QColor parsed = it.key() == QStringLiteral("progress_bar_color")
                                      ? progressBarColorFromString(it->toString())
                                      : QColor(it->toString());
            if (!parsed.isValid())
                return fail("invalid_parameters");
        }
    }
    const snow_shot::storage::RecordingSettings settings;
    s.outputFormat = options.value(QStringLiteral("format")).toString(settings.outputFormat());
    s.postProcessingEnabled =
        options.value(QStringLiteral("post_processing")).toBool(settings.postProcessingEnabled());
    s.postProcessingEffect = options.value(QStringLiteral("post_processing_effect"))
                                 .toString(settings.postProcessingEffect());
    s.retainedSourcePath.clear();
    s.microphoneEnabled =
        options.value(QStringLiteral("microphone")).toBool(settings.microphoneEnabled());
    s.systemAudioEnabled =
        options.value(QStringLiteral("system_audio")).toBool(settings.systemAudioEnabled());
    s.systemAudioGainDb =
        options.value(QStringLiteral("system_audio_gain_db")).toInt(settings.systemAudioGainDb());
    s.microphoneGainDb =
        options.value(QStringLiteral("microphone_gain_db")).toInt(settings.microphoneGainDb());
    s.startDelaySeconds =
        options.value(QStringLiteral("start_delay_seconds")).toInt(settings.startDelaySeconds());
    s.showCursor = options.value(QStringLiteral("show_cursor")).toBool(settings.showCursor());
    s.showKeyboard = options.value(QStringLiteral("show_keyboard")).toBool(settings.showKeyboard());
    s.webcamEnabled = options.value(QStringLiteral("webcam")).toBool(settings.webcamEnabled());
    s.webcamSize = options.value(QStringLiteral("webcam_size")).toInt(settings.webcamSize());
    s.webcamOffset = QPoint(settings.webcamOffsetLeft(), settings.webcamOffsetBottom());
    s.webcamShape = options.value(QStringLiteral("webcam_shape")).toString(settings.webcamShape());
    s.webcamOrientation = settings.webcamOrientation();
    s.recordMouseClicks =
        options.value(QStringLiteral("record_mouse_clicks")).toBool(settings.recordMouseClicks());
    s.recordMouseScrolling = options.value(QStringLiteral("record_mouse_scrolling"))
                                 .toBool(settings.recordMouseScrolling());
    s.mouseHighlightEnabled =
        options.value(QStringLiteral("mouse_highlight")).toBool(settings.mouseHighlightEnabled());
    s.mouseTrailDurationMs = options.value(QStringLiteral("mouse_trail_duration_ms"))
                                 .toInt(settings.mouseTrailDurationMs());
    s.keyboardSize = options.value(QStringLiteral("keyboard_size")).toInt(settings.keyboardSize());
    const auto color = [&options](const char* key, QColor fallback) {
        return options.contains(QLatin1String(key))
                   ? QColor(options.value(QLatin1String(key)).toString())
                   : fallback;
    };
    s.progressBarColor = options.contains(QStringLiteral("progress_bar_color"))
                             ? progressBarColorFromString(
                                   options.value(QStringLiteral("progress_bar_color")).toString())
                             : settings.progressBarColor();
    s.mouseTrailColor = color("mouse_trail", settings.mouseTrailColor());
    s.mouseClickColor = color("mouse_click", settings.mouseClickColor());
    s.mouseHighlightColor = color("mouse_highlight_color", settings.mouseHighlightColor());
    s.mouseHighlightRadius = options.value(QStringLiteral("mouse_highlight_radius"))
                                 .toInt(settings.mouseHighlightRadius());
    s.keyboardBackgroundColor = color("keyboard_background", settings.keyboardBackgroundColor());
    s.keyboardForegroundColor = color("keyboard_foreground", settings.keyboardForegroundColor());
    if (!s.allowRecording(false))
        return fail("permission_required");
    s.automationOwned = true;
    s.automationNextStart = true;
    s.automationOptions = options;
    s.automationError.clear();
    s.finalizedOutputPath.clear();
    s.finalizedDurationMilliseconds = 0;
    s.pendingOutputPath.clear();
    s.open(region);
    if (!s.isOpen())
        return fail("capture_unavailable");
    s.start();
    return true;
}

bool ScreenRecordingController::controlAutomation(const QString& action, const QJsonObject& payload,
                                                  QString* error) {
    auto& s = *m_impl;
    const QScopedValueRollback<bool> automationCommand(s.automationCommand, true);
    auto fail = [error](const char* code) {
        if (error)
            *error = QString::fromLatin1(code);
        return false;
    };
    if (s.sessionEnding)
        return fail("busy");
    if (s.renderJob) {
        bool ok = false;
        if (action == QStringLiteral("cancel_render"))
            ok = s.renderJob->cancel();
        else if (action == QStringLiteral("retry"))
            ok = s.renderJob->retry();
        else if (action == QStringLiteral("keep_source"))
            ok = s.renderJob->release(false);
        else if (action == QStringLiteral("discard"))
            ok = s.renderJob->release(true);
        else if (action == QStringLiteral("close") || action == QStringLiteral("cancel")) {
            s.destroyUi();
            return true;
        } else
            return fail("busy");
        return ok || fail("invalid_state");
    }
    s.automationError.clear();
    if (action == QStringLiteral("cancel") || action == QStringLiteral("close")) {
        s.close();
        return true;
    }
    if (s.trimRequested) {
        if (action == QStringLiteral("copy") && s.trimSession && s.trimSession->ready()) {
            s.trimSession->exportClip(false);
            return true;
        }
        return fail("invalid_state");
    }
    if (action == QStringLiteral("copy") && !s.finalizedOutputPath.isEmpty()) {
        copyFileToClipboard(s.finalizedOutputPath);
        return true;
    }
    if (s.sessionStatus.busy() || s.startScheduled)
        return fail("busy");
    if (action == QStringLiteral("pause")) {
        if (s.sessionStatus.state() != ScreenshotToolPalette::RecordingState::Recording)
            return fail("invalid_state");
        s.pause();
    } else if (action == QStringLiteral("resume")) {
        if (s.sessionStatus.state() != ScreenshotToolPalette::RecordingState::Paused)
            return fail("invalid_state");
        s.resume();
    } else if (action == QStringLiteral("stop") || action == QStringLiteral("copy")) {
        if (!s.recordingSession)
            return fail("invalid_state");
        s.stop(action == QStringLiteral("copy"));
    } else if (s.areaWindow &&
               (action == QStringLiteral("annotations") || action == QStringLiteral("undo") ||
                action == QStringLiteral("redo") || action == QStringLiteral("reset"))) {
        auto& runtime = s.areaWindow->canvasRuntime();
        bool ok = false;
        if (action == QStringLiteral("annotations")) {
            const auto result =
                QJsonDocument::fromJson(runtime.applyAnnotationTransaction(
                                            QJsonDocument(payload).toJson(QJsonDocument::Compact)))
                    .object();
            ok = !result.isEmpty();
        } else if (action == QStringLiteral("undo"))
            ok = runtime.undo();
        else if (action == QStringLiteral("redo"))
            ok = runtime.redo();
        else
            ok = s.areaWindow->canvas()->deleteAllElements();
        if (!ok)
            return fail("action_unavailable");
        s.areaWindow->canvas()->update();
        s.automationRevision = snow_shot::presentation::nextAutomationRevision();
    } else
        return fail("invalid_parameters");
    return s.automationError.isEmpty() || fail("recording_failed");
}

void ScreenRecordingController::detachAutomation() {
    if (m_impl->automationOwned && !m_impl->sessionEnding)
        m_impl->close();
}
