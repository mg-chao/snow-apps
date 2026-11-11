#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWINDOW_H

#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshotocrtextlayer.h"
#include "snow_shot/presentation/screenshotimageconversion.h"
#include "snow_shot/app/edition.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "snow_shot/presentation/screenshotoriginalimagepreviewwindow.h"
#include "snow_shot/presentation/screenshottextannotation.h"
#include <optional>

#include <QPointF>
#include <QHash>
#include <QRect>
#include <QRectF>
#include <QStringList>
#include <QTransform>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

class QKeyEvent;
class QFocusEvent;
class QEvent;
class QContextMenuEvent;
class QMouseEvent;
class QMimeData;
class QPaintEvent;
class QResizeEvent;
class QScreen;
class QWindow;
class QStackedLayout;
class QTextDocument;
class QTextBrowser;
class QTextEdit;
class QTimer;
class ScreenshotTableDocument;
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
class AdButton;
class AdAlert;
class AdContextMenu;
} // namespace adqt::widgets
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
    // Selection gestures use global logical positions: this surface moves and resizes with
    // the selection, so its local coordinates and rendered bounds cannot define drag input.
    std::function<ScreenshotSelectionDragMode(const QPointF&)>
        selectionResizeDragModeAtGlobalPosition = noResizeHandle;
    std::function<bool(const QPointF&)> beginSelectionResizeAtGlobalPosition = declineResize;
    std::function<void(const QPointF&)> updateSelectionResizeAtGlobalPosition = ignoreResize;
    std::function<void(const QPointF&)> finishSelectionResizeAtGlobalPosition = ignoreResize;
    std::function<void()> selectionResizeFinished = noAction;
    std::function<void()> handleCopy = noAction;
    std::function<bool()> copyOcrResult{};
};

struct ScreenshotTextSelectionSnapshot {
    ScreenshotTextAnnotationSource source = ScreenshotTextAnnotationSource::Ocr;
    QVector<ScreenshotTextSelectionRange> ranges;
    QRectF canvasBounds;
    bool inProgress = false;

    [[nodiscard]] bool empty() const {
        return ranges.isEmpty();
    }
};

class ScreenshotRecognitionWindow final : public QWidget {
    Q_OBJECT

  public:
    void setEmbeddedPointerEventHandler(std::function<bool(QObject*, QEvent*)> handler) {
        m_embeddedPointerEventHandler = std::move(handler);
    }
    enum class PresentationMode {
        TopLevelWindow,
        EmbeddedChild,
    };
    enum class TextSelectionSnapshotMode {
        WithGeometry,
        // During a pointer gesture, return only its source and in-progress state.
        CompletedOnly,
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
    [[nodiscard]] bool showsOcrLayer() const;
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
    using AttachmentToken = quint64;
    struct SessionCallbacks {
        std::function<bool()> translationActive;
        std::function<std::unique_ptr<QMimeData>()> translationClipboard;
    };
    [[nodiscard]] AttachmentToken attachSession(QObject* owner, SessionCallbacks callbacks);
    [[nodiscard]] bool ownsAttachment(const QObject* owner, AttachmentToken token) const;
    void detachSession(const QObject* owner, AttachmentToken token);
    [[nodiscard]] QImage translationDocumentImage() const;
    void setTextHighlighting(bool enabled, const QColor& maskColor);
    void setOcrPresentation(
        std::shared_ptr<ScreenshotOcrPresentation> presentation,
        ScreenshotOcrTextLayer::RenderingMode mode = ScreenshotOcrTextLayer::RenderingMode::Normal,
        bool takeFocus = true);
    void updateOcrSelection();
    void clearOcrSelection();
    void clearTextSelection();
    [[nodiscard]] bool textSelectionActive() const;
    [[nodiscard]] ScreenshotTextSelectionSnapshot textSelectionSnapshot(
        TextSelectionSnapshotMode mode = TextSelectionSnapshotMode::WithGeometry) const;
    void setTextSelectionFocusPreserver(std::function<bool(QWidget*)> predicate);
    // Only the active result page contributes a selection; no selection returns null.
    [[nodiscard]] std::unique_ptr<QMimeData> textSelectionClipboardMimeData() const;
    void updateOcrText(int lineIndex, const QString& text, bool streaming = false);
    void clearOcrPresentation();
    [[nodiscard]] std::optional<ScreenshotRecognitionImageSnapshot>
    imageSnapshot(QImage image, const QRectF& canvasRect, QImage filteredImage,
                  const QRectF& filteredCanvasRect, const ScreenshotResultStyle& style,
                  bool includeHiddenContent = false) const;
    // The backdrop must not contain textAnnotations: canvas-text pages compose them once
    // after recognition content and before output opacity. Other result pages ignore them.
    [[nodiscard]] QImage
    printViewportSnapshot(QImage background = {}, const QRectF& canvasRect = {},
                          QImage filtered = {}, const QRectF& filteredRect = {},
                          qreal contentOpacity = 1.0, QSize outputPixelSize = {},
                          const ScreenshotTextAnnotationRenderSnapshot& textAnnotations = {});
    void showFormattedText(
        std::shared_ptr<QTextDocument> document,
        ScreenshotOcrTextLayer::RenderingMode mode = ScreenshotOcrTextLayer::RenderingMode::Normal,
        bool takeFocus = true, std::optional<QTransform> documentToCanvas = std::nullopt);
    void clearFormattedText();

    void setTableSession(std::shared_ptr<ScreenshotTableEditingSession> session);
    void showTablePreview(const ScreenshotTableDocument& document);
    void clearTableSession(bool retainEditor = false);
    [[nodiscard]] ScreenshotTableCommandState tableCommandState() const;
    void mergeTableSelection();
    void splitTableSelection();
    void resetTable();
    void undoTableEdit();
    void redoTableEdit();
    void commitActiveTableEdit();
    [[nodiscard]] QVector<int> tableColumnWidths() const;

    void showTextEditor(QTextDocument* document, bool readOnly = false, bool streaming = false);
    void showTextEditor(std::shared_ptr<QTextDocument> document, bool readOnly = false,
                        bool streaming = false);
    void setTextEditorEditingEnabled(bool enabled);
    void setTextEditorStreaming(bool streaming);
    void hideTextEditor();
    void showLatexEditor(QTextDocument* document, std::function<QString()> source = {},
                         bool streaming = false);
    void showLatexEditor(std::shared_ptr<QTextDocument> document,
                         std::function<QString()> source = {}, bool streaming = false);
    void setLatexPreviewEnabled(bool enabled);
    void clearLatexPreview();

    void showQrContents(const QStringList& contents, bool detectLinks = true);
    void clearQrContents();
    void showImageConversion(SnowShotImageConversionFormat format, const QString& source, bool busy,
                             const QString& error);
    void clearImageConversion();
    void showRecognitionError(const QString& error);
    void clearRecognitionError();

    [[nodiscard]] bool copyVisibleContentToClipboard();
    [[nodiscard]] bool isOcrBackgroundAt(const QPointF& localPosition) const;
    [[nodiscard]] bool boxSelectionEnabled() const;
    [[nodiscard]] bool lineBoxSelectionActive() const;
    void cancelLineBoxSelection();

  signals:
    void resultPageChanged();
    void textSelectionChanged();
    void textSelectionGeometryChanged();
    void embeddedContextMenuRequested(const QPoint& globalPosition);
    void imageConversionRetryRequested();
    void recognitionRetryRequested();

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
    friend struct ScreenshotRecognitionWindowTestAccess;
    void populateOcrContextMenu(adqt::widgets::AdContextMenu& menu);
    void populateTextEditorContextMenu(adqt::widgets::AdContextMenu& menu);
    [[nodiscard]] bool commandAttachmentMatches(const QObject* owner, AttachmentToken token) const;
    [[nodiscard]] bool translationCopyActive() const;
    [[nodiscard]] ScreenshotRecognitionImageSnapshot ocrImageSnapshot() const;
    void showTextEditorWithOwner(QTextDocument* document,
                                 std::shared_ptr<QTextDocument> documentOwner, bool readOnly,
                                 bool streaming, bool preserveLatexPreview = false);
    void showLatexEditorWithOwner(QTextDocument* document,
                                  std::shared_ptr<QTextDocument> documentOwner,
                                  std::function<QString()> source, bool streaming);
    [[nodiscard]] QPointF canvasPositionForLocalPoint(const QPointF& localPosition) const;
    [[nodiscard]] QTransform canvasToLocalTransform() const;
    void registerWindowShortcuts();
    void synchronizeTextLayer();
    void notifyTextSelectionChanged();
    void cancelPointerTextSelection();
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
    [[nodiscard]] bool forwardFormattedTextPointerEvent(QObject* watched, QEvent* event);
    void updateSelectionResizeCursor(const QPointF& localPosition);
    void updateLineBoxSelection(const QPointF& localPosition);
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
    bool m_latexStreaming = false;
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
    QWidget* m_recognitionErrorPage = nullptr;
    adqt::widgets::AdButton* m_recognitionRetry = nullptr;
    bool m_showOriginalImage = false;
    QStackedLayout* m_stack = nullptr;
    ScreenshotOcrTextLayer* m_textLayer = nullptr;
    QWidget* m_textEditorContainer = nullptr;
    QTextEdit* m_textEditor = nullptr;
    bool m_textEditorEditingEnabled = true;
    bool m_textEditorStreaming = false;
    QPointer<QObject> m_sessionOwner;
    bool m_sessionAttached = false;
    AttachmentToken m_attachmentToken = 0;
    SessionCallbacks m_sessionCallbacks;
    std::shared_ptr<QTextDocument> m_textEditorDocumentOwner;
    adqt::widgets::AdSpin* m_textEditorSpin = nullptr;
#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    QTextBrowser* m_qrBrowser = nullptr;
    QString m_qrSource;
    QVector<qsizetype> m_qrSourceOffsets;
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
    bool m_tablePreview = false;
#else
    static constexpr QWidget* m_tableEditor = nullptr;
#endif
    QRectF m_canvasSelection;
    qreal m_formattedTextDevicePixelRatio = 1.0;
    PresentationMode m_presentationMode = PresentationMode::TopLevelWindow;
    std::function<bool(QObject*, QEvent*)> m_embeddedPointerEventHandler;
    bool m_selectionResizeActive = false;
    bool m_selectionOnly = false;
    bool m_textSelectionPointerActive = false;
    std::function<bool(QWidget*)> m_textSelectionFocusPreserver;
    mutable ScreenshotTextSelectionSnapshot m_cachedTextSelectionSnapshot;
    mutable const ScreenshotOcrPresentation* m_cachedTextSelectionPresentation = nullptr;
    mutable quint64 m_cachedTextSelectionRevision = 0;
    mutable QFont m_cachedTextSelectionFont;
    mutable QHash<int, QRectF> m_cachedTextSelectionLineBounds;
    std::optional<QPointF> m_lineBoxPressPosition;
    bool m_lineBoxThresholdReached = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONWINDOW_H
