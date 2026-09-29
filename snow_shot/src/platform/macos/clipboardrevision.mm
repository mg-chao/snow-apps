#include "snow_shot/presentation/screenshotclipboardplacement.h"
#import <AppKit/AppKit.h>

quint64 screenshotClipboardRevision() {
    return static_cast<quint64>(NSPasteboard.generalPasteboard.changeCount);
}
