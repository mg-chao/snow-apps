#include "snow_canvas_text_editor_session.h"

#include "snow_canvas_element_id.h"
#include "snow_canvas_text.h"
#include "snow_canvas_text_edit_geometry.h"
#include "snow_canvas_text_editor_input.h"
#include "snow_canvas_text_editor_view.h"
#include "snow_canvas_text_layout.h"

#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>

#include <cstdint>

bool SnowCanvasTextEditorSession::FinishedEdit::shouldCommit(bool hasViewport) const {
    return hasViewport && (hasExistingElement || !text.trimmed().isEmpty());
}

bool SnowCanvasTextEditorSession::begin(const SnowTextElementInfo& info,
                                        const SnowSceneDisplayItem* existingSceneItem,
                                        const QFont& baseFont, const SnowTextStyle* newTextStyle,
                                        const QString* completeText) {
    cancel();

    const bool hasExisting = snow_canvas_element_id::hasElementId(info.id);
    const QString initialText =
        completeText != nullptr ? *completeText : snow_canvas_text::textFromElementInfo(info);
    m_arrowId = info.arrow_id;
    m_arrowWidth = info.arrow_width;
    SnowCanvasSceneItem preview = snow_canvas_text::defaultPreviewItem(info);
    if (hasExisting) {
        if (existingSceneItem != nullptr) {
            preview = *existingSceneItem;
        }
        snow_canvas_text::copyTextToSceneItem(preview, initialText);
    } else {
        if (newTextStyle != nullptr) {
            snow_canvas_text::applyTextStyleToSceneItem(preview, *newTextStyle);
        }
        const QSizeF initialSize =
            snow_canvas_text_layout::measureNaturalText(initialText, baseFont, preview);
        preview.width = initialSize.width();
        preview.height = initialSize.height();
    }

    if (newTextStyle != nullptr && snow_canvas_element_id::hasElementId(m_arrowId)) {
        snow_canvas_text::applyTextStyleToSceneItem(preview, *newTextStyle);
    }
    const QPointF creationPoint(info.center_x, info.center_y);
    m_canvasAnchor =
        hasExisting
            ? snow_canvas_text_edit_geometry::topAnchorForItem(preview)
            : snow_canvas_text_edit_geometry::topAnchorForCreationPoint(preview, creationPoint);
    const QPointF anchoredCenter =
        snow_canvas_text_edit_geometry::centerForTopAnchor(preview, m_canvasAnchor);
    preview.center_x = anchoredCenter.x();
    preview.center_y = anchoredCenter.y();
    m_previewItem = preview;
    m_hasPreview = true;
    m_previewAutoResize = !hasExisting || info.auto_resize != 0;
    m_hasExistingElement = hasExisting;
    m_elementId = hasExisting ? info.id : SnowElementId{};
    m_draft.begin(initialText);
    if (snow_canvas_element_id::hasElementId(m_arrowId)) {
        m_canvasAnchor = creationPoint;
        updatePreviewLayout(baseFont, true);
    }
    return true;
}

SnowCanvasTextEditorSession::FinishedEdit
SnowCanvasTextEditorSession::finish(const QFont& baseFont) {
    FinishedEdit result;
    if (!m_hasPreview) {
        resetState();
        return result;
    }

    m_draft.clearPreedit();
    updatePreviewFromState(baseFont);
    result.text = m_draft.text();
    result.canvasCenter = QPointF(m_previewItem.center_x, m_previewItem.center_y);
    result.elementId = m_elementId;
    result.arrowId = m_arrowId;
    result.hasExistingElement = m_hasExistingElement;
    result.measuredLayout = SnowTextLayoutSize{
        m_previewItem.width,
        m_previewItem.height,
    };
    result.style = snow_canvas_text::textStyleFromSceneItem(m_previewItem);
    result.autoResize = m_previewAutoResize;
    result.styleChanged = m_styleChanged;
    resetState();
    return result;
}

void SnowCanvasTextEditorSession::cancel() {
    resetState();
}

bool SnowCanvasTextEditorSession::isActive() const {
    return m_hasPreview;
}

bool SnowCanvasTextEditorSession::containsViewPosition(const SceneDisplayInfo& sceneInfo,
                                                       const QPointF& viewPosition) const {
    if (!m_hasPreview) {
        return false;
    }
    return snow_canvas_text_editor_view::itemContainsViewPosition(m_previewItem, sceneInfo,
                                                                  viewPosition);
}

const SnowSceneDisplayItem* SnowCanvasTextEditorSession::previewItem() const {
    return m_hasPreview ? &m_previewItem : nullptr;
}

QString SnowCanvasTextEditorSession::presentationText() const {
    return m_hasPreview ? m_draft.displayText() : QString();
}

bool SnowCanvasTextEditorSession::activeDraftHasExistingElement() const {
    return m_hasPreview && m_hasExistingElement;
}

bool SnowCanvasTextEditorSession::activeDraftAutoResize() const {
    return m_hasPreview && m_previewAutoResize;
}

SnowTextStyle SnowCanvasTextEditorSession::currentTextStyle() const {
    return m_hasPreview ? snow_canvas_text::textStyleFromSceneItem(m_previewItem) : SnowTextStyle{};
}

bool SnowCanvasTextEditorSession::applyActiveDraftPresentation(const SnowTextElementInfo& info,
                                                               const SnowTextStyle& style) {
    if (!m_hasPreview) {
        return false;
    }
    const bool infoHasElement = snow_canvas_element_id::hasElementId(info.id);
    if (m_hasExistingElement != infoHasElement) {
        return false;
    }
    if (m_hasExistingElement && !snow_canvas_element_id::sameElementId(m_elementId, info.id)) {
        return false;
    }

    m_arrowId = info.arrow_id;
    m_arrowWidth = info.arrow_width;
    m_previewItem.center_x = info.center_x;
    m_previewItem.center_y = info.center_y;
    m_previewItem.width = qMax(1.0, info.width);
    m_previewItem.height = qMax(1.0, info.height);
    m_previewItem.rotation = info.rotation;
    snow_canvas_text::applyTextStyleToSceneItem(m_previewItem, style);
    snow_canvas_text::copyTextToSceneItem(m_previewItem, m_draft.displayText());
    m_previewAutoResize = info.auto_resize != 0;
    m_canvasAnchor = snow_canvas_element_id::hasElementId(m_arrowId)
                         ? QPointF(m_previewItem.center_x, m_previewItem.center_y)
                         : snow_canvas_text_edit_geometry::topAnchorForItem(m_previewItem);
    return true;
}

bool SnowCanvasTextEditorSession::syncActiveDraftPresentation(SnowRuntime runtime,
                                                              SnowViewport viewport,
                                                              const QFont& baseFont,
                                                              const SceneDisplayInfo& sceneInfo,
                                                              QRegion* outUpdateRegion) {
    if (!m_hasPreview || runtime == nullptr || viewport == nullptr) {
        return false;
    }

    SnowTextElementInfo info{};
    SnowTextStyle style{};
    std::uint8_t active = 0;
    if (snow_viewport_get_active_text_draft_presentation(runtime, viewport, &info, &style,
                                                         &active) != SNOW_OK ||
        active == 0) {
        return false;
    }

    QRegion updateRegion = editingRegion(baseFont, sceneInfo);
    if (!applyActiveDraftPresentation(info, style)) {
        return false;
    }
    updateRegion += editingRegion(baseFont, sceneInfo);
    if (outUpdateRegion != nullptr) {
        *outUpdateRegion += updateRegion;
    }
    return true;
}

QRegion SnowCanvasTextEditorSession::previewRegion(const SceneDisplayInfo& sceneInfo,
                                                   int paddingPx) const {
    if (!m_hasPreview) {
        return {};
    }
    return snow_canvas_text_editor_view::previewRegion(m_previewItem, sceneInfo, paddingPx);
}

QRegion SnowCanvasTextEditorSession::editingRegion(const QFont& baseFont,
                                                   const SceneDisplayInfo& sceneInfo) const {
    if (!m_hasPreview) {
        return {};
    }
    return snow_canvas_text_editor_view::editingRegion(m_previewItem, m_draft, baseFont, sceneInfo);
}

QRegion SnowCanvasTextEditorSession::caretRegion(const QFont& baseFont,
                                                 const SceneDisplayInfo& sceneInfo) const {
    if (!m_hasPreview) {
        return {};
    }
    return snow_canvas_text_editor_view::caretRegion(m_previewItem, m_draft, baseFont, sceneInfo);
}

void SnowCanvasTextEditorSession::updatePreviewFromState(const QFont& baseFont) {
    updatePreviewLayout(baseFont, false);
}

QRegion SnowCanvasTextEditorSession::updatePreviewFromState(const QFont& baseFont,
                                                            const SceneDisplayInfo& sceneInfo) {
    if (!m_hasPreview) {
        return {};
    }

    QRegion updateRegion = editingRegion(baseFont, sceneInfo);
    updatePreviewFromState(baseFont);
    updateRegion += editingRegion(baseFont, sceneInfo);
    return updateRegion;
}

QRegion SnowCanvasTextEditorSession::applyTextStyle(const SnowTextStyle& style,
                                                    const QFont& baseFont,
                                                    const SceneDisplayInfo& sceneInfo) {
    if (!m_hasPreview) {
        return {};
    }

    QRegion updateRegion = editingRegion(baseFont, sceneInfo);
    const double previousFontSize = m_previewItem.font_size;
    const QString previousFontFamily = snow_canvas_text::fontFamilyFromSceneItem(m_previewItem);
    snow_canvas_text::applyTextStyleToSceneItem(m_previewItem, style);
    m_styleChanged = true;
    m_canvasAnchor = snow_canvas_element_id::hasElementId(m_arrowId)
                         ? QPointF(m_previewItem.center_x, m_previewItem.center_y)
                         : snow_canvas_text_edit_geometry::topAnchorForItem(m_previewItem);
    const bool textLayoutChanged =
        previousFontSize != m_previewItem.font_size ||
        previousFontFamily != snow_canvas_text::fontFamilyFromSceneItem(m_previewItem);
    updatePreviewLayout(baseFont, textLayoutChanged);
    updateRegion += editingRegion(baseFont, sceneInfo);
    return updateRegion;
}

void SnowCanvasTextEditorSession::updateGeometry(const QFont& baseFont,
                                                 const SceneDisplayInfo& sceneInfo) {
    Q_UNUSED(sceneInfo);
    updatePreviewFromState(baseFont);
}

bool SnowCanvasTextEditorSession::setCursorFromViewPosition(const QFont& baseFont,
                                                            const SceneDisplayInfo& sceneInfo,
                                                            const QPointF& viewPosition,
                                                            bool keepSelection) {
    if (!m_hasPreview) {
        return false;
    }

    const bool clearedPreedit = m_draft.ensureNoPreeditForDirectEdit();
    const int position = snow_canvas_text_editor_view::cursorPositionForViewPosition(
        m_previewItem, baseFont, sceneInfo, viewPosition, m_draft.text());
    const bool cursorChanged = m_draft.setCursorPosition(position, keepSelection);
    if (clearedPreedit) {
        updatePreviewFromState(baseFont);
    }
    return clearedPreedit || cursorChanged;
}

SnowCanvasTextEditorSession::KeyResult
SnowCanvasTextEditorSession::handleKeyPress(QKeyEvent* event, const QFont& baseFont,
                                            const SceneDisplayInfo& sceneInfo) {
    KeyResult result;
    if (!m_hasPreview || event == nullptr) {
        return result;
    }

    const snow_canvas_text_editor_input::KeyResult commandResult =
        snow_canvas_text_editor_input::handleKeyPress(
            event, m_draft,
            [this, &baseFont, &sceneInfo](QTextCursor::MoveOperation operation,
                                          QTextCursor::MoveMode mode) {
                return moveCursor(operation, mode, baseFont, sceneInfo);
            });
    result.handled = commandResult.handled;
    result.changed = commandResult.changed;
    switch (commandResult.command) {
    case snow_canvas_text_editor_input::EventCommand::Commit:
        result.command = EventCommand::Commit;
        break;
    case snow_canvas_text_editor_input::EventCommand::Cancel:
        result.command = EventCommand::Cancel;
        break;
    case snow_canvas_text_editor_input::EventCommand::None:
    default:
        break;
    }

    if (result.changed) {
        updatePreviewFromState(baseFont);
    }
    return result;
}

bool SnowCanvasTextEditorSession::handleInputMethodEvent(QInputMethodEvent* event,
                                                         const QFont& baseFont) {
    if (!m_hasPreview || event == nullptr) {
        return false;
    }

    const bool changed = m_draft.handleInputMethodEvent(*event);
    if (changed) {
        updatePreviewFromState(baseFont);
    }
    return changed;
}

QVariant SnowCanvasTextEditorSession::inputMethodQuery(Qt::InputMethodQuery query,
                                                       const QFont& baseFont,
                                                       const SceneDisplayInfo& sceneInfo) const {
    if (!m_hasPreview) {
        return {};
    }

    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImHints:
        return static_cast<int>(Qt::ImhMultiLine);
    case Qt::ImCursorRectangle:
        return snow_canvas_text_editor_view::cursorRect(m_previewItem, baseFont, sceneInfo,
                                                        m_draft.displayText(),
                                                        m_draft.inputMethodCursorPosition());
    case Qt::ImCursorPosition:
        return m_draft.inputMethodCursorPosition();
    case Qt::ImAnchorPosition:
        return m_draft.inputMethodAnchorPosition();
    case Qt::ImSurroundingText:
        return m_draft.displayText();
    case Qt::ImCurrentSelection:
        return m_draft.inputMethodCurrentSelection();
    case Qt::ImFont:
        return snow_canvas_text_editor_view::inputMethodFont(m_previewItem, baseFont, sceneInfo);
    default:
        return {};
    }
}

void SnowCanvasTextEditorSession::renderEditorOverlay(QPainter& painter, const QFont& baseFont,
                                                      const SceneDisplayInfo& sceneInfo,
                                                      bool caretVisible) const {
    if (!m_hasPreview) {
        return;
    }

    snow_canvas_text_editor_view::renderOverlay(painter, m_previewItem, m_draft, baseFont,
                                                sceneInfo, caretVisible);
}

void SnowCanvasTextEditorSession::resetState() {
    m_canvasAnchor = {};
    m_elementId = {};
    m_arrowId = {};
    m_arrowWidth = 0.0;
    m_hasExistingElement = false;
    m_previewItem = SnowCanvasSceneItem{};
    m_hasPreview = false;
    m_previewAutoResize = false;
    m_styleChanged = false;
    m_draft.reset();
}

void SnowCanvasTextEditorSession::updatePreviewLayout(const QFont& baseFont, bool forceLayout) {
    if (!m_hasPreview) {
        return;
    }

    const QString text = m_draft.displayText();
    // Persisted engine geometry remains authoritative until a text or font input changes.
    if (!forceLayout && snow_canvas_text::textFromSceneItem(m_previewItem) == text) {
        return;
    }

    if (snow_canvas_element_id::hasElementId(m_arrowId)) {
        snow_canvas_text::copyTextToSceneItem(m_previewItem, text);
        const QSizeF natural =
            snow_canvas_text_layout::measureNaturalText(text, baseFont, m_previewItem);
        const double maximum = qMax(m_arrowWidth * 0.7, m_previewItem.font_size * 11.0);
        m_previewItem.width = qMin(natural.width(), maximum);
        m_previewItem.height = snow_canvas_text_layout::measureWrappedText(
                                   text, baseFont, m_previewItem, m_previewItem.width)
                                   .height();
        m_previewItem.rotation = 0.0;
        m_previewItem.center_x = m_canvasAnchor.x();
        m_previewItem.center_y = m_canvasAnchor.y();
        return;
    }
    snow_canvas_text::updatePreviewFromEditorText(m_previewItem, text, m_previewAutoResize,
                                                  baseFont);
    updatePreviewAnchor();
}

void SnowCanvasTextEditorSession::updatePreviewAnchor() {
    if (!m_hasPreview) {
        return;
    }

    const QPointF center =
        snow_canvas_text_edit_geometry::centerForTopAnchor(m_previewItem, m_canvasAnchor);
    m_previewItem.center_x = center.x();
    m_previewItem.center_y = center.y();
}

bool SnowCanvasTextEditorSession::moveCursor(QTextCursor::MoveOperation operation,
                                             QTextCursor::MoveMode mode, const QFont& baseFont,
                                             const SceneDisplayInfo& sceneInfo) {
    const bool clearedPreedit = m_draft.ensureNoPreeditForDirectEdit();
    const int position = snow_canvas_text_editor_view::movedCursorPosition(
        m_previewItem, baseFont, sceneInfo, m_draft.text(), m_draft.cursorPosition(), operation,
        mode);
    return m_draft.setCursorPosition(position, mode == QTextCursor::KeepAnchor) || clearedPreedit;
}
