#ifndef SNOW_SHOT_PRESENTATION_LIVEWINDOWNATIVE_P_H
#define SNOW_SHOT_PRESENTATION_LIVEWINDOWNATIVE_P_H

#include "snow_shot/presentation/livewindowsource.h"

namespace snow_shot::presentation::live_window_detail {
struct NativeState {
    LiveWindowState state = LiveWindowState::Live;
    QRect desktopBounds;
    QString error;
};

class NativeWindow {
  public:
    // Native adapters invoke this on the GUI thread for lifecycle/geometry
    // events; polling remains a fallback for providers without notifications.
    std::function<void()> changed;
    virtual ~NativeWindow() = default;
    virtual bool initialize(QString* error) = 0;
    virtual NativeState inspect() = 0;
    virtual bool present(const LiveWindowPresentation& presentation, const QRectF& crop,
                         const QRectF& initialBounds, QString* error) = 0;
    virtual bool presenting() const = 0;
    virtual void stopPresentation() = 0;
};

std::unique_ptr<NativeWindow> createNativeWindow(const LiveWindowTarget& target);

// A fixed source-local rectangle is intersected without changing its destination extent.
struct CropMapping {
    QRectF source;
    QRectF destination;
};
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
