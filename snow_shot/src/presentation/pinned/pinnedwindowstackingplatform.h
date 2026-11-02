#ifndef SNOW_SHOT_PRESENTATION_PINNEDWINDOWSTACKINGPLATFORM_H
#define SNOW_SHOT_PRESENTATION_PINNEDWINDOWSTACKINGPLATFORM_H

#include <QPointer>
#include <QVector>
#include <QWidget>
#include <functional>
#include <optional>

namespace snow_shot::presentation {
using PinnedStackingWindows = QVector<QPointer<QWidget>>;

// Call only on the GUI thread. Capture returns front-to-back visible surfaces;
// nullopt means unavailable, not an empty desktop. The injectable port also lets
// offscreen tests exercise real lifecycle decisions without pretending to be a WM.
struct PinnedWindowStackingPlatform {
    std::function<std::optional<PinnedStackingWindows>(const PinnedStackingWindows&)> capture;
    std::function<qint64(QWidget*)> level;
    std::function<bool(QWidget*, QWidget* above, QWidget* below)> place;
    std::function<bool(const PinnedStackingWindows&)> apply;
};

// Optional GUI-thread counters for the focused native benchmark.
struct PinnedWindowStackingMetrics {
    quint64 snapshots = 0;
    quint64 placements = 0;
    quint64 deferredBatches = 0;
};
[[nodiscard]] PinnedWindowStackingPlatform
createPinnedWindowStackingPlatform(PinnedWindowStackingMetrics* metrics = nullptr);
} // namespace snow_shot::presentation
#endif
