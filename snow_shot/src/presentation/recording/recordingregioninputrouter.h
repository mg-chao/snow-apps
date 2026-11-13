#ifndef SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGREGIONINPUTROUTER_H
#define SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGREGIONINPUTROUTER_H

#include <QPoint>
#include <functional>
#include <memory>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>

struct RecordingRegionMouseApi {
    decltype(&SetWindowsHookExW) installHook = SetWindowsHookExW;
    decltype(&UnhookWindowsHookEx) removeHook = UnhookWindowsHookEx;
    decltype(&CallNextHookEx) nextHook = CallNextHookEx;
    std::function<bool()> supported;
    decltype(&GetPhysicalCursorPos) cursorPosition = GetPhysicalCursorPos;
};
#endif

// Passive observation must never make system input wait for the UI thread.
// Handlers run on the constructing thread and are retired with the router.
class RecordingRegionInputRouter final {
  public:
    enum class Events { All, Movement };
    using Handler = std::function<void(const QPoint&)>;
    explicit RecordingRegionInputRouter(Handler changed, Events events = Events::All);
#ifdef Q_OS_WIN
    RecordingRegionInputRouter(Handler changed, RecordingRegionMouseApi api,
                               Events events = Events::All);
#endif
    ~RecordingRegionInputRouter();

    // Physical desktop pixels on Windows, desktop points on macOS.
    [[nodiscard]] static std::optional<QPoint> cursorPosition();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif
