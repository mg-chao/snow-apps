#include "snow_canvas_eraser_render_fixture.h"

#include <QApplication>
#include <cmath>

namespace {

void rectanglesRestoreOriginalPixelsAndRespectChronology() {
    RenderFixture fixture;
    const QRect erased(20, 15, 70, 65);
    fixture.items.push_back(rectangle(erased, 2));
    fixture.items.push_back(annotation({40, 35, 12, 10}, 3, {10, 190, 20, 255}));
    const auto output = fixture.render();
    for (int y = 0; y < output.height(); ++y) {
        for (int x = 0; x < output.width(); ++x) {
            const QRgb expected = QRect(40, 35, 12, 10).contains(x, y) ? qRgba(10, 190, 20, 255)
                                  : erased.contains(x, y) ? fixture.background.pixel(x, y)
                                                          : qRgba(220, 30, 90, 255);
            require(output.pixel(x, y) == expected,
                    "rectangle eraser must restore exact original RGBA beneath later ink");
        }
    }
    const auto diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(diagnostics.restorationDispatchCount == 1 && diagnostics.maskPixelCount == 0 &&
                diagnostics.restoredPixelCount == 70u * 65u,
            "aligned opaque rectangle must copy rows without constructing a mask");
}

void brushDotsAndOverlapsBlendOriginalPremultipliedChannels() {
    RenderFixture fixture({32, 32});
    const SnowArrowPoint center{16.25, 16.75};
    fixture.items.push_back(brush({center}, 9, 2));
    fixture.items.back().width = 0;
    fixture.items.back().height = 0;
    fixture.items.back().center_x = center.x;
    fixture.items.back().center_y = center.y;
    fixture.items.push_back(brush({center, center}, 9, 3));
    const auto output = fixture.render();
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const double distance = std::hypot(x + 0.5 - center.x, y + 0.5 - center.y);
            const int coverage = qRound(qBound(0.0, 5.0 - distance, 1.0) * 255);
            const QRgb original = fixture.background.pixel(x, y);
            const QRgb once = interpolate(qRgba(220, 30, 90, 255), original, coverage);
            require(output.pixel(x, y) == interpolate(once, original, coverage),
                    "brush dot must restore a round antialiased disc, including alpha");
        }
    }
    require(snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread()
                    .restorationBlendPixelCount != 0,
            "brush edges must blend coverage instead of applying SourceOver");
}

void filtersOnEitherSideReadTheCorrectBackdrop() {
    RenderFixture fixture({64, 64});
    fixture.background.fill(QColor(20, 60, 110));
    fixture.items = {rectangle({0, 0, 64, 64}, 1, 3), rectangle({8, 8, 48, 48}, 2),
                     rectangle({16, 16, 32, 32}, 3, 3)};
    const auto output = fixture.render();
    require(output.pixelColor(4, 4) == QColor(235, 195, 145),
            "earlier filter remains outside erase");
    require(output.pixelColor(12, 12) == QColor(20, 60, 110), "erase restores unfiltered source");
    require(output.pixelColor(24, 24) == QColor(235, 195, 145),
            "later filter must sample restored background");
    const auto runs =
        snow_canvas_renderer::buildUnculledRenderPlan(fixture.items.data(), 3, fixture.info, 1);
    require(runs.size() == 2 && runs[0].source_pass.index != runs[1].source_pass.index &&
                snow_canvas_renderer::validateRenderPlan(runs, fixture.items.data(), 3),
            "eraser separates ordinary filter source passes without joining a run");
}

void tiledPartialAndFractionalProjectionMatchFullRendering() {
    for (const qreal dpr : {1.0, 1.25, 2.0}) {
        RenderFixture fixture({420, 280}, dpr);
        fixture.items.push_back(rectangle({61.25, 42.5, 210.75, 120.25}, 2));
        fixture.items.push_back(brush({{91.5, 22.75}, {197.25, 179.5}, {370.75, 74.25}}, 19.5, 3));
        fixture.info.camera_center_x += 0.25;
        fixture.info.camera_center_y -= 0.125;
        fixture.info.camera_zoom = 1.125;
        const auto full = fixture.render(dpr);
        require(fixture.render(dpr, true) == full,
                "full and tiled erasing must share device-pixel origins and alpha");
        require(fixture.render(dpr, true) == full, "warm tiled erasing must preserve pixels");
        const QRegion exposed(QRect(13, 17, 300, 170));
        const auto partial = fixture.render(dpr, true, exposed);
        for (int y = 0; y < partial.height(); ++y) {
            for (int x = 0; x < partial.width(); ++x) {
                const QPoint logical(qFloor((x + 0.5) / dpr), qFloor((y + 0.5) / dpr));
                if (exposed.contains(logical) && x > qCeil(13 * dpr) && x < qFloor(313 * dpr) - 1 &&
                    y > qCeil(17 * dpr) && y < qFloor(187 * dpr) - 1) {
                    require(partial.pixel(x, y) == full.pixel(x, y),
                            "partial repaint must equal full restoration inside the exposed area");
                }
                if (!exposed.contains(logical)) {
                    require(partial.pixelColor(x, y) == QColor(7, 11, 19),
                            "partial restoration must leave unexposed pixels untouched");
                }
            }
        }
    }
}

class OriginalRenderer final : public SnowCanvasCustomRenderer {
  public:
    std::uint64_t contentRevision() const override {
        return revision;
    }
    std::uint64_t originalBackgroundRevision() const override {
        return sourceRevision;
    }
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        painter.fillRect(context.viewportRect, previewColor);
    }
    void renderOriginalBackground(QPainter& painter,
                                  const SnowCanvasRenderContext& context) override {
        ++calls;
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(context.viewportRect, color);
    }
    std::uint64_t revision = 1;
    std::uint64_t sourceRevision = 1;
    int calls = 0;
    QColor previewColor{90, 10, 230};
    QColor color{20, 90, 130, 64};
};

void pristineCachesIgnoreAnnotationsButFollowSourceIdentity() {
    RenderFixture fixture({96, 80});
    OriginalRenderer renderer;
    fixture.renderer = &renderer;
    fixture.background = {};
    fixture.items.push_back(rectangle({12, 10, 64, 50}, 2));
    const auto first = fixture.render(1, true);
    require(first.pixel(30, 30) == qPremultiply(renderer.color.rgba()),
            "restoration must use the original callback rather than effect preview pixels");
    require(renderer.calls == 1, "first erase renders one pristine tile");
    fixture.render(1, true);
    require(renderer.calls == 1 &&
                snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread().pristineTileHits ==
                    1,
            "unchanged erasers must reuse pristine source pixels");
    fixture.items[0].fill = {10, 110, 90, 255};
    snow_canvas_filter_tile_cache::invalidateRegion(&fixture, QRect(0, 0, 96, 80), 1);
    require(fixture.render(1, true).pixel(30, 30) == qPremultiply(renderer.color.rgba()) &&
                renderer.calls == 1,
            "annotation damage must not invalidate pristine source tiles");
    renderer.previewColor = QColor(30, 210, 50);
    ++renderer.revision;
    require(fixture.render(1, true).pixel(30, 30) == qPremultiply(renderer.color.rgba()) &&
                renderer.calls == 1,
            "temporary preview revision must not invalidate pristine source tiles");
    renderer.color = QColor(0, 0, 0, 0);
    ++renderer.revision;
    ++renderer.sourceRevision;
    require(fixture.render(1, true).pixel(30, 30) == 0 && renderer.calls == 2,
            "source revision must replace cached colors and transparent pixels");
    snow_canvas_filter_tile_cache::invalidateNamespace(&fixture);
    fixture.render(1, true);
    require(renderer.calls == 3, "namespace clearing must release pristine tiles");
}

void blankBackgroundsAndFailuresHaveDefinedPixels() {
    RenderFixture fixture({32, 32});
    fixture.background = {};
    fixture.items.push_back(rectangle({4, 4, 24, 24}, 2));
    require(fixture.render().pixel(8, 8) == 0,
            "blank transparent hosts restore transparent pixels");
    fixture.clearBackground = true;
    fixture.info.clear_color = {0, 0, 0, 2};
    require(fixture.render().pixel(8, 8) == qRgba(0, 0, 0, 2),
            "blank custom input baselines must retain their original alpha");
    fixture.workspace.setAllocationFailureForTests(true);
    const auto failed = fixture.render();
    require(failed.pixelColor(8, 8) == QColor(7, 11, 19),
            "allocation failure must not expose uninitialized scratch pixels");
}

void appendedBrushGeometryReusesChunksAndWarmMasks() {
    RenderFixture fixture({512, 240});
    std::vector<SnowArrowPoint> points;
    for (int i = 0; i < 4096; ++i) {
        points.push_back({20 + i * 0.11, 120 + std::sin(i * 0.008) * 80});
    }
    fixture.items.push_back(brush(points, 13, 2));
    fixture.render();
    fixture.render();
    auto diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(diagnostics.penGeometryChunkBuildCount == 0 && diagnostics.penRasterizedTileCount == 0,
            "finalized brush repaint must reuse immutable chunks and masks");
    const std::uint32_t prefix = static_cast<std::uint32_t>(points.size());
    for (int i = 4096; i < 4104; ++i) {
        points.push_back({20 + i * 0.11, 120 + std::sin(i * 0.008) * 80});
    }
    require(fixture.items.back().applyPenFilterGeometryPatch(0, 1, prefix, points.data() + prefix,
                                                             8, false),
            "append brush geometry");
    const QRectF changed(460, 25, 30, 190);
    fixture.atlas.invalidate({{2, 1}, 0, 1, changed, changed, false, false});
    const auto appended = fixture.render();
    diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(diagnostics.penGeometryChunkBuildCount <= 2 &&
                diagnostics.penGeometryChunkReuseCount >= 63,
            "eight appended points must rebuild only the geometry tail");
    RenderFixture fresh({512, 240});
    fresh.items.push_back(brush(points, 13, 2));
    require(appended == fresh.render(),
            "incremental restoration must equal fresh complete geometry");
    require(fixture.atlas.retainedBytes() <= fixture.atlas.byteBudget(),
            "brush eraser masks must stay within the established atlas budget");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    rectanglesRestoreOriginalPixelsAndRespectChronology();
    brushDotsAndOverlapsBlendOriginalPremultipliedChannels();
    filtersOnEitherSideReadTheCorrectBackdrop();
    tiledPartialAndFractionalProjectionMatchFullRendering();
    pristineCachesIgnoreAnnotationsButFollowSourceIdentity();
    blankBackgroundsAndFailuresHaveDefinedPixels();
    appendedBrushGeometryReusesChunksAndWarmMasks();
    return 0;
}
