#include "editing/worker_core.h"

#include "editing/raster_package.h"
#include "editing/worker_protocol.h"

#include <snow/image/io.h>
#include <snow/image/processing.h>
#include <snow/image/raster_conversion.h>
#include <snow/image/service.h>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>

namespace snow::image_viewer::worker_core {
namespace {

std::filesystem::path nativePath(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

QString statusText(const snow::image::Status& status) {
    const QString message = QString::fromStdString(status.message).trimmed();
    return message.isEmpty() ? QStringLiteral("The image operation failed.") : message;
}

snow::image::Result<snow::image::Document> prepareColorForExport(snow::image::Document document,
                                                                 snow::image::Format format,
                                                                 std::stop_token stop) {
    const bool pngNativeSamples = std::all_of(
        document.frames.begin(), document.frames.end(), [](const snow::image::Frame& frame) {
            const snow::image::PixelFormat& pixel = frame.image.format();
            return pixel.sample_type == snow::image::SampleType::unsigned_integer &&
                   (pixel.bits_per_channel == 8 || pixel.bits_per_channel == 16) &&
                   pixel.channels != snow::image::ChannelLayout::cmyk &&
                   pixel.channels != snow::image::ChannelLayout::indexed;
        });
    if (format == snow::image::Format::png && pngNativeSamples)
        return document;
    if (format == snow::image::Format::png || format == snow::image::Format::jpeg ||
        format == snow::image::Format::webp || format == snow::image::Format::heif) {
        return snow::image::convert_to_sdr_srgb(document, true, stop);
    }
    if (format == snow::image::Format::avif) {
        const bool hdr = document.color.dynamic_range == snow::image::DynamicRange::high ||
                         std::any_of(document.frames.begin(), document.frames.end(),
                                     [](const snow::image::Frame& frame) {
                                         return frame.image.format().sample_type ==
                                                snow::image::SampleType::floating_point;
                                     });
        if (hdr)
            return snow::image::convert_to_hdr_rec2020_pq16(document, stop);
    }
    return document;
}

bool sameColor(const snow::image::ColorEncoding& left, const snow::image::ColorEncoding& right) {
    return left.primaries == right.primaries && left.transfer == right.transfer &&
           left.dynamic_range == right.dynamic_range && left.icc_profile == right.icc_profile &&
           left.source_peak_nits == right.source_peak_nits &&
           left.diffuse_white_nits == right.diffuse_white_nits &&
           left.mastering_primaries_and_white == right.mastering_primaries_and_white &&
           left.max_content_light_level == right.max_content_light_level &&
           left.max_frame_average_light_level == right.max_frame_average_light_level;
}

bool previewEquivalent(const snow::image::Document& base, const snow::image::Document& prepared) {
    if (base.canvas_width != prepared.canvas_width ||
        base.canvas_height != prepared.canvas_height || base.loop_count != prepared.loop_count ||
        base.frames.size() != prepared.frames.size() || !sameColor(base.color, prepared.color))
        return false;
    for (std::size_t index = 0; index < base.frames.size(); ++index) {
        const auto& left = base.frames[index];
        const auto& right = prepared.frames[index];
        if (left.x != right.x || left.y != right.y || left.duration != right.duration ||
            left.blend != right.blend || left.disposal != right.disposal ||
            left.image.width() != right.image.width() ||
            left.image.height() != right.image.height() ||
            left.image.format() != right.image.format() ||
            left.image.row_stride() != right.image.row_stride() ||
            left.image.storage().owner() != right.image.storage().owner() ||
            left.image.pixels().data() != right.image.pixels().data() ||
            left.image.pixels().size() != right.image.pixels().size() ||
            !sameColor(left.color, right.color))
            return false;
    }
    return true;
}

bool descriptorHasAlpha(const snow::image::DocumentDescriptor& descriptor) {
    return std::any_of(descriptor.frames.begin(), descriptor.frames.end(),
                       [](const snow::image::RasterFrameDescriptor& frame) {
                           return std::any_of(
                               frame.layout.planes.begin(), frame.layout.planes.end(),
                               [](const snow::image::PlaneDescriptor& plane) {
                                   return plane.semantic == snow::image::PlaneSemantic::alpha ||
                                          plane.format.alpha != snow::image::AlphaMode::none;
                               });
                       });
}

bool jpegPackedCompatible(const snow::image::PixelFormat& pixel) {
    if (pixel.sample_type != snow::image::SampleType::unsigned_integer ||
        pixel.bits_per_channel != 8)
        return false;
    return pixel.channels == snow::image::ChannelLayout::gray ||
           pixel.channels == snow::image::ChannelLayout::rgb ||
           pixel.channels == snow::image::ChannelLayout::rgba ||
           pixel.channels == snow::image::ChannelLayout::bgr ||
           pixel.channels == snow::image::ChannelLayout::bgra;
}

snow::image::Result<snow::image::Document>
materializePackedRaster(const snow::image::RasterSource& source, std::stop_token stop) {
    const snow::image::DocumentDescriptor& descriptor = source.descriptor();
    if (descriptor.frames.size() != 1)
        return snow::image::Status::error(snow::image::ErrorCode::unsupported_feature,
                                          "The native raster fallback requires one frame.");
    const snow::image::RasterFrameDescriptor& frame = descriptor.frames.front();
    auto allocated =
        snow::image::MutableImage::allocate(frame.width, frame.height, snow::image::kRgba8);
    if (!allocated)
        return allocated.error();
    snow::image::MutableImage pixels = std::move(allocated).value();
    snow::image::MutablePlaneView destination{frame.width, frame.height, snow::image::kRgba8,
                                              pixels.row_stride(), pixels.pixels()};
    auto converted = snow::image::read_rgba8_region(source, 0, {0, 0, frame.width, frame.height},
                                                    destination, stop);
    if (!converted)
        return converted.error();
    snow::image::Document document;
    document.format = descriptor.format;
    document.canvas_width = descriptor.canvas_width;
    document.canvas_height = descriptor.canvas_height;
    document.loop_count = descriptor.loop_count;
    document.metadata = descriptor.metadata;
    document.color = descriptor.color;
    snow::image::Frame packed;
    packed.image = std::move(pixels).freeze();
    packed.x = frame.x;
    packed.y = frame.y;
    packed.duration = frame.duration;
    packed.blend = frame.blend;
    packed.disposal = frame.disposal;
    packed.metadata = frame.metadata;
    packed.color = frame.color;
    packed.cursor_hotspot = frame.cursor_hotspot;
    document.frames.push_back(std::move(packed));
    return document;
}

bool validateNativeReceipt(const snow::image::EncodedArtifactReceipt& receipt,
                           const snow::image::DocumentDescriptor& descriptor,
                           const EditExportSettings& settings, QString* error) {
    const auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!receipt.encoder_finalized_and_sink_flushed || receipt.format != settings.format ||
        receipt.document_kind != snow::image::DocumentKind::raster ||
        receipt.canvas_width != descriptor.canvas_width ||
        receipt.canvas_height != descriptor.canvas_height || receipt.emitted_frame_count != 1 ||
        receipt.emitted_frame_extents.size() != 1)
        return fail(QStringLiteral("The native raster receipt is invalid."));
    const auto& frame = descriptor.frames.front();
    const auto& extent = receipt.emitted_frame_extents.front();
    if (extent.x != 0 || extent.y != 0 || extent.width != frame.width ||
        extent.height != frame.height)
        return fail(QStringLiteral("The native receipt does not match its raster."));
    if (settings.format == snow::image::Format::jpeg) {
        const bool grayscale = frame.layout.color_model == snow::image::ColorModel::gray;
        const auto expected =
            snow::image::resolve_jpeg_chroma_subsampling(settings.encode, grayscale);
        if (!expected || receipt.jpeg_chroma_subsampling != expected.value())
            return fail(
                QStringLiteral("The native JPEG receipt sampling does not match its raster."));
    } else if (receipt.jpeg_chroma_subsampling) {
        return fail(QStringLiteral("A native non-JPEG receipt contains JPEG sampling metadata."));
    }
    return true;
}

snow::image::Result<snow::image::AlphaContent>
classifyDocumentAlpha(const snow::image::Document& document, std::stop_token stop) {
    for (const snow::image::Frame& frame : document.frames) {
        const auto classified = snow::image::classify_alpha(frame.image, stop);
        if (!classified)
            return classified.error();
        if (classified.value() == snow::image::AlphaContent::non_opaque)
            return snow::image::AlphaContent::non_opaque;
    }
    return snow::image::AlphaContent::opaque;
}

bool validateReceipt(const snow::image::EncodedArtifactReceipt& receipt,
                     const snow::image::Document& document, const EditExportSettings& settings,
                     QString* error) {
    const auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!receipt.encoder_finalized_and_sink_flushed || receipt.format != settings.format ||
        receipt.document_kind != snow::image::DocumentKind::raster ||
        receipt.emitted_frame_count != receipt.emitted_frame_extents.size()) {
        return fail(QStringLiteral("The encoder returned an invalid artifact receipt."));
    }
    const bool firstFrame =
        animationPolicyForFormat(settings.format) == snow::image::AnimationPolicy::first_frame;
    if (document.frames.empty())
        return fail(QStringLiteral("The prepared raster has no frames."));
    const std::size_t expectedCount = firstFrame ? 1U : document.frames.size();
    if (receipt.emitted_frame_extents.size() != expectedCount)
        return fail(
            QStringLiteral("The artifact receipt frame count does not match the export policy."));

    std::uint32_t expectedCanvasWidth = document.canvas_width;
    std::uint32_t expectedCanvasHeight = document.canvas_height;
    if (settings.format == snow::image::Format::ico ||
        settings.format == snow::image::Format::cur) {
        expectedCanvasWidth = 0;
        expectedCanvasHeight = 0;
        for (const auto& frame : document.frames) {
            expectedCanvasWidth = std::max(expectedCanvasWidth, frame.image.width());
            expectedCanvasHeight = std::max(expectedCanvasHeight, frame.image.height());
        }
    } else if (firstFrame) {
        expectedCanvasWidth = document.frames.front().image.width();
        expectedCanvasHeight = document.frames.front().image.height();
    }
    if (receipt.canvas_width != expectedCanvasWidth ||
        receipt.canvas_height != expectedCanvasHeight)
        return fail(
            QStringLiteral("The artifact receipt canvas does not match the prepared raster."));

    if (settings.format == snow::image::Format::jpeg) {
        const bool grayscale =
            document.frames.front().image.format().channels == snow::image::ChannelLayout::gray;
        auto expectedSampling =
            snow::image::resolve_jpeg_chroma_subsampling(settings.encode, grayscale);
        if (!expectedSampling || receipt.jpeg_chroma_subsampling != expectedSampling.value()) {
            return fail(QStringLiteral(
                "The JPEG artifact receipt sampling does not match the export settings."));
        }
    } else if (receipt.jpeg_chroma_subsampling) {
        return fail(QStringLiteral("A non-JPEG artifact receipt contains JPEG sampling metadata."));
    }

    for (std::size_t index = 0; index < expectedCount; ++index) {
        const auto& frame = document.frames[firstFrame ? 0U : index];
        const auto& extent = receipt.emitted_frame_extents[index];
        const bool resetOrigin = firstFrame || settings.format == snow::image::Format::ico ||
                                 settings.format == snow::image::Format::cur;
        if (extent.x != (resetOrigin ? 0U : frame.x) || extent.y != (resetOrigin ? 0U : frame.y) ||
            extent.width != frame.image.width() || extent.height != frame.image.height()) {
            return fail(QStringLiteral(
                "The artifact receipt frame extent does not match the prepared raster."));
        }
    }
    return true;
}

class DocumentRasterSource final : public snow::image::RasterSource {
  public:
    static snow::image::Result<DocumentRasterSource> create(const snow::image::Document& document) {
        auto descriptor = snow::image::describe_document(document);
        if (!descriptor)
            return descriptor.error();
        return DocumentRasterSource(document, std::move(descriptor).value());
    }

    const snow::image::DocumentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    snow::image::RasterAccess access() const noexcept override {
        return snow::image::RasterAccess::sequential_rows | snow::image::RasterAccess::random_rows |
               snow::image::RasterAccess::mapped_planes |
               snow::image::RasterAccess::concurrent_reads;
    }

    snow::image::Result<void> read_rows(std::uint32_t frameIndex, std::uint32_t planeIndex,
                                        std::uint32_t firstRow, std::uint32_t rowCount,
                                        std::size_t destinationStride,
                                        std::span<std::byte> destination,
                                        std::stop_token stop) const override {
        if (planeIndex != 0 || frameIndex >= document_->frames.size()) {
            return snow::image::Status::error(snow::image::ErrorCode::invalid_argument,
                                              "The document raster row request is invalid.");
        }
        const snow::image::Image& image = document_->frames[frameIndex].image;
        const auto bytesPerPixel = image.format().bytes_per_pixel();
        if (!bytesPerPixel || firstRow > image.height() || rowCount == 0 ||
            rowCount > image.height() - firstRow ||
            image.width() > std::numeric_limits<std::size_t>::max() / bytesPerPixel.value()) {
            return snow::image::Status::error(snow::image::ErrorCode::invalid_argument,
                                              "The document raster row range is invalid.");
        }
        const std::size_t rowBytes =
            static_cast<std::size_t>(image.width()) * bytesPerPixel.value();
        if (destinationStride < rowBytes ||
            (rowCount > 1 &&
             destinationStride >
                 (std::numeric_limits<std::size_t>::max() - rowBytes) / (rowCount - 1U)) ||
            destination.size() < destinationStride * (rowCount - 1U) + rowBytes) {
            return snow::image::Status::error(snow::image::ErrorCode::invalid_argument,
                                              "The document raster destination is too small.");
        }
        for (std::uint32_t row = 0; row < rowCount; ++row) {
            if (stop.stop_requested()) {
                return snow::image::Status::error(snow::image::ErrorCode::cancelled,
                                                  "The document raster read was cancelled.");
            }
            std::memcpy(destination.data() + static_cast<std::size_t>(row) * destinationStride,
                        image.pixels().data() +
                            static_cast<std::size_t>(firstRow + row) * image.row_stride(),
                        rowBytes);
        }
        return {};
    }

    snow::image::Result<snow::image::MappedPlane>
    map_plane(std::uint32_t frameIndex, std::uint32_t planeIndex) const override {
        if (planeIndex != 0 || frameIndex >= document_->frames.size()) {
            return snow::image::Status::error(snow::image::ErrorCode::invalid_argument,
                                              "The document raster mapping request is invalid.");
        }
        const snow::image::Image& image = document_->frames[frameIndex].image;
        return snow::image::MappedPlane{image.storage().owner(), image.pixels(),
                                        image.row_stride()};
    }

  private:
    DocumentRasterSource(const snow::image::Document& document,
                         snow::image::DocumentDescriptor descriptor)
        : document_(&document), descriptor_(std::move(descriptor)) {}

    const snow::image::Document* document_ = nullptr;
    snow::image::DocumentDescriptor descriptor_;
};

} // namespace

snow::image::Result<PreparationResult>
prepareForExport(snow::image::Document document, const EditExportSettings& settings,
                 const snow::image::EncoderInfo& encoder, QString* warning, std::stop_token stop,
                 std::optional<snow::image::AlphaContent> verifiedAlphaContent) {
    const snow::image::Document base = document;
    if (verifiedAlphaContent && *verifiedAlphaContent == snow::image::AlphaContent::non_opaque) {
        snow::image::Result<snow::image::DocumentDescriptor> descriptor =
            snow::image::describe_document(document);
        if (!descriptor)
            return descriptor.error();
        if (!descriptorHasAlpha(descriptor.value())) {
            return snow::image::Status::error(
                snow::image::ErrorCode::invalid_argument,
                "Verified non-opaque alpha metadata contradicts the prepared raster.");
        }
    }
    if (document.frames.size() > 1 &&
        !snow::image::has_feature(encoder.features, snow::image::EncoderFeature::animation)) {
        return snow::image::Status::error(
            snow::image::ErrorCode::invalid_argument,
            "The base raster animation policy does not match the selected encoder.");
    }
    Q_UNUSED(warning);
    std::optional<snow::image::AlphaContent> alphaContent = verifiedAlphaContent;
    std::chrono::nanoseconds alphaDuration{0};
    const auto classify = [&]() -> snow::image::Result<void> {
        if (alphaContent)
            return {};
        const auto alphaStart = std::chrono::steady_clock::now();
        const auto classified = classifyDocumentAlpha(document, stop);
        if (!classified)
            return classified.error();
        alphaContent = classified.value();
        alphaDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - alphaStart);
        return {};
    };

    if (settings.format == snow::image::Format::jpeg && !settings.reducePalette) {
        auto classified = classify();
        if (!classified)
            return classified.error();
        snow::image::SdrConversionOptions options;
        options.verified_alpha_content = *alphaContent;
        if (*alphaContent == snow::image::AlphaContent::non_opaque) {
            options.background = std::array<std::uint8_t, 3>{0, 0, 0};
            options.output_format = snow::image::kRgb8;
        } else if (!document.frames.empty() &&
                   jpegPackedCompatible(document.frames.front().image.format()) &&
                   std::all_of(document.frames.begin(), document.frames.end(),
                               [&](const snow::image::Frame& frame) {
                                   return frame.image.format() ==
                                          document.frames.front().image.format();
                               })) {
            options.output_format = document.frames.front().image.format();
        } else {
            options.output_format = snow::image::kRgb8;
        }
        auto converted = snow::image::convert_to_sdr_srgb(document, options, stop);
        if (!converted)
            return converted.error();
        document = std::move(converted).value();
        if (options.background)
            alphaContent = snow::image::AlphaContent::opaque;
    } else {
        auto converted = prepareColorForExport(std::move(document), settings.format, stop);
        if (!converted)
            return converted.error();
        document = std::move(converted).value();
    }
    if (settings.reducePalette) {
        snow::image::TransformOptions options;
        options.palette =
            snow::image::PaletteOptions{static_cast<std::uint16_t>(settings.paletteColors),
                                        static_cast<float>(settings.ditheringPercent) / 100.0F};
        auto reduced = snow::image::transform(document, options, stop);
        if (!reduced)
            return reduced.error();
        document = std::move(reduced).value();
        alphaContent.reset();
    }
    if (!settings.encode.preserve_metadata) {
        document.metadata = {};
        for (auto& frame : document.frames)
            frame.metadata = {};
    }
    const bool equivalent = previewEquivalent(base, document);
    auto classified = classify();
    if (!classified)
        return classified.error();

    if (!snow::image::has_feature(encoder.features, snow::image::EncoderFeature::alpha) &&
        *alphaContent == snow::image::AlphaContent::non_opaque) {
        snow::image::SdrConversionOptions options;
        options.verified_alpha_content = *alphaContent;
        options.background = std::array<std::uint8_t, 3>{0, 0, 0};
        options.output_format = snow::image::kRgb8;
        auto composited = snow::image::convert_to_sdr_srgb(document, options, stop);
        if (!composited)
            return composited.error();
        document = std::move(composited).value();
        alphaContent = snow::image::AlphaContent::opaque;
    }
    return PreparationResult{std::move(document), equivalent, *alphaContent, alphaDuration};
}

QJsonObject executeEncodeJob(const QJsonObject& job, const EditExportSettings& settings,
                             const ArtifactReadyCallback& artifactReady, std::stop_token stop,
                             bool* artifactPublished) {
    const QString artifactPath = job.value(QStringLiteral("artifactPath")).toString();
    const QString previewPath = job.value(QStringLiteral("previewPath")).toString();
    QJsonObject result{{QStringLiteral("success"), false}};
    QString error;
    QElapsedTimer total;
    QElapsedTimer stage;
    total.start();
    stage.start();
    std::shared_ptr<MappedRasterPackage> base;
    const QJsonObject transport = job.value(QStringLiteral("baseRaster")).toObject();
    const QString transportKind = transport.value(QStringLiteral("kind")).toString();
    if (transportKind == QStringLiteral("verified_file")) {
        base =
            MappedRasterPackage::open(transport.value(QStringLiteral("path")).toString(), &error);
    } else if (transportKind == QStringLiteral("shared_memory")) {
        bool sizeValid = false;
        const std::uint64_t size =
            transport.value(QStringLiteral("size")).toString().toULongLong(&sizeValid);
        std::array<std::byte, 16> nonce{};
        const QJsonArray nonceJson = transport.value(QStringLiteral("nonce")).toArray();
        if (nonceJson.size() != static_cast<qsizetype>(nonce.size())) {
            error = QStringLiteral("The shared raster nonce is malformed.");
        } else {
            bool nonceValid = true;
            for (qsizetype index = 0; index < nonceJson.size(); ++index) {
                const int value = nonceJson.at(index).toInt(-1);
                if (value < 0 || value > 255) {
                    nonceValid = false;
                    break;
                }
                nonce[static_cast<std::size_t>(index)] = static_cast<std::byte>(value);
            }
            if (!nonceValid)
                error = QStringLiteral("The shared raster nonce is malformed.");
            else if (!sizeValid || size == 0)
                error = QStringLiteral("The shared raster size is malformed.");
            else {
                base = MappedRasterPackage::openShared(
                    transport.value(QStringLiteral("key")).toString(), size, nonce, &error);
            }
        }
    } else {
        error = QStringLiteral("The base raster transport is malformed.");
    }
    if (!base) {
        result.insert(QStringLiteral("error"), error);
        if (transportKind == QStringLiteral("shared_memory") &&
            error.startsWith(QStringLiteral("Could not attach shared raster storage:"))) {
            result.insert(QStringLiteral("retriableSharedMemoryAttach"), true);
        }
        return result;
    }
    result.insert(QStringLiteral("openBaseNs"), QString::number(stage.nsecsElapsed()));
    stage.restart();
    snow::image::Service service;
    const snow::image::EncoderInfo* encoder = service.encoder_info(settings.format);
    if (!encoder) {
        result.insert(QStringLiteral("error"),
                      QStringLiteral("The selected encoder is unavailable."));
        return result;
    }
    QString warning;
    if (job.value(QStringLiteral("sourceFrameCount")).toInt(1) > 1 &&
        animationPolicyForFormat(settings.format) == snow::image::AnimationPolicy::first_frame) {
        warning = QStringLiteral("Only the first frame will be exported.");
    }
    std::optional<snow::image::AlphaContent> verifiedAlphaContent;
    const QJsonValue verifiedAlphaValue = job.value(QStringLiteral("verifiedAlphaContent"));
    if (!verifiedAlphaValue.isUndefined()) {
        const int value = verifiedAlphaValue.toInt(-1);
        if (value < static_cast<int>(snow::image::AlphaContent::opaque) ||
            value > static_cast<int>(snow::image::AlphaContent::non_opaque)) {
            result.insert(
                QStringLiteral("error"),
                QStringLiteral("The encode job contains invalid verified alpha metadata."));
            return result;
        }
        verifiedAlphaContent = static_cast<snow::image::AlphaContent>(value);
    }
    const std::optional<snow::image::AlphaContent> packageAlpha = base->verifiedAlphaContent();
    if (verifiedAlphaContent && packageAlpha && *verifiedAlphaContent != *packageAlpha) {
        result.insert(
            QStringLiteral("error"),
            QStringLiteral("The encode job alpha metadata does not match the raster package."));
        return result;
    }
    if (!verifiedAlphaContent)
        verifiedAlphaContent = packageAlpha;
    snow::image::Result<snow::image::EncodeOptions> normalized =
        snow::image::normalize_encode_options(*encoder, settings.encode);
    if (!normalized) {
        result.insert(QStringLiteral("error"), statusText(normalized.error()));
        return result;
    }
    snow::image::Result<snow::image::RasterEncodeRoute> selectedRoute =
        service.raster_encode_route(base->source().descriptor(), normalized.value());
    if (!selectedRoute) {
        result.insert(QStringLiteral("error"), statusText(selectedRoute.error()));
        return result;
    }
    const bool directNative =
        !settings.reducePalette && selectedRoute.value() == snow::image::RasterEncodeRoute::native;
    snow::image::AlphaContent directAlpha = snow::image::AlphaContent::opaque;
    if (directNative) {
        directAlpha = verifiedAlphaContent.value_or(descriptorHasAlpha(base->source().descriptor())
                                                        ? snow::image::AlphaContent::non_opaque
                                                        : snow::image::AlphaContent::opaque);
    }
    std::optional<PreparationResult> prepared;
    std::shared_ptr<MappedRasterPackage> preparedPackage;
    const auto cachedPreparation = job.value(QStringLiteral("preparedRaster")).toObject();
    if (!directNative && !cachedPreparation.isEmpty()) {
        preparedPackage = MappedRasterPackage::open(
            cachedPreparation.value(QStringLiteral("path")).toString(), &error);
        const auto* document = preparedPackage ? preparedPackage->document() : nullptr;
        const auto alpha = preparedPackage ? preparedPackage->verifiedAlphaContent() : std::nullopt;
        if (!document || !alpha || document->frames.empty() ||
            document->frames.size() != base->source().descriptor().frames.size() ||
            document->frames.front().image.width() != static_cast<std::uint32_t>(settings.width) ||
            document->frames.front().image.height() !=
                static_cast<std::uint32_t>(settings.height)) {
            result.insert(QStringLiteral("error"),
                          error.isEmpty()
                              ? QStringLiteral("The prepared preview document is unavailable.")
                              : error);
            return result;
        }
        prepared = PreparationResult{
            *document, job.value(QStringLiteral("preparedPreviewEquivalentToBase")).toBool(),
            *alpha, std::chrono::nanoseconds{0}};
        result.insert(QStringLiteral("preparationCacheHit"), true);
    } else if (!directNative) {
        std::optional<snow::image::Document> materialized;
        const snow::image::Document* packed = base->document();
        if (!packed) {
            auto fallback = materializePackedRaster(base->source(), stop);
            if (!fallback) {
                result.insert(QStringLiteral("error"), statusText(fallback.error()));
                return result;
            }
            materialized = std::move(fallback).value();
            packed = &*materialized;
        }
        auto preparedResult =
            prepareForExport(*packed, settings, *encoder, &warning, stop, verifiedAlphaContent);
        if (!preparedResult) {
            result.insert(QStringLiteral("error"), statusText(preparedResult.error()));
            return result;
        }
        prepared = std::move(preparedResult).value();
        const QString preparedPath = job.value(QStringLiteral("preparedPath")).toString();
        bool limitValid = false;
        const auto limit =
            job.value(QStringLiteral("preparedCacheLimit")).toString().toULongLong(&limitValid);
        std::uint64_t pixels = 0;
        bool withinLimit = limitValid;
        for (const auto& frame : prepared->document.frames) {
            if (frame.image.pixels().size() > limit - pixels) {
                withinLimit = false;
                break;
            }
            pixels += frame.image.pixels().size();
        }
        if (!preparedPath.isEmpty() && withinLimit) {
            const QString partial = preparedPath + QStringLiteral(".partial");
            preparedPackage = MappedRasterPackage::create(partial, prepared->document, &error, {},
                                                          prepared->alphaContent);
            if (preparedPackage && preparedPackage->mappedBytes() <= limit) {
                preparedPackage.reset();
                if (QFile::rename(partial, preparedPath)) {
                    preparedPackage = MappedRasterPackage::open(preparedPath, &error);
                    if (preparedPackage && preparedPackage->document()) {
                        prepared->document = *preparedPackage->document();
                        result.insert(QStringLiteral("preparedPath"), preparedPath);
                        result.insert(QStringLiteral("preparedPreviewEquivalentToBase"),
                                      prepared->previewEquivalentToBase);
                    } else {
                        preparedPackage.reset();
                    }
                }
            } else {
                preparedPackage.reset();
            }
            QFile::remove(partial);
            if (!preparedPackage)
                QFile::remove(preparedPath);
            // A cache publication failure does not invalidate the prepared export.
            error.clear();
        }
    }
    result.insert(QStringLiteral("alphaContent"),
                  static_cast<int>(directNative ? directAlpha : prepared->alphaContent));
    result.insert(
        QStringLiteral("alphaClassificationNs"),
        QString::number(directNative ? 0 : prepared->alphaClassificationDuration.count()));
    result.insert(QStringLiteral("prepareNs"), QString::number(stage.nsecsElapsed()));
    stage.restart();

    const QString partialArtifact = artifactPath + QStringLiteral(".partial");
    auto output = snow::image::file_output(nativePath(partialArtifact));
    if (!output) {
        result.insert(QStringLiteral("error"), statusText(output.error()));
        return result;
    }
    snow::image::EncodeOptions options = std::move(normalized).value();
    options.format = settings.format;
    options.verified_alpha_content = directNative ? directAlpha : prepared->alphaContent;
    auto encoded = [&]() -> snow::image::Result<snow::image::EncodeResult> {
        if (directNative)
            return service.encode(base->source(), output.value(), options, stop);
        auto preparedSource = DocumentRasterSource::create(prepared->document);
        if (!preparedSource)
            return preparedSource.error();
        return service.encode(preparedSource.value(), output.value(), options, stop);
    }();
    output.value().sink.reset();
    if (!encoded) {
        QFile::remove(partialArtifact);
        result.insert(QStringLiteral("error"), statusText(encoded.error()));
        return result;
    }
    QString receiptError;
    const bool validReceipt =
        directNative
            ? validateNativeReceipt(encoded.value().receipt, base->source().descriptor(), settings,
                                    &receiptError)
            : validateReceipt(encoded.value().receipt, prepared->document, settings, &receiptError);
    if (!validReceipt) {
        QFile::remove(partialArtifact);
        result.insert(QStringLiteral("error"), receiptError);
        return result;
    }
    const QFileInfo partialInfo(partialArtifact);
    if (!partialInfo.isFile() || partialInfo.size() <= 0 ||
        static_cast<std::uint64_t>(partialInfo.size()) != encoded.value().bytes_written) {
        QFile::remove(partialArtifact);
        result.insert(
            QStringLiteral("error"),
            QStringLiteral("The encoded partial artifact size does not match the receipt."));
        return result;
    }
    if (!QFile::rename(partialArtifact, artifactPath)) {
        QFile::remove(partialArtifact);
        result.insert(QStringLiteral("error"),
                      QStringLiteral("The worker could not publish the encoded artifact."));
        return result;
    }
    QFile::setPermissions(artifactPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    result.insert(QStringLiteral("encodeNs"), QString::number(stage.nsecsElapsed()));
    if (directNative) {
        result.insert(QStringLiteral("directNativeEncodeNs"),
                      result.value(QStringLiteral("encodeNs")));
    }
    stage.restart();

    auto artifactInput = snow::image::file_input(nativePath(artifactPath));
    if (!artifactInput) {
        result.insert(QStringLiteral("error"), statusText(artifactInput.error()));
        return result;
    }
    const QString partialPreview = previewPath + QStringLiteral(".partial");
    const snow::image::Document* preparedDocument = prepared ? &prepared->document : nullptr;
    result.insert(QStringLiteral("validateReceiptNs"), QString::number(stage.nsecsElapsed()));
    stage.restart();

    const bool pixelExact = encoded.value().round_trip == snow::image::PixelRoundTrip::exact;
    const bool basePreview = pixelExact && (!prepared || prepared->previewEquivalentToBase);
    const QString previewKind = basePreview  ? QStringLiteral("base")
                                : pixelExact ? QStringLiteral("prepared")
                                             : QStringLiteral("codec");
    QJsonObject publication{
        {QStringLiteral("artifactPath"), artifactPath},
        {QStringLiteral("artifactBytes"), QString::number(encoded.value().bytes_written)},
        {QStringLiteral("warning"), warning},
        {QStringLiteral("previewKind"), previewKind},
        {QStringLiteral("pixelExact"), pixelExact},
        {QStringLiteral("receipt"), worker_protocol::receiptToJson(encoded.value().receipt)},
        {QStringLiteral("alphaContent"), result.value(QStringLiteral("alphaContent"))},
        {QStringLiteral("alphaClassificationNs"),
         result.value(QStringLiteral("alphaClassificationNs"))},
        {QStringLiteral("prepareNs"), result.value(QStringLiteral("prepareNs"))},
        {QStringLiteral("encodeNs"), result.value(QStringLiteral("encodeNs"))},
        {QStringLiteral("validateReceiptNs"), result.value(QStringLiteral("validateReceiptNs"))}};
    if (result.contains(QStringLiteral("preparedPath"))) {
        publication.insert(QStringLiteral("preparedPath"),
                           result.value(QStringLiteral("preparedPath")));
        publication.insert(QStringLiteral("preparedPreviewEquivalentToBase"),
                           result.value(QStringLiteral("preparedPreviewEquivalentToBase")));
    }
    if (result.value(QStringLiteral("preparationCacheHit")).toBool())
        publication.insert(QStringLiteral("preparationCacheHit"), true);
    const bool previewUsesPreparedRaster =
        previewKind == QStringLiteral("prepared") && preparedPackage;
    if (previewUsesPreparedRaster)
        publication.insert(QStringLiteral("previewUsesPreparedRaster"), true);
    if (directNative) {
        publication.insert(QStringLiteral("directNativeEncodeNs"),
                           result.value(QStringLiteral("directNativeEncodeNs")));
    }
    artifactReady(std::move(publication));
    if (artifactPublished)
        *artifactPublished = true;
    if (job.value(QStringLiteral("testMode")).toString() ==
        QStringLiteral("block-preview-cooperative")) {
        while (!stop.stop_requested())
            QThread::msleep(1);
        result.insert(QStringLiteral("error"), QStringLiteral("The preview was cancelled."));
        return result;
    }
    if (job.value(QStringLiteral("testMode")).toString() == QStringLiteral("preview-failure")) {
        result.insert(QStringLiteral("error"), QStringLiteral("Simulated exact preview failure."));
        return result;
    }
    if (previewKind == QStringLiteral("prepared") && !previewUsesPreparedRaster) {
        if (!prepared || preparedDocument == nullptr) {
            result.insert(QStringLiteral("error"),
                          QStringLiteral("The prepared preview document is unavailable."));
            return result;
        }
        auto preview = MappedRasterPackage::create(partialPreview, *preparedDocument, &error, {},
                                                   prepared->alphaContent);
        if (!preview) {
            result.insert(QStringLiteral("error"), error);
            return result;
        }
        preview.reset();
    } else if (previewKind == QStringLiteral("codec")) {
        base.reset();
        if (prepared)
            prepared->document = {};
        preparedPackage.reset();
        snow::image::Result<void> streamed;
        if (settings.format == snow::image::Format::jpeg) {
            snow::image::DecodeOptions decodeOptions;
            decodeOptions.raster_layout = snow::image::RasterLayoutPolicy::native;
            decodeOptions.preserve_metadata = false;
            snow::image::RasterStoreOptions storeOptions;
            storeOptions.analysis.alpha_content = snow::image::AlphaContent::opaque;
            auto decoded =
                service.decode_to_store(artifactInput.value(), nativePath(partialPreview),
                                        decodeOptions, storeOptions, stop);
            if (decoded)
                decoded.value().reset();
            else
                streamed = decoded.error();
        } else {
            auto sink = std::make_unique<MappedRasterSink>(partialPreview);
            streamed = service.decode_to_sink(artifactInput.value(), *sink, {}, stop);
            sink.reset();
        }
        if (!streamed) {
            QFile::remove(partialPreview);
            result.insert(QStringLiteral("error"), statusText(streamed.error()));
            return result;
        }
        result.insert(QStringLiteral("artifactDecodeNs"), QString::number(stage.nsecsElapsed()));
        stage.restart();
    }
    if (previewKind != QStringLiteral("base") && !previewUsesPreparedRaster) {
        if (!QFile::rename(partialPreview, previewPath)) {
            QFile::remove(partialPreview);
            result.insert(QStringLiteral("error"),
                          QStringLiteral("The worker could not publish the preview package."));
            return result;
        }
        QFile::setPermissions(previewPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        result.insert(QStringLiteral("previewPath"), previewPath);
        result.insert(QStringLiteral("previewPackageNs"), QString::number(stage.nsecsElapsed()));
    }
    if (stop.stop_requested()) {
        result.insert(QStringLiteral("error"), QStringLiteral("The export was cancelled."));
        return result;
    }
    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("warning"), warning);
    result.insert(QStringLiteral("previewKind"), previewKind);
    result.insert(QStringLiteral("pixelExact"), pixelExact);
    result.insert(QStringLiteral("artifactBytes"), QString::number(encoded.value().bytes_written));
    result.insert(QStringLiteral("totalNs"), QString::number(total.nsecsElapsed()));
    return result;
}

QJsonObject executePreviewJob(const QString& artifactPath, const QString& previewPath,
                              snow::image::Format format, std::stop_token stop) {
    QJsonObject result{{QStringLiteral("success"), false},
                       {QStringLiteral("artifactPublished"), true}};
    const QString partialPreview = previewPath + QStringLiteral(".partial");
    auto input = snow::image::file_input(nativePath(artifactPath));
    if (!input) {
        result.insert(QStringLiteral("error"), statusText(input.error()));
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    snow::image::Service service;
    snow::image::Result<void> decoded;
    if (format == snow::image::Format::jpeg) {
        snow::image::DecodeOptions decodeOptions;
        decodeOptions.raster_layout = snow::image::RasterLayoutPolicy::native;
        decodeOptions.preserve_metadata = false;
        snow::image::RasterStoreOptions storeOptions;
        storeOptions.analysis.alpha_content = snow::image::AlphaContent::opaque;
        auto store = service.decode_to_store(input.value(), nativePath(partialPreview),
                                             decodeOptions, storeOptions, stop);
        if (store)
            store.value().reset();
        else
            decoded = store.error();
    } else {
        auto sink = std::make_unique<MappedRasterSink>(partialPreview);
        decoded = service.decode_to_sink(input.value(), *sink, {}, stop);
        sink.reset();
    }
    if (!decoded) {
        QFile::remove(partialPreview);
        result.insert(QStringLiteral("error"), statusText(decoded.error()));
        return result;
    }
    if (!QFile::rename(partialPreview, previewPath)) {
        QFile::remove(partialPreview);
        result.insert(QStringLiteral("error"),
                      QStringLiteral("The worker could not publish the preview package."));
        return result;
    }
    QFile::setPermissions(previewPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("previewKind"), QStringLiteral("codec"));
    result.insert(QStringLiteral("previewPath"), previewPath);
    result.insert(QStringLiteral("artifactDecodeNs"), QString::number(timer.nsecsElapsed()));
    return result;
}

} // namespace snow::image_viewer::worker_core
