// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/livewindowsource.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QThread>
#include <stdexcept>

int macLivePinnedWindowTests();

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename Predicate> void waitFor(Predicate predicate, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    require(predicate(), message);
}
NSWindow* native(QWidget& widget) {
    return reinterpret_cast<NSView*>(widget.winId()).window;
}
NSWindow* contentHost(QWidget& widget) {
    for (NSWindow* child in native(widget).childWindows)
        if ([NSStringFromClass(child.class) isEqualToString:@"SnowLiveWindowHost"])
            return child;
    return nil;
}
QRect desktopBounds(NSWindow* window) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow,
                                                    CGWindowID(window.windowNumber));
    if (!windows)
        return {};
    const auto release = qScopeGuard([windows] { CFRelease(windows); });
    if (!CFArrayGetCount(windows))
        return {};
    const auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, 0));
    const auto bounds = static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
    CGRect rectangle{};
    if (!bounds || !CGRectMakeWithDictionaryRepresentation(bounds, &rectangle))
        return {};
    return QRect(qRound(rectangle.origin.x), qRound(rectangle.origin.y),
                 qRound(rectangle.size.width), qRound(rectangle.size.height));
}
QRect contentGeometry(const ScreenshotPinnedWindow& pin) {
    const auto bounds = pin.automationState().value(QStringLiteral("geometry")).toArray();
    return QRect(bounds[0].toInt(), bounds[1].toInt(), bounds[2].toInt(), bounds[3].toInt());
}
bool shellAboveContent(QWidget& pin, NSWindow* host) {
    CFArrayRef windows =
        CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windows)
        return false;
    const auto release = qScopeGuard([windows] { CFRelease(windows); });
    const auto list = reinterpret_cast<NSArray*>(windows);
    NSUInteger shellIndex = NSNotFound;
    NSUInteger hostIndex = NSNotFound;
    for (NSUInteger index = 0; index < list.count; ++index) {
        NSDictionary* info = list[index];
        const NSInteger number = [info[reinterpret_cast<NSString*>(kCGWindowNumber)] integerValue];
        if (number == native(pin).windowNumber)
            shellIndex = index;
        if (number == host.windowNumber)
            hostIndex = index;
    }
    return shellIndex != NSNotFound && hostIndex != NSNotFound && shellIndex < hostIndex;
}
CALayer* rasterLayer(CALayer* layer) {
    if (layer.contents &&
        CFGetTypeID(reinterpret_cast<CFTypeRef>(layer.contents)) == IOSurfaceGetTypeID())
        return layer;
    for (CALayer* child in layer.sublayers)
        if (CALayer* raster = rasterLayer(child))
            return raster;
    return nil;
}
QColor presentedPixel(QWidget& pin, QPoint point) {
    // Inspect Qt's published surface, not QWidget::grab(), which forces a paint
    // and cannot detect missing native paint delivery or covering native windows.
    NSView* view = reinterpret_cast<NSView*>(pin.winId());
    CALayer* layer = rasterLayer(view.layer);
    if (!layer)
        return {};
    IOSurfaceRef surface = reinterpret_cast<IOSurfaceRef>(layer.contents);
    if (IOSurfaceGetBytesPerElement(surface) != 4 ||
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) != 0)
        return {};
    const auto unlock =
        qScopeGuard([surface] { IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr); });
    const QImage image(static_cast<const uchar*>(IOSurfaceGetBaseAddress(surface)),
                       int(IOSurfaceGetWidth(surface)), int(IOSurfaceGetHeight(surface)),
                       qsizetype(IOSurfaceGetBytesPerRow(surface)),
                       QImage::Format_ARGB32_Premultiplied);
    point = (QPointF(point) * pin.devicePixelRatioF()).toPoint();
    return image.rect().contains(point) ? image.pixelColor(point) : QColor();
}
void requireStationaryOverlays(ScreenshotPinnedWindow& pin, NSWindow* host) {
    const QRect geometry = pin.geometry();
    const QRect content = contentGeometry(pin).translated(-pin.pos());
    const QPoint borderPoint(content.left(), content.center().y());
    const auto hasPixels = [&](QPoint point) {
        const QColor color = presentedPixel(pin, point);
        return color.isValid() && color.alpha() > 0;
    };
    waitFor([&] { return hasPixels(borderPoint); },
            "The stationary Qt shell publishes its border.");
    QEnterEvent enter(content.center(), content.center(), pin.mapToGlobal(content.center()));
    QApplication::sendEvent(&pin, &enter);
    auto* close = pin.findChild<QWidget*>(QStringLiteral("screenshotPinnedCloseButton"));
    require(close && close->isVisible(), "Pointer entry reveals the close button.");
    const QPoint buttonPoint = close->mapTo(&pin, close->rect().center());
    waitFor([&] { return hasPixels(buttonPoint); },
            "The stationary Qt shell publishes the upper-right controls.");
    const QPoint start = content.topLeft() + QPoint(30, 50);
    const QPoint end = content.center();
    QMouseEvent press(QEvent::MouseButtonPress, start, pin.mapToGlobal(start), Qt::RightButton,
                      Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(&pin, &press);
    QMouseEvent move(QEvent::MouseMove, end, pin.mapToGlobal(end), Qt::NoButton, Qt::RightButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&pin, &move);
    const QPoint cropPoint = (start + end) / 2;
    waitFor([&] { return presentedPixel(pin, cropPoint).alpha() == 0x33; },
            "The stationary Qt shell publishes the translucent crop preview.");
    require(pin.geometry() == geometry && shellAboveContent(pin, host) &&
                host.level == native(pin).level,
            "Showing Qt overlays must not promote native content above the stationary shell.");
    QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&pin, &cancel);
    waitFor([&] { return presentedPixel(pin, cropPoint).alpha() == 0; },
            "Cancelling the crop clears its published preview without dragging.");
}
} // namespace

int macLivePinnedWindowTests() {
    using namespace snow_shot::presentation;
    if (QGuiApplication::platformName() != QStringLiteral("cocoa") ||
        !CGPreflightScreenCaptureAccess())
        return 77;
    QWidget original;
    original.setGeometry(100, 100, 640, 480);
    original.setAutoFillBackground(true);
    QPalette palette = original.palette();
    palette.setColor(QPalette::Window, QColor(32, 96, 192));
    original.setPalette(palette);
    original.show();
    waitFor([&] { return !desktopBounds(native(original)).isEmpty(); },
            "The native source window appears.");
    const QRect bounds = desktopBounds(native(original));
    const LiveWindowTarget target{quintptr(native(original).windowNumber),
                                  NSProcessInfo.processInfo.processIdentifier, bounds,
                                  bounds.center()};
    auto source =
        createLiveWindowSource(target, QRect(bounds.topLeft() + QPoint(30, 50), QSize(480, 400)));
    require(bool(source), "The pinned shell has a valid source.");
    bool initialized = false;
    bool initializationSucceeded = false;
    QString initializationError;
    source->initialize([&](bool ok, const QString& message) {
        initializationSucceeded = ok;
        initializationError = message;
        initialized = true;
    });
    waitFor([&] { return initialized; }, "Native capture initialization completes.");
    require(initializationSucceeded, qPrintable(initializationError));
    ScreenshotPinnedWindow pin;
    ScreenshotPinnedWindow::Config config;
    config.screen = QGuiApplication::primaryScreen();
    config.nativeGeometry = QRect(800, 150, 480, 400);
    config.initialWindowSize = config.nativeGeometry.size();
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(config.initialWindowSize));
    config.imageSource =
        ScreenshotImageSource::fromImage(source->initialFrame(), config.canvasSourceRect);
    config.liveSource = source;
    config.automaticTextRecognition = false;
    config.checkerboardEnabled = false;
    config.initialBorderVisible = true;
    config.initialShadowVisible = false;
    require(pin.present(config), "The actual pinned shell starts presentation.");
    pin.setAttribute(Qt::WA_DeleteOnClose, false);
    waitFor([&] { return source->usesNativePresentation() && contentHost(pin).visible; },
            "The actual pinned shell switches to its native content surface.");
    NSWindow* host = [contentHost(pin) retain];
    const auto releaseHost = qScopeGuard([host] { [host release]; });
    const auto aligned = [&] { return desktopBounds(host) == contentGeometry(pin); };
    waitFor(aligned, "Native content aligns with the shell inside its decoration margins.");
    require(host.ignoresMouseEvents && !host.canBecomeKeyWindow,
            "The native content leaves input and focus to the Qt shell.");
    waitFor([&] { return shellAboveContent(pin, host); },
            "The Qt decorations must stay above the native content while the pin is stationary.");
    requireStationaryOverlays(pin, host);
    int cpuFrames = 0;
    QObject::connect(source.get(), &LiveWindowSource::frameChanged, &pin,
                     [&](const QImage&) { ++cpuFrames; });
    QString error;
    require(pin.automationUpdate({{QStringLiteral("geometry"), QJsonArray{620, 180, 240, 160}},
                                  {QStringLiteral("opacity_percent"), 65},
                                  {QStringLiteral("always_on_top"), false}},
                                 &error),
            "Moving, resizing, and changing native pin appearance succeeds.");
    waitFor(
        [&] {
            return aligned() && qAbs(host.alphaValue - 0.65) < 0.01 &&
                   host.level == native(pin).level && shellAboveContent(pin, host);
        },
        "Native placement, opacity, and stacking follow the real shell.");
    require(pin.automationUpdate({{QStringLiteral("rotation"), QStringLiteral("clockwise")},
                                  {QStringLiteral("flip"), QStringLiteral("horizontal")}},
                                 &error),
            "Native pin transforms succeed.");
    waitFor(aligned, "The native surface follows rotated pin geometry.");
    require(pin.automationUpdate({{QStringLiteral("always_on_top"), true}}, &error),
            "Restoring topmost presentation succeeds.");
    waitFor([&] { return shellAboveContent(pin, host) && host.level == native(pin).level; },
            "Re-entering the pinned capture band preserves the content's below-shell order.");
    require(source->usesNativePresentation() && cpuFrames == 0,
            "Real pin geometry changes keep GPU presentation without CPU frames.");
    pin.hide();
    waitFor([&] { return !host.visible; }, "Hiding the pin hides its native child.");
    pin.show();
    waitFor([&] { return host.visible && aligned() && shellAboveContent(pin, host); },
            "Showing the pin restores aligned native presentation.");
    pin.close();
    waitFor([&] { return !host.visible && !host.parentWindow; },
            "Closing the real pin releases its native child.");
    return 0;
}
