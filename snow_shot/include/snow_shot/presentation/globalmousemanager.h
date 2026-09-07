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
};

[[nodiscard]] std::unique_ptr<GlobalMouseBackend> createGlobalMouseBackend();

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

  signals:
    void dragEvent(const snow_shot::presentation::GlobalMouseDragEvent& event);
    void operationFailed(const QString& message);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
#endif
