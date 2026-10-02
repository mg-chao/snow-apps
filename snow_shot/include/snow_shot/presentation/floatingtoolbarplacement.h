#ifndef SNOW_SHOT_PRESENTATION_FLOATINGTOOLBARPLACEMENT_H
#define SNOW_SHOT_PRESENTATION_FLOATINGTOOLBARPLACEMENT_H

#include <QPointF>
#include <QRect>
#include <algorithm>

namespace snow_shot::presentation::floating_toolbar {
inline QPoint constrain(QPoint point, QSize size, const QRect& bounds) {
    return {std::clamp(point.x(), bounds.left(),
                       std::max(bounds.left(), bounds.right() + 1 - size.width())),
            std::clamp(point.y(), bounds.top(),
                       std::max(bounds.top(), bounds.bottom() + 1 - size.height()))};
}
inline QPoint restore(QPointF fraction, QSize size, const QRect& bounds) {
    return constrain(
        {bounds.left() + qRound(fraction.x() * std::max(0, bounds.width() - size.width())),
         bounds.top() + qRound(fraction.y() * std::max(0, bounds.height() - size.height()))},
        size, bounds);
}
inline QPointF remember(QPoint point, QSize size, const QRect& bounds) {
    point = constrain(point, size, bounds);
    return {double(point.x() - bounds.left()) / std::max(1, bounds.width() - size.width()),
            double(point.y() - bounds.top()) / std::max(1, bounds.height() - size.height())};
}
inline bool rightSide(QPoint point, QSize size, const QRect& bounds) {
    return point.x() + size.width() / 2 >= bounds.center().x();
}
inline QPoint dock(QPoint point, QSize size, const QRect& bounds, int threshold = 16) {
    point = constrain(point, size, bounds);
    if (point.x() - bounds.left() <= threshold)
        point.setX(bounds.left());
    else if (bounds.right() + 1 - point.x() - size.width() <= threshold)
        point.setX(bounds.right() + 1 - size.width());
    return point;
}
inline QPoint tucked(QPoint point, QSize size, const QRect& bounds) {
    if (point.x() == bounds.left())
        point.rx() -= size.width() / 2;
    else if (point.x() + size.width() == bounds.right() + 1)
        point.rx() += size.width() / 2;
    return point;
}
} // namespace snow_shot::presentation::floating_toolbar
#endif
