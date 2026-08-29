#include "presentation/capture/adaptivescrollingcapturecadence.h"

#include <cstdlib>
#include <iostream>

namespace {
using Cadence = snow_shot::capture_detail::AdaptiveScrollingCaptureCadence;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void startsAtTheWarmupRate() {
    Cadence cadence;
    cadence.reset();
    require(cadence.fps() > 29.9 && cadence.fps() < 30.1,
            "cadence must start at the 30 fps warm-up rate");
}

void ratesNeverExceedTheConfiguredBounds() {
    Cadence cadence;
    cadence.reset();
    cadence.recordCapture(5ms);
    cadence.recordStitch(5ms);
    require(cadence.fps() <= 30.0, "cadence must never exceed 30 fps");
    require(cadence.fps() >= 29.9, "fast healthy work should reach the 30 fps cap");

    cadence.recordCapture(2s);
    require(cadence.fps() >= 1.0, "cadence must never fall below 1 fps");
    require(cadence.fps() <= 1.01, "slow capture must clamp to the 1 fps floor");
}

void customMaximumFpsIsHonored() {
    Cadence cadence(Cadence::Config{});
    cadence.setMaximumFps(30);
    require(cadence.maximumFps() == 30, "custom maximum FPS must be retained");
    cadence.recordCapture(5ms);
    cadence.recordStitch(5ms);
    require(cadence.fps() >= 29.9 && cadence.fps() <= 30.0,
            "healthy work must reach the custom FPS ceiling");
}

void invalidConfigurationFallsBackToSafeBounds() {
    Cadence::Config config;
    config.minimumFps = 0;
    config.maximumFps = -10;
    config.initialFps = 0;
    config.capacityHeadroom = 0.0;
    config.ewmaSampleWeight = 2.0;
    Cadence cadence(config);
    require(cadence.maximumFps() >= 1, "invalid FPS configuration must retain a positive cap");
    require(cadence.fps() >= 1.0 && cadence.fps() <= cadence.maximumFps(),
            "invalid initial FPS configuration must be normalized");
    require(cadence.period() > Cadence::Duration::zero(),
            "normalized cadence must always have a positive period");
}

void slowerPipelineStageSetsTheSustainableRate() {
    Cadence cadence;
    cadence.reset();
    cadence.recordCapture(200ms);
    cadence.recordStitch(40ms);
    require(cadence.fps() > 3.9 && cadence.fps() < 4.1,
            "a 200 ms capture stage with headroom should cap at 4 fps");

    cadence.recordStitch(400ms);
    require(cadence.fps() > 1.9 && cadence.fps() < 2.1,
            "the slower stitch stage should become the throughput ceiling");
    require(cadence.limitingStage() == Cadence::LimitingStage::Stitch,
            "the stitch stage should be reported as the throughput limiter");
}

void fasterSamplesRecoverCapacityGradually() {
    Cadence cadence;
    cadence.reset();
    cadence.recordCapture(200ms);
    require(cadence.fps() > 3.9 && cadence.fps() < 4.1, "slow work must lower rate");

    for (int sample = 0; sample < 120; ++sample) {
        cadence.recordCapture(5ms);
    }
    require(cadence.fps() >= 29.0,
            "sustained faster samples must recover the available machine capacity");
}

void scrollingCadenceHasAThirtyFpsHardCap() {
    Cadence::Config config;
    config.maximumFps = 120;
    config.initialFps = 120;
    Cadence cadence(config);
    require(cadence.maximumFps() == 30,
            "scrolling capture must enforce a 30 fps hard ceiling");
    require(cadence.fps() <= 30.0,
            "scrolling capture must never start above the 30 fps ceiling");
}

void streamPressureReducesRateImmediately() {
    Cadence cadence;
    cadence.recordStitch(5ms);
    require(cadence.fps() >= 29.9, "healthy stitching should begin at the configured cap");
    cadence.recordStreamPressure(2, 0);
    require(cadence.fps() < 23.0, "queue pressure must reduce the target immediately");
}

void droppedFramesReduceRateImmediately() {
    Cadence cadence;
    cadence.recordStitch(5ms);
    cadence.recordStreamPressure(0, 3);
    require(cadence.fps() < 23.0, "dropped frames must reduce the target immediately");
}

void resetDiscardsPreviousSessionState() {
    Cadence cadence;
    cadence.reset();
    cadence.recordCapture(1s);
    cadence.recordStitch(1s);
    cadence.reset();
    require(cadence.fps() > 29.9 && cadence.fps() < 30.1,
            "a new session must discard prior performance measurements");
}
} // namespace

int main() {
    startsAtTheWarmupRate();
    ratesNeverExceedTheConfiguredBounds();
    customMaximumFpsIsHonored();
    scrollingCadenceHasAThirtyFpsHardCap();
    invalidConfigurationFallsBackToSafeBounds();
    slowerPipelineStageSetsTheSustainableRate();
    fasterSamplesRecoverCapacityGradually();
    streamPressureReducesRateImmediately();
    droppedFramesReduceRateImmediately();
    resetDiscardsPreviousSessionState();
    return 0;
}
