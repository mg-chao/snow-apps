#ifndef SNOW_SHOT_PLATFORM_MACOS_APPLICATIONACTIVATION_H
#define SNOW_SHOT_PLATFORM_MACOS_APPLICATIONACTIVATION_H

#include <functional>
#include <memory>

class QWidget;

namespace snow_shot::platform::macos {
void activateWindow(QWidget* window);
void configureMainWindowTitleBar(QWidget* window, int titleBarHeight);

// Captures the external application that was frontmost before a global
// screenshot action activated Snow Shot, and restores it after the action
// finishes while Snow Shot is still frontmost.
class PreviousApplicationActivation final {
  public:
    PreviousApplicationActivation();
    ~PreviousApplicationActivation();

    PreviousApplicationActivation(const PreviousApplicationActivation&) = delete;
    PreviousApplicationActivation& operator=(const PreviousApplicationActivation&) = delete;
    PreviousApplicationActivation(PreviousApplicationActivation&&) noexcept;
    PreviousApplicationActivation& operator=(PreviousApplicationActivation&&) noexcept;

    [[nodiscard]] bool valid() const;
    void restore();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

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
