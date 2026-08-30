#include "snow_shot/presentation/screenshotrecognitionwindow.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtextlayer.h"
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "theme/theme_manager.h"
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
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
QMargins adTextAreaContentMargins(const adqt::theme::ThemeMapToken& theme) {
    const int borderInset = std::max(1, qRound(theme.lineWidth));
    const int horizontalPadding = std::max(8, qRound(theme.sizeSM - theme.lineWidth));
    const double baseVerticalPadding =
        (theme.controlHeight - theme.fontSize * theme.lineHeight) / 2.0;
    const double roundedVerticalPadding = std::round(baseVerticalPadding * 10.0) / 10.0;
    const int verticalPadding =
        std::max(0, qRound(roundedVerticalPadding - theme.lineWidth));
    return QMargins(borderInset + horizontalPadding, borderInset + verticalPadding,
                    borderInset + horizontalPadding, borderInset + verticalPadding);
}

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

bool isHttpUrl(const QString& text, QUrl* result = nullptr) {
    const QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    const bool valid = url.isValid() && !url.isRelative() && !url.host().isEmpty() &&
                       (scheme == QStringLiteral("http") ||
                        scheme == QStringLiteral("https"));
    if (valid && result != nullptr) {
        *result = url;
    }
    return valid;
}

QList<QKeyCombination> commandCombinations(Qt::Key key) {
    QList<QKeyCombination> combinations;
    constexpr Qt::KeyboardModifier optionalModifiers[] = {
        Qt::ShiftModifier,
        Qt::AltModifier,
        Qt::KeypadModifier,
    };
    for (int optionalMask = 0; optionalMask < 8; ++optionalMask) {
        Qt::KeyboardModifiers optional;
        for (int index = 0; index < 3; ++index) {
            if ((optionalMask & (1 << index)) != 0) {
                optional |= optionalModifiers[index];
            }
        }
        combinations.push_back(QKeyCombination(optional | Qt::ControlModifier, key));
        combinations.push_back(QKeyCombination(optional | Qt::MetaModifier, key));
        combinations.push_back(
            QKeyCombination(optional | Qt::ControlModifier | Qt::MetaModifier, key));
    }
    return combinations;
}

bool focusInside(const QWidget* container, const QWidget* focus) {
    return container != nullptr && focus != nullptr &&
           (container == focus || container->isAncestorOf(focus));
}
}  // namespace

class ScreenshotFormattedTextLayer final : public QGraphicsView {
  public:
    explicit ScreenshotFormattedTextLayer(QWidget* parent = nullptr)
        : QGraphicsView(parent), m_scene(new QGraphicsScene(this)),
          m_textItem(new QGraphicsTextItem) {
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
        setStyleSheet(
            QStringLiteral("QGraphicsView#screenshotClipboardText { border: none; }"));

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
    ScreenshotRecognitionWindowActions actions, QWidget* parent,
    PresentationMode presentationMode,
    snow_shot::presentation::WindowShortcutManager* shortcutManager)
    : QWidget(parent,
               presentationMode == PresentationMode::EmbeddedChild
                   ? Qt::Widget
                   : Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      m_actions(std::move(actions)),
      m_stack(new QStackedLayout(this)),
      m_textLayer(new ScreenshotOcrTextLayer(this)),
      m_presentationMode(presentationMode) {
    if (shortcutManager == nullptr) {
        m_ownedShortcutManager =
            std::make_unique<snow_shot::presentation::WindowShortcutManager>();
        shortcutManager = m_ownedShortcutManager.get();
    }
    m_shortcutManager = shortcutManager;
    m_shortcutManager->addScopeWindow(this);
    setObjectName(QStringLiteral("screenshotRecognitionWindow"));
    if (m_presentationMode == PresentationMode::TopLevelWindow) {
        setAttribute(Qt::WA_TranslucentBackground, true);
    }
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

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
}

ScreenshotRecognitionWindow::~ScreenshotRecognitionWindow() {
    clearFormattedText();
    if (m_textEditor != nullptr) {
        const QSignalBlocker blocker(m_textEditor);
        m_textEditor->setDocument(nullptr);
    }
    if (m_tableEditor != nullptr) {
        m_tableEditor->clearSession();
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
        !config.canvasSelection.isValid() ||
        config.canvasSelection.isEmpty() ||
        !std::isfinite(config.formattedTextDevicePixelRatio) ||
        config.formattedTextDevicePixelRatio <= 0.0) {
        return false;
    }

    m_canvasSelection = config.canvasSelection.normalized();
    m_formattedTextDevicePixelRatio = config.formattedTextDevicePixelRatio;
    m_presentationMode = config.presentationMode;
    if (m_presentationMode == PresentationMode::TopLevelWindow) {
        static_cast<void>(winId());
        if (QWindow* handle = windowHandle()) {
            handle->setScreen(config.screen);
            if (config.transientOwner != nullptr) {
                static_cast<void>(config.transientOwner->winId());
                handle->setTransientParent(config.transientOwner->windowHandle());
            }
        }
    }
    setGeometry(config.geometry);
    show();
    if (m_presentationMode == PresentationMode::TopLevelWindow) {
        raise();
        activateWindow();
    }
    setFocus(Qt::OtherFocusReason);
    synchronizeTextLayer();
    installSelectionResizeEventFilters(this);
    return true;
}

bool ScreenshotRecognitionWindow::updateSelectionGeometry(const QRect& geometry,
                                                           const QRectF& canvasSelection) {
    if (!geometry.isValid() || geometry.isEmpty() || !canvasSelection.isValid() ||
        canvasSelection.isEmpty()) {
        return false;
    }
    m_canvasSelection = canvasSelection.normalized();
    setGeometry(geometry);
    synchronizeTextLayer();
    update();
    return true;
}

ScreenshotRecognitionWindow::PresentationMode ScreenshotRecognitionWindow::presentationMode()
    const {
    return m_presentationMode;
}

void ScreenshotRecognitionWindow::setOcrPresentation(
    std::shared_ptr<ScreenshotOcrPresentation> presentation) {
    hideTextEditor();
    clearFormattedText();
    clearTableSession();
    clearQrContents();
    m_ocrPresentation = std::move(presentation);
    m_textLayer->setPresentation(m_ocrPresentation);
    m_stack->setCurrentWidget(m_textLayer);
    synchronizeTextLayer();
    setFocus(Qt::OtherFocusReason);
}

void ScreenshotRecognitionWindow::clearOcrPresentation() {
    m_ocrPresentation.reset();
    m_textLayer->clearPresentation();
    unsetCursor();
}

void ScreenshotRecognitionWindow::showFormattedText(std::shared_ptr<QTextDocument> document) {
    if (document == nullptr) {
        return;
    }
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
    m_formattedTextLayer->focusText();
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
        connect(m_tableEditor, &ScreenshotTableEditor::operationRejected, this,
                [this](const QString& message) {
                    m_actions.handleTableOperationRejected(message);
                });
    }
    m_tableEditor->setSession(std::move(session));
    m_stack->setCurrentWidget(m_tableEditor);
    m_tableEditor->setFocus(Qt::OtherFocusReason);
}

void ScreenshotRecognitionWindow::clearTableSession() {
    if (m_tableEditor == nullptr) {
        return;
    }
    m_tableEditor->clearSession();
    m_stack->removeWidget(m_tableEditor);
    delete m_tableEditor;
    m_tableEditor = nullptr;
    m_stack->setCurrentWidget(m_textLayer);
}

ScreenshotTableCommandState ScreenshotRecognitionWindow::tableCommandState() const {
    return m_tableEditor != nullptr ? m_tableEditor->commandState()
                                    : ScreenshotTableCommandState{};
}

void ScreenshotRecognitionWindow::mergeTableSelection() {
    if (m_tableEditor != nullptr) {
        m_tableEditor->mergeSelection();
    }
}

void ScreenshotRecognitionWindow::splitTableSelection() {
    if (m_tableEditor != nullptr) {
        m_tableEditor->splitSelection();
    }
}

void ScreenshotRecognitionWindow::resetTable() {
    if (m_tableEditor != nullptr) {
        m_tableEditor->resetDocument();
    }
}

void ScreenshotRecognitionWindow::undoTableEdit() {
    if (m_tableEditor != nullptr) {
        m_tableEditor->undoEdit();
    }
}

void ScreenshotRecognitionWindow::redoTableEdit() {
    if (m_tableEditor != nullptr) {
        m_tableEditor->redoEdit();
    }
}

void ScreenshotRecognitionWindow::commitActiveTableEdit() {
    if (m_tableEditor != nullptr) {
        static_cast<void>(m_tableEditor->commitActiveEdit());
    }
}

void ScreenshotRecognitionWindow::showTextEditor(QTextDocument* document, bool readOnly,
                                                 bool streaming) {
    if (document == nullptr) {
        return;
    }
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
        connect(editor, &adqt::widgets::AdTextEdit::textEdited, this,
                [this](const QString& text) { m_actions.handleTextEdited(text); });
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
    if (!readOnly) {
        static_cast<adqt::widgets::AdTextEdit*>(m_textEditor)->focusEditor();
    }
}

void ScreenshotRecognitionWindow::registerWindowShortcuts() {
    using ShortcutManager = snow_shot::presentation::WindowShortcutManager;

    ShortcutManager::Binding cancel;
    cancel.id = QStringLiteral("recognition.cancel");
    cancel.keyCombinations = {QKeyCombination(Qt::NoModifier, Qt::Key_Escape)};
    cancel.priority = ShortcutManager::StandardPriority::WindowCommand;
    cancel.canActivate = [this](const auto&) { return isVisible(); };
    cancel.activate = [this](const auto&) {
        if (m_tableEditor == nullptr || !m_tableEditor->cancelActiveEdit()) {
            m_actions.handleCancel();
        }
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(cancel)));

    const auto ocrCommandsAllowed = [this](const ShortcutManager::ActivationContext& context) {
        QWidget* focus = context.focusWidget;
        return isVisible() && m_ocrPresentation != nullptr &&
               !focusInside(m_textEditorContainer, focus) &&
               !focusInside(m_tableEditor, focus) && !focusInside(m_qrBrowser, focus) &&
               !focusInside(m_formattedTextLayer, focus);
    };

    ShortcutManager::Binding selectAll;
    selectAll.id = QStringLiteral("recognition.select_all");
    selectAll.keyCombinations = commandCombinations(Qt::Key_A);
    selectAll.priority = ShortcutManager::StandardPriority::WindowCommand;
    selectAll.canActivate = ocrCommandsAllowed;
    selectAll.activate = [this](const auto&) {
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
    copy.keyCombinations = commandCombinations(Qt::Key_C);
    copy.priority = ShortcutManager::StandardPriority::WindowCommand;
    copy.canActivate = ocrCommandsAllowed;
    copy.activate = [this](const auto&) {
        QString text;
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
        if (!text.isEmpty() && QApplication::clipboard() != nullptr) {
            QApplication::clipboard()->setText(text);
        }
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(copy)));

    const auto textEditorActive = [this](const ShortcutManager::ActivationContext& context) {
        return focusInside(m_textEditor, context.focusWidget);
    };
    ShortcutManager::Binding undo;
    undo.id = QStringLiteral("recognition.text.undo");
    undo.keyCombinations = {
        QKeyCombination(Qt::ControlModifier, Qt::Key_Z),
        QKeyCombination(Qt::MetaModifier, Qt::Key_Z),
    };
    undo.priority = ShortcutManager::StandardPriority::WindowCommand;
    undo.canActivate = textEditorActive;
    undo.activate = [this](const auto&) {
        m_actions.handleUndoTextEdit();
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(undo)));

    ShortcutManager::Binding redo;
    redo.id = QStringLiteral("recognition.text.redo");
    redo.keyCombinations = {
        QKeyCombination(Qt::ControlModifier, Qt::Key_Y),
        QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Z),
        QKeyCombination(Qt::MetaModifier | Qt::ShiftModifier, Qt::Key_Z),
    };
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

void ScreenshotRecognitionWindow::showQrContents(const QStringList& contents) {
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
        m_stack->addWidget(m_qrBrowser);
        installSelectionResizeEventFilters(m_qrBrowser);
        connect(m_qrBrowser, &QTextBrowser::anchorClicked, this,
                [this](const QUrl& url) { m_actions.handleLinkActivated(url); });
    }

    const adqt::theme::ThemeMapToken theme =
        adqt::theme::ThemeManager::instance().resolveTheme(m_qrBrowser);
    const QMargins contentMargins = adTextAreaContentMargins(theme);
    m_qrBrowser->setStyleSheet(
        QStringLiteral("QTextBrowser#screenshotQrContents { border: none; border-radius: 0; "
                       "padding: %1px %2px; background-color: palette(base); }")
            .arg(contentMargins.top())
            .arg(contentMargins.left()));

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
        if (!trimmed.isEmpty() && isHttpUrl(trimmed, &url)) {
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
    m_stack->setCurrentWidget(m_qrBrowser);
    m_qrBrowser->setFocus(Qt::OtherFocusReason);
}

void ScreenshotRecognitionWindow::clearQrContents() {
    if (m_qrBrowser == nullptr) {
        return;
    }
    m_stack->removeWidget(m_qrBrowser);
    delete m_qrBrowser;
    m_qrBrowser = nullptr;
    m_stack->setCurrentWidget(m_textLayer);
}

void ScreenshotRecognitionWindow::focusOutEvent(QFocusEvent* event) {
    if (m_ocrPresentation != nullptr) {
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

Qt::CursorShape ScreenshotRecognitionWindow::cursorForSelectionResize(
    ScreenshotSelectionDragMode dragMode) {
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

void ScreenshotRecognitionWindow::updateSelectionResizeCursor(
    const QPointF& localPosition) {
    const ScreenshotSelectionDragMode dragMode =
        selectionResizeDragModeAtLocalPoint(localPosition);
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
        if (dragMode == ScreenshotSelectionDragMode::None ||
            !m_actions.beginSelectionResize(canvasPosition)) {
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
        m_actions.finishSelectionResize(canvasPosition);
        unsetCursor();
        m_actions.selectionResizeFinished();
        mouseEvent->accept();
        return true;
    }
    return false;
}

bool ScreenshotRecognitionWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event != nullptr && event->type() == QEvent::ChildAdded) {
        const auto* childEvent = static_cast<const QChildEvent*>(event);
        if (auto* childWidget = qobject_cast<QWidget*>(childEvent->child());
            childWidget != nullptr) {
            installSelectionResizeEventFilters(childWidget);
        }
    }
    if (event != nullptr && event->type() == QEvent::ContextMenu &&
        m_presentationMode == PresentationMode::EmbeddedChild) {
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
    if (event != nullptr && m_presentationMode == PresentationMode::EmbeddedChild) {
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
    if (event != nullptr && event->button() == Qt::LeftButton && m_ocrPresentation != nullptr) {
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

void ScreenshotRecognitionWindow::mouseMoveEvent(QMouseEvent* event) {
    if (handleSelectionResizeEvent(this, event)) {
        return;
    }
    if (event != nullptr && m_ocrPresentation != nullptr) {
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
    if (event != nullptr && event->button() == Qt::LeftButton && m_ocrPresentation != nullptr) {
        const quint64 previousRevision = m_ocrPresentation->selectionRevision();
        m_ocrPresentation->updateTextSelection(m_textLayer->textPositionAt(
            canvasPositionForLocalPoint(event->position()), true));
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

QPointF ScreenshotRecognitionWindow::canvasPositionForLocalPoint(
    const QPointF& localPosition) const {
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
