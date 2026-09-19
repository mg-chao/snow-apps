#include "captureframeimage.h"

#include <array>
#include <cstdlib>
#include <iostream>

struct SnowCaptureFrameLeaseImpl {
    std::array<uint8_t, 16> pixels{};
};

namespace {
int releases = 0;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

extern "C" void snow_capture_frame_lease_release(SnowCaptureFrameLease* lease) {
    ++releases;
    delete lease;
}

int main() {
    using namespace snow_shot::presentation::capture;
    auto* lease = new SnowCaptureFrameLease;
    lease->pixels = {123, 123, 123, 255, 123, 123, 123, 255,
                     123, 123, 123, 255, 123, 123, 123, 255};
    auto image = imageFromFrameLease(lease, lease->pixels.data(), 16, 2, 2, 8,
                                     SNOW_CAPTURE_PIXEL_FORMAT_BGRA8, FrameAlphaMode::Opaque);
    require(!image.isNull() && image.format() == QImage::Format_RGB32 &&
                image.pixelColor(1, 1).red() == 123,
            "native image pixels were not wrapped correctly");
    auto retained = image;
    image = {};
    require(releases == 0 && retained.pixelColor(0, 0).green() == 123,
            "image copy lost its native lease");
    retained = {};
    require(releases == 1, "final image release did not release the native lease exactly once");
    for (const auto pixelFormat :
         {SNOW_CAPTURE_PIXEL_FORMAT_BGRA8, SNOW_CAPTURE_PIXEL_FORMAT_RGBA8}) {
        auto* transparent = new SnowCaptureFrameLease;
        transparent->pixels = {10, 20, 30, 0, 40, 50, 60, 64, 70, 80, 90, 128, 100, 110, 120, 255};
        const auto* pixels = transparent->pixels.data();
        const QImage wrapped = imageFromFrameLease(transparent, pixels, 16, 2, 2, 8, pixelFormat,
                                                   FrameAlphaMode::Preserve);
        require(wrapped.constBits() == pixels && wrapped.hasAlphaChannel(),
                "preserving native alpha copied pixels or lost the alpha format tag");
        const QImage converted = wrapped.convertToFormat(QImage::Format_RGBA8888);
        const std::array<int, 4> alpha{0, 64, 128, 255};
        for (int pixel = 0; pixel < 4; ++pixel) {
            const QColor expected(
                pixelFormat == SNOW_CAPTURE_PIXEL_FORMAT_BGRA8 ? pixels[pixel * 4 + 2]
                                                               : pixels[pixel * 4],
                pixels[pixel * 4 + 1],
                pixelFormat == SNOW_CAPTURE_PIXEL_FORMAT_BGRA8 ? pixels[pixel * 4]
                                                               : pixels[pixel * 4 + 2],
                alpha[static_cast<std::size_t>(pixel)]);
            require(wrapped.pixelColor(pixel % 2, pixel / 2) == expected &&
                        converted.pixelColor(pixel % 2, pixel / 2) == expected,
                    "native frame conversion changed colors or transparency");
        }
    }
    require(releases == 3, "transparent native images leaked their leases");
    for (const auto width : {0u, 3u, 0xffffffffu}) {
        auto* invalid = new SnowCaptureFrameLease;
        require(imageFromFrameLease(invalid, invalid->pixels.data(), 16, width, 2, 8, 0,
                                    FrameAlphaMode::Preserve)
                    .isNull(),
                "invalid native dimensions were accepted");
    }
    require(releases == 6, "invalid image leaked a native lease");
    SnowCaptureFrameInfo info{};
    require(!validFrameInfo(info), "empty frame info was accepted");
    return 0;
}
