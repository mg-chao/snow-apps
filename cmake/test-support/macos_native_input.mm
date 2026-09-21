#include "macos_native_input.h"

#include <QPoint>
#include <QWidget>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QThread>

#import <AppKit/AppKit.h>

bool macWindowReceivesPoint(QWidget* widget, const QPoint& globalPoint) {
    auto* view = reinterpret_cast<NSView*>(widget->winId());
    const NSPoint point =
        NSMakePoint(globalPoint.x(), NSMaxY(NSScreen.screens.firstObject.frame) - globalPoint.y());
    return [NSWindow windowNumberAtPoint:point
               belowWindowWithWindowNumber:0] == view.window.windowNumber;
}

bool macWindowHasShadow(QWidget* widget) {
    auto* view = reinterpret_cast<NSView*>(widget->winId());
    return view.window.hasShadow;
}

void macActivateApplication() {
    [NSApp activate];
}

void macPostClick(const QPoint& globalPoint) {
    for (CGEventType type : {kCGEventMouseMoved, kCGEventLeftMouseDown, kCGEventLeftMouseUp}) {
        CGEventRef event = CGEventCreateMouseEvent(
            nullptr, type, CGPointMake(globalPoint.x(), globalPoint.y()), kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 50) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
    }
}

void macRaiseTestWindow(QWidget* widget, int level) {
    auto* view = reinterpret_cast<NSView*>(widget->winId());
    view.window.level = level;
    [view.window orderFrontRegardless];
}

void macPostMove(const QPoint& globalPoint) {
    CGEventRef event =
        CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
                                CGPointMake(globalPoint.x(), globalPoint.y()), kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

bool macCanPostMouseEvents() {
    return CGPreflightPostEventAccess();
}

MacCursorRestore::MacCursorRestore() {
    CGEventRef event = CGEventCreate(nullptr);
    const CGPoint position = CGEventGetLocation(event);
    m_position = QPointF(position.x, position.y);
    CFRelease(event);
}

MacCursorRestore::~MacCursorRestore() {
    CGWarpMouseCursorPosition(CGPointMake(m_position.x(), m_position.y()));
}

namespace {
void postDragEvent(CGEventType type, const QPoint& position) {
    CGEventRef event = CGEventCreateMouseEvent(
        nullptr, type, CGPointMake(position.x(), position.y()), kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 50) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}
} // namespace

MacMouseDrag::MacMouseDrag(const QPoint& start) : m_position(start) {
    postDragEvent(kCGEventMouseMoved, start);
    postDragEvent(kCGEventLeftMouseDown, start);
}

MacMouseDrag::~MacMouseDrag() {
    finish();
}

void MacMouseDrag::moveTo(const QPoint& point) {
    m_position = point;
    postDragEvent(kCGEventLeftMouseDragged, point);
}

void MacMouseDrag::finish() {
    if (m_pressed) {
        m_pressed = false;
        postDragEvent(kCGEventLeftMouseUp, m_position);
    }
}
