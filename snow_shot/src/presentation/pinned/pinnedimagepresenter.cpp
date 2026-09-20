#include "pinnedimagepresenter.h"

#include <QtMath>
#include <cmath>
#include <utility>

namespace snow_shot::presentation {

PinnedImagePresenter::PinnedImagePresenter(std::unique_ptr<PinnedImagePresentationBackend> backend)
    : m_backend(std::move(backend)) {}

PinnedImagePresenter::~PinnedImagePresenter() = default;

bool PinnedImagePresenter::present(WId window, const QRect& geometry, qreal ratio, quint8 opacity,
                                   const QRegion& dirtyLogical, const Paint& paint) {
    if (!m_backend || window == 0 || geometry.isEmpty() || !std::isfinite(ratio) || ratio <= 0 ||
        !paint) {
        return false;
    }

    const bool replace = !m_surface || m_surface->image().size() != geometry.size();
    auto candidate = replace ? m_backend->allocate(geometry.size()) : nullptr;
    auto* surface = replace ? candidate.get() : m_surface.get();
    if (!surface || surface->image().isNull() || surface->image().size() != geometry.size() ||
        surface->image().format() != QImage::Format_ARGB32_Premultiplied) {
        return false;
    }

    const bool full =
        replace || m_needsFullPaint || m_window != window || m_devicePixelRatio != ratio;
    const QRect logicalBounds(0, 0, qCeil(geometry.width() / ratio),
                              qCeil(geometry.height() / ratio));
    const QRegion dirty = full ? QRegion(logicalBounds) : dirtyLogical.intersected(logicalBounds);
    QRegion dirtyPixels;
    for (const QRect& rect : dirty) {
        dirtyPixels +=
            QRectF(rect.x() * ratio, rect.y() * ratio, rect.width() * ratio, rect.height() * ratio)
                .toAlignedRect();
    }
    dirtyPixels &= QRect(QPoint(), geometry.size());

    QImage& image = surface->image();
    image.setDevicePixelRatio(ratio);
    if (!dirty.isEmpty()) {
        QPainter painter(&image);
        if (!painter.isActive())
            return false;
        painter.setClipRegion(dirty);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(logicalBounds, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (!paint(painter, dirty)) {
            m_needsFullPaint = true;
            return false;
        }
    }

    if (!m_backend->publish(window, geometry, *surface, opacity, dirtyPixels)) {
        // USER32 retains the last successfully published pixels. The working
        // bitmap may have changed, so the next attempt must rebuild it in full.
        m_needsFullPaint = true;
        return false;
    }
    if (replace)
        m_surface = std::move(candidate);
    m_geometry = geometry;
    m_window = window;
    m_devicePixelRatio = ratio;
    m_needsFullPaint = false;
    return true;
}

bool PinnedImagePresenter::move(WId window, const QRect& geometry) {
    if (!hasFrame() || window != m_window || geometry.size() != m_geometry.size() ||
        !m_backend->move(window, geometry)) {
        return false;
    }
    m_geometry = geometry;
    return true;
}

void PinnedImagePresenter::observePosition(WId window, const QRect& geometry) {
    if (hasFrame() && window == m_window && geometry.size() == m_geometry.size())
        m_geometry.moveTopLeft(geometry.topLeft());
}

void PinnedImagePresenter::reset() {
    m_surface.reset();
    m_geometry = {};
    m_window = 0;
    m_devicePixelRatio = 1.0;
    m_needsFullPaint = true;
}

QRect PinnedImagePresenter::committedGeometry() const {
    return m_geometry;
}
bool PinnedImagePresenter::hasFrame() const {
    return m_surface && !m_geometry.isEmpty();
}

} // namespace snow_shot::presentation
