#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QRegion>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace snow_canvas_filter_tile_cache {

constexpr int kTilePhysicalSize = 256;
constexpr std::size_t kByteLimit = 64u * 1024u * 1024u;

struct Key {
    const void* canvasNamespace = nullptr;
    QPoint tile;
    QRect sourceRect;
    QSize logicalSize;
    std::uint64_t devicePixelRatioBits = 0;
    std::uint64_t contentKey = 0;
    std::uint64_t dependencyFingerprint = 0;
    std::uint64_t nodeFingerprint = 0;

    bool operator==(const Key& other) const;
};

struct Entry {
    QImage image;
    QRect physicalRect;
};

struct Diagnostics {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
    std::size_t candidateTiles = 0;
    std::size_t visitedTiles = 0;
    std::size_t mergedNodes = 0;
    std::size_t overlappingNodes = 0;
    std::size_t dependencyInvalidations = 0;
    std::size_t retainedBytes = 0;
};

std::shared_ptr<const Entry> find(const Key& key, Diagnostics* diagnostics = nullptr);
bool store(const Key& key, const QImage& image, const QRect& physicalRect,
           Diagnostics* diagnostics = nullptr);

void invalidateNamespace(const void* canvasNamespace);
void invalidateRegion(const void* canvasNamespace, const QRect& logicalRegion, qreal devicePixelRatio,
                      std::uint64_t dependencyFingerprint = 0,
                      Diagnostics* diagnostics = nullptr);
void invalidateRegion(const void* canvasNamespace, const QRegion& logicalRegion,
                      qreal devicePixelRatio, std::uint64_t dependencyFingerprint = 0,
                      Diagnostics* diagnostics = nullptr);
void clear();

Diagnostics takeDiagnostics();
std::size_t retainedBytes();

} // namespace snow_canvas_filter_tile_cache
