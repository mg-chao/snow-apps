#include "snow_shot/platform/applicationqos.h"
#include "recordingeffectpreview.h"
#include "recordingaudiogainpopover.h"
#include "widgets/popover.h"
#include "snow_shot/storage/applicationstorage.h"
#include <array>
#include "recordingcolorsampler.h"
#include "recordingrenderjob.h"
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
#include <QMimeData>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <future>
#include <thread>
#include <memory>
#include <utility>

namespace {
constexpr int kDurationTickMilliseconds = 100;
constexpr int kCountdownTickMilliseconds = 16;

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
    return encoder == QStringLiteral("h265") ? SNOW_CAPTURE_VIDEO_CODEC_H265
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
    result.useHardwareEncoder = encoder == QStringLiteral("h264_hw");

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

// Runs on the start worker thread: only touches the filesystem, never storage or UI.
QString chooseRecordingOutputPath(const QStringList& directories, const QString& baseName,
                                  const QString& extension) {
    const QString normalizedExtension =
        extension.startsWith(QLatin1Char('.')) ? extension : QStringLiteral(".%1").arg(extension);
    for (const QString& candidate : directories) {
        QDir directory(candidate);
        if ((!directory.exists() && !directory.mkpath(QStringLiteral("."))) ||
            !QFileInfo(directory.absolutePath()).isWritable()) {
            continue;
        }
        QString path = directory.filePath(baseName + normalizedExtension);
        for (int suffix = 1; QFileInfo::exists(path); ++suffix) {
            path = directory.filePath(
                QStringLiteral("%1_%2%3").arg(baseName).arg(suffix).arg(normalizedExtension));
        }
        return path;
    }
    return {};
}

// Owns the native recording session (capture pipeline, input hooks while
// running, and worker threads) through its destroy entry point on every path.
struct RecordingSessionDeleter {
    void operator()(SnowRecordingSession* session) const {
        snow_recording_session_destroy(session);
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
        keyboardBackgroundColor = settings.keyboardBackgroundColor();
        keyboardForegroundColor = settings.keyboardForegroundColor();
        mouseClickColor = settings.mouseClickColor();
        mouseHighlightEnabled = settings.mouseHighlightEnabled();
        recordMouseClicks = settings.recordMouseClicks();
        mouseHighlightColor = settings.mouseHighlightColor();
        showCursor = settings.showCursor();
        showKeyboard = settings.showKeyboard();
        startDelaySeconds = settings.startDelaySeconds();
        postProcessingEnabled = settings.postProcessingEnabled();
        postProcessingEffect = settings.postProcessingEffect();
        progressBarColor = settings.progressBarColor();
        QObject::connect(&snow_shot::storage::ApplicationStorage::instance().configuration(),
                         &snow_shot::storage::ConfigurationStore::valueChanged, &owner,
                         [this](const QString& key, const QJsonValue&) {
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
                         [this] { pollAudioExclusions(); });
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
        QObject::connect(&finalizationPollTimer, &QTimer::timeout, &owner,
                         [this]() { pollFinalization(); });
        startPollTimer.setInterval(50);
        QObject::connect(&startPollTimer, &QTimer::timeout, &owner, [this]() { pollStart(); });
        // The countdown shares one clock with the overlay digits: every tick
        // pushes the same elapsed reading that decides when recording starts.
        countdownTimer.setInterval(kCountdownTickMilliseconds);
        countdownTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&countdownTimer, &QTimer::timeout, &owner, [this]() { tickCountdown(); });
    }

    ~Impl() {
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
        if (startFuture.valid() || finalizationFuture.valid() || recordingSession != nullptr ||
            pendingDimensions.valid()) {
            std::thread([start = std::move(startFuture), finish = std::move(finalizationFuture),
                         session = std::move(recordingSession),
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
                session.reset();
                if (dimensions.valid())
                    dimensions.wait();
            }).detach();
        }
        if (renderJob) {
            delete renderJob;
            renderJob = nullptr;
        }
        destroyUi();
    }

    void open(const QRect& requestedRegion) {
        const QRect region =
            snow_shot::presentation::recording::screenRecordingNormalizedRegion(requestedRegion);
        if (!region.isValid() || region.isEmpty() || sessionStatus.busy() ||
            sessionStatus.state() != ScreenshotToolPalette::RecordingState::Idle) {
            return;
        }
        cancelPendingStart();
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

        recordingRegion = region;
        updateCaptureRegion();
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.before_ui_session");
        uiSession = new RecordingUiSession(&owner);
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
            if (toolbarWindow == nullptr) {
                return;
            }
            adqt::widgets::AdMessage::Request request;
            request.key = QStringLiteral("screen-recording-preview-error");
            request.content = tr("Motion preview unavailable: %1").arg(error);
            adqt::widgets::AdMessageService::warning(std::move(request), toolbarWindow);
        };
        toolbarWindow = uiSession->toolbar.get();
        // Keep the toolbar above the area even when drawing or resizing activates the area.
        toolbarWindow->setTransientOwnerWindow(areaWindow);
        areaWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        toolbarWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        areaWindow->setRecordingRegion(region);
        toolbarWindow->placeForRecordingRegion(region);
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.region_applied");
        connectToolbar();
        SNOW_SHOT_RECORDING_PERF_MILESTONE("open.toolbar_connected");
        uiSession->shortcuts =
            std::make_unique<ScreenRecordingShortcutController>(*areaWindow, *toolbarWindow);
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
               (areaWindow->isVisible() || toolbarWindow->isVisible());
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
                    QObject::connect(modal, &adqt::widgets::AdModal::finished,
                                     uiSession->connections.get(), [this] {
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
        QObject::connect(palette, &ScreenshotToolPalette::recordingKeyboardSizeChanged,
                         uiSession->connections.get(), [this](int value) {
                             setOption(keyboardSize, value);
                             snow_shot::storage::RecordingSettings().setKeyboardSize(value);
                             syncPreview();
                         });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::recordingRegionChanged,
                         uiSession->connections.get(), [this](const QRect& region) {
                             if (sessionStatus.state() !=
                                     ScreenshotToolPalette::RecordingState::Idle ||
                                 sessionStatus.busy()) {
                                 return;
                             }
                             setOption(recordingRegion, region);
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
                             syncAudioMeter();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingSystemAudioToggled,
                         uiSession->connections.get(), [this](bool enabled) {
                             setOption(systemAudioEnabled, enabled);
                             snow_shot::storage::RecordingSettings().setSystemAudioEnabled(enabled);
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
            QObject::connect(
                popover, &RecordingAudioGainPopover::visibleChanged, uiSession->connections.get(),
                [this, microphone, popover](bool visible) {
                    if (visible) {
                        const bool retryExclusion =
                            audioPopoversExcluded &&
                            audioFailedPopupIds != std::array<uint32_t, 2>{};
                        audioMeterSource = microphone ? 1 : 0;
                        audioFailedPopupIds = {};
                        displayedAudioPeak = 0;
                        audioClipDeadline = 0;
                        lastAudioMeterTick = audioMeterClock.elapsed();
                        if (retryExclusion) {
                            // The show guard runs before visibleChanged. Retry after
                            // clearing its failed native IDs, outside that stack.
                            QTimer::singleShot(
                                0, popover, [popup = QPointer<RecordingAudioGainPopover>(popover)] {
                                    if (popup && popup->popover()->isVisible())
                                        popup->popover()->refreshPopupLayout();
                                });
                        }
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
                         uiSession->connections.get(), [this]() { stop(true); });
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
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingPostProcessingEffectChanged,
                         uiSession->connections.get(), [this](const QString& value) {
                             setOption(postProcessingEffect, value);
                             snow_shot::storage::RecordingSettings().setPostProcessingEffect(value);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingProgressBarColorChanged,
                         uiSession->connections.get(), [this](const QColor& value) {
                             setOption(progressBarColor, value);
                             snow_shot::storage::RecordingSettings().setProgressBarColor(value);
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
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseHighlightColorChanged,
                         uiSession->connections.get(), [this](const QColor& value) {
                             setOption(mouseHighlightColor, value);
                             snow_shot::storage::RecordingSettings().setMouseHighlightColor(value);
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
        QObject::connect(&palette, &ScreenshotToolPalette::shapeRequested,
                         uiSession->connections.get(),
                         [activate]() { activate(SnowCanvasTool::Shape); });
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
        if (!isOpen() || sessionStatus.state() != ScreenshotToolPalette::RecordingState::Idle ||
            sessionStatus.busy() || startScheduled || recordingSession != nullptr) {
            return;
        }
        // UI starts after an automated session use current saved preferences.
        if (automationOwned && !automationNextStart) {
            automationRevision = snow_shot::presentation::nextAutomationRevision();
            automationOptions = {};
            automationOwned = false;
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
            mouseHighlightColor = settings.mouseHighlightColor();
            showCursor = settings.showCursor();
            showKeyboard = settings.showKeyboard();
            startDelaySeconds = settings.startDelaySeconds();
            postProcessingEnabled = settings.postProcessingEnabled();
            postProcessingEffect = settings.postProcessingEffect();
            progressBarColor = settings.progressBarColor();
        }
        automationNextStart = false;
        if (!allowRecording(true))
            return;
        // A new accepted recording owns the state even during its countdown.
        // Earlier retained sources remain on disk; earlier exports cease being
        // eligible for this recording's Copy action.
        retainedSourcePath.clear();
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
        const bool input = showKeyboard || recordMouseClicks || mouseTrailColor.alpha() != 0 ||
                           mouseClickColor.alpha() != 0;
        return !permissionCheck ||
               permissionCheck(outputFormat == QStringLiteral("mp4") && microphoneEnabled, input,
                               notify);
    }

    void scheduleStart() {
        if (!allowRecording(true))
            return;
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
            automationRevision = snow_shot::presentation::nextAutomationRevision();
            syncUi();

            // Snapshot every UI and storage value on the GUI thread; the worker
            // below must not touch either.
            const snow_shot::storage::RecordingSettings settings;
            sessionOutputSettings =
                directRecordingSettings(outputFormat, captureRegion.size(), automationOptions);
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
                // Direct recording Auto tries DXGI, then WGC and GDI on eligible failures.
                static_cast<uint32_t>(SNOW_CAPTURE_BACKEND_AUTO),
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
                                          ? SNOW_CAPTURE_ENCODER_PREFERENCE_H264_HARDWARE
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
            };
            const QString baseName =
                ScreenshotImageFileService::suggestedBaseName(settings.videoFilenameFormat());
            const QStringList directories =
                snow_shot::presentation::recording::screenRecordingDirectories();
            const QString extension = sessionOutputSettings.extension;
            const bool keyboard = showKeyboard || recordMouseClicks;
            const QString requestedPath =
                automationOptions.value(QStringLiteral("path")).toString();
            // Session creation blocks on capture, audio, hooks, and encoder
            // initialization; keep it off the GUI thread so the busy state can
            // paint. The FFI error string is thread-local, so it is read here.
            startFuture = std::async(
                std::launch::async,
                [config, excludedWindowIds, directories, baseName, extension, keyboard,
                 keyboardFont, requestedPath, previewRetirement, deferred = sessionDeferred,
                 overlay, barColor]() mutable -> StartAttemptResult {
                    if (previewRetirement.valid())
                        previewRetirement.wait();
                    snow_shot::platform::applyApplicationQoSToCurrentThread();
                    StartAttemptResult result;
                    result.outputPath =
                        requestedPath.isEmpty()
                            ? chooseRecordingOutputPath(directories, baseName, extension)
                            : requestedPath;
                    // The recording exporter owns its private staging file and
                    // publishes only after finalization. Reserving the destination
                    // here would make that publication reject our own empty file.
                    if (result.outputPath.isEmpty()) {
                        result.error =
                            QCoreApplication::translate("ScreenRecordingController",
                                                        "Unable to create the recording directory");
                        return result;
                    }
                    const QByteArray outputUtf8 =
                        QDir::toNativeSeparators(result.outputPath).toUtf8();
                    const RecordingKeyboardLabels labels(keyboard);
                    keyboardFont.applyTo(config);
                    config.output_file_utf8 = outputUtf8.constData();
                    config.keyboard_labels = labels.entries.constData();
                    config.keyboard_label_count = static_cast<uint32_t>(labels.entries.size());
                    config.exclusions.windows = excludedWindowIds.constData();
                    config.exclusions.window_count = static_cast<size_t>(excludedWindowIds.size());
                    SnowRecordingSession* created = nullptr;
                    const SnowRecordingDeferredOptions deferredOptions{
                        SNOW_RECORDING_DEFERRED_OPTIONS_VERSION,
                        sizeof(SnowRecordingDeferredOptions), overlay, barColor, nullptr};
                    const SnowRecordingResult createResult =
                        deferred ? snow_recording_session_create_deferred(&config, &deferredOptions,
                                                                          &created)
                                 : snow_recording_session_create_direct(&config, &created);
                    result.session.reset(created);
                    if (createResult != SNOW_RECORDING_RESULT_OK || result.session == nullptr ||
                        snow_recording_session_start(result.session.get()) == 0) {
                        result.error = captureError();
                        return result;
                    }
                    return result;
                });
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
        if (uiSession == nullptr) {
            // The UI was retired while the backend was starting. A session that
            // did start is shut down through the regular finalization path; any
            // other attempt is destroyed when this result is dropped.
            if (result.session != nullptr && result.error.isEmpty()) {
                recordingSession.reset(result.session.release());
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
                report(QStringLiteral("recording.failed"), QtWarningMsg);
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

        recordingSession.reset(result.session.release());
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
        finalizationFuture = std::async(std::launch::async, [session, deferred = sessionDeferred] {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            FinalizationResult result;
            result.ok =
                (deferred ? snow_recording_session_finalize_deferred(session, &result.source)
                          : snow_recording_session_stop(session)) == SNOW_RECORDING_RESULT_OK;
            if (!result.ok)
                result.error = captureError();
            return result;
        });
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
        // Finalized observers can destroy the controller or start a new session.
        // Keep the original Copy intent and its path independent of that callback.
        emit owner.finalized();
        if (shouldCopy)
            copyFileToClipboard(completedPath);
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
        static_cast<void>(snow_shot::presentation::recording::openScreenRecordingFolder());
    }

    void close() {
        stop(false);
        destroyUi();
    }

    void destroyUi() {
        stopAudioMeter();
        if (toolbarWindow)
            toolbarWindow->palette()->closeRecordingAudioGainPopovers();
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
        restoreToolbarCaptureVisibility(retiring->toolbar->palette());
        // Close can originate in a toolbar button's event handler.
        retiring->deleteLater();
    }

    bool excludeToolbarFromCapture() {
        restoreToolbarCaptureVisibility();
        audioPopoversExcluded = true;
        const bool excluded = captureExclusion.exclude(toolbarWindow);
        if (toolbarWindow && outputFormat == QStringLiteral("mp4")) {
            for (bool microphone : {false, true}) {
                if (!(microphone ? microphoneEnabled : systemAudioEnabled))
                    continue;
                auto* control = toolbarWindow->palette()->recordingAudioGainPopover(microphone);
                control->setRetainNativeSurfaceOnHide(true);
                control->setSurfaceShowGuard(
                    [this](QWidget* surface) { return guardAudioSurface(surface); });
                QWidget* surface = control->prepareSurface();
                // Initial macOS filters may not discover a prepared hidden window.
                // Only a required-window update acknowledgment permits showing it.
                static_cast<void>(captureExclusion.exclude(surface));
            }
        }
        return excluded;
    }

    void restoreToolbarCaptureVisibility(ScreenshotToolPalette* palette = nullptr) {
        exclusionPollTimer.stop();
        audioPopoversExcluded = false;
        audioExclusionErrorReported = false;
        audioExclusionGeneration = 0;
        audioAppliedPopupIds = {};
        audioPendingPopupIds = {};
        audioFailedPopupIds = {};
        captureExclusion.restore();
        if (!palette && toolbarWindow)
            palette = toolbarWindow->palette();
        if (palette) {
            for (bool microphone : {false, true}) {
                if (auto* control = palette->recordingAudioGainPopover(microphone)) {
                    control->setSurfaceShowGuard({});
                    control->setRetainNativeSurfaceOnHide(false);
                }
            }
        }
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
            {QStringLiteral("record_mouse_clicks"), recordMouseClicks},
            {QStringLiteral("mouse_highlight"), mouseHighlightEnabled},
            {QStringLiteral("start_delay_seconds"), startDelaySeconds},
            {QStringLiteral("keyboard_size"), keyboardSize},
            {QStringLiteral("mouse_trail_duration_ms"), mouseTrailDurationMs},
            {QStringLiteral("mouse_trail"), mouseTrailColor.name(QColor::HexArgb)},
            {QStringLiteral("mouse_click"), mouseClickColor.name(QColor::HexArgb)},
            {QStringLiteral("mouse_highlight_color"), mouseHighlightColor.name(QColor::HexArgb)},
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
        std::thread([retiring, completion = std::move(completion)]() mutable {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            snow_recording_audio_monitor_destroy(retiring);
            completion.set_value();
        }).detach();
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

    bool guardAudioSurface(QWidget* surface) {
        if (!audioPopoversExcluded)
            return true;
        if (!surface || !snow_shot::platform::setWindowExcludedFromCapture(surface, true)) {
            reportAudioExclusionError();
            return false;
        }
        static_cast<void>(captureExclusion.exclude(surface));
#ifdef Q_OS_MACOS
        if (!recordingSession || sessionStatus.busy())
            return false;
        std::array<uint32_t, 2> ids{};
        for (bool microphone : {false, true}) {
            if (!(microphone ? microphoneEnabled : systemAudioEnabled))
                continue;
            auto* control = toolbarWindow->palette()->recordingAudioGainPopover(microphone);
            const auto id = snow_shot::platform::captureWindowId(control->prepareSurface());
            if (!id) {
                reportAudioExclusionError();
                return false;
            }
            ids[microphone ? 1 : 0] = *id;
        }
        if (ids == audioAppliedPopupIds)
            return true;
        if (ids == audioFailedPopupIds || (audioExclusionGeneration && ids == audioPendingPopupIds))
            return false;
        const auto windows = captureExclusion.windowIds(snow_shot::platform::captureWindowId);
        QVector<uint32_t> required;
        for (auto id : ids)
            if (id)
                required.push_back(id);
        SnowCaptureExclusions exclusions{windows.constData(), static_cast<size_t>(windows.size()),
                                         nullptr, 0};
        if (snow_recording_session_request_exclusions(
                recordingSession.get(), &exclusions, required.constData(),
                static_cast<uint32_t>(required.size()), &audioExclusionGeneration)) {
            audioPendingPopupIds = ids;
            exclusionPollTimer.start();
        } else {
            audioFailedPopupIds = ids;
            reportAudioExclusionError();
        }
        return false;
#else
        return true;
#endif
    }

    void reportAudioExclusionError() {
        if (audioExclusionErrorReported || !toolbarWindow)
            return;
        audioExclusionErrorReported = true;
        adqt::widgets::AdMessage::Request request;
        request.key = QStringLiteral("recording-audio-exclusion-error");
        request.content = QCoreApplication::translate(
            "ScreenRecordingController", "Unable to exclude audio controls from recording");
        adqt::widgets::AdMessageService::warning(std::move(request), toolbarWindow);
    }

    void pollAudioExclusions() {
#ifdef Q_OS_MACOS
        if (!recordingSession || sessionStatus.busy() || !audioExclusionGeneration) {
            exclusionPollTimer.stop();
            return;
        }
        SnowRecordingExclusionStatus status{};
        if (!snow_recording_session_exclusion_status(recordingSession.get(), &status))
            return;
        if (status.requested_generation != audioExclusionGeneration || status.status == 1)
            return;
        exclusionPollTimer.stop();
        if (status.status == 0 && status.applied_generation == audioExclusionGeneration) {
            audioAppliedPopupIds = audioPendingPopupIds;
            audioExclusionGeneration = 0;
            if (toolbarWindow)
                for (bool microphone : {false, true}) {
                    auto* control = toolbarWindow->palette()->recordingAudioGainPopover(microphone);
                    if (control->popover()->isVisible())
                        control->popover()->refreshPopupLayout();
                }
        } else {
            audioFailedPopupIds = audioPendingPopupIds;
            audioExclusionGeneration = 0;
            reportAudioExclusionError();
        }
#endif
    }

    void syncPreview() {
        if (uiSession == nullptr) {
            return;
        }
        const bool eligible =
            sessionStatus.state() == ScreenshotToolPalette::RecordingState::Idle &&
            !sessionStatus.busy() && !startScheduled && recordingSession == nullptr;
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
                dimensionsFuture =
                    std::async(std::launch::async, [region, maximum, format, generation] {
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
                    });
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
            recordMouseClicks);
        uiSession->preview->setEligible(true);
    }

    void syncUi() {
        refreshAudioGains();
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
            palette->setRecordingKeyboardBackgroundColor(keyboardBackgroundColor);
            palette->setRecordingKeyboardForegroundColor(keyboardForegroundColor);
            palette->setRecordingMouseClickColor(mouseClickColor);
            palette->setRecordingMouseHighlightEnabled(mouseHighlightEnabled);
            palette->setRecordingRecordMouseClicks(recordMouseClicks);
            palette->setRecordingMouseHighlightColor(mouseHighlightColor);
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
        report(QStringLiteral("recording.failed"), QtWarningMsg);
        automationError = message;
        automationRevision = snow_shot::presentation::nextAutomationRevision();
        if (automationOwned || automationCommand)
            return;
        QMessageBox::critical(toolbarWindow, tr("Screen recording"),
                              message.isEmpty() ? tr("The recording operation failed") : message);
    }

    PermissionCheck permissionCheck;
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
    RecordingRenderJob* renderJob = nullptr;
    QString retainedSourcePath;
    bool sessionDeferred = false;
    bool postProcessingEnabled = false;
    QString postProcessingEffect = QStringLiteral("progress_bar");
    QColor progressBarColor{22, 119, 255};
    std::future<StartAttemptResult> startFuture;
    ScreenshotToolPalette::RecordingSessionStatus sessionStatus =
        ScreenshotToolPalette::RecordingSessionStatus::idle();
    qint64 durationMilliseconds = 0;
    QString pendingOutputPath;
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
    bool audioPopoversExcluded = false;
    bool audioExclusionErrorReported = false;
    uint64_t audioExclusionGeneration = 0;
    std::array<uint32_t, 2> audioAppliedPopupIds{};
    std::array<uint32_t, 2> audioPendingPopupIds{};
    std::array<uint32_t, 2> audioFailedPopupIds{};
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
    QColor mouseHighlightColor{255, 255, 0, 128};
    bool showCursor = true;
    bool showKeyboard = false;
    DirectRecordingSettings sessionOutputSettings;
    QColor sessionMouseTrailColor{0, 0, 0, 0};
    QColor sessionMouseClickColor{0, 0, 0, 0};
    bool sessionShowCursor = true;
    void report(const QString& event, QtMsgType level = QtInfoMsg) const {
        snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.recording"), event,
                                         {{QStringLiteral("operation"), operation},
                                          {QStringLiteral("duration_ms"),
                                           operationTimer.isValid() ? operationTimer.elapsed() : 0},
                                          {QStringLiteral("backend"),
#ifdef Q_OS_MACOS
                                           QStringLiteral("screencapturekit")
#else
                                           QStringLiteral("auto")
#endif
                                          }},
                                         level);
    }
    QString operation;
    QElapsedTimer operationTimer;
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

ScreenRecordingController::~ScreenRecordingController() = default;

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

void ScreenRecordingController::startRecording() {
    m_impl->start();
}

void ScreenRecordingController::stopRecordingAndCopy() {
    m_impl->stop(true);
}

void ScreenRecordingController::openRecordingFolder() {
    m_impl->openFolder();
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
        {QStringLiteral("state"), states.at(static_cast<int>(s.sessionStatus.state()))},
        {QStringLiteral("operation"), busy.at(static_cast<int>(s.sessionStatus.busyOperation()))},
        {QStringLiteral("busy"), s.sessionStatus.busy() || s.startScheduled},
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
    if (s.isOpen() || s.sessionStatus.busy() || s.recordingSession || s.startScheduled)
        return fail("busy");
    if (!region.isValid() || region.width() > 32768 || region.height() > 32768)
        return fail("invalid_region");
    const QHash<QString, QStringList> enums{
        {QStringLiteral("post_processing_effect"),
         {QStringLiteral("progress_bar"), QStringLiteral("playback_time")}},
        {QStringLiteral("format"),
         {QStringLiteral("mp4"), QStringLiteral("gif"), QStringLiteral("apng"),
          QStringLiteral("webp")}},
        {QStringLiteral("clarity"),
         {QStringLiteral("4k"), QStringLiteral("2k"), QStringLiteral("1080p"),
          QStringLiteral("720p"), QStringLiteral("480p")}},
        {QStringLiteral("animated_clarity"),
         {QStringLiteral("1080p"), QStringLiteral("720p"), QStringLiteral("480p")}},
        {QStringLiteral("encoder"),
         {QStringLiteral("h264"), QStringLiteral("h265"), QStringLiteral("h264_hw")}},
        {QStringLiteral("encoding_preset"),
         {QStringLiteral("ultrafast"), QStringLiteral("veryfast"), QStringLiteral("medium"),
          QStringLiteral("veryslow"), QStringLiteral("placebo")}}};
    const QStringList booleans{QStringLiteral("post_processing"),
                               QStringLiteral("microphone"),
                               QStringLiteral("system_audio"),
                               QStringLiteral("separate_audio_tracks"),
                               QStringLiteral("loop"),
                               QStringLiteral("capture_toolbar"),
                               QStringLiteral("show_cursor"),
                               QStringLiteral("show_keyboard"),
                               QStringLiteral("mouse_highlight"),
                               QStringLiteral("record_mouse_clicks")};
    const QStringList colors{
        QStringLiteral("progress_bar_color"),  QStringLiteral("keyboard_background"),
        QStringLiteral("keyboard_foreground"), QStringLiteral("mouse_trail"),
        QStringLiteral("mouse_click"),         QStringLiteral("mouse_highlight_color")};
    const QHash<QString, QPair<int, int>> integers{
        {QStringLiteral("system_audio_gain_db"), {-24, 24}},
        {QStringLiteral("microphone_gain_db"), {-24, 24}},
        {QStringLiteral("start_delay_seconds"), {0, 10}},
        {QStringLiteral("frame_rate"), {1, 120}},
        {QStringLiteral("quality"), {0, 100}},
        {QStringLiteral("animated_frame_rate"), {1, 24}},
        {QStringLiteral("mouse_trail_duration_ms"), {100, 2000}},
        {QStringLiteral("keyboard_size"), {32, 128}}};
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (enums.contains(it.key())) {
            if (!it->isString() || !enums.value(it.key()).contains(it->toString()))
                return fail("invalid_parameters");
        } else if (booleans.contains(it.key())) {
            if (!it->isBool())
                return fail("invalid_parameters");
        } else if (colors.contains(it.key())) {
            const QColor parsed = it.key() == QStringLiteral("progress_bar_color")
                                      ? progressBarColorFromString(it->toString())
                                      : QColor(it->toString());
            if (!it->isString() || !parsed.isValid())
                return fail("invalid_parameters");
        } else if (integers.contains(it.key())) {
            const double n = it->toDouble(-1);
            const auto range = integers.value(it.key());
            if (!it->isDouble() || !std::isfinite(n) || std::floor(n) != n || n < range.first ||
                n > range.second)
                return fail("invalid_parameters");
            if (it.key() == QStringLiteral("frame_rate") &&
                !QList<int>{5, 10, 15, 24, 30, 60, 120, 83}.contains(static_cast<int>(n)))
                return fail("invalid_parameters");
            if (it.key() == QStringLiteral("animated_frame_rate") &&
                !QList<int>{5, 10, 15, 24}.contains(static_cast<int>(n)))
                return fail("invalid_parameters");
        } else if (it.key() == QStringLiteral("path")) {
            if (!it->isString() || !QFileInfo(it->toString()).isAbsolute() ||
                QFileInfo::exists(it->toString()) ||
                !QFileInfo(QFileInfo(it->toString()).absolutePath()).isDir())
                return fail("output_path_unavailable");
        } else
            return fail("invalid_parameters");
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
    s.recordMouseClicks =
        options.value(QStringLiteral("record_mouse_clicks")).toBool(settings.recordMouseClicks());
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
    if (m_impl->automationOwned)
        m_impl->close();
}
