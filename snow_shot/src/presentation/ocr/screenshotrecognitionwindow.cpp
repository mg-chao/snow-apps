#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include <QAbstractTextDocumentLayout>
#include <QAbstractItemDelegate>
#include <QScrollBar>
#include <QTableView>
#include <QStyleOptionViewItem>
#include <QPainter>
#include <QSet>

#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
#include "snow_shot/presentation/screenshotimageconversionview.h"
#endif
#include "snow_shot/platform/screenshotnative.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtexttransform.h"
#include "snow_shot/presentation/screenshotocrtextlayer.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/context_menu.h"
#include "widgets/input_text_edit.h"
#include "widgets/scroll_area.h"
#include "widgets/spin.h"

#include <QApplication>
#include <QChildEvent>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
#include <QTextBrowser>
#endif
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QtMath>
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
#include "snow_shot/presentation/screenshotlatexrenderer.h"
#endif

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
QMargins adTextAreaContentMargins(const adqt::theme::ThemeMapToken& theme) {
    const int borderInset = std::max(1, qRound(theme.lineWidth));
    const int horizontalPadding = std::max(8, qRound(theme.sizeSM - theme.lineWidth));
    const double baseVerticalPadding =
        (theme.controlHeight - theme.fontSize * theme.lineHeight) / 2.0;
    const double roundedVerticalPadding = std::round(baseVerticalPadding * 10.0) / 10.0;
    const int verticalPadding = std::max(0, qRound(roundedVerticalPadding - theme.lineWidth));
    return QMargins(borderInset + horizontalPadding, borderInset + verticalPadding,
                    borderInset + horizontalPadding, borderInset + verticalPadding);
}
#endif

void applyTextEditorContainerBackground(QWidget* container) {
    if (container == nullptr) {
        return;
    }
    const adqt::theme::ThemeMapToken theme =
        adqt::theme::ThemeManager::instance().resolveTheme(container);
    QPalette palette = container->palette();
    palette.setColor(QPalette::Window, theme.colorBgContainer);
    palette.setColor(QPalette::Base, theme.colorBgContainer);
    container->setPalette(palette);
    container->setAutoFillBackground(true);
}

#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
bool isHttpUrl(const QString& text, QUrl* result = nullptr) {
    const QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    const bool valid = url.isValid() && !url.isRelative() && !url.host().isEmpty() &&
                       (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"));
    if (valid && result != nullptr) {
        *result = url;
    }
    return valid;
}
#endif

QList<QKeyCombination> standardCombinations(QKeySequence::StandardKey standardKey) {
    QList<QKeyCombination> combinations;
    for (const QKeySequence& sequence : QKeySequence::keyBindings(standardKey)) {
        if (sequence.count() == 1 && !combinations.contains(sequence[0])) {
            combinations.push_back(sequence[0]);
        }
    }
    return combinations;
}

bool focusInside(const QWidget* container, const QWidget* focus) {
    return container != nullptr && focus != nullptr &&
           (container == focus || container->isAncestorOf(focus));
}
} // namespace

class ScreenshotFormattedTextItem final : public QGraphicsTextItem {
  public:
    using QGraphicsTextItem::QGraphicsTextItem;

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget = nullptr) override {
        QStyleOptionGraphicsItem textOption(*option);
        // Qt's graphics-text focus indicator is a dashed item outline. The pinned surface
        // still needs focus for keyboard selection, but that outline is not part of the HTML.
        textOption.state &= ~QStyle::State_HasFocus;
        QGraphicsTextItem::paint(painter, &textOption, widget);
    }
};

class ScreenshotFormattedTextLayer final : public QGraphicsView {
  public:
    explicit ScreenshotFormattedTextLayer(QWidget* parent = nullptr)
        : QGraphicsView(parent), m_scene(new QGraphicsScene(this)),
          m_textItem(new ScreenshotFormattedTextItem) {
        setObjectName(QStringLiteral("screenshotClipboardText"));
        setScene(m_scene);
        m_scene->addItem(m_textItem);
        setFrameShape(QFrame::NoFrame);
        setContentsMargins(0, 0, 0, 0);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        const QColor background = QGuiApplication::palette().color(QPalette::Base);
        QPalette layerPalette = palette();
        layerPalette.setColor(QPalette::Base, background);
        layerPalette.setColor(QPalette::Window, background);
        setPalette(layerPalette);
        viewport()->setPalette(layerPalette);
        viewport()->setAutoFillBackground(true);
        setBackgroundBrush(background);
        setFocusPolicy(Qt::StrongFocus);
        setInteractive(true);
        setOptimizationFlag(QGraphicsView::DontSavePainterState, true);
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                       QPainter::SmoothPixmapTransform);
        setStyleSheet(QStringLiteral("QGraphicsView#screenshotClipboardText { border: none; }"));

        m_textItem->setObjectName(QStringLiteral("screenshotClipboardTextItem"));
        m_textItem->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                            Qt::TextSelectableByKeyboard);
        m_textItem->setOpenExternalLinks(false);
        m_textItem->setFlag(QGraphicsItem::ItemIsFocusable, true);
        m_textItem->setAcceptedMouseButtons(Qt::LeftButton);
        m_textItem->hide();
        hide();
    }

    ~ScreenshotFormattedTextLayer() override {
        clearDocument();
    }

    void setDocument(std::shared_ptr<QTextDocument> document, const QRectF& canvasRect,
                     qreal devicePixelRatio) {
        clearDocument();
        if (document == nullptr || !canvasRect.isValid() || canvasRect.isEmpty() ||
            !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0) {
            return;
        }
        m_document = std::move(document);
        m_canvasRect = canvasRect.normalized();
        m_textItem->setDocument(m_document.get());
        m_textItem->setPos(m_canvasRect.topLeft());
        m_textItem->setScale(devicePixelRatio);
        m_textItem->show();
    }

    void paintPrintViewport(QPainter& painter) const {
        if (!m_document)
            return;
        painter.save();
        painter.setTransform(m_textItem->sceneTransform() * viewportTransform(), true);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette = palette();
        m_document->documentLayout()->draw(&painter, context);
        painter.restore();
    }

    void clearDocument() {
        if (m_textItem != nullptr) {
            m_textItem->clearFocus();
            m_textItem->hide();
            m_textItem->setScale(1.0);
            m_textItem->setDocument(nullptr);
        }
        m_document.reset();
        m_canvasRect = {};
        hide();
    }

    void synchronize(const QTransform& canvasToViewTransform, const QRect& viewportRect) {
        if (m_document == nullptr || m_canvasRect.isEmpty() || viewportRect.isEmpty()) {
            hide();
            return;
        }
        if (geometry() != viewportRect) {
            setGeometry(viewportRect);
        }
        setSceneRect(m_canvasRect);
        setTransform(canvasToViewTransform, false);
        show();
        raise();
        viewport()->update();
    }

    void focusText() {
        setFocus(Qt::OtherFocusReason);
        m_textItem->setFocus(Qt::OtherFocusReason);
    }

  private:
    QGraphicsScene* m_scene = nullptr;
    QGraphicsTextItem* m_textItem = nullptr;
    std::shared_ptr<QTextDocument> m_document;
    QRectF m_canvasRect;
};

ScreenshotRecognitionWindow::ScreenshotRecognitionWindow(
    ScreenshotRecognitionWindowActions actions, QWidget* parent, PresentationMode presentationMode,
    snow_shot::presentation::WindowShortcutManager* shortcutManager)
    : QWidget(parent, presentationMode == PresentationMode::EmbeddedChild
                          ? Qt::Widget
                          : Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      m_actions(std::move(actions)), m_contentContainer(new QWidget(this)),
      m_stack(new QStackedLayout(m_contentContainer)),
      m_textLayer(new ScreenshotOcrTextLayer(this)), m_presentationMode(presentationMode) {
    if (shortcutManager == nullptr) {
        m_ownedShortcutManager = std::make_unique<snow_shot::presentation::WindowShortcutManager>();
        shortcutManager = m_ownedShortcutManager.get();
    }
    m_shortcutManager = shortcutManager;
    // The recognition surface is an input-bearing top-level window. Register
    // it with the manager that owns the surrounding screenshot session so
    // shortcut dispatch remains active even before the platform publishes its
    // transient-parent relationship.
    m_shortcutManager->addScopeWindow(this);
    setObjectName(QStringLiteral("screenshotRecognitionWindow"));
    if (m_presentationMode == PresentationMode::TopLevelWindow) {
        // This is an exact selection overlay, not a floating panel. Cocoa otherwise
        // shadows every nontransparent pixel, outlining Message's painted shadow.
        setWindowFlag(Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground, true);
    }
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    auto* outerLayout = new QStackedLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSizeConstraint(QLayout::SetNoConstraint);
    outerLayout->addWidget(m_contentContainer);
    m_contentContainer->setObjectName(QStringLiteral("screenshotRecognitionContent"));
    // The mouse-transparent OCR layer exposes this container as the hit-test
    // receiver. It must receive button-free moves so they can reach the window's
    // hover cursor and selection-resize handling in both presentation modes.
    m_contentContainer->setMouseTracking(true);
    m_stack->setContentsMargins(0, 0, 0, 0);
    // This window is an exact overlay for the screenshot selection. Child pages such as
    // QGraphicsView and AdTextEdit have useful standalone minimum size hints, but those hints
    // must never enlarge the overlay and desynchronize it from the selected pixels.
    m_stack->setSizeConstraint(QLayout::SetNoConstraint);
    m_stack->setStackingMode(QStackedLayout::StackOne);
    m_stack->addWidget(m_textLayer);
    m_textLayer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_textLayer->viewport()->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    registerWindowShortcuts();
    m_originalImagePreviewHost = this;
    installEventFilter(this);
    connect(qApp, &QGuiApplication::screenAdded, this, [this] {
        m_originalImagePreviewHostHandle = nullptr;
        refreshOriginalImagePreview();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] {
        m_originalImagePreviewHostHandle = nullptr;
        refreshOriginalImagePreview();
    });
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this] { refreshOriginalImagePreview(); });
#endif
}

ScreenshotRecognitionWindow::~ScreenshotRecognitionWindow() {
    clearLatexPreview();
    delete m_originalImagePreview.data();
    clearFormattedText();
    if (m_textEditor != nullptr) {
        const QSignalBlocker blocker(m_textEditor);
        m_textEditor->setDocument(nullptr);
    }
    if (m_tableEditor != nullptr) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        m_tableEditor->clearSession();
#endif
    }
}

void ScreenshotRecognitionWindow::installSelectionResizeEventFilters(QWidget* widget) {
    if (widget == nullptr) {
        return;
    }
    if (widget != this) {
        widget->installEventFilter(this);
    }
    const QList<QWidget*> children = widget->findChildren<QWidget*>();
    for (QWidget* child : children) {
        if (child != nullptr) {
            child->installEventFilter(this);
        }
    }
}

bool ScreenshotRecognitionWindow::present(const Config& config) {
    if (config.screen == nullptr || !config.geometry.isValid() || config.geometry.isEmpty() ||
        !config.canvasSelection.isValid() || config.canvasSelection.isEmpty() ||
        !std::isfinite(config.formattedTextDevicePixelRatio) ||
        config.formattedTextDevicePixelRatio <= 0.0) {
        return false;
    }

    m_canvasSelection = config.canvasSelection.normalized();
    m_originalImagePreviewTransientOwner = config.transientOwner;
    m_formattedTextDevicePixelRatio = config.formattedTextDevicePixelRatio;
    m_presentationMode = config.presentationMode;
    if (m_presentationMode == PresentationMode::TopLevelWindow) {
#ifdef Q_OS_MACOS
        // Recognition is selection content: keep it below the toolbar and popovers,
        // even when showing or activating this window raises its native surface.
        snow_shot::platform::configureScreenshotRecognitionWindow(this);
#endif
        static_cast<void>(winId());
        if (QWindow* handle = windowHandle()) {
            handle->setScreen(config.screen);
            if (config.transientOwner != nullptr) {
                static_cast<void>(config.transientOwner->winId());
                handle->setTransientParent(config.transientOwner->windowHandle());
            }
        }
    }
    // The selection owns this surface's size. Native edge resizing must not
    // intercept the mouse events used to resize the screenshot selection.
    if (m_presentationMode == PresentationMode::TopLevelWindow) {
        setFixedSize(config.geometry.size());
    }
    setGeometry(config.geometry);
    show();
    if (m_presentationMode == PresentationMode::TopLevelWindow) {
        raise();
        activateWindow();
    }
    if (config.takeFocus) {
        setFocus(Qt::OtherFocusReason);
    }
    synchronizeTextLayer();
    installSelectionResizeEventFilters(this);
    refreshOriginalImagePreview();
    return true;
}

bool ScreenshotRecognitionWindow::updateSelectionGeometry(const QRect& geometry,
                                                          const QRectF& canvasSelection) {
    if (!geometry.isValid() || geometry.isEmpty() || !canvasSelection.isValid() ||
        canvasSelection.isEmpty()) {
        return false;
    }
    m_canvasSelection = canvasSelection.normalized();
    if (m_presentationMode == PresentationMode::TopLevelWindow) {
        setFixedSize(geometry.size());
    }
    setGeometry(geometry);
    synchronizeTextLayer();
    update();
    refreshOriginalImagePreview();
    return true;
}

void ScreenshotRecognitionWindow::setOriginalImagePreviewEnabled(bool enabled) {
    if (m_originalImagePreviewEnabled == enabled) {
        return;
    }
    m_originalImagePreviewEnabled = enabled;
    refreshOriginalImagePreview();
}

bool ScreenshotRecognitionWindow::originalImagePreviewEnabled() const {
    return m_originalImagePreviewEnabled;
}

void ScreenshotRecognitionWindow::setOriginalImagePreviewSource(QImage image,
                                                                const QRectF& canvasRect) {
    if (m_originalImagePreviewSource.cacheKey() == image.cacheKey() &&
        m_originalImagePreviewCanvasRect == canvasRect) {
        return;
    }
    m_originalImagePreviewSource = std::move(image);
    m_originalImagePreviewCanvasRect = canvasRect;
    refreshOriginalImagePreview();
}

void ScreenshotRecognitionWindow::setOriginalImagePreviewProvider(
    std::function<std::optional<ScreenshotOriginalImagePreviewState>()> provider, QWidget* host) {
    if (m_originalImagePreviewHost && m_originalImagePreviewHost != this) {
        m_originalImagePreviewHost->removeEventFilter(this);
    }
    m_originalImagePreviewProvider = std::move(provider);
    m_originalImagePreviewHost = host != nullptr ? host : this;
    m_originalImagePreviewHost->installEventFilter(this);
    if (m_originalImagePreviewHostHandle) {
        m_originalImagePreviewHostHandle->removeEventFilter(this);
    }
    m_originalImagePreviewHostHandle = nullptr;
    refreshOriginalImagePreview();
}

void ScreenshotRecognitionWindow::setOriginalImagePreviewSuppressed(bool suppressed) {
    if (m_originalImagePreviewSuppressed == suppressed) {
        return;
    }
    m_originalImagePreviewSuppressed = suppressed;
    refreshOriginalImagePreview();
}

void ScreenshotRecognitionWindow::setOriginalImagePreviewAboveSiblingProvider(
    std::function<QWidget*()> provider) {
    m_originalImagePreviewAboveSiblingProvider = std::move(provider);
    refreshOriginalImagePreview();
}

void ScreenshotRecognitionWindow::syncOriginalImagePreviewStacking(bool staysOnTop) {
    if (m_originalImagePreviewStaysOnTop == staysOnTop) {
        return;
    }
    m_originalImagePreviewStaysOnTop = staysOnTop;
    refreshOriginalImagePreview();
}

void ScreenshotRecognitionWindow::refreshOriginalImagePreview() {
    if (!companionPreviewEnabled() || m_originalImagePreviewSuppressed || m_showOriginalImage ||
        !isVisible() || !m_originalImagePreviewHost || !m_originalImagePreviewHost->isVisible() ||
        m_originalImagePreviewHost->isMinimized()) {
        destroyOriginalImagePreview();
        return;
    }
    if (m_originalImagePreviewRefreshPending) {
        return;
    }
    m_originalImagePreviewRefreshPending = true;
    QTimer::singleShot(0, this, [this] {
        m_originalImagePreviewRefreshPending = false;
        updateOriginalImagePreview();
    });
}

void ScreenshotRecognitionWindow::destroyOriginalImagePreview() {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_latexRenderer && m_latexRenderPending) {
        m_latexRenderer->cancel();
        m_latexRequestToken = 0;
        m_latexRenderPending = false;
        m_latexFallbackPending = false;
        m_latexPendingKey = {};
        m_latexFallbackAttemptKey = {};
        m_latexRequestedSize = {};
    }
#endif
    if (!m_originalImagePreview) {
        return;
    }
    auto* preview = m_originalImagePreview.data();
    m_originalImagePreview = nullptr;
    preview->removeEventFilter(this);
    preview->hide();
    // Hide can arrive while Qt is dispatching events to the preview or its host.
    preview->deleteLater();
}

void ScreenshotRecognitionWindow::observeOriginalImagePreviewHost() {
    QWindow* handle = m_originalImagePreviewHost->window()->windowHandle();
    if (m_originalImagePreviewHostHandle == handle &&
        !m_originalImagePreviewConnections.isEmpty()) {
        return;
    }
    for (const auto& connection : std::as_const(m_originalImagePreviewConnections)) {
        disconnect(connection);
    }
    m_originalImagePreviewConnections.clear();
    if (m_originalImagePreviewHostHandle) {
        m_originalImagePreviewHostHandle->removeEventFilter(this);
    }
    m_originalImagePreviewHostHandle = handle;
    if (handle != nullptr) {
        handle->installEventFilter(this);
        m_originalImagePreviewConnections.append(connect(
            handle, &QWindow::screenChanged, this, [this] { refreshOriginalImagePreview(); }));
    }
    for (QScreen* screen : QGuiApplication::screens()) {
        m_originalImagePreviewConnections.append(
            connect(screen, &QScreen::availableGeometryChanged, this,
                    [this] { refreshOriginalImagePreview(); }));
        m_originalImagePreviewConnections.append(connect(
            screen, &QScreen::geometryChanged, this, [this] { refreshOriginalImagePreview(); }));
        m_originalImagePreviewConnections.append(
            connect(screen, &QScreen::logicalDotsPerInchChanged, this,
                    [this] { refreshOriginalImagePreview(); }));
    }
}

void ScreenshotRecognitionWindow::updateOriginalImagePreview() {
    if (!companionPreviewEnabled() || m_originalImagePreviewSuppressed || m_showOriginalImage ||
        !isVisible() || !m_originalImagePreviewHost || !m_originalImagePreviewHost->isVisible() ||
        m_originalImagePreviewHost->isMinimized()) {
        destroyOriginalImagePreview();
        return;
    }
    observeOriginalImagePreviewHost();
    std::optional<ScreenshotOriginalImagePreviewState> state;
    if (m_originalImagePreviewProvider) {
        state = m_originalImagePreviewProvider();
    } else {
        state.emplace();
        state->image = m_originalImagePreviewSource;
        state->imageRectInViewport =
            canvasToLocalTransform().mapRect(m_originalImagePreviewCanvasRect);
        if (ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry()) {
            const qreal dpr = devicePixelRatioF();
            state->imageRectInViewport = QRectF(state->imageRectInViewport.topLeft() * dpr,
                                                state->imageRectInViewport.size() * dpr);
        }
        state->resultRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(this);
        state->transientOwner = m_originalImagePreviewTransientOwner
                                    ? m_originalImagePreviewTransientOwner.data()
                                    : this;
    }
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (state && m_latexPreviewEnabled)
        prepareLatexPreview(*state);
#endif
    if (!state || (!state->formula && state->image.isNull()) || state->resultRect.isEmpty() ||
        !state->imageRectInViewport.isValid()) {
        destroyOriginalImagePreview();
        return;
    }
    state->staysOnTop = m_originalImagePreviewStaysOnTop;
    if (m_originalImagePreviewAboveSiblingProvider) {
        state->aboveSibling = m_originalImagePreviewAboveSiblingProvider();
    }
    if (!m_originalImagePreview) {
        m_originalImagePreview = new ScreenshotOriginalImagePreviewWindow(this);
        auto* preview = m_originalImagePreview.data();
        connect(preview, &ScreenshotOriginalImagePreviewWindow::hidden, this, [this, preview] {
            if (m_originalImagePreview == preview) {
                destroyOriginalImagePreview();
            }
        });
    }
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_originalImagePreview->setAccessibleName(m_latexPreviewEnabled ? tr("LaTeX formula preview")
                                                                    : tr("Original image preview"));
#endif
    if (!m_originalImagePreview->present(*state)) {
        destroyOriginalImagePreview();
    }
}

std::optional<ScreenshotRecognitionImageSnapshot>
ScreenshotRecognitionWindow::imageSnapshot(QImage image, const QRectF& canvasRect,
                                           QImage filteredImage, const QRectF& filteredCanvasRect,
                                           const ScreenshotResultStyle& style) const {
    if (m_showOriginalImage || m_stack->currentWidget() != m_textLayer || image.isNull())
        return std::nullopt;
    ScreenshotRecognitionImageSnapshot snapshot;
    snapshot.image = std::move(image);
    snapshot.canvasRect = canvasRect;
    snapshot.filteredImage = std::move(filteredImage);
    snapshot.filteredCanvasRect = filteredCanvasRect;
    if (m_ocrPresentation != nullptr)
        snapshot.lines = m_ocrPresentation->lines;
    snapshot.font = QApplication::font();
    snapshot.textColor = m_textLayer->textColor();
    snapshot.resultStyle = style;
    return snapshot;
}

QImage ScreenshotRecognitionWindow::printViewportSnapshot(QImage background,
                                                          const QRectF& canvasRect, QImage filtered,
                                                          const QRectF& filteredRect,
                                                          qreal contentOpacity) {
    if (m_showOriginalImage || !m_stack->currentWidget())
        return {};
    QWidget* content = m_stack->currentWidget();
    QTextEdit* text = qobject_cast<QTextEdit*>(content);
    if (!text) {
        for (auto* candidate : content->findChildren<QTextEdit*>()) {
            if (candidate->isVisibleTo(content)) {
                text = candidate;
                break;
            }
        }
    }
    auto* table = qobject_cast<QTableView*>(content);
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (table && table == m_tableEditor && m_tableEditor->isEditingCell() &&
        !m_tableEditor->commitActiveEdit())
        return {};
#endif
    QWidget* viewport = content;
    if (auto* scroll = qobject_cast<QAbstractScrollArea*>(content))
        viewport = scroll->viewport();
    if (text)
        viewport = text->viewport();
    const qreal dpr = viewport->devicePixelRatioF();
    QImage snapshot(viewport->size() * dpr, QImage::Format_ARGB32_Premultiplied);
    if (snapshot.isNull())
        return {};
    snapshot.fill(Qt::transparent);
    QPainter painter(&snapshot);
    painter.scale(dpr, dpr);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (content == m_textLayer) {
        const auto transform = canvasToLocalTransform();
        painter.save();
        painter.setTransform(transform, true);
        if (!background.isNull() && !canvasRect.isEmpty())
            painter.drawImage(canvasRect, background);
        if (!filtered.isNull() && !filteredRect.isEmpty())
            painter.drawImage(filteredRect, filtered);
        painter.restore();
        if (!m_selectionOnly && m_ocrPresentation) {
            for (const auto& line : m_ocrPresentation->lines) {
                ScreenshotOcrTextLayout layout;
                QTransform textTransform;
                if (configureScreenshotOcrTextLayout(layout, line, transform, QApplication::font(),
                                                     m_textLayer->textColor(), {},
                                                     &textTransform)) {
                    painter.save();
                    painter.setTransform(textTransform, true);
                    layout.paint(&painter);
                    painter.restore();
                }
            }
        }
    } else if (content == m_formattedTextLayer) {
        painter.fillRect(viewport->rect(), viewport->palette().base());
        m_formattedTextLayer->paintPrintViewport(painter);
    } else if (text) {
        // AdTextEdit has a transparent base; its visible background belongs to
        // the editor container. Other recognition browsers paint their own base.
        painter.fillRect(viewport->rect(), text == m_textEditor
                                               ? m_textEditorContainer->palette().window()
                                               : text->palette().base());
        painter.save();
        painter.translate(-text->horizontalScrollBar()->value(),
                          -text->verticalScrollBar()->value());
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette = text->palette();
        text->document()->documentLayout()->draw(&painter, context);
        painter.restore();
    } else if (table && table->model()) {
        painter.fillRect(viewport->rect(), table->palette().base());
        QSet<QModelIndex> painted;
        const int firstRow = std::max(0, table->rowAt(0));
        const int firstColumn = std::max(0, table->columnAt(0));
        const int bottom = table->rowAt(viewport->height() - 1);
        const int right = table->columnAt(viewport->width() - 1);
        const int lastRow = bottom < 0 ? table->model()->rowCount() - 1 : bottom;
        const int lastColumn = right < 0 ? table->model()->columnCount() - 1 : right;
        for (int row = firstRow; row <= lastRow; ++row) {
            for (int column = firstColumn; column <= lastColumn; ++column) {
                const auto index = table->model()->index(row, column);
                const QRect rect = table->visualRect(index);
                const QRect visible = rect.intersected(viewport->rect());
                if (visible.isEmpty())
                    continue;
                const auto anchor = table->indexAt(visible.center());
                if (!anchor.isValid() || painted.contains(anchor))
                    continue;
                painted.insert(anchor);
                QStyleOptionViewItem option;
                option.initFrom(table);
                option.state = QStyle::State_Enabled;
                option.rect = table->visualRect(anchor);
                option.font = table->font();
                option.widget = table;
                table->itemDelegate()->paint(&painter, option, anchor);
            }
        }
    } else {
        viewport->render(&painter);
    }
    if (contentOpacity < 1.0) {
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.fillRect(viewport->rect(),
                         QColor(0, 0, 0, qRound(255 * std::clamp(contentOpacity, 0.0, 1.0))));
    }
    return snapshot;
}

void ScreenshotRecognitionWindow::setShowOriginalImage(bool show) {
    m_showOriginalImage = show;
    m_contentContainer->setVisible(!show);
    if (show) {
        setFocus(Qt::OtherFocusReason);
    }
    refreshOriginalImagePreview();
}

void ScreenshotRecognitionWindow::setOcrCopyDefaultsEnabled(bool enabled) {
    m_ocrCopyDefaultsEnabled = enabled;
}

void ScreenshotRecognitionWindow::setOcrPresentation(
    std::shared_ptr<ScreenshotOcrPresentation> presentation,
    ScreenshotOcrTextLayer::RenderingMode mode, bool takeFocus) {
    clearImageConversion();
    hideTextEditor();
    clearFormattedText();
    clearTableSession();
    clearQrContents();
    m_selectionOnly = mode == ScreenshotOcrTextLayer::RenderingMode::SelectionOnly;
    m_ocrPresentation = std::move(presentation);
    m_textLayer->setPresentation(m_ocrPresentation, mode);
    m_stack->setCurrentWidget(m_textLayer);
    synchronizeTextLayer();
    if (takeFocus && !m_showOriginalImage) {
        setFocus(Qt::OtherFocusReason);
    }
}

void ScreenshotRecognitionWindow::updateOcrSelection() {
    m_textLayer->updateSelection();
}

void ScreenshotRecognitionWindow::clearOcrSelection() {
    if (m_ocrPresentation != nullptr) {
        m_ocrPresentation->clearTextSelection();
        m_textLayer->updateSelection();
    }
    unsetCursor();
}

void ScreenshotRecognitionWindow::updateOcrText(int lineIndex, const QString& text) {
    if (m_ocrPresentation != nullptr) {
        m_ocrPresentation->setLineText(lineIndex, text);
        m_textLayer->updateLineText(lineIndex);
    }
}

void ScreenshotRecognitionWindow::clearOcrPresentation() {
    m_selectionOnly = false;
    m_ocrPresentation.reset();
    m_textLayer->clearPresentation();
    unsetCursor();
}

void ScreenshotRecognitionWindow::showFormattedText(std::shared_ptr<QTextDocument> document) {
    if (document == nullptr) {
        return;
    }
    clearImageConversion();
    hideTextEditor();
    clearOcrPresentation();
    clearTableSession();
    clearQrContents();
    if (m_formattedTextLayer == nullptr) {
        m_formattedTextLayer = new ScreenshotFormattedTextLayer(this);
        m_stack->addWidget(m_formattedTextLayer);
        installSelectionResizeEventFilters(m_formattedTextLayer);
    }
    m_formattedTextLayer->setDocument(std::move(document), m_canvasSelection,
                                      m_formattedTextDevicePixelRatio);
    m_stack->setCurrentWidget(m_formattedTextLayer);
    synchronizeTextLayer();
    if (!m_showOriginalImage) {
        m_formattedTextLayer->focusText();
    }
}

void ScreenshotRecognitionWindow::clearFormattedText() {
    if (m_formattedTextLayer == nullptr) {
        return;
    }
    m_formattedTextLayer->clearDocument();
    m_stack->removeWidget(m_formattedTextLayer);
    delete m_formattedTextLayer;
    m_formattedTextLayer = nullptr;
    m_stack->setCurrentWidget(m_textLayer);
}

void ScreenshotRecognitionWindow::setTableSession(
    std::shared_ptr<ScreenshotTableEditingSession> session) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    clearImageConversion();
    hideTextEditor();
    clearFormattedText();
    clearOcrPresentation();
    clearQrContents();
    if (m_tableEditor == nullptr) {
        m_tableEditor = new ScreenshotTableEditor(this);
        m_stack->addWidget(m_tableEditor);
        installSelectionResizeEventFilters(m_tableEditor);
        connect(m_tableEditor, &ScreenshotTableEditor::commandStateChanged, this,
                [this](const ScreenshotTableCommandState& state) {
                    if (m_actions.handleTableCommandStateChanged) {
                        m_actions.handleTableCommandStateChanged(state);
                    }
                });
        connect(
            m_tableEditor, &ScreenshotTableEditor::operationRejected, this,
            [this](const QString& message) { m_actions.handleTableOperationRejected(message); });
        connect(m_tableEditor, &ScreenshotTableEditor::copyCompleted, this, [this]() {
            // Context-menu copies originate inside the table editor's
            // copy method. Defer capture teardown until that call has
            // returned, otherwise the editor can be destroyed
            // re-entrantly while it is still unwinding.
            QTimer::singleShot(0, this, [this]() { m_actions.handleCopy(); });
        });
    }
    m_tableEditor->setSession(std::move(session));
    m_stack->setCurrentWidget(m_tableEditor);
    if (!m_showOriginalImage) {
        m_tableEditor->setFocus(Qt::OtherFocusReason);
    }
#else
    Q_UNUSED(session)
#endif
}

void ScreenshotRecognitionWindow::clearTableSession() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableEditor == nullptr) {
        return;
    }
    m_tableEditor->clearSession();
    m_stack->removeWidget(m_tableEditor);
    delete m_tableEditor;
    m_tableEditor = nullptr;
    m_stack->setCurrentWidget(m_textLayer);
#endif
}

ScreenshotTableCommandState ScreenshotRecognitionWindow::tableCommandState() const {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    return m_tableEditor != nullptr ? m_tableEditor->commandState() : ScreenshotTableCommandState{};
#else
    return {};
#endif
}

void ScreenshotRecognitionWindow::mergeTableSelection() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableEditor != nullptr) {
        m_tableEditor->mergeSelection();
    }
#endif
}

void ScreenshotRecognitionWindow::splitTableSelection() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableEditor != nullptr) {
        m_tableEditor->splitSelection();
    }
#endif
}

void ScreenshotRecognitionWindow::resetTable() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableEditor != nullptr) {
        m_tableEditor->resetDocument();
    }
#endif
}

void ScreenshotRecognitionWindow::undoTableEdit() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableEditor != nullptr) {
        m_tableEditor->undoEdit();
    }
#endif
}

void ScreenshotRecognitionWindow::redoTableEdit() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableEditor != nullptr) {
        m_tableEditor->redoEdit();
    }
#endif
}

void ScreenshotRecognitionWindow::commitActiveTableEdit() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableEditor != nullptr) {
        static_cast<void>(m_tableEditor->commitActiveEdit());
    }
#endif
}

bool ScreenshotRecognitionWindow::companionPreviewEnabled() const {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    return m_originalImagePreviewEnabled || m_latexPreviewEnabled;
#else
    return m_originalImagePreviewEnabled;
#endif
}

void ScreenshotRecognitionWindow::setLatexPreviewEnabled(bool enabled) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_latexPreviewEnabled == enabled)
        return;
    m_latexPreviewEnabled = enabled;
    if (!enabled && m_latexRenderer) {
        m_latexRenderer->cancel();
        m_latexRequestToken = 0;
        m_latexRenderPending = false;
        m_latexFallbackPending = false;
        m_latexPendingKey = {};
        m_latexFallbackAttemptKey = {};
        m_latexRequestedSize = {};
    }
    refreshOriginalImagePreview();
#else
    Q_UNUSED(enabled)
#endif
}

void ScreenshotRecognitionWindow::clearLatexPreview() {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    const bool enabled = std::exchange(m_latexPreviewEnabled, false);
    if (enabled && m_textEditor)
        m_textEditor->setAccessibleName({});
    disconnect(m_latexDocumentConnection);
    m_latexDocument = nullptr;
    m_latexSource = {};
    if (m_latexRenderer)
        m_latexRenderer->clearCache();
    if (m_latexPreviewTimer)
        m_latexPreviewTimer->stop();
    m_latexRequestToken = 0;
    m_latexPreviewImage = {};
    m_latexValidKey = {};
    m_latexPendingKey = {};
    m_latexFallbackAttemptKey = {};
    m_latexRequestedSource.clear();
    m_latexRequestedSize = {};
    m_latexPreviewError = 0;
    m_latexRenderPending = false;
    m_latexFallbackPending = false;
    if (enabled)
        refreshOriginalImagePreview();
#endif
}

void ScreenshotRecognitionWindow::showLatexEditor(QTextDocument* document,
                                                  std::function<QString()> source) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    showTextEditor(document);
    if (!document || !m_textEditor)
        return;
    m_textEditor->setAccessibleName(tr("LaTeX formula source"));
    m_latexDocument = document;
    m_latexSource = std::move(source);
    if (!m_latexPreviewTimer) {
        m_latexPreviewTimer = new QTimer(this);
        m_latexPreviewTimer->setSingleShot(true);
        m_latexPreviewTimer->setInterval(150);
        connect(m_latexPreviewTimer, &QTimer::timeout, this,
                &ScreenshotRecognitionWindow::refreshOriginalImagePreview);
    }
    m_latexDocumentConnection = connect(document, &QTextDocument::contentsChanged, this,
                                        &ScreenshotRecognitionWindow::scheduleLatexPreview);
    setLatexPreviewEnabled(true);
#else
    Q_UNUSED(document)
    Q_UNUSED(source)
#endif
}

#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
QString ScreenshotRecognitionWindow::currentLatexSource() const {
    if (!m_latexDocument)
        return {};
    return m_latexSource ? m_latexSource() : m_latexDocument->toPlainText();
}

QString ScreenshotRecognitionWindow::currentLatexSource(int selectionStart,
                                                        int selectionEnd) const {
    const QString source = currentLatexSource();
    qsizetype sourceStart = source.size();
    qsizetype sourceEnd = source.size();
    qsizetype documentPosition = 0;
    for (qsizetype index = 0; index < source.size(); ++index) {
        if (documentPosition == selectionStart)
            sourceStart = index;
        if (documentPosition == selectionEnd) {
            sourceEnd = index;
            break;
        }
        // QTextDocument stores a CRLF pair as one paragraph position. Other source
        // characters, including NBSP and Unicode separators, each retain their position.
        if (source.at(index) == QLatin1Char('\r') && index + 1 < source.size() &&
            source.at(index + 1) == QLatin1Char('\n')) {
            ++index;
        }
        ++documentPosition;
    }
    return source.mid(sourceStart, sourceEnd - sourceStart);
}

void ScreenshotRecognitionWindow::scheduleLatexPreview() {
    if (m_latexRenderPending && m_latexRenderer) {
        m_latexRenderer->cancel();
        m_latexRequestedSize = {};
    }
    m_latexRequestToken = 0;
    m_latexRenderPending = false;
    m_latexFallbackPending = false;
    m_latexPendingKey = {};
    m_latexFallbackAttemptKey = {};
    if (currentLatexSource().trimmed().isEmpty()) {
        m_latexPreviewTimer->stop();
        m_latexPreviewImage = {};
        m_latexValidKey = {};
        m_latexRequestedSource.clear();
        m_latexRequestedSize = {};
        m_latexPreviewError = 0;
        refreshOriginalImagePreview();
    } else {
        m_latexPreviewTimer->start();
    }
}

QString ScreenshotRecognitionWindow::latexPreviewStatus() const {
    using Error = ScreenshotLatexRenderer::Error;
    switch (static_cast<Error>(m_latexPreviewError)) {
    case Error::InvalidFormula:
        return tr("Unable to preview this formula. Check the LaTeX source.");
    case Error::LimitExceeded:
        return tr("Formula preview limit exceeded.");
    case Error::InitializationFailed:
        return tr("Formula renderer is unavailable.");
    case Error::None:
        break;
    }
    if (m_latexRenderPending)
        return tr("Rendering formula...");
    if (currentLatexSource().trimmed().isEmpty())
        return tr("No formula to preview");
    return {};
}

void ScreenshotRecognitionWindow::prepareLatexPreview(ScreenshotOriginalImagePreviewState& state) {
    state.formula = true;
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
    state.background = theme.colorBgContainer;
    state.statusColor = theme.colorText;
    const qreal dpr = devicePixelRatioF();
    const qreal geometryScale =
        ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry() ? 1.0 / dpr : 1.0;
    const QSizeF logicalViewport = QSizeF(state.resultRect.size()) * geometryScale;
    const qreal padding =
        std::min({12.0, logicalViewport.width() / 8.0, logicalViewport.height() / 8.0});
    const QSize renderSize(std::max(1, qFloor(logicalViewport.width() - padding * 2)),
                           std::max(1, qFloor(logicalViewport.height() - padding * 2)));
    if (m_latexDocument) {
        const QString source = currentLatexSource();
        if (!source.trimmed().isEmpty() && !m_latexPreviewTimer->isActive() &&
            (source != m_latexRequestedSource || renderSize != m_latexRequestedSize ||
             !qFuzzyCompare(dpr, m_latexRequestedDpr) ||
             theme.colorText != m_latexRequestedForeground)) {
            if (!m_latexRenderer) {
                m_latexRenderer = new ScreenshotLatexRenderer(this);
                connect(m_latexRenderer, &ScreenshotLatexRenderer::rendered, this,
                        [this](quint64 token, const QImage& image,
                               ScreenshotLatexRenderer::Error error) {
                            if (token != m_latexRequestToken || !m_latexPreviewEnabled ||
                                !m_latexDocument)
                                return;
                            m_latexRenderPending = false;
                            if (!m_latexFallbackPending)
                                m_latexPreviewError = static_cast<int>(error);
                            if (error == ScreenshotLatexRenderer::Error::None) {
                                m_latexPreviewImage = image;
                                m_latexValidKey = m_latexPendingKey;
                                if (!m_latexFallbackPending)
                                    m_latexFallbackAttemptKey = {};
                            }
                            m_latexFallbackPending = false;
                            refreshOriginalImagePreview();
                        });
            }
            m_latexRequestedSource = source;
            m_latexRequestedSize = renderSize;
            m_latexRequestedDpr = dpr;
            m_latexRequestedForeground = theme.colorText;
            m_latexRenderPending = true;
            if (m_latexFallbackPending)
                m_latexFallbackAttemptKey = {};
            m_latexFallbackPending = false;
            m_latexPendingKey = {source, renderSize, dpr, theme.colorText};
            m_latexRequestToken =
                m_latexRenderer->request(source, renderSize, dpr, theme.colorText);
        } else if (!m_latexRenderPending && !m_latexPreviewTimer->isActive() &&
                   m_latexPreviewError != 0 && !m_latexPreviewImage.isNull() &&
                   !m_latexValidKey.source.isEmpty()) {
            const LatexRenderKey fallback{m_latexValidKey.source, renderSize, dpr, theme.colorText};
            if (!(fallback == m_latexValidKey) && !(fallback == m_latexFallbackAttemptKey)) {
                // Keep the invalid draft's request key and error intact. The retained formula
                // needs its own raster for a new theme or viewport, not another draft retry.
                m_latexFallbackAttemptKey = fallback;
                m_latexPendingKey = fallback;
                m_latexRenderPending = true;
                m_latexFallbackPending = true;
                m_latexRequestToken = m_latexRenderer->request(fallback.source, fallback.size,
                                                               fallback.dpr, fallback.foreground);
            }
        }
    }
    state.image = m_latexPreviewImage;
    state.dimmed = m_latexPreviewError != 0;
    state.status = latexPreviewStatus();
    const qreal viewScale = 1.0 / geometryScale;
    const QRectF logicalRect(QPointF(), logicalViewport);
    QRectF formulaRect = logicalRect;
    if (!state.image.isNull()) {
        QSizeF formulaSize = QSizeF(state.image.size()) / state.image.devicePixelRatio();
        if (formulaSize.width() > renderSize.width() || formulaSize.height() > renderSize.height())
            formulaSize.scale(QSizeF(renderSize), Qt::KeepAspectRatio);
        formulaRect = QRectF(logicalRect.center() -
                                 QPointF(formulaSize.width() / 2, formulaSize.height() / 2),
                             formulaSize);
    }
    state.imageRectInViewport =
        QRectF(formulaRect.topLeft() * viewScale, formulaRect.size() * viewScale);
}
#endif

void ScreenshotRecognitionWindow::showTextEditor(QTextDocument* document, bool readOnly,
                                                 bool streaming) {
    clearLatexPreview();
    if (document == nullptr) {
        return;
    }
    clearImageConversion();
    clearOcrPresentation();
    clearFormattedText();
    clearTableSession();
    clearQrContents();
    if (m_textEditor == nullptr) {
        m_textEditorContainer = new QWidget(this);
        m_textEditorContainer->setObjectName(QStringLiteral("screenshotOcrEditorContainer"));
        m_textEditorContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* editorLayout = new QVBoxLayout(m_textEditorContainer);
        editorLayout->setContentsMargins(0, 0, 0, 0);
        auto* editor = new adqt::widgets::AdTextEdit(m_textEditorContainer);
        m_textEditor = editor;
        m_textEditor->setObjectName(QStringLiteral("screenshotOcrEditor"));
        editor->setHeightMode(adqt::widgets::AdTextEdit::HeightMode::FixedGeometry);
        editor->setVariant(adqt::widgets::AdTextEdit::Variant::Borderless);
        m_textEditor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_textEditor->setAcceptRichText(false);
        m_textEditor->setContextMenuPolicy(Qt::CustomContextMenu);
        editorLayout->addWidget(editor);
        m_textEditorSpin = new adqt::widgets::AdSpin(m_textEditorContainer);
        m_textEditorSpin->setObjectName(QStringLiteral("screenshotOcrTranslationSpin"));
        m_textEditorSpin->setSizeClass(adqt::widgets::AdSpin::SizeClass::Small);
        m_textEditorSpin->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_textEditorSpin->hide();
        applyTextEditorContainerBackground(m_textEditorContainer);
        connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                m_textEditorContainer, [container = m_textEditorContainer]() {
                    applyTextEditorContainerBackground(container);
                });
        m_stack->addWidget(m_textEditorContainer);
        installSelectionResizeEventFilters(m_textEditorContainer);
        connect(editor, &adqt::widgets::AdTextEdit::textEdited, this, [this](const QString& text) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            // The formula session observes the document directly and preserves source
            // whitespace. AdTextEdit's plain-text notification normalizes that source.
            if (m_latexDocument)
                return;
#endif
            m_actions.handleTextEdited(text);
        });
        connect(m_textEditor, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& position) {
                    showTextEditorContextMenu(m_textEditor->viewport()->mapToGlobal(position));
                });
    }
    const QSignalBlocker blocker(m_textEditor);
    m_textEditor->setDocument(document);
    m_textEditor->setReadOnly(readOnly);
    const adqt::theme::ThemeMapToken theme =
        adqt::theme::ThemeManager::instance().resolveTheme(m_textEditor);
    QFont editorFont = theme.appFont.family().isEmpty() ? m_textEditor->font() : theme.appFont;
    editorFont.setPixelSize(qRound(theme.fontSize));
    m_textEditor->setFont(editorFont);
    document->setDefaultFont(editorFont);
    m_stack->setCurrentWidget(m_textEditorContainer);
    setTextEditorStreaming(streaming);
    if (!readOnly && !m_showOriginalImage) {
        static_cast<adqt::widgets::AdTextEdit*>(m_textEditor)->focusEditor();
    }
}

void ScreenshotRecognitionWindow::registerWindowShortcuts() {
    using ShortcutManager = snow_shot::presentation::WindowShortcutManager;

    ShortcutManager::Binding cancel;
    cancel.id = QStringLiteral("recognition.cancel");
    cancel.keyCombinations = {QKeyCombination(Qt::NoModifier, Qt::Key_Escape)};
    cancel.priority = ShortcutManager::StandardPriority::WindowCommand;
    cancel.activationTrigger = ShortcutManager::Binding::ActivationTrigger::Release;
    cancel.canActivate = [this](const ShortcutManager::ActivationContext& context) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        return context.scopeWindow == this && isVisible() &&
               (m_tableEditor == nullptr || !m_tableEditor->isEditingCell());
#else
        return context.scopeWindow == this && isVisible();
#endif
    };
    cancel.activate = [this](const auto&) {
        m_actions.handleCancel();
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(cancel)));

#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    ShortcutManager::Binding cancelEdit;
    cancelEdit.id = QStringLiteral("recognition.cancel_edit");
    cancelEdit.keyCombinations = {QKeyCombination(Qt::NoModifier, Qt::Key_Escape)};
    cancelEdit.priority = ShortcutManager::StandardPriority::WindowCommand + 1;
    cancelEdit.canActivate = [this](const auto& context) {
        return context.scopeWindow == this && isVisible() && !m_showOriginalImage &&
               m_tableEditor && m_tableEditor->isEditingCell();
    };
    cancelEdit.activate = [this](const auto&) { return m_tableEditor->cancelActiveEdit(); };
    cancelEdit.release = [](const auto&) { return true; };
    cancelEdit.cancel = [] {};
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(cancelEdit)));
#endif

    const auto recognitionCommandsAllowed =
        [this](const ShortcutManager::ActivationContext& context) {
            QWidget* focus = context.focusWidget;
            if (context.scopeWindow != this || !isVisible() || m_showOriginalImage) {
                return false;
            }

            // OCR's canvas and QR's read-only browser are recognition surfaces. The
            // editable/table/formatted-text pages retain native Qt command handling.
            if (m_conversionView != nullptr) {
                return focusInside(m_conversionView, focus);
            }
            if (m_qrBrowser != nullptr) {
                return focusInside(m_qrBrowser, focus);
            }
            return m_ocrPresentation != nullptr && !focusInside(m_textEditorContainer, focus) &&
                   !focusInside(m_tableEditor, focus) && !focusInside(m_formattedTextLayer, focus);
        };

    const auto copyCommandsAllowed = [this](const ShortcutManager::ActivationContext& context) {
        if (m_selectionOnly && m_presentationMode == PresentationMode::EmbeddedChild) {
            return false;
        }
        QWidget* focus = context.focusWidget;
        if (context.scopeWindow != this || !isVisible()) {
            return false;
        }
        if (m_showOriginalImage) {
            return m_conversionView != nullptr || m_tableEditor != nullptr ||
                   m_textEditor != nullptr || m_qrBrowser != nullptr ||
                   m_ocrPresentation != nullptr;
        }
        if (m_conversionView != nullptr) {
            return focusInside(m_conversionView, focus);
        }
        if (m_tableEditor != nullptr) {
            return focusInside(m_tableEditor, focus);
        }
        if (m_textEditor != nullptr) {
            return focusInside(m_textEditor, focus);
        }
        if (m_qrBrowser != nullptr) {
            return focusInside(m_qrBrowser, focus);
        }
        return m_ocrPresentation != nullptr && !focusInside(m_textEditorContainer, focus) &&
               !focusInside(m_formattedTextLayer, focus);
    };

    ShortcutManager::Binding selectAll;
    selectAll.id = QStringLiteral("recognition.select_all");
    selectAll.keyCombinations = standardCombinations(QKeySequence::SelectAll);
    selectAll.allowedAdditionalModifiers = Qt::ShiftModifier | Qt::AltModifier | Qt::KeypadModifier;
    selectAll.priority = ShortcutManager::StandardPriority::WindowCommand;
    selectAll.canActivate = recognitionCommandsAllowed;
    selectAll.activate = [this](const auto&) {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
        if (m_conversionView != nullptr) {
            m_conversionView->selectAll();
            return true;
        }
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        if (m_qrBrowser != nullptr) {
            m_qrBrowser->selectAll();
            return true;
        }
#endif
        if (m_ocrPresentation == nullptr) {
            return false;
        }
        const quint64 previousRevision = m_ocrPresentation->selectionRevision();
        m_ocrPresentation->selectAll();
        if (m_ocrPresentation->selectionRevision() != previousRevision) {
            m_textLayer->updateSelection();
        }
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(selectAll)));

    ShortcutManager::Binding copy;
    copy.id = QStringLiteral("recognition.copy");
    copy.keyCombinations = standardCombinations(QKeySequence::Copy);
    copy.allowedAdditionalModifiers = Qt::ShiftModifier | Qt::AltModifier | Qt::KeypadModifier;
    copy.priority = ShortcutManager::StandardPriority::WindowCommand;
    copy.canActivate = copyCommandsAllowed;
    copy.activate = [this](const auto&) {
        const bool copied = copyVisibleContentToClipboard();
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        if (m_latexDocument)
            return true;
#endif
        if (copied && m_conversionView == nullptr) {
            m_actions.handleCopy();
        }
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(copy)));

    const auto textEditorActive = [this](const ShortcutManager::ActivationContext& context) {
        return context.scopeWindow == this && focusInside(m_textEditor, context.focusWidget);
    };
    ShortcutManager::Binding undo;
    undo.id = QStringLiteral("recognition.text.undo");
    undo.keyCombinations = standardCombinations(QKeySequence::Undo);
    undo.priority = ShortcutManager::StandardPriority::WindowCommand;
    undo.canActivate = textEditorActive;
    undo.activate = [this](const auto&) {
        m_actions.handleUndoTextEdit();
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(undo)));

    ShortcutManager::Binding redo;
    redo.id = QStringLiteral("recognition.text.redo");
    redo.keyCombinations = standardCombinations(QKeySequence::Redo);
    redo.priority = ShortcutManager::StandardPriority::WindowCommand;
    redo.canActivate = textEditorActive;
    redo.activate = [this](const auto&) {
        m_actions.handleRedoTextEdit();
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(redo)));
}

void ScreenshotRecognitionWindow::setTextEditorStreaming(bool streaming) {
    if (m_textEditor != nullptr) {
        m_textEditor->setReadOnly(streaming);
    }
    if (m_textEditorSpin != nullptr) {
        m_textEditorSpin->setSpinning(streaming);
        m_textEditorSpin->setVisible(streaming);
        if (streaming) {
            updateTextEditorSpinGeometry();
            m_textEditorSpin->raise();
        }
    }
}

void ScreenshotRecognitionWindow::hideTextEditor() {
    clearLatexPreview();
    if (m_textEditor == nullptr) {
        return;
    }
    // Detaching the document can emit textChanged with an empty editor. This is
    // teardown, not a user edit, so do not forward it to the OCR draft cache.
    {
        const QSignalBlocker blocker(m_textEditor);
        m_textEditor->setDocument(nullptr);
    }
    m_stack->removeWidget(m_textEditorContainer);
    delete m_textEditorContainer;
    m_textEditorContainer = nullptr;
    m_textEditor = nullptr;
    m_textEditorSpin = nullptr;
    m_stack->setCurrentWidget(m_textLayer);
}

void ScreenshotRecognitionWindow::showQrContents(const QStringList& contents, bool detectLinks) {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (detectLinks && !snow_shot::app::edition::qrRecognition)
        return;
    clearImageConversion();
    hideTextEditor();
    clearFormattedText();
    clearOcrPresentation();
    clearTableSession();
    if (m_qrBrowser == nullptr) {
        m_qrBrowser = new QTextBrowser(this);
        m_qrBrowser->setObjectName(QStringLiteral("screenshotQrContents"));
        m_qrBrowser->setFrameStyle(QFrame::NoFrame);
        m_qrBrowser->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_qrBrowser->setContentsMargins(0, 0, 0, 0);
        m_qrBrowser->setLineWrapMode(QTextEdit::WidgetWidth);
        m_qrBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_qrBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_qrBrowser->setVerticalScrollBar(
            new adqt::widgets::AdScrollBar(Qt::Vertical, m_qrBrowser));
        m_qrBrowser->setOpenLinks(false);
        m_qrBrowser->setOpenExternalLinks(false);
        m_qrBrowser->setTextInteractionFlags(Qt::TextBrowserInteraction);
        m_qrBrowser->setContextMenuPolicy(Qt::CustomContextMenu);
        m_stack->addWidget(m_qrBrowser);
        installSelectionResizeEventFilters(m_qrBrowser);
        connect(m_qrBrowser, &QTextBrowser::anchorClicked, this,
                [this](const QUrl& url) { m_actions.handleLinkActivated(url); });
        connect(m_qrBrowser, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& position) {
                    showQrContextMenu(m_qrBrowser->viewport()->mapToGlobal(position));
                });
    }

    const adqt::theme::ThemeMapToken theme =
        adqt::theme::ThemeManager::instance().resolveTheme(m_qrBrowser);
    const QMargins contentMargins = adTextAreaContentMargins(theme);
    m_qrBrowser->setStyleSheet(
        QStringLiteral("QTextBrowser#screenshotQrContents { border: none; border-radius: 0; "
                       "padding: %1px %2px; background-color: palette(base); }")
            .arg(contentMargins.top())
            .arg(contentMargins.left()));

    m_qrDetectLinks = detectLinks;
    m_qrBrowser->setAccessibleName(detectLinks ? tr("Barcode recognition result")
                                               : tr("LaTeX formula source"));
    QTextDocument* document = m_qrBrowser->document();
    document->clear();
    document->setDocumentMargin(0.0);
    QFont browserFont = theme.appFont.family().isEmpty() ? m_qrBrowser->font() : theme.appFont;
    browserFont.setPixelSize(qRound(theme.fontSize));
    m_qrBrowser->setFont(browserFont);
    document->setDefaultFont(browserFont);

    QTextCursor cursor(document);
    QTextCharFormat plainFormat;
    plainFormat.setFont(browserFont);
    QTextCharFormat linkFormat = plainFormat;
    linkFormat.setAnchor(true);
    linkFormat.setFontUnderline(true);
    linkFormat.setForeground(theme.colorLink);

    for (qsizetype index = 0; index < contents.size(); ++index) {
        if (index > 0) {
            cursor.insertBlock();
        }
        const QString content = contents.at(index);
        const QString trimmed = content.trimmed();
        QUrl url;
        if (detectLinks && !trimmed.isEmpty() && isHttpUrl(trimmed, &url)) {
            const qsizetype start = content.indexOf(trimmed);
            cursor.insertText(content.left(start), plainFormat);
            linkFormat.setAnchorHref(url.toString(QUrl::FullyEncoded));
            cursor.insertText(trimmed, linkFormat);
            cursor.insertText(content.mid(start + trimmed.size()), plainFormat);
        } else {
            cursor.insertText(content, plainFormat);
        }
    }
    cursor.movePosition(QTextCursor::Start);
    m_qrBrowser->setTextCursor(cursor);
    // Present the decoded payload fully selected so a plain Ctrl+C copies
    // every payload without requiring a manual Select All first.
    m_qrBrowser->selectAll();
    m_stack->setCurrentWidget(m_qrBrowser);
    if (!m_showOriginalImage) {
        m_qrBrowser->setFocus(Qt::OtherFocusReason);
    }
#else
    Q_UNUSED(contents)
    Q_UNUSED(detectLinks)
#endif
}

void ScreenshotRecognitionWindow::clearQrContents() {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_qrBrowser == nullptr) {
        return;
    }
    m_stack->removeWidget(m_qrBrowser);
    delete m_qrBrowser;
    m_qrBrowser = nullptr;
    m_stack->setCurrentWidget(m_textLayer);
#endif
}

void ScreenshotRecognitionWindow::showImageConversion(SnowShotImageConversionFormat format,
                                                      const QString& source, bool busy,
                                                      const QString& error) {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (m_conversionView == nullptr) {
        hideTextEditor();
        clearFormattedText();
        clearOcrPresentation();
        clearTableSession();
        clearQrContents();
        m_conversionView = new ScreenshotImageConversionView(this);
        m_stack->addWidget(m_conversionView);
        installSelectionResizeEventFilters(m_conversionView);
        connect(m_conversionView, &ScreenshotImageConversionView::retryRequested, this,
                &ScreenshotRecognitionWindow::imageConversionRetryRequested);
        connect(m_conversionView, &ScreenshotImageConversionView::linkActivated, this,
                [this](const QUrl& url) { m_actions.handleLinkActivated(url); });
        if (!m_showOriginalImage) {
            m_conversionView->setFocus(Qt::OtherFocusReason);
        }
    }
    m_conversionView->setContent(format, source, busy, error);
    m_stack->setCurrentWidget(m_conversionView);
#else
    Q_UNUSED(format)
    Q_UNUSED(source)
    Q_UNUSED(busy)
    Q_UNUSED(error)
#endif
}

void ScreenshotRecognitionWindow::clearImageConversion() {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (m_conversionView != nullptr) {
        m_stack->removeWidget(m_conversionView);
        delete m_conversionView;
        m_conversionView = nullptr;
        m_stack->setCurrentWidget(m_textLayer);
    }
#endif
}

bool ScreenshotRecognitionWindow::copyVisibleContentToClipboard() {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (m_conversionView != nullptr) {
        return m_conversionView->copyToClipboard();
    }
#endif
    QClipboard* clipboard = QApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }

#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableEditor != nullptr) {
        return m_tableEditor->copySelectionToClipboard();
    }
#endif

    QString text;
    bool contentAvailable = false;
    if (m_textEditor != nullptr) {
        contentAvailable = true;
        const QTextCursor cursor = m_textEditor->textCursor();
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        if (m_latexDocument) {
            clipboard->setText(cursor.hasSelection() ? currentLatexSource(cursor.selectionStart(),
                                                                          cursor.selectionEnd())
                                                     : currentLatexSource());
            return true;
        }
#endif
        text = cursor.hasSelection() ? QTextDocumentFragment(cursor).toPlainText()
                                     : m_textEditor->toPlainText();
    }
#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    else if (m_qrBrowser != nullptr) {
        contentAvailable = true;
        const QTextCursor cursor = m_qrBrowser->textCursor();
        text = cursor.hasSelection() ? QTextDocumentFragment(cursor).toPlainText()
                                     : m_qrBrowser->toPlainText();
    }
#endif
    else if (m_ocrPresentation != nullptr) {
        contentAvailable = true;
        if (m_ocrPresentation->hasTextSelection()) {
            text = m_ocrPresentation->selectedText();
        } else {
            QStringList lines;
            lines.reserve(m_ocrPresentation->lines.size());
            for (const ScreenshotOcrLine& line : m_ocrPresentation->lines) {
                lines.push_back(line.text);
            }
            text = lines.join(QLatin1Char('\n'));
        }
        if (m_ocrCopyDefaultsEnabled) {
            const snow_shot::storage::TextRecognitionSettings settings;
            text = snow_shot::presentation::applyOcrTextTransforms(
                *m_ocrPresentation, settings.defaultFormatting(), settings.defaultPunctuation());
        }
    }
    if (!contentAvailable) {
        return false;
    }
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    clipboard->setText(text);
    return true;
}

bool ScreenshotRecognitionWindow::activeContentOwnsContextMenu(const QObject* watched) const {
    const auto* widget = qobject_cast<const QWidget*>(watched);
    return widget != nullptr &&
           (focusInside(m_conversionView, widget) || focusInside(m_qrBrowser, widget) ||
            focusInside(m_textEditor, widget) || focusInside(m_tableEditor, widget));
}

void ScreenshotRecognitionWindow::showOcrContextMenu(const QPoint& globalPosition) {
    if (m_ocrPresentation == nullptr) {
        return;
    }
    adqt::widgets::AdContextMenu menu(this);
    menu.setObjectName(QStringLiteral("screenshotOcrContextMenu"));
    QAction* copy = menu.addItem(tr("Copy"), adqt::icons::antd::outlined::Copy());
    QAction* selectAll = menu.addItem(tr("Select All"), adqt::icons::antd::outlined::Select());
    selectAll->setEnabled(!m_ocrPresentation->lines.isEmpty());
    connect(copy, &QAction::triggered, this,
            [this]() { static_cast<void>(copyVisibleContentToClipboard()); });
    connect(selectAll, &QAction::triggered, this, [this]() {
        if (m_ocrPresentation == nullptr) {
            return;
        }
        const quint64 previousRevision = m_ocrPresentation->selectionRevision();
        m_ocrPresentation->selectAll();
        if (m_ocrPresentation->selectionRevision() != previousRevision) {
            m_textLayer->updateSelection();
        }
    });
    menu.execAt(globalPosition);
}

void ScreenshotRecognitionWindow::showQrContextMenu(const QPoint& globalPosition) {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_qrBrowser == nullptr) {
        return;
    }
    adqt::widgets::AdContextMenu menu(this);
    menu.setObjectName(QStringLiteral("screenshotQrContextMenu"));
    QAction* copy = menu.addItem(tr("Copy"), adqt::icons::antd::outlined::Copy());
    QAction* selectAll = menu.addItem(tr("Select All"), adqt::icons::antd::outlined::Select());
    selectAll->setEnabled(!m_qrBrowser->document()->isEmpty());
    connect(copy, &QAction::triggered, this,
            [this]() { static_cast<void>(copyVisibleContentToClipboard()); });
    connect(selectAll, &QAction::triggered, m_qrBrowser, &QTextBrowser::selectAll);
    menu.execAt(globalPosition);
#else
    Q_UNUSED(globalPosition)
#endif
}

void ScreenshotRecognitionWindow::showTextEditorContextMenu(const QPoint& globalPosition) {
    if (m_textEditor == nullptr) {
        return;
    }
    adqt::widgets::AdContextMenu menu(this);
    menu.setObjectName(QStringLiteral("screenshotTextEditorContextMenu"));
    QAction* copy = menu.addItem(tr("Copy"), adqt::icons::antd::outlined::Copy());
    QAction* cut = menu.addItem(tr("Cut"), adqt::icons::antd::outlined::Scissor());
    QAction* paste = menu.addItem(tr("Paste"), adqt::icons::antd::outlined::Snippets());
    QAction* remove = menu.addItem(tr("Delete"), adqt::icons::antd::outlined::IconDelete());
    menu.addSeparator();
    QAction* selectAll = menu.addItem(tr("Select All"), adqt::icons::antd::outlined::Select());

    const bool hasSelection = m_textEditor->textCursor().hasSelection();
    const bool writable = !m_textEditor->isReadOnly();
    copy->setEnabled(hasSelection);
    cut->setEnabled(writable && hasSelection);
    paste->setEnabled(writable && m_textEditor->canPaste());
    remove->setEnabled(writable && hasSelection);
    selectAll->setEnabled(!m_textEditor->document()->isEmpty());

    connect(copy, &QAction::triggered, m_textEditor, [this]() {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        if (m_latexDocument) {
            static_cast<void>(copyVisibleContentToClipboard());
            return;
        }
#endif
        m_textEditor->copy();
    });
    connect(cut, &QAction::triggered, m_textEditor, &QTextEdit::cut);
    connect(paste, &QAction::triggered, m_textEditor, &QTextEdit::paste);
    connect(remove, &QAction::triggered, this, [this]() {
        if (m_textEditor == nullptr || m_textEditor->isReadOnly()) {
            return;
        }
        QTextCursor cursor = m_textEditor->textCursor();
        if (cursor.hasSelection()) {
            cursor.removeSelectedText();
            m_textEditor->setTextCursor(cursor);
        }
    });
    connect(selectAll, &QAction::triggered, m_textEditor, &QTextEdit::selectAll);
    menu.execAt(globalPosition);
}

void ScreenshotRecognitionWindow::focusOutEvent(QFocusEvent* event) {
    if (m_ocrPresentation != nullptr && event->reason() != Qt::PopupFocusReason) {
        const quint64 previousRevision = m_ocrPresentation->selectionRevision();
        m_ocrPresentation->clearTextSelection();
        if (m_ocrPresentation->selectionRevision() != previousRevision) {
            m_textLayer->updateSelection();
        }
    }
    QWidget::focusOutEvent(event);
}

ScreenshotSelectionDragMode ScreenshotRecognitionWindow::selectionResizeDragModeAtLocalPoint(
    const QPointF& localPosition) const {
    return m_actions.selectionResizeDragMode(canvasPositionForLocalPoint(localPosition));
}

Qt::CursorShape
ScreenshotRecognitionWindow::cursorForSelectionResize(ScreenshotSelectionDragMode dragMode) {
    switch (dragMode) {
    case ScreenshotSelectionDragMode::TopLeft:
    case ScreenshotSelectionDragMode::BottomRight:
        return Qt::SizeFDiagCursor;
    case ScreenshotSelectionDragMode::TopRight:
    case ScreenshotSelectionDragMode::BottomLeft:
        return Qt::SizeBDiagCursor;
    case ScreenshotSelectionDragMode::Top:
    case ScreenshotSelectionDragMode::Bottom:
        return Qt::SizeVerCursor;
    case ScreenshotSelectionDragMode::Right:
    case ScreenshotSelectionDragMode::Left:
        return Qt::SizeHorCursor;
    case ScreenshotSelectionDragMode::All:
        return Qt::SizeAllCursor;
    case ScreenshotSelectionDragMode::None:
    case ScreenshotSelectionDragMode::Marquee:
    default:
        return Qt::ArrowCursor;
    }
}

void ScreenshotRecognitionWindow::updateSelectionResizeCursor(const QPointF& localPosition) {
    const ScreenshotSelectionDragMode dragMode = selectionResizeDragModeAtLocalPoint(localPosition);
    if (dragMode != ScreenshotSelectionDragMode::None) {
        setCursor(cursorForSelectionResize(dragMode));
        return;
    }
    if (m_ocrPresentation != nullptr) {
        const ScreenshotOcrTextPosition position =
            m_textLayer->textPositionAt(canvasPositionForLocalPoint(localPosition), false);
        setCursor(position.valid() ? Qt::IBeamCursor : Qt::ArrowCursor);
        return;
    }
    unsetCursor();
}

bool ScreenshotRecognitionWindow::handleSelectionResizeEvent(QObject* watched, QEvent* event) {
    if (event == nullptr ||
        (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove &&
         event->type() != QEvent::MouseButtonRelease)) {
        return false;
    }

    auto* mouseEvent = static_cast<QMouseEvent*>(event);
    QPointF localPosition = mouseEvent->position();
    if (auto* watchedWidget = qobject_cast<QWidget*>(watched);
        watchedWidget != nullptr && watchedWidget != this) {
        localPosition = QPointF(watchedWidget->mapTo(this, mouseEvent->position().toPoint()));
    }
    const QPointF canvasPosition = canvasPositionForLocalPoint(localPosition);

    if (event->type() == QEvent::MouseButtonPress && mouseEvent->button() == Qt::LeftButton) {
        const ScreenshotSelectionDragMode dragMode =
            selectionResizeDragModeAtLocalPoint(localPosition);
        if (dragMode == ScreenshotSelectionDragMode::None) {
            return false;
        }
        // Beginning a resize clears recognition content and can delete the pressed viewport.
        // Keep the entire mouse gesture on the surviving window before invoking that callback.
        grabMouse();
        if (!m_actions.beginSelectionResize(canvasPosition)) {
            releaseMouse();
            return false;
        }
        m_selectionResizeActive = true;
        setCursor(cursorForSelectionResize(dragMode));
        mouseEvent->accept();
        return true;
    }

    if (event->type() == QEvent::MouseMove) {
        if (m_selectionResizeActive) {
            m_actions.updateSelectionResize(canvasPosition);
            mouseEvent->accept();
            return true;
        }
        updateSelectionResizeCursor(localPosition);
        if (selectionResizeDragModeAtLocalPoint(localPosition) !=
            ScreenshotSelectionDragMode::None) {
            mouseEvent->accept();
            return true;
        }
        return false;
    }

    if (event->type() == QEvent::MouseButtonRelease && mouseEvent->button() == Qt::LeftButton &&
        m_selectionResizeActive) {
        m_selectionResizeActive = false;
        releaseMouse();
        unsetCursor();
        // Restoring recognition can replace this window. Finish all local state changes and
        // copy callbacks before handing control back to the selection controller.
        const auto finishSelectionResize = m_actions.finishSelectionResize;
        const auto selectionResizeFinished = m_actions.selectionResizeFinished;
        mouseEvent->accept();
        finishSelectionResize(canvasPosition);
        selectionResizeFinished();
        return true;
    }
    return false;
}

bool ScreenshotRecognitionWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_originalImagePreview) {
        return QWidget::eventFilter(watched, event);
    }
    if (event != nullptr && (watched == this || watched == m_originalImagePreviewHost ||
                             watched == m_originalImagePreviewHostHandle)) {
        switch (event->type()) {
        case QEvent::Hide:
        case QEvent::Close:
            destroyOriginalImagePreview();
            refreshOriginalImagePreview();
            break;
        case QEvent::Show:
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::WindowStateChange:
        case QEvent::ScreenChangeInternal:
        case QEvent::DevicePixelRatioChange:
        case QEvent::WinIdChange:
        case QEvent::PlatformSurface:
            refreshOriginalImagePreview();
            break;
        default:
            break;
        }
    }
    if (event != nullptr && event->type() == QEvent::ChildAdded) {
        const auto* childEvent = static_cast<const QChildEvent*>(event);
        if (auto* childWidget = qobject_cast<QWidget*>(childEvent->child());
            childWidget != nullptr) {
            installSelectionResizeEventFilters(childWidget);
        }
    }
    if (event != nullptr && event->type() == QEvent::ContextMenu && watched != this &&
        m_presentationMode == PresentationMode::EmbeddedChild &&
        !activeContentOwnsContextMenu(watched)) {
        auto* contextMenuEvent = static_cast<QContextMenuEvent*>(event);
        emit embeddedContextMenuRequested(contextMenuEvent->globalPos());
        contextMenuEvent->accept();
        return true;
    }
    if (handleSelectionResizeEvent(watched, event)) {
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void ScreenshotRecognitionWindow::contextMenuEvent(QContextMenuEvent* event) {
    if (event == nullptr) {
        return;
    }
    if (!m_showOriginalImage && m_ocrPresentation != nullptr) {
        showOcrContextMenu(event->globalPos());
        event->accept();
        return;
    }
    if (m_presentationMode == PresentationMode::EmbeddedChild) {
        emit embeddedContextMenuRequested(event->globalPos());
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
}

void ScreenshotRecognitionWindow::mousePressEvent(QMouseEvent* event) {
    if (handleSelectionResizeEvent(this, event)) {
        return;
    }
    if (event != nullptr && event->button() == Qt::LeftButton && !m_showOriginalImage &&
        m_ocrPresentation != nullptr) {
        setFocus(Qt::MouseFocusReason);
        const quint64 previousRevision = m_ocrPresentation->selectionRevision();
        m_ocrPresentation->beginTextSelection(
            m_textLayer->textPositionAt(canvasPositionForLocalPoint(event->position()), false));
        if (m_ocrPresentation->selectionRevision() != previousRevision) {
            m_textLayer->updateSelection();
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ScreenshotRecognitionWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event == nullptr) {
        return;
    }
    if (event->button() == Qt::LeftButton &&
        (m_selectionResizeActive || selectionResizeDragModeAtLocalPoint(event->position()) !=
                                        ScreenshotSelectionDragMode::None)) {
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || m_showOriginalImage || m_ocrPresentation == nullptr ||
        m_stack->currentWidget() != m_textLayer) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    const ScreenshotOcrTextPosition position =
        m_textLayer->textPositionAt(canvasPositionForLocalPoint(event->position()), false);
    if (!position.valid() || m_ocrPresentation->lines.at(position.lineIndex).text.isEmpty()) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    setFocus(Qt::MouseFocusReason);
    m_ocrPresentation->beginTextSelection(ScreenshotOcrTextPosition{position.lineIndex, 0});
    m_ocrPresentation->updateTextSelection(ScreenshotOcrTextPosition{
        position.lineIndex,
        static_cast<int>(m_ocrPresentation->lines.at(position.lineIndex).text.size()),
    });
    m_ocrPresentation->finishTextSelection();
    m_textLayer->updateSelection();
    static_cast<void>(copyVisibleContentToClipboard());
    event->accept();
}

bool ScreenshotRecognitionWindow::isOcrBackgroundAt(const QPointF& localPosition) const {
    return !m_showOriginalImage && m_ocrPresentation != nullptr &&
           m_stack->currentWidget() == m_textLayer && !m_ocrPresentation->textSelectionActive() &&
           rect().contains(localPosition.toPoint()) &&
           !m_textLayer->textPositionAt(canvasPositionForLocalPoint(localPosition), false).valid();
}

void ScreenshotRecognitionWindow::mouseMoveEvent(QMouseEvent* event) {
    if (handleSelectionResizeEvent(this, event)) {
        return;
    }
    if (event != nullptr && !m_showOriginalImage && m_ocrPresentation != nullptr) {
        const QPointF canvasPosition = canvasPositionForLocalPoint(event->position());
        const ScreenshotOcrTextPosition exactPosition =
            m_textLayer->textPositionAt(canvasPosition, false);
        if (m_ocrPresentation->textSelectionActive()) {
            const quint64 previousRevision = m_ocrPresentation->selectionRevision();
            m_ocrPresentation->updateTextSelection(
                exactPosition.valid() ? exactPosition
                                      : m_textLayer->textPositionAt(canvasPosition, true));
            if (m_ocrPresentation->selectionRevision() != previousRevision) {
                m_textLayer->updateSelection();
            }
        }
        setCursor(exactPosition.valid() ? Qt::IBeamCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ScreenshotRecognitionWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (handleSelectionResizeEvent(this, event)) {
        return;
    }
    if (event != nullptr && event->button() == Qt::LeftButton && !m_showOriginalImage &&
        m_ocrPresentation != nullptr && m_ocrPresentation->textSelectionActive()) {
        const quint64 previousRevision = m_ocrPresentation->selectionRevision();
        m_ocrPresentation->updateTextSelection(
            m_textLayer->textPositionAt(canvasPositionForLocalPoint(event->position()), true));
        m_ocrPresentation->finishTextSelection();
        if (m_ocrPresentation->selectionRevision() != previousRevision) {
            m_textLayer->updateSelection();
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ScreenshotRecognitionWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (m_presentationMode == PresentationMode::EmbeddedChild) {
        return;
    }
    // Windows excludes fully zero-alpha layered pixels from mouse hit testing.
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), QColor(0, 0, 0, 2));
}

void ScreenshotRecognitionWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    synchronizeTextLayer();
    updateTextEditorSpinGeometry();
}

QPointF
ScreenshotRecognitionWindow::canvasPositionForLocalPoint(const QPointF& localPosition) const {
    bool invertible = false;
    const QTransform localToCanvas = canvasToLocalTransform().inverted(&invertible);
    return invertible ? localToCanvas.map(localPosition) : m_canvasSelection.topLeft();
}

QTransform ScreenshotRecognitionWindow::canvasToLocalTransform() const {
    const QRectF localRect(QPointF(0.0, 0.0), QSizeF(size()));
    if (!m_canvasSelection.isValid() || m_canvasSelection.isEmpty() || localRect.isEmpty()) {
        return {};
    }
    const QPolygonF canvasQuad({
        m_canvasSelection.topLeft(),
        m_canvasSelection.topRight(),
        m_canvasSelection.bottomRight(),
        m_canvasSelection.bottomLeft(),
    });
    const QPolygonF localQuad({
        localRect.topLeft(),
        localRect.topRight(),
        localRect.bottomRight(),
        localRect.bottomLeft(),
    });
    QTransform transform;
    return QTransform::quadToQuad(canvasQuad, localQuad, transform) ? transform : QTransform();
}

void ScreenshotRecognitionWindow::synchronizeTextLayer() {
    if (m_textLayer != nullptr) {
        m_textLayer->synchronize(canvasToLocalTransform(), rect());
    }
    if (m_formattedTextLayer != nullptr) {
        m_formattedTextLayer->synchronize(canvasToLocalTransform(), rect());
    }
}

void ScreenshotRecognitionWindow::updateTextEditorSpinGeometry() {
    if (m_textEditorContainer == nullptr || m_textEditorSpin == nullptr) {
        return;
    }
    const QSize spinSize = m_textEditorSpin->sizeHint().expandedTo(QSize(20, 20));
    constexpr int margin = 12;
    m_textEditorSpin->setGeometry(m_textEditorContainer->width() - spinSize.width() - margin,
                                  m_textEditorContainer->height() - spinSize.height() - margin,
                                  spinSize.width(), spinSize.height());
}

void ScreenshotRecognitionWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
#if SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (event->type() == QEvent::LanguageChange && m_qrBrowser) {
        m_qrBrowser->setAccessibleName(m_qrDetectLinks ? tr("Barcode recognition result")
                                                       : tr("LaTeX formula source"));
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (event->type() == QEvent::LanguageChange && m_latexPreviewEnabled && m_latexDocument &&
        m_textEditor)
        m_textEditor->setAccessibleName(tr("LaTeX formula source"));
#endif
    if (event->type() == QEvent::LanguageChange && companionPreviewEnabled())
        refreshOriginalImagePreview();
}
