#ifndef SNOW_SHOT_APP_MCP_MCPSTYLEPATCH_H
#define SNOW_SHOT_APP_MCP_MCPSTYLEPATCH_H
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QColor>
#include <QStringList>
#include <cmath>
#include <type_traits>
#include "snow_draw_engine_qt/snow_canvas_widget.h"

namespace snow_shot::app::mcp {
inline const QHash<QString, SnowCanvasTool>& mcpCanvasTools() {
    static const QHash<QString, SnowCanvasTool> tools{
        {QStringLiteral("select"), SnowCanvasTool::Select},
        {QStringLiteral("rectangle"), SnowCanvasTool::Shape},
        {QStringLiteral("arrow"), SnowCanvasTool::Arrow},
        {QStringLiteral("line"), SnowCanvasTool::Line},
        {QStringLiteral("freehand"), SnowCanvasTool::FreeDraw},
        {QStringLiteral("rectangle_highlight"), SnowCanvasTool::RectangleHighlight},
        {QStringLiteral("pen_highlight"), SnowCanvasTool::PenHighlight},
        {QStringLiteral("eraser"), SnowCanvasTool::Eraser},
        {QStringLiteral("rectangle_filter"), SnowCanvasTool::RectangleFilter},
        {QStringLiteral("pen_filter"), SnowCanvasTool::PenFilter},
        {QStringLiteral("text"), SnowCanvasTool::Text},
        {QStringLiteral("serial_number"), SnowCanvasTool::SerialNumber},
        {QStringLiteral("watermark"), SnowCanvasTool::Watermark},
        {QStringLiteral("spotlight"), SnowCanvasTool::Spotlight},
        {QStringLiteral("auto_filter"), SnowCanvasTool::AutoFilter}};
    return tools;
}
template <class Commands, class Canvas>
inline bool mcpStylePatch(Commands& commands, Canvas& canvas, const QJsonObject& params) {
    const auto target = params.value(QStringLiteral("target")).toString();
    const auto patch = params.value(QStringLiteral("style")).toObject();
    if (patch.isEmpty())
        return false;
    const QHash<QString, QStringList> enums{
        {QStringLiteral("shape"),
         {QStringLiteral("rectangle"), QStringLiteral("ellipse"), QStringLiteral("diamond")}},
        {QStringLiteral("fill_style"),
         {QStringLiteral("line"), QStringLiteral("cross_line"), QStringLiteral("solid")}},
        {QStringLiteral("stroke_style"),
         {QStringLiteral("solid"), QStringLiteral("dashed"), QStringLiteral("dotted")}},
        {QStringLiteral("arrow_type"),
         {QStringLiteral("straight"), QStringLiteral("curve"), QStringLiteral("elbow")}},
        {QStringLiteral("arrow_shaft_type"), {QStringLiteral("plain"), QStringLiteral("tapered")}},
        {QStringLiteral("horizontal_align"),
         {QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")}},
        {QStringLiteral("vertical_align"),
         {QStringLiteral("top"), QStringLiteral("center"), QStringLiteral("bottom")}},
        {QStringLiteral("serial_type"),
         {QStringLiteral("outlined_circle"), QStringLiteral("solid_circle"),
          QStringLiteral("outlined_square"), QStringLiteral("solid_square"),
          QStringLiteral("circle")}},
        {QStringLiteral("filter"),
         {QStringLiteral("mosaic"), QStringLiteral("gaussian_blur"), QStringLiteral("grayscale"),
          QStringLiteral("inversion"), QStringLiteral("emboss"), QStringLiteral("smart_erase")}},
        {QStringLiteral("start_arrowhead"),
         {QStringLiteral("none"), QStringLiteral("arrow"), QStringLiteral("bar"),
          QStringLiteral("dot"), QStringLiteral("circle"), QStringLiteral("circle_outline"),
          QStringLiteral("triangle"), QStringLiteral("triangle_outline"), QStringLiteral("diamond"),
          QStringLiteral("diamond_outline"), QStringLiteral("crowfoot_one"),
          QStringLiteral("crowfoot_many"), QStringLiteral("crowfoot_one_or_many"),
          QStringLiteral("indented_triangle")}}};
    const QStringList colors{QStringLiteral("stroke"), QStringLiteral("fill"),
                             QStringLiteral("color")};
    const QStringList textFields{QStringLiteral("text"), QStringLiteral("font_family")};
    const QStringList numbers{
        QStringLiteral("stroke_width"),  QStringLiteral("font_size"),   QStringLiteral("opacity"),
        QStringLiteral("corner_radius"), QStringLiteral("strength"),    QStringLiteral("angle"),
        QStringLiteral("gap"),           QStringLiteral("arrow_ratio"), QStringLiteral("number")};
    QStringList allowed;
    if (target == QStringLiteral("text"))
        allowed = {QStringLiteral("color"),
                   QStringLiteral("font_size"),
                   QStringLiteral("font_family"),
                   QStringLiteral("opacity"),
                   QStringLiteral("fill"),
                   QStringLiteral("stroke"),
                   QStringLiteral("stroke_width"),
                   QStringLiteral("fill_style"),
                   QStringLiteral("corner_radius"),
                   QStringLiteral("corner_radii"),
                   QStringLiteral("horizontal_align"),
                   QStringLiteral("vertical_align")};
    else if (target == QStringLiteral("serial_number"))
        allowed = {QStringLiteral("color"),       QStringLiteral("font_size"),
                   QStringLiteral("font_family"), QStringLiteral("opacity"),
                   QStringLiteral("fill"),        QStringLiteral("stroke_width"),
                   QStringLiteral("fill_style"),  QStringLiteral("stroke_style"),
                   QStringLiteral("serial_type"), QStringLiteral("number")};
    else if (target == QStringLiteral("watermark"))
        allowed = {QStringLiteral("color"),       QStringLiteral("font_size"),
                   QStringLiteral("font_family"), QStringLiteral("opacity"),
                   QStringLiteral("text"),        QStringLiteral("angle"),
                   QStringLiteral("gap")};
    else if (target == QStringLiteral("spotlight"))
        allowed = {QStringLiteral("color"), QStringLiteral("opacity")};
    else if (target == QStringLiteral("rectangle_filter") || target == QStringLiteral("pen_filter"))
        allowed = {QStringLiteral("filter"), QStringLiteral("strength"), QStringLiteral("opacity"),
                   QStringLiteral("stroke_width")};
    else if (target == QStringLiteral("rectangle"))
        allowed = {QStringLiteral("fill"),          QStringLiteral("stroke"),
                   QStringLiteral("stroke_width"),  QStringLiteral("fill_style"),
                   QStringLiteral("stroke_style"),  QStringLiteral("shape"),
                   QStringLiteral("corner_radius"), QStringLiteral("corner_radii")};
    else if (target == QStringLiteral("arrow"))
        allowed = {QStringLiteral("stroke"),           QStringLiteral("stroke_width"),
                   QStringLiteral("stroke_style"),     QStringLiteral("start_arrowhead"),
                   QStringLiteral("end_arrowhead"),    QStringLiteral("arrow_type"),
                   QStringLiteral("arrow_shaft_type"), QStringLiteral("arrow_ratio")};
    else if (target == QStringLiteral("line") || target == QStringLiteral("freehand")) {
        allowed = {QStringLiteral("fill"),         QStringLiteral("stroke"),
                   QStringLiteral("stroke_width"), QStringLiteral("fill_style"),
                   QStringLiteral("stroke_style"), QStringLiteral("opacity")};
        if (target == QStringLiteral("line"))
            allowed.append(QStringLiteral("arrow_type"));
    } else if (target == QStringLiteral("rectangle_highlight"))
        allowed = {QStringLiteral("fill"), QStringLiteral("stroke"),
                   QStringLiteral("stroke_width")};
    else if (target == QStringLiteral("pen_highlight"))
        allowed = {QStringLiteral("stroke"), QStringLiteral("stroke_width")};
    else
        return false;
    if (patch.contains(QStringLiteral("corner_radius")) &&
        patch.contains(QStringLiteral("corner_radii")))
        return false;
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (!allowed.contains(it.key()))
            return false;
        if (colors.contains(it.key())) {
            const auto array = it->toArray();
            if (array.size() != 4)
                return false;
            for (auto v : array)
                if (!v.isDouble() || v.toDouble() < 0 || v.toDouble() > 255 ||
                    std::floor(v.toDouble()) != v.toDouble())
                    return false;
        } else if (it.key() == QStringLiteral("corner_radii")) {
            const auto radii = it->toArray();
            if (radii.size() != 4)
                return false;
            for (const auto radius : radii)
                if (!radius.isDouble() || !std::isfinite(radius.toDouble()) ||
                    radius.toDouble() < 0 || radius.toDouble() > 8192)
                    return false;
        } else if (textFields.contains(it.key())) {
            if (!it->isString() || it->toString().toUtf8().size() > 65536)
                return false;
        } else if (numbers.contains(it.key())) {
            const auto value = it->toDouble(-1);
            const double maximum =
                it.key() == QStringLiteral("opacity") || it.key() == QStringLiteral("strength") ? 1
                : it.key() == QStringLiteral("number") ? 9007199254740991.0
                                                       : 8192;
            if (!it->isDouble() || !std::isfinite(value) || value > maximum ||
                value < (it.key() == QStringLiteral("angle") ? -360 : 0))
                return false;
            if (it.key() == QStringLiteral("number") && std::floor(value) != value)
                return false;
            if (it.key() == QStringLiteral("arrow_ratio") && (value < 1 || value > 3))
                return false;
        } else {
            const auto key = it.key() == QStringLiteral("end_arrowhead")
                                 ? QStringLiteral("start_arrowhead")
                                 : it.key();
            if (!enums.value(key).contains(it->toString()))
                return false;
        }
    }
    const auto color = [&](const char* name, QColor fallback) {
        const auto v = patch.value(QLatin1String(name));
        if (v.isUndefined())
            return fallback;
        const auto c = v.toArray();
        return QColor(c[0].toInt(), c[1].toInt(), c[2].toInt(), c[3].toInt());
    };
    const auto number = [&](const char* name, double fallback) {
        return patch.value(QLatin1String(name)).toDouble(fallback);
    };
    const auto enumeration = [&](const char* name, int fallback) {
        const auto value = patch.value(QLatin1String(name));
        const auto key = QString::fromLatin1(name) == QStringLiteral("end_arrowhead")
                             ? QStringLiteral("start_arrowhead")
                             : QString::fromLatin1(name);
        return value.isUndefined() ? fallback : enums.value(key).indexOf(value.toString());
    };
    const auto state = canvas.canvasStyleToolbarState();
    const auto dispatch = [](auto&& operation) {
        if constexpr (std::is_void_v<decltype(operation())>) {
            operation();
            return true;
        } else {
            return operation();
        }
    };
    const auto cornerRadii = [&](SnowCanvasCornerRadii fallback) {
        if (patch.contains(QStringLiteral("corner_radius"))) {
            const auto r = number("corner_radius", 0);
            return SnowCanvasCornerRadii{r, r, r, r};
        }
        if (patch.contains(QStringLiteral("corner_radii"))) {
            const auto radii = patch.value(QStringLiteral("corner_radii")).toArray();
            return SnowCanvasCornerRadii{radii[0].toDouble(), radii[1].toDouble(),
                                         radii[2].toDouble(), radii[3].toDouble()};
        }
        return fallback;
    };
    if (target == QStringLiteral("watermark")) {
        auto style = canvas.canvasWatermarkConfig();
        style.color = color("color", style.color);
        style.fontSize = number("font_size", style.fontSize);
        style.opacity = number("opacity", style.opacity);
        style.angle = number("angle", style.angle);
        style.gap = number("gap", style.gap);
        if (patch.contains(QStringLiteral("text"))) {
            style.text = patch.value(QStringLiteral("text")).toString();
            style.templateValue = style.text;
        }
        if (patch.contains(QStringLiteral("font_family")))
            style.fontFamily = patch.value(QStringLiteral("font_family")).toString();
        return dispatch([&] { return commands.setWatermarkConfigFromToolbar(style); });
    } else if (target == QStringLiteral("spotlight")) {
        auto style = canvas.canvasSpotlightConfig();
        style.color = color("color", style.color);
        style.opacity = number("opacity", style.opacity);
        return dispatch([&] { return commands.setSpotlightConfigFromToolbar(style); });
    } else if (target == QStringLiteral("text")) {
        auto style = state.textStyle;
        style.color = color("color", style.color);
        style.fill = color("fill", style.fill);
        style.stroke = color("stroke", style.stroke);
        style.fontSize = number("font_size", style.fontSize);
        style.strokeWidth = number("stroke_width", style.strokeWidth);
        style.opacity = number("opacity", style.opacity);
        if (patch.contains(QStringLiteral("font_family")))
            style.fontFamily = patch.value(QStringLiteral("font_family")).toString();
        style.cornerRadii = cornerRadii(style.cornerRadii);
        style.horizontalAlign = static_cast<SnowCanvasTextHorizontalAlign>(
            enumeration("horizontal_align", static_cast<int>(style.horizontalAlign)));
        style.verticalAlign = static_cast<SnowCanvasTextVerticalAlign>(
            enumeration("vertical_align", static_cast<int>(style.verticalAlign)));
        style.fillStyle = static_cast<SnowCanvasFillStyle>(
            enumeration("fill_style", static_cast<int>(style.fillStyle)));
        return dispatch([&] { return commands.setTextStyleFromToolbar(style); });
    } else if (target == QStringLiteral("serial_number")) {
        auto style = state.serialNumberStyle;
        if (patch.contains(QStringLiteral("number")))
            style.number = patch.value(QStringLiteral("number")).toInteger();
        style.type = static_cast<SnowCanvasSerialNumberType>(
            enumeration("serial_type", static_cast<int>(style.type)));
        style.color = color("color", style.color);
        style.fill = color("fill", style.fill);
        style.fontSize = number("font_size", style.fontSize);
        style.strokeWidth = number("stroke_width", style.strokeWidth);
        style.opacity = number("opacity", style.opacity);
        if (patch.contains(QStringLiteral("font_family")))
            style.fontFamily = patch.value(QStringLiteral("font_family")).toString();
        style.fillStyle = static_cast<SnowCanvasFillStyle>(
            enumeration("fill_style", static_cast<int>(style.fillStyle)));
        style.strokeStyle = static_cast<SnowCanvasStrokeStyle>(
            enumeration("stroke_style", static_cast<int>(style.strokeStyle)));
        return dispatch([&] { return commands.setSerialNumberStyleFromToolbar(style); });
    } else if (target.endsWith(QStringLiteral("filter"))) {
        auto style = state.filterStyle;
        style.type =
            static_cast<SnowCanvasFilterType>(enumeration("filter", static_cast<int>(style.type)));
        style.strength = number("strength", style.strength);
        style.opacity = number("opacity", style.opacity);
        style.strokeWidth = number("stroke_width", style.strokeWidth);
        quint32 flags = 0;
        if (patch.contains(QStringLiteral("filter")))
            flags |= SnowCanvasFilterStylePropertyType;
        if (patch.contains(QStringLiteral("strength")))
            flags |= SnowCanvasFilterStylePropertyStrength;
        if (patch.contains(QStringLiteral("opacity")))
            flags |= SnowCanvasFilterStylePropertyOpacity;
        if (patch.contains(QStringLiteral("stroke_width")))
            flags |= SnowCanvasFilterStylePropertyStrokeWidth;
        return dispatch([&] { return commands.setFilterStyleFromToolbar(style, flags); });
    } else {
        auto style = state.shapeStyle;
        style.fill = color("fill", style.fill);
        style.stroke = color("stroke", style.stroke);
        style.strokeWidth = number("stroke_width", style.strokeWidth);
        style.opacity = number("opacity", style.opacity);
        style.shape = static_cast<SnowCanvasRectangleShape>(
            enumeration("shape", static_cast<int>(style.shape)));
        if (target == QStringLiteral("rectangle_highlight")) {
            if (style.shape == SnowCanvasRectangleShape::Diamond)
                return false;
            style.highlightShape = style.shape == SnowCanvasRectangleShape::Ellipse
                                       ? SnowCanvasHighlightShape::Ellipse
                                       : SnowCanvasHighlightShape::Rectangle;
        }
        style.fillStyle = static_cast<SnowCanvasFillStyle>(
            enumeration("fill_style", static_cast<int>(style.fillStyle)));
        style.strokeStyle = static_cast<SnowCanvasStrokeStyle>(
            enumeration("stroke_style", static_cast<int>(style.strokeStyle)));
        style.startArrowhead = static_cast<SnowCanvasArrowhead>(
            enumeration("start_arrowhead", static_cast<int>(style.startArrowhead)));
        style.endArrowhead = static_cast<SnowCanvasArrowhead>(
            enumeration("end_arrowhead", static_cast<int>(style.endArrowhead)));
        style.arrowType = static_cast<SnowCanvasArrowType>(
            enumeration("arrow_type", static_cast<int>(style.arrowType)));
        style.arrowShaftType = static_cast<SnowCanvasArrowShaftType>(
            enumeration("arrow_shaft_type", static_cast<int>(style.arrowShaftType)));
        style.arrowRatio = number("arrow_ratio", style.arrowRatio);
        style.cornerRadii = cornerRadii(style.cornerRadii);
        const QHash<QString, quint32> properties{
            {QStringLiteral("fill"), SnowCanvasShapeStylePropertyFillColor},
            {QStringLiteral("stroke"), SnowCanvasShapeStylePropertyStrokeColor},
            {QStringLiteral("stroke_width"), SnowCanvasShapeStylePropertyStrokeWidth},
            {QStringLiteral("fill_style"), SnowCanvasShapeStylePropertyFillStyle},
            {QStringLiteral("stroke_style"), SnowCanvasShapeStylePropertyStrokeStyle},
            {QStringLiteral("opacity"), SnowCanvasShapeStylePropertyOpacity},
            {QStringLiteral("corner_radius"), SnowCanvasShapeStylePropertyCornerRadius},
            {QStringLiteral("corner_radii"), SnowCanvasShapeStylePropertyCornerRadius},
            {QStringLiteral("shape"), SnowCanvasShapeStylePropertyShape},
            {QStringLiteral("start_arrowhead"), SnowCanvasShapeStylePropertyStartArrowhead},
            {QStringLiteral("end_arrowhead"), SnowCanvasShapeStylePropertyEndArrowhead},
            {QStringLiteral("arrow_type"), SnowCanvasShapeStylePropertyArrowType},
            {QStringLiteral("arrow_shaft_type"), SnowCanvasShapeStylePropertyArrowShaftType},
            {QStringLiteral("arrow_ratio"), SnowCanvasShapeStylePropertyArrowRatio}};
        quint32 flags = 0;
        for (auto it = patch.begin(); it != patch.end(); ++it)
            flags |= properties.value(it.key());
        const QStringList kinds{QStringLiteral("rectangle"),
                                QStringLiteral("arrow"),
                                QStringLiteral("line"),
                                QStringLiteral("freehand"),
                                QStringLiteral("rectangle_highlight"),
                                QStringLiteral("pen_highlight")};
        return dispatch([&] {
            return commands.setShapeStyleFromToolbar(
                style, flags, static_cast<SnowCanvasShapeKind>(kinds.indexOf(target)));
        });
    }
}
} // namespace snow_shot::app::mcp
#endif
