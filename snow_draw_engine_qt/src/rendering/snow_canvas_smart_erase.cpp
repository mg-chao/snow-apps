#include "snow_canvas_smart_erase.h"

#include "snow_canvas_render_geometry.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QPainter>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

struct SnowCanvasSmartEraseSnapshot::Data {
    struct Entry {
        SnowCanvasSceneItem item;
        QByteArray geometry;
        std::shared_ptr<const snow_canvas_smart_erase::Result> result;
    };
    std::vector<Entry> entries;
};

namespace snow_canvas_smart_erase {
namespace {
quint64 elementKey(const SnowCanvasSceneItem& item) {
    return (static_cast<quint64>(item.element_id.generation) << 32) | item.element_id.index;
}

QThreadPool& workerPool() {
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(2);
        return true;
    }();
    (void)configured;
    return pool;
}

struct Completed {
    quint64 id = 0;
    QByteArray key;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::shared_ptr<const Result> result;
};
struct Mailbox {
    std::mutex mutex;
    std::vector<Completed> completed;
};
} // namespace

QByteArray geometryKey(const SnowCanvasSceneItem& item) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << item.center_x << item.center_y << item.width << item.height << item.rotation
           << item.stroke_width << item.is_free_draw << item.arrow_point_count;
    for (std::uint32_t i = 0; i < item.arrow_point_count; ++i) {
        stream << item.arrow_points[i].x << item.arrow_points[i].y;
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

struct Coordinator::Impl {
    struct Record {
        SnowCanvasSmartEraseSnapshot::Data::Entry entry;
        QByteArray key;
        std::shared_ptr<std::atomic_bool> cancelled;
        bool attempted = false;
    };
    explicit Impl(std::function<void()> notify, Compute work)
        : repaint(std::move(notify)), compute(std::move(work)) {
        timer.setInterval(16);
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] { collect(); });
    }
    ~Impl() {
        cancelAll();
    }
    void cancelAll() {
        for (auto& [id, record] : records) {
            (void)id;
            if (record.cancelled)
                record.cancelled->store(true);
        }
    }
    void collect() {
        std::vector<Completed> completed;
        {
            std::lock_guard lock(mailbox->mutex);
            completed.swap(mailbox->completed);
        }
        bool changed = false;
        for (auto& completion : completed) {
            if (activeJobs > 0)
                --activeJobs;
            const auto found = records.find(completion.id);
            if (completion.cancelled->load() || found == records.end() ||
                found->second.key != completion.key)
                continue;
            auto& record = found->second;
            if (completion.result->success) {
                record.entry.result = completion.result;
                cache.insert(completion.key, completion.result);
                cacheOrder.push_back(completion.key);
                retainedBytes += completion.result->original.sizeInBytes() +
                                 completion.result->filled.sizeInBytes();
                // Current records retain their results; only historical reuse is evicted.
                while (retainedBytes > 128 * 1024 * 1024 && !cacheOrder.empty()) {
                    const auto oldest = cacheOrder.front();
                    cacheOrder.erase(cacheOrder.begin());
                    const auto value = cache.take(oldest);
                    if (value)
                        retainedBytes -=
                            value->original.sizeInBytes() + value->filled.sizeInBytes();
                }
            }
            changed = true;
        }
        if (activeJobs == 0)
            timer.stop();
        if (changed)
            repaint();
    }
    std::function<void()> repaint;
    Compute compute;
    QTimer timer;
    std::map<const void*, QList<SnowCanvasBaseImageSource>> owners;
    QList<SnowCanvasBaseImageSource> sources;
    QByteArray sourceKey;
    quint64 sourceRevision = 0;
    std::map<quint64, Record> records;
    QHash<QByteArray, std::shared_ptr<const Result>> cache;
    std::vector<QByteArray> cacheOrder;
    qsizetype retainedBytes = 0;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    unsigned activeJobs = 0;
    SnowCanvasSmartEraseSnapshot frozen;
};

Coordinator::Coordinator(std::function<void()> repaint, Compute compute)
    : m_impl(std::make_unique<Impl>(std::move(repaint), std::move(compute))) {}
Coordinator::~Coordinator() = default;

void Coordinator::setSources(const void* owner, const QList<SnowCanvasBaseImageSource>& sources) {
    auto& state = *m_impl;
    state.owners[owner] = sources;
    QList<SnowCanvasBaseImageSource> combined;
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    for (const auto& [sourceOwner, layers] : state.owners) {
        (void)sourceOwner;
        for (const auto& layer : layers) {
            const auto finiteRect = [](const QRectF& rect) {
                return std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
                       std::isfinite(rect.width()) && std::isfinite(rect.height());
            };
            if (layer.image.isNull() || layer.canvasRect.isEmpty() ||
                !finiteRect(layer.canvasRect) || !finiteRect(layer.coverage))
                continue;
            const auto duplicate =
                std::find_if(combined.begin(), combined.end(), [&](const auto& other) {
                    return layer.image.cacheKey() == other.image.cacheKey() &&
                           layer.canvasRect == other.canvasRect && layer.coverage == other.coverage;
                });
            if (duplicate != combined.end())
                continue;
            stream << layer.image.cacheKey() << layer.canvasRect << layer.coverage;
            combined.push_back(layer);
        }
    }
    if (bytes == state.sourceKey)
        return;
    state.cancelAll();
    state.sourceKey = bytes;
    state.sources = std::move(combined);
    ++state.sourceRevision;
    state.cache.clear();
    state.cacheOrder.clear();
    state.retainedBytes = 0;
    state.frozen = {};
    for (auto& [id, record] : state.records) {
        (void)id;
        record.entry.result.reset();
    }
    state.repaint();
}

void Coordinator::removeSources(const void* owner) {
    setSources(owner, {});
    m_impl->owners.erase(owner);
}

void Coordinator::reset() {
    auto& state = *m_impl;
    state.cancelAll();
    state.records.clear();
    state.cache.clear();
    state.cacheOrder.clear();
    state.retainedBytes = 0;
    state.frozen = {};
    ++state.sourceRevision;
}

void Coordinator::sync(SnowRuntime runtime) {
    auto& state = *m_impl;
    if (state.frozen.data || !runtime)
        return;
    std::vector<SnowCanvasSceneItem> items;
    const auto visitor = [](void* context, const SnowSceneDisplayItem* item) {
        static_cast<std::vector<SnowCanvasSceneItem>*>(context)->emplace_back(*item);
    };
    if (snow_runtime_visit_smart_erase(runtime, visitor, &items) != SNOW_OK)
        return;
    syncItems(std::move(items));
}

void Coordinator::syncItems(std::vector<SnowCanvasSceneItem> items) {
    auto& state = *m_impl;
    if (state.frozen.data)
        return;
    std::map<quint64, Impl::Record> next;
    for (auto& item : items) {
        const quint64 id = elementKey(item);
        const QByteArray geometry = geometryKey(item);
        const QByteArray key = QByteArray::number(state.sourceRevision) + ':' +
                               QByteArray::number(id) + ':' + geometry;
        const auto existing = state.records.find(id);
        const bool same = existing != state.records.end() && existing->second.key == key;
        const bool becameStable = existing != state.records.end() &&
                                  existing->second.entry.item.filter.render_phase != 0 &&
                                  item.filter.render_phase == 0;
        Impl::Record record;
        if (same)
            record = existing->second;
        else if (existing != state.records.end() && existing->second.cancelled)
            existing->second.cancelled->store(true);
        record.entry.item = std::move(item);
        record.entry.geometry = geometry;
        record.key = key;
        if (record.entry.item.filter.render_phase != 0) {
            if (record.cancelled)
                record.cancelled->store(true);
            record.attempted = false;
        } else {
            if (!record.entry.result)
                record.entry.result = state.cache.value(key);
            if (!record.entry.result && (!record.attempted || becameStable) &&
                !state.sources.empty()) {
                record.cancelled = std::make_shared<std::atomic_bool>(false);
                record.attempted = true;
                const auto cancelled = record.cancelled;
                const auto workItem = record.entry.item;
                const auto sources = state.sources;
                const auto mailbox = state.mailbox;
                const auto compute = state.compute;
                ++state.activeJobs;
                state.timer.start();
                workerPool().start([id, key, cancelled, workItem, sources, mailbox, compute] {
                    auto result = std::make_shared<Result>();
                    try {
                        if (!cancelled->load())
                            *result = compute(workItem, sources, *cancelled);
                    } catch (const std::exception&) {
                        // Failure is represented by the unchanged unsuccessful result.
                    }
                    std::lock_guard lock(mailbox->mutex);
                    mailbox->completed.push_back({id, key, cancelled, std::move(result)});
                });
            }
        }
        next.emplace(id, std::move(record));
    }
    for (auto& [id, record] : state.records) {
        if (next.find(id) == next.end() && record.cancelled)
            record.cancelled->store(true);
    }
    state.records = std::move(next);
}

SnowCanvasSmartEraseSnapshot Coordinator::snapshot() const {
    if (m_impl->frozen.data)
        return m_impl->frozen;
    auto data = std::make_shared<SnowCanvasSmartEraseSnapshot::Data>();
    for (const auto& [id, record] : m_impl->records) {
        (void)id;
        data->entries.push_back(record.entry);
    }
    return {std::move(data)};
}

void Coordinator::restoreSnapshot(const SnowCanvasSmartEraseSnapshot& snapshot) {
    reset();
    m_impl->frozen = snapshot;
}

void paint(QPainter& painter, const SceneDisplayInfo& info, const SnowCanvasSceneItem& item,
           const SnowCanvasSmartEraseSnapshot& snapshot) {
    const Result* result = nullptr;
    if (item.filter.render_phase == 0 && snapshot.data) {
        const auto geometry = geometryKey(item);
        for (const auto& entry : snapshot.data->entries) {
            if (elementKey(entry.item) == elementKey(item) && entry.geometry == geometry &&
                entry.item.filter.render_phase == 0 && entry.result) {
                result = entry.result.get();
                break;
            }
        }
    }
    const auto projection = snow_canvas_render_geometry::sceneProjection(info);
    QTransform transform;
    transform.translate(info.surface_width / 2.0, info.surface_height / 2.0);
    transform.scale(projection.cameraZoom, projection.cameraZoom);
    transform.translate(-info.camera_center_x, -info.camera_center_y);
    const QPainterPath shape = transform.map(path(item));
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (result) {
        if (!result->canvasRect.isEmpty() && item.opacity > 0) {
            painter.setClipPath(shape, Qt::IntersectClip);
            const QRectF rect = transform.mapRect(result->canvasRect);
            painter.drawImage(rect, result->original);
            painter.setOpacity(painter.opacity() * item.opacity);
            painter.drawImage(rect, result->filled);
        }
    } else {
        painter.setBrush(QColor(255, 77, 79, 51));
        painter.setPen(item.is_free_draw != 0 ? QPen(Qt::NoPen) : QPen(QColor(255, 77, 79), 1.0));
        painter.drawPath(shape);
    }
    painter.restore();
}

bool hasItems(const SnowCanvasSmartEraseSnapshot& snapshot) {
    return snapshot.data && !snapshot.data->entries.empty();
}

void applySnapshot(std::vector<SnowCanvasSceneItem>& items,
                   const SnowCanvasSmartEraseSnapshot& snapshot) {
    if (!snapshot.data)
        return;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const auto& item) {
                                   return item.kind == SNOW_SCENE_DISPLAY_ITEM_FILTER &&
                                          item.filter.filter_type == 5;
                               }),
                items.end());
    std::vector<SnowCanvasSceneItem> smart;
    for (const auto& entry : snapshot.data->entries)
        smart.push_back(entry.item);
    items.insert(items.begin(), smart.begin(), smart.end());
}
} // namespace snow_canvas_smart_erase
