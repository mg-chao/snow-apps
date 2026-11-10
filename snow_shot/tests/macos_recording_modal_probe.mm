#include "macos_recording_modal_probe.h"

#include <QWidget>
#import <AppKit/AppKit.h>

bool macosRecordingModalAboveControls(QWidget* modal, QWidget* area, QWidget* toolbar) {
    NSWindow* surface = reinterpret_cast<NSView*>(modal->winId()).window;
    NSArray<NSNumber*>* ordered = [NSWindow windowNumbersWithOptions:0];
    const NSUInteger surfaceIndex = [ordered indexOfObject:@(surface.windowNumber)];
    if (!surface.visible || surface.alphaValue < 0.99 || surfaceIndex == NSNotFound ||
        NSApp.modalWindow != surface)
        return false;
    for (QWidget* widget : {area, toolbar}) {
        NSWindow* control = reinterpret_cast<NSView*>(widget->winId()).window;
        const NSUInteger controlIndex = [ordered indexOfObject:@(control.windowNumber)];
        if (!control.visible || controlIndex == NSNotFound || surface.level <= control.level ||
            surfaceIndex >= controlIndex)
            return false;
        if (qAbs(control.alphaValue - widget->windowOpacity()) > 0.01)
            return false;
    }
    return true;
}
