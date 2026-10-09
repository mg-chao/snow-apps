#pragma once

#include "snow_draw_engine.h"

#include <QByteArray>
#include <QPainterPath>
#include <QRectF>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

class SnowCanvasSceneItem : public SnowSceneDisplayItem {
  public:
    SnowCanvasSceneItem();
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
                                bool closed, bool fullReset);
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
        std::uint32_t commandStart = 0;
        QRectF canvasBounds;
        double cumulativeStartLength = 0.0;
        QPainterPath canvasPath;
        std::vector<std::int64_t> spatialCells;
    };
    const std::vector<PathChunk>& pathChunks() const;
    const QPainterPath& aggregateClosedPath() const;
    bool pathIsClosed() const;
    void queryPathChunks(const QRectF& canvasBounds,
                         std::vector<std::uint32_t>* outChunkIndices) const;
    std::uint64_t pathGeometryRevision() const;
    bool needsArrowTextClip(double canvasAntialiasGuard) const;

    QRectF viewBounds;

  private:
    void assign(const SnowSceneDisplayItem& item);
    void refreshPointers();
    void rebuildPenFilterGeometry(std::size_t firstChangedPoint);
    void rebuildPathSpatialIndex();
    void rebuildAggregateClosedPath();

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
    std::vector<PathChunk> m_pathChunks;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> m_pathSpatialCells;
    QPainterPath m_aggregateClosedPath;
    std::uint64_t m_pathGeometryRevision = 0;
    bool m_pathClosed = false;
    mutable bool m_arrowTextClipCacheValid = false;
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
};
