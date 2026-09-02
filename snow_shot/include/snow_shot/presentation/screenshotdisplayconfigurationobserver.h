#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTDISPLAYCONFIGURATIONOBSERVER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTDISPLAYCONFIGURATIONOBSERVER_H

#include <QList>
#include <QObject>
#include <QSet>
#include <Qt>

#include <functional>

class QGuiApplication;
class QRect;
class QScreen;

class ScreenshotDisplayConfigurationObserver final : public QObject {
  public:
    using ChangeHandler = std::function<void()>;

    explicit ScreenshotDisplayConfigurationObserver(ChangeHandler onChanged,
                                                    QObject* parent = nullptr);

    void connectApplicationSignals(QGuiApplication* application);
    void observeCurrentScreens();
    void observeScreens(const QList<QScreen*>& screens);
    void observeScreen(QScreen* screen);


    void handleScreenAdded(QScreen* screen);
    void handleScreenRemoved(QScreen* screen);
    void handleApplicationStateChanged(Qt::ApplicationState state);
    void handleObservedScreenChanged();

  private:
    void notifyChanged();

    ChangeHandler m_onChanged;
    QSet<QScreen*> m_observedScreens;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTDISPLAYCONFIGURATIONOBSERVER_H
