#pragma once

#include <QVector>
#include <algorithm>
#include <cstdint>
#include <utility>

namespace snow_shot::presentation {

// GUI-thread state for asynchronous replacement of the recording capture filter.
// Every replacement must include all currently live excluded windows. Therefore
// an already acknowledged identity stays safe while an expanded filter is pending,
// even if that update fails or only some capture streams apply it.
class RecordingPopupExclusions final {
  public:
    enum class Completion { Pending, Applied, Failed };

    template <typename Request>
    bool canShow(std::uint32_t windowId, QVector<std::uint32_t> windows, Request&& request) {
        std::sort(windows.begin(), windows.end());
        windows.erase(std::unique(windows.begin(), windows.end()), windows.end());
        if (!windowId || !windows.contains(windowId))
            return false;

        const bool acknowledged = m_acknowledged.contains(windowId);
        // A submitted filter may already be mutating native streams even before
        // its acknowledgment. Reconcile removals against that snapshot as well.
        if ((windows != m_acknowledged || windows != m_submitted) && !m_failed.contains(windows) &&
            (!m_generation || windows != m_submitted)) {
            const std::uint64_t generation = request(windows);
            if (generation) {
                // Once a removal is submitted, an old approval is no longer
                // safe to reuse: that removal may apply before a newer update.
                m_acknowledged.erase(
                    std::remove_if(m_acknowledged.begin(), m_acknowledged.end(),
                                   [&windows](auto id) { return !windows.contains(id); }),
                    m_acknowledged.end());
                m_generation = generation;
                m_submitted = std::move(windows);
            } else {
                // A rejected replacement does not cancel an earlier accepted request.
                m_failed.push_back(std::move(windows));
            }
        }
        return acknowledged;
    }

    Completion complete(std::uint64_t requested, std::uint64_t applied, unsigned status) {
        if (!m_generation || requested != m_generation || status == 1)
            return Completion::Pending;
        m_generation = 0;
        if (status == 0 && applied == requested) {
            m_acknowledged = m_submitted;
            return Completion::Applied;
        }
        if (!m_failed.contains(m_submitted))
            m_failed.push_back(m_submitted);
        return Completion::Failed;
    }

    void retry() {
        m_failed.clear();
    }

    bool pending() const {
        return m_generation != 0;
    }

  private:
    std::uint64_t m_generation = 0;
    QVector<std::uint32_t> m_acknowledged;
    QVector<std::uint32_t> m_submitted;
    QVector<QVector<std::uint32_t>> m_failed;
};

} // namespace snow_shot::presentation
