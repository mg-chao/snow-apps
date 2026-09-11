#import <AppKit/AppKit.h>

#include "snow_shot/platform/macos/applicationreopenhandler.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <utility>

namespace {
constexpr auto kDisableForegroundTransform = "QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM";

bool nativeLoginLaunch() {
    NSAppleEventDescriptor* event = NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
    return event.eventID == kAEOpenApplication &&
           [event paramDescriptorForKeyword:keyAEPropData].enumCodeValue ==
               keyAELaunchedAsLogInItem;
}

struct ReopenState : std::enable_shared_from_this<ReopenState> {
    void restoreForegroundTransform() {
        if (!foregroundTransformChanged) {
            return;
        }
        foregroundTransformChanged = false;
        if (qgetenv(kDisableForegroundTransform) == QByteArrayLiteral("1")) {
            if (foregroundTransformWasSet) {
                qputenv(kDisableForegroundTransform, previousForegroundTransform);
            } else {
                qunsetenv(kDisableForegroundTransform);
            }
        }
    }

    void queueStartup() {
        if (!enabled || !launchFinished || startupDelivered || startupQueued ||
            QCoreApplication::instance() == nullptr) {
            return;
        }
        startupQueued = true;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [weak = weak_from_this()] {
                if (const auto state = weak.lock()) {
                    state->startupQueued = false;
                    // Qt's didFinishLaunching delegate has now returned, so it can no longer
                    // steal focus from the application that was active at login.
                    state->restoreForegroundTransform();
                    if (state->enabled && state->startupContext && state->startupHandler &&
                        !state->startupDelivered) {
                        state->startupDelivered = true;
                        const auto handler = state->startupHandler;
                        handler(state->launchedAtLogin);
                    }
                }
            },
            Qt::QueuedConnection);
    }

    void finishLaunch() {
        if (!enabled || launchFinished) {
            return;
        }
        launchFinished = true;
        launchedAtLogin = loginLaunchDetector();
        const bool background = launchedAtLogin || QCoreApplication::arguments().contains(
                                                       QStringLiteral("--autostart"));
        if (background && qEnvironmentVariableIsEmpty(kDisableForegroundTransform)) {
            foregroundTransformWasSet = qEnvironmentVariableIsSet(kDisableForegroundTransform);
            previousForegroundTransform = qgetenv(kDisableForegroundTransform);
            qputenv(kDisableForegroundTransform, QByteArrayLiteral("1"));
            foregroundTransformChanged = true;
        }
        queueStartup();
    }

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
    QPointer<QObject> startupContext;
    std::function<void(bool)> startupHandler;
    std::function<bool()> loginLaunchDetector;
    QByteArray previousForegroundTransform;
    bool foregroundTransformWasSet = false;
    bool foregroundTransformChanged = false;
    bool launchFinished = false;
    bool startupQueued = false;
    bool startupDelivered = false;
    bool pending = false;
    bool enabled = true;
    bool launchedAtLogin = false;
};
} // namespace

@interface SnowShotApplicationReopenDelegate : NSObject <NSApplicationDelegate> {
  @public
    std::shared_ptr<ReopenState> state;
    id<NSApplicationDelegate> previousDelegate;
}
@end

@implementation SnowShotApplicationReopenDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    if (state) {
        state->finishLaunch();
    }
    if ([previousDelegate respondsToSelector:@selector(applicationDidFinishLaunching:)]) {
        [previousDelegate applicationDidFinishLaunching:notification];
    }
}

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
    explicit Impl(std::function<bool()> loginLaunchDetector)
        : state(std::make_shared<ReopenState>()) {
        state->loginLaunchDetector =
            loginLaunchDetector ? std::move(loginLaunchDetector) : nativeLoginLaunch;
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
        state->startupContext.clear();
        state->startupHandler = {};
        state->restoreForegroundTransform();
        delegate->state.reset();
        if (NSApplication.sharedApplication.delegate == delegate) {
            NSApplication.sharedApplication.delegate = delegate->previousDelegate;
        }
    }

    std::shared_ptr<ReopenState> state;
    SnowShotApplicationReopenDelegate* __strong delegate = nil;
};

ApplicationReopenHandler::ApplicationReopenHandler(std::function<bool()> loginLaunchDetector)
    : m_impl(std::make_unique<Impl>(std::move(loginLaunchDetector))) {}
ApplicationReopenHandler::~ApplicationReopenHandler() = default;

bool ApplicationReopenHandler::launchedAtLogin() const {
    return m_impl->state->launchedAtLogin;
}

void ApplicationReopenHandler::setStartupHandler(QObject* context,
                                                 std::function<void(bool)> handler) {
    m_impl->state->startupContext = context;
    m_impl->state->startupHandler = std::move(handler);
    m_impl->state->queueStartup();
}

void ApplicationReopenHandler::setHandler(QObject* context, std::function<void()> handler) {
    m_impl->state->context = context;
    m_impl->state->handler = std::move(handler);
    if (m_impl->state->pending) {
        m_impl->state->pending = false;
        m_impl->state->request();
    }
}
} // namespace snow_shot::platform::macos
