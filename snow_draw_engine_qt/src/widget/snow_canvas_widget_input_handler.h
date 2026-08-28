#pragma once

#include "snow_canvas_commands.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_interaction.h"
#include "snow_draw_engine.h"

#include <functional>

class QEvent;
class QWidget;
class SnowCanvasCursorController;

class SnowCanvasWidgetInputHandler final {
  public:
    using ProcessInputCommand =
        std::function<snow_canvas_commands::ProcessInputResult(const SnowInputEvent&)>;
    using ProcessInputBatchCommand =
        std::function<snow_canvas_commands::ProcessInputResult(const std::vector<SnowInputEvent>&)>;

    struct Context {
        bool hasViewport = false;
        ProcessInputCommand processInput;
        ProcessInputBatchCommand processInputBatch;
    };

    struct ProcessResult {
        bool success = false;
        SnowInteractionOutput output{};
        ScopedChangedViewportList changedViewports;
    };

    struct DispatchResult {
        bool accepted = false;
        ProcessResult process;
    };

    SnowCanvasWidgetInputHandler(QWidget& widget, SnowCanvasCursorController& cursorController);

    bool interactionEnabled() const;
    void setInteractionEnabled(bool enabled);
    void clearTransientState();

    ProcessResult process(const Context& context, const SnowInputEvent& event);
    ProcessResult processBatch(const Context& context, const std::vector<SnowInputEvent>& events);
    DispatchResult dispatch(QEvent* event, const Context& context, const SnowInputEvent& input);

  private:
    QWidget& m_widget;
    SnowCanvasCursorController& m_cursorController;
    snow_canvas_interaction::Controller m_interaction;
};
