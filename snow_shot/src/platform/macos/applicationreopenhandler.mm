#import <AppKit/AppKit.h>

#include "snow_shot/platform/macos/applicationreopenhandler.h"

#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <utility>

namespace {
struct ReopenState : std::enable_shared_from_this<ReopenState> {
    void request() {
        if (!enabled) {
            return;
        }
        if (!handler) {
            pending = true;
            return;
        }
        if (context.isNull()) {
            return;
        }
        // Deliver after AppKit finishes handling the Apple event. The QObject context and
        // weak state both cancel delivery when the controller or native bridge is destroyed.
        QMetaObject::invokeMethod(
            context,
            [weak = weak_from_this()] {
                if (const auto state = weak.lock();
                    state && state->enabled && state->context && state->handler) {
                    state->handler();
                }
            },
            Qt::QueuedConnection);
    }

    QPointer<QObject> context;
    std::function<void()> handler;
    bool pending = false;
    bool enabled = true;
};
} // namespace

@interface SnowShotApplicationReopenDelegate : NSObject <NSApplicationDelegate> {
  @public
    std::shared_ptr<ReopenState> state;
    id<NSApplicationDelegate> previousDelegate;
}
@end

@implementation SnowShotApplicationReopenDelegate
- (BOOL)applicationShouldHandleReopen:(NSApplication*)application hasVisibleWindows:(BOOL)visible {
    (void)application;
    (void)visible;
    if (state) {
        state->request();
    }
    return YES;
}

- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector] || [previousDelegate respondsToSelector:selector];
}

- (id)forwardingTargetForSelector:(SEL)selector {
    if ([previousDelegate respondsToSelector:selector]) {
        return previousDelegate;
    }
    return [super forwardingTargetForSelector:selector];
}
@end

namespace snow_shot::platform::macos {
struct ApplicationReopenHandler::Impl {
    Impl() : state(std::make_shared<ReopenState>()) {
        @autoreleasepool {
            NSApplication* application = NSApplication.sharedApplication;
            delegate = [[SnowShotApplicationReopenDelegate alloc] init];
            delegate->state = state;
            delegate->previousDelegate = application.delegate;
            application.delegate = delegate;
        }
    }

    ~Impl() {
        state->enabled = false;
        state->handler = {};
        state->context.clear();
        delegate->state.reset();
        if (NSApplication.sharedApplication.delegate == delegate) {
            NSApplication.sharedApplication.delegate = delegate->previousDelegate;
        }
    }

    std::shared_ptr<ReopenState> state;
    SnowShotApplicationReopenDelegate* __strong delegate = nil;
};

ApplicationReopenHandler::ApplicationReopenHandler() : m_impl(std::make_unique<Impl>()) {}
ApplicationReopenHandler::~ApplicationReopenHandler() = default;

void ApplicationReopenHandler::setHandler(QObject* context, std::function<void()> handler) {
    m_impl->state->context = context;
    m_impl->state->handler = std::move(handler);
    if (m_impl->state->pending) {
        m_impl->state->pending = false;
        m_impl->state->request();
    }
}
} // namespace snow_shot::platform::macos
