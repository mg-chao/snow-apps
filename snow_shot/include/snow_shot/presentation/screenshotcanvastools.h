#pragma once

#include "snow_shot/presentation/screenshotcanvastoolmapping.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include <array>
#include <functional>
#include <optional>

namespace snow_shot::presentation {
struct ScreenshotCanvasToolDescriptor {
    SnowCanvasTool canvasTool;
    ScreenshotActiveTool activeTool;
    ScreenshotToolPalette::Tool paletteTool;
};
#define SNOW_SHOT_CANVAS_TOOL(signal, appTool, canvasTool)                                         \
    ScreenshotCanvasToolDescriptor{SnowCanvasTool::canvasTool, ScreenshotActiveTool::appTool,      \
                                   ScreenshotToolPalette::Tool::appTool},
inline constexpr std::array screenshotCanvasTools{

#include "snow_shot/presentation/screenshotcanvastools.inc"
};
#undef SNOW_SHOT_CANVAS_TOOL

struct ScreenshotCanvasToolRequest {
    void (ScreenshotToolPalette::*requested)();
    SnowCanvasTool canvasTool;
};
#define SNOW_SHOT_CANVAS_TOOL(signal, appTool, canvasTool)                                         \
    ScreenshotCanvasToolRequest{&ScreenshotToolPalette::signal, SnowCanvasTool::canvasTool},
inline constexpr std::array screenshotCanvasToolRequests{

#include "snow_shot/presentation/screenshotcanvastools.inc"
};
#undef SNOW_SHOT_CANVAS_TOOL

[[nodiscard]] inline std::optional<ScreenshotToolPalette::Tool>
paletteToolForCanvasTool(SnowCanvasTool tool) {
    for (const auto& descriptor : screenshotCanvasTools) {
        if (descriptor.canvasTool == tool)
            return descriptor.paletteTool;
    }
    return std::nullopt;
}

// Host callbacks retain recognition teardown, focus and capability policies.
inline void
bindScreenshotCanvasToolRequests(ScreenshotToolPalette& palette, QObject& owner,
                                 const std::function<void(SnowCanvasTool)>& activate,
                                 const std::function<bool(SnowCanvasTool)>& enabled = {}) {
    for (const auto& descriptor : screenshotCanvasToolRequests) {
        if (enabled && !enabled(descriptor.canvasTool))
            continue;
        QObject::connect(&palette, descriptor.requested, &owner,
                         [activate, tool = descriptor.canvasTool]() { activate(tool); });
    }
}
} // namespace snow_shot::presentation
