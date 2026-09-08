#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTGLOBALMOUSEDRAG_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTGLOBALMOUSEDRAG_H

#include <QPoint>
#include <QRectF>
#include <QtGlobal>

class ScreenshotGlobalMouseDrag final {
  public:
    void begin(quint64 id, const QPoint& position);
    bool update(quint64 id, const QPoint& position, bool released = false);
    void reset();
    void setReady() {
        m_ready = true;
    }
    [[nodiscard]] quint64 id() const {
        return m_id;
    }
    [[nodiscard]] bool active() const {
        return m_id != 0;
    }
    [[nodiscard]] bool ready() const {
        return m_ready;
    }
    [[nodiscard]] bool released() const {
        return m_released;
    }
    [[nodiscard]] QPoint start() const {
        return m_start;
    }
    [[nodiscard]] QPoint end() const {
        return m_end;
    }
    [[nodiscard]] static QRectF selection(const QPointF& start, const QPointF& end,
                                          const QRectF& bounds);

  private:
    quint64 m_id = 0;
    QPoint m_start;
    QPoint m_end;
    bool m_ready = false;
    bool m_released = false;
};
#endif
