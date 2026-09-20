#include "snow_canvas_widget_pointer_flow.h"

namespace snow_canvas_widget_pointer_flow {

bool isSelectedTextCopyGesture(Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
    return button == Qt::LeftButton && modifiers.testFlag(Qt::AltModifier) &&
           !modifiers.testFlag(Qt::ShiftModifier);
}

PressPlan planPress(const PressRequest& request) {
    PressPlan plan;
    if (!request.hasEvent) {
        return plan;
    }

    plan.shouldFocusWidget = true;
    using snow_canvas_widget_selection_hit_testing::SelectionInteractionTarget;
    const bool copySelectedText =
        isSelectedTextCopyGesture(request.button, request.modifiers) &&
        request.selectionTarget != SelectionInteractionTarget::Handle &&
        (request.pointerHitsSelectedText ||
         (request.textEditorActive && request.pointerOverSelectionInteraction &&
          request.selectionTarget == SelectionInteractionTarget::Move));
    if (copySelectedText) {
        // Duplication owns this press. The engine refuses copy while a draft is
        // active, so pending edits commit first and keep the element selected.
        plan.shouldCommitTextEditor = request.textEditorActive;
        plan.shouldRestoreSelectionOnCommit = request.textEditorActive;
        plan.dispatchAfterCommitRequiresRestoredSelection = request.textEditorActive;
        plan.shouldDispatchToEngine = true;
        return plan;
    }
    if (request.textEditorActive && request.pointerInsideTextEditor &&
        request.button == Qt::LeftButton) {
        plan.shouldHandleEditorPress = true;
        return plan;
    }

    const bool activeSelectionInteractionPress =
        request.button == Qt::LeftButton && request.pointerOverSelectionInteraction;
    plan.shouldCommitTextEditor = request.textEditorActive && !request.pointerInsideTextEditor &&
                                  !activeSelectionInteractionPress;
    plan.shouldRestoreSelectionOnCommit =
        plan.shouldCommitTextEditor && request.pointerOverSelectionInteraction;
    const bool suppressedTextCreateForPress = request.suppressNextTextToolCreate &&
                                              request.canvasTool == SnowCanvasTool::Text &&
                                              request.button == Qt::LeftButton;
    plan.suppressedTextCreateForPress = suppressedTextCreateForPress;
    plan.suppressedTextCreateRestoredSelection =
        suppressedTextCreateForPress && request.restoredSelectionForNextTextToolPress;
    plan.shouldBeginText =
        request.canvasTool == SnowCanvasTool::Text && request.button == Qt::LeftButton &&
        !(request.modifiers & Qt::ShiftModifier) && !activeSelectionInteractionPress;
    plan.shouldBeginSelectedText = (request.canvasTool == SnowCanvasTool::Select ||
                                    request.canvasTool == SnowCanvasTool::SerialNumber) &&
                                   request.button == Qt::LeftButton &&
                                   !(request.modifiers & Qt::ShiftModifier) &&
                                   !plan.shouldCommitTextEditor;
    plan.allowCreateText =
        plan.shouldBeginText && !plan.shouldCommitTextEditor && !suppressedTextCreateForPress;
    plan.shouldAcceptIfTextBeginFails =
        plan.shouldCommitTextEditor && !activeSelectionInteractionPress;
    plan.shouldAcceptSuppressedTextCreate =
        suppressedTextCreateForPress && !request.pointerOverSelectionInteraction;
    plan.dispatchAfterCommitRequiresRestoredSelection =
        plan.shouldCommitTextEditor && activeSelectionInteractionPress;
    plan.shouldDispatchToEngine = !plan.shouldCommitTextEditor || activeSelectionInteractionPress;
    return plan;
}

} // namespace snow_canvas_widget_pointer_flow
