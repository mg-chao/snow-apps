#include "snow_canvas_widget_runtime_binding.h"

#include "snow_canvas_lifecycle.h"
#include "snow_canvas_widget_display_state.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

SnowCanvasWidgetRuntimeBinding::SnowCanvasWidgetRuntimeBinding()
    : m_ownedRuntime(std::make_unique<SnowCanvasRuntime>()), m_runtimeOwner(m_ownedRuntime.get()) {}

SnowCanvasWidgetRuntimeBinding::SnowCanvasWidgetRuntimeBinding(SnowCanvasRuntime& runtime)
    : m_runtimeOwner(&runtime) {}

SnowCanvasWidgetRuntimeBinding::~SnowCanvasWidgetRuntimeBinding() = default;

SnowRuntime SnowCanvasWidgetRuntimeBinding::engine() const {
    return m_engine;
}

SnowViewport SnowCanvasWidgetRuntimeBinding::viewportHandle() const {
    return m_viewport.get();
}

SnowCanvasViewport& SnowCanvasWidgetRuntimeBinding::viewport() {
    return m_viewport;
}

const SnowCanvasViewport& SnowCanvasWidgetRuntimeBinding::viewport() const {
    return m_viewport;
}

std::uint64_t SnowCanvasWidgetRuntimeBinding::viewportId() const {
    return m_currentViewportId;
}

bool SnowCanvasWidgetRuntimeBinding::hasViewport() const {
    return snow_canvas_lifecycle::hasViewport(m_engine, m_viewport);
}

bool SnowCanvasWidgetRuntimeBinding::clearDocumentPreservingViewports() {
    return m_runtimeOwner != nullptr && m_runtimeOwner->clearDocumentPreservingViewports();
}

void SnowCanvasWidgetRuntimeBinding::registerClient(snow_canvas_runtime::Client& client) {
    if (m_runtimeOwner != nullptr) {
        snow_canvas_runtime::Access::registerClient(*m_runtimeOwner, client);
    }
}

void SnowCanvasWidgetRuntimeBinding::unregisterClient(snow_canvas_runtime::Client& client) {
    if (m_runtimeOwner != nullptr) {
        snow_canvas_runtime::Access::unregisterClient(*m_runtimeOwner, client);
    }
}

void SnowCanvasWidgetRuntimeBinding::initializeAttachment(
    SnowCanvasWidgetDisplayState& displayState) {
    attachRuntime(m_runtimeOwner != nullptr ? snow_canvas_runtime::Access::handle(*m_runtimeOwner)
                                            : nullptr,
                  displayState);
}

void SnowCanvasWidgetRuntimeBinding::detachAttachment() {
    m_viewport.reset();
    m_engine = nullptr;
    m_currentViewportId = 0;
}

void SnowCanvasWidgetRuntimeBinding::attachRuntime(SnowRuntime runtime,
                                                   SnowCanvasWidgetDisplayState& displayState) {
    m_engine = runtime;
    m_currentViewportId = displayState.initializeEngine(m_engine, m_viewport);
}

bool SnowCanvasWidgetRuntimeBinding::isRuntimeOwner(SnowCanvasRuntime* owner) const {
    return m_runtimeOwner == owner;
}

bool SnowCanvasWidgetRuntimeBinding::detachRuntimeOwner(SnowCanvasRuntime* owner) {
    if (m_runtimeOwner != owner) {
        return false;
    }

    m_runtimeOwner = nullptr;
    detachAttachment();
    return true;
}

void SnowCanvasWidgetRuntimeBinding::syncChangedViewports(
    SnowChangedViewportList changedViewports) {
    if (m_runtimeOwner != nullptr) {
        snow_canvas_runtime::Access::syncChangedViewports(*m_runtimeOwner, changedViewports);
    }
}

void SnowCanvasWidgetRuntimeBinding::syncChangedViewportIds(
    const std::vector<std::uint64_t>& changedViewportIds) {
    if (m_runtimeOwner != nullptr) {
        snow_canvas_runtime::Access::syncChangedViewportIds(*m_runtimeOwner, changedViewportIds);
    }
}
