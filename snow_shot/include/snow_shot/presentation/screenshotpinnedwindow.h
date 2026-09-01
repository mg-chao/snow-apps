#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOW_H

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotrecognitionresults.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QMap>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>
#include <QWidget>

#include <memory>
#include <functional>
#include <vector>

namespace adqt::widgets {
class AdButton;
class AdContextMenu;
} // namespace adqt::widgets
namespace snow_shot::presentation {
class WindowShortcutManager;
}
namespace snow_shot::platform {
class PhysicalCursor;
enum class PhysicalCursorDirection;
} // namespace snow_shot::platform

class QAction;
class QActionGroup;
class QFrame;
class QLabel;
class QCloseEvent;
class QContextMenuEvent;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QMoveEvent;
class QResizeEvent;
class QScreen;
class QShowEvent;
class QTimer;
class QVariantAnimation;
class QWheelEvent;
class SnowCanvasWidget;
class ScreenshotCanvasRenderer;
class ScreenshotOcrPresentation;
class ScreenshotOcrRecognitionPort;
class ScreenshotQrRecognitionPort;
class SnowShotApiClient;
class ScreenshotRecognitionWindow;
class ScreenshotRecognitionSessionController;
class ScreenshotPinnedEditController;
class ScreenshotPinnedCopyService;
class ScreenshotPinnedNativeGeometryController;
class QTextDocument;

struct ScreenshotPinnedRecognitionProviders {
    ScreenshotOcrRecognitionPort* recognition = nullptr;
    ScreenshotQrRecognitionPort* qrRecognition = nullptr;
    SnowShotApiClient* tableRecognition = nullptr;
};

class ScreenshotPinnedWindow final : public QWidget {
    Q_OBJECT

  public:
    enum class RuntimeMode {
        NoDocument,
    };

    struct Config {
        QRect nativeGeometry;
        QRectF canvasSourceRect;
        QRectF contentCanvasRect;
        QRectF surfaceCanvasRect;
        ScreenshotResultStyle resultStyle;
        QSize fullResolutionScaleBasis;
        double initialScalePercent = 100.0;
        QString mouseWheelZoomMode = QStringLiteral("mouse_position");
        ScreenshotImageSource imageSource;
        ScreenshotImageLoader imageLoader;
        // Compatibility input for direct callers. New pin transactions use imageSource.
        QImage backgroundImage;
        QScreen* screen = nullptr;
        bool enableEditing = true;
        bool automaticTextRecognition = true;
        std::shared_ptr<QTextDocument> formattedTextDocument;
        QString formattedPlainText;
        qreal formattedTextDevicePixelRatio = 1.0;
        ScreenshotClipboardOriginalContent originalClipboardContent;
        ScreenshotOcrRecognitionPort* recognition = nullptr;
        ScreenshotQrRecognitionPort* qrRecognition = nullptr;
        SnowShotApiClient* tableRecognition = nullptr;
        std::function<ScreenshotPinnedRecognitionProviders()> recognitionProvider;
        ScreenshotRecognitionResults recognitionResults;
    };

    explicit ScreenshotPinnedWindow(RuntimeMode mode, QWidget* parent = nullptr);
    ~ScreenshotPinnedWindow() override;

    bool present(const Config& config, std::function<void(bool, QImage)> completion = {});
    bool presentPending(const Config& config,
                        std::function<void(bool, QImage)> completion = {});
    bool publishMaterializedImage(QImage image);
    bool prewarm(QScreen* screen = nullptr);
    QRect currentNativeGeometry() const;
    static void setRuntimeBorderColor(const QColor& color);
    [[nodiscard]] static QColor runtimeBorderColor();
    static void setRuntimeTrayEnabled(bool enabled);
    [[nodiscard]] static bool runtimeTrayEnabled();

  signals:
    void showMainWindowRequested();

  private:
    friend class ScreenshotPinnedEditController;

    enum class GeometryMutation {
        Scale,
        ImageTransform,
        Thumbnail,
        Animation,
    };

    bool event(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

    void createUi();
    void registerWindowShortcuts();
    void reloadPinnedWindowShortcuts();
    void createContextMenu();
    void applyRuntimeBorderColor();
    void updateShowMainInterfaceAction();
    void retranslateUi();
    void refreshContextMenu();
    void showContextMenu(const QPoint& globalPosition);
    void updateCanvasViewport();
    void updateControlsGeometry();
    void destroyCanvas();
    bool presentInternal(const Config& config, std::function<void(bool, QImage)> completion,
                         bool allowPending);
    using MaterializationCallback = std::function<void(bool)>;
    using PresentationCompletion = std::function<void(bool, QImage)>;
    void requestMaterializedImage(MaterializationCallback callback);
    void finishMaterializedImage(ScreenshotExportTaskResult result);
    void requestFirstContentFramePaint();
    void handleFirstContentFramePainted();
    void finishMaterializationCallbacks(bool succeeded);
    bool installMaterializedImage(QImage image);
    void configureRecognitionTarget();
    void scheduleDeferredPresentationSetup();
    void finishDeferredPresentationSetup(quint64 generation);
    void finishPresentation(bool succeeded, QImage image = {});
    void commitClipboardPayload(ScreenshotClipboardPayload payload);
    void ensureEditController();
    void setEditMode(bool enabled);
    void setOcrMode(bool enabled);
    void stopRecognition();
    void updateOcrPresentation();
    void updateRecognitionContentGeometry();
    void activateRecognitionMode(int mode);
    void ensureRecognitionProviders();
    void deactivateRecognition();
    void updateRecognitionToolbarState();
    void activateTextTranslation();
    void handleTextEditingRequested();
    void handleTextTranslationRequested();
    void handleTextResetRequested();
    void handleTextSettingsRequested();
    void handleTextFormattingRequested(const QString& value);
    void handleTextPunctuationRequested(const QString& value);
    void handleTableMergeRequested();
    void handleTableSplitRequested();
    void handleTableResetRequested();
    QPointF canvasPositionForViewPosition(const QPointF& position) const;
    void copyEditToolbarContent();
    void copyCurrentViewport();
    void copyOriginalContent();
    void saveAsFile();
    void invalidatePendingCopy();
    void applyImageOperation(const QTransform& operation, int quarterTurnDelta = 0);
    void resetImageTransform();
    void rebuildTransformedImage();
    void applyScale(int percent);
    void applyWheelScale(int percent, const QPointF& nativeCursor);
    bool handleOpacityWheel(QObject* watched, QWheelEvent* event);
    bool handleScaleWheel(QObject* watched, QWheelEvent* event);
    QSize orientedInitialPhysicalSize() const;
    QRect logicalRectForNativeRect(const QRect& nativeRect) const;
    void setEffectiveScale(double percent, bool showReadout);
    void showScaleReadout();
    void showOpacityReadout();
    void scheduleNativeScaleAdoption();
    void adoptSettledNativeScale();
    void setOpacityPercent(int percent);
    void setThumbnailMode(bool enabled, bool animate = true);
    void restoreFromThumbnailImmediately();
    void animateGeometryTo(const QRect& nativeTarget);
    bool applyWindowGeometry(const QRect& nativeGeometry, GeometryMutation mutation);
    bool finishNativeGeometryInteraction();
    bool reconcilePassiveNativeGeometry();
    bool restoreCommittedNativeGeometry();
    QRect nativeRectForLogicalRect(const QRect& logical, QScreen* screen) const;
    QRectF visibleCanvasRect() const;
    void showAllPinnedWindows();
    void hideOtherPinnedWindows();
    void closeOtherPinnedWindows();
    void closeAllPinnedWindows();
    [[nodiscard]] std::optional<QPoint> physicalCursorPosition() const;
    bool cursorMovementEnabled() const;
    bool moveCursorOnePixel(snow_shot::platform::PhysicalCursorDirection direction);
    bool startWindowMove();
    bool updateWindowMove(const QPoint& nativeCursorPosition);
    void finishWindowMove();
    bool windowDragEnabled() const;
    void updateWindowDragCursor(const QPoint& position);
    void setWindowDragCursor(Qt::CursorShape shape);
    void clearWindowDragCursor();
    bool nativeTrackSizeConstraintsEnabled() const;
    bool interactiveResizingEnabled() const;
    QPointF windowPositionForEvent(QObject* watched, const QPointF& position) const;
    QPointF nativePositionForWindowPosition(const QPointF& position) const;
    QPoint globalPositionForNativePosition(const QPoint& position) const;
    bool isControlsPanelPosition(const QPoint& position) const;

    SnowCanvasRuntime m_runtime;
    std::unique_ptr<snow_shot::presentation::WindowShortcutManager> m_shortcutManager;
    std::unique_ptr<snow_shot::platform::PhysicalCursor> m_physicalCursor;
    QMap<QString, quint64> m_pinnedShortcutBindings;
    std::unique_ptr<ScreenshotPinnedCopyService> m_copyService;
    ScreenshotExportJobHandle m_materializationJob;
    ScreenshotExportJobHandle m_fileSaveJob;
    ScreenshotClipboardCommitHandle m_clipboardCommit;
    std::vector<MaterializationCallback> m_materializationCallbacks;
    PresentationCompletion m_presentationCompletion;
    ScreenshotImageLoader m_imageLoader;
    bool m_materializationLoading = false;
    bool m_pendingImage = false;
    bool m_firstContentFramePublished = false;
    bool m_firstFramePaintPending = false;
    bool m_firstFramePaintSucceeded = true;
    bool m_completePresentationAfterFirstFrame = false;
    bool m_recognitionTargetReady = false;
    bool m_deferredPresentationSetupScheduled = false;
    quint64 m_presentationGeneration = 0;
    SnowCanvasWidget* m_canvas = nullptr;
    std::unique_ptr<ScreenshotCanvasRenderer> m_screenshotRenderer;
    QFrame* m_borderFrame = nullptr;
    QFrame* m_controlsPanel = nullptr;
    QLabel* m_scaleLabel = nullptr;
    QTimer* m_scaleLabelTimer = nullptr;
    QTimer* m_nativeScaleSettleTimer = nullptr;
    ScreenshotPinnedEditController* m_editController = nullptr;
    adqt::widgets::AdButton* m_editButton = nullptr;
    adqt::widgets::AdButton* m_closeButton = nullptr;
    adqt::widgets::AdContextMenu* m_contextMenu = nullptr;
    QAction* m_ocrAction = nullptr;
    QAction* m_drawingAction = nullptr;
    QAction* m_thumbnailAction = nullptr;
    QAction* m_showMainInterfaceAction = nullptr;
    QAction* m_closeAction = nullptr;
    QActionGroup* m_opacityActions = nullptr;
    QActionGroup* m_scaleActions = nullptr;
    QAction* m_scaleMenuAction = nullptr;
    QAction* m_opacityReadoutAction = nullptr;
    QAction* m_scaleReadoutAction = nullptr;
    QVariantAnimation* m_geometryAnimation = nullptr;
    QRectF m_canvasSourceRect;
    QRectF m_backgroundCanvasRect;
    QRectF m_resultSurfaceCanvasRect;
    ScreenshotResultStyle m_resultStyle;
    ScreenshotImageSource m_imageSource;
    QImage m_originalImage;
    QImage m_transformedImage;
    QTransform m_imageTransform;
    std::shared_ptr<ScreenshotOcrPresentation> m_originalOcrPresentation;
    std::shared_ptr<ScreenshotOcrPresentation> m_displayOcrPresentation;
    std::shared_ptr<QTextDocument> m_formattedTextDocument;
    QString m_formattedPlainText;
    qreal m_formattedTextDevicePixelRatio = 1.0;
    ScreenshotClipboardOriginalContent m_originalClipboardContent;
    QPointer<ScreenshotOcrRecognitionPort> m_recognition;
    QPointer<ScreenshotQrRecognitionPort> m_qrRecognition;
    QPointer<SnowShotApiClient> m_tableRecognition;
    std::function<ScreenshotPinnedRecognitionProviders()> m_recognitionProvider;
    ScreenshotRecognitionResults m_recognitionResults;
    ScreenshotRecognitionWindow* m_recognitionContent = nullptr;
    QSize m_initialPhysicalSize;
    QSize m_originalPixelSize;
    QRect m_preThumbnailNativeGeometry;
    std::unique_ptr<ScreenshotPinnedNativeGeometryController> m_nativeGeometryController;
    std::unique_ptr<ScreenshotRecognitionSessionController> m_recognitionSession;
    double m_viewportZoom = 1.0;
    QPointF m_viewportCenter;
    double m_scalePercent = 100.0;
    QString m_mouseWheelZoomMode = QStringLiteral("mouse_position");
    int m_wheelAngleRemainder = 0;
    int m_opacityWheelAngleRemainder = 0;
    int m_opacityPercent = 100;
    int m_quarterTurns = 0;
    bool m_ocrReady = false;
    bool m_ocrSupported = false;
    bool m_formattedTextAvailable = false;
    bool m_ocrMode = false;
    bool m_translateAfterRecognition = false;
    bool m_automaticTextRecognition = true;
    bool m_editingEnabled = true;
    bool m_thumbnailMode = false;
    bool m_thumbnailAnimationTarget = false;
    bool m_geometryAnimating = false;
    bool m_preserveScaleForSettledGeometry = false;
    bool m_presented = false;
    bool m_closing = false;
    bool m_systemSizingActive = false;
    bool m_windowDragActive = false;
    bool m_windowDragCursorSet = false;
    bool m_pointerInside = false;
    bool m_passiveGeometryReconciliationActive = false;
    WId m_synchronizedResizeWindowId = 0;
    WId m_windowMoveCaptureId = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOW_H
