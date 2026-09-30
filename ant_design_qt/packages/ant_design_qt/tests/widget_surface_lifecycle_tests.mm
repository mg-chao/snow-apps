#include "widgets/platform_compatibility.h"

#include <QApplication>
#include <QPlatformSurfaceEvent>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>

#include <cstdlib>
#include <iostream>

static int destroyedCursors = 0;
@interface AdQtCursorLifetimeProbe : NSCursor
@end
@implementation AdQtCursorLifetimeProbe
- (void)dealloc {
    ++destroyedCursors;
    [super dealloc];
}
@end

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class Surface final : public QWidget {
  public:
    void retire() {
        destroy(true, true);
    }
};

void cleanupDoesNotCreateNativeSurfaces() {
    QWindow window;
    QPlatformSurfaceEvent event(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
    QCoreApplication::sendEvent(&window, &event);
    require(window.handle() == nullptr, "cursor cleanup must not create a native window");
}

void retiredViewsReleaseTheirCursor() {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    Surface surface;
    for (int cycle = 0; cycle < 24; ++cycle) {
        auto* view = reinterpret_cast<NSView*>(surface.winId());
        require([view respondsToSelector:@selector(setCursor:)],
                "the native fixture must expose Qt's retained cursor property");
        auto* image = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
        auto* cursor = [[AdQtCursorLifetimeProbe alloc] initWithImage:image hotSpot:NSZeroPoint];
        [image release];
        [view setValue:cursor forKey:@"cursor"];
        [cursor release];
        require(destroyedCursors == cycle, "the view must own its cursor until retirement");
        surface.retire();
        require(destroyedCursors == cycle + 1, "native surface retirement must release its cursor");
        require(surface.internalWinId() == 0, "cleanup must leave the widget retired");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    adqt::widgets::initializePlatformCompatibility(app);
    adqt::widgets::initializePlatformCompatibility(app);
    cleanupDoesNotCreateNativeSurfaces();
    retiredViewsReleaseTheirCursor();
    return 0;
}
