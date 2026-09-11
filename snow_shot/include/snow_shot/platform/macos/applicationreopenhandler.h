#pragma once

#include <functional>
#include <memory>

class QObject;

namespace snow_shot::platform::macos {
// Construct before QApplication so Qt's Cocoa delegate can forward native reopen requests.
class ApplicationReopenHandler final {
  public:
    ApplicationReopenHandler();
    ~ApplicationReopenHandler();

    ApplicationReopenHandler(const ApplicationReopenHandler&) = delete;
    ApplicationReopenHandler& operator=(const ApplicationReopenHandler&) = delete;

    void setHandler(QObject* context, std::function<void()> handler);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::platform::macos
