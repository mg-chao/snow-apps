#include "snow_shot/presentation/screenshotcanvastoolstyles.h"

#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace snow_shot::presentation {
namespace {
constexpr quint32 kRectangleShapeProperties =
    SnowCanvasShapeStylePropertyFillColor | SnowCanvasShapeStylePropertyFillStyle |
    SnowCanvasShapeStylePropertyStrokeColor | SnowCanvasShapeStylePropertyStrokeWidth |
    SnowCanvasShapeStylePropertyCornerRadius | SnowCanvasShapeStylePropertyStrokeStyle |
    SnowCanvasShapeStylePropertyShape;
constexpr quint32 kArrowShapeProperties =
    SnowCanvasShapeStylePropertyStrokeColor | SnowCanvasShapeStylePropertyStrokeWidth |
    SnowCanvasShapeStylePropertyStartArrowhead | SnowCanvasShapeStylePropertyEndArrowhead |
    SnowCanvasShapeStylePropertyStrokeStyle | SnowCanvasShapeStylePropertyArrowType;
constexpr quint32 kLineShapeProperties =
    SnowCanvasShapeStylePropertyFillColor | SnowCanvasShapeStylePropertyFillStyle |
    SnowCanvasShapeStylePropertyStrokeColor | SnowCanvasShapeStylePropertyStrokeWidth |
    SnowCanvasShapeStylePropertyStrokeStyle | SnowCanvasShapeStylePropertyOpacity;
constexpr quint32 kRectangleHighlightProperties = SnowCanvasShapeStylePropertyFillColor |
                                                  SnowCanvasShapeStylePropertyStrokeColor |
                                                  SnowCanvasShapeStylePropertyStrokeWidth;
constexpr quint32 kPenHighlightProperties = SnowCanvasShapeStylePropertyStrokeColor |
                                             SnowCanvasShapeStylePropertyStrokeWidth;
constexpr quint32 kAllFilterProperties = SnowCanvasFilterStylePropertyType |
                                         SnowCanvasFilterStylePropertyStrength |
                                         SnowCanvasFilterStylePropertyOpacity |
                                         SnowCanvasFilterStylePropertyStrokeWidth;

const QString kShapeKey = QStringLiteral("drawing/shape_style");
const QString kArrowKey = QStringLiteral("drawing/arrow_style");
const QString kLineKey = QStringLiteral("drawing/line_style");
const QString kFreeDrawKey = QStringLiteral("drawing/free_draw_style");
const QString kRectangleHighlightKey = QStringLiteral("drawing/rectangle_highlight_style");
const QString kPenHighlightKey = QStringLiteral("drawing/pen_highlight_style");
const QString kRectangleFilterKey = QStringLiteral("drawing/rectangle_filter_style");
const QString kPenFilterKey = QStringLiteral("drawing/pen_filter_style");
const QString kTextKey = QStringLiteral("drawing/text_style");
const QString kSerialNumberKey = QStringLiteral("drawing/serial_number_style");

QJsonValue colorValue(const QColor& color) {
    if (!color.isValid()) {
        return {};
    }
    return QStringLiteral("#%1%2%3%4")
        .arg(color.red(), 2, 16, QLatin1Char('0'))
        .arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(), 2, 16, QLatin1Char('0'))
        .arg(color.alpha(), 2, 16, QLatin1Char('0'));
}

bool colorValue(const QJsonValue& value, QColor* color) {
    if (color == nullptr || !value.isString()) {
        return false;
    }
    const QString text = value.toString();
    if (text.size() != 9 || !text.startsWith(u'#')) {
        return false;
    }
    bool ok = false;
    const int red = text.mid(1, 2).toInt(&ok, 16);
    if (!ok) return false;
    const int green = text.mid(3, 2).toInt(&ok, 16);
    if (!ok) return false;
    const int blue = text.mid(5, 2).toInt(&ok, 16);
    if (!ok) return false;
    const int alpha = text.mid(7, 2).toInt(&ok, 16);
    if (!ok) return false;
    *color = QColor(red, green, blue, alpha);
    return color->isValid();
}

void putDouble(QJsonObject* object, const QString& key, double value) {
    if (object != nullptr && std::isfinite(value)) {
        object->insert(key, value);
    }
}

bool readDouble(const QJsonObject& object, const QString& key, double* value) {
    if (value == nullptr || !object.value(key).isDouble() ||
        !std::isfinite(object.value(key).toDouble())) {
        return false;
    }
    *value = object.value(key).toDouble();
    return true;
}

double bounded(double value, double minimum, double maximum) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : minimum;
}

template <typename Enum>
void putEnum(QJsonObject* object, const QString& key, Enum value) {
    if (object != nullptr) {
        object->insert(key, static_cast<int>(value));
    }
}

template <typename Enum>
bool readEnum(const QJsonObject& object, const QString& key, int maximum, Enum* value) {
    if (value == nullptr || !object.value(key).isDouble()) {
        return false;
    }
    const double raw = object.value(key).toDouble();
    if (!std::isfinite(raw) || std::floor(raw) != raw || raw < 0 || raw > maximum) {
        return false;
    }
    *value = static_cast<Enum>(static_cast<int>(raw));
    return true;
}

QJsonObject cornerRadiiValue(const SnowCanvasCornerRadii& radii) {
    QJsonObject value;
    putDouble(&value, QStringLiteral("top_left"), radii.topLeft);
    putDouble(&value, QStringLiteral("top_right"), radii.topRight);
    putDouble(&value, QStringLiteral("bottom_right"), radii.bottomRight);
    putDouble(&value, QStringLiteral("bottom_left"), radii.bottomLeft);
    return value;
}

void readCornerRadii(const QJsonObject& object, SnowCanvasCornerRadii* radii) {
    if (radii == nullptr) return;
    const QJsonObject value = object.value(QStringLiteral("corner_radii")).toObject();
    readDouble(value, QStringLiteral("top_left"), &radii->topLeft);
    readDouble(value, QStringLiteral("top_right"), &radii->topRight);
    readDouble(value, QStringLiteral("bottom_right"), &radii->bottomRight);
    readDouble(value, QStringLiteral("bottom_left"), &radii->bottomLeft);
}

QJsonObject shapeValue(const SnowCanvasShapeStyle& style) {
    QJsonObject value;
    value.insert(QStringLiteral("fill"), colorValue(style.fill));
    putEnum(&value, QStringLiteral("fill_style"), style.fillStyle);
    value.insert(QStringLiteral("stroke"), colorValue(style.stroke));
    putDouble(&value, QStringLiteral("stroke_width"), style.strokeWidth);
    value.insert(QStringLiteral("corner_radii"), cornerRadiiValue(style.cornerRadii));
    putEnum(&value, QStringLiteral("start_arrowhead"), style.startArrowhead);
    putEnum(&value, QStringLiteral("end_arrowhead"), style.endArrowhead);
    putEnum(&value, QStringLiteral("stroke_style"), style.strokeStyle);
    putEnum(&value, QStringLiteral("arrow_type"), style.arrowType);
    putDouble(&value, QStringLiteral("opacity"), style.opacity);
    putEnum(&value, QStringLiteral("highlight_shape"), style.highlightShape);
    putEnum(&value, QStringLiteral("shape"), style.shape);
    return value;
}

void readShapeValue(const QJsonObject& object, SnowCanvasShapeStyle* style) {
    if (style == nullptr) return;
    QColor color;
    if (colorValue(object.value(QStringLiteral("fill")), &color)) style->fill = color;
    if (colorValue(object.value(QStringLiteral("stroke")), &color)) style->stroke = color;
    readDouble(object, QStringLiteral("stroke_width"), &style->strokeWidth);
    readCornerRadii(object, &style->cornerRadii);
    readEnum(object, QStringLiteral("fill_style"), static_cast<int>(SnowCanvasFillStyle::Solid),
             &style->fillStyle);
    readEnum(object, QStringLiteral("start_arrowhead"),
             static_cast<int>(SnowCanvasArrowhead::CrowfootOneOrMany), &style->startArrowhead);
    readEnum(object, QStringLiteral("end_arrowhead"),
             static_cast<int>(SnowCanvasArrowhead::CrowfootOneOrMany), &style->endArrowhead);
    readEnum(object, QStringLiteral("stroke_style"),
             static_cast<int>(SnowCanvasArrowStrokeStyle::Dotted), &style->strokeStyle);
    readEnum(object, QStringLiteral("arrow_type"), static_cast<int>(SnowCanvasArrowType::Elbow),
             &style->arrowType);
    readDouble(object, QStringLiteral("opacity"), &style->opacity);
    readEnum(object, QStringLiteral("highlight_shape"),
             static_cast<int>(SnowCanvasHighlightShape::Ellipse), &style->highlightShape);
    readEnum(object, QStringLiteral("shape"), static_cast<int>(SnowCanvasRectangleShape::Diamond),
             &style->shape);
}

QJsonObject filterValue(const SnowCanvasFilterStyle& style) {
    QJsonObject value;
    putEnum(&value, QStringLiteral("type"), style.type);
    putDouble(&value, QStringLiteral("strength"), style.strength);
    putDouble(&value, QStringLiteral("opacity"), style.opacity);
    putDouble(&value, QStringLiteral("stroke_width"), style.strokeWidth);
    return value;
}

void readFilterValue(const QJsonObject& object, SnowCanvasFilterStyle* style) {
    if (style == nullptr) return;
    readEnum(object, QStringLiteral("type"), static_cast<int>(SnowCanvasFilterType::Inversion),
             &style->type);
    readDouble(object, QStringLiteral("strength"), &style->strength);
    readDouble(object, QStringLiteral("opacity"), &style->opacity);
    readDouble(object, QStringLiteral("stroke_width"), &style->strokeWidth);
}

QJsonObject textValue(const SnowCanvasTextStyle& style) {
    QJsonObject value;
    value.insert(QStringLiteral("color"), colorValue(style.color));
    putDouble(&value, QStringLiteral("font_size"), style.fontSize);
    value.insert(QStringLiteral("font_family"), style.fontFamily);
    value.insert(QStringLiteral("fill"), colorValue(style.fill));
    putEnum(&value, QStringLiteral("fill_style"), style.fillStyle);
    value.insert(QStringLiteral("stroke"), colorValue(style.stroke));
    putDouble(&value, QStringLiteral("stroke_width"), style.strokeWidth);
    value.insert(QStringLiteral("corner_radii"), cornerRadiiValue(style.cornerRadii));
    putEnum(&value, QStringLiteral("horizontal_align"), style.horizontalAlign);
    putEnum(&value, QStringLiteral("vertical_align"), style.verticalAlign);
    putDouble(&value, QStringLiteral("opacity"), style.opacity);
    return value;
}

void readTextValue(const QJsonObject& object, SnowCanvasTextStyle* style) {
    if (style == nullptr) return;
    QColor color;
    if (colorValue(object.value(QStringLiteral("color")), &color)) style->color = color;
    if (colorValue(object.value(QStringLiteral("fill")), &color)) style->fill = color;
    if (colorValue(object.value(QStringLiteral("stroke")), &color)) style->stroke = color;
    readDouble(object, QStringLiteral("font_size"), &style->fontSize);
    if (object.value(QStringLiteral("font_family")).isString())
        style->fontFamily = object.value(QStringLiteral("font_family")).toString();
    readEnum(object, QStringLiteral("fill_style"), static_cast<int>(SnowCanvasFillStyle::Solid),
             &style->fillStyle);
    readDouble(object, QStringLiteral("stroke_width"), &style->strokeWidth);
    readCornerRadii(object, &style->cornerRadii);
    readEnum(object, QStringLiteral("horizontal_align"),
             static_cast<int>(SnowCanvasTextHorizontalAlign::Right), &style->horizontalAlign);
    readEnum(object, QStringLiteral("vertical_align"),
             static_cast<int>(SnowCanvasTextVerticalAlign::Bottom), &style->verticalAlign);
    readDouble(object, QStringLiteral("opacity"), &style->opacity);
}

QJsonObject serialNumberValue(const SnowCanvasSerialNumberStyle& style) {
    QJsonObject value;
    value.insert(QStringLiteral("number"), style.number);
    value.insert(QStringLiteral("color"), colorValue(style.color));
    value.insert(QStringLiteral("fill"), colorValue(style.fill));
    putEnum(&value, QStringLiteral("fill_style"), style.fillStyle);
    putDouble(&value, QStringLiteral("font_size"), style.fontSize);
    value.insert(QStringLiteral("font_family"), style.fontFamily);
    putDouble(&value, QStringLiteral("stroke_width"), style.strokeWidth);
    putEnum(&value, QStringLiteral("stroke_style"), style.strokeStyle);
    putDouble(&value, QStringLiteral("opacity"), style.opacity);
    return value;
}

void readSerialNumberValue(const QJsonObject& object, SnowCanvasSerialNumberStyle* style) {
    if (style == nullptr) return;
    QColor color;
    if (colorValue(object.value(QStringLiteral("color")), &color)) style->color = color;
    if (colorValue(object.value(QStringLiteral("fill")), &color)) style->fill = color;
    const QJsonValue numberValue = object.value(QStringLiteral("number"));
    if (numberValue.isDouble()) {
        bool ok = false;
        const qint64 number =
            QString::fromUtf8(numberValue.toJson(QJsonValue::JsonFormat::Compact))
                .toLongLong(&ok);
        if (ok && number >= 0) {
            style->number = number;
        }
    }
    readEnum(object, QStringLiteral("fill_style"), static_cast<int>(SnowCanvasFillStyle::Solid),
             &style->fillStyle);
    readDouble(object, QStringLiteral("font_size"), &style->fontSize);
    if (object.value(QStringLiteral("font_family")).isString())
        style->fontFamily = object.value(QStringLiteral("font_family")).toString();
    readDouble(object, QStringLiteral("stroke_width"), &style->strokeWidth);
    readEnum(object, QStringLiteral("stroke_style"), static_cast<int>(SnowCanvasStrokeStyle::Dotted),
             &style->strokeStyle);
    readDouble(object, QStringLiteral("opacity"), &style->opacity);
}

} // namespace

SnowCanvasStyleDefaults screenshotCanvasToolStyleDefaults() {
    SnowCanvasStyleDefaults defaults = screenshotCanvasStyleDefaults();
    auto& storage = storage::ApplicationStorage::instance();
    if (!storage.isInitialized()) return defaults;
    const auto& configuration = storage.configuration();
    readShapeValue(configuration.value(kShapeKey).toObject(), &defaults.rectangle);
    readShapeValue(configuration.value(kArrowKey).toObject(), &defaults.arrow);
    readShapeValue(configuration.value(kLineKey).toObject(), &defaults.line);
    readShapeValue(configuration.value(kFreeDrawKey).toObject(), &defaults.freeDraw);
    readShapeValue(configuration.value(kRectangleHighlightKey).toObject(),
                   &defaults.rectangleHighlight);
    readShapeValue(configuration.value(kPenHighlightKey).toObject(), &defaults.penHighlight);
    readFilterValue(configuration.value(kRectangleFilterKey).toObject(), &defaults.rectangleFilter);
    readFilterValue(configuration.value(kPenFilterKey).toObject(), &defaults.penFilter);
    readTextValue(configuration.value(kTextKey).toObject(), &defaults.text);
    readSerialNumberValue(configuration.value(kSerialNumberKey).toObject(), &defaults.serialNumber);
    const auto normalizeShape = [](SnowCanvasShapeStyle& style, bool rectangle) {
        style.strokeWidth = bounded(style.strokeWidth, 0.0, 72.0);
        style.cornerRadii.topLeft = bounded(style.cornerRadii.topLeft, 0.0, 83.0);
        style.cornerRadii.topRight = bounded(style.cornerRadii.topRight, 0.0, 83.0);
        style.cornerRadii.bottomRight = bounded(style.cornerRadii.bottomRight, 0.0, 83.0);
        style.cornerRadii.bottomLeft = bounded(style.cornerRadii.bottomLeft, 0.0, 83.0);
        style.opacity = bounded(style.opacity, 0.0, 1.0);
        if (rectangle) {
            style.startArrowhead = SnowCanvasArrowhead::None;
            style.endArrowhead = SnowCanvasArrowhead::None;
            style.arrowType = SnowCanvasArrowType::Straight;
        }
    };
    normalizeShape(defaults.rectangle, true);
    normalizeShape(defaults.arrow, false);
    normalizeShape(defaults.line, false);
    normalizeShape(defaults.freeDraw, false);
    normalizeShape(defaults.rectangleHighlight, false);
    normalizeShape(defaults.penHighlight, false);
    defaults.rectangleFilter.strength = bounded(defaults.rectangleFilter.strength, 0.0, 1.0);
    defaults.rectangleFilter.opacity = bounded(defaults.rectangleFilter.opacity, 0.0, 1.0);
    defaults.rectangleFilter.strokeWidth = bounded(defaults.rectangleFilter.strokeWidth, 1.0, 72.0);
    defaults.penFilter.strength = bounded(defaults.penFilter.strength, 0.0, 1.0);
    defaults.penFilter.opacity = bounded(defaults.penFilter.opacity, 0.0, 1.0);
    defaults.penFilter.strokeWidth = bounded(defaults.penFilter.strokeWidth, 1.0, 72.0);
    defaults.text.fontSize = bounded(defaults.text.fontSize, 6.0, 256.0);
    defaults.text.strokeWidth = bounded(defaults.text.strokeWidth, 0.0, 72.0);
    defaults.text.cornerRadii.topLeft = bounded(defaults.text.cornerRadii.topLeft, 0.0, 83.0);
    defaults.text.cornerRadii.topRight = bounded(defaults.text.cornerRadii.topRight, 0.0, 83.0);
    defaults.text.cornerRadii.bottomRight = bounded(defaults.text.cornerRadii.bottomRight, 0.0, 83.0);
    defaults.text.cornerRadii.bottomLeft = bounded(defaults.text.cornerRadii.bottomLeft, 0.0, 83.0);
    defaults.text.opacity = bounded(defaults.text.opacity, 0.0, 1.0);
    defaults.serialNumber.fontSize = bounded(defaults.serialNumber.fontSize, 6.0, 512.0);
    defaults.serialNumber.strokeWidth = bounded(defaults.serialNumber.strokeWidth, 0.0, 72.0);
    defaults.serialNumber.opacity = bounded(defaults.serialNumber.opacity, 0.0, 1.0);
    return defaults;
}

bool persistScreenshotCanvasToolStyles(const SnowCanvasStyleDefaults& defaults) {
    auto& storage = storage::ApplicationStorage::instance();
    if (!storage.isInitialized()) return false;
    const QMap<QString, QJsonValue> values{
        {kShapeKey, shapeValue(defaults.rectangle)},
        {kArrowKey, shapeValue(defaults.arrow)},
        {kLineKey, shapeValue(defaults.line)},
        {kFreeDrawKey, shapeValue(defaults.freeDraw)},
        {kRectangleHighlightKey, shapeValue(defaults.rectangleHighlight)},
        {kPenHighlightKey, shapeValue(defaults.penHighlight)},
        {kRectangleFilterKey, filterValue(defaults.rectangleFilter)},
        {kPenFilterKey, filterValue(defaults.penFilter)},
        {kTextKey, textValue(defaults.text)},
        {kSerialNumberKey, serialNumberValue(defaults.serialNumber)},
    };
    storage::ConfigurationStore& configuration = storage.configuration();
    if (QThread::currentThread() != configuration.thread()) {
        return false;
    }
    return configuration.setValues(values);
}

void applyScreenshotCanvasToolStyles(SnowCanvasWidget& canvas,
                                     const SnowCanvasStyleDefaults& defaults) {
    const SnowCanvasTool previousTool = canvas.canvasTool();
    // Style setters also update selected elements when a selection is active.
    // Refreshing creation defaults must never rewrite document content.
    if (!canvas.resetEditingState()) {
        return;
    }
    const auto applyShape = [&canvas](const SnowCanvasShapeStyle& style, quint32 properties,
                                      SnowCanvasShapeKind kind) {
        static_cast<void>(canvas.setCanvasShapeStylePatch(style, properties, kind));
    };
    applyShape(defaults.rectangle, kRectangleShapeProperties, SnowCanvasShapeKind::Rectangle);
    applyShape(defaults.arrow, kArrowShapeProperties, SnowCanvasShapeKind::Arrow);
    applyShape(defaults.line, kLineShapeProperties, SnowCanvasShapeKind::Line);
    applyShape(defaults.freeDraw, kLineShapeProperties, SnowCanvasShapeKind::FreeDraw);
    applyShape(defaults.rectangleHighlight, kRectangleHighlightProperties,
               SnowCanvasShapeKind::RectangleHighlight);
    applyShape(defaults.penHighlight, kPenHighlightProperties, SnowCanvasShapeKind::PenHighlight);
    static_cast<void>(canvas.setCanvasTextStyle(defaults.text));
    static_cast<void>(canvas.setCanvasSerialNumberStyle(defaults.serialNumber));
    static_cast<void>(canvas.setCanvasTool(SnowCanvasTool::RectangleFilter));
    static_cast<void>(canvas.setCanvasFilterStyle(defaults.rectangleFilter, kAllFilterProperties));
    static_cast<void>(canvas.setCanvasTool(SnowCanvasTool::PenFilter));
    static_cast<void>(canvas.setCanvasFilterStyle(defaults.penFilter, kAllFilterProperties));
    static_cast<void>(canvas.setCanvasTool(previousTool));
}

} // namespace snow_shot::presentation
