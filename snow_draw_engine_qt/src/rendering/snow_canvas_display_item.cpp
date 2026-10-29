#include "snow_canvas_display_item.h"

#include "snow_canvas_fill_render.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_stroke_rendering.h"

#include <QPainterPathStroker>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace {

std::atomic<std::uint64_t> nextPathChunkCacheId{1};
thread_local SnowCanvasSceneItem::PathGeometryDiagnostics pathGeometryDiagnostics;

template <typename T>
void assignBorrowed(std::vector<T>& destination, const T* source, std::uint32_t count) {
    if (source == nullptr || count == 0) {
        destination.clear();
        return;
    }
    destination.assign(source, source + count);
}

template <typename T> const T* dataOrNull(const std::vector<T>& values) {
    return values.empty() ? nullptr : values.data();
}

template <typename T>
bool sameBorrowed(const std::vector<T>& owned, const T* source, std::uint32_t count) {
    return owned.size() == count &&
           (count == 0 ||
            (source != nullptr && std::memcmp(owned.data(), source, sizeof(T) * count) == 0));
}

QByteArray borrowedBytes(const char* bytes, std::uint32_t length) {
    if (bytes == nullptr || length == 0) {
        return {};
    }
    return QByteArray(bytes, static_cast<qsizetype>(length));
}

bool hasAnyRadius(const SnowCornerRadii& radii) {
    return radii.top_left > 0.0 || radii.top_right > 0.0 || radii.bottom_right > 0.0 ||
           radii.bottom_left > 0.0;
}

bool fuzzyEqual(double left, double right) {
    return std::abs(left - right) <= 1e-12;
}

bool rectsOverlapInclusive(const QRectF& left, const QRectF& right) {
    return left.right() >= right.left() && right.right() >= left.left() &&
           left.bottom() >= right.top() && right.bottom() >= left.top();
}

constexpr double kPenSpatialCellSize = 256.0;

std::int64_t penSpatialCellKey(int x, int y) {
    return static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
        static_cast<std::uint32_t>(y));
}

int penSpatialCell(double coordinate) {
    return static_cast<int>(std::floor(coordinate / kPenSpatialCellSize));
}

QPainterPath canvasPathFromCommands(double startX, double startY,
                                    const SnowArrowPathCommand* commands, std::uint32_t count) {
    QPainterPath path;
    path.moveTo(startX, startY);
    if (commands == nullptr) {
        return path;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const SnowArrowPathCommand& command = commands[index];
        switch (command.kind) {
        case SNOW_ARROW_PATH_COMMAND_MOVE_TO:
            path.moveTo(command.point.x, command.point.y);
            break;
        case SNOW_ARROW_PATH_COMMAND_LINE_TO:
            path.lineTo(command.point.x, command.point.y);
            break;
        case SNOW_ARROW_PATH_COMMAND_QUAD_TO:
            path.quadTo(command.control1.x, command.control1.y, command.point.x, command.point.y);
            break;
        case SNOW_ARROW_PATH_COMMAND_CUBIC_TO:
            path.cubicTo(command.control1.x, command.control1.y, command.control2.x,
                         command.control2.y, command.point.x, command.point.y);
            break;
        default:
            break;
        }
    }
    return path;
}

bool hasUniformRadii(const SnowCornerRadii& radii) {
    return fuzzyEqual(radii.top_left, radii.top_right) &&
           fuzzyEqual(radii.top_left, radii.bottom_right) &&
           fuzzyEqual(radii.top_left, radii.bottom_left);
}

std::uint64_t pathFingerprint(double startX, double startY, const SnowArrowPathCommand* commands,
                              std::uint32_t count) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto append = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    const auto appendDouble = [&](double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        append(bits);
    };
    appendDouble(startX);
    appendDouble(startY);
    append(count);
    for (std::uint32_t i = 0; commands != nullptr && i < count; ++i) {
        const auto& command = commands[i];
        append(static_cast<std::uint64_t>(command.kind));
        appendDouble(command.point.x);
        appendDouble(command.point.y);
        if (command.kind == SNOW_ARROW_PATH_COMMAND_QUAD_TO ||
            command.kind == SNOW_ARROW_PATH_COMMAND_CUBIC_TO) {
            appendDouble(command.control1.x);
            appendDouble(command.control1.y);
        }
        if (command.kind == SNOW_ARROW_PATH_COMMAND_CUBIC_TO) {
            appendDouble(command.control2.x);
            appendDouble(command.control2.y);
        }
    }
    return hash;
}

void rebuildRectangleGeometry(double width, double height, const SnowCornerRadii& radii,
                              QRectF* localRect, QPainterPath* path, bool* rounded, bool* uniform) {
    *localRect = QRectF(-width / 2.0, -height / 2.0, width, height);
    *rounded = hasAnyRadius(radii);
    *uniform = *rounded && hasUniformRadii(radii);
    *path = {};
    if (*rounded && !*uniform && localRect->isValid() && !localRect->isEmpty()) {
        *path = snow_canvas_render_geometry::roundedRectPath(
            *localRect, snow_canvas_render_geometry::toViewCornerRadii(radii, 1.0, *localRect));
    }
}

} // namespace

SnowCanvasSceneItem::SnowCanvasSceneItem() : SnowSceneDisplayItem{} {
    opacity = 1.0;
    refreshPointers();
    rebuildLocalRectangleGeometry();
}

SnowCanvasSceneItem::~SnowCanvasSceneItem() = default;

SnowCanvasSceneItem::SnowCanvasSceneItem(const SnowSceneDisplayItem& item) : SnowCanvasSceneItem() {
    assign(item);
}

SnowCanvasSceneItem::SnowCanvasSceneItem(const SnowCanvasSceneItem& other) : SnowCanvasSceneItem() {
    assign(other);
    if (other.m_magnifierGeometry)
        m_magnifierGeometry = std::make_unique<MagnifierGeometryCache>(*other.m_magnifierGeometry);
    viewBounds = other.viewBounds;
    m_penGeometryRevision = other.m_penGeometryRevision;
    m_pathGeometry = other.m_pathGeometry;
    m_pressureGeometry = other.m_pressureGeometry;
    m_aggregateClosedPath = other.m_aggregateClosedPath;
    m_pathClosed = other.m_pathClosed;
    m_strokeRenderingGeometry = other.m_strokeRenderingGeometry;
    m_strokeRenderingGeometryRevision = other.m_strokeRenderingGeometryRevision;
}

SnowCanvasSceneItem::SnowCanvasSceneItem(SnowCanvasSceneItem&& other) noexcept
    : SnowSceneDisplayItem(static_cast<const SnowSceneDisplayItem&>(other)),
      viewBounds(other.viewBounds), m_arrowPoints(std::move(other.m_arrowPoints)),
      m_arrowPathCommands(std::move(other.m_arrowPathCommands)),
      m_arrowheadPrimitives(std::move(other.m_arrowheadPrimitives)),
      m_textUtf8(std::move(other.m_textUtf8)), m_fontFamilyUtf8(std::move(other.m_fontFamilyUtf8)),
      m_localRect(other.m_localRect), m_localRectanglePath(std::move(other.m_localRectanglePath)),
      m_hasRoundedCorners(other.m_hasRoundedCorners),
      m_hasUniformCorners(other.m_hasUniformCorners),
      m_penGeometryChunks(std::move(other.m_penGeometryChunks)),
      m_penSpatialCells(std::move(other.m_penSpatialCells)),
      m_penGeometryRevision(other.m_penGeometryRevision),
      m_pathGeometry(std::move(other.m_pathGeometry)),
      m_pressureGeometry(std::move(other.m_pressureGeometry)),
      m_aggregateClosedPath(std::move(other.m_aggregateClosedPath)),
      m_pathClosed(other.m_pathClosed), m_magnifierGeometry(std::move(other.m_magnifierGeometry)),
      m_strokeRenderingGeometry(std::move(other.m_strokeRenderingGeometry)),
      m_strokeRenderingGeometryRevision(other.m_strokeRenderingGeometryRevision),
      m_pendingPenGeometryChunkBuildCount(other.m_pendingPenGeometryChunkBuildCount),
      m_pendingPenGeometryChunkReuseCount(other.m_pendingPenGeometryChunkReuseCount) {
    refreshPointers();
    other.refreshPointers();
    other.m_pendingPenGeometryChunkBuildCount = 0;
    other.m_pendingPenGeometryChunkReuseCount = 0;
}

SnowCanvasSceneItem& SnowCanvasSceneItem::operator=(const SnowCanvasSceneItem& other) {
    if (this != &other) {
        assign(other);
        m_magnifierGeometry =
            other.m_magnifierGeometry
                ? std::make_unique<MagnifierGeometryCache>(*other.m_magnifierGeometry)
                : nullptr;
        viewBounds = other.viewBounds;
        m_penGeometryRevision = other.m_penGeometryRevision;
        m_pathGeometry = other.m_pathGeometry;
        m_pressureGeometry = other.m_pressureGeometry;
        m_aggregateClosedPath = other.m_aggregateClosedPath;
        m_pathClosed = other.m_pathClosed;
        m_strokeRenderingGeometry = other.m_strokeRenderingGeometry;
        m_strokeRenderingGeometryRevision = other.m_strokeRenderingGeometryRevision;
    }
    return *this;
}

SnowCanvasSceneItem& SnowCanvasSceneItem::operator=(SnowCanvasSceneItem&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    static_cast<SnowSceneDisplayItem&>(*this) = other;
    viewBounds = other.viewBounds;
    m_arrowPoints = std::move(other.m_arrowPoints);
    m_arrowPathCommands = std::move(other.m_arrowPathCommands);
    m_arrowheadPrimitives = std::move(other.m_arrowheadPrimitives);
    m_textUtf8 = std::move(other.m_textUtf8);
    m_fontFamilyUtf8 = std::move(other.m_fontFamilyUtf8);
    m_localRect = other.m_localRect;
    m_localRectanglePath = std::move(other.m_localRectanglePath);
    m_hasRoundedCorners = other.m_hasRoundedCorners;
    m_hasUniformCorners = other.m_hasUniformCorners;
    m_penGeometryChunks = std::move(other.m_penGeometryChunks);
    m_penSpatialCells = std::move(other.m_penSpatialCells);
    m_penGeometryRevision = other.m_penGeometryRevision;
    m_pathGeometry = std::move(other.m_pathGeometry);
    m_pressureGeometry = std::move(other.m_pressureGeometry);
    m_aggregateClosedPath = std::move(other.m_aggregateClosedPath);
    m_pathClosed = other.m_pathClosed;
    m_arrowTextClipCacheValid = false;
    m_magnifierGeometry = std::move(other.m_magnifierGeometry);
    m_strokeRenderingGeometry = std::move(other.m_strokeRenderingGeometry);
    m_strokeRenderingGeometryRevision = other.m_strokeRenderingGeometryRevision;
    m_pendingPenGeometryChunkBuildCount = other.m_pendingPenGeometryChunkBuildCount;
    m_pendingPenGeometryChunkReuseCount = other.m_pendingPenGeometryChunkReuseCount;
    refreshPointers();
    other.refreshPointers();
    other.m_pendingPenGeometryChunkBuildCount = 0;
    other.m_pendingPenGeometryChunkReuseCount = 0;
    return *this;
}

SnowCanvasSceneItem& SnowCanvasSceneItem::operator=(const SnowSceneDisplayItem& item) {
    assign(item);
    return *this;
}

void SnowCanvasSceneItem::assign(const SnowSceneDisplayItem& item) {
    m_arrowTextClipCacheValid = false;
    if (item.kind != SNOW_SCENE_DISPLAY_ITEM_MAGNIFIER)
        m_magnifierGeometry.reset();
    const bool preservePenGeometry = kind == SNOW_SCENE_DISPLAY_ITEM_FILTER && is_free_draw != 0 &&
                                     item.kind == SNOW_SCENE_DISPLAY_ITEM_FILTER &&
                                     item.is_free_draw != 0 &&
                                     element_id.index == item.element_id.index &&
                                     element_id.generation == item.element_id.generation &&
                                     item.arrow_points == nullptr && item.arrow_point_count == 0;
    const bool geometryStyleChanged =
        preservePenGeometry &&
        (!fuzzyEqual(stroke_width, item.stroke_width) || !fuzzyEqual(rotation, item.rotation));
    const bool preservePathGeometry =
        (kind == SNOW_SCENE_DISPLAY_ITEM_ARROW || kind == SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE) &&
        item.kind == kind && element_id.index == item.element_id.index &&
        element_id.generation == item.element_id.generation;
    if (!sameBorrowed(m_arrowPoints, item.arrow_points, item.arrow_point_count) ||
        !sameBorrowed(m_arrowPathCommands, item.arrow_path_commands,
                      item.arrow_path_command_count) ||
        !sameBorrowed(m_arrowheadPrimitives, item.arrowhead_primitives,
                      item.arrowhead_primitive_count)) {
        ++m_strokeRenderingGeometryRevision;
    }
    static_cast<SnowSceneDisplayItem&>(*this) = item;
    if (!preservePenGeometry) {
        assignBorrowed(m_arrowPoints, item.arrow_points, item.arrow_point_count);
        m_penGeometryRevision = 0;
    }
    assignBorrowed(m_arrowPathCommands, item.arrow_path_commands, item.arrow_path_command_count);
    if (!preservePathGeometry) {
        m_pathGeometry = {};
        m_pressureGeometry = {};
        m_aggregateClosedPath = {};
        m_pathClosed = false;
    }
    assignBorrowed(m_arrowheadPrimitives, item.arrowhead_primitives,
                   item.arrowhead_primitive_count);
    m_textUtf8 = borrowedBytes(item.text_utf8, item.text_utf8_len);
    m_fontFamilyUtf8 = borrowedBytes(item.font_family_utf8, item.font_family_utf8_len);
    refreshPointers();
    rebuildLocalRectangleGeometry();
    if (preservePathGeometry)
        rebuildAggregateClosedPath(false);
    if (!preservePenGeometry || geometryStyleChanged) {
        rebuildPenFilterGeometry(0);
    }
}

snow_canvas_stroke_rendering::Cache& SnowCanvasSceneItem::strokeRenderingCache() const {
    if (!m_strokeRenderingGeometry) {
        m_strokeRenderingGeometry = std::make_shared<snow_canvas_stroke_rendering::Cache>();
        snow_canvas_stroke_rendering::recordCacheAllocation(false);
    } else if (m_strokeRenderingGeometry.use_count() != 1) {
        m_strokeRenderingGeometry =
            std::make_shared<snow_canvas_stroke_rendering::Cache>(*m_strokeRenderingGeometry);
        snow_canvas_stroke_rendering::recordCacheAllocation(true);
    }
    return *m_strokeRenderingGeometry;
}

const snow_canvas_stroke_rendering::Cache*
SnowCanvasSceneItem::strokeRenderingCacheIfPresent() const {
    return m_strokeRenderingGeometry.get();
}

std::uint64_t SnowCanvasSceneItem::strokeRenderingGeometryRevision() const {
    return m_strokeRenderingGeometryRevision;
}

SnowCanvasSceneItem::MagnifierGeometryCache& SnowCanvasSceneItem::magnifierGeometryCache() const {
    if (!m_magnifierGeometry)
        m_magnifierGeometry = std::make_unique<MagnifierGeometryCache>();
    return *m_magnifierGeometry;
}

void SnowCanvasSceneItem::rebuildPenFilterGeometry(std::size_t firstChangedPoint) {
    constexpr std::uint32_t kSegmentsPerChunk = 32;
    const bool dot = filter.filter_type == 7 && m_arrowPoints.size() == 1;
    if (kind != SNOW_SCENE_DISPLAY_ITEM_FILTER || is_free_draw == 0 ||
        (m_arrowPoints.size() < 2 && !dot) || !std::isfinite(stroke_width) || stroke_width <= 0.0) {
        m_penGeometryChunks.clear();
        m_penSpatialCells.clear();
        return;
    }

    std::uint32_t firstSegment = 0;
    std::size_t reused = 0;
    if (firstChangedPoint == 0 || m_penGeometryChunks.empty()) {
        m_penGeometryChunks.clear();
        m_penSpatialCells.clear();
    } else {
        const std::uint32_t changedSegment = static_cast<std::uint32_t>(firstChangedPoint - 1);
        while (!m_penGeometryChunks.empty()) {
            const std::uint32_t chunkIndex =
                static_cast<std::uint32_t>(m_penGeometryChunks.size() - 1);
            const PenSegmentChunk& chunk = m_penGeometryChunks.back();
            const std::uint32_t chunkFirstSegment = chunk.firstPoint;
            const std::uint32_t segmentCount = chunk.pointCount - 1;
            if (chunk.pointCount > 1 && chunkFirstSegment + segmentCount <= changedSegment) {
                break;
            }
            for (std::int64_t cell : chunk.spatialCells) {
                auto found = m_penSpatialCells.find(cell);
                if (found == m_penSpatialCells.end()) {
                    continue;
                }
                auto& indices = found->second;
                indices.erase(std::remove(indices.begin(), indices.end(), chunkIndex),
                              indices.end());
                if (indices.empty()) {
                    m_penSpatialCells.erase(found);
                }
            }
            m_penGeometryChunks.pop_back();
        }
        reused = m_penGeometryChunks.size();
        if (!m_penGeometryChunks.empty()) {
            const PenSegmentChunk& chunk = m_penGeometryChunks.back();
            firstSegment = chunk.firstPoint + chunk.pointCount - 1;
        }
    }

    const std::uint32_t segmentCount =
        dot ? 1 : static_cast<std::uint32_t>(m_arrowPoints.size() - 1);
    std::size_t built = 0;
    while (firstSegment < segmentCount) {
        const std::uint32_t count =
            dot ? 0 : std::min(kSegmentsPerChunk, segmentCount - firstSegment);
        double minX = std::numeric_limits<double>::infinity();
        double minY = std::numeric_limits<double>::infinity();
        double maxX = -std::numeric_limits<double>::infinity();
        double maxY = -std::numeric_limits<double>::infinity();
        for (std::uint32_t offset = 0; offset <= count; ++offset) {
            const SnowArrowPoint& point = m_arrowPoints[firstSegment + offset];
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
        const double radius = stroke_width * 0.5;
        PenSegmentChunk chunk;
        chunk.firstPoint = firstSegment;
        chunk.pointCount = count + 1;
        chunk.canvasBounds =
            QRectF(QPointF(minX - radius, minY - radius), QPointF(maxX + radius, maxY + radius));
        chunk.revision = m_penGeometryRevision;
        const int firstCellX = penSpatialCell(chunk.canvasBounds.left());
        const int lastCellX = penSpatialCell(chunk.canvasBounds.right());
        const int firstCellY = penSpatialCell(chunk.canvasBounds.top());
        const int lastCellY = penSpatialCell(chunk.canvasBounds.bottom());
        for (int y = firstCellY; y <= lastCellY; ++y) {
            for (int x = firstCellX; x <= lastCellX; ++x) {
                chunk.spatialCells.push_back(penSpatialCellKey(x, y));
            }
        }
        const std::uint32_t chunkIndex = static_cast<std::uint32_t>(m_penGeometryChunks.size());
        for (std::int64_t cell : chunk.spatialCells) {
            m_penSpatialCells[cell].push_back(chunkIndex);
        }
        m_penGeometryChunks.push_back(std::move(chunk));
        ++built;
        firstSegment += std::max(1u, count);
    }
    m_pendingPenGeometryChunkBuildCount += built;
    m_pendingPenGeometryChunkReuseCount += reused;
}

const std::vector<SnowCanvasSceneItem::PenSegmentChunk>&
SnowCanvasSceneItem::penSegmentChunks() const {
    return m_penGeometryChunks;
}

const std::vector<SnowArrowPoint>& SnowCanvasSceneItem::penFilterPoints() const {
    return m_arrowPoints;
}

void SnowCanvasSceneItem::queryPenSegmentChunks(const QRectF& canvasBounds,
                                                std::vector<std::uint32_t>* outChunkIndices,
                                                std::size_t* candidateCount) const {
    if (outChunkIndices == nullptr || canvasBounds.isEmpty()) {
        return;
    }
    outChunkIndices->clear();
    std::unordered_set<std::uint32_t> seen;
    const int firstCellX = penSpatialCell(canvasBounds.left());
    const int lastCellX = penSpatialCell(canvasBounds.right());
    const int firstCellY = penSpatialCell(canvasBounds.top());
    const int lastCellY = penSpatialCell(canvasBounds.bottom());
    for (int y = firstCellY; y <= lastCellY; ++y) {
        for (int x = firstCellX; x <= lastCellX; ++x) {
            const auto found = m_penSpatialCells.find(penSpatialCellKey(x, y));
            if (found == m_penSpatialCells.end()) {
                continue;
            }
            if (candidateCount != nullptr) {
                *candidateCount += found->second.size();
            }
            for (std::uint32_t index : found->second) {
                if (seen.insert(index).second && index < m_penGeometryChunks.size() &&
                    m_penGeometryChunks[index].canvasBounds.intersects(canvasBounds)) {
                    outChunkIndices->push_back(index);
                }
            }
        }
    }
}

void SnowCanvasSceneItem::takePenFilterGeometryDiagnostics(std::size_t* chunkBuildCount,
                                                           std::size_t* chunkReuseCount) const {
    if (chunkBuildCount != nullptr) {
        *chunkBuildCount += m_pendingPenGeometryChunkBuildCount;
    }
    if (chunkReuseCount != nullptr) {
        *chunkReuseCount += m_pendingPenGeometryChunkReuseCount;
    }
    m_pendingPenGeometryChunkBuildCount = 0;
    m_pendingPenGeometryChunkReuseCount = 0;
}

std::uint64_t SnowCanvasSceneItem::penFilterGeometryRevision() const {
    return m_penGeometryRevision;
}

void SnowCanvasSceneItem::refreshPointers() {
    arrow_points = dataOrNull(m_arrowPoints);
    arrow_point_count = static_cast<std::uint32_t>(m_arrowPoints.size());
    arrow_path_commands = dataOrNull(m_arrowPathCommands);
    arrow_path_command_count = static_cast<std::uint32_t>(m_arrowPathCommands.size());
    arrowhead_primitives = dataOrNull(m_arrowheadPrimitives);
    arrowhead_primitive_count = static_cast<std::uint32_t>(m_arrowheadPrimitives.size());
    text_utf8 = m_textUtf8.isEmpty() ? nullptr : m_textUtf8.constData();
    text_utf8_len = static_cast<std::uint32_t>(m_textUtf8.size());
    font_family_utf8 = m_fontFamilyUtf8.isEmpty() ? nullptr : m_fontFamilyUtf8.constData();
    font_family_utf8_len = static_cast<std::uint32_t>(m_fontFamilyUtf8.size());
}

void SnowCanvasSceneItem::setArrowPoints(const SnowArrowPoint* points, std::uint32_t count) {
    if (!sameBorrowed(m_arrowPoints, points, count))
        ++m_strokeRenderingGeometryRevision;
    assignBorrowed(m_arrowPoints, points, count);
    m_penGeometryRevision = 0;
    refreshPointers();
    rebuildPenFilterGeometry(0);
}

bool SnowCanvasSceneItem::applyPenFilterGeometryPatch(std::uint64_t expectedRevision,
                                                      std::uint64_t revision,
                                                      std::uint32_t retainPrefixCount,
                                                      const SnowArrowPoint* appendedPoints,
                                                      std::uint32_t appendCount, bool fullReset) {
    if (kind != SNOW_SCENE_DISPLAY_ITEM_FILTER || is_free_draw == 0 ||
        (!fullReset && m_penGeometryRevision != expectedRevision) ||
        (appendCount != 0 && appendedPoints == nullptr) ||
        (!fullReset && retainPrefixCount > m_arrowPoints.size())) {
        return false;
    }
    const std::size_t firstChangedPoint = fullReset ? 0 : retainPrefixCount;
    if (fullReset) {
        m_arrowPoints.clear();
    } else {
        m_arrowPoints.resize(firstChangedPoint);
    }
    if (appendCount != 0) {
        m_arrowPoints.insert(m_arrowPoints.end(), appendedPoints, appendedPoints + appendCount);
    }
    m_penGeometryRevision = revision;
    refreshPointers();
    rebuildPenFilterGeometry(firstChangedPoint);
    return true;
}

bool SnowCanvasSceneItem::applyPathGeometryPatch(
    std::uint64_t expectedRevision, std::uint64_t revision, const SnowPathChunkRange* ranges,
    std::uint32_t rangeCount, const SnowPathChunk* chunks, std::uint32_t chunkCount,
    const SnowArrowPathCommand* commands, std::uint32_t commandCount, bool closed, bool fullReset,
    std::uint8_t role) {
    if (role > SNOW_PATH_GEOMETRY_ROLE_PRESSURE_OUTLINE) {
        return false;
    }
    PathStorage& storage =
        role == SNOW_PATH_GEOMETRY_ROLE_PRESSURE_OUTLINE ? m_pressureGeometry : m_pathGeometry;
    if ((kind != SNOW_SCENE_DISPLAY_ITEM_ARROW && kind != SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE) ||
        (!fullReset && storage.revision != expectedRevision) ||
        (rangeCount != 0 && ranges == nullptr)) {
        return false;
    }
    m_arrowTextClipCacheValid = false;
    // A repaint-only Rust update may publish a new transport revision with a
    // full reset of identical path chunks. Retain their paths, spatial index,
    // cache identities and derived ink instead of rebuilding vector geometry.
    if (fullReset && rangeCount == 1 && ranges[0].start == 0 &&
        ranges[0].insert_chunk_offset == 0 && ranges[0].insert_chunk_count == chunkCount &&
        storage.chunks.size() == chunkCount && (chunkCount == 0 || chunks != nullptr) &&
        (role != SNOW_PATH_GEOMETRY_ROLE_CENTERLINE || m_pathClosed == closed)) {
        bool unchanged = true;
        for (std::uint32_t i = 0; i < chunkCount; ++i) {
            const auto& source = chunks[i];
            const auto& owned = storage.chunks[i];
            if (source.command_offset > commandCount ||
                source.command_count > commandCount - source.command_offset ||
                (source.command_count != 0 && commands == nullptr)) {
                unchanged = false;
                break;
            }
            const auto* chunkCommands =
                source.command_count == 0 ? nullptr : commands + source.command_offset;
            const QRectF bounds(QPointF(source.min_x, source.min_y),
                                QPointF(source.max_x, source.max_y));
            if (owned.geometryFingerprint != pathFingerprint(source.start_x, source.start_y,
                                                             chunkCommands, source.command_count) ||
                owned.canvasBounds != bounds.normalized() ||
                owned.cumulativeStartLength != source.cumulative_start_length ||
                owned.commandStart != source.command_start) {
                unchanged = false;
                break;
            }
        }
        if (unchanged) {
            pathGeometryDiagnostics.reuses += chunkCount;
            storage.revision = revision;
            for (std::uint32_t i = 0; i < chunkCount; ++i)
                storage.chunks[i].stableId = chunks[i].stable_id;
            if (role == SNOW_PATH_GEOMETRY_ROLE_CENTERLINE)
                rebuildAggregateClosedPath(false);
            return true;
        }
    }
    if (fullReset) {
        storage = {};
    }
    using Difference = std::vector<PathChunk>::difference_type;
    for (std::uint32_t rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex) {
        const SnowPathChunkRange& range = ranges[rangeIndex];
        const std::size_t start = std::min<std::size_t>(storage.chunks.size(), range.start);
        const std::size_t deleteCount =
            std::min<std::size_t>(storage.chunks.size() - start, range.delete_count);
        if (range.insert_chunk_offset > chunkCount ||
            range.insert_chunk_count > chunkCount - range.insert_chunk_offset) {
            return false;
        }
        std::vector<PathChunk> inserted;
        inserted.reserve(range.insert_chunk_count);
        for (std::uint32_t offset = 0; offset < range.insert_chunk_count; ++offset) {
            const SnowPathChunk& source = chunks[range.insert_chunk_offset + offset];
            if (source.command_offset > commandCount ||
                source.command_count > commandCount - source.command_offset) {
                return false;
            }
            PathChunk chunk;
            chunk.stableId = source.stable_id;
            chunk.cacheId = nextPathChunkCacheId.fetch_add(1, std::memory_order_relaxed);
            chunk.geometryFingerprint = pathFingerprint(
                source.start_x, source.start_y,
                source.command_count == 0 ? nullptr : commands + source.command_offset,
                source.command_count);
            chunk.commandStart = source.command_start;
            chunk.startsSubpath =
                source.command_count != 0 &&
                commands[source.command_offset].kind == SNOW_ARROW_PATH_COMMAND_MOVE_TO;
            for (std::uint32_t index = 0; index < source.command_count; ++index)
                if (commands[source.command_offset + index].kind == SNOW_ARROW_PATH_COMMAND_MOVE_TO)
                    ++chunk.subpathCount;
            chunk.canvasBounds =
                QRectF(QPointF(source.min_x, source.min_y), QPointF(source.max_x, source.max_y))
                    .normalized();
            chunk.cumulativeStartLength = source.cumulative_start_length;
            ++pathGeometryDiagnostics.builds;
            chunk.canvasPath = canvasPathFromCommands(
                source.start_x, source.start_y,
                source.command_count == 0 ? nullptr : commands + source.command_offset,
                source.command_count);
            if (role == SNOW_PATH_GEOMETRY_ROLE_PRESSURE_OUTLINE)
                chunk.canvasPath.setFillRule(Qt::WindingFill);
            inserted.push_back(std::move(chunk));
        }
        const bool shiftsIndices = deleteCount != inserted.size();
        const std::size_t indexEnd = shiftsIndices ? storage.chunks.size() : start + deleteCount;
        removePathIndex(storage, start, indexEnd);
        storage.chunks.erase(storage.chunks.begin() + static_cast<Difference>(start),
                             storage.chunks.begin() + static_cast<Difference>(start + deleteCount));
        storage.chunks.insert(storage.chunks.begin() + static_cast<Difference>(start),
                              std::make_move_iterator(inserted.begin()),
                              std::make_move_iterator(inserted.end()));
        insertPathIndex(storage, start,
                        shiftsIndices ? storage.chunks.size() : start + inserted.size());
    }
    storage.revision = revision;
    if (role == SNOW_PATH_GEOMETRY_ROLE_CENTERLINE) {
        ++m_strokeRenderingGeometryRevision;
        m_pathClosed = closed;
        rebuildAggregateClosedPath();
    }
    return true;
}

void SnowCanvasSceneItem::removePathIndex(PathStorage& storage, std::size_t start,
                                          std::size_t end) {
    for (std::size_t index = start; index < end; ++index) {
        const PathChunk& chunk = storage.chunks[index];
        for (const auto cell : chunk.spatialCells) {
            auto found = storage.spatialCells.find(cell);
            if (found == storage.spatialCells.end())
                continue;
            auto& indices = found->second;
            indices.erase(
                std::remove(indices.begin(), indices.end(), static_cast<std::uint32_t>(index)),
                indices.end());
            if (found->second.empty())
                storage.spatialCells.erase(found);
        }
        const std::array<double, 4> values{chunk.canvasBounds.left(), chunk.canvasBounds.top(),
                                           chunk.canvasBounds.right(), chunk.canvasBounds.bottom()};
        for (std::size_t axis = 0; axis < values.size(); ++axis) {
            const auto found = storage.bounds[axis].find(values[axis]);
            if (found != storage.bounds[axis].end())
                storage.bounds[axis].erase(found);
        }
    }
}

void SnowCanvasSceneItem::insertPathIndex(PathStorage& storage, std::size_t start,
                                          std::size_t end) {
    for (std::size_t index = start; index < end; ++index) {
        PathChunk& chunk = storage.chunks[index];
        chunk.spatialCells.clear();
        const int firstX = penSpatialCell(chunk.canvasBounds.left());
        const int lastX = penSpatialCell(chunk.canvasBounds.right());
        const int firstY = penSpatialCell(chunk.canvasBounds.top());
        const int lastY = penSpatialCell(chunk.canvasBounds.bottom());
        for (int y = firstY; y <= lastY; ++y) {
            for (int x = firstX; x <= lastX; ++x) {
                const auto cell = penSpatialCellKey(x, y);
                chunk.spatialCells.push_back(cell);
                storage.spatialCells[cell].push_back(static_cast<std::uint32_t>(index));
            }
        }
        storage.bounds[0].insert(chunk.canvasBounds.left());
        storage.bounds[1].insert(chunk.canvasBounds.top());
        storage.bounds[2].insert(chunk.canvasBounds.right());
        storage.bounds[3].insert(chunk.canvasBounds.bottom());
    }
}

QRectF SnowCanvasSceneItem::storageBounds(const PathStorage& storage) {
    if (storage.chunks.empty())
        return {};
    return QRectF(QPointF(*storage.bounds[0].begin(), *storage.bounds[1].begin()),
                  QPointF(*storage.bounds[2].rbegin(), *storage.bounds[3].rbegin()));
}

QRectF SnowCanvasSceneItem::pathCanvasBounds() const {
    return storageBounds(m_pathGeometry);
}

QRectF SnowCanvasSceneItem::pressureCanvasBounds() const {
    return storageBounds(m_pressureGeometry);
}

std::uint64_t SnowCanvasSceneItem::pressureGeometryRevision() const {
    return m_pressureGeometry.revision;
}

const std::vector<SnowCanvasSceneItem::PathChunk>& SnowCanvasSceneItem::pressurePathChunks() const {
    return m_pressureGeometry.chunks;
}

void SnowCanvasSceneItem::queryPathChunks(const QRectF& canvasBounds,
                                          std::vector<std::uint32_t>* outChunkIndices) const {
    queryPathStorage(m_pathGeometry, canvasBounds, outChunkIndices);
}

void SnowCanvasSceneItem::queryPressurePathChunks(
    const QRectF& canvasBounds, std::vector<std::uint32_t>* outChunkIndices) const {
    queryPathStorage(m_pressureGeometry, canvasBounds, outChunkIndices);
}

void SnowCanvasSceneItem::rebuildAggregateClosedPath(bool geometryChanged) {
    // Speech bubbles draw their outline from this path even without a visible fill.
    if (!m_pathClosed || (kind != SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE &&
                          !snow_canvas_fill_render::hasVisibleFill(fill, fill_style))) {
        m_aggregateClosedPath = {};
        return;
    }
    if (!geometryChanged && !m_aggregateClosedPath.isEmpty())
        return;
    m_aggregateClosedPath = {};
    std::uint32_t subpaths = 0;
    if (kind == SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE) {
        for (const PathChunk& chunk : m_pathGeometry.chunks) {
            subpaths += chunk.subpathCount;
            if (subpaths > 1)
                break;
        }
    }
    for (const PathChunk& chunk : m_pathGeometry.chunks) {
        // Retain the inexpensive Qt append for the existing continuous outlines.
        if (subpaths <= 1) {
            m_aggregateClosedPath.connectPath(chunk.canvasPath);
            continue;
        }
        // Chunk-local paths begin with a synthetic move when they continue an
        // earlier contour. Replay explicit moves against the aggregate so each
        // contour closes at its real start, even when it spans several chunks.
        for (int index = 0; index < chunk.canvasPath.elementCount(); ++index) {
            const auto element = chunk.canvasPath.elementAt(index);
            if (element.isMoveTo()) {
                if (index != 0 || chunk.startsSubpath || m_aggregateClosedPath.isEmpty()) {
                    m_aggregateClosedPath.closeSubpath();
                    m_aggregateClosedPath.moveTo(element.x, element.y);
                }
            } else if (element.isLineTo()) {
                m_aggregateClosedPath.lineTo(element.x, element.y);
            } else if (element.type == QPainterPath::CurveToElement) {
                const auto control = chunk.canvasPath.elementAt(++index);
                const auto end = chunk.canvasPath.elementAt(++index);
                m_aggregateClosedPath.cubicTo(element.x, element.y, control.x, control.y, end.x,
                                              end.y);
            }
        }
    }
    m_aggregateClosedPath.closeSubpath();
    if (kind == SNOW_SCENE_DISPLAY_ITEM_SPEECH_BUBBLE)
        m_aggregateClosedPath.setFillRule(Qt::WindingFill);
}

const std::vector<SnowCanvasSceneItem::PathChunk>& SnowCanvasSceneItem::pathChunks() const {
    pathGeometryDiagnostics.reads += m_pathGeometry.chunks.size();
    return m_pathGeometry.chunks;
}

const QPainterPath& SnowCanvasSceneItem::aggregateClosedPath() const {
    return m_aggregateClosedPath;
}

bool SnowCanvasSceneItem::pathIsClosed() const {
    return m_pathClosed;
}
std::uint64_t SnowCanvasSceneItem::pathGeometryRevision() const {
    return m_pathGeometry.revision;
}

bool SnowCanvasSceneItem::needsArrowTextClip(double canvasAntialiasGuard) const {
    if (m_pathGeometry.chunks.empty() || arrow_shaft_type == SNOW_ARROW_SHAFT_TYPE_TAPERED ||
        arrowhead_primitive_count != 0 || arrow_start_head != SNOW_ARROWHEAD_NONE ||
        arrow_end_head != SNOW_ARROWHEAD_NONE ||
        (m_pathClosed && snow_canvas_fill_render::hasVisibleFill(fill, fill_style)) ||
        !std::isfinite(canvasAntialiasGuard) || canvasAntialiasGuard < 0.0 ||
        !std::isfinite(stroke_width) || stroke_width <= 0.0) {
        return true;
    }
    const double margin = 5.0 + canvasAntialiasGuard;
    const QRectF bounds = QRectF(QPointF(arrow_text_bounds[0], arrow_text_bounds[1]),
                                 QPointF(arrow_text_bounds[2], arrow_text_bounds[3]))
                              .normalized()
                              .adjusted(-margin, -margin, margin, margin);
    if (m_arrowTextClipCacheValid && m_arrowTextClipGeometryRevision == m_pathGeometry.revision &&
        m_arrowTextClipStrokeWidth == stroke_width && m_arrowTextClipBounds == bounds) {
        return m_arrowTextClipNeeded;
    }
    QPainterPathStroker stroker;
    stroker.setWidth(stroke_width);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    bool intersects = false;
    const double halfStroke = stroke_width / 2.0;
    for (const PathChunk& chunk : m_pathGeometry.chunks) {
        if (rectsOverlapInclusive(
                chunk.canvasBounds.adjusted(-halfStroke, -halfStroke, halfStroke, halfStroke),
                bounds) &&
            stroker.createStroke(chunk.canvasPath).intersects(bounds)) {
            intersects = true;
            break;
        }
    }
    // Cache only the decision; transient stroke outlines do not grow the path cache.
    m_arrowTextClipGeometryRevision = m_pathGeometry.revision;
    m_arrowTextClipStrokeWidth = stroke_width;
    m_arrowTextClipBounds = bounds;
    m_arrowTextClipNeeded = intersects;
    m_arrowTextClipCacheValid = true;
    return intersects;
}

void SnowCanvasSceneItem::queryPathStorage(const PathStorage& storage, const QRectF& canvasBounds,
                                           std::vector<std::uint32_t>* outChunkIndices) {
    if (outChunkIndices == nullptr) {
        return;
    }
    outChunkIndices->clear();
    // Short annotation paths cost less to cull directly than to probe every
    // spatial cell covered by the viewport.
    if (storage.chunks.size() <= 16) {
        for (std::uint32_t index = 0; index < storage.chunks.size(); ++index) {
            if (rectsOverlapInclusive(storage.chunks[index].canvasBounds, canvasBounds)) {
                outChunkIndices->push_back(index);
            }
        }
        return;
    }
    std::unordered_set<std::uint32_t> seen;
    const int firstCellX = penSpatialCell(canvasBounds.left());
    const int lastCellX = penSpatialCell(canvasBounds.right());
    const int firstCellY = penSpatialCell(canvasBounds.top());
    const int lastCellY = penSpatialCell(canvasBounds.bottom());
    for (int y = firstCellY; y <= lastCellY; ++y) {
        for (int x = firstCellX; x <= lastCellX; ++x) {
            const auto found = storage.spatialCells.find(penSpatialCellKey(x, y));
            if (found == storage.spatialCells.end()) {
                continue;
            }
            for (std::uint32_t index : found->second) {
                if (seen.insert(index).second && index < storage.chunks.size() &&
                    rectsOverlapInclusive(storage.chunks[index].canvasBounds, canvasBounds)) {
                    outChunkIndices->push_back(index);
                }
            }
        }
    }
    std::sort(outChunkIndices->begin(), outChunkIndices->end());
}

void SnowCanvasSceneItem::setTextUtf8(const QByteArray& text) {
    m_textUtf8 = text;
    refreshPointers();
}

void SnowCanvasSceneItem::setFontFamilyUtf8(const QByteArray& family) {
    m_fontFamilyUtf8 = family;
    refreshPointers();
}

void SnowCanvasSceneItem::rebuildLocalRectangleGeometry() {
    rebuildRectangleGeometry(width, height, corner_radii, &m_localRect, &m_localRectanglePath,
                             &m_hasRoundedCorners, &m_hasUniformCorners);
}

const QRectF& SnowCanvasSceneItem::localRect() const {
    return m_localRect;
}
const QPainterPath& SnowCanvasSceneItem::localRectanglePath() const {
    return m_localRectanglePath;
}
bool SnowCanvasSceneItem::hasRoundedCorners() const {
    return m_hasRoundedCorners;
}
bool SnowCanvasSceneItem::hasUniformCorners() const {
    return m_hasUniformCorners;
}

SnowCanvasOverlayItem::SnowCanvasOverlayItem() : SnowOverlayDisplayItem{} {
    refreshPointers();
    rebuildLocalRectangleGeometry();
}

SnowCanvasOverlayItem::SnowCanvasOverlayItem(const SnowOverlayDisplayItem& item) {
    assign(item);
}

SnowCanvasOverlayItem::SnowCanvasOverlayItem(const SnowCanvasOverlayItem& other)
    : SnowOverlayDisplayItem(static_cast<const SnowOverlayDisplayItem&>(other)),
      viewBounds(other.viewBounds), m_arrowPoints(other.m_arrowPoints),
      m_arrowPathCommands(other.m_arrowPathCommands),
      m_arrowheadPrimitives(other.m_arrowheadPrimitives), m_localRect(other.m_localRect),
      m_localRectanglePath(other.m_localRectanglePath),
      m_hasRoundedCorners(other.m_hasRoundedCorners),
      m_hasUniformCorners(other.m_hasUniformCorners),
      m_strokeRenderingGeometry(other.m_strokeRenderingGeometry),
      m_strokeRenderingGeometryRevision(other.m_strokeRenderingGeometryRevision) {
    refreshPointers();
}

SnowCanvasOverlayItem::SnowCanvasOverlayItem(SnowCanvasOverlayItem&& other) noexcept
    : SnowOverlayDisplayItem(static_cast<const SnowOverlayDisplayItem&>(other)),
      viewBounds(other.viewBounds), m_arrowPoints(std::move(other.m_arrowPoints)),
      m_arrowPathCommands(std::move(other.m_arrowPathCommands)),
      m_arrowheadPrimitives(std::move(other.m_arrowheadPrimitives)), m_localRect(other.m_localRect),
      m_localRectanglePath(std::move(other.m_localRectanglePath)),
      m_hasRoundedCorners(other.m_hasRoundedCorners),
      m_hasUniformCorners(other.m_hasUniformCorners),
      m_strokeRenderingGeometry(std::move(other.m_strokeRenderingGeometry)),
      m_strokeRenderingGeometryRevision(other.m_strokeRenderingGeometryRevision) {
    refreshPointers();
    other.refreshPointers();
}

SnowCanvasOverlayItem& SnowCanvasOverlayItem::operator=(const SnowCanvasOverlayItem& other) {
    if (this != &other) {
        assign(other);
        viewBounds = other.viewBounds;
        m_strokeRenderingGeometry = other.m_strokeRenderingGeometry;
        m_strokeRenderingGeometryRevision = other.m_strokeRenderingGeometryRevision;
    }
    return *this;
}

SnowCanvasOverlayItem& SnowCanvasOverlayItem::operator=(SnowCanvasOverlayItem&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    static_cast<SnowOverlayDisplayItem&>(*this) = other;
    viewBounds = other.viewBounds;
    m_arrowPoints = std::move(other.m_arrowPoints);
    m_arrowPathCommands = std::move(other.m_arrowPathCommands);
    m_arrowheadPrimitives = std::move(other.m_arrowheadPrimitives);
    m_localRect = other.m_localRect;
    m_localRectanglePath = std::move(other.m_localRectanglePath);
    m_hasRoundedCorners = other.m_hasRoundedCorners;
    m_hasUniformCorners = other.m_hasUniformCorners;
    m_strokeRenderingGeometry = std::move(other.m_strokeRenderingGeometry);
    m_strokeRenderingGeometryRevision = other.m_strokeRenderingGeometryRevision;
    refreshPointers();
    other.refreshPointers();
    return *this;
}

SnowCanvasOverlayItem& SnowCanvasOverlayItem::operator=(const SnowOverlayDisplayItem& item) {
    assign(item);
    return *this;
}

void SnowCanvasOverlayItem::assign(const SnowOverlayDisplayItem& item) {
    if (!sameBorrowed(m_arrowPoints, item.arrow_points, item.arrow_point_count) ||
        !sameBorrowed(m_arrowPathCommands, item.arrow_path_commands,
                      item.arrow_path_command_count) ||
        !sameBorrowed(m_arrowheadPrimitives, item.arrowhead_primitives,
                      item.arrowhead_primitive_count)) {
        ++m_strokeRenderingGeometryRevision;
    }
    static_cast<SnowOverlayDisplayItem&>(*this) = item;
    assignBorrowed(m_arrowPoints, item.arrow_points, item.arrow_point_count);
    assignBorrowed(m_arrowPathCommands, item.arrow_path_commands, item.arrow_path_command_count);
    assignBorrowed(m_arrowheadPrimitives, item.arrowhead_primitives,
                   item.arrowhead_primitive_count);
    refreshPointers();
    rebuildLocalRectangleGeometry();
}

void SnowCanvasOverlayItem::refreshPointers() {
    arrow_points = dataOrNull(m_arrowPoints);
    arrow_point_count = static_cast<std::uint32_t>(m_arrowPoints.size());
    arrow_path_commands = dataOrNull(m_arrowPathCommands);
    arrow_path_command_count = static_cast<std::uint32_t>(m_arrowPathCommands.size());
    arrowhead_primitives = dataOrNull(m_arrowheadPrimitives);
    arrowhead_primitive_count = static_cast<std::uint32_t>(m_arrowheadPrimitives.size());
}

void SnowCanvasOverlayItem::rebuildLocalRectangleGeometry() {
    rebuildRectangleGeometry(width, height, corner_radii, &m_localRect, &m_localRectanglePath,
                             &m_hasRoundedCorners, &m_hasUniformCorners);
}

const QRectF& SnowCanvasOverlayItem::localRect() const {
    return m_localRect;
}
const QPainterPath& SnowCanvasOverlayItem::localRectanglePath() const {
    return m_localRectanglePath;
}
bool SnowCanvasOverlayItem::hasRoundedCorners() const {
    return m_hasRoundedCorners;
}
bool SnowCanvasOverlayItem::hasUniformCorners() const {
    return m_hasUniformCorners;
}

snow_canvas_stroke_rendering::Cache& SnowCanvasOverlayItem::strokeRenderingCache() const {
    if (!m_strokeRenderingGeometry) {
        m_strokeRenderingGeometry = std::make_shared<snow_canvas_stroke_rendering::Cache>();
        snow_canvas_stroke_rendering::recordCacheAllocation(false);
    } else if (m_strokeRenderingGeometry.use_count() != 1) {
        m_strokeRenderingGeometry =
            std::make_shared<snow_canvas_stroke_rendering::Cache>(*m_strokeRenderingGeometry);
        snow_canvas_stroke_rendering::recordCacheAllocation(true);
    }
    return *m_strokeRenderingGeometry;
}

const snow_canvas_stroke_rendering::Cache*
SnowCanvasOverlayItem::strokeRenderingCacheIfPresent() const {
    return m_strokeRenderingGeometry.get();
}

std::uint64_t SnowCanvasOverlayItem::strokeRenderingGeometryRevision() const {
    return m_strokeRenderingGeometryRevision;
}

SnowCanvasSceneItem::PathGeometryDiagnostics SnowCanvasSceneItem::takePathGeometryDiagnostics() {
    return std::exchange(pathGeometryDiagnostics, {});
}
