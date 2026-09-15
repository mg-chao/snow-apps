#include "snow_shot/shortcuts/shortcutrecorder.h"

#ifdef Q_OS_WIN
#include "snow_shot/platform/windows/printscreenshortcutrecorder.h"
#endif

#include <QKeyEvent>

#include <utility>

namespace snow_shot::shortcuts {

class ShortcutRecorder::Impl {
  public:
    Impl(QWidget& target, Handler handler)
#ifdef Q_OS_WIN
        : native(target, std::move(handler))
#endif
    {
#ifndef Q_OS_WIN
        Q_UNUSED(target);
        Q_UNUSED(handler);
#endif
    }

#ifdef Q_OS_WIN
    platform::windows::PrintScreenShortcutRecorder native;
#endif
};

ShortcutRecorder::ShortcutRecorder(QWidget& target, Handler handler)
    : m_impl(std::make_unique<Impl>(target, std::move(handler))) {}

ShortcutRecorder::~ShortcutRecorder() = default;

bool ShortcutRecorder::handleKeyEvent(const QKeyEvent& event) {
#ifdef Q_OS_WIN
    return m_impl->native.handleKeyEvent(event);
#else
    Q_UNUSED(event);
    return false;
#endif
}

void ShortcutRecorder::cancelPendingCapture() {
#ifdef Q_OS_WIN
    m_impl->native.cancelPendingCapture();
#endif
}

} // namespace snow_shot::shortcuts
