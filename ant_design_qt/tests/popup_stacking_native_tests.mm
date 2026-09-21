#include "widgets/detail/top_level_popup_window.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/color_picker.h"
#include "widgets/tooltip.h"
#include <QPushButton>
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QAbstractEventDispatcher>
#include <QWidget>
#include <QWindow>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
class Tool : public QWidget {
  public:
    Tool() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        setAttribute(Qt::WA_MacAlwaysShowToolWindow);
        setGeometry(200, 200, 100, 100);
    }
    void releaseSurface() {
        hide();
        destroy();
    }
};
NSWindow* native(QWidget& widget) {
    return reinterpret_cast<NSView*>(widget.winId()).window;
}
void settle() {
    QEventLoop loop;
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
                     &loop, &QEventLoop::quit, Qt::QueuedConnection);
    // Animated widgets may keep the dispatcher busy; still bound this native event-loop turn.
    QTimer::singleShot(100, &loop, &QEventLoop::quit);
    loop.exec();
}
bool cocoa() {
    return QGuiApplication::platformName() == QStringLiteral("cocoa");
}
void above(QWidget& popup, QWidget& owner) {
    settle();
    require(popup.windowHandle()->transientParent() == owner.windowHandle(),
            "the popup must retain its Qt transient owner");
    if (!cocoa())
        return;
    require(popup.isVisible() && owner.isVisible(), "stacking requires visible windows");
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    NSUInteger popupIndex = NSNotFound;
    NSUInteger ownerIndex = NSNotFound;
    for (NSDictionary* info in reinterpret_cast<NSArray*>(list)) {
        const NSInteger number = [info[reinterpret_cast<NSString*>(kCGWindowNumber)] integerValue];
        const NSUInteger index = [reinterpret_cast<NSArray*>(list) indexOfObject:info];
        if (number == native(popup).windowNumber)
            popupIndex = index;
        if (number == native(owner).windowNumber)
            ownerIndex = index;
    }
    CFRelease(list);
    require(popupIndex != NSNotFound && ownerIndex != NSNotFound,
            "both windows must exist in WindowServer order");
    require(popupIndex < ownerIndex,
            "a popup must remain above its owner after the owner is raised");
}
void popupOwnership() {
    Tool owner;
    Tool otherOwner;
    Tool popup;
    Tool nested;
    owner.show();
    otherOwner.show();
    for (int recreation = 0; recreation < 2; ++recreation) {
        adqt::widgets::detail::syncTopLevelToolTransientParent(&popup, &owner);
        popup.show();
        adqt::widgets::detail::syncTopLevelToolTransientParent(&nested, &popup);
        nested.show();
        owner.raise();
        popup.raise();
        QCoreApplication::processEvents();
        above(popup, owner);
        above(nested, popup);
        nested.hide();
        popup.hide();
        if (cocoa())
            require(native(popup).parentWindow == nil && native(nested).parentWindow == nil,
                    "hidden popups must release native ownership");
        popup.show();
        adqt::widgets::detail::syncTopLevelToolTransientParent(&popup, &otherOwner);
        otherOwner.raise();
        QCoreApplication::processEvents();
        above(popup, otherOwner);
        popup.windowHandle()->setTransientParent(nullptr);
        if (cocoa())
            require(native(popup).parentWindow == nil,
                    "detaching Qt ownership must detach Cocoa ownership");
        nested.releaseSurface();
        popup.releaseSurface();
    }
}
QWidget* visibleSurface(const char* name) {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->isVisible() && widget->objectName() == QString::fromLatin1(name))
            return widget;
    }
    std::cerr << "Missing " << name << "\n";
    require(false, "the actual component must show its popup surface");
    return nullptr;
}
void componentsKeepOwnership() {
    using namespace adqt::widgets;
    Tool owner;
    owner.resize(480, 320);
    QPushButton trigger(QStringLiteral("Tools"), &owner);
    trigger.setGeometry(100, 100, 100, 30);
    AdPopover popover(&trigger);
    popover.setSourceWidget(&trigger);
    popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    auto* content = new QPushButton(QStringLiteral("Rectangle"));
    popover.setContentWidget(content);
    owner.show();
    settle();
    for (int reuse = 0; reuse < 2; ++reuse) {
        popover.show();
        settle();
        QWidget* surface = content->window();
        owner.raise();
        above(*surface, owner);
        popover.hide();
        settle();
    }
    AdSelect select(&owner);
    select.setGeometry(100, 150, 160, 32);
    select.setOptions({{QStringLiteral("one"), QStringLiteral("One")}});
    select.setPopupLayerMode(AdSelect::PopupLayerMode::QtTool);
    select.show();
    select.showPopup();
    settle();
    QWidget* menu = visibleSurface("adselect-popup");
    owner.raise();
    above(*menu, owner);
    select.hidePopup();
    settle();
    AdColorPicker picker(&owner);
    picker.setGeometry(240, 100, 100, 30);
    picker.setPopupLayerMode(AdColorPicker::PopupLayerMode::QtTool);
    picker.show();
    picker.setPopupVisible(true);
    settle();
    QWidget* colorSurface = visibleSurface("adpopover-surface");
    owner.raise();
    above(*colorSurface, owner);
    picker.setPopupVisible(false);
    settle();
    AdTooltip tooltip(&trigger);
    tooltip.setTargetWidget(&trigger);
    tooltip.setText(QStringLiteral("Tool options"));
    tooltip.setLayerMode(AdTooltip::LayerMode::TopLevelTransient);
    tooltip.show();
    settle();
    QWidget* tipSurface = visibleSurface("adtooltip-surface");
    if (cocoa())
        require(native(*tipSurface).parentWindow == native(owner),
                "tooltips must share native ownership with other popups");
    owner.raise();
    above(*tipSurface, owner);
    tooltip.hide();
}

} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (cocoa())
        [NSApp activate];
    settle();
    @autoreleasepool {
        popupOwnership();
        componentsKeepOwnership();
    }
    std::cout << "Popup native stacking tests passed\n";
}
