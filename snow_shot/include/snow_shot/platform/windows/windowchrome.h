#ifndef SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCHROME_H
#define SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCHROME_H

class QWidget;
class QObject;

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <QtTypes>
#include <optional>
#include <functional>
#include <memory>

namespace snow_shot::platform::windows {
void setupDwmShadow(QWidget* window);
void bringWindowToForeground(QWidget* window);
[[nodiscard]] bool supportsWindowCaptureExclusion();
[[nodiscard]] bool setWindowExcludedFromCapture(QWidget* window, bool excluded);
// Requires an existing layered HWND. Returns the previous state, or nullopt on failure.
[[nodiscard]] std::optional<bool> setWindowInputTransparent(QWidget* window, bool transparent);
// Request native mouse routing at the unchanged position; cursor selection is asynchronous.
[[nodiscard]] bool refreshCursorUnderPointer();
// Arm before input surfaces become transparent/hidden, then request a native mouse update.
// Complete on the desktop cursor update (1 s deadline), without sampling. Windows may
// publish the event from DWM rather than from the underlying application's thread.
// Destruction cancels the operation. The callback is suppressed if its context is destroyed.
class CursorRefresh final {
  public:
    explicit CursorRefresh(QObject* context);
    ~CursorRefresh();
    void refresh(std::function<void(bool)> completed);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] bool isNativeWindowVisible(QWidget* window);
[[nodiscard]] bool flushWindowComposition();
bool handleNativeWindowEvent(QWidget* titleBar, void* message, qintptr* result);
namespace detail {
[[nodiscard]] bool isNativeCaptionControlHit(qintptr hitTestResult);
}
} // namespace snow_shot::platform::windows
#endif

#endif // SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCHROME_H
