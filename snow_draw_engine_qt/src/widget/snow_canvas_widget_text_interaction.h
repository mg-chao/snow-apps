#pragma once

#include "snow_canvas_commands.h"
#include "snow_canvas_display_cache.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_text_editor_session.h"
#include "snow_draw_engine.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QPoint>
#include <QPointF>
#include <QRegion>
#include <QTimer>
#include <QVariant>

class QFont;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QPainter;
class QWidget;
class SnowCanvasCursorController;

class SnowCanvasWidgetTextInteraction final {
  public:
    struct FinishedExistingEdit {
        SnowElementId elementId{};
        bool hasElement = false;
    };
    struct EditorEventResult {
        bool handled = false;
        bool consume = false;
        ScopedChangedViewportList changedViewports;
        FinishedExistingEdit finishedExistingEdit;
        bool sessionEnded = false;
    };
    struct CommitResult {
        ScopedChangedViewportList changedViewports;
        FinishedExistingEdit finishedExistingEdit;
        bool restoredExistingSelection = false;
        bool sessionEnded = false;
    };
    struct CancelResult {
        ScopedChangedViewportList changedViewports;
        bool sessionEnded = false;
    };
    struct InputMethodEventResult {
        bool handled = false;
        ScopedChangedViewportList changedViewports;
    };
    struct SelectionRestoreResult {
        bool restored = false;
        ScopedChangedViewportList changedViewports;
    };
    struct BeginResult {
        bool started = false;
        ScopedChangedViewportList firstChangedViewports;
        ScopedChangedViewportList secondChangedViewports;
    };
    struct StyleChangeResult {
        bool success = false;
        bool toolbarStateChanged = false;
        ScopedChangedViewportList changedViewports;
    };
    struct ActiveResizeMeasurementState {
        bool success = false;
        bool active = false;
    };
    struct ActiveResizeMeasurementResult {
        bool success = false;
        bool active = false;
        ScopedChangedViewportList changedViewports;
    };

    SnowCanvasWidgetTextInteraction(QWidget& widget, SnowCanvasCursorController& cursorController);

    SnowCanvasTextEditorSession& session();
    const SnowCanvasTextEditorSession& session() const;

    snow_canvas_commands::MutationResult measureArrowText(SnowRuntime runtime,
                                                          SnowViewport viewport);
    snow_canvas_commands::MutationResult measureSerialLabelLayout(SnowRuntime runtime,
                                                                  SnowViewport viewport);
    bool isActive() const;
    bool editorContains(const SnowCanvasDisplayCache& displayCache, const QPointF& position) const;
    bool selectionInteractionContains(const SnowCanvasDisplayCache& displayCache,
                                      const QPointF& position) const;
    const SnowSceneDisplayItem* previewItem() const;
    SnowTextStyle currentTextStyle() const;
    void renderEditorOverlay(QPainter& painter, const QFont& baseFont,
                             const SnowCanvasDisplayCache& displayCache);
    StyleChangeResult applyTextStyle(SnowRuntime runtime, SnowViewport viewport,
                                     SnowCanvasDisplayCache& displayCache,
                                     const SnowTextStyle& style);
    StyleChangeResult stepFontSize(SnowRuntime runtime, SnowViewport viewport,
                                   SnowCanvasDisplayCache& displayCache,
                                   const SnowTextStyle& fallbackStyle, bool increase);

    BeginResult beginArrow(SnowRuntime runtime, SnowViewport viewport,
                           SnowCanvasDisplayCache& displayCache, const QPointF& viewPosition,
                           bool selected = false);
    BeginResult beginAt(SnowRuntime runtime, SnowViewport viewport,
                        SnowCanvasDisplayCache& displayCache, const QPointF& viewPosition,
                        const SnowTextStyle& newTextStyle, bool allowCreate = true);
    BeginResult beginSelectedAt(SnowRuntime runtime, SnowViewport viewport,
                                SnowCanvasDisplayCache& displayCache, const QPointF& viewPosition,
                                bool requireSerialBoundText = false);
    bool beginForElement(const SnowTextElementInfo& info,
                         const SnowCanvasDisplayCache& displayCache,
                         const QPointF& fallbackViewPosition,
                         const SnowTextStyle* newTextStyle = nullptr,
                         bool placeCursorFromViewPosition = true, SnowRuntime runtime = nullptr);
    // Creates the bound label only. The caller must sync the returned changed
    // viewports into its display cache before beginning the editor: the editor
    // session needs the label's styled scene item, or its commit strips the
    // styling the label was created with.
    snow_canvas_commands::CreateSerialNumberTextResult
    createSerialNumberText(SnowRuntime runtime, SnowViewport viewport,
                           const SnowTextStyle& textStyle,
                           const SnowSerialNumberStyle& serialNumberStyle);
    BeginResult beginRequestedTextEdit(SnowRuntime runtime, SnowViewport viewport,
                                       const SnowCanvasDisplayCache& displayCache);
    // Starts a blank-canvas draft only after the engine authorized it by
    // `snow_viewport_take_new_text_draft_request` during this press.
    BeginResult beginRequestedNewTextDraft(SnowRuntime runtime, SnowViewport viewport,
                                           SnowCanvasDisplayCache& displayCache,
                                           const QPointF& viewPosition,
                                           const SnowTextStyle& newTextStyle);
    // Begins editing a freshly created bound label; requires the creation's
    // changed viewports to have been synced so the styled scene item exists.
    BeginResult beginCreatedText(SnowRuntime runtime, SnowViewport viewport,
                                 const snow_canvas_commands::CreateSerialNumberTextResult& result,
                                 const SnowCanvasDisplayCache& displayCache);
    ActiveResizeMeasurementState activeResizeMeasurementState(SnowRuntime runtime,
                                                              SnowViewport viewport) const;
    ActiveResizeMeasurementResult
    applyActiveResizeMeasurementIfNeeded(SnowRuntime runtime, SnowViewport viewport,
                                         const SnowCanvasDisplayCache& displayCache);

    CommitResult commit(SnowRuntime runtime, SnowViewport viewport, bool hasViewport,
                        SnowCanvasDisplayCache& displayCache, bool refocusWidget = true,
                        bool deleteElement = false);
    SelectionRestoreResult restoreFinishedExistingSelection(SnowRuntime runtime,
                                                            SnowViewport viewport,
                                                            const FinishedExistingEdit& edit);
    CancelResult cancel(SnowRuntime runtime, SnowViewport viewport,
                        const SnowCanvasDisplayCache& displayCache);

    bool handleEditorMousePress(QMouseEvent* event, const SnowCanvasDisplayCache& displayCache,
                                const QFont& baseFont);
    bool handleEditorMouseMove(QMouseEvent* event, const SnowCanvasDisplayCache& displayCache,
                               const QFont& baseFont);
    bool handleEditorMouseRelease(QMouseEvent* event);
    EditorEventResult handleKeyPress(QKeyEvent* event, SnowRuntime runtime, SnowViewport viewport,
                                     bool hasViewport, SnowCanvasDisplayCache& displayCache);
    bool handleKeyRelease(QKeyEvent* event);
    InputMethodEventResult handleInputMethodEvent(QInputMethodEvent* event, SnowRuntime runtime,
                                                  SnowViewport viewport,
                                                  const SnowCanvasDisplayCache& displayCache,
                                                  const QFont& baseFont);
    QVariant inputMethodQuery(Qt::InputMethodQuery query,
                              const SnowCanvasDisplayCache& displayCache,
                              const QFont& baseFont) const;
    QRegion setCursorFromViewPosition(const SnowCanvasDisplayCache& displayCache,
                                      const QFont& baseFont, const QPointF& viewPosition,
                                      bool keepSelection);

  private:
    QRegion applyEditorTextStyle(const SnowTextStyle& style,
                                 const SnowCanvasDisplayCache& displayCache, const QFont& baseFont);
    QRegion editingRegion(const SnowCanvasDisplayCache& displayCache, const QFont& baseFont) const;
    const SnowSceneDisplayItem* findTextSceneItem(const SnowCanvasDisplayCache& displayCache,
                                                  SnowElementId id) const;
    QPointF viewToCanvasPoint(const SnowCanvasDisplayCache& displayCache,
                              const QPointF& viewPosition) const;
    QPointF canvasToViewPoint(const SnowCanvasDisplayCache& displayCache,
                              const QPointF& canvasPosition) const;
    bool attachEditor(const SnowCanvasDisplayCache& displayCache,
                      const QPointF& fallbackViewPosition, bool placeCursorFromViewPosition);
    void setInputMethodEnabled(bool enabled);
    snow_canvas_commands::MutationResult
    publishActiveDraftPresentation(SnowRuntime runtime, SnowViewport viewport) const;
    snow_canvas_commands::MutationResult clearActiveDraftPresentation(SnowRuntime runtime,
                                                                      SnowViewport viewport) const;
    QRegion resetCaretBlink(const SnowCanvasDisplayCache& displayCache, const QFont& baseFont);
    void setCaretFlashTime(int flashTimeMs);
    void stopCaretBlink();
    void handleCaretBlinkTimeout();

    QWidget& m_widget;
    SnowCanvasCursorController& m_cursorController;
    SnowCanvasTextEditorSession m_session;
    QTimer m_caretBlinkTimer;
    QRegion m_caretUpdateRegion;
    int m_caretFlashTimeMs = 0;
    bool m_caretVisible = true;
    bool m_selectionDragging = false;
};
