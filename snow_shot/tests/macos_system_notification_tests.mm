#include "systemnotificationservice.h"
#include "systemnotificationservice_p.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVector>
#include <functional>
#include <iostream>
#include <thread>
#include <utility>

using snow_shot::platform::macos::SystemNotificationService;
using Action = SystemNotificationService::Action;
using Result = SystemNotificationService::Result;

@interface FixtureNotificationCenter : NSObject <SnowShotNotificationCenter>
@property(nonatomic, assign) id<UNUserNotificationCenterDelegate> delegate;
@property(nonatomic, copy) void (^authorizationCompletion)(BOOL, NSError*);
@property(nonatomic, retain) NSMutableArray<UNNotificationRequest*>* requests;
@property(nonatomic, retain) NSMutableArray* deliveryCompletions;
@property(nonatomic) BOOL resolvesDeliveryImmediately;
@property(nonatomic, retain) NSError* deliveryError;
@property(nonatomic) NSUInteger authorizationRequests;
@property(nonatomic) UNAuthorizationOptions options;
- (void)resolveAuthorization:(BOOL)granted error:(NSError*)error;
- (void)resolveDelivery:(NSUInteger)index error:(NSError*)error;
@end

@implementation FixtureNotificationCenter
- (instancetype)init {
    self = [super init];
    if (self) {
        self.requests = [NSMutableArray array];
        self.deliveryCompletions = [NSMutableArray array];
        self.resolvesDeliveryImmediately = YES;
    }
    return self;
}
- (void)dealloc {
    [_authorizationCompletion release];
    [_requests release];
    [_deliveryCompletions release];
    [_deliveryError release];
    [super dealloc];
}
- (void)requestAuthorizationWithOptions:(UNAuthorizationOptions)options
                      completionHandler:(void (^)(BOOL, NSError*))completion {
    self.options = options;
    ++_authorizationRequests;
    self.authorizationCompletion = completion;
}
- (void)resolveAuthorization:(BOOL)granted error:(NSError*)error {
    void (^completion)(BOOL, NSError*) = [self.authorizationCompletion copy];
    self.authorizationCompletion = nil;
    completion(granted, error);
    [completion release];
}
- (void)addNotificationRequest:(UNNotificationRequest*)request
         withCompletionHandler:(void (^)(NSError*))completion {
    [self.requests addObject:request];
    if (self.resolvesDeliveryImmediately)
        completion(self.deliveryError);
    else
        [self.deliveryCompletions addObject:[[completion copy] autorelease]];
}
- (void)resolveDelivery:(NSUInteger)index error:(NSError*)error {
    void (^completion)(NSError*) = [self.deliveryCompletions[index] copy];
    self.deliveryCompletions[index] = NSNull.null;
    completion(error);
    [completion release];
}
@end

// Notification/response objects are normally created by macOS. These fixtures expose
// the same read-only payload without using private constructors or posting real alerts.
@interface FixtureNotification : NSObject
@property(nonatomic, retain) UNNotificationRequest* request;
@end
@implementation FixtureNotification
- (void)dealloc {
    [_request release];
    [super dealloc];
}
@end

@interface FixtureResponse : NSObject
@property(nonatomic, retain) FixtureNotification* notification;
@property(nonatomic, copy) NSString* actionIdentifier;
@end
@implementation FixtureResponse
- (void)dealloc {
    [_notification release];
    [_actionIdentifier release];
    [super dealloc];
}
@end

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void waitFor(const std::function<bool()>& predicate, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
    }
    require(predicate(), message);
}

void drainCallbacks() {
    auto done = std::make_shared<bool>(false);
    dispatch_async(dispatch_get_main_queue(), ^{
      *done = true;
    });
    waitFor([&] { return *done; }, "main queue must drain");
}

void show(SystemNotificationService& service, const QString& title, const QString& body,
          Action action = Action::None, const QString& path = {},
          SystemNotificationService::Completion completed = {}) {
    service.show(
        {title, body, snow_shot::platform::SystemNotificationSeverity::Information, action, path},
        std::move(completed));
}

FixtureNotification* notification(UNNotificationRequest* request) {
    auto* value = [[[FixtureNotification alloc] init] autorelease];
    value.request = request;
    return value;
}

void respond(FixtureNotificationCenter* center, UNNotificationRequest* request,
             NSString* action = UNNotificationDefaultActionIdentifier) {
    auto* response = [[[FixtureResponse alloc] init] autorelease];
    response.notification = notification(request);
    response.actionIdentifier = action;
    __block bool completed = false;
    [center.delegate userNotificationCenter:reinterpret_cast<UNUserNotificationCenter*>(center)
             didReceiveNotificationResponse:reinterpret_cast<UNNotificationResponse*>(response)
                      withCompletionHandler:^{
                        completed = true;
                      }];
    require(completed, "notification responses must always acknowledge completion");
    drainCallbacks();
}

void authorizedDeliveryAndActions() {
    auto* center = [[[FixtureNotificationCenter alloc] init] autorelease];
    SystemNotificationService service(center, nullptr);
    require(center.delegate != nil && center.authorizationRequests == 0,
            "construction installs a retained delegate without prompting for permission");
    const QString first = QStringLiteral("/tmp/Video exports/视频 one.mp4");
    const QString second = QStringLiteral("/tmp/Video exports/second.mov");
    show(service, QStringLiteral("Video export completed"), first, Action::OpenRecording, first);
    show(service, QStringLiteral("Video export completed"), second, Action::OpenRecording, second);
    show(service, QStringLiteral("Update"), QStringLiteral("Update ready"), Action::OpenAbout);
    require(center.authorizationRequests == 1 &&
                center.options == (UNAuthorizationOptionAlert | UNAuthorizationOptionSound) &&
                center.requests.count == 0,
            "concurrent exports must wait for alert and sound authorization before delivery");
    std::thread([center] {
        @autoreleasepool {
            [center resolveAuthorization:YES error:nil];
        }
    }).join();
    waitFor([&] { return center.requests.count == 3; },
            "permission completion on a worker must deliver every queued notification");
    UNNotificationRequest* firstRequest = center.requests[0];
    UNNotificationRequest* secondRequest = center.requests[1];
    require([firstRequest.content.title isEqualToString:@"Video export completed"] &&
                QString::fromNSString(firstRequest.content.body) == first &&
                firstRequest.trigger == nil &&
                ![firstRequest.identifier isEqualToString:secondRequest.identifier],
            "immediate notifications preserve localized copy and have independent identifiers");
    require([firstRequest.content.sound isEqual:UNNotificationSound.defaultSound] &&
                firstRequest.content.interruptionLevel == UNNotificationInterruptionLevelActive,
            "exports must request the default sound without bypassing system interruption policy");

    __block UNNotificationPresentationOptions presentation = UNNotificationPresentationOptionNone;
    [center.delegate
         userNotificationCenter:reinterpret_cast<UNUserNotificationCenter*>(center)
        willPresentNotification:reinterpret_cast<UNNotification*>(notification(firstRequest))
          withCompletionHandler:^(UNNotificationPresentationOptions options) {
            presentation = options;
          }];
    require(presentation ==
                (UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList |
                 UNNotificationPresentationOptionSound),
            "foreground exports must request a banner, Notification Center entry and sound");

    QStringList opened;
    int about = 0;
    QObject::connect(&service, &SystemNotificationService::activated, &service,
                     [&](Action action, const QString& path) {
                         require(QThread::currentThread() == service.thread(),
                                 "notification actions must run on the Qt owning thread");
                         if (action == Action::OpenRecording)
                             opened.append(path);
                         if (action == Action::OpenAbout)
                             ++about;
                     });
    respond(center, secondRequest);
    respond(center, center.requests[2]);
    respond(center, firstRequest);
    require(opened == QStringList{second, first} && about == 1,
            "clicks must use their own payload even after a newer export or update");
    respond(center, firstRequest, UNNotificationDismissActionIdentifier);
    require(opened.size() == 2 && about == 1, "dismissal must not open files or About");

    show(service, QStringLiteral("Warning"), QStringLiteral("Capture unavailable"));
    [center resolveAuthorization:YES error:nil];
    waitFor([&] { return center.requests.count == 4; },
            "shared warning notifications must deliver");
    respond(center, center.requests[3]);
    require(opened.size() == 2 && about == 1, "warnings must not inherit an older click action");
}

void deniedPermissionAndLifetime() {
    auto* center = [[[FixtureNotificationCenter alloc] init] autorelease];
    QStringList completedMessages;
    QVector<Result> results;
    const auto completion = [&](const QString& message) {
        return [&, message](Result result) {
            require(QThread::currentThread() == QCoreApplication::instance()->thread(),
                    "delivery results must run on the owning thread");
            completedMessages.append(message);
            results.append(std::move(result));
        };
    };
    {
        SystemNotificationService service(center, nullptr);
        show(service, QStringLiteral("Export"), QStringLiteral("first"), Action::None, {},
             completion(QStringLiteral("first")));
        show(service, QStringLiteral("Export"), QStringLiteral("second"), Action::None, {},
             completion(QStringLiteral("second")));
        [center resolveAuthorization:NO error:nil];
        waitFor([&] { return results.size() == 2; },
                "every notification in a denied batch must report its own delivery failure");
        require(completedMessages ==
                        QStringList{QStringLiteral("first"), QStringLiteral("second")} &&
                    results[0].status == Result::Status::Denied &&
                    results[1].status == Result::Status::Denied,
                "a denied batch preserves each original request and its authorization result");
        require(center.requests.count == 0, "denied notification batches must not be scheduled");
        show(service, QStringLiteral("Export"), QStringLiteral("third"), Action::None, {},
             completion(QStringLiteral("third")));
        [center resolveAuthorization:YES error:nil];
        waitFor([&] { return results.size() == 3; },
                "new notifications must honor permission changes without replaying denied exports");
        require(center.requests.count == 1 && results.last().accepted(),
                "accepted native delivery must report success for only the new request");

        center.deliveryError =
            [NSError errorWithDomain:@"fixture"
                                code:1
                            userInfo:@{NSLocalizedDescriptionKey : @"delivery failed"}];
        show(service, QStringLiteral("Export"), QStringLiteral("fourth"), Action::None, {},
             completion(QStringLiteral("fourth")));
        [center resolveAuthorization:YES error:nil];
        waitFor([&] { return results.size() == 4; }, "native scheduling errors must be observable");
        require(results.last().status == Result::Status::Failed &&
                    results.last().diagnostic == QStringLiteral("delivery failed"),
                "native error details must be preserved");
        show(service, QStringLiteral("Export"), QStringLiteral("fifth"), Action::None, {},
             completion(QStringLiteral("fifth")));
        [center resolveAuthorization:NO error:center.deliveryError];
        waitFor([&] { return results.size() == 5; }, "authorization errors must be observable");
        require(results.last().status == Result::Status::Failed,
                "native authorization errors must retain their distinct failure status");
        center.deliveryError = [NSError errorWithDomain:UNErrorDomain
                                                   code:UNErrorCodeNotificationsNotAllowed
                                               userInfo:nil];
        show(service, QStringLiteral("Export"), QStringLiteral("revoked"), Action::None, {},
             completion(QStringLiteral("revoked")));
        [center resolveAuthorization:YES error:nil];
        waitFor([&] { return results.size() == 6; }, "revoked permission must be observable");
        require(results.last().status == Result::Status::Denied,
                "permission revoked between authorization and scheduling is a denial");
        show(service, QStringLiteral("Export"), QStringLiteral("shutdown"), Action::None, {},
             completion(QStringLiteral("shutdown")));
    }
    require(center.delegate == nil, "shutdown must detach the notification delegate");
    [center resolveAuthorization:YES error:nil];
    drainCallbacks();
    require(
        center.requests.count == 3 && results.size() == 6,
        "late permission callbacks must not access a destroyed service or deliver stale exports");

    SystemNotificationService unavailable(nil, nullptr);
    show(unavailable, QStringLiteral("Export"), QStringLiteral("standalone"), Action::None, {},
         completion(QStringLiteral("standalone")));
    require(results.size() == 7 && results.last().status == Result::Status::Unavailable,
            "unbundled executables report unavailable delivery without invoking the native API");
}

void asynchronousDeliveryAndShutdown() {
    auto* center = [[[FixtureNotificationCenter alloc] init] autorelease];
    center.resolvesDeliveryImmediately = NO;
    QStringList finished;
    QVector<Result> results;
    const auto completion = [&](const QString& name) {
        return [&, name](Result result) {
            require(QThread::currentThread() == QCoreApplication::instance()->thread(),
                    "native scheduling completion must return to the owning thread");
            finished.append(name);
            results.append(std::move(result));
        };
    };
    {
        SystemNotificationService service(center, nullptr);
        show(service, {}, QStringLiteral("first"), Action::None, {},
             completion(QStringLiteral("first")));
        show(service, {}, QStringLiteral("second"), Action::None, {},
             completion(QStringLiteral("second")));
        [center resolveAuthorization:YES error:nil];
        waitFor([&] { return center.requests.count == 2; },
                "accepted permission must schedule concurrent requests");
        require(results.isEmpty(), "authorization alone must not report successful delivery");
        std::thread([center] {
            @autoreleasepool {
                [center resolveDelivery:1 error:nil];
            }
        }).join();
        [center resolveDelivery:0 error:[NSError errorWithDomain:@"fixture" code:2 userInfo:nil]];
        waitFor([&] { return results.size() == 2; },
                "out-of-order scheduling callbacks must finish their own requests");
        require(finished == QStringList{QStringLiteral("second"), QStringLiteral("first")} &&
                    results.first().accepted() && results.last().status == Result::Status::Failed,
                "scheduling success and error must remain attached to the correct request");
        show(service, {}, QStringLiteral("shutdown"), Action::None, {},
             completion(QStringLiteral("shutdown")));
        [center resolveAuthorization:YES error:nil];
        waitFor([&] { return center.requests.count == 3; }, "shutdown request must be pending");
    }
    [center resolveDelivery:2 error:nil];
    drainCallbacks();
    require(
        results.size() == 2 && center.delegate == nil,
        "late scheduling callbacks must neither invoke feedback nor access a destroyed service");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    @autoreleasepool {
        authorizedDeliveryAndActions();
        deniedPermissionAndLifetime();
        asynchronousDeliveryAndShutdown();
    }
    return 0;
}
