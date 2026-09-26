#pragma once

#include "snow_shot/app/mcp/screenshotmcpserver.h"
#include <memory>

namespace snow_shot::storage {
class ApplicationStorage;
}
namespace snow_shot::presentation {
class AppPermissionService;
namespace settings {
class SettingsRuntimeSession;
}
} // namespace snow_shot::presentation
namespace snow_shot::translation {
class TranslationService;
}
namespace snow_shot::update {
class UpdateService;
}

namespace snow_shot::app::mcp {
class McpJobRegistry;

// Application-wide operations do not acquire the legacy screenshot editor lease.
class McpApplicationService final : public QObject {
    Q_OBJECT
  public:
    struct Ports {
        storage::ApplicationStorage* storage = nullptr;
        presentation::settings::SettingsRuntimeSession* settings = nullptr;
        presentation::AppPermissionService* permissions = nullptr;
        translation::TranslationService* translation = nullptr;
        update::UpdateService* updates = nullptr;
        McpJobRegistry* jobs = nullptr;
        std::function<bool(const QString&, const QJsonObject&)> action;
        std::function<bool()> restartAllowed;
        std::function<bool(const QString&, const QString&)> historyAction;
        using SelectedTextCompletion = std::function<void(QString, QString)>;
        std::function<std::function<void()>(SelectedTextCompletion)> selectedText;
        std::function<QJsonObject(quint64, QByteArray, QString)> artifactWriter;
    };
    explicit McpApplicationService(Ports ports, QObject* parent = nullptr);
    ~McpApplicationService() override;
    [[nodiscard]] bool handles(const QString& method) const;
    [[nodiscard]] static QStringList methods();
    void request(const ScreenshotMcpRequest& request, ScreenshotMcpServer::Completion completion);
    void disconnected(quint64 connection);
    bool cancelRequest(quint64 connection, const QString& requestId);
    void shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::app::mcp
