#ifndef SCREENSHOTPINNEDSHAKEDETECTOR_H
#define SCREENSHOTPINNEDSHAKEDETECTOR_H

#include <QPointF>
#include <QtGlobal>

#include <cmath>

// Pointer positions are in logical pixels. No window queries, clocks or allocations belong here.
class ScreenshotPinnedShakeDetector final {
  public:
    static constexpr qreal minimumLeg = 48;
    static constexpr int minimumLegCount = 6;
    static constexpr qint64 maximumLegMs = 250;
    static constexpr qint64 maximumSequenceMs = 700;

    void reset() {
        *this = {};
    }

    void begin(const QPointF& position, qint64 timeMs = 0) {
        reset();
        if (!valid(position) || timeMs < 0)
            return;
        m_x.begin(position.x(), timeMs);
        m_y.begin(position.y(), timeMs);
        m_lastTime = timeMs;
        m_active = true;
    }

    [[nodiscard]] bool triggered() const {
        return m_triggered;
    }

    // Returns true exactly once until begin/reset, independently of event frequency.
    [[nodiscard]] bool sample(const QPointF& position, qint64 timeMs) {
        if (!m_active || m_triggered)
            return false;
        if (!valid(position) || timeMs < m_lastTime) {
            begin(position, timeMs);
            return false;
        }
        m_lastTime = timeMs;
        const bool x = m_x.sample(position.x(), timeMs);
        const bool y = m_y.sample(position.y(), timeMs);
        m_triggered = x || y;
        return m_triggered;
    }

  private:
    struct Axis {
        qreal origin = 0;
        qreal extreme = 0;
        qint64 sequenceStart = 0;
        qint64 legStart = 0;
        qint64 extremeTime = 0;
        int direction = 0;
        int legs = 0;

        void begin(qreal position, qint64 timeMs) {
            origin = extreme = position;
            sequenceStart = legStart = extremeTime = timeMs;
            direction = legs = 0;
        }

        bool sample(qreal position, qint64 timeMs) {
            if (timeMs - sequenceStart > maximumSequenceMs) {
                begin(position, timeMs);
                return false;
            }
            if (direction == 0) {
                if (timeMs - legStart > maximumLegMs) {
                    begin(position, timeMs);
                    return false;
                }
                if (std::abs(position - origin) >= minimumLeg) {
                    direction = position > origin ? 1 : -1;
                    extreme = position;
                    extremeTime = timeMs;
                    legs = 1;
                }
                return false;
            }
            const qreal advance = (position - extreme) * direction;
            if (advance > 0) {
                // Extending a leg must not turn a long, ordinary sweep into a rapid leg.
                if (timeMs - legStart > maximumLegMs) {
                    begin(position, timeMs);
                    return false;
                }
                extreme = position;
                extremeTime = timeMs;
            } else {
                if (timeMs - extremeTime > maximumLegMs) {
                    begin(position, timeMs);
                    return false;
                }
                if (-advance >= minimumLeg) {
                    legStart = extremeTime;
                    extreme = position;
                    extremeTime = timeMs;
                    direction = -direction;
                    return ++legs == minimumLegCount;
                }
            }
            return false;
        }
    };

    static bool valid(const QPointF& position) {
        return std::isfinite(position.x()) && std::isfinite(position.y());
    }

    Axis m_x;
    Axis m_y;
    qint64 m_lastTime = 0;
    bool m_active = false;
    bool m_triggered = false;
};

#endif
