#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONSESSIONCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONSESSIONCONTROLLER_H

#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/app/edition.h"
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "snow_shot/translation/translationservice.h"
#endif
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/presentation/screenshotrecognitionresults.h"
#include "snow_shot/presentation/screenshotrecognitionfileexport.h"

#include <QObject>
#include <QJsonObject>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QStringList>
#include <QVector>
#include "snow_shot/storage/settingsadapters.h"

#include <functional>
#include <memory>

class QUrl;
class QMimeData;
class QTextDocument;
class QWidget;
class ScreenshotOcrPresentation;
class ScreenshotOcrTextEditingSession;
class ScreenshotRecognitionWindow;
class ScreenshotImageConversionController;
class ScreenshotTableEditingSession;
struct ScreenshotTableCommandState;

namespace adqt::widgets {
class AdModal;
}

struct ScreenshotRecognitionTarget {
    QString key;
    QImage image;
    QRectF canvasRect;
    std::shared_ptr<QTextDocument> formattedTextDocument{};
    QString formattedPlainText{};

    [[nodiscard]] bool isValid() const {
        return !key.isEmpty() && !image.isNull() && canvasRect.isValid() && !canvasRect.isEmpty();
    }

    [[nodiscard]] bool hasFormattedText() const {
        return formattedTextDocument != nullptr;
    }
};

struct ScreenshotRecognitionSessionActions {
    std::function<ScreenshotRecognitionWindow*()> ensureContent;
    std::function<void(std::shared_ptr<ScreenshotOcrPresentation>)> applyOcrPresentation;
    std::function<void(std::shared_ptr<ScreenshotOcrPresentation>)> applyOcrBackground;
    std::function<void(std::shared_ptr<QTextDocument>)> applyFormattedText;
    std::function<void()> clearOcrBackground;
    std::function<void(bool)> setRecognitionVisualState;
    std::function<void(int)> setActiveMode;
    std::function<void(bool, bool, bool, bool)> setTextEditingState;
    std::function<void(bool, bool, bool, bool, bool, bool, bool)> setTextTranslationState;
    std::function<void(bool, bool, bool, bool, bool, bool)> setTableEditingState;
    std::function<void(bool, bool, bool)> setBusyState;
    std::function<void()> hideLoading;
    std::function<void(const QString&, bool)> showStatus;
    std::function<QWidget*()> translationSettingsOwner;
    std::function<void(const QString&, const QString&)> setTextTransformState;
    // Split loading callbacks keep the model-download and recognition messages
    // addressable by separate keys.
    std::function<void(const QString&)> showModelDownload;
    std::function<void(const QString&)> showRecognition;
    std::function<void()> hideModelDownload;
    std::function<QColor()> ocrBackgroundColor;
    std::function<void(ScreenshotOcrRequest&)> prepareOcrRenderRequest;
    std::function<bool()> renderRecognitionInWorker;
    // The filtered image is a crop; filteredImageCanvasRect is the canvas-space
    // rect it covers and is invalid when the filtered region is empty.
    std::function<void(std::shared_ptr<ScreenshotOcrPresentation>, QImage, QRectF)>
        applyOcrBackgroundImage;
    std::function<void(int, const QString&)> updateOcrText;
    std::function<void(bool, bool, SnowShotImageConversionFormat)> setConversionState;
    std::function<void(bool)> setShowOriginalImage;
    std::function<void(const QString&)> setTextTargetLanguage;
};

class ScreenshotRecognitionSessionController final : public QObject {
    Q_OBJECT

  public:
    enum class Mode { Text = 0, Table = 1, Qr = 2, Markdown = 3, Html = 4, Latex = 5 };

    ScreenshotRecognitionSessionController(ScreenshotOcrRecognitionPort* recognition,
                                           ScreenshotQrRecognitionPort* qrRecognition,
                                           SnowShotApiClient* tableRecognition,
                                           ScreenshotRecognitionSessionActions actions,
                                           QObject* parent = nullptr);
    ~ScreenshotRecognitionSessionController() override;

    void setTarget(ScreenshotRecognitionTarget target);
    void setProviders(ScreenshotOcrRecognitionPort* recognition,
                      ScreenshotQrRecognitionPort* qrRecognition,
                      SnowShotApiClient* tableRecognition);
    void seedRecognitionResults(ScreenshotRecognitionResults results);
    [[nodiscard]] ScreenshotRecognitionResults cachedRecognitionResults() const;
    [[nodiscard]] ScreenshotRecognitionResults recognitionResultsSnapshot() const;
    [[nodiscard]] bool hasTarget() const;
    void prefetchText();
    void renderTextBackground();
    void activate(Mode mode);
    void deactivate();
    void invalidate();

    [[nodiscard]] bool active() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool busy(Mode mode) const;
    [[nodiscard]] Mode mode() const;
    [[nodiscard]] bool tableModeActive() const;
    [[nodiscard]] bool qrModeActive() const;
    [[nodiscard]] bool conversionModeActive() const;
    void openImageConversionSettings();

    void mergeTableSelection();
    void splitTableSelection();
    void resetTable();
    void undoTableEdit();
    void redoTableEdit();
    void undoTextEdit();
    void redoTextEdit();

    void beginTextEditing();
    void beginTextTranslation();
    [[nodiscard]] bool activateCachedTextTranslation();
    void synchronizeUiState() const;
    void endTextEditing();
    void openTranslationSettings();
    void resetTextEditing();
    void applyTextFormatting(const QString& value);
    void applyTextPunctuation(const QString& value);
    void applyTextTargetLanguage(const QString& language);
    [[nodiscard]] bool editing() const;
    [[nodiscard]] bool translating() const;
    [[nodiscard]] bool originalImageTranslationActive() const;
    // Image-based OCR presentation is visible and eligible for recognition-image export.
    [[nodiscard]] bool originalImageVisible() const;
    void setShowOriginalImage(bool show);
    [[nodiscard]] bool showOriginalImage() const {
        return m_showOriginalImage;
    }
    [[nodiscard]] bool hasTextResult() const;
    [[nodiscard]] QString textDraft() const;
    [[nodiscard]] QString sourceTextDraft() const;
    [[nodiscard]] QString originalText() const;
    [[nodiscard]] std::unique_ptr<QMimeData> recognitionClipboardMimeData(
        const ScreenshotOcrPresentation* displayedPresentation = nullptr) const;
    [[nodiscard]] std::optional<ScreenshotRecognitionFileSnapshot> fileExportSnapshot() const;
    void setTextDraft(const QString& text);
    [[nodiscard]] QJsonObject workflowState() const;
    [[nodiscard]] QJsonObject workflowResult() const;
    [[nodiscard]] bool editWorkflow(const QJsonObject& params);
    void cancelWorkflow();
    void handleTableCommandState(const ScreenshotTableCommandState& state);

  signals:
    void textEditingChanged(bool editing);
    void textResultChanged(bool available);
    void textDraftChanged(const QString& text);
    void recognitionResultsChanged();
    void workflowStateChanged() const;

  private:
    struct TextCacheEntry {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        enum class TranslationStatus { Absent, Streaming, Completed, Failed };
        struct OverlayTranslation {
            std::shared_ptr<ScreenshotOcrPresentation> presentation;
            TranslationStatus status = TranslationStatus::Absent;
            bool failureReported = false;
            bool captured = false;
        };
#endif
        ScreenshotOcrRecognitionResult recognitionResult;
        std::shared_ptr<ScreenshotOcrPresentation> presentation;
        std::shared_ptr<QTextDocument> formattedDocument;
        bool formatted = false;
        std::shared_ptr<ScreenshotOcrTextEditingSession> editingSession;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        std::shared_ptr<ScreenshotOcrTextEditingSession> translationSession;
        QPointer<snow_shot::translation::TranslationJob> translationJob;
        bool jobInImage = false;
        QString translationText;
        QString successfulTranslation;
        TranslationStatus translationStatus = TranslationStatus::Absent;
        bool hasSuccessfulTranslation = false;
#endif
        bool editing = false;
        bool defaultTransformsApplied = false;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        OverlayTranslation overlayTranslation;
        snow_shot::storage::ScreenshotTranslationConfiguration translationConfiguration;
        bool hasTranslationConfiguration = false;
#endif
    };

    [[nodiscard]] std::shared_ptr<ScreenshotOcrTextEditingSession>
    currentEditingSession(const TextCacheEntry& entry) const {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        if (m_translating)
            return entry.translationSession;
#endif
        return entry.editingSession;
    }

    void startTextRecognition(ScreenshotOcrRequestPriority priority);
    void startTextRender();
    void startTableRecognition();
    void startQrRecognition();
    void startLatexRecognition();
    void applyLatexContents(const QString& source);
    void handleLatexOutput(quint64 generation, const QString& key, SnowShotLatexResult result);
    void handleTextOutput(quint64 generation, const QString& key,
                          ScreenshotOcrRecognitionResult output);
    void handleTableOutput(quint64 generation, const QString& key, SnowShotTableResult result);
    void handleQrOutput(quint64 generation, const QString& key,
                        ScreenshotQrRecognitionResult result);
    void ensureContent();
    void clearContent();
    void applyPresentation(const std::shared_ptr<ScreenshotOcrPresentation>& presentation,
                           QImage filteredImage = {}, QRectF filteredImageCanvasRect = {});
    void applyFormattedText(const std::shared_ptr<QTextDocument>& document);
    void applyTableSession(const std::shared_ptr<ScreenshotTableEditingSession>& session);
    void applyQrContents(const QStringList& contents);
    void handleTextDocumentChanged(const QString& key);
    void handleTranslationDocumentChanged(const QString& key);
    void startTranslation();
    void prepareOverlayTranslation(TextCacheEntry& entry);
    void reportOverlayTranslationFailure();
    void failTranslationPreparation(const QString& message);
    void cancelTranslationRequests();
    void handleTranslationFinished(quint64 generation, const QString& key,
                                   SnowShotTranslationResult result);
    void invalidateCurrentTranslation(bool restartIfVisible);
    void updateBusyState() const;
    void updateConversionState() const;
    void updateConversionMessage();
    void updateTextState() const;
    void updateOriginalImagePreview() const;
    void updateTableState(const ScreenshotTableCommandState& state) const;
    void clearTextEditingState();
    [[nodiscard]] bool shouldRenderRecognitionInWorker() const;
    void setPendingTextRecognitionRendering(bool enabled);
    void pollTextModelDownload(quint64 generation);
    [[nodiscard]] bool textModelDownloading() const;
    void showModelDownloadMessage();
    void hideModelDownloadMessage();
    void showRecognitionMessage() const;
    void hideRecognitionMessage() const;
    void showStatus(const QString& message, bool error) const;
    void cancelOutstandingRequests();
    void resetTargetState();
    void handleRecognitionProviderDestroyed(Mode mode);
    [[nodiscard]] ScreenshotRecognitionWindow* content() const;

    QPointer<ScreenshotOcrRecognitionPort> m_recognition;
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    QPointer<ScreenshotQrRecognitionPort> m_qrRecognition;
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                    \
    SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    QPointer<SnowShotApiClient> m_tableRecognition;
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    QPointer<snow_shot::translation::TranslationService> m_translationService;
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    ScreenshotImageConversionController* m_conversion = nullptr;
    bool m_conversionMessageShown = false;
#endif
    ScreenshotRecognitionSessionActions m_actions;
    ScreenshotRecognitionTarget m_target;
    QPointer<ScreenshotRecognitionWindow> m_content;
    QHash<QString, TextCacheEntry> m_textCache;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    QHash<QString, std::shared_ptr<ScreenshotTableEditingSession>> m_tableCache;
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    QHash<QString, QStringList> m_qrCache;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    QHash<QString, SnowShotLatexResult> m_latexResults;
    SnowShotApiClient::RequestToken m_latexRequestToken = 0;
    quint64 m_latexGeneration = 0;
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    QHash<QString, SnowShotTableResult> m_tableResults;
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    QHash<QString, ScreenshotQrRecognitionResult> m_qrResults;
#endif
    std::shared_ptr<ScreenshotOcrPresentation> m_presentation;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    std::shared_ptr<ScreenshotTableEditingSession> m_tableSession;
#endif
    QString m_textCacheKey;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    QString m_tableCacheKey;
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    QString m_qrCacheKey;
    QStringList m_qrContents;
#endif
    QPointer<QTextDocument> m_textDocument;
    QString m_editingKey;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    QString m_translationKey;
#endif
    ScreenshotOcrRecognitionPort::RequestToken m_textRequestToken = 0;
    ScreenshotOcrRecognitionPort::RequestToken m_textRenderRequestToken = 0;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    SnowShotApiClient::RequestToken m_tableRequestToken = 0;
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    ScreenshotQrRecognitionPort::RequestToken m_qrRequestToken = 0;
#endif
    quint64 m_textGeneration = 0;
    quint64 m_textRenderGeneration = 0;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    quint64 m_tableGeneration = 0;
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    quint64 m_qrGeneration = 0;
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    quint64 m_translationGeneration = 0;
#endif
    mutable QString m_workflowError;
    Mode m_mode = Mode::Text;
    bool m_active = false;
    bool m_showOriginalImage = false;
    // "Shown" tracks the visible download prompt; "in progress" tracks that
    // asset acquisition is still pending for the in-flight text request, even
    // while only cache verification or helper start-up is running.
    bool m_textModelDownloadShown = false;
    bool m_textModelDownloadInProgress = false;
    bool m_editing = false;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    bool m_translating = false;
    bool m_translationInImage = false;
#else
    static constexpr bool m_translating = false;
    static constexpr bool m_translationInImage = false;
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    QPointer<adqt::widgets::AdModal> m_translationSettingsModal;
#endif
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONSESSIONCONTROLLER_H
