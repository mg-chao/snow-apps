#include "snow_shot/presentation/screenrecordingcontroller.h"

#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "screenrecordinggeometry.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/storageusagetracker.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include "snow_shot/platform/windows/windowchrome.h"
#endif

#include "snow_capture.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <future>
#include <utility>

namespace {
constexpr int kDurationTickMilliseconds = 100;

struct RecordingExportSettings {
    SnowCaptureRecordingExportFormat format = SNOW_CAPTURE_RECORDING_EXPORT_FORMAT_MP4;
    SnowCaptureVideoCodec codec = SNOW_CAPTURE_VIDEO_CODEC_H264;
    SnowCaptureVideoEncodingPreset preset = SNOW_CAPTURE_VIDEO_ENCODING_PRESET_VERYFAST;
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

RecordingExportSettings recordingExportSettings(bool animatedImage, const QSize& captureSize) {
    const snow_shot::storage::RecordingSettings settings;
    RecordingExportSettings result;
    const QString encoder = settings.encoder();
    result.codec = videoCodec(encoder);
    result.preset = videoEncodingPreset(settings.encodingPreset());
    result.useHardwareEncoder = encoder == QStringLiteral("h264_hw");

    if (!animatedImage) {
        result.maximumSize =
            snow_shot::presentation::recording::screenRecordingMaximumSizeForClarity(
                settings.screenRecordingClarity());
        result.maximumSize =
            snow_shot::presentation::recording::screenRecordingOrientedMaximumSize(
                result.maximumSize, captureSize);
        result.targetFps = static_cast<uint32_t>(validRecordingFrameRate(settings.frameRate()));
        return result;
    }

    result.maximumSize =
        snow_shot::presentation::recording::screenRecordingMaximumSizeForClarity(
            settings.animatedImageClarity());
    result.maximumSize = snow_shot::presentation::recording::screenRecordingOrientedMaximumSize(
        result.maximumSize, captureSize);
    result.targetFps =
        static_cast<uint32_t>(validAnimatedImageFrameRate(settings.animatedImageFrameRate()));
    const QString format = settings.animatedImageFormat();
    if (format == QStringLiteral("apng")) {
        result.format = SNOW_CAPTURE_RECORDING_EXPORT_FORMAT_APNG;
        result.extension = QStringLiteral("apng");
    } else if (format == QStringLiteral("webp")) {
        result.format = SNOW_CAPTURE_RECORDING_EXPORT_FORMAT_WEBP;
        result.extension = QStringLiteral("webp");
    } else {
        result.format = SNOW_CAPTURE_RECORDING_EXPORT_FORMAT_GIF;
        result.extension = QStringLiteral("gif");
    }
    return result;
}

QStringList defaultRecordingDirectories() {
    QStringList directories;
    for (QStandardPaths::StandardLocation location :
         {QStandardPaths::MoviesLocation, QStandardPaths::DocumentsLocation}) {
        const QString root = QStandardPaths::writableLocation(location);
        if (root.isEmpty()) {
            continue;
        }
        const QString directory = QDir(root).filePath(QStringLiteral("SnowShot"));
        if (!directories.contains(directory, Qt::CaseInsensitive)) {
            directories.push_back(directory);
        }
    }
    return directories;
}

QStringList recordingDirectories() {
    QStringList directories;
    const QString configured = QDir::cleanPath(
        snow_shot::storage::RecordingSettings().videoSaveDirectory().trimmed());
    const QFileInfo configuredInfo(configured);
    if (!configured.isEmpty() && configuredInfo.isDir() && configuredInfo.isWritable()) {
        directories.push_back(configured);
    }
    for (const QString& fallback : defaultRecordingDirectories()) {
        if (!directories.contains(fallback, Qt::CaseInsensitive)) {
            directories.push_back(fallback);
        }
    }
    return directories;
}

QString recordingDirectory() {
    const QStringList directories = recordingDirectories();
    for (const QString& candidate : directories) {
        QDir directory(candidate);
        if ((directory.exists() || directory.mkpath(QStringLiteral("."))) &&
            QFileInfo(directory.absolutePath()).isWritable()) {
            return directory.absolutePath();
        }
    }
    return directories.isEmpty() ? QString() : directories.constFirst();
}

QString recordingFilePath(const QString& extension) {
    const QString baseName = ScreenshotImageFileService::suggestedBaseName(
        snow_shot::storage::RecordingSettings().videoFilenameFormat());
    const QDir directory(recordingDirectory());
    const QString normalizedExtension = extension.startsWith(QLatin1Char('.'))
                                            ? extension
                                            : QStringLiteral(".%1").arg(extension);
    QString path = directory.filePath(baseName + normalizedExtension);
    for (int suffix = 1; QFileInfo::exists(path); ++suffix) {
        path = directory.filePath(
            QStringLiteral("%1_%2%3").arg(baseName).arg(suffix).arg(normalizedExtension));
    }
    return path;
}

QString recordingWorkingDirectory() {
    return snow_shot::storage::StorageUsageTracker::defaultRecordingTempDirectory();
}

QString captureError() {
    const char* error = snow_capture_last_error_message();
    return QString::fromUtf8(error != nullptr ? error : "Unknown recording error");
}

void copyFileToClipboard(const QString& filePath) {
    auto* mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile(filePath)});
    QApplication::clipboard()->setMimeData(mimeData);
}
} // namespace

struct ScreenRecordingController::Impl {
    explicit Impl(ScreenRecordingController& owner) : owner(owner) {
        const snow_shot::storage::RecordingSettings settings;
        microphoneEnabled = settings.microphoneEnabled();
        systemAudioEnabled = settings.systemAudioEnabled();
        durationTimer.setInterval(kDurationTickMilliseconds);
        durationTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&durationTimer, &QTimer::timeout, &owner, [this]() {
            if (state != ScreenshotToolPalette::RecordingState::Recording) {
                return;
            }
            durationMilliseconds += kDurationTickMilliseconds;
            syncUi();
        });
        exportPollTimer.setInterval(50);
        QObject::connect(&exportPollTimer, &QTimer::timeout, &owner, [this]() { pollExport(); });
    }

    ~Impl() {
        durationTimer.stop();
        exportPollTimer.stop();
        if (exportFuture.valid()) {
            exportFuture.wait();
            exportFuture.get();
        }
        if (recordingSession != nullptr) {
            snow_capture_recording_session_destroy(recordingSession);
        }
        recordingSession = nullptr;
        restoreToolbarCaptureVisibility();
        if (toolbarWindow != nullptr) {
            toolbarWindow->hide();
            toolbarWindow->deleteLater();
        }
        if (areaWindow != nullptr) {
            areaWindow->hide();
            areaWindow->deleteLater();
        }
    }

    void open(const QRect& region) {
        if (!region.isValid() || region.isEmpty() || busy) {
            return;
        }
        if (isOpen()) {
            if (state != ScreenshotToolPalette::RecordingState::Idle) {
                return;
            }
            physicalRegion = region;
            updateCaptureRegion();
            areaWindow->setPhysicalRegion(region);
            toolbarWindow->placeForPhysicalRegion(region);
            areaWindow->show();
            toolbarWindow->show();
            areaWindow->raise();
            toolbarWindow->raise();
            return;
        }

        physicalRegion = region;
        updateCaptureRegion();
        areaWindow = new ScreenRecordingAreaWindow();
        toolbarWindow = new ScreenRecordingToolbarWindow();
        areaWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        toolbarWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        areaWindow->setPhysicalRegion(region);
        toolbarWindow->placeForPhysicalRegion(region);
        connectToolbar();

        state = ScreenshotToolPalette::RecordingState::Idle;
        durationMilliseconds = 0;
        syncUi();

        areaWindow->show();
        toolbarWindow->show();
        areaWindow->raise();
        toolbarWindow->raise();
    }

    bool isOpen() const {
        return areaWindow != nullptr && toolbarWindow != nullptr &&
               (areaWindow->isVisible() || toolbarWindow->isVisible());
    }

    void connectToolbar() {
        ScreenshotToolPalette* palette =
            toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
        if (palette == nullptr) {
            return;
        }
        QObject::connect(palette, &ScreenshotToolPalette::recordingStartRequested, &owner,
                         [this]() { start(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingStopRequested, &owner,
                         [this]() { stop(false, false, false); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingPauseRequested, &owner,
                         [this]() { pause(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingResumeRequested, &owner,
                         [this]() { resume(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMicrophoneToggled, &owner,
                         [this](bool enabled) {
                             microphoneEnabled = enabled;
                             snow_shot::storage::RecordingSettings().setMicrophoneEnabled(enabled);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingSystemAudioToggled, &owner,
                         [this](bool enabled) {
                             systemAudioEnabled = enabled;
                             snow_shot::storage::RecordingSettings().setSystemAudioEnabled(enabled);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingOpenFolderRequested, &owner,
                         [this]() { openFolder(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCloseRequested, &owner,
                         [this]() { close(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCopyAnimatedImageRequested,
                         &owner,
                         [this]() { stop(true, true, false); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCopyVideoRequested, &owner,
                         [this]() { stop(false, true, false); });
    }

    void start() {
        if (state != ScreenshotToolPalette::RecordingState::Idle || busy || startScheduled ||
            recordingSession != nullptr) {
            return;
        }
        startScheduled = true;
        QTimer::singleShot(0, &owner, [this]() {
            startScheduled = false;
            if (state != ScreenshotToolPalette::RecordingState::Idle || busy ||
                recordingSession != nullptr || !isOpen()) {
                return;
            }
            busy = true;
            syncUi();
            QDir outputDirectory(recordingDirectory());
            QDir workingDirectory(recordingWorkingDirectory());
            if (!outputDirectory.mkpath(QStringLiteral(".")) ||
                !workingDirectory.mkpath(QStringLiteral("."))) {
                busy = false;
                syncUi();
                showError(tr("Unable to create the recording directories"));
                return;
            }

            const QByteArray workingDirectoryUtf8 =
                QDir::toNativeSeparators(workingDirectory.absolutePath()).toUtf8();
            const snow_shot::storage::RecordingSettings settings;
            const SnowCaptureRecordingConfig config{
                captureRegion.x(),
                captureRegion.y(),
                static_cast<uint32_t>(captureRegion.width()),
                static_cast<uint32_t>(captureRegion.height()),
                static_cast<uint32_t>(validRecordingFrameRate(settings.frameRate())),
                static_cast<uint8_t>(microphoneEnabled),
                static_cast<uint8_t>(systemAudioEnabled),
                static_cast<uint8_t>(SNOW_CAPTURE_BACKEND_WGC),
                0,
                workingDirectoryUtf8.constData(),
                {},
            };
            recordingSession = snow_capture_recording_session_create(&config);
            if (recordingSession != nullptr && settings.hideToolbarInRecording()) {
                excludeToolbarFromCapture();
            }
            if (recordingSession == nullptr ||
                snow_capture_recording_session_start(recordingSession) == 0) {
                const QString error = captureError();
                if (recordingSession != nullptr) {
                    snow_capture_recording_session_destroy(recordingSession);
                    recordingSession = nullptr;
                }
                restoreToolbarCaptureVisibility();
                busy = false;
                syncUi();
                if (areaWindow != nullptr) {
                    areaWindow->show();
                    areaWindow->raise();
                }
                if (toolbarWindow != nullptr) {
                    toolbarWindow->show();
                    toolbarWindow->raise();
                }
                showError(error);
                return;
            }

            durationMilliseconds = 0;
            state = ScreenshotToolPalette::RecordingState::Recording;
            busy = false;
            syncUi();
            if (areaWindow != nullptr) {
                areaWindow->show();
                areaWindow->raise();
            }
            if (toolbarWindow != nullptr) {
                if (!toolbarHiddenForCapture) {
                    toolbarWindow->show();
                    toolbarWindow->raise();
                }
            }
            durationTimer.start();
        });
    }

    void pause() {
        if (recordingSession == nullptr ||
            state != ScreenshotToolPalette::RecordingState::Recording || busy) {
            return;
        }
        if (snow_capture_recording_session_pause(recordingSession) == 0) {
            showError(captureError());
            return;
        }
        state = ScreenshotToolPalette::RecordingState::Paused;
        durationTimer.stop();
        syncUi();
    }

    void resume() {
        if (recordingSession == nullptr || state != ScreenshotToolPalette::RecordingState::Paused ||
            busy) {
            return;
        }
        if (snow_capture_recording_session_resume(recordingSession) == 0) {
            showError(captureError());
            return;
        }
        state = ScreenshotToolPalette::RecordingState::Recording;
        durationTimer.start();
        syncUi();
    }

    void stop(bool animatedImage, bool copyToClipboard, bool closeAfter) {
        if (busy) {
            return;
        }
        if (state == ScreenshotToolPalette::RecordingState::Idle || recordingSession == nullptr) {
            if (closeAfter) {
                hideWindows();
            }
            return;
        }

        durationTimer.stop();
        durationMilliseconds = 0;
        busy = true;
        syncUi();
        const RecordingExportSettings exportSettings =
            recordingExportSettings(animatedImage, captureRegion.size());
        const QString outputPath = recordingFilePath(exportSettings.extension);
        const QByteArray outputUtf8 = QDir::toNativeSeparators(outputPath).toUtf8();
        SnowCaptureRecordingSession* session = recordingSession;
        exportFuture = std::async(std::launch::async, [session, outputUtf8, exportSettings]() {
            const SnowCaptureRecordingExportConfig config{
                SNOW_CAPTURE_RECORDING_EXPORT_CONFIG_VERSION,
                sizeof(SnowCaptureRecordingExportConfig),
                outputUtf8.constData(),
                static_cast<uint32_t>(exportSettings.format),
                static_cast<uint32_t>(exportSettings.maximumSize.width()),
                static_cast<uint32_t>(exportSettings.maximumSize.height()),
                exportSettings.targetFps,
                static_cast<uint32_t>(exportSettings.codec),
                static_cast<uint32_t>(exportSettings.preset),
                static_cast<uint32_t>(exportSettings.useHardwareEncoder
                                          ? SNOW_CAPTURE_ENCODER_PREFERENCE_H264_HARDWARE
                                          : SNOW_CAPTURE_ENCODER_PREFERENCE_SOFTWARE),
                {},
            };
            const bool ok = snow_capture_recording_session_stop_and_export(session, &config) != 0;
            return std::make_pair(ok, ok ? QString() : captureError());
        });
        pendingOutputPath = outputPath;
        pendingCopyToClipboard = copyToClipboard;
        pendingCloseAfter = closeAfter;
        exportPollTimer.start();
    }

    void pollExport() {
        if (!exportFuture.valid() ||
            exportFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            return;
        }
        exportPollTimer.stop();
        const std::pair<bool, QString> result = exportFuture.get();
        const bool ok = result.first;
        const QString& error = result.second;
        if (recordingSession != nullptr) {
            snow_capture_recording_session_destroy(recordingSession);
            recordingSession = nullptr;
        }
        restoreToolbarCaptureVisibility();
        state = ScreenshotToolPalette::RecordingState::Idle;
        busy = false;
        durationMilliseconds = 0;
        syncUi();

        if (!ok) {
            showError(error);
            return;
        }
        if (pendingCopyToClipboard) {
            copyFileToClipboard(pendingOutputPath);
        }
        if (pendingCloseAfter) {
            hideWindows();
        }
    }

    void openFolder() {
        QDir directory(recordingDirectory());
        directory.mkpath(QStringLiteral("."));
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory.absolutePath()));
    }

    void close() {
        if (state == ScreenshotToolPalette::RecordingState::Idle) {
            hideWindows();
            return;
        }
        stop(false, false, true);
    }

    void hideWindows() {
        restoreToolbarCaptureVisibility();
        if (toolbarWindow != nullptr) {
            toolbarWindow->hide();
        }
        if (areaWindow != nullptr) {
            areaWindow->hide();
        }
    }

    void excludeToolbarFromCapture() {
        restoreToolbarCaptureVisibility();
        if (toolbarWindow == nullptr) {
            return;
        }
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (snow_shot::platform::windows::setWindowExcludedFromCapture(toolbarWindow, true)) {
            toolbarExcludedFromCapture = true;
            return;
        }
#endif
        toolbarWindow->hide();
        toolbarHiddenForCapture = true;
    }

    void restoreToolbarCaptureVisibility() {
        if (toolbarWindow == nullptr) {
            toolbarExcludedFromCapture = false;
            toolbarHiddenForCapture = false;
            return;
        }
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (toolbarExcludedFromCapture) {
            static_cast<void>(snow_shot::platform::windows::setWindowExcludedFromCapture(
                toolbarWindow, false));
        }
#endif
        const bool showToolbar = toolbarHiddenForCapture &&
                                 (areaWindow == nullptr || areaWindow->isVisible());
        toolbarExcludedFromCapture = false;
        toolbarHiddenForCapture = false;
        if (showToolbar) {
            toolbarWindow->show();
            toolbarWindow->raise();
        }
    }

    void syncUi() {
        if (areaWindow != nullptr) {
            areaWindow->setRecordingState(state);
        }
        ScreenshotToolPalette* palette =
            toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
        if (palette != nullptr) {
            palette->setRecordingState(state);
            palette->setRecordingDuration(durationMilliseconds);
            palette->setRecordingMicrophoneEnabled(microphoneEnabled);
            palette->setRecordingSystemAudioEnabled(systemAudioEnabled);
            palette->setRecordingBusy(busy);
        }
    }

    void updateCaptureRegion() {
        QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(physicalRegion);
        const QRect bounds =
            screen != nullptr ? ScreenshotGeometryMapper::physicalRectForScreen(*screen) : QRect();
        captureRegion = snow_shot::presentation::recording::screenRecordingCompatibleCaptureRegion(
            physicalRegion, bounds);
    }

    void showError(const QString& message) {
        QMessageBox::critical(toolbarWindow, tr("Screen recording"),
                              message.isEmpty() ? tr("The recording operation failed") : message);
    }

    ScreenRecordingController& owner;
    ScreenRecordingAreaWindow* areaWindow = nullptr;
    ScreenRecordingToolbarWindow* toolbarWindow = nullptr;
    SnowCaptureRecordingSession* recordingSession = nullptr;
    QRect physicalRegion;
    QRect captureRegion;
    QTimer durationTimer;
    QTimer exportPollTimer;
    std::future<std::pair<bool, QString>> exportFuture;
    ScreenshotToolPalette::RecordingState state = ScreenshotToolPalette::RecordingState::Idle;
    qint64 durationMilliseconds = 0;
    QString pendingOutputPath;
    bool microphoneEnabled = false;
    bool systemAudioEnabled = true;
    bool busy = false;
    bool startScheduled = false;
    bool pendingCopyToClipboard = false;
    bool pendingCloseAfter = false;
    bool toolbarExcludedFromCapture = false;
    bool toolbarHiddenForCapture = false;
};

ScreenRecordingController::ScreenRecordingController(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}

ScreenRecordingController::~ScreenRecordingController() = default;

void ScreenRecordingController::open(const QRect& physicalRegion) {
    m_impl->open(physicalRegion);
}

bool ScreenRecordingController::isOpen() const {
    return m_impl->isOpen();
}

bool ScreenRecordingController::isRecording() const {
    return m_impl->state != ScreenshotToolPalette::RecordingState::Idle;
}

void ScreenRecordingController::startRecording() {
    m_impl->start();
}

void ScreenRecordingController::stopRecordingAndCopyVideo() {
    m_impl->stop(false, true, false);
}
