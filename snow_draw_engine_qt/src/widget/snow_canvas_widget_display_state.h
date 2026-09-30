#pragma once

#include "snow_canvas_display_cache.h"
#include "snow_canvas_state.h"

#include <QFont>
#include <QRect>
#include <QRegion>

class SnowCanvasTextEditorSession;
class SnowCanvasViewport;

class SnowCanvasWidgetDisplayState final {
  public:
    SnowCanvasWidgetDisplayState();

    SnowCanvasDisplayCache& displayCache();
    const SnowCanvasDisplayCache& displayCache() const;
    const snow_canvas_state::Snapshot& snapshot() const;

    void resetRetainedState();
    void resetDocumentRetainedState();
    std::uint64_t initializeEngine(SnowRuntime runtime, SnowCanvasViewport& viewport);
    bool refreshState(SnowRuntime runtime, SnowViewport viewport,
                      snow_canvas_state::Changes* outChanges = nullptr);
    QRegion syncDisplayCache(SnowRuntime runtime, SnowViewport viewport, const QRect& widgetRect,
                             const QFont& font, SnowCanvasTextEditorSession& textEditorSession,
                             bool showDirtyRects);

    const QRegion& dirtyVisualizationRegion() const;

  private:
    SnowCanvasDisplayCache m_displayCache;
    snow_canvas_state::Store m_engineState;
    QRegion m_dirtyVisualizationRegion;
};
