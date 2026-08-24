#pragma once

#include <QImage>
#include <QSize>

namespace snow_draw_engine_qt {

// Allocates 32-bit raster storage that is returned directly to the operating
// system when the last shallow QImage owner is destroyed.
[[nodiscard]] QImage allocateTransientImage(const QSize& size, QImage::Format format);

} // namespace snow_draw_engine_qt
