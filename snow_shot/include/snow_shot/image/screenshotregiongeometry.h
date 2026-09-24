#pragma once

#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPainterPath>
#include <QRegion>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>

enum class ScreenshotRegionType { Rectangle, Polyline, Curve, Freehand };

inline QString screenshotRegionTypeId(ScreenshotRegionType type) {
    switch (type) {
    case ScreenshotRegionType::Polyline:
        return QStringLiteral("polyline");
    case ScreenshotRegionType::Curve:
        return QStringLiteral("curve");
    case ScreenshotRegionType::Freehand:
        return QStringLiteral("freehand");
    default:
        return QStringLiteral("rectangle");
    }
}

inline ScreenshotRegionType screenshotRegionTypeFromId(const QString& id) {
    if (id == QStringLiteral("polyline"))
        return ScreenshotRegionType::Polyline;
    if (id == QStringLiteral("curve"))
        return ScreenshotRegionType::Curve;
    if (id == QStringLiteral("freehand"))
        return ScreenshotRegionType::Freehand;
    return ScreenshotRegionType::Rectangle;
}

// Rectangle-only regions retain their exact integer representation. Custom regions
// retain the original vector operands so Boolean flattening never becomes source data.
// Copies share immutable operands and a bounded, synchronized derived-path cache.
class ScreenshotRegionGeometry {
  public:
    enum class Operation { Add, Subtract, Intersect };
    struct Operand {
        QPainterPath path;
        Operation operation = Operation::Add;
        ScreenshotRegionType type = ScreenshotRegionType::Rectangle;
        bool operator==(const Operand&) const = default;
    };

    ScreenshotRegionGeometry() = default;
    ScreenshotRegionGeometry(const QRect& rectangle) : m_rectangles(rectangle) {}
    ScreenshotRegionGeometry(const QRegion& rectangles) : m_rectangles(rectangles) {}

    static ScreenshotRegionGeometry fromPath(const QPainterPath& path, ScreenshotRegionType type) {
        ScreenshotRegionGeometry result;
        result.m_vector = std::make_shared<VectorData>();
        result.m_vector->operands.push_back({path, Operation::Add, type});
        return result;
    }

    bool custom() const {
        return bool(m_vector);
    }
    QPainterPath path(qreal deviceScale = 1.0) const {
        if (!m_vector) {
            QPainterPath result;
            result.addRegion(m_rectangles);
            return result.simplified();
        }
        const auto contour = localPath(deviceScale);
        return m_offset.isNull() ? contour : contour.translated(m_offset);
    }
    QRect boundingRect() const {
        return custom() ? localPath().boundingRect().toAlignedRect().translated(m_offset)
                        : m_rectangles.boundingRect();
    }
    bool isEmpty() const {
        return custom() ? localPath().isEmpty() : m_rectangles.isEmpty();
    }
    int rectCount() const {
        return custom() ? (isEmpty() ? 0 : 2) : m_rectangles.rectCount();
    }
    bool contains(const QPointF& point) const {
        return custom() ? localPath().contains(point - m_offset)
                        : m_rectangles.contains(point.toPoint());
    }
    QRegion rasterRegion() const {
        return custom() ? QRegion(path().toFillPolygon().toPolygon(), Qt::OddEvenFill)
                        : m_rectangles;
    }
    const QRegion& rectangles() const {
        return m_rectangles;
    }
    auto begin() const {
        return m_rectangles.begin();
    }
    auto end() const {
        return m_rectangles.end();
    }

    ScreenshotRegionGeometry translated(const QPoint& offset) const {
        if (!custom())
            return m_rectangles.translated(offset);
        auto result = *this;
        result.m_offset += offset;
        return result;
    }
    void translate(const QPoint& offset) {
        *this = translated(offset);
    }
    void translate(int dx, int dy) {
        translate(QPoint(dx, dy));
    }
    ScreenshotRegionGeometry translated(int dx, int dy) const {
        return translated(QPoint(dx, dy));
    }
    ScreenshotRegionGeometry united(const ScreenshotRegionGeometry& other) const {
        return combined(other, Operation::Add);
    }
    ScreenshotRegionGeometry subtracted(const ScreenshotRegionGeometry& other) const {
        return combined(other, Operation::Subtract);
    }
    ScreenshotRegionGeometry intersected(const ScreenshotRegionGeometry& other) const {
        return combined(other, Operation::Intersect);
    }
    ScreenshotRegionGeometry& operator+=(const QRect& rect) {
        *this = united(ScreenshotRegionGeometry(rect));
        return *this;
    }
    bool operator==(const ScreenshotRegionGeometry& other) const {
        if (m_vector == other.m_vector)
            return m_rectangles == other.m_rectangles && m_offset == other.m_offset;
        if (!m_vector || !other.m_vector)
            return false;
        if (m_offset == other.m_offset)
            return m_vector->operands == other.m_vector->operands;
        if (m_vector->operands.size() != other.m_vector->operands.size())
            return false;
        const QPoint delta = m_offset - other.m_offset;
        for (qsizetype i = 0; i < m_vector->operands.size(); ++i) {
            const auto& left = m_vector->operands[i];
            const auto& right = other.m_vector->operands[i];
            if (left.operation != right.operation || left.type != right.type ||
                left.path.translated(delta) != right.path)
                return false;
        }
        return true;
    }

    // Capacity-based estimate of vector storage and its bounded contour cache.
    // Implicitly shared allocations may also belong to other snapshots.
    qsizetype retainedBytesEstimate() const {
        if (!m_vector)
            return static_cast<qsizetype>(sizeof(*this)) +
                   static_cast<qsizetype>(m_rectangles.rectCount()) *
                       static_cast<qsizetype>(sizeof(QRect));
        QMutexLocker lock(&m_vector->mutex);
        qsizetype bytes = static_cast<qsizetype>(sizeof(*this)) +
                          static_cast<qsizetype>(sizeof(VectorData)) +
                          m_vector->operands.capacity() * static_cast<qsizetype>(sizeof(Operand));
        for (const auto& operand : m_vector->operands)
            bytes += static_cast<qsizetype>(operand.path.capacity()) *
                     static_cast<qsizetype>(sizeof(QPainterPath::Element));
        for (const auto& contour : m_vector->contours)
            bytes += static_cast<qsizetype>(contour.path.capacity()) *
                     static_cast<qsizetype>(sizeof(QPainterPath::Element));
        return bytes;
    }

    // Drop derived contours for all snapshots sharing these immutable operands.
    // Geometry and persistence remain unchanged; future readers rebuild on demand.
    void clearDerivedCache() const {
        if (!m_vector)
            return;
        QMutexLocker lock(&m_vector->mutex);
        m_vector->contours = {};
    }

    QJsonObject toJson() const {
        if (!custom()) {
            QJsonArray rectangles;
            for (const auto& rect : m_rectangles)
                rectangles.append(QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()});
            return {{QStringLiteral("version"), 1}, {QStringLiteral("rectangles"), rectangles}};
        }
        QJsonArray operands;
        const auto source = custom() ? m_vector->operands : QVector<Operand>{{path()}};
        for (const auto& operand : source) {
            QJsonArray commands;
            for (int i = 0; i < operand.path.elementCount(); ++i) {
                const auto e = operand.path.elementAt(i);
                commands.append(QJsonArray{int(e.type), e.x + m_offset.x(), e.y + m_offset.y()});
            }
            operands.append(
                QJsonObject{{QStringLiteral("operation"), int(operand.operation)},
                            {QStringLiteral("type"), screenshotRegionTypeId(operand.type)},
                            {QStringLiteral("fill"), int(operand.path.fillRule())},
                            {QStringLiteral("commands"), commands}});
        }
        return {{QStringLiteral("version"), 1}, {QStringLiteral("operands"), operands}};
    }

    static std::optional<ScreenshotRegionGeometry> fromJson(const QJsonValue& value) {
        if (!value.isObject())
            return std::nullopt;
        const auto object = value.toObject();
        if (object.value(QStringLiteral("version")) != QJsonValue(1))
            return std::nullopt;
        if (object.contains(QStringLiteral("rectangles"))) {
            if (object.contains(QStringLiteral("operands")))
                return std::nullopt;
            const auto rects = object.value(QStringLiteral("rectangles"));
            if (!rects.isArray() || rects.toArray().size() > 65536)
                return std::nullopt;
            QRegion region;
            for (const auto& item : rects.toArray()) {
                const auto rect = item.toArray();
                if (rect.size() != 4)
                    return std::nullopt;
                for (const auto& n : rect) {
                    const double coordinate = n.toDouble();
                    if (!n.isDouble() || !std::isfinite(coordinate) ||
                        std::floor(coordinate) != coordinate || std::abs(coordinate) > 10000000)
                        return std::nullopt;
                }
                if (rect[2].toInt() < 1 || rect[3].toInt() < 1)
                    return std::nullopt;
                region += QRect(rect[0].toInt(), rect[1].toInt(), rect[2].toInt(), rect[3].toInt());
            }
            return ScreenshotRegionGeometry(region);
        }
        const auto items = object.value(QStringLiteral("operands"));
        if (!items.isArray() || items.toArray().isEmpty() || items.toArray().size() > 4096)
            return std::nullopt;
        ScreenshotRegionGeometry result;
        result.m_vector = std::make_shared<VectorData>();
        qsizetype total = 0;
        for (const auto& item : items.toArray()) {
            const auto op = item.toObject();
            const int operation = op.value(QStringLiteral("operation")).toInt(-1);
            const QString typeId = op.value(QStringLiteral("type")).toString();
            const auto type = screenshotRegionTypeFromId(typeId);
            const int fill = op.value(QStringLiteral("fill")).toInt(-1);
            if (operation < 0 || operation > 2 ||
                (result.m_vector->operands.isEmpty() && operation != 0) ||
                screenshotRegionTypeId(type) != typeId || fill < 0 || fill > 1)
                return std::nullopt;
            const auto commands = op.value(QStringLiteral("commands"));
            if (!commands.isArray())
                return std::nullopt;
            const auto array = commands.toArray();
            total += array.size();
            if (array.isEmpty() || total > 1048576)
                return std::nullopt;
            QPainterPath path;
            path.setFillRule(Qt::FillRule(fill));
            auto point = [&](qsizetype index, int expected) -> std::optional<QPointF> {
                if (index >= array.size() || !array[index].isArray())
                    return std::nullopt;
                const auto parts = array[index].toArray();
                if (parts.size() != 3 || parts[0] != QJsonValue(expected) || !parts[1].isDouble() ||
                    !parts[2].isDouble())
                    return std::nullopt;
                const double x = parts[1].toDouble(), y = parts[2].toDouble();
                if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 10000000 ||
                    std::abs(y) > 10000000)
                    return std::nullopt;
                return QPointF(x, y);
            };
            for (qsizetype i = 0; i < array.size(); ++i) {
                const auto parts = array[i].toArray();
                if (parts.size() != 3)
                    return std::nullopt;
                const int kind = parts[0].toInt(-1);
                const auto p = point(i, kind);
                if (!p || (i == 0 && kind != 0))
                    return std::nullopt;
                if (kind == 0)
                    path.moveTo(*p);
                else if (kind == 1)
                    path.lineTo(*p);
                else if (kind == 2) {
                    const auto c = point(i + 1, 3), end = point(i + 2, 3);
                    if (!c || !end)
                        return std::nullopt;
                    path.cubicTo(*p, *c, *end);
                    i += 2;
                } else
                    return std::nullopt;
            }
            result.m_vector->operands.append({path, Operation(operation), type});
        }
        return result;
    }

  private:
    static constexpr qreal kContourOversampling = 4.0;
    struct Contour {
        qreal scale = 0;
        QPainterPath path;
    };
    struct VectorData {
        QVector<Operand> operands;
        mutable QMutex mutex;
        // Slot zero is the canonical contour used by bounds/hit tests/edits.
        // Rendering at another DPI must never evict it. The other slots are MRU.
        mutable std::array<Contour, 3> contours;
    };
    QPainterPath localPath(qreal deviceScale = 1.0) const {
        if (m_vector->operands.size() == 1)
            return m_vector->operands.first().path;
        const qreal scale = std::max(qreal(1), std::abs(deviceScale)) * kContourOversampling;
        {
            QMutexLocker lock(&m_vector->mutex);
            for (std::size_t i = 0; i < m_vector->contours.size(); ++i) {
                if (m_vector->contours[i].scale != scale)
                    continue;
                if (i == 2)
                    std::swap(m_vector->contours[1], m_vector->contours[2]);
                return m_vector->contours[i == 2 ? 1 : i].path;
            }
        }
        // Operands are immutable. Do expensive Boolean work without blocking
        // other readers of this snapshot, then publish the derived contour.
        const auto transform = QTransform::fromScale(scale, scale);
        QPainterPath result;
        bool first = true;
        for (const auto& operand : m_vector->operands) {
            const auto next = transform.map(operand.path);
            if (first) {
                result = next;
                first = false;
            } else if (operand.operation == Operation::Add) {
                result = result.united(next);
            } else if (operand.operation == Operation::Subtract) {
                result = result.subtracted(next);
            } else {
                result = result.intersected(next);
            }
        }
        result = transform.inverted().map(result);
        QMutexLocker lock(&m_vector->mutex);
        if (scale == kContourOversampling)
            m_vector->contours[0] = {scale, result};
        else {
            m_vector->contours[2] = std::move(m_vector->contours[1]);
            m_vector->contours[1] = {scale, result};
        }
        return result;
    }
    ScreenshotRegionGeometry copyVector() const {
        ScreenshotRegionGeometry result;
        result.m_vector = std::make_shared<VectorData>();
        result.m_vector->operands = m_vector->operands;
        result.m_offset = m_offset;
        return result;
    }
    ScreenshotRegionGeometry combined(const ScreenshotRegionGeometry& other, Operation op) const {
        if (!custom() && !other.custom()) {
            if (op == Operation::Add)
                return m_rectangles.united(other.m_rectangles);
            if (op == Operation::Subtract)
                return m_rectangles.subtracted(other.m_rectangles);
            return m_rectangles.intersected(other.m_rectangles);
        }
        if (other.isEmpty() && op != Operation::Intersect)
            return *this;
        if (isEmpty())
            return op == Operation::Add ? other : ScreenshotRegionGeometry();
        const auto before = path();
        const auto operand = other.path();
        if ((op == Operation::Add && before.boundingRect().contains(operand.boundingRect()) &&
             operand.subtracted(before).isEmpty()) ||
            (op == Operation::Subtract && !before.intersects(operand)) ||
            (op == Operation::Intersect && operand.boundingRect().contains(before.boundingRect()) &&
             before.subtracted(operand).isEmpty()))
            return *this;
        // Extend the cached confirmed contour by just this operation. The source
        // operands still retain their cubics for reconstruction at another scale.
        QTransform transform;
        transform.scale(kContourOversampling, kContourOversampling);
        const auto scaledBefore = transform.map(before);
        const auto scaledOperand = transform.map(operand);
        const auto scaledAfter = op == Operation::Add ? scaledBefore.united(scaledOperand)
                                 : op == Operation::Subtract
                                     ? scaledBefore.subtracted(scaledOperand)
                                     : scaledBefore.intersected(scaledOperand);
        const auto after = transform.inverted().map(scaledAfter);
        if (after == before)
            return *this;
        if (after.isEmpty())
            return {};
        auto result = custom() ? copyVector() : fromPath(before, ScreenshotRegionType::Rectangle);
        // The right operand of a user operation is a single untouched contour.
        // Retain its cubics instead of the flattened Boolean result.
        result.m_vector->operands.append({operand.translated(-result.m_offset), op,
                                          other.custom() ? other.m_vector->operands.first().type
                                                         : ScreenshotRegionType::Rectangle});
        result.m_vector->contours[0] = {kContourOversampling, after.translated(-result.m_offset)};
        return result;
    }
    QRegion m_rectangles;
    QPoint m_offset;
    std::shared_ptr<VectorData> m_vector;
};

inline QDataStream& operator<<(QDataStream& stream, const ScreenshotRegionGeometry& region) {
    return stream << QJsonDocument(region.toJson()).toJson(QJsonDocument::Compact);
}
