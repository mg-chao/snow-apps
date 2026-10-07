#ifndef SNOW_SHOT_PLATFORM_MACOS_SYSTEMNOTIFICATIONSERVICE_P_H
#define SNOW_SHOT_PLATFORM_MACOS_SYSTEMNOTIFICATIONSERVICE_P_H

#import <UserNotifications/UserNotifications.h>

// The native center and deterministic fixtures share this small API surface.
@protocol SnowShotNotificationCenter <NSObject>
@property(nonatomic, assign) id<UNUserNotificationCenterDelegate> delegate;
- (void)requestAuthorizationWithOptions:(UNAuthorizationOptions)options
                      completionHandler:(void (^)(BOOL granted, NSError* error))completion;
- (void)addNotificationRequest:(UNNotificationRequest*)request
         withCompletionHandler:(void (^)(NSError* error))completion;
@end

#endif
