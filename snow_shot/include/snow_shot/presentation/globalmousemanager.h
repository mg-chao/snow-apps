#ifndef SNOW_SHOT_PRESENTATION_GLOBALMOUSEMANAGER_H
#define SNOW_SHOT_PRESENTATION_GLOBALMOUSEMANAGER_H

#include "snow_shot/presentation/globalmousegesture.h"

#include <QObject>
#include <functional>
#include <memory>

namespace snow_shot::presentation {
class GlobalMouseBackend {
  public:
    using Handler = std::function<void(GlobalMouseDragEvent)>;
    using FailureHandler = std::function<void(quint32)>;
    virtual ~GlobalMouseBackend() = default;
    virtual void start(Handler handler, FailureHandler failure) = 0;
    virtual void stop() = 0;
    virtual void configure(const GlobalMouseConfiguration& configuration) = 0;
    virtual void cancel(quint64 id) = 0;
    virtual void beginButtonDrag(settings::SettingsGlobalMouseAction action) = 0;
    virtual GlobalMousePermissionState permissionState() const {
        return {GlobalMousePermissionState::Status::Ready, true, true, true};
    }
    using StateHandler = std::function<void(GlobalMousePermissionState)>;
    virtual void setStateHandler(StateHandler) {}
    virtual void refreshPermission() {}
    virtual void usePermissionSnapshot(bool, bool) {}
    virtual void setPermissionRefreshHandler(std::function<void()>) {}
    virtual void requestPermission() {}
    virtual void openPermissionSettings() {}
};

[[nodiscard]] std::unique_ptr<GlobalMouseBackend> createGlobalMouseBackend();
[[nodiscard]] QString globalMousePermissionMessage(const GlobalMousePermissionState& state);

class GlobalMouseManager final : public QObject {
    Q_OBJECT
  public:
    explicit GlobalMouseManager(QObject* parent = nullptr);
    explicit GlobalMouseManager(std::unique_ptr<GlobalMouseBackend> backend,
                                QObject* parent = nullptr);
    ~GlobalMouseManager() override;
    void initialize();
    void shutdown();
    void setCaptureAvailable(bool available);
    void cancelGesture(quint64 id);
    void beginButtonDrag(settings::SettingsGlobalMouseAction action);
    [[nodiscard]] GlobalMousePermissionState permissionState() const;
    void refreshPermission();
    void usePermissionSnapshot(bool listen, bool accessibility);
    void requestPermission();
    void openPermissionSettings();

  signals:
    void permissionRefreshRequested();
    void dragEvent(const snow_shot::presentation::GlobalMouseDragEvent& event);
    void operationFailed(const QString& message);
    void permissionStateChanged(snow_shot::presentation::GlobalMousePermissionState state);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
#endif
