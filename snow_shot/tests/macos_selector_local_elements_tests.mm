#include "../src/platform/macos/selectorlocalelements_p.h"

#include <QGuiApplication>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>

@interface SelectorLocalTestElement : NSObject {
  @public
    NSRect frame;
    __unsafe_unretained id parent;
    __unsafe_unretained id hit;
    NSPoint hitPoint;
    bool hidden;
    bool throwsOnFrame;
    int calls;
}
@end
@implementation SelectorLocalTestElement
- (NSRect)accessibilityFrame {
    ++calls;
    if (throwsOnFrame)
        @throw [NSException exceptionWithName:NSInternalInconsistencyException
                                       reason:@"fixture"
                                     userInfo:nil];
    return frame;
}
- (id)accessibilityParent {
    ++calls;
    return parent;
}
- (id)accessibilityHitTest:(NSPoint)point {
    ++calls;
    hitPoint = point;
    return hit;
}
- (BOOL)isAccessibilityHidden {
    ++calls;
    return hidden;
}
@end

namespace {
using namespace snow_shot::platform::detail;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
NSRect nativeFrame(const QRectF& rect) {
    return NSMakeRect(rect.x(), 1000 - rect.bottom(), rect.width(), rect.height());
}

void localPathsValidateOwnershipAndGeometry() {
    CapturedDisplayModel display;
    display.nativeDisplayId = 7;
    display.canvasUsesPoints = true;
    display.capturedLogicalRect = QRect(-100, 50, 400, 300);
    display.physicalRect = QRect(-100, 50, 800, 600);
    auto* window = [SelectorLocalTestElement new];
    auto* container = [SelectorLocalTestElement new];
    auto* duplicate = [SelectorLocalTestElement new];
    auto* child = [SelectorLocalTestElement new];
    window->frame = nativeFrame(QRectF(-120, 60, 240, 200));
    container->frame = nativeFrame(QRectF(-95, 75, 120, 90));
    child->frame = nativeFrame(QRectF(-90.25, 80.25, 50.5, 20.5));
    duplicate->frame = child->frame;
    child->parent = duplicate;
    duplicate->parent = container;
    container->parent = window;
    window->hit = child;
    const QPoint point(-60, 130);
    const QRectF fallback(-100, 70, 440, 400);
    const auto query = [&] {
        return selectorLocalElementPath(window, point, display, 1000, fallback);
    };
    const auto path = query();
    require(path ==
                QVector<QRectF>{QRectF(-81, 110, 102, 42), QRectF(-90, 100, 240, 180), fallback},
            "local path must be deepest-first, deduplicated, clipped and rounded outward at Retina "
            "scale");
    require(window->hitPoint.x == -80 && window->hitPoint.y == 910,
            "local hit must use bottom-left native screen points");

    container->parent = nil;
    require(query().isEmpty(), "unrelated or destroyed ancestry must not publish children");
    container->parent = child;
    require(query().isEmpty(), "cyclic ancestry must not publish children");
    container->parent = window;
    container->hidden = true;
    require(query().isEmpty(), "hidden ancestry must not publish children");
    container->hidden = false;
    container->throwsOnFrame = true;
    require(query().isEmpty(), "provider exceptions must retain the window fallback");
    container->throwsOnFrame = false;
    window->frame.origin.x += 10;
    require(query().isEmpty(), "moved windows must not contribute children to a frozen snapshot");
    window->frame.origin.x -= 10;
    window->frame.size.width = std::numeric_limits<double>::quiet_NaN();
    require(query().isEmpty(), "nonfinite window bounds must be rejected");
    window->frame = nativeFrame(QRectF(-120, 60, 240, 200));

    child->frame = nativeFrame(QRectF(-400, 80, 400, 20));
    require(query().front() == QRectF(-100, 110, 200, 40),
            "child rectangles must be clipped to the visible window and selected display");
    child->frame = nativeFrame(QRectF(50, 80, 20, 20));
    child->parent = container;
    require(query().front() == QRectF(-90, 100, 240, 180),
            "frames that miss the pointer must be omitted");

    const int calls = window->calls;
    std::thread worker([&] {
        require(query().isEmpty(), "worker calls must fail before accessing AppKit objects");
    });
    worker.join();
    require(window->calls == calls, "worker guard must not invoke the accessibility provider");

    auto* chain = [NSMutableArray new];
    for (int i = 0; i < 65; ++i) {
        auto* node = [SelectorLocalTestElement new];
        node->frame = child->frame;
        node->parent = chain.lastObject ? chain.lastObject : window;
        [chain addObject:node];
    }
    window->hit = chain.lastObject;
    require(query().isEmpty(),
            "overlong ancestry must terminate without publishing partial ownership");

    CapturedDisplayModel other = display;
    other.nativeDisplayId = 8;
    other.capturedLogicalRect = QRect(300, 50, 400, 300);
    other.physicalRect = QRect(300, 50, 400, 300);
    const QVector<CapturedDisplayModel> displays{display, other};
    require(selectorDisplayAtPhysicalPoint(QPoint(320, 100), 8, displays) == &displays[1],
            "display ID must disambiguate overlapping physical capture coordinates");
    require(selectorDisplayAtPhysicalPoint(point, 99, displays) == nullptr,
            "stale display IDs must not select a different display");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    @autoreleasepool {
        localPathsValidateOwnershipAndGeometry();
    }
    std::cout
        << "Local accessibility ownership, threading, geometry, and traversal checks passed\n";
}
