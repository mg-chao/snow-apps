#include "snow_canvas_runtime_clients.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <algorithm>

namespace snow_canvas_runtime {
namespace {

bool containsViewportId(const std::vector<std::uint64_t>& viewportIds, std::uint64_t viewportId) {
    return std::find(viewportIds.begin(), viewportIds.end(), viewportId) != viewportIds.end();
}

} // namespace

void ClientRegistry::registerClient(Client* client) {
    if (client == nullptr || m_clients.contains(client)) {
        return;
    }
    m_clients.push_back(client);
}

void ClientRegistry::unregisterClient(Client* client) {
    m_clients.removeAll(client);
}

ClientRegistry::DetachedClients ClientRegistry::detachForRuntimeReplacement() {
    DetachedClients detachedClients;
    detachedClients.reserve(m_clients.size());
    const QList<Client*> snapshot = m_clients;
    for (Client* client : snapshot) {
        if (client == nullptr || !m_clients.contains(client)) {
            continue;
        }
        client->detachRuntimeForReplacement();
        if (m_clients.contains(client)) {
            detachedClients.push_back(client);
        }
    }

    return detachedClients;
}

void ClientRegistry::attachAfterRuntimeReplacement(const DetachedClients& clients,
                                                   SnowRuntime runtime) {
    for (Client* client : clients) {
        if (client != nullptr && m_clients.contains(client)) {
            client->attachRuntime(runtime);
        }
    }
}

void ClientRegistry::detachAndReleaseClients(SnowCanvasRuntime& owner) {
    while (!m_clients.isEmpty()) {
        Client* client = m_clients.takeFirst();
        if (client != nullptr) {
            client->detachRuntimeOwner(&owner);
        }
    }
    m_clients.clear();
}

void ClientRegistry::abandonClientsWithoutDetach() {
    m_clients.clear();
}

void ClientRegistry::clearRenderState() {
    const QList<Client*> snapshot = m_clients;
    for (Client* client : snapshot) {
        if (client != nullptr && m_clients.contains(client)) {
            client->clearRenderState();
        }
    }
}

void ClientRegistry::syncChangedViewports(const std::vector<std::uint64_t>& changedViewportIds) {
    const QList<Client*> snapshot = m_clients;
    for (Client* client : snapshot) {
        if (client == nullptr || !m_clients.contains(client)) {
            continue;
        }
        if (containsViewportId(changedViewportIds, client->runtimeViewportId())) {
            client->syncAfterEngineMutation();
        } else {
            client->refreshStateFromEngine(true);
        }
    }
}

} // namespace snow_canvas_runtime
