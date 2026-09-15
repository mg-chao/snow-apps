#ifndef SNOW_SHOT_SHORTCUTS_SHORTCUTRECORDER_H
#define SNOW_SHOT_SHORTCUTS_SHORTCUTRECORDER_H

#include <Qt>

#include <functional>
#include <memory>

class QKeyEvent;
class QWidget;

namespace snow_shot::shortcuts {

// Platform-neutral recording-session helper. Ordinary keys are captured from
// QKeyEvent by the editor; the implementation supplies only native keys that
// Qt does not reliably deliver (Print Screen on Windows).
class ShortcutRecorder final {
  public:
    using Handler = std::function<void(Qt::KeyboardModifiers)>;

    ShortcutRecorder(QWidget& target, Handler handler);
    ~ShortcutRecorder();

    bool handleKeyEvent(const QKeyEvent& event);
    void cancelPendingCapture();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::shortcuts

#endif // SNOW_SHOT_SHORTCUTS_SHORTCUTRECORDER_H
