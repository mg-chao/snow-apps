#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QUuid>
#include <QElapsedTimer>

#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenrecordingshortcutcontroller.h"
#include "screenrecordinggeometry.h"
#include "../capture/windowcaptureexclusion.h"
#include "snow_shot/storage/settingsadapters.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include "snow_shot/platform/windows/windowchrome.h"
#endif

#include "snow_capture.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

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

struct DirectRecordingSettings {
    SnowCaptureRecordingOutputFormat format = SNOW_CAPTURE_RECORDING_OUTPUT_FORMAT_MP4;
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

DirectRecordingSettings directRecordingSettings(const QString& outputFormat,
                                                const QSize& captureSize) {
    const snow_shot::storage::RecordingSettings settings;
    DirectRecordingSettings result;
    const QString encoder = settings.encoder();
    result.codec = videoCodec(encoder);
    result.preset = videoEncodingPreset(settings.encodingPreset());
    result.useHardwareEncoder = encoder == QStringLiteral("h264_hw");

    if (outputFormat == QStringLiteral("mp4")) {
        result.maximumSize =
            snow_shot::presentation::recording::screenRecordingMaximumSizeForClarity(
                settings.screenRecordingClarity());
        result.maximumSize = snow_shot::presentation::recording::screenRecordingOrientedMaximumSize(
            result.maximumSize, captureSize);
        result.targetFps = static_cast<uint32_t>(validRecordingFrameRate(settings.frameRate()));
        return result;
    }

    result.maximumSize = snow_shot::presentation::recording::screenRecordingMaximumSizeForClarity(
        settings.animatedImageClarity());
    result.maximumSize = snow_shot::presentation::recording::screenRecordingOrientedMaximumSize(
        result.maximumSize, captureSize);
    result.targetFps =
        static_cast<uint32_t>(validAnimatedImageFrameRate(settings.animatedImageFrameRate()));
    if (outputFormat == QStringLiteral("apng")) {
        result.format = SNOW_CAPTURE_RECORDING_OUTPUT_FORMAT_APNG;
        result.extension = QStringLiteral("apng");
    } else if (outputFormat == QStringLiteral("webp")) {
        result.format = SNOW_CAPTURE_RECORDING_OUTPUT_FORMAT_WEBP;
        result.extension = QStringLiteral("webp");
    } else {
        result.format = SNOW_CAPTURE_RECORDING_OUTPUT_FORMAT_GIF;
        result.extension = QStringLiteral("gif");
    }
    return result;
}

QStringList defaultRecordingDirectories() {
    QStringList directories;
    for (QStandardPaths::StandardLocation location :
         {QStandardPaths::MoviesLocation, QStandardPaths::DocumentsLocation}) {
        const QString directory = QStandardPaths::writableLocation(location);
        if (directory.isEmpty()) {
            continue;
        }
        if (!directories.contains(directory, Qt::CaseInsensitive)) {
            directories.push_back(directory);
        }
    }
    return directories;
}

QStringList recordingDirectories() {
    QStringList directories;
    const QString configured =
        QDir::cleanPath(snow_shot::storage::RecordingSettings().videoSaveDirectory().trimmed());
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
    const QString normalizedExtension =
        extension.startsWith(QLatin1Char('.')) ? extension : QStringLiteral(".%1").arg(extension);
    QString path = directory.filePath(baseName + normalizedExtension);
    for (int suffix = 1; QFileInfo::exists(path); ++suffix) {
        path = directory.filePath(
            QStringLiteral("%1_%2%3").arg(baseName).arg(suffix).arg(normalizedExtension));
    }
    return path;
}

QString captureError() {
    const char* error = snow_capture_last_error_message();
    return QString::fromUtf8(error != nullptr ? error : "Unknown recording error");
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

struct ScreenRecordingController::Impl {
    explicit Impl(ScreenRecordingController& owner) : owner(owner) {
        const snow_shot::storage::RecordingSettings settings;
        microphoneEnabled = settings.microphoneEnabled();
        systemAudioEnabled = settings.systemAudioEnabled();
        outputFormat = settings.outputFormat();
        mouseTrailColor = settings.mouseTrailColor();
        mouseClickColor = settings.mouseClickColor();
        showCursor = settings.showCursor();
        durationTimer.setInterval(kDurationTickMilliseconds);
        durationTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&durationTimer, &QTimer::timeout, &owner, [this]() {
            if (pollSessionLiveness()) {
                return;
            }
            if (state != ScreenshotToolPalette::RecordingState::Recording) {
                return;
            }
            durationMilliseconds += kDurationTickMilliseconds;
            syncUi();
        });
        finalizationPollTimer.setInterval(50);
        QObject::connect(&finalizationPollTimer, &QTimer::timeout, &owner,
                         [this]() { pollFinalization(); });
    }

    ~Impl() {
        durationTimer.stop();
        finalizationPollTimer.stop();
        if (finalizationFuture.valid()) {
            finalizationFuture.wait();
            finalizationFuture.get();
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
        shortcutController = std::make_unique<ScreenRecordingShortcutController>(
            *areaWindow, *toolbarWindow, &owner);

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
        connectDrawingToolbar(*palette);
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::physicalRegionChanged, &owner,
                         [this](const QRect& region) {
                             if (state != ScreenshotToolPalette::RecordingState::Idle || busy) {
                                 return;
                             }
                             physicalRegion = region;
                             updateCaptureRegion();
                             toolbarWindow->placeForPhysicalRegion(region);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingExportSettingsVisibleChanged,
                         &owner, [this](bool visible) {
                             areaWindow->setInputMode(
                                 visible ? ScreenRecordingAreaWindow::InputMode::RegionEditing
                                         : ScreenRecordingAreaWindow::InputMode::PassThrough);
                         });
        if (palette->recordingExportSettingsVisible()) {
            areaWindow->setInputMode(ScreenRecordingAreaWindow::InputMode::RegionEditing);
        }
        QObject::connect(palette, &ScreenshotToolPalette::recordingStartRequested, &owner,
                         [this]() { start(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingStopRequested, &owner,
                         [this]() { stop(false, false); });
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
        QObject::connect(palette, &ScreenshotToolPalette::recordingCopyRequested, &owner,
                         [this]() { stop(true, false); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingOutputFormatChanged, &owner,
                         [this](const QString& format) {
                             outputFormat = format;
                             snow_shot::storage::RecordingSettings().setOutputFormat(format);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseTrailColorChanged, &owner,
                         [this](const QColor& color) {
                             mouseTrailColor = color;
                             snow_shot::storage::RecordingSettings().setMouseTrailColor(color);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseClickColorChanged, &owner,
                         [this](const QColor& color) {
                             mouseClickColor = color;
                             snow_shot::storage::RecordingSettings().setMouseClickColor(color);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCursorVisibleChanged, &owner,
                         [this](bool visible) {
                             showCursor = visible;
                             snow_shot::storage::RecordingSettings().setShowCursor(visible);
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
        QObject::connect(
            &palette, &ScreenshotToolPalette::selectRequested, &owner, [this, &palette]() {
                palette.clearActiveTool();
                areaWindow->setInputMode(ScreenRecordingAreaWindow::InputMode::PassThrough);
            });
        QObject::connect(&palette, &ScreenshotToolPalette::shapeRequested, &owner,
                         [activate]() { activate(SnowCanvasTool::Shape); });
        QObject::connect(&palette, &ScreenshotToolPalette::arrowRequested, &owner,
                         [activate]() { activate(SnowCanvasTool::Arrow); });
        QObject::connect(&palette, &ScreenshotToolPalette::lineRequested, &owner,
                         [activate]() { activate(SnowCanvasTool::Line); });
        QObject::connect(&palette, &ScreenshotToolPalette::freeDrawRequested, &owner,
                         [activate]() { activate(SnowCanvasTool::FreeDraw); });
        QObject::connect(&palette, &ScreenshotToolPalette::spotlightRequested, &owner,
                         [activate]() { activate(SnowCanvasTool::Spotlight); });
        QObject::connect(&palette, &ScreenshotToolPalette::eraserRequested, &owner,
                         [activate]() { activate(SnowCanvasTool::Eraser); });
        QObject::connect(&palette, &ScreenshotToolPalette::watermarkRequested, &owner,
                         [activate]() { activate(SnowCanvasTool::Watermark); });
        QObject::connect(&palette, &ScreenshotToolPalette::textRequested, &owner,
                         [activate]() { activate(SnowCanvasTool::Text); });
        QObject::connect(&palette, &ScreenshotToolPalette::serialNumberRequested, &owner,
                         [activate]() { activate(SnowCanvasTool::SerialNumber); });
        QObject::connect(&palette, &ScreenshotToolPalette::undoRequested, &owner,
                         [canvas]() { static_cast<void>(canvas->undo()); });
        QObject::connect(&palette, &ScreenshotToolPalette::redoRequested, &owner,
                         [canvas]() { static_cast<void>(canvas->redo()); });
        QObject::connect(
            canvas, &SnowCanvasWidget::historyStateChanged, &owner,
            [&palette, canvas]() { palette.setHistoryState(canvas->canvasHistoryState()); });
        QObject::connect(canvas, &SnowCanvasWidget::styleToolbarStateChanged, &owner,
                         [&palette, canvas]() {
                             palette.setStyleToolbarState(canvas->canvasStyleToolbarState());
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::shapeStyleChanged, &owner,
                         [canvas, &palette](const SnowCanvasShapeStyle& style, quint32 properties,
                                            SnowCanvasShapeKind kind) {
                             canvas->setCanvasShapeStylePatch(style, properties, kind);
                             static_cast<void>(
                                 snow_shot::presentation::persistScreenshotCanvasToolStyles(
                                     palette.creationStyleDefaults()));
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::textStyleChanged, &owner,
                         [canvas, &palette](const SnowCanvasTextStyle& style) {
                             static_cast<void>(canvas->setCanvasTextStyle(style));
                             static_cast<void>(
                                 snow_shot::presentation::persistScreenshotCanvasToolStyles(
                                     palette.creationStyleDefaults()));
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::serialNumberStyleChanged, &owner,
                         [canvas, &palette](const SnowCanvasSerialNumberStyle& style) {
                             static_cast<void>(canvas->setCanvasSerialNumberStyle(style));
                             static_cast<void>(
                                 snow_shot::presentation::persistScreenshotCanvasToolStyles(
                                     palette.creationStyleDefaults()));
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::watermarkConfigChanged, &owner,
                         [canvas](const SnowCanvasWatermarkConfig& config) {
                             static_cast<void>(canvas->setCanvasWatermarkConfig(config));
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::watermarkPreviewChanged, &owner,
                         [canvas](const SnowCanvasWatermarkConfig& config) {
                             canvas->previewCanvasWatermarkConfig(config);
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::spotlightConfigChanged, &owner,
                         [canvas](const SnowCanvasSpotlightConfig& config) {
                             static_cast<void>(canvas->setCanvasSpotlightConfig(config));
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::spotlightPreviewChanged, &owner,
                         [canvas](const SnowCanvasSpotlightConfig& config) {
                             canvas->previewCanvasSpotlightConfig(config);
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::textStylePopupInteractionBegan, &owner,
                         [canvas]() { canvas->beginTextStylePopupInteraction(); });
        QObject::connect(&palette, &ScreenshotToolPalette::textStylePopupInteractionEnded, &owner,
                         [canvas, this]() { canvas->endTextStylePopupInteraction(toolbarWindow); });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::drawingDeactivationRequested,
                         &owner, [this, &palette]() {
                             palette.clearActiveTool();
                             areaWindow->setInputMode(
                                 ScreenRecordingAreaWindow::InputMode::PassThrough);
                         });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::drawingWheelRequested, &owner,
                         [canvas, &palette](int direction) {
                             switch (canvas->canvasTool()) {
                             case SnowCanvasTool::Shape:
                             case SnowCanvasTool::Arrow:
                             case SnowCanvasTool::Line:
                             case SnowCanvasTool::FreeDraw:
                                 static_cast<void>(palette.stepStrokeWidth(direction));
                                 break;
                             case SnowCanvasTool::Spotlight:
                                 static_cast<void>(palette.stepSpotlightOpacity(direction));
                                 break;
                             case SnowCanvasTool::Watermark:
                                 static_cast<void>(palette.stepWatermarkFontSize(direction));
                                 break;
                             default:
                                 break;
                             }
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

    void start() {
        if (state != ScreenshotToolPalette::RecordingState::Idle || busy || startScheduled ||
            recordingSession != nullptr) {
            return;
        }
        startScheduled = true;
        operation = QUuid::createUuid().toString(QUuid::Id128);
        operationTimer.start();
        QTimer::singleShot(0, &owner, [this]() {
            startScheduled = false;
            if (state != ScreenshotToolPalette::RecordingState::Idle || busy ||
                recordingSession != nullptr || !isOpen()) {
                return;
            }
            busy = true;
            syncUi();
            QDir outputDirectory(recordingDirectory());
            if (!outputDirectory.mkpath(QStringLiteral("."))) {
                busy = false;
                syncUi();
                showError(tr("Unable to create the recording directory"));
                return;
            }

            const snow_shot::storage::RecordingSettings settings;
            sessionOutputSettings = directRecordingSettings(outputFormat, captureRegion.size());
            sessionMouseTrailColor = mouseTrailColor;
            sessionMouseClickColor = mouseClickColor;
            sessionShowCursor = showCursor;
            const bool audioSupported = outputFormat == QStringLiteral("mp4");
            pendingOutputPath = recordingFilePath(sessionOutputSettings.extension);
            const QByteArray outputUtf8 = QDir::toNativeSeparators(pendingOutputPath).toUtf8();
            const SnowCaptureDirectRecordingConfig config{
                SNOW_CAPTURE_DIRECT_RECORDING_CONFIG_VERSION,
                sizeof(SnowCaptureDirectRecordingConfig),
                captureRegion.x(),
                captureRegion.y(),
                static_cast<uint32_t>(captureRegion.width()),
                static_cast<uint32_t>(captureRegion.height()),
                static_cast<uint32_t>(SNOW_CAPTURE_BACKEND_WGC),
                outputUtf8.constData(),
                static_cast<uint32_t>(sessionOutputSettings.format),
                static_cast<uint32_t>(validRecordingFrameRate(settings.frameRate())),
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
            };
            const SnowCaptureResult createResult =
                snow_capture_recording_session_create_direct(&config, &recordingSession);
            if (recordingSession != nullptr && settings.hideToolbarInRecording()) {
                excludeToolbarFromCapture();
            }
            if (createResult != SNOW_CAPTURE_RESULT_OK || recordingSession == nullptr ||
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
                toolbarWindow->show();
                toolbarWindow->raise();
            }
            durationTimer.start();
            report(QStringLiteral("recording.started"));
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
        report(QStringLiteral("recording.paused"));
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
        report(QStringLiteral("recording.resumed"));
        durationTimer.start();
        syncUi();
    }

    void stop(bool copyToClipboard, bool closeAfter) {
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
        SnowCaptureRecordingSession* session = recordingSession;
        finalizationFuture = std::async(std::launch::async, [session]() {
            const bool ok = snow_capture_recording_session_stop(session) == SNOW_CAPTURE_RESULT_OK;
            return std::make_pair(ok, ok ? QString() : captureError());
        });
        pendingCopyToClipboard = copyToClipboard;
        pendingCloseAfter = closeAfter;
        finalizationPollTimer.start();
    }

    void pollFinalization() {
        if (!finalizationFuture.valid() || finalizationFuture.wait_for(std::chrono::milliseconds(
                                               0)) != std::future_status::ready) {
            return;
        }
        finalizationPollTimer.stop();
        const std::pair<bool, QString> result = finalizationFuture.get();
        const bool ok = result.first;
        if (ok)
            report(QStringLiteral("recording.export_finished"));
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

    bool pollSessionLiveness() {
        if (recordingSession == nullptr || busy ||
            state == ScreenshotToolPalette::RecordingState::Idle) {
            return false;
        }
        SnowCaptureRecordingState nativeState = SNOW_CAPTURE_RECORDING_STATE_CREATED;
        if (snow_capture_recording_session_state(recordingSession, &nativeState) == 0 ||
            nativeState != SNOW_CAPTURE_RECORDING_STATE_STOPPED) {
            return false;
        }
        stop(false, false);
        return true;
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
        stop(false, true);
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
        captureExclusion.exclude(toolbarWindow);
    }

    void restoreToolbarCaptureVisibility() {
        captureExclusion.restore();
    }

    void syncUi() {
        if (areaWindow != nullptr) {
            areaWindow->setRecordingState(state);
            areaWindow->setDrawingBlocked(busy);
        }
        ScreenshotToolPalette* palette =
            toolbarWindow != nullptr ? toolbarWindow->palette() : nullptr;
        if (palette != nullptr) {
            palette->setRecordingState(state);
            palette->setRecordingDuration(durationMilliseconds);
            palette->setRecordingMicrophoneEnabled(microphoneEnabled);
            palette->setRecordingSystemAudioEnabled(systemAudioEnabled);
            palette->setRecordingOutputFormat(outputFormat);
            palette->setRecordingMouseTrailColor(mouseTrailColor);
            palette->setRecordingMouseClickColor(mouseClickColor);
            palette->setRecordingCursorVisible(showCursor);
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
        report(QStringLiteral("recording.failed"), QtWarningMsg);
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
    QTimer finalizationPollTimer;
    std::future<std::pair<bool, QString>> finalizationFuture;
    ScreenshotToolPalette::RecordingState state = ScreenshotToolPalette::RecordingState::Idle;
    qint64 durationMilliseconds = 0;
    QString pendingOutputPath;
    bool microphoneEnabled = false;
    bool systemAudioEnabled = true;
    QString outputFormat = QStringLiteral("mp4");
    QColor mouseTrailColor{0, 0, 0, 0};
    QColor mouseClickColor{0, 0, 0, 0};
    bool showCursor = true;
    DirectRecordingSettings sessionOutputSettings;
    QColor sessionMouseTrailColor{0, 0, 0, 0};
    QColor sessionMouseClickColor{0, 0, 0, 0};
    bool sessionShowCursor = true;
    bool busy = false;
    std::unique_ptr<ScreenRecordingShortcutController> shortcutController;
    void report(const QString& event, QtMsgType level = QtInfoMsg) const {
        snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.recording"), event,
                                         {{QStringLiteral("operation"), operation},
                                          {QStringLiteral("duration_ms"),
                                           operationTimer.isValid() ? operationTimer.elapsed() : 0},
                                          {QStringLiteral("backend"), QStringLiteral("wgc")}},
                                         level);
    }
    QString operation;
    QElapsedTimer operationTimer;
    bool startScheduled = false;
    bool pendingCopyToClipboard = false;
    bool pendingCloseAfter = false;
    snow_shot::presentation::WindowCaptureExclusion captureExclusion{
#if defined(Q_OS_WIN) || defined(_WIN32)
        snow_shot::platform::windows::setWindowExcludedFromCapture
#endif
    };
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

void ScreenRecordingController::stopRecordingAndCopy() {
    m_impl->stop(true, false);
}
