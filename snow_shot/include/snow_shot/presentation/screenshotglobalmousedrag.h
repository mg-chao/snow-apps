#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTGLOBALMOUSEDRAG_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTGLOBALMOUSEDRAG_H

#include "snow_shot/presentation/globalmousetypes.h"
#include <QPointF>
#include <QRectF>
#include <QtGlobal>

#include <optional>

class ScreenshotGlobalMouseDrag final {
  public:
    using CoordinateSpace = snow_shot::presentation::GlobalMouseCoordinateSpace;
    void begin(quint64 id, const QPointF& position,
               CoordinateSpace space = CoordinateSpace::PhysicalPixels);
    [[nodiscard]] CoordinateSpace coordinateSpace() const {
        return m_coordinateSpace;
    }
    bool update(quint64 id, const QPointF& position, bool released = false);
    // Adopt the live cursor position once the capture is presented: reveal work
    // delays paced drag deliveries, leaving the buffered end point behind the
    // actual cursor. A buffered release keeps its exact position.
    void refreshEndFromLivePosition(const std::optional<QPointF>& position);
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
    [[nodiscard]] QPointF start() const {
        return m_start;
    }
    [[nodiscard]] QPointF end() const {
        return m_end;
    }
    [[nodiscard]] static QRectF selection(const QPointF& start, const QPointF& end,
                                          const QRectF& bounds);

  private:
    CoordinateSpace m_coordinateSpace = CoordinateSpace::PhysicalPixels;
    quint64 m_id = 0;
    QPointF m_start;
    QPointF m_end;
    bool m_ready = false;
    bool m_released = false;
};
#endif
