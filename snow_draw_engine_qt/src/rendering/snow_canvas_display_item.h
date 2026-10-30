#pragma once

#include "snow_draw_engine.h"

#include <QByteArray>
#include <QPainterPath>
#include <QRectF>

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace snow_canvas_stroke_rendering {
struct Cache;
}

class SnowCanvasSceneItem : public SnowSceneDisplayItem {
  public:
    struct MagnifierGeometryCache {
        std::array<double, 14> key{};
        std::uint8_t shape = 0;
        bool valid = false;
        bool sourceContained = false;
        QPainterPath lens;
        QPainterPath source;
        QPainterPath hull;
        QPainterPath outsideSource;
        QPainterPath outsideLens;
        QPainterPath outsideBoth;
        QPainterPath disjointBridge;
        double physicalScale = 0.0;
        double failedPhysicalScale = 0.0;
        double exteriorGuard = 0.0;
    };
    struct PathGeometryDiagnostics {
        std::uint64_t builds = 0;
        std::uint64_t reuses = 0;
        std::uint64_t reads = 0;
    };
    static PathGeometryDiagnostics takePathGeometryDiagnostics();
    SnowCanvasSceneItem();
    ~SnowCanvasSceneItem();
    explicit SnowCanvasSceneItem(const SnowSceneDisplayItem& item);
    SnowCanvasSceneItem(const SnowCanvasSceneItem& other);
    SnowCanvasSceneItem(SnowCanvasSceneItem&& other) noexcept;
    SnowCanvasSceneItem& operator=(const SnowCanvasSceneItem& other);
    SnowCanvasSceneItem& operator=(SnowCanvasSceneItem&& other) noexcept;
    SnowCanvasSceneItem& operator=(const SnowSceneDisplayItem& item);

    void setArrowPoints(const SnowArrowPoint* points, std::uint32_t count);
    bool applyPenFilterGeometryPatch(std::uint64_t expectedRevision, std::uint64_t revision,
                                     std::uint32_t retainPrefixCount,
                                     const SnowArrowPoint* appendedPoints,
                                     std::uint32_t appendCount, bool fullReset);
    bool applyPathGeometryPatch(std::uint64_t expectedRevision, std::uint64_t revision,
                                const SnowPathChunkRange* ranges, std::uint32_t rangeCount,
                                const SnowPathChunk* chunks, std::uint32_t chunkCount,
                                const SnowArrowPathCommand* commands, std::uint32_t commandCount,
                                bool closed, bool fullReset,
                                std::uint8_t role = SNOW_PATH_GEOMETRY_ROLE_CENTERLINE);
    void setTextUtf8(const QByteArray& text);
    void setFontFamilyUtf8(const QByteArray& family);
    void rebuildLocalRectangleGeometry();

    const QRectF& localRect() const;
    const QPainterPath& localRectanglePath() const;
    bool hasRoundedCorners() const;
    bool hasUniformCorners() const;
    struct PenSegmentChunk {
        std::uint32_t firstPoint = 0;
        std::uint32_t pointCount = 0;
        QRectF canvasBounds;
        std::uint64_t revision = 0;
        std::vector<std::int64_t> spatialCells;
    };
    const std::vector<PenSegmentChunk>& penSegmentChunks() const;
    const std::vector<SnowArrowPoint>& penFilterPoints() const;
    void queryPenSegmentChunks(const QRectF& canvasBounds,
                               std::vector<std::uint32_t>* outChunkIndices,
                               std::size_t* candidateCount = nullptr) const;
    void takePenFilterGeometryDiagnostics(std::size_t* chunkBuildCount,
                                          std::size_t* chunkReuseCount) const;
    std::uint64_t penFilterGeometryRevision() const;
    struct PathChunk {
        std::uint64_t stableId = 0;
        // Unique across documents and cache lifetimes, retained when chunks are copied/reused.
        std::uint64_t cacheId = 0;
        std::uint64_t geometryFingerprint = 0;
        std::uint32_t commandStart = 0;
        QRectF canvasBounds;
        double cumulativeStartLength = 0.0;
        QPainterPath canvasPath;
        bool startsSubpath = false;
        std::uint32_t subpathCount = 0;
        std::vector<std::int64_t> spatialCells;
    };
    const std::vector<PathChunk>& pathChunks() const;
    const std::vector<PathChunk>& pressurePathChunks() const;
    void queryPressurePathChunks(const QRectF& canvasBounds,
                                 std::vector<std::uint32_t>* outChunkIndices) const;
    QRectF pathCanvasBounds() const;
    QRectF pressureCanvasBounds() const;
    std::uint64_t pressureGeometryRevision() const;
    const QPainterPath& aggregateClosedPath() const;
    bool pathIsClosed() const;
    void queryPathChunks(const QRectF& canvasBounds,
                         std::vector<std::uint32_t>* outChunkIndices) const;
    std::uint64_t pathGeometryRevision() const;
    bool needsArrowTextClip(double canvasAntialiasGuard) const;
    // Retain vector geometry with the owned snapshot. The renderer checks the key
    // because callers may also update the public display fields directly.
    MagnifierGeometryCache& magnifierGeometryCache() const;
    snow_canvas_stroke_rendering::Cache& strokeRenderingCache() const;
    const snow_canvas_stroke_rendering::Cache* strokeRenderingCacheIfPresent() const;
    std::uint64_t strokeRenderingGeometryRevision() const;

    QRectF viewBounds;

  private:
    void assign(const SnowSceneDisplayItem& item);
    void refreshPointers();
    void rebuildPenFilterGeometry(std::size_t firstChangedPoint);
    struct PathStorage {
        std::vector<PathChunk> chunks;
        std::unordered_map<std::int64_t, std::vector<std::uint32_t>> spatialCells;
        std::array<std::multiset<double>, 4> bounds;
        std::uint64_t revision = 0;
    };
    static void removePathIndex(PathStorage& storage, std::size_t start, std::size_t end);
    static void insertPathIndex(PathStorage& storage, std::size_t start, std::size_t end);
    static QRectF storageBounds(const PathStorage& storage);
    static void queryPathStorage(const PathStorage& storage, const QRectF& canvasBounds,
                                 std::vector<std::uint32_t>* outChunkIndices);
    void rebuildAggregateClosedPath(bool geometryChanged = true);

    std::vector<SnowArrowPoint> m_arrowPoints;
    std::vector<SnowArrowPathCommand> m_arrowPathCommands;
    std::vector<SnowArrowheadPrimitive> m_arrowheadPrimitives;
    QByteArray m_textUtf8;
    QByteArray m_fontFamilyUtf8;
    QRectF m_localRect;
    QPainterPath m_localRectanglePath;
    bool m_hasRoundedCorners = false;
    bool m_hasUniformCorners = false;
    std::vector<PenSegmentChunk> m_penGeometryChunks;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> m_penSpatialCells;
    std::uint64_t m_penGeometryRevision = 0;
    PathStorage m_pathGeometry;
    PathStorage m_pressureGeometry;
    QPainterPath m_aggregateClosedPath;
    bool m_pathClosed = false;
    mutable bool m_arrowTextClipCacheValid = false;
    mutable std::unique_ptr<MagnifierGeometryCache> m_magnifierGeometry;
    mutable std::shared_ptr<snow_canvas_stroke_rendering::Cache> m_strokeRenderingGeometry;
    std::uint64_t m_strokeRenderingGeometryRevision = 0;
    mutable bool m_arrowTextClipNeeded = true;
    mutable std::uint64_t m_arrowTextClipGeometryRevision = 0;
    mutable double m_arrowTextClipStrokeWidth = 0.0;
    mutable QRectF m_arrowTextClipBounds;
    mutable std::size_t m_pendingPenGeometryChunkBuildCount = 0;
    mutable std::size_t m_pendingPenGeometryChunkReuseCount = 0;
};
class SnowCanvasOverlayItem : public SnowOverlayDisplayItem {
  public:
    SnowCanvasOverlayItem();
    explicit SnowCanvasOverlayItem(const SnowOverlayDisplayItem& item);
    SnowCanvasOverlayItem(const SnowCanvasOverlayItem& other);
    SnowCanvasOverlayItem(SnowCanvasOverlayItem&& other) noexcept;
    SnowCanvasOverlayItem& operator=(const SnowCanvasOverlayItem& other);
    SnowCanvasOverlayItem& operator=(SnowCanvasOverlayItem&& other) noexcept;
    SnowCanvasOverlayItem& operator=(const SnowOverlayDisplayItem& item);

    void rebuildLocalRectangleGeometry();

    const QRectF& localRect() const;
    const QPainterPath& localRectanglePath() const;
    bool hasRoundedCorners() const;
    bool hasUniformCorners() const;
    snow_canvas_stroke_rendering::Cache& strokeRenderingCache() const;
    const snow_canvas_stroke_rendering::Cache* strokeRenderingCacheIfPresent() const;
    std::uint64_t strokeRenderingGeometryRevision() const;

    QRectF viewBounds;

  private:
    void assign(const SnowOverlayDisplayItem& item);
    void refreshPointers();

    std::vector<SnowArrowPoint> m_arrowPoints;
    std::vector<SnowArrowPathCommand> m_arrowPathCommands;
    std::vector<SnowArrowheadPrimitive> m_arrowheadPrimitives;
    QRectF m_localRect;
    QPainterPath m_localRectanglePath;
    bool m_hasRoundedCorners = false;
    bool m_hasUniformCorners = false;
    mutable std::shared_ptr<snow_canvas_stroke_rendering::Cache> m_strokeRenderingGeometry;
    std::uint64_t m_strokeRenderingGeometryRevision = 0;
};
