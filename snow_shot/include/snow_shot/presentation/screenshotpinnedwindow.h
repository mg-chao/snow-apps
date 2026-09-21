#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOW_H

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotrecognitionresults.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/storage/pinnedwindowtypes.h"
#include "snow_shot/storage/pinnedwindowplacement.h"

#include <QByteArray>
#include "snow_shot/presentation/mousereleaseactioncontroller.h"

#include <QColor>
#include <QImage>
#include <QMap>
#include <QMetaObject>
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
class AdSlider;
} // namespace adqt::widgets
namespace snow_shot::presentation {
class WindowShortcutManager;
class PinnedWindowGroupManager;
class PinnedWindowPlatform;
} // namespace snow_shot::presentation
namespace snow_shot::platform {
class PhysicalCursor;
enum class PhysicalCursorDirection;
} // namespace snow_shot::platform

class QAction;
class QActionGroup;
class QFrame;
class QLabel;
class CanvasStatusReadout;
class QCloseEvent;
class QContextMenuEvent;
class QEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
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
class ScreenshotFloatingToolPaletteWindow;
class ScreenshotExportArtifact;
class ScreenshotPinnedHideToTopController;
class ScreenshotPinnedPointerPresence;
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
    QRectF autoFilterSourceBounds() const {
        return m_backgroundCanvasRect;
    }
    void requestAutoFilterSource(std::function<void(QImage)> completion);
    struct Config {
        snow_shot::storage::PinnedWindowPlacement placement;
        // Pixel rectangle scoped to screen, for capture/export geometry adapters.
        QRect nativeGeometry;
        QRectF canvasSourceRect;
        QRectF contentCanvasRect;
        QRectF surfaceCanvasRect;
        ScreenshotResultStyle resultStyle;
        QSize initialWindowSize;
        QString mouseWheelZoomMode = QStringLiteral("mouse_position");
        ScreenshotImageSource imageSource;
        ScreenshotImageLoader imageLoader;
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
        bool recognitionVisible = false;
        bool translationVisible = false;
        QString persistenceId;
        bool restorePersistentState = false;
        double persistedFirstCreationTextDpi = 1.0;
        int persistedOpacityPercent = 100;
        int persistedClickThroughOpacityPercent = 50;
        QTransform persistedImageTransform;
        int persistedQuarterTurns = 0;
        bool persistedHideToTopMode = false;
        QRect persistedHideToTopHandleNativeGeometry;
        int persistedHideToTopAccentIndex = -1;
        bool persistedThumbnailMode = false;
        bool persistedClickThroughMode = false;
        bool persistedAlwaysOnTop = true;
        bool persistedShowBorder = true;
        QRect persistedPreThumbnailNativeGeometry;
        snow_shot::storage::PinnedWindowPlacement persistedPreThumbnailPlacement;
        QByteArray persistedCanvasSession;
        QByteArray persistedRecognitionResults;
        bool persistedRecognitionVisible = false;
        bool persistedTranslationVisible = false;
        std::function<void(const snow_shot::storage::PinnedWindowRecord&)> persistenceWriter;
        std::function<void(const snow_shot::storage::PinnedWindowRecord&)>
            replacementPersistenceWriter;
        std::function<void(const QString&)> persistenceRemover;
        snow_shot::presentation::PinnedWindowGroupManager* groupManager = nullptr;
        QString groupId = QStringLiteral("default");
    };

    explicit ScreenshotPinnedWindow(QWidget* parent = nullptr);
    ~ScreenshotPinnedWindow() override;

    bool present(const Config& config, std::function<void(bool, QImage)> completion = {});
    bool prewarm(QScreen* screen = nullptr);
    QRect currentNativeGeometry() const;
    [[nodiscard]] snow_shot::storage::PinnedWindowRecord persistenceSnapshot() const;
    [[nodiscard]] QString persistenceId() const {
        return m_persistenceId;
    }
    [[nodiscard]] QString groupId() const {
        return m_groupId;
    }

  public slots:
    void setGroupId(const QString& id);
    void closeForInactiveGroup();
    void cancelDeferredInactiveGroupClose();

  public:
    static void setRuntimeBorderColor(const QColor& color);
    static void setRuntimeBorderActiveColor(const QColor& color);
    static void setRuntimeTrayEnabled(bool enabled);

  signals:
    void showMainWindowRequested();
    void closingForPersistence(const snow_shot::storage::PinnedWindowRecord& snapshot,
                               bool removalRequested);

  private:
    friend class ScreenshotPinnedEditController;
    friend class ScreenshotPinnedWindowTestAccess;
    friend class PinnedWindowWindowsEvents;

    enum class GeometryMutation {
        Move,
        Scale,
        ImageTransform,
        Thumbnail,
        Animation,
        HideToTop,
        ContentReplacement,
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
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    [[nodiscard]] QStringList eligibleDropPaths(const QDropEvent& event) const;
    void setFileDragActive(bool active);

    void createUi();
    void registerWindowShortcuts();
    void reloadPinnedWindowShortcuts();
    void createContextMenu();
    void rebuildGroupMenu();
    void refreshContextMenuForGroup(const QString& groupId);
    void deleteIfInGroup(const QString& groupId);
    void applyRuntimeBorderColor();
    void updateShowMainInterfaceAction();
    void retranslateUi();
    void refreshContextMenu();
    void showContextMenu(const QPoint& globalPosition);
    void updateCanvasViewport();
    void updateControlsGeometry();
    // Single writer for hover presence: the live native cursor against the
    // complete window frame. Native mouse messages, queued Enter/Leave, and
    // passive geometry settlement all resolve through this. Returns false when
    // the native query is unavailable so the caller can fall back to
    // event-derived presence.
    bool applyNativePointerPresence();
    void schedulePointerPresence(bool inside);
    void destroyCanvas();
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
    void configureEditToolbar(ScreenshotFloatingToolPaletteWindow* toolbarWindow);
    void setEditMode(bool enabled);
    void stopRecognition();
    void configureRecognitionSession();
    ScreenshotRecognitionWindow* ensureRecognitionContent();
    void synchronizeHiddenTextSelection();
    [[nodiscard]] bool copyHiddenTextSelection();
    void updateOcrPresentation();
    void updateRecognitionContentGeometry();
    void activateRecognitionMode(int mode, bool showToolbar = true);
    void ensureRecognitionProviders();
    void deactivateRecognition();
    void updateRecognitionToolbarState();
    [[nodiscard]] bool recognitionModeAvailable(int mode) const;
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
    void loadImageFile();
    void loadClipboardContent();
    void requestContentReplacement(QStringList paths,
                                   std::optional<ScreenshotClipboardContentSnapshot> snapshot = {});
    bool replaceContent(ScreenshotClipboardContent content);
    void cancelContentReplacement();
    void saveAsFile();
    [[nodiscard]] std::shared_ptr<ScreenshotExportArtifact> fileSaveArtifact();
    void quickSave();
    void invalidatePendingCopy();
    void applyImageOperation(const QTransform& operation, int quarterTurnDelta = 0);
    void resetImageTransform();
    void rebuildTransformedImage();
    void applyScale(int percent);
    void applyWheelScale(double percent, const QPointF& nativeCursor);
    void applyWheelScaleSteps(int steps, const QPointF& nativeCursor);
    bool handleOpacityWheel(QObject* watched, QWheelEvent* event);
    bool handleScaleWheel(QObject* watched, QWheelEvent* event);
    QSize orientedInitialWindowSize() const;
    QRect logicalRectForNativeRect(const QRect& nativeRect) const;
    void setEffectiveScale(double percent, bool showReadout);
    void showScaleReadout();
    void showOpacityReadout();
    void scheduleNativeScaleAdoption();
    void adoptSettledNativeScale();
    void setOpacityPercent(int percent);
    void schedulePersistence();
    void persistNow();
    void removePersistence();
    [[nodiscard]] snow_shot::storage::PinnedWindowRecord persistenceRecord() const;
    void restorePersistentState(const Config& config);
    [[nodiscard]] QRect intendedNativeGeometry() const;
    [[nodiscard]] QRect authoritativeNativeGeometry() const;
    [[nodiscard]] QRect observedNativeGeometry() const;
    void toggleHideToTop();
    void exitHideToTop();
    [[nodiscard]] bool hideToTopActive() const;
    [[nodiscard]] bool setClickThroughMode(bool enabled);
    void toggleClickThrough();
    void setAlwaysOnTop(bool enabled);
    void toggleAlwaysOnTop();
    void setShowBorder(bool enabled);
    void toggleShowBorder();
    [[nodiscard]] bool ensureClickThroughExitButton();
    [[nodiscard]] bool updateClickThroughExitButtonGeometry();
    void setClickThroughScreen(QScreen* screen);
    void shutdownClickThrough();
    void applyEffectiveOpacity();
    void setClickThroughOpacityPercent(int percent);
    void updateThumbnailPresentation();
    void setThumbnailMode(bool enabled, bool animate = true);
    void restoreFromThumbnailImmediately();
    void animateGeometryTo(const QRect& nativeTarget);
    bool applyWindowGeometry(const QRect& nativeGeometry, GeometryMutation mutation);
    bool applyAndVerifyNativeGeometry(const QRect& target, bool discardContents = false);
    void commitNativeGeometry(bool adoptScale = false);
    void handleNativeGeometryObservation();
    bool finishNativeGeometryInteraction();
    bool reconcilePassiveNativeGeometry();
    bool restoreCommittedNativeGeometry(bool closeOnFailure = true);
    void showAllPinnedWindows();
    void hideOtherPinnedWindows();
    void closeOtherPinnedWindows();
    void closeAllPinnedWindows();
    void requestUserClose();
    [[nodiscard]] std::optional<QPoint> physicalCursorPosition() const;
    bool cursorMovementEnabled() const;
    bool moveCursorOnePixel(snow_shot::platform::PhysicalCursorDirection direction);
    bool startWindowMove();
    void finishWindowMove();
    bool windowDragEnabled() const;
    bool windowDragEnabledAt(const QPoint& position) const;
    bool handleDoubleClick(const QPoint& position);
    bool handleMiddleClick(const QPoint& position);
    void updateWindowDragCursor(const QPoint& position);
    void setWindowDragCursor(Qt::CursorShape shape);
    void clearWindowDragCursor();
    bool nativeTrackSizeConstraintsEnabled() const;
    bool interactiveResizingEnabled() const;
    QPointF windowPositionForEvent(QObject* watched, const QPointF& position) const;
    QPointF nativePositionForWindowPosition(const QPointF& position) const;
    QPoint globalPositionForNativePosition(const QPoint& position) const;
    bool isControlsPanelPosition(const QPoint& position) const;

    void reconcilePlatformEnvironment(bool layoutChanged = false);
    bool handleControlledPointer(QObject* watched, QEvent* event);
    void resetPinnedGestures();
    bool handlePinnedGesture(QObject* watched, QEvent* event);
    bool beginControlledInteraction(const QPointF& desktopPosition,
                                    std::optional<int> resizeHandle);
    void updateControlledInteraction(const QPointF& desktopPosition);
    void endControlledInteraction(bool cancel);
    std::unique_ptr<snow_shot::presentation::PinnedWindowPlatform> m_platform;
    bool m_platformReconciliationPending = false;
    bool m_platformRecoveryPending = false;
    bool m_platformApplying = false;
    bool m_nativeRestoreInFlight = false;
    std::optional<snow_shot::storage::PinnedWindowPlacement> m_platformPlacement;
    std::optional<snow_shot::storage::PinnedWindowPlacement> m_interactionPlacement;
    QPointF m_interactionPointer;
    QPointF m_interactionAnchor;
    std::optional<int> m_interactionResizeHandle;
    QPointer<QWidget> m_interactionGrabber;
    int m_scrollWheelRemainder = 0;
    int m_scrollWheelDirection = 0;
    int m_scrollWheelStepDelta = 0;
    quint64 m_scrollWheelTimestamp = 0;
    double m_scrollOpacity = 0;
    bool m_pinchActive = false;
    bool m_controlledEscapeRelease = false;
    SnowCanvasRuntime m_runtime;
    std::unique_ptr<snow_shot::presentation::WindowShortcutManager> m_shortcutManager;
    snow_shot::presentation::MouseReleaseActionController m_mouseReleaseAction;
    std::unique_ptr<snow_shot::platform::PhysicalCursor> m_physicalCursor;
    QMap<QString, quint64> m_pinnedShortcutBindings;
    std::shared_ptr<ScreenshotExportArtifact> m_exportArtifact;
    ScreenshotExportJobHandle m_materializationJob;
    ScreenshotExportJobHandle m_contentReplacementJob;
    quint64 m_contentReplacementGeneration = 0;
    ScreenshotExportJobHandle m_fileSaveJob;
    std::shared_ptr<ScreenshotExportArtifact> m_quickSaveArtifact;
    bool m_quickSavePending = false;
    ScreenshotClipboardCommitHandle m_clipboardCommit;
    std::vector<MaterializationCallback> m_materializationCallbacks;
    PresentationCompletion m_presentationCompletion;
    ScreenshotImageLoader m_imageLoader;
    bool m_materializationLoading = false;
    bool m_firstContentFramePublished = false;
    bool m_firstFramePaintPending = false;
    bool m_firstFramePaintSucceeded = true;
    bool m_deferFirstFrameNativeFlush = false;
    bool m_completePresentationAfterFirstFrame = false;
    bool m_recognitionTargetReady = false;
    bool m_deferredPresentationSetupScheduled = false;
    quint64 m_presentationGeneration = 0;
    SnowCanvasWidget* m_canvas = nullptr;
    std::unique_ptr<ScreenshotCanvasRenderer> m_screenshotRenderer;
    QFrame* m_borderFrame = nullptr;
    QFrame* m_controlsPanel = nullptr;
    CanvasStatusReadout* m_scaleLabel = nullptr;
    QTimer* m_scaleLabelTimer = nullptr;
    bool m_scaleReadoutShowsOpacity = false;
    QTimer* m_nativeScaleSettleTimer = nullptr;
    ScreenshotPinnedEditController* m_editController = nullptr;
    adqt::widgets::AdButton* m_editButton = nullptr;
    adqt::widgets::AdButton* m_closeButton = nullptr;
    std::unique_ptr<adqt::widgets::AdButton> m_clickThroughMoveButton;
    std::optional<QPoint> m_clickThroughDragOrigin;
    QRect m_clickThroughDragGeometry;
    std::unique_ptr<adqt::widgets::AdButton> m_clickThroughExitButton;
    std::unique_ptr<QWidget> m_clickThroughOpacityEditor;
    adqt::widgets::AdSlider* m_clickThroughOpacitySlider = nullptr;
    adqt::widgets::AdContextMenu* m_contextMenu = nullptr;
    adqt::widgets::AdContextMenu* m_groupMenu = nullptr;
    adqt::widgets::AdContextMenu* m_deleteSpecifiedGroupMenu = nullptr;
    QAction* m_ocrAction = nullptr;
    QAction* m_drawingAction = nullptr;
    QAction* m_thumbnailAction = nullptr;
    QAction* m_hideToTopAction = nullptr;
    QAction* m_clickThroughAction = nullptr;
    QAction* m_alwaysOnTopAction = nullptr;
    QAction* m_showBorderAction = nullptr;
    QAction* m_showMainInterfaceAction = nullptr;
    QAction* m_closeAction = nullptr;
    QAction* m_loadContentAction = nullptr;
    QActionGroup* m_opacityActions = nullptr;
    QActionGroup* m_scaleActions = nullptr;
    QAction* m_scaleMenuAction = nullptr;
    QAction* m_opacityReadoutAction = nullptr;
    QAction* m_scaleReadoutAction = nullptr;
    std::unique_ptr<ScreenshotPinnedHideToTopController> m_hideToTop;
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
    // Scale is 100 * current window width / oriented initial window width.
    // Window units are logical pixels on macOS and physical pixels on Windows;
    // source image density and formatted-text DPR affect rendering only.
    QSize m_initialWindowSize;
    QSize m_originalPixelSize;
    QRect m_preThumbnailNativeGeometry;
    snow_shot::storage::PinnedWindowPlacement m_preThumbnailPlacement;
    std::unique_ptr<ScreenshotPinnedNativeGeometryController> m_nativeGeometryController;
    std::function<void(const snow_shot::storage::PinnedWindowRecord&)> m_persistenceWriter;
    std::function<void(const snow_shot::storage::PinnedWindowRecord&)>
        m_replacementPersistenceWriter;
    std::function<void(const QString&)> m_persistenceRemover;
    QPointer<snow_shot::presentation::PinnedWindowGroupManager> m_groupManager;
    std::unique_ptr<ScreenshotRecognitionSessionController> m_recognitionSession;
    double m_viewportZoom = 1.0;
    QPointF m_viewportCenter;
    bool m_synchronizingViewportGeometry = false;
    // Derived value: 100 * expanded native width / oriented initial window
    // width. In thumbnail mode the saved expansion rectangle supplies that
    // width. Never carries an externally computed or DPI-translated percent.
    double m_scalePercent = 100.0;
    QString m_mouseWheelZoomMode = QStringLiteral("mouse_position");
    int m_wheelAngleRemainder = 0;
    int m_opacityWheelAngleRemainder = 0;
    int m_opacityPercent = 100;
    int m_clickThroughOpacityPercent = 50;
    int m_quarterTurns = 0;
    bool m_ocrReady = false;
    bool m_ocrSupported = false;
    bool m_formattedTextAvailable = false;
    bool m_ocrMode = false;
    bool m_hiddenTextSelection = false;
    bool m_initialRecognitionVisible = false;
    bool m_initialTranslationVisible = false;
    bool m_translateAfterRecognition = false;
    bool m_automaticTextRecognition = true;
    bool m_editingEnabled = true;
    bool m_thumbnailMode = false;
    bool m_clickThroughActive = false;
    bool m_alwaysOnTop = true;
    bool m_showBorder = true;
    bool m_geometryAnimating = false;
    bool m_preserveScaleForSettledGeometry = false;
    bool m_presented = false;
    bool m_closing = false;
    bool m_deferredInactiveGroupClose = false;
    bool m_inactiveGroupClosing = false;
    QString m_persistenceId;
    QString m_groupId = QStringLiteral("default");
    bool m_persistenceEnabled = true;
    bool m_persistenceRemovalRequested = false;
    qreal m_firstCreationTextDpi = 1.0;
    QTimer* m_persistenceTimer = nullptr;
    bool m_systemSizingActive = false;
    bool m_windowDragActive = false;
    bool m_windowDragCursorSet = false;
    bool m_pointerInside = false;
    QPointer<QScreen> m_clickThroughScreen;
    QMetaObject::Connection m_clickThroughScreenGeometryConnection;
    QMetaObject::Connection m_clickThroughScreenDpiConnection;
    std::unique_ptr<ScreenshotPinnedPointerPresence> m_pointerPresence;
    bool m_windowActive = false;
    bool m_fileDragActive = false;
    bool m_passiveGeometryReconciliationActive = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDWINDOW_H
