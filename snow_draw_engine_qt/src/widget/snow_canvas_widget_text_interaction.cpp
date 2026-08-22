#include "snow_canvas_widget_text_interaction.h"

#include "snow_canvas_element_id.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_text.h"
#include "snow_canvas_text_edit_target.h"
#include "snow_canvas_text_editor_connector.h"
#include "snow_canvas_text_editor_input.h"
#include "snow_canvas_text_measurement.h"
#include "snow_canvas_widget_repaint.h"
#include "snow_canvas_widget_selection_hit_testing.h"

#include <QByteArray>
#include <QFont>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QStyleHints>
#include <QWheelEvent>
#include <QWidget>

#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace {

int caretBlinkPhaseDuration(int flashTimeMs) {
    // Qt reports the complete visible-plus-hidden flash cycle.
    return qMax(1, flashTimeMs / 2);
}

void updateInputMethod() {
    if (QInputMethod* inputMethod = QGuiApplication::inputMethod()) {
        inputMethod->update(Qt::ImEnabled | Qt::ImCursorRectangle | Qt::ImCursorPosition |
                            Qt::ImAnchorPosition | Qt::ImSurroundingText | Qt::ImCurrentSelection);
    }
}

void resetInputMethod() {
    if (QInputMethod* inputMethod = QGuiApplication::inputMethod()) {
        inputMethod->reset();
    }
}

void commitInputMethod() {
    if (QInputMethod* inputMethod = QGuiApplication::inputMethod()) {
        inputMethod->commit();
    }
}

QRegion committedTextRegion(const QRegion& editorRegion, const SnowCanvasDisplayCache& displayCache,
                            const QRect& widgetRect) {
    return editorRegion +
           snow_canvas_display::dirtyRectsToRegion(displayCache.sceneDirtyRects(),
                                                   displayCache.sceneDirtyRectCount(), widgetRect) +
           snow_canvas_display::dirtyRectsToRegion(
               displayCache.overlayDirtyRects(), displayCache.overlayDirtyRectCount(), widgetRect);
}

} // namespace

SnowCanvasWidgetTextInteraction::SnowCanvasWidgetTextInteraction(QWidget& widget)
    : m_widget(widget) {
    m_caretBlinkTimer.setTimerType(Qt::CoarseTimer);
    QObject::connect(&m_caretBlinkTimer, &QTimer::timeout, &m_caretBlinkTimer,
                     [this]() { handleCaretBlinkTimeout(); });

    if (QStyleHints* styleHints = QGuiApplication::styleHints()) {
        setCaretFlashTime(styleHints->cursorFlashTime());
        QObject::connect(styleHints, &QStyleHints::cursorFlashTimeChanged, &m_caretBlinkTimer,
                         [this](int flashTimeMs) { setCaretFlashTime(flashTimeMs); });
    }
}

SnowCanvasTextEditorSession& SnowCanvasWidgetTextInteraction::session() {
    return m_session;
}

const SnowCanvasTextEditorSession& SnowCanvasWidgetTextInteraction::session() const {
    return m_session;
}

bool SnowCanvasWidgetTextInteraction::isActive() const {
    return m_session.isActive();
}

bool SnowCanvasWidgetTextInteraction::editorContains(const SnowCanvasDisplayCache& displayCache,
                                                     const QPointF& position) const {
    return m_session.containsViewPosition(displayCache.sceneInfo(), position);
}

bool SnowCanvasWidgetTextInteraction::selectionInteractionContains(
    const SnowCanvasDisplayCache& displayCache, const QPointF& position) const {
    if (!m_session.isActive()) {
        return false;
    }
    return snow_canvas_widget_selection_hit_testing::pointerHitsSelectionInteraction(displayCache,
                                                                                     position);
}

const SnowSceneDisplayItem* SnowCanvasWidgetTextInteraction::previewItem() const {
    return m_session.previewItem();
}

SnowTextStyle SnowCanvasWidgetTextInteraction::currentTextStyle() const {
    return m_session.currentTextStyle();
}

snow_canvas_commands::MutationResult
SnowCanvasWidgetTextInteraction::publishActiveDraftPresentation(SnowRuntime runtime,
                                                                SnowViewport viewport) const {
    snow_canvas_commands::MutationResult result;
    const SnowSceneDisplayItem* preview = m_session.previewItem();
    if (preview == nullptr || preview->kind != SNOW_SCENE_DISPLAY_ITEM_TEXT) {
        result.success = true;
        return result;
    }

    const QByteArray utf8 = m_session.presentationText().toUtf8();
    return snow_canvas_commands::setActiveTextDraftPresentation(
        runtime, viewport,
        snow_canvas_commands::ActiveTextDraftPresentationRequest{
            preview->element_id,
            m_session.activeDraftHasExistingElement(),
            preview->center_x,
            preview->center_y,
            preview->width,
            preview->height,
            preview->rotation,
            utf8.constData(),
            static_cast<std::uint32_t>(utf8.size()),
            snow_canvas_text::textStyleFromSceneItem(*preview),
            m_session.activeDraftAutoResize(),
        });
}

snow_canvas_commands::MutationResult
SnowCanvasWidgetTextInteraction::clearActiveDraftPresentation(SnowRuntime runtime,
                                                              SnowViewport viewport) const {
    return snow_canvas_commands::clearActiveTextDraftPresentation(runtime, viewport);
}

void SnowCanvasWidgetTextInteraction::renderEditorOverlay(
    QPainter& painter, const QFont& baseFont, const SnowCanvasDisplayCache& displayCache) {
    m_caretUpdateRegion = m_session.caretRegion(baseFont, displayCache.sceneInfo());
    m_session.renderEditorOverlay(painter, baseFont, displayCache.sceneInfo(), m_caretVisible);
}

SnowCanvasWidgetTextInteraction::StyleChangeResult
SnowCanvasWidgetTextInteraction::applyTextStyle(SnowRuntime runtime, SnowViewport viewport,
                                                SnowCanvasDisplayCache& displayCache,
                                                const SnowTextStyle& style) {
    StyleChangeResult result;
    if (m_session.isActive()) {
        const QRegion updateRegion = applyEditorTextStyle(style, displayCache, m_widget.font());
        snow_canvas_widget_repaint::updateCoalesced(m_widget, updateRegion);
        snow_canvas_commands::MutationResult draftResult =
            publishActiveDraftPresentation(runtime, viewport);
        result.success = draftResult.success;
        result.changedViewports = std::move(draftResult.changedViewports);
        result.toolbarStateChanged = true;
        return result;
    }

    const snow_canvas_text_measurement::TextLayoutOverrideMeasurement layoutOverrides =
        snow_canvas_text_measurement::measureSelectedAutoResizeLayoutOverrides(
            snow_canvas_text_measurement::SelectedTextLayoutMeasurementRequest{
                runtime,
                viewport,
                style,
                m_widget.font(),
            });
    if (!layoutOverrides.success) {
        return result;
    }

    snow_canvas_commands::MutationResult mutation =
        snow_canvas_commands::setTextStyle(runtime, viewport, style, layoutOverrides.layouts);
    if (!mutation.success) {
        return result;
    }

    result.success = true;
    result.changedViewports = std::move(mutation.changedViewports);
    return result;
}

SnowCanvasWidgetTextInteraction::StyleChangeResult
SnowCanvasWidgetTextInteraction::stepFontSize(SnowRuntime runtime, SnowViewport viewport,
                                              SnowCanvasDisplayCache& displayCache,
                                              const SnowTextStyle& fallbackStyle, bool increase) {
    SnowTextStyle style = m_session.isActive() ? m_session.currentTextStyle() : fallbackStyle;
    const double nextFontSize =
        snow_canvas_text_measurement::steppedFontSize(style.font_size, increase);
    if (std::abs(nextFontSize - style.font_size) <= std::numeric_limits<double>::epsilon()) {
        StyleChangeResult result;
        result.success = true;
        return result;
    }

    style.font_size = nextFontSize;
    return applyTextStyle(runtime, viewport, displayCache, style);
}

SnowCanvasWidgetTextInteraction::WheelFontSizeResult
SnowCanvasWidgetTextInteraction::handleFontSizeWheel(SnowRuntime runtime, SnowViewport viewport,
                                                     SnowCanvasDisplayCache& displayCache,
                                                     const SnowTextStyle& fallbackStyle,
                                                     SnowCanvasTool canvasTool,
                                                     const QWheelEvent* event) {
    WheelFontSizeResult result;
    const snow_canvas_text_editor_input::FontSizeWheelPlan plan =
        snow_canvas_text_editor_input::planFontSizeWheel(
            snow_canvas_text_editor_input::FontSizeWheelRequest{
                event != nullptr,
                canvasTool,
                event != nullptr ? event->modifiers() : Qt::NoModifier,
                event != nullptr ? event->pixelDelta().y() : 0,
                event != nullptr ? event->angleDelta().y() : 0,
            });
    result.matchedToolWheel = plan.matchedToolWheel;
    if (!plan.shouldStepFontSize) {
        return result;
    }

    StyleChangeResult styleResult =
        stepFontSize(runtime, viewport, displayCache, fallbackStyle, plan.increase);
    result.success = styleResult.success;
    if (!styleResult.success) {
        return result;
    }

    result.handled = true;
    result.toolbarStateChanged = styleResult.toolbarStateChanged;
    result.changedViewports = std::move(styleResult.changedViewports);
    return result;
}

QRegion SnowCanvasWidgetTextInteraction::applyEditorTextStyle(
    const SnowTextStyle& style, const SnowCanvasDisplayCache& displayCache, const QFont& baseFont) {
    QRegion region = editingRegion(displayCache, baseFont);
    region += m_session.applyTextStyle(style, baseFont, displayCache.sceneInfo());
    region += editingRegion(displayCache, baseFont);
    region += resetCaretBlink(displayCache, baseFont);
    updateInputMethod();
    return region;
}

SnowCanvasWidgetTextInteraction::BeginResult SnowCanvasWidgetTextInteraction::beginAt(
    SnowRuntime runtime, SnowViewport viewport, SnowCanvasDisplayCache& displayCache,
    const QPointF& viewPosition, const SnowTextStyle& newTextStyle, bool allowCreate) {
    BeginResult result;
    if (runtime == nullptr || viewport == nullptr) {
        return result;
    }

    const std::optional<SnowTextElementInfo> target =
        snow_canvas_text_edit_target::resolveTextEditTarget(
            runtime, viewport, viewToCanvasPoint(displayCache, viewPosition), m_widget.font(),
            newTextStyle, allowCreate);
    if (!target.has_value()) {
        return result;
    }

    const bool existingText = snow_canvas_element_id::hasElementId(target->id);
    result.started = beginForElement(*target, displayCache, viewPosition,
                                     existingText ? nullptr : &newTextStyle);
    if (!result.started || !existingText) {
        if (result.started) {
            snow_canvas_commands::MutationResult draftResult =
                publishActiveDraftPresentation(runtime, viewport);
            result.firstChangedViewports = std::move(draftResult.changedViewports);
        }
        return result;
    }

    snow_canvas_commands::MutationResult selectResult =
        snow_canvas_commands::selectElement(runtime, viewport, target->id);
    if (selectResult.success) {
        result.firstChangedViewports = std::move(selectResult.changedViewports);
    }
    snow_canvas_commands::MutationResult draftResult =
        publishActiveDraftPresentation(runtime, viewport);
    if (draftResult.success) {
        result.secondChangedViewports = std::move(draftResult.changedViewports);
    }
    return result;
}

SnowCanvasWidgetTextInteraction::BeginResult SnowCanvasWidgetTextInteraction::beginSelectedAt(
    SnowRuntime runtime, SnowViewport viewport, SnowCanvasDisplayCache& displayCache,
    const QPointF& viewPosition, bool requireSerialBoundText) {
    BeginResult result;
    if (runtime == nullptr || viewport == nullptr) {
        return result;
    }

    const std::optional<SnowTextElementInfo> target =
        snow_canvas_text_edit_target::resolveSelectedTextEditTarget(
            runtime, viewport, viewToCanvasPoint(displayCache, viewPosition),
            requireSerialBoundText);
    if (!target.has_value()) {
        return result;
    }

    result.started = beginForElement(*target, displayCache, viewPosition);
    if (result.started) {
        snow_canvas_commands::MutationResult draftResult =
            publishActiveDraftPresentation(runtime, viewport);
        result.firstChangedViewports = std::move(draftResult.changedViewports);
    }
    return result;
}

bool SnowCanvasWidgetTextInteraction::beginForElement(const SnowTextElementInfo& info,
                                                      const SnowCanvasDisplayCache& displayCache,
                                                      const QPointF& fallbackViewPosition,
                                                      const SnowTextStyle* newTextStyle,
                                                      bool placeCursorFromViewPosition) {
    const SnowSceneDisplayItem* existingSceneItem = nullptr;
    if (snow_canvas_element_id::hasElementId(info.id)) {
        existingSceneItem = findTextSceneItem(displayCache, info.id);
    }

    if (!m_session.begin(info, existingSceneItem, m_widget.font(), newTextStyle)) {
        return false;
    }
    const bool attached =
        attachEditor(displayCache, fallbackViewPosition, placeCursorFromViewPosition);
    if (!attached) {
        m_session.cancel();
        setInputMethodEnabled(false);
    }
    return attached;
}

SnowCanvasWidgetTextInteraction::BeginResult SnowCanvasWidgetTextInteraction::beginCreatedText(
    SnowRuntime runtime, SnowViewport viewport,
    const snow_canvas_commands::CreateSerialNumberTextResult& result,
    const SnowCanvasDisplayCache& displayCache) {
    BeginResult beginResult;
    if (!result.success) {
        return beginResult;
    }

    if (!result.hasTextId) {
        beginResult.started = true;
        return beginResult;
    }

    if (runtime == nullptr) {
        return beginResult;
    }

    SnowTextElementInfo textInfo{};
    if (snow_runtime_get_text_element(runtime, result.textId, &textInfo) == SNOW_OK) {
        const bool started = beginForElement(
            textInfo, displayCache,
            canvasToViewPoint(displayCache, QPointF(textInfo.center_x, textInfo.center_y)), nullptr,
            false);
        if (started) {
            snow_canvas_commands::MutationResult draftResult =
                publishActiveDraftPresentation(runtime, viewport);
            beginResult.started = true;
            if (draftResult.success) {
                beginResult.firstChangedViewports = std::move(draftResult.changedViewports);
            }
            return beginResult;
        }
    }
    return beginResult;
}

SnowCanvasWidgetTextInteraction::SerialTextCreationResult
SnowCanvasWidgetTextInteraction::createSerialNumberText(
    SnowRuntime runtime, SnowViewport viewport, const SnowCanvasDisplayCache& displayCache,
    const SnowTextStyle& textStyle, const SnowSerialNumberStyle& serialNumberStyle) {
    SerialTextCreationResult result;
    const SnowTextLayoutSize layout =
        snow_canvas_text_measurement::measureSerialNumberBoundTextLayout(
            textStyle, serialNumberStyle, m_widget.font());
    snow_canvas_commands::CreateSerialNumberTextResult createResult =
        snow_canvas_commands::createSerialNumberText(runtime, viewport, layout);
    if (!createResult.success) {
        return result;
    }

    result.success = true;
    result.firstChangedViewports = std::move(createResult.changedViewports);
    BeginResult beginResult = beginCreatedText(runtime, viewport, createResult, displayCache);
    result.shouldRefocus = beginResult.started;
    result.secondChangedViewports = std::move(beginResult.firstChangedViewports);
    return result;
}

SnowCanvasWidgetTextInteraction::ActiveResizeMeasurementState
SnowCanvasWidgetTextInteraction::activeResizeMeasurementState(SnowRuntime runtime,
                                                              SnowViewport viewport) const {
    ActiveResizeMeasurementState state;
    const snow_canvas_commands::TextResizeMeasurementResult measurement =
        snow_canvas_commands::activeTextResizeMeasurement(runtime, viewport);
    if (!measurement.success) {
        return state;
    }

    state.success = true;
    state.active = measurement.active;
    return state;
}

SnowCanvasWidgetTextInteraction::ActiveResizeMeasurementResult
SnowCanvasWidgetTextInteraction::applyActiveResizeMeasurementIfNeeded(
    SnowRuntime runtime, SnowViewport viewport, const SnowCanvasDisplayCache& displayCache) {
    ActiveResizeMeasurementResult result;
    const snow_canvas_commands::TextResizeMeasurementResult measurement =
        snow_canvas_commands::activeTextResizeMeasurement(runtime, viewport);
    if (!measurement.success) {
        return result;
    }
    if (!measurement.active) {
        result.success = true;
        return result;
    }

    const SnowTextLayoutSize layout = snow_canvas_text_measurement::measureResizeLayout(
        snow_canvas_text_measurement::ResizeLayoutMeasurementRequest{
            measurement.info,
            m_widget.font(),
            displayCache.sceneInfo().camera_zoom,
        });
    snow_canvas_commands::MutationResult mutation =
        snow_canvas_commands::applyActiveTextResizeMeasurement(runtime, viewport, layout);
    if (!mutation.success) {
        return result;
    }

    result.success = true;
    result.active = true;
    result.changedViewports = std::move(mutation.changedViewports);
    return result;
}

SnowCanvasWidgetTextInteraction::CommitResult
SnowCanvasWidgetTextInteraction::commit(SnowRuntime runtime, SnowViewport viewport,
                                        bool hasViewport, SnowCanvasDisplayCache& displayCache,
                                        bool refocusWidget) {
    CommitResult commitResult;
    if (!m_session.isActive()) {
        return commitResult;
    }

    commitInputMethod();
    QRegion updateRegion = editingRegion(displayCache, m_widget.font());
    const SnowCanvasTextEditorSession::FinishedEdit edit = m_session.finish(m_widget.font());
    setInputMethodEnabled(false);
    stopCaretBlink();
    m_selectionDragging = false;
    commitResult.sessionEnded = true;
    commitResult.finishedExistingEdit.elementId = edit.elementId;
    commitResult.finishedExistingEdit.hasElement = edit.hasExistingElement;

    if (edit.shouldCommit(hasViewport)) {
        const QByteArray utf8 = edit.text.toUtf8();
        snow_canvas_commands::MutationResult result =
            snow_canvas_commands::commitText(runtime, viewport,
                                             snow_canvas_commands::CommitTextRequest{
                                                 edit.elementId,
                                                 edit.hasExistingElement,
                                                 edit.canvasCenter.x(),
                                                 edit.canvasCenter.y(),
                                                 utf8.constData(),
                                                 static_cast<std::uint32_t>(utf8.size()),
                                                 edit.measuredLayout,
                                                 edit.style,
                                                 edit.autoResize,
                                                 edit.styleChanged,
                                             });
        if (result.success) {
            commitResult.changedViewports = std::move(result.changedViewports);
        } else {
            snow_canvas_commands::MutationResult clearResult =
                clearActiveDraftPresentation(runtime, viewport);
            if (clearResult.success) {
                commitResult.changedViewports = std::move(clearResult.changedViewports);
            }
        }
        updateRegion = committedTextRegion(updateRegion, displayCache, m_widget.rect());
    } else {
        snow_canvas_commands::MutationResult clearResult =
            clearActiveDraftPresentation(runtime, viewport);
        if (clearResult.success) {
            commitResult.changedViewports = std::move(clearResult.changedViewports);
        }
    }

    resetInputMethod();
    if (refocusWidget) {
        m_widget.setFocus(Qt::OtherFocusReason);
    }
    snow_canvas_widget_repaint::updateCoalesced(m_widget, updateRegion);
    return commitResult;
}

SnowCanvasWidgetTextInteraction::SelectionRestoreResult
SnowCanvasWidgetTextInteraction::restoreFinishedExistingSelection(
    SnowRuntime runtime, SnowViewport viewport, const FinishedExistingEdit& edit) {
    SelectionRestoreResult result;
    if (!edit.hasElement) {
        return result;
    }

    snow_canvas_commands::MutationResult selectResult =
        snow_canvas_commands::selectElement(runtime, viewport, edit.elementId);
    if (!selectResult.success) {
        return result;
    }

    result.restored = true;
    result.changedViewports = std::move(selectResult.changedViewports);
    return result;
}

SnowCanvasWidgetTextInteraction::CancelResult
SnowCanvasWidgetTextInteraction::cancel(SnowRuntime runtime, SnowViewport viewport,
                                        const SnowCanvasDisplayCache& displayCache) {
    CancelResult result;
    if (!m_session.isActive()) {
        return result;
    }

    const QRegion updateRegion = editingRegion(displayCache, m_widget.font());
    m_session.cancel();
    setInputMethodEnabled(false);
    stopCaretBlink();
    m_selectionDragging = false;
    snow_canvas_commands::MutationResult clearResult =
        clearActiveDraftPresentation(runtime, viewport);
    result.changedViewports = std::move(clearResult.changedViewports);
    result.sessionEnded = true;
    resetInputMethod();
    m_widget.setFocus(Qt::OtherFocusReason);
    snow_canvas_widget_repaint::updateCoalesced(m_widget, updateRegion);
    return result;
}

bool SnowCanvasWidgetTextInteraction::handleEditorMousePress(
    QMouseEvent* event, const SnowCanvasDisplayCache& displayCache, const QFont& baseFont) {
    m_selectionDragging = false;
    if (event == nullptr || !m_session.isActive() || event->button() != Qt::LeftButton) {
        return false;
    }
    if (!editorContains(displayCache, event->position())) {
        return false;
    }

    m_widget.setFocus(Qt::MouseFocusReason);
    m_selectionDragging = true;
    setCursorFromViewPosition(displayCache, baseFont, event->position(),
                              (event->modifiers() & Qt::ShiftModifier) != 0);
    event->accept();
    return true;
}

bool SnowCanvasWidgetTextInteraction::handleEditorMouseMove(
    QMouseEvent* event, const SnowCanvasDisplayCache& displayCache, const QFont& baseFont) {
    if (event == nullptr || !m_session.isActive()) {
        return false;
    }

    if (!m_selectionDragging) {
        if (event->buttons() != Qt::NoButton) {
            return false;
        }

        if (editorContains(displayCache, event->position())) {
            event->accept();
            return true;
        }
        return false;
    }

    setCursorFromViewPosition(displayCache, baseFont, event->position(), true);
    event->accept();
    return true;
}

bool SnowCanvasWidgetTextInteraction::handleEditorMouseRelease(QMouseEvent* event) {
    if (event == nullptr || !m_selectionDragging || event->button() != Qt::LeftButton) {
        return false;
    }

    m_selectionDragging = false;
    event->accept();
    return true;
}

SnowCanvasWidgetTextInteraction::EditorEventResult
SnowCanvasWidgetTextInteraction::handleKeyPress(QKeyEvent* event, SnowRuntime runtime,
                                                SnowViewport viewport, bool hasViewport,
                                                SnowCanvasDisplayCache& displayCache) {
    EditorEventResult result;
    if (!m_session.isActive()) {
        return result;
    }

    const QRegion beforeRegion = editingRegion(displayCache, m_widget.font());
    const SnowCanvasTextEditorSession::KeyResult keyResult =
        m_session.handleKeyPress(event, m_widget.font(), displayCache.sceneInfo());
    if (!keyResult.handled) {
        return result;
    }

    result.handled = true;
    result.consume = true;
    if (event != nullptr) {
        event->accept();
    }
    switch (keyResult.command) {
    case SnowCanvasTextEditorSession::EventCommand::Commit: {
        CommitResult commitResult = commit(runtime, viewport, hasViewport, displayCache);
        result.finishedExistingEdit = commitResult.finishedExistingEdit;
        result.changedViewports = std::move(commitResult.changedViewports);
        result.sessionEnded = commitResult.sessionEnded;
        return result;
    }
    case SnowCanvasTextEditorSession::EventCommand::Cancel: {
        CancelResult cancelResult = cancel(runtime, viewport, displayCache);
        result.changedViewports = std::move(cancelResult.changedViewports);
        result.sessionEnded = cancelResult.sessionEnded;
        return result;
    }
    case SnowCanvasTextEditorSession::EventCommand::None:
    default:
        break;
    }

    const QRegion caretResetRegion = resetCaretBlink(displayCache, m_widget.font());
    if (keyResult.changed) {
        snow_canvas_commands::MutationResult draftResult =
            publishActiveDraftPresentation(runtime, viewport);
        result.changedViewports = std::move(draftResult.changedViewports);
        QRegion updateRegion = beforeRegion;
        updateRegion += editingRegion(displayCache, m_widget.font());
        updateRegion += caretResetRegion;
        snow_canvas_widget_repaint::updateCoalesced(m_widget, updateRegion);
        updateInputMethod();
    } else {
        snow_canvas_widget_repaint::updateClipped(m_widget, caretResetRegion);
    }
    return result;
}

bool SnowCanvasWidgetTextInteraction::handleKeyRelease(QKeyEvent* event) {
    if (!m_session.isActive()) {
        return false;
    }
    if (event != nullptr) {
        event->accept();
    }
    return true;
}

SnowCanvasWidgetTextInteraction::InputMethodEventResult
SnowCanvasWidgetTextInteraction::handleInputMethodEvent(QInputMethodEvent* event,
                                                        SnowRuntime runtime, SnowViewport viewport,
                                                        const SnowCanvasDisplayCache& displayCache,
                                                        const QFont& baseFont) {
    InputMethodEventResult result;
    if (!m_session.isActive()) {
        return result;
    }

    QRegion updateRegion = editingRegion(displayCache, baseFont);
    const bool changed = m_session.handleInputMethodEvent(event, baseFont);
    updateRegion += editingRegion(displayCache, baseFont);
    const QRegion caretResetRegion = resetCaretBlink(displayCache, baseFont);
    if (changed) {
        snow_canvas_commands::MutationResult draftResult =
            publishActiveDraftPresentation(runtime, viewport);
        result.changedViewports = std::move(draftResult.changedViewports);
        updateRegion += caretResetRegion;
        snow_canvas_widget_repaint::updateCoalesced(m_widget, updateRegion);
        updateInputMethod();
    } else {
        snow_canvas_widget_repaint::updateClipped(m_widget, caretResetRegion);
    }
    if (event != nullptr) {
        event->accept();
    }
    result.handled = true;
    return result;
}

QVariant
SnowCanvasWidgetTextInteraction::inputMethodQuery(Qt::InputMethodQuery query,
                                                  const SnowCanvasDisplayCache& displayCache,
                                                  const QFont& baseFont) const {
    if (!m_session.isActive()) {
        return {};
    }
    return m_session.inputMethodQuery(query, baseFont, displayCache.sceneInfo());
}

QRegion SnowCanvasWidgetTextInteraction::setCursorFromViewPosition(
    const SnowCanvasDisplayCache& displayCache, const QFont& baseFont, const QPointF& viewPosition,
    bool keepSelection) {
    if (!m_session.isActive()) {
        return {};
    }

    QRegion updateRegion = editingRegion(displayCache, baseFont);
    const bool changed = m_session.setCursorFromViewPosition(baseFont, displayCache.sceneInfo(),
                                                             viewPosition, keepSelection);
    const QRegion caretResetRegion = resetCaretBlink(displayCache, baseFont);
    if (!changed && caretResetRegion.isEmpty()) {
        return {};
    }
    if (changed) {
        updateRegion += editingRegion(displayCache, baseFont);
    } else {
        updateRegion = {};
    }
    updateRegion += caretResetRegion;
    snow_canvas_widget_repaint::updateCoalesced(m_widget, updateRegion);
    updateInputMethod();
    return updateRegion;
}

QRegion SnowCanvasWidgetTextInteraction::editingRegion(const SnowCanvasDisplayCache& displayCache,
                                                       const QFont& baseFont) const {
    QRegion region = m_session.editingRegion(baseFont, displayCache.sceneInfo());
    region += snow_canvas_text_editor_connector::connectorRegion(
        displayCache.sceneInfo(), displayCache.sceneItems(), displayCache.sceneItemCount(),
        m_session.previewItem());
    return region;
}

const SnowSceneDisplayItem*
SnowCanvasWidgetTextInteraction::findTextSceneItem(const SnowCanvasDisplayCache& displayCache,
                                                   SnowElementId id) const {
    if (!snow_canvas_element_id::hasElementId(id)) {
        return nullptr;
    }
    const SnowCanvasSceneItem* sceneItems = displayCache.sceneItems();
    const std::uint32_t sceneItemCount = displayCache.sceneItemCount();
    for (std::uint32_t index = 0; index < sceneItemCount; ++index) {
        const SnowSceneDisplayItem& item = sceneItems[index];
        if (item.kind == SNOW_SCENE_DISPLAY_ITEM_TEXT &&
            snow_canvas_element_id::sameElementId(item.element_id, id)) {
            return &item;
        }
    }
    return nullptr;
}

QPointF
SnowCanvasWidgetTextInteraction::viewToCanvasPoint(const SnowCanvasDisplayCache& displayCache,
                                                   const QPointF& viewPosition) const {
    const SceneDisplayInfo& sceneInfo = displayCache.sceneInfo();
    return snow_canvas_render_geometry::viewToCanvas(
        sceneInfo.camera_center_x, sceneInfo.camera_center_y, sceneInfo.camera_zoom,
        sceneInfo.surface_width, sceneInfo.surface_height, viewPosition);
}

QPointF
SnowCanvasWidgetTextInteraction::canvasToViewPoint(const SnowCanvasDisplayCache& displayCache,
                                                   const QPointF& canvasPosition) const {
    const SceneDisplayInfo& sceneInfo = displayCache.sceneInfo();
    return snow_canvas_render_geometry::canvasToView(
        sceneInfo.camera_center_x, sceneInfo.camera_center_y, sceneInfo.camera_zoom,
        sceneInfo.surface_width, sceneInfo.surface_height, canvasPosition.x(), canvasPosition.y());
}

bool SnowCanvasWidgetTextInteraction::attachEditor(const SnowCanvasDisplayCache& displayCache,
                                                   const QPointF& fallbackViewPosition,
                                                   bool placeCursorFromViewPosition) {
    if (!m_session.isActive()) {
        return false;
    }

    setInputMethodEnabled(true);
    resetInputMethod();
    QRegion updateRegion = editingRegion(displayCache, m_widget.font());
    m_session.updatePreviewFromState(m_widget.font());
    updateRegion += editingRegion(displayCache, m_widget.font());
    if (placeCursorFromViewPosition) {
        if (m_session.setCursorFromViewPosition(m_widget.font(), displayCache.sceneInfo(),
                                                fallbackViewPosition)) {
            updateRegion += editingRegion(displayCache, m_widget.font());
        }
    }
    updateRegion += resetCaretBlink(displayCache, m_widget.font());

    m_widget.setFocus(Qt::MouseFocusReason);
    snow_canvas_widget_repaint::updateCoalesced(m_widget, updateRegion);
    updateInputMethod();
    return true;
}

void SnowCanvasWidgetTextInteraction::setInputMethodEnabled(bool enabled) {
    if (m_widget.testAttribute(Qt::WA_InputMethodEnabled) == enabled) {
        return;
    }

    m_widget.setAttribute(Qt::WA_InputMethodEnabled, enabled);
    if (m_widget.hasFocus()) {
        updateInputMethod();
    }
}

QRegion SnowCanvasWidgetTextInteraction::resetCaretBlink(const SnowCanvasDisplayCache& displayCache,
                                                         const QFont& baseFont) {
    const QRegion previousCaretRegion = m_caretUpdateRegion;
    m_caretUpdateRegion = m_session.caretRegion(baseFont, displayCache.sceneInfo());

    QRegion repaintRegion;
    if (!m_caretVisible) {
        repaintRegion = previousCaretRegion + m_caretUpdateRegion;
    }
    m_caretVisible = true;
    if (m_caretFlashTimeMs > 0 && m_session.isActive()) {
        m_caretBlinkTimer.start(caretBlinkPhaseDuration(m_caretFlashTimeMs));
    } else {
        m_caretBlinkTimer.stop();
    }
    return repaintRegion;
}

void SnowCanvasWidgetTextInteraction::setCaretFlashTime(int flashTimeMs) {
    m_caretFlashTimeMs = qMax(0, flashTimeMs);
    if (!m_session.isActive()) {
        m_caretBlinkTimer.stop();
        m_caretVisible = true;
        return;
    }

    const bool needsRepaint = !m_caretVisible;
    m_caretVisible = true;
    if (m_caretFlashTimeMs > 0) {
        m_caretBlinkTimer.start(caretBlinkPhaseDuration(m_caretFlashTimeMs));
    } else {
        m_caretBlinkTimer.stop();
    }
    if (needsRepaint) {
        snow_canvas_widget_repaint::updateClipped(m_widget, m_caretUpdateRegion);
    }
}

void SnowCanvasWidgetTextInteraction::stopCaretBlink() {
    m_caretBlinkTimer.stop();
    m_caretUpdateRegion = {};
    m_caretVisible = true;
}

void SnowCanvasWidgetTextInteraction::handleCaretBlinkTimeout() {
    if (!m_session.isActive() || m_caretFlashTimeMs <= 0) {
        stopCaretBlink();
        return;
    }

    m_caretVisible = !m_caretVisible;
    snow_canvas_widget_repaint::updateClipped(m_widget, m_caretUpdateRegion);
}
