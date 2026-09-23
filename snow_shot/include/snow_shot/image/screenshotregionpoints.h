#pragma once

#include <QPointF>
#include <QVector>

#include <algorithm>

// Iterative Ramer-Douglas-Peucker, preserving both endpoints. The tolerance is
// measured in source canvas units, so callers convert from physical pixels.
inline QVector<QPointF> simplifyScreenshotRegionPoints(const QVector<QPointF>& points,
                                                       qreal tolerance) {
    if (points.size() < 3)
        return points;
    QVector<bool> keep(points.size(), false);
    keep.first() = keep.last() = true;
    QVector<QPair<qsizetype, qsizetype>> pending{{0, points.size() - 1}};
    const qreal threshold = tolerance * tolerance;
    while (!pending.isEmpty()) {
        const auto [first, last] = pending.takeLast();
        const auto start = points[first];
        const auto direction = points[last] - start;
        const qreal lengthSquared = QPointF::dotProduct(direction, direction);
        qreal largest = threshold;
        qsizetype split = -1;
        for (qsizetype i = first + 1; i < last; ++i) {
            const qreal amount =
                lengthSquared > 0
                    ? std::clamp(QPointF::dotProduct(points[i] - start, direction) / lengthSquared,
                                 qreal(0), qreal(1))
                    : 0;
            const auto delta = points[i] - start - direction * amount;
            const qreal distance = QPointF::dotProduct(delta, delta);
            if (distance > largest) {
                largest = distance;
                split = i;
            }
        }
        if (split >= 0) {
            keep[split] = true;
            pending.append({first, split});
            pending.append({split, last});
        }
    }
    QVector<QPointF> result;
    result.reserve(points.size());
    for (qsizetype i = 0; i < points.size(); ++i)
        if (keep[i])
            result.append(points[i]);
    return result;
}
