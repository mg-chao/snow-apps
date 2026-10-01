#include "snow_canvas_widget_display_state.h"

#include "snow_canvas_lifecycle.h"
#include "snow_canvas_sync_plan.h"
#include "snow_canvas_text_editor_session.h"

SnowCanvasWidgetDisplayState::SnowCanvasWidgetDisplayState() {
    resetRetainedState();
}

SnowCanvasDisplayCache& SnowCanvasWidgetDisplayState::displayCache() {
    return m_displayCache;
}

const SnowCanvasDisplayCache& SnowCanvasWidgetDisplayState::displayCache() const {
    return m_displayCache;
}

const snow_canvas_state::Snapshot& SnowCanvasWidgetDisplayState::snapshot() const {
    return m_engineState.snapshot();
}

void SnowCanvasWidgetDisplayState::resetRetainedState() {
    resetDocumentRetainedState();
    m_engineState.reset();
}

void SnowCanvasWidgetDisplayState::resetDocumentRetainedState() {
    m_displayCache.reset(SnowColorRgba8{255, 255, 255, 255});
    m_dirtyVisualizationRegion = QRegion();
}

std::uint64_t SnowCanvasWidgetDisplayState::initializeEngine(SnowRuntime runtime,
                                                             SnowCanvasViewport& viewport) {
    const snow_canvas_lifecycle::InitializeEngineResult result =
        snow_canvas_lifecycle::initializeEngine(runtime, viewport, m_displayCache, m_engineState);
    return result.viewportId;
}

bool SnowCanvasWidgetDisplayState::refreshState(SnowRuntime runtime, SnowViewport viewport,
                                                snow_canvas_state::Changes* outChanges) {
    return m_engineState.refresh(runtime, viewport, outChanges);
}

QRegion SnowCanvasWidgetDisplayState::syncDisplayCache(
    SnowRuntime runtime, SnowViewport viewport, const QRect& widgetRect, const QFont& font,
    SnowCanvasTextEditorSession& textEditorSession, bool showDirtyRects) {
    const QRegion previousVisualizationRegion = m_dirtyVisualizationRegion;
    const bool textEditorActive = textEditorSession.isActive();
    const QRegion textEditorRegionBeforeSync =
        textEditorSession.editingRegion(font, m_displayCache.sceneInfo());
    if (!m_displayCache.sync(runtime, viewport)) {
        return {};
    }

    QRegion textEditorRegionAfterSync;
    QRegion activeDraftPresentationRegion;
    if (textEditorActive) {
        const bool syncedActiveDraft = textEditorSession.syncActiveDraftPresentation(
            runtime, viewport, font, m_displayCache.sceneInfo(), &activeDraftPresentationRegion);
        if (!syncedActiveDraft) {
            textEditorSession.updateGeometry(font, m_displayCache.sceneInfo());
        }
        textEditorRegionAfterSync =
            textEditorSession.editingRegion(font, m_displayCache.sceneInfo());
    }

    const snow_canvas_sync_plan::Plan plan =
        snow_canvas_sync_plan::build(snow_canvas_sync_plan::Request{
            snow_canvas_sync_plan::dirtyRegionsForCache(m_displayCache, widgetRect),
            previousVisualizationRegion,
            textEditorRegionBeforeSync,
            textEditorRegionAfterSync + activeDraftPresentationRegion,
            textEditorActive,
            showDirtyRects,
        });
    m_dirtyVisualizationRegion = plan.dirtyVisualizationRegion;
    return plan.updateRegion;
}

const QRegion& SnowCanvasWidgetDisplayState::dirtyVisualizationRegion() const {
    return m_dirtyVisualizationRegion;
}
