#pragma once

#include "snow_draw_engine.h"

#include <cstdint>

class SnowCanvasView;
class SnowCanvasCursorController;

namespace snow_canvas_interaction {

class Controller final {
  public:
    bool isEnabled() const;

    void setEnabled(SnowCanvasView& widget, SnowCanvasCursorController& cursorController,
                    bool enabled);
    void clearTransientState(SnowCanvasView& widget, SnowCanvasCursorController& cursorController);
    void applyOutput(SnowCanvasView& widget, SnowCanvasCursorController& cursorController,
                     const SnowInteractionOutput& output);

  private:
    bool m_enabled = true;
    std::uint32_t m_capturedPointerId = 0;
};

} // namespace snow_canvas_interaction
