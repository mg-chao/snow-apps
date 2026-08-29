#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace snow_shot::capture_detail {
struct AdaptiveScrollingCaptureCadenceConfig {
    int minimumFps = 1;
    int maximumFps = 30;
    int initialFps = 30;
    double capacityHeadroom = 1.25;
    double ewmaSampleWeight = 0.25;
    int recoverySamples = 4;
    std::uint32_t pressureQueueDepth = 2;
};

// Chooses the highest sustainable cadence from measured pipeline cost.
class AdaptiveScrollingCaptureCadence final {
  public:
    enum class LimitingStage {
        Warmup,
        Capture,
        Stitch,
    };

    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using Config = AdaptiveScrollingCaptureCadenceConfig;

    explicit AdaptiveScrollingCaptureCadence(
        AdaptiveScrollingCaptureCadenceConfig config = {})
        : m_config(normalizeConfig(config)), m_targetFps(m_config.initialFps) {}

    void reset() {
        m_captureCostMilliseconds = 0.0;
        m_captureLatestMilliseconds = 0.0;
        m_stitchCostMilliseconds = 0.0;
        m_stitchLatestMilliseconds = 0.0;
        m_targetFps = static_cast<double>(m_config.initialFps);
        m_recoverySampleCount = 0;
        m_lastDroppedFrames = 0;
    }

    void recordCapture(Duration duration) {
        updateCost(duration, m_captureCostMilliseconds, m_captureLatestMilliseconds);
        updateTarget();
    }

    void recordStitch(Duration duration) {
        updateCost(duration, m_stitchCostMilliseconds, m_stitchLatestMilliseconds);
        updateTarget();
    }

    void recordStreamPressure(std::uint32_t queueDepth, std::uint64_t droppedFrames) {
        const bool dropped = droppedFrames > m_lastDroppedFrames;
        m_lastDroppedFrames = droppedFrames;
        if (!dropped && queueDepth < m_config.pressureQueueDepth) {
            return;
        }
        m_recoverySampleCount = 0;
        m_targetFps = std::max(
            static_cast<double>(m_config.minimumFps),
            std::floor(std::min(m_targetFps * 0.75, sustainableFps())));
    }

    void setMaximumFps(int maximumFps) {
        m_config.maximumFps = std::clamp(maximumFps, m_config.minimumFps, kAbsoluteMaximumFps);
        m_config.initialFps = std::clamp(m_config.initialFps, m_config.minimumFps,
                                         m_config.maximumFps);
        m_targetFps = std::clamp(m_targetFps, static_cast<double>(m_config.minimumFps),
                                 static_cast<double>(m_config.maximumFps));
    }

    [[nodiscard]] int maximumFps() const {
        return m_config.maximumFps;
    }

    [[nodiscard]] LimitingStage limitingStage() const {
        const double captureCost =
            std::max(m_captureCostMilliseconds, m_captureLatestMilliseconds);
        const double stitchCost = std::max(m_stitchCostMilliseconds, m_stitchLatestMilliseconds);
        if (captureCost <= 0.0 && stitchCost <= 0.0) {
            return LimitingStage::Warmup;
        }
        return captureCost >= stitchCost ? LimitingStage::Capture : LimitingStage::Stitch;
    }

    [[nodiscard]] double fps() const {
        return m_targetFps;
    }

    [[nodiscard]] Duration period() const {
        return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(1.0 / fps()));
    }

    [[nodiscard]] double sustainableFps() const {
        const double stageCostMilliseconds =
            std::max(std::max(m_captureCostMilliseconds, m_captureLatestMilliseconds),
                     std::max(m_stitchCostMilliseconds, m_stitchLatestMilliseconds));
        if (stageCostMilliseconds <= 0.0) {
            return static_cast<double>(m_config.maximumFps);
        }
        return std::clamp(
            std::floor(1000.0 / (stageCostMilliseconds * m_config.capacityHeadroom)),
            static_cast<double>(m_config.minimumFps), static_cast<double>(m_config.maximumFps));
    }

  private:
    static constexpr int kAbsoluteMinimumFps = 1;
    static constexpr int kAbsoluteMaximumFps = 30;
    static constexpr double kDefaultCapacityHeadroom = 1.25;
    static constexpr double kDefaultEwmaSampleWeight = 0.25;

    static AdaptiveScrollingCaptureCadenceConfig normalizeConfig(
        AdaptiveScrollingCaptureCadenceConfig config) {
        config.minimumFps = std::clamp(config.minimumFps, kAbsoluteMinimumFps,
                                       kAbsoluteMaximumFps);
        config.maximumFps = std::clamp(config.maximumFps, config.minimumFps,
                                       kAbsoluteMaximumFps);
        config.initialFps = std::clamp(config.initialFps, config.minimumFps,
                                       config.maximumFps);
        if (!std::isfinite(config.capacityHeadroom) || config.capacityHeadroom < 1.0) {
            config.capacityHeadroom = kDefaultCapacityHeadroom;
        }
        if (!std::isfinite(config.ewmaSampleWeight) || config.ewmaSampleWeight <= 0.0 ||
            config.ewmaSampleWeight > 1.0) {
            config.ewmaSampleWeight = kDefaultEwmaSampleWeight;
        }
        config.recoverySamples = std::max(1, config.recoverySamples);
        config.pressureQueueDepth = std::max<std::uint32_t>(1, config.pressureQueueDepth);
        return config;
    }

    [[nodiscard]] bool hasPerformanceSample() const {
        return m_captureLatestMilliseconds > 0.0 || m_stitchLatestMilliseconds > 0.0;
    }

    static double milliseconds(Duration duration) {
        return std::max(0.0, std::chrono::duration<double, std::milli>(duration).count());
    }

    void updateCost(Duration duration, double& ewma, double& latest) {
        latest = milliseconds(duration);
        if (latest <= 0.0) {
            return;
        }
        ewma = ewma <= 0.0
                   ? latest
                   : (ewma * (1.0 - m_config.ewmaSampleWeight)) +
                         (latest * m_config.ewmaSampleWeight);
    }

    void updateTarget() {
        if (!hasPerformanceSample()) {
            return;
        }
        const double sustainable = sustainableFps();
        if (sustainable < m_targetFps) {
            m_targetFps = sustainable;
            m_recoverySampleCount = 0;
            return;
        }
        if (sustainable <= m_targetFps) {
            m_recoverySampleCount = 0;
            return;
        }
        ++m_recoverySampleCount;
        if (m_recoverySampleCount < m_config.recoverySamples) {
            return;
        }
        m_recoverySampleCount = 0;
        m_targetFps = std::min(sustainable, m_targetFps + 1.0);
    }

    AdaptiveScrollingCaptureCadenceConfig m_config;
    double m_captureCostMilliseconds = 0.0;
    double m_captureLatestMilliseconds = 0.0;
    double m_stitchCostMilliseconds = 0.0;
    double m_stitchLatestMilliseconds = 0.0;
    double m_targetFps = 30.0;
    int m_recoverySampleCount = 0;
    std::uint64_t m_lastDroppedFrames = 0;
};
} // namespace snow_shot::capture_detail
