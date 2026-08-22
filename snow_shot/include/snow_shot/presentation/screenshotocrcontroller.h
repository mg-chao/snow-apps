#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRCONTROLLER_H

#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshotmessageservice.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/network/snowshotapiclient.h"

#include <QObject>
#include <QPointer>
#include <QImage>
#include <QRect>
#include <QVector>

#include <functional>
#include <memory>

class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotOcrPresentation;
class ScreenshotRecognitionWindow;
class ScreenshotRecognitionSessionController;
class ScreenshotTableEditingSession;
class ScreenshotOverlayCoordinator;
class ScreenshotOverlayWindow;
class ScreenshotSelectionModel;
class SnowCanvasRuntime;
class SnowCanvasWidget;
class QWidget;
class QUrl;
struct ScreenshotCaptureState;
struct ScreenshotTableCommandState;

struct ScreenshotOcrControllerContext {
    ScreenshotCaptureState& captureState;
    ScreenshotInteractionState& interaction;
    ScreenshotSelectionModel& selection;
    ScreenshotDisplaySession& displaySession;
    ScreenshotGeometryMapper& geometry;
    ScreenshotOverlayCoordinator& overlayCoordinator;
    ScreenshotOcrRecognitionPort& recognition;
    ScreenshotQrRecognitionPort& qrRecognition;
    SnowShotApiClient* tableRecognition = nullptr;
    std::function<void()> hideColorPicker = []() {};
    std::function<void()> cancelCapture = []() {};
    std::function<ScreenshotSelectionDragMode(const QPointF&)> selectionResizeDragMode =
        [](const QPointF&) { return ScreenshotSelectionDragMode::None; };
    std::function<bool(const QPointF&)> beginSelectionResize = [](const QPointF&) {
        return false;
    };
    std::function<void(const QPointF&)> updateSelectionResize = [](const QPointF&) {};
    std::function<void(const QPointF&)> finishSelectionResize = [](const QPointF&) {};
};

class ScreenshotOcrController final : public QObject {
    Q_OBJECT

  public:
    enum class Mode { Text, Table, Qr };

    explicit ScreenshotOcrController(ScreenshotOcrControllerContext context,
                                     QObject* parent = nullptr);
    ~ScreenshotOcrController() override;

    void activate();
    void activateTable();
    void activateQr();
    // Leaves the visible recognition tool but deliberately keeps requests and cache entries alive.
    void deactivate();
    void deactivateForSelectionResize();
    // Invalidates the capture session and cancels all recognition work.
    void invalidateSession();
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool busy(Mode mode) const;
    [[nodiscard]] Mode mode() const;
    [[nodiscard]] bool tableModeActive() const;
    [[nodiscard]] bool qrModeActive() const;
    void setMode(Mode mode);
    [[nodiscard]] bool copyRecognitionToClipboard(bool endCapture = true);
    void mergeTableSelection();
    void splitTableSelection();
    void resetTable();
    void undoTableEdit();
    void redoTableEdit();
    [[nodiscard]] ScreenshotTableCommandState tableCommandState() const;
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
    void setTextDraft(const QString& text);

  signals:
    void textEditingChanged(bool editing);
    void textResultChanged(bool available);
    void textDraftChanged(const QString& text);

  private:
    struct CanvasState {
        QPointer<ScreenshotOverlayWindow> overlay;
        QPointer<SnowCanvasWidget> canvas;
        bool contentVisible = true;
        bool interactionEnabled = true;
        bool hadSelection = false;
        bool selectionHandlesVisible = true;
        bool selectionBorderVisible = true;
    };
    void activateMode(Mode mode);
    void handleQrLinkActivated(const QUrl& url);
    void updateOverlays() const;
    void applyOcrBackgroundToOverlays(
        const std::shared_ptr<ScreenshotOcrPresentation>& presentation) const;
    void clearOcrBackgroundFromOverlays() const;
    void deactivateImpl(bool preserveRecognitionWindow);
    void restorePreviousToolAfterFailure();
    void showStatus(const QString& message, bool error) const;
    [[nodiscard]] bool ensureRecognitionWindow();
    void updateRecognitionWindowGeometry();
    void destroyRecognitionWindow();
    [[nodiscard]] QString currentCacheKey() const;

    ScreenshotOcrControllerContext m_context;
    QVector<CanvasState> m_canvasStates;
    std::shared_ptr<ScreenshotOcrPresentation> m_presentation;
    ScreenshotActiveTool m_previousTool = ScreenshotActiveTool::Move;
    std::unique_ptr<ScreenshotMessageService> m_messages;
    std::unique_ptr<ScreenshotRecognitionSessionController> m_session;
    QPointer<ScreenshotRecognitionWindow> m_recognitionWindow;
    QString m_surfaceKey;
    QImage m_surfaceImage;
    Mode m_mode = Mode::Text;
    bool m_active = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRCONTROLLER_H
