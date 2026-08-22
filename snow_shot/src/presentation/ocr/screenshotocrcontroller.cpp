#include "snow_shot/presentation/screenshotocrcontroller.h"

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtexttransform.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotsourceimagecomposer.h"
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QMimeData>
#include <QScreen>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace {
constexpr auto kRecognitionMessageKey = "screenshot-ocr-recognition";
constexpr auto kStatusMessageKey = "screenshot-ocr-status";

ScreenshotToolPalette::Tool paletteTool(ScreenshotActiveTool tool) {
    switch (tool) {
    case ScreenshotActiveTool::Select:
        return ScreenshotToolPalette::Tool::Select;
    case ScreenshotActiveTool::Shape:
        return ScreenshotToolPalette::Tool::Shape;
    case ScreenshotActiveTool::Arrow:
        return ScreenshotToolPalette::Tool::Arrow;
    case ScreenshotActiveTool::Line:
        return ScreenshotToolPalette::Tool::Line;
    case ScreenshotActiveTool::FreeDraw:
        return ScreenshotToolPalette::Tool::FreeDraw;
    case ScreenshotActiveTool::RectangleHighlight:
        return ScreenshotToolPalette::Tool::RectangleHighlight;
    case ScreenshotActiveTool::PenHighlight:
        return ScreenshotToolPalette::Tool::PenHighlight;
    case ScreenshotActiveTool::Eraser:
        return ScreenshotToolPalette::Tool::Eraser;
    case ScreenshotActiveTool::RectangleFilter:
        return ScreenshotToolPalette::Tool::RectangleFilter;
    case ScreenshotActiveTool::PenFilter:
        return ScreenshotToolPalette::Tool::PenFilter;
    case ScreenshotActiveTool::Text:
        return ScreenshotToolPalette::Tool::Text;
    case ScreenshotActiveTool::SerialNumber:
        return ScreenshotToolPalette::Tool::SerialNumber;
    case ScreenshotActiveTool::Ocr:
        return ScreenshotToolPalette::Tool::Ocr;
    case ScreenshotActiveTool::Table:
        return ScreenshotToolPalette::Tool::Table;
    case ScreenshotActiveTool::Qr:
        return ScreenshotToolPalette::Tool::Qr;
    case ScreenshotActiveTool::Move:
    default:
        return ScreenshotToolPalette::Tool::Move;
    }
}

QRect recognitionGeometryForDisplay(const ScreenshotGeometryMapper& geometry,
                                    const CapturedDisplayModel& display,
                                    const QRectF& canvasSelection) {
    const QRectF canvasRect = ScreenshotGeometryMapper::displayCanvasRect(display);
    if (!canvasRect.isValid() || canvasRect.isEmpty() || !display.logicalRect.isValid() ||
        display.logicalRect.isEmpty() || !canvasSelection.isValid() ||
        canvasSelection.isEmpty()) {
        return {};
    }
    return QRectF(geometry.logicalPositionForCanvasPoint(display, canvasSelection.topLeft()),
                  geometry.logicalPositionForCanvasPoint(display, canvasSelection.bottomRight()))
        .normalized()
        .toAlignedRect();
}
} // namespace

ScreenshotOcrController::ScreenshotOcrController(ScreenshotOcrControllerContext context,
                                                 QObject* parent)
    : QObject(parent),
      m_context(std::move(context)),
      m_messages(std::make_unique<ScreenshotMessageService>(
          m_context.displaySession, m_context.geometry, m_context.selection,
          [this]() { return m_context.overlayCoordinator.toolbar(); })) {
    m_session = std::make_unique<ScreenshotRecognitionSessionController>(
        &m_context.recognition, &m_context.qrRecognition, m_context.tableRecognition,
        ScreenshotRecognitionSessionActions{
            [this]() -> ScreenshotRecognitionWindow* {
                return ensureRecognitionWindow() ? m_recognitionWindow.data() : nullptr;
            },
            [this]() { destroyRecognitionWindow(); },
            [this](std::shared_ptr<ScreenshotOcrPresentation> presentation) {
                if (m_recognitionWindow != nullptr) {
                    m_recognitionWindow->setOcrPresentation(presentation);
                }
            },
            [this](std::shared_ptr<ScreenshotOcrPresentation> presentation) {
                m_presentation = std::move(presentation);
                applyOcrBackgroundToOverlays(m_presentation);
            },
            [](std::shared_ptr<QTextDocument>) {},
            [this]() { clearOcrBackgroundFromOverlays(); },
            [](bool) {},
            [this](int mode) {
                if (mode < 0) {
                    return;
                }
                if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
                    const auto tool =
                        mode == static_cast<int>(
                                    ScreenshotRecognitionSessionController::Mode::Text)
                            ? ScreenshotActiveTool::Ocr
                        : mode == static_cast<int>(
                                     ScreenshotRecognitionSessionController::Mode::Table)
                            ? ScreenshotActiveTool::Table
                            : ScreenshotActiveTool::Qr;
                    toolbar->setActiveTool(paletteTool(tool));
                }
            },
            [this](bool available, bool editing, bool canUndo, bool canRedo) {
                if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
                    toolbar->setTextEditingState(available, editing, canUndo, canRedo);
                }
            },
            [this](bool available, bool translating, bool streaming, bool canUndo, bool canRedo,
                   bool canReset) {
                if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
                    toolbar->setTextTranslationState(available, translating, streaming, canUndo,
                                                     canRedo, canReset);
                }
            },
            [this](bool available, bool canUndo, bool canRedo, bool canMerge, bool canSplit,
                   bool canReset) {
                if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
                    toolbar->setTableEditingState(available, canUndo, canRedo, canMerge, canSplit,
                                                  canReset);
                }
            },
            [this](bool textBusy, bool tableBusy, bool qrBusy) {
                if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
                    toolbar->setOcrBusy(textBusy);
                    toolbar->setTableBusy(tableBusy);
                    toolbar->setQrBusy(qrBusy);
                }
            },
            [this](const QString& message) {
                m_messages->loading(QString::fromLatin1(kRecognitionMessageKey), message, {},
                                    m_recognitionWindow.data());
            },
            [this]() {
                m_messages->destroy(QString::fromLatin1(kRecognitionMessageKey));
            },
            [this](const QString& message, bool error) { showStatus(message, error); },
            [this](const QUrl& url) { handleQrLinkActivated(url); },
            [this]() -> QWidget* {
                const QRectF selection = m_context.selection.normalizedSelection();
                const CapturedDisplayModel* display = m_context.geometry.displayForCanvasPoint(
                    m_context.displaySession, selection.center());
                if (display == nullptr) {
                    display = m_context.geometry.displayForCanvasRect(m_context.displaySession,
                                                                     selection);
                }
                return m_context.displaySession.overlayForDisplay(display);
            },
            [this](const QString& formatting, const QString& punctuation) {
                if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
                    toolbar->setTextTransformSelections(formatting, punctuation);
                }
            },
        },
        this);
    connect(m_session.get(), &ScreenshotRecognitionSessionController::textEditingChanged, this,
            &ScreenshotOcrController::textEditingChanged);
    connect(m_session.get(), &ScreenshotRecognitionSessionController::textResultChanged, this,
            &ScreenshotOcrController::textResultChanged);
    connect(m_session.get(), &ScreenshotRecognitionSessionController::textDraftChanged, this,
            &ScreenshotOcrController::textDraftChanged);
}

ScreenshotOcrController::~ScreenshotOcrController() {
    invalidateSession();
}

bool ScreenshotOcrController::active() const {
    return m_active;
}

bool ScreenshotOcrController::busy() const {
    return m_session->busy();
}

bool ScreenshotOcrController::busy(Mode mode) const {
    return m_session->busy(static_cast<ScreenshotRecognitionSessionController::Mode>(mode));
}

ScreenshotOcrController::Mode ScreenshotOcrController::mode() const {
    return m_mode;
}

bool ScreenshotOcrController::tableModeActive() const {
    return m_session->tableModeActive();
}

bool ScreenshotOcrController::qrModeActive() const {
    return m_session->qrModeActive();
}

void ScreenshotOcrController::activate() {
    activateMode(Mode::Text);
}

void ScreenshotOcrController::activateTable() {
    activateMode(Mode::Table);
}

void ScreenshotOcrController::activateQr() {
    activateMode(Mode::Qr);
}

void ScreenshotOcrController::setMode(Mode mode) {
    activateMode(mode);
}

QString ScreenshotOcrController::currentCacheKey() const {
    const QRect selection = m_context.selection.pixelSelection();
    return QStringLiteral("%1:%2,%3,%4,%5")
        .arg(m_context.captureState.sessionId)
        .arg(selection.x())
        .arg(selection.y())
        .arg(selection.width())
        .arg(selection.height());
}

void ScreenshotOcrController::activateMode(Mode mode) {
    const QRect selection = m_context.selection.pixelSelection();
    if (selection.width() < 1 || selection.height() < 1) {
        if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
            toolbar->setActiveTool(paletteTool(m_context.interaction.activeTool()));
        }
        showStatus(tr("Select an area to recognize"), false);
        return;
    }

    if (!m_active) {
        m_previousTool = m_context.interaction.activeTool();
        m_canvasStates.clear();
        m_context.displaySession.forEachOverlay(
            [this](qsizetype, ScreenshotOverlayWindow* overlay) {
                if (overlay == nullptr || overlay->canvas() == nullptr) {
                    return;
                }
                SnowCanvasWidget* canvas = overlay->canvas();
                m_canvasStates.push_back(CanvasState{
                    overlay,
                    canvas,
                    canvas->canvasContentVisible(),
                    canvas->interactionEnabled(),
                    overlay->hasScreenshotSelection(),
                    overlay->screenshotSelectionHandlesVisible(),
                    overlay->screenshotSelectionBorderVisible(),
                });
                canvas->setInteractionEnabled(false);
                canvas->setCanvasContentVisible(false);
                overlay->setScreenshotSelection(m_context.selection.normalizedSelection(), false,
                                                m_context.selection.cornerRadius());
                overlay->setScreenshotSelectionBorderVisible(false);
            });
        m_active = true;
        m_context.captureState.sessionState = ScreenshotSessionState::Editing;
        m_context.hideColorPicker();
    }

    m_mode = mode;
    clearOcrBackgroundFromOverlays();
    if (!ensureRecognitionWindow()) {
        restorePreviousToolAfterFailure();
        return;
    }
    m_recognitionWindow->clearOcrPresentation();
    m_recognitionWindow->clearTableSession();
    m_recognitionWindow->clearQrContents();
    if (mode == Mode::Text) {
        m_context.interaction.setOcrTool();
    } else if (mode == Mode::Table) {
        m_context.interaction.setTableTool();
    } else {
        m_context.interaction.setQrTool();
    }
    if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
        const ScreenshotActiveTool activeTool =
            mode == Mode::Text    ? ScreenshotActiveTool::Ocr
            : mode == Mode::Table ? ScreenshotActiveTool::Table
                                  : ScreenshotActiveTool::Qr;
        toolbar->setActiveTool(paletteTool(activeTool));
    }

    const QString key = currentCacheKey();
    QImage source = m_surfaceKey == key
                        ? m_surfaceImage
                        : composeScreenshotSourceSelection(m_context.displaySession, selection);
    if (source.isNull()) {
        showStatus(tr("Unable to read the selected screenshot"), true);
        restorePreviousToolAfterFailure();
        return;
    }
    m_session->setTarget(ScreenshotRecognitionTarget{key, std::move(source), QRectF(selection)});
    m_session->activate(static_cast<ScreenshotRecognitionSessionController::Mode>(mode));
}

bool ScreenshotOcrController::copyRecognitionToClipboard(bool endCapture) {
    if (!m_active || QApplication::clipboard() == nullptr) {
        return false;
    }
    if (m_session->qrModeActive()) {
        const QStringList contents = m_session->qrContents();
        if (contents.isEmpty()) {
            return false;
        }
        QApplication::clipboard()->setText(contents.join(QLatin1Char('\n')));
        if (endCapture) {
            m_context.cancelCapture();
        }
        return true;
    }
    if (m_session->tableModeActive()) {
        if (m_recognitionWindow != nullptr) {
            m_recognitionWindow->commitActiveTableEdit();
        }
        const auto session = m_session->tableSession();
        if (session == nullptr || session->document.empty()) {
            return false;
        }
        auto* mimeData = new QMimeData;
        mimeData->setData(QStringLiteral("text/html"), session->document.toHtml().toUtf8());
        mimeData->setText(session->document.toPlainText());
        QApplication::clipboard()->setMimeData(mimeData, QClipboard::Clipboard);
        if (endCapture) {
            m_context.cancelCapture();
        }
        return true;
    }

    QString text;
    if (m_session->editing()) {
        text = m_session->textDraft();
    } else if (m_presentation != nullptr) {
        text = m_presentation->hasTextSelection()
                   ? m_presentation->selectedText()
                   : snow_shot::presentation::originalOcrText(*m_presentation);
    }
    if (text.isEmpty()) {
        return false;
    }
    QApplication::clipboard()->setText(text);
    if (endCapture) {
        m_context.cancelCapture();
    }
    return true;
}

void ScreenshotOcrController::mergeTableSelection() {
    m_session->mergeTableSelection();
}

void ScreenshotOcrController::splitTableSelection() {
    m_session->splitTableSelection();
}

void ScreenshotOcrController::resetTable() {
    m_session->resetTable();
}

void ScreenshotOcrController::undoTableEdit() {
    m_session->undoTableEdit();
}

void ScreenshotOcrController::redoTableEdit() {
    m_session->redoTableEdit();
}

ScreenshotTableCommandState ScreenshotOcrController::tableCommandState() const {
    return tableModeActive() && m_recognitionWindow != nullptr
               ? m_recognitionWindow->tableCommandState()
               : ScreenshotTableCommandState{};
}

void ScreenshotOcrController::undoTextEdit() {
    m_session->undoTextEdit();
}

void ScreenshotOcrController::redoTextEdit() {
    m_session->redoTextEdit();
}

void ScreenshotOcrController::beginTextEditing() {
    m_session->beginTextEditing();
}

void ScreenshotOcrController::beginTextTranslation() {
    m_session->beginTextTranslation();
}

void ScreenshotOcrController::endTextEditing() {
    m_session->endTextEditing();
}

void ScreenshotOcrController::openTranslationSettings() {
    m_session->openTranslationSettings();
}

void ScreenshotOcrController::resetTextEditing() {
    m_session->resetTextEditing();
}

void ScreenshotOcrController::applyTextFormatting(const QString& value) {
    m_session->applyTextFormatting(value);
}

void ScreenshotOcrController::applyTextPunctuation(const QString& value) {
    m_session->applyTextPunctuation(value);
}

bool ScreenshotOcrController::editing() const {
    return m_session->editing();
}

bool ScreenshotOcrController::translating() const {
    return m_session->translating();
}

bool ScreenshotOcrController::hasTextResult() const {
    return m_session->hasTextResult();
}

QString ScreenshotOcrController::textDraft() const {
    return m_session->textDraft();
}

QString ScreenshotOcrController::originalText() const {
    return m_session->originalText();
}

void ScreenshotOcrController::setTextDraft(const QString& text) {
    m_session->setTextDraft(text);
}

void ScreenshotOcrController::handleQrLinkActivated(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    if (!qrModeActive() || !url.isValid() || url.isRelative() || url.host().isEmpty() ||
        (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))) {
        return;
    }
    if (!QDesktopServices::openUrl(url)) {
        showStatus(tr("Unable to open the recognized link"), true);
        return;
    }
    QTimer::singleShot(0, this, [this]() {
        if (qrModeActive()) {
            m_context.cancelCapture();
        }
    });
}

void ScreenshotOcrController::deactivate() {
    deactivateImpl(false);
}

void ScreenshotOcrController::deactivateForSelectionResize() {
    deactivateImpl(true);
}

void ScreenshotOcrController::deactivateImpl(bool preserveRecognitionWindow) {
    m_session->deactivate();
    if (!m_active && m_canvasStates.isEmpty() && m_recognitionWindow == nullptr) {
        return;
    }
    m_active = false;
    clearOcrBackgroundFromOverlays();
    if (!preserveRecognitionWindow) {
        destroyRecognitionWindow();
    }
    m_context.displaySession.forEachOverlay([](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != nullptr && overlay->canvas() != nullptr) {
            overlay->canvas()->unsetCursor();
        }
    });
    for (const CanvasState& state : std::as_const(m_canvasStates)) {
        if (state.overlay != nullptr) {
            state.overlay->setScreenshotSelectionBorderVisible(state.selectionBorderVisible);
            if (state.hadSelection) {
                state.overlay->setScreenshotSelection(m_context.selection.normalizedSelection(),
                                                      state.selectionHandlesVisible,
                                                      m_context.selection.cornerRadius());
            } else {
                state.overlay->clearScreenshotSelection();
            }
        }
        if (state.canvas != nullptr) {
            state.canvas->setCanvasContentVisible(state.contentVisible);
            state.canvas->setInteractionEnabled(state.interactionEnabled);
        }
    }
    m_canvasStates.clear();
}

void ScreenshotOcrController::invalidateSession() {
    m_session->invalidate();
    deactivate();
    m_presentation.reset();
    m_surfaceKey.clear();
    m_surfaceImage = QImage();
}

void ScreenshotOcrController::restorePreviousToolAfterFailure() {
    const ScreenshotActiveTool previousTool = m_previousTool;
    deactivate();
    if (previousTool == ScreenshotActiveTool::Move) {
        m_context.interaction.setMoveTool(m_context.selection.hasPixelSelection(), false);
    } else {
        m_context.interaction.setCanvasTool(previousTool);
    }
    if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
        toolbar->setActiveTool(paletteTool(previousTool));
    }
    updateOverlays();
}

void ScreenshotOcrController::updateOverlays() const {
    m_context.displaySession.forEachOverlay([](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != nullptr && overlay->canvas() != nullptr) {
            overlay->canvas()->update();
        }
    });
}

void ScreenshotOcrController::applyOcrBackgroundToOverlays(
    const std::shared_ptr<ScreenshotOcrPresentation>& presentation) const {
    m_context.displaySession.forEachOverlay(
        [&presentation](qsizetype, ScreenshotOverlayWindow* overlay) {
            if (overlay != nullptr) {
                overlay->setScreenshotOcrBackground(presentation);
            }
        });
}

void ScreenshotOcrController::clearOcrBackgroundFromOverlays() const {
    m_context.displaySession.forEachOverlay([](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay != nullptr) {
            overlay->clearScreenshotOcrBackground();
        }
    });
}

bool ScreenshotOcrController::ensureRecognitionWindow() {
    const QRect selection = m_context.selection.pixelSelection();
    const QString key = currentCacheKey();
    const QPointF center = QRectF(selection).center();
    const CapturedDisplayModel* display =
        m_context.geometry.displayForCanvasPoint(m_context.displaySession, center);
    if (display == nullptr) {
        display = m_context.geometry.displayForCanvasRect(m_context.displaySession,
                                                         QRectF(selection));
    }
    if (display == nullptr) {
        showStatus(tr("Unable to read the selected screenshot"), true);
        return false;
    }

    ScreenshotOverlayWindow* overlay = m_context.displaySession.overlayForDisplay(display);
    if (overlay == nullptr) {
        showStatus(tr("Unable to read the selected screenshot"), true);
        return false;
    }
    QScreen* screen =
        ScreenshotGeometryMapper::screenForCaptureDisplay(display->name, display->physicalRect);
    if (screen == nullptr) {
        screen = overlay->screen();
    }
    if (screen == nullptr) {
        showStatus(tr("Unable to read the selected screenshot"), true);
        return false;
    }

    const ScreenshotRecognitionWindow::Config config{
        screen,
        overlay,
        recognitionGeometryForDisplay(m_context.geometry, *display, QRectF(selection)),
        QRectF(selection),
    };
    if (m_recognitionWindow != nullptr && m_surfaceKey == key) {
        if (!m_recognitionWindow->present(config)) {
            showStatus(tr("Unable to read the selected screenshot"), true);
            return false;
        }
        if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
            toolbar->raise();
        }
        return true;
    }

    destroyRecognitionWindow();
    QImage source = composeScreenshotSourceSelection(m_context.displaySession, selection);
    if (source.isNull()) {
        showStatus(tr("Unable to read the selected screenshot"), true);
        return false;
    }
    auto* window = new ScreenshotRecognitionWindow(ScreenshotRecognitionWindowActions{
        [this]() { m_context.cancelCapture(); },
        [this](const QString& text) { setTextDraft(text); },
        [this](const ScreenshotTableCommandState& state) {
            m_session->handleTableCommandState(state);
        },
        [this](const QString& message) { showStatus(message, false); },
        [this](const QUrl& url) { handleQrLinkActivated(url); },
        [this]() { undoTextEdit(); },
        [this]() { redoTextEdit(); },
        [this](const QPointF& canvasPosition) {
            return m_context.selectionResizeDragMode(canvasPosition);
        },
        [this](const QPointF& canvasPosition) {
            return m_context.beginSelectionResize(canvasPosition);
        },
        [this](const QPointF& canvasPosition) {
            m_context.updateSelectionResize(canvasPosition);
            updateRecognitionWindowGeometry();
        },
        [this](const QPointF& canvasPosition) {
            m_context.finishSelectionResize(canvasPosition);
            updateRecognitionWindowGeometry();
        },
        []() {},
    });
    if (!window->present(config)) {
        delete window;
        showStatus(tr("Unable to read the selected screenshot"), true);
        return false;
    }

    m_surfaceKey = key;
    m_surfaceImage = std::move(source);
    m_recognitionWindow = window;
    if (ScreenshotToolbarWindow* toolbar = m_context.overlayCoordinator.toolbar()) {
        toolbar->raise();
    }
    return true;
}

void ScreenshotOcrController::updateRecognitionWindowGeometry() {
    if (m_recognitionWindow == nullptr) {
        return;
    }
    const QRectF selection = m_context.selection.normalizedSelection();
    const CapturedDisplayModel* display =
        m_context.geometry.displayForCanvasPoint(m_context.displaySession, selection.center());
    if (display == nullptr) {
        display = m_context.geometry.displayForCanvasRect(m_context.displaySession, selection);
    }
    if (display == nullptr) {
        return;
    }
    static_cast<void>(m_recognitionWindow->updateSelectionGeometry(
        recognitionGeometryForDisplay(m_context.geometry, *display, selection), selection));
}

void ScreenshotOcrController::destroyRecognitionWindow() {
    if (m_recognitionWindow != nullptr) {
        delete m_recognitionWindow.data();
        m_recognitionWindow = nullptr;
    }
    m_surfaceKey.clear();
    m_surfaceImage = QImage();
}

void ScreenshotOcrController::showStatus(const QString& message, bool error) const {
    if (message.isEmpty()) {
        return;
    }
    if (error) {
        m_messages->error(QString::fromLatin1(kStatusMessageKey), message, {},
                          m_recognitionWindow.data());
    } else {
        m_messages->warning(QString::fromLatin1(kStatusMessageKey), message, {},
                            m_recognitionWindow.data());
    }
}
