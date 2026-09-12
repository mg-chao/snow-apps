#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_STANDALONETRANSLATIONWINDOW_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_STANDALONETRANSLATIONWINDOW_H

#include <QObject>
#include <QPointer>

class QScreen;
class SnowShotApiClient;
class TranslationPageWidget;
namespace adqt::widgets {
class AdModal;
}

namespace snow_shot::presentation {
class StandaloneTranslationWindow final : public QObject {
    Q_OBJECT
  public:
    explicit StandaloneTranslationWindow(SnowShotApiClient* client, QObject* parent = nullptr);
    ~StandaloneTranslationWindow() override;
    void showTranslation(const QString& text, QScreen* screen);
    void close();

  private:
    SnowShotApiClient* m_client;
    adqt::widgets::AdModal* m_modal;
    QPointer<TranslationPageWidget> m_page;
};
} // namespace snow_shot::presentation
#endif
