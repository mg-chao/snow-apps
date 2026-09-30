#include "snow_canvas_reference_scene_fixture.h"

namespace {
QImage scaledReference(const Fixture& fixture, const QImage& reference, double dpr = 1.0,
                       const QRegion& exposed = {}) {
    QImage output(QSize(qCeil(fixture.canvas.width() * dpr), qCeil(fixture.canvas.height() * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(dpr);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!exposed.isEmpty())
        painter.setClipRegion(exposed);
    const QRectF target =
        fixture.canvas.canvasToViewTransform().mapRect(fixture.renderer.reference.canvasRect);
    const QRectF deviceTarget = painter.deviceTransform().mapRect(target);
    const QSize deviceSize(qRound(deviceTarget.width()), qRound(deviceTarget.height()));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, deviceSize != reference.size());
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(target, reference);
    return output;
}

void requireReuse(const snow_canvas_renderer::FilterRenderDiagnostics& diagnostics) {
    require(diagnostics.referenceSceneHits == 1 && diagnostics.referenceSceneBuildCount == 0,
            "zoom must reuse one reference surface");
    require(diagnostics.effectDispatchCount == 0 && diagnostics.executionPlanBuildCount == 0 &&
                diagnostics.penRasterizedTileCount == 0 && diagnostics.sourceTileMisses == 0 &&
                diagnostics.sourceTileEvictions == 0 && diagnostics.totalWorkingPixelCount == 0,
            "zoom must do no filter, mask, tile-cache or execution-plan work");
}

void filtersScaleTheirReferencePixels() {
    for (const auto type : {SnowCanvasFilterType::Mosaic, SnowCanvasFilterType::GaussianBlur,
                            SnowCanvasFilterType::Grayscale, SnowCanvasFilterType::Inversion,
                            SnowCanvasFilterType::Emboss, SnowCanvasFilterType::Brightness}) {
        for (bool pen : {false, true}) {
            Fixture fixture;
            fixture.filter(type, pen);
            fixture.renderer.enabled = false;
            const QImage reference = fixture.render();
            fixture.renderer.enabled = true;
            // The first fast-path frame can arrive while the window is already reduced.
            fixture.zoom(0.5);
            require(fixture.render() == scaledReference(fixture, reference),
                    "first reduced frame must scale the 100% filter pixels");
            auto diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
            require(diagnostics.referenceSceneBuildCount == 1,
                    "first filtered frame builds exactly one reference surface");
            const int backgroundCalls = fixture.renderer.beforeCalls;
            for (const auto [zoom, dpr] :
                 {std::pair{1.0, 1.0}, {2.0, 1.0}, {0.75, 1.25}, {0.5, 2.0}, {1.4, 1.0}}) {
                fixture.zoom(zoom);
                const auto actual = fixture.render(dpr);
                const auto expected = scaledReference(fixture, reference, dpr);
                require(actual == expected,
                        "zoom and DPR changes must only resample the reference pixels");
                diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
                requireReuse(diagnostics);
                require(diagnostics.retainedReferenceSceneBytes == 160u * 120u * 4u,
                        "every scale retains only the same reference image");
            }
            require(fixture.renderer.beforeCalls == backgroundCalls,
                    "zoom must not replay the backdrop renderer");
            fixture.zoom(1.0, {95, 0});
            fixture.render();
            requireReuse(snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread());
            fixture.zoom(1.0);
            require(fixture.render() == reference, "panning must preserve culled filter sources");
            requireReuse(snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread());
        }
    }
}

void sourceOrderingAndRetinaReferenceStayStable() {
    Fixture fixture({320, 240}, {20.25, -10.5, 160, 120});
    fixture.filter(SnowCanvasFilterType::Inversion, false, 1.0);
    fixture.draw(SnowCanvasTool::Shape, {250, 30}, {290, 75});
    fixture.filter(SnowCanvasFilterType::GaussianBlur, true);
    fixture.filter(SnowCanvasFilterType::Mosaic);
    fixture.renderer.enabled = false;
    const auto reference = fixture.render();
    fixture.renderer.enabled = true;
    require(fixture.render() == reference,
            "reference rendering preserves explicit passes, annotations, pen masks and density");
    fixture.zoom(0.5);
    require(fixture.render(2.0) == scaledReference(fixture, reference, 2.0),
            "Retina density must stay fixed when the live DPR changes");
    requireReuse(snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread());
}

void contentChangesReplaceTheSurface() {
    Fixture fixture;
    fixture.filter(SnowCanvasFilterType::Inversion);
    const auto original = fixture.render();
    fixture.renderer.image.fill(QColor(90, 150, 210));
    ++fixture.renderer.revision;
    const auto replaced = fixture.render();
    require(replaced != original, "same-size background replacement updates filter pixels");
    require(
        snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread().referenceSceneBuildCount ==
            1,
        "background revision replaces the retained image");
    fixture.filter(SnowCanvasFilterType::Grayscale);
    const auto edited = fixture.render();
    require(edited != replaced, "filter edits replace the retained image");
    require(fixture.canvas.undo(), "undo filter edit");
    require(fixture.render() == replaced, "undo restores the reference pixels");
    require(fixture.canvas.redo(), "redo filter edit");
    require(fixture.render() == edited, "redo restores the edited reference pixels");
    fixture.canvas.clearRenderState();
    require(fixture.render() == edited, "cache clearing preserves reference pixels");
    require(
        snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread().referenceSceneBuildCount ==
            1,
        "explicit cache clearing rebuilds exactly once");
    const auto session = fixture.runtime.serializeDocumentSession();
    require(fixture.runtime.restoreDocumentSession(session), "replace runtime with saved session");
    fixture.zoom(1.0);
    require(fixture.render() == edited,
            "runtime replacement releases and rebuilds reference viewport");
    require(fixture.canvas.deleteAllElements(), "delete filters");
    require(fixture.render() == fixture.renderer.image,
            "filter-free scene returns to direct background rendering");
    require(snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread()
                    .retainedReferenceSceneBytes == 0,
            "removing the last filter releases the reference surface");
}

void partialPaintsPreserveAlphaAndUnexposedPixels() {
    Fixture fixture;
    {
        QPainter painter(&fixture.renderer.image);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(QRect(30, 20, 50, 40), Qt::transparent);
    }
    ++fixture.renderer.revision;
    fixture.filter(SnowCanvasFilterType::GaussianBlur, false, 0.5);
    fixture.renderer.enabled = false;
    const auto reference = fixture.render();
    fixture.renderer.enabled = true;
    require(fixture.render() == reference, "transparent filter backdrop matches 100% output");
    // QWidget::render places source-region bounds at targetOffset; use a region
    // starting at the origin to retain the same coordinates as normal paint events.
    const QRegion exposed = QRegion(QRect(0, 0, 40, 30)) | QRegion(QRect(70, 40, 12, 10));
    for (const double zoom : {0.75, 1.5}) {
        fixture.zoom(zoom);
        require(fixture.render(1.25, exposed) == scaledReference(fixture, reference, 1.25, exposed),
                "partial paints must scale reference alpha and leave unexposed pixels untouched");
        requireReuse(snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread());
    }
}

void changingTheReferenceGridRebuildsOnce() {
    Fixture fixture;
    fixture.filter(SnowCanvasFilterType::GaussianBlur);
    fixture.render();
    fixture.renderer.reference.pixelsPerCanvasUnit = 2;
    fixture.zoom(1.0);
    fixture.renderer.enabled = false;
    const auto expected = fixture.render();
    fixture.renderer.enabled = true;
    require(fixture.render() == expected,
            "a changed source density must rebuild filters on the new reference grid");
    require(
        snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread().referenceSceneBuildCount ==
            1,
        "new reference grid must build exactly once");
    fixture.zoom(0.5);
    require(fixture.render() == scaledReference(fixture, expected),
            "zoom must use the replacement reference grid");
    requireReuse(snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread());
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    filtersScaleTheirReferencePixels();
    sourceOrderingAndRetinaReferenceStayStable();
    contentChangesReplaceTheSurface();
    partialPaintsPreserveAlphaAndUnexposedPixels();
    changingTheReferenceGridRebuildsOnce();
    return 0;
}
