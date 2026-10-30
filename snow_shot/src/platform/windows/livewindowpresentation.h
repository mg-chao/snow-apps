// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PLATFORM_WINDOWS_LIVEWINDOWPRESENTATION_H
#define SNOW_SHOT_PLATFORM_WINDOWS_LIVEWINDOWPRESENTATION_H

#include "snow_shot/presentation/livewindowsource.h"
#include <qt_windows.h>
#include <memory>

namespace snow_shot::presentation {
// Owns the DWM destination and its binding to the Qt shell. Source inspection and capture
// deliberately live elsewhere: a source refresh must never control the pin's placement.
class LiveWindowPresentationSurface final {
  public:
    explicit LiveWindowPresentationSurface(HWND source);
    ~LiveWindowPresentationSurface();
    bool present(const LiveWindowPresentation& presentation, const QRectF& crop,
                 const QRect& sourceBounds);
    bool presenting() const;
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation
#endif
