#ifndef SNOW_SHOT_PRESENTATION_LIVEWINDOWNATIVE_P_H
#define SNOW_SHOT_PRESENTATION_LIVEWINDOWNATIVE_P_H

#include "snow_shot/presentation/livewindowsource.h"

namespace snow_shot::presentation::live_window_detail {
struct NativeState {
    LiveWindowState state = LiveWindowState::Live;
    QRect desktopBounds;
    QString error;
};

class NativeWindowMonitor {
  public:
    // Native adapters invoke this on the GUI thread for lifecycle/geometry
    // events; polling remains a fallback for providers without notifications.
    std::function<void()> changed;
    virtual ~NativeWindowMonitor() = default;
    virtual bool initialize(QString* error) = 0;
    virtual NativeState inspect() = 0;
    virtual bool hasLifecycleNotifications() const {
        return false;
    }
};

class NativeWindowPresentation {
  public:
    virtual ~NativeWindowPresentation() = default;
    // Compositor thumbnails need no frames. Texture presenters consume immutable
    // native leases; CPU pixels are materialized only for explicit image actions.
    virtual bool consumesFrames() const {
        return false;
    }
    virtual bool supportsTransform(const QTransform& transform) const {
        return transform.isIdentity();
    }
    virtual bool present(const LiveWindowPresentation& presentation, const QRectF& crop,
                         const QRect& sourceBounds, QString* error) = 0;
    virtual bool presenting() const = 0;
    virtual void stopPresentation() = 0;
    virtual bool presentFrame(std::shared_ptr<const class NativeWindowFrame>, const QRectF&) {
        return false;
    }
};

class NativeWindowFrame {
  public:
    virtual ~NativeWindowFrame() = default;
    virtual QSize size() const = 0;
    // Called on capture workers, never the GUI thread.
    virtual QImage image() const = 0;
    virtual const void* pixelBuffer() const = 0;
};
struct NativeCaptureFrame {
    std::shared_ptr<const NativeWindowFrame> pixels;
    QRectF desktopBounds;
    bool duplicate = false;
};
class NativeWindowCapture {
  public:
    virtual ~NativeWindowCapture() = default;
    // A successful timeout/configuration event has no pixels. Worker-thread only.
    virtual bool receive(NativeCaptureFrame* frame) = 0;
    // Thread-safe; interrupts receive and startup without waiting for teardown.
    virtual void cancel() = 0;
};
std::unique_ptr<NativeWindowCapture> createNativeWindowCapture(const LiveWindowTarget& target);

std::unique_ptr<NativeWindowMonitor> createNativeWindowMonitor(const LiveWindowTarget& target);
std::unique_ptr<NativeWindowPresentation>
createNativeWindowPresentation(const LiveWindowTarget& target);

// A fixed source-local rectangle is intersected without changing its destination extent.
struct CropMapping {
    QRectF source;
    QRectF destination;
};

// Both origins and the crop use native desktop units. Invisible source-window
// margins shift the crop origin without scaling its pixel extent.
inline QRectF windowRelativeCrop(const QRectF& visibleFrameCrop, const QPoint& visibleFrameOrigin,
                                 const QPoint& windowOrigin) {
    return visibleFrameCrop.translated(visibleFrameOrigin - windowOrigin);
}
inline CropMapping cropMapping(const QRectF& crop, const QRectF& available,
                               const QRectF& destination) {
    const QRectF source = crop.intersected(available);
    if (source.isEmpty() || crop.isEmpty() || destination.isEmpty())
        return {};
    return {source,
            QRectF(destination.x() + (source.x() - crop.x()) * destination.width() / crop.width(),
                   destination.y() + (source.y() - crop.y()) * destination.height() / crop.height(),
                   source.width() * destination.width() / crop.width(),
                   source.height() * destination.height() / crop.height())};
}
} // namespace snow_shot::presentation::live_window_detail
#endif
