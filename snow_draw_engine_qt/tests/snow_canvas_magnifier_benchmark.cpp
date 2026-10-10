#include "snow_canvas_magnifier_renderer.h"
#include "snow_canvas_renderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

SnowCanvasSceneItem lens(int index) {
    SnowSceneDisplayItem item{};
    item.kind = SNOW_SCENE_DISPLAY_ITEM_MAGNIFIER;
    item.element_id = {static_cast<std::uint32_t>(index + 1), 1};
    item.center_x = 180 + (index % 6) * 300;
    item.center_y = 170 + (index / 6) * 250;
    item.width = 200;
    item.height = 160;
    item.opacity = 1;
    item.stroke = {255, 255, 255, 255};
    item.stroke_width = 3;
    item.rect_shape = static_cast<std::uint8_t>(index % 3);
    item.magnifier = {500 + index * 10.0, 500, 20, 16, 10};
    return SnowCanvasSceneItem(item);
}

void appendGuides(std::vector<SnowCanvasOverlayItem>& guides, const SnowCanvasSceneItem& lensItem,
                  bool selected) {
    auto source = lensItem;
    source.center_x = source.magnifier.source_center_x;
    source.center_y = source.magnifier.source_center_y;
    source.width = source.magnifier.source_width;
    source.height = source.magnifier.source_height;
    const QPainterPath path = snow_canvas_magnifier_renderer::lensPath(source);
    std::vector<SnowArrowPathCommand> commands;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto point = path.elementAt(i);
        SnowArrowPathCommand command{};
        if (point.isCurveTo()) {
            const auto control2 = path.elementAt(++i);
            const auto end = path.elementAt(++i);
            command.kind = SNOW_ARROW_PATH_COMMAND_CUBIC_TO;
            command.control1 = {point.x, point.y};
            command.control2 = {control2.x, control2.y};
            command.point = {end.x, end.y};
        } else {
            command.kind = point.isMoveTo() ? SNOW_ARROW_PATH_COMMAND_MOVE_TO
                                            : SNOW_ARROW_PATH_COMMAND_LINE_TO;
            command.point = {point.x, point.y};
        }
        commands.push_back(command);
    }
    SnowOverlayDisplayItem guide{};
    guide.kind = SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION;
    guide.stroke = {64, 150, 255, 255};
    guide.stroke_width = 1;
    guide.arrow_stroke_style = SNOW_STROKE_STYLE_DASHED;
    guide.arrow_path_commands = commands.data();
    guide.arrow_path_command_count = static_cast<std::uint32_t>(commands.size());
    guides.emplace_back(guide);
    if (selected) {
        guide.kind = SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT;
        guide.rect_kind = SNOW_OVERLAY_RECT_MAGNIFIER_SELECTION_FRAME;
        guide.center_x = source.center_x;
        guide.center_y = source.center_y;
        guide.width = source.width + 8;
        guide.height = source.height + 8;
        guide.rotation = source.rotation;
        guide.arrow_path_commands = nullptr;
        guide.arrow_path_command_count = 0;
        guides.emplace_back(guide);
    }
}

bool benchmark(const std::string& scenario, int iterations) {
    const int count = scenario == "single" || scenario == "creation" ? 1 : 24;
    std::vector<SnowCanvasSceneItem> items;
    if (scenario == "annotations") {
        for (int index = 0; index < 1000; ++index) {
            auto item = lens(index);
            item.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
            item.center_x = (index % 50) * 38 + 15;
            item.center_y = (index / 50) * 52 + 20;
            item.width = 20;
            item.height = 20;
            item.fill = {200, 60, 90, 255};
            item.stroke.a = 0;
            item.rect_shape = SNOW_DISPLAY_RECT_SHAPE_RECTANGLE;
            items.push_back(item);
        }
    }
    const auto firstLens = items.size();
    for (int index = 0; index < count; ++index)
        items.push_back(lens(index));
    std::vector<SnowCanvasOverlayItem> guides;
    if (scenario == "creation" || scenario == "selected") {
        for (const auto& item : items)
            appendGuides(guides, item, scenario == "selected");
    }
    QImage original(1920, 1080, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < original.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(original.scanLine(y));
        for (int x = 0; x < original.width(); ++x)
            row[x] = qRgb(x % 256, y % 256, (x + y) % 256);
    }
    const auto originalKey = original.cacheKey();
    const QList<SnowCanvasBaseImageSource> sources{{original, QRectF(0, 0, 1920, 1080), {}}};
    QImage output(original.size(), QImage::Format_ARGB32_Premultiplied);
    SceneDisplayInfo info{};
    info.surface_width = 1920;
    info.surface_height = 1080;
    info.camera_center_x = 960;
    info.camera_center_y = 540;
    info.camera_zoom = 1;
    OverlayDisplayInfo overlayInfo{};
    overlayInfo.surface_width = info.surface_width;
    overlayInfo.surface_height = info.surface_height;
    overlayInfo.camera_center_x = info.camera_center_x;
    overlayInfo.camera_center_y = info.camera_center_y;
    overlayInfo.camera_zoom = info.camera_zoom;
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    std::size_t sourceDraws = 0;
    for (int frame = -5; frame < iterations; ++frame) {
        output.fill(Qt::transparent);
        const double offset = (frame + 5) % 30;
        for (std::size_t index = firstLens; index < items.size(); ++index) {
            if (scenario == "lens_drag")
                items[index].center_x = lens(static_cast<int>(index - firstLens)).center_x + offset;
            if (scenario == "source_drag")
                items[index].magnifier.source_center_x = 500 + offset;
        }
        QPainter painter(&output);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setClipRect(QRect(0, 0, 1920, 1080));
        snow_canvas_renderer::SceneRenderRequest request;
        request.painter = &painter;
        request.displayInfo = &info;
        request.sceneItems = items.data();
        request.sceneItemCount = static_cast<std::uint32_t>(items.size());
        request.exposedRegion = QRegion(QRect(0, 0, 1920, 1080));
        request.clearBackgroundEnabled = false;
        request.baseImageSources = &sources;
        snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
        QElapsedTimer timer;
        timer.start();
        snow_canvas_renderer::renderSceneItems(request);
        snow_canvas_renderer::renderOverlayItems(painter, overlayInfo, guides.data(),
                                                 static_cast<std::uint32_t>(guides.size()),
                                                 request.exposedRegion);
        painter.end();
        const double milliseconds = timer.nsecsElapsed() / 1000000.0;
        if (frame >= 0)
            samples.push_back(milliseconds);
        const auto diagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
        sourceDraws = diagnostics.sourceLayerDrawCount;
        const auto effects = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
        if (effects.usedFilterPath || effects.replayedItemCount != 0 ||
            original.cacheKey() != originalKey || sourceDraws != static_cast<std::size_t>(count)) {
            std::cerr << "magnifier benchmark unexpectedly replayed/copied source content\n";
            return false;
        }
    }
    std::sort(samples.begin(), samples.end());
    const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
    const auto percentile = [&](double fraction) {
        return samples[static_cast<std::size_t>((samples.size() - 1) * fraction)];
    };
    std::cout << scenario << "," << count << "," << iterations << "," << std::fixed
              << std::setprecision(3) << mean << "," << percentile(0.5) << "," << percentile(0.95)
              << "," << sourceDraws << ","
              << static_cast<unsigned long long>(output.pixel(180, 170)) << '\n';
    return true;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    std::string scenario;
    int iterations = 100;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--iterations" && index + 1 < argc) {
            try {
                iterations = std::stoi(argv[++index]);
            } catch (...) {
                return 2;
            }
            if (iterations < 1)
                return 2;
        } else if (argument == "--scenario" && index + 1 < argc) {
            scenario = argv[++index];
        } else {
            std::cerr
                << "usage: snow-canvas-magnifier-benchmark [--iterations N] "
                   "[--scenario single|many|lens_drag|source_drag|annotations|creation|selected]\n";
            return 2;
        }
    }
    const std::vector<std::string> scenarios{"single",      "many",     "lens_drag", "source_drag",
                                             "annotations", "creation", "selected"};
    if (!scenario.empty() &&
        std::find(scenarios.begin(), scenarios.end(), scenario) == scenarios.end())
        return 2;
    std::cout << "scenario,lenses,iterations,mean_ms,p50_ms,p95_ms,source_draws,checksum\n";
    for (const auto& name : scenarios) {
        if ((scenario.empty() || scenario == name) && !benchmark(name, iterations))
            return 1;
    }
    return 0;
}
