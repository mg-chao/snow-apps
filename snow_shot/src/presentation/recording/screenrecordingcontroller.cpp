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
#include "screenrecordingselection.h"
#include "../capture/windowcaptureexclusion.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

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

struct RecordingKeyboardLabels {
    QVector<QByteArray> text;
    QVector<SnowCaptureKeyboardLabel> entries;

    explicit RecordingKeyboardLabels(bool enabled) {
        if (!enabled) {
            return;
        }
        const auto add = [this](uint32_t key, const QString& label) {
            text.push_back(label.toUtf8());
            entries.push_back({key, reinterpret_cast<const uint8_t*>(text.back().constData()),
                               static_cast<uint32_t>(text.back().size())});
        };
        const auto keyText = [](const char* source) {
            return QCoreApplication::translate("RecordingKeyboard", source);
        };
        const std::pair<uint32_t, const char*> names[] = {
            {0x08, QT_TRANSLATE_NOOP("RecordingKeyboard", "Backspace")},
            {0x09, QT_TRANSLATE_NOOP("RecordingKeyboard", "Tab")},
            {0x0C, QT_TRANSLATE_NOOP("RecordingKeyboard", "Clear")},
            {0x0D, QT_TRANSLATE_NOOP("RecordingKeyboard", "Enter")},
            {0x10, QT_TRANSLATE_NOOP("RecordingKeyboard", "Shift")},
            {0x11, QT_TRANSLATE_NOOP("RecordingKeyboard", "Ctrl")},
            {0x12, QT_TRANSLATE_NOOP("RecordingKeyboard", "Alt")},
            {0x13, QT_TRANSLATE_NOOP("RecordingKeyboard", "Pause")},
            {0x14, QT_TRANSLATE_NOOP("RecordingKeyboard", "Caps Lock")},
            {0x1B, QT_TRANSLATE_NOOP("RecordingKeyboard", "Esc")},
            {0x20, QT_TRANSLATE_NOOP("RecordingKeyboard", "Space")},
            {0x21, QT_TRANSLATE_NOOP("RecordingKeyboard", "Page Up")},
            {0x22, QT_TRANSLATE_NOOP("RecordingKeyboard", "Page Down")},
            {0x23, QT_TRANSLATE_NOOP("RecordingKeyboard", "End")},
            {0x24, QT_TRANSLATE_NOOP("RecordingKeyboard", "Home")},
            {0x25, QT_TRANSLATE_NOOP("RecordingKeyboard", "Left")},
            {0x26, QT_TRANSLATE_NOOP("RecordingKeyboard", "Up")},
            {0x27, QT_TRANSLATE_NOOP("RecordingKeyboard", "Right")},
            {0x28, QT_TRANSLATE_NOOP("RecordingKeyboard", "Down")},
            {0x2C, QT_TRANSLATE_NOOP("RecordingKeyboard", "Print Screen")},
            {0x2D, QT_TRANSLATE_NOOP("RecordingKeyboard", "Insert")},
            {0x2E, QT_TRANSLATE_NOOP("RecordingKeyboard", "Delete")},
            {0x5B, QT_TRANSLATE_NOOP("RecordingKeyboard", "Win")},
            {0x5D, QT_TRANSLATE_NOOP("RecordingKeyboard", "Menu")},
            {0x6A, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num *")},
            {0x6B, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num +")},
            {0x6C, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num Separator")},
            {0x6D, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num -")},
            {0x6E, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num .")},
            {0x6F, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num /")},
            {0x90, QT_TRANSLATE_NOOP("RecordingKeyboard", "Num Lock")},
            {0x91, QT_TRANSLATE_NOOP("RecordingKeyboard", "Scroll Lock")},
            {0xA5, QT_TRANSLATE_NOOP("RecordingKeyboard", "AltGr")},
            {0xAD, QT_TRANSLATE_NOOP("RecordingKeyboard", "Mute")},
            {0xAE, QT_TRANSLATE_NOOP("RecordingKeyboard", "Volume Down")},
            {0xAF, QT_TRANSLATE_NOOP("RecordingKeyboard", "Volume Up")},
            {0xB0, QT_TRANSLATE_NOOP("RecordingKeyboard", "Next Track")},
            {0xB1, QT_TRANSLATE_NOOP("RecordingKeyboard", "Previous Track")},
            {0xB2, QT_TRANSLATE_NOOP("RecordingKeyboard", "Stop")},
            {0xB3, QT_TRANSLATE_NOOP("RecordingKeyboard", "Play/Pause")},
        };
        text.reserve(64);
        entries.reserve(64);
        for (const auto& [key, label] : names) {
            add(key, keyText(label));
        }
        for (uint32_t key = 0x60; key <= 0x69; ++key) {
            add(key, keyText(QT_TRANSLATE_NOOP("RecordingKeyboard", "Num %1")).arg(key - 0x60));
        }
    }
};

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
    const QString message = QString::fromUtf8(error != nullptr ? error : "");
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
    std::unique_ptr<ScreenRecordingAreaWindow> area = std::make_unique<ScreenRecordingAreaWindow>();
    std::unique_ptr<ScreenRecordingToolbarWindow> toolbar =
        std::make_unique<ScreenRecordingToolbarWindow>();
    std::unique_ptr<QObject> connections = std::make_unique<QObject>();
    std::unique_ptr<ScreenRecordingShortcutController> shortcuts;
};
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
        showKeyboard = settings.showKeyboard();
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
        destroyUi();
    }

    void open(const QRect& region) {
        if (region.width() < 2 || region.height() < 2 || busy ||
            state != ScreenshotToolPalette::RecordingState::Idle) {
            return;
        }
        cancelPendingStart();
        if (uiSession != nullptr) {
            physicalRegion = region;
            updateCaptureRegion();
            areaWindow->setPhysicalRegion(region);
            toolbarWindow->placeForPhysicalRegion(region);
            areaWindow->show();
            areaWindow->raise();
            toolbarWindow->showAndActivate();
            return;
        }

        physicalRegion = region;
        updateCaptureRegion();
        uiSession = new RecordingUiSession(&owner);
        areaWindow = uiSession->area.get();
        toolbarWindow = uiSession->toolbar.get();
        // Keep the toolbar above the area even when drawing or resizing activates the area.
        toolbarWindow->setTransientOwnerWindow(areaWindow);
        areaWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        toolbarWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        areaWindow->setPhysicalRegion(region);
        toolbarWindow->placeForPhysicalRegion(region);
        connectToolbar();
        uiSession->shortcuts =
            std::make_unique<ScreenRecordingShortcutController>(*areaWindow, *toolbarWindow);

        state = ScreenshotToolPalette::RecordingState::Idle;
        durationMilliseconds = 0;
        syncUi();

        areaWindow->show();
        areaWindow->raise();
        toolbarWindow->showAndActivate();
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
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::physicalRegionChanged,
                         uiSession->connections.get(), [this](const QRect& region) {
                             if (state != ScreenshotToolPalette::RecordingState::Idle || busy) {
                                 return;
                             }
                             physicalRegion = region;
                             updateCaptureRegion();
                             toolbarWindow->placeForPhysicalRegion(region);
                         });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::regionInteractionStarted,
                         uiSession->connections.get(),
                         [this]() { toolbarWindow->beginRegionInteraction(); });
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::regionInteractionFinished,
                         uiSession->connections.get(), [this]() {
                             if (areaWindow->isVisible()) {
                                 toolbarWindow->endRegionInteraction(areaWindow->physicalRegion());
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
                             microphoneEnabled = enabled;
                             snow_shot::storage::RecordingSettings().setMicrophoneEnabled(enabled);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingSystemAudioToggled,
                         uiSession->connections.get(), [this](bool enabled) {
                             systemAudioEnabled = enabled;
                             snow_shot::storage::RecordingSettings().setSystemAudioEnabled(enabled);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingOpenFolderRequested,
                         uiSession->connections.get(), [this]() { openFolder(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCloseRequested,
                         uiSession->connections.get(), [this]() { close(); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCopyRequested,
                         uiSession->connections.get(), [this]() { stop(true); });
        QObject::connect(palette, &ScreenshotToolPalette::recordingOutputFormatChanged,
                         uiSession->connections.get(), [this](const QString& format) {
                             outputFormat = format;
                             snow_shot::storage::RecordingSettings().setOutputFormat(format);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseTrailColorChanged,
                         uiSession->connections.get(), [this](const QColor& color) {
                             mouseTrailColor = color;
                             snow_shot::storage::RecordingSettings().setMouseTrailColor(color);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingMouseClickColorChanged,
                         uiSession->connections.get(), [this](const QColor& color) {
                             mouseClickColor = color;
                             snow_shot::storage::RecordingSettings().setMouseClickColor(color);
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingKeyboardVisibleChanged,
                         uiSession->connections.get(), [this](bool visible) {
                             showKeyboard = visible;
                             snow_shot::storage::RecordingSettings().setShowKeyboard(visible);
                             syncUi();
                         });
        QObject::connect(palette, &ScreenshotToolPalette::recordingCursorVisibleChanged,
                         uiSession->connections.get(), [this](bool visible) {
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
        QObject::connect(
            &palette, &ScreenshotToolPalette::shapeStyleChanged, uiSession->connections.get(),
            [canvas, &palette](const SnowCanvasShapeStyle& style, quint32 properties,
                               SnowCanvasShapeKind kind) {
                canvas->setCanvasShapeStylePatch(style, properties, kind);
                static_cast<void>(snow_shot::presentation::persistScreenshotCanvasToolStyles(
                    palette.creationStyleDefaults()));
            });
        QObject::connect(
            &palette, &ScreenshotToolPalette::textStyleChanged, uiSession->connections.get(),
            [canvas, &palette](const SnowCanvasTextStyle& style) {
                static_cast<void>(canvas->setCanvasTextStyle(style));
                static_cast<void>(snow_shot::presentation::persistScreenshotCanvasToolStyles(
                    palette.creationStyleDefaults()));
            });
        QObject::connect(&palette, &ScreenshotToolPalette::serialNumberStyleChanged,
                         uiSession->connections.get(),
                         [canvas, &palette](const SnowCanvasSerialNumberStyle& style) {
                             static_cast<void>(canvas->setCanvasSerialNumberStyle(style));
                             static_cast<void>(
                                 snow_shot::presentation::persistScreenshotCanvasToolStyles(
                                     palette.creationStyleDefaults()));
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::watermarkConfigChanged,
                         uiSession->connections.get(),
                         [canvas](const SnowCanvasWatermarkConfig& config) {
                             static_cast<void>(canvas->setCanvasWatermarkConfig(config));
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::watermarkPreviewChanged,
                         uiSession->connections.get(),
                         [canvas](const SnowCanvasWatermarkConfig& config) {
                             canvas->previewCanvasWatermarkConfig(config);
                         });
        QObject::connect(&palette, &ScreenshotToolPalette::spotlightConfigChanged,
                         uiSession->connections.get(),
                         [canvas](const SnowCanvasSpotlightConfig& config) {
                             static_cast<void>(canvas->setCanvasSpotlightConfig(config));
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
        QObject::connect(areaWindow, &ScreenRecordingAreaWindow::drawingWheelRequested,
                         uiSession->connections.get(), [canvas, &palette](int direction) {
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

    void cancelPendingStart() {
        ++startGeneration;
        startScheduled = false;
    }

    void start() {
        if (!isOpen() || state != ScreenshotToolPalette::RecordingState::Idle || busy ||
            startScheduled || recordingSession != nullptr) {
            return;
        }
        startScheduled = true;
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
            const RecordingKeyboardLabels keyboardLabels(showKeyboard);
            const auto keyboardTheme = snow_shot::presentation::styles::generateThemeColorScheme();
            QColor keyboardBackground = keyboardTheme.map.colorBgElevated;
            keyboardBackground.setAlpha(204);
            QColor keyboardBorder = keyboardTheme.map.colorBorder;
            keyboardBorder.setAlpha(100);
            const SnowCaptureDirectRecordingConfig config{
                SNOW_CAPTURE_DIRECT_RECORDING_CONFIG_VERSION,
                sizeof(SnowCaptureDirectRecordingConfig),
                captureRegion.x(),
                captureRegion.y(),
                static_cast<uint32_t>(captureRegion.width()),
                static_cast<uint32_t>(captureRegion.height()),
                // Auto tries WGC first, then DXGI and GDI on eligible capture failures.
                static_cast<uint32_t>(SNOW_CAPTURE_BACKEND_AUTO),
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
                static_cast<uint32_t>(showKeyboard),
                packedRgba(keyboardBackground),
                packedRgba(keyboardTheme.map.colorText),
                packedRgba(keyboardBorder),
                keyboardLabels.entries.constData(),
                static_cast<uint32_t>(keyboardLabels.entries.size()),
                0,
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
                toolbarWindow->showAndActivate();
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

    void stop(bool copyToClipboard) {
        if (busy) {
            return;
        }
        if (state == ScreenshotToolPalette::RecordingState::Idle || recordingSession == nullptr) {
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
        stop(false);
        return true;
    }

    void openFolder() {
        QDir directory(recordingDirectory());
        directory.mkpath(QStringLiteral("."));
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory.absolutePath()));
    }

    void close() {
        stop(false);
        destroyUi();
    }

    void destroyUi() {
        cancelPendingStart();
        if (uiSession == nullptr) {
            return;
        }
        auto* retiring = uiSession;
        uiSession = nullptr;
        areaWindow = nullptr;
        toolbarWindow = nullptr;
        // Disconnect before hiding: hide/focus events can emit canvas and geometry signals.
        retiring->connections.reset();
        retiring->shortcuts.reset();
        retiring->toolbar->hide();
        retiring->area->hide();
        restoreToolbarCaptureVisibility();
        // Close can originate in a toolbar button's event handler.
        retiring->deleteLater();
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
            palette->setRecordingKeyboardVisible(showKeyboard);
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
    RecordingUiSession* uiSession = nullptr;
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
    bool showKeyboard = false;
    DirectRecordingSettings sessionOutputSettings;
    QColor sessionMouseTrailColor{0, 0, 0, 0};
    QColor sessionMouseClickColor{0, 0, 0, 0};
    bool sessionShowCursor = true;
    bool busy = false;
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
    quint64 startGeneration = 0;
    bool pendingCopyToClipboard = false;
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
    m_impl->stop(true);
}
