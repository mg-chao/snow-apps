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

// Initialize every storage byte, including row padding, to zero. Alpha formats
// start transparent; opaque and indexed formats interpret zero according to
// their format/palette. This is not QImage::fill(0), which also sets opaque bits
// in some RGB formats. Fresh managed pages are already zeroed by the OS.
[[nodiscard]] QImage snowCanvasAllocateZeroedImage(const QSize& size, QImage::Format format);

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

// Synchronously copy/convert complete source rows into borrowed RGBA8888 bytes.
// No destination ownership is taken and padding is left unchanged. The caller
// must provide writable storage that does not overlap the source. A valid target
// color space requires a valid source profile; an invalid target means format
// conversion only. Default direct-color conversions use bounded temporary
// tiles, including palette expansion. Explicit non-threshold dithering retains
// Qt's global palette/phase behavior through a full fallback.
// On failure the destination may contain partially converted rows.
[[nodiscard]] bool snowCanvasCopyRgba8888Rows(const QImage& image, int firstRow, int rowCount,
                                              uchar* destination, qsizetype destinationBytes,
                                              qsizetype destinationStride,
                                              const QColorSpace& colorSpace = {},
                                              Qt::ImageConversionFlags flags = Qt::AutoColor);

// Quarter-turn rotations use tiled pixel copies into their final storage.
// Large smooth flips use bounded conversion chunks to retain Qt's alpha rounding.
// Common arbitrary 32-bit rotations paint directly into their final storage.
// Uncommon transforms and scaling preserve Qt resampling, then transfer large
// results into managed storage; Qt owns their temporary resampling allocation.
[[nodiscard]] QImage snowCanvasTransformImage(const QImage& image, const QTransform& transform,
                                              Qt::TransformationMode mode = Qt::FastTransformation);

[[nodiscard]] QImage snowCanvasScaleImage(const QImage& image, const QSize& size,
                                          Qt::AspectRatioMode aspectRatio = Qt::IgnoreAspectRatio,
                                          Qt::TransformationMode mode = Qt::FastTransformation);

#endif
