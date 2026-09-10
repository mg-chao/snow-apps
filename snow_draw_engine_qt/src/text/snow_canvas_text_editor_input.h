#pragma once

#include "snow_canvas_text_draft.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QTextCursor>
#include <Qt>

#include <functional>

class QKeyEvent;

namespace snow_canvas_text_editor_input {

enum class EventCommand {
    None,
    Commit,
    Cancel,
    DeleteElement,
};

struct KeyResult {
    EventCommand command = EventCommand::None;
    bool handled = false;
    bool changed = false;
};

using MoveCursor = std::function<bool(QTextCursor::MoveOperation, QTextCursor::MoveMode)>;

KeyResult handleKeyPress(QKeyEvent* event, SnowCanvasTextDraft& draft,
                         const MoveCursor& moveCursor);

struct FontSizeWheelRequest {
    bool hasEvent = false;
    SnowCanvasTool canvasTool = SnowCanvasTool::Select;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    int pixelDeltaY = 0;
    int angleDeltaY = 0;
};

struct FontSizeWheelPlan {
    bool matchedToolWheel = false;
    bool shouldStepFontSize = false;
    bool increase = false;
};

FontSizeWheelPlan planFontSizeWheel(const FontSizeWheelRequest& request);

} // namespace snow_canvas_text_editor_input
