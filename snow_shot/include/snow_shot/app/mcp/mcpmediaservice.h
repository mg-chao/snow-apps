#ifndef SNOW_SHOT_APP_MCP_MCPMEDIASERVICE_H
#define SNOW_SHOT_APP_MCP_MCPMEDIASERVICE_H

#include "snow_shot/app/mcp/screenshotmcpserver.h"
#include <memory>

class ScreenshotController;
namespace snow_shot::presentation {
class PinnedWindowGroupManager;
}

namespace snow_shot::app::mcp {
class McpMediaService final : public QObject {
  public:
    McpMediaService(ScreenshotController& controller,
                    presentation::PinnedWindowGroupManager& groups, QObject* parent = nullptr);
    ~McpMediaService() override;
    [[nodiscard]] bool handles(const QString& method) const;
    void request(const ScreenshotMcpRequest& request, ScreenshotMcpServer::Completion completion);
    bool cancelRequest(quint64 connection, const QString& requestId);
    using ArtifactWriter = std::function<QJsonObject(quint64, QByteArray, QString)>;
    void setArtifactWriter(ArtifactWriter writer);
    using FileArtifactWriter =
        std::function<void(quint64, QString, QString, std::function<void(QJsonObject, QString)>)>;
    void setFileArtifactWriter(FileArtifactWriter writer);
    void disconnected(quint64 connection);
    void shutdown();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::app::mcp
#endif
