// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/platform/macos/screenshotsessionfocus.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <QApplication>
#include <QPointer>
#include <QThread>
#include <QWidget>
#include <type_traits>

namespace snow_shot::platform::macos {
namespace {
constexpr float kAccessibilityTimeoutSeconds = 0.05F;

struct ReleaseElement {
    void operator()(AXUIElementRef element) const {
        CFRelease(element);
    }
};
using Element = std::unique_ptr<std::remove_pointer_t<AXUIElementRef>, ReleaseElement>;

Element copyElement(AXUIElementRef owner, CFStringRef attribute) {
    if (!owner)
        return {};
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(owner, attribute, &value) != kAXErrorSuccess || !value)
        return {};
    if (CFGetTypeID(value) != AXUIElementGetTypeID()) {
        CFRelease(value);
        return {};
    }
    Element result(static_cast<AXUIElementRef>(value));
    AXUIElementSetMessagingTimeout(result.get(), kAccessibilityTimeoutSeconds);
    return result;
}

void setIfSupported(AXUIElementRef element, CFStringRef attribute, CFTypeRef value) {
    if (!element)
        return;
    Boolean settable = false;
    if (AXUIElementIsAttributeSettable(element, attribute, &settable) == kAXErrorSuccess &&
        settable)
        static_cast<void>(AXUIElementSetAttributeValue(element, attribute, value));
}

class NativeFocusTarget final : public ScreenshotFocusTarget {
  public:
    NativeFocusTarget(NSRunningApplication* application, std::function<bool()> accessibilityTrusted)
        : m_application([application retain]),
          m_accessibilityTrusted(std::move(accessibilityTrusted)) {
        if (application.processIdentifier == NSProcessInfo.processInfo.processIdentifier) {
            m_localWindow = [NSApp.keyWindow retain];
            m_localFocus = QApplication::focusWidget();
            return;
        }
        if (!m_accessibilityTrusted())
            return;
        m_axApplication.reset(AXUIElementCreateApplication(application.processIdentifier));
        AXUIElementSetMessagingTimeout(m_axApplication.get(), kAccessibilityTimeoutSeconds);
        m_window = copyElement(m_axApplication.get(), kAXFocusedWindowAttribute);
        m_input = copyElement(m_axApplication.get(), kAXFocusedUIElementAttribute);
        if (m_input && m_window) {
            const auto window = copyElement(m_input.get(), kAXWindowAttribute);
            if (!window || !CFEqual(window.get(), m_window.get()))
                m_input.reset();
        } else {
            m_input.reset();
        }
    }
    ~NativeFocusTarget() override {
        [m_localWindow release];
        [m_application release];
    }

    bool canRestore() const override {
        if (m_application.terminated)
            return false;
        const auto foreground = NSWorkspace.sharedWorkspace.frontmostApplication;
        return foreground &&
               (foreground.processIdentifier == m_application.processIdentifier ||
                foreground.processIdentifier == NSProcessInfo.processInfo.processIdentifier);
    }

    void restore() override {
        if (!canRestore())
            return;
        if (m_application.processIdentifier == NSProcessInfo.processInfo.processIdentifier) {
            // Retaining an NSWindow does not mean its Qt owner is still alive or visible.
            if (!m_localWindow.visible || ![NSApp.windows containsObject:m_localWindow])
                return;
            [NSApp activate];
            [m_localWindow makeKeyAndOrderFront:nil];
            if (m_localFocus && m_localFocus->isVisible() &&
                reinterpret_cast<NSView*>(m_localFocus->window()->internalWinId()).window ==
                    m_localWindow)
                m_localFocus->setFocus(Qt::OtherFocusReason);
            return;
        }

        // Raising a retained AX identity fails safely if the original window closed.
        const bool raised =
            m_accessibilityTrusted() && m_window &&
            AXUIElementPerformAction(m_window.get(), kAXRaiseAction) == kAXErrorSuccess;
        if (raised) {
            setIfSupported(m_window.get(), kAXMainAttribute, kCFBooleanTrue);
            setIfSupported(m_axApplication.get(), kAXFocusedWindowAttribute, m_window.get());
        }
        [NSApp yieldActivationToApplication:m_application];
        if (![m_application activateWithOptions:0])
            return;
        if (raised && m_input) {
            const auto window = copyElement(m_input.get(), kAXWindowAttribute);
            if (window && CFEqual(window.get(), m_window.get()))
                setIfSupported(m_input.get(), kAXFocusedAttribute, kCFBooleanTrue);
        }
    }

  private:
    NSRunningApplication* m_application = nil;
    NSWindow* m_localWindow = nil;
    QPointer<QWidget> m_localFocus;
    std::function<bool()> m_accessibilityTrusted;
    Element m_axApplication;
    Element m_window;
    Element m_input;
};
} // namespace

std::unique_ptr<ScreenshotSessionFocus>
createScreenshotSessionFocus(std::function<bool()> accessibilityTrusted) {
    if (!accessibilityTrusted)
        accessibilityTrusted = [] { return AXIsProcessTrusted(); };
    return std::make_unique<ScreenshotSessionFocus>(
        [accessibilityTrusted =
             std::move(accessibilityTrusted)]() -> std::unique_ptr<ScreenshotFocusTarget> {
            if (!qApp || QThread::currentThread() != qApp->thread() ||
                QGuiApplication::platformName() != QStringLiteral("cocoa"))
                return {};
            NSRunningApplication* application = NSWorkspace.sharedWorkspace.frontmostApplication;
            if (!application || application.terminated)
                return {};
            return std::make_unique<NativeFocusTarget>(application, accessibilityTrusted);
        });
}
} // namespace snow_shot::platform::macos
