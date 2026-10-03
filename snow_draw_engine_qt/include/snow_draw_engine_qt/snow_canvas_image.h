// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_IMAGE_H
#define SNOW_DRAW_ENGINE_QT_SNOW_CANVAS_IMAGE_H

#include <QImage>
#include <QColorSpace>
#include <QTransform>

// Large rasters return their storage to the OS when the last sharing image is
// destroyed. Small rasters use Qt's normal allocator. As with QImage, a writable
// detached copy must be allocated explicitly if its storage policy matters.
[[nodiscard]] QImage snowCanvasAllocateImage(const QSize& size, QImage::Format format);

// Copy/crop into exclusively writable storage, preserving QImage's layout and
// metadata. A null rectangle means the entire image, as with QImage::copy().
[[nodiscard]] QImage snowCanvasCopyImage(const QImage& image, const QRect& rect = {});

// Make storage exclusively writable. Callers owning writable pixels (including
// render-pool views) may avoid copying a unique image by declaring that access.
// Unknown external storage is copied, including unique read-only capture leases.
[[nodiscard]] bool snowCanvasDetachImage(QImage& image, bool writableStorage = false);

// Direct-color conversions write into the final allocation with bounded,
// concurrent work chunks. Indexed/monochrome output retains Qt's global palette
// and dithering; ordered dithering keeps its global phase across direct-color chunks.
[[nodiscard]] QImage snowCanvasConvertImage(const QImage& image, QImage::Format format,
                                            Qt::ImageConversionFlags flags = Qt::AutoColor);
[[nodiscard]] QImage snowCanvasColorConvertedImage(const QImage& image,
                                                   const QColorSpace& colorSpace,
                                                   QImage::Format format,
                                                   Qt::ImageConversionFlags flags = Qt::AutoColor);

// Quarter-turn rotations use tiled pixel copies into their final storage.
// Large smooth flips use bounded conversion chunks to retain Qt's alpha rounding.
// Other transforms and scaling preserve Qt resampling, then transfer large
// results into managed storage; Qt owns their temporary resampling allocation.
[[nodiscard]] QImage snowCanvasTransformImage(const QImage& image, const QTransform& transform,
                                              Qt::TransformationMode mode = Qt::FastTransformation);

[[nodiscard]] QImage snowCanvasScaleImage(const QImage& image, const QSize& size,
                                          Qt::AspectRatioMode aspectRatio = Qt::IgnoreAspectRatio,
                                          Qt::TransformationMode mode = Qt::FastTransformation);

#endif
