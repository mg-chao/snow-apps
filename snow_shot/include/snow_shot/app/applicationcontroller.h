#ifndef SNOW_SHOT_APP_APPLICATIONCONTROLLER_H
#define SNOW_SHOT_APP_APPLICATIONCONTROLLER_H

#include <QObject>
#include <QStringList>

#include <memory>

class QApplication;

namespace snow_shot::app {
class ApplicationController final : public QObject {
    Q_OBJECT

  public:
    explicit ApplicationController(QApplication& application, QObject* parent = nullptr);
    ~ApplicationController() override;

    void start();
    void showMainWindow();
    void handleLaunchRequest(const QStringList& arguments);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_APPLICATIONCONTROLLER_H
