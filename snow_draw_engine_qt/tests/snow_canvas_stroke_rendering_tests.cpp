#include "snow_canvas_display_item.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_stroke_rendering.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_viewport.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QPainterPathStroker>
#include <QMouseEvent>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

SceneDisplayInfo displayInfo(int width = 320, int height = 220) {
    SceneDisplayInfo info{};
    info.camera_zoom = 1;
    info.surface_width = width;
    info.surface_height = height;
    info.clear_color = {255, 255, 255, 255};
    return info;
}

SnowCanvasSceneItem rectangle(SnowStrokeRenderingStyle style, int shape = 0) {
    SnowSceneDisplayItem source{};
    source.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    source.width = 180;
    source.height = 90;
    source.stroke = {25, 50, 90, 255};
    source.stroke_width = 2;
    source.opacity = 1;
    source.fill_style = SNOW_FILL_STYLE_SOLID;
    source.stroke_rendering_style = style;
    source.stroke_rendering_seed = 142857;
    source.rect_shape = static_cast<std::uint8_t>(shape);
    return SnowCanvasSceneItem(source);
}

SnowCanvasSceneItem speechBubbleRectangle(SnowStrokeRenderingStyle style, QPointF center = {},
                                          double rotation = 0) {
    SnowSceneDisplayItem source = rectangle(style);
    source.kind = SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE;
    source.center_x = center.x();
    source.center_y = center.y();
    source.rotation = rotation;
    const auto transform =
        snow_canvas_stroke_rendering::localToCanvas(center.x(), center.y(), rotation);
    std::array<SnowArrowPathCommand, 5> commands{};
    const std::array<QPointF, 5> vertices{{{-90, -45}, {90, -45}, {90, 45}, {-90, 45}, {-90, -45}}};
    for (std::size_t i = 0; i < commands.size(); ++i) {
        commands[i].kind =
            i == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
        const auto point = transform.map(vertices[i]);
        commands[i].point = {point.x(), point.y()};
    }
    source.arrow_path_commands = commands.data();
    source.arrow_path_command_count = static_cast<std::uint32_t>(commands.size());
    return SnowCanvasSceneItem(source);
}

SnowCanvasSceneItem arrow(SnowStrokeRenderingStyle style,
                          SnowArrowType route = SNOW_ARROW_TYPE_STRAIGHT,
                          SnowArrowhead head = SNOW_ARROWHEAD_ARROW) {
    std::array<SnowArrowPoint, 4> points{{{-100, 10}, {-35, -40}, {35, 40}, {100, 0}}};
    if (route == SNOW_ARROW_TYPE_STRAIGHT)
        points = {{{-100, 0}, {100, 0}, {0, 0}, {0, 0}}};
    if (route == SNOW_ARROW_TYPE_ELBOW)
        points = {{{-100, -30}, {0, -30}, {0, 35}, {100, 35}}};
    SnowSceneDisplayItem source{};
    source.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
    source.width = 200;
    source.height = 80;
    source.stroke = {25, 50, 90, 255};
    source.stroke_width = 2;
    source.opacity = 1;
    source.stroke_rendering_style = style;
    source.stroke_rendering_seed = 142857;
    source.arrow_type = route;
    source.arrow_ratio = 1;
    source.arrow_end_head = head;
    source.arrow_points = points.data();
    source.arrow_point_count = route == SNOW_ARROW_TYPE_STRAIGHT ? 2 : 4;
    return SnowCanvasSceneItem(source);
}

void paint(QImage& image, const SnowCanvasSceneItem& item, const SceneDisplayInfo& info,
           const QRegion& clip) {
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setClipRegion(clip);
    snow_canvas_renderer::SceneRenderRequest request;
    request.painter = &painter;
    request.displayInfo = &info;
    request.sceneItems = &item;
    request.sceneItemCount = 1;
    request.exposedRegion = clip;
    snow_canvas_renderer::renderSceneItems(request);
}

QImage render(const SnowCanvasSceneItem& item, double zoom = 1, double dpr = 1) {
    const QSize size(320, 220);
    QImage image(size * dpr, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    auto info = displayInfo();
    info.camera_zoom = zoom;
    paint(image, item, info, QRegion(QRect(QPoint(), size)));
    return image;
}

void publicStrokeCoverageMatchesCanvasLines() {
    QPainterPath path;
    path.moveTo(-100, 0);
    path.lineTo(100, 0);
    for (const auto style :
         {SnowCanvasStrokeRenderingStyle::Architect, SnowCanvasStrokeRenderingStyle::Artist,
          SnowCanvasStrokeRenderingStyle::Cartoonist}) {
        for (const auto pattern : {SnowCanvasStrokeStyle::Solid, SnowCanvasStrokeStyle::Dashed,
                                   SnowCanvasStrokeStyle::Dotted}) {
            auto item = arrow(static_cast<SnowStrokeRenderingStyle>(style),
                              SNOW_ARROW_TYPE_STRAIGHT, SNOW_ARROWHEAD_NONE);
            item.arrow_stroke_style = static_cast<SnowStrokeStyle>(pattern);
            const auto coverage = snowCanvasStrokeCoverage(path, item.stroke_width, pattern, style,
                                                           item.stroke_rendering_seed);
            require(!coverage.isEmpty() &&
                        coverage == snow_canvas_stroke_rendering::geometry(item).ink,
                    "public stroke coverage shares canvas dash and seeded rendering geometry");
            require(snowCanvasStrokeCoverage(path, item.stroke_width, pattern, style,
                                             item.stroke_rendering_seed) == coverage,
                    "public stroke coverage is deterministic without a runtime or viewport");
        }
    }
    require(snowCanvasStrokeCoverage(path, 0, SnowCanvasStrokeStyle::Solid,
                                     SnowCanvasStrokeRenderingStyle::Artist, 0)
                .isEmpty(),
            "zero-width public strokes contain no ink");
}

void arrowOutlinesCoverGeometryAndPreserveDisabledPixels() {
    for (const auto style :
         {SNOW_STROKE_RENDERING_STYLE_ARCHITECT, SNOW_STROKE_RENDERING_STYLE_ARTIST,
          SNOW_STROKE_RENDERING_STYLE_CARTOONIST}) {
        for (const auto route :
             {SNOW_ARROW_TYPE_STRAIGHT, SNOW_ARROW_TYPE_CURVE, SNOW_ARROW_TYPE_ELBOW}) {
            for (const auto pattern :
                 {SNOW_STROKE_STYLE_SOLID, SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED}) {
                for (const auto head :
                     {SNOW_ARROWHEAD_ARROW, SNOW_ARROWHEAD_TRIANGLE, SNOW_ARROWHEAD_CIRCLE_OUTLINE,
                      SNOW_ARROWHEAD_DIAMOND_OUTLINE}) {
                    auto item = arrow(style, route, head);
                    item.arrow_stroke_style = pattern;
                    const QImage plain = render(item);
                    item.arrow_outline_color = {255, 0, 100, 128};
                    require(render(item) == plain,
                            "zero outline width must preserve existing pixels");
                    item.arrow_outline_width = 8;
                    item.arrow_outline_color.a = 0;
                    require(render(item) == plain,
                            "transparent outline must preserve existing pixels");
                    item.arrow_outline_color.a = 128;
                    const QImage outlined = render(item);
                    require(outlined != plain,
                            "every arrow route, pattern, and head must support outlines");
                    const auto bounds =
                        snow_canvas_render_geometry::sceneItemBounds(displayInfo(), item)
                            .adjusted(-1, -1, 1, 1)
                            .toAlignedRect();
                    bool outerInk = false;
                    for (int y = 0; y < outlined.height(); ++y) {
                        for (int x = 0; x < outlined.width(); ++x) {
                            require(bounds.contains(x, y) || qAlpha(outlined.pixel(x, y)) == 0,
                                    "outline pixels remain inside culling bounds");
                            if (qAlpha(plain.pixel(x, y)) == 0 &&
                                qAlpha(outlined.pixel(x, y)) > 0) {
                                outerInk = true;
                                require(
                                    qAlpha(outlined.pixel(x, y)) <= 129,
                                    "overlapping outline contours must receive alpha only once");
                            }
                        }
                    }
                    require(outerInk, "outline extends the rendered arrow silhouette");
                }
            }
        }
    }
    auto item = arrow(SNOW_STROKE_RENDERING_STYLE_ARCHITECT);
    item.arrow_outline_width = 8;
    item.arrow_outline_color = {255, 0, 100, 128};
    render(item);
    snow_canvas_stroke_rendering::takeDiagnostics();
    item.arrow_outline_color = {30, 180, 10, 200};
    render(item, 1.5, 2.0);
    require(snow_canvas_stroke_rendering::takeDiagnostics().geometryBuilds == 0,
            "outline color, zoom, and DPI changes reuse coverage geometry");
    item.stroke.a = 0;
    const auto outlineOnly = render(item);
    require(qAlpha(outlineOnly.pixel(160, 113)) > 0,
            "outline remains visible with transparent arrow stroke");
    item.has_bound_text_element = 1;
    item.arrow_text_bounds[0] = -22;
    item.arrow_text_bounds[1] = -9;
    item.arrow_text_bounds[2] = 22;
    item.arrow_text_bounds[3] = 9;
    const auto labeled = render(item);
    for (int y = 96; y < 124; ++y)
        for (int x = 133; x < 187; ++x)
            require(qAlpha(labeled.pixel(x, y)) == 0,
                    "outline preserves transparent label exclusion");
}

void architectKeepsTheOriginalFastPath() {
    auto item = rectangle(SNOW_STROKE_RENDERING_STYLE_ARCHITECT);
    const QImage original = render(item);
    snow_canvas_stroke_rendering::takeDiagnostics();
    item.stroke_rendering_seed = 0;
    require(original == render(item), "Architect must be independent of the rendering seed");
    const auto diagnostics = snow_canvas_stroke_rendering::takeDiagnostics();
    require(diagnostics.geometryBuilds == 0 && diagnostics.strokeBuilds == 0,
            "Architect must retain its original rendering fast path");
}

void stylesAreStableAndDistinct() {
    for (int shape = 0; shape < 3; ++shape) {
        const auto architect = render(rectangle(SNOW_STROKE_RENDERING_STYLE_ARCHITECT, shape));
        const auto artist = render(rectangle(SNOW_STROKE_RENDERING_STYLE_ARTIST, shape));
        auto cartoon = rectangle(SNOW_STROKE_RENDERING_STYLE_CARTOONIST, shape);
        const auto cartoonImage = render(cartoon);
        require(architect != artist && artist != cartoonImage && architect != cartoonImage,
                "each rendering style must have a distinct outline");
        require(render(cartoon) == cartoonImage, "repeated rendering must not change the stroke");
        cartoon.stroke_rendering_seed = 0;
        require(render(cartoon) == render(cartoon), "seed zero must remain deterministic");
    }
}

void speechBubbleFillsKeepCanvasCoordinates() {
    for (const QPointF center : {QPointF(), QPointF(13, -9)}) {
        for (double rotation : {0.0, 0.37}) {
            for (auto fill : {SNOW_FILL_STYLE_SOLID, SNOW_FILL_STYLE_LINE,
                              SNOW_FILL_STYLE_CROSS_LINE, SNOW_FILL_STYLE_BORDER}) {
                auto architect =
                    speechBubbleRectangle(SNOW_STROKE_RENDERING_STYLE_ARCHITECT, center, rotation);
                architect.stroke.a = 0;
                architect.fill = {30, 80, 220, 128};
                architect.fill_style = fill;
                architect.opacity = 0.5;
                for (double zoom : {0.75, 1.25}) {
                    for (double dpr : {1.0, 1.5}) {
                        const auto expected = render(architect, zoom, dpr);
                        for (auto style : {SNOW_STROKE_RENDERING_STYLE_ARTIST,
                                           SNOW_STROKE_RENDERING_STYLE_CARTOONIST}) {
                            auto styled = architect;
                            styled.stroke_rendering_style = style;
                            require(render(styled, zoom, dpr) == expected,
                                    "bubble rendering styles preserve canvas-space fill patterns "
                                    "under translation, rotation, zoom and DPR changes");
                            styled.stroke.a = 128;
                            const QRect interior(
                                static_cast<int>((160 + center.x() * zoom) * dpr) - 8,
                                static_cast<int>((110 + center.y() * zoom) * dpr) - 8, 16, 16);
                            require(render(styled, zoom, dpr).copy(interior) ==
                                        expected.copy(interior),
                                    "visible styled bubble ink leaves the interior fill unchanged");
                        }
                    }
                }
            }
        }
    }
}

void speechBubbleRenderingStylesPreserveDashAndDotMetrics() {
    for (auto pattern : {SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED}) {
        for (auto style :
             {SNOW_STROKE_RENDERING_STYLE_ARCHITECT, SNOW_STROKE_RENDERING_STYLE_ARTIST,
              SNOW_STROKE_RENDERING_STYLE_CARTOONIST}) {
            auto item = speechBubbleRectangle(style);
            item.stroke_style = pattern;
            const auto image = render(item);
            const auto coverage = [&image](int x) {
                int alpha = 0;
                for (int y = 58; y <= 72; ++y)
                    alpha = std::max(alpha, qAlpha(image.pixel(x, y)));
                return alpha;
            };
            // The top edge starts at x=70 with a two-pixel stroke. Check established
            // mark centers and gaps away from the corners, allowing the styled edge
            // to bend vertically without changing the dash or dot measurements.
            const bool dashed = pattern == SNOW_STROKE_STYLE_DASHED;
            const double period = dashed ? 16.0 : 6.02;
            for (double distance = period * 2; distance < 160; distance += period) {
                const int mark = static_cast<int>(70 + distance + (dashed ? 4.0 : 0.0));
                const int gap = static_cast<int>(70 + distance + (dashed ? 12.0 : 2.5));
                require(coverage(mark) > 64 && coverage(gap) == 0,
                        "bubble rendering styles preserve shape dash gaps and circular dots");
            }
        }
    }
}

void speechBubbleContoursRetainFillsPatternsOpacityAndCaches() {
    std::vector<SnowArrowPathCommand> commands;
    QPainterPath contour;
    contour.setFillRule(Qt::WindingFill);
    for (double cx : {-50.0, 50.0}) {
        for (int vertex = 0; vertex <= 70; ++vertex) {
            const double angle = vertex == 70 ? 0.0 : vertex * 2 * 3.14159265358979323846 / 70;
            const QPointF point(cx + 25 * std::cos(angle), 25 * std::sin(angle));
            SnowArrowPathCommand command{};
            command.kind =
                vertex == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
            command.point = {point.x(), point.y()};
            commands.push_back(command);
            if (vertex == 0)
                contour.moveTo(point);
            else
                contour.lineTo(point);
        }
        contour.closeSubpath();
    }
    std::vector<SnowPathChunk> chunks;
    for (std::size_t offset = 0; offset < commands.size(); offset += 64) {
        SnowPathChunk chunk{};
        chunk.stable_id = chunks.size() + 1;
        chunk.command_start = chunk.command_offset = static_cast<std::uint32_t>(offset);
        chunk.command_count =
            static_cast<std::uint32_t>(std::min<std::size_t>(64, commands.size() - offset));
        if (offset != 0) {
            chunk.start_x = commands[offset - 1].point.x;
            chunk.start_y = commands[offset - 1].point.y;
        }
        chunk.min_x = -75;
        chunk.min_y = -25;
        chunk.max_x = 75;
        chunk.max_y = 25;
        chunks.push_back(chunk);
    }
    SnowPathChunkRange range{};
    range.insert_chunk_count = static_cast<std::uint32_t>(chunks.size());
    for (auto style :
         {SNOW_STROKE_RENDERING_STYLE_ARTIST, SNOW_STROKE_RENDERING_STYLE_CARTOONIST}) {
        for (auto pattern :
             {SNOW_STROKE_STYLE_SOLID, SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED}) {
            SnowSceneDisplayItem raw = rectangle(style);
            raw.kind = SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE;
            raw.stroke_style = pattern;
            raw.arrow_stroke_style = pattern;
            raw.stroke.a = 128;
            raw.opacity = 0.5;
            raw.arrow_path_commands = commands.data();
            raw.arrow_path_command_count = static_cast<std::uint32_t>(commands.size());
            SnowCanvasSceneItem unchunked(raw);
            raw.arrow_path_commands = nullptr;
            raw.arrow_path_command_count = 0;
            SnowCanvasSceneItem chunked(raw);
            require(chunked.applyPathGeometryPatch(
                        0, 1, &range, 1, chunks.data(), static_cast<std::uint32_t>(chunks.size()),
                        commands.data(), static_cast<std::uint32_t>(commands.size()), true, true),
                    "accept bubble contours crossing chunk boundaries");
            snow_canvas_stroke_rendering::takeDiagnostics();
            const QImage original = render(chunked);
            const auto built = snow_canvas_stroke_rendering::takeDiagnostics();
            require(built.geometryBuilds == 1 &&
                        built.strokeBuilds == (pattern == SNOW_STROKE_STYLE_SOLID ? 2u : 1u),
                    "bubble patterns use the existing single/double trace policy");
            require(original == render(unchunked),
                    "chunking must preserve styled disconnected contours");
            require(snow_canvas_stroke_rendering::geometry(chunked).originalFill == contour,
                    "hand-drawn bubbles retain their exact original fill contours");
            auto rotatedCommands = commands;
            const auto transform = snow_canvas_stroke_rendering::localToCanvas(13, -9, 0.37);
            for (auto& command : rotatedCommands) {
                const auto point = transform.map(QPointF(command.point.x, command.point.y));
                command.point = {point.x(), point.y()};
            }
            auto rotatedRaw = raw;
            rotatedRaw.center_x = 13;
            rotatedRaw.center_y = -9;
            rotatedRaw.rotation = 0.37;
            rotatedRaw.arrow_path_commands = rotatedCommands.data();
            rotatedRaw.arrow_path_command_count =
                static_cast<std::uint32_t>(rotatedCommands.size());
            const SnowCanvasSceneItem rotated(rotatedRaw);
            const auto& expectedInk = snow_canvas_stroke_rendering::geometry(chunked).ink;
            const auto& rotatedInk = snow_canvas_stroke_rendering::geometry(rotated).ink;
            require(expectedInk.elementCount() == rotatedInk.elementCount(),
                    "bubble transforms preserve seeded contour topology");
            for (int i = 0; i < expectedInk.elementCount(); ++i) {
                const auto a = expectedInk.elementAt(i);
                const auto b = rotatedInk.elementAt(i);
                require(QLineF(QPointF(a.x, a.y), QPointF(b.x, b.y)).length() < 1e-6,
                        "bubble transforms preserve local hand-drawn ink");
            }
            require(original.pixelColor(160, 110).alpha() == 0,
                    "disconnected bubble contours never acquire connecting strokes");
            for (int y = 0; y < original.height(); ++y)
                for (int x = 0; x < original.width(); ++x)
                    require(original.pixelColor(x, y).alpha() <= 64,
                            "translucent bubble strokes apply opacity exactly once");
            snow_canvas_stroke_rendering::takeDiagnostics();
            for (double zoom : {0.75, 1.25}) {
                for (double dpr : {1.0, 1.5, 2.0})
                    require(render(chunked, zoom, dpr) == render(chunked, zoom, dpr),
                            "zoom and DPR preserve deterministic bubble coverage");
                auto info = displayInfo();
                info.camera_zoom = zoom;
                QImage full(320, 220, QImage::Format_ARGB32_Premultiplied);
                full.fill(Qt::transparent);
                paint(full, chunked, info, QRegion(full.rect()));
                QImage tiled(full.size(), full.format());
                tiled.fill(Qt::transparent);
                for (int y = 0; y < full.height(); y += 64)
                    for (int x = 0; x < full.width(); x += 64)
                        paint(tiled, chunked, info, QRegion(QRect(x, y, 64, 64)));
                require(full == tiled, "partial bubble repaint matches complete coverage");
                const QRect bounds = snow_canvas_render_geometry::sceneItemBounds(info, chunked)
                                         .adjusted(-1, -1, 1, 1)
                                         .toAlignedRect();
                const QRect coldBounds =
                    snow_canvas_render_geometry::sceneItemBounds(
                        info, static_cast<const SnowSceneDisplayItem&>(chunked))
                        .adjusted(-1, -1, 1, 1)
                        .toAlignedRect();
                for (int y = 0; y < full.height(); ++y)
                    for (int x = 0; x < full.width(); ++x)
                        require((bounds.contains(x, y) && coldBounds.contains(x, y)) ||
                                    qAlpha(full.pixel(x, y)) == 0,
                                "cold and cached bubble bounds include all styled ink");
            }
            chunked.stroke = {220, 30, 40, 255};
            chunked.opacity = 0.75;
            static_cast<void>(render(chunked));
            const auto warm = snow_canvas_stroke_rendering::takeDiagnostics();
            require(warm.geometryBuilds == 0 && warm.strokeBuilds == 0,
                    "bubble geometry survives recolor, opacity, zoom and DPR changes");
            chunked.stroke.a = 0;
            for (auto fill : {SNOW_FILL_STYLE_SOLID, SNOW_FILL_STYLE_LINE,
                              SNOW_FILL_STYLE_CROSS_LINE, SNOW_FILL_STYLE_BORDER}) {
                chunked.fill = {30, 80, 220, 128};
                chunked.fill_style = fill;
                auto architect = chunked;
                architect.stroke_rendering_style = SNOW_STROKE_RENDERING_STYLE_ARCHITECT;
                require(render(chunked) == render(architect),
                        "all bubble fills stay identical to the original contours");
            }
            chunked.stroke.a = 255;
            chunked.stroke_width = 5;
            static_cast<void>(render(chunked));
            require(snow_canvas_stroke_rendering::takeDiagnostics().geometryBuilds == 1,
                    "stroke width changes invalidate bubble ink geometry");
            chunked.stroke_rendering_seed = 0;
            static_cast<void>(render(chunked));
            require(snow_canvas_stroke_rendering::takeDiagnostics().geometryBuilds == 1 &&
                        render(chunked) == render(chunked),
                    "changing to seed zero rebuilds deterministic bubble ink");
        }
    }
}

void serialNumberStrokeTypesOnlyChangeOutlinedRings() {
    for (const auto type :
         {SNOW_SERIAL_NUMBER_TYPE_OUTLINED_CIRCLE, SNOW_SERIAL_NUMBER_TYPE_OUTLINED_SQUARE,
          SNOW_SERIAL_NUMBER_TYPE_SOLID_CIRCLE, SNOW_SERIAL_NUMBER_TYPE_SOLID_SQUARE,
          SNOW_SERIAL_NUMBER_TYPE_CIRCLE}) {
        SnowSceneDisplayItem source{};
        source.kind = SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER;
        source.serial_number_type = static_cast<std::uint8_t>(type);
        source.width = source.height = 120;
        source.corner_radii = {10, 10, 10, 10};
        source.font_size = 28;
        source.text_utf8 = "12";
        source.text_utf8_len = 2;
        source.serial_number = 12;
        source.stroke = source.text_color = {25, 50, 90, 255};
        source.fill = {240, 220, 100, 255};
        source.fill_style = SNOW_FILL_STYLE_SOLID;
        source.stroke_width = 3;
        source.opacity = 0.7;
        source.stroke_rendering_seed = 142857;
        source.stroke_rendering_style = SNOW_STROKE_RENDERING_STYLE_ARCHITECT;
        const QImage architect = render(SnowCanvasSceneItem(source));
        source.stroke_rendering_style = SNOW_STROKE_RENDERING_STYLE_ARTIST;
        SnowCanvasSceneItem artist(source);
        const QImage artistImage = render(artist);
        source.stroke_rendering_style = SNOW_STROKE_RENDERING_STYLE_CARTOONIST;
        const QImage cartoon = render(SnowCanvasSceneItem(source));
        const bool outlined = type == SNOW_SERIAL_NUMBER_TYPE_OUTLINED_CIRCLE ||
                              type == SNOW_SERIAL_NUMBER_TYPE_OUTLINED_SQUARE;
        if (outlined) {
            require(architect != artistImage && artistImage != cartoon && architect != cartoon,
                    "outlined sequence numbers must expose three distinct stroke types");
            const QRect interior(120, 70, 80, 80);
            require(architect.copy(interior) == artistImage.copy(interior) &&
                        architect.copy(interior) == cartoon.copy(interior),
                    "stroke types must preserve sequence number glyphs and interior fills");
            require(artistImage == render(artist), "sequence number strokes must be deterministic");
            snow_canvas_stroke_rendering::takeDiagnostics();
            render(artist, 1.5, 1.5);
            require(snow_canvas_stroke_rendering::takeDiagnostics().geometryBuilds == 0,
                    "sequence number strokes must reuse coverage across zoom and DPI");
        } else {
            require(architect == artistImage && architect == cartoon,
                    "solid and numberless sequence styles must ignore stroke types");
        }
    }
}

void warmGeometrySurvivesColourAndOpacityChanges() {
    auto item = rectangle(SNOW_STROKE_RENDERING_STYLE_ARTIST, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE);
    render(item);
    snow_canvas_stroke_rendering::takeDiagnostics();
    render(item, 1.3, 1.5);
    item.stroke = {240, 30, 50, 96};
    item.opacity = 0.5;
    render(item);
    SnowSceneDisplayItem changed = item;
    changed.stroke = {30, 200, 50, 150};
    changed.opacity = 0.7;
    item = changed;
    render(item);
    auto copied = item;
    render(copied);
    const auto diagnostics = snow_canvas_stroke_rendering::takeDiagnostics();
    require(diagnostics.geometryBuilds == 0 && diagnostics.strokeBuilds == 0 &&
                diagnostics.coverageUnions == 0 && diagnostics.cacheHits == 4 &&
                diagnostics.cacheAllocations == 0 && diagnostics.cacheDetaches == 0,
            "warm paint, camera, colour, opacity and snapshot copies must reuse vector coverage");
    item.stroke_rendering_style = SNOW_STROKE_RENDERING_STYLE_CARTOONIST;
    render(item);
    require(snow_canvas_stroke_rendering::takeDiagnostics().geometryBuilds == 1,
            "style changes must rebuild coverage exactly once");
}

void translucentInkReceivesOpacityOnce() {
    for (auto style :
         {SNOW_STROKE_RENDERING_STYLE_ARTIST, SNOW_STROKE_RENDERING_STYLE_CARTOONIST}) {
        for (int shape = 0; shape < 6; ++shape) {
            auto item = shape < 3 ? rectangle(style, shape)
                                  : arrow(style, static_cast<SnowArrowType>(shape - 3));
            item.stroke.a = 128;
            item.opacity = 0.5;
            const QImage image = render(item);
            int inkPixels = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const int alpha = qAlpha(image.pixel(x, y));
                    require(alpha <= 65,
                            "overlapping sketch traces and arrowheads must not darken alpha");
                    inkPixels += alpha > 0 ? 1 : 0;
                }
            }
            require(inkPixels > 100, "each shape/arrow route must produce visible ink");
        }
    }
}

void patternsUseOneTraceAndFillsStayCrisp() {
    for (auto pattern :
         {SNOW_STROKE_STYLE_SOLID, SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED}) {
        auto item = rectangle(SNOW_STROKE_RENDERING_STYLE_CARTOONIST);
        item.stroke_style = pattern;
        item.fill = {80, 150, 220, 255};
        render(item);
        snow_canvas_stroke_rendering::takeDiagnostics();
        const auto& geometry = snow_canvas_stroke_rendering::geometry(item);
        require(geometry.originalFill.contains(QPointF(0, 0)) &&
                    geometry.originalFill.boundingRect() == QRectF(-90, -45, 180, 90),
                "stroke styling must preserve the original shape fill boundary");
        auto fresh = rectangle(SNOW_STROKE_RENDERING_STYLE_CARTOONIST);
        fresh.stroke_style = pattern;
        render(fresh);
        const auto diagnostics = snow_canvas_stroke_rendering::takeDiagnostics();
        require(diagnostics.strokeBuilds == (pattern == SNOW_STROKE_STYLE_SOLID ? 2U : 1U),
                "dashed and dotted outlines must use a single trace");
    }
}

void clippingAndBoundsIncludeStyledInk() {
    for (double zoom : {0.75, 1.0, 1.5}) {
        for (int shape = 0; shape < 8; ++shape) {
            auto item = shape < 3   ? rectangle(SNOW_STROKE_RENDERING_STYLE_CARTOONIST, shape)
                        : shape < 6 ? arrow(SNOW_STROKE_RENDERING_STYLE_CARTOONIST,
                                            static_cast<SnowArrowType>(shape - 3))
                                    : rectangle(SNOW_STROKE_RENDERING_STYLE_CARTOONIST);
            if (shape >= 6) {
                item.kind = SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER;
                item.serial_number_type =
                    static_cast<std::uint8_t>(shape == 6 ? SNOW_SERIAL_NUMBER_TYPE_OUTLINED_CIRCLE
                                                         : SNOW_SERIAL_NUMBER_TYPE_OUTLINED_SQUARE);
                item.width = item.height = 90;
                item.corner_radii = {6, 6, 6, 6};
            }
            item.rotation = shape < 3 ? 0.21 : 0;
            auto info = displayInfo();
            info.camera_zoom = zoom;
            QImage full(320, 220, QImage::Format_ARGB32_Premultiplied);
            full.fill(Qt::transparent);
            paint(full, item, info, QRegion(full.rect()));
            QImage tiled(full.size(), full.format());
            tiled.fill(Qt::transparent);
            for (int y = 0; y < full.height(); y += 64)
                for (int x = 0; x < full.width(); x += 64)
                    paint(tiled, item, info, QRegion(QRect(x, y, 64, 64)));
            require(full == tiled, "partial exposed tiles must match full repaint exactly");
            const QRect bounds = snow_canvas_render_geometry::sceneItemBounds(info, item)
                                     .adjusted(-1, -1, 1, 1)
                                     .toAlignedRect();
            for (int y = 0; y < full.height(); ++y)
                for (int x = 0; x < full.width(); ++x)
                    require(bounds.contains(x, y) || qAlpha(full.pixel(x, y)) == 0,
                            "styled paint must stay inside conservative culling bounds");
        }
    }
}

void retainedOverlayMatchesSceneArrow() {
    auto scene = arrow(SNOW_STROKE_RENDERING_STYLE_CARTOONIST, SNOW_ARROW_TYPE_CURVE);
    scene.arrow_outline_width = 8;
    scene.arrow_outline_color = {200, 100, 10, 128};
    SnowOverlayDisplayItem raw{};
    raw.kind = SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION;
    raw.width = scene.width;
    raw.height = scene.height;
    raw.stroke = scene.stroke;
    raw.stroke_width = scene.stroke_width;
    raw.arrow_outline_color = scene.arrow_outline_color;
    raw.arrow_outline_width = scene.arrow_outline_width;
    raw.stroke_rendering_style = scene.stroke_rendering_style;
    raw.stroke_rendering_seed = scene.stroke_rendering_seed;
    raw.arrow_type = scene.arrow_type;
    raw.arrow_ratio = scene.arrow_ratio;
    raw.arrow_end_head = scene.arrow_end_head;
    raw.arrow_points = scene.arrow_points;
    raw.arrow_point_count = scene.arrow_point_count;
    SnowCanvasOverlayItem overlay(raw);
    require(snow_canvas_stroke_rendering::geometry(scene).ink ==
                snow_canvas_stroke_rendering::geometry(overlay).ink,
            "overlay and committed arrows must share identical geometry");
}

void arrowLabelsKeepTransparentExclusion() {
    auto item = arrow(SNOW_STROKE_RENDERING_STYLE_CARTOONIST);
    item.has_bound_text_element = 1;
    item.arrow_text_bounds[0] = -22;
    item.arrow_text_bounds[1] = -9;
    item.arrow_text_bounds[2] = 22;
    item.arrow_text_bounds[3] = 9;
    const QImage image = render(item);
    for (int y = 96; y < 124; ++y)
        for (int x = 133; x < 187; ++x)
            require(qAlpha(image.pixel(x, y)) == 0,
                    "arrow label exclusion must remain transparent under styled strokes");
}

void taperedContoursKeepOneSilhouetteAndFixedTips() {
    const std::array<SnowArrowPoint, 2> points{{{-100, 0}, {100, 0}}};
    std::array<SnowArrowPathCommand, 8> commands{};
    const std::array<SnowArrowPoint, 8> contour{
        {{-100, 0}, {70, -6}, {70, -15}, {100, 0}, {70, 15}, {70, 6}, {-100, 0}, {-100, 0}}};
    for (std::size_t i = 0; i < commands.size(); ++i) {
        commands[i].kind =
            i == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
        commands[i].point = contour[i];
    }
    SnowSceneDisplayItem raw = arrow(SNOW_STROKE_RENDERING_STYLE_CARTOONIST);
    raw.arrow_shaft_type = SNOW_ARROW_SHAFT_TYPE_TAPERED;
    raw.arrow_end_head = SNOW_ARROWHEAD_TRIANGLE;
    raw.arrow_points = points.data();
    raw.arrow_point_count = static_cast<std::uint32_t>(points.size());
    raw.arrow_path_commands = commands.data();
    raw.arrow_path_command_count = static_cast<std::uint32_t>(commands.size());
    SnowCanvasSceneItem item(raw);
    const auto& geometry = snow_canvas_stroke_rendering::geometry(item);
    require(geometry.ink.contains(QPointF(99, 0)), "tapered arrow retains its exact terminal tip");
    require(geometry.ink.contains(QPointF(70, 0)),
            "tapered arrow retains its exact joined head neck");
    require(!geometry.ink.contains(QPointF(90, 12)),
            "tapered arrow does not acquire a doubled outline");
    item.stroke.a = 128;
    const auto image = render(item);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            require(qAlpha(image.pixel(x, y)) <= 129,
                    "a styled tapered contour must receive alpha exactly once");
}

void allArrowheadPrimitivesRespectCaps() {
    for (auto kind : {SNOW_ARROWHEAD_PRIMITIVE_LINE, SNOW_ARROWHEAD_PRIMITIVE_POLYGON,
                      SNOW_ARROWHEAD_PRIMITIVE_CIRCLE}) {
        SnowArrowheadPrimitive primitive{};
        primitive.kind = kind;
        primitive.points[0] = {100, 0};
        primitive.points[1] = {75, -12};
        primitive.points[2] = {75, 12};
        primitive.points[3] = {100, 0};
        primitive.point_count = kind == SNOW_ARROWHEAD_PRIMITIVE_LINE ? 2 : 4;
        primitive.center = {100, 0};
        primitive.diameter = 20;
        primitive.dash_mode = SNOW_ARROWHEAD_DASH_SOLID;
        primitive.fill_mode = SNOW_ARROWHEAD_FILL_BACKGROUND;
        primitive.roughness_cap = kind == SNOW_ARROWHEAD_PRIMITIVE_CIRCLE ? 0.5 : 1;
        auto item = arrow(SNOW_STROKE_RENDERING_STYLE_CARTOONIST);
        SnowSceneDisplayItem raw = item;
        raw.arrowhead_primitives = &primitive;
        raw.arrowhead_primitive_count = 1;
        item = raw;
        require(!render(item).isNull(), "capped native arrowhead primitives render offscreen");
        const auto& geometry = snow_canvas_stroke_rendering::geometry(item);
        require(geometry.ink.boundingRect().right() <= 112,
                "capped arrowhead jitter stays inside conservative head paint bounds");
    }
}

void chunkBoundariesPreservePatternPhase() {
    std::array<SnowArrowPathCommand, 3> commands{};
    commands[0].kind = SNOW_ARROW_PATH_COMMAND_MOVE_TO;
    commands[0].point = {-100, 0};
    commands[1].kind = commands[2].kind = SNOW_ARROW_PATH_COMMAND_LINE_TO;
    commands[1].point = {-7, 0};
    commands[2].point = {100, 0};
    SnowPathChunk chunks[2]{};
    chunks[0].stable_id = 1;
    chunks[0].command_count = 2;
    chunks[0].start_x = chunks[0].min_x = -100;
    chunks[0].max_x = -7;
    chunks[1].stable_id = 2;
    chunks[1].command_start = chunks[1].command_offset = 2;
    chunks[1].command_count = 1;
    chunks[1].start_x = chunks[1].min_x = -7;
    chunks[1].max_x = 100;
    chunks[1].cumulative_start_length = 93;
    SnowPathChunkRange range{};
    range.insert_chunk_count = 2;
    for (auto pattern : {SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED}) {
        auto unchunked = arrow(SNOW_STROKE_RENDERING_STYLE_ARTIST, SNOW_ARROW_TYPE_STRAIGHT,
                               SNOW_ARROWHEAD_NONE);
        SnowSceneDisplayItem raw = unchunked;
        raw.arrow_stroke_style = pattern;
        raw.arrow_path_commands = commands.data();
        raw.arrow_path_command_count = static_cast<std::uint32_t>(commands.size());
        unchunked = raw;
        auto chunked = unchunked;
        require(chunked.applyPathGeometryPatch(0, 1, &range, 1, chunks, 2, commands.data(),
                                               static_cast<std::uint32_t>(commands.size()), false,
                                               true),
                "styled arrow accepts retained centerline chunks");
        require(render(unchunked) == render(chunked),
                "continuous path chunks must preserve dash/dot phase across their boundary");
    }
}

void translationAndRotationPreserveSeededGeometry() {
    const auto original = arrow(SNOW_STROKE_RENDERING_STYLE_CARTOONIST, SNOW_ARROW_TYPE_CURVE);
    const QPainterPath expected = snow_canvas_stroke_rendering::geometry(original).ink;
    const auto transform = snow_canvas_stroke_rendering::localToCanvas(37, -24, 0.37);
    std::vector<SnowArrowPoint> points;
    for (std::uint32_t i = 0; i < original.arrow_point_count; ++i) {
        const auto& source = original.arrow_points[i];
        const QPointF point = transform.map(QPointF(source.x, source.y));
        points.push_back({point.x(), point.y()});
    }
    SnowSceneDisplayItem raw = original;
    raw.center_x = 37;
    raw.center_y = -24;
    raw.rotation = 0.37;
    raw.arrow_points = points.data();
    SnowCanvasSceneItem transformed(raw);
    const QPainterPath actual = snow_canvas_stroke_rendering::geometry(transformed).ink;
    const QRectF a = actual.boundingRect();
    const QRectF b = expected.boundingRect();
    require(std::abs(a.left() - b.left()) < 1e-5 && std::abs(a.top() - b.top()) < 1e-5 &&
                std::abs(a.right() - b.right()) < 1e-5 && std::abs(a.bottom() - b.bottom()) < 1e-5,
            "translation and rotation must preserve local seeded paint bounds");
    for (double y = -50.37; y < 50; y += 1.29)
        for (double x = -110.21; x < 110; x += 1.73)
            require(actual.contains(QPointF(x, y)) == expected.contains(QPointF(x, y)),
                    "translation and rotation must not reseed or reshape local sketch ink");
}

void runtimeArrowRecolourAndOpacityRetainInk() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(320, 220);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0, 0, 1) && canvas.setCanvasTool(SnowCanvasTool::Arrow),
            "create runtime arrow cache fixture");
    SnowCanvasShapeStyle style;
    style.strokeRenderingStyle = SnowCanvasStrokeRenderingStyle::Cartoonist;
    style.stroke = QColor(20, 50, 90);
    style.strokeWidth = 2;
    style.endArrowhead = SnowCanvasArrowhead::CircleOutline;
    require(canvas.setCanvasShapeStylePatch(style,
                                            SnowCanvasShapeStylePropertyStrokeRenderingStyle |
                                                SnowCanvasShapeStylePropertyStrokeColor |
                                                SnowCanvasShapeStylePropertyStrokeWidth |
                                                SnowCanvasShapeStylePropertyEndArrowhead,
                                            SnowCanvasShapeKind::Arrow),
            "set runtime sketch arrow style");
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    mouse(QEvent::MouseButtonPress, {60, 110}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, {260, 110}, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {260, 110}, Qt::LeftButton, Qt::NoButton);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "retain the selected runtime arrow");
    mouse(QEvent::MouseButtonPress, {160, 110}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {160, 110}, Qt::LeftButton, Qt::NoButton);
    const auto handle = snow_canvas_runtime::Access::handle(runtime);
    SnowCanvasViewport viewport;
    require(viewport.create(handle, snow_canvas_viewport::defaultEngineConfig()) &&
                snow_viewport_set_surface_size(handle, viewport.get(), 320, 220) == SNOW_OK &&
                snow_viewport_set_camera(handle, viewport.get(), 0, 0, 1) == SNOW_OK,
            "create independently retained runtime viewport");
    SnowCanvasDisplayCache cache;
    require(cache.sync(handle, viewport.get()) && cache.sceneItemCount() == 1,
            "synchronize the committed runtime arrow");
    const QImage original = render(cache.sceneItems()[0]);
    require(!cache.sceneItems()[0].pathChunks().empty(), "runtime arrow uses retained path chunks");
    const auto cacheId = cache.sceneItems()[0].pathChunks()[0].cacheId;
    const auto revision = cache.sceneItems()[0].strokeRenderingGeometryRevision();
    snow_canvas_stroke_rendering::takeDiagnostics();
    style.stroke = QColor(230, 40, 60);
    require(canvas.setCanvasShapeStylePatch(style, SnowCanvasShapeStylePropertyStrokeColor,
                                            SnowCanvasShapeKind::Arrow) &&
                cache.sync(handle, viewport.get()),
            "recolour runtime selected arrow");
    require(render(cache.sceneItems()[0]) != original, "runtime arrow recolouring changes output");
    require(canvas.setSelectedOpacity(0.4), "change runtime selected arrow opacity");
    require(cache.sync(handle, viewport.get()), "synchronize runtime arrow opacity patch");
    render(cache.sceneItems()[0]);
    const auto diagnostics = snow_canvas_stroke_rendering::takeDiagnostics();
    require(cache.sceneItems()[0].pathChunks()[0].cacheId == cacheId &&
                cache.sceneItems()[0].strokeRenderingGeometryRevision() == revision &&
                diagnostics.geometryBuilds == 0 && diagnostics.strokeBuilds == 0 &&
                diagnostics.coverageUnions == 0 && diagnostics.cacheAllocations == 0,
            "real Rust colour/opacity reset patches must retain native chunks and sketch ink");

    style.arrowOutlineWidth = 8;
    style.arrowOutlineColor = QColor(12, 160, 240, 128);
    require(canvas.setCanvasShapeStylePatch(style,
                                            SnowCanvasShapeStylePropertyArrowOutlineWidth |
                                                SnowCanvasShapeStylePropertyArrowOutlineColor,
                                            SnowCanvasShapeKind::Arrow) &&
                cache.sync(handle, viewport.get()),
            "enable outline on a retained arrow");
    const auto outlined = render(cache.sceneItems()[0]);
    snow_canvas_stroke_rendering::takeDiagnostics();
    const auto outlinedRevision = cache.sceneItems()[0].strokeRenderingGeometryRevision();
    const auto outlinedChunk = cache.sceneItems()[0].pathChunks()[0].cacheId;
    style.arrowOutlineColor = QColor(240, 180, 12, 128);
    require(canvas.setCanvasShapeStylePatch(style, SnowCanvasShapeStylePropertyArrowOutlineColor,
                                            SnowCanvasShapeKind::Arrow) &&
                cache.sync(handle, viewport.get()),
            "recolour an outlined arrow through Rust and the retained display cache");
    require(render(cache.sceneItems()[0]) != outlined,
            "outline recolour updates the exported pixels");
    require(cache.sceneItems()[0].strokeRenderingGeometryRevision() == outlinedRevision,
            "outline colour changes preserve native geometry revisions");
    require(cache.sceneItems()[0].pathChunks()[0].cacheId == outlinedChunk,
            "outline colour changes preserve retained path chunks");
    require(snow_canvas_stroke_rendering::takeDiagnostics().geometryBuilds == 0,
            "outline colour changes reuse cached coverage geometry");

    const QPainterPath before = snow_canvas_stroke_rendering::geometry(cache.sceneItems()[0]).ink;
    const auto rotation = snow_canvas_stroke_rendering::localToCanvas(0, 0, 0.37);
    const auto& source = cache.sceneItems()[0];
    require(source.center_x == 0 && source.center_y == 0 && source.rotation == 0,
            "runtime arrow display geometry is expressed in global coordinates");
    std::vector<SnowArrowPoint> rotatedPoints(source.arrow_points,
                                              source.arrow_points + source.arrow_point_count);
    for (auto& point : rotatedPoints) {
        const QPointF moved = rotation.map(QPointF(point.x, point.y));
        point = {moved.x(), moved.y()};
    }
    std::vector<SnowArrowheadPrimitive> rotatedHeads(source.arrowhead_primitives,
                                                     source.arrowhead_primitives +
                                                         source.arrowhead_primitive_count);
    for (auto& primitive : rotatedHeads) {
        const QPointF center = rotation.map(QPointF(primitive.center.x, primitive.center.y));
        primitive.center = {center.x(), center.y()};
        for (std::uint32_t i = 0; i < primitive.point_count; ++i) {
            const QPointF moved =
                rotation.map(QPointF(primitive.points[i].x, primitive.points[i].y));
            primitive.points[i] = {moved.x(), moved.y()};
        }
    }
    SnowSceneDisplayItem rotatedSource = source;
    rotatedSource.arrow_points = rotatedPoints.data();
    rotatedSource.arrow_path_commands = nullptr;
    rotatedSource.arrow_path_command_count = 0;
    rotatedSource.arrowhead_primitives = rotatedHeads.data();
    SnowCanvasSceneItem rotatedItem(rotatedSource);
    const QPainterPath after =
        rotation.inverted().map(snow_canvas_stroke_rendering::geometry(rotatedItem).ink);
    QPainterPathStroker boundaryStroker;
    boundaryStroker.setWidth(0.1);
    const QPainterPath numericalBoundary = boundaryStroker.createStroke(before);
    int boundaryDifferences = 0;
    for (double y = -20.37; y < 20; y += 1.29)
        for (double x = -110.21; x < 120; x += 1.73) {
            const QPointF point(x, y);
            if (before.contains(point) != after.contains(point)) {
                ++boundaryDifferences;
                require(numericalBoundary.contains(point),
                        "runtime circular head rotation error must stay below 0.05 canvas pixels");
            }
        }
    if (boundaryDifferences != 0)
        std::printf("circle rotation boundary differences=%d maximum_error_px<=0.05\n",
                    boundaryDifferences);
}

void largeEllipsesRespectTheAbsoluteExcursion() {
    for (auto style :
         {SNOW_STROKE_RENDERING_STYLE_ARTIST, SNOW_STROKE_RENDERING_STYLE_CARTOONIST}) {
        for (const QSizeF size : {QSizeF(20000, 20000), QSizeF(20000, 200)}) {
            auto item = rectangle(style, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE);
            item.width = size.width();
            item.height = size.height();
            item.rebuildLocalRectangleGeometry();
            const auto& geometry = snow_canvas_stroke_rendering::geometry(item);
            QPainterPathStroker guard;
            guard.setWidth(
                item.stroke_width +
                2 * snow_stroke_rendering_outset(style, std::min(item.width, item.height)) + 0.2);
            const QPainterPath permitted = guard.createStroke(geometry.originalFill);
            for (const auto& polygon : geometry.ink.toSubpathPolygons()) {
                const qsizetype stride = std::max<qsizetype>(1, polygon.size() / 128);
                for (qsizetype i = 0; i < polygon.size(); i += stride)
                    require(
                        permitted.contains(polygon[i]),
                        "large/eccentric ellipses must not accumulate size-dependent curve error");
            }
        }
    }
}

void closedTaperedContoursStillReceiveStyling() {
    for (double endpointOffset : {0.0, 0.001}) {
        const std::array<SnowArrowPoint, 4> points{
            {{-80, 0}, {-25, -60}, {75, 0}, {-80 + endpointOffset, 0}}};
        const std::array<SnowArrowPoint, 7> outline{
            {{-80, 0}, {-25, -64}, {75, -4}, {75, 4}, {-25, -56}, {-80, 0}, {-80, 0}}};
        std::array<SnowArrowPathCommand, 7> commands{};
        QPainterPath baseline;
        baseline.setFillRule(Qt::WindingFill);
        baseline.moveTo(outline[0].x, outline[0].y);
        for (std::size_t i = 0; i < commands.size(); ++i) {
            commands[i].kind =
                i == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
            commands[i].point = outline[i];
            if (i != 0)
                baseline.lineTo(outline[i].x, outline[i].y);
        }
        for (auto style :
             {SNOW_STROKE_RENDERING_STYLE_ARTIST, SNOW_STROKE_RENDERING_STYLE_CARTOONIST}) {
            SnowSceneDisplayItem raw{};
            raw.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
            raw.stroke = {20, 50, 90, 255};
            raw.stroke_width = 2;
            raw.opacity = 1;
            raw.stroke_rendering_style = style;
            raw.stroke_rendering_seed = 142857;
            raw.arrow_shaft_type = SNOW_ARROW_SHAFT_TYPE_TAPERED;
            raw.arrow_end_head = SNOW_ARROWHEAD_TRIANGLE;
            raw.arrow_points = points.data();
            raw.arrow_point_count = static_cast<std::uint32_t>(points.size());
            raw.arrow_path_commands = commands.data();
            raw.arrow_path_command_count = static_cast<std::uint32_t>(commands.size());
            SnowCanvasSceneItem item(raw);
            const QPainterPath expected = snow_canvas_stroke_rendering::geometry(item).ink;
            require(
                expected != baseline,
                "closed and nearly closed tapered paths must retain their selected stroke style");
            const auto rotation = snow_canvas_stroke_rendering::localToCanvas(0, 0, 0.73);
            auto rotatedPoints = points;
            for (auto& point : rotatedPoints) {
                const QPointF moved = rotation.map(QPointF(point.x, point.y));
                point = {moved.x(), moved.y()};
            }
            auto rotatedCommands = commands;
            for (auto& command : rotatedCommands) {
                const QPointF moved = rotation.map(QPointF(command.point.x, command.point.y));
                command.point = {moved.x(), moved.y()};
            }
            raw.arrow_points = rotatedPoints.data();
            raw.arrow_path_commands = rotatedCommands.data();
            SnowCanvasSceneItem rotatedItem(raw);
            const QPainterPath actual =
                rotation.inverted().map(snow_canvas_stroke_rendering::geometry(rotatedItem).ink);
            require(actual.elementCount() == expected.elementCount(),
                    "rotating a closed tapered path must preserve its contour topology");
            for (int i = 0; i < actual.elementCount(); ++i) {
                const auto a = actual.elementAt(i);
                const auto b = expected.elementAt(i);
                require(QLineF(QPointF(a.x, a.y), QPointF(b.x, b.y)).length() < 1e-5,
                        "closed tapered styling must rotate with its first nonzero tangent");
            }
        }
    }
}

void headerFillChangesRefreshAggregateWithoutReplacingGeometry() {
    std::array<SnowArrowPathCommand, 5> commands{};
    const std::array<SnowArrowPoint, 5> points{
        {{-40, -40}, {40, -40}, {40, 40}, {-40, 40}, {-40, -40}}};
    for (std::size_t i = 0; i < commands.size(); ++i) {
        commands[i].kind =
            i == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
        commands[i].point = points[i];
    }
    SnowPathChunk chunk{};
    chunk.command_count = static_cast<std::uint32_t>(commands.size());
    chunk.min_x = chunk.min_y = -40;
    chunk.max_x = chunk.max_y = 40;
    SnowPathChunkRange range{};
    range.insert_chunk_count = 1;
    for (const auto kind : {SNOW_SCENE_DISPLAY_ITEM_ARROW, SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE}) {
        SnowCanvasSceneItem item;
        item.kind = kind;
        item.stroke_rendering_style = SNOW_STROKE_RENDERING_STYLE_ARTIST;
        item.stroke_width = 2;
        item.fill_style = SNOW_FILL_STYLE_BORDER;
        require(item.applyPathGeometryPatch(0, 1, &range, 1, &chunk, 1, commands.data(),
                                            static_cast<std::uint32_t>(commands.size()), true,
                                            true),
                "closed geometry accepts the initial unfilled path");
        const auto cacheId = item.pathChunks()[0].cacheId;
        const auto contentRevision = item.strokeRenderingGeometryRevision();
        struct FillCase {
            SnowFillStyle style;
            std::uint8_t alpha;
        };
        for (const FillCase fill :
             {FillCase{SNOW_FILL_STYLE_SOLID, 255}, FillCase{SNOW_FILL_STYLE_SOLID, 0},
              FillCase{SNOW_FILL_STYLE_SOLID, 255}, FillCase{SNOW_FILL_STYLE_BORDER, 255},
              FillCase{SNOW_FILL_STYLE_SOLID, 255}}) {
            SnowSceneDisplayItem header = item;
            header.fill_style = fill.style;
            header.fill = {255, 204, 199, fill.alpha};
            item = header;
            const bool visible = fill.style != SNOW_FILL_STYLE_BORDER && fill.alpha != 0;
            require(item.aggregateClosedPath().isEmpty() ==
                        (!visible && kind != SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE),
                    "header-only fill visibility updates aggregate path ownership");
            require(item.pathChunks()[0].cacheId == cacheId &&
                        item.strokeRenderingGeometryRevision() == contentRevision,
                    "fill visibility preserves retained centerline and styled geometry");
        }
    }
}

void appendedStrokeCoveragePreservesUnionAndHoles() {
    std::array<QPainterPath, 4> sources;
    sources[0].moveTo(-80, -30);
    sources[0].cubicTo(-25, 50, 30, -60, 80, 30);
    sources[1].addEllipse(QRectF(-75, -40, 150, 80));
    sources[2].addRoundedRect(QRectF(-75, -40, 150, 80), 18, 18);
    sources[3].moveTo(-75, -40);
    sources[3].lineTo(75, 40);
    sources[3].lineTo(75, -40);
    sources[3].lineTo(-75, 40);
    sources[3].closeSubpath();
    for (const auto& source : sources) {
        for (auto pattern : {Qt::SolidLine, Qt::DashLine, Qt::DotLine}) {
            QPainterPathStroker stroker;
            stroker.setWidth(4);
            stroker.setCapStyle(Qt::RoundCap);
            stroker.setJoinStyle(Qt::RoundJoin);
            stroker.setDashPattern(pattern);
            const QPainterPath first = stroker.createStroke(source);
            const QPainterPath second = stroker.createStroke(source.toReversed());
            QPainterPath actual = first;
            actual.addPath(second);
            actual.setFillRule(Qt::WindingFill);
            const QPainterPath expected = first.united(second);
            int ink = 0;
            int disagreements = 0;
            for (double y = -49.37; y < 50; y += 1.63)
                for (double x = -89.21; x < 90; x += 1.71) {
                    const QPointF p(x, y);
                    const bool covered = expected.contains(p);
                    ink += covered ? 1 : 0;
                    disagreements += covered != actual.contains(p) ? 1 : 0;
                }
            // Boolean unions flatten curves; allow their subpixel boundary
            // approximation while rejecting cancellation or filled inner holes.
            require(ink > 100 && disagreements < std::max(3, ink / 20),
                    "appended normalized stroke coverage must preserve unions and inner holes");
        }
    }
}

void writeMatrix(const QString& output, double dpr = 1, bool transparent = false) {
    constexpr int columnWidth = 320;
    constexpr int rowHeight = 150;
    const QStringList names{QStringLiteral("Architect"), QStringLiteral("Artist"),
                            QStringLiteral("Cartoonist")};
    const QStringList rows{QStringLiteral("Rectangle"),     QStringLiteral("Rounded rectangle"),
                           QStringLiteral("Ellipse"),       QStringLiteral("Diamond"),
                           QStringLiteral("Straight line"), QStringLiteral("Arrow"),
                           QStringLiteral("Curve"),         QStringLiteral("Elbow"),
                           QStringLiteral("Dashed"),        QStringLiteral("Dotted")};
    const QSize logicalSize(columnWidth * 3, rowHeight * static_cast<int>(rows.size()) + 55);
    QImage matrix(logicalSize * dpr, QImage::Format_ARGB32_Premultiplied);
    matrix.setDevicePixelRatio(dpr);
    matrix.fill(transparent ? QColor(Qt::transparent) : QColor(249, 250, 252));
    QPainter painter(&matrix);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 11));
    for (int column = 0; column < 3; ++column) {
        painter.setPen(QColor(30, 40, 60));
        painter.drawText(QRect(column * columnWidth, 0, columnWidth, 40), Qt::AlignCenter,
                         names[column]);
        for (int row = 0; row < rows.size(); ++row) {
            const auto style = static_cast<SnowStrokeRenderingStyle>(column);
            SnowCanvasSceneItem item;
            if (row < 4)
                item = rectangle(style, row == 2   ? SNOW_DISPLAY_RECT_SHAPE_ELLIPSE
                                        : row == 3 ? SNOW_DISPLAY_RECT_SHAPE_DIAMOND
                                                   : SNOW_DISPLAY_RECT_SHAPE_RECTANGLE);
            else
                item = arrow(style,
                             row == 6   ? SNOW_ARROW_TYPE_CURVE
                             : row == 7 ? SNOW_ARROW_TYPE_ELBOW
                                        : SNOW_ARROW_TYPE_STRAIGHT,
                             row == 4 ? SNOW_ARROWHEAD_NONE : SNOW_ARROWHEAD_ARROW);
            if (row == 1)
                item.corner_radii = {18, 18, 18, 18};
            if (row == 8)
                item.arrow_stroke_style = SNOW_STROKE_STYLE_DASHED;
            if (row == 9)
                item.arrow_stroke_style = SNOW_STROKE_STYLE_DOTTED;
            if (row < 4)
                item.fill = {222, 235, 250, 255};
            item.rebuildLocalRectangleGeometry();
            const QImage cell = render(item, 1, dpr);
            painter.drawImage(QPoint(column * columnWidth, 45 + row * rowHeight - 35), cell);
            painter.setPen(QColor(90, 100, 120));
            painter.drawText(
                QRect(column * columnWidth, 45 + row * rowHeight + 122, columnWidth, 25),
                Qt::AlignCenter, rows[row]);
        }
    }
    painter.end();
    require(matrix.save(output), "save representative stroke rendering matrix");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
    if (application.arguments().contains(QStringLiteral("--public-coverage-only"))) {
        publicStrokeCoverageMatchesCanvasLines();
        return 0;
    }
    publicStrokeCoverageMatchesCanvasLines();
    if (application.arguments().contains(QStringLiteral("--arrow-outline-only"))) {
        arrowOutlinesCoverGeometryAndPreserveDisabledPixels();
        runtimeArrowRecolourAndOpacityRetainInk();
        retainedOverlayMatchesSceneArrow();
        return 0;
    }
    arrowOutlinesCoverGeometryAndPreserveDisabledPixels();
    architectKeepsTheOriginalFastPath();
    stylesAreStableAndDistinct();
    speechBubbleFillsKeepCanvasCoordinates();
    speechBubbleRenderingStylesPreserveDashAndDotMetrics();
    speechBubbleContoursRetainFillsPatternsOpacityAndCaches();
    serialNumberStrokeTypesOnlyChangeOutlinedRings();
    warmGeometrySurvivesColourAndOpacityChanges();
    translucentInkReceivesOpacityOnce();
    patternsUseOneTraceAndFillsStayCrisp();
    clippingAndBoundsIncludeStyledInk();
    retainedOverlayMatchesSceneArrow();
    arrowLabelsKeepTransparentExclusion();
    taperedContoursKeepOneSilhouetteAndFixedTips();
    allArrowheadPrimitivesRespectCaps();
    chunkBoundariesPreservePatternPhase();
    translationAndRotationPreserveSeededGeometry();
    runtimeArrowRecolourAndOpacityRetainInk();
    closedTaperedContoursStillReceiveStyling();
    headerFillChangesRefreshAggregateWithoutReplacingGeometry();
    appendedStrokeCoveragePreservesUnionAndHoles();
    largeEllipsesRespectTheAbsoluteExcursion();
    if (argc == 2) {
        const QString output = QString::fromLocal8Bit(argv[1]);
        writeMatrix(output);
        const QString stem = output.endsWith(QStringLiteral(".png")) ? output.chopped(4) : output;
        writeMatrix(stem + QStringLiteral("-150.png"), 1.5);
        writeMatrix(stem + QStringLiteral("-transparent.png"), 1, true);
    }
    std::puts("stroke rendering tests passed");
    return 0;
}
