#ifndef SNOW_SHOT_PRESENTATION_SELECTEDTEXTTRANSLATIONCOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_SELECTEDTEXTTRANSLATIONCOORDINATOR_H

#include "snow_shot/presentation/selectedtexttranslationcontroller.h"

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class QScreen;
class SnowShotApiClient;
namespace snow_shot::storage {
class ConfigurationStore;
}
namespace snow_shot::presentation {
class StandaloneTranslationWindow;

class SelectedTextTranslationCoordinator final : public QObject {
    Q_OBJECT
  public:
    using ScreenProvider = std::function<QScreen*()>;
    SelectedTextTranslationCoordinator(storage::ConfigurationStore& configuration,
                                       SnowShotApiClient* client, QObject* parent = nullptr,
                                       std::unique_ptr<SelectedTextCaptureBackend> backend = {},
                                       ScreenProvider screenProvider = {});
    ~SelectedTextTranslationCoordinator() override;
    void capture();
    void shutdown();

  signals:
    void mainTranslationRequested(const QString& text);
    void operationFailed(const QString& message);

  private:
    storage::ConfigurationStore& m_configuration;
    SelectedTextTranslationController* m_capture;
    StandaloneTranslationWindow* m_window;
    ScreenProvider m_screenProvider;
    QPointer<QScreen> m_screen;
    bool m_standalone = false;
    bool m_shutdown = false;
};
} // namespace snow_shot::presentation
#endif
