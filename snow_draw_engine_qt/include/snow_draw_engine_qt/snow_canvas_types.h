#pragma once

#include <QColor>
#include <QRectF>
#include <QString>
#include <QtGlobal>

#include <cstring>
#include <optional>
#include <QSet>

inline bool snowCanvasExactDoubleEqual(double lhs, double rhs) noexcept {
    return std::memcmp(&lhs, &rhs, sizeof(double)) == 0;
}

enum class SnowCanvasTool {
    Select,
    Shape,
    Arrow,
    Line,
    FreeDraw,
    RectangleHighlight,
    Highlight = RectangleHighlight,
    Eraser,
    RectangleFilter,
    Filter = RectangleFilter,
    Watermark,
    Text,
    SerialNumber,
    PenHighlight,
    PenFilter,
    Spotlight,
};

enum class SnowCanvasStyleToolbarSource {
    DefaultRectangle,
    SelectedRectangle,
    DefaultArrow,
    SelectedArrow,
    DefaultLine,
    SelectedLine,
    DefaultFreeDraw,
    SelectedFreeDraw,
    DefaultRectangleHighlight,
    DefaultHighlight = DefaultRectangleHighlight,
    SelectedRectangleHighlight,
    SelectedHighlight = SelectedRectangleHighlight,
    Eraser,
    DefaultRectangleFilter,
    DefaultFilter = DefaultRectangleFilter,
    SelectedRectangleFilter,
    SelectedFilter = SelectedRectangleFilter,
    Watermark,
    DefaultText,
    SelectedText,
    DefaultSerialNumber,
    SelectedSerialNumber,
    DefaultPenHighlight,
    SelectedPenHighlight,
    DefaultPenFilter,
    SelectedPenFilter,
    DefaultSpotlight,
    SelectedSpotlight,
};

enum SnowCanvasTextStyleMixedFlag : quint32 {
    SnowCanvasTextStyleMixedColor = 1u << 0,
    SnowCanvasTextStyleMixedFontSize = 1u << 1,
    SnowCanvasTextStyleMixedFontFamily = 1u << 2,
    SnowCanvasTextStyleMixedFill = 1u << 3,
    SnowCanvasTextStyleMixedFillStyle = 1u << 4,
    SnowCanvasTextStyleMixedStroke = 1u << 5,
    SnowCanvasTextStyleMixedStrokeWidth = 1u << 6,
    SnowCanvasTextStyleMixedCornerRadii = 1u << 7,
    SnowCanvasTextStyleMixedHorizontalAlign = 1u << 8,
    SnowCanvasTextStyleMixedVerticalAlign = 1u << 9,
    SnowCanvasTextStyleMixedOpacity = 1u << 10,
};

enum SnowCanvasSerialNumberStyleMixedFlag : quint32 {
    SnowCanvasSerialNumberStyleMixedNumber = 1u << 0,
    SnowCanvasSerialNumberStyleMixedColor = 1u << 1,
    SnowCanvasSerialNumberStyleMixedFill = 1u << 2,
    SnowCanvasSerialNumberStyleMixedFillStyle = 1u << 3,
    SnowCanvasSerialNumberStyleMixedFontSize = 1u << 4,
    SnowCanvasSerialNumberStyleMixedFontFamily = 1u << 5,
    SnowCanvasSerialNumberStyleMixedStrokeWidth = 1u << 6,
    SnowCanvasSerialNumberStyleMixedStrokeStyle = 1u << 7,
    SnowCanvasSerialNumberStyleMixedOpacity = 1u << 8,
};

enum SnowCanvasShapeStyleMixedFlag : quint32 {
    SnowCanvasShapeStyleMixedFill = 1u << 0,
    SnowCanvasShapeStyleMixedFillStyle = 1u << 1,
    SnowCanvasShapeStyleMixedStroke = 1u << 2,
    SnowCanvasShapeStyleMixedStrokeWidth = 1u << 3,
    SnowCanvasShapeStyleMixedCornerRadii = 1u << 4,
    SnowCanvasShapeStyleMixedStartArrowhead = 1u << 5,
    SnowCanvasShapeStyleMixedEndArrowhead = 1u << 6,
    SnowCanvasShapeStyleMixedStrokeStyle = 1u << 7,
    SnowCanvasShapeStyleMixedArrowType = 1u << 8,
    SnowCanvasShapeStyleMixedOpacity = 1u << 9,
    SnowCanvasShapeStyleMixedHighlightShape = 1u << 10,
    SnowCanvasShapeStyleMixedShape = 1u << 11,
};

enum class SnowCanvasSelectionOrder {
    SendToBack = 0,
    SendBackward = 1,
    BringForward = 2,
    BringToFront = 3,
};

enum SnowCanvasShapeStyleProperty : quint32 {
    SnowCanvasShapeStylePropertyFillColor = 1u << 0,
    SnowCanvasShapeStylePropertyFillStyle = 1u << 1,
    SnowCanvasShapeStylePropertyStrokeColor = 1u << 2,
    SnowCanvasShapeStylePropertyStrokeWidth = 1u << 3,
    SnowCanvasShapeStylePropertyCornerRadius = 1u << 4,
    SnowCanvasShapeStylePropertyStartArrowhead = 1u << 5,
    SnowCanvasShapeStylePropertyEndArrowhead = 1u << 6,
    SnowCanvasShapeStylePropertyStrokeStyle = 1u << 7,
    SnowCanvasShapeStylePropertyArrowType = 1u << 8,
    SnowCanvasShapeStylePropertyOpacity = 1u << 9,
    SnowCanvasShapeStylePropertyHighlightShape = 1u << 10,
    SnowCanvasShapeStylePropertyShape = 1u << 11,
};

enum class SnowCanvasShapeKind {
    Rectangle,
    Arrow,
    Line,
    FreeDraw,
    RectangleHighlight,
    Highlight = RectangleHighlight,
    PenHighlight,
    Spotlight,
};

enum class SnowCanvasHighlightShape {
    Rectangle,
    Ellipse,
};

enum class SnowCanvasRectangleShape {
    Rectangle,
    Ellipse,
    Diamond,
};

enum class SnowCanvasFilterType {
    Mosaic,
    GaussianBlur,
    Grayscale,
    Inversion,
};

enum SnowCanvasFilterStyleProperty : quint32 {
    SnowCanvasFilterStylePropertyType = 1u << 0,
    SnowCanvasFilterStylePropertyStrength = 1u << 1,
    SnowCanvasFilterStylePropertyOpacity = 1u << 2,
    SnowCanvasFilterStylePropertyStrokeWidth = 1u << 3,
};

struct SnowCanvasFilterStyle {
    SnowCanvasFilterType type = SnowCanvasFilterType::Mosaic;
    double strength = 0.5;
    double opacity = 1.0;
    double strokeWidth = 2.0;
};

inline bool operator==(const SnowCanvasFilterStyle& lhs, const SnowCanvasFilterStyle& rhs) {
    return lhs.type == rhs.type && snowCanvasExactDoubleEqual(lhs.strength, rhs.strength) &&
           snowCanvasExactDoubleEqual(lhs.opacity, rhs.opacity) &&
           snowCanvasExactDoubleEqual(lhs.strokeWidth, rhs.strokeWidth);
}

inline bool operator!=(const SnowCanvasFilterStyle& lhs, const SnowCanvasFilterStyle& rhs) {
    return !(lhs == rhs);
}

enum class SnowCanvasArrowhead {
    None,
    Arrow,
    Bar,
    Dot,
    Circle,
    CircleOutline,
    Triangle,
    TriangleOutline,
    Diamond,
    DiamondOutline,
    CrowfootOne,
    CrowfootMany,
    CrowfootOneOrMany,
};

enum class SnowCanvasArrowStrokeStyle {
    Solid,
    Dashed,
    Dotted,
};

enum class SnowCanvasArrowType {
    Straight,
    Curve,
    Elbow,
};

enum class SnowCanvasTextHorizontalAlign {
    Left,
    Center,
    Right,
};

enum class SnowCanvasTextVerticalAlign {
    Top,
    Center,
    Bottom,
};

enum class SnowCanvasFillStyle {
    Line,
    CrossLine,
    Solid,
};

enum class SnowCanvasStrokeStyle {
    Solid,
    Dashed,
    Dotted,
};

struct SnowCanvasCornerRadii {
    double topLeft = 0.0;
    double topRight = 0.0;
    double bottomRight = 0.0;
    double bottomLeft = 0.0;
};

inline bool operator==(const SnowCanvasCornerRadii& lhs, const SnowCanvasCornerRadii& rhs) {
    return snowCanvasExactDoubleEqual(lhs.topLeft, rhs.topLeft) &&
           snowCanvasExactDoubleEqual(lhs.topRight, rhs.topRight) &&
           snowCanvasExactDoubleEqual(lhs.bottomRight, rhs.bottomRight) &&
           snowCanvasExactDoubleEqual(lhs.bottomLeft, rhs.bottomLeft);
}

inline bool operator!=(const SnowCanvasCornerRadii& lhs, const SnowCanvasCornerRadii& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasShapeStyle {
    QColor fill;
    SnowCanvasFillStyle fillStyle = SnowCanvasFillStyle::Solid;
    QColor stroke;
    double strokeWidth = 0.0;
    SnowCanvasCornerRadii cornerRadii;
    SnowCanvasArrowhead startArrowhead = SnowCanvasArrowhead::None;
    SnowCanvasArrowhead endArrowhead = SnowCanvasArrowhead::None;
    SnowCanvasArrowStrokeStyle strokeStyle = SnowCanvasArrowStrokeStyle::Solid;
    SnowCanvasArrowType arrowType = SnowCanvasArrowType::Straight;
    double opacity = 1.0;
    SnowCanvasHighlightShape highlightShape = SnowCanvasHighlightShape::Rectangle;
    SnowCanvasRectangleShape shape = SnowCanvasRectangleShape::Rectangle;
};

inline bool operator==(const SnowCanvasShapeStyle& lhs, const SnowCanvasShapeStyle& rhs) {
    return lhs.fill == rhs.fill && lhs.fillStyle == rhs.fillStyle && lhs.stroke == rhs.stroke &&
           snowCanvasExactDoubleEqual(lhs.strokeWidth, rhs.strokeWidth) &&
           lhs.cornerRadii == rhs.cornerRadii && lhs.startArrowhead == rhs.startArrowhead &&
           lhs.endArrowhead == rhs.endArrowhead && lhs.strokeStyle == rhs.strokeStyle &&
           lhs.arrowType == rhs.arrowType && snowCanvasExactDoubleEqual(lhs.opacity, rhs.opacity) &&
           lhs.highlightShape == rhs.highlightShape && lhs.shape == rhs.shape;
}

inline bool operator!=(const SnowCanvasShapeStyle& lhs, const SnowCanvasShapeStyle& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasWatermarkConfig {
    QColor color = QColor(0, 0, 0, 255);
    QString text;
    double fontSize = 12.0;
    QString fontFamily;
    double angle = 30.0;
    double gap = 56.0;
    double opacity = 0.16;
};

struct SnowCanvasSpotlightConfig {
    QColor color = QColor(0, 0, 0, 255);
    double opacity = 0.64;
};

inline bool operator==(const SnowCanvasSpotlightConfig& lhs, const SnowCanvasSpotlightConfig& rhs) {
    return lhs.color == rhs.color && snowCanvasExactDoubleEqual(lhs.opacity, rhs.opacity);
}

inline bool operator!=(const SnowCanvasSpotlightConfig& lhs, const SnowCanvasSpotlightConfig& rhs) {
    return !(lhs == rhs);
}

inline bool operator==(const SnowCanvasWatermarkConfig& lhs, const SnowCanvasWatermarkConfig& rhs) {
    return lhs.color == rhs.color && lhs.text == rhs.text &&
           snowCanvasExactDoubleEqual(lhs.fontSize, rhs.fontSize) &&
           lhs.fontFamily == rhs.fontFamily && snowCanvasExactDoubleEqual(lhs.angle, rhs.angle) &&
           snowCanvasExactDoubleEqual(lhs.gap, rhs.gap) &&
           snowCanvasExactDoubleEqual(lhs.opacity, rhs.opacity);
}

inline bool operator!=(const SnowCanvasWatermarkConfig& lhs, const SnowCanvasWatermarkConfig& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasRectangleShapeStyle {
    QColor fill;
    SnowCanvasFillStyle fillStyle = SnowCanvasFillStyle::Solid;
    QColor stroke;
    double strokeWidth = 0.0;
    SnowCanvasStrokeStyle strokeStyle = SnowCanvasStrokeStyle::Solid;
    SnowCanvasCornerRadii cornerRadii{6.0, 6.0, 6.0, 6.0};
};

inline bool operator==(const SnowCanvasRectangleShapeStyle& lhs,
                       const SnowCanvasRectangleShapeStyle& rhs) {
    return lhs.fill == rhs.fill && lhs.fillStyle == rhs.fillStyle && lhs.stroke == rhs.stroke &&
           snowCanvasExactDoubleEqual(lhs.strokeWidth, rhs.strokeWidth) &&
           lhs.strokeStyle == rhs.strokeStyle && lhs.cornerRadii == rhs.cornerRadii;
}

inline bool operator!=(const SnowCanvasRectangleShapeStyle& lhs,
                       const SnowCanvasRectangleShapeStyle& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasArrowStyle {
    QColor stroke;
    double strokeWidth = 0.0;
    SnowCanvasArrowhead startArrowhead = SnowCanvasArrowhead::None;
    SnowCanvasArrowhead endArrowhead = SnowCanvasArrowhead::None;
    SnowCanvasArrowStrokeStyle strokeStyle = SnowCanvasArrowStrokeStyle::Solid;
    SnowCanvasArrowType arrowType = SnowCanvasArrowType::Straight;
};

inline bool operator==(const SnowCanvasArrowStyle& lhs, const SnowCanvasArrowStyle& rhs) {
    return lhs.stroke == rhs.stroke &&
           snowCanvasExactDoubleEqual(lhs.strokeWidth, rhs.strokeWidth) &&
           lhs.startArrowhead == rhs.startArrowhead && lhs.endArrowhead == rhs.endArrowhead &&
           lhs.strokeStyle == rhs.strokeStyle && lhs.arrowType == rhs.arrowType;
}

inline bool operator!=(const SnowCanvasArrowStyle& lhs, const SnowCanvasArrowStyle& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasTextStyle {
    QColor color{0xf4, 0x21, 0x2c};
    double fontSize = 30.0;
    QString fontFamily;
    QColor fill;
    SnowCanvasFillStyle fillStyle = SnowCanvasFillStyle::Solid;
    QColor stroke{0xff, 0xcc, 0xc7};
    double strokeWidth = 0.0;
    SnowCanvasCornerRadii cornerRadii{6.0, 6.0, 6.0, 6.0};
    SnowCanvasTextHorizontalAlign horizontalAlign = SnowCanvasTextHorizontalAlign::Left;
    SnowCanvasTextVerticalAlign verticalAlign = SnowCanvasTextVerticalAlign::Center;
    double opacity = 1.0;
};

inline bool operator==(const SnowCanvasTextStyle& lhs, const SnowCanvasTextStyle& rhs) {
    return lhs.color == rhs.color && snowCanvasExactDoubleEqual(lhs.fontSize, rhs.fontSize) &&
           lhs.fontFamily == rhs.fontFamily && lhs.fill == rhs.fill &&
           lhs.fillStyle == rhs.fillStyle && lhs.stroke == rhs.stroke &&
           snowCanvasExactDoubleEqual(lhs.strokeWidth, rhs.strokeWidth) &&
           lhs.cornerRadii == rhs.cornerRadii && lhs.horizontalAlign == rhs.horizontalAlign &&
           lhs.verticalAlign == rhs.verticalAlign &&
           snowCanvasExactDoubleEqual(lhs.opacity, rhs.opacity);
}

inline bool operator!=(const SnowCanvasTextStyle& lhs, const SnowCanvasTextStyle& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasSerialNumberStyle {
    qint64 number = 1;
    QColor color{0xf4, 0x21, 0x2c};
    QColor fill;
    SnowCanvasFillStyle fillStyle = SnowCanvasFillStyle::Solid;
    double fontSize = 24.0;
    QString fontFamily;
    double strokeWidth = 2.0;
    SnowCanvasStrokeStyle strokeStyle = SnowCanvasStrokeStyle::Solid;
    double opacity = 1.0;
};

inline bool operator==(const SnowCanvasSerialNumberStyle& lhs,
                       const SnowCanvasSerialNumberStyle& rhs) {
    return lhs.number == rhs.number && lhs.color == rhs.color && lhs.fill == rhs.fill &&
           lhs.fillStyle == rhs.fillStyle &&
           snowCanvasExactDoubleEqual(lhs.fontSize, rhs.fontSize) &&
           lhs.fontFamily == rhs.fontFamily &&
           snowCanvasExactDoubleEqual(lhs.strokeWidth, rhs.strokeWidth) &&
           lhs.strokeStyle == rhs.strokeStyle &&
           snowCanvasExactDoubleEqual(lhs.opacity, rhs.opacity);
}

inline bool operator!=(const SnowCanvasSerialNumberStyle& lhs,
                       const SnowCanvasSerialNumberStyle& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasStyleDefaults {
    SnowCanvasShapeStyle rectangle;
    SnowCanvasShapeStyle arrow;
    SnowCanvasShapeStyle line;
    SnowCanvasShapeStyle freeDraw;
    SnowCanvasShapeStyle rectangleHighlight;
    SnowCanvasShapeStyle penHighlight;
    SnowCanvasFilterStyle rectangleFilter;
    SnowCanvasFilterStyle penFilter;
    SnowCanvasTextStyle text;
    SnowCanvasSerialNumberStyle serialNumber;
    SnowCanvasWatermarkConfig watermark;
    SnowCanvasSpotlightConfig spotlight;
};

inline bool operator==(const SnowCanvasStyleDefaults& lhs, const SnowCanvasStyleDefaults& rhs) {
    return lhs.rectangle == rhs.rectangle && lhs.arrow == rhs.arrow && lhs.line == rhs.line &&
           lhs.freeDraw == rhs.freeDraw && lhs.rectangleHighlight == rhs.rectangleHighlight &&
           lhs.penHighlight == rhs.penHighlight && lhs.rectangleFilter == rhs.rectangleFilter &&
           lhs.penFilter == rhs.penFilter && lhs.text == rhs.text &&
           lhs.serialNumber == rhs.serialNumber && lhs.watermark == rhs.watermark &&
           lhs.spotlight == rhs.spotlight;
}

inline bool operator!=(const SnowCanvasStyleDefaults& lhs, const SnowCanvasStyleDefaults& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasRuntimeConfig {
    std::optional<SnowCanvasStyleDefaults> styleDefaults;
    QSet<SnowCanvasTool> quickSelectionDisabledTools;
};

struct SnowCanvasStyleToolbarState {
    SnowCanvasStyleToolbarSource source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    SnowCanvasShapeStyle shapeStyle;
    SnowCanvasTextStyle textStyle;
    SnowCanvasSerialNumberStyle serialNumberStyle;
    quint32 textStyleMixed = 0;
    quint32 serialNumberStyleMixed = 0;
    quint32 shapeStyleMixed = 0;
    SnowCanvasFilterStyle filterStyle;
    quint32 filterStyleMixed = 0;
};

inline bool operator==(const SnowCanvasStyleToolbarState& lhs,
                       const SnowCanvasStyleToolbarState& rhs) {
    return lhs.source == rhs.source && lhs.shapeStyle == rhs.shapeStyle &&
           lhs.textStyle == rhs.textStyle && lhs.serialNumberStyle == rhs.serialNumberStyle &&
           lhs.textStyleMixed == rhs.textStyleMixed &&
           lhs.serialNumberStyleMixed == rhs.serialNumberStyleMixed &&
           lhs.shapeStyleMixed == rhs.shapeStyleMixed && lhs.filterStyle == rhs.filterStyle &&
           lhs.filterStyleMixed == rhs.filterStyleMixed;
}

inline bool operator!=(const SnowCanvasStyleToolbarState& lhs,
                       const SnowCanvasStyleToolbarState& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasSerialNumberToolbarState {
    bool visible = false;
    QRectF geometry;
    bool canDecrease = false;
    bool canIncrease = false;
    bool canCreateText = false;
};

inline bool operator==(const SnowCanvasSerialNumberToolbarState& lhs,
                       const SnowCanvasSerialNumberToolbarState& rhs) {
    return lhs.visible == rhs.visible && lhs.geometry == rhs.geometry &&
           lhs.canDecrease == rhs.canDecrease && lhs.canIncrease == rhs.canIncrease &&
           lhs.canCreateText == rhs.canCreateText;
}

inline bool operator!=(const SnowCanvasSerialNumberToolbarState& lhs,
                       const SnowCanvasSerialNumberToolbarState& rhs) {
    return !(lhs == rhs);
}

struct SnowCanvasHistoryState {
    bool canUndo = false;
    bool canRedo = false;
};

struct SnowCanvasSnapConfig {
    bool enabled = false;
    bool enablePointSnaps = true;
    bool enableGapSnaps = true;
    bool showGuides = true;
    bool showGapSize = false;
    double distance = 8.0;
    QColor lineColor = QColor(255, 107, 107, 255);
    double lineWidth = 1.0;
    double markerSize = 8.0;
    double gapDashLength = 4.0;
    double gapDashGap = 4.0;
};

struct SnowCanvasGridConfig {
    bool enabled = false;
    double size = 20.0;
};
