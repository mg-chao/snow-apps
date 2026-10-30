#ifndef SNOW_SHOT_APP_APPLICATIONCONTROLLER_H
#define SNOW_SHOT_APP_APPLICATIONCONTROLLER_H

#include <QObject>
#include <QStringList>

#include <memory>
#include <functional>

class QApplication;

namespace snow_shot::app {
class ApplicationController final : public QObject {
    Q_OBJECT

  public:
    explicit ApplicationController(QApplication& application, QObject* parent = nullptr);
    ~ApplicationController() override;

    // Explicit file launches present before background startup and maintenance.
    void start(const QStringList& initialArguments = {}, std::function<void()> maintenance = {});
    void showMainWindow();
    void handleLaunchRequest(const QStringList& arguments,
                             std::function<void()> firstFileFinished = {});
    void restorePinnedWindows();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_APPLICATIONCONTROLLER_H
