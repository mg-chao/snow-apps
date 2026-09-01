#include "snow_canvas_display_cache.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_filter_render.h"
#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_viewport.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    std::size_t samples = 32'000;
    std::size_t iterations = 120;
    std::string trace = "noisy";
};

SnowInputEvent pointerEvent(SnowPointerEventType type, double x, double y) {
    SnowInputEvent event{};
    event.kind = SNOW_INPUT_EVENT_POINTER;
    event.pointer.pointer_id = 1;
    event.pointer.event_type = type;
    event.pointer.device = SNOW_POINTER_DEVICE_PEN;
    event.pointer.position_x = x;
    event.pointer.position_y = y;
    event.pointer.button =
        type == SNOW_POINTER_EVENT_MOVE ? SNOW_POINTER_BUTTON_NONE : SNOW_POINTER_BUTTON_PRIMARY;
    event.pointer.buttons = type == SNOW_POINTER_EVENT_UP ? 0 : 1;
    return event;
}

QPointF tracePoint(const Options& options, std::size_t index) {
    const double progress =
        options.samples <= 1 ? 0.0 : static_cast<double>(index) / (options.samples - 1);
    const double x = 100.0 + progress * 1'700.0;
    if (options.trace == "serpentine") {
        return {x, 540.0 + std::sin(progress * 24.0 * 3.141592653589793) * 260.0};
    }
    if (options.trace == "tight") {
        return {
            960.0 + std::cos(progress * 40.0 * 3.141592653589793) * (80.0 + progress * 500.0),
            540.0 + std::sin(progress * 40.0 * 3.141592653589793) * (80.0 + progress * 300.0),
        };
    }
    const double noise = options.trace == "straight" ? 0.0 : (index % 2 == 0 ? -0.2 : 0.2);
    return {x, 540.0 + noise};
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string name = argv[index];
        const std::string value = argv[index + 1];
        if (name == "--samples") {
            options.samples = std::max<std::size_t>(2, std::stoull(value));
        } else if (name == "--iterations") {
            options.iterations = std::max<std::size_t>(1, std::stoull(value));
        } else if (name == "--trace") {
            options.trace = value;
        }
    }
    return options;
}

bool dispatchBatch(SnowRuntime runtime, SnowViewport viewport,
                   const std::vector<SnowInputEvent>& events) {
    SnowInteractionOutput output{};
    ScopedChangedViewportList changed;
    return snow_viewport_process_pointer_move_batch_ex(runtime, viewport, events.data(),
                                                       static_cast<std::uint32_t>(events.size()),
                                                       &output, changed.outParam()) == SNOW_OK;
}

double milliseconds(Clock::duration value) {
    return std::chrono::duration<double, std::milli>(value).count();
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const std::size_t index =
        static_cast<std::size_t>(std::round(fraction * static_cast<double>(values.size() - 1)));
    return values[index];
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    const Options options = parseOptions(argc, argv);
    ScopedRuntimeHandle runtime;
    SnowCanvasViewport viewport;
    ScopedChangedViewportList toolChange;
    if (snow_runtime_create(runtime.outParam()) != SNOW_OK ||
        !viewport.create(runtime.get(), snow_canvas_viewport::defaultEngineConfig()) ||
        snow_viewport_set_surface_size(runtime.get(), viewport.get(), 1920, 1080) != SNOW_OK ||
        snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(), SNOW_ACTIVE_TOOL_PEN_FILTER,
                                         toolChange.outParam()) != SNOW_OK) {
        return 1;
    }
    SnowCanvasDisplayCache cache;
    if (!cache.sync(runtime.get(), viewport.get())) {
        return 2;
    }

    const QPointF first = tracePoint(options, 0);
    SnowInputEvent down = pointerEvent(SNOW_POINTER_EVENT_DOWN, first.x(), first.y());
    SnowInteractionOutput output{};
    ScopedChangedViewportList downChange;
    if (snow_viewport_process_input_ex(runtime.get(), viewport.get(), &down, &output,
                                       downChange.outParam()) != SNOW_OK) {
        return 3;
    }
    std::vector<SnowInputEvent> initial;
    initial.reserve(options.samples - 1);
    for (std::size_t index = 1; index < options.samples; ++index) {
        const QPointF point = tracePoint(options, index);
        initial.push_back(pointerEvent(SNOW_POINTER_EVENT_MOVE, point.x(), point.y()));
    }
    if (!dispatchBatch(runtime.get(), viewport.get(), initial) ||
        !cache.sync(runtime.get(), viewport.get())) {
        return 4;
    }

    QImage background(QSize(1920, 1080), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(72, 118, 164));
    QImage destination(background.size(), QImage::Format_ARGB32_Premultiplied);
    snow_canvas_filter_render::RenderWorkspace workspace;
    snow_canvas_pen_mask::PenMaskAtlas penMaskAtlas;
    std::vector<double> inputTimes;
    std::vector<double> cacheTimes;
    std::vector<double> renderTimes;
    std::size_t transportedPoints = 0;
    std::size_t maximumTransportedPoints = 0;

    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        std::vector<SnowInputEvent> moves;
        moves.reserve(8);
        for (std::size_t sample = 0; sample < 8; ++sample) {
            const double phase = static_cast<double>(iteration * 8 + sample);
            moves.push_back(pointerEvent(SNOW_POINTER_EVENT_MOVE,
                                         1700.0 + std::sin(phase * 0.05) * 80.0,
                                         540.0 + std::sin(phase * 0.15) * 12.0));
        }
        const auto inputStart = Clock::now();
        if (!dispatchBatch(runtime.get(), viewport.get(), moves)) {
            return 5;
        }
        const auto inputEnd = Clock::now();
        if (!cache.sync(runtime.get(), viewport.get())) {
            return 6;
        }
        const auto cacheEnd = Clock::now();
        const std::size_t patchPoints = cache.lastPenFilterGeometryPointCount();
        transportedPoints += patchPoints;
        maximumTransportedPoints = std::max(maximumTransportedPoints, patchPoints);

        inputTimes.push_back(milliseconds(inputEnd - inputStart));
        cacheTimes.push_back(milliseconds(cacheEnd - inputEnd));
        if (iteration % 10 == 0) {
            QRegion dirty = snow_canvas_display::dirtyRectsToRegion(
                cache.sceneDirtyRects(), cache.sceneDirtyRectCount(), destination.rect());
            if (dirty.isEmpty() || iteration % 1000 == 0) {
                dirty = QRegion(destination.rect());
            }
            QPainter painter(&destination);
            painter.setClipRegion(dirty);
            snow_canvas_renderer::SceneRenderRequest request;
            request.painter = &painter;
            request.displayInfo = &cache.sceneInfo();
            request.sceneItems = cache.sceneItems();
            request.sceneItemCount = cache.sceneItemCount();
            request.exposedRegion = dirty;
            request.backgroundImage = &background;
            request.displayCache = &cache;
            request.workspace = &workspace;
            request.cacheNamespace = &cache;
            request.penMaskAtlas = &penMaskAtlas;
            snow_canvas_renderer::renderSceneItems(request);
            painter.end();
            renderTimes.push_back(milliseconds(Clock::now() - cacheEnd));
        }
    }

    const std::uint32_t simplifiedPoints =
        cache.sceneItemCount() == 0
            ? 0
            : cache.sceneItems()[cache.sceneItemCount() - 1].arrow_point_count;
    const std::size_t rawSamples = options.samples + options.iterations * 8;
    std::cout << "trace=" << options.trace << " raw_samples=" << rawSamples
              << " simplified_points=" << simplifiedPoints
              << " reduction=" << (1.0 - static_cast<double>(simplifiedPoints) / rawSamples)
              << " input_p50_ms=" << percentile(inputTimes, 0.50)
              << " input_p95_ms=" << percentile(inputTimes, 0.95)
              << " cache_p50_ms=" << percentile(cacheTimes, 0.50)
              << " cache_p95_ms=" << percentile(cacheTimes, 0.95)
              << " render_p50_ms=" << percentile(renderTimes, 0.50)
              << " render_p95_ms=" << percentile(renderTimes, 0.95) << " mean_patch_points="
              << static_cast<double>(transportedPoints) / options.iterations
              << " max_patch_points=" << maximumTransportedPoints
              << " pen_atlas_bytes=" << penMaskAtlas.retainedBytes()
              << " pen_atlas_entries=" << penMaskAtlas.entryCount()
              << " total_atlas_budget=" << penMaskAtlas.byteBudget() << '\n';
    return penMaskAtlas.retainedBytes() < 8u * 1024u * 1024u ? 0 : 7;
}
