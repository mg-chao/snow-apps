#include <QColorSpace>
#include <QImage>

#include <CoreGraphics/CoreGraphics.h>

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    // Cocoa converts both custom cursors and standard cursor artwork through
    // QImage::toCGImage. An explicit ICC profile exercises QTBUG-147602 without
    // a GUI session; an untagged image covers the fallback color space too.
    for (int iteration = 0; iteration < 256; ++iteration) {
        const std::array spaces{
            QColorSpace(), QColorSpace(QColorSpace::SRgb), QColorSpace(QColorSpace::DisplayP3),
            QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::Gamma,
                        1.8F + static_cast<float>(iteration) / 1000.0F)};
        for (const auto& space : spaces) {
            QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
            image.setColorSpace(space);
            image.fill(QColor(128, 32, 200, 192));
            CGImageRef nativeImage = image.toCGImage();
            require(nativeImage != nullptr, "cursor image must convert to a native image");
            // The returned native image must own its color space and pixel data
            // even after the source QImage has been destroyed.
            image = {};
            require(CGImageGetWidth(nativeImage) == 32 && CGImageGetHeight(nativeImage) == 32,
                    "native image must preserve cursor dimensions");
            require(CGColorSpaceGetModel(CGImageGetColorSpace(nativeImage)) ==
                        kCGColorSpaceModelRGB,
                    "native image must retain a valid RGB color space");
            CFDataRef pixels = CGDataProviderCopyData(CGImageGetDataProvider(nativeImage));
            require(pixels != nullptr && CFDataGetLength(pixels) >= 32 * 32 * 4,
                    "native image must retain its pixel buffer");
            CFRelease(pixels);
            CGImageRelease(nativeImage);
        }
    }
    return 0;
}
