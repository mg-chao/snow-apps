// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/app/mcp/mcpapplicationservice.h"
#include "snow_shot/app/mcp/mcpdocumentservice.h"
#include "snow_shot/app/mcp/mcpruntimelifecycle.h"
#include "snow_shot/app/mcp/mcpsourceresolver.h"
#include "snow_shot/app/mcp/screenshotmcpsession.h"

class ScreenshotController;
namespace snow_shot::presentation {
class PinnedWindowGroupManager;
}

namespace snow_shot::app::mcp {
// Owns one listener and its replaceable service graph. The application supplies domain
// operations; this owner supplies cross-service routing and joins work before releasing them.
// All calls run on the GUI thread. Borrowed ports must outlive stop(); their cancellation and
// shutdown callbacks must not throw or synchronously destroy the graph's application owner.
// Desired enable state, failure retry and suspension policy belong to McpRuntimeLifecycle.
class McpRuntimeGraph final : public QObject {
    Q_OBJECT
  public:
    struct Ports {
        McpSourceResolver::Ports sources;
        McpApplicationService::Ports application;
        McpDocumentService::Ports documents;
        ScreenshotMcpSession::Ports session;
        ScreenshotController* screenshots = nullptr;
        presentation::PinnedWindowGroupManager* groups = nullptr;
        // Native selected-text objects share the application service's lifetime. Shutdown
        // requests cancellation before that service and its children are destroyed.
        std::function<std::function<void()>(QObject*,
                                            McpApplicationService::Ports::SelectedTextCompletion)>
            selectedText;
    };

    explicit McpRuntimeGraph(QObject* parent = nullptr);
    ~McpRuntimeGraph() override;
    // Construct a fresh graph and open its listener. A failed/throwing start may leave a
    // partial graph; its lifecycle owner must call stop() before retrying.
    [[nodiscard]] McpRuntimeLifecycle::StartOutcome start(Ports ports);
    void drain(std::function<void()> done);
    // Synchronous worker barrier, idempotent. The listener is retained to expose its last
    // startup error; every terminal service is replaced on the next start.
    void stop();
    void setSourcesSuspended(bool suspended);
    // Preserve the application's existing admission policy: canceled workers still occupy
    // lanes; delayed document sources are represented by their running job reservations.
    [[nodiscard]] bool busy() const;
    [[nodiscard]] McpStartupError startupError() const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::app::mcp
