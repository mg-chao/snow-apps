#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWINDOW_H

#include "snow_shot/presentation/screenshotselectiongeometry.h"

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QStringList>
#include <QTransform>
#include <QWidget>

#include <functional>
#include <memory>

class QKeyEvent;
class QFocusEvent;
class QEvent;
class QContextMenuEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QScreen;
class QStackedLayout;
class QTextDocument;
class QTextBrowser;
class QTextEdit;
class QUrl;
class ScreenshotFormattedTextLayer;
class ScreenshotOcrPresentation;
class ScreenshotOcrTextLayer;
class ScreenshotTableEditingSession;
class ScreenshotTableEditor;
struct ScreenshotTableCommandState;
namespace adqt::widgets {
class AdSpin;
}
namespace snow_shot::presentation {
class WindowShortcutManager;
}

struct ScreenshotRecognitionWindowActions {
    std::function<void()> handleCancel = []() {};
    std::function<void(const QString&)> handleTextEdited = [](const QString&) {};
    std::function<void(const ScreenshotTableCommandState&)> handleTableCommandStateChanged;
    std::function<void(const QString&)> handleTableOperationRejected = [](const QString&) {};
    std::function<void(const QUrl&)> handleLinkActivated = [](const QUrl&) {};
    std::function<void()> handleUndoTextEdit = []() {};
    std::function<void()> handleRedoTextEdit = []() {};
    std::function<ScreenshotSelectionDragMode(const QPointF&)> selectionResizeDragMode =
        [](const QPointF&) { return ScreenshotSelectionDragMode::None; };
    std::function<bool(const QPointF&)> beginSelectionResize = [](const QPointF&) {
        return false;
    };
    std::function<void(const QPointF&)> updateSelectionResize = [](const QPointF&) {};
    std::function<void(const QPointF&)> finishSelectionResize = [](const QPointF&) {};
    std::function<void()> selectionResizeFinished = []() {};
};

class ScreenshotRecognitionWindow final : public QWidget {
    Q_OBJECT

  public:
    enum class PresentationMode {
        TopLevelWindow,
        EmbeddedChild,
    };

    struct Config {
        QScreen* screen = nullptr;
        QWidget* transientOwner = nullptr;
        QRect geometry;
        QRectF canvasSelection;
        PresentationMode presentationMode = PresentationMode::TopLevelWindow;
        qreal formattedTextDevicePixelRatio = 1.0;
    };

    explicit ScreenshotRecognitionWindow(
        ScreenshotRecognitionWindowActions actions,
        QWidget* parent = nullptr,
        PresentationMode presentationMode = PresentationMode::TopLevelWindow,
        snow_shot::presentation::WindowShortcutManager* shortcutManager = nullptr);
    ~ScreenshotRecognitionWindow() override;

    [[nodiscard]] bool present(const Config& config);
    [[nodiscard]] bool updateSelectionGeometry(const QRect& geometry,
                                               const QRectF& canvasSelection);
    [[nodiscard]] PresentationMode presentationMode() const;

    void setOcrPresentation(std::shared_ptr<ScreenshotOcrPresentation> presentation);
    void clearOcrPresentation();
    void showFormattedText(std::shared_ptr<QTextDocument> document);
    void clearFormattedText();

    void setTableSession(std::shared_ptr<ScreenshotTableEditingSession> session);
    void clearTableSession();
    [[nodiscard]] ScreenshotTableCommandState tableCommandState() const;
    void mergeTableSelection();
    void splitTableSelection();
    void resetTable();
    void undoTableEdit();
    void redoTableEdit();
    void commitActiveTableEdit();

    void showTextEditor(QTextDocument* document, bool readOnly = false,
                        bool streaming = false);
    void setTextEditorStreaming(bool streaming);
    void hideTextEditor();

    void showQrContents(const QStringList& contents);
    void clearQrContents();

  signals:
    void embeddedContextMenuRequested(const QPoint& globalPosition);

  protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    [[nodiscard]] QPointF canvasPositionForLocalPoint(const QPointF& localPosition) const;
    [[nodiscard]] QTransform canvasToLocalTransform() const;
    void registerWindowShortcuts();
    void synchronizeTextLayer();
    void updateTextEditorSpinGeometry();
    void installSelectionResizeEventFilters(QWidget* widget);
    [[nodiscard]] ScreenshotSelectionDragMode selectionResizeDragModeAtLocalPoint(
        const QPointF& localPosition) const;
    [[nodiscard]] bool handleSelectionResizeEvent(QObject* watched, QEvent* event);
    void updateSelectionResizeCursor(const QPointF& localPosition);
    [[nodiscard]] static Qt::CursorShape cursorForSelectionResize(
        ScreenshotSelectionDragMode dragMode);

    ScreenshotRecognitionWindowActions m_actions;
    std::unique_ptr<snow_shot::presentation::WindowShortcutManager> m_ownedShortcutManager;
    snow_shot::presentation::WindowShortcutManager* m_shortcutManager = nullptr;
    std::shared_ptr<ScreenshotOcrPresentation> m_ocrPresentation;
    QStackedLayout* m_stack = nullptr;
    ScreenshotOcrTextLayer* m_textLayer = nullptr;
    QWidget* m_textEditorContainer = nullptr;
    QTextEdit* m_textEditor = nullptr;
    adqt::widgets::AdSpin* m_textEditorSpin = nullptr;
    QTextBrowser* m_qrBrowser = nullptr;
    ScreenshotFormattedTextLayer* m_formattedTextLayer = nullptr;
    ScreenshotTableEditor* m_tableEditor = nullptr;
    QRectF m_canvasSelection;
    qreal m_formattedTextDevicePixelRatio = 1.0;
    PresentationMode m_presentationMode = PresentationMode::TopLevelWindow;
    bool m_selectionResizeActive = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWINDOW_H
