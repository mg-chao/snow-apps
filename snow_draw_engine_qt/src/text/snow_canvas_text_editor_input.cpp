#include "snow_canvas_text_editor_input.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>

namespace snow_canvas_text_editor_input {
namespace {

bool hasShortcutModifier(const QKeyEvent& event) {
    return (event.modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) != 0;
}

bool hasTextInsertionBlocker(const QKeyEvent& event) {
    return (event.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) != 0;
}

double wheelDeltaY(const FontSizeWheelRequest& request) {
    if (request.pixelDeltaY != 0) {
        return static_cast<double>(request.pixelDeltaY);
    }
    return static_cast<double>(request.angleDeltaY) / 120.0 * 100.0;
}

} // namespace

KeyResult handleKeyPress(QKeyEvent* event, SnowCanvasTextDraft& draft,
                         const MoveCursor& moveCursor) {
    KeyResult result;
    if (event == nullptr) {
        return result;
    }

    result.handled = true;
    const int key = event->key();
    if (key == Qt::Key_Escape) {
        result.command = EventCommand::Cancel;
        return result;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
        (event->modifiers() & Qt::ControlModifier) != 0) {
        result.command = EventCommand::Commit;
        return result;
    }

    if (event->matches(QKeySequence::Undo)) {
        result.changed = draft.undoEdit();
        return result;
    }
    if (event->matches(QKeySequence::Redo)) {
        result.changed = draft.redoEdit();
        return result;
    }

    if (event->matches(QKeySequence::SelectAll)) {
        result.changed = draft.selectAll();
        return result;
    }
    if (event->matches(QKeySequence::Copy)) {
        if (draft.hasSelection()) {
            QApplication::clipboard()->setText(draft.selectedText());
        }
        return result;
    }
    if (event->matches(QKeySequence::Cut)) {
        if (draft.hasSelection()) {
            QApplication::clipboard()->setText(draft.selectedText());
            result.changed = draft.replaceSelection(QString());
        }
        return result;
    }
    if (event->matches(QKeySequence::Paste)) {
        if (const QClipboard* clipboard = QApplication::clipboard()) {
            result.changed = draft.replaceSelection(clipboard->text());
        }
        return result;
    }

    const QTextCursor::MoveMode moveMode = (event->modifiers() & Qt::ShiftModifier) != 0
                                               ? QTextCursor::KeepAnchor
                                               : QTextCursor::MoveAnchor;
    switch (key) {
    case Qt::Key_Backspace:
        result.changed = draft.deletePreviousCharacter();
        return result;
    case Qt::Key_Delete:
        result.command = EventCommand::DeleteElement;
        return result;
    case Qt::Key_Left:
        if (moveCursor) {
            result.changed =
                moveCursor(hasShortcutModifier(*event) ? QTextCursor::PreviousWord
                                                       : QTextCursor::PreviousCharacter,
                           moveMode);
        }
        return result;
    case Qt::Key_Right:
        if (moveCursor) {
            result.changed = moveCursor(hasShortcutModifier(*event) ? QTextCursor::NextWord
                                                                    : QTextCursor::NextCharacter,
                                        moveMode);
        }
        return result;
    case Qt::Key_Up:
        if (moveCursor) {
            result.changed = moveCursor(QTextCursor::Up, moveMode);
        }
        return result;
    case Qt::Key_Down:
        if (moveCursor) {
            result.changed = moveCursor(QTextCursor::Down, moveMode);
        }
        return result;
    case Qt::Key_Home:
        if (moveCursor) {
            result.changed = moveCursor(hasShortcutModifier(*event) ? QTextCursor::Start
                                                                    : QTextCursor::StartOfLine,
                                        moveMode);
        }
        return result;
    case Qt::Key_End:
        if (moveCursor) {
            result.changed = moveCursor(
                hasShortcutModifier(*event) ? QTextCursor::End : QTextCursor::EndOfLine, moveMode);
        }
        return result;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        result.changed = draft.replaceSelection(QStringLiteral("\n"));
        return result;
    default:
        break;
    }

    if (!event->text().isEmpty() && !hasTextInsertionBlocker(*event)) {
        result.changed = draft.replaceSelection(event->text());
        return result;
    }

    if (hasShortcutModifier(*event)) {
        return result;
    }

    result.handled = false;
    return result;
}

FontSizeWheelPlan planFontSizeWheel(const FontSizeWheelRequest& request) {
    FontSizeWheelPlan plan;
    if (!request.hasEvent || (request.canvasTool != SnowCanvasTool::Text &&
                              request.canvasTool != SnowCanvasTool::SerialNumber)) {
        return plan;
    }
    if ((request.modifiers & (Qt::ControlModifier | Qt::ShiftModifier)) != 0) {
        return plan;
    }

    plan.matchedToolWheel = true;
    const double deltaY = wheelDeltaY(request);
    if (deltaY == 0.0) {
        return plan;
    }

    plan.shouldStepFontSize = true;
    plan.increase = deltaY > 0.0;
    return plan;
}

} // namespace snow_canvas_text_editor_input
