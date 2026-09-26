#pragma once

#include "snow_shot/presentation/screenshotselectionmodel.h"
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace snow_shot::app::mcp {
// Validates the complete operand before touching the live model. All coordinates are
// half-open canvas coordinates; out-of-canvas values are rejected, never silently rounded in.
inline bool applySelection(ScreenshotSelectionModel& model, const QRectF& canvas,
                           const QJsonObject& params, QString* field) {
    const auto fail = [field](const QString& value) {
        if (field)
            *field = value;
        return false;
    };
    const QString operation =
        params.value(QStringLiteral("operation")).toString(QStringLiteral("replace"));
    if (operation != QStringLiteral("replace") && operation != QStringLiteral("add") &&
        operation != QStringLiteral("subtract"))
        return fail(QStringLiteral("operation"));
    const QString type = params.value(QStringLiteral("type")).toString(QStringLiteral("rectangle"));
    ScreenshotRegionGeometry operand;
    ScreenshotRegionType regionType = ScreenshotRegionType::Rectangle;
    const auto finite = [](const QJsonValue& v) {
        return v.isDouble() && std::isfinite(v.toDouble());
    };
    if (type == QStringLiteral("rectangle")) {
        const auto b = params.value(QStringLiteral("bounds")).toArray();
        if (b.size() != 4 || !std::all_of(b.begin(), b.end(), finite))
            return fail(QStringLiteral("bounds"));
        const QRectF rect(b[0].toDouble(), b[1].toDouble(), b[2].toDouble(), b[3].toDouble());
        if (rect.width() < 1 || rect.height() < 1 || !canvas.contains(rect))
            return fail(QStringLiteral("bounds"));
        operand = ScreenshotRegionGeometry(rect.toAlignedRect());
    } else {
        if (type != QStringLiteral("polygon") && type != QStringLiteral("polyline") &&
            type != QStringLiteral("freehand"))
            return fail(QStringLiteral("type"));
        const auto points = params.value(QStringLiteral("points")).toArray();
        if (points.size() < 3 || points.size() > 8192)
            return fail(QStringLiteral("points"));
        QPainterPath path;
        for (qsizetype i = 0; i < points.size(); ++i) {
            const auto p = points[i].toArray();
            if (p.size() != 2 || !finite(p[0]) || !finite(p[1]) ||
                !canvas.contains(QPointF(p[0].toDouble(), p[1].toDouble())))
                return fail(QStringLiteral("points[%1]").arg(i));
            const QPointF point(p[0].toDouble(), p[1].toDouble());
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        path.closeSubpath();
        regionType = type == QStringLiteral("freehand") ? ScreenshotRegionType::Freehand
                                                        : ScreenshotRegionType::Polyline;
        operand = ScreenshotRegionGeometry::fromPath(path, regionType);
    }
    const auto current = model.selectionRegion();
    if (operation != QStringLiteral("replace") &&
        (current.toJson().value(QStringLiteral("operands")).toArray().size() >= 64 ||
         current.toJson().value(QStringLiteral("rectangles")).toArray().size() >= 64))
        return fail(QStringLiteral("operation"));
    auto result = operation == QStringLiteral("add")        ? current.united(operand)
                  : operation == QStringLiteral("subtract") ? current.subtracted(operand)
                                                            : operand;
    if (result.isEmpty() || result.boundingRect().width() < 1 || result.boundingRect().height() < 1)
        return fail(QStringLiteral("bounds"));
    model.setRegionType(regionType);
    model.setSelectionRegion(result);
    return true;
}
} // namespace snow_shot::app::mcp
