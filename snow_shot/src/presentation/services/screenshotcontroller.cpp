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
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
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
#include "snow_shot/presentation/screenshotuipreferences.h"
#include "snow_shot/presentation/screenshotscrollingcapturecontroller.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlayinteractionadapter.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotoverlayshortcutcontroller.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotpresentationservices.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
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
#include <QScopedValueRollback>
#include <QSet>
#include <QUrl>
#include <QMimeData>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <QHash>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

template <typename Function> class ScopeExit final {
  public:
    explicit ScopeExit(Function function) : m_function(std::move(function)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&& other) noexcept
        : m_function(std::move(other.m_function)), m_active(std::exchange(other.m_active, false)) {}
    ~ScopeExit() {
        if (m_active) {
            m_function();
        }
    }
  private:
    Function m_function;
    bool m_active = true;
};

template <typename Function> ScopeExit<Function> makeScopeExit(Function function) {
    return ScopeExit<Function>(std::move(function));
}

QString resolvedOcrProxyUrl(const QString& proxyMode) {
    if (proxyMode != QStringLiteral("system")) {
        return {};
    }
    const QNetworkProxyQuery query(QUrl(QStringLiteral("https://www.modelscope.cn/")));
    const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
    for (const QNetworkProxy& proxy : proxies) {
        if (proxy.type() != QNetworkProxy::HttpProxy &&
            proxy.type() != QNetworkProxy::Socks5Proxy) {
            continue;
        }
        QUrl url;
        url.setScheme(proxy.type() == QNetworkProxy::Socks5Proxy ? QStringLiteral("socks5")
                                                                  : QStringLiteral("http"));
        url.setHost(proxy.hostName());
        url.setPort(proxy.port());
        if (!proxy.user().isEmpty()) {
            url.setUserName(proxy.user());
        }
        if (!proxy.password().isEmpty()) {
            url.setPassword(proxy.password());
        }
        return url.toString(QUrl::FullyEncoded);
    }
    return {};
}

struct ScrollingPinMaterializationState final {
    QPointer<ScreenshotController> controller;
    QPointer<QObject> windowReceiver;
    std::optional<ScreenshotScrollingSnapshot> snapshot;
    ScreenshotImageLoadCallback callback;
    ScreenshotExportJobHandle job;
    quint64 generation = 0;
    bool snapshotFailed = false;
    bool loadingStarted = false;
};

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

struct ScreenshotController::Impl final : public ScreenshotToolbarCommandSink,
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

    enum class ExportDetachMode {
        Immediate,
        DeferredPresentation,
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
    [[nodiscard]] bool ensureRecognitionFeature();
    [[nodiscard]] bool ensureScrollingFeature();
    [[nodiscard]] bool ensureRecordingFeature();
    [[nodiscard]] bool ensureExportFeature();
    [[nodiscard]] bool ensureCanvasSamplingUi();
    void deactivateRecognition();
    void invalidateRecognitionSession();
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
    [[nodiscard]] ScreenshotOverlayWindow* overlayUnderCursor() const;
    [[nodiscard]] ScreenshotOverlayWindow* overlayForWidget(QWidget* widget) const;
    [[nodiscard]] ScreenshotOverlayWindow* keyboardOwnerOverlay() const;
    void rememberKeyboardOwner(QWidget* widget = nullptr);
    void restoreKeyboardOwnerQueued(ScreenshotOverlayWindow* overlay);
    void setHistoryLoadingMessageVisible(bool visible);
    [[nodiscard]] bool stopScrollingCapture(bool restoreScreenshotPresentation);
    void pauseScrollingCaptureForSelectionResize();
    void resumeScrollingCaptureAfterSelectionResize();
    [[nodiscard]] bool activateToolForSelectionResize(ScreenshotActiveTool tool);
    void activateRecognitionToolAfterSelectionResize(ScreenshotActiveTool tool);
    [[nodiscard]] std::optional<quint64> beginImageExport();
    [[nodiscard]] bool imageExportCurrent(quint64 generation) const;
    [[nodiscard]] bool imageExportNotificationCurrent(quint64 generation) const;
    [[nodiscard]] bool finishImageExport(quint64 generation);
    void hideImageExportPresentation();
    void detachCaptureForExport(ExportDetachMode mode = ExportDetachMode::Immediate);
    void scheduleDeferredExportCleanup();
    void trackExportJob(const ScreenshotExportJobHandle& handle);
    void trackClipboardCommit(const ScreenshotClipboardCommitHandle& handle);
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
    QPointer<ScreenshotOverlayWindow> m_keyboardOwnerOverlay;
    QPointer<adqt::widgets::AdColorPicker> m_canvasColorSamplingTarget;
    QMetaObject::Connection m_canvasColorSamplingDestroyedConnection;
    bool m_canvasColorSamplingCursorOverridden = false;
    QString m_pendingHistoryEditRecordId;
    quint64 m_imageExportGeneration = 0;
    QSet<quint64> m_activeImageExports;
    QHash<quint64, quint64> m_imageExportCaptureEpochs;
    quint64 m_captureEpoch = 0;
    ScreenshotExportJobHandle m_exportJob;
    ScreenshotExportJobHandle m_backgroundSaveJob;
    ScreenshotClipboardCommitHandle m_clipboardCommit;
    std::vector<ScreenshotExportJobHandle> m_exportJobs;
    std::vector<ScreenshotClipboardCommitHandle> m_clipboardCommits;
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
    bool m_constructingRecognitionFeature = false;
    bool m_constructingScrollingFeature = false;
    bool m_constructingRecordingFeature = false;
    bool m_constructingExportFeature = false;
    bool m_constructingCanvasSamplingUi = false;
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
            &storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged, &owner,
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
                } else if (key == QStringLiteral("text_recognition/direct_ml_acceleration") &&
                           m_ocrRecognition != nullptr) {
                    const auto preference =
                        value.toBool() ? ScreenshotOcrBackendPreference::DirectMl
                                       : ScreenshotOcrBackendPreference::Cpu;
                    m_ocrRecognition->setBackendPreference(preference);
                } else if (key == QStringLiteral("network/proxy")) {
                    if (m_tableRecognition != nullptr) {
                        m_tableRecognition->setUseSystemProxy(
                            value.toString() == QStringLiteral("system"));
                    }
                    if (m_ocrRecognition != nullptr) {
                        m_ocrRecognition->setProxyUrl(resolvedOcrProxyUrl(value.toString()));
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

ScreenshotOverlayWindow* ScreenshotController::Impl::overlayForWidget(QWidget* widget) const {
    if (widget == nullptr) {
        return nullptr;
    }

    QWidget* widgetWindow = widget->window();
    QWindow* widgetHandle = widgetWindow != nullptr ? widgetWindow->windowHandle() : nullptr;
    ScreenshotOverlayWindow* result = nullptr;
    m_displaySession.forEachActiveOverlay(
        [widget, widgetWindow, widgetHandle, &result](
            qsizetype, const CapturedDisplayModel&, ScreenshotOverlayWindow* overlay) {
            if (result != nullptr || overlay == nullptr) {
                return;
            }
            if (widget == overlay || overlay->isAncestorOf(widget) || widgetWindow == overlay) {
                result = overlay;
                return;
            }

            QWindow* candidate = widgetHandle;
            QSet<QWindow*> visited;
            while (candidate != nullptr && !visited.contains(candidate)) {
                visited.insert(candidate);
                if (candidate == overlay->windowHandle()) {
                    result = overlay;
                    return;
                }
                candidate = candidate->transientParent();
            }

            // Before a QtTool surface has a native handle, Qt retains the
            // QObject parent used by setParent(owner, Qt::Tool).
            for (QWidget* parent = widgetWindow != nullptr ? widgetWindow->parentWidget()
                                                            : nullptr;
                 parent != nullptr; parent = parent->parentWidget()) {
                if (parent == overlay || parent->window() == overlay) {
                    result = overlay;
                    return;
                }
            }
        });
    return result;
}

ScreenshotOverlayWindow* ScreenshotController::Impl::keyboardOwnerOverlay() const {
    if (m_keyboardOwnerOverlay != nullptr && m_keyboardOwnerOverlay->isVisible()) {
        return m_keyboardOwnerOverlay.data();
    }
    if (ScreenshotOverlayWindow* focused = overlayForWidget(QApplication::focusWidget())) {
        return focused;
    }
    return overlayUnderCursor();
}

void ScreenshotController::Impl::rememberKeyboardOwner(QWidget* widget) {
    ScreenshotOverlayWindow* overlay = overlayForWidget(widget);
    if (overlay != nullptr && overlay->isVisible()) {
        m_keyboardOwnerOverlay = overlay;
    }
}

void ScreenshotController::Impl::restoreKeyboardOwnerQueued(ScreenshotOverlayWindow* overlay) {
    QPointer<ScreenshotOverlayWindow> target(overlay != nullptr ? overlay
                                                                   : keyboardOwnerOverlay());
    if (target == nullptr) {
        return;
    }
    m_keyboardOwnerOverlay = target;
    const QPointer<ScreenshotController> controller(&owner);
    QTimer::singleShot(0, &owner, [target, controller]() {
        if (!controller || target == nullptr || !target->isVisible()) {
            return;
        }
        target->raise();
        target->activateWindow();
        if (target->canvas() != nullptr) {
            target->canvas()->setFocus(Qt::OtherFocusReason);
        }
        target->commitInitialSelectionCursor();
        QTimer::singleShot(0, target, [target, controller]() {
            if (!controller || target == nullptr || !target->isVisible()) {
                return;
            }
            target->activateWindow();
            if (target->canvas() != nullptr) {
                target->canvas()->setFocus(Qt::OtherFocusReason);
            }
        });
    });
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
    QObject::connect(qApp, &QApplication::focusChanged, &owner,
                     [this](QWidget*, QWidget* nowFocused) {
                         rememberKeyboardOwner(nowFocused);
                     });
    m_messages = std::make_unique<ScreenshotMessageService>(
        m_displaySession, m_geometry, m_selection, [this]() {
            return m_overlayCoordinator != nullptr ? m_overlayCoordinator->toolbar() : nullptr;
        });
    m_physicalCursor = std::make_unique<snow_shot::platform::PhysicalCursor>();
    m_colorPickerController = std::make_unique<ScreenshotColorPickerController>(
        *m_overlayCoordinator, m_geometry, m_displaySession, *m_physicalCursor);
    m_toolbarPresenter = std::make_unique<ScreenshotToolbarPresenter>(*m_overlayCoordinator,
                                                                      m_geometry, m_displaySession);
    m_selectorCoordinator = new ScreenshotSelectorCoordinator(&owner);
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
}

bool ScreenshotController::Impl::ensureRecognitionFeature() {
    if (m_ocrController != nullptr) {
        return true;
    }
    if (m_constructingRecognitionFeature || m_overlayCoordinator == nullptr ||
        m_colorPickerController == nullptr) {
        return false;
    }

    const QScopedValueRollback<bool> constructingGuard(m_constructingRecognitionFeature, true);
    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    const auto backendPreference =
        applicationStorage.configuration()
                .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                .toBool()
            ? ScreenshotOcrBackendPreference::DirectMl
            : ScreenshotOcrBackendPreference::Cpu;
    ScreenshotOcrRecognitionService::Options ocrOptions;
    ocrOptions.offlineRoot = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("assets/ocr"));
    if (applicationStorage.isInitialized() &&
        !applicationStorage.configurationDirectory().trimmed().isEmpty()) {
        ocrOptions.cacheRoot = QDir(applicationStorage.configurationDirectory())
                                   .filePath(QStringLiteral("assets/ocr"));
    }
    if (applicationStorage.isInitialized()) {
        ocrOptions.proxyUrl = resolvedOcrProxyUrl(
            applicationStorage.configuration().value(QStringLiteral("network/proxy")).toString());
    }
    m_ocrRecognition = std::make_unique<ScreenshotOcrRecognitionService>(
        ocrOptions, backendPreference, &owner);
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
            m_windowShortcutManager.get(),
        },
        &owner);
    QObject::connect(m_ocrController.get(), &ScreenshotOcrController::textResultChanged, &owner,
                     [this](bool available) { handleAutomaticTextRecognitionAction(available); });
    return true;
}

bool ScreenshotController::Impl::ensureScrollingFeature() {
    if (m_scrollingCaptureController != nullptr) {
        return true;
    }
    if (m_constructingScrollingFeature || m_overlayCoordinator == nullptr) {
        return false;
    }
    const QScopedValueRollback<bool> constructingGuard(m_constructingScrollingFeature, true);
    m_scrollingCaptureController = std::make_unique<ScreenshotScrollingCaptureController>(
        ScreenshotScrollingCaptureControllerContext{
            m_displaySession,
            m_geometry,
            *m_overlayCoordinator,
        },
        &owner);
    return m_scrollingCaptureController != nullptr;
}

bool ScreenshotController::Impl::ensureRecordingFeature() {
    if (m_screenRecordingController != nullptr) {
        return true;
    }
    if (m_constructingRecordingFeature) {
        return false;
    }
    const QScopedValueRollback<bool> constructingGuard(m_constructingRecordingFeature, true);
    m_screenRecordingController = std::make_unique<ScreenRecordingController>(&owner);
    return m_screenRecordingController != nullptr;
}

bool ScreenshotController::Impl::ensureCanvasSamplingUi() {
    if (m_canvasColorSamplerWindow != nullptr) {
        return true;
    }
    if (m_constructingCanvasSamplingUi) {
        return false;
    }
    const QScopedValueRollback<bool> constructingGuard(m_constructingCanvasSamplingUi, true);
    m_canvasColorSamplerWindow = std::make_unique<ScreenshotCanvasColorSamplerWindow>();
    return m_canvasColorSamplerWindow != nullptr;
}

void ScreenshotController::Impl::deactivateRecognition() {
    if (m_ocrController == nullptr) {
        return;
    }
    const bool wasActive = m_ocrController->active();
    m_ocrController->deactivate();
    if (wasActive) {
        restoreKeyboardOwnerQueued(nullptr);
    }
}

void ScreenshotController::Impl::invalidateRecognitionSession() {
    if (m_ocrController != nullptr) {
        m_ocrController->invalidateSession();
    }
}

void ScreenshotController::Impl::createSelectionWorkflows() {
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

bool ScreenshotController::Impl::ensureExportFeature() {
    if (m_exportService != nullptr && m_selectionExportUiServices != nullptr &&
        m_selectionExportWorkflow != nullptr) {
        return true;
    }
    if (m_constructingExportFeature || m_selectionSettings == nullptr) {
        return false;
    }

    const QScopedValueRollback<bool> constructingGuard(m_constructingExportFeature, true);
    auto exportService = std::make_unique<ScreenshotExportService>(ScreenshotExportServiceContext{
        m_displaySession,
        m_canvasRuntime,
        m_geometry,
    });
    auto exportUiServices = std::make_unique<ScreenshotSelectionExportUiServices>(
        m_ocrRecognition.get(), m_qrRecognition.get(), m_tableRecognition.get(),
        [controller = QPointer<ScreenshotController>(&owner)]() {
            if (controller != nullptr) {
                emit controller->showMainWindowRequested();
            }
        },
        [controller = QPointer<ScreenshotController>(&owner)]() {
            ScreenshotPinnedRecognitionProviders providers;
            if (controller == nullptr || controller->m_impl == nullptr ||
                !controller->m_impl->ensureRecognitionFeature()) {
                return providers;
            }
            providers.recognition = controller->m_impl->m_ocrRecognition.get();
            providers.qrRecognition = controller->m_impl->m_qrRecognition.get();
            providers.tableRecognition = controller->m_impl->m_tableRecognition.get();
            return providers;
        });
    auto exportWorkflow = std::make_unique<ScreenshotSelectionExportWorkflow>(
        ScreenshotSelectionExportWorkflowContext{
            m_captureState,
            m_geometry,
            m_selection,
            *exportService,
            *exportUiServices,
            *m_selectionSettings,
            owner,
            [this]() {
                return m_ocrController != nullptr ? m_ocrController->cachedRecognitionResults()
                                                   : ScreenshotRecognitionResults{};
            },
        });
    m_exportService = std::move(exportService);
    m_selectionExportUiServices = std::move(exportUiServices);
    m_selectionExportWorkflow = std::move(exportWorkflow);
    return true;
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
            [this]() {
                if (m_overlayCoordinator != nullptr) {
                    m_overlayCoordinator->refreshCanvasCreationStyles(
                        m_displaySession,
                        snow_shot::presentation::screenshotCanvasToolStyleDefaults());
                }
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
    invalidateRecognitionSession();
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
            if (snow_shot::presentation::WindowShortcutManager::focusAcceptsTextInput(focus)) {
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
    QObject::connect(m_selectorCoordinator, &ScreenshotSelectorCoordinator::refreshFinished, &owner,
                     [this](bool ok) { m_selectorWorkflow->handleRefreshFinished(ok); });
    QObject::connect(m_selectorCoordinator, &ScreenshotSelectorCoordinator::hitTestFinished, &owner,
                     [this](bool ok, const QVector<QRectF>& hitRects) {
                         m_selectorWorkflow->handleHitTestFinished(ok, hitRects);
                     });
}

void ScreenshotController::Impl::setMoveTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    static_cast<void>(resetCanvasEditingState());
    m_toolCommandWorkflow->setMoveTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

bool ScreenshotController::Impl::activateToolForSelectionResize(ScreenshotActiveTool tool) {
    switch (tool) {
    case ScreenshotActiveTool::Move: {
        if (m_ocrController != nullptr) {
            const bool wasActive = m_ocrController->active();
            m_ocrController->deactivateForSelectionResize();
            if (wasActive) {
                restoreKeyboardOwnerQueued(nullptr);
            }
        }
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
        QTimer::singleShot(0, &owner,
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
    if (!ensureRecognitionFeature()) {
        return;
    }
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
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setSelectTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setShapeTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setShapeTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setArrowTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setArrowTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setTextTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setTextTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setSerialNumberTool() {
    deactivateRecognition();
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
    if (!ensureRecognitionFeature()) {
        return;
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
    if (!ensureRecognitionFeature()) {
        return;
    }
    m_ocrController->activateTable();
    m_presentationServices->updateOverlayState();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setQrTool() {
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    if (!ensureRecognitionFeature()) {
        return;
    }
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
    if (!ensureRecognitionFeature()) {
        return;
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
    if (m_ocrController != nullptr) {
        m_ocrController->mergeTableSelection();
    }
}

void ScreenshotController::Impl::splitTableSelection() {
    if (m_ocrController != nullptr) {
        m_ocrController->splitTableSelection();
    }
}

void ScreenshotController::Impl::resetTable() {
    if (m_ocrController != nullptr) {
        m_ocrController->resetTable();
    }
}

void ScreenshotController::Impl::toggleTextEditing() {
    if (m_ocrController == nullptr) {
        return;
    }
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
    if (m_ocrController == nullptr) {
        return;
    }
    if (m_ocrController->translating()) {
        m_ocrController->endTextEditing();
    } else {
        m_ocrController->beginTextTranslation();
    }
}

void ScreenshotController::Impl::resetTextEditing() {
    if (m_ocrController != nullptr) {
        m_ocrController->resetTextEditing();
    }
}

void ScreenshotController::Impl::openTextTranslationSettings() {
    if (ensureRecognitionFeature()) {
        m_ocrController->openTranslationSettings();
    }
}

void ScreenshotController::Impl::applyTextFormatting(const QString& value) {
    if (m_ocrController != nullptr) {
        m_ocrController->applyTextFormatting(value);
    }
}

void ScreenshotController::Impl::applyTextPunctuation(const QString& value) {
    if (m_ocrController != nullptr) {
        m_ocrController->applyTextPunctuation(value);
    }
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
    const quint64 generation = ++m_imageExportGeneration;
    m_activeImageExports.insert(generation);
    m_imageExportCaptureEpochs.insert(generation, m_captureEpoch);
    return generation;
}

bool ScreenshotController::Impl::finishImageExport(quint64 generation) {
    if (!m_activeImageExports.remove(generation)) {
        return false;
    }
    m_imageExportCaptureEpochs.remove(generation);
    return true;
}

bool ScreenshotController::Impl::imageExportCurrent(quint64 generation) const {
    return m_activeImageExports.contains(generation);
}

bool ScreenshotController::Impl::imageExportNotificationCurrent(quint64 generation) const {
    const auto epoch = m_imageExportCaptureEpochs.constFind(generation);
    return epoch != m_imageExportCaptureEpochs.cend() && epoch.value() == m_captureEpoch;
}

void ScreenshotController::Impl::hideImageExportPresentation() {
    SNOW_SHOT_PIN_PERF_SCOPE("controller.hide_presentation");
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.hide_presentation.enter");
    if (m_colorPickerController != nullptr) {
        m_colorPickerController->hide();
    }
    if (m_toolbarPresenter != nullptr) {
        m_toolbarPresenter->hideSelectionToolbar();
    }
    if (m_overlayCoordinator != nullptr) {
        m_overlayCoordinator->hideOverlayWindowsImmediately(m_displaySession);
    }
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.hide_presentation.exit");
}

void ScreenshotController::Impl::detachCaptureForExport(ExportDetachMode mode) {
    SNOW_SHOT_PIN_PERF_SCOPE("controller.detach_capture");
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.detach_capture.enter");
    if (mode == ExportDetachMode::Immediate) {
        hideImageExportPresentation();
    }
    if (m_scrollingCaptureController != nullptr) {
        m_scrollingCaptureController->detachPendingResultRequest();
    }
    static_cast<void>(stopScrollingCapture(false));
    if (m_ocrController != nullptr) {
        m_ocrController->invalidateSession();
    }
    if (m_captureWorkflow != nullptr) {
        if (mode == ExportDetachMode::DeferredPresentation) {
            m_captureWorkflow->cancelCaptureForExport();
        } else {
            m_captureWorkflow->cancelCapture();
        }
    }
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.detach_capture.exit");
}

void ScreenshotController::Impl::scheduleDeferredExportCleanup() {
    SNOW_SHOT_PIN_PERF_MILESTONE("cleanup.schedule_deferred");
    const QPointer<ScreenshotController> receiver(&owner);
    QTimer::singleShot(0, &owner, [receiver]() {
        if (!receiver.isNull() && receiver->m_impl != nullptr &&
            receiver->m_impl->m_captureWorkflow != nullptr) {
            SNOW_SHOT_PIN_PERF_MILESTONE("cleanup.deferred_started");
            receiver->m_impl->m_captureWorkflow->completeDeferredExportCleanup();
            SNOW_SHOT_PIN_PERF_MILESTONE("cleanup.deferred_finished");
        }
    });
}

void ScreenshotController::Impl::trackExportJob(const ScreenshotExportJobHandle& handle) {
    if (handle.isValid()) {
        m_exportJobs.push_back(handle);
    }
}

void ScreenshotController::Impl::trackClipboardCommit(
    const ScreenshotClipboardCommitHandle& handle) {
    if (handle.isValid()) {
        m_clipboardCommits.push_back(handle);
    }
}

void ScreenshotController::Impl::completeScrollingResultExport(quint64 generation) {
    if (!finishImageExport(generation)) {
        return;
    }
    m_exportJob = {};
    m_clipboardCommit = {};
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
    deactivateRecognition();
    if (!ensureScrollingFeature() || m_scrollingCaptureController->active() ||
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
    if (!ensureExportFeature()) {
        return;
    }
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
        const bool autoResizeWindow = snow_shot::storage::PinToScreenSettings().autoResizeWindow();
        const std::optional<quint64> exportGeneration = beginImageExport();
        if (!exportGeneration.has_value()) {
            SNOW_SHOT_PIN_PERF_FINISH(false);
            return;
        }
        SNOW_SHOT_PIN_PERF_MILESTONE("controller.export_scheduled");
        // The trimmed dimensions are already known from the scrolling thumbnail. Keep the
        // snapshot request in flight while presenting the shell, then materialize it through the
        // pinned window loader after the capture is detached.
        const ScreenshotPinnedImageFit fit =
            autoResizeWindow
                ? ScreenshotGeometryMapper::fitImageToAvailableGeometry(
                      sourceSize, display->screen->availableGeometry(), display->screen->geometry(),
                      ScreenshotGeometryMapper::physicalRectForScreen(*display->screen), 16)
                : ScreenshotGeometryMapper::centerImageAtFullResolution(
                      sourceSize, display->screen->availableGeometry(), display->screen->geometry(),
                      ScreenshotGeometryMapper::physicalRectForScreen(*display->screen));
        const auto loadState = std::make_shared<ScrollingPinMaterializationState>();
        loadState->controller = receiver;
        loadState->generation = *exportGeneration;
        const auto failLoad = [loadState]() {
            if (loadState->callback) {
                auto callback = std::move(loadState->callback);
                callback({});
            }
        };
        QObject::connect(m_scrollingCaptureController.get(), &QObject::destroyed,
                         [loadState, failLoad]() {
                             loadState->snapshotFailed = true;
                             failLoad();
                         });
        const auto startMaterialization = std::make_shared<std::function<void()>>();
        *startMaterialization = [loadState, failLoad]() mutable {
            if (loadState->loadingStarted || !loadState->callback) {
                return;
            }
            if (loadState->snapshotFailed) {
                failLoad();
                return;
            }
            if (!loadState->snapshot.has_value()) {
                return;
            }
            if (loadState->controller.isNull() || loadState->controller->m_impl == nullptr ||
                !loadState->controller->m_impl->imageExportCurrent(loadState->generation)) {
                failLoad();
                return;
            }
            const QPointer<QObject> windowReceiver = loadState->windowReceiver;
            if (windowReceiver.isNull()) {
                failLoad();
                return;
            }
            loadState->loadingStarted = true;
            ScreenshotScrollingSnapshot snapshot = std::move(*loadState->snapshot);
            loadState->snapshot.reset();
            auto callback = std::move(loadState->callback);
            loadState->job = ScreenshotExportCoordinator::shared().submit(
                windowReceiver, ScreenshotExportCoordinator::Priority::Foreground,
                [snapshot = std::move(snapshot)](
                    const ScreenshotExportCancellation& cancellation) mutable {
                    if (cancellation.isCancellationRequested()) {
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Cancelled,
                            QStringLiteral("The screenshot pin was cancelled"));
                    }
                    SNOW_SHOT_PIN_PERF_MILESTONE("controller.materialize_started");
                    QImage image;
                    {
                        SNOW_SHOT_PIN_PERF_SCOPE("export.materialize_scrolling_snapshot");
                        image = snapshot.materialize();
                    }
                    if (image.isNull()) {
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Render,
                            QStringLiteral("The scrolling screenshot could not be rendered"));
                    }
                    ScreenshotExportTaskResult result;
                    result.image = std::move(image);
                    return result;
                },
                [loadState, callback = std::move(callback)](
                    ScreenshotExportTaskResult result) mutable {
                    loadState->job = {};
                    if (callback) {
                        callback(result.succeeded() ? std::move(result.image) : QImage{});
                    }
                });
            if (!loadState->job.isValid()) {
                loadState->loadingStarted = false;
                failLoad();
                return;
            }
            loadState->controller->m_impl->m_exportJob = loadState->job;
            loadState->controller->m_impl->trackExportJob(loadState->job);
            SNOW_SHOT_PIN_PERF_MILESTONE("controller.materialize_submitted");
        };
        const ScreenshotImageLoader imageLoader = [loadState, startMaterialization](
                                                       QObject* windowReceiver,
                                                       ScreenshotImageLoadCallback callback) mutable {
            loadState->windowReceiver = windowReceiver;
            loadState->callback = std::move(callback);
            (*startMaterialization)();
        };
        const bool scheduled = m_scrollingCaptureController->requestTrimmedSnapshot(
            [loadState, startMaterialization](ScreenshotScrollingSnapshot snapshot) mutable {
                if (loadState->controller.isNull() || loadState->controller->m_impl == nullptr ||
                    !loadState->controller->m_impl->imageExportCurrent(loadState->generation) ||
                    !snapshot.isValid()) {
                    loadState->snapshotFailed = true;
                    if (loadState->callback) {
                        auto callback = std::move(loadState->callback);
                        callback({});
                    }
                    return;
                }
                loadState->snapshot = std::move(snapshot);
                SNOW_SHOT_PIN_PERF_MILESTONE("controller.snapshot_ready");
                (*startMaterialization)();
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
        SNOW_SHOT_PIN_PERF_MILESTONE("controller.snapshot_requested");
        const bool presented = fit.valid && targetScreen != nullptr &&
                               m_selectionExportUiServices != nullptr &&
                               m_selectionExportUiServices->presentPinnedImage(
                                   QImage{}, targetScreen, fit.nativeGeometry, fit.fullResolutionSize,
                                   {}, {}, 1.0, {}, imageLoader,
                                   [receiver, loadState,
                                    generation = *exportGeneration](bool success, QImage) {
                                       SNOW_SHOT_PIN_PERF_MILESTONE(
                                           "controller.presentation_complete");
                                       SNOW_SHOT_PIN_PERF_FINISH(success);
                                       if (!success) {
                                           loadState->job.cancel();
                                       }
                                       if (receiver.isNull() || receiver->m_impl == nullptr ||
                                           !receiver->m_impl->imageExportCurrent(generation)) {
                                           return;
                                       }
                                       if (!success && receiver->m_impl->imageExportNotificationCurrent(
                                                           generation)) {
                                           receiver->m_impl->m_messages->error(
                                               QString::fromLatin1(kCopyMessageKey),
                                               QCoreApplication::translate(
                                                   "ScreenshotController",
                                                   "The scrolling screenshot could not be pinned"));
                                        }
                                        receiver->m_impl->completeScrollingResultExport(generation);
                                        receiver->m_impl->scheduleDeferredExportCleanup();
                                    });
        SNOW_SHOT_PIN_PERF_MILESTONE("controller.presented");
        if (!presented) {
            SNOW_SHOT_PIN_PERF_FINISH(false);
            if (imageExportNotificationCurrent(*exportGeneration)) {
                m_messages->error(
                    QString::fromLatin1(kCopyMessageKey),
                    QCoreApplication::translate(
                        "ScreenshotController", "The scrolling screenshot could not be pinned"));
            }
            completeScrollingResultExport(*exportGeneration);
            scheduleDeferredExportCleanup();
        }
        hideImageExportPresentation();
        detachCaptureForExport(ExportDetachMode::DeferredPresentation);
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
    deactivateRecognition();
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
        bool pinDone = false;
        bool pinSuccess = false;
        bool committed = false;
    };
    const auto historyState = std::make_shared<PinHistoryState>();
    const QPointer<ScreenshotController> receiver(&owner);
    const auto maybeCommitHistory = std::make_shared<std::function<void()>>();
    *maybeCommitHistory = [receiver, historyState]() {
        if (receiver.isNull() || receiver->m_impl == nullptr || historyState->committed ||
            !historyState->pinDone) {
            return;
        }
        if (historyState->pinSuccess && historyState->candidate.has_value() &&
            receiver->m_impl->m_historyService != nullptr) {
            if (!historyState->candidate->resultImage.has_value()) {
                return;
            }
            historyState->committed = true;
            historyState->candidate->source =
                snow_shot::storage::CaptureHistorySource::PinnedToScreen;
            receiver->m_impl->m_historyService->commit(std::move(*historyState->candidate));
        }
    };
    const bool scheduled = m_selectionExportWorkflow->pinSelectionToScreen(
        [receiver, generation = *exportGeneration]() {
            return !receiver.isNull() && receiver->m_impl != nullptr &&
                   receiver->m_impl->imageExportCurrent(generation);
        },
         [receiver, generation = *exportGeneration, historyState,
         maybeCommitHistory](bool success, QImage image) mutable {
            SNOW_SHOT_PIN_PERF_SCOPE("controller.pin_result_callback");
            SNOW_SHOT_PIN_PERF_MILESTONE("controller.pin_result_callback.enter");
            SNOW_SHOT_PIN_PERF_FINISH(success);
            if (receiver.isNull() || receiver->m_impl == nullptr ||
                !receiver->m_impl->finishImageExport(generation)) {
                return;
            }
            historyState->pinDone = true;
            historyState->pinSuccess = success;
            if (success && historyState->candidate.has_value() && !image.isNull()) {
                historyState->candidate->resultImage = std::move(image);
            }
            if (!success) {
                qWarning("Screenshot pin export failed");
            }
            (*maybeCommitHistory)();
            if (receiver->m_impl->m_historyService != nullptr) {
                SNOW_SHOT_PIN_PERF_MILESTONE("controller.history_navigation_reset.enter");
                receiver->m_impl->m_historyService->resetCaptureNavigation();
                SNOW_SHOT_PIN_PERF_MILESTONE("controller.history_navigation_reset.exit");
            }
            receiver->m_impl->scheduleDeferredExportCleanup();
            SNOW_SHOT_PIN_PERF_MILESTONE("controller.pin_result_callback.exit");
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
        return;
    }
    detachCaptureForExport(ExportDetachMode::DeferredPresentation);
}

void ScreenshotController::Impl::setScrollingScreenshotRecognitionMode(
    ScreenshotScrollingRecognitionMode mode) {
    if (m_scrollingCaptureController == nullptr || !m_scrollingCaptureController->active()) {
        return;
    }
    static_cast<void>(m_scrollingCaptureController->setRecognitionMode(mode));
}

void ScreenshotController::Impl::pinClipboardContentToScreen() {
    if (!ensureExportFeature()) {
        return;
    }
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        qWarning("No screen is available for clipboard pinning");
        return;
    }

    SNOW_SHOT_PIN_PERF_BEGIN("clipboard-input", 0, 0);
    SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.input_started");
    const auto perfReaderStarted = std::chrono::steady_clock::now();
    SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.snapshot_started");
    auto snapshot = ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(),
                                                               screen->devicePixelRatio());
    const qint64 perfReaderNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - perfReaderStarted)
            .count();
    SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.snapshot_finished");
    if (!snapshot.has_value()) {
        SNOW_SHOT_PIN_PERF_FINISH(false);
        qWarning("Clipboard content could not be pinned");
        m_messages->error(QString::fromLatin1(kPinClipboardMessageKey),
                          QCoreApplication::translate(
                              "ScreenshotController",
                              "The clipboard does not contain content that can be pinned"));
        return;
    }

    const bool clipboardFastPath = snapshot->encodedImages.isEmpty() &&
                                   !snapshot->localImage.has_value() &&
                                   ((snapshot->nativeDib.has_value() &&
                                     snapshot->nativeDib->isValid()) ||
                                    (!snapshot->detachedImage.isNull() &&
                                     !snapshot->detachedImage.size().isEmpty()));
    const char* perfScenario = !snapshot->encodedImages.isEmpty()
                                   ? "clipboard-image-encoded"
                                   : clipboardFastPath
                                         ? "clipboard-image-detached"
                                         : snapshot->localImage.has_value()
                                               ? "clipboard-file-image"
                                               : !snapshot->html.isEmpty() ? "clipboard-html"
                                                                           : "clipboard-text";
    const QSize nativeSize = snapshot->nativeDib.has_value() ? snapshot->nativeDib->size
                                                               : snapshot->detachedImage.size();
    SNOW_SHOT_PIN_PERF_DESCRIPTOR(perfScenario, nativeSize.width(), nativeSize.height());
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.enter");
    SNOW_SHOT_PIN_PERF_COUNTER("clipboard.reader_snapshot_ns", perfReaderNanoseconds);

    const bool autoResizeWindow = snow_shot::storage::PinToScreenSettings().autoResizeWindow();
    m_clipboardPinJob.cancel();
    const quint64 generation = ++m_clipboardPinGeneration;

    // QMimeData may already expose a detached image with its final dimensions. In
    // that case the shell can be placed immediately while the clipboard decode
    // runs asynchronously. Encoded, file-backed, and text payloads continue
    // through the decode-first path below because their size is not known yet.
    if (clipboardFastPath) {
        const ScreenshotPinnedImageFit fit =
            autoResizeWindow
                ? ScreenshotGeometryMapper::fitImageToAvailableGeometry(
                      nativeSize, screen->availableGeometry(), screen->geometry(),
                      ScreenshotGeometryMapper::physicalRectForScreen(*screen), 16)
                    : ScreenshotGeometryMapper::centerImageAtFullResolution(
                          nativeSize, screen->availableGeometry(), screen->geometry(),
                          ScreenshotGeometryMapper::physicalRectForScreen(*screen));
        SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.fit_computed");
        const QPointer<ScreenshotController> receiver(&owner);
        const QPointer<QScreen> guardedScreen(screen);
        const bool nativeSnapshot = snapshot->nativeDib.has_value();
        const ScreenshotImageLoader imageLoader =
            [receiver, guardedScreen, generation, snapshot = std::move(*snapshot)](
                QObject* windowReceiver, ScreenshotImageLoadCallback callback) mutable {
                const QPointer<QObject> guardedWindow(windowReceiver);
                auto content = std::make_shared<std::optional<ScreenshotClipboardContent>>();
                const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
                    windowReceiver, ScreenshotExportCoordinator::Priority::Foreground,
                    [snapshot = std::move(snapshot), content](
                        const ScreenshotExportCancellation& cancellation) mutable {
                        SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.decode_started");
                        *content = ScreenshotClipboardContentReader::decode(
                            std::move(snapshot), [&cancellation]() {
                                return cancellation.isCancellationRequested();
                            });
                        if (content->has_value() && content->value().isValid()) {
                            SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.decode_finished");
                        }
                        if (!content->has_value() || !content->value().isValid()) {
                            return ScreenshotExportTaskResult::failure(
                                cancellation.isCancellationRequested()
                                    ? ScreenshotExportFailureStage::Cancelled
                                    : ScreenshotExportFailureStage::Source,
                                cancellation.isCancellationRequested()
                                    ? QStringLiteral("The clipboard pin was cancelled")
                                    : QStringLiteral("The clipboard content could not be decoded"));
                        }
                        return ScreenshotExportTaskResult{};
                    },
                    [receiver, guardedScreen, guardedWindow, generation, content,
                     callback = std::move(callback)](ScreenshotExportTaskResult result) mutable {
                        if (guardedWindow.isNull()) {
                            return;
                        }
                        if (receiver.isNull() || receiver->m_impl == nullptr ||
                            generation != receiver->m_impl->m_clipboardPinGeneration ||
                            guardedScreen.isNull() || !result.succeeded() ||
                            !content->has_value() || !content->value().isValid()) {
                            callback({});
                            return;
                        }
                        receiver->m_impl->m_clipboardPinJob = {};
                        callback(std::move(content->value().image));
                    });
                if (receiver.isNull() || receiver->m_impl == nullptr ||
                    generation != receiver->m_impl->m_clipboardPinGeneration) {
                    callback({});
                    return;
                }
                receiver->m_impl->m_clipboardPinJob = job;
                if (!job.isValid()) {
                    receiver->m_impl->m_clipboardPinJob = {};
                    callback({});
                }
            };
        const bool presented = fit.valid &&
                               m_selectionExportUiServices != nullptr &&
                               m_selectionExportUiServices->presentPinnedImage(
                                   nativeSnapshot ? QImage{} : snapshot->detachedImage,
                                   screen, fit.nativeGeometry, fit.fullResolutionSize,
                                   {}, {}, 1.0, {}, imageLoader,
                                   [receiver, generation](bool success, QImage) {
                                       SNOW_SHOT_PIN_PERF_MILESTONE(
                                           "controller.presentation_complete");
                                       SNOW_SHOT_PIN_PERF_FINISH(success);
                                       if (success || receiver.isNull() ||
                                           receiver->m_impl == nullptr ||
                                           generation != receiver->m_impl->m_clipboardPinGeneration) {
                                           return;
                                       }
                                       receiver->m_impl->m_clipboardPinJob.cancel();
                                       receiver->m_impl->m_clipboardPinJob = {};
                                       receiver->m_impl->m_messages->error(
                                           QString::fromLatin1(kPinClipboardMessageKey),
                                           QCoreApplication::translate(
                                               "ScreenshotController",
                                               "The clipboard content could not be pinned"));
                                   });
        SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.presented");
        if (!presented) {
            SNOW_SHOT_PIN_PERF_FINISH(false);
            m_messages->error(
                QString::fromLatin1(kPinClipboardMessageKey),
                QCoreApplication::translate("ScreenshotController",
                                            "The clipboard pin could not be presented"));
        }
        return;
    }

    // Encoded images, local files, and text do not expose their final pixel size
    // until the export worker decodes or renders them. Prepare the native pinned
    // shell now, but leave it hidden until the decoded size is available.
    if (m_selectionExportUiServices != nullptr) {
        m_selectionExportUiServices->prewarmPinnedWindow(screen);
    }
    SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.prewarmed");

    auto content = std::make_shared<std::optional<ScreenshotClipboardContent>>();
    const QPointer<ScreenshotController> receiver(&owner);
    const QPointer<QScreen> guardedScreen(screen);
    m_clipboardPinJob = ScreenshotExportCoordinator::shared().submit(
        &owner, ScreenshotExportCoordinator::Priority::Foreground,
        [snapshot = std::move(*snapshot),
         content](const ScreenshotExportCancellation& cancellation) mutable {
            SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.decode_started");
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

            SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.decode_finished");
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
            SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.fit_computed");
            if (!fit.valid || receiver->m_impl->m_selectionExportUiServices == nullptr ||
                !receiver->m_impl->m_selectionExportUiServices->presentPinnedImage(
                    decoded.image, guardedScreen, fit.nativeGeometry, fit.fullResolutionSize,
                    std::move(decoded.formattedDocument), decoded.plainText,
                    decoded.formattedTextDevicePixelRatio, std::move(decoded.originalContent), {},
                    [](bool success, QImage) {
                        SNOW_SHOT_PIN_PERF_MILESTONE("controller.presentation_complete");
                        SNOW_SHOT_PIN_PERF_FINISH(success);
                    })) {
                SNOW_SHOT_PIN_PERF_FINISH(false);
                receiver->m_impl->m_messages->error(
                    QString::fromLatin1(kPinClipboardMessageKey),
                    QCoreApplication::translate("ScreenshotController",
                                                "The clipboard pin could not be presented"));
            }
        });
    SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.decode_scheduled");
    if (!m_clipboardPinJob.isValid()) {
        SNOW_SHOT_PIN_PERF_FINISH(false);
        m_messages->error(
            QString::fromLatin1(kPinClipboardMessageKey),
            QCoreApplication::translate("ScreenshotController", "The clipboard pin queue is full"));
    }
}

void ScreenshotController::Impl::saveSelectionToFile() {
    rememberKeyboardOwner(QApplication::focusWidget());
    QPointer<ScreenshotOverlayWindow> dialogOwner(keyboardOwnerOverlay());
    const snow_shot::presentation::WindowShortcutManager::InputSuspensionHandle suspension =
        m_windowShortcutManager != nullptr ? m_windowShortcutManager->suspendInput() : 0;
    [[maybe_unused]] const auto interactionGuard = makeScopeExit([this, dialogOwner, suspension]() {
        if (m_windowShortcutManager != nullptr && suspension != 0) {
            m_windowShortcutManager->resumeInput(suspension);
        }
        restoreKeyboardOwnerQueued(dialogOwner.data());
    });

    const snow_shot::storage::ScreenshotSettings outputSettings;
    const QString directory = ScreenshotImageFileService::saveDialogDirectory(
        outputSettings.lastManualSaveDirectory(), outputSettings.imageSaveDirectory());
    static_cast<void>(QDir().mkpath(directory));
    const QString initialPath = QDir(directory).filePath(
        ScreenshotImageFileService::suggestedBaseName(outputSettings.manualSaveFilenameFormat()) +
        QStringLiteral(".png"));
    QString selectedFilter =
        ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Png);
    const QString selectedPath = QFileDialog::getSaveFileName(
        dialogOwner.data(), QCoreApplication::translate("ScreenshotController", "Save screenshot"),
        initialPath,
        ScreenshotImageFileService::saveDialogFilter(), &selectedFilter);
    if (selectedPath.isEmpty()) {
        return;
    }
    if (!ensureExportFeature()) {
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
        return;
    }
    detachCaptureForExport();
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
    trackExportJob(m_exportJob);
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
    trackExportJob(m_exportJob);
    if (!m_exportJob.isValid()) {
        completeFileSave(ScreenshotExportTaskResult::failure(
                             ScreenshotExportFailureStage::Queue,
                             QStringLiteral("The screenshot export queue is full")),
                         generation);
    }
}

void ScreenshotController::Impl::completeFileSave(ScreenshotExportTaskResult result,
                                                  quint64 generation) {
    const bool notify = imageExportNotificationCurrent(generation);
    if (!finishImageExport(generation)) {
        return;
    }
    m_exportJob = {};
    if (!result.succeeded() && notify) {
        m_messages->error(QString::fromLatin1(kSaveMessageKey),
                          QCoreApplication::translate("ScreenshotController",
                                                      "The screenshot could not be saved: %1")
                              .arg(result.error));
        return;
    }
}

void ScreenshotController::Impl::cancelCapture() {
    ++m_captureEpoch;
    clearCanvasColorSampling();
    if (m_overlayInputHandler != nullptr) {
        m_overlayInputHandler->resetTransientShortcuts();
    }
    resetPendingCaptureRequest();
    invalidateRecognitionSession();
    static_cast<void>(stopScrollingCapture(false));
    if (m_historyService != nullptr) {
        m_historyService->resetCaptureNavigation();
    }
    m_captureWorkflow->cancelCapture();
}

void ScreenshotController::Impl::copySelectionToClipboard() {
    copySelectionToClipboardWithSource(snow_shot::storage::CaptureHistorySource::CopiedToClipboard);
}

void ScreenshotController::Impl::copySelectionToClipboardWithSource(
    snow_shot::storage::CaptureHistorySource historySource) {
    if (!ensureExportFeature()) {
        return;
    }
    if (m_ocrController != nullptr && m_ocrController->active()) {
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
                            if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                                receiver->m_impl->m_messages->error(
                                QString::fromLatin1(kCopyMessageKey),
                                QCoreApplication::translate(
                                    "ScreenshotController",
                                    "The scrolling screenshot could not be copied: %1")
                                    .arg(result.error));
                            }
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
                                    if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                                        receiver->m_impl->m_messages->error(
                                        QString::fromLatin1(kCopyMessageKey),
                                        QCoreApplication::translate(
                                            "ScreenshotController",
                                            "The scrolling screenshot could not be copied: %1")
                                            .arg(commit.errorString()));
                                    }
                                }
                                receiver->m_impl->completeScrollingResultExport(generation);
                            });
                        receiver->m_impl->trackClipboardCommit(receiver->m_impl->m_clipboardCommit);
                        if (!receiver->m_impl->m_clipboardCommit.isValid()) {
                            if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                                receiver->m_impl->m_messages->error(
                                    QString::fromLatin1(kCopyMessageKey),
                                    QCoreApplication::translate(
                                        "ScreenshotController",
                                        "The scrolling screenshot clipboard operation could not be "
                                        "started"));
                            }
                            receiver->m_impl->completeScrollingResultExport(generation);
                        }
                    });
                receiver->m_impl->trackExportJob(receiver->m_impl->m_exportJob);
                if (!receiver->m_impl->m_exportJob.isValid()) {
                    if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                        receiver->m_impl->m_messages->error(
                            QString::fromLatin1(kCopyMessageKey),
                            QCoreApplication::translate("ScreenshotController",
                                                        "The screenshot export queue is full"));
                    }
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
        detachCaptureForExport();
        return;
    }
    const bool historyEligible = m_interaction.activeTool() != ScreenshotActiveTool::Ocr &&
                                 m_interaction.activeTool() != ScreenshotActiveTool::Table &&
                                 m_interaction.activeTool() != ScreenshotActiveTool::Qr;
    deactivateRecognition();
    const std::optional<quint64> exportGeneration = beginImageExport();
    if (!exportGeneration.has_value()) {
        return;
    }
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
            return;
        }
        detachCaptureForExport();
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
        return;
    }
    detachCaptureForExport();
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
            [receiver, generation](ScreenshotExportTaskResult result) {
                if (!receiver.isNull() && receiver->m_impl != nullptr && !result.succeeded() &&
                    result.failureStage != ScreenshotExportFailureStage::Cancelled &&
                    receiver->m_impl->imageExportNotificationCurrent(generation)) {
                    receiver->m_impl->m_messages->warning(
                        QString::fromLatin1(kSaveMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "Automatic screenshot saving failed: %1")
                            .arg(result.error));
                }
            });
        trackExportJob(m_backgroundSaveJob);
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
                if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "The screenshot could not be copied: %1")
                            .arg(reason));
                }
                receiver->m_impl->completeCopyExport(false, generation, historySource,
                                                     historyCandidate, scrolling);
                return;
            }

            if (copyFileToClipboard) {
                auto* mimeData = new QMimeData();
                mimeData->setUrls({QUrl::fromLocalFile(QFileInfo(result.savedPath).absoluteFilePath())});
                receiver->m_impl->m_clipboardCommit = ScreenshotClipboardService::commitMimeData(
                    QApplication::clipboard(), receiver, mimeData,
                    [receiver, generation, historySource, historyCandidate,
                     scrolling](ScreenshotClipboardCommitResult commit) mutable {
                        if (receiver.isNull() || receiver->m_impl == nullptr ||
                            !receiver->m_impl->imageExportCurrent(generation)) {
                            return;
                        }
                        if (!commit.succeeded() &&
                            receiver->m_impl->imageExportNotificationCurrent(generation)) {
                            receiver->m_impl->m_messages->error(
                                QString::fromLatin1(kCopyMessageKey),
                                QCoreApplication::translate(
                                    "ScreenshotController",
                                    "The screenshot file could not be copied: the clipboard did not "
                                    "accept the file"));
                        }
                        receiver->m_impl->completeCopyExport(
                            commit.succeeded(), generation, historySource, historyCandidate, scrolling);
                    });
                receiver->m_impl->trackClipboardCommit(receiver->m_impl->m_clipboardCommit);
                if (!receiver->m_impl->m_clipboardCommit.isValid()) {
                    receiver->m_impl->completeCopyExport(false, generation, historySource,
                                                         historyCandidate, scrolling);
                }
                return;
            }

            receiver->m_impl->m_exportJob = {};
            if (result.clipboardPayload == nullptr) {
                if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "The screenshot clipboard image is unavailable"));
                }
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
                        if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                            receiver->m_impl->m_messages->error(
                                QString::fromLatin1(kCopyMessageKey),
                                QCoreApplication::translate("ScreenshotController",
                                                            "The screenshot could not be copied: %1")
                                    .arg(commit.errorString()));
                        }
                    }
                    receiver->m_impl->completeCopyExport(
                        commit.succeeded(), generation, historySource, historyCandidate, scrolling);
                });
            receiver->m_impl->trackClipboardCommit(receiver->m_impl->m_clipboardCommit);
            if (!receiver->m_impl->m_clipboardCommit.isValid()) {
                if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate(
                            "ScreenshotController",
                            "The screenshot clipboard operation could not be started"));
                }
                receiver->m_impl->completeCopyExport(false, generation, historySource,
                                                     historyCandidate, scrolling);
            }
        });
    trackExportJob(m_exportJob);
    if (!m_exportJob.isValid()) {
        if (imageExportNotificationCurrent(generation)) {
            m_messages->error(QString::fromLatin1(kCopyMessageKey),
                              QCoreApplication::translate("ScreenshotController",
                                                          "The screenshot export queue is full"));
        }
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
            [receiver, generation](ScreenshotExportTaskResult result) {
                if (!receiver.isNull() && receiver->m_impl != nullptr && !result.succeeded() &&
                    result.failureStage != ScreenshotExportFailureStage::Cancelled &&
                    receiver->m_impl->imageExportNotificationCurrent(generation)) {
                    receiver->m_impl->m_messages->warning(
                        QString::fromLatin1(kSaveMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "Automatic screenshot saving failed: %1")
                            .arg(result.error));
                }
            });
        trackExportJob(m_backgroundSaveJob);
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
                if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "The screenshot could not be copied: %1")
                            .arg(reason));
                }
                receiver->m_impl->completeCopyExport(false, generation, historySource, {}, true);
                return;
            }
            if (copyFileToClipboard) {
                auto* mimeData = new QMimeData();
                mimeData->setUrls({QUrl::fromLocalFile(QFileInfo(result.savedPath).absoluteFilePath())});
                receiver->m_impl->m_clipboardCommit = ScreenshotClipboardService::commitMimeData(
                    QApplication::clipboard(), receiver, mimeData,
                    [receiver, generation, historySource](ScreenshotClipboardCommitResult commit) mutable {
                        if (receiver.isNull() || receiver->m_impl == nullptr ||
                            !receiver->m_impl->imageExportCurrent(generation)) {
                            return;
                        }
                        if (!commit.succeeded() &&
                            receiver->m_impl->imageExportNotificationCurrent(generation)) {
                            receiver->m_impl->m_messages->error(
                                QString::fromLatin1(kCopyMessageKey),
                                QCoreApplication::translate(
                                    "ScreenshotController",
                                    "The screenshot file could not be copied: the clipboard did not "
                                    "accept the file"));
                        }
                        receiver->m_impl->completeCopyExport(
                            commit.succeeded(), generation, historySource, {}, true);
                    });
                receiver->m_impl->trackClipboardCommit(receiver->m_impl->m_clipboardCommit);
                if (!receiver->m_impl->m_clipboardCommit.isValid()) {
                    receiver->m_impl->completeCopyExport(false, generation, historySource, {}, true);
                }
                return;
            }

            receiver->m_impl->m_exportJob = {};
            if (result.clipboardPayload == nullptr) {
                if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate("ScreenshotController",
                                                    "The screenshot clipboard image is unavailable"));
                }
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
                        if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                            receiver->m_impl->m_messages->error(
                                QString::fromLatin1(kCopyMessageKey),
                                QCoreApplication::translate(
                                    "ScreenshotController",
                                    "The scrolling screenshot could not be copied: %1")
                                    .arg(commit.errorString()));
                        }
                    }
                    receiver->m_impl->completeCopyExport(commit.succeeded(), generation,
                                                         historySource, {}, true);
                });
            receiver->m_impl->trackClipboardCommit(receiver->m_impl->m_clipboardCommit);
            if (!receiver->m_impl->m_clipboardCommit.isValid()) {
                if (receiver->m_impl->imageExportNotificationCurrent(generation)) {
                    receiver->m_impl->m_messages->error(
                        QString::fromLatin1(kCopyMessageKey),
                        QCoreApplication::translate(
                            "ScreenshotController",
                            "The screenshot clipboard operation could not be started"));
                }
                receiver->m_impl->completeCopyExport(false, generation, historySource, {}, true);
            }
        });
    trackExportJob(m_exportJob);
    if (!m_exportJob.isValid()) {
        if (imageExportNotificationCurrent(generation)) {
            m_messages->error(QString::fromLatin1(kCopyMessageKey),
                              QCoreApplication::translate("ScreenshotController",
                                                          "The screenshot export queue is full"));
        }
        completeCopyExport(false, generation, historySource, {}, true);
    }
}

void ScreenshotController::Impl::completeCopyExport(
    bool success, quint64 generation, snow_shot::storage::CaptureHistorySource historySource,
    std::shared_ptr<std::optional<ScreenshotHistoryEntry>> historyCandidate, bool scrolling) {
    const bool notify = imageExportNotificationCurrent(generation);
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
    } else if (!success && notify) {
        qWarning("Screenshot clipboard export failed");
    }
    if (m_historyService != nullptr) {
        m_historyService->resetCaptureNavigation();
    }
}

bool ScreenshotController::Impl::resetCanvasEditingState() {
    SNOW_SHOT_PIN_PERF_SCOPE("controller.reset_canvas_editing_state");
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.reset_canvas_editing_state.enter");
    const bool reset = m_overlayCoordinator != nullptr &&
                       m_overlayCoordinator->resetEditingState(m_displaySession);
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.reset_canvas_editing_state.exit");
    return reset;
}

bool ScreenshotController::Impl::prepareHistoryCandidate(
    std::optional<ScreenshotHistoryEntry>* candidate) {
    SNOW_SHOT_PIN_PERF_SCOPE("controller.history_snapshot");
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.history_snapshot.enter");
    if (candidate == nullptr) {
        SNOW_SHOT_PIN_PERF_MILESTONE("controller.history_snapshot.exit");
        return true;
    }
    candidate->reset();
    if (m_historyService == nullptr) {
        SNOW_SHOT_PIN_PERF_MILESTONE("controller.history_snapshot.exit");
        return true;
    }
    *candidate = m_historyService->snapshotCurrent(true);
    SNOW_SHOT_PIN_PERF_MILESTONE("controller.history_snapshot.exit");
    return true;
}

void ScreenshotController::Impl::startScreenRecording() {
    deactivateRecognition();
    if (!m_selection.hasPixelSelection() || !ensureRecordingFeature() ||
        (m_scrollingCaptureController != nullptr && m_scrollingCaptureController->active())) {
        return;
    }
    QRect physicalRegion = m_selection.pixelSelection().translated(m_geometry.canvasOrigin());
    if (physicalRegion.width() < 2 || physicalRegion.height() < 2) {
        return;
    }
    static_cast<void>(resetCanvasEditingState());

    invalidateRecognitionSession();
    m_captureWorkflow->cancelCapture();
    if (m_historyService != nullptr) {
        m_historyService->resetCaptureNavigation();
    }
    QTimer::singleShot(
        0, &owner, [this, physicalRegion]() { m_screenRecordingController->open(physicalRegion); });
}

void ScreenshotController::Impl::setShapeStyleFromToolbar(const SnowCanvasShapeStyle& style,
                                                          quint32 properties,
                                                          SnowCanvasShapeKind kind) {
    m_toolCommandWorkflow->setShapeStyleFromToolbar(style, properties, kind);
}

void ScreenshotController::Impl::setLineTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setLineTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setFreeDrawTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setFreeDrawTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setHighlightTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setHighlightTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setPenHighlightTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setPenHighlightTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setEraserTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setEraserTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setFilterTool() {
    setRectangleFilterTool();
}

void ScreenshotController::Impl::setSpotlightTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setSpotlightTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setRectangleFilterTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setRectangleFilterTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setPenFilterTool() {
    deactivateRecognition();
    const bool scrollingCaptureStopped = stopScrollingCapture(true);
    m_toolCommandWorkflow->setPenFilterTool();
    restoreToolUiAfterScrollingCapture(scrollingCaptureStopped);
}

void ScreenshotController::Impl::setWatermarkTool() {
    deactivateRecognition();
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
    if (!ensureCanvasSamplingUi()) {
        return;
    }

    clearCanvasColorSampling();
    m_canvasColorSamplingTarget = picker;
    m_canvasColorSamplingDestroyedConnection = QObject::connect(
        picker, &QObject::destroyed, &owner, [this]() { clearCanvasColorSampling(); });
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
    ++m_delayedCaptureGeneration;
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
    if (m_captureState.captureInProgress || !m_interaction.inactive() ||
        (m_captureState.sessionState != ScreenshotSessionState::IdleCold &&
         m_captureState.sessionState != ScreenshotSessionState::IdlePrepared)) {
        return false;
    }
    return m_captureWorkflow != nullptr;
}

bool ScreenshotController::Impl::beginCapture(
    PendingSelectionAction action, snow_shot::storage::CaptureHistorySource historySource,
    AutomaticSelectionMode automaticMode, const QPoint& automaticPhysicalPoint,
    quintptr focusedWindowHandle) {
    if (!canBeginCapture()) {
        return false;
    }

    ++m_captureEpoch;
    clearCanvasColorSampling();
    if (m_overlayInputHandler != nullptr) {
        m_overlayInputHandler->resetTransientShortcuts();
    }
    invalidateDelayedCapture();
    m_pendingHistoryEditRecordId.clear();
    m_pendingSelectionAction = action;
    m_pendingOcrFromQuickFunction = action == PendingSelectionAction::RecognizeText;
    m_pendingHistorySource = historySource;
    m_automaticSelectionMode = automaticMode;
    m_automaticPhysicalPoint = automaticPhysicalPoint;
    if (automaticMode != AutomaticSelectionMode::FocusedWindow) {
        m_focusedWindowCapture.reset();
    }
    invalidateRecognitionSession();
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
    QTimer::singleShot(0, &owner,
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
                               if (!directSourceImage.isNull() && ensureExportFeature()) {
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
    m_keyboardOwnerOverlay.clear();
    if (m_overlayInputHandler != nullptr) {
        m_overlayInputHandler->resetTransientShortcuts();
    }
    for (const auto& handle : m_exportJobs) {
        handle.cancel();
    }
    for (const auto& handle : m_clipboardCommits) {
        handle.cancel();
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
    ++m_clipboardPinGeneration;
    ++m_imageExportGeneration;
    m_activeImageExports.clear();
    m_imageExportCaptureEpochs.clear();
    resetPendingCaptureRequest();
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
        QObject::disconnect(m_selectorCoordinator, nullptr, &owner, nullptr);
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
    m_presentationServices.reset();
    m_colorPickerController.reset();
    m_toolbarPresenter.reset();
    m_selectionResizeWorkflow.reset();
    m_selectionExportUiServices.reset();
    m_selectionExportWorkflow.reset();
    m_exportService.reset();
    m_selectionSettings.reset();
    m_screenRecordingController.reset();
    m_overlayCoordinator.reset();
    m_overlayEventAdapter.reset();
}

ScreenshotController::ScreenshotController(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}

ScreenshotController::~ScreenshotController() = default;

void ScreenshotController::prewarmResources() {
    QTimer::singleShot(0, this, [this]() { m_impl->m_captureWorkflow->prewarmResources(); });
}

void ScreenshotController::startCapture() {
    static_cast<void>(m_impl->beginCapture());
}

void ScreenshotController::startDelayedCapture(int delaySeconds) {
    if (!m_impl->canBeginCapture()) {
        return;
    }
    const int seconds = std::clamp(delaySeconds, 1, 10);
    const quint64 generation = ++m_impl->m_delayedCaptureGeneration;
    QTimer::singleShot(seconds * 1000, this, [this, generation]() {
        if (m_impl == nullptr || generation != m_impl->m_delayedCaptureGeneration ||
            !m_impl->canBeginCapture()) {
            return;
        }
        startCapture();
    });
}

void ScreenshotController::captureAndPinSelection() {
    static_cast<void>(m_impl->beginCapture(Impl::PendingSelectionAction::Pin));
}

void ScreenshotController::captureAndRecognizeText() {
    static_cast<void>(m_impl->beginCapture(Impl::PendingSelectionAction::RecognizeText));
}

void ScreenshotController::captureAndTranslateText() {
    static_cast<void>(m_impl->beginCapture(Impl::PendingSelectionAction::RecognizeTextTranslation));
}

void ScreenshotController::captureAndCopySelection() {
    static_cast<void>(m_impl->beginCapture(Impl::PendingSelectionAction::Copy));
}

void ScreenshotController::captureCurrentMonitor() {
    if (!m_impl->canBeginCapture()) {
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
    static_cast<void>(m_impl->beginCapture(Impl::PendingSelectionAction::Copy,
                                           snow_shot::storage::CaptureHistorySource::CurrentMonitor,
                                           Impl::AutomaticSelectionMode::CurrentMonitor,
                                           cursorPosition));
}

void ScreenshotController::captureFocusedWindow() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!m_impl->canBeginCapture()) {
        return;
    }
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return;
    }
    const HWND root = GetAncestor(foreground, GA_ROOT);
    const HWND target = root != nullptr ? root : foreground;

    m_impl->m_focusedWindowCapture.reset();
    static_cast<void>(m_impl->beginCapture(
        Impl::PendingSelectionAction::Copy, snow_shot::storage::CaptureHistorySource::FocusedWindow,
        Impl::AutomaticSelectionMode::FocusedWindow, QPoint(), reinterpret_cast<quintptr>(target)));
#else
    Q_UNUSED(this);
#endif
}

void ScreenshotController::captureAndStartScreenRecording() {
    static_cast<void>(m_impl->beginCapture(Impl::PendingSelectionAction::StartVideo));
}

void ScreenshotController::startOrStopScreenRecordingAndCopy() {
    if (m_impl->m_screenRecordingController == nullptr) {
        captureAndStartScreenRecording();
        return;
    }
    if (!m_impl->m_screenRecordingController->isOpen()) {
        captureAndStartScreenRecording();
    } else if (!m_impl->m_screenRecordingController->isRecording()) {
        m_impl->m_screenRecordingController->startRecording();
    } else {
        m_impl->m_screenRecordingController->stopRecordingAndCopyVideo();
    }
}

void ScreenshotController::editHistoryRecord(const QString& recordId) {
    ++m_impl->m_captureEpoch;
    m_impl->startHistoryEdit(recordId);
}

void ScreenshotController::pinClipboardContentToScreen() {
    m_impl->pinClipboardContentToScreen();
}

void ScreenshotController::Impl::setSelectionToolbarHovered(bool hovered) {
    if (m_presentationServices != nullptr) {
        m_presentationServices->setSelectionToolbarHovered(hovered);
    }
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

