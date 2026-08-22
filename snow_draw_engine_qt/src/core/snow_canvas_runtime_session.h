#pragma once

#include "snow_canvas_runtime_clients.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_draw_engine.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <future>
#include <QByteArray>
#include <vector>

class SnowCanvasRuntime;

namespace snow_canvas_runtime {

enum class OwnerDestructionPolicy {
    DetachClients,
    AbandonClientsAndDestroyAsync,
};

class RuntimeSession final {
  public:
    RuntimeSession();
    explicit RuntimeSession(const SnowCanvasRuntimeConfig& config);
    ~RuntimeSession();

    bool isValid() const;
    bool reset();
    bool cloneDocumentSessionFrom(const RuntimeSession& source);
    QByteArray serializeDocumentSession() const;
    bool restoreDocumentSession(const QByteArray& payload);
    QByteArray serializeDocumentHistory() const;
    bool restoreDocumentHistory(const QByteArray& payload);
    bool restoreDocumentHistoryPreservingEditorStyles(const QByteArray& payload);
    bool clearDocumentPreservingViewports();
    bool setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools);
    void destroyAsync(SnowCanvasRuntime& owner);
    void destroyForOwnerDestruction(SnowCanvasRuntime& owner, OwnerDestructionPolicy policy);

    SnowRuntime handle() const;
    void registerClient(Client* client);
    void unregisterClient(Client* client);
    void syncChangedViewports(SnowChangedViewportList changedViewports);
    void syncChangedViewportIds(const std::vector<std::uint64_t>& changedViewportIds);

  private:
    bool replaceRuntime(ScopedRuntimeHandle replacement);
    void destroyRuntimeAsync();
    void waitForPendingDestroy();

    SnowCanvasRuntimeConfig m_config;
    ScopedRuntimeHandle m_runtime;
    ClientRegistry m_clients;
    std::future<void> m_pendingDestroy;
};

} // namespace snow_canvas_runtime
