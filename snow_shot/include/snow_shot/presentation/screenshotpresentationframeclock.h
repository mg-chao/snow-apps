#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPRESENTATIONFRAMECLOCK_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPRESENTATIONFRAMECLOCK_H

#include <QtGlobal>

#include <cmath>

// Keep the display phase in nanoseconds. Late GUI work skips expired deadlines instead of
// shifting every subsequent frame; the native timer only wakes the deadline scheduler.
class ScreenshotPresentationFrameClock final {
  public:
    void setRefreshRate(qreal rate, qint64 lastFrameNs) {
        const qreal validRate = std::isfinite(rate) && rate > 1.0 ? rate : 60.0;
        const qint64 period = qMax<qint64>(1, qRound64(1000000000.0 / validRate));
        if (period != m_periodNs || m_nextFrameNs == 0) {
            m_periodNs = period;
            m_nextFrameNs = lastFrameNs + period;
        }
    }

    [[nodiscard]] bool due(qint64 nowNs) const {
        return nowNs >= m_nextFrameNs;
    }

    void advancePast(qint64 nowNs) {
        if (m_nextFrameNs != 0 && due(nowNs))
            m_nextFrameNs += ((nowNs - m_nextFrameNs) / m_periodNs + 1) * m_periodNs;
    }

    void reset() {
        m_nextFrameNs = 0;
    }

    [[nodiscard]] qint64 nextFrameNanoseconds() const {
        return m_nextFrameNs;
    }

  private:
    qint64 m_periodNs = 16666667;
    qint64 m_nextFrameNs = 0;
};

#endif
