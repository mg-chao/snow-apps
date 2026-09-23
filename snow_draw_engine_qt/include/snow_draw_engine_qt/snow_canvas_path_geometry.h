#pragma once

#include <QPainterPath>
#include <QPointF>
#include <QVector>

// Stateless access to the engine's free-draw curve algorithm.
QPainterPath snowCanvasCatmullRomPath(const QVector<QPointF>& vertices, bool closed);
