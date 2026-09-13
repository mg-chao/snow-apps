#pragma once

#include <utility>

namespace snow_shot::app {
// Background services can discover missing access before any user-facing window exists.
// Retain that request until a window is actually shown, including a queued show request
// that is cancelled by closing the window before the next event-loop turn.
class DeferredPermissionPrompt {
  public:
    void request() {
        m_pending = true;
    }

    bool takeIfWindowVisible(bool visible) {
        return visible && std::exchange(m_pending, false);
    }

  private:
    bool m_pending = false;
};
} // namespace snow_shot::app
