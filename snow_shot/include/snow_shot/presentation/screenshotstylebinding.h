#pragma once

#include "snow_draw_engine_qt/snow_canvas_style_edit.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/screenshotwheelinput.h"
#include <QObject>
#include <functional>
#include <optional>

class SnowCanvasWidget;
class ScreenshotToolPalette;

namespace snow_shot::presentation {

// One binding per editor; only successful user commits are remembered and saved.
class ScreenshotStyleBinding final : public QObject {
  public:
    using Save = std::function<bool(const SnowCanvasStyleEdit&)>;
    using Replicate = std::function<void(const SnowCanvasStyleEdit&)>;
    ScreenshotStyleBinding(ScreenshotToolPalette& palette, SnowCanvasWidget& canvas,
                           QObject* parent, Replicate replicate = {}, Save save = {});
    [[nodiscard]] std::optional<bool> lastSaveSucceeded() const {
        return m_lastSaveSucceeded;
    }

  private:
    std::optional<bool> m_lastSaveSucceeded;
};

void replicateScreenshotStyleEdit(SnowCanvasWidget& peer, const SnowCanvasStyleEdit& edit);

// Returns false for font input, which belongs to the canvas text/draft handler.
[[nodiscard]] bool stepScreenshotStyle(ScreenshotToolPalette& palette, SnowCanvasWidget& canvas,
                                       int direction, bool fine = false);
[[nodiscard]] inline bool angleWheelTarget(const SnowCanvasWidget& canvas) {
    return canvas.interactionEnabled() && !canvas.hasActiveTextEditing() &&
           (canvas.canvasTool() == SnowCanvasTool::Angle ||
            canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedAngle);
}
[[nodiscard]] bool handleScreenshotStyleWheel(ScreenshotToolPalette& palette,
                                              SnowCanvasWidget& canvas, const QWheelEvent& event,
                                              WheelStepAccumulator& accumulator);
} // namespace snow_shot::presentation
