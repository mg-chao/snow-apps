#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONSESSIONCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONSESSIONCONTROLLER_H

#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"

#include <QObject>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QStringList>
#include "snow_shot/storage/settingsadapters.h"

#include <functional>
#include <memory>

class QUrl;
class QTextDocument;
class QWidget;
class ScreenshotOcrPresentation;
class ScreenshotOcrTextEditingSession;
class ScreenshotRecognitionWindow;
class ScreenshotTableEditingSession;
struct ScreenshotTableCommandState;

namespace adqt::widgets {
class AdModal;
}

struct ScreenshotRecognitionTarget {
    QString key;
    QImage image;
    QRectF canvasRect;
    std::shared_ptr<QTextDocument> formattedTextDocument;
    QString formattedPlainText;

    [[nodiscard]] bool isValid() const {
        return !key.isEmpty() && !image.isNull() && canvasRect.isValid() &&
               !canvasRect.isEmpty();
    }

    [[nodiscard]] bool hasFormattedText() const {
        return formattedTextDocument != nullptr;
    }
};

struct ScreenshotRecognitionSessionActions {
    std::function<ScreenshotRecognitionWindow*()> ensureContent;
    std::function<void()> destroyContent;
    std::function<void(std::shared_ptr<ScreenshotOcrPresentation>)> applyOcrPresentation;
    std::function<void(std::shared_ptr<ScreenshotOcrPresentation>)> applyOcrBackground;
    std::function<void(std::shared_ptr<QTextDocument>)> applyFormattedText;
    std::function<void()> clearOcrBackground;
    std::function<void(bool)> setRecognitionVisualState;
    std::function<void(int)> setActiveMode;
    std::function<void(bool, bool, bool, bool)> setTextEditingState;
    std::function<void(bool, bool, bool, bool, bool, bool)> setTextTranslationState;
    std::function<void(bool, bool, bool, bool, bool, bool)> setTableEditingState;
    std::function<void(bool, bool, bool)> setBusyState;
    std::function<void(const QString&)> showLoading;
    std::function<void()> hideLoading;
    std::function<void(const QString&, bool)> showStatus;
    std::function<void(const QUrl&)> handleQrLink;
    std::function<QWidget*()> translationSettingsOwner;
    std::function<void(const QString&, const QString&)> setTextTransformState;
};

class ScreenshotRecognitionSessionController final : public QObject {
    Q_OBJECT

  public:
    enum class Mode { Text = 0, Table = 1, Qr = 2 };

    ScreenshotRecognitionSessionController(ScreenshotOcrRecognitionPort* recognition,
                                           ScreenshotQrRecognitionPort* qrRecognition,
                                           SnowShotApiClient* tableRecognition,
                                           ScreenshotRecognitionSessionActions actions,
                                           QObject* parent = nullptr);
    ~ScreenshotRecognitionSessionController() override;

    void setTarget(ScreenshotRecognitionTarget target);
    [[nodiscard]] bool hasTarget() const;
    void prefetchText();
    void activate(Mode mode);
    void deactivate();
    void invalidate();

    [[nodiscard]] bool active() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool busy(Mode mode) const;
    [[nodiscard]] Mode mode() const;
    [[nodiscard]] bool tableModeActive() const;
    [[nodiscard]] bool qrModeActive() const;

    void mergeTableSelection();
    void splitTableSelection();
    void resetTable();
    void undoTableEdit();
    void redoTableEdit();
    void undoTextEdit();
    void redoTextEdit();

    void beginTextEditing();
    void beginTextTranslation();
    void endTextEditing();
    void openTranslationSettings();
    void resetTextEditing();
    void applyTextFormatting(const QString& value);
    void applyTextPunctuation(const QString& value);
    [[nodiscard]] bool editing() const;
    [[nodiscard]] bool translating() const;
    [[nodiscard]] bool hasTextResult() const;
    [[nodiscard]] QString textDraft() const;
    [[nodiscard]] QString originalText() const;
    [[nodiscard]] std::shared_ptr<ScreenshotTableEditingSession> tableSession() const;
    [[nodiscard]] QStringList qrContents() const;
    void setTextDraft(const QString& text);
    void handleTableCommandState(const ScreenshotTableCommandState& state);

  signals:
    void textEditingChanged(bool editing);
    void textResultChanged(bool available);
    void textDraftChanged(const QString& text);

  private:
    struct TextCacheEntry {
        enum class TranslationStatus { Absent, Streaming, Completed, Failed };
        std::shared_ptr<ScreenshotOcrPresentation> presentation;
        std::shared_ptr<QTextDocument> formattedDocument;
        bool formatted = false;
        std::shared_ptr<ScreenshotOcrTextEditingSession> editingSession;
        std::shared_ptr<ScreenshotOcrTextEditingSession> translationSession;
        snow_shot::storage::ScreenshotTranslationConfiguration translationSettings;
        QString translationText;
        QString successfulTranslation;
        TranslationStatus translationStatus = TranslationStatus::Absent;
        bool hasSuccessfulTranslation = false;
        bool editing = false;
    };

    void startTextRecognition(ScreenshotOcrRequestPriority priority);
    void startTableRecognition();
    void startQrRecognition();
    void handleTextOutput(quint64 generation, const QString& key,
                          ScreenshotOcrRecognitionResult output);
    void handleTableOutput(quint64 generation, const QString& key, SnowShotTableResult result);
    void handleQrOutput(quint64 generation, const QString& key,
                        ScreenshotQrRecognitionResult result);
    void ensureContent();
    void clearContent();
    void applyPresentation(const std::shared_ptr<ScreenshotOcrPresentation>& presentation);
    void applyFormattedText(const std::shared_ptr<QTextDocument>& document);
    void applyTableSession(const std::shared_ptr<ScreenshotTableEditingSession>& session);
    void applyQrContents(const QStringList& contents);
    void handleTextDocumentChanged(const QString& key);
    void handleTranslationDocumentChanged(const QString& key);
    void startTranslation();
    void startTranslationWithModels(const QVector<SnowShotChatModel>& models);
    void handleTranslationDelta(quint64 generation, const QString& key, const QString& delta);
    void handleTranslationFinished(quint64 generation, const QString& key,
                                   SnowShotTranslationResult result);
    void showTranslationSettingsModal(const QVector<SnowShotChatModel>& models);
    void invalidateCurrentTranslation(bool restartIfVisible);
    void updateBusyState() const;
    void updateTextState() const;
    void updateTableState(const ScreenshotTableCommandState& state) const;
    void clearTextEditingState();
    void showRecognitionMessage() const;
    void hideRecognitionMessage() const;
    void showStatus(const QString& message, bool error) const;
    void cancelOutstandingRequests();
    void handleRecognitionProviderDestroyed(Mode mode);
    [[nodiscard]] ScreenshotRecognitionWindow* content() const;

    QPointer<ScreenshotOcrRecognitionPort> m_recognition;
    QPointer<ScreenshotQrRecognitionPort> m_qrRecognition;
    QPointer<SnowShotApiClient> m_tableRecognition;
    ScreenshotRecognitionSessionActions m_actions;
    ScreenshotRecognitionTarget m_target;
    QPointer<ScreenshotRecognitionWindow> m_content;
    QHash<QString, TextCacheEntry> m_textCache;
    QHash<QString, std::shared_ptr<ScreenshotTableEditingSession>> m_tableCache;
    QHash<QString, QStringList> m_qrCache;
    std::shared_ptr<ScreenshotOcrPresentation> m_presentation;
    std::shared_ptr<ScreenshotTableEditingSession> m_tableSession;
    QString m_textCacheKey;
    QString m_tableCacheKey;
    QString m_qrCacheKey;
    QStringList m_qrContents;
    QPointer<QTextDocument> m_textDocument;
    QString m_editingKey;
    QString m_translationKey;
    ScreenshotOcrRecognitionPort::RequestToken m_textRequestToken = 0;
    SnowShotApiClient::RequestToken m_tableRequestToken = 0;
    SnowShotApiClient::RequestToken m_modelsRequestToken = 0;
    SnowShotApiClient::RequestToken m_settingsModelsRequestToken = 0;
    SnowShotApiClient::RequestToken m_translationRequestToken = 0;
    ScreenshotQrRecognitionPort::RequestToken m_qrRequestToken = 0;
    quint64 m_textGeneration = 0;
    quint64 m_tableGeneration = 0;
    quint64 m_qrGeneration = 0;
    quint64 m_translationGeneration = 0;
    Mode m_mode = Mode::Text;
    bool m_active = false;
    bool m_editing = false;
    bool m_translating = false;
    QPointer<adqt::widgets::AdModal> m_translationSettingsModal;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONSESSIONCONTROLLER_H
