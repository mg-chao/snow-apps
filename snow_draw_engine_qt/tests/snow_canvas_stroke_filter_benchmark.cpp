#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"

#include <QCoreApplication>
#include <chrono>
#include <iostream>

// Run only with a Release performance preset. Measures the real Qt/FFI bridge,
// including conversion and output copies; history is never sent back to Rust.
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using Clock = std::chrono::steady_clock;
    constexpr int samples = 16000;
    constexpr int iterations = 30;
    for (const int batchSize : {1, 16, 128}) {
        qsizetype outputCount = 0;
        const auto begin = Clock::now();
        for (int iteration = 0; iteration < iterations; ++iteration) {
            SnowCanvasStrokeFilter filter;
            filter.reset({0, 0});
            for (int i = 1; i <= samples; ++i) {
                filter.append({i * 0.1, i % 2 == 0 ? 1.0 : -1.0});
                if (i % batchSize == 0)
                    outputCount += filter.takePoints().size();
            }
            outputCount += filter.takePoints(true).size();
        }
        const double elapsed =
            std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
        std::cout << "batch_size=" << batchSize << " input_points=" << samples
                  << " output_points=" << outputCount / iterations
                  << " mean_stroke_us=" << elapsed / iterations
                  << " mean_batch_us=" << elapsed / (iterations * (samples / batchSize + 1))
                  << '\n';
    }
}
