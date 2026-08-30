#include "snow_canvas_filter_tile_cache.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

namespace snow_canvas_filter_tile_cache {
namespace {

std::uint64_t folded(QPoint point) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(point.x())) << 32) |
           static_cast<std::uint32_t>(point.y());
}

struct KeyHash {
    std::size_t operator()(const Key& key) const {
        std::size_t result = std::hash<const void*>{}(key.canvasNamespace);
        result ^= static_cast<std::size_t>(folded(key.tile) ^ (folded(key.tile) >> 32));
        result ^= static_cast<std::size_t>(key.sourceRect.left()) * 0x27d4eb2fu;
        result ^= static_cast<std::size_t>(key.sourceRect.top()) * 0x165667b1u;
        result ^= static_cast<std::size_t>(key.sourceRect.width()) * 0xc2b2ae3du;
        result ^= static_cast<std::size_t>(key.sourceRect.height()) * 0xc2b2ae3du;
        result ^= static_cast<std::size_t>(key.logicalSize.width()) * 0x9e3779b1u;
        result ^= static_cast<std::size_t>(key.logicalSize.height()) * 0x85ebca77u;
        result ^= static_cast<std::size_t>(key.devicePixelRatioBits ^
                                           (key.devicePixelRatioBits >> 32));
        result ^= static_cast<std::size_t>(key.contentKey ^ (key.contentKey >> 32));
        result ^= static_cast<std::size_t>(key.dependencyFingerprint ^
                                           (key.dependencyFingerprint >> 32));
        result ^= static_cast<std::size_t>(key.nodeFingerprint ^ (key.nodeFingerprint >> 32));
        return result;
    }
};

struct Retained {
    std::shared_ptr<Entry> entry;
    std::size_t bytes = 0;
    std::list<Key>::iterator lru;
};

struct Cache {
    std::mutex mutex;
    std::unordered_map<Key, Retained, KeyHash> entries;
    std::list<Key> lru;
    std::size_t bytes = 0;
    Diagnostics pending;
};

Cache& cache() {
    static Cache value;
    return value;
}

std::size_t imageBytes(const QImage& image) {
    return image.isNull() ? 0u : static_cast<std::size_t>(image.sizeInBytes());
}

void touchLocked(Cache& state, Retained& retained, const Key& key) {
    state.lru.erase(retained.lru);
    state.lru.push_front(key);
    retained.lru = state.lru.begin();
}

void evictLocked(Cache& state, Diagnostics* diagnostics) {
    while (state.bytes > kByteLimit && !state.lru.empty()) {
        const Key key = state.lru.back();
        state.lru.pop_back();
        const auto found = state.entries.find(key);
        if (found == state.entries.end()) {
            continue;
        }
        state.bytes -= found->second.bytes;
        state.entries.erase(found);
        ++state.pending.evictions;
        if (diagnostics != nullptr) {
            ++diagnostics->evictions;
        }
    }
}

} // namespace

bool Key::operator==(const Key& other) const {
    return canvasNamespace == other.canvasNamespace && tile == other.tile &&
           sourceRect == other.sourceRect &&
           logicalSize == other.logicalSize && devicePixelRatioBits == other.devicePixelRatioBits &&
           contentKey == other.contentKey && dependencyFingerprint == other.dependencyFingerprint &&
           nodeFingerprint == other.nodeFingerprint;
}

std::shared_ptr<const Entry> find(const Key& key, Diagnostics* diagnostics) {
    if (key.canvasNamespace == nullptr) {
        return {};
    }
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.entries.find(key);
    if (found == state.entries.end()) {
        if (diagnostics != nullptr) {
            ++diagnostics->misses;
        }
        ++state.pending.misses;
        return {};
    }
    touchLocked(state, found->second, key);
    if (diagnostics != nullptr) {
        ++diagnostics->hits;
    }
    ++state.pending.hits;
    return found->second.entry;
}

bool store(const Key& key, const QImage& image, const QRect& physicalRect,
           Diagnostics* diagnostics) {
    if (key.canvasNamespace == nullptr || image.isNull() || physicalRect.isEmpty()) {
        return false;
    }
    std::shared_ptr<Entry> retainedEntry;
    std::size_t bytes = 0;
    try {
        retainedEntry = std::make_shared<Entry>();
        retainedEntry->image = image.copy();
        retainedEntry->physicalRect = physicalRect;
        if (retainedEntry->image.isNull()) {
            return false;
        }
        bytes = imageBytes(retainedEntry->image);
    } catch (const std::bad_alloc&) {
        return false;
    }

    Cache& state = cache();
    try {
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto existing = state.entries.find(key);
        if (existing != state.entries.end()) {
            state.bytes -= existing->second.bytes;
            state.lru.erase(existing->second.lru);
            state.entries.erase(existing);
        }
        state.lru.push_front(key);
        try {
            state.entries.emplace(key,
                                  Retained{std::move(retainedEntry), bytes, state.lru.begin()});
        } catch (...) {
            state.lru.pop_front();
            throw;
        }
        state.bytes += bytes;
        evictLocked(state, diagnostics);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void invalidateNamespace(const void* canvasNamespace) {
    if (canvasNamespace == nullptr) {
        return;
    }
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (auto iterator = state.entries.begin(); iterator != state.entries.end();) {
        if (iterator->first.canvasNamespace != canvasNamespace) {
            ++iterator;
            continue;
        }
        state.bytes -= iterator->second.bytes;
        state.lru.erase(iterator->second.lru);
        iterator = state.entries.erase(iterator);
        ++state.pending.dependencyInvalidations;
    }
}

void invalidateRegion(const void* canvasNamespace, const QRect& logicalRegion, qreal devicePixelRatio,
                      std::uint64_t dependencyFingerprint, Diagnostics* diagnostics) {
    if (canvasNamespace == nullptr) {
        return;
    }
    const qreal dpr = std::max<qreal>(0.01, devicePixelRatio);
    const bool invalidateAll = logicalRegion.isEmpty();
    const QRect physicalRegion(
        static_cast<int>(std::floor(logicalRegion.left() * dpr)),
        static_cast<int>(std::floor(logicalRegion.top() * dpr)),
        std::max(1, static_cast<int>(std::ceil(logicalRegion.width() * dpr))),
        std::max(1, static_cast<int>(std::ceil(logicalRegion.height() * dpr))));
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (auto iterator = state.entries.begin(); iterator != state.entries.end();) {
        const Key& key = iterator->first;
        const bool fingerprintMatches = dependencyFingerprint == 0 ||
                                         key.dependencyFingerprint == dependencyFingerprint;
        if (key.canvasNamespace != canvasNamespace || !fingerprintMatches ||
            (!invalidateAll && !iterator->second.entry->physicalRect.intersects(physicalRegion))) {
            ++iterator;
            continue;
        }
        state.bytes -= iterator->second.bytes;
        state.lru.erase(iterator->second.lru);
        iterator = state.entries.erase(iterator);
        ++state.pending.dependencyInvalidations;
        if (diagnostics != nullptr) {
            ++diagnostics->dependencyInvalidations;
        }
    }
}

void invalidateRegion(const void* canvasNamespace, const QRegion& logicalRegion,
                      qreal devicePixelRatio, std::uint64_t dependencyFingerprint,
                      Diagnostics* diagnostics) {
    if (logicalRegion.isEmpty()) {
        invalidateRegion(canvasNamespace, QRect(), devicePixelRatio, dependencyFingerprint,
                         diagnostics);
        return;
    }
    for (const QRect& rect : logicalRegion) {
        invalidateRegion(canvasNamespace, rect, devicePixelRatio, dependencyFingerprint,
                         diagnostics);
    }
}

void clear() {
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.entries.clear();
    state.lru.clear();
    state.bytes = 0;
    state.pending = {};
}

Diagnostics takeDiagnostics() {
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    Diagnostics result = state.pending;
    result.retainedBytes = state.bytes;
    state.pending = {};
    return result;
}

std::size_t retainedBytes() {
    Cache& state = cache();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.bytes;
}

} // namespace snow_canvas_filter_tile_cache
