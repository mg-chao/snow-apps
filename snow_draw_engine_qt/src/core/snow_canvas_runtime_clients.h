#pragma once

#include "snow_canvas_runtime_access.h"
#include "snow_draw_engine.h"

#include <QList>

#include <cstdint>
#include <vector>

class SnowCanvasRuntime;

namespace snow_canvas_runtime {

class ClientRegistry final {
  public:
    using DetachedClients = QList<Client*>;

    void registerClient(Client* client);
    void unregisterClient(Client* client);

    DetachedClients detachForRuntimeReplacement();
    void attachAfterRuntimeReplacement(const DetachedClients& clients, SnowRuntime runtime);
    void detachAndReleaseClients(SnowCanvasRuntime& owner);
    void abandonClientsWithoutDetach();
    void clearRenderState();
    void syncChangedViewports(const std::vector<std::uint64_t>& changedViewportIds);

  private:
    QList<Client*> m_clients;
};

} // namespace snow_canvas_runtime
