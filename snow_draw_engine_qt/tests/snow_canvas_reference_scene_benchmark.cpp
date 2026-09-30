#include "snow_canvas_reference_scene_fixture.h"

#include <QElapsedTimer>

namespace {
void benchmark() {
    Fixture fixture({1024, 640}, {0, 0, 1024, 640});
    fixture.filter(SnowCanvasFilterType::GaussianBlur);
    fixture.filter(SnowCanvasFilterType::Mosaic, true);
    for (bool enabled : {false, true}) {
        fixture.renderer.enabled = enabled;
        fixture.zoom(1.0);
        fixture.canvas.clearRenderState();
        fixture.render();
        std::vector<double> samples;
        std::size_t dispatches = 0;
        for (int frame = 0; frame < 44; ++frame) {
            QElapsedTimer timer;
            timer.start();
            fixture.zoom(frame % 2 == 0 ? 0.7 : 1.3);
            fixture.render();
            const double ms = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
            if (frame >= 4) {
                samples.push_back(ms);
                dispatches += snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread()
                                  .effectDispatchCount;
            }
        }
        std::sort(samples.begin(), samples.end());
        std::cout << (enabled ? "reference" : "viewport") << ": p50=" << samples[samples.size() / 2]
                  << "ms p95=" << samples[samples.size() * 95 / 100]
                  << "ms filter_dispatches=" << dispatches << '\n';
        if (enabled)
            require(dispatches == 0, "reference zoom benchmark must dispatch no filter effects");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    benchmark();
    return 0;
}
