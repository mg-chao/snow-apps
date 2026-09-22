#include "snow_shot/platform/macos/loginitemservice.h"
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <QCoreApplication>
#include <cstdlib>
#include <iostream>

namespace {
bool observedLogin = false;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace
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
    }
    std::cout << "Native login item tests passed\n";
}
