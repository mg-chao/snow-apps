#pragma once

#include <functional>
#include <memory>

class QObject;

namespace snow_shot::platform::macos {
// Construct before QApplication so Qt's Cocoa delegate can forward native reopen requests.
class ApplicationReopenHandler final {
  public:
    explicit ApplicationReopenHandler(std::function<bool()> loginLaunchDetector = {});
    ~ApplicationReopenHandler();

    ApplicationReopenHandler(const ApplicationReopenHandler&) = delete;
    ApplicationReopenHandler& operator=(const ApplicationReopenHandler&) = delete;

    void setHandler(QObject* context, std::function<void()> handler);
    // AppKit delivers the login launch event after QApplication construction. The initial
    // window decision must wait for this callback; it is queued and delivered at most once.
    void setStartupHandler(QObject* context, std::function<void(bool)> handler);
    [[nodiscard]] bool launchedAtLogin() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::platform::macos
