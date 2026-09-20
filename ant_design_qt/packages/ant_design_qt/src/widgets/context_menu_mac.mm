#include "context_menu_mac_p.h"

#include "context_menu.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QWindow>
#include <qpa/qplatformmenu.h>
#include <QtMath>

#import <AppKit/AppKit.h>
#import <objc/message.h>
#import <objc/runtime.h>

namespace {
char imagePolicyKey;
void prepareNativeImages(NSMenu* menu);
void prepareNativeImage(NSMenuItem* item);
} // namespace

// The menu owns its observer; the observer does not retain the menu or replace Qt's delegate.
@interface ADQTMenuImageObserver : NSObject
- (void)menuChanged:(NSNotification*)notification;
@end

@implementation ADQTMenuImageObserver
- (void)menuChanged:(NSNotification*)notification {
    NSMenu* menu = notification.object;
    // AppKit supplies the changed item's index for both add and change notifications.
    // Updating one item must not rescan every icon and submenu in the menu tree.
    NSNumber* index = notification.userInfo[@"NSMenuItemIndex"];
    if (index && index.integerValue >= 0 && index.integerValue < menu.numberOfItems) {
        prepareNativeImage([menu itemAtIndex:index.integerValue]);
    }
}
- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [super dealloc];
}
@end

namespace {
void prepareNativeImages(NSMenu* menu) {
    if (!menu || objc_getAssociatedObject(menu, &imagePolicyKey)) {
        return;
    }
    auto* observer = [[ADQTMenuImageObserver alloc] init];
    objc_setAssociatedObject(menu, &imagePolicyKey, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    for (NSNotificationName name :
         {NSMenuDidAddItemNotification, NSMenuDidChangeItemNotification}) {
        [NSNotificationCenter.defaultCenter addObserver:observer
                                               selector:@selector(menuChanged:)
                                                   name:name
                                                 object:menu];
    }
    [observer release];
    for (NSMenuItem* item in menu.itemArray) {
        prepareNativeImage(item);
    }
}

void prepareNativeImage(NSMenuItem* item) {
    // macOS 27 hides images by default. Runtime lookup also supports binaries built
    // with older SDKs. Visible is the public NSMenuItemImageVisibility value 1.
    static const SEL getter = NSSelectorFromString(@"preferredImageVisibility");
    static const SEL setter = NSSelectorFromString(@"setPreferredImageVisibility:");
    if ([item respondsToSelector:setter]) {
        constexpr NSInteger visible = 1;
        const auto getVisibility = reinterpret_cast<NSInteger (*)(id, SEL)>(objc_msgSend);
        const auto setVisibility = reinterpret_cast<void (*)(id, SEL, NSInteger)>(objc_msgSend);
        if (getVisibility(item, getter) != visible) {
            setVisibility(item, setter, visible);
        }
    }
    prepareNativeImages(item.submenu);
}
} // namespace

namespace adqt::widgets::detail {
namespace {

QColor resolvedNativeColor(NSColor* color, NSAppearance* appearance) {
    __block CGFloat red = 0.0;
    __block CGFloat green = 0.0;
    __block CGFloat blue = 0.0;
    __block CGFloat alpha = 0.0;
    [appearance performAsCurrentDrawingAppearance:^{
      NSColor* rgb = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
      if (rgb) {
          [rgb getRed:&red green:&green blue:&blue alpha:&alpha];
      }
    }];
    return QColor::fromRgbF(static_cast<float>(red), static_cast<float>(green),
                            static_cast<float>(blue), static_cast<float>(alpha));
}

struct NativeAction {
    QPointer<QPlatformMenuItem> item;
    QPointer<QAction> action;
};

QList<NativeAction> menuActions(QMenu* menu) {
    QList<NativeAction> result;
    QSet<QMenu*> visited;
    QList<QMenu*> remaining{menu};
    while (!remaining.isEmpty()) {
        QMenu* current = remaining.takeLast();
        if (visited.contains(current)) {
            continue;
        }
        visited.insert(current);
        for (QAction* action : current->actions()) {
            auto* platform = current->platformMenu();
            result.append(
                {platform ? platform->menuItemForTag(reinterpret_cast<quintptr>(action)) : nullptr,
                 action});
            if (action->menu()) {
                remaining.append(action->menu());
            }
        }
    }
    return result;
}

} // namespace

bool usesNativeContextMenu() {
    return QGuiApplication::platformName() == QStringLiteral("cocoa");
}

QIcon nativeContextMenuIcon(QMenu* menu, const adqt::icons::IconRef& icon) {
    const auto* descriptor = icon.descriptor();
    const bool monochromeTemplate =
        icon.colors().isEmpty() && descriptor->defaultColors.isEmpty() &&
        descriptor->colorModel == adqt::icons::IconColorModel::Monochrome;
    if (monochromeTemplate) {
        QIcon result = adqt::icons::makeIcon(icon);
        result.setIsMask(true);
        return result;
    }

    adqt::icons::IconStatePalette statePalette;
    if (descriptor->colorModel != adqt::icons::IconColorModel::FullColor &&
        !icon.colors().primarySlot() && descriptor->defaultColors.primary.empty()) {
        // The custom menu painter normally binds an unassigned primary slot to its text color.
        // Native Cocoa menus bypass that painter, so preserve the same contract in the QIcon.
        QColor normal;
        QColor selected;
        QColor disabled;
        if (usesNativeContextMenu()) {
            NSMenu* native = menu->toNSMenu();
            NSAppearance* appearance = native.effectiveAppearance;
            if (!appearance) {
                appearance = NSApp.effectiveAppearance;
            }
            normal = resolvedNativeColor(NSColor.controlTextColor, appearance);
            selected = resolvedNativeColor(NSColor.selectedMenuItemTextColor, appearance);
            disabled = resolvedNativeColor(NSColor.disabledControlTextColor, appearance);
        } else {
            const QPalette palette = menu->palette();
            normal = palette.color(QPalette::Active, QPalette::Text);
            selected = palette.color(QPalette::Active, QPalette::HighlightedText);
            disabled = palette.color(QPalette::Disabled, QPalette::Text);
        }
        const auto colorsFor = [&icon](const QColor& primary) {
            return icon.colors().withPrimary(primary);
        };
        statePalette.set(QIcon::Normal, QIcon::Off, colorsFor(normal));
        statePalette.set(QIcon::Active, QIcon::Off, colorsFor(selected));
        statePalette.set(QIcon::Selected, QIcon::Off, colorsFor(selected));
        statePalette.set(QIcon::Disabled, QIcon::Off, colorsFor(disabled));
    }
    return adqt::icons::makeIcon(icon, statePalette);
}

void initializeNativeContextMenu(QMenu* menu) {
    if (usesNativeContextMenu()) {
        @autoreleasepool {
            prepareNativeImages(menu->toNSMenu());
        }
    }
}

QSize nativeContextMenuSize(QMenu* menu) {
    @autoreleasepool {
        NSMenu* native = menu->toNSMenu();
        if (native) {
            const NSSize size = native.size;
            return QSize(qCeil(size.width), qCeil(size.height));
        }
        return menu->QMenu::sizeHint();
    }
}

void dismissNativeContextMenu(QMenu* menu) {
    @autoreleasepool {
        [menu->toNSMenu() cancelTracking];
    }
}

QAction* execNativeContextMenu(AdContextMenu* menu, const QPoint& globalPosition,
                               QAction* initialAction) {
    @autoreleasepool {
        // Keep AppKit's menu alive if a signal handler destroys its Qt owner while tracking.
        NSMenu* native = [[menu->toNSMenu() retain] autorelease];
        if (!native) {
            return menu->QMenu::exec(globalPosition, initialAction);
        }
        QPointer<AdContextMenu> guard(menu);
        QPointer<QAction> selected;
        QObject observer;
        QObject::connect(menu, &QMenu::triggered, &observer,
                         [&selected](QAction* action) { selected = action; });

        QWidget* owner = menu->triggerWidget() ? menu->triggerWidget() : menu->parentWidget();
        while (qobject_cast<QMenu*>(owner)) {
            owner = owner->parentWidget();
        }
        if (owner) {
            owner->window()->winId();
            QWindow* window = owner->window()->windowHandle();
            QPlatformMenu* platform = menu->platformMenu();
            // Qt's Cocoa presenter associates the modal owner and clears its mouse-button
            // state after AppKit swallows the release. Keep those platform invariants in Qt.
            const QPoint local = window->mapFromGlobal(globalPosition);
            platform->showPopup(
                window, QRect(local, QSize(0, 0)),
                platform->menuItemForTag(reinterpret_cast<quintptr>(initialAction)));
        } else {
            // A tray menu may have no QWidget owner. AppKit screen coordinates are logical
            // points with an inverted Y axis relative to the primary display, not pixels.
            // Using that display's frame also handles monitors above/left of the primary.
            const NSRect primaryFrame = NSScreen.screens.firstObject.frame;
            const NSPoint point =
                NSMakePoint(globalPosition.x(), NSMaxY(primaryFrame) - globalPosition.y());
            const qsizetype index = menu->actions().indexOf(initialAction);
            NSMenuItem* initialItem =
                index >= 0 && index < native.numberOfItems ? [native itemAtIndex:index] : nil;
            [native popUpMenuPositioningItem:initialItem atLocation:point inView:nil];
        }

        // Cocoa queues QPlatformMenuItem::activated, which Qt then queues to QAction.
        // Drain both hops before returning from execAt; otherwise a stack-owned menu
        // can be destroyed before its selected action is ever triggered. Include items
        // added dynamically by a submenu's aboutToShow handler.
        // Guard each action: triggering one can delete the menu or any of its siblings.
        if (guard) {
            const auto actions = menuActions(guard);
            for (const auto& entry : actions) {
                if (entry.item) {
                    QCoreApplication::sendPostedEvents(entry.item, QEvent::MetaCall);
                }
                if (entry.action) {
                    QCoreApplication::sendPostedEvents(entry.action, QEvent::MetaCall);
                }
            }
        }
        return selected;
    }
}

} // namespace adqt::widgets::detail
