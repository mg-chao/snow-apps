#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTUISERVICES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTUISERVICES_H

#include "snow_shot/storage/pinnedwindowtypes.h"
#include "snow_shot/storage/storageresult.h"

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"

#include <atomic>
#include <QSet>
#include <QHash>
#include <QPointer>
#include <QStringList>
#include <functional>
#include <memory>

class QScreen;
class ScreenshotOcrRecognitionPort;
class ScreenshotQrRecognitionPort;
class SnowShotApiClient;
class ScreenshotPinnedWindowPool;
class ScreenshotPinnedWindow;
class ScreenshotPendingPinCoordinator;
class QTextDocument;
struct ScreenshotPinnedRecognitionProviders;
struct ScreenshotHistoryEntry;

struct ScreenshotAutomaticSaveRequest final {
    QPointer<QObject> receiver;
    QString configuredSaveDirectory;
    ScreenshotImageFileFormat format = ScreenshotImageFileFormat::Png;
    QString filenameFormat;
    ScreenshotImageEncodingOptions encoding;
    ScreenshotPdfOptions pdf;
    QDateTime requestedAt;
    ScreenshotExportCoordinator::Completion completion;
    snow_shot::storage::ScreenshotWindowNames windowNames{};
};

namespace snow_shot::presentation {
class LiveWindowSource;
class PinnedWindowGroupManager;
class PinnedWindowSelectionController;
} // namespace snow_shot::presentation

class ScreenshotSelectionExportUiServices final : public ScreenshotSelectionExportDestinationPort {
  public:
    explicit ScreenshotSelectionExportUiServices(
        ScreenshotOcrRecognitionPort* recognition = nullptr,
        ScreenshotQrRecognitionPort* qrRecognition = nullptr,
        SnowShotApiClient* tableRecognition = nullptr,
        std::function<void()> showMainWindowRequested = {},
        std::function<ScreenshotPinnedRecognitionProviders()> recognitionProvider = {},
        snow_shot::presentation::PinnedWindowGroupManager* groupManager = nullptr,
        std::function<void(const QVector<ScreenshotPinnedImageSnapshot>&)> imageStitchingRequested =
            {},
        std::function<void(const ScreenshotPinnedImageSnapshot&)> imageSplittingRequested = {},
        std::function<void(const QString&)> imageEditingRequested = {});
    ~ScreenshotSelectionExportUiServices() override;

    [[nodiscard]] bool publishClipboard(QObject* receiver, ScreenshotClipboardPayload payload,
                                        ClipboardCompletion completion) override;
    [[nodiscard]] bool publishClipboard(QObject* receiver, ScreenshotClipboardPayload payload,
                                        ClipboardCompletion completion,
                                        quint64 publicationId) override;
    void cancelClipboardPublication();
    // Prepares one hidden native shell for the next Pin to Screen presentation.
    void prewarmPinnedWindow(QScreen* screen = nullptr);
    [[nodiscard]] bool
    presentLiveWindow(std::shared_ptr<snow_shot::presentation::LiveWindowSource> source,
                      QScreen* screen, const QRect& nativeGeometry, const QSize& initialWindowSize,
                      PinnedCompletion completion = {});
    // Fits decoded file or drop content on the target screen, retaining its text document.
    [[nodiscard]] bool presentDecodedContentOnScreen(
        ScreenshotClipboardContent content, QScreen* screen, bool autoResizeWindow,
        snow_shot::storage::PinnedWindowCreationSource source, bool initialAnnotationMode = false,
        PinnedCompletion completion = {});
    // A null image is accepted when imageLoader is provided and
    // initialWindowSize supplies the known canvas dimensions.
    [[nodiscard]] bool presentPinnedImage(
        const QImage& image, QScreen* screen, const QRect& nativeGeometry,
        const QSize& initialWindowSize = {},
        std::shared_ptr<QTextDocument> formattedTextDocument = {},
        const QString& formattedPlainText = {}, qreal formattedTextDevicePixelRatio = 1.0,
        ScreenshotClipboardOriginalContent originalContent = {},
        ScreenshotImageLoader imageLoader = {}, PinnedCompletion completion = {},
        std::optional<snow_shot::storage::PinnedBorderAppearance> borderAppearance = {},
        std::optional<bool> checkerboardEnabled = {},
        snow_shot::storage::PinnedWindowCreationSource source =
            snow_shot::storage::PinnedWindowCreationSource::Other,
        snow_shot::storage::PinnedSourceIdentity sourceIdentity = {},
        std::optional<bool> initialBorderVisible = {},
        std::optional<bool> initialShadowVisible = {},
        std::shared_ptr<const ScreenshotAnimationSource> animationSource = {},
        bool initialAnnotationMode = false, QImage preparedDisplayImage = {});
    // An already composited selection bitmap placed by screenshotSelectionPinRequest.
    [[nodiscard]] bool
    presentCompositedSelectionImage(const QImage& image,
                                    const ScreenshotPinnedSelectionRequest& request,
                                    PinnedCompletion completion = {});
    [[nodiscard]] bool presentPinnedSelection(const ScreenshotPinnedSelectionRequest& request,
                                              ScreenshotPinnedSelectionResultHandle result,
                                              PinnedCompletion completion) override;
    [[nodiscard]] bool presentPinnedDocument(const QImage& background, QScreen* screen,
                                             const QRect& nativeGeometry,
                                             const ScreenshotHistoryEntry& entry,
                                             PinnedCompletion completion);
    [[nodiscard]] bool
    presentPinnedArtifact(const ScreenshotPinnedSelectionRequest& request,
                          std::shared_ptr<ScreenshotExportArtifact> artifact,
                          PinnedCompletion completion = {},
                          std::optional<ScreenshotAutomaticSaveRequest> automaticSave = {});
    [[nodiscard]] bool
    presentPinnedImageArtifact(std::shared_ptr<ScreenshotExportArtifact> artifact, QScreen* screen,
                               const QRect& nativeGeometry, const QSize& initialWindowSize,
                               PinnedCompletion completion = {},
                               std::optional<ScreenshotAutomaticSaveRequest> automaticSave = {});
    void restorePersistedWindows();
    // Returns whether restoration was queued; completion and failures are asynchronous.
    bool restoreRecord(const QString& id, bool activateGroup = true);
    // Validates the whole selection before activating its common group once.
    // Mixed groups or missing records are rejected without partial restoration.
    bool restoreRecords(const QStringList& ids);
    void restoreLastClosedWindow();
    void showAllWindows();
    bool hideOtherWindows(const QString& exceptId);
    bool toggleAllWindowsVisibility();
    void setVisibilityFailureHandler(std::function<void()> handler) {
        m_visibilityFailure = std::move(handler);
    }
    [[nodiscard]] ScreenshotPinnedWindow*
    findDuplicatePin(const snow_shot::storage::PinnedSourceIdentity& identity) const;
    [[nodiscard]] QSet<QString> duplicateSourceKeys() const;
    // A true result consumes the request. The caller owns one restore guard per action/batch.
    bool handleDuplicatePin(const snow_shot::storage::PinnedSourceIdentity& identity,
                            const QString& action, bool& restored);

    void setRestoreFailureHandler(std::function<void()> handler) {
        m_restoreFailure = std::move(handler);
    }
    void setLiveImageEditingHandler(std::function<void(const ScreenshotExportSource&)> handler) {
        m_liveImageEditingRequested = std::move(handler);
    }
    void destroyRecords(const QVector<QString>& ids);
    [[nodiscard]] snow_shot::storage::StorageResult tryDestroyRecords(const QVector<QString>& ids);

  private:
    void configureVisibility(ScreenshotPinnedWindow::Config* config);
    bool persistHiddenWindow(const snow_shot::storage::PinnedWindowRecord& record);
    bool hideRecords(const QVector<QString>& ids);
    [[nodiscard]] ScreenshotPinnedWindow::HideOtherWindowsResult
    hideOtherWindowsWithResult(const QString& exceptId);
    void cancelRestore(const QString& id);
    [[nodiscard]] ScreenshotPinnedWindow* liveWindow(const QString& id) const;
    [[nodiscard]] QString
    hiddenDuplicateId(const snow_shot::storage::PinnedSourceIdentity& identity) const;
    void trackSourceWindow(ScreenshotPinnedWindow* window,
                           const snow_shot::storage::PinnedSourceIdentity& identity);
    [[nodiscard]] bool presentRestoredRecord(snow_shot::storage::PinnedWindowRecord record,
                                             QImage initialPreparedAnimationFrame = {},
                                             QTransform initialPreparedAnimationTransform = {});
    [[nodiscard]] bool presentPinnedImageOnCanvas(
        const QImage& image, QScreen* screen, const QRect& nativeGeometry,
        const QSize& initialWindowSize, const QRectF& canvasRect,
        std::shared_ptr<QTextDocument> formattedTextDocument, const QString& formattedPlainText,
        qreal formattedTextDevicePixelRatio, ScreenshotClipboardOriginalContent originalContent,
        ScreenshotImageLoader imageLoader, PinnedCompletion completion,
        std::optional<snow_shot::storage::PinnedBorderAppearance> borderAppearance = {},
        std::optional<bool> checkerboardEnabled = {},
        snow_shot::storage::PinnedWindowCreationSource source =
            snow_shot::storage::PinnedWindowCreationSource::Other,
        const ScreenshotHistoryEntry* document = nullptr,
        snow_shot::storage::PinnedSourceIdentity sourceIdentity = {},
        std::optional<bool> initialBorderVisible = {},
        std::optional<bool> initialShadowVisible = {},
        std::shared_ptr<const ScreenshotAnimationSource> animationSource = {},
        bool initialAnnotationMode = false, QImage preparedDisplayImage = {});

    QHash<QString, QList<QPointer<ScreenshotPinnedWindow>>> m_sourceWindows;
    std::function<void()> m_restoreFailure;
    std::function<void()> m_visibilityFailure;
    struct RestoringPin {
        snow_shot::storage::PinnedSourceIdentity sourceIdentity;
        QString groupId;
        QDateTime createdUtc;
        bool attentionPending = false;
        quint64 generation = 0;
        QPointer<ScreenshotPinnedWindow> window;
    };
    QHash<QString, RestoringPin> m_restoringIds;
    quint64 m_nextRestoreGeneration = 0;
    QSet<QString> m_pendingAttention;
    std::shared_ptr<std::atomic_bool> m_restoreAlive = std::make_shared<std::atomic_bool>(true);
    ScreenshotOcrRecognitionPort* m_recognition = nullptr;
    ScreenshotQrRecognitionPort* m_qrRecognition = nullptr;
    SnowShotApiClient* m_tableRecognition = nullptr;
    std::function<void()> m_showMainWindowRequested;
    std::function<void(const ScreenshotPinnedImageSnapshot&)> m_imageSplittingRequested;
    std::function<void(const QString&)> m_imageEditingRequested;
    std::function<void(const ScreenshotExportSource&)> m_liveImageEditingRequested;
    std::function<ScreenshotPinnedRecognitionProviders()> m_recognitionProvider;
    snow_shot::presentation::PinnedWindowGroupManager* m_groupManager = nullptr;
    std::unique_ptr<snow_shot::presentation::PinnedWindowSelectionController> m_selectionController;
    std::unique_ptr<ScreenshotPinnedWindowPool> m_windowPool;
    std::unique_ptr<ScreenshotPendingPinCoordinator> m_pendingPinCoordinator;
    ScreenshotClipboardCommitScope m_clipboardScope;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTUISERVICES_H
