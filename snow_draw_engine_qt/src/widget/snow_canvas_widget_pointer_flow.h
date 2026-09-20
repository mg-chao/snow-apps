#pragma once

#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_canvas_widget_selection_hit_testing.h"

#include <Qt>

namespace snow_canvas_widget_pointer_flow {

struct PressRequest {
    bool hasEvent = false;
    bool textEditorActive = false;
    bool pointerInsideTextEditor = false;
    SnowCanvasTool canvasTool = SnowCanvasTool::Select;
    Qt::MouseButton button = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    bool pointerOverSelectionInteraction = false;
    bool suppressNextTextToolCreate = false;
    bool restoredSelectionForNextTextToolPress = false;
    bool pointerHitsSelectedText = false;
    snow_canvas_widget_selection_hit_testing::SelectionInteractionTarget selectionTarget =
        snow_canvas_widget_selection_hit_testing::SelectionInteractionTarget::None;
};

struct PressPlan {
    bool shouldFocusWidget = false;
    bool shouldHandleEditorPress = false;
    bool shouldCommitTextEditor = false;
    bool shouldRestoreSelectionOnCommit = false;
    bool shouldBeginText = false;
    bool shouldBeginSelectedText = false;
    bool allowCreateText = false;
    bool shouldAcceptIfTextBeginFails = false;
    bool suppressedTextCreateForPress = false;
    bool suppressedTextCreateRestoredSelection = false;
    bool shouldAcceptSuppressedTextCreate = false;
    bool dispatchAfterCommitRequiresRestoredSelection = false;
    bool shouldDispatchToEngine = false;
};

bool isSelectedTextCopyGesture(Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
PressPlan planPress(const PressRequest& request);

} // namespace snow_canvas_widget_pointer_flow
