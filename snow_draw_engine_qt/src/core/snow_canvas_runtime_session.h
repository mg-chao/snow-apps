#pragma once

#include "snow_canvas_runtime_clients.h"
#include "snow_canvas_smart_erase.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_draw_engine.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QByteArray>
#include <optional>
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
    QByteArray serializeDocumentSnapshot() const;
    QByteArray documentCheckpoint() const;
    QByteArray serializeSelectedDrawTemplate() const;
    bool restoreDocumentSession(const QByteArray& payload);
    QByteArray serializeDocumentHistory() const;
    bool restoreDocumentHistory(const QByteArray& payload);
    bool restoreDocumentHistoryPreservingEditorStyles(const QByteArray& payload);
    bool clearDocumentPreservingViewports();
    void clearRenderState();
    bool setAutomaticallySelectNewlyCreatedElements(bool enabled);
    bool setAutomaticSelectionExcludedTools(const QSet<SnowCanvasTool>& tools);
    bool setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools);
    bool setSharedCreationColor(const std::optional<QColor>& color);
    void destroyAsync(SnowCanvasRuntime& owner);
    void destroyForOwnerDestruction(SnowCanvasRuntime& owner, OwnerDestructionPolicy policy);

    SnowRuntime handle() const;
    RuntimeLease retainHandle() const {
        return m_runtime;
    }
    snow_canvas_smart_erase::Coordinator& smartErase() {
        return m_smartErase;
    }
    void registerClient(Client* client);
    void unregisterClient(Client* client);
    void syncChangedViewports(SnowChangedViewportList changedViewports);
    void syncChangedViewportIds(const std::vector<std::uint64_t>& changedViewportIds);

  private:
    bool replaceRuntime(ScopedRuntimeHandle replacement);
    void destroyRuntimeAsync();

    SnowCanvasRuntimeConfig m_config;
    RuntimeLease m_runtime;
    std::uint64_t m_replacementGeneration = 0;
    mutable std::optional<std::uint64_t> m_checkpointRevision;
    mutable QByteArray m_checkpoint;
    ClientRegistry m_clients;
    snow_canvas_smart_erase::Coordinator m_smartErase;
    std::shared_ptr<const int> m_lifetime = std::make_shared<const int>(0);
};

} // namespace snow_canvas_runtime
