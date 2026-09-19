#include "snow_shot/platform/windowcaptureexclusion.h"

#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QHash>
#include <QThread>

namespace snow_shot::platform {
namespace {
// Ownership is tied to the QWidget, not a recyclable native pointer. Retaining
// the original NSWindow lets teardown restore it after native handle replacement.
class SharingState final : public QObject {
  public:
    SharingState(QWidget* owner, NSWindow* window)
        : QObject(owner), m_window([window retain]), m_previous(window.sharingType) {
        m_window.sharingType = NSWindowSharingNone;
    }
    ~SharingState() override {
        m_window.sharingType = m_previous;
        [m_window release];
    }
    NSWindow* window() const {
        return m_window;
    }

  private:
    NSWindow* m_window;
    NSWindowSharingType m_previous;
};
QHash<QWidget*, SharingState*>& states() {
    static QHash<QWidget*, SharingState*> values;
    return values;
}
NSWindow* nativeWindow(QWidget* widget) {
    if (widget == nullptr || QGuiApplication::platformName() != QStringLiteral("cocoa") ||
        QThread::currentThread() != qApp->thread())
        return nil;
    auto* view = reinterpret_cast<NSView*>(widget->window()->winId());
    return view != nil ? view.window : nil;
}
} // namespace

bool setWindowExcludedFromCapture(QWidget* widget, bool excluded) {
    if (widget == nullptr || QThread::currentThread() != widget->thread())
        return false;
    widget = widget->window();
    auto& saved = states();
    if (!excluded) {
        delete saved.take(widget);
        return true;
    }
    @autoreleasepool {
        NSWindow* window = nativeWindow(widget);
        if (window == nil || window.windowNumber <= 0)
            return false;
        if (auto* existing = saved.value(widget)) {
            if (existing->window() == window)
                return true;
            delete saved.take(widget);
        }
        auto* state = new SharingState(widget, window);
        saved.insert(widget, state);
        QObject::connect(state, &QObject::destroyed, [widget, state]() {
            if (states().value(widget) == state)
                states().remove(widget);
        });
        if (window.sharingType != NSWindowSharingNone) {
            delete saved.take(widget);
            return false;
        }
        return true;
    }
}
std::optional<std::uint32_t> captureWindowId(QWidget* widget) {
    @autoreleasepool {
        NSWindow* window = nativeWindow(widget);
        if (window == nil || window.windowNumber <= 0)
            return std::nullopt;
        return static_cast<std::uint32_t>(window.windowNumber);
    }
}
} // namespace snow_shot::platform
