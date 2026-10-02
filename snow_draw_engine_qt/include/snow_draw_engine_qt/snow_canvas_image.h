// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_IMAGE_H
#define SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_IMAGE_H

#include <QImage>

// Large rasters return their storage to the OS when the last sharing image is
// destroyed. Small rasters use Qt's normal allocator. As with QImage, a writable
// detached copy must be allocated explicitly if its storage policy matters.
[[nodiscard]] QImage snowCanvasAllocateImage(const QSize& size, QImage::Format format);

#endif
