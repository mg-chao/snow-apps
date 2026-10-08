#pragma once

#include <QWheelEvent>

namespace snow_shot::presentation {

inline bool usesPreciseWheelDelta(const QWheelEvent& event) {
#ifdef Q_OS_MACOS
    // Qt Cocoa supplies estimated pixels for ordinary mouse notches too. Its
    // angle delta preserves the notch, while the pixel estimate is accelerated
    // and can be as small as two pixels. True precise Cocoa input is marked
    // MouseEventSynthesizedBySystem, including devices without scroll phases.
    if (event.source() == Qt::MouseEventNotSynthesized && event.phase() == Qt::NoScrollPhase &&
        !event.angleDelta().isNull()) {
        return false;
    }
#endif
    return !event.pixelDelta().isNull();
}

// Discrete actions share the same movement thresholds and gesture boundaries.
// Respond on the first point, then once per full step of continued movement.
class WheelStepAccumulator final {
  public:
    void reset() {
        m_remainder = 0;
        m_direction = 0;
        m_stepDelta = 0;
        m_timestamp = 0;
    }

    [[nodiscard]] int consume(const QWheelEvent& event) {
        if (event.phase() == Qt::ScrollMomentum)
            return 0;
        if (event.phase() == Qt::ScrollBegin || event.phase() == Qt::ScrollEnd)
            reset();
        if (event.phase() == Qt::ScrollEnd)
            return 0;

        const bool precise = usesPreciseWheelDelta(event);
        const int delta = precise ? event.pixelDelta().y() : event.angleDelta().y();
        if (delta == 0)
            return 0;
        const int stepDelta = precise ? 100 : 120;
        const int direction = delta > 0 ? 1 : -1;
        const quint64 timestamp = event.timestamp();
        // Wheels without scroll phases need an idle boundary for a new immediate step.
        const bool newBurst = event.phase() == Qt::NoScrollPhase && timestamp > m_timestamp &&
                              timestamp - m_timestamp >= 250;
        if (direction != m_direction || stepDelta != m_stepDelta || newBurst) {
            // Keeping the initial credit here makes event coalescing irrelevant.
            m_remainder = direction * (stepDelta - 1);
        }
        m_direction = direction;
        m_stepDelta = stepDelta;
        m_timestamp = timestamp;
        const qint64 accumulated = qint64(m_remainder) + delta;
        m_remainder = int(accumulated % stepDelta);
        return int(accumulated / stepDelta);
    }

  private:
    int m_remainder = 0;
    int m_direction = 0;
    int m_stepDelta = 0;
    quint64 m_timestamp = 0;
};

} // namespace snow_shot::presentation
