#pragma once

#include "snow_shot/presentation/screenshotimagerowsource.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <snow/image/codec.h>
#include <snow/image/format.h>

#include <optional>
#include <functional>
#include <memory>

struct ScreenshotAnimationSource;

class QIODevice;
class QImageReader;

namespace snow_shot::image_codec {

using AnimationCancellationCheck = std::function<bool()>;
using AnimationAllocationCheck = std::function<bool(qint64)>;

class AnimationDecoder final {
  public:
    ~AnimationDecoder();
    AnimationDecoder(AnimationDecoder&&) noexcept;
    AnimationDecoder& operator=(AnimationDecoder&&) noexcept;
    AnimationDecoder(const AnimationDecoder&) = delete;
    AnimationDecoder& operator=(const AnimationDecoder&) = delete;
    [[nodiscard]] bool advance(QString* error = nullptr, AnimationCancellationCheck cancelled = {});
    [[nodiscard]] QImage snapshot(QString* error = nullptr);
    [[nodiscard]] bool reset(QString* error = nullptr);
    [[nodiscard]] int frameIndex() const noexcept;

  private:
    class Impl;
    explicit AnimationDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend std::unique_ptr<AnimationDecoder>
    openAnimationDecoder(std::shared_ptr<const ScreenshotAnimationSource>, QString*,
                         AnimationAllocationCheck, AnimationCancellationCheck);
    friend std::shared_ptr<const ScreenshotAnimationSource>
    decodeAnimationSource(const QByteArray&, QString*, AnimationCancellationCheck,
                          AnimationAllocationCheck);
    friend QImage decodeAnimationFirstFrame(const QByteArray&, QString*, AnimationCancellationCheck,
                                            AnimationAllocationCheck);
};

// A null result and empty error means an ordinary static image. Recognized
// malformed animations return an error so callers do not silently flatten them.
[[nodiscard]] std::shared_ptr<const ScreenshotAnimationSource>
decodeAnimationSource(const QByteArray& encoded, QString* error = nullptr,
                      AnimationCancellationCheck cancelled = {},
                      AnimationAllocationCheck allocationCheck = {});
// Composes the same initial frame without retaining playback bytes/timing or a display raster.
// A null image and empty error denotes an ordinary static image.
[[nodiscard]] QImage decodeAnimationFirstFrame(const QByteArray& encoded, QString* error = nullptr,
                                               AnimationCancellationCheck cancelled = {},
                                               AnimationAllocationCheck allocationCheck = {});
[[nodiscard]] std::unique_ptr<AnimationDecoder>
openAnimationDecoder(std::shared_ptr<const ScreenshotAnimationSource> source,
                     QString* error = nullptr, AnimationAllocationCheck allocationCheck = {},
                     AnimationCancellationCheck cancelled = {});

enum class SkinDecodeError {
    None,
    UnsupportedFormat,
    UnreadableFile,
    InputTooLarge,
    InvalidImage,
    ResourceLimit,
};

struct SkinDecodeResult final {
    QImage image;
    SkinDecodeError error = SkinDecodeError::None;
};

// Decodes one static preview, including the first composited animation frame.
// Skin decoding has independent resource limits and never uses Qt image plugins.
[[nodiscard]] SkinDecodeResult decodeSkinFile(const QString& path);

struct EncodeResult final {
    quint64 bytesWritten = 0;
    snow::image::Format format = snow::image::Format::unknown;
    QSize size;
    quint32 emittedFrameCount = 0;
    snow::image::PixelRoundTrip roundTrip = snow::image::PixelRoundTrip::codec_artifact;
    bool finalizedAndFlushed = false;
};

[[nodiscard]] std::optional<snow::image::EncoderInfo> encoderInfo(snow::image::Format format);
// Dimension-only admission estimate, including Qt's bounded row-conversion cache. Does not scan
// alpha or include caller-owned encoded bytes; unknown routes use the codec's conservative plan.
[[nodiscard]] std::optional<qint64> encodeWorkingBytes(QSize size, snow::image::Format format,
                                                       const snow::image::EncodeOptions& options);

[[nodiscard]] bool encodeToDevice(const ScreenshotImageRowSource& source, QIODevice* device,
                                  snow::image::Format format,
                                  const snow::image::EncodeOptions& options,
                                  QString* error = nullptr, EncodeResult* result = nullptr);
[[nodiscard]] bool encodeToDevice(const QImage& image, QIODevice* device,
                                  snow::image::Format format,
                                  const snow::image::EncodeOptions& options,
                                  QString* error = nullptr, EncodeResult* result = nullptr);

[[nodiscard]] bool resizeToRgba8(const ScreenshotImageRowSource& source, const QSize& outputSize,
                                 uchar* destination, qsizetype destinationStride,
                                 qsizetype destinationSize, QString* error = nullptr);

struct ResizeResourceEstimate final {
    qint64 retainedBytes = 0;
    qint64 scratchBytes = 0;
};
// Source conversion caches and source/output rasters are budgeted by the caller. Retained bytes
// cover immutable filter tables and the bounded output tile cache; scratch includes codec headroom.
[[nodiscard]] std::optional<ResizeResourceEstimate> resizeResources(QSize source, QSize output);
// The synchronous full resize uses the same tables/scratch without a lazy output tile cache.
[[nodiscard]] std::optional<qint64> resizeWorkingBytes(QSize source, QSize output);
[[nodiscard]] ScreenshotImageRowSource resizedRowSource(ScreenshotImageRowSource source,
                                                        QSize output, QString* error = nullptr);

[[nodiscard]] QByteArray encodePng(const QImage& image, int compressionLevel = 0);
[[nodiscard]] QByteArray encodePng(const ScreenshotImageRowSource& source,
                                   int compressionLevel = 0);
[[nodiscard]] ScreenshotImageRowSource srgbRowSource(const QImage& image);
[[nodiscard]] QByteArray encodeWebp(const QImage& image, int quality = 75);
// Decoded RGB images retain their source color space. Convert to sRGB at the
// rendering/encoding boundary; merely assigning an sRGB tag changes their meaning.
// Decode/inspect functions accept Format::unknown to detect content without a format constraint.
// Explicit formats remain strict, e.g. when validating an application's own stored PNG payload.
[[nodiscard]] QImage decode(const QByteArray& encoded, snow::image::Format expectedFormat,
                            const char* nameHint);
[[nodiscard]] QImage decodeBgra(const QByteArray& encoded, snow::image::Format expectedFormat);
[[nodiscard]] QImage decodeFirstFrame(const QByteArray& encoded,
                                      snow::image::Format expectedFormat);
// Preserves natural dimensions while requesting native reduction from supported codecs.
[[nodiscard]] QImage decodeThumbnail(const QByteArray& encoded, snow::image::Format expectedFormat,
                                     QSize targetSize, QSize* naturalSize = nullptr,
                                     AnimationCancellationCheck cancelled = {});
// Retains Qt's decoding, metadata and transformation behavior while putting
// the returned raster in managed storage. Compatible readers decode in place.
[[nodiscard]] QImage readManagedImage(QImageReader& reader);
[[nodiscard]] QImage decodeIconFile(const QString& path, uint32_t preferredExtent);
[[nodiscard]] QSize inspectSize(const QByteArray& encoded, snow::image::Format expectedFormat);
[[nodiscard]] QSize inspectCanvasSize(const QByteArray& encoded,
                                      snow::image::Format expectedFormat);
[[nodiscard]] QImage decodeFile(const QString& path, snow::image::Format expectedFormat);
[[nodiscard]] QImage decodeFileBgra(const QString& path, snow::image::Format expectedFormat);
[[nodiscard]] bool inspectFile(const QString& path, snow::image::Format expectedFormat,
                               const QSize& expectedSize);

} // namespace snow_shot::image_codec
