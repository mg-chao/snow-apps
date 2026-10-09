#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWINDOW_H

#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshotocrtextlayer.h"
#include "snow_shot/presentation/screenshotimageconversion.h"
#include "snow_shot/app/edition.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "snow_shot/presentation/screenshotoriginalimagepreviewwindow.h"
#include <optional>

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
class QWindow;
class QStackedLayout;
class QTextDocument;
class QTextBrowser;
class QTextEdit;
class QTimer;
class ScreenshotLatexRenderer;
class QUrl;
class ScreenshotFormattedTextLayer;
class ScreenshotImageConversionView;
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
    // Named defaults keep partially initialized action aggregates consistent across
    // translation units, including MSVC builds with different lambda instantiations.
    static void noAction() {}
    static void ignoreText(const QString&) {}
    static void ignoreLink(const QUrl&) {}
    static ScreenshotSelectionDragMode noResizeHandle(const QPointF&) {
        return ScreenshotSelectionDragMode::None;
    }
    static bool declineResize(const QPointF&) {
        return false;
    }
    static void ignoreResize(const QPointF&) {}

    std::function<void()> handleCancel = noAction;
    std::function<void(const QString&)> handleTextEdited = ignoreText;
    std::function<void(const ScreenshotTableCommandState&)> handleTableCommandStateChanged;
    std::function<void(const QString&)> handleTableOperationRejected = ignoreText;
    std::function<void(const QUrl&)> handleLinkActivated = ignoreLink;
    std::function<void()> handleUndoTextEdit = noAction;
    std::function<void()> handleRedoTextEdit = noAction;
    std::function<ScreenshotSelectionDragMode(const QPointF&)> selectionResizeDragMode =
        noResizeHandle;
    std::function<bool(const QPointF&)> beginSelectionResize = declineResize;
    std::function<void(const QPointF&)> updateSelectionResize = ignoreResize;
    std::function<void(const QPointF&)> finishSelectionResize = ignoreResize;
    std::function<void()> selectionResizeFinished = noAction;
    std::function<void()> handleCopy = noAction;
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
        bool takeFocus = true;
    };

    explicit ScreenshotRecognitionWindow(
        ScreenshotRecognitionWindowActions actions, QWidget* parent = nullptr,
        PresentationMode presentationMode = PresentationMode::TopLevelWindow,
        snow_shot::presentation::WindowShortcutManager* shortcutManager = nullptr);
    ~ScreenshotRecognitionWindow() override;

    [[nodiscard]] bool present(const Config& config);
    [[nodiscard]] bool updateSelectionGeometry(const QRect& geometry,
                                               const QRectF& canvasSelection);

    void setShowOriginalImage(bool show);
    void setOriginalImagePreviewEnabled(bool enabled);
    [[nodiscard]] bool originalImagePreviewEnabled() const;
    void setOriginalImagePreviewSource(QImage image, const QRectF& canvasRect);
    void setOriginalImagePreviewProvider(
        std::function<std::optional<ScreenshotOriginalImagePreviewState>()> provider,
        QWidget* host);
    void setOriginalImagePreviewSuppressed(bool suppressed);
    void setOriginalImagePreviewAboveSiblingProvider(std::function<QWidget*()> provider);
    void refreshOriginalImagePreview();
    void syncOriginalImagePreviewStacking(bool staysOnTop);
    void setOcrCopyDefaultsEnabled(bool enabled);
    void setOcrPresentation(
        std::shared_ptr<ScreenshotOcrPresentation> presentation,
        ScreenshotOcrTextLayer::RenderingMode mode = ScreenshotOcrTextLayer::RenderingMode::Normal,
        bool takeFocus = true);
    void updateOcrSelection();
    void clearOcrSelection();
    void updateOcrText(int lineIndex, const QString& text);
    void clearOcrPresentation();
    [[nodiscard]] std::optional<ScreenshotRecognitionImageSnapshot>
    imageSnapshot(QImage image, const QRectF& canvasRect, QImage filteredImage,
                  const QRectF& filteredCanvasRect, const ScreenshotResultStyle& style) const;
    [[nodiscard]] QImage printViewportSnapshot(QImage background = {},
                                               const QRectF& canvasRect = {}, QImage filtered = {},
                                               const QRectF& filteredRect = {},
                                               qreal contentOpacity = 1.0);
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

    void showTextEditor(QTextDocument* document, bool readOnly = false, bool streaming = false);
    void setTextEditorStreaming(bool streaming);
    void hideTextEditor();
    void showLatexEditor(QTextDocument* document, std::function<QString()> source = {});
    void setLatexPreviewEnabled(bool enabled);
    void clearLatexPreview();

    void showQrContents(const QStringList& contents, bool detectLinks = true);
    void clearQrContents();
    void showImageConversion(SnowShotImageConversionFormat format, const QString& source, bool busy,
                             const QString& error);
    void clearImageConversion();

    [[nodiscard]] bool copyVisibleContentToClipboard();
    [[nodiscard]] bool isOcrBackgroundAt(const QPointF& localPosition) const;

  signals:
    void embeddedContextMenuRequested(const QPoint& globalPosition);
    void imageConversionRetryRequested();

  protected:
    void changeEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
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
    void updateOriginalImagePreview();
    void destroyOriginalImagePreview();
    void observeOriginalImagePreviewHost();
    [[nodiscard]] bool companionPreviewEnabled() const;
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    void prepareLatexPreview(ScreenshotOriginalImagePreviewState& state);
    void scheduleLatexPreview();
    [[nodiscard]] QString currentLatexSource() const;
    [[nodiscard]] QString currentLatexSource(int selectionStart, int selectionEnd) const;
    [[nodiscard]] QString latexPreviewStatus() const;
#endif
    void installSelectionResizeEventFilters(QWidget* widget);
    [[nodiscard]] bool activeContentOwnsContextMenu(const QObject* watched) const;
    void showOcrContextMenu(const QPoint& globalPosition);
    void showQrContextMenu(const QPoint& globalPosition);
    void showTextEditorContextMenu(const QPoint& globalPosition);
    [[nodiscard]] ScreenshotSelectionDragMode
    selectionResizeDragModeAtLocalPoint(const QPointF& localPosition) const;
    [[nodiscard]] bool handleSelectionResizeEvent(QObject* watched, QEvent* event);
    void updateSelectionResizeCursor(const QPointF& localPosition);
    [[nodiscard]] static Qt::CursorShape
    cursorForSelectionResize(ScreenshotSelectionDragMode dragMode);

    ScreenshotRecognitionWindowActions m_actions;
    QPointer<ScreenshotOriginalImagePreviewWindow> m_originalImagePreview;
    QPointer<QWidget> m_originalImagePreviewHost;
    QPointer<QWidget> m_originalImagePreviewTransientOwner;
    QPointer<QWindow> m_originalImagePreviewHostHandle;
    QVector<QMetaObject::Connection> m_originalImagePreviewConnections;
    std::function<std::optional<ScreenshotOriginalImagePreviewState>()>
        m_originalImagePreviewProvider;
    std::function<QWidget*()> m_originalImagePreviewAboveSiblingProvider;
    QImage m_originalImagePreviewSource;
    QRectF m_originalImagePreviewCanvasRect;
    bool m_originalImagePreviewEnabled = false;
    bool m_originalImagePreviewSuppressed = false;
    bool m_originalImagePreviewRefreshPending = false;
    bool m_originalImagePreviewStaysOnTop = true;
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    struct LatexRenderKey {
        QString source;
        QSize size;
        qreal dpr = 0;
        QColor foreground;

        [[nodiscard]] bool operator==(const LatexRenderKey& other) const {
            return source == other.source && size == other.size && qFuzzyCompare(dpr, other.dpr) &&
                   foreground == other.foreground;
        }
    };
    bool m_latexPreviewEnabled = false;
    QPointer<QTextDocument> m_latexDocument;
    std::function<QString()> m_latexSource;
    QMetaObject::Connection m_latexDocumentConnection;
    ScreenshotLatexRenderer* m_latexRenderer = nullptr;
    QTimer* m_latexPreviewTimer = nullptr;
    QImage m_latexPreviewImage;
    LatexRenderKey m_latexValidKey;
    LatexRenderKey m_latexPendingKey;
    LatexRenderKey m_latexFallbackAttemptKey;
    QString m_latexRequestedSource;
    QSize m_latexRequestedSize;
    QColor m_latexRequestedForeground;
    qreal m_latexRequestedDpr = 0;
    quint64 m_latexRequestToken = 0;
    int m_latexPreviewError = 0;
    bool m_latexRenderPending = false;
    bool m_latexFallbackPending = false;
#endif
    std::unique_ptr<snow_shot::presentation::WindowShortcutManager> m_ownedShortcutManager;
    snow_shot::presentation::WindowShortcutManager* m_shortcutManager = nullptr;
    std::shared_ptr<ScreenshotOcrPresentation> m_ocrPresentation;
    bool m_ocrCopyDefaultsEnabled = true;
    QWidget* m_contentContainer = nullptr;
    bool m_showOriginalImage = false;
    QStackedLayout* m_stack = nullptr;
    ScreenshotOcrTextLayer* m_textLayer = nullptr;
    QWidget* m_textEditorContainer = nullptr;
    QTextEdit* m_textEditor = nullptr;
    adqt::widgets::AdSpin* m_textEditorSpin = nullptr;
#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    QTextBrowser* m_qrBrowser = nullptr;
    bool m_qrDetectLinks = true;
#else
    static constexpr QWidget* m_qrBrowser = nullptr;
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    ScreenshotImageConversionView* m_conversionView = nullptr;
#else
    static constexpr QWidget* m_conversionView = nullptr;
#endif
    ScreenshotFormattedTextLayer* m_formattedTextLayer = nullptr;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    ScreenshotTableEditor* m_tableEditor = nullptr;
#else
    static constexpr QWidget* m_tableEditor = nullptr;
#endif
    QRectF m_canvasSelection;
    qreal m_formattedTextDevicePixelRatio = 1.0;
    PresentationMode m_presentationMode = PresentationMode::TopLevelWindow;
    bool m_selectionResizeActive = false;
    bool m_selectionOnly = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWINDOW_H
