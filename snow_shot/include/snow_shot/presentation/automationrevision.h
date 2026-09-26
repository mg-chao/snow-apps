#ifndef SNOW_SHOT_PRESENTATION_AUTOMATIONREVISION_H
#define SNOW_SHOT_PRESENTATION_AUTOMATIONREVISION_H
#include <QtGlobal>
#include <atomic>

namespace snow_shot::presentation {
// Revisions identify mutations, including a change followed by its inverse. All
// instances share a sequence so restoring a window cannot reuse an old revision.
inline quint64 nextAutomationRevision() {
    static std::atomic<quint64> sequence{0};
    return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}
} // namespace snow_shot::presentation
#endif
