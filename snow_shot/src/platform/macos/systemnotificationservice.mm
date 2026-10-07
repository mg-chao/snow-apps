#include "systemnotificationservice.h"
#include "systemnotificationservice_p.h"

#include <QDebug>
#include <QPointer>
#include <QUuid>
#include <QVector>
#include <utility>

using snow_shot::platform::macos::SystemNotificationService;

namespace {
NSString* const actionKey = @"snow-shot-action";
NSString* const filePathKey = @"snow-shot-file-path";
NSString* const identifierPrefix = @"snow-shot-";

id<SnowShotNotificationCenter> nativeCenter() {
    // UserNotifications requires an application bundle; currentNotificationCenter aborts
    // for standalone command-line executables. Capability is independent of tray visibility.
    NSBundle* bundle = NSBundle.mainBundle;
    if (bundle.bundleIdentifier.length == 0 ||
        ![bundle.bundleURL.pathExtension isEqualToString:@"app"])
        return nil;
    return reinterpret_cast<id<SnowShotNotificationCenter>>(
        UNUserNotificationCenter.currentNotificationCenter);
}
} // namespace

@interface SnowShotNotificationDelegate : NSObject <UNUserNotificationCenterDelegate> {
  @public
    QPointer<SystemNotificationService> service;
}
@end

@implementation SnowShotNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completion {
    Q_UNUSED(center);
    const bool owned = [notification.request.identifier hasPrefix:identifierPrefix];
    completion(owned
                   ? UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList |
                         UNNotificationPresentationOptionSound
                   : UNNotificationPresentationOptionNone);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completion {
    Q_UNUSED(center);
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier] &&
        [response.notification.request.identifier hasPrefix:identifierPrefix]) {
        NSDictionary* info = response.notification.request.content.userInfo;
        const auto action =
            static_cast<SystemNotificationService::Action>([info[actionKey] intValue]);
        NSString* nativePath = info[filePathKey];
        const QString path = [nativePath isKindOfClass:NSString.class]
                                 ? QString::fromNSString(nativePath)
                                 : QString();
        const QPointer<SystemNotificationService> guard = service;
        dispatch_async(dispatch_get_main_queue(), ^{
          if (guard &&
              (action == SystemNotificationService::Action::OpenAbout ||
               (action == SystemNotificationService::Action::OpenRecording && !path.isEmpty())))
              emit guard->activated(action, path);
        });
    }
    completion();
}
@end

namespace snow_shot::platform::macos {

class SystemNotificationService::Impl {
  public:
    struct Message {
        SystemNotificationRequest request;
        Completion completed;
    };

    Impl(SystemNotificationService& owner, id<SnowShotNotificationCenter> native)
        : q(owner), center([native retain]), delegate([[SnowShotNotificationDelegate alloc] init]) {
        delegate->service = &q;
        center.delegate = delegate;
    }
    ~Impl() {
        if (center.delegate == delegate)
            center.delegate = nil;
        [delegate release];
        [center release];
    }

    static void complete(const Completion& completed, Result result) {
        if (!result.accepted())
            qWarning().noquote() << "macOS notification:" << result.diagnostic;
        if (completed)
            completed(std::move(result));
    }

    static Result errorResult(NSError* error) {
        if (error == nil)
            return {};
        const auto status = [error.domain isEqualToString:UNErrorDomain] &&
                                    error.code == UNErrorCodeNotificationsNotAllowed
                                ? Result::Status::Denied
                                : Result::Status::Failed;
        return {status, QString::fromNSString(error.localizedDescription)};
    }

    void show(Message message) {
        if (center == nil) {
            complete(message.completed, {Result::Status::Unavailable,
                                         QStringLiteral("An application bundle is required.")});
            return;
        }
        pending.append(std::move(message));
        if (authorizing)
            return;
        authorizing = true;
        const QPointer<SystemNotificationService> guard(&q);
        // Query authorization for each batch, so changes in System Settings take effect.
        // Concurrent exports wait for the same permission result without losing their paths.
        [center
            requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                          completionHandler:^(BOOL granted, NSError* error) {
                            Result result = errorResult(error);
                            if (error == nil && !granted) {
                                result = {Result::Status::Denied,
                                          QStringLiteral("Notification authorization denied.")};
                            }
                            dispatch_async(dispatch_get_main_queue(), ^{
                              if (guard)
                                  guard->m_impl->authorized(result);
                            });
                          }];
    }

    void authorized(const Result& result) {
        authorizing = false;
        const auto messages = std::exchange(pending, {});
        const QPointer<SystemNotificationService> guard(&q);
        if (!result.accepted()) {
            for (const auto& message : messages) {
                if (!guard)
                    return;
                complete(message.completed, result);
            }
            return;
        }
        for (const auto& message : messages) {
            @autoreleasepool {
                auto* content = [[UNMutableNotificationContent alloc] init];
                content.title = message.request.title.toNSString();
                content.body = message.request.body.toNSString();
                content.sound = UNNotificationSound.defaultSound;
                content.userInfo = @{
                    actionKey : @(static_cast<int>(message.request.action)),
                    filePathKey : message.request.filePath.toNSString()
                };
                NSString* identifier =
                    [identifierPrefix stringByAppendingString:QUuid::createUuid()
                                                                  .toString(QUuid::WithoutBraces)
                                                                  .toNSString()];
                auto* request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                     content:content
                                                                     trigger:nil];
                [content release];
                const auto completed = message.completed;
                [center addNotificationRequest:request
                         withCompletionHandler:^(NSError* nativeError) {
                           const Result delivery = errorResult(nativeError);
                           dispatch_async(dispatch_get_main_queue(), ^{
                             if (guard)
                                 complete(completed, delivery);
                           });
                         }];
            }
        }
    }

    SystemNotificationService& q;
    id<SnowShotNotificationCenter> center;
    SnowShotNotificationDelegate* delegate;
    QVector<Message> pending;
    bool authorizing = false;
};

SystemNotificationService::SystemNotificationService(QObject* parent)
    : SystemNotificationService(nativeCenter(), parent) {}

SystemNotificationService::SystemNotificationService(id<SnowShotNotificationCenter> center,
                                                     QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, center)) {
    setObjectName(QStringLiteral("systemNotificationService"));
}

SystemNotificationService::~SystemNotificationService() = default;

void SystemNotificationService::show(SystemNotificationRequest request, Completion completed) {
    m_impl->show({std::move(request), std::move(completed)});
}

} // namespace snow_shot::platform::macos
