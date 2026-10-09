#pragma once

#include <QWheelEvent>

namespace snow_canvas_wheel {

// Discrete tool adjustments respond immediately and retain coalesced movement.
class StepAccumulator final {
  public:
    void reset() {
        m_remainder = 0;
        m_direction = 0;
        m_stepDelta = 0;
        m_timestamp = 0;
        m_modifiers = Qt::NoModifier;
    }

    int consume(const QWheelEvent& event) {
        if (event.phase() == Qt::ScrollMomentum)
            return 0;
        if (event.phase() == Qt::ScrollBegin || event.phase() == Qt::ScrollEnd)
            reset();
        if (event.phase() == Qt::ScrollEnd)
            return 0;
        if (event.modifiers() != m_modifiers) {
            reset();
            m_modifiers = event.modifiers();
        }
        bool precise = !event.pixelDelta().isNull();
#ifdef Q_OS_MACOS
        if (event.source() == Qt::MouseEventNotSynthesized && event.phase() == Qt::NoScrollPhase &&
            !event.angleDelta().isNull())
            precise = false;
#endif
        const int delta = precise ? event.pixelDelta().y() : event.angleDelta().y();
        if (delta == 0)
            return 0;
        const int threshold = precise ? 100 : 120;
        const int direction = delta > 0 ? 1 : -1;
        const quint64 timestamp = event.timestamp();
        const bool newBurst = event.phase() == Qt::NoScrollPhase && timestamp > m_timestamp &&
                              timestamp - m_timestamp >= 250;
        if (direction != m_direction || threshold != m_stepDelta || newBurst)
            m_remainder = direction * (threshold - 1);
        m_direction = direction;
        m_stepDelta = threshold;
        m_timestamp = timestamp;
        const qint64 total = qint64(m_remainder) + delta;
        m_remainder = int(total % threshold);
        return int(total / threshold);
    }

  private:
    int m_remainder = 0;
    int m_direction = 0;
    int m_stepDelta = 0;
    quint64 m_timestamp = 0;
    Qt::KeyboardModifiers m_modifiers = Qt::NoModifier;
};

} // namespace snow_canvas_wheel
