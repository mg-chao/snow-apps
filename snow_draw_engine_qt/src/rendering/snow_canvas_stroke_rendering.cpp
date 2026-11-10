#include "snow_canvas_stroke_rendering.h"

#include "snow_canvas_display_item.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_type_conversions.h"
#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"

#include <QLineF>
#include <QPainterPathStroker>
#include <QPen>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace snow_canvas_stroke_rendering {
namespace {

thread_local Diagnostics diagnostics;
constexpr double kPi = 3.14159265358979323846;

std::uint64_t append(std::uint64_t hash, std::uint64_t value) {
    return hash ^ (value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2));
}

std::uint64_t appendDouble(std::uint64_t hash, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return append(hash, bits);
}

// Integer hashing gives stable independent samples, including seed zero. There
// is no frame-global PRNG state and no dependency on draw order or clipping.
double sample(std::uint32_t seed, std::uint32_t index) {
    std::uint32_t value = seed + 0x9e3779b9U * (index + 1U);
    value = (value ^ (value >> 16U)) * 0x85ebca6bU;
    value = (value ^ (value >> 13U)) * 0xc2b2ae35U;
    value ^= value >> 16U;
    return static_cast<double>(value) / 2147483647.5 - 1.0;
}

QPointF normal(const QPointF& delta) {
    const double length = std::hypot(delta.x(), delta.y());
    return length > 1e-9 ? QPointF(-delta.y() / length, delta.x() / length) : QPointF(0, 1);
}

struct Options {
    SnowStrokeRenderingStyle style = SNOW_STROKE_RENDERING_STYLE_ARCHITECT;
    SnowStrokeStyle pattern = SNOW_STROKE_STYLE_SOLID;
    std::uint32_t seed = 0;
    double width = 0;
    double smallAxis = std::numeric_limits<double>::infinity();
    double roughnessCap = -1;
    bool shapePattern = false;
    bool preserveVertices = true;
    bool ellipse = false;
    double ellipseAngle = 0;
};

double excursion(const Options& options, double scale) {
    double amount = snow_stroke_rendering_outset(options.style, std::min(scale, options.smallAxis));
    const double roughness = options.style == SNOW_STROKE_RENDERING_STYLE_CARTOONIST ? 2.0 : 1.0;
    if (options.roughnessCap >= 0)
        amount *= std::min(roughness, options.roughnessCap) / roughness;
    return amount;
}

QPainterPath variedEllipse(const QPainterPath& source, const Options& options, int pass) {
    const QRectF rect = source.boundingRect();
    const double rx = rect.width() / 2;
    const double ry = rect.height() / 2;
    if (!(rx > 0) || !(ry > 0))
        return source;
    const double limit = excursion(options, std::min(rx, ry) * 2);
    const double amplitude = limit * (pass == 0 ? 0.6 : 0.3);
    const std::uint32_t seed = options.seed ^ (pass == 0 ? 0x7f4a7c15U : 0x94d049bbU);
    const double wave2 = 0.55 * sample(seed, 0);
    const double wave3 = 0.3 * sample(seed, 1);
    const double wave1 = 0.15 * sample(seed, 2);
    const auto displacement = [&](const QPointF& point) {
        const double x = (point.x() - rect.center().x()) / rx;
        const double y = (point.y() - rect.center().y()) / ry;
        const double angle = std::atan2(y, x) - options.ellipseAngle;
        const double variation =
            wave2 * std::sin(2 * angle) + wave3 * std::cos(3 * angle) + wave1 * std::sin(angle);
        const QPointF gradient(x / rx, y / ry);
        const double length = std::hypot(gradient.x(), gradient.y());
        return length > 1e-12 ? gradient * (amplitude * variation / length) : QPointF();
    };
    // Differentiate the field along each endpoint tangent. Adjacent pieces
    // share these derivatives, retaining C1 continuity. Control offsets also
    // obey the excursion bound for very eccentric or tiny ellipses.
    const auto controlDisplacement = [&](const QPointF& point, const QPointF& tangent) {
        constexpr double step = 0.0001;
        const QPointF anchor = displacement(point);
        QPointF derivative =
            (displacement(point + tangent * step) - displacement(point - tangent * step)) /
            (2 * step);
        const double a = QPointF::dotProduct(derivative, derivative);
        const double b = 2 * std::abs(QPointF::dotProduct(anchor, derivative));
        const double c = QPointF::dotProduct(anchor, anchor) - limit * limit;
        if (a + b + c > 0 && a > 1e-24) {
            // Use the same bound for anchor +/- derivative so opposite
            // controls at a shared endpoint retain a common tangent.
            const double denominator = b + std::sqrt(std::max(0.0, b * b - 4 * a * c));
            derivative *= denominator > 0 ? std::clamp(-2 * c / denominator, 0.0, 1.0) : 0;
        }
        return anchor + derivative;
    };
    using Cubic = std::array<QPointF, 4>;
    const auto split = [](const Cubic& c, double t) {
        const QPointF a = c[0] + (c[1] - c[0]) * t;
        const QPointF b = c[1] + (c[2] - c[1]) * t;
        const QPointF d = c[2] + (c[3] - c[2]) * t;
        const QPointF e = a + (b - a) * t;
        const QPointF f = b + (d - b) * t;
        const QPointF m = e + (f - e) * t;
        return std::array<Cubic, 2>{{{{c[0], a, e, m}}, {{m, f, d, c[3]}}}};
    };
    QPainterPath path;
    QPointF at;
    for (int i = 0; i < source.elementCount(); ++i) {
        const auto element = source.elementAt(i);
        if (element.isMoveTo()) {
            at = QPointF(element.x, element.y);
            path.moveTo(at + displacement(at));
            continue;
        }
        if (element.type != QPainterPath::CurveToElement || i + 2 >= source.elementCount())
            continue;
        const auto c2 = source.elementAt(i + 1);
        const auto end = source.elementAt(i + 2);
        const Cubic cubic{
            {at, QPointF(element.x, element.y), QPointF(c2.x, c2.y), QPointF(end.x, end.y)}};
        const auto firstSplit = split(cubic, 1.0 / 3.0);
        const auto secondSplit = split(firstSplit[1], 0.5);
        for (const Cubic& piece : {firstSplit[0], secondSplit[0], secondSplit[1]}) {
            path.cubicTo(piece[1] + controlDisplacement(piece[0], piece[1] - piece[0]),
                         piece[2] + controlDisplacement(piece[3], piece[2] - piece[3]),
                         piece[3] + displacement(piece[3]));
        }
        at = cubic[3];
        i += 2;
    }
    path.closeSubpath();
    return path;
}

// Each base segment stays one cubic. Its control polygon is bounded by the
// central excursion contract, so generation is independent of image size/DPR.
QPainterPath variedPath(const QPainterPath& source, const Options& options, int pass) {
    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    QPointF at;
    QPointF originalAt;
    QPointF originalStart;
    QPointF start;
    std::uint32_t segment = 0;
    const std::uint32_t seed = options.seed ^ (pass == 0 ? 0x68bc21ebU : 0x02e5be93U);
    const double passGain = pass == 0 ? 0.98 : 0.5;
    const auto displacedVertex = [&](const QPointF& p, const QPointF& delta, double scale,
                                     std::uint32_t index) {
        if (options.preserveVertices)
            return p;
        return p + normal(delta) * (excursion(options, scale) * 0.22 * sample(seed, index));
    };
    for (int i = 0; i < source.elementCount(); ++i) {
        const auto e = source.elementAt(i);
        const QPointF p(e.x, e.y);
        if (e.isMoveTo()) {
            originalAt = originalStart = p;
            QPointF delta(1, 0);
            if (i + 1 < source.elementCount()) {
                const auto next = source.elementAt(i + 1);
                delta = QPointF(next.x, next.y) - p;
            }
            at = start = displacedVertex(p, delta, QLineF(QPointF(), delta).length(), segment * 8);
            result.moveTo(at);
            continue;
        }
        QPointF end = p;
        QPointF control1;
        QPointF control2;
        const bool cubic = e.type == QPainterPath::CurveToElement && i + 2 < source.elementCount();
        if (cubic) {
            const auto c2 = source.elementAt(i + 1);
            const auto e2 = source.elementAt(i + 2);
            control1 = p;
            control2 = QPointF(c2.x, c2.y);
            end = QPointF(e2.x, e2.y);
            i += 2;
        } else if (!e.isLineTo()) {
            continue;
        }
        const QPointF delta = end - originalAt;
        const double span = std::max({QLineF(originalAt, end).length(),
                                      cubic ? QLineF(originalAt, control1).length() : 0.0,
                                      cubic ? QLineF(originalAt, control2).length() : 0.0});
        if (span <= 1e-9) {
            originalAt = end;
            continue;
        }
        const QPointF n = normal(delta);
        const double amplitude = excursion(options, span) * passGain;
        const double bendSample = sample(options.seed ^ 0x632be59bU, segment * 8 + 5);
        const double bend = std::copysign(0.88 + 0.04 * std::abs(bendSample), bendSample) *
                            (pass == 0 ? 1 : -1) * (cubic ? 0.65 : 1.0);
        const auto jitter = [&](std::uint32_t index) {
            return n * (amplitude * (bend + 0.06 * sample(seed, index)));
        };
        if (!cubic) {
            control1 = originalAt + delta * (0.28 + 0.04 * sample(seed, segment * 8 + 1));
            control2 = originalAt + delta * (0.71 + 0.04 * sample(seed, segment * 8 + 2));
        }
        const bool closes = QLineF(end, originalStart).length() < 1e-8;
        const QPointF variedEnd =
            closes ? start : displacedVertex(end, delta, span, (segment + 1) * 8);
        // Two independently bowed traces meet at shared vertices. Continuous
        // rounded contours and arrow endpoints retain their exact joins.
        result.cubicTo(control1 + jitter(segment * 8 + 3), control2 + jitter(segment * 8 + 4),
                       variedEnd);
        at = variedEnd;
        originalAt = end;
        if (closes)
            result.closeSubpath();
        ++segment;
    }
    return result;
}

void mergeCoverage(QPainterPath& destination, const QPainterPath& coverage) {
    if (coverage.isEmpty())
        return;
    if (destination.isEmpty()) {
        destination = coverage;
    } else {
        destination = destination.united(coverage);
        ++diagnostics.coverageUnions;
    }
    destination.setFillRule(Qt::WindingFill);
}

template <typename Stroke> void configureShapeStroke(Stroke& stroke, SnowStrokeStyle style) {
    stroke.setCapStyle(style == SNOW_STROKE_STYLE_DASHED || style == SNOW_STROKE_STYLE_DOTTED
                           ? Qt::RoundCap
                           : Qt::FlatCap);
    if (style == SNOW_STROKE_STYLE_DASHED)
        stroke.setDashPattern(QVector<qreal>{4.0, 4.0});
    else if (style == SNOW_STROKE_STYLE_DOTTED)
        stroke.setDashPattern(QVector<qreal>{0.01, 3.0});
}

void addStroke(Cache& cache, const QPainterPath& source, const Options& options) {
    if (source.isEmpty() || !(options.width > 0) || !std::isfinite(options.width))
        return;
    QPainterPathStroker stroker;
    stroker.setWidth(options.width);
    stroker.setJoinStyle(Qt::RoundJoin);
    if (options.shapePattern) {
        configureShapeStroke(stroker, options.pattern);
    } else {
        stroker.setCapStyle(Qt::RoundCap);
        if (options.pattern == SNOW_STROKE_STYLE_DASHED)
            stroker.setDashPattern(QVector<qreal>{4.0, 2.0});
        else if (options.pattern == SNOW_STROKE_STYLE_DOTTED)
            stroker.setDashPattern(QVector<qreal>{1.0, 2.0});
    }
    const int passes = options.pattern == SNOW_STROKE_STYLE_SOLID &&
                               options.style != SNOW_STROKE_RENDERING_STYLE_ARCHITECT
                           ? 2
                           : 1;
    for (int pass = 0; pass < passes; ++pass) {
        const QPainterPath path = options.ellipse ? variedEllipse(source, options, pass)
                                                  : variedPath(source, options, pass);
        const QPainterPath coverage = stroker.createStroke(path);
        if (cache.strokeCoverageOnly) {
            // Qt's stroker normalizes outer/inner winding. Appending those
            // contours gives union coverage in one fill without the costly
            // flattening and polygon clipping of a boolean path union.
            cache.ink.addPath(coverage);
            cache.ink.setFillRule(Qt::WindingFill);
        } else {
            mergeCoverage(cache.ink, coverage);
        }
        ++diagnostics.strokeBuilds;
    }
}

QPainterPath rectanglePath(double width, double height, const SnowCornerRadii& radii,
                           SnowDisplayRectShape shape) {
    const QRectF rect(-width / 2, -height / 2, width, height);
    QPainterPath path;
    if (shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE)
        path.addEllipse(rect);
    else if (shape == SNOW_DISPLAY_RECT_SHAPE_DIAMOND)
        path = snow_canvas_render_geometry::roundedDiamondPath(rect, radii);
    else
        path = snow_canvas_render_geometry::roundedRectPath(
            rect, snow_canvas_render_geometry::toViewCornerRadii(radii, 1, rect));
    return path;
}

template <typename Item>
std::uint64_t geometryKey(const Item& item, bool rectangle, SnowStrokeStyle pattern,
                          std::uint64_t revision, std::uint64_t pathRevision,
                          SnowDisplayRectShape shape) {
    std::uint64_t hash = append(0x8cb92baa9d28ab3bULL, item.stroke_rendering_seed);
    for (double value : {item.width, item.height, item.stroke_width, item.corner_radii.top_left,
                         item.corner_radii.top_right, item.corner_radii.bottom_right,
                         item.corner_radii.bottom_left})
        hash = appendDouble(hash, value);
    hash = append(hash, static_cast<std::uint64_t>(item.stroke_rendering_style));
    hash = append(hash, static_cast<std::uint64_t>(pattern));
    hash = append(hash, static_cast<std::uint64_t>(shape));
    hash = append(hash, static_cast<std::uint64_t>(item.kind));
    hash = append(hash, rectangle);
    if (!rectangle) {
        for (double value : {item.center_x, item.center_y, item.rotation, item.arrow_ratio,
                             item.arrow_outline_width})
            hash = appendDouble(hash, value);
        hash = append(hash, revision);
        hash = append(hash, pathRevision);
        hash = append(hash, static_cast<std::uint64_t>(item.arrow_type));
        hash = append(hash, static_cast<std::uint64_t>(item.arrow_shaft_type));
        hash = append(hash, static_cast<std::uint64_t>(item.arrow_start_head));
        hash = append(hash, static_cast<std::uint64_t>(item.arrow_end_head));
    }
    return hash;
}

template <typename Item> QPainterPath arrowPath(const Item& item) {
    const snow_canvas_render_geometry::ViewProjection identity{};
    QPainterPath path = snow_canvas_render_geometry::arrowPathFromCommands(
        identity, item.arrow_path_commands, item.arrow_path_command_count);
    if (path.isEmpty()) {
        const auto points = snow_canvas_render_geometry::arrowPointsToView(
            identity, item.arrow_points, item.arrow_point_count);
        path = snow_canvas_render_geometry::arrowPathForPoints(points, item.arrow_type, 1);
    }
    return path;
}

void addHead(Cache& cache, const SnowArrowheadPrimitive& primitive, const QTransform& toLocal,
             Options options) {
    QPainterPath path;
    if (primitive.kind == SNOW_ARROWHEAD_PRIMITIVE_CIRCLE) {
        if (!(primitive.diameter > 0))
            return;
        path.addEllipse(QRectF(primitive.center.x - primitive.diameter / 2,
                               primitive.center.y - primitive.diameter / 2, primitive.diameter,
                               primitive.diameter));
        options.smallAxis = primitive.diameter;
        options.ellipse = true;
    } else {
        const auto count =
            std::min<std::uint32_t>(primitive.point_count, SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY);
        if (count < 2)
            return;
        path.moveTo(primitive.points[0].x, primitive.points[0].y);
        for (std::uint32_t i = 1; i < count; ++i)
            path.lineTo(primitive.points[i].x, primitive.points[i].y);
        if (primitive.kind == SNOW_ARROWHEAD_PRIMITIVE_POLYGON)
            path.closeSubpath();
    }
    path = toLocal.map(path);
    options.preserveVertices = true;
    options.roughnessCap = primitive.roughness_cap;
    if (primitive.dash_mode == SNOW_ARROWHEAD_DASH_SOLID)
        options.pattern = SNOW_STROKE_STYLE_SOLID;
    else if (primitive.dash_mode == SNOW_ARROWHEAD_DASH_DOTTED_CAP)
        options.pattern = SNOW_STROKE_STYLE_DOTTED;
    if (primitive.kind != SNOW_ARROWHEAD_PRIMITIVE_LINE) {
        if (primitive.fill_mode == SNOW_ARROWHEAD_FILL_BACKGROUND) {
            mergeCoverage(cache.backgroundHeads, path);
            if (!cache.ink.isEmpty()) {
                cache.ink = cache.ink.subtracted(path);
                cache.strokeCoverageOnly = false;
                ++diagnostics.coverageUnions;
            }
        } else {
            mergeCoverage(cache.ink, path);
            cache.strokeCoverageOnly = false;
        }
    }
    addStroke(cache, path, options);
}

// Tapered shafts are a single filled ribbon. Apply a smooth displacement field
// to both edges, keeping the tips and head/neck vertices coherent. This avoids
// a doubled silhouette and keeps arbitrarily dense contour samples smooth.
QPainterPath variedRibbon(const QPainterPath& source, const Options& options, const QPointF& first,
                          const QPointF& last, const QPointF& firstTangent) {
    QPointF axis = last - first;
    double length = QLineF(first, last).length();
    double extent = 0;
    for (int i = 0; i < source.elementCount(); ++i) {
        const auto e = source.elementAt(i);
        extent = std::max(extent, QLineF(first, QPointF(e.x, e.y)).length());
    }
    const bool loop = length < extent * 0.05;
    if (loop && extent > 1e-6) {
        axis = firstTangent;
        if (std::hypot(axis.x(), axis.y()) <= 1e-6) {
            for (int i = 0; i < source.elementCount(); ++i) {
                const auto e = source.elementAt(i);
                axis = QPointF(e.x, e.y) - first;
                if (std::hypot(axis.x(), axis.y()) > 1e-6)
                    break;
            }
        }
        const double tangentLength = std::hypot(axis.x(), axis.y());
        if (tangentLength <= 1e-6)
            return source;
        axis /= tangentLength;
        length = 0;
        for (int i = 0; i < source.elementCount(); ++i) {
            const auto e = source.elementAt(i);
            length =
                std::max(length, std::abs(QPointF::dotProduct(QPointF(e.x, e.y) - first, axis)));
        }
        axis *= length;
    }
    if (length < 1e-6)
        return source;
    const QPointF tangent = axis / length;
    const QPointF n = normal(axis);
    const double amount = excursion(options, length) * 0.85;
    QPainterPath result = source;
    for (int i = 0; i < source.elementCount(); ++i) {
        const auto e = source.elementAt(i);
        const QPointF p(e.x, e.y);
        const QPointF relative = p - first;
        const double along = QPointF::dotProduct(relative, tangent) / length;
        const double t = std::clamp(loop ? std::abs(along) : along, 0.0, 1.0);
        // The first/last 20% includes the joined head and neck. The envelope
        // and its derivative vanish there, preserving all shared vertices.
        const double u = std::clamp((t - 0.2) / 0.6, 0.0, 1.0);
        const double envelope = std::sin(kPi * u);
        const double offset =
            amount * envelope * envelope *
            (0.7 * sample(options.seed, 0) + 0.3 * sample(options.seed, 1) * std::sin(2 * kPi * u));
        const QPointF moved = p + n * offset;
        result.setElementPositionAt(i, moved.x(), moved.y());
    }
    result.setFillRule(Qt::WindingFill);
    return result;
}

template <typename Item>
void addFallbackHead(Cache& cache, const Item& item, const QTransform& toLocal, bool atStart,
                     Options options) {
    const SnowArrowhead head = atStart ? item.arrow_start_head : item.arrow_end_head;
    if (head == SNOW_ARROWHEAD_NONE)
        return;
    const snow_canvas_render_geometry::ViewProjection identity{};
    const auto points = snow_canvas_render_geometry::arrowPointsToView(identity, item.arrow_points,
                                                                       item.arrow_point_count);
    QPointF endpoint;
    QPointF previous;
    QPointF direction;
    double length = 0;
    if (!snow_canvas_render_geometry::arrowEndpointGeometry(
            points, item.arrow_type, atStart, &endpoint, &previous, &direction, &length))
        return;
    const QPointF localDirection = toLocal.map(direction) - toLocal.map(QPointF());
    options.ellipseAngle = std::atan2(localDirection.y(), localDirection.x());
    const double size = std::min(snow_canvas_render_geometry::arrowheadSize(head) *
                                     snowCanvasNormalizeArrowRatio(item.arrow_ratio),
                                 length * 0.5);
    const QPointF base = endpoint - direction * size;
    const double angle = snow_canvas_render_geometry::arrowheadAngleDegrees(head) * kPi / 180;
    const QPointF a = snow_canvas_render_geometry::rotatePoint(base, endpoint, -angle);
    const QPointF b = snow_canvas_render_geometry::rotatePoint(base, endpoint, angle);
    SnowArrowheadPrimitive primitive{};
    primitive.kind = SNOW_ARROWHEAD_PRIMITIVE_POLYGON;
    primitive.fill_mode = SNOW_ARROWHEAD_FILL_STROKE;
    primitive.dash_mode = item.arrow_stroke_style == SNOW_STROKE_STYLE_DOTTED
                              ? SNOW_ARROWHEAD_DASH_DOTTED_CAP
                              : SNOW_ARROWHEAD_DASH_SOLID;
    primitive.roughness_cap = 1;
    const auto setPoints = [&](std::initializer_list<QPointF> vertices) {
        primitive.point_count = 0;
        for (const auto& p : vertices)
            primitive.points[primitive.point_count++] = SnowArrowPoint{p.x(), p.y()};
    };
    switch (head) {
    case SNOW_ARROWHEAD_CIRCLE_OUTLINE:
        primitive.fill_mode = SNOW_ARROWHEAD_FILL_BACKGROUND;
        [[fallthrough]];
    case SNOW_ARROWHEAD_DOT:
    case SNOW_ARROWHEAD_CIRCLE:
        primitive.kind = SNOW_ARROWHEAD_PRIMITIVE_CIRCLE;
        primitive.center = {endpoint.x(), endpoint.y()};
        primitive.diameter = size;
        primitive.roughness_cap = 0.5;
        primitive.dash_mode = SNOW_ARROWHEAD_DASH_SOLID;
        break;
    case SNOW_ARROWHEAD_TRIANGLE_OUTLINE:
        primitive.fill_mode = SNOW_ARROWHEAD_FILL_BACKGROUND;
        [[fallthrough]];
    case SNOW_ARROWHEAD_TRIANGLE:
    case SNOW_ARROWHEAD_INVERTED_TRIANGLE:
        setPoints({endpoint, a, b, endpoint});
        primitive.dash_mode = SNOW_ARROWHEAD_DASH_SOLID;
        break;
    case SNOW_ARROWHEAD_INDENTED_TRIANGLE:
        setPoints({endpoint, a, (a + b) * 0.375 + endpoint * 0.25, b, endpoint});
        primitive.dash_mode = SNOW_ARROWHEAD_DASH_SOLID;
        break;
    case SNOW_ARROWHEAD_DIAMOND_OUTLINE:
        primitive.fill_mode = SNOW_ARROWHEAD_FILL_BACKGROUND;
        [[fallthrough]];
    case SNOW_ARROWHEAD_DIAMOND:
    case SNOW_ARROWHEAD_SQUARE:
        setPoints({endpoint, a, endpoint - direction * (size * 2), b, endpoint});
        primitive.dash_mode = SNOW_ARROWHEAD_DASH_SOLID;
        break;
    case SNOW_ARROWHEAD_BAR:
    case SNOW_ARROWHEAD_CROWFOOT_ONE:
        primitive.kind = SNOW_ARROWHEAD_PRIMITIVE_LINE;
        setPoints({a, b});
        break;
    default:
        primitive.kind = SNOW_ARROWHEAD_PRIMITIVE_LINE;
        if (head == SNOW_ARROWHEAD_CROWFOOT_MANY || head == SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY) {
            setPoints({snow_canvas_render_geometry::rotatePoint(endpoint, base, -angle), base});
            addHead(cache, primitive, toLocal, options);
            setPoints({snow_canvas_render_geometry::rotatePoint(endpoint, base, angle), base});
            if (head == SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY) {
                addHead(cache, primitive, toLocal, options);
                setPoints({a, b});
            }
        } else {
            setPoints({a, endpoint});
            addHead(cache, primitive, toLocal, options);
            setPoints({b, endpoint});
        }
        break;
    }
    addHead(cache, primitive, toLocal, options);
}

template <typename Item> void buildArrow(Cache& cache, const Item& item, QPainterPath path) {
    const QTransform toLocal =
        localToCanvas(item.center_x, item.center_y, item.rotation).inverted();
    path = toLocal.map(path);
    Options options{item.stroke_rendering_style, item.arrow_stroke_style,
                    item.stroke_rendering_seed, item.stroke_width};
    const bool tapered = item.arrow_shaft_type == SNOW_ARROW_SHAFT_TYPE_TAPERED;
    if (tapered) {
        QPointF first;
        QPointF last;
        QPointF firstTangent;
        if (item.arrow_points != nullptr && item.arrow_point_count >= 2) {
            first = toLocal.map(QPointF(item.arrow_points[0].x, item.arrow_points[0].y));
            const auto& endpoint = item.arrow_points[item.arrow_point_count - 1];
            last = toLocal.map(QPointF(endpoint.x, endpoint.y));
            for (std::uint32_t i = 1; i < item.arrow_point_count; ++i) {
                const auto& point = item.arrow_points[i];
                firstTangent = toLocal.map(QPointF(point.x, point.y)) - first;
                if (std::hypot(firstTangent.x(), firstTangent.y()) > 1e-6)
                    break;
            }
        }
        const QPainterPath ribbon = variedRibbon(path, options, first, last, firstTangent);
        const auto destination = item.arrow_end_head == SNOW_ARROWHEAD_NONE ? item.arrow_start_head
                                                                            : item.arrow_end_head;
        if (destination == SNOW_ARROWHEAD_TRIANGLE_OUTLINE) {
            // A hollow ribbon has one outline, using the same coherent field.
            options.style = SNOW_STROKE_RENDERING_STYLE_ARCHITECT;
            options.roughnessCap = 0;
            addStroke(cache, ribbon, options);
            if (options.pattern != SNOW_STROKE_STYLE_SOLID) {
                const QPointF tip = item.arrow_end_head == SNOW_ARROWHEAD_NONE ? first : last;
                for (int i = 1; i + 1 < ribbon.elementCount(); ++i) {
                    const auto p = ribbon.elementAt(i);
                    if (QLineF(QPointF(p.x, p.y), tip).length() > 1e-8)
                        continue;
                    const auto before = ribbon.elementAt(i - 1);
                    const auto after = ribbon.elementAt(i + 1);
                    QPainterPath headEdges;
                    headEdges.moveTo(before.x, before.y);
                    headEdges.lineTo(tip);
                    headEdges.lineTo(after.x, after.y);
                    options.pattern = SNOW_STROKE_STYLE_SOLID;
                    addStroke(cache, headEdges, options);
                    break;
                }
            }
        } else {
            mergeCoverage(cache.ink, ribbon);
            cache.strokeCoverageOnly = false;
        }
    } else {
        const bool closed =
            path.elementCount() > 2 &&
            QLineF(path.currentPosition(), QPointF(path.elementAt(0).x, path.elementAt(0).y))
                    .length() < 1e-8;
        if (closed) {
            cache.originalFill = path;
            cache.originalFill.closeSubpath();
        }
        addStroke(cache, path, options);
    }
    const auto count =
        std::min<std::uint32_t>(item.arrowhead_primitive_count, SNOW_ARROWHEAD_PRIMITIVE_CAPACITY);
    for (std::uint32_t i = 0; item.arrowhead_primitives != nullptr && i < count; ++i) {
        options.seed = item.stroke_rendering_seed ^ (0x517cc1b7U * (i + 1));
        if (item.arrowhead_primitives[i].kind == SNOW_ARROWHEAD_PRIMITIVE_CIRCLE &&
            item.arrow_points != nullptr && item.arrow_point_count >= 2) {
            const snow_canvas_render_geometry::ViewProjection identity{};
            const auto points = snow_canvas_render_geometry::arrowPointsToView(
                identity, item.arrow_points, item.arrow_point_count);
            const auto center = item.arrowhead_primitives[i].center;
            const QPointF headCenter(center.x, center.y);
            QPointF endpoint;
            QPointF previous;
            QPointF direction;
            double length = 0;
            const bool atStart = QLineF(headCenter, points.front()).length() <
                                 QLineF(headCenter, points.back()).length();
            if (snow_canvas_render_geometry::arrowEndpointGeometry(
                    points, item.arrow_type, atStart, &endpoint, &previous, &direction, &length)) {
                const QPointF localDirection = toLocal.map(direction) - toLocal.map(QPointF());
                options.ellipseAngle = std::atan2(localDirection.y(), localDirection.x());
            }
        }
        addHead(cache, item.arrowhead_primitives[i], toLocal, options);
    }
    if (count == 0 && !tapered) {
        options.seed = item.stroke_rendering_seed ^ 0x517cc1b7U;
        addFallbackHead(cache, item, toLocal, true, options);
        options.seed ^= 0x7f4a7c15U;
        addFallbackHead(cache, item, toLocal, false, options);
    }
}

void buildPathGeometry(Cache& cache, const SnowCanvasSceneItem& item, QPainterPath path) {
    if (item.kind != SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE) {
        buildArrow(cache, item, path);
        return;
    }
    cache.originalFill =
        localToCanvas(item.center_x, item.center_y, item.rotation).inverted().map(path);
    cache.originalFill.setFillRule(Qt::WindingFill);
    Options options{item.stroke_rendering_style, item.stroke_style, item.stroke_rendering_seed,
                    item.stroke_width};
    options.shapePattern = true;
    options.smallAxis = std::min(item.width, item.height);
    addStroke(cache, cache.originalFill, options);
}

void buildPathGeometry(Cache& cache, const SnowCanvasOverlayItem& item, QPainterPath path) {
    buildArrow(cache, item, path);
}

template <typename Item>
const Cache& prepare(const Item& item, bool rectangle, SnowStrokeStyle pattern,
                     SnowDisplayRectShape shape, std::uint64_t pathRevision,
                     QPainterPath (*buildPath)(const Item&)) {
    const auto key = geometryKey(item, rectangle, pattern, item.strokeRenderingGeometryRevision(),
                                 pathRevision, shape);
    const Cache* existing = item.strokeRenderingCacheIfPresent();
    if (existing != nullptr && existing->valid && existing->key == key) {
        ++diagnostics.cacheHits;
        return *existing;
    }
    Cache& cache = item.strokeRenderingCache();
    cache = {};
    cache.key = key;
    cache.valid = true;
    ++diagnostics.geometryBuilds;
    if (rectangle) {
        cache.originalFill = rectanglePath(item.width, item.height, item.corner_radii, shape);
        Options options{item.stroke_rendering_style, pattern, item.stroke_rendering_seed,
                        item.stroke_width};
        options.shapePattern = true;
        options.ellipse = shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE;
        options.smallAxis = std::min(item.width, item.height);
        const bool rounded = item.corner_radii.top_left > 0 || item.corner_radii.top_right > 0 ||
                             item.corner_radii.bottom_left > 0 ||
                             item.corner_radii.bottom_right > 0;
        options.preserveVertices =
            rounded || shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE ||
            item.stroke_rendering_style != SNOW_STROKE_RENDERING_STYLE_CARTOONIST;
        addStroke(cache, cache.originalFill, options);
    } else {
        buildPathGeometry(cache, item, buildPath(item));
    }
    if (!rectangle && item.arrow_outline_width > 0.0 && !cache.ink.isEmpty()) {
        QPainterPathStroker stroker;
        stroker.setWidth(item.arrow_outline_width);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        cache.arrowOutline = stroker.createStroke(cache.ink);
    }
    for (const QPainterPath* path :
         {&cache.originalFill, &cache.backgroundHeads, &cache.ink, &cache.arrowOutline}) {
        if (!path->isEmpty())
            cache.paintBounds = cache.paintBounds.isEmpty()
                                    ? path->boundingRect()
                                    : cache.paintBounds.united(path->boundingRect());
    }
    return cache;
}

QPainterPath sceneArrowPath(const SnowCanvasSceneItem& item) {
    if (item.kind == SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE && !item.aggregateClosedPath().isEmpty())
        return item.aggregateClosedPath();
    if (item.pathChunks().empty())
        return arrowPath(item);
    QPainterPath path;
    for (const auto& chunk : item.pathChunks()) {
        const auto& part = chunk.canvasPath;
        if (part.isEmpty())
            continue;
        const auto first = part.elementAt(0);
        if (!path.isEmpty() &&
            QLineF(path.currentPosition(), QPointF(first.x, first.y)).length() < 1e-8)
            path.connectPath(part);
        else
            path.addPath(part);
    }
    return path;
}

} // namespace

Diagnostics takeDiagnostics() {
    const auto result = diagnostics;
    diagnostics = {};
    return result;
}

void recordCacheAllocation(bool detach) {
    ++diagnostics.cacheAllocations;
    diagnostics.cacheDetaches += detach ? 1 : 0;
}

void applyShapeStrokeStyle(QPen& pen, SnowStrokeStyle style) {
    pen.setStyle(Qt::SolidLine);
    configureShapeStroke(pen, style);
}

const Cache& geometry(const SnowCanvasSceneItem& item) {
    const bool serial = item.kind == SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER;
    const bool rectangle = item.kind == SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT || serial;
    const auto shape = serial ? (item.serial_number_type == SNOW_SERIAL_NUMBER_TYPE_OUTLINED_SQUARE
                                     ? SNOW_DISPLAY_RECT_SHAPE_RECTANGLE
                                     : SNOW_DISPLAY_RECT_SHAPE_ELLIPSE)
                              : static_cast<SnowDisplayRectShape>(item.rect_shape);
    const auto pattern = rectangle || item.kind == SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE
                             ? item.stroke_style
                             : item.arrow_stroke_style;
    return prepare(item, rectangle, pattern, shape, 0, sceneArrowPath);
}

const Cache& geometry(const SnowCanvasOverlayItem& item) {
    return prepare(item, item.kind == SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT, item.arrow_stroke_style,
                   SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, 0, arrowPath<SnowCanvasOverlayItem>);
}

const Cache* cachedGeometry(const SnowCanvasSceneItem& item) {
    const Cache* cache = item.strokeRenderingCacheIfPresent();
    if (cache == nullptr || !cache->valid)
        return nullptr;
    const bool serial = item.kind == SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER;
    const bool rectangle = item.kind == SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT || serial;
    const auto shape = serial ? (item.serial_number_type == SNOW_SERIAL_NUMBER_TYPE_OUTLINED_SQUARE
                                     ? SNOW_DISPLAY_RECT_SHAPE_RECTANGLE
                                     : SNOW_DISPLAY_RECT_SHAPE_ELLIPSE)
                              : static_cast<SnowDisplayRectShape>(item.rect_shape);
    const auto pattern = rectangle || item.kind == SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE
                             ? item.stroke_style
                             : item.arrow_stroke_style;
    const auto key =
        geometryKey(item, rectangle, pattern, item.strokeRenderingGeometryRevision(), 0, shape);
    return cache->key == key ? cache : nullptr;
}

const Cache* cachedGeometry(const SnowCanvasOverlayItem& item) {
    const Cache* cache = item.strokeRenderingCacheIfPresent();
    if (cache == nullptr || !cache->valid)
        return nullptr;
    const auto key =
        geometryKey(item, item.kind == SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT, item.arrow_stroke_style,
                    item.strokeRenderingGeometryRevision(), 0, SNOW_DISPLAY_RECT_SHAPE_RECTANGLE);
    return cache->key == key ? cache : nullptr;
}

QTransform localToCanvas(double centerX, double centerY, double rotation) {
    QTransform transform;
    transform.translate(centerX, centerY);
    transform.rotateRadians(rotation);
    return transform;
}

} // namespace snow_canvas_stroke_rendering

QPainterPath snowCanvasStrokeCoverage(const QPainterPath& path, qreal width,
                                      SnowCanvasStrokeStyle pattern,
                                      SnowCanvasStrokeRenderingStyle rendering, quint32 seed) {
    snow_canvas_stroke_rendering::Cache coverage;
    const snow_canvas_stroke_rendering::Options options{
        snow_canvas_types::toEngineStrokeRenderingStyle(rendering),
        snow_canvas_types::toEngineStrokeStyle(pattern), seed, width};
    snow_canvas_stroke_rendering::addStroke(coverage, path, options);
    return coverage.ink;
}

qreal snowCanvasStrokeRenderingOutset(SnowCanvasStrokeRenderingStyle rendering) {
    return snow_stroke_rendering_outset(snow_canvas_types::toEngineStrokeRenderingStyle(rendering),
                                        std::numeric_limits<double>::infinity());
}
