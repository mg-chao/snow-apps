#pragma once

#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_viewport.h"

#include <cstdint>
#include <memory>
#include <vector>

class SnowCanvasRuntime;
class SnowCanvasWidgetDisplayState;

class SnowCanvasWidgetRuntimeBinding final {
  public:
    SnowCanvasWidgetRuntimeBinding();
    explicit SnowCanvasWidgetRuntimeBinding(SnowCanvasRuntime& runtime);
    ~SnowCanvasWidgetRuntimeBinding();

    SnowCanvasWidgetRuntimeBinding(const SnowCanvasWidgetRuntimeBinding&) = delete;
    SnowCanvasWidgetRuntimeBinding& operator=(const SnowCanvasWidgetRuntimeBinding&) = delete;

    SnowRuntime engine() const;
    SnowViewport viewportHandle() const;
    SnowCanvasViewport& viewport();
    const SnowCanvasViewport& viewport() const;
    std::uint64_t viewportId() const;
    bool hasViewport() const;
    bool clearDocumentPreservingViewports();

    void registerClient(snow_canvas_runtime::Client& client);
    void unregisterClient(snow_canvas_runtime::Client& client);

    void initializeAttachment(SnowCanvasWidgetDisplayState& displayState);
    void detachAttachment();
    void attachRuntime(SnowRuntime runtime, SnowCanvasWidgetDisplayState& displayState);
    bool isRuntimeOwner(SnowCanvasRuntime* owner) const;
    bool detachRuntimeOwner(SnowCanvasRuntime* owner);
    void syncChangedViewports(SnowChangedViewportList changedViewports);
    void syncChangedViewportIds(const std::vector<std::uint64_t>& changedViewportIds);

  private:
    std::unique_ptr<SnowCanvasRuntime> m_ownedRuntime;
    SnowCanvasRuntime* m_runtimeOwner = nullptr;
    SnowRuntime m_engine = nullptr;
    SnowCanvasViewport m_viewport;
    std::uint64_t m_currentViewportId = 0;
};
