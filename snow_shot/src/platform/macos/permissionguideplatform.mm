#include "snow_shot/presentation/permissionguidecontroller.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QApplication>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QScreen>
#include <QWidget>

namespace snow_shot::presentation {
namespace {
QVector<PermissionGuideWindow> settingsWindows(pid_t processId, quint32 windowId) {
    QVector<PermissionGuideWindow> result;
    CFArrayRef windows = CGWindowListCopyWindowInfo(windowId ? kCGWindowListOptionIncludingWindow
                                                             : kCGWindowListExcludeDesktopElements,
                                                    windowId);
    if (!windows)
        return result;
    for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index) {
        const auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, index));
        const auto number = [info](CFStringRef key, CFNumberType type, void* value) {
            const auto entry = static_cast<CFNumberRef>(CFDictionaryGetValue(info, key));
            return entry && CFGetTypeID(entry) == CFNumberGetTypeID() &&
                   CFNumberGetValue(entry, type, value);
        };
        int owner = 0;
        if (!number(kCGWindowOwnerPID, kCFNumberIntType, &owner) || owner != processId)
            continue;
        CGRect bounds;
        const auto rectangle =
            static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
        if (!rectangle || !CGRectMakeWithDictionaryRepresentation(rectangle, &bounds) ||
            CGRectIsEmpty(bounds) || CGRectIsNull(bounds))
            continue;
        PermissionGuideWindow window;
        int identifier = 0;
        double alpha = 0;
        if (!number(kCGWindowNumber, kCFNumberIntType, &identifier) ||
            !number(kCGWindowLayer, kCFNumberIntType, &window.layer) ||
            !number(kCGWindowAlpha, kCFNumberDoubleType, &alpha))
            continue;
        window.id = static_cast<quint32>(identifier);
        window.alpha = alpha;
        window.bounds =
            QRectF(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height);
        window.onScreen = CFDictionaryGetValue(info, kCGWindowIsOnscreen) == kCFBooleanTrue;
        result.append(window);
    }
    CFRelease(windows);
    return result;
}
class MacPermissionGuidePlatform final : public PermissionGuidePlatform {
  public:
    ~MacPermissionGuidePlatform() override {
        stop();
    }
    PermissionGuideApplication application() override {
        NSBundle* bundle = NSBundle.mainBundle;
        NSString* name = [bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"];
        if (!name)
            name = [bundle objectForInfoDictionaryKey:@"CFBundleName"];
        const QString path = QString::fromNSString(bundle.bundlePath);
        PermissionGuideApplication result;
        result.name = name ? QString::fromNSString(name) : QApplication::applicationDisplayName();
        if (path.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive)) {
            result.bundleUrl = QUrl::fromLocalFile(path);
            result.icon = QFileIconProvider().icon(QFileInfo(path));
        } else {
            result.icon = QApplication::windowIcon();
        }
        return result;
    }
    PermissionGuideEnvironment environment() override {
        PermissionGuideEnvironment result;
        for (QScreen* screen : QGuiApplication::screens())
            result.availableScreens.append(screen->availableGeometry());
        if (!m_settings || m_settings.terminated) {
            [m_settings release];
            m_settings =
                [[NSRunningApplication
                     runningApplicationsWithBundleIdentifier:@"com.apple.systempreferences"]
                        .firstObject retain];
            m_settingsWindowId = 0;
        }
        if (!m_settings)
            return result;
        result.settingsRunning = true;
        result.settingsActive =
            NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier ==
            m_settings.processIdentifier;
        result.guideActive = NSApp.active && m_window && m_window.visible &&
                             (NSApp.keyWindow == m_window || NSApp.mainWindow == m_window);
        if (!result.settingsActive && !result.guideActive)
            return result;
        result.windows = settingsWindows(m_settings.processIdentifier, m_settingsWindowId);
        auto selected = selectPermissionGuideWindow(result.windows, m_settingsWindowId);
        if (!selected && m_settingsWindowId) {
            result.windows = settingsWindows(m_settings.processIdentifier, 0);
            selected = selectPermissionGuideWindow(result.windows, m_settingsWindowId);
        }
        m_settingsWindowId = selected ? selected->id : 0;
        return result;
    }
    void start(std::function<void()> changed) override {
        stop();
        m_callback = std::make_shared<std::function<void()>>(std::move(changed));
        const auto callback = m_callback;
        m_workspaceObservers = [[NSMutableArray alloc] init];
        auto* center = NSWorkspace.sharedWorkspace.notificationCenter;
        for (NSNotificationName name in @[
                 NSWorkspaceDidActivateApplicationNotification,
                 NSWorkspaceDidDeactivateApplicationNotification,
                 NSWorkspaceDidTerminateApplicationNotification,
                 NSWorkspaceDidLaunchApplicationNotification,
                 NSWorkspaceActiveSpaceDidChangeNotification,
                 NSWorkspaceDidUnhideApplicationNotification, NSWorkspaceDidWakeNotification
             ]) {
            id observer = [center addObserverForName:name
                                              object:nil
                                               queue:NSOperationQueue.mainQueue
                                          usingBlock:^(NSNotification*) {
                                            if (*callback)
                                                (*callback)();
                                          }];
            [m_workspaceObservers addObject:observer];
        }
        m_displayObserver = [[NSNotificationCenter.defaultCenter
            addObserverForName:NSApplicationDidChangeScreenParametersNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification*) {
                      if (*callback)
                          (*callback)();
                    }] retain];
    }
    void stop() override {
        if (m_callback)
            *m_callback = {};
        m_callback.reset();
        for (id token in m_workspaceObservers)
            [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:token];
        [m_workspaceObservers release];
        m_workspaceObservers = nil;
        if (m_displayObserver) {
            [NSNotificationCenter.defaultCenter removeObserver:m_displayObserver];
            [m_displayObserver release];
            m_displayObserver = nil;
        }
        m_window = nil;
        [m_settings release];
        m_settings = nil;
        m_settingsWindowId = 0;
    }
    void prepareWindow(QWidget* widget) override {
        NSView* view = reinterpret_cast<NSView*>(widget->winId());
        m_window = view.window;
        m_window.hidesOnDeactivate = NO;
        m_window.hasShadow = YES;
        m_window.level = NSFloatingWindowLevel;
        m_window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                      NSWindowCollectionBehaviorFullScreenAuxiliary;
        if ([m_window isKindOfClass:NSPanel.class]) {
            m_window.styleMask |= NSWindowStyleMaskNonactivatingPanel;
            static_cast<NSPanel*>(m_window).becomesKeyOnlyIfNeeded = YES;
        }
    }

  private:
    std::shared_ptr<std::function<void()>> m_callback;
    NSMutableArray* m_workspaceObservers = nil;
    id m_displayObserver = nil;
    NSWindow* m_window = nil;
    NSRunningApplication* m_settings = nil;
    quint32 m_settingsWindowId = 0;
};
} // namespace
std::unique_ptr<PermissionGuidePlatform> createPermissionGuidePlatform() {
    return std::make_unique<MacPermissionGuidePlatform>();
}
} // namespace snow_shot::presentation
