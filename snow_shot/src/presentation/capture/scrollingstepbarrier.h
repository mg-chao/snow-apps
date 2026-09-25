#pragma once
#include <QPoint>
#include <QString>
#include <optional>
#include <chrono>
namespace snow_shot::capture_detail {
inline std::optional<QPoint> scrollingStepDelta(const QString& direction) {
    if (direction == QStringLiteral("up"))
        return QPoint(0, 120);
    if (direction == QStringLiteral("down"))
        return QPoint(0, -120);
    if (direction == QStringLiteral("left"))
        return QPoint(-120, 0);
    if (direction == QStringLiteral("right"))
        return QPoint(120, 0);
    return std::nullopt;
}
struct ScrollingStepBarrier {
    using Clock = std::chrono::steady_clock;
    Clock::time_point dispatchedAt;
    [[nodiscard]] bool settled(Clock::time_point observedAt, Clock::time_point changedAt,
                               bool pipelineIdle) const {
        return pipelineIdle && observedAt - dispatchedAt >= std::chrono::milliseconds(200) &&
               observedAt - std::max(changedAt, dispatchedAt) >= std::chrono::milliseconds(100);
    }
};
} // namespace snow_shot::capture_detail
