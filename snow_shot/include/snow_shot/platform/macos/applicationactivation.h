#ifndef SNOW_SHOT_PLATFORM_MACOS_APPLICATIONACTIVATION_H
#define SNOW_SHOT_PLATFORM_MACOS_APPLICATIONACTIVATION_H

#include <functional>
#include <memory>

class QWidget;

namespace snow_shot::platform::macos {
void activateWindow(QWidget* window);
void configureMainWindowTitleBar(QWidget* window, int titleBarHeight);

class ApplicationReopenHandler final {
  public:
    explicit ApplicationReopenHandler(std::function<void()> reopen);
    ~ApplicationReopenHandler();

    ApplicationReopenHandler(const ApplicationReopenHandler&) = delete;
    ApplicationReopenHandler& operator=(const ApplicationReopenHandler&) = delete;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::platform::macos

#endif // SNOW_SHOT_PLATFORM_MACOS_APPLICATIONACTIVATION_H
