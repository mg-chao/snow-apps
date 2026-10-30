// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../presentation/pinned/livewindownative_p.h"
#include "../../presentation/pinned/livewindownativemapping_p.h"
#include "capturewindowlayers_p.h"

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
#include <QEvent>
#include <QGuiApplication>
#include <QTimer>
#include <QWidget>

@interface SnowLiveWindowHost : NSPanel <SnowCaptureContentWindow>
@end
@implementation SnowLiveWindowHost
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

@interface SnowLiveWindowView : NSView
@end
@implementation SnowLiveWindowView
- (BOOL)isFlipped {
    return YES;
}
@end

namespace snow_shot::presentation::live_window_detail {
namespace {
class MacLivePresentation final : public QObject, public NativeWindowPresentation {
  public:
    ~MacLivePresentation() override {
        stopPresentation();
    }
    bool consumesFrames() const override {
        return true;
    }
    bool supportsTransform(const QTransform& transform) const override {
        return transform.isAffine() && transform.isInvertible();
    }
    bool present(const LiveWindowPresentation& presentation, const QRectF& crop, const QRect&,
                 QString*) override {
        if (!presentation.owner || QGuiApplication::platformName() != QStringLiteral("cocoa"))
            return false;
        if (m_presentation.owner != presentation.owner) {
            stopPresentation();
            presentation.owner->installEventFilter(this);
        }
        m_presentation = presentation;
        m_crop = crop;
        if (!presentation.visible) {
            if (m_host)
                [m_host orderOut:nil];
            clearFrame();
            return true;
        }
        if (!attach())
            return false;
        synchronize();
        return true;
    }
    bool presenting() const override {
        return m_host && m_presentation.visible;
    }
    bool presentFrame(std::shared_ptr<const NativeWindowFrame> frame,
                      const QRectF& bounds) override {
        if (!presenting() || !frame || bounds.isEmpty())
            return false;
        auto buffer = static_cast<CVPixelBufferRef>(const_cast<void*>(frame->pixelBuffer()));
        if (!buffer)
            return false;
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(buffer);
        if (!surface)
            return false;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        // Core Animation transports IOSurfaces to WindowServer using native
        // surface ownership. Also retain the old CVPixelBuffer lease through the
        // replacement transaction; the bounded capture pool cannot overwrite it.
        auto previous = std::move(m_frame);
        [CATransaction setCompletionBlock:^{
          (void)previous;
        }];
        m_frame = std::move(frame);
        m_bounds = bounds;
        m_pixels.contents = reinterpret_cast<id>(surface);
        updateLayers();
        [CATransaction commit];
        return true;
    }
    void stopPresentation() override {
        if (m_presentation.owner)
            m_presentation.owner->removeEventFilter(this);
        m_presentation.owner = nullptr;
        clearFrame();
        if (m_host) {
            [m_host.parentWindow removeChildWindow:m_host];
            [m_host orderOut:nil];
            [m_host release];
            m_host = nil;
        }
        [m_pixels release];
        m_pixels = nil;
        [m_root release];
        m_root = nil;
        m_shell = nil;
    }

  private:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == m_presentation.owner) {
            if (event->type() == QEvent::Hide) {
                if (m_host)
                    [m_host orderOut:nil];
            } else if (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
                       event->type() == QEvent::Show ||
                       event->type() == QEvent::ScreenChangeInternal ||
                       event->type() == QEvent::DevicePixelRatioChange) {
                // Qt must finish updating the native surface before placement.
                if (!m_syncPending) {
                    m_syncPending = true;
                    QTimer::singleShot(0, this, [this] {
                        m_syncPending = false;
                        if (m_presentation.owner && m_host)
                            synchronize();
                    });
                }
            }
        }
        return false;
    }
    bool attach() {
        NSView* view = reinterpret_cast<NSView*>(m_presentation.owner->winId());
        NSWindow* shell = view.window;
        if (!shell)
            return false;
        if (!m_host) {
            m_host =
                [[SnowLiveWindowHost alloc] initWithContentRect:NSMakeRect(0, 0, 1, 1)
                                                      styleMask:NSWindowStyleMaskBorderless |
                                                                NSWindowStyleMaskNonactivatingPanel
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
            m_host.releasedWhenClosed = NO;
            m_host.hasShadow = NO;
            m_host.ignoresMouseEvents = YES;
            m_host.hidesOnDeactivate = NO;
            m_host.opaque = YES;
            m_host.colorSpace = NSColorSpace.sRGBColorSpace;
            m_host.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                        NSWindowCollectionBehaviorFullScreenAuxiliary |
                                        NSWindowCollectionBehaviorIgnoresCycle;
            // AppKit owns the backing layer's geometryFlipped value. The view
            // itself must use top-left coordinates to match the source crop.
            auto* content = [[SnowLiveWindowView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
            m_host.contentView = content;
            [content release];
            m_root = [[CALayer alloc] init];
            m_root.masksToBounds = YES;
            m_pixels = [[CALayer alloc] init];
            m_pixels.anchorPoint = CGPointZero;
            m_pixels.position = CGPointZero;
            m_pixels.contentsGravity = kCAGravityResize;
            m_pixels.minificationFilter = kCAFilterLinear;
            m_pixels.magnificationFilter = kCAFilterLinear;
            [m_root addSublayer:m_pixels];
            m_host.contentView.layer = m_root;
            m_host.contentView.wantsLayer = YES;
        }
        if (m_shell != shell) {
            [m_host.parentWindow removeChildWindow:m_host];
            [shell addChildWindow:m_host ordered:NSWindowBelow];
            m_shell = shell;
        }
        return true;
    }
    void synchronize() {
        if (!m_host || !m_presentation.owner || !attach())
            return;
        NSView* view = reinterpret_cast<NSView*>(m_presentation.owner->winId());
        NSRect frame = [m_shell convertRectToScreen:[view convertRect:view.bounds toView:nil]];
        const auto margins = m_presentation.nativeContentMargins;
        frame.origin.x += margins.left();
        frame.origin.y += margins.bottom();
        frame.size.width = std::max(1.0, frame.size.width - margins.left() - margins.right());
        frame.size.height = std::max(1.0, frame.size.height - margins.top() - margins.bottom());
        if (!NSEqualRects(m_host.frame, frame))
            [m_host setFrame:frame display:NO];
        m_host.alphaValue = m_presentation.effectiveOpacity;
        m_host.level = m_shell.level;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        const QColor color = m_presentation.backgroundColor;
        m_root.backgroundColor = [NSColor colorWithSRGBRed:CGFloat(color.redF())
                                                     green:CGFloat(color.greenF())
                                                      blue:CGFloat(color.blueF())
                                                     alpha:1]
                                     .CGColor;
        m_root.contentsScale = m_shell.backingScaleFactor;
        updateLayers();
        [CATransaction commit];
        if (m_presentation.visible && m_presentation.owner->isVisible() && !m_shell.miniaturized) {
            if (!m_host.visible)
                [m_host orderWindow:NSWindowBelow relativeTo:m_shell.windowNumber];
        } else {
            [m_host orderOut:nil];
        }
    }
    void updateLayers() {
        if (!m_pixels || m_bounds.isEmpty())
            return;
        const NSSize viewport = m_host.contentView.bounds.size;
        const auto placement = nativeFramePlacement(m_crop, m_presentation.imageTransform,
                                                    QSizeF(viewport.width, viewport.height));
        m_pixels.hidden = !placement.has_value();
        if (!placement)
            return;
        m_pixels.bounds = CGRectMake(0, 0, m_bounds.width(), m_bounds.height());
        const auto& t = *placement;
        m_pixels.affineTransform =
            CGAffineTransformMake(t.m11(), t.m12(), t.m21(), t.m22(), t.dx(), t.dy());
    }
    void clearFrame() {
        if (m_pixels) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            auto previous = std::move(m_frame);
            [CATransaction setCompletionBlock:^{
              (void)previous;
            }];
            m_pixels.contents = nil;
            [CATransaction commit];
        }
        m_frame.reset();
        m_bounds = {};
    }
    LiveWindowPresentation m_presentation;
    QRectF m_crop;
    QRectF m_bounds;
    std::shared_ptr<const NativeWindowFrame> m_frame;
    SnowLiveWindowHost* m_host = nil;
    NSWindow* m_shell = nil;
    CALayer* m_root = nil;
    CALayer* m_pixels = nil;
    bool m_syncPending = false;
};
} // namespace
std::unique_ptr<NativeWindowPresentation> createNativeWindowPresentation(const LiveWindowTarget&) {
    return std::make_unique<MacLivePresentation>();
}
} // namespace snow_shot::presentation::live_window_detail
