#ifndef SNOW_SHOT_APP_IDLEMEMORYRECLAIMPOLICY_H
#define SNOW_SHOT_APP_IDLEMEMORYRECLAIMPOLICY_H

namespace snow_shot::app {

struct IdleMemoryReclaimDecision {
    bool shouldReclaim = false;
    bool trimWorkingSet = false;
};

class IdleMemoryReclaimPolicy final {
  public:
    void request(bool trimWorkingSet = false) noexcept {
        m_reclaimPending = true;
        m_trimWorkingSetPending = m_trimWorkingSetPending || trimWorkingSet;
    }

    [[nodiscard]] bool shouldArmTimer(bool activeWork, bool visibleWindow = false) const noexcept {
        return m_reclaimPending && !activeWork && !visibleWindow;
    }

    [[nodiscard]] IdleMemoryReclaimDecision takeIfIdle(bool activeWork,
                                                       bool visibleWindow) noexcept {
        if (!m_reclaimPending || activeWork || visibleWindow) {
            return {};
        }
        const IdleMemoryReclaimDecision decision{true, m_trimWorkingSetPending};
        m_reclaimPending = false;
        m_trimWorkingSetPending = false;
        return decision;
    }

    [[nodiscard]] bool hasPendingRequest() const noexcept {
        return m_reclaimPending;
    }

    [[nodiscard]] bool hasPendingWorkingSetTrim() const noexcept {
        return m_trimWorkingSetPending;
    }

  private:
    bool m_reclaimPending = false;
    bool m_trimWorkingSetPending = false;
};

} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_IDLEMEMORYRECLAIMPOLICY_H
