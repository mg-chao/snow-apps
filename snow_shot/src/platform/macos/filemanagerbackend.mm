#include "../filemanagerbackend_p.h"

#import <AppKit/AppKit.h>

namespace snow_shot::platform::detail {

bool isSystemFileManager(const QString& directory) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:directory.toNSString() isDirectory:YES];
        NSURL* application = [NSWorkspace.sharedWorkspace URLForApplicationToOpenURL:url];
        NSBundle* bundle = application ? [NSBundle bundleWithURL:application] : nil;
        return [bundle.bundleIdentifier isEqualToString:@"com.apple.finder"];
    }
}

bool selectInSystemFileManager(const QString& path) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:path.toNSString() isDirectory:NO];
        if (!url)
            return false;
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ url ]];
        return true;
    }
}

} // namespace snow_shot::platform::detail
