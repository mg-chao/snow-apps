#include "windowshortcutinput_p.h"

#import <AppKit/AppKit.h>

#include <QApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QWidget>

#include <utility>

namespace snow_shot::platform::macos {
namespace {

QWidget* receiverForNativeWindow(quintptr nativeWindow) {
    QWidget* receiver = QApplication::focusWidget();
    if (receiver == nullptr || nativeWindow == 0)
        return nullptr;
    NSView* view = reinterpret_cast<NSView*>(receiver->window()->winId());
    return reinterpret_cast<quintptr>(view.window) == nativeWindow ? receiver : nullptr;
}

class WindowShortcutInputFilter final : public QObject, public QAbstractNativeEventFilter {
  public:
    WindowShortcutInputFilter(std::function<bool(QWidget*, QKeyEvent&)> dispatch,
                              std::function<QWidget*(quintptr)> receiverForWindow)
        : m_dispatch(std::move(dispatch)),
          m_receiverForWindow(receiverForWindow ? std::move(receiverForWindow)
                                                : receiverForNativeWindow) {
        if (auto* application = QCoreApplication::instance())
            application->installNativeEventFilter(this);
    }

    ~WindowShortcutInputFilter() override {
        if (auto* application = QCoreApplication::instance())
            application->removeNativeEventFilter(this);
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr*) override {
        if (eventType != QByteArrayLiteral("mac_generic_NSEvent") || message == nullptr)
            return false;
        NSEvent* native = static_cast<NSEvent*>(message);
        const bool release = native.type == NSEventTypeKeyUp;
        if ((native.type != NSEventTypeKeyDown && !release) || native.keyCode != 48)
            return false;

        // Cocoa handles Control+Tab as a key equivalent. Qt sends only a
        // ShortcutOverride for it, which cannot dispatch our configured bindings,
        // and AppKit then consumes the keyDown. Route this native input through
        // the same manager as ordinary key presses before AppKit handles it.
        if (!release && !(native.modifierFlags & NSEventModifierFlagControl))
            return false;
        QWidget* receiver = release
                                ? m_pressReceiver.data()
                                : m_receiverForWindow(reinterpret_cast<quintptr>(native.window));
        if (receiver == nullptr)
            return false;

        Qt::KeyboardModifiers modifiers;
        if (native.modifierFlags & NSEventModifierFlagControl)
            modifiers |= Qt::MetaModifier;
        if (native.modifierFlags & NSEventModifierFlagCommand)
            modifiers |= Qt::ControlModifier;
        if (native.modifierFlags & NSEventModifierFlagOption)
            modifiers |= Qt::AltModifier;
        if (native.modifierFlags & NSEventModifierFlagShift)
            modifiers |= Qt::ShiftModifier;
        QKeyEvent event(release ? QEvent::KeyRelease : QEvent::KeyPress,
                        modifiers.testFlag(Qt::ShiftModifier) ? Qt::Key_Backtab : Qt::Key_Tab,
                        modifiers, 0, native.keyCode, static_cast<quint32>(native.modifierFlags),
                        QString::fromNSString(native.characters), native.ARepeat);
        event.setTimestamp(static_cast<ulong>(native.timestamp * 1000));
        event.ignore();

        const QPointer<WindowShortcutInputFilter> guard(this);
        const QPointer<QWidget> receiverGuard(receiver);
        const auto dispatch = m_dispatch;
        if (release)
            m_pressReceiver.clear();
        const bool handled = dispatch(receiver, event);
        if (!guard)
            return handled;
        if (!release && handled)
            m_pressReceiver = receiverGuard;
        // Retain ownership through repeats and release even for non-repeating
        // bindings, or if the shortcut was disabled while its key was held.
        return release || handled || (native.ARepeat && m_pressReceiver != nullptr);
    }

  private:
    std::function<bool(QWidget*, QKeyEvent&)> m_dispatch;
    std::function<QWidget*(quintptr)> m_receiverForWindow;
    QPointer<QWidget> m_pressReceiver;
};

} // namespace

std::unique_ptr<QAbstractNativeEventFilter>
makeWindowShortcutInputFilter(std::function<bool(QWidget*, QKeyEvent&)> dispatch,
                              std::function<QWidget*(quintptr)> receiverForWindow) {
    return std::make_unique<WindowShortcutInputFilter>(std::move(dispatch),
                                                       std::move(receiverForWindow));
}

} // namespace snow_shot::platform::macos
