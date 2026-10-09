#include "snow_shot/platform/macos/loginitemservice.h"
#include "snow_shot/app/edition.h"
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <ServiceManagement/ServiceManagement.h>
#import <objc/runtime.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
bool observedLogin = false;
id bundleFixture = nil;
id serviceFixture = nil;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
id fixtureMainBundle(id object, SEL selector) {
    Q_UNUSED(object);
    Q_UNUSED(selector);
    return bundleFixture;
}
id fixtureMainAppService(id object, SEL selector) {
    Q_UNUSED(object);
    Q_UNUSED(selector);
    return serviceFixture;
}
class ClassMethodOverride {
  public:
    ClassMethodOverride(Class type, SEL selector, IMP implementation)
        : m_method(class_getClassMethod(type, selector)) {
        require(m_method != nullptr, "native class method must exist");
        m_original = method_setImplementation(m_method, implementation);
    }
    ~ClassMethodOverride() {
        method_setImplementation(m_method, m_original);
    }

  private:
    Method m_method;
    IMP m_original;
};
} // namespace
@interface SnowLoginItemServiceFixture : NSObject
@property(nonatomic) SMAppServiceStatus nativeStatus;
@property(nonatomic) int statusReads;
@property(nonatomic) int registrations;
@property(nonatomic) int unregistrations;
@property(nonatomic, retain) NSError* failure;
@property(nonatomic, readonly) SMAppServiceStatus status;
- (BOOL)registerAndReturnError:(NSError**)error;
- (BOOL)unregisterAndReturnError:(NSError**)error;
@end
@implementation SnowLoginItemServiceFixture
- (SMAppServiceStatus)status {
    ++_statusReads;
    return _nativeStatus;
}
- (BOOL)registerAndReturnError:(NSError**)error {
    ++_registrations;
    *error = _failure;
    if (_failure != nil)
        return NO;
    _nativeStatus = SMAppServiceStatusEnabled;
    return YES;
}
- (BOOL)unregisterAndReturnError:(NSError**)error {
    ++_unregistrations;
    *error = _failure;
    if (_failure != nil)
        return NO;
    _nativeStatus = SMAppServiceStatusNotRegistered;
    return YES;
}
- (void)dealloc {
    [_failure release];
    [super dealloc];
}
@end
@interface SnowLoginEventFixture : NSObject
- (void)handle:(NSAppleEventDescriptor*)event reply:(NSAppleEventDescriptor*)reply;
@end
@implementation SnowLoginEventFixture
- (void)handle:(NSAppleEventDescriptor*)event reply:(NSAppleEventDescriptor*)reply {
    Q_UNUSED(event);
    Q_UNUSED(reply);
    observedLogin = snow_shot::platform::macos::isNativeLoginItemLaunch();
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSApplicationDidFinishLaunchingNotification
                      object:nil];
}
@end

namespace {
void nativeStatusAndRegistration() {
    using namespace snow_shot::platform::macos;
    const QString applications = QDir::homePath() + QStringLiteral("/Applications");
    const bool createdApplications = !QDir(applications).exists();
    require(QDir().mkpath(applications), "create writable Applications fixture directory");
    {
        QTemporaryDir directory(applications + QStringLiteral("/LoginItemTest-XXXXXX.app"));
        require(directory.isValid(), "create isolated unsigned installed bundle");
        const QString contents = directory.filePath(QStringLiteral("Contents"));
        require(QDir().mkpath(contents + QStringLiteral("/MacOS")), "create fixture bundle");
        NSDictionary* info = @{
            @"CFBundleIdentifier" : snow_shot::app::edition::bundleId().toNSString(),
            @"CFBundleExecutable" : @"snow_shot",
            @"CFBundlePackageType" : @"APPL"
        };
        require([info writeToFile:(contents + QStringLiteral("/Info.plist")).toNSString()
                       atomically:YES],
                "write native bundle metadata");
        QFile executable(contents + QStringLiteral("/MacOS/snow_shot"));
        require(executable.open(QIODevice::WriteOnly) && executable.write("unsigned fixture") > 0,
                "write unsigned fixture executable");
        executable.close();
        bundleFixture = [NSBundle bundleWithPath:directory.path().toNSString()];
        require(bundleFixture != nil, "load unsigned fixture bundle");
        SnowLoginItemServiceFixture* native = [[SnowLoginItemServiceFixture alloc] init];
        serviceFixture = native;
        {
            ClassMethodOverride bundleOverride(NSBundle.class, @selector(mainBundle),
                                               reinterpret_cast<IMP>(fixtureMainBundle));
            ClassMethodOverride serviceOverride(SMAppService.class, @selector(mainAppService),
                                                reinterpret_cast<IMP>(fixtureMainAppService));
            // Exercise the real native backend while isolating preference/marker writes and
            // intercepting ServiceManagement so no real login item is registered.
            bool initialized = false;
            bool preference = false;
            auto operations = nativeLoginItemOperations();
            operations.initialized = [&] { return initialized; };
            operations.markInitialized = [&] {
                initialized = true;
                return true;
            };
            operations.savePreference = [&](bool enabled) {
                preference = enabled;
                return true;
            };
            LoginItemService service(std::move(operations));
            require(service.available() &&
                        service.snapshot().status == LoginItemStatus::Unregistered &&
                        native.statusReads == 1,
                    "routine query must read macOS status without validating an unsigned bundle");
            for (const auto status : {SMAppServiceStatusEnabled, SMAppServiceStatusRequiresApproval,
                                      SMAppServiceStatusNotRegistered}) {
                const int before = native.statusReads;
                native.nativeStatus = status;
                service.refresh();
                const auto expected = status == SMAppServiceStatusEnabled ? LoginItemStatus::Enabled
                                      : status == SMAppServiceStatusRequiresApproval
                                          ? LoginItemStatus::ApprovalRequired
                                          : LoginItemStatus::Unregistered;
                require(service.available() && service.snapshot().status == expected &&
                            native.statusReads == before + 1 && native.registrations == 0 &&
                            native.unregistrations == 0,
                        "refresh observes changing native status without mutating registration");
            }
            native.nativeStatus = SMAppServiceStatusNotFound;
            service.refresh();
            require(!service.available() && !service.hint().isEmpty(),
                    "missing native service remains unavailable with guidance");
            native.nativeStatus = SMAppServiceStatusNotRegistered;
            native.failure = [NSError errorWithDomain:SMAppServiceErrorDomain
                                                 code:kSMErrorInvalidSignature
                                             userInfo:nil];
            const auto signatureFailure = service.initialize(true);
            require(!signatureFailure.success &&
                        signatureFailure.error.contains(QStringLiteral("valid code signature")) &&
                        initialized && native.registrations == 1 && !preference &&
                        !service.pending() &&
                        service.snapshot().status == LoginItemStatus::Unregistered,
                    "registration signature failure is explained without falsely enabling login");
            service.refresh();
            require(service.available() && service.initialize(true).success &&
                        native.registrations == 1,
                    "status refresh neither prevalidates signing nor retries failed registration");
            native.failure =
                [NSError errorWithDomain:NSPOSIXErrorDomain
                                    code:kSMErrorInvalidSignature
                                userInfo:@{NSLocalizedDescriptionKey : @"native failure"}];
            const auto otherFailure = service.setEnabled(true);
            require(!otherFailure.success &&
                        otherFailure.error == u"Could not change launch at login: native failure",
                    "same numeric code in another domain preserves the native error");
            native.failure = nil;
            require(service.setEnabled(true).success && service.snapshot().requested() &&
                        preference,
                    "explicit retry can register after signing is repaired");
            require(service.setEnabled(false).success && !service.snapshot().requested() &&
                        !preference && native.unregistrations == 1,
                    "unregistration continues to reflect native status");
        }
        serviceFixture = nil;
        bundleFixture = nil;
        [native release];
    }
    if (createdApplications)
        require(QDir().rmdir(applications), "remove empty Applications fixture directory");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace snow_shot::platform::macos;
    @autoreleasepool {
        NSAppleEventManager* manager = NSAppleEventManager.sharedAppleEventManager;
        SnowLoginEventFixture* fixture = [[SnowLoginEventFixture alloc] init];
        [manager setEventHandler:fixture
                     andSelector:@selector(handle:reply:)
                   forEventClass:kCoreEventClass
                      andEventID:kAEOpenApplication];
        for (const bool login : {false, true, false}) {
            observeNativeLoginItemLaunch();
            NSAppleEventDescriptor* event =
                [NSAppleEventDescriptor appleEventWithEventClass:kCoreEventClass
                                                         eventID:kAEOpenApplication
                                                targetDescriptor:nil
                                                        returnID:kAutoGenerateReturnID
                                                   transactionID:kAnyTransactionID];
            if (login)
                [event setParamDescriptor:[NSAppleEventDescriptor
                                              descriptorWithEnumCode:keyAELaunchedAsLogInItem]
                               forKeyword:keyAEPropData];
            AppleEvent reply = {typeNull, nullptr};
            require([manager dispatchRawAppleEvent:event.aeDesc
                                      withRawReply:&reply
                                     handlerRefCon:reinterpret_cast<SRefCon>(fixture)] == noErr,
                    "synthetic native launch event dispatches");
            AEDisposeDesc(&reply);
            require(observedLogin == login,
                    "native Apple event reason distinguishes login and manual launch");
            require(initialNativeLoginItemLaunch() == login,
                    "initial launch reason survives event completion");
            require(!isNativeLoginItemLaunch(),
                    "cached initial reason must not suppress later manual reopening");
        }
        [manager removeEventHandlerForEventClass:kCoreEventClass andEventID:kAEOpenApplication];
        [fixture release];
        const auto& service = loginItemService();
        require(!service.available(), "test executable outside Applications cannot register");
        require(!service.hint().isEmpty(), "ineligible native bundle has actionable guidance");
        nativeStatusAndRegistration();
    }
    std::cout << "Native login item tests passed\n";
}
