#include "snowimageqtcodec.h"
#include "snowimageanimationbridge.h"
#include "snow_shot/image/screenshotanimationsource.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QTransform>
#include <QtEndian>

#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace snow_shot::image_codec {
namespace {
constexpr std::size_t kErrorCapacity = 1024;
void reportError(QString* error, const char* detail) {
    if (!error)
        return;
    *error = detail && *detail ? QCoreApplication::translate("SnowImageQtCodec",
                                                             "Could not decode animated image.")
                               : QString{};
}

int SNOW_SHOT_IMAGE_CODEC_CALL animationCancelledCallback(void* context) noexcept {
    const auto* check = static_cast<const AnimationCancellationCheck*>(context);
    try {
        return check && *check && (*check)() ? 1 : 0;
    } catch (...) {
        return 1;
    }
}
int SNOW_SHOT_IMAGE_CODEC_CALL admissionCallback(void* context, uint64_t bytes) noexcept {
    const auto* check = static_cast<const AnimationAllocationCheck*>(context);
    if (bytes > static_cast<uint64_t>(std::numeric_limits<qint64>::max()))
        return 0;
    try {
        return !check || !*check || (*check)(static_cast<qint64>(bytes)) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// Static PNG import retains its existing allocation path. Probe only chunk
// headers here, leaving all animation integrity checks to the native indexer.
bool couldBeAnimation(const QByteArray& encoded) {
    if (!encoded.startsWith("\x89PNG\r\n\x1a\n"))
        return encoded.startsWith("GIF87a") || encoded.startsWith("GIF89a") ||
               (encoded.size() >= 12 && encoded.startsWith("RIFF") && encoded.mid(8, 4) == "WEBP");
    for (qsizetype position = 8; position <= encoded.size() - 12;) {
        const auto* data = reinterpret_cast<const uchar*>(encoded.constData() + position);
        const quint64 length = (quint64{data[0]} << 24U) | (quint64{data[1]} << 16U) |
                               (quint64{data[2]} << 8U) | data[3];
        if (std::memcmp(data + 4, "acTL", 4) == 0 || std::memcmp(data + 4, "fcTL", 4) == 0 ||
            std::memcmp(data + 4, "fdAT", 4) == 0)
            return true;
        if (length > static_cast<quint64>(encoded.size() - position - 12))
            return false;
        position += static_cast<qsizetype>(length) + 12;
    }
    return false;
}

QColorSpace colorSpace(const SnowShotImageCodecColorEncoding& color) {
    if (color.icc_profile && color.icc_profile_size &&
        color.icc_profile_size <= static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
        auto space =
            QColorSpace::fromIccProfile(QByteArray(reinterpret_cast<const char*>(color.icc_profile),
                                                   static_cast<qsizetype>(color.icc_profile_size)));
        if (space.isValid())
            return space;
    }
    QColorSpace::Primaries primaries;
    switch (color.primaries) {
    case SNOW_SHOT_IMAGE_CODEC_PRIMARIES_SRGB:
        primaries = QColorSpace::Primaries::SRgb;
        break;
    case SNOW_SHOT_IMAGE_CODEC_PRIMARIES_DISPLAY_P3:
        primaries = QColorSpace::Primaries::DciP3D65;
        break;
    case SNOW_SHOT_IMAGE_CODEC_PRIMARIES_ADOBE_RGB:
        primaries = QColorSpace::Primaries::AdobeRgb;
        break;
    case SNOW_SHOT_IMAGE_CODEC_PRIMARIES_REC2020:
        primaries = QColorSpace::Primaries::Bt2020;
        break;
    default:
        return {};
    }
    QColorSpace::TransferFunction transfer;
    switch (color.transfer) {
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_LINEAR:
        transfer = QColorSpace::TransferFunction::Linear;
        break;
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_SRGB:
        transfer = QColorSpace::TransferFunction::SRgb;
        break;
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_GAMMA:
        transfer = QColorSpace::TransferFunction::Gamma;
        break;
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_PQ:
        transfer = QColorSpace::TransferFunction::St2084;
        break;
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_HLG:
        transfer = QColorSpace::TransferFunction::Hlg;
        break;
    default:
        return {};
    }
    return QColorSpace(primaries, transfer);
}

void releaseFrame(void* raw) {
    auto* frame = static_cast<SnowShotAnimationFrame*>(raw);
    snow_shot_animation_frame_release(frame);
    delete frame;
}

QTransform orientationTransform(uint32_t orientation) {
    // EXIF maps source coordinates into the correctly oriented display canvas.
    switch (orientation) {
    case 2:
        return QTransform(-1, 0, 0, 1, 0, 0);
    case 3:
        return QTransform(-1, 0, 0, -1, 0, 0);
    case 4:
        return QTransform(1, 0, 0, -1, 0, 0);
    case 5:
        return QTransform(0, 1, 1, 0, 0, 0);
    case 6:
        return QTransform(0, 1, -1, 0, 0, 0);
    case 7:
        return QTransform(0, -1, -1, 0, 0, 0);
    case 8:
        return QTransform(0, -1, 1, 0, 0, 0);
    default:
        return {};
    }
}
} // namespace

class AnimationDecoder::Impl final {
  public:
    static std::unique_ptr<Impl> openEncoded(const QByteArray& encoded, QString* error,
                                             AnimationCancellationCheck cancelled,
                                             AnimationAllocationCheck allocationCheck,
                                             bool firstFrameOnly = false) {
        if (error)
            error->clear();
        if (!couldBeAnimation(encoded))
            return {};
        if (snow_shot_animation_abi_version() != SNOW_SHOT_ANIMATION_CODEC_ABI_VERSION) {
            reportError(error, "Animation codec ABI is incompatible.");
            return {};
        }
        auto impl = std::make_unique<Impl>();
        impl->encoded = encoded;
        std::array<char, kErrorCapacity> detail{};
        impl->decoder = snow_shot_animation_open(
            reinterpret_cast<const uint8_t*>(impl->encoded.constData()),
            static_cast<uint64_t>(impl->encoded.size()), firstFrameOnly ? 1 : 0,
            animationCancelledCallback, &cancelled, admissionCallback, &allocationCheck,
            &impl->info, detail.data(), detail.size());
        if (!impl->decoder) {
            reportError(error, detail.data());
            return {};
        }
        impl->frameColorSpace = colorSpace(impl->info.color);
        return impl;
    }
    ~Impl() {
        snow_shot_animation_destroy(decoder);
    }
    QByteArray encoded;
    std::shared_ptr<const ScreenshotAnimationSource> source;
    SnowShotAnimationDecoder* decoder = nullptr;
    SnowShotAnimationInfo info{};
    QColorSpace frameColorSpace;
    AnimationAllocationCheck allocationCheck;
    uint64_t requiredBytes = 0;
    bool admitted() {
        return admissionCallback(&allocationCheck, requiredBytes) != 0;
    }
};

AnimationDecoder::AnimationDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AnimationDecoder::~AnimationDecoder() = default;
AnimationDecoder::AnimationDecoder(AnimationDecoder&&) noexcept = default;
AnimationDecoder& AnimationDecoder::operator=(AnimationDecoder&&) noexcept = default;

bool AnimationDecoder::advance(QString* error, AnimationCancellationCheck cancelled) {
    std::array<char, kErrorCapacity> detail{};
    const int result = impl_
                           ? snow_shot_animation_advance(impl_->decoder, animationCancelledCallback,
                                                         &cancelled, detail.data(), detail.size())
                           : -1;
    reportError(error, detail.data());
    return result == 1;
}
bool AnimationDecoder::reset(QString* error) {
    std::array<char, kErrorCapacity> detail{};
    if (impl_ && !impl_->admitted()) {
        reportError(error, "Animation allocation was not admitted.");
        return false;
    }
    const bool result =
        impl_ && snow_shot_animation_reset(impl_->decoder, detail.data(), detail.size());
    reportError(error, detail.data());
    return result;
}
int AnimationDecoder::frameIndex() const noexcept {
    return impl_ ? static_cast<int>(snow_shot_animation_index(impl_->decoder)) : -1;
}
QImage AnimationDecoder::snapshot(QString* error) {
    std::array<char, kErrorCapacity> detail{};
    if (impl_ && !impl_->admitted()) {
        reportError(error, "Animation allocation was not admitted.");
        return {};
    }
    auto frame = std::unique_ptr<SnowShotAnimationFrame, decltype(&releaseFrame)>(
        new SnowShotAnimationFrame{}, &releaseFrame);
    if (!impl_ ||
        !snow_shot_animation_snapshot(impl_->decoder, frame.get(), detail.data(), detail.size())) {
        reportError(error, detail.data());
        return {};
    }
    QImage::Format format = QImage::Format_Invalid;
    if (frame->pixel_format == SNOW_SHOT_ANIMATION_PIXEL_FORMAT_RGBA8_PREMULTIPLIED)
        format = QImage::Format_RGBA8888_Premultiplied;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    else if (frame->pixel_format == SNOW_SHOT_ANIMATION_PIXEL_FORMAT_BGRA8_PREMULTIPLIED)
        format = QImage::Format_ARGB32_Premultiplied;
#endif
    if (format == QImage::Format_Invalid) {
        reportError(error, "Frame pixel format is unsupported.");
        return {};
    }
    QImage image(frame->data, static_cast<int>(frame->width), static_cast<int>(frame->height),
                 static_cast<qsizetype>(frame->row_stride), format, &releaseFrame, frame.get());
    if (image.isNull()) {
        reportError(error, "Frame allocation failed.");
        return {};
    }
    // The QImage cleanup callback now owns this frame, even after the decoder
    // has been destroyed or the image has crossed to another thread.
    [[maybe_unused]] const auto* imageOwner = frame.release();
    image.setColorSpace(impl_->frameColorSpace);
    if (impl_->info.orientation > 1 && impl_->info.orientation <= 8)
        image = snowCanvasTransformImage(image, orientationTransform(impl_->info.orientation));
    if (impl_->source && !impl_->source->sourceCropRect.isEmpty())
        image = snowCanvasCopyImage(image, impl_->source->sourceCropRect);
    reportError(error, image.isNull() ? "Frame transformation failed." : "");
    return image;
}

std::shared_ptr<const ScreenshotAnimationSource>
decodeAnimationSource(const QByteArray& encoded, QString* error,
                      AnimationCancellationCheck cancelled,
                      AnimationAllocationCheck allocationCheck) {
    auto impl = AnimationDecoder::Impl::openEncoded(encoded, error, cancelled, allocationCheck);
    if (!impl)
        return {};
    auto source = std::make_shared<ScreenshotAnimationSource>();
    source->encoded = encoded;
    source->format = impl->info.format == SNOW_SHOT_IMAGE_CODEC_FORMAT_GIF ? QStringLiteral("gif")
                     : impl->info.format == SNOW_SHOT_IMAGE_CODEC_FORMAT_WEBP
                         ? QStringLiteral("webp")
                         : QStringLiteral("png");
    source->totalPlayCount = impl->info.total_play_count;
    source->frameDurationsNs.reserve(static_cast<qsizetype>(impl->info.frame_count));
    for (uint32_t index = 0; index < impl->info.frame_count; ++index) {
        const auto duration = snow_shot_animation_duration(impl->decoder, index);
        source->frameDurationsNs.push_back(duration > 0 ? duration : 100'000'000);
    }
    {
        AnimationDecoder decoder(std::move(impl));
        if (!decoder.advance(error, cancelled))
            return {};
        source->firstFrame = decoder.snapshot(error);
    }
    if (source->firstFrame.isNull())
        return {};
    source->canvasSize = source->firstFrame.size();
    // Release composition scratch before preparing the display raster. The
    // conservative admission allows two rasters even when compatible source
    // pixels can be shared directly with the display.
    const qint64 displayBytes =
        source->encoded.size() + 2 * source->firstFrame.sizeInBytes() + 8LL * 1024LL * 1024LL;
    if (animationCancelledCallback(&cancelled) != 0 ||
        admissionCallback(&allocationCheck, static_cast<uint64_t>(displayBytes)) == 0 ||
        animationCancelledCallback(&cancelled) != 0) {
        reportError(error, "Animation display preparation was not admitted.");
        return {};
    }
    const QColorSpace srgb(QColorSpace::SRgb);
    source->firstDisplayFrame =
        source->firstFrame.colorSpace().isValid() && source->firstFrame.colorSpace() != srgb
            ? snowCanvasColorConvertedImage(source->firstFrame, srgb,
                                            QImage::Format_ARGB32_Premultiplied)
            : snowCanvasConvertImage(source->firstFrame, QImage::Format_ARGB32_Premultiplied);
    if (source->firstDisplayFrame.isNull() || animationCancelledCallback(&cancelled) != 0) {
        reportError(error, "Animation display preparation failed.");
        return {};
    }
    if (source->firstDisplayFrame.devicePixelRatio() != 1.0)
        source->firstDisplayFrame.setDevicePixelRatio(1.0);
    return source;
}

QImage decodeAnimationFirstFrame(const QByteArray& encoded, QString* error,
                                 AnimationCancellationCheck cancelled,
                                 AnimationAllocationCheck allocationCheck) {
    auto impl =
        AnimationDecoder::Impl::openEncoded(encoded, error, cancelled, allocationCheck, true);
    if (!impl)
        return {};
    AnimationDecoder decoder(std::move(impl));
    if (!decoder.advance(error, cancelled))
        return {};
    auto image = decoder.snapshot(error);
    if (animationCancelledCallback(&cancelled) != 0) {
        reportError(error, "Animation decoding was cancelled.");
        return {};
    }
    return image;
}

std::unique_ptr<AnimationDecoder>
openAnimationDecoder(std::shared_ptr<const ScreenshotAnimationSource> source, QString* error,
                     AnimationAllocationCheck allocationCheck,
                     AnimationCancellationCheck cancelled) {
    if (error)
        error->clear();
    if (!source || source->encoded.isEmpty())
        return {};
    if (snow_shot_animation_abi_version() != SNOW_SHOT_ANIMATION_CODEC_ABI_VERSION) {
        reportError(error, "Animation codec ABI is incompatible.");
        return {};
    }
    auto impl = std::make_unique<AnimationDecoder::Impl>();
    impl->source = std::move(source);
    // The immutable initial snapshot already owns the parsed source profile.
    // Reopened playback sessions and later frames reuse that shared metadata.
    impl->frameColorSpace = impl->source->firstFrame.colorSpace();
    impl->allocationCheck = std::move(allocationCheck);
    std::array<char, kErrorCapacity> detail{};
    impl->decoder = snow_shot_animation_open(
        reinterpret_cast<const uint8_t*>(impl->source->encoded.constData()),
        static_cast<uint64_t>(impl->source->encoded.size()), 0, animationCancelledCallback,
        &cancelled, admissionCallback, &impl->allocationCheck, &impl->info, detail.data(),
        detail.size());
    if (!impl->decoder) {
        reportError(error, detail.data());
        return {};
    }
    impl->requiredBytes = static_cast<uint64_t>(impl->source->encoded.size()) +
                          uint64_t{impl->info.width} * impl->info.height * 16U + 8U * 1024U * 1024U;
    return std::unique_ptr<AnimationDecoder>(new AnimationDecoder(std::move(impl)));
}
} // namespace snow_shot::image_codec
