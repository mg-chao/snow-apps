#pragma once

#include "snow_draw_engine_qt/snow_canvas_style_edit.h"
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
                                       int direction);
} // namespace snow_shot::presentation
