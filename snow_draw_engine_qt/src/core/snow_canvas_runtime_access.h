#pragma once

#include "snow_draw_engine.h"

#include <cstdint>
#include <vector>

class SnowCanvasRuntime;

namespace snow_canvas_runtime {

class Client {
  public:
    virtual ~Client() = default;

    virtual std::uint64_t runtimeViewportId() const = 0;
    virtual void detachRuntimeForReplacement() = 0;
    virtual void attachRuntime(SnowRuntime runtime) = 0;
    virtual void detachRuntimeOwner(SnowCanvasRuntime* runtimeOwner) = 0;
    virtual void clearRenderState() = 0;
    virtual void syncAfterEngineMutation() = 0;
    virtual void refreshStateFromEngine(bool emitSignals) = 0;
};

struct Access {
    static SnowRuntime handle(const SnowCanvasRuntime& runtime);
    static void registerClient(SnowCanvasRuntime& runtime, Client& client);
    static void unregisterClient(SnowCanvasRuntime& runtime, Client& client);
    static void syncChangedViewports(SnowCanvasRuntime& runtime,
                                     SnowChangedViewportList changedViewports);
    static void syncChangedViewportIds(SnowCanvasRuntime& runtime,
                                       const std::vector<std::uint64_t>& changedViewportIds);
};

} // namespace snow_canvas_runtime
