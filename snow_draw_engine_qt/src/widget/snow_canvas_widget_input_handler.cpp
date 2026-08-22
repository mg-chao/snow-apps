#include "snow_canvas_widget_input_handler.h"

#include "snow_canvas_event_flow.h"
#include "snow_canvas_interaction.h"

#include <QEvent>
#include <QWidget>

#include <utility>

SnowCanvasWidgetInputHandler::SnowCanvasWidgetInputHandler(QWidget& widget) : m_widget(widget) {}

bool SnowCanvasWidgetInputHandler::interactionEnabled() const {
    return m_interaction.isEnabled();
}

void SnowCanvasWidgetInputHandler::setInteractionEnabled(bool enabled) {
    m_interaction.setEnabled(m_widget, enabled);
}

void SnowCanvasWidgetInputHandler::setBaselineCursor(SnowCursorStyle style) {
    m_interaction.setBaselineCursor(m_widget, style);
}

void SnowCanvasWidgetInputHandler::setCursor(SnowCursorStyle style) {
    m_interaction.setCursor(m_widget, style);
}

void SnowCanvasWidgetInputHandler::clearTransientState() {
    m_interaction.clearTransientState(m_widget);
}

SnowCanvasWidgetInputHandler::ProcessResult
SnowCanvasWidgetInputHandler::process(const Context& context, const SnowInputEvent& event) {
    ProcessResult output;
    if (!m_interaction.isEnabled() || !context.hasViewport || !context.processInput) {
        return output;
    }

    snow_canvas_commands::ProcessInputResult result = context.processInput(event);
    if (!result.success) {
        return output;
    }

    m_interaction.applyOutput(m_widget, result.output);

    output.success = true;
    output.output = result.output;
    output.changedViewports = std::move(result.changedViewports);
    return output;
}

SnowCanvasWidgetInputHandler::ProcessResult
SnowCanvasWidgetInputHandler::processBatch(const Context& context,
                                           const std::vector<SnowInputEvent>& events) {
    ProcessResult output;
    if (!m_interaction.isEnabled() || !context.hasViewport || !context.processInputBatch ||
        events.empty()) {
        return output;
    }
    snow_canvas_commands::ProcessInputResult result = context.processInputBatch(events);
    if (!result.success) {
        return output;
    }
    m_interaction.applyOutput(m_widget, result.output);
    output.success = true;
    output.output = result.output;
    output.changedViewports = std::move(result.changedViewports);
    return output;
}

SnowCanvasWidgetInputHandler::DispatchResult
SnowCanvasWidgetInputHandler::dispatch(QEvent* event, const Context& context,
                                       const SnowInputEvent& input) {
    DispatchResult result;
    result.process = process(context, input);
    if (!snow_canvas_event_flow::shouldAcceptInputResult(result.process.success,
                                                         result.process.output)) {
        return result;
    }
    if (event != nullptr) {
        event->accept();
    }
    result.accepted = true;
    return result;
}
