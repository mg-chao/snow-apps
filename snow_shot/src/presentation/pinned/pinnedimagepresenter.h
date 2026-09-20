#ifndef SNOW_SHOT_PRESENTATION_PINNEDIMAGEPRESENTER_H
#define SNOW_SHOT_PRESENTATION_PINNEDIMAGEPRESENTER_H

#include <QImage>
#include <QPainter>
#include <QRect>
#include <QRegion>
#include <QtGui/qwindowdefs.h>
#include <functional>
#include <memory>

namespace snow_shot::presentation {

// The backend owns the bitmap's storage. Its QImage is a non-owning view of that
// storage, so QPainter writes the bytes which are submitted to the compositor.
class PinnedImageSurface {
  public:
    virtual ~PinnedImageSurface() = default;
    virtual QImage& image() = 0;
};

class PinnedImagePresentationBackend {
  public:
    virtual ~PinnedImagePresentationBackend() = default;
    virtual std::unique_ptr<PinnedImageSurface> allocate(const QSize& pixels) = 0;
    virtual bool publish(WId window, const QRect& pixels, PinnedImageSurface& surface,
                         quint8 opacity, const QRegion& dirtyPixels) = 0;
    virtual bool move(WId window, const QRect& pixels) = 0;
};

class PinnedImagePresenter final {
  public:
    using Paint = std::function<bool(QPainter&, const QRegion&)>;

    explicit PinnedImagePresenter(std::unique_ptr<PinnedImagePresentationBackend> backend);
    ~PinnedImagePresenter();
    PinnedImagePresenter(const PinnedImagePresenter&) = delete;
    PinnedImagePresenter& operator=(const PinnedImagePresenter&) = delete;

    bool present(WId window, const QRect& physicalGeometry, qreal devicePixelRatio, quint8 opacity,
                 const QRegion& dirtyLogical, const Paint& paint);
    bool move(WId window, const QRect& physicalGeometry);
    // Observe a successful USER32 position-only move without painting or moving again.
    void observePosition(WId window, const QRect& physicalGeometry);
    void reset();
    QRect committedGeometry() const;
    bool hasFrame() const;

  private:
    std::unique_ptr<PinnedImagePresentationBackend> m_backend;
    std::unique_ptr<PinnedImageSurface> m_surface;
    QRect m_geometry;
    WId m_window = 0;
    qreal m_devicePixelRatio = 1.0;
    bool m_needsFullPaint = true;
};

std::unique_ptr<PinnedImagePresentationBackend> createWindowsPinnedImagePresentationBackend();

} // namespace snow_shot::presentation
#endif
