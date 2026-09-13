#include "snow_shot/presentation/screenshotglobalmousedrag.h"

#include <algorithm>

void ScreenshotGlobalMouseDrag::begin(quint64 id, const QPoint& position) {
    m_id = id;
    m_start = position;
    m_end = position;
    m_ready = false;
    m_released = false;
}

bool ScreenshotGlobalMouseDrag::update(quint64 id, const QPoint& position, bool released) {
    if (!active() || m_id != id || m_released) {
        return false;
    }
    m_end = position;
    m_released = released;
    return true;
}

void ScreenshotGlobalMouseDrag::refreshEndFromLivePosition(const std::optional<QPoint>& position) {
    if (!position.has_value() || !active() || m_released) {
        return;
    }
    static_cast<void>(update(m_id, *position));
}

void ScreenshotGlobalMouseDrag::reset() {
    *this = {};
}

QRectF ScreenshotGlobalMouseDrag::selection(const QPointF& start, const QPointF& end,
                                            const QRectF& bounds) {
    if (bounds.isEmpty()) {
        return {};
    }
    const auto bounded = [&bounds](const QPointF& point) {
        return QPointF(std::clamp(point.x(), bounds.left(), bounds.right()),
                       std::clamp(point.y(), bounds.top(), bounds.bottom()));
    };
    return QRectF(bounded(start), bounded(end)).normalized();
}
