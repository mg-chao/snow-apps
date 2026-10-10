#ifndef SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGREGIONINPUTROUTER_H
#define SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGREGIONINPUTROUTER_H

#include <QPoint>
#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#include <qt_windows.h>

struct RecordingRegionMouseApi {
    decltype(&SetWindowsHookExW) installHook = SetWindowsHookExW;
    decltype(&UnhookWindowsHookEx) removeHook = UnhookWindowsHookEx;
    decltype(&CallNextHookEx) nextHook = CallNextHookEx;
    std::function<bool()> supported;
};
#endif

// Passive observation must never make system input wait for the UI thread.
// Handlers run on the constructing thread and are retired with the router.
class RecordingRegionInputRouter final {
  public:
    using Handler = std::function<void(const QPoint&)>;
    explicit RecordingRegionInputRouter(Handler changed);
#ifdef Q_OS_WIN
    RecordingRegionInputRouter(Handler changed, RecordingRegionMouseApi api);
#endif
    ~RecordingRegionInputRouter();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif
