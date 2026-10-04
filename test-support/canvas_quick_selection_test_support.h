#pragma once

#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QMouseEvent>
#include <cstdlib>
#include <iostream>

namespace canvas_quick_selection_test {
inline void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

inline bool mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point,
                  Qt::MouseButton button, Qt::MouseButtons buttons,
                  Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point), button, buttons, modifiers);
    event.setAccepted(false);
    QApplication::sendEvent(&canvas, &event);
    return event.isAccepted();
}

inline void drawStroke(SnowCanvasWidget& canvas) {
    require(canvas.setCanvasTool(SnowCanvasTool::FreeDraw), "activate pen fixture");
    mouse(canvas, QEvent::MouseButtonPress, {60, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {120, 100}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {180, 100}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {180, 100}, Qt::LeftButton, Qt::NoButton);
    require(canvas.resetEditingState() && canvas.setCanvasTool(SnowCanvasTool::FreeDraw),
            "clear fixture selection and restore pen");
    require(canvas.hasQuickSelectionTargetAt({120, 100}, Qt::RightButton),
            "right-click query finds stroke");
}

inline void selectAndDragStroke(SnowCanvasWidget& canvas) {
    require(mouse(canvas, QEvent::MouseButtonPress, {120, 100}, Qt::RightButton, Qt::RightButton,
                  Qt::AltModifier),
            "right press selects stroke");
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedFreeDraw,
            "right selection publishes selected stroke style");
    mouse(canvas, QEvent::MouseMove, {120, 135}, Qt::NoButton, Qt::RightButton, Qt::AltModifier);
    mouse(canvas, QEvent::MouseButtonRelease, {120, 135}, Qt::RightButton, Qt::NoButton);
    require(canvas.canvasTool() == SnowCanvasTool::FreeDraw &&
                canvas.hasQuickSelectionTargetAt({120, 135}, Qt::RightButton) &&
                !canvas.hasQuickSelectionTargetAt({120, 100}, Qt::RightButton),
            "right drag moves the stroke without duplication or tool change");
    require(canvas.undo() && canvas.hasQuickSelectionTargetAt({120, 100}, Qt::RightButton),
            "one undo restores the original position");
    require(canvas.redo() && canvas.hasQuickSelectionTargetAt({120, 135}, Qt::RightButton),
            "redo restores right drag");
}
} // namespace canvas_quick_selection_test
