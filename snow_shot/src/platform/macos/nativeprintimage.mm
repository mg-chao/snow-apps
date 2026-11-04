// SPDX-License-Identifier: GPL-3.0-or-later
#include "nativeprintimage.h"

#include <memory>
#include <new>
#include <utility>

namespace snow_shot::platform::macos {
CGImageRef createPrintImage(QImage prepared) {
    if (prepared.isNull() || prepared.format() != QImage::Format_RGB32)
        return nullptr;
    std::unique_ptr<QImage> owner(new (std::nothrow) QImage(std::move(prepared)));
    if (!owner)
        return nullptr;
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        owner.get(), owner->constBits(), static_cast<size_t>(owner->sizeInBytes()),
        [](void* context, const void*, size_t) { delete static_cast<QImage*>(context); });
    if (!provider)
        return nullptr;
    const auto pixels = owner.release();
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const auto info = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Host) |
                      static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst);
    CGImageRef image =
        colorSpace ? CGImageCreate(static_cast<size_t>(pixels->width()),
                                   static_cast<size_t>(pixels->height()), 8, 32,
                                   static_cast<size_t>(pixels->bytesPerLine()), colorSpace, info,
                                   provider, nullptr, false, kCGRenderingIntentDefault)
                   : nullptr;
    if (colorSpace)
        CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    return image;
}
} // namespace snow_shot::platform::macos
