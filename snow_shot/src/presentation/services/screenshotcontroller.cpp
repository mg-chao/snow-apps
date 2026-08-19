#include "snow_shot/presentation/screenshotcontroller.h"
#include "snow_shot/network/snowshotapiclient.h"

#include "snow_shot/platform/physicalcursor.h"
#include "snow_shot/presentation/screenshotcaptureruntimeadapter.h"
#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotcaptureworkflow.h"
#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"
#include "snow_shot/presentation/screenshotdisplayconfigurationobserver.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotexportservice.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshothistoryservice.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotmessageservice.h"
#include "snow_shot/presentation/screenshotocrcontroller.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/presentation/screenshotselectioneditworkflow.h"
#include "snow_shot/presentation/screenshotselectionexportworkflow.h"
#include "snow_shot/presentation/screenshotselectionexportuiservices.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionresizeworkflow.h"
#include "snow_shot/presentation/screenshotselectionsettingsstore.h"
#include "snow_shot/presentation/screenshotscrollingcapturecontroller.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlayinteractionadapter.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotoverlayshortcutcontroller.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotpresentationservices.h"
#include "snow_shot/presentation/screenshotselectorcoordinator.h"
#include "snow_shot/presentation/screenshotselectorworkflow.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarpresenter.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshottoolcommandworkflow.h"
#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_capture.h"
#include "widgets/color_picker.h"
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QPointer>
#include <QRectF>
#include <QScreen>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTimer>
#include <QPainter>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {
constexpr auto kCopyMessageKey = "screenshot-copy";
constexpr auto kSaveMessageKey = "screenshot-save";
constexpr auto kPinClipboardMessageKey = "screenshot-pin-clipboard";
#if defined(Q_OS_WIN) || defined(_WIN32)
QString cameraShutterAudioPath() {
    const QString installedPath = QDir(QCoreApplication::applicationDirPath())
                                      .filePath(QStringLiteral("audios/camera_shutter.mp3"));
    if (QFileInfo(installedPath).isFile()) {
        return installedPath;
    }
    return QStringLiteral(SNOW_SHOT_CAMERA_SHUTTER_AUDIO_SOURCE);
}

QString mediaControlError(MCIERROR error) {
    wchar_t message[256]{};
    if (mciGetErrorStringW(error, message, 256) != FALSE) {
        return QString::fromWCharArray(message);
    }
    return QString::number(error);
}

MCIERROR sendMediaControlCommand(const QString& command) {
    const std::wstring nativeCommand = command.toStdWString();
    return mciSendStringW(nativeCommand.c_str(), nullptr, 0, nullptr);
}

void playCameraShutterSound() {
    const QString audioPath = cameraShutterAudioPath();
    if (!QFileInfo(audioPath).isFile()) {
        qWarning("Camera shutter audio is unavailable: %s", qPrintable(audioPath));
        return;
    }

    static quint64 playbackId = 0;
    const QString alias = QStringLiteral("snow_shot_camera_shutter_%1").arg(++playbackId);
    const MCIERROR openError =
        sendMediaControlCommand(QStringLiteral("open \"%1\" type mpegvideo alias %2")
                                    .arg(QDir::toNativeSeparators(audioPath), alias));
    if (openError != 0) {
        qWarning("Failed to open camera shutter audio: %s",
                 qPrintable(mediaControlError(openError)));
        return;
    }

    const MCIERROR playError = sendMediaControlCommand(QStringLiteral("play %1 from 0").arg(alias));
    if (playError != 0) {
        qWarning("Failed to play camera shutter audio: %s",
                 qPrintable(mediaControlError(playError)));
        static_cast<void>(sendMediaControlCommand(QStringLiteral("close %1").arg(alias)));
        return;
    }
    QTimer::singleShot(5000, QCoreApplication::instance(), [alias]() {
        static_cast<void>(sendMediaControlCommand(QStringLiteral("close %1").arg(alias)));
    });
}
#else
void playCameraShutterSound() {}
#endif

ScreenshotToolPalette::Tool paletteToolForActiveTool(ScreenshotActiveTool tool) {
    switch (tool) {
    case ScreenshotActiveTool::Select:
        return ScreenshotToolPalette::Tool::Select;
    case ScreenshotActiveTool::Shape:
        return ScreenshotToolPalette::Tool::Shape;
    case ScreenshotActiveTool::Arrow:
        return ScreenshotToolPalette::Tool::Arrow;
    case ScreenshotActiveTool::Line:
        return ScreenshotToolPalette::Tool::Line;
    case ScreenshotActiveTool::FreeDraw:
        return ScreenshotToolPalette::Tool::FreeDraw;
    case ScreenshotActiveTool::RectangleHighlight:
        return ScreenshotToolPalette::Tool::RectangleHighlight;
    case ScreenshotActiveTool::PenHighlight:
        return ScreenshotToolPalette::Tool::PenHighlight;
    case ScreenshotActiveTool::Eraser:
        return ScreenshotToolPalette::Tool::Eraser;
    case ScreenshotActiveTool::RectangleFilter:
        return ScreenshotToolPalette::Tool::RectangleFilter;
    case ScreenshotActiveTool::Watermark:
        return ScreenshotToolPalette::Tool::Watermark;
    case ScreenshotActiveTool::Text:
        return ScreenshotToolPalette::Tool::Text;
    case ScreenshotActiveTool::SerialNumber:
        return ScreenshotToolPalette::Tool::SerialNumber;
    case ScreenshotActiveTool::Ocr:
        return ScreenshotToolPalette::Tool::Ocr;
    case ScreenshotActiveTool::Table:
        return ScreenshotToolPalette::Tool::Table;
    case ScreenshotActiveTool::Qr:
        return ScreenshotToolPalette::Tool::Qr;
    case ScreenshotActiveTool::PenFilter:
        return ScreenshotToolPalette::Tool::PenFilter;
    case ScreenshotActiveTool::Spotlight:
        return ScreenshotToolPalette::Tool::Spotlight;
    case ScreenshotActiveTool::Move:
    default:
        return ScreenshotToolPalette::Tool::Move;
    }
}
} // namespace

struct ScreenshotController::Impl final : public QObject,
                                          public ScreenshotToolbarCommandSink,
                                          public ScreenshotSelectionToolbarCommandSink {
    using CapturedDisplay = CapturedDisplayModel;

    enum class PendingSelectionAction {
        None,
        Pin,
        RecognizeText,
        RecognizeTextTranslation,
        Copy,
        StartVideo,
    };

    enum class AutomaticSelectionMode {
        None,
        CurrentMonitor,
        FocusedWindow,
    };

    explicit Impl(ScreenshotController& controller);
    ~Impl();

    void createPresentationInfrastructure();
    void createSelectionWorkflows();
    void createSelectorWorkflow();
    void createToolCommandWorkflow();
    void createCaptureRuntimeAdapter();
    void createCaptureWorkflow();
    void createHistoryService();
    void createDisplayConfigurationObserver();
    void createOverlayInputPipeline();
    void createToolbarCommands();
    void connectSelectorSignals();
    void reloadUiPreferences();
    void reloadDrawingPreferences();
    void updateSmartSelectionSettingForCurrentSession(bool enabled);
    void applyUiPreferences(const ScreenshotUiPreferences& preferences);
    void shutdown();
    void startHistoryEdit(const QString& recordId);
    void handleCapturePresented();
    void invalidateDelayedCapture();
    void resetPendingCaptureRequest();
    [[nodiscard]] bool
    beginCapture(PendingSelectionAction action = PendingSelectionAction::None,
                 snow_shot::storage::CaptureHistorySource historySource =
                     snow_shot::storage::CaptureHistorySource::CopiedToClipboard,
                 AutomaticSelectionMode automaticMode = AutomaticSelectionMode::None,
                 const QPoint& automaticPhysicalPoint = QPoint(), quintptr focusedWindowHandle = 0);
    void handleSelectionConfirmed();
    [[nodiscard]] bool selectPreviousSelection();
    void handleAutomaticTextRecognitionAction(bool available);
    void executeAutomaticSelection();
    [[nodiscard]] bool canBeginCapture() const;
    [[nodiscard]] bool canReleaseAfterCancel() const;
    [[nodiscard]] ScreenshotOverlayWindow* overlayUnderCursor() const;
    void setHistoryLoadingMessageVisible(bool visible);
    [[nodiscard]] bool stopScrollingCapture(bool restoreScreenshotPresentation);
    void pauseScrollingCaptureForSelectionResize();
    void resumeScrollingCaptureAfterSelectionResize();
    [[nodiscard]] bool activateToolForSelectionResize(ScreenshotActiveTool tool);
    void activateRecognitionToolAfterSelectionResize(ScreenshotActiveTool tool);
    [[nodiscard]] std::optional<quint64> beginImageExport();
    [[nodiscard]] bool imageExportCurrent(quint64 generation) const;
    [[nodiscard]] bool finishImageExport(quint64 generation);
    void hideImageExportPresentation();
    void completeScrollingResultExport(quint64 generation);
    void restoreToolUiAfterScrollingCapture(bool scrollingCaptureStopped);
    [[nodiscard]] bool resetCanvasEditingState();
    [[nodiscard]] bool prepareHistoryCandidate(std::optional<ScreenshotHistoryEntry>* candidate);
    [[nodiscard]] QPoint canvasColorPhysicalPositionAt(ScreenshotOverlayWindow* overlay,
                                                       const QPointF& localPosition) const;
    [[nodiscard]] QImage canvasColorPreviewAtPhysicalPoint(ScreenshotOverlayWindow* overlay,
                                                           const QPoint& physicalPosition);
    void updateCanvasColorSamplingPreview(ScreenshotOverlayWindow* overlay,
                                          const QPointF& localPosition);
    void updateCanvasColorSamplingPreviewAtPhysicalPoint(ScreenshotOverlayWindow* overlay,
                                                         const QPoint& physicalPosition);
    void setCanvasColorSamplingCursor(bool enabled);
    void setCanvasColorSamplingShortcutScope(bool enabled);
    void clearCanvasColorSampling();
    [[nodiscard]] bool moveCursorOnePixel(snow_shot::platform::PhysicalCursorDirection direction);

    void undoCanvasEdit() override;
    void redoCanvasEdit() override;
    void setMoveTool() override;
    void setSelectTool() override;
    void setShapeTool() override;
    void setArrowTool() override;
    void setLineTool() override;
    void setFreeDrawTool() override;
    void setHighlightTool() override;
    void setPenHighlightTool() override;
    void setSpotlightTool() override;
    void setEraserTool() override;
    void setFilterTool() override;
    void setRectangleFilterTool() override;
    void setPenFilterTool() override;
    void setWatermarkTool() override;
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig& config) override;
    void setSpotlightConfigFromToolbar(const SnowCanvasSpotlightConfig& config) override;
    void previewSpotlightFromToolbar(const SnowCanvasSpotlightConfig& config) override;
    void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig& config) override;
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle& style, quint32 properties) override;
    void setTextTool() override;
    void setSerialNumberTool() override;
    void setOcrTool() override;
    void setTextTranslationTool() override;
    void setTableTool() override;
    void setQrTool() override;
    void mergeTableSelection() override;
    void splitTableSelection() override;
    void resetTable() override;
    void toggleTextEditing() override;
    void toggleTextTranslation() override;
    void resetTextEditing() override;
    void openTextTranslationSettings() override;
    void applyTextFormatting(const QString& value) override;
    void applyTextPunctuation(const QString& value) override;
    void startScrollingScreenshot() override;
    void setScrollingScreenshotRecognitionMode(ScreenshotScrollingRecognitionMode mode) override;
    void pinSelectionToScreen() override;
    void pinClipboardContentToScreen();
    void saveSelectionToFile() override;
    void saveImageToFile(QImage image, const QString& outputPath, ScreenshotImageFileFormat format,
                         quint64 generation);
    void saveSnapshotToFile(ScreenshotScrollingSnapshot snapshot, const QString& outputPath,
                            ScreenshotImageFileFormat format, quint64 generation);
    void completeFileSave(ScreenshotExportTaskResult result, quint64 generation);
    void cancelCapture() override;
    void copySelectionToClipboard() override;
    void copySelectionToClipboardWithSource(snow_shot::storage::CaptureHistorySource historySource);
    void saveImageForCopy(QImage image, quint64 generation, bool copyFileToClipboard,
                          ScreenshotClipboardFormatMode clipboardFormat,
                          snow_shot::storage::CaptureHistorySource historySource,
                          std::shared_ptr<std::optional<ScreenshotHistoryEntry>> historyCandidate,
                          bool scrolling);
    void saveScrollingSnapshotForCopy(ScreenshotScrollingSnapshot snapshot, quint64 generation,
                                      bool copyFileToClipboard,
                                      snow_shot::storage::CaptureHistorySource historySource);
    void completeCopyExport(bool success, quint64 generation,
                            snow_shot::storage::CaptureHistorySource historySource,
                            std::shared_ptr<std::optional<ScreenshotHistoryEntry>> historyCandidate,
                            bool scrolling);
    void startScreenRecording() override;
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle& style, quint32 properties,
                                  SnowCanvasShapeKind kind) override;
    void setTextStyleFromToolbar(const SnowCanvasTextStyle& style) override;
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle& style) override;
    void decrementSelectedSerialNumbers() override;
    void incrementSelectedSerialNumbers() override;
    void createTextForSelectedSerialNumber() override;
    void reorderSelectedElements(SnowCanvasSelectionOrder order) override;
    void setSelectedElementsOpacity(qreal opacity) override;
    void duplicateSelectedElements() override;
    void deleteSelectedElements() override;
    void repositionToolbarForContentChange() override;
    void toggleSelectionAspectRatioLockFromToolbar() override;
    void openSelectionResizeModalFromToolbar() override;
    void hideColorPickersForScreenshotUi() override;
    void beginCanvasColorSampling(adqt::widgets::AdColorPicker* picker) override;
    void adjustSelectionFromToolbar(int minDx, int minDy, int maxDx, int maxDy) override;
    void setSelectionCornerRadiusFromToolbar(int radius) override;
    void setSelectionShadowWidthFromToolbar(int shadowWidth) override;
    void setSelectionToolbarHovered(bool hovered) override;

    ScreenshotController& owner;
    ScreenshotSelectorCoordinator* m_selectorCoordinator = nullptr;
    ScreenshotCaptureState m_captureState;
    std::unique_ptr<snow_shot::presentation::WindowShortcutManager> m_windowShortcutManager;
    std::unique_ptr<ScreenshotOverlayEventAdapter> m_overlayEventAdapter;
    std::unique_ptr<ScreenshotOverlayCoordinator> m_overlayCoordinator;
    std::unique_ptr<ScreenshotPresentationServices> m_presentationServices;
    std::unique_ptr<ScreenshotCaptureRuntimeAdapter> m_captureRuntime;
    std::unique_ptr<ScreenshotCaptureWorkflow> m_captureWorkflow;
    std::unique_ptr<ScreenshotDisplayConfigurationObserver> m_displayConfigurationObserver;
    std::unique_ptr<ScreenshotHistoryService> m_historyService;
    std::unique_ptr<ScreenshotSelectionSettingsStore> m_selectionSettings;
    std::unique_ptr<ScreenshotExportService> m_exportService;
    std::unique_ptr<ScreenshotSelectionExportUiServices> m_selectionExportUiServices;
    std::unique_ptr<ScreenshotSelectionExportWorkflow> m_selectionExportWorkflow;
    std::unique_ptr<ScreenshotSelectionEditWorkflow> m_selectionEditWorkflow;
    std::unique_ptr<snow_shot::platform::PhysicalCursor> m_physicalCursor;
    std::unique_ptr<ScreenshotColorPickerController> m_colorPickerController;
    std::unique_ptr<ScreenshotCanvasColorSamplerWindow> m_canvasColorSamplerWindow;
    ScreenshotCanvasColorSampler m_canvasColorSampler;
    std::unique_ptr<ScreenshotToolbarPresenter> m_toolbarPresenter;
    std::unique_ptr<ScreenshotToolCommandWorkflow> m_toolCommandWorkflow;
    std::unique_ptr<ScreenshotOcrRecognitionService> m_ocrRecognition;
    std::unique_ptr<ScreenshotQrRecognitionService> m_qrRecognition;
    std::unique_ptr<ScreenshotMessageService> m_messages;
    std::unique_ptr<SnowShotApiClient> m_tableRecognition;
    std::unique_ptr<ScreenshotOcrController> m_ocrController;
    std::unique_ptr<ScreenshotSelectionResizeWorkflow> m_selectionResizeWorkflow;
    std::unique_ptr<ScreenshotScrollingCaptureController> m_scrollingCaptureController;
    std::unique_ptr<ScreenshotOverlayInputHandler> m_overlayInputHandler;
    std::unique_ptr<ScreenshotOverlayShortcutController> m_overlayShortcutController;
    std::unique_ptr<ScreenshotSelectorWorkflow> m_selectorWorkflow;
    QPointer<ScreenshotOverlayWindow> m_historyLoadingMessageOwner;
    QPointer<adqt::widgets::AdColorPicker> m_canvasColorSamplingTarget;
    QMetaObject::Connection m_canvasColorSamplingDestroyedConnection;
    bool m_canvasColorSamplingCursorOverridden = false;
    QString m_pendingHistoryEditRecordId;
    quint64 m_imageExportGeneration = 0;
    bool m_imageExportInFlight = false;
    ScreenshotExportJobHandle m_exportJob;
    ScreenshotExportJobHandle m_backgroundSaveJob;
    ScreenshotClipboardCommitHandle m_clipboardCommit;
    ScreenshotExportJobHandle m_clipboardPinJob;
    quint64 m_clipboardPinGeneration = 0;
    quint64 m_delayedCaptureGeneration = 0;
    PendingSelectionAction m_pendingSelectionAction = PendingSelectionAction::None;
    snow_shot::storage::CaptureHistorySource m_pendingHistorySource =
        snow_shot::storage::CaptureHistorySource::CopiedToClipboard;
    AutomaticSelectionMode m_automaticSelectionMode = AutomaticSelectionMode::None;
    QPoint m_automaticPhysicalPoint;
    bool m_pendingOcrFromQuickFunction = false;
    bool m_ocrFromQuickFunction = false;
    bool m_ocrTranslateAfterRecognition = false;
    bool m_activatingQuickOcr = false;
    quint64 m_ocrActivationId = 0;
    quint64 m_ocrAutoActionHandledActivationId = 0;
    std::optional<ScreenshotWindowCaptureFrame> m_focusedWindowCapture;
    SnowCanvasRuntime m_canvasRuntime;
    ScreenshotGeometryMapper m_geometry;
    ScreenshotDisplaySession m_displaySession;
    ScreenshotInteractionState m_interaction;
    ScreenshotSelectionModel m_selection;
    ScreenshotIntelligentSelectionModel m_intelligentSelection;
    QSet<SnowCanvasTool> m_quickSelectionDisabledTools;
    ScreenshotUiPreferences m_uiPreferences;
    std::unique_ptr<ScreenRecordingController> m_screenRecordingController;
};

ScreenshotController::Impl::Impl(ScreenshotController& controller)
    : owner(controller), m_canvasRuntime(SnowCanvasRuntimeConfig{
                             snow_shot::presentation::screenshotCanvasStyleDefaults()}) {
    createPresentationInfrastructure();
    reloadUiPreferences();
    reloadDrawingPreferences();
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized()) {
        QObject::connect(
            &storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue& value) {
                if (key == QStringLiteral("screenshot_selection/smart_selection")) {
                    updateSmartSelectionSettingForCurrentSession(value.toBool());
                } else if (key.startsWith(QStringLiteral("screenshot_ui/"))) {
                    reloadUiPreferences();
                } else if (key == QStringLiteral("drawing/quick_selection_disabled_tools")) {
                    reloadDrawingPreferences();
                } else if (key.startsWith(QStringLiteral("screenshot_shortcuts/")) &&
                           m_presentationServices != nullptr) {
                    m_presentationServices->reloadConfiguredShortcuts();
                    if (!m_interaction.inactive()) {
                        m_presentationServices->updateOverlayState();
                    }
                }
            });
    }
    createSelectionWorkflows();
    createSelectorWorkflow();
    createToolCommandWorkflow();
    createCaptureRuntimeAdapter();
    createCaptureWorkflow();
    createHistoryService();
    createDisplayConfigurationObserver();
    createOverlayInputPipeline();
    createToolbarCommands();
    connectSelectorSignals();
}

void ScreenshotController::Impl::reloadDrawingPreferences() {
    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    if (!applicationStorage.isInitialized()) {
        return;
    }
    const auto tools = snow_shot::presentation::screenshotQuickSelectionDisabledTools(
        snow_shot::storage::DrawingSettings().quickSelectionDisabledTools());
    m_quickSelectionDisabledTools = tools;
    if (!m_canvasRuntime.setQuickSelectionDisabledTools(tools)) {
        qWarning("Failed to apply screenshot drawing quick-selection preferences");
    }
    if (m_presentationServices != nullptr) {
        m_presentationServices->setQuickSelectionDisabledTools(tools);
    }
}

void ScreenshotController::Impl::updateSmartSelectionSettingForCurrentSession(bool enabled) {
    if (m_interaction.inactive() || !m_intelligentSelection.updateSmartSelectionEnabled(enabled)) {
        return;
    }

    if (m_interaction.intelligentSelecting()) {
        if (m_intelligentSelection.hasCurrentSelection()) {
            m_selection.setSelectionRect(m_intelligentSelection.currentSelection());
        } else {
            m_selection.clearSelection();
        }
        if (m_selectorWorkflow != nullptr) {
            static_cast<void>(m_selectorWorkflow->updateSelectionAt(
                m_geometry.physicalPositionForLogicalPoint(m_displaySession, QCursor::pos())));
        }
    }
    if (m_presentationServices != nullptr) {
        m_presentationServices->updateOverlayState();
    }
}

void ScreenshotController::Impl::reloadUiPreferences() {
    ScreenshotUiPreferences preferences;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized()) {
        const snow_shot::storage::ScreenshotUiSettings settings;
        preferences.selectionTransitionAnimationEnabled =
            settings.selectionTransitionAnimationEnabled();
        preferences.colorPickerDisplayMode =
            screenshotColorPickerDisplayModeFromString(settings.colorPickerDisplayMode());
        preferences.selectionMaskColor = settings.selectionMaskColor();
        preferences.shortcutHintOpacity =
            static_cast<qreal>(settings.shortcutHintOpacity()) / 100.0;
        preferences.cursorGuideLineColor = settings.cursorGuideLineColor();
        preferences.monitorCenterGuideLineColor = settings.monitorCenterGuideLineColor();
        preferences.colorPickerCenterGuideLineColor = settings.colorPickerCenterGuideLineColor();
    }
    applyUiPreferences(preferences);
}

void ScreenshotController::Impl::applyUiPreferences(const ScreenshotUiPreferences& preferences) {
    m_uiPreferences = preferences.normalized();
    if (m_colorPickerController != nullptr) {
        m_colorPickerController->setDisplayMode(m_uiPreferences.colorPickerDisplayMode);
    }
    if (m_overlayCoordinator != nullptr) {
        m_overlayCoordinator->setColorPickerCenterGuideLineColor(
            m_uiPreferences.colorPickerCenterGuideLineColor);
        m_overlayCoordinator->clearGuideLines(m_displaySession);
        if (m_interaction.selecting()) {
            if (ScreenshotOverlayWindow* overlay = overlayUnderCursor()) {
                m_overlayCoordinator->updateGuideLines(m_displaySession, overlay,
                                                       overlay->mapFromGlobal(QCursor::pos()), true,
                                                       m_uiPreferences.cursorGuideLineColor,
                                                       m_uiPreferences.monitorCenterGuideLineColor);
            }
        }
    }
    if (m_presentationServices != nullptr) {
        m_presentationServices->setUiPreferences(m_uiPreferences);
        if (!m_interaction.inactive() && m_colorPickerController != nullptr) {
            m_colorPickerController->updateAtCurrentCursor(
                m_presentationServices->colorPickerContext());
        }
    }
}

void ScreenshotController::Impl::createHistoryService() {
    m_historyService = std::make_unique<ScreenshotHistoryService>(
        ScreenshotHistoryServiceContext{
            m_displaySession,
            m_canvasRuntime,
            m_selection,
            m_interaction,
            m_intelligentSelection,
            [this]() {
                if (m_ocrController != nullptr) {
                    m_ocrController->invalidateSession();
                }
                if (m_overlayCoordinator != nullptr) {
                    m_overlayCoordinator->applyDisplayModels(m_displaySession);
                }
                const bool smartSelecting = m_interaction.intelligentSelecting();
                if (smartSelecting && m_overlayCoordinator != nullptr) {
                    m_overlayCoordinator->setCanvasInteractionEnabled(m_displaySession, false);
                }
                if (!smartSelecting) {
                    setMoveTool();
                }
                if (m_overlayCoordinator != nullptr) {
                    if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
                        toolbar->setActiveTool(ScreenshotToolPalette::Tool::Move);
                    }
                }
                if (m_presentationServices != nullptr) {
                    if (smartSelecting) {
                        m_presentationServices->hideToolbar();
                    }
                    m_presentationServices->updateOverlayState();
                    if (smartSelecting) {
                        m_presentationServices->updateOverlayCursors();
                    } else {
                        m_presentationServices->showToolbar();
                    }
                }
                m_colorPickerController->updateAtCurrentCursor(
                    m_presentationServices->colorPickerContext());
            },
            [this](bool loading) { setHistoryLoadingMessageVisible(loading); },
            [this]() {
                if (m_selectorWorkflow == nullptr) {
                    return;
                }
                static_cast<void>(m_selectorWorkflow->updateSelectionAt(
                    m_geometry.physicalPositionForLogicalPoint(m_displaySession, QCursor::pos())));
            },
        },
        snow_shot::storage::ApplicationStorage::instance().captureHistory());
}

ScreenshotOverlayWindow* ScreenshotController::Impl::overlayUnderCursor() const {
    const QPoint cursorPosition = QCursor::pos();
    ScreenshotOverlayWindow* result = nullptr;
    m_displaySession.forEachActiveOverlay(
        [&result, &cursorPosition](qsizetype, const CapturedDisplayModel& display,
                                   ScreenshotOverlayWindow* overlay) {
            if (result == nullptr && overlay != nullptr && overlay->isVisible() &&
                display.logicalRect.contains(cursorPosition, false)) {
                result = overlay;
            }
        });
    return result;
}

void ScreenshotController::Impl::setHistoryLoadingMessageVisible(bool visible) {
    if (!visible) {
        if (m_historyLoadingMessageOwner != nullptr) {
            m_historyLoadingMessageOwner->setHistoryLoadingVisible(false);
            m_historyLoadingMessageOwner = nullptr;
        }
        return;
    }

    ScreenshotOverlayWindow* messageOwner = overlayUnderCursor();
    if (messageOwner == nullptr) {
        if (m_historyLoadingMessageOwner != nullptr) {
            m_historyLoadingMessageOwner->setHistoryLoadingVisible(false);
            m_historyLoadingMessageOwner = nullptr;
        }
        return;
    }
    if (m_historyLoadingMessageOwner != nullptr && m_historyLoadingMessageOwner != messageOwner) {
        m_historyLoadingMessageOwner->setHistoryLoadingVisible(false);
    }
    messageOwner->setHistoryLoadingVisible(true);
    m_historyLoadingMessageOwner = messageOwner;
}

void ScreenshotController::Impl::createPresentationInfrastructure() {
    m_windowShortcutManager =
        std::make_unique<snow_shot::presentation::WindowShortcutManager>(&owner);
    m_overlayEventAdapter = std::make_unique<ScreenshotOverlayEventAdapter>();
    m_overlayCoordinator = std::make_unique<ScreenshotOverlayCoordinator>(
        *m_overlayEventAdapter, m_canvasRuntime, *m_windowShortcutManager);
    m_messages = std::make_unique<ScreenshotMessageService>(
        m_displaySession, m_geometry, m_selection, [this]() {
            return m_overlayCoordinator != nullptr ? m_overlayCoordinator->toolbar() : nullptr;
        });
    m_physicalCursor = std::make_unique<snow_shot::platform::PhysicalCursor>();
    m_colorPickerController = std::make_unique<ScreenshotColorPickerController>(
        *m_overlayCoordinator, m_geometry, m_displaySession, *m_physicalCursor);
    m_canvasColorSamplerWindow = std::make_unique<ScreenshotCanvasColorSamplerWindow>();
    m_toolbarPresenter = std::make_unique<ScreenshotToolbarPresenter>(*m_overlayCoordinator,
                                                                      m_geometry, m_displaySession);
    m_scrollingCaptureController = std::make_unique<ScreenshotScrollingCaptureController>(
        ScreenshotScrollingCaptureControllerContext{
            m_displaySession,
            m_geometry,
            *m_overlayCoordinator,
        },
        &owner);
    m_selectorCoordinator = new ScreenshotSelectorCoordinator(&owner);
    m_screenRecordingController = std::make_unique<ScreenRecordingController>(&owner);
    m_selectionSettings = std::make_unique<ScreenshotSelectionSettingsStore>();
    m_presentationServices =
        std::make_unique<ScreenshotPresentationServices>(ScreenshotPresentationServicesContext{
            m_captureState,
            *m_overlayCoordinator,
            *m_toolbarPresenter,
            m_geometry,
            m_displaySession,
            m_interaction,
            m_selection,
            m_intelligentSelection,
            m_quickSelectionDisabledTools,
        });
    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    const auto backendPreference =
        applicationStorage.configuration()
                .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                .toBool()
            ? ScreenshotOcrBackendPreference::DirectMl
            : ScreenshotOcrBackendPreference::Cpu;
    m_ocrRecognition = std::make_unique<ScreenshotOcrRecognitionService>(backendPreference, &owner);
    QObject::connect(
        &applicationStorage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged,
        this, [this](const QString& key, const QJsonValue& value) {
            if (key == QStringLiteral("text_recognition/direct_ml_acceleration") &&
                m_ocrRecognition != nullptr) {
                const auto preference = value.toBool() ? ScreenshotOcrBackendPreference::DirectMl
                                                       : ScreenshotOcrBackendPreference::Cpu;
                m_ocrRecognition->setBackendPreference(preference);
            } else if (key == QStringLiteral("network/proxy") && m_tableRecognition != nullptr) {
                m_tableRecognition->setUseSystemProxy(value.toString() == QStringLiteral("system"));
            }
        });
    m_qrRecognition = std::make_unique<ScreenshotQrRecognitionService>(&owner);
    QString tableApiUrl = QStringLiteral(SNOW_SHOT_API_BASE_URL);
    const QString runtimeTableApiUrl =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("SNOW_SHOT_API_BASE_URL"));
    if (!runtimeTableApiUrl.trimmed().isEmpty()) {
        tableApiUrl = runtimeTableApiUrl;
    }
    m_tableRecognition = std::make_unique<SnowShotApiClient>(tableApiUrl, &owner);
    m_tableRecognition->setUseSystemProxy(
        applicationStorage.configuration().value(QStringLiteral("network/proxy")).toString() ==
        QStringLiteral("system"));
    m_ocrController = std::make_unique<ScreenshotOcrController>(
        ScreenshotOcrControllerContext{
            m_captureState,
            m_interaction,
            m_selection,
            m_displaySession,
            m_geometry,
            *m_overlayCoordinator,
            *m_ocrRecognition,
            *m_qrRecognition,
            m_tableRecognition.get(),
            [this]() { m_colorPickerController->hide(); },
            [this]() { cancelCapture(); },
            [this](const QPointF& canvasPosition) {
                return m_overlayInputHandler != nullptr
                           ? m_overlayInputHandler->selectionResizeDragModeAtCanvasPosition(
                                 canvasPosition)
                           : ScreenshotSelectionDragMode::None;
            },
            [this](const QPointF& canvasPosition) {
                return m_overlayInputHandler != nullptr &&
                       m_overlayInputHandler->beginSelectionResizeAtCanvasPosition(canvasPosition);
            },
            [this](const QPointF& canvasPosition) {
                if (m_overlayInputHandler != nullptr) {
                    m_overlayInputHandler->updateSelectionResizeAtCanvasPosition(canvasPosition);
                }
            },
            [this](const QPointF& canvasPosition) {
                if (m_overlayInputHandler != nullptr) {
                    m_overlayInputHandler->finishSelectionResizeAtCanvasPosition(canvasPosition);
                }
            },
        },
        &owner);
    QObject::connect(m_ocrController.get(), &ScreenshotOcrController::textResultChanged, this,
                     [this](bool available) { handleAutomaticTextRecognitionAction(available); });
}

void ScreenshotController::Impl::createSelectionWorkflows() {
    m_exportService = std::make_unique<ScreenshotExportService>(ScreenshotExportServiceContext{
        m_displaySession,
        m_canvasRuntime,
        m_geometry,
    });
    m_selectionExportUiServices = std::make_unique<ScreenshotSelectionExportUiServices>(
        m_canvasRuntime, m_ocrRecognition.get(), m_qrRecognition.get(), m_tableRecognition.get(),
        [controller = QPointer<ScreenshotController>(&owner)]() {
            if (controller != nullptr) {
                emit controller->showMainWindowRequested();
            }
        });
    m_selectionExportWorkflow = std::make_unique<ScreenshotSelectionExportWorkflow>(
        ScreenshotSelectionExportWorkflowContext{
            m_captureState,
            m_geometry,
            m_selection,
            *m_exportService,
            *m_selectionExportUiServices,
            *m_selectionSettings,
            *this,
        });
    m_selectionResizeWorkflow =
        std::make_unique<ScreenshotSelectionResizeWorkflow>(*m_selectionSettings);
    m_selectionEditWorkflow =
        std::make_unique<ScreenshotSelectionEditWorkflow>(ScreenshotSelectionEditWorkflowContext{
            owner,
            m_captureState,
            m_displaySession,
            m_geometry,
            m_interaction,
            m_selection,
            ScreenshotSelectionEditUiActions{
                [this]() { m_presentationServices->updateOverlayState(); },
                [this]() { m_presentationServices->showSelectionToolbar(); },
                [this]() { m_presentationServices->moveToolbar(); },
                [this]() { m_presentationServices->repositionToolbarForContentChange(); },
                [this]() { m_presentationServices->showToolbar(); },
                [this](QObject* modalParent, const ScreenshotSelectionResizeRequest& request,
                       ScreenshotApplySelectionCallback applySelection) {
                    return m_selectionResizeWorkflow->open(modalParent, request,
                                                           std::move(applySelection));
                },
                [this]() { m_colorPickerController->hide(); },
                [this](bool suppressed) { m_colorPickerController->setSuppressed(suppressed); },
            },
        });
}

void ScreenshotController::Impl::createSelectorWorkflow() {
    m_selectorWorkflow =
        std::make_unique<ScreenshotSelectorWorkflow>(ScreenshotSelectorWorkflowContext{
            m_captureState,
            *m_selectorCoordinator,
            *m_overlayCoordinator,
            m_displaySession,
            m_geometry,
            m_interaction,
            m_selection,
            m_intelligentSelection,
            ScreenshotSelectorPresentationCallbacks{
                [this]() { m_presentationServices->updateOverlayState(); },
                [this]() {
                    m_colorPickerController->updateAtCurrentCursor(
                        m_presentationServices->colorPickerContext());
                },
                [this]() { m_presentationServices->hideToolbar(); },
                [this]() { m_presentationServices->updateOverlayCursors(); },
                [this](quint64 sessionId) {
                    if (m_captureWorkflow != nullptr) {
                        m_captureWorkflow->handleInitialSmartSelectionResolved(sessionId);
                    }
                },
            },
        });
}

void ScreenshotController::Impl::createToolCommandWorkflow() {
    m_toolCommandWorkflow =
        std::make_unique<ScreenshotToolCommandWorkflow>(ScreenshotToolCommandWorkflowContext{
            m_captureState,
            ScreenshotToolCommandActions{
                [this]() { return m_selectorCoordinator->ready(); },
                [this]() { m_selectorWorkflow->startRefresh(); },
                [this](const QPoint& physicalPoint) {
                    static_cast<void>(m_selectorWorkflow->updateSelectionAt(physicalPoint));
                },
                [this]() { m_selectorWorkflow->clearSelection(); },
                [this](bool enabled) {
                    m_overlayCoordinator->setCanvasInteractionEnabled(m_displaySession, enabled);
                },
                [this](SnowCanvasTool tool) {
                    m_overlayCoordinator->setCanvasTool(m_displaySession, tool);
                },
                [this](SnowCanvasShapeStyle* outStyle) {
                    return m_overlayCoordinator->tryCurrentRectangleStyle(m_displaySession,
                                                                          outStyle);
                },
                [this](SnowCanvasStyleToolbarState* outState) {
                    return m_overlayCoordinator->tryCurrentStyleToolbarState(m_displaySession,
                                                                             outState);
                },
                [this](const SnowCanvasShapeStyle& style, quint32 properties,
                       SnowCanvasShapeKind kind) {
                    m_overlayCoordinator->setShapeStylePatch(m_displaySession, style, properties,
                                                             kind);
                },
                [this](const SnowCanvasFilterStyle& style, quint32 properties) {
                    m_overlayCoordinator->setFilterStyle(m_displaySession, style, properties);
                },
                [this](const SnowCanvasWatermarkConfig& config) {
                    m_overlayCoordinator->setWatermarkConfig(m_displaySession, config);
                },
                [this](const SnowCanvasSpotlightConfig& config) {
                    m_overlayCoordinator->setSpotlightConfig(m_displaySession, config);
                },
                [this](const SnowCanvasTextStyle& style) {
                    m_overlayCoordinator->setTextStyle(m_displaySession, style);
                },
                [this](const SnowCanvasSerialNumberStyle& style) {
                    m_overlayCoordinator->setSerialNumberStyle(m_displaySession, style);
                },
                [this](qint64 delta) {
                    m_overlayCoordinator->adjustSelectedSerialNumbers(m_displaySession, delta);
                },
                [this]() {
                    m_overlayCoordinator->createTextForSelectedSerialNumber(m_displaySession);
                },
                [this](int direction) {
                    return m_overlayCoordinator->stepToolbarStrokeWidth(direction);
                },
                [this]() { m_presentationServices->updateOverlayState(); },
                [this]() { m_presentationServices->updateOverlayCursors(); },
                [this]() { m_presentationServices->raiseToolbarForCanvasInteraction(); },
            },
            m_displaySession,
            m_geometry,
            m_interaction,
            m_selection,
            m_intelligentSelection,
        });
}

void ScreenshotController::Impl::createCaptureRuntimeAdapter() {
    m_captureRuntime =
        std::make_unique<ScreenshotCaptureRuntimeAdapter>(ScreenshotCaptureRuntimeAdapterContext{
            *m_selectorCoordinator,
            *m_selectorWorkflow,
            *m_overlayCoordinator,
            *m_colorPickerController,
            m_canvasRuntime,
        });
}

void ScreenshotController::Impl::createCaptureWorkflow() {
    m_captureWorkflow =
        std::make_unique<ScreenshotCaptureWorkflow>(ScreenshotCaptureWorkflowContext{
            m_captureState,
            *m_captureRuntime,
            m_geometry,
            m_displaySession,
            m_interaction,
            m_selection,
            m_intelligentSelection,
            ScreenshotCapturePresentationCallbacks{
                [this]() { m_presentationServices->hideToolbar(); },
                [this]() { m_presentationServices->updateOverlayState(); },
                [this]() {
                    m_colorPickerController->updateAtCurrentCursor(
                        m_presentationServices->colorPickerContext());
                },
                [this]() { handleCapturePresented(); },
            },
            [this]() {
                m_pendingHistoryEditRecordId.clear();
                resetPendingCaptureRequest();
                if (m_ocrController != nullptr) {
                    m_ocrController->invalidateSession();
                }
            },
            []() {
                return snow_shot::storage::ApplicationStorage::instance().smartSelectionEnabled();
            },
            [this](std::optional<ScreenshotWindowCaptureFrame> frame) {
                m_focusedWindowCapture = std::move(frame);
            },
        });
}

void ScreenshotController::Impl::startHistoryEdit(const QString& recordId) {
    if (recordId.isEmpty()) {
        return;
    }
    // An explicit history edit supersedes a delayed shortcut that has not fired yet.
    invalidateDelayedCapture();
    const bool idleSession = m_captureState.sessionState == ScreenshotSessionState::IdleCold ||
                             m_captureState.sessionState == ScreenshotSessionState::IdlePrepared;
    if (!idleSession || m_captureState.captureInProgress || !m_interaction.inactive() ||
        m_captureWorkflow == nullptr || m_historyService == nullptr) {
        return;
    }

    resetPendingCaptureRequest();
    m_pendingHistoryEditRecordId = recordId;
    m_ocrController->invalidateSession();
    m_historyService->resetCaptureNavigation();
    m_captureWorkflow->startCapture();
}

void ScreenshotController::Impl::handleCapturePresented() {
    if (!m_pendingHistoryEditRecordId.isEmpty()) {
        const QString recordId = std::exchange(m_pendingHistoryEditRecordId, QString());
        if (m_historyService != nullptr) {
            static_cast<void>(m_historyService->navigateToRecord(recordId));
        }
        return;
    }
    executeAutomaticSelection();
}

void ScreenshotController::Impl::createDisplayConfigurationObserver() {
    m_displayConfigurationObserver = std::make_unique<ScreenshotDisplayConfigurationObserver>(
        [this]() {
            static_cast<void>(stopScrollingCapture(false));
            if (m_captureWorkflow != nullptr) {
                m_captureWorkflow->handleDisplayConfigurationChanged();
            }
        },
        &owner);
    m_displayConfigurationObserver->connectApplicationSignals(qApp);
    m_displayConfigurationObserver->observeCurrentScreens();
}

void ScreenshotController::Impl::createOverlayInputPipeline() {
    ScreenshotOverlayInputActions actions{
        [this](const QPoint& physicalPoint) {
            return m_selectorWorkflow->returnToSelection(physicalPoint);
        },
        [this](const QPoint& physicalPoint) {
            static_cast<void>(m_selectorWorkflow->requestHitTest(physicalPoint));
        },
        [this]() { m_selectorCoordinator->resetHitTestState(); },
        [this](ScreenshotOverlayWindow* overlay, ScreenshotSelectionDragMode dragMode) {
            m_overlayCoordinator->setOverlayCursor(overlay, dragMode);
        },
        [this]() { m_presentationServices->hideMainToolbar(); },
        [this]() { m_presentationServices->updateOverlayState(); },
        [this]() { m_presentationServices->showToolbar(); },
        [this]() { m_presentationServices->showSelectionToolbar(); },
        [this]() { cancelCapture(); },
        [this](int delta) { return m_toolCommandWorkflow->stepStrokeWidth(delta); },
        [this](int delta) { return m_overlayCoordinator->stepToolbarSelectionOpacity(delta); },
        [this](int delta) { return m_overlayCoordinator->stepToolbarSpotlightOpacity(delta); },
        [this](int delta) { return m_overlayCoordinator->stepToolbarFilterIntensity(delta); },
        [this](int delta) { return m_overlayCoordinator->stepToolbarPenFilterStrokeWidth(delta); },
        [this](int delta) { return m_overlayCoordinator->stepToolbarWatermarkFontSize(delta); },
        [this]() { copySelectionToClipboard(); },
        [this](const QString& action) {
            if (action == QStringLiteral("copy")) {
                copySelectionToClipboard();
            } else if (action == QStringLiteral("save")) {
                saveSelectionToFile();
            } else if (action == QStringLiteral("pin")) {
                pinSelectionToScreen();
            }
        },
        [this]() {
            QWidget* focus = QApplication::focusWidget();
            if (qobject_cast<QLineEdit*>(focus) != nullptr ||
                qobject_cast<QTextEdit*>(focus) != nullptr ||
                qobject_cast<QPlainTextEdit*>(focus) != nullptr) {
                return false;
            }
            bool allowed = true;
            m_displaySession.forEachOverlay(
                [&allowed](qsizetype, ScreenshotOverlayWindow* overlay) {
                    if (overlay != nullptr && overlay->canvas() != nullptr &&
                        overlay->canvas()->hasActiveTextEditing()) {
                        allowed = false;
                    }
                });
            return allowed;
        },
        [this]() {
            setMoveTool();
            if (m_overlayCoordinator != nullptr) {
                if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
                    toolbar->setActiveTool(ScreenshotToolPalette::Tool::Move);
                }
            }
            return m_interaction.moveToolActive();
        },
        [this](const QString& toolId) {
            ScreenshotToolbarWindow* toolbar =
                m_overlayCoordinator != nullptr ? m_overlayCoordinator->toolbar() : nullptr;
            return toolbar != nullptr && toolbar->activateDrawingShortcut(toolId);
        },
        [this]() { return m_historyService != nullptr && m_historyService->navigatePrevious(); },
        [this]() { return m_historyService != nullptr && m_historyService->navigateNext(); },
        [this]() {
            return m_historyService != nullptr && m_historyService->returnToCurrentScreenshot();
        },
        [this](ScreenshotOverlayWindow* overlay, const QPointF& localPosition) {
            m_colorPickerController->updateForOverlay(overlay, localPosition,
                                                      m_presentationServices->colorPickerContext());
        },
        [this](ScreenshotOverlayWindow* overlay, const QPointF& localPosition) {
            m_overlayCoordinator->updateGuideLines(
                m_displaySession, overlay, localPosition, m_interaction.selecting(),
                m_uiPreferences.cursorGuideLineColor, m_uiPreferences.monitorCenterGuideLineColor);
        },
        [this](const QPointF& virtualPosition) {
            m_colorPickerController->updateForSelectionDrag(
                virtualPosition, m_presentationServices->colorPickerContext());
        },
        [this]() {
            return m_colorPickerController->copyColorToClipboard(
                m_presentationServices->colorPickerContext());
        },
        [this]() {
            return m_colorPickerController->cycleFormat(
                m_presentationServices->colorPickerContext());
        },
        [this](snow_shot::platform::PhysicalCursorDirection direction) {
            return moveCursorOnePixel(direction);
        },
        [this]() { handleSelectionConfirmed(); },
        [this]() { return selectPreviousSelection(); },
        [this](ScreenshotActiveTool tool) { return activateToolForSelectionResize(tool); },
        [this]() {
            m_canvasColorSamplingTarget.clear();
            disconnect(m_canvasColorSamplingDestroyedConnection);
            m_canvasColorSamplingDestroyedConnection = {};
            m_canvasColorSampler.reset();
            setCanvasColorSamplingShortcutScope(false);
            if (m_canvasColorSamplerWindow != nullptr) {
                m_canvasColorSamplerWindow->endSampling();
            }
            setCanvasColorSamplingCursor(false);
        },
        [this](ScreenshotOverlayWindow* overlay, const QPointF& localPosition) {
            QPointer<adqt::widgets::AdColorPicker> picker = m_canvasColorSamplingTarget;
            const QImage preview =
                picker.isNull()
                    ? QImage()
                    : canvasColorPreviewAtPhysicalPoint(
                          overlay, canvasColorPhysicalPositionAt(overlay, localPosition));
            m_canvasColorSamplingTarget.clear();
            disconnect(m_canvasColorSamplingDestroyedConnection);
            m_canvasColorSamplingDestroyedConnection = {};
            m_canvasColorSampler.reset();
            setCanvasColorSamplingShortcutScope(false);
            if (m_canvasColorSamplerWindow != nullptr) {
                m_canvasColorSamplerWindow->endSampling();
            }
            setCanvasColorSamplingCursor(false);
            if (picker.isNull()) {
                return false;
            }
            const QColor sampled =
                preview.isNull() ? QColor()
                                 : preview.pixelColor(preview.width() / 2, preview.height() / 2);
            if (!sampled.isValid()) {
                return false;
            }
            picker->commitValue(adqt::widgets::AdColorValue::solid(sampled));
            return true;
        },
        [this]() { pauseScrollingCaptureForSelectionResize(); },
        [this]() { resumeScrollingCaptureAfterSelectionResize(); },
        [this]() {
            const ScreenshotToolbarWindow* toolbar =
                m_overlayCoordinator != nullptr ? m_overlayCoordinator->toolbar() : nullptr;
            return toolbar != nullptr && toolbar->isVisible();
        },
        [this](ScreenshotOverlayWindow* overlay, const QPointF& localPosition) {
            updateCanvasColorSamplingPreview(overlay, localPosition);
        },
        [this]() {
            setOcrTool();
            if (m_overlayCoordinator != nullptr) {
                if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
                    toolbar->setActiveTool(ScreenshotToolPalette::Tool::Ocr);
                }
            }
            return true;
        },
        [this]() {
            setTableTool();
            if (m_overlayCoordinator != nullptr) {
                if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
                    toolbar->setActiveTool(ScreenshotToolPalette::Tool::Table);
                }
            }
            return true;
        },
        [this]() {
            setQrTool();
            if (m_overlayCoordinator != nullptr) {
                if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
                    toolbar->setActiveTool(ScreenshotToolPalette::Tool::Qr);
                }
            }
            return true;
        },
        [this]() {
            startScreenRecording();
            return true;
        },
        [this]() {
            startScrollingScreenshot();
            return true;
        },
        [this]() {
            saveSelectionToFile();
            return true;
        },
        [this]() {
            setTextTranslationTool();
            return true;
        },
        [this]() {
            pinSelectionToScreen();
            return true;
        },
        [this]() {
            undoCanvasEdit();
            return true;
        },
        [this]() {
            redoCanvasEdit();
            return true;
        },
        [this]() { return m_physicalCursor != nullptr && m_physicalCursor->isSupported(); },
    };
    m_overlayInputHandler =
        std::make_unique<ScreenshotOverlayInputHandler>(ScreenshotOverlayInputHandlerContext{
            m_captureState,
            m_interaction,
            m_selection,
            m_intelligentSelection,
            m_geometry,
            m_displaySession,
            actions,
        });
    m_overlayShortcutController = std::make_unique<ScreenshotOverlayShortcutController>(
        *m_windowShortcutManager, *m_overlayInputHandler, m_interaction, m_intelligentSelection,
        std::move(actions), &owner);
    m_overlayEventAdapter->setEventTargets(*m_overlayInputHandler, [this]() {
        m_presentationServices->raiseToolbarForCanvasInteraction();
    });
}

bool ScreenshotController::Impl::moveCursorOnePixel(
    snow_shot::platform::PhysicalCursorDirection direction) {
    const bool canvasColorSampling =
        m_overlayInputHandler != nullptr && m_overlayInputHandler->canvasColorSamplingActive();
    if (m_physicalCursor == nullptr || m_colorPickerController == nullptr ||
        m_presentationServices == nullptr ||
        (!canvasColorSampling && !m_interaction.cursorMovementEnabled())) {
        return false;
    }

    const ScreenshotColorPickerContext context = m_presentationServices->colorPickerContext();
    if (!context.active) {
        return false;
    }

    const snow_shot::platform::PhysicalCursorMoveResult result =
        m_physicalCursor->moveOnePixel(direction);
    if (!result.commandApplied()) {
        return false;
    }
    if (!result.position.has_value()) {
        return true;
    }

    if (canvasColorSampling) {
        const CapturedDisplayModel* display =
            m_geometry.displayForPhysicalPoint(m_displaySession, result.position.value());
        ScreenshotOverlayWindow* overlay = m_displaySession.overlayForDisplay(display);
        if (display != nullptr && overlay != nullptr) {
            updateCanvasColorSamplingPreviewAtPhysicalPoint(overlay, result.position.value());
        }
        return true;
    }
    m_colorPickerController->updateAfterCursorMove(result.position.value(), context);
    return true;
}

void ScreenshotController::Impl::createToolbarCommands() {
    m_overlayCoordinator->setToolbarCommandSinks(*this, *this);
}

void ScreenshotController::Impl::undoCanvasEdit() {
    if (m_ocrController != nullptr && m_ocrController->tableModeActive()) {
        m_ocrController->undoTableEdit();
        return;
    }
    if (m_ocrController != nullptr && m_ocrController->qrModeActive()) {
        return;
    }
    if (m_ocrController != nullptr && m_ocrController->editing()) {
        m_ocrController->undoTextEdit();
        return;
    }
    m_overlayCoordinator->undoCanvasEdit();
}

void ScreenshotController::Impl::redoCanvasEdit() {
    if (m_ocrController != nullptr && m_ocrController->tableModeActive()) {
        m_ocrController->redoTableEdit();
        return;
    }
    if (m_ocrController != nullptr && m_ocrController->qrModeActive()) {
        return;
    }
    if (m_ocrController != nullptr && m_ocrController->editing()) {
        m_ocrController->redoTextEdit();
        return;
    }
    m_overlayCoordinator->redoCanvasEdit();
}

void ScreenshotController::Impl::connectSelectorSignals() {
    QObject::connect(m_selectorCoordinator, &ScreenshotSelectorCoordinator::refreshFinished, this,
                     [this](bool ok) { m_selectorWorkflow->handleRefreshFinished(ok); });
    QObject::connect(m_selectorCoordinator, &ScreenshotSelectorCoordinator::hitTestFinished, this,
                     [this](bool ok, const QVector<QRectF>& hitRects) {
                         m_selectorWorkflow->handleHitTestFinished(ok, hitRects);
                     });
}

void ScreenshotController::Impl::setMoveTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    static_cast<void>(resetCanvasEditingState());
    m_toolCommandWorkflow->setMoveTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

bool ScreenshotController::Impl::activateToolForSelectionResize(ScreenshotActiveTool tool) {
    switch (tool) {
    case ScreenshotActiveTool::Move: {
        m_ocrController->deactivateForSelectionResize();
        const bool scrollingCaptureStopped = stopScrollingCapture(true);
        static_cast<void>(resetCanvasEditingState());
        m_toolCommandWorkflow->setMoveTool();
        restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
        break;
    }
    case ScreenshotActiveTool::Select:
        setSelectTool();
        break;
    case ScreenshotActiveTool::Shape:
        setShapeTool();
        break;
    case ScreenshotActiveTool::Arrow:
        setArrowTool();
        break;
    case ScreenshotActiveTool::Line:
        setLineTool();
        break;
    case ScreenshotActiveTool::FreeDraw:
        setFreeDrawTool();
        break;
    case ScreenshotActiveTool::RectangleHighlight:
        setHighlightTool();
        break;
    case ScreenshotActiveTool::PenHighlight:
        setPenHighlightTool();
        break;
    case ScreenshotActiveTool::Eraser:
        setEraserTool();
        break;
    case ScreenshotActiveTool::RectangleFilter:
        setRectangleFilterTool();
        break;
    case ScreenshotActiveTool::PenFilter:
        setPenFilterTool();
        break;
    case ScreenshotActiveTool::Watermark:
        setWatermarkTool();
        break;
    case ScreenshotActiveTool::Text:
        setTextTool();
        break;
    case ScreenshotActiveTool::SerialNumber:
        setSerialNumberTool();
        break;
    case ScreenshotActiveTool::Spotlight:
        setSpotlightTool();
        break;
    case ScreenshotActiveTool::Ocr:
    case ScreenshotActiveTool::Table:
    case ScreenshotActiveTool::Qr:
        QTimer::singleShot(0, this,
                           [this, tool]() { activateRecognitionToolAfterSelectionResize(tool); });
        break;
    }

    if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
        toolbar->setActiveTool(paletteToolForActiveTool(tool));
    }
    return tool == ScreenshotActiveTool::Ocr || tool == ScreenshotActiveTool::Table ||
           tool == ScreenshotActiveTool::Qr || m_interaction.activeTool() == tool;
}

void ScreenshotController::Impl::activateRecognitionToolAfterSelectionResize(
    ScreenshotActiveTool tool) {
    if (!m_interaction.moveToolActive() || m_interaction.dragging() ||
        !m_selection.hasPixelSelection()) {
        return;
    }

    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    if (tool == ScreenshotActiveTool::Ocr) {
        m_ocrController->activate();
    } else if (tool == ScreenshotActiveTool::Table) {
        m_ocrController->activateTable();
    } else if (tool == ScreenshotActiveTool::Qr) {
        m_ocrController->activateQr();
    } else {
        return;
    }
    m_presentationServices->updateOverlayState();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setSelectTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setSelectTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setShapeTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setShapeTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setArrowTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setArrowTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setTextTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setTextTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setSerialNumberTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setSerialNumberTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setOcrTool() {
    ++m_ocrActivationId;
    m_ocrFromQuickFunction = m_activatingQuickOcr;
    m_ocrTranslateAfterRecognition = false;
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    if (resetCanvasEditingState()) {
        m_interaction.setCanvasTool(ScreenshotActiveTool::Select);
    }
    m_ocrController->activate();
    m_presentationServices->updateOverlayState();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::handleAutomaticTextRecognitionAction(bool available) {
    if (!available || m_ocrController == nullptr || !m_ocrController->active() ||
        m_ocrController->mode() != ScreenshotOcrController::Mode::Text ||
        !m_ocrController->hasTextResult()) {
        return;
    }
    if (m_ocrAutoActionHandledActivationId == m_ocrActivationId) {
        return;
    }

    m_ocrAutoActionHandledActivationId = m_ocrActivationId;

    if (m_ocrTranslateAfterRecognition) {
        m_ocrTranslateAfterRecognition = false;
        m_ocrController->beginTextTranslation();
        return;
    }

    const QString action =
        snow_shot::storage::ScreenshotSettings().autoExecuteAfterTextRecognition();
    const bool quickOnly = action == QStringLiteral("quick_copy_text") ||
                           action == QStringLiteral("quick_copy_text_and_end_screenshot");
    if (quickOnly && !m_ocrFromQuickFunction) {
        return;
    }

    if (action == QStringLiteral("copy_text") || action == QStringLiteral("quick_copy_text")) {
        static_cast<void>(m_ocrController->copyRecognitionToClipboard(false));
    } else if (action == QStringLiteral("copy_text_and_end_screenshot") ||
               action == QStringLiteral("quick_copy_text_and_end_screenshot")) {
        static_cast<void>(m_ocrController->copyRecognitionToClipboard(true));
    } else if (action == QStringLiteral("enable_edit_mode")) {
        m_ocrController->beginTextEditing();
    }
}

void ScreenshotController::Impl::setTableTool() {
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_ocrController->activateTable();
    m_presentationServices->updateOverlayState();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setQrTool() {
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_ocrController->activateQr();
    m_presentationServices->updateOverlayState();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setTextTranslationTool() {
    ++m_ocrActivationId;
    m_ocrFromQuickFunction = false;
    m_ocrTranslateAfterRecognition = true;
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    if (resetCanvasEditingState()) {
        m_interaction.setCanvasTool(ScreenshotActiveTool::Select);
    }
    m_ocrController->activate();
    m_presentationServices->updateOverlayState();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
    if (m_overlayCoordinator != nullptr) {
        if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
            toolbar->setActiveTool(ScreenshotToolPalette::Tool::TextTranslation);
        }
    }
}

void ScreenshotController::Impl::mergeTableSelection() {
    m_ocrController->mergeTableSelection();
}

void ScreenshotController::Impl::splitTableSelection() {
    m_ocrController->splitTableSelection();
}

void ScreenshotController::Impl::resetTable() {
    m_ocrController->resetTable();
}

void ScreenshotController::Impl::toggleTextEditing() {
    if (m_ocrController->translating()) {
        m_ocrController->endTextEditing();
        m_ocrController->beginTextEditing();
    } else if (m_ocrController->editing()) {
        m_ocrController->endTextEditing();
    } else {
        m_ocrController->beginTextEditing();
    }
}

void ScreenshotController::Impl::toggleTextTranslation() {
    if (m_ocrController->translating()) {
        m_ocrController->endTextEditing();
    } else {
        m_ocrController->beginTextTranslation();
    }
}

void ScreenshotController::Impl::resetTextEditing() {
    m_ocrController->resetTextEditing();
}

void ScreenshotController::Impl::openTextTranslationSettings() {
    m_ocrController->openTranslationSettings();
}

void ScreenshotController::Impl::applyTextFormatting(const QString& value) {
    m_ocrController->applyTextFormatting(value);
}

void ScreenshotController::Impl::applyTextPunctuation(const QString& value) {
    m_ocrController->applyTextPunctuation(value);
}

bool ScreenshotController::Impl::stopScrollingCapture(bool restoreScreenshotPresentation) {
    if (m_scrollingCaptureController == nullptr || !m_scrollingCaptureController->active()) {
        return false;
    }

    m_scrollingCaptureController->stop(restoreScreenshotPresentation);
    if (m_overlayCoordinator != nullptr) {
        if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
            toolbar->setScrollingScreenshotMode(false);
        }
    }
    return true;
}

void ScreenshotController::Impl::pauseScrollingCaptureForSelectionResize() {
    static_cast<void>(stopScrollingCapture(false));
}

void ScreenshotController::Impl::resumeScrollingCaptureAfterSelectionResize() {
    if (m_scrollingCaptureController == nullptr || m_scrollingCaptureController->active() ||
        !m_selection.hasPixelSelection()) {
        return;
    }

    const ScreenshotScrollingRecognitionMode mode = m_scrollingCaptureController->recognitionMode();
    if (!m_scrollingCaptureController->start(m_selection.pixelSelection(), mode)) {
        m_interaction.setMoveTool(true, false);
        m_captureState.sessionState = ScreenshotSessionState::Editing;
        restoreToolUiAfterScrollingCapture(true);
        return;
    }

    m_interaction.enterScrollingCapture();
    m_captureState.sessionState = ScreenshotSessionState::Editing;
    m_presentationServices->updateOverlayState();
    m_colorPickerController->hide();
    m_toolbarPresenter->hideSelectionToolbar();
    if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
        toolbar->setScrollingScreenshotMode(true);
    }
}

std::optional<quint64> ScreenshotController::Impl::beginImageExport() {
    if (m_imageExportInFlight) {
        return std::nullopt;
    }
    m_imageExportInFlight = true;
    m_imageExportGeneration = owner.nextOperationGeneration();
    return m_imageExportGeneration;
}

bool ScreenshotController::Impl::finishImageExport(quint64 generation) {
    if (!m_imageExportInFlight || generation != m_imageExportGeneration) {
        return false;
    }
    m_imageExportInFlight = false;
    return true;
}

bool ScreenshotController::Impl::imageExportCurrent(quint64 generation) const {
    return m_imageExportInFlight && generation == m_imageExportGeneration;
}

void ScreenshotController::Impl::hideImageExportPresentation() {
    if (m_colorPickerController != nullptr) {
        m_colorPickerController->hide();
    }
    if (m_toolbarPresenter != nullptr) {
        m_toolbarPresenter->hideSelectionToolbar();
    }
    if (m_overlayCoordinator != nullptr) {
        m_overlayCoordinator->hideOverlayWindowsImmediately(m_displaySession);
    }
}

void ScreenshotController::Impl::completeScrollingResultExport(quint64 generation) {
    if (!finishImageExport(generation)) {
        return;
    }
    m_exportJob = {};
    m_clipboardCommit = {};
    static_cast<void>(stopScrollingCapture(false));
    m_ocrController->invalidateSession();
    m_captureWorkflow->cancelCapture();
}

void ScreenshotController::Impl::restoreToolUiAfterScrollingCapture(bool scrollingCaptureStopped) {
    if (!scrollingCaptureStopped) {
        return;
    }

    m_presentationServices->updateOverlayState();
    m_presentationServices->showToolbar();
    m_presentationServices->showSelectionToolbar();
}

void ScreenshotController::Impl::startScrollingScreenshot() {
    m_ocrController->deactivate();
    if (m_scrollingCaptureController == nullptr || m_scrollingCaptureController->active() ||
        !m_selection.hasPixelSelection()) {
        return;
    }

    const QRect selection = m_selection.pixelSelection();
    if (!m_scrollingCaptureController->start(selection,
                                             ScreenshotScrollingRecognitionMode::Vertical)) {
        if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
            toolbar->setScrollingScreenshotMode(false);
        }
        return;
    }
    static_cast<void>(resetCanvasEditingState());

    m_interaction.enterScrollingCapture();
    m_captureState.sessionState = ScreenshotSessionState::Editing;
    m_presentationServices->updateOverlayState();
    m_colorPickerController->hide();
    m_toolbarPresenter->hideSelectionToolbar();
    if (ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar()) {
        toolbar->setScrollingScreenshotMode(true);
    }
}

void ScreenshotController::Impl::pinSelectionToScreen() {
    if (m_scrollingCaptureController != nullptr && m_scrollingCaptureController->active()) {
        const QSize sourceSize = m_scrollingCaptureController->trimmedSize();
        if (sourceSize.isEmpty()) {
            return;
        }
        SNOW_SHOT_PIN_PERF_BEGIN("scrolling-selection", sourceSize.width(), sourceSize.height());
        SNOW_SHOT_PIN_PERF_MILESTONE("controller.enter");
        const QRect selection = m_scrollingCaptureController->canvasSelection();
        const CapturedDisplayModel* display = m_geometry.displayForCanvasPoint(
            m_displaySession, ScreenshotHalfOpenRect::fromRect(selection).center());
        if (display == nullptr || display->screen == nullptr) {
            SNOW_SHOT_PIN_PERF_FINISH(false);
            return;
        }
        const QPointer<ScreenshotController> receiver(&owner);
        const QPointer<QScreen> targetScreen(display->screen);
        const QString targetScreenName = display->name;
        const QRect targetPhysicalRect = display->physicalRect;
        const bool autoResizeWindow = snow_shot::storage::PinToScreenSettings().autoResizeWindow();
        const std::optional<quint64> exportGeneration = beginImageExport();
        if (!exportGeneration.has_value()) {
            SNOW_SHOT_PIN_PERF_FINISH(false);
            return;
        }
        const bool scheduled = m_scrollingCaptureController->requestTrimmedSnapshot(
            [receiver, targetScreen, targetScreenName, targetPhysicalRect, autoResizeWindow,
             generation = *exportGeneration](ScreenshotScrollingSnapshot snapshot) mutable {
                if (receiver.isNull() || receiver->m_impl == nullptr) {
                    SNOW_SHOT_PIN_PERF_FINISH(false);
                    return;
                }
                if (!receiver->m_impl->imageExportCurrent(generation)) {
                    SNOW_SHOT_PIN_PERF_FINISH(false);
                    return;
                }
                receiver->m_impl->m_exportJob = ScreenshotExportCoordinator::shared().submit(
                    receiver, ScreenshotExportCoordinator::Priority::Foreground,
                    [snapshot = std::move(snapshot)](
                        const ScreenshotExportCancellation& cancellation) mutable {
                        if (cancellation.isCancellationRequested()) {
                            return ScreenshotExportTaskResult::failure(
                                ScreenshotExportFailureStage::Cancelled,
                                QStringLiteral("The screenshot pin was cancelled"));
                        }
                        QImage image = snapshot.materialize();
                        if (image.isNull()) {
                            return ScreenshotExportTaskResult::failure(
                                ScreenshotExportFailureStage::Render,
                                QStringLiteral("The scrolling screenshot could not be rendered"));
                        }
                        ScreenshotExportTaskResult result;
                        result.image = std::move(image);
                        return result;
                    },
                    [receiver, targetScreen, targetScreenName, targetPhysicalRect, autoResizeWindow,
                     generation](ScreenshotExportTaskResult result) mutable {
                        if (receiver.isNull() || receiver->m_impl == nullptr ||
                            !receiver->m_impl->imageExportCurrent(generation)) {
                            SNOW_SHOT_PIN_PERF_FINISH(false);
                            return;
                        }
                        QScreen* resolvedScreen = targetScreen;
                        if (resolvedScreen == nullptr ||
                            !QGuiApplication::screens().contains(resolvedScreen)) {
                            resolvedScreen = ScreenshotGeometryMapper::screenForCaptureDisplay(
                                targetScreenName, targetPhysicalRect);
                        }
                        const ScreenshotPinnedImageFit fit =
                            resolvedScreen != nullptr && !result.image.isNull()
                                ? (autoResizeWindow
                                       ? ScreenshotGeometryMapper::fitImageToAvailableGeometry(
                                             result.image.size(),
                                             resolvedScreen->availableGeometry(),
                                             resolvedScreen->geometry(),
                                             ScreenshotGeometryMapper::physicalRectForScreen(
                                                 *resolvedScreen),
                                             16)
                                       : ScreenshotGeometryMapper::centerImageAtFullResolution(
                                             result.image.size(),
                                             resolvedScreen->availableGeometry(),
                                             resolvedScreen->geometry(),
                                             ScreenshotGeometryMapper::physicalRectForScreen(
                                                 *resolvedScreen)))
                                : ScreenshotPinnedImageFit{};
                        const bool presented =
                            result.succeeded() && fit.valid &&
                            receiver->m_impl->m_selectionExportUiServices->presentPinnedImage(
                                result.image, resolvedScreen, fit.nativeGeometry,
                                fit.fullResolutionSize);
                        SNOW_SHOT_PIN_PERF_MILESTONE("controller.presentation_complete");
                        SNOW_SHOT_PIN_PERF_FINISH(presented);
                        if (!presented) {
                            receiver->m_impl->m_messages->error(
                                QString::fromLatin1(kCopyMessageKey),
                                QCoreApplication::translate(
                                    "ScreenshotController",
                                    "The scrolling screenshot could not be pinned: %1")
                                    .arg(result.error));
                        }
                        receiver->m_impl->completeScrollingResultExport(generation);
                    });
                if (!receiver->m_impl->m_exportJob.isValid()) {
                    SNOW_SHOT_PIN_PERF_FINISH(false);
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "The screenshot export queue is full"));
                    receiver->m_impl->completeScrollingResultExport(generation);
                }
            });
        if (!scheduled) {
            SNOW_SHOT_PIN_PERF_FINISH(false);
            m_messages->error(
                QString::fromLatin1(kCopyMessageKey),
                QCoreApplication::translate("ScreenshotController",
                                            "The scrolling screenshot could not be prepared"));
            completeScrollingResultExport(*exportGeneration);
            return;
        }
        hideImageExportPresentation();
        SNOW_SHOT_PIN_PERF_MILESTONE("controller.presentation_hidden");
        return;
    }
    const QRect perfSelection = m_selection.pixelSelection();
    SNOW_SHOT_PIN_PERF_BEGIN("normal-selection", perfSelection.width(), perfSelection.height());
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.enter");
    SNOW_SHOT_PIN_PERF_SCOPE("controller.pin_selection");
    const bool historyEligible = m_interaction.activeTool() != ScreenshotActiveTool::Ocr &&
                                 m_interaction.activeTool() != ScreenshotActiveTool::Table &&
                                 m_interaction.activeTool() != ScreenshotActiveTool::Qr;
    m_ocrController->deactivate();
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.ocr_deactivated");
    const std::optional<quint64> exportGeneration = beginImageExport();
    if (!exportGeneration.has_value()) {
        SNOW_SHOT_PIN_PERF_FINISH(false);
        return;
    }
    const bool shouldSnapshotHistory =
        historyEligible && m_historyService != nullptr && resetCanvasEditingState();
    struct PinHistoryState {
        std::optional<ScreenshotHistoryEntry> candidate;
        QImage resultImage;
        bool pinDone = false;
        bool pinSuccess = false;
        bool resultDone = false;
        bool committed = false;
    };
    const auto historyState = std::make_shared<PinHistoryState>();
    const QPointer<ScreenshotController> receiver(&owner);
    const auto maybeCommitHistory = std::make_shared<std::function<void()>>();
    *maybeCommitHistory = [receiver, historyState]() {
        if (receiver.isNull() || receiver->m_impl == nullptr || historyState->committed ||
            !historyState->pinDone || !historyState->resultDone) {
            return;
        }
        historyState->committed = true;
        if (historyState->pinSuccess && historyState->candidate.has_value() &&
            !historyState->resultImage.isNull() && receiver->m_impl->m_historyService != nullptr) {
            historyState->candidate->resultImage = std::move(historyState->resultImage);
            historyState->candidate->source =
                snow_shot::storage::CaptureHistorySource::PinnedToScreen;
            receiver->m_impl->m_historyService->commit(std::move(*historyState->candidate));
        }
    };
    if (shouldSnapshotHistory && m_exportService != nullptr) {
        const ScreenshotResultStyle style{m_selection.cornerRadius(), m_selection.shadowWidth(),
                                          m_selection.shadowColor()};
        const bool resultScheduled = m_exportService->requestSelectionResult(
            m_selection.pixelSelection(), style, &owner,
            [receiver, historyState, maybeCommitHistory](QImage image) mutable {
                if (receiver.isNull() || receiver->m_impl == nullptr) {
                    return;
                }
                historyState->resultDone = true;
                if (!image.isNull()) {
                    historyState->resultImage = std::move(image);
                }
                (*maybeCommitHistory)();
            });
        if (!resultScheduled) {
            historyState->resultDone = true;
        }
    } else {
        historyState->resultDone = true;
    }
    const bool scheduled = m_selectionExportWorkflow->pinSelectionToScreen(
        [receiver, generation = *exportGeneration]() {
            return !receiver.isNull() && receiver->m_impl != nullptr &&
                   receiver->m_impl->imageExportCurrent(generation);
        },
        [receiver, generation = *exportGeneration, historyState,
         maybeCommitHistory](bool success) mutable {
            SNOW_SHOT_PIN_PERF_FINISH(success);
            if (receiver.isNull() || receiver->m_impl == nullptr ||
                !receiver->m_impl->finishImageExport(generation)) {
                return;
            }
            historyState->pinDone = true;
            historyState->pinSuccess = success;
            if (!success) {
                qWarning("Screenshot pin export failed");
            }
            (*maybeCommitHistory)();
            if (receiver->m_impl->m_historyService != nullptr) {
                receiver->m_impl->m_historyService->resetCaptureNavigation();
            }
            receiver->m_impl->m_ocrController->invalidateSession();
            receiver->m_impl->m_captureWorkflow->cancelCapture();
        });
    if (!scheduled) {
        historyState->pinDone = true;
        SNOW_SHOT_PIN_PERF_FINISH(false);
        static_cast<void>(finishImageExport(*exportGeneration));
        qWarning("Failed to schedule screenshot pin export");
        m_captureWorkflow->cancelCapture();
        return;
    }
    // Queue the immutable request before touching the live capture presentation.
    // Its callback runs after this event-loop turn, once hide and history work settle.
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.export_scheduled");
    hideImageExportPresentation();
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.presentation_hidden");
    if (shouldSnapshotHistory && !prepareHistoryCandidate(&historyState->candidate)) {
        static_cast<void>(finishImageExport(*exportGeneration));
        m_captureWorkflow->cancelCapture();
    }
}

void ScreenshotController::Impl::setScrollingScreenshotRecognitionMode(
    ScreenshotScrollingRecognitionMode mode) {
    if (m_scrollingCaptureController == nullptr || !m_scrollingCaptureController->active()) {
        return;
    }
    static_cast<void>(m_scrollingCaptureController->setRecognitionMode(mode));
}

void ScreenshotController::Impl::pinClipboardContentToScreen() {
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        qWarning("No screen is available for clipboard pinning");
        return;
    }

    auto snapshot = ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(),
                                                               screen->devicePixelRatio());
    if (!snapshot.has_value()) {
        qWarning("Clipboard content could not be pinned");
        m_messages->error(QString::fromLatin1(kPinClipboardMessageKey),
                          QCoreApplication::translate(
                              "ScreenshotController",
                              "The clipboard does not contain content that can be pinned"));
        return;
    }

    const bool autoResizeWindow = snow_shot::storage::PinToScreenSettings().autoResizeWindow();
    m_clipboardPinJob.cancel();
    m_clipboardPinGeneration = owner.nextOperationGeneration();
    const quint64 generation = m_clipboardPinGeneration;
    auto content = std::make_shared<std::optional<ScreenshotClipboardContent>>();
    const QPointer<ScreenshotController> receiver(&owner);
    const QPointer<QScreen> guardedScreen(screen);
    m_clipboardPinJob = ScreenshotExportCoordinator::shared().submit(
        &owner, ScreenshotExportCoordinator::Priority::Foreground,
        [snapshot = std::move(*snapshot),
         content](const ScreenshotExportCancellation& cancellation) mutable {
            *content =
                ScreenshotClipboardContentReader::decode(std::move(snapshot), [&cancellation]() {
                    return cancellation.isCancellationRequested();
                });
            if (!content->has_value() || !content->value().isValid()) {
                return ScreenshotExportTaskResult::failure(
                    cancellation.isCancellationRequested() ? ScreenshotExportFailureStage::Cancelled
                                                           : ScreenshotExportFailureStage::Source,
                    cancellation.isCancellationRequested()
                        ? QStringLiteral("The clipboard pin was cancelled")
                        : QStringLiteral("The clipboard content could not be decoded"));
            }
            return ScreenshotExportTaskResult{};
        },
        [receiver, guardedScreen, generation, autoResizeWindow,
         content](ScreenshotExportTaskResult result) mutable {
            if (receiver.isNull() || receiver->m_impl == nullptr ||
                generation != receiver->m_impl->m_clipboardPinGeneration) {
                return;
            }
            receiver->m_impl->m_clipboardPinJob = {};
            if (!result.succeeded() || !content->has_value() || guardedScreen.isNull()) {
                if (result.failureStage != ScreenshotExportFailureStage::Cancelled) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kPinClipboardMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "The clipboard content could not be pinned: %1")
                            .arg(result.error));
                }
                return;
            }

            ScreenshotClipboardContent decoded = std::move(content->value());
            const ScreenshotPinnedImageFit fit =
                autoResizeWindow
                    ? ScreenshotGeometryMapper::fitImageToAvailableGeometry(
                          decoded.image.size(), guardedScreen->availableGeometry(),
                          guardedScreen->geometry(),
                          ScreenshotGeometryMapper::physicalRectForScreen(*guardedScreen), 16)
                    : ScreenshotGeometryMapper::centerImageAtFullResolution(
                          decoded.image.size(), guardedScreen->availableGeometry(),
                          guardedScreen->geometry(),
                          ScreenshotGeometryMapper::physicalRectForScreen(*guardedScreen));
            if (!fit.valid || receiver->m_impl->m_selectionExportUiServices == nullptr ||
                !receiver->m_impl->m_selectionExportUiServices->presentPinnedImage(
                    decoded.image, guardedScreen, fit.nativeGeometry, fit.fullResolutionSize,
                    std::move(decoded.formattedDocument), decoded.plainText,
                    decoded.formattedTextDevicePixelRatio, std::move(decoded.originalContent))) {
                receiver->m_impl->m_messages->error(
                    QString::fromLatin1(kPinClipboardMessageKey),
                    QCoreApplication::translate("ScreenshotController",
                                                "The clipboard pin could not be presented"));
            }
        });
    if (!m_clipboardPinJob.isValid()) {
        m_messages->error(
            QString::fromLatin1(kPinClipboardMessageKey),
            QCoreApplication::translate("ScreenshotController", "The clipboard pin queue is full"));
    }
}

void ScreenshotController::Impl::saveSelectionToFile() {
    if (m_ocrController != nullptr && m_ocrController->active()) {
        m_messages->warning(
            QString::fromLatin1(kSaveMessageKey),
            QCoreApplication::translate("ScreenshotController",
                                        "Exit text recognition before saving the screenshot"));
        return;
    }

    const snow_shot::storage::ScreenshotSettings outputSettings;
    const QString directory = ScreenshotImageFileService::saveDialogDirectory(
        outputSettings.lastManualSaveDirectory(), outputSettings.imageSaveDirectory());
    static_cast<void>(QDir().mkpath(directory));
    const QString initialPath = QDir(directory).filePath(
        ScreenshotImageFileService::suggestedBaseName(outputSettings.manualSaveFilenameFormat()) +
        QStringLiteral(".png"));
    QString selectedFilter =
        ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Png);
    QWidget* parent = m_overlayCoordinator != nullptr ? m_overlayCoordinator->toolbar() : nullptr;
    const QString selectedPath = QFileDialog::getSaveFileName(
        parent, QCoreApplication::translate("ScreenshotController", "Save screenshot"), initialPath,
        ScreenshotImageFileService::saveDialogFilter(), &selectedFilter);
    if (selectedPath.isEmpty()) {
        return;
    }
    static_cast<void>(
        outputSettings.setLastManualSaveDirectory(QFileInfo(selectedPath).absolutePath()));

    const ScreenshotImageFileFormat format =
        ScreenshotImageFileService::formatForDialogSelection(selectedPath, selectedFilter);
    const QString outputPath = ScreenshotImageFileService::normalizedPath(selectedPath, format);
    const std::optional<quint64> exportGeneration = beginImageExport();
    if (!exportGeneration.has_value()) {
        return;
    }

    const QPointer<ScreenshotController> receiver(&owner);
    const auto imageReady = [receiver, generation = *exportGeneration, outputPath,
                             format](QImage image) mutable {
        if (receiver.isNull() || receiver->m_impl == nullptr ||
            !receiver->m_impl->imageExportCurrent(generation)) {
            return;
        }
        receiver->m_impl->saveImageToFile(std::move(image), outputPath, format, generation);
    };

    bool scheduled = false;
    if (m_scrollingCaptureController != nullptr && m_scrollingCaptureController->active()) {
        scheduled = m_scrollingCaptureController->requestTrimmedSnapshot(
            [receiver, generation = *exportGeneration, outputPath,
             format](ScreenshotScrollingSnapshot snapshot) mutable {
                if (receiver.isNull() || receiver->m_impl == nullptr ||
                    !receiver->m_impl->imageExportCurrent(generation)) {
                    return;
                }
                receiver->m_impl->saveSnapshotToFile(std::move(snapshot), outputPath, format,
                                                     generation);
            });
    } else if (m_selection.hasPixelSelection() && m_exportService != nullptr) {
        const ScreenshotResultStyle style{m_selection.cornerRadius(), m_selection.shadowWidth(),
                                          m_selection.shadowColor()};
        scheduled = m_exportService->requestSelectionResult(m_selection.pixelSelection(), style,
                                                            &owner, std::move(imageReady));
    }
    if (!scheduled) {
        static_cast<void>(finishImageExport(*exportGeneration));
        m_messages->error(
            QString::fromLatin1(kSaveMessageKey),
            QCoreApplication::translate("ScreenshotController",
                                        "The screenshot could not be prepared for saving"));
    }
}

void ScreenshotController::Impl::saveImageToFile(QImage image, const QString& outputPath,
                                                 ScreenshotImageFileFormat format,
                                                 quint64 generation) {
    const QPointer<ScreenshotController> receiver(&owner);
    m_exportJob = ScreenshotExportCoordinator::shared().submit(
        &owner, ScreenshotExportCoordinator::Priority::Foreground,
        [image = std::move(image), outputPath,
         format](const ScreenshotExportCancellation& cancellation) mutable {
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The screenshot save was cancelled"));
            }
            const ScreenshotImageFileSaveResult saved =
                ScreenshotImageFileService::write(image, outputPath, format);
            if (!saved.succeeded()) {
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                           saved.error);
            }
            ScreenshotExportTaskResult result;
            result.savedPath = saved.path;
            return result;
        },
        [receiver, generation](ScreenshotExportTaskResult result) mutable {
            if (!receiver.isNull() && receiver->m_impl != nullptr) {
                receiver->m_impl->completeFileSave(std::move(result), generation);
            }
        });
    if (!m_exportJob.isValid()) {
        completeFileSave(ScreenshotExportTaskResult::failure(
                             ScreenshotExportFailureStage::Queue,
                             QStringLiteral("The screenshot export queue is full")),
                         generation);
    }
}

void ScreenshotController::Impl::saveSnapshotToFile(ScreenshotScrollingSnapshot snapshot,
                                                    const QString& outputPath,
                                                    ScreenshotImageFileFormat format,
                                                    quint64 generation) {
    const QPointer<ScreenshotController> receiver(&owner);
    m_exportJob = ScreenshotExportCoordinator::shared().submit(
        &owner, ScreenshotExportCoordinator::Priority::Foreground,
        [snapshot = std::move(snapshot), outputPath,
         format](const ScreenshotExportCancellation& cancellation) mutable {
            const ScreenshotImageRowSource source = snapshot.rowSource(
                [&cancellation]() { return cancellation.isCancellationRequested(); });
            if (!source.isValid()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Source,
                    QStringLiteral("The scrolling screenshot is unavailable"));
            }
            const ScreenshotImageFileSaveResult saved =
                ScreenshotImageFileService::write(source, outputPath, format);
            if (!saved.succeeded()) {
                return ScreenshotExportTaskResult::failure(
                    cancellation.isCancellationRequested() ? ScreenshotExportFailureStage::Cancelled
                                                           : ScreenshotExportFailureStage::File,
                    saved.error);
            }
            ScreenshotExportTaskResult result;
            result.savedPath = saved.path;
            return result;
        },
        [receiver, generation](ScreenshotExportTaskResult result) mutable {
            if (!receiver.isNull() && receiver->m_impl != nullptr) {
                receiver->m_impl->completeFileSave(std::move(result), generation);
            }
        });
    if (!m_exportJob.isValid()) {
        completeFileSave(ScreenshotExportTaskResult::failure(
                             ScreenshotExportFailureStage::Queue,
                             QStringLiteral("The screenshot export queue is full")),
                         generation);
    }
}

void ScreenshotController::Impl::completeFileSave(ScreenshotExportTaskResult result,
                                                  quint64 generation) {
    if (!finishImageExport(generation)) {
        return;
    }
    m_exportJob = {};
    if (!result.succeeded()) {
        m_messages->error(QString::fromLatin1(kSaveMessageKey),
                          QCoreApplication::translate("ScreenshotController",
                                                      "The screenshot could not be saved: %1")
                              .arg(result.error));
        return;
    }
    static_cast<void>(stopScrollingCapture(false));
    m_ocrController->invalidateSession();
    m_captureWorkflow->cancelCapture();
}

void ScreenshotController::Impl::cancelCapture() {
    clearCanvasColorSampling();
    if (m_overlayInputHandler != nullptr) {
        m_overlayInputHandler->resetTransientShortcuts();
    }
    m_exportJob.cancel();
    m_exportJob = {};
    m_backgroundSaveJob.cancel();
    m_backgroundSaveJob = {};
    m_clipboardCommit.cancel();
    m_clipboardCommit = {};
    if (m_selectionExportUiServices != nullptr) {
        m_selectionExportUiServices->cancelClipboardPublication();
    }
    m_imageExportGeneration = owner.nextOperationGeneration();
    m_imageExportInFlight = false;
    resetPendingCaptureRequest();
    m_ocrController->invalidateSession();
    static_cast<void>(stopScrollingCapture(false));
    if (m_historyService != nullptr) {
        m_historyService->resetCaptureNavigation();
    }
    m_captureWorkflow->cancelCapture();
    owner.scheduleIdleImplementationRelease(this);
}

void ScreenshotController::Impl::copySelectionToClipboard() {
    copySelectionToClipboardWithSource(snow_shot::storage::CaptureHistorySource::CopiedToClipboard);
}

void ScreenshotController::Impl::copySelectionToClipboardWithSource(
    snow_shot::storage::CaptureHistorySource historySource) {
    if (m_ocrController->active()) {
        if (m_ocrController->copyRecognitionToClipboard()) {
            return;
        }
        m_messages->error(QString::fromLatin1(kCopyMessageKey),
                          QCoreApplication::translate("ScreenshotController",
                                                      "No recognized result is available to copy"));
        return;
    }
    const snow_shot::storage::ScreenshotSettings settings;
    const bool autoSave = settings.autoSaveAfterCopy();
    const bool copyFileToClipboard = settings.copyImageFileToClipboard();
    const bool materializeImage = autoSave || copyFileToClipboard;
    if (m_scrollingCaptureController != nullptr && m_scrollingCaptureController->active()) {
        const std::optional<quint64> exportGeneration = beginImageExport();
        if (!exportGeneration.has_value()) {
            return;
        }
        const QPointer<ScreenshotController> receiver(&owner);
        const bool scheduled = m_scrollingCaptureController->requestTrimmedSnapshot(
            [receiver, generation = *exportGeneration, materializeImage, copyFileToClipboard,
             historySource](ScreenshotScrollingSnapshot snapshot) mutable {
                if (receiver.isNull() || receiver->m_impl == nullptr ||
                    !receiver->m_impl->imageExportCurrent(generation)) {
                    return;
                }
                if (materializeImage) {
                    receiver->m_impl->saveScrollingSnapshotForCopy(
                        std::move(snapshot), generation, copyFileToClipboard, historySource);
                    return;
                }
                receiver->m_impl->m_exportJob = ScreenshotExportCoordinator::shared().submit(
                    receiver, ScreenshotExportCoordinator::Priority::Foreground,
                    [snapshot = std::move(snapshot)](
                        const ScreenshotExportCancellation& cancellation) mutable {
                        const ScreenshotImageRowSource source = snapshot.rowSource(
                            [&cancellation]() { return cancellation.isCancellationRequested(); });
                        auto payload = std::make_shared<ScreenshotClipboardPayload>(
                            ScreenshotClipboardService::prepare(
                                source, ScreenshotClipboardFormatMode::CompatibleDib));
                        if (!payload->isValid()) {
                            return ScreenshotExportTaskResult::failure(
                                cancellation.isCancellationRequested()
                                    ? ScreenshotExportFailureStage::Cancelled
                                    : ScreenshotExportFailureStage::Clipboard,
                                cancellation.isCancellationRequested()
                                    ? QStringLiteral("The screenshot copy was cancelled")
                                    : QStringLiteral("The screenshot could not be prepared for the "
                                                     "clipboard"));
                        }
                        ScreenshotExportTaskResult result;
                        result.clipboardPayload = std::move(payload);
                        return result;
                    },
                    [receiver, generation](ScreenshotExportTaskResult result) mutable {
                        if (receiver.isNull() || receiver->m_impl == nullptr ||
                            !receiver->m_impl->imageExportCurrent(generation)) {
                            return;
                        }
                        receiver->m_impl->m_exportJob = {};
                        if (!result.succeeded() || result.clipboardPayload == nullptr) {
                            receiver->m_impl->m_messages->error(
                                QString::fromLatin1(kCopyMessageKey),
                                QCoreApplication::translate(
                                    "ScreenshotController",
                                    "The scrolling screenshot could not be copied: %1")
                                    .arg(result.error));
                            receiver->m_impl->completeScrollingResultExport(generation);
                            return;
                        }
                        receiver->m_impl->m_clipboardCommit = ScreenshotClipboardService::commit(
                            QApplication::clipboard(), receiver,
                            std::move(*result.clipboardPayload),
                            [receiver, generation](ScreenshotClipboardCommitResult commit) {
                                if (receiver.isNull() || receiver->m_impl == nullptr ||
                                    !receiver->m_impl->imageExportCurrent(generation)) {
                                    return;
                                }
                                if (!commit.succeeded()) {
                                    receiver->m_impl->m_messages->error(
                                        QString::fromLatin1(kCopyMessageKey),
                                        QCoreApplication::translate(
                                            "ScreenshotController",
                                            "The scrolling screenshot could not be copied: %1")
                                            .arg(commit.errorString()));
                                }
                                receiver->m_impl->completeScrollingResultExport(generation);
                            });
                        if (!receiver->m_impl->m_clipboardCommit.isValid()) {
                            receiver->m_impl->m_messages->error(
                                QString::fromLatin1(kCopyMessageKey),
                                QCoreApplication::translate(
                                    "ScreenshotController",
                                    "The scrolling screenshot clipboard operation could not be "
                                    "started"));
                            receiver->m_impl->completeScrollingResultExport(generation);
                        }
                    });
                if (!receiver->m_impl->m_exportJob.isValid()) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "The screenshot export queue is full"));
                    receiver->m_impl->completeScrollingResultExport(generation);
                }
            });
        if (!scheduled) {
            m_messages->error(
                QString::fromLatin1(kCopyMessageKey),
                QCoreApplication::translate("ScreenshotController",
                                            "The scrolling screenshot could not be prepared"));
            completeScrollingResultExport(*exportGeneration);
            return;
        }
        hideImageExportPresentation();
        return;
    }
    const bool historyEligible = m_interaction.activeTool() != ScreenshotActiveTool::Ocr &&
                                 m_interaction.activeTool() != ScreenshotActiveTool::Table &&
                                 m_interaction.activeTool() != ScreenshotActiveTool::Qr;
    m_ocrController->deactivate();
    const std::optional<quint64> exportGeneration = beginImageExport();
    if (!exportGeneration.has_value()) {
        return;
    }
    hideImageExportPresentation();
    const bool shouldSnapshotHistory =
        historyEligible && m_historyService != nullptr && resetCanvasEditingState();
    auto historyCandidate = std::make_shared<std::optional<ScreenshotHistoryEntry>>();
    const QPointer<ScreenshotController> receiver(&owner);
    if (materializeImage) {
        const ScreenshotResultStyle style{m_selection.cornerRadius(), m_selection.shadowWidth(),
                                          m_selection.shadowColor()};
        const ScreenshotResultStyle normalizedStyle =
            ScreenshotResultCompositor::normalizedStyle(style);
        const ScreenshotClipboardFormatMode clipboardFormat =
            normalizedStyle.cornerRadius == 0 && normalizedStyle.shadowWidth == 0
                ? ScreenshotClipboardFormatMode::CompatibleDib
                : ScreenshotClipboardFormatMode::DibV5;
        const bool scheduled = m_exportService->requestSelectionResult(
            m_selection.pixelSelection(), style, &owner,
            [receiver, generation = *exportGeneration, copyFileToClipboard, clipboardFormat,
             historyCandidate, historySource](QImage image) mutable {
                if (receiver.isNull() || receiver->m_impl == nullptr ||
                    !receiver->m_impl->imageExportCurrent(generation)) {
                    return;
                }
                receiver->m_impl->saveImageForCopy(std::move(image), generation,
                                                   copyFileToClipboard, clipboardFormat,
                                                   historySource, historyCandidate, false);
            });
        if (!scheduled) {
            static_cast<void>(finishImageExport(*exportGeneration));
            qWarning("Failed to schedule screenshot image export");
            m_captureWorkflow->cancelCapture();
            return;
        }
        if (shouldSnapshotHistory && !prepareHistoryCandidate(historyCandidate.get())) {
            static_cast<void>(finishImageExport(*exportGeneration));
            m_captureWorkflow->cancelCapture();
        }
        return;
    }
    const bool scheduled = m_selectionExportWorkflow->copySelectionToClipboard(
        [receiver, generation = *exportGeneration]() {
            return !receiver.isNull() && receiver->m_impl != nullptr &&
                   receiver->m_impl->imageExportCurrent(generation);
        },
        [receiver, generation = *exportGeneration, historyCandidate,
         historySource](bool success, QImage resultImage) mutable {
            if (receiver.isNull() || receiver->m_impl == nullptr ||
                !receiver->m_impl->finishImageExport(generation)) {
                return;
            }
            if (success && historyCandidate->has_value() && !resultImage.isNull()) {
                historyCandidate->value().resultImage = std::move(resultImage);
            }
            if (success && historyCandidate->has_value() &&
                !historyCandidate->value().resultImage.has_value()) {
                qWarning("Screenshot history commit skipped because the rendered result was "
                         "unavailable");
            }
            if (success && historyCandidate->has_value() &&
                historyCandidate->value().resultImage.has_value() &&
                receiver->m_impl->m_historyService != nullptr) {
                historyCandidate->value().source = historySource;
                receiver->m_impl->m_historyService->commit(std::move(historyCandidate->value()));
            } else if (!success) {
                qWarning("Screenshot clipboard export failed");
            }
            if (receiver->m_impl->m_historyService != nullptr) {
                receiver->m_impl->m_historyService->resetCaptureNavigation();
            }
            receiver->m_impl->m_ocrController->invalidateSession();
            receiver->m_impl->m_captureWorkflow->cancelCapture();
        });
    if (!scheduled) {
        static_cast<void>(finishImageExport(*exportGeneration));
        qWarning("Failed to schedule screenshot clipboard export");
        m_captureWorkflow->cancelCapture();
        return;
    }
    if (shouldSnapshotHistory && !prepareHistoryCandidate(historyCandidate.get())) {
        static_cast<void>(finishImageExport(*exportGeneration));
        m_captureWorkflow->cancelCapture();
    }
}

void ScreenshotController::Impl::saveImageForCopy(
    QImage image, quint64 generation, bool copyFileToClipboard,
    ScreenshotClipboardFormatMode clipboardFormat,
    snow_shot::storage::CaptureHistorySource historySource,
    std::shared_ptr<std::optional<ScreenshotHistoryEntry>> historyCandidate, bool scrolling) {
    if (!scrolling && historyCandidate != nullptr && historyCandidate->has_value() &&
        !image.isNull()) {
        historyCandidate->value().resultImage = image;
    }
    const snow_shot::storage::ScreenshotSettings outputSettings;
    const QStringList outputDirectories =
        ScreenshotImageFileService::automaticDirectories(outputSettings.imageSaveDirectory());
    const ScreenshotImageFileFormat outputFormat =
        ScreenshotImageFileService::formatForKey(outputSettings.imageFormat());
    const QString outputFilenameFormat = outputSettings.autoSaveFilenameFormat();
    const QPointer<ScreenshotController> receiver(&owner);
    if (!copyFileToClipboard) {
        m_backgroundSaveJob = ScreenshotExportCoordinator::shared().submit(
            &owner, ScreenshotExportCoordinator::Priority::Background,
            [image, outputDirectories, outputFormat,
             outputFilenameFormat](const ScreenshotExportCancellation& cancellation) mutable {
                if (cancellation.isCancellationRequested()) {
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled,
                        QStringLiteral("The automatic screenshot save was cancelled"));
                }
                const ScreenshotImageFileSaveResult saved =
                    ScreenshotImageFileService::saveAutomatically(
                        image, outputDirectories, outputFormat, outputFilenameFormat);
                if (!saved.succeeded()) {
                    return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                               saved.error);
                }
                ScreenshotExportTaskResult result;
                result.savedPath = saved.path;
                return result;
            },
            [receiver](ScreenshotExportTaskResult result) {
                if (!receiver.isNull() && receiver->m_impl != nullptr && !result.succeeded() &&
                    result.failureStage != ScreenshotExportFailureStage::Cancelled) {
                    receiver->m_impl->m_messages->warning(
                        QString::fromLatin1(kSaveMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "Automatic screenshot saving failed: %1")
                            .arg(result.error));
                }
            });
        if (!m_backgroundSaveJob.isValid()) {
            m_messages->warning(
                QString::fromLatin1(kSaveMessageKey),
                QCoreApplication::translate(
                    "ScreenshotController",
                    "The screenshot will be copied, but automatic saving could not be queued"));
        }
    }
    m_exportJob = ScreenshotExportCoordinator::shared().submit(
        &owner, ScreenshotExportCoordinator::Priority::Foreground,
        [copyFileToClipboard, clipboardFormat, outputDirectories, outputFormat,
         outputFilenameFormat,
         image = std::move(image)](const ScreenshotExportCancellation& cancellation) mutable {
            if (cancellation.isCancellationRequested() || image.isNull()) {
                return ScreenshotExportTaskResult::failure(
                    cancellation.isCancellationRequested() ? ScreenshotExportFailureStage::Cancelled
                                                           : ScreenshotExportFailureStage::Source,
                    cancellation.isCancellationRequested()
                        ? QStringLiteral("The screenshot copy was cancelled")
                        : QStringLiteral("The screenshot image is empty"));
            }
            if (!copyFileToClipboard) {
                auto payload = std::make_shared<ScreenshotClipboardPayload>(
                    ScreenshotClipboardService::prepareImage(image, clipboardFormat));
                if (!payload->isValid()) {
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Clipboard,
                        QStringLiteral("The screenshot could not be prepared for the clipboard"));
                }
                ScreenshotExportTaskResult result;
                result.clipboardPayload = std::move(payload);
                return result;
            }
            const ScreenshotImageFileSaveResult fileResult =
                ScreenshotImageFileService::saveAutomatically(image, outputDirectories,
                                                              outputFormat, outputFilenameFormat);
            if (!fileResult.succeeded()) {
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                           fileResult.error);
            }
            ScreenshotExportTaskResult result;
            result.savedPath = fileResult.path;
            return result;
        },
        [receiver, generation, copyFileToClipboard, historySource,
         historyCandidate = std::move(historyCandidate),
         scrolling](ScreenshotExportTaskResult result) mutable {
            if (receiver.isNull() || receiver->m_impl == nullptr ||
                !receiver->m_impl->imageExportCurrent(generation)) {
                return;
            }
            if (!result.succeeded()) {
                const QString reason =
                    result.error.isEmpty()
                        ? QCoreApplication::translate("ScreenshotController",
                                                      "The clipboard did not accept the screenshot")
                        : result.error;
                receiver->m_impl->m_messages->error(
                    QString::fromLatin1(kCopyMessageKey),
                    QCoreApplication::translate("ScreenshotController",
                                                "The screenshot could not be copied: %1")
                        .arg(reason));
                receiver->m_impl->completeCopyExport(false, generation, historySource,
                                                     historyCandidate, scrolling);
                return;
            }

            if (copyFileToClipboard) {
                const bool copied = ScreenshotImageFileService::publishFileToClipboard(
                    QApplication::clipboard(), result.savedPath);
                if (!copied) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate(
                            "ScreenshotController",
                            "The screenshot file could not be copied: the clipboard did not "
                            "accept the file"));
                }
                receiver->m_impl->completeCopyExport(copied, generation, historySource,
                                                     historyCandidate, scrolling);
                return;
            }

            receiver->m_impl->m_exportJob = {};
            if (result.clipboardPayload == nullptr) {
                receiver->m_impl->m_messages->error(
                    QString::fromLatin1(kCopyMessageKey),
                    QCoreApplication::translate("ScreenshotController",
                                                "The screenshot clipboard image is unavailable"));
                receiver->m_impl->completeCopyExport(false, generation, historySource,
                                                     historyCandidate, scrolling);
                return;
            }
            receiver->m_impl->m_clipboardCommit = ScreenshotClipboardService::commit(
                QApplication::clipboard(), receiver, std::move(*result.clipboardPayload),
                [receiver, generation, historySource, historyCandidate,
                 scrolling](ScreenshotClipboardCommitResult commit) mutable {
                    if (receiver.isNull() || receiver->m_impl == nullptr ||
                        !receiver->m_impl->imageExportCurrent(generation)) {
                        return;
                    }
                    if (!commit.succeeded()) {
                        receiver->m_impl->m_messages->error(
                            QString::fromLatin1(kCopyMessageKey),
                            QCoreApplication::translate("ScreenshotController",
                                                        "The screenshot could not be copied: %1")
                                .arg(commit.errorString()));
                    }
                    receiver->m_impl->completeCopyExport(
                        commit.succeeded(), generation, historySource, historyCandidate, scrolling);
                });
            if (!receiver->m_impl->m_clipboardCommit.isValid()) {
                receiver->m_impl->m_messages->error(
                    QString::fromLatin1(kCopyMessageKey),
                    QCoreApplication::translate(
                        "ScreenshotController",
                        "The screenshot clipboard operation could not be started"));
                receiver->m_impl->completeCopyExport(false, generation, historySource,
                                                     historyCandidate, scrolling);
            }
        });
    if (!m_exportJob.isValid()) {
        m_messages->error(QString::fromLatin1(kCopyMessageKey),
                          QCoreApplication::translate("ScreenshotController",
                                                      "The screenshot export queue is full"));
        completeCopyExport(false, generation, historySource, std::move(historyCandidate),
                           scrolling);
    }
}

void ScreenshotController::Impl::saveScrollingSnapshotForCopy(
    ScreenshotScrollingSnapshot snapshot, quint64 generation, bool copyFileToClipboard,
    snow_shot::storage::CaptureHistorySource historySource) {
    const snow_shot::storage::ScreenshotSettings outputSettings;
    const QStringList outputDirectories =
        ScreenshotImageFileService::automaticDirectories(outputSettings.imageSaveDirectory());
    const ScreenshotImageFileFormat outputFormat =
        ScreenshotImageFileService::formatForKey(outputSettings.imageFormat());
    const QString outputFilenameFormat = outputSettings.autoSaveFilenameFormat();
    const QPointer<ScreenshotController> receiver(&owner);
    if (!copyFileToClipboard) {
        const ScreenshotScrollingSnapshot saveSnapshot = snapshot;
        m_backgroundSaveJob = ScreenshotExportCoordinator::shared().submit(
            &owner, ScreenshotExportCoordinator::Priority::Background,
            [saveSnapshot, outputDirectories, outputFormat,
             outputFilenameFormat](const ScreenshotExportCancellation& cancellation) mutable {
                const ScreenshotImageRowSource source = saveSnapshot.rowSource(
                    [&cancellation]() { return cancellation.isCancellationRequested(); });
                if (!source.isValid()) {
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Source,
                        QStringLiteral("The scrolling screenshot is unavailable"));
                }
                const ScreenshotImageFileSaveResult saved =
                    ScreenshotImageFileService::saveAutomatically(
                        source, outputDirectories, outputFormat, outputFilenameFormat);
                if (!saved.succeeded()) {
                    return ScreenshotExportTaskResult::failure(
                        cancellation.isCancellationRequested()
                            ? ScreenshotExportFailureStage::Cancelled
                            : ScreenshotExportFailureStage::File,
                        saved.error);
                }
                ScreenshotExportTaskResult result;
                result.savedPath = saved.path;
                return result;
            },
            [receiver](ScreenshotExportTaskResult result) {
                if (!receiver.isNull() && receiver->m_impl != nullptr && !result.succeeded() &&
                    result.failureStage != ScreenshotExportFailureStage::Cancelled) {
                    receiver->m_impl->m_messages->warning(
                        QString::fromLatin1(kSaveMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "Automatic screenshot saving failed: %1")
                            .arg(result.error));
                }
            });
        if (!m_backgroundSaveJob.isValid()) {
            m_messages->warning(
                QString::fromLatin1(kSaveMessageKey),
                QCoreApplication::translate(
                    "ScreenshotController",
                    "The screenshot will be copied, but automatic saving could not be queued"));
        }
    }
    m_exportJob = ScreenshotExportCoordinator::shared().submit(
        &owner, ScreenshotExportCoordinator::Priority::Foreground,
        [snapshot = std::move(snapshot), copyFileToClipboard, outputDirectories, outputFormat,
         outputFilenameFormat](const ScreenshotExportCancellation& cancellation) mutable {
            const ScreenshotImageRowSource source = snapshot.rowSource(
                [&cancellation]() { return cancellation.isCancellationRequested(); });
            if (!source.isValid()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Source,
                    QStringLiteral("The scrolling screenshot is unavailable"));
            }
            if (!copyFileToClipboard) {
                auto payload = std::make_shared<ScreenshotClipboardPayload>(
                    ScreenshotClipboardService::prepare(
                        source, ScreenshotClipboardFormatMode::CompatibleDib));
                if (!payload->isValid()) {
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Clipboard,
                        QStringLiteral("The screenshot could not be prepared for the clipboard"));
                }
                ScreenshotExportTaskResult result;
                result.clipboardPayload = std::move(payload);
                return result;
            }
            const ScreenshotImageFileSaveResult fileResult =
                ScreenshotImageFileService::saveAutomatically(source, outputDirectories,
                                                              outputFormat, outputFilenameFormat);
            if (!fileResult.succeeded()) {
                return ScreenshotExportTaskResult::failure(
                    cancellation.isCancellationRequested() ? ScreenshotExportFailureStage::Cancelled
                                                           : ScreenshotExportFailureStage::File,
                    fileResult.error);
            }
            ScreenshotExportTaskResult result;
            result.savedPath = fileResult.path;
            return result;
        },
        [receiver, generation, copyFileToClipboard,
         historySource](ScreenshotExportTaskResult result) mutable {
            if (receiver.isNull() || receiver->m_impl == nullptr ||
                !receiver->m_impl->imageExportCurrent(generation)) {
                return;
            }
            if (!result.succeeded()) {
                const QString reason =
                    result.error.isEmpty()
                        ? QCoreApplication::translate("ScreenshotController",
                                                      "The clipboard did not accept the screenshot")
                        : result.error;
                receiver->m_impl->m_messages->error(
                    QString::fromLatin1(kCopyMessageKey),
                    QCoreApplication::translate("ScreenshotController",
                                                "The screenshot could not be copied: %1")
                        .arg(reason));
                receiver->m_impl->completeCopyExport(false, generation, historySource, {}, true);
                return;
            }
            if (copyFileToClipboard) {
                const bool copied = ScreenshotImageFileService::publishFileToClipboard(
                    QApplication::clipboard(), result.savedPath);
                if (!copied) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate(
                            "ScreenshotController",
                            "The screenshot file could not be copied: the clipboard did not "
                            "accept the file"));
                }
                receiver->m_impl->completeCopyExport(copied, generation, historySource, {}, true);
                return;
            }

            receiver->m_impl->m_exportJob = {};
            if (result.clipboardPayload == nullptr) {
                receiver->m_impl->m_messages->error(
                    QString::fromLatin1(kCopyMessageKey),
                    QCoreApplication::translate("ScreenshotController",
                                                "The screenshot clipboard image is unavailable"));
                receiver->m_impl->completeCopyExport(false, generation, historySource, {}, true);
                return;
            }
            receiver->m_impl->m_clipboardCommit = ScreenshotClipboardService::commit(
                QApplication::clipboard(), receiver, std::move(*result.clipboardPayload),
                [receiver, generation,
                 historySource](ScreenshotClipboardCommitResult commit) mutable {
                    if (receiver.isNull() || receiver->m_impl == nullptr ||
                        !receiver->m_impl->imageExportCurrent(generation)) {
                        return;
                    }
                    if (!commit.succeeded()) {
                        receiver->m_impl->m_messages->error(
                            QString::fromLatin1(kCopyMessageKey),
                            QCoreApplication::translate(
                                "ScreenshotController",
                                "The scrolling screenshot could not be copied: %1")
                                .arg(commit.errorString()));
                    }
                    receiver->m_impl->completeCopyExport(commit.succeeded(), generation,
                                                         historySource, {}, true);
                });
            if (!receiver->m_impl->m_clipboardCommit.isValid()) {
                receiver->m_impl->m_messages->error(
                    QString::fromLatin1(kCopyMessageKey),
                    QCoreApplication::translate(
                        "ScreenshotController",
                        "The screenshot clipboard operation could not be started"));
                receiver->m_impl->completeCopyExport(false, generation, historySource, {}, true);
            }
        });
    if (!m_exportJob.isValid()) {
        m_messages->error(QString::fromLatin1(kCopyMessageKey),
                          QCoreApplication::translate("ScreenshotController",
                                                      "The screenshot export queue is full"));
        completeCopyExport(false, generation, historySource, {}, true);
    }
}

void ScreenshotController::Impl::completeCopyExport(
    bool success, quint64 generation, snow_shot::storage::CaptureHistorySource historySource,
    std::shared_ptr<std::optional<ScreenshotHistoryEntry>> historyCandidate, bool scrolling) {
    if (!finishImageExport(generation)) {
        return;
    }
    m_exportJob = {};
    m_clipboardCommit = {};
    if (success && !scrolling && historyCandidate != nullptr && historyCandidate->has_value() &&
        !historyCandidate->value().resultImage.has_value()) {
        qWarning("Screenshot history commit skipped because the rendered result was unavailable");
        success = false;
    }
    if (success && historyCandidate != nullptr && historyCandidate->has_value() &&
        m_historyService != nullptr) {
        historyCandidate->value().source = historySource;
        m_historyService->commit(std::move(historyCandidate->value()));
    } else if (!success) {
        qWarning("Screenshot clipboard export failed");
    }
    if (m_historyService != nullptr) {
        m_historyService->resetCaptureNavigation();
    }
    if (scrolling) {
        static_cast<void>(stopScrollingCapture(false));
    }
    m_ocrController->invalidateSession();
    m_captureWorkflow->cancelCapture();
}

bool ScreenshotController::Impl::resetCanvasEditingState() {
    return m_overlayCoordinator != nullptr &&
           m_overlayCoordinator->resetEditingState(m_displaySession);
}

bool ScreenshotController::Impl::prepareHistoryCandidate(
    std::optional<ScreenshotHistoryEntry>* candidate) {
    if (candidate == nullptr) {
        return true;
    }
    candidate->reset();
    if (m_historyService == nullptr) {
        return true;
    }
    *candidate = m_historyService->snapshotCurrent(true);
    return true;
}

void ScreenshotController::Impl::startScreenRecording() {
    m_ocrController->deactivate();
    if (!m_selection.hasPixelSelection() || m_screenRecordingController == nullptr ||
        (m_scrollingCaptureController != nullptr && m_scrollingCaptureController->active())) {
        return;
    }
    QRect physicalRegion = m_selection.pixelSelection().translated(m_geometry.canvasOrigin());
    if (physicalRegion.width() < 2 || physicalRegion.height() < 2) {
        return;
    }
    static_cast<void>(resetCanvasEditingState());

    m_ocrController->invalidateSession();
    m_captureWorkflow->cancelCapture();
    if (m_historyService != nullptr) {
        m_historyService->resetCaptureNavigation();
    }
    QTimer::singleShot(
        0, this, [this, physicalRegion]() { m_screenRecordingController->open(physicalRegion); });
}

void ScreenshotController::Impl::setShapeStyleFromToolbar(const SnowCanvasShapeStyle& style,
                                                          quint32 properties,
                                                          SnowCanvasShapeKind kind) {
    m_toolCommandWorkflow->setShapeStyleFromToolbar(style, properties, kind);
}

void ScreenshotController::Impl::setLineTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setLineTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setFreeDrawTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setFreeDrawTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setHighlightTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setHighlightTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setPenHighlightTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setPenHighlightTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setEraserTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setEraserTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setFilterTool() {
    setRectangleFilterTool();
}

void ScreenshotController::Impl::setSpotlightTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setSpotlightTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setRectangleFilterTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setRectangleFilterTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setPenFilterTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setPenFilterTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setWatermarkTool() {
    m_ocrController->deactivate();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setWatermarkTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setWatermarkConfigFromToolbar(
    const SnowCanvasWatermarkConfig& config) {
    m_toolCommandWorkflow->setWatermarkConfigFromToolbar(config);
}

void ScreenshotController::Impl::setSpotlightConfigFromToolbar(
    const SnowCanvasSpotlightConfig& config) {
    m_toolCommandWorkflow->setSpotlightConfigFromToolbar(config);
}

void ScreenshotController::Impl::previewSpotlightFromToolbar(
    const SnowCanvasSpotlightConfig& config) {
    m_overlayCoordinator->previewSpotlightConfig(m_displaySession, config);
}

void ScreenshotController::Impl::previewWatermarkFromToolbar(
    const SnowCanvasWatermarkConfig& config) {
    m_overlayCoordinator->previewWatermarkConfig(m_displaySession, config);
}

void ScreenshotController::Impl::setFilterStyleFromToolbar(const SnowCanvasFilterStyle& style,
                                                           quint32 properties) {
    m_toolCommandWorkflow->setFilterStyleFromToolbar(style, properties);
}

void ScreenshotController::Impl::setTextStyleFromToolbar(const SnowCanvasTextStyle& style) {
    m_toolCommandWorkflow->setTextStyleFromToolbar(style);
}

void ScreenshotController::Impl::setSerialNumberStyleFromToolbar(
    const SnowCanvasSerialNumberStyle& style) {
    m_toolCommandWorkflow->setSerialNumberStyleFromToolbar(style);
}

void ScreenshotController::Impl::decrementSelectedSerialNumbers() {
    m_toolCommandWorkflow->decrementSelectedSerialNumbers();
}

void ScreenshotController::Impl::incrementSelectedSerialNumbers() {
    m_toolCommandWorkflow->incrementSelectedSerialNumbers();
}

void ScreenshotController::Impl::createTextForSelectedSerialNumber() {
    m_toolCommandWorkflow->createTextForSelectedSerialNumber();
}

void ScreenshotController::Impl::repositionToolbarForContentChange() {
    m_selectionEditWorkflow->repositionToolbarForContentChange();
}

void ScreenshotController::Impl::toggleSelectionAspectRatioLockFromToolbar() {
    m_selectionEditWorkflow->toggleSelectionAspectRatioLockFromToolbar();
}

void ScreenshotController::Impl::openSelectionResizeModalFromToolbar() {
    m_selectionEditWorkflow->openSelectionResizeModalFromToolbar();
}

void ScreenshotController::Impl::hideColorPickersForScreenshotUi() {
    m_selectionEditWorkflow->hideColorPickersForScreenshotUi();
}

QPoint
ScreenshotController::Impl::canvasColorPhysicalPositionAt(ScreenshotOverlayWindow* overlay,
                                                          const QPointF& localPosition) const {
    const QPointF canvasPosition =
        m_geometry.canvasPositionForOverlayLocalPoint(m_displaySession, overlay, localPosition);
    return m_geometry.physicalPositionForCanvasPoint(m_displaySession, canvasPosition);
}

QImage
ScreenshotController::Impl::canvasColorPreviewAtPhysicalPoint(ScreenshotOverlayWindow* overlay,
                                                              const QPoint& physicalPosition) {
    const CapturedDisplayModel* display = m_geometry.displayForOverlay(m_displaySession, overlay);
    SnowCanvasWidget* canvas = overlay != nullptr ? overlay->canvas() : nullptr;
    if (display == nullptr || canvas == nullptr ||
        !display->physicalRect.contains(physicalPosition) ||
        !m_canvasColorSampler.ensureSnapshot(*canvas, display->physicalRect)) {
        return {};
    }
    return m_canvasColorSampler.previewAtPhysicalPoint(physicalPosition);
}

void ScreenshotController::Impl::updateCanvasColorSamplingPreview(ScreenshotOverlayWindow* overlay,
                                                                  const QPointF& localPosition) {
    updateCanvasColorSamplingPreviewAtPhysicalPoint(
        overlay, canvasColorPhysicalPositionAt(overlay, localPosition));
}

void ScreenshotController::Impl::updateCanvasColorSamplingPreviewAtPhysicalPoint(
    ScreenshotOverlayWindow* overlay, const QPoint& physicalPosition) {
    if (m_canvasColorSamplerWindow == nullptr || m_canvasColorSamplingTarget.isNull()) {
        return;
    }
    const CapturedDisplayModel* display = m_geometry.displayForOverlay(m_displaySession, overlay);
    const QImage preview = canvasColorPreviewAtPhysicalPoint(overlay, physicalPosition);
    if (preview.isNull()) {
        return;
    }
    const QPoint globalLogicalPosition =
        display != nullptr
            ? m_geometry.logicalPositionForPhysicalPoint(*display, physicalPosition).toPoint()
            : QCursor::pos();
    m_canvasColorSamplerWindow->updateSample(preview, globalLogicalPosition);
}

void ScreenshotController::Impl::setCanvasColorSamplingCursor(bool enabled) {
    if (enabled && !m_canvasColorSamplingCursorOverridden) {
        QApplication::setOverrideCursor(ScreenshotCanvasColorSamplerWindow::samplingCursor());
        m_canvasColorSamplingCursorOverridden = true;
        return;
    }
    if (!enabled && m_canvasColorSamplingCursorOverridden) {
        QApplication::restoreOverrideCursor();
        m_canvasColorSamplingCursorOverridden = false;
    }
    if (!enabled && m_presentationServices != nullptr) {
        m_presentationServices->updateOverlayCursors();
    }
}

void ScreenshotController::Impl::setCanvasColorSamplingShortcutScope(bool enabled) {
    if (m_windowShortcutManager == nullptr || m_overlayCoordinator == nullptr) {
        return;
    }
    ScreenshotToolbarWindow* toolbar = m_overlayCoordinator->toolbar();
    if (toolbar == nullptr) {
        return;
    }
    if (enabled) {
        m_windowShortcutManager->addScopeWindow(toolbar);
    } else {
        m_windowShortcutManager->removeScopeWindow(toolbar);
    }
}

void ScreenshotController::Impl::clearCanvasColorSampling() {
    m_canvasColorSamplingTarget.clear();
    disconnect(m_canvasColorSamplingDestroyedConnection);
    m_canvasColorSamplingDestroyedConnection = {};
    m_canvasColorSampler.reset();
    if (m_overlayInputHandler != nullptr) {
        m_overlayInputHandler->cancelCanvasColorSampling();
    }
    if (m_canvasColorSamplerWindow != nullptr) {
        m_canvasColorSamplerWindow->endSampling();
    }
    setCanvasColorSamplingShortcutScope(false);
    setCanvasColorSamplingCursor(false);
}

void ScreenshotController::Impl::beginCanvasColorSampling(adqt::widgets::AdColorPicker* picker) {
    if (picker == nullptr || m_overlayInputHandler == nullptr || m_interaction.scrollingCapture()) {
        return;
    }

    clearCanvasColorSampling();
    m_canvasColorSamplingTarget = picker;
    m_canvasColorSamplingDestroyedConnection = QObject::connect(
        picker, &QObject::destroyed, this, [this]() { clearCanvasColorSampling(); });
    if (m_canvasColorSamplerWindow != nullptr) {
        m_canvasColorSamplerWindow->beginSampling();
    }
    m_canvasColorSampler.reset();
    setCanvasColorSamplingShortcutScope(true);
    setCanvasColorSamplingCursor(true);
    m_overlayInputHandler->armCanvasColorSampling();
    const std::optional<QPoint> physicalPosition =
        m_physicalCursor != nullptr ? m_physicalCursor->position() : std::nullopt;
    if (physicalPosition.has_value()) {
        const CapturedDisplayModel* display =
            m_geometry.displayForPhysicalPoint(m_displaySession, *physicalPosition);
        if (ScreenshotOverlayWindow* overlay = m_displaySession.overlayForDisplay(display)) {
            updateCanvasColorSamplingPreviewAtPhysicalPoint(overlay, *physicalPosition);
        }
    } else if (ScreenshotOverlayWindow* overlay = overlayUnderCursor()) {
        updateCanvasColorSamplingPreview(overlay, overlay->mapFromGlobal(QCursor::pos()));
    }
}

void ScreenshotController::Impl::adjustSelectionFromToolbar(int minDx, int minDy, int maxDx,
                                                            int maxDy) {
    m_selectionEditWorkflow->adjustSelectionFromToolbar(minDx, minDy, maxDx, maxDy);
}

void ScreenshotController::Impl::setSelectionCornerRadiusFromToolbar(int radius) {
    m_selectionEditWorkflow->setSelectionCornerRadiusFromToolbar(radius);
}

void ScreenshotController::Impl::setSelectionShadowWidthFromToolbar(int shadowWidth) {
    m_selectionEditWorkflow->setSelectionShadowWidthFromToolbar(shadowWidth);
}

ScreenshotController::Impl::~Impl() {
    shutdown();
}

void ScreenshotController::Impl::invalidateDelayedCapture() {
    m_delayedCaptureGeneration = owner.nextOperationGeneration();
}

void ScreenshotController::Impl::resetPendingCaptureRequest() {
    invalidateDelayedCapture();
    m_pendingSelectionAction = PendingSelectionAction::None;
    m_pendingHistorySource = snow_shot::storage::CaptureHistorySource::CopiedToClipboard;
    m_automaticSelectionMode = AutomaticSelectionMode::None;
    m_automaticPhysicalPoint = QPoint();
    m_focusedWindowCapture.reset();
    m_pendingOcrFromQuickFunction = false;
    m_ocrTranslateAfterRecognition = false;
}

bool ScreenshotController::Impl::canBeginCapture() const {
    if (m_imageExportInFlight || m_captureState.captureInProgress || !m_interaction.inactive() ||
        (m_captureState.sessionState != ScreenshotSessionState::IdleCold &&
         m_captureState.sessionState != ScreenshotSessionState::IdlePrepared)) {
        return false;
    }
    return m_captureWorkflow != nullptr && m_ocrController != nullptr;
}

bool ScreenshotController::Impl::canReleaseAfterCancel() const {
    return !m_captureState.captureInProgress && m_interaction.inactive() &&
           m_captureState.sessionState == ScreenshotSessionState::IdleCold &&
           !m_imageExportInFlight && !m_clipboardPinJob.isValid() &&
           (m_screenRecordingController == nullptr || !m_screenRecordingController->isOpen());
}

bool ScreenshotController::Impl::beginCapture(
    PendingSelectionAction action, snow_shot::storage::CaptureHistorySource historySource,
    AutomaticSelectionMode automaticMode, const QPoint& automaticPhysicalPoint,
    quintptr focusedWindowHandle) {
    if (!canBeginCapture()) {
        return false;
    }

    clearCanvasColorSampling();
    if (m_overlayInputHandler != nullptr) {
        m_overlayInputHandler->resetTransientShortcuts();
    }
    invalidateDelayedCapture();
    m_exportJob.cancel();
    m_exportJob = {};
    m_backgroundSaveJob.cancel();
    m_backgroundSaveJob = {};
    m_clipboardCommit.cancel();
    m_clipboardCommit = {};
    if (m_selectionExportUiServices != nullptr) {
        m_selectionExportUiServices->cancelClipboardPublication();
    }
    m_imageExportGeneration = owner.nextOperationGeneration();
    m_imageExportInFlight = false;
    m_pendingHistoryEditRecordId.clear();
    m_pendingSelectionAction = action;
    m_pendingOcrFromQuickFunction = action == PendingSelectionAction::RecognizeText;
    m_pendingHistorySource = historySource;
    m_automaticSelectionMode = automaticMode;
    m_automaticPhysicalPoint = automaticPhysicalPoint;
    if (automaticMode != AutomaticSelectionMode::FocusedWindow) {
        m_focusedWindowCapture.reset();
    }
    m_ocrController->invalidateSession();
    static_cast<void>(stopScrollingCapture(false));
    if (m_historyService != nullptr) {
        m_historyService->resetCaptureNavigation();
    }
    m_captureWorkflow->startCapture(automaticMode == AutomaticSelectionMode::None
                                        ? ScreenshotCapturePresentationMode::Overlay
                                        : ScreenshotCapturePresentationMode::Silent,
                                    focusedWindowHandle);
    return true;
}

bool ScreenshotController::Impl::selectPreviousSelection() {
    if (!m_selectionSettings || !m_selectionSettings->hasPreviousSelectionParams() ||
        !m_interaction.moveToolActive()) {
        return false;
    }

    const QRectF canvasBounds = m_geometry.canvasBounds();
    if (canvasBounds.isNull() || canvasBounds.isEmpty()) {
        return false;
    }
    const QRect bounds = ScreenshotHalfOpenRect::fromRectF(canvasBounds).toAlignedQRect();
    if (!m_selection.applyParams(m_selectionSettings->previousSelectionParams(), bounds)) {
        return false;
    }

    m_intelligentSelection.clearTransientState();
    m_interaction.confirmSelection();
    m_captureState.sessionState = ScreenshotSessionState::Editing;
    if (m_presentationServices != nullptr) {
        m_presentationServices->updateOverlayState();
        m_presentationServices->showToolbar();
        m_presentationServices->showSelectionToolbar();
    }
    if (m_colorPickerController != nullptr && m_presentationServices != nullptr) {
        m_colorPickerController->updateAtCurrentCursor(
            m_presentationServices->colorPickerContext());
    }
    return true;
}

void ScreenshotController::Impl::handleSelectionConfirmed() {
    const PendingSelectionAction action =
        std::exchange(m_pendingSelectionAction, PendingSelectionAction::None);
    const snow_shot::storage::CaptureHistorySource source = m_pendingHistorySource;
    m_pendingHistorySource = snow_shot::storage::CaptureHistorySource::CopiedToClipboard;
    QImage directSourceImage;
    if (source == snow_shot::storage::CaptureHistorySource::FocusedWindow &&
        m_focusedWindowCapture.has_value()) {
        directSourceImage = m_focusedWindowCapture->image;
    }
    if (action == PendingSelectionAction::None) {
        return;
    }

    const quint64 sessionId = m_captureState.sessionId;
    QTimer::singleShot(0, this,
                       [this, action, source, sessionId,
                        directSourceImage = std::move(directSourceImage)]() mutable {
                           if (m_captureState.sessionId != sessionId ||
                               m_captureState.sessionState != ScreenshotSessionState::Editing) {
                               return;
                           }
                           switch (action) {
                           case PendingSelectionAction::Pin:
                               pinSelectionToScreen();
                               break;
                           case PendingSelectionAction::RecognizeText:
                               m_activatingQuickOcr = m_pendingOcrFromQuickFunction;
                               m_pendingOcrFromQuickFunction = false;
                               setOcrTool();
                               m_activatingQuickOcr = false;
                               break;
                           case PendingSelectionAction::RecognizeTextTranslation:
                               m_pendingOcrFromQuickFunction = false;
                               setTextTranslationTool();
                               break;
                           case PendingSelectionAction::Copy:
                               if (!directSourceImage.isNull() && m_exportService != nullptr) {
                                   m_exportService->setNextSelectionSourceImage(
                                       std::move(directSourceImage));
                               }
                               copySelectionToClipboardWithSource(source);
                               if (m_exportService != nullptr) {
                                   m_exportService->clearNextSelectionSourceImage();
                               }
                               break;
                           case PendingSelectionAction::StartVideo:
                               startScreenRecording();
                               break;
                           case PendingSelectionAction::None:
                               break;
                           }
                       });
}

void ScreenshotController::Impl::executeAutomaticSelection() {
    const AutomaticSelectionMode mode =
        std::exchange(m_automaticSelectionMode, AutomaticSelectionMode::None);
    if (mode == AutomaticSelectionMode::None) {
        return;
    }

    const CapturedDisplayModel* display = nullptr;
    if (mode == AutomaticSelectionMode::CurrentMonitor) {
        display = m_geometry.displayForPhysicalPoint(m_displaySession, m_automaticPhysicalPoint);
    }

    QRectF selection;
    if (mode == AutomaticSelectionMode::FocusedWindow && m_focusedWindowCapture.has_value()) {
        selection = m_geometry.canvasRectForPhysicalRect(m_displaySession,
                                                         m_focusedWindowCapture->physicalRect);
    } else if (display != nullptr) {
        selection = display->canvasRect;
    }
    if (!selection.isValid() || selection.isEmpty()) {
        m_automaticSelectionMode = AutomaticSelectionMode::None;
        m_focusedWindowCapture.reset();
        cancelCapture();
        return;
    }
    m_selection.setSelectionRect(selection);
    m_automaticSelectionMode = AutomaticSelectionMode::None;
    playCameraShutterSound();
    m_interaction.confirmSelection();
    m_captureState.sessionState = ScreenshotSessionState::Editing;
    m_intelligentSelection.clearPress();
    handleSelectionConfirmed();
    m_focusedWindowCapture.reset();
}

void ScreenshotController::Impl::shutdown() {
    clearCanvasColorSampling();
    if (m_overlayInputHandler != nullptr) {
        m_overlayInputHandler->resetTransientShortcuts();
    }
    m_exportJob.cancel();
    m_exportJob = {};
    m_backgroundSaveJob.cancel();
    m_backgroundSaveJob = {};
    m_clipboardCommit.cancel();
    m_clipboardCommit = {};
    if (m_selectionExportUiServices != nullptr) {
        m_selectionExportUiServices->cancelClipboardPublication();
    }
    m_clipboardPinJob.cancel();
    m_clipboardPinJob = {};
    m_clipboardPinGeneration = owner.nextOperationGeneration();
    m_imageExportGeneration = owner.nextOperationGeneration();
    m_imageExportInFlight = false;
    resetPendingCaptureRequest();
    m_exportService.reset();
    m_ocrController.reset();
    static_cast<void>(stopScrollingCapture(false));
    if (m_captureWorkflow != nullptr) {
        m_captureWorkflow->cancelCapture();
        m_captureWorkflow->destroyDisplayPool();
        m_captureWorkflow->shutdownCaptureWorker();
        m_captureWorkflow->destroyUiSelectorService();
    }
    if (m_historyService != nullptr) {
        m_historyService->resetCaptureNavigation();
        m_historyService->drainPendingWrites();
    }
    if (m_selectorCoordinator != nullptr) {
        QObject::disconnect(m_selectorCoordinator, nullptr, this, nullptr);
    }
    if (m_overlayEventAdapter != nullptr) {
        m_overlayEventAdapter->clearEventTargets();
    }
    m_overlayInputHandler.reset();
    m_scrollingCaptureController.reset();
    m_displayConfigurationObserver.reset();
    m_historyService.reset();
    m_captureWorkflow.reset();
    m_captureRuntime.reset();
    m_selectionEditWorkflow.reset();
    m_toolCommandWorkflow.reset();
    m_selectorWorkflow.reset();
    delete m_selectorCoordinator;
    m_selectorCoordinator = nullptr;
    m_presentationServices.reset();
    m_colorPickerController.reset();
    m_toolbarPresenter.reset();
    m_selectionResizeWorkflow.reset();
    m_selectionExportWorkflow.reset();
    m_selectionExportUiServices.reset();
    m_selectionSettings.reset();
    m_screenRecordingController.reset();
    // Every conversion-producing capture worker is joined before releasing the shared pool.
    snow_capture_release_conversion_pool();
    m_overlayCoordinator.reset();
    m_overlayEventAdapter.reset();
}

ScreenshotController::ScreenshotController(QObject* parent) : QObject(parent) {}

ScreenshotController::~ScreenshotController() = default;

ScreenshotController::Impl& ScreenshotController::ensureImpl() {
    if (m_impl == nullptr) {
        m_impl = std::make_unique<Impl>(*this);
    }
    return *m_impl;
}

quint64 ScreenshotController::nextOperationGeneration() {
    return ++m_operationGeneration;
}

void ScreenshotController::scheduleIdleImplementationRelease(Impl* implementation) {
    const QPointer<Impl> guardedImplementation(implementation);
    QTimer::singleShot(0, this, [this, guardedImplementation]() {
        if (guardedImplementation.isNull() || m_impl.get() != guardedImplementation.data() ||
            !guardedImplementation->canReleaseAfterCancel()) {
            return;
        }
        m_impl.reset();
    });
}

void ScreenshotController::setUiPreferences(const ScreenshotUiPreferences& preferences) {
    ensureImpl().applyUiPreferences(preferences);
}

void ScreenshotController::prewarmResources() {
    QTimer::singleShot(0, this, [this]() { ensureImpl().m_captureWorkflow->prewarmResources(); });
}

void ScreenshotController::startCapture() {
    static_cast<void>(ensureImpl().beginCapture());
}

void ScreenshotController::startDelayedCapture(int delaySeconds) {
    Impl& implementation = ensureImpl();
    if (!implementation.canBeginCapture()) {
        return;
    }
    const int seconds = std::clamp(delaySeconds, 1, 10);
    implementation.m_delayedCaptureGeneration = nextOperationGeneration();
    const quint64 generation = implementation.m_delayedCaptureGeneration;
    Impl* expectedImplementation = &implementation;
    QTimer::singleShot(seconds * 1000, this, [this, expectedImplementation, generation]() {
        if (m_impl.get() != expectedImplementation ||
            generation != expectedImplementation->m_delayedCaptureGeneration ||
            !expectedImplementation->canBeginCapture()) {
            return;
        }
        startCapture();
    });
}

void ScreenshotController::captureAndPinSelection() {
    static_cast<void>(ensureImpl().beginCapture(Impl::PendingSelectionAction::Pin));
}

void ScreenshotController::captureAndRecognizeText() {
    static_cast<void>(ensureImpl().beginCapture(Impl::PendingSelectionAction::RecognizeText));
}

void ScreenshotController::captureAndTranslateText() {
    static_cast<void>(
        ensureImpl().beginCapture(Impl::PendingSelectionAction::RecognizeTextTranslation));
}

void ScreenshotController::captureAndCopySelection() {
    static_cast<void>(ensureImpl().beginCapture(Impl::PendingSelectionAction::Copy));
}

void ScreenshotController::captureCurrentMonitor() {
    Impl& implementation = ensureImpl();
    if (!implementation.canBeginCapture()) {
        return;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    POINT nativeCursor{};
    if (GetCursorPos(&nativeCursor) == FALSE) {
        qWarning("Failed to query the physical cursor position for current-monitor capture");
        return;
    }
    const QPoint cursorPosition(nativeCursor.x, nativeCursor.y);
#else
    const QPoint cursorPosition = QCursor::pos();
#endif
    static_cast<void>(implementation.beginCapture(
        Impl::PendingSelectionAction::Copy,
        snow_shot::storage::CaptureHistorySource::CurrentMonitor,
        Impl::AutomaticSelectionMode::CurrentMonitor, cursorPosition));
}

void ScreenshotController::captureFocusedWindow() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    Impl& implementation = ensureImpl();
    if (!implementation.canBeginCapture()) {
        return;
    }
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return;
    }
    const HWND root = GetAncestor(foreground, GA_ROOT);
    const HWND target = root != nullptr ? root : foreground;

    implementation.m_focusedWindowCapture.reset();
    static_cast<void>(implementation.beginCapture(
        Impl::PendingSelectionAction::Copy, snow_shot::storage::CaptureHistorySource::FocusedWindow,
        Impl::AutomaticSelectionMode::FocusedWindow, QPoint(), reinterpret_cast<quintptr>(target)));
#else
    Q_UNUSED(this);
#endif
}

void ScreenshotController::captureAndStartScreenRecording() {
    static_cast<void>(ensureImpl().beginCapture(Impl::PendingSelectionAction::StartVideo));
}

void ScreenshotController::startOrStopScreenRecordingAndCopy() {
    Impl& implementation = ensureImpl();
    if (implementation.m_screenRecordingController == nullptr) {
        return;
    }
    if (!implementation.m_screenRecordingController->isOpen()) {
        captureAndStartScreenRecording();
    } else if (!implementation.m_screenRecordingController->isRecording()) {
        implementation.m_screenRecordingController->startRecording();
    } else {
        implementation.m_screenRecordingController->stopRecordingAndCopyVideo();
    }
}

void ScreenshotController::editHistoryRecord(const QString& recordId) {
    Impl& implementation = ensureImpl();
    implementation.m_imageExportGeneration = nextOperationGeneration();
    implementation.m_imageExportInFlight = false;
    implementation.startHistoryEdit(recordId);
}

void ScreenshotController::cancelCapture() {
    if (m_impl != nullptr) {
        m_impl->cancelCapture();
    }
}

void ScreenshotController::copySelectionToClipboard() {
    ensureImpl().copySelectionToClipboard();
}

void ScreenshotController::pinSelectionToScreen() {
    ensureImpl().pinSelectionToScreen();
}

void ScreenshotController::pinClipboardContentToScreen() {
    ensureImpl().pinClipboardContentToScreen();
}

void ScreenshotController::startScreenRecording() {
    ensureImpl().startScreenRecording();
}

void ScreenshotController::setMoveTool() {
    ensureImpl().setMoveTool();
}

void ScreenshotController::setSelectTool() {
    ensureImpl().setSelectTool();
}

void ScreenshotController::setShapeTool() {
    ensureImpl().setShapeTool();
}

void ScreenshotController::setArrowTool() {
    ensureImpl().setArrowTool();
}

void ScreenshotController::setLineTool() {
    ensureImpl().setLineTool();
}

void ScreenshotController::setFreeDrawTool() {
    ensureImpl().setFreeDrawTool();
}

void ScreenshotController::setHighlightTool() {
    ensureImpl().setHighlightTool();
}

void ScreenshotController::setPenHighlightTool() {
    ensureImpl().setPenHighlightTool();
}

void ScreenshotController::Impl::setSelectionToolbarHovered(bool hovered) {
    if (m_presentationServices != nullptr) {
        m_presentationServices->setSelectionToolbarHovered(hovered);
    }
}

void ScreenshotController::setSpotlightTool() {
    ensureImpl().setSpotlightTool();
}

void ScreenshotController::setEraserTool() {
    ensureImpl().setEraserTool();
}

void ScreenshotController::setFilterTool() {
    ensureImpl().setFilterTool();
}

void ScreenshotController::setRectangleFilterTool() {
    ensureImpl().setRectangleFilterTool();
}

void ScreenshotController::setPenFilterTool() {
    ensureImpl().setPenFilterTool();
}

void ScreenshotController::setWatermarkTool() {
    ensureImpl().setWatermarkTool();
}

void ScreenshotController::setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig& config) {
    ensureImpl().setWatermarkConfigFromToolbar(config);
}

void ScreenshotController::setSpotlightConfigFromToolbar(const SnowCanvasSpotlightConfig& config) {
    ensureImpl().setSpotlightConfigFromToolbar(config);
}

void ScreenshotController::setTextTool() {
    ensureImpl().setTextTool();
}

void ScreenshotController::setSerialNumberTool() {
    ensureImpl().setSerialNumberTool();
}

void ScreenshotController::decrementSelectedSerialNumbers() {
    ensureImpl().decrementSelectedSerialNumbers();
}

void ScreenshotController::incrementSelectedSerialNumbers() {
    ensureImpl().incrementSelectedSerialNumbers();
}

void ScreenshotController::createTextForSelectedSerialNumber() {
    ensureImpl().createTextForSelectedSerialNumber();
}

SnowCanvasShapeStyle ScreenshotController::currentRectangleStyle() const {
    const Impl& implementation = const_cast<ScreenshotController*>(this)->ensureImpl();
    return implementation.m_toolCommandWorkflow->currentRectangleStyle();
}

void ScreenshotController::setShapeStyleFromToolbar(const SnowCanvasShapeStyle& style,
                                                    quint32 properties, SnowCanvasShapeKind kind) {
    ensureImpl().setShapeStyleFromToolbar(style, properties, kind);
}

void ScreenshotController::Impl::reorderSelectedElements(SnowCanvasSelectionOrder order) {
    m_overlayCoordinator->reorderSelectedElements(m_displaySession, order);
}

void ScreenshotController::Impl::setSelectedElementsOpacity(qreal opacity) {
    m_overlayCoordinator->setSelectedElementsOpacity(m_displaySession, opacity);
}

void ScreenshotController::Impl::duplicateSelectedElements() {
    m_overlayCoordinator->duplicateSelectedElements(m_displaySession);
}

void ScreenshotController::Impl::deleteSelectedElements() {
    m_overlayCoordinator->deleteSelectedElements(m_displaySession);
}

void ScreenshotController::setTextStyleFromToolbar(const SnowCanvasTextStyle& style) {
    ensureImpl().setTextStyleFromToolbar(style);
}

void ScreenshotController::setSerialNumberStyleFromToolbar(
    const SnowCanvasSerialNumberStyle& style) {
    ensureImpl().setSerialNumberStyleFromToolbar(style);
}

void ScreenshotController::adjustSelectionFromToolbar(int minDx, int minDy, int maxDx, int maxDy) {
    ensureImpl().adjustSelectionFromToolbar(minDx, minDy, maxDx, maxDy);
}

void ScreenshotController::setSelectionCornerRadiusFromToolbar(int radius) {
    ensureImpl().setSelectionCornerRadiusFromToolbar(radius);
}

void ScreenshotController::setSelectionShadowWidthFromToolbar(int shadowWidth) {
    ensureImpl().setSelectionShadowWidthFromToolbar(shadowWidth);
}

void ScreenshotController::toggleSelectionAspectRatioLockFromToolbar() {
    ensureImpl().toggleSelectionAspectRatioLockFromToolbar();
}

void ScreenshotController::openSelectionResizeModalFromToolbar() {
    ensureImpl().openSelectionResizeModalFromToolbar();
}

void ScreenshotController::repositionToolbarForContentChange() {
    ensureImpl().repositionToolbarForContentChange();
}

void ScreenshotController::hideColorPickersForScreenshotUi() {
    ensureImpl().hideColorPickersForScreenshotUi();
}
