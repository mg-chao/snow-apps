#pragma once

#include "core/folder_sequence.h"
#include "core/image_types.h"
#include "decoding/image_loader.h"
#include "editing/edit_pipeline_controller.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <QThreadPool>
#include <QVector>

#include <memory>
#include <optional>

QT_BEGIN_NAMESPACE
class QCloseEvent;
class QByteArray;
class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QAction;
class QLabel;
class QPoint;
class QStackedWidget;
class QTimer;
class QWidget;
QT_END_NAMESPACE

namespace adqt::widgets {
class AdAlert;
class AdButton;
class AdContextMenu;
class AdModal;
} // namespace adqt::widgets

namespace snow::image_viewer {

class RhiImageWindow;
class WindowsBackgroundController;
class WindowsPrintController;
class WindowsShareController;
class EditSizeFormatWindow;

struct EditPerformanceOptions final {
    int iterations = 10;
    int warmupIterations = 5;
    double scale = 1.0;
    snow::image::Format format = snow::image::Format::unknown;
    bool lossless = false;
    bool preserveMetadata = false;
    bool forceCpu = false;
    bool rapidSuperseding = false;
};

class ViewerWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit ViewerWindow(QWidget* parent = nullptr);
    ViewerWindow(ImageLoader& startupLoader, const QString& initialImagePath,
                 QWidget* parent = nullptr);
    ~ViewerWindow() override;

    void openImage(const QString& filePath);
    void startEditModePerformanceTest(qint64 applicationStartupNanoseconds,
                                      EditPerformanceOptions options = {});

  signals:
    void editModePerformanceTestFinished(bool succeeded, const QByteArray& report);

  protected:
    bool event(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    friend class ViewerWindowTestAccess;

    enum class PixelReadbackPurpose {
        None,
        Clipboard,
        LockScreen,
        Wallpaper,
        Print,
    };

    void initializeWindow(const QString& initialImagePath);
    void attachInitialImageLoad(const QString& filePath);
    void buildInterface();
    void connectSignals();
    void prepareCanvasForLoading();
    void deferFolderSequenceLoad(const QString& filePath);
    void startPendingFolderSequenceLoad();
    void prefetchCurrentNeighbors();
    void chooseImage();
    void openPrevious();
    void openNext();
    void copyCurrentImage();
    void requestCurrentPixels(PixelReadbackPurpose purpose);
    void consumeCurrentPixels(PixelReadbackPurpose purpose, const QImage& pixels);
    void copyCurrentImagePath();
    void setCurrentImageAsLockScreenBackground();
    void setCurrentImageAsBackground();
    void setCurrentImageAsWindowsBackground(bool lockScreen);
    void setWindowsBackgroundFromPixels(bool lockScreen, const QImage& pixels);
    void printCurrentImage();
    void printPixels(const QImage& pixels);
    void shareCurrentImage();
    void openCurrentImageWith();
    void revealCurrentImage();
    void showImageContextMenu(const QPoint& globalPosition);
    void deleteCurrentImage();
    void openSizeFormatEditor();
    void closeSizeFormatEditor();
    void saveEditedImage();
    void positionSizeFormatEditor();
    void scheduleSizeFormatEditorReveal();
    void revealSizeFormatEditor();
    void showLoadError(const QString& path, const QString& message);
    void updateImageControls();
    void updateActualSizeButtonAction();
    void updateWindowTitle();
    void refreshTheme();
    void startCurrentAnimation();
    void stopCurrentAnimation();
    void advanceCurrentAnimation();
    void restoreSettings();
    void saveSettings() const;
    void recordPerformanceTiming(const QString& name, qint64 nanoseconds);
    void recordPerformanceMilestone(const QString& name, qint64 nanoseconds);
    void capturePerformanceScenarioSnapshot(int phase);
    void finishEditModePerformanceTest(bool succeeded, const QString& error = {});
    EditExportSettings activeEditSettings() const;
    void requestRapidPerformanceIteration();
    adqt::widgets::AdButton* makeIconButton(const QString& accessibleName,
                                            const QString& tooltipText, QWidget* parent);

    std::unique_ptr<ImageLoader> ownedLoader_;
    ImageLoader* loader_ = nullptr;
    QThreadPool folderSequencePool_;
    FolderSequence sequence_;
    DecodedImage currentImage_;
    DecodedImage editPreviewImage_;
    QString loadingFilePath_;
    QString desiredSequenceFilePath_;
    QString lastDirectory_;
    bool folderSequenceLoadInFlight_ = false;
    bool folderSequenceLoadAllowed_ = false;
    bool fitToWindowAction_ = false;
    bool revealInExplorerInFlight_ = false;
    bool windowsBackgroundChangeInFlight_ = false;
    bool windowsPrintInFlight_ = false;
    bool pixelReadbackInFlight_ = false;
    quint64 pixelReadbackRequest_ = 0;
    PixelReadbackPurpose pixelReadbackPurpose_ = PixelReadbackPurpose::None;
    bool editingActive_ = false;
    bool editWindowMinimizedWithViewer_ = false;
    bool editWindowRevealScheduled_ = false;
    bool performanceTestActive_ = false;
    bool performanceGpuPath_ = false;
    bool performanceCpuPixelsReleased_ = false;
    qint64 performanceEditStartNanoseconds_ = 0;
    qint64 performanceVisualRequestNanoseconds_ = 0;
    qint64 performanceEncodedBytes_ = 0;
    QElapsedTimer performanceTimer_;
    QMap<QString, QVector<qint64>> performanceWarmupTimings_;
    QMap<QString, QVector<qint64>> performanceColdTimings_;
    QMap<QString, QVector<qint64>> performanceTimings_;
    QMap<QString, QVector<qint64>> performanceArtifactHitTimings_;
    QMap<QString, QVector<qint64>> performancePreviewOnlyTimings_;
    QMap<QString, QVector<qint64>> performanceRapidTimings_;
    QMap<QString, qint64> performanceMilestones_;
    QMap<int, QJsonObject> performanceScenarioSnapshots_;
    EditRequestId performanceRequestId_ = 0;
    bool performanceReusedGpuPixels_ = false;
    ExactPreviewSource performancePreviewSource_ = ExactPreviewSource::base_raster;
    RasterProvenance performanceProvenance_ = RasterProvenance::cpu_reference;
    QString performanceAlphaContent_ = QStringLiteral("unknown");
    int performanceIterationsTarget_ = 10;
    int performanceWarmupTarget_ = 5;
    int performanceIterationsCompleted_ = 0;
    int performanceScenarioPhase_ = 0;
    int performanceResourceCacheHits_ = 0;
    int performanceResourceCacheMisses_ = 0;
    EditPerformanceOptions performanceOptions_;
    std::optional<EditExportSettings> performanceSettings_;
    int animationFrameIndex_ = 0;
    int completedAnimationLoops_ = 0;

    QWidget* root_ = nullptr;
    QWidget* bottomToolbar_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
    QWidget* emptyState_ = nullptr;
    QLabel* emptyIcon_ = nullptr;
    QTimer* animationTimer_ = nullptr;
    adqt::widgets::AdButton* emptyOpenButton_ = nullptr;
    QWidget* canvasPage_ = nullptr;
    QWidget* canvasContainer_ = nullptr;
    RhiImageWindow* rhiWindow_ = nullptr;
    adqt::widgets::AdAlert* errorAlert_ = nullptr;

    adqt::widgets::AdButton* actualSizeButton_ = nullptr;
    adqt::widgets::AdButton* zoomInButton_ = nullptr;
    adqt::widgets::AdButton* zoomOutButton_ = nullptr;
    adqt::widgets::AdButton* previousButton_ = nullptr;
    adqt::widgets::AdButton* nextButton_ = nullptr;
    adqt::widgets::AdButton* rotateLeftButton_ = nullptr;
    adqt::widgets::AdButton* rotateRightButton_ = nullptr;
    adqt::widgets::AdButton* deleteButton_ = nullptr;
    adqt::widgets::AdButton* editButton_ = nullptr;
    EditSizeFormatWindow* editWindow_ = nullptr;
    EditPipelineController* editSession_ = nullptr;
    bool saveAfterArtifact_ = false;
    adqt::widgets::AdModal* deleteConfirm_ = nullptr;
    adqt::widgets::AdContextMenu* imageContextMenu_ = nullptr;
    adqt::widgets::AdContextMenu* setAsMenu_ = nullptr;
    QAction* copyImageAction_ = nullptr;
    QAction* copyImagePathAction_ = nullptr;
    QAction* setAsLockScreenAction_ = nullptr;
    QAction* setAsBackgroundAction_ = nullptr;
    QAction* printImageAction_ = nullptr;
    QAction* shareImageAction_ = nullptr;
    QAction* openWithAction_ = nullptr;
    QAction* revealInExplorerAction_ = nullptr;
    QAction* deleteImageAction_ = nullptr;
    std::unique_ptr<WindowsBackgroundController> windowsBackgroundController_;
    std::unique_ptr<WindowsPrintController> windowsPrintController_;
    std::unique_ptr<WindowsShareController> windowsShareController_;
};

} // namespace snow::image_viewer
