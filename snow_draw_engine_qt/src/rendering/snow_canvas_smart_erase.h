#pragma once

#include "snow_canvas_display_item.h"
#include "snow_draw_engine_qt/snow_canvas_smart_erase.h"
#include "snow_canvas_display_cache.h"

#include <QPainterPath>
#include <atomic>
#include <functional>
#include <memory>

class QPainter;

namespace snow_canvas_smart_erase {

struct Result {
    QImage original;
    QImage filled;
    QRectF canvasRect;
    bool success = false;
};

QPainterPath path(const SnowCanvasSceneItem& item);
QByteArray geometryKey(const SnowCanvasSceneItem& item);
Result reconstruct(const SnowCanvasSceneItem& item, const QList<SnowCanvasBaseImageSource>& sources,
                   const std::atomic_bool& cancelled);
bool hasItems(const SnowCanvasSmartEraseSnapshot& snapshot);
void applySnapshot(std::vector<SnowCanvasSceneItem>& items,
                   const SnowCanvasSmartEraseSnapshot& snapshot);
void paint(QPainter& painter, const SceneDisplayInfo& info, const SnowCanvasSceneItem& item,
           const SnowCanvasSmartEraseSnapshot& snapshot);

class Coordinator {
  public:
    using Compute =
        std::function<Result(const SnowCanvasSceneItem&, const QList<SnowCanvasBaseImageSource>&,
                             const std::atomic_bool&)>;
    explicit Coordinator(std::function<void()> repaint, Compute compute = reconstruct);
    ~Coordinator();
    void setSources(const void* owner, const QList<SnowCanvasBaseImageSource>& sources);
    void removeSources(const void* owner);
    void sync(SnowRuntime runtime);
    void syncItems(std::vector<SnowCanvasSceneItem> items);
    void reset();
    SnowCanvasSmartEraseSnapshot snapshot() const;
    void restoreSnapshot(const SnowCanvasSmartEraseSnapshot& snapshot);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_canvas_smart_erase
