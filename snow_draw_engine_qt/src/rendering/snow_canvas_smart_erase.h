#pragma once

#include "snow_canvas_display_item.h"
#include "snow_draw_engine_qt/snow_canvas_smart_erase.h"
#include "snow_canvas_display_cache.h"

#include <QPainterPath>
#include <QSize>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class QPainter;

namespace snow_canvas_smart_erase {

struct Result {
    QImage original;
    QImage filled;
    QRectF canvasRect;
    bool success = false;
};

// Private reconstruction controls for quality tests and performance experiments.
// Application callers use reconstruct(), which always uses the selected defaults.
struct ReconstructionOptions {
    int coarsePasses = 5;
    int intermediatePasses = 3;
    int finePasses = 2;
    bool earlyRejection = true;
    bool parallelVoting = true;
};
struct LevelDiagnostics {
    QSize size;
    int maskedPixels = 0;
    int passes = 0;
    double searchMs = 0;
    double votingMs = 0;
};
struct ReconstructionDiagnostics {
    QSize workingSize;
    int maskedPixels = 0;
    enum class Path { Empty, Surface, Periodic, Patches } path = Path::Empty;
    double preparationMs = 0;
    double fastPathsMs = 0;
    double guidePyramidMs = 0;
    std::vector<LevelDiagnostics> levels;
};

Result reconstructWithOptions(const SnowCanvasSceneItem& item,
                              const QList<SnowCanvasBaseImageSource>& sources,
                              const std::atomic_bool& cancelled,
                              const ReconstructionOptions& options,
                              ReconstructionDiagnostics* diagnostics = nullptr);

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
