#include "core/folder_sequence.h"
#include "core/image_raster_store.h"
#include "core/image_tile_store.h"
#include "decoding/image_loader.h"
#include "decoding/snow_image_decoder.h"
#include "editing/edit_pipeline_controller.h"
#include "editing/raster_asset.h"
#include "editing/raster_package.h"
#include "editing/worker_core.h"
#include "editing/worker_protocol.h"
#include "render/texture_size.h"

#include <snow/image/resource_estimate.h>
#include <snow/image/service.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

class ChunkedWriteDevice final : public QIODevice {
  public:
    ChunkedWriteDevice() {
        open(QIODevice::WriteOnly);
    }
    [[nodiscard]] const QByteArray& bytes() const noexcept {
        return bytes_;
    }

  protected:
    qint64 readData(char*, qint64) override {
        return -1;
    }
    qint64 writeData(const char* data, qint64 size) override {
        const qint64 count = std::min<qint64>(size, 7);
        bytes_.append(data, count);
        return count;
    }

  private:
    QByteArray bytes_;
};

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition)
        fail(message);
}

template <typename T> T take(snow::image::Result<T> result, std::string_view message) {
    if (!result) {
        std::cerr << "FAILED: " << message << ": " << result.error().message << '\n';
        std::exit(1);
    }
    return std::move(result).value();
}

void take(const snow::image::Result<void>& result, std::string_view message) {
    if (!result) {
        std::cerr << "FAILED: " << message << ": " << result.error().message << '\n';
        std::exit(1);
    }
}

snow::image::Image sampleImage(std::byte red, std::uint32_t width = 2, std::uint32_t height = 2) {
    snow::image::MutableImage image =
        take(snow::image::MutableImage::allocate(width, height, snow::image::kRgba8),
             "allocate test image");
    for (std::size_t offset = 0; offset < image.pixels().size(); offset += 4U) {
        image.pixels()[offset] = red;
        image.pixels()[offset + 1U] = std::byte{0x20};
        image.pixels()[offset + 2U] = std::byte{0x40};
        image.pixels()[offset + 3U] = std::byte{0xFF};
    }
    return std::move(image).freeze();
}

std::filesystem::path nativePath(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

snow::image_viewer::GpuRasterResult gpuReadback(QImage pixels) {
    pixels = pixels.convertToFormat(QImage::Format_RGBA8888);
    snow::image_viewer::GpuRasterResult readback;
    readback.pixelSize = pixels.size();
    readback.encoding = snow::image_viewer::PixelEncoding::Srgb8;
    readback.color.sourceColorSpace = QColorSpace(QColorSpace::SRgb);
    const qsizetype rowBytes = static_cast<qsizetype>(pixels.width()) * 4;
    readback.rowStride = static_cast<std::size_t>(rowBytes);
    readback.storage = std::make_shared<QByteArray>(rowBytes * pixels.height(), Qt::Uninitialized);
    for (int y = 0; y < pixels.height(); ++y) {
        std::memcpy(readback.storage->data() + static_cast<qsizetype>(y) * rowBytes,
                    pixels.constScanLine(y), static_cast<std::size_t>(rowBytes));
    }
    return readback;
}

std::array<unsigned char, 4> tiledPixel(int x, int y) {
    return {static_cast<unsigned char>(x * 31 + y), static_cast<unsigned char>(y * 53 + x),
            static_cast<unsigned char>((x + y) * 17), 0xFF};
}

snow::image_viewer::GpuRasterResult tiledGpuReadback(const QSize& size, int tileLimit,
                                                     std::uint64_t* storageBytes = nullptr) {
    snow::image_viewer::GpuRasterResult readback;
    readback.pixelSize = size;
    readback.encoding = snow::image_viewer::PixelEncoding::Srgb8;
    readback.color.sourceColorSpace = QColorSpace(QColorSpace::SRgb);
    for (const QRect& rect : snow::image_viewer::textureTilesForLimit(size, tileLimit)) {
        const std::size_t rowStride = static_cast<std::size_t>(rect.width()) * 4U + 4U;
        auto storage = std::make_shared<QByteArray>(
            static_cast<qsizetype>(rowStride * static_cast<std::size_t>(rect.height())),
            static_cast<char>(0xA5));
        for (int y = 0; y < rect.height(); ++y) {
            auto* row = reinterpret_cast<unsigned char*>(
                storage->data() + static_cast<qsizetype>(y) * static_cast<qsizetype>(rowStride));
            for (int x = 0; x < rect.width(); ++x) {
                const auto pixel = tiledPixel(rect.x() + x, rect.y() + y);
                std::memcpy(row + static_cast<std::size_t>(x) * 4U, pixel.data(), pixel.size());
            }
        }
        readback.tiles.push_back({std::move(storage), rect, rowStride});
    }
    if (storageBytes)
        *storageBytes = readback.storageBytes();
    return readback;
}

void testGpuRasterTileContract() {
    auto valid = tiledGpuReadback(QSize(5, 3), 2);
    require(valid.isValid() && valid.tiles.size() == 6,
            "GPU tile contract accepts canonical partial-edge coverage");
    require(valid.storageBytes() == 96,
            "GPU tile storage accounting includes padded tile allocations");

    auto missingEdge = valid;
    missingEdge.tiles.pop_back();
    require(!missingEdge.isValid(), "GPU tile contract rejects missing edge coverage");

    auto gap = valid;
    gap.tiles.front().pixelRect.translate(1, 0);
    require(!gap.isValid(), "GPU tile contract rejects coverage gaps");

    auto outOfOrder = valid;
    std::swap(outOfOrder.tiles[0], outOfOrder.tiles[1]);
    require(!outOfOrder.isValid(), "GPU tile contract rejects overlaps and out-of-order tiles");

    auto inconsistentBand = valid;
    inconsistentBand.tiles[1].pixelRect.setHeight(1);
    require(!inconsistentBand.isValid(), "GPU tile contract rejects inconsistent row-band heights");

    auto shortStride = valid;
    shortStride.tiles.front().rowStride = 7;
    require(!shortStride.isValid(), "GPU tile contract rejects short row strides");

    auto shortStorage = valid;
    shortStorage.tiles.front().storage = std::make_shared<QByteArray>(1, '\0');
    require(!shortStorage.isValid(), "GPU tile contract rejects short storage");

    auto mixed = valid;
    mixed.storage = std::make_shared<QByteArray>(5 * 3 * 4, '\0');
    mixed.rowStride = 5 * 4;
    require(!mixed.isValid(), "GPU tile contract rejects mixed storage forms");

    auto tiledWithGlobalStride = valid;
    tiledWithGlobalStride.rowStride = 5 * 4;
    require(!tiledWithGlobalStride.isValid(),
            "GPU tile contract rejects a global stride in tiled form");

    snow::image_viewer::GpuRasterResult overflow;
    overflow.pixelSize = QSize(1, 2);
    overflow.rowStride = std::numeric_limits<std::size_t>::max();
    overflow.storage = std::make_shared<QByteArray>(8, '\0');
    require(!overflow.isValid(), "GPU tile contract rejects byte-size overflow");
}

void testTextureTilePlanning() {
    const auto tiles = snow::image_viewer::textureTilesForLimit(QSize(5, 3), 2);
    const std::vector<QRect> expected{{0, 0, 2, 2}, {2, 0, 2, 2}, {4, 0, 1, 2},
                                      {0, 2, 2, 1}, {2, 2, 2, 1}, {4, 2, 1, 1}};
    require(tiles == expected, "texture tiling is canonical and preserves partial edges");
    require(snow::image_viewer::textureTilesForLimit(QSize(1, INT_MAX), INT_MAX) ==
                std::vector<QRect>{{0, 0, 1, INT_MAX}},
            "texture tiling handles maximum signed dimensions without overflow");
    require(snow::image_viewer::textureTilesForLimit({}, 2).empty() &&
                snow::image_viewer::textureTilesForLimit(QSize(2, 2), 0).empty(),
            "texture tiling rejects invalid inputs");

    const auto arrayPlan =
        snow::image_viewer::textureArrayTilePlan(QSize(16'385, 8), 16'384, 2'048);
    require(arrayPlan.isValid() && arrayPlan.columns > 1 && arrayPlan.rows == 1 &&
                arrayPlan.tiles.front().topLeft().isNull() &&
                arrayPlan.tiles.back().right() == 16'384 &&
                arrayPlan.allocatedPixels() < 16'385U * 8U * 2U,
            "array tiling avoids full-layer padding for narrow oversized images");
    for (const QRect& tile : arrayPlan.tiles) {
        require(tile.width() <= arrayPlan.layerSize.width() &&
                    tile.height() <= arrayPlan.layerSize.height(),
                "array tiling keeps every edge tile inside its layer");
    }
    require(!snow::image_viewer::textureArrayTilePlan(QSize(INT_MAX, INT_MAX), 16'384, 1).isValid(),
            "array tiling rejects sources beyond the available layer count");
}

void testSuffixes() {
    const QStringList suffixes = snow::image_viewer::FolderSequence::supportedSuffixes();
    constexpr std::array<std::string_view, 22> expected{
        "bmp", "cur",  "gif", "ico", "jfif", "jpeg", "jpg", "pbm",  "pgm", "png", "ppm",
        "svg", "svgz", "xbm", "xpm", "heic", "heif", "hif", "avif", "jxl", "exr", "webp"};
    for (const std::string_view suffix : expected) {
        require(suffixes.contains(
                    QString::fromUtf8(suffix.data(), static_cast<qsizetype>(suffix.size()))),
                "viewer suffix discovery includes every snow_image alias");
    }
}

void testDecoder(const QString& directory) {
    snow::image::Document document;
    document.format = snow::image::Format::png;
    document.canvas_width = 128;
    document.canvas_height = 64;
    snow::image::Frame frame;
    frame.image = sampleImage(std::byte{0xE0}, document.canvas_width, document.canvas_height);
    document.frames.push_back(std::move(frame));
    const QString path = directory + QStringLiteral("/sample.png");
    snow::image::Service service;
    snow::image::EncodeOptions options;
    options.format = snow::image::Format::png;
    {
        auto output = take(snow::image::file_output(nativePath(path)), "open PNG output");
        require(service.encode(document, output, options).has_value(), "encode viewer PNG fixture");
    }

    snow::image_viewer::DecodeResult decoded =
        snow::image_viewer::ImageLoader::decodeSynchronously(path);
    if (!decoded.succeeded()) {
        std::cerr << "viewer PNG decode error: " << decoded.error.toStdString() << '\n';
    }
    require(decoded.succeeded() && decoded.image.sourceSize == QSize(128, 64),
            "viewer decodes PNG through snow_image");
    require(decoded.image.decoderName.contains(QStringLiteral("libpng")),
            "viewer reports the snow_image backend");
    require(decoded.image.rasterStore && decoded.image.rasterStore->isValid() &&
                decoded.image.rasterStore->store()->complete() &&
                decoded.image.rasterStore->verifiedAlphaContent() ==
                    snow::image::AlphaContent::opaque,
            "viewer retains the streaming decode as a committed shared raster");
    const QString rasterPath = decoded.image.rasterStore->filePath();
    require(QFileInfo::exists(rasterPath),
            "shared decode raster exists while the decoded image owns it");
    QImage retainedPixels = std::move(decoded.image.pixels);
    decoded.image.rasterStore.reset();
    require(QFileInfo::exists(rasterPath),
            "mapped display pixels retain the shared raster after explicit owner release");
    retainedPixels = {};
    require(!QFileInfo::exists(rasterPath),
            "shared decode raster is removed after its final mapped-image owner releases it");

#if defined(Q_OS_WIN)
    const snow::image_viewer::ImageThumbnail thumbnail =
        snow::image_viewer::loadSystemThumbnail(path, 64);
    require(thumbnail.isValid(), "Windows Shell produces an image thumbnail");
    require(thumbnail.sourceSize == QSize(128, 64),
            "Windows Shell thumbnail preserves the source dimensions");
    require(thumbnail.pixels.width() <= 64 && thumbnail.pixels.height() <= 64,
            "Windows Shell thumbnail respects the requested extent");
#endif
}

void testAnimation(const QString& directory) {
    snow::image::Document document;
    document.format = snow::image::Format::gif;
    document.canvas_width = 2;
    document.canvas_height = 2;
    document.loop_count = 3;
    snow::image::Frame first;
    first.image = sampleImage(std::byte{0xF0});
    first.duration = std::chrono::milliseconds(40);
    document.frames.push_back(std::move(first));
    snow::image::Frame second;
    second.image = sampleImage(std::byte{0x10});
    second.duration = std::chrono::milliseconds(70);
    document.frames.push_back(std::move(second));
    const QString path = directory + QStringLiteral("/animated.gif");
    snow::image::Service service;
    snow::image::EncodeOptions options;
    options.format = snow::image::Format::gif;
    auto output = take(snow::image::file_output(nativePath(path)), "open GIF output");
    require(service.encode(document, output, options).has_value(), "encode viewer GIF fixture");

    const snow::image_viewer::DecodeResult decoded =
        snow::image_viewer::ImageLoader::decodeSynchronously(path);
    require(decoded.succeeded() && decoded.image.animationFrames.size() == 2 &&
                decoded.image.loopCount == 3,
            "viewer exposes decoded animation frames and loop count");
    require(decoded.image.animationFrames[0].durationMilliseconds == 40 &&
                decoded.image.animationFrames[1].durationMilliseconds == 70,
            "viewer preserves animation timing");
}

void testSharedSourceRasterPipeline(const QString& directory) {
    const QString path = directory + QStringLiteral("/sample.png");
    snow::image_viewer::DecodeResult decoded =
        snow::image_viewer::ImageLoader::decodeSynchronously(path);
    require(decoded.succeeded() && decoded.image.rasterStore,
            "decode an identity-reuse source raster");

    snow::image_viewer::EditPipelineController controller;
    snow::image_viewer::EditExportSettings identity;
    identity.sourceSize = QSize(128, 64);
    identity.width = 128;
    identity.height = 64;
    identity.format = snow::image::Format::png;
    identity.encode.format = identity.format;
    identity.encode.compression_level = 1;
    auto resized = identity;
    resized.width = 64;
    resized.height = 32;

    QEventLoop loop;
    bool sourceReuseStage = false;
    bool verifiedAlphaReused = false;
    bool identityReady = false;
    bool preservedSamples = false;
    bool resizeRequested = false;
    int rasterRequests = 0;
    QString failure;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { controller.requestEdit(identity); });
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](quint64, const QString& stage, qint64 nanoseconds) {
                         if (stage == QStringLiteral("exact.source_raster_reuse"))
                             sourceReuseStage = true;
                         if (stage == QStringLiteral("exact.classify_alpha") && nanoseconds == 0)
                             verifiedAlphaReused = true;
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactRasterRequested,
                     &loop, [&](quint64, const auto& requested) {
                         ++rasterRequests;
                         if (requested == resized) {
                             resizeRequested = true;
                             loop.quit();
                         }
                     });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
        [&](const snow::image_viewer::ExactEditResult& result) {
            if (result.settings != identity)
                return;
            snow::image::Service service;
            snow::image::DecodeOptions inspectOptions;
            inspectOptions.raster_layout = snow::image::RasterLayoutPolicy::packed;
            const auto descriptor =
                result.artifact
                    ? service.inspect_raster(result.artifact->input(), inspectOptions)
                    : snow::image::Result<snow::image::DocumentDescriptor>(
                          snow::image::Status::error(snow::image::ErrorCode::invalid_argument,
                                                     "Identity artifact is unavailable."));
            preservedSamples = descriptor && descriptor.value().frames.size() == 1 &&
                               descriptor.value().frames.front().layout.planes.size() == 1 &&
                               descriptor.value().frames.front().layout.planes.front().format ==
                                   snow::image::kRgba8;
            identityReady =
                result.provenance == snow::image_viewer::RasterProvenance::source_exact &&
                result.previewSource == snow::image_viewer::ExactPreviewSource::gpu_raster &&
                !result.displayPreview.has_value() && controller.readbackBytes() == 0;
            QTimer::singleShot(0, &loop, [&]() { controller.requestEdit(resized); });
        });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(15'000, &loop, &QEventLoop::quit);
    controller.setGpuSource(path, decoded.image.rasterStore);
    loop.exec();
    if (!failure.isEmpty()) {
        std::cerr << "Shared source-raster pipeline error: " << failure.toStdString() << '\n';
    }
    require(
        failure.isEmpty() && sourceReuseStage && verifiedAlphaReused && identityReady &&
            preservedSamples && resizeRequested && rasterRequests == 1,
        "identity export reuses the exact shared source while resize still requests GPU output");
}

void testSharedSourceRasterPreserves16Bit(const QString& directory) {
    auto pixels = take(snow::image::MutableImage::allocate(3, 2, snow::image::kRgba16),
                       "allocate 16-bit shared-raster fixture");
    for (std::size_t index = 0; index < pixels.pixels().size(); ++index)
        pixels.pixels()[index] = static_cast<std::byte>((index * 29U + 7U) & 0xFFU);
    snow::image::Document document;
    document.format = snow::image::Format::png;
    document.canvas_width = 3;
    document.canvas_height = 2;
    document.color.primaries = snow::image::ColorPrimaries::srgb;
    document.color.transfer = snow::image::TransferFunction::srgb;
    snow::image::Frame frame;
    frame.image = std::move(pixels).freeze();
    frame.color = document.color;
    document.frames.push_back(std::move(frame));
    const QString path = directory + QStringLiteral("/shared-16.png");
    snow::image::Service service;
    snow::image::EncodeOptions encode;
    encode.format = snow::image::Format::png;
    require(service
                .encode(document,
                        take(snow::image::file_output(nativePath(path)),
                             "open 16-bit shared-raster fixture"),
                        encode)
                .has_value(),
            "encode 16-bit shared-raster fixture");

    snow::image_viewer::DecodeResult decoded =
        snow::image_viewer::ImageLoader::decodeSynchronously(path);
    if (!decoded.succeeded() || !decoded.image.rasterStore ||
        decoded.image.pixels.format() != QImage::Format_RGBA8888) {
        std::cerr << "16-bit shared PNG decode: error='" << decoded.error.toStdString()
                  << "', raster=" << static_cast<bool>(decoded.image.rasterStore)
                  << ", format=" << static_cast<int>(decoded.image.pixels.format()) << '\n';
    }
    require(decoded.succeeded() && decoded.image.rasterStore &&
                decoded.image.pixels.format() == QImage::Format_RGBA8888,
            "viewer derives an RGBA8 display image from the shared 16-bit raster");
    const auto& sourceDescriptor = decoded.image.rasterStore->store()->descriptor();
    require(sourceDescriptor.frames.size() == 1 &&
                sourceDescriptor.frames.front().layout.planes.size() == 1 &&
                sourceDescriptor.frames.front().layout.planes.front().format ==
                    snow::image::kRgba16 &&
                sourceDescriptor.frames.front().layout.planes.front().significant_bits == 16,
            "shared decode raster retains native 16-bit PNG samples");
    require(decoded.image.rasterStore->verifiedAlphaContent() ==
                snow::image::AlphaContent::non_opaque,
            "shared 16-bit raster records its exact alpha classification");

    snow::image_viewer::EditPipelineController controller;
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(3, 2);
    settings.width = 3;
    settings.height = 2;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;
    QEventLoop loop;
    bool ready = false;
    bool rasterRequested = false;
    QString failure;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { controller.requestEdit(settings); });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactRasterRequested,
                     &loop, [&](quint64, const auto&) { rasterRequested = true; });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
        [&](const snow::image_viewer::ExactEditResult& result) {
            snow::image::DecodeOptions inspectOptions;
            const auto descriptor =
                result.artifact
                    ? service.inspect_raster(result.artifact->input(), inspectOptions)
                    : snow::image::Result<snow::image::DocumentDescriptor>(
                          snow::image::Status::error(snow::image::ErrorCode::invalid_argument,
                                                     "16-bit identity artifact is unavailable."));
            ready = result.provenance == snow::image_viewer::RasterProvenance::source_exact &&
                    descriptor && descriptor.value().frames.size() == 1 &&
                    descriptor.value().frames.front().layout.planes.front().format ==
                        snow::image::kRgba16;
            loop.quit();
        });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(15'000, &loop, &QEventLoop::quit);
    controller.setGpuSource(path, decoded.image.rasterStore);
    loop.exec();
    if (!failure.isEmpty()) {
        std::cerr << "16-bit shared source-raster pipeline error: " << failure.toStdString()
                  << '\n';
    }
    require(failure.isEmpty() && ready && !rasterRequested && controller.readbackBytes() == 0,
            "16-bit identity export retains native precision without GPU readback");
}

void testPremultipliedRasterIsNotSourceExact() {
    snow::image::PixelFormat premultiplied = snow::image::kRgba8;
    premultiplied.alpha = snow::image::AlphaMode::premultiplied;
    snow::image::DocumentDescriptor descriptor;
    descriptor.canvas_width = 2;
    descriptor.canvas_height = 2;
    snow::image::RasterFrameDescriptor frame;
    frame.width = 2;
    frame.height = 2;
    frame.layout.alpha = snow::image::AlphaMode::premultiplied;
    frame.layout.planes.push_back({snow::image::PlaneSemantic::packed, 2, 2, premultiplied, 8});
    descriptor.frames.push_back(std::move(frame));
    QString error;
    auto backing = snow::image_viewer::ImageRasterStore::create(std::move(descriptor), {}, &error);
    require(backing && error.isEmpty(), "create premultiplied raster reuse fixture");
    const std::array<std::byte, 16> bytes{
        std::byte{0x40}, std::byte{0x20}, std::byte{0x10}, std::byte{0x80},
        std::byte{0x20}, std::byte{0x10}, std::byte{0x08}, std::byte{0x40},
        std::byte{0x40}, std::byte{0x20}, std::byte{0x10}, std::byte{0x80},
        std::byte{0x20}, std::byte{0x10}, std::byte{0x08}, std::byte{0x40}};
    require(backing->store()->write_rows(0, 0, 0, 2, 8, bytes).has_value() &&
                backing->store()->commit().has_value(),
            "commit premultiplied raster reuse fixture");

    snow::image_viewer::EditPipelineController controller;
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(2, 2);
    settings.width = 2;
    settings.height = 2;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;
    QEventLoop loop;
    bool requested = false;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { controller.requestEdit(settings); });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactRasterRequested,
                     &loop, [&](quint64, const auto&) {
                         requested = true;
                         loop.quit();
                     });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    controller.setGpuSource(QStringLiteral("premultiplied-fixture.png"), backing);
    loop.exec();
    require(requested, "premultiplied display rasters are never labeled source-exact");
}

void testAnimatedExportCacheIsolation(const QString& directory) {
    const auto runOrder = [&](snow::image::Format firstFormat, snow::image::Format secondFormat) {
        snow::image_viewer::EditPipelineController controller;
        snow::image_viewer::EditExportSettings first;
        first.sourceSize = QSize(2, 2);
        first.width = 2;
        first.height = 2;
        first.format = firstFormat;
        first.encode.format = firstFormat;
        first.encode.lossless = true;
        first.encode.quality = 100;
        first.encode.effort = 4;
        auto second = first;
        second.format = secondFormat;
        second.encode.format = secondFormat;
        QEventLoop loop;
        int publications = 0;
        bool validPng = false;
        bool validWebp = false;
        bool reusedBaseRaster = false;
        QString failure;
        snow::image::Service service;
        QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady,
                         &loop, [&]() { controller.requestEdit(first); });
        QObject::connect(&controller,
                         &snow::image_viewer::EditPipelineController::performanceStageCompleted,
                         &loop, [&](quint64, const QString& stage, qint64) {
                             if (stage == QStringLiteral("exact.raster_cache_hit"))
                                 reusedBaseRaster = true;
                         });
        QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady,
                         &loop, [&](const snow::image_viewer::ExactEditResult& result) {
                             ++publications;
                             if (!result.artifact) {
                                 failure = QStringLiteral("missing encoded artifact");
                                 loop.quit();
                                 return;
                             }
                             auto decoded = service.decode(result.artifact->input());
                             if (!decoded) {
                                 failure = QString::fromStdString(decoded.error().message);
                                 loop.quit();
                                 return;
                             }
                             if (result.settings.format == snow::image::Format::png) {
                                 const auto& document = decoded.value();
                                 validPng =
                                     document.frames.size() == 1 &&
                                     result.warning.contains(QStringLiteral("first frame")) &&
                                     result.displayPreview &&
                                     result.displayPreview->animationFrames.empty() &&
                                     std::to_integer<unsigned char>(
                                         document.frames.front().image.pixels().front()) > 0x80;
                             } else if (result.settings.format == snow::image::Format::webp) {
                                 const auto& document = decoded.value();
                                 const auto firstMs =
                                     document.frames.empty()
                                         ? -1
                                         : std::chrono::duration_cast<std::chrono::milliseconds>(
                                               document.frames[0].duration)
                                               .count();
                                 const auto secondMs =
                                     document.frames.size() < 2
                                         ? -1
                                         : std::chrono::duration_cast<std::chrono::milliseconds>(
                                               document.frames[1].duration)
                                               .count();
                                 validWebp = document.frames.size() == 2 && firstMs == 40 &&
                                             secondMs == 70 && result.warning.isEmpty() &&
                                             result.displayPreview &&
                                             result.displayPreview->animationFrames.size() == 2 &&
                                             document.frames[0].image.pixels().front() !=
                                                 document.frames[1].image.pixels().front();
                             }
                             if (publications == 1) {
                                 controller.requestEdit(second);
                             } else {
                                 loop.quit();
                             }
                         });
        QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                         [&](const QString& message) {
                             failure = message;
                             loop.quit();
                         });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        controller.setSource(directory + QStringLiteral("/animated.gif"));
        loop.exec();
        if (!failure.isEmpty()) {
            std::cerr << "animated cache isolation error: " << failure.toStdString() << '\n';
        }
        require(failure.isEmpty() && publications == 2 && validPng && validWebp &&
                    !reusedBaseRaster,
                "animation policy isolates first-frame and preserved base rasters");
    };

    runOrder(snow::image::Format::png, snow::image::Format::webp);
    runOrder(snow::image::Format::webp, snow::image::Format::png);
}

void testEditSession(const QString& directory) {
    const QString path = directory + QStringLiteral("/sample.png");
    snow::image_viewer::EditPipelineController session;
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(128, 64);
    settings.width = 64;
    settings.height = 32;
    settings.format = snow::image::Format::webp;
    settings.encode.format = settings.format;
    settings.encode.lossless = true;
    settings.encode.quality = 100;
    settings.encode.effort = 4;
    QEventLoop loop;
    bool succeeded = false;
    bool reusedExactPixels = false;
    QString failure;
    QObject::connect(&session, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&session, settings]() { session.requestEdit(settings); });
    QObject::connect(&session, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         reusedExactPixels =
                             result.displayPreview.has_value() &&
                             result.previewSource ==
                                 snow::image_viewer::ExactPreviewSource::base_raster;
                         succeeded = result.displayPreview &&
                                     result.displayPreview->sourceSize == QSize(64, 32) &&
                                     result.artifact && result.artifact->byteSize() > 0 &&
                                     session.hasExactResult(settings);
                         loop.quit();
                     });
    QObject::connect(&session, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    session.setSource(path);
    loop.exec();
    if (!failure.isEmpty()) {
        std::cerr << "edit session error: " << failure.toStdString() << '\n';
    }
    require(succeeded && reusedExactPixels,
            "lossless WebP reuses transformed pixels without a codec decode");
}

void testGpuEditEncoding(const QString& directory) {
    const QString path = directory + QStringLiteral("/sample.png");
    snow::image_viewer::EditPipelineController session;
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(128, 64);
    settings.width = 64;
    settings.height = 32;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;
    settings.encode.compression_level = 1;
    QEventLoop loop;
    bool succeeded = false;
    QString failure;
    QObject::connect(&session, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&session, settings]() { session.requestEdit(settings); });
    QObject::connect(
        &session, &snow::image_viewer::EditPipelineController::exactRasterRequested, &loop,
        [&session, settings](quint64 generation,
                             const snow::image_viewer::EditExportSettings& requested) {
            require(requested == settings, "GPU edit session preserves requested settings");
            QImage pixels(requested.width, requested.height, QImage::Format_RGBA8888);
            pixels.fill(QColor(0x30, 0x80, 0xD0, 0xC0));
            session.submitGpuResizeResult(generation, gpuReadback(std::move(pixels)));
        });
    QObject::connect(&session, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         QByteArray firstSave;
                         QByteArray secondSave;
                         QBuffer firstBuffer(&firstSave);
                         QBuffer secondBuffer(&secondSave);
                         ChunkedWriteDevice chunkedBuffer;
                         firstBuffer.open(QIODevice::WriteOnly);
                         secondBuffer.open(QIODevice::WriteOnly);
                         QString copyError;
                         const bool copiedTwice =
                             result.artifact && result.artifact->copyTo(firstBuffer, &copyError) &&
                             result.artifact->copyTo(secondBuffer, &copyError) &&
                             result.artifact->copyTo(chunkedBuffer, &copyError) &&
                             !firstSave.isEmpty() && firstSave == secondSave;
                         succeeded = result.previewSource ==
                                         snow::image_viewer::ExactPreviewSource::gpu_raster &&
                                     !result.displayPreview && result.artifact &&
                                     result.artifact->byteSize() > 0 && copiedTwice &&
                                     chunkedBuffer.bytes() == firstSave && copyError.isEmpty() &&
                                     session.hasExactResult(settings);
                         loop.quit();
                     });
    QObject::connect(&session, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    session.setGpuSource(path);
    loop.exec();
    if (!failure.isEmpty()) {
        std::cerr << "GPU edit encoding error: " << failure.toStdString() << '\n';
    }
    require(succeeded, "edit session encodes a GPU resize readback without a CPU preview");
}

void testTiledGpuEditEncoding(const QString& directory) {
    snow::image_viewer::EditPipelineController session;
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(128, 64);
    settings.width = 5;
    settings.height = 3;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;
    settings.encode.compression_level = 1;
    QEventLoop loop;
    bool succeeded = false;
    QString failure;
    std::uint64_t expectedStorageBytes = 0;
    QObject::connect(&session, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { session.requestEdit(settings); });
    QObject::connect(
        &session, &snow::image_viewer::EditPipelineController::exactRasterRequested, &loop,
        [&](quint64 requestId, const snow::image_viewer::EditExportSettings& requested) {
            require(requested == settings, "tiled GPU edit preserves requested export settings");
            auto readback = tiledGpuReadback(QSize(requested.width, requested.height), 2,
                                             &expectedStorageBytes);
            session.submitGpuResizeResult(requestId, std::move(readback));
        });
    QObject::connect(
        &session, &snow::image_viewer::EditPipelineController::exactReady, &loop,
        [&](const snow::image_viewer::ExactEditResult& result) {
            snow::image::Service service;
            std::optional<snow::image::Document> document;
            if (result.artifact) {
                auto decoded = service.decode(result.artifact->input());
                if (decoded)
                    document = std::move(decoded).value();
            }
            bool pixelsMatch = document && document->frames.size() == 1;
            if (pixelsMatch) {
                const snow::image::Image& image = document->frames.front().image;
                pixelsMatch = image.width() == 5 && image.height() == 3 &&
                              image.format() == snow::image::kRgba8;
                for (int y = 0; pixelsMatch && y < 3; ++y) {
                    for (int x = 0; x < 5; ++x) {
                        const auto expected = tiledPixel(x, y);
                        const auto* actual = image.pixels().data() +
                                             static_cast<std::size_t>(y) * image.row_stride() +
                                             static_cast<std::size_t>(x) * expected.size();
                        for (std::size_t channel = 0; channel < expected.size(); ++channel) {
                            if (std::to_integer<unsigned char>(actual[channel]) !=
                                expected[channel]) {
                                pixelsMatch = false;
                                break;
                            }
                        }
                        if (!pixelsMatch)
                            break;
                    }
                }
            }
            succeeded =
                pixelsMatch &&
                result.provenance == snow::image_viewer::RasterProvenance::gpu_approximate &&
                result.previewSource == snow::image_viewer::ExactPreviewSource::gpu_raster &&
                session.readbackBytes() == 0;
            loop.quit();
        });
    QObject::connect(&session, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(15'000, &loop, &QEventLoop::quit);
    session.setGpuSource(directory + QStringLiteral("/sample.png"));
    loop.exec();
    if (!failure.isEmpty()) {
        std::cerr << "Tiled GPU edit encoding error: " << failure.toStdString() << '\n';
    }
    require(succeeded, "tiled GPU readback publishes exact seam and partial-edge pixels");
}

void testPreparedPreviewSource(const QString& directory) {
    snow::image_viewer::EditPipelineController controller;
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(128, 64);
    settings.width = 64;
    settings.height = 32;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;
    settings.paletteColors = 2;
    settings.ditheringPercent = 0;
    QEventLoop loop;
    bool preparedPreview = false;
    bool decodedCodec = false;
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](quint64, const QString& stage, qint64) {
                         if (stage == QStringLiteral("exact.decode_codec"))
                             decodedCodec = true;
                     });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::exactRasterRequested, &loop,
        [&](quint64 requestId, const snow::image_viewer::EditExportSettings& requested) {
            QImage pixels(requested.width, requested.height, QImage::Format_RGBA8888);
            for (int y = 0; y < pixels.height(); ++y) {
                for (int x = 0; x < pixels.width(); ++x)
                    pixels.setPixelColor(x, y, x == 0 ? Qt::blue : Qt::red);
            }
            controller.submitGpuResizeResult(requestId, gpuReadback(std::move(pixels)));
        });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         if (!result.settings.reducePalette) {
                             settings.reducePalette = true;
                             controller.requestEdit(settings);
                             return;
                         }
                         preparedPreview =
                             result.previewSource ==
                                 snow::image_viewer::ExactPreviewSource::prepared_raster &&
                             result.displayPreview.has_value() && result.artifact;
                         loop.quit();
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString&) { loop.quit(); });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    controller.setGpuSource(directory + QStringLiteral("/sample.png"));
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { controller.requestEdit(settings); });
    loop.exec();
    require(preparedPreview && !decodedCodec,
            "editing palette reduction publishes prepared pixels without a worker crash");
}

void testPngPreparationPreserves16Bit() {
    auto pixels = take(snow::image::MutableImage::allocate(2, 1, snow::image::kRgba16),
                       "allocate 16-bit PNG preparation fixture");
    for (std::size_t index = 0; index < pixels.pixels().size(); ++index)
        pixels.pixels()[index] = static_cast<std::byte>(index * 13U);
    const std::vector<std::byte> expected(pixels.pixels().begin(), pixels.pixels().end());
    snow::image::Document document;
    document.canvas_width = 2;
    document.canvas_height = 1;
    document.color.primaries = snow::image::ColorPrimaries::srgb;
    document.color.transfer = snow::image::TransferFunction::srgb;
    snow::image::Frame frame;
    frame.image = std::move(pixels).freeze();
    frame.color = document.color;
    document.frames.push_back(std::move(frame));
    const auto fixtureFormat = document.frames.front().image.format();
    require(document.color.dynamic_range == snow::image::DynamicRange::standard &&
                document.color.primaries == snow::image::ColorPrimaries::srgb &&
                document.color.transfer == snow::image::TransferFunction::srgb &&
                fixtureFormat.sample_type == snow::image::SampleType::unsigned_integer &&
                fixtureFormat.bits_per_channel == 16 &&
                document.frames.front().color.dynamic_range ==
                    snow::image::DynamicRange::standard &&
                document.frames.front().color.primaries == snow::image::ColorPrimaries::srgb &&
                document.frames.front().color.transfer == snow::image::TransferFunction::srgb,
            "16-bit PNG preparation fixture is SDR sRGB");
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(2, 1);
    settings.width = 2;
    settings.height = 1;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;
    settings.encode.preserve_metadata = true;
    snow::image::Service service;
    const snow::image::EncoderInfo* encoder = service.encoder_info(snow::image::Format::png);
    require(encoder != nullptr, "PNG encoder is available for 16-bit preparation test");
    require(snow::image::has_feature(encoder->features, snow::image::EncoderFeature::alpha),
            "PNG encoder advertises alpha for 16-bit preparation test");
    QString warning;
    auto prepared = take(snow::image_viewer::worker_core::prepareForExport(
                             std::move(document), settings, *encoder, &warning),
                         "prepare 16-bit PNG export");
    require(prepared.document.frames.front().image.format() == snow::image::kRgba16,
            "PNG export preparation preserves the native 16-bit layout");
    require(std::equal(expected.begin(), expected.end(),
                       prepared.document.frames.front().image.pixels().begin()),
            "PNG export preparation preserves every native 16-bit sample");
}

void testVerifiedAlphaPreparation() {
    auto opaque = take(snow::image::MutableImage::allocate(4, 4, snow::image::kRgba8),
                       "allocate known-opaque alpha fixture");
    for (std::size_t offset = 0; offset < opaque.pixels().size(); offset += 4U) {
        opaque.pixels()[offset] = std::byte{0x18};
        opaque.pixels()[offset + 1U] = std::byte{0x29};
        opaque.pixels()[offset + 2U] = std::byte{0x3A};
        opaque.pixels()[offset + 3U] = std::byte{0xFF};
    }
    snow::image::Document opaqueDocument;
    opaqueDocument.canvas_width = 4;
    opaqueDocument.canvas_height = 4;
    snow::image::Frame opaqueFrame;
    opaqueFrame.image = std::move(opaque).freeze();
    opaqueDocument.frames.push_back(std::move(opaqueFrame));

    snow::image_viewer::EditExportSettings identity;
    identity.sourceSize = QSize(4, 4);
    identity.width = 4;
    identity.height = 4;
    identity.format = snow::image::Format::png;
    identity.encode.format = identity.format;
    snow::image::Service service;
    const auto* encoder = service.encoder_info(identity.format);
    require(encoder != nullptr, "PNG encoder is available for alpha propagation tests");
    std::stop_source cancelled;
    cancelled.request_stop();
    QString warning;
    const auto knownOpaque = snow::image_viewer::worker_core::prepareForExport(
        opaqueDocument, identity, *encoder, &warning, cancelled.get_token(),
        snow::image::AlphaContent::opaque);
    require(knownOpaque.has_value(),
            "verified opaque preparation does not rescan alpha after cancellation");

    auto transparent = take(snow::image::MutableImage::allocate(4, 4, snow::image::kRgba8),
                            "allocate resized-alpha fixture");
    std::fill(transparent.pixels().begin(), transparent.pixels().end(), std::byte{0xFF});
    transparent.pixels()[3] = std::byte{0};
    snow::image::Document transparentDocument = opaqueDocument;
    transparentDocument.frames.front().image = std::move(transparent).freeze();
    snow::image_viewer::EditExportSettings resized = identity;
    resized.width = 1;
    resized.height = 1;
    resized.resampling = snow::image::ResamplingMethod::nearest;
    resized.reducePalette = true;
    resized.paletteColors = 2;
    resized.ditheringPercent = 0;
    const auto conservative = snow::image_viewer::worker_core::prepareForExport(
        transparentDocument, resized, *encoder, &warning, {},
        snow::image::AlphaContent::non_opaque);
    require(conservative.has_value(), "resized non-opaque preparation completes palette reduction");
    require(!conservative.value().previewEquivalentToBase,
            "resized non-opaque preparation produces a distinct raster");
    require(conservative.value().alphaContent == snow::image::AlphaContent::opaque,
            "resized non-opaque preparation reclassifies the output alpha");
    require(conservative.value().alphaClassificationDuration.count() > 0,
            "resized non-opaque preparation reclassifies instead of trusting source alpha");
}

void testInvalidGpuReadback(const QString& directory) {
    const QString path = directory + QStringLiteral("/sample.png");
    snow::image_viewer::EditPipelineController session;
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(128, 64);
    settings.width = 64;
    settings.height = 32;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;
    QEventLoop loop;
    bool fellBack = false;
    QObject::connect(&session, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&session, settings]() { session.requestEdit(settings); });
    QObject::connect(
        &session, &snow::image_viewer::EditPipelineController::exactRasterRequested, &loop,
        [&](quint64 requestId, const auto&) {
            snow::image_viewer::GpuRasterResult readback;
            readback.pixelSize = QSize(64, 32);
            auto storage = std::make_shared<QByteArray>(63 * 32 * 4, Qt::Uninitialized);
            snow::image_viewer::GpuRasterTile tile{std::move(storage), QRect(0, 0, 63, 32), 63 * 4};
            readback.tiles.push_back(std::move(tile));
            session.submitGpuResizeResult(requestId, std::move(readback));
        });
    QObject::connect(
        &session, &snow::image_viewer::EditPipelineController::exactReady, &loop,
        [&](const snow::image_viewer::ExactEditResult& result) {
            fellBack = result.provenance == snow::image_viewer::RasterProvenance::cpu_reference &&
                       result.previewSource == snow::image_viewer::ExactPreviewSource::base_raster;
            loop.quit();
        });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    session.setGpuSource(path);
    loop.exec();
    require(fellBack, "GPU edit session falls back after incomplete tiled readback data");
}

void testEffectiveEditCache(const QString& directory) {
    snow::image_viewer::EditPipelineOptions options;
    options.cacheBudgetBytes = 8U * 1024U * 1024U;
    snow::image_viewer::EditPipelineController controller(options, nullptr);
    snow::image_viewer::EditExportSettings first;
    first.sourceSize = QSize(128, 64);
    first.width = 64;
    first.height = 32;
    first.format = snow::image::Format::png;
    first.encode.format = first.format;
    first.encode.compression_level = 1;
    snow::image_viewer::EditExportSettings equivalent = first;
    equivalent.maintainAspectRatio = !first.maintainAspectRatio;
    snow::image_viewer::EditExportSettings reencoded = equivalent;
    reencoded.encode.compression_level = 2;

    QEventLoop loop;
    QElapsedTimer cacheTimer;
    int rasterRequests = 0;
    int publications = 0;
    bool cacheHit = false;
    bool rasterCacheHit = false;
    QString failure;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { controller.requestEdit(first); });
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](quint64, const QString& stage, qint64) {
                         if (stage == QStringLiteral("exact.raster_cache_hit")) {
                             rasterCacheHit = true;
                         }
                     });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::exactRasterRequested, &loop,
        [&](quint64 requestId, const snow::image_viewer::EditExportSettings& settings) {
            ++rasterRequests;
            QImage pixels(settings.width, settings.height, QImage::Format_RGBA8888);
            pixels.fill(QColor(0x18, 0x48, 0x88, 0xCC));
            controller.submitGpuResizeResult(requestId, gpuReadback(std::move(pixels)));
        });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         ++publications;
                         if (publications == 1) {
                             cacheTimer.start();
                             controller.requestEdit(equivalent);
                             return;
                         }
                         if (publications == 2) {
                             cacheHit = result.settings == equivalent && rasterRequests == 1 &&
                                        cacheTimer.elapsed() < 100;
                             controller.requestEdit(reencoded);
                             return;
                         }
                         cacheHit = cacheHit && result.settings == reencoded &&
                                    result.previewSource ==
                                        snow::image_viewer::ExactPreviewSource::gpu_raster &&
                                    rasterRequests == 1;
                         loop.quit();
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    controller.setGpuSource(directory + QStringLiteral("/sample.png"));
    loop.exec();
    require(failure.isEmpty() && publications == 3 && cacheHit && rasterCacheHit,
            "effective keys reuse exact output for UI state and GPU rasters for re-encoding");
}

void testGpuRasterPreviewRecovery(const QString& directory) {
    snow::image_viewer::EditPipelineOptions options;
    options.cacheBudgetBytes = 8U * 1024U * 1024U;
    snow::image_viewer::EditPipelineController controller(options, nullptr);
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(128, 64);
    settings.width = 64;
    settings.height = 32;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;
    settings.encode.compression_level = 1;

    QEventLoop loop;
    int exactPublications = 0;
    int rasterRequests = 0;
    bool decodedCodec = false;
    bool recoveredFromRaster = false;
    QString failure;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { controller.requestEdit(settings); });
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](quint64, const QString& stage, qint64) {
                         if (stage == QStringLiteral("exact.decode_codec"))
                             decodedCodec = true;
                         if (stage == QStringLiteral("exact.raster_preview_recovery"))
                             recoveredFromRaster = true;
                     });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::exactRasterRequested, &loop,
        [&](quint64 requestId, const snow::image_viewer::EditExportSettings& requested) {
            ++rasterRequests;
            QImage pixels(requested.width, requested.height, QImage::Format_RGBA8888);
            pixels.fill(QColor(0x24, 0x68, 0xA0, 0xD0));
            controller.submitGpuResizeResult(requestId, gpuReadback(std::move(pixels)));
        });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         ++exactPublications;
                         if (exactPublications == 1) {
                             controller.clearExactPreviewCacheForBenchmark();
                             controller.requestEdit(settings);
                             return;
                         }
                         require(result.previewSource ==
                                         snow::image_viewer::ExactPreviewSource::gpu_raster &&
                                     !result.displayPreview,
                                 "GPU raster recovery preserves the live texture preview contract");
                         loop.quit();
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    controller.setGpuSource(directory + QStringLiteral("/sample.png"));
    loop.exec();
    require(failure.isEmpty() && exactPublications == 2 && rasterRequests == 1 &&
                recoveredFromRaster && !decodedCodec,
            "preview-only cache recovery uses the matching GPU raster without codec decode");
}

void testProgressiveEditController(const QString& directory) {
    const QString path = directory + QStringLiteral("/sample.png");
    snow::image_viewer::EditPipelineController controller;
    snow::image_viewer::EditExportSettings first;
    first.sourceSize = QSize(128, 64);
    first.width = 96;
    first.height = 48;
    first.format = snow::image::Format::png;
    first.encode.format = first.format;
    first.encode.compression_level = 1;
    snow::image_viewer::EditExportSettings latest = first;
    latest.width = 64;
    latest.height = 32;

    QEventLoop loop;
    QElapsedTimer debounce;
    snow::image_viewer::EditRequestId firstId = 0;
    snow::image_viewer::EditRequestId latestId = 0;
    int visualRequests = 0;
    int exactRequests = 0;
    int exactPublications = 0;
    bool reusedGpuPixels = false;
    bool decodedCodec = false;
    bool validatedReceipt = false;
    QString failure;

    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::visualRequested,
                     &loop,
                     [&](snow::image_viewer::EditRequestId requestId,
                         const snow::image_viewer::EditExportSettings&) {
                         ++visualRequests;
                         controller.submitVisualFrame(requestId);
                     });
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](snow::image_viewer::EditRequestId, const QString& stage, qint64) {
                         if (stage == QStringLiteral("exact.decode_codec"))
                             decodedCodec = true;
                         if (stage == QStringLiteral("exact.validate_receipt"))
                             validatedReceipt = true;
                     });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::exactRasterRequested, &loop,
        [&](snow::image_viewer::EditRequestId requestId,
            const snow::image_viewer::EditExportSettings& settings) {
            ++exactRequests;
            require(debounce.elapsed() >= 150, "exact export observes the trailing debounce");
            QImage pixels(settings.width, settings.height, QImage::Format_RGBA8888);
            pixels.fill(QColor(0x32, 0x78, 0xB8, 0xD0));
            if (requestId == firstId) {
                latestId = controller.requestEdit(
                    latest, snow::image_viewer::EditChangeKind::dimension_typing);
                require(latestId > firstId && visualRequests == 2 &&
                            !controller.hasExactResult(first),
                        "new settings immediately invalidate exact output");
                debounce.restart();
                controller.submitGpuResizeResult(requestId, gpuReadback(std::move(pixels)));
                return;
            }
            require(requestId == latestId && settings == latest,
                    "only the newest pending exact settings are requested");
            controller.submitGpuResizeResult(requestId, gpuReadback(std::move(pixels)));
        });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         ++exactPublications;
                         reusedGpuPixels = result.previewSource ==
                                               snow::image_viewer::ExactPreviewSource::gpu_raster &&
                                           !result.displayPreview.has_value();
                         require(result.requestId == latestId && result.settings == latest &&
                                     result.artifact && result.artifact->byteSize() > 0 &&
                                     controller.hasExactResult(latest),
                                 "only the latest exact result becomes saveable");
                         const auto retained = controller.encodedArtifact();
                         require(retained == controller.encodedArtifact(),
                                 "repeated saves reuse the immutable encoded artifact");
                         loop.quit();
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop, [&]() {
            debounce.start();
            firstId =
                controller.requestEdit(first, snow::image_viewer::EditChangeKind::dimension_typing);
            require(firstId != 0 && visualRequests == 1 &&
                        controller.state() == snow::image_viewer::EditPipelineState::VisualReady,
                    "visual requests are immediate and request-ID aware");
        });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    controller.setGpuSource(path);
    loop.exec();
    if (!failure.isEmpty()) {
        std::cerr << "progressive controller error: " << failure.toStdString() << '\n';
    }
    require(failure.isEmpty() && exactRequests == 2 && exactPublications == 1 && reusedGpuPixels &&
                validatedReceipt && !decodedCodec,
            "progressive controller validates the PNG receipt without decoding it");
}

void testGpuResidentImageState() {
    snow::image_viewer::DecodedImage image;
    image.filePath = QStringLiteral("C:/test/static.png");
    image.sourceSize = QSize(32, 16);
    require(image.isValid() && !image.hasCpuPixels(),
            "GPU-resident image identity remains valid without CPU pixels");
    snow::image_viewer::DecodeResult incomplete{image, {}};
    require(!incomplete.succeeded(), "decode results still require an initial CPU payload");
}

void testTileStore() {
    constexpr std::array<std::uint8_t, 16> raw{1, 2, 3, 255, 4,  5,  6,  255,
                                               7, 8, 9, 255, 10, 11, 12, 255};
    QString path;
    {
        QTemporaryFile file;
        file.setAutoRemove(false);
        require(file.open(), "open tile cache fixture");
        path = file.fileName();
        file.close();
    }
    require(QFile::remove(path), "reserve raster tile store path");

    snow::image::DocumentInfo info;
    info.canvas_width = 2;
    info.canvas_height = 2;
    info.frames.push_back({2,
                           2,
                           0,
                           0,
                           {},
                           snow::image::kRgba8,
                           true,
                           {},
                           {},
                           {},
                           snow::image::FrameBlend::source,
                           snow::image::FrameDisposal::keep});
    auto descriptor = take(snow::image::describe_document(info), "describe tile store fixture");
    descriptor.frames.front().layout.alpha = snow::image::AlphaMode::premultiplied;
    auto rasterStore =
        take(snow::image::RasterStore::create(std::filesystem::path(path.toStdU16String()),
                                              std::move(descriptor)),
             "create raster tile store fixture");
    const auto rawBytes = std::as_bytes(std::span(raw));
    take(rasterStore->write_rows(0, 0, 0, 2, 8, rawBytes), "write raster tile store fixture");
    take(rasterStore->commit(), "commit raster tile store fixture");

    {
        snow::image_viewer::ImageTileStore store(path, rasterStore, QSize(2, 2), {}, QSize(1, 1));
        rasterStore.reset();
        const auto selected = store.tilesIntersecting(QRectF(0.5, 0.5, 1.0, 1.0));
        require(selected.size() == 4, "tile store selects only intersecting grid cells");
        const auto boundary = store.tilesIntersecting(QRectF(1.0, 0.0, 1.0, 1.0));
        require(boundary.size() == 1 && boundary.front().sourceRect == QRect(1, 0, 1, 1),
                "tile store excludes the cell before an exact grid boundary");
        QString error;
        const QImage image = store.load(selected.front(), &error);
        require(!image.isNull() && error.isEmpty() && image.sizeInBytes() == 4,
                "tile store reads a cached raster region");
        require(std::memcmp(image.constBits(), raw.data(), 4) == 0,
                "tile store preserves tile bytes exactly");
    }
    require(!QFile::exists(path), "tile cache is removed with its owning store");

    snow::image::Document jpegDocument;
    jpegDocument.canvas_width = 7;
    jpegDocument.canvas_height = 5;
    snow::image::Frame jpegFrame;
    jpegFrame.image = sampleImage(std::byte{0xC0}, 7, 5);
    jpegDocument.frames.push_back(std::move(jpegFrame));
    auto jpegBytes = std::make_shared<std::vector<std::byte>>();
    snow::image::Service service;
    snow::image::EncodeOptions jpegEncode;
    jpegEncode.format = snow::image::Format::jpeg;
    jpegEncode.quality = 95;
    jpegEncode.chroma_subsampling = snow::image::ChromaSubsampling::yuv420;
    take(
        service.encode(jpegDocument, snow::image::memory_output(jpegBytes, "tile.jpg"), jpegEncode),
        "encode native tile-store JPEG fixture");
    QString nativePathString;
    {
        QTemporaryFile file;
        file.setAutoRemove(false);
        require(file.open(), "open native tile cache fixture");
        nativePathString = file.fileName();
        file.close();
    }
    require(QFile::remove(nativePathString), "reserve native raster tile store path");
    snow::image::DecodeOptions nativeOptions;
    nativeOptions.raster_layout = snow::image::RasterLayoutPolicy::native;
    auto nativeStore =
        take(service.decode_to_store(snow::image::memory_input(jpegBytes, "tile.jpg"),
                                     std::filesystem::path(nativePathString.toStdU16String()),
                                     nativeOptions),
             "decode native planar JPEG tile store");
    require(nativeStore->descriptor().frames.front().layout.planes.size() == 3 &&
                nativeStore->descriptor().frames.front().layout.planes.front().width == 8 &&
                nativeStore->descriptor().frames.front().layout.planes.front().height == 6,
            "native JPEG tile store retains padded YCbCr planes");
    {
        snow::image_viewer::ImageTileStore store(nativePathString, nativeStore, QSize(7, 5), {},
                                                 QSize(4, 4));
        nativeStore.reset();
        QString error;
        const QImage image = store.load({QRect(1, 1, 5, 3)}, &error);
        require(!image.isNull() && error.isEmpty() && image.size() == QSize(5, 3) &&
                    image.format() == QImage::Format_RGBA8888_Premultiplied,
                "viewer converts an odd planar JPEG tile to display RGBA");
        for (int row = 0; row < image.height(); ++row) {
            for (int column = 0; column < image.width(); ++column) {
                require(image.constScanLine(row)[column * 4 + 3] == 0xFF,
                        "native JPEG display tiles remain opaque");
            }
        }
    }
    require(!QFile::exists(nativePathString), "native tile cache is removed with its owning store");

    snow::image::MutableImage webpPixels =
        take(snow::image::MutableImage::allocate(3, 3, snow::image::kRgba8),
             "allocate native alpha WebP tile fixture");
    for (std::size_t offset = 0; offset < webpPixels.pixels().size(); offset += 4U) {
        webpPixels.pixels()[offset] = std::byte{0xE0};
        webpPixels.pixels()[offset + 1U] = std::byte{0x80};
        webpPixels.pixels()[offset + 2U] = std::byte{0x40};
        webpPixels.pixels()[offset + 3U] = std::byte{0x80};
    }
    snow::image::Document webpDocument;
    webpDocument.canvas_width = 3;
    webpDocument.canvas_height = 3;
    snow::image::Frame webpFrame;
    webpFrame.image = std::move(webpPixels).freeze();
    webpDocument.frames.push_back(std::move(webpFrame));
    auto webpBytes = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions webpEncode;
    webpEncode.format = snow::image::Format::webp;
    webpEncode.lossless = false;
    webpEncode.quality = 95;
    take(service.encode(webpDocument, snow::image::memory_output(webpBytes, "tile.webp"),
                        webpEncode),
         "encode native alpha WebP tile fixture");
    QString webpPathString;
    {
        QTemporaryFile file;
        file.setAutoRemove(false);
        require(file.open(), "open native WebP tile cache fixture");
        webpPathString = file.fileName();
        file.close();
    }
    require(QFile::remove(webpPathString), "reserve native WebP raster tile store path");
    auto webpStore =
        take(service.decode_to_store(snow::image::memory_input(webpBytes, "tile.webp"),
                                     std::filesystem::path(webpPathString.toStdU16String()),
                                     nativeOptions),
             "decode native planar WebP tile store");
    require(webpStore->descriptor().frames.front().layout.planes.size() == 4 &&
                webpStore->descriptor().frames.front().layout.alpha ==
                    snow::image::AlphaMode::straight,
            "native WebP tile store retains its separate alpha plane");
    {
        snow::image_viewer::ImageTileStore store(webpPathString, webpStore, QSize(3, 3), {},
                                                 QSize(2, 2));
        webpStore.reset();
        QString error;
        const QImage image = store.load({QRect(0, 0, 3, 3)}, &error);
        require(!image.isNull() && error.isEmpty() &&
                    image.format() == QImage::Format_RGBA8888_Premultiplied,
                "viewer converts native WebP YUVA to premultiplied display RGBA");
        for (int row = 0; row < image.height(); ++row) {
            const uchar* pixels = image.constScanLine(row);
            for (int column = 0; column < image.width(); ++column) {
                const int offset = column * 4;
                require(pixels[offset + 3] == 0x80 && pixels[offset] <= pixels[offset + 3] &&
                            pixels[offset + 1] <= pixels[offset + 3] &&
                            pixels[offset + 2] <= pixels[offset + 3],
                        "native WebP display bytes satisfy premultiplied-alpha invariants");
            }
        }
    }
    require(!QFile::exists(webpPathString),
            "native WebP tile cache is removed with its owning store");
}

void testWorkerProtocolAndPackages(const QString& directory) {
    namespace protocol = snow::image_viewer::worker_protocol;
    require(protocol::kVersion == 1, "worker protocol uses checksummed binary framing");
    constexpr std::array protocolMessages{
        protocol::MessageType::ready,         protocol::MessageType::encode_job,
        protocol::MessageType::preview_job,   protocol::MessageType::artifact_ready,
        protocol::MessageType::preview_ready, protocol::MessageType::job_failed,
        protocol::MessageType::cancel,        protocol::MessageType::cancelled,
        protocol::MessageType::shutdown};
    for (const protocol::MessageType message : protocolMessages) {
        QByteArray bytes = protocol::encodeFrame(
            message, {{QStringLiteral("requestId"), QStringLiteral("17")},
                      {QStringLiteral("nonce"), QStringLiteral("protocol-test")}});
        protocol::Frame decoded;
        QString decodeError;
        require(protocol::takeFrame(&bytes, &decoded, &decodeError) && bytes.isEmpty() &&
                    decoded.type == message && decodeError.isEmpty() &&
                    decoded.payload.value(QStringLiteral("requestId")).toString() ==
                        QStringLiteral("17"),
                "every worker protocol binary message round-trips through framing");
    }
    const QByteArray encoded = protocol::encodeFrame(
        protocol::MessageType::ready,
        {{QStringLiteral("protocolVersion"), static_cast<int>(protocol::kVersion)}});
    QByteArray partial = encoded.left(7);
    protocol::Frame frame;
    QString error;
    require(!protocol::takeFrame(&partial, &frame, &error) && error.isEmpty(),
            "worker protocol retains a truncated frame");
    partial.append(encoded.mid(7));
    require(protocol::takeFrame(&partial, &frame, &error) && partial.isEmpty() &&
                frame.type == protocol::MessageType::ready,
            "worker protocol reconstructs a framed message");
    QByteArray malformed = encoded;
    malformed[0] = 'X';
    require(!protocol::takeFrame(&malformed, &frame, &error) && !error.isEmpty(),
            "worker protocol rejects a malformed header");
    QByteArray corruptPayload = encoded;
    corruptPayload[corruptPayload.size() - 1] = static_cast<char>(corruptPayload.back() ^ 0x01);
    error.clear();
    require(!protocol::takeFrame(&corruptPayload, &frame, &error) &&
                error.contains(QStringLiteral("checksum"), Qt::CaseInsensitive),
            "worker protocol rejects payload corruption before decoding");

    snow::image_viewer::EditExportSettings jpegSettings;
    jpegSettings.sourceSize = QSize(7, 5);
    jpegSettings.width = 7;
    jpegSettings.height = 5;
    jpegSettings.format = snow::image::Format::jpeg;
    jpegSettings.encode.format = jpegSettings.format;
    jpegSettings.encode.quality = 85;
    jpegSettings.encode.chroma_subsampling = snow::image::ChromaSubsampling::yuv422;
    snow::image_viewer::EditExportSettings decodedSettings;
    error.clear();
    require(protocol::settingsFromJson(protocol::settingsToJson(jpegSettings), &decodedSettings,
                                       &error) &&
                decodedSettings.encode.chroma_subsampling == snow::image::ChromaSubsampling::yuv422,
            "worker protocol round-trips requested JPEG sampling");
    snow::image_viewer::EditExportSettings webpSettings = jpegSettings;
    webpSettings.format = snow::image::Format::webp;
    webpSettings.encode = {};
    webpSettings.encode.format = webpSettings.format;
    webpSettings.encode.lossless = true;
    webpSettings.encode.lossless_effort = 9;
    webpSettings.encode.preserve_metadata = true;
    require(protocol::settingsFromJson(protocol::settingsToJson(webpSettings), &decodedSettings,
                                       &error) &&
                decodedSettings.encode.lossless && decodedSettings.encode.lossless_effort == 9 &&
                decodedSettings.encode.preserve_metadata,
            "worker protocol round-trips WebP lossless effort and metadata policy");

    snow::image::EncodedArtifactReceipt jpegReceipt;
    jpegReceipt.format = snow::image::Format::jpeg;
    jpegReceipt.canvas_width = 7;
    jpegReceipt.canvas_height = 5;
    jpegReceipt.emitted_frame_count = 1;
    jpegReceipt.emitted_frame_extents.push_back({0, 0, 7, 5});
    jpegReceipt.jpeg_chroma_subsampling = snow::image::ChromaSubsampling::yuv422;
    jpegReceipt.encoder_finalized_and_sink_flushed = true;
    snow::image::EncodedArtifactReceipt decodedReceipt;
    require(
        protocol::receiptFromJson(protocol::receiptToJson(jpegReceipt), &decodedReceipt, &error) &&
            decodedReceipt == jpegReceipt,
        "worker protocol round-trips resolved JPEG receipt sampling");
    QJsonObject malformedReceipt = protocol::receiptToJson(jpegReceipt);
    malformedReceipt.insert(QStringLiteral("resolvedJpegChromaSubsampling"),
                            static_cast<int>(snow::image::ChromaSubsampling::yuv440));
    require(!protocol::receiptFromJson(malformedReceipt, &decodedReceipt, &error),
            "worker protocol rejects malformed JPEG receipt sampling");

    snow::image::Document document;
    document.canvas_width = 4;
    document.canvas_height = 3;
    document.color.primaries = snow::image::ColorPrimaries::srgb;
    document.color.transfer = snow::image::TransferFunction::srgb;
    snow::image::Frame sourceFrame;
    sourceFrame.image = sampleImage(std::byte{0xA4}, 4, 3);
    sourceFrame.color = document.color;
    document.frames.push_back(std::move(sourceFrame));

    snow::image::DocumentInfo sinkInfo;
    sinkInfo.canvas_width = 4;
    sinkInfo.canvas_height = 3;
    sinkInfo.color = document.color;
    sinkInfo.frames.push_back({4, 3, 0, 0, {}, snow::image::kRgba8, true, {}, document.color});
    const auto exerciseUnclaimedSink = [&](const QString& path, bool commit) {
        {
            snow::image_viewer::MappedRasterSink sink(path);
            auto status = sink.begin(sinkInfo);
            if (status)
                status = sink.begin_frame(0, sinkInfo.frames.front());
            const std::size_t stride = 4U * 4U;
            auto storage =
                status ? sink.frame_storage(0, stride, stride * 3U) : std::span<std::byte>{};
            require(status && storage.size() == stride * 3U,
                    "map an unclaimed raster sink fixture");
            std::fill(storage.begin(), storage.end(), std::byte{0x7F});
            if (commit) {
                status = sink.end_frame(0);
                if (status)
                    status = sink.end();
                require(status.has_value(), "commit an unclaimed raster sink fixture");
            }
        }
        if (commit) {
            require(QFileInfo::exists(path) && QFile::remove(path),
                    "committed sinks close their files for atomic publication");
        } else {
            require(!QFileInfo::exists(path), "incomplete mapped raster sinks remove their files");
        }
    };
    exerciseUnclaimedSink(directory + QStringLiteral("/discard-incomplete.raster"), false);
    exerciseUnclaimedSink(directory + QStringLiteral("/discard-complete.raster"), true);

    const QString packagePath = directory + QStringLiteral("/ownership.raster");
    error.clear();
    auto package = snow::image_viewer::MappedRasterPackage::create(
        packagePath, document, &error, {}, snow::image::AlphaContent::opaque);
    require(package && error.isEmpty(), "create mapped raster package");
    auto reopenedPackage = snow::image_viewer::MappedRasterPackage::open(packagePath, &error);
    require(reopenedPackage &&
                reopenedPackage->verifiedAlphaContent() == snow::image::AlphaContent::opaque,
            "verified-file reopening preserves stored alpha analysis");
    reopenedPackage.reset();
    {
        auto asset = snow::image_viewer::RasterAsset::fileBacked(package);
        const QJsonObject transport = asset ? asset->workerTransport() : QJsonObject{};
        require(asset && !asset->isSharedMemory() && asset->byteSize() == package->mappedBytes() &&
                    transport.value(QStringLiteral("kind")).toString() ==
                        QStringLiteral("verified_file") &&
                    transport.value(QStringLiteral("path")).toString() == packagePath,
                "file-backed raster assets expose their source and worker descriptor");
    }
    const QString sharedKey = QStringLiteral("snow-edit-v1-") + QString(32, QLatin1Char('a')) +
                              QLatin1Char('-') + QString(32, QLatin1Char('b'));
    QString sharedError;
    auto sharedPackage = snow::image_viewer::MappedRasterPackage::createShared(
        sharedKey, document, snow::image::AlphaContent::opaque, &sharedError);
    require(sharedPackage && sharedError.isEmpty(), "create a shared-memory raster asset fixture");
    {
        auto asset = snow::image_viewer::RasterAsset::sharedMemory(sharedPackage);
        const QJsonObject transport = asset ? asset->workerTransport() : QJsonObject{};
        require(asset && asset->isSharedMemory() &&
                    asset->byteSize() == sharedPackage->mappedBytes() &&
                    transport.value(QStringLiteral("kind")).toString() ==
                        QStringLiteral("shared_memory") &&
                    transport.value(QStringLiteral("key")).toString() == sharedKey &&
                    transport.value(QStringLiteral("nonce")).toArray().size() == 16,
                "shared-memory raster assets expose their source and worker descriptor");
    }
    sharedPackage.reset();
    snow::image_viewer::EditExportSettings invalidAlphaSettings;
    invalidAlphaSettings.sourceSize = QSize(4, 3);
    invalidAlphaSettings.width = 4;
    invalidAlphaSettings.height = 3;
    invalidAlphaSettings.format = snow::image::Format::png;
    invalidAlphaSettings.encode.format = invalidAlphaSettings.format;
    bool invalidAlphaPublished = false;
    const QJsonObject invalidAlphaResult = snow::image_viewer::worker_core::executeEncodeJob(
        {{QStringLiteral("baseRaster"),
          QJsonObject{{QStringLiteral("kind"), QStringLiteral("verified_file")},
                      {QStringLiteral("path"), packagePath}}},
         {QStringLiteral("artifactPath"), directory + QStringLiteral("/invalid-alpha.png")},
         {QStringLiteral("previewPath"),
          directory + QStringLiteral("/invalid-alpha-preview.raster")},
         {QStringLiteral("verifiedAlphaContent"), 99}},
        invalidAlphaSettings, [](const QJsonObject&) {}, {}, &invalidAlphaPublished);
    require(!invalidAlphaResult.value(QStringLiteral("success")).toBool() &&
                invalidAlphaResult.value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("verified alpha"), Qt::CaseInsensitive) &&
                !invalidAlphaPublished &&
                !QFileInfo::exists(directory + QStringLiteral("/invalid-alpha.png")),
            "worker rejects invalid verified alpha metadata before publication");
    snow::image_viewer::DecodeCancellation cancellation;
    require(package->document() != nullptr,
            "mapped test package exposes a packed compatibility document");
    auto preview = snow::image_viewer::prepareMappedSnowDocument(
        packagePath, *package->document(), QStringLiteral("mapped-test"), cancellation);
    require(preview.succeeded() && !preview.image.pixelsPremultiplied,
            "mapped package creates a straight-alpha preview");
    const QColor retainedColor = preview.image.pixels.pixelColor(0, 0);
    package.reset();
    require(preview.image.pixels.pixelColor(0, 0) == retainedColor,
            "mapped QImage survives package handle release");
    preview = {};
    require(QFile::remove(packagePath),
            "mapped file closes after the final QImage owner releases it");

    const QString malformedPath = directory + QStringLiteral("/malformed.raster");
    QFile malformedFile(malformedPath);
    require(malformedFile.open(QIODevice::WriteOnly) && malformedFile.write("NOTMAGIC", 8) == 8,
            "write malformed package fixture");
    malformedFile.close();
    error.clear();
    require(!snow::image_viewer::MappedRasterPackage::open(malformedPath, &error) &&
                !error.isEmpty(),
            "mapped package rejects truncation");

    const QString incompletePath = directory + QStringLiteral("/incomplete.raster");
    error.clear();
    auto incompletePackage =
        snow::image_viewer::MappedRasterPackage::create(incompletePath, document, &error);
    require(incompletePackage && error.isEmpty(),
            "create package for publication-marker corruption");
    incompletePackage.reset();
    QFile incompleteFile(incompletePath);
    require(incompleteFile.open(QIODevice::ReadWrite) && incompleteFile.seek(12) &&
                incompleteFile.write("\0\0\0\0", 4) == 4,
            "clear raster package publication marker");
    incompleteFile.close();
    error.clear();
    require(!snow::image_viewer::MappedRasterPackage::open(incompletePath, &error) &&
                error.contains(QStringLiteral("incomplete"), Qt::CaseInsensitive),
            "mapped package rejects an unpublished payload");

    const QString invalidBoundsPath = directory + QStringLiteral("/invalid-bounds.raster");
    error.clear();
    auto invalidBoundsPackage =
        snow::image_viewer::MappedRasterPackage::create(invalidBoundsPath, document, &error);
    require(invalidBoundsPackage && error.isEmpty(), "create package for bounds corruption");
    invalidBoundsPackage.reset();
    QFile boundsFile(invalidBoundsPath);
    require(boundsFile.open(QIODevice::ReadWrite), "open package for bounds corruption");
    require(boundsFile.seek(64), "locate binary raster manifest");
    QByteArray manifestByte = boundsFile.read(1);
    require(manifestByte.size() == 1, "read binary raster manifest byte");
    manifestByte[0] = static_cast<char>(manifestByte[0] ^ 0x5a);
    require(boundsFile.seek(64) && boundsFile.write(manifestByte) == 1,
            "corrupt binary raster manifest");
    boundsFile.close();
    error.clear();
    require(!snow::image_viewer::MappedRasterPackage::open(invalidBoundsPath, &error) &&
                error.contains(QStringLiteral("checksum"), Qt::CaseInsensitive),
            "mapped package rejects manifest corruption");

    snow::image::Document outsideCanvas = document;
    outsideCanvas.frames.front().x = 1;
    error.clear();
    require(!snow::image_viewer::MappedRasterPackage::create(
                directory + QStringLiteral("/outside-canvas.raster"), outsideCanvas, &error) &&
                error.contains(QStringLiteral("canvas"), Qt::CaseInsensitive),
            "mapped package rejects a frame outside its canvas");

    const QString invalidLengthPath = directory + QStringLiteral("/invalid-length.raster");
    error.clear();
    auto invalidLengthPackage =
        snow::image_viewer::MappedRasterPackage::create(invalidLengthPath, document, &error);
    require(invalidLengthPackage && error.isEmpty(), "create package for length corruption");
    invalidLengthPackage.reset();
    QFile lengthFile(invalidLengthPath);
    require(lengthFile.open(QIODevice::ReadWrite), "open package for length corruption");
    require(lengthFile.seek(lengthFile.size()) && lengthFile.write("x", 1) == 1,
            "extend package beyond its declared file size");
    lengthFile.close();
    error.clear();
    require(!snow::image_viewer::MappedRasterPackage::open(invalidLengthPath, &error) &&
                error.contains(QStringLiteral("sizes"), Qt::CaseInsensitive),
            "mapped package requires an exact declared file size");

    snow::image::Document invalidColor = document;
    invalidColor.color.source_peak_nits = std::numeric_limits<float>::quiet_NaN();
    error.clear();
    require(!snow::image_viewer::MappedRasterPackage::create(
                directory + QStringLiteral("/invalid-color.raster"), invalidColor, &error) &&
                error.contains(QStringLiteral("color"), Qt::CaseInsensitive),
            "mapped package rejects non-finite color metadata");
}

void testMappedPreviewOwnership(const QString& directory) {
    QImage retained;
    QColor expected;
    QString failure;
    bool cacheCleared = false;
    {
        snow::image_viewer::EditPipelineController controller;
        snow::image_viewer::EditExportSettings settings;
        settings.sourceSize = QSize(128, 64);
        settings.width = 48;
        settings.height = 24;
        settings.format = snow::image::Format::webp;
        settings.encode.format = settings.format;
        settings.encode.lossless = false;
        settings.encode.quality = 60;
        settings.encode.effort = 4;
        QEventLoop loop;
        QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady,
                         &loop, [&]() { controller.requestEdit(settings); });
        QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady,
                         &loop, [&](const snow::image_viewer::ExactEditResult& result) {
                             if (!result.displayPreview || result.displayPreview->pixels.isNull() ||
                                 result.previewSource !=
                                     snow::image_viewer::ExactPreviewSource::codec_artifact) {
                                 failure = QStringLiteral("missing mapped codec preview");
                             } else {
                                 retained = result.displayPreview->pixels;
                                 expected = retained.pixelColor(0, 0);
                             }
                             loop.quit();
                         });
        QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                         [&](const QString& message) {
                             failure = message;
                             loop.quit();
                         });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        controller.setSource(directory + QStringLiteral("/sample.png"));
        loop.exec();
        controller.clearExactArtifactCacheForBenchmark();
        cacheCleared = !controller.encodedArtifact();
    }
    require(failure.isEmpty() && cacheCleared && !retained.isNull() &&
                retained.pixelColor(0, 0) == expected,
            "mapped preview survives worker exit, cache eviction, and controller teardown");
    retained = {};
}

void testSplitArtifactAndPreviewReadiness(const QString& directory) {
    snow::image_viewer::EditPipelineOptions options;
    options.workerTestMode = QStringLiteral("preview-failure");
    snow::image_viewer::EditPipelineController controller(options, nullptr);
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(128, 64);
    settings.width = 80;
    settings.height = 40;
    settings.format = snow::image::Format::jpeg;
    settings.encode.format = settings.format;
    settings.encode.quality = 75;
    settings.encode.progressive = true;

    QEventLoop loop;
    quint64 firstRequest = 0;
    quint64 recoveryRequest = 0;
    quint64 secondRecoveryRequest = 0;
    int artifactPublications = 0;
    int encodeStages = 0;
    int exactRecoveries = 0;
    bool initialArtifactUsable = false;
    bool previewFailureWasNonFatal = false;
    bool recoveryWasExact = false;
    QString artifactPath;
    QString failure;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { firstRequest = controller.requestEdit(settings); });
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](quint64, const QString& stage, qint64) {
                         if (stage == QStringLiteral("exact.encode"))
                             ++encodeStages;
                     });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::artifactReady, &loop,
        [&](const snow::image_viewer::EncodedEditResult& result) {
            ++artifactPublications;
            const bool saveReady =
                result.isValid() && !controller.isBusy() &&
                controller.state() == snow::image_viewer::EditPipelineState::ArtifactReady &&
                controller.hasEncodedArtifact(settings) && !controller.hasExactPreview(settings) &&
                controller.isPreviewPending() && QFileInfo::exists(result.artifact->path());
            if (result.requestId == firstRequest) {
                initialArtifactUsable = saveReady;
                artifactPath = result.artifact->path();
            } else if (result.requestId == recoveryRequest ||
                       result.requestId == secondRecoveryRequest) {
                initialArtifactUsable =
                    initialArtifactUsable && saveReady && result.artifact->path() == artifactPath;
            }
        });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::previewUnavailable, &loop,
        [&](quint64 requestId, const QString& message) {
            if (requestId != firstRequest)
                return;
            previewFailureWasNonFatal =
                message.contains(QStringLiteral("preview failure"), Qt::CaseInsensitive) &&
                controller.state() == snow::image_viewer::EditPipelineState::ArtifactReady &&
                controller.hasEncodedArtifact(settings) && !controller.hasExactPreview(settings) &&
                !controller.isPreviewPending();
            recoveryRequest = controller.requestEdit(settings);
        });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
        [&](const snow::image_viewer::ExactEditResult& result) {
            if (result.requestId != recoveryRequest && result.requestId != secondRecoveryRequest)
                return;
            const bool exact =
                result.displayPreview.has_value() && result.displayPreview->pixels.isNull() &&
                result.displayPreview->rasterStore && result.displayPreview->tileStore &&
                result.displayPreview->sourceSize == QSize(80, 40) &&
                result.previewSource == snow::image_viewer::ExactPreviewSource::codec_artifact &&
                controller.state() == snow::image_viewer::EditPipelineState::ExactReady &&
                controller.hasExactPreview(settings) && !controller.isPreviewPending() &&
                controller.encodedArtifact() &&
                controller.encodedArtifact()->path() == artifactPath;
            recoveryWasExact = recoveryWasExact || exact;
            if (exact)
                ++exactRecoveries;
            if (result.requestId == recoveryRequest) {
                controller.clearExactPreviewCacheForBenchmark();
                secondRecoveryRequest = controller.requestEdit(settings);
                return;
            }
            loop.quit();
        });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    controller.setSource(directory + QStringLiteral("/sample.png"));
    loop.exec();
    require(failure.isEmpty() && artifactPublications == 3 && encodeStages == 1 &&
                exactRecoveries == 2 && initialArtifactUsable && previewFailureWasNonFatal &&
                recoveryWasExact,
            "JPEG artifact readiness is independent and repeated preview-only recovery is native "
            "without re-encode");
}

void testCancelAfterArtifactPublication(const QString& directory) {
    snow::image_viewer::EditPipelineOptions options;
    options.workerTestMode = QStringLiteral("block-preview-cooperative");
    snow::image_viewer::EditPipelineController controller(options, nullptr);
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(128, 64);
    settings.width = 72;
    settings.height = 36;
    settings.format = snow::image::Format::webp;
    settings.encode.format = settings.format;
    settings.encode.lossless = false;
    settings.encode.quality = 68;
    settings.encode.effort = 4;

    QEventLoop loop;
    quint64 firstRequest = 0;
    quint64 recoveryRequest = 0;
    int encodeStages = 0;
    QString artifactPath;
    QString failure;
    bool recovered = false;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { firstRequest = controller.requestEdit(settings); });
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](quint64, const QString& stage, qint64) {
                         if (stage == QStringLiteral("exact.encode"))
                             ++encodeStages;
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::artifactReady, &loop,
                     [&](const snow::image_viewer::EncodedEditResult& result) {
                         if (result.requestId == firstRequest) {
                             artifactPath = result.artifact->path();
                             controller.cancel();
                             QTimer::singleShot(0, &loop, [&]() {
                                 recoveryRequest = controller.requestEdit(settings);
                             });
                         } else if (result.requestId == recoveryRequest) {
                             recovered = result.artifact &&
                                         result.artifact->path() == artifactPath &&
                                         QFileInfo::exists(artifactPath) && !controller.isBusy();
                         }
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady,
                     &loop, [&](const snow::image_viewer::ExactEditResult& result) {
                         if (result.requestId != recoveryRequest)
                             return;
                         recovered = recovered && result.displayPreview.has_value() &&
                                     controller.hasExactPreview(settings);
                         loop.quit();
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    controller.setSource(directory + QStringLiteral("/sample.png"));
    loop.exec();
    require(failure.isEmpty() && recovered && encodeStages == 1 &&
                controller.cancellationCount() >= 1,
            "cancelling a pending preview preserves and recovers the published artifact");
}

void testResourceAdmission(const QString& directory) {
    snow::image::DocumentInfo overflow;
    overflow.canvas_width = std::numeric_limits<std::uint32_t>::max();
    overflow.canvas_height = std::numeric_limits<std::uint32_t>::max();
    overflow.frames.push_back({std::numeric_limits<std::uint32_t>::max(),
                               std::numeric_limits<std::uint32_t>::max(),
                               0,
                               0,
                               {},
                               snow::image::kRgba32Float,
                               true,
                               {}});
    snow::image::TransformOptions transform;
    transform.resize = snow::image::ResizeOptions{std::numeric_limits<std::uint32_t>::max(),
                                                  std::numeric_limits<std::uint32_t>::max()};
    require(!snow::image::estimate_transform_resources(overflow, transform),
            "resource estimates reject arithmetic overflow");

    snow::image_viewer::EditPipelineOptions options;
    options.memoryBudgetBytes = 1024U * 1024U;
    options.cacheBudgetBytes = 0;
    snow::image_viewer::EditPipelineController controller(options, nullptr);
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(128, 64);
    settings.width = 128;
    settings.height = 64;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;
    QEventLoop loop;
    bool rejected = false;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactRasterRequested,
                     &loop, [&](quint64 requestId, const auto&) {
                         QImage pixels(128, 64, QImage::Format_RGBA8888);
                         pixels.fill(Qt::red);
                         controller.submitGpuResizeResult(requestId,
                                                          gpuReadback(std::move(pixels)));
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         rejected = message.contains(QStringLiteral("requires")) &&
                                    message.contains(QStringLiteral("available")) &&
                                    message.contains(QStringLiteral("live preview"));
                         loop.quit();
                     });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    controller.setGpuSource(directory + QStringLiteral("/sample.png"));
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { controller.requestEdit(settings); });
    loop.exec();
    require(rejected && !controller.encodedArtifact(),
            "irreducible exact work is rejected before worker dispatch");
}

void testNonCooperativeCancellation(const QString& directory) {
    snow::image_viewer::EditPipelineOptions options;
    options.workerTestMode = QStringLiteral("block-noncooperative");
    options.workerTimeoutMs = 5000;
    snow::image_viewer::EditPipelineController controller(options, nullptr);
    snow::image_viewer::EditExportSettings first;
    first.sourceSize = QSize(128, 64);
    first.width = 96;
    first.height = 48;
    first.format = snow::image::Format::webp;
    first.encode.format = first.format;
    auto latest = first;
    latest.width = 64;
    latest.height = 32;
    QEventLoop loop;
    QElapsedTimer supersede;
    quint64 firstId = 0;
    quint64 latestId = 0;
    bool dispatchedWithinLimit = false;
    bool publishedLatest = false;
    int rasterRequests = 0;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactRasterRequested,
                     &loop, [&](quint64 requestId, const auto& settings) {
                         ++rasterRequests;
                         if (requestId == latestId)
                             dispatchedWithinLimit = supersede.elapsed() < 100;
                         QImage pixels(settings.width, settings.height, QImage::Format_RGBA8888);
                         pixels.fill(Qt::green);
                         controller.submitGpuResizeResult(requestId,
                                                          gpuReadback(std::move(pixels)));
                         if (requestId == firstId) {
                             QTimer::singleShot(80, &loop, [&]() {
                                 supersede.start();
                                 latestId = controller.requestEdit(latest);
                             });
                         }
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         publishedLatest = result.requestId == latestId;
                         loop.quit();
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         std::cerr << "cancellation test error: " << message.toStdString() << '\n';
                         loop.quit();
                     });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    controller.setGpuSource(directory + QStringLiteral("/sample.png"));
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { firstId = controller.requestEdit(first); });
    loop.exec();
    require(rasterRequests == 2 && dispatchedWithinLimit && publishedLatest &&
                controller.cancellationCount() >= 1,
            "non-cooperative worker cancellation dispatches the latest job within 100 ms");
}

void testCooperativeCancellation(const QString& directory) {
    snow::image_viewer::EditPipelineOptions options;
    options.workerTestMode = QStringLiteral("block-cooperative");
    options.workerTimeoutMs = 5000;
    snow::image_viewer::EditPipelineController controller(options, nullptr);
    snow::image_viewer::EditExportSettings first;
    first.sourceSize = QSize(128, 64);
    first.width = 96;
    first.height = 48;
    first.format = snow::image::Format::jpeg;
    first.encode.format = first.format;
    first.encode.quality = 75;
    first.encode.progressive = true;
    snow::image::Service service;
    const auto* jpegEncoder = service.encoder_info(snow::image::Format::jpeg);
    require(jpegEncoder && jpegEncoder->cancellation == snow::image::CodecCancellation::cooperative,
            "JPEG advertises cooperative worker cancellation");
    auto latest = first;
    latest.width = 64;
    latest.height = 32;
    QEventLoop loop;
    quint64 firstId = 0;
    quint64 latestId = 0;
    bool acknowledged = false;
    bool replacementDispatched = false;
    bool publishedLatest = false;
    QString failure;
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
        [&](quint64 requestId, const QString& stage, qint64 nanoseconds) {
            if (stage == QStringLiteral("worker.cooperative_cancellation"))
                acknowledged = nanoseconds >= 0 && nanoseconds < 100'000'000;
            if (stage == QStringLiteral("worker.replacement_dispatch"))
                replacementDispatched =
                    requestId == latestId && nanoseconds >= 0 && nanoseconds < 100'000'000;
            if (requestId == firstId && stage == QStringLiteral("worker.job_dispatched")) {
                QTimer::singleShot(20, &loop, [&]() { latestId = controller.requestEdit(latest); });
            }
        });
    QObject::connect(
        &controller, &snow::image_viewer::EditPipelineController::exactRasterRequested, &loop,
        [&](quint64 requestId, const snow::image_viewer::EditExportSettings& settings) {
            QImage pixels(settings.width, settings.height, QImage::Format_RGBA8888);
            pixels.fill(Qt::cyan);
            controller.submitGpuResizeResult(requestId, gpuReadback(std::move(pixels)));
        });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         publishedLatest = result.requestId == latestId;
                         loop.quit();
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    controller.setGpuSource(directory + QStringLiteral("/sample.png"));
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { firstId = controller.requestEdit(first); });
    loop.exec();
    if (!acknowledged || !replacementDispatched || !publishedLatest ||
        controller.workerRestartCount() != 0) {
        std::cerr << "cooperative cancellation diagnostics: acknowledged=" << acknowledged
                  << " replacementDispatched=" << replacementDispatched
                  << " publishedLatest=" << publishedLatest
                  << " restarts=" << controller.workerRestartCount()
                  << " cancellations=" << controller.cancellationCount()
                  << " failure=" << failure.toStdString() << '\n';
    }
    require(acknowledged && replacementDispatched && publishedLatest &&
                controller.workerRestartCount() == 0,
            "cooperative cancellation acknowledges and dispatches its replacement without "
            "restarting the worker");
}

void testJxlWorkerRetirement(const QString& directory) {
    snow::image_viewer::EditPipelineController controller({}, nullptr);
    snow::image_viewer::EditExportSettings first;
    first.sourceSize = QSize(128, 64);
    first.width = 128;
    first.height = 64;
    first.format = snow::image::Format::jxl;
    first.encode.format = first.format;
    first.encode.quality = 80;
    first.encode.effort = 1;
    auto second = first;
    second.encode.quality = 81;

    QEventLoop loop;
    quint64 firstId = 0;
    quint64 secondId = 0;
    bool secondReady = false;
    QString failure;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { firstId = controller.requestEdit(first); });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         if (result.requestId == firstId) {
                             controller.clearAllCachesForBenchmark();
                             secondId = controller.requestEdit(second);
                         } else if (result.requestId == secondId) {
                             secondReady = result.artifact && result.artifact->byteSize() > 0;
                             loop.quit();
                         }
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(10'000, &loop, &QEventLoop::quit);
    controller.setSource(directory + QStringLiteral("/sample.png"));
    loop.exec();
    require(failure.isEmpty() && secondReady && controller.workerRestartCount() >= 1,
            "JPEG XL jobs use a fresh worker after a completed encode");
}

void testWorkerFailureModes(const QString& directory) {
    const auto runMode = [&](const QString& mode, bool expectReady, const QString& expectedFailure,
                             int workerTimeoutMs) {
        snow::image_viewer::EditPipelineOptions options;
        options.workerTestMode = mode;
        options.workerTimeoutMs = workerTimeoutMs;
        snow::image_viewer::EditPipelineController controller(options, nullptr);
        snow::image_viewer::EditExportSettings settings;
        settings.sourceSize = QSize(64, 32);
        settings.width = 32;
        settings.height = 16;
        settings.format = snow::image::Format::png;
        settings.encode.format = settings.format;
        QEventLoop loop;
        bool ready = false;
        QString failure;
        QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady,
                         &loop, [&]() { controller.requestEdit(settings); });
        QObject::connect(
            &controller, &snow::image_viewer::EditPipelineController::exactRasterRequested, &loop,
            [&](quint64 requestId, const auto& requested) {
                QImage pixels(requested.width, requested.height, QImage::Format_RGBA8888);
                pixels.fill(Qt::cyan);
                controller.submitGpuResizeResult(requestId, gpuReadback(std::move(pixels)));
            });
        QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady,
                         &loop, [&](const auto&) {
                             ready = true;
                             loop.quit();
                         });
        QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                         [&](const QString& message) {
                             failure = message;
                             loop.quit();
                         });
        QTimer::singleShot(4000, &loop, &QEventLoop::quit);
        controller.setGpuSource(directory + QStringLiteral("/sample.png"));
        loop.exec();
        require(ready == expectReady &&
                    (expectReady ? failure.isEmpty() : failure.contains(expectedFailure)),
                "worker fault mode produces the expected current-job outcome");
    };

    runMode(QStringLiteral("stale-result"), true, {}, 2000);
    runMode(QStringLiteral("partial-artifact"), false, QStringLiteral("partial artifact"), 2000);
    runMode(QStringLiteral("crash"), false, QStringLiteral("exited unexpectedly"), 2000);
    runMode(QStringLiteral("block-noncooperative"), false, QStringLiteral("timed out"), 100);
}

void testSharedMemoryAttachRetry(const QString& directory) {
    snow::image_viewer::EditPipelineOptions options;
    options.workerTestMode = QStringLiteral("shared-attach-failure");
    options.rasterHandoffMode = snow::image_viewer::RasterHandoffMode::shared_memory;
    options.cacheBudgetBytes = 1;
    options.workerTimeoutMs = 5000;
    snow::image_viewer::EditPipelineController controller(options, nullptr);
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(64, 32);
    settings.width = 32;
    settings.height = 16;
    settings.format = snow::image::Format::png;
    settings.encode.format = settings.format;

    QEventLoop loop;
    bool ready = false;
    bool failed = false;
    bool sawSharedRaster = false;
    QString failure;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { controller.requestEdit(settings); });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactRasterRequested,
                     &loop, [&](quint64 requestId, const auto& requested) {
                         QImage pixels(requested.width, requested.height, QImage::Format_RGBA8888);
                         pixels.fill(Qt::cyan);
                         controller.submitGpuResizeResult(requestId,
                                                          gpuReadback(std::move(pixels)));
                     });
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](quint64, const QString& stage, qint64) {
                         if (stage == QStringLiteral("worker.job_dispatched") &&
                             controller.sharedRasterBytes() > 0)
                             sawSharedRaster = true;
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const auto& result) {
                         ready = result.artifact && result.artifact->byteSize() > 0;
                         loop.quit();
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failed = true;
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(10'000, &loop, &QEventLoop::quit);
    controller.setGpuSource(directory + QStringLiteral("/sample.png"));
    loop.exec();
    require(!failed && failure.isEmpty() && ready && sawSharedRaster &&
                controller.sharedRasterBytes() == 0 && controller.temporaryFileCount() == 1,
            "shared-memory attach failure retries once through a verified file and cleans up");
}

void testExportKeyNormalization() {
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(64, 32);
    settings.width = 64;
    settings.height = 32;
    settings.format = snow::image::Format::jpeg;
    settings.encode.format = settings.format;
    settings.encode.quality = 0;
    const auto clampedZero = snow::image_viewer::exportKey(settings, 7);
    settings.encode.quality = 1;
    const auto clampedOne = snow::image_viewer::exportKey(settings, 7);
    require(clampedZero == clampedOne, "JPEG quality 0 and 1 share one normalized export key");

    settings.encode.quality = 75;
    settings.encode.chroma_subsampling.reset();
    const auto automatic420 = snow::image_viewer::exportKey(settings, 7);
    settings.encode.chroma_subsampling = snow::image::ChromaSubsampling::yuv420;
    require(automatic420 == snow::image_viewer::exportKey(settings, 7),
            "JPEG Auto and manual 4:2:0 share a key when they encode identically");
    settings.encode.chroma_subsampling = snow::image::ChromaSubsampling::yuv444;
    require(automatic420 != snow::image_viewer::exportKey(settings, 7),
            "different resolved JPEG sampling modes use distinct export keys");

    settings.format = snow::image::Format::webp;
    settings.encode = {};
    settings.encode.format = settings.format;
    settings.encode.lossless = true;
    settings.encode.lossless_effort = 6;
    settings.encode.quality = 10;
    const auto losslessSix = snow::image_viewer::exportKey(settings, 7);
    settings.encode.quality = 99;
    require(losslessSix == snow::image_viewer::exportKey(settings, 7),
            "lossless WebP quality is normalized out of export keys");
    settings.encode.lossless_effort = 9;
    require(losslessSix != snow::image_viewer::exportKey(settings, 7),
            "WebP lossless compression presets isolate artifact cache keys");
    settings.encode.lossless = false;
    settings.encode.quality = 75;
    settings.encode.effort = 4;
    settings.encode.lossless_effort = 0;
    const auto lossy = snow::image_viewer::exportKey(settings, 7);
    settings.encode.lossless_effort = 9;
    require(lossy == snow::image_viewer::exportKey(settings, 7),
            "lossy WebP lossless effort is normalized out of export keys");
    settings.encode.preserve_metadata = false;
    const auto stripped = snow::image_viewer::exportKey(settings, 7);
    settings.encode.preserve_metadata = true;
    require(stripped != snow::image_viewer::exportKey(settings, 7),
            "WebP metadata policy isolates artifact cache keys");
    settings.format = snow::image::Format::jpeg;
    settings.encode.format = settings.format;
    settings.encode.preserve_metadata = false;
    const auto jpegStripped = snow::image_viewer::exportKey(settings, 7);
    settings.encode.preserve_metadata = true;
    require(jpegStripped == snow::image_viewer::exportKey(settings, 7),
            "unsupported JPEG metadata preservation normalizes to one cache key");

    settings.format = snow::image::Format::webp;
    settings.encode.format = settings.format;
    settings.width = 16'383;
    settings.height = 1;
    require(settings.isValid(), "viewer accepts WebP dimensions at the codec boundary");
    settings.width = 16'384;
    require(!settings.isValid(), "viewer rejects WebP dimensions above the codec boundary");
}

void testWebpNativeRoutingAndArtifactReuse(const QString& directory) {
    const QString sourcePath = directory + QStringLiteral("/native-source.webp");
    snow::image::MutableImage pixels =
        take(snow::image::MutableImage::allocate(17, 9, snow::image::kRgba8),
             "allocate native WebP viewer source");
    for (std::uint32_t y = 0; y < pixels.height(); ++y) {
        std::byte* row = pixels.pixels().data() + static_cast<std::size_t>(y) * pixels.row_stride();
        for (std::uint32_t x = 0; x < pixels.width(); ++x) {
            const std::size_t offset = static_cast<std::size_t>(x) * 4U;
            row[offset] = static_cast<std::byte>((x * 13U + y * 3U) & 0xffU);
            row[offset + 1U] = static_cast<std::byte>((x * 5U + y * 17U) & 0xffU);
            row[offset + 2U] = static_cast<std::byte>((x * 19U + y * 7U) & 0xffU);
            row[offset + 3U] = static_cast<std::byte>(64U + ((x * 11U + y * 23U) % 192U));
        }
    }
    snow::image::Document document;
    document.format = snow::image::Format::webp;
    document.canvas_width = pixels.width();
    document.canvas_height = pixels.height();
    snow::image::Frame frame;
    frame.image = std::move(pixels).freeze();
    document.frames.push_back(std::move(frame));
    snow::image::EncodeOptions sourceOptions;
    sourceOptions.format = snow::image::Format::webp;
    sourceOptions.quality = 90;
    sourceOptions.effort = 4;
    snow::image::Service service;
    auto sourceOutput =
        take(snow::image::file_output(std::filesystem::path(sourcePath.toStdU16String())),
             "open native WebP viewer source");
    require(service.encode(document, sourceOutput, sourceOptions).has_value(),
            "encode native WebP viewer source");
    sourceOutput.sink.reset();

    snow::image_viewer::EditPipelineController controller;
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(17, 9);
    settings.width = 17;
    settings.height = 9;
    settings.format = snow::image::Format::webp;
    settings.encode.format = settings.format;
    settings.encode.quality = 75;
    settings.encode.effort = 4;
    settings.encode.preserve_metadata = true;
    const auto decodedSource = snow::image_viewer::ImageLoader::decodeSynchronously(sourcePath);
    const auto sourceRoute =
        decodedSource.image.rasterStore
            ? service.raster_encode_route(decodedSource.image.rasterStore->store()->descriptor(),
                                          settings.encode)
            : snow::image::Result<snow::image::RasterEncodeRoute>(
                  snow::image::Status::error(snow::image::ErrorCode::internal_error,
                                             "The native WebP test source has no raster store."));
    QEventLoop loop;
    quint64 firstId = 0;
    quint64 secondId = 0;
    int rasterRequests = 0;
    bool directStage = false;
    bool correctReceipt = false;
    bool correctAlpha = false;
    bool codecPreview = false;
    bool reused = false;
    QString failure;
    QStringList performanceStages;
    std::shared_ptr<const snow::image_viewer::EncodedArtifact> firstArtifact;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { firstId = controller.requestEdit(settings); });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactRasterRequested,
                     &loop, [&](quint64, const auto&) { ++rasterRequests; });
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](quint64, const QString& stage, qint64) {
                         performanceStages.push_back(stage);
                         if (stage == QStringLiteral("exact.direct_native_encode"))
                             directStage = true;
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         if (result.requestId == firstId) {
                             firstArtifact = result.artifact;
                             if (result.artifact) {
                                 const auto& receipt = result.artifact->receipt();
                                 correctReceipt = receipt.format == snow::image::Format::webp &&
                                                  receipt.canvas_width == 17 &&
                                                  receipt.canvas_height == 9 &&
                                                  receipt.emitted_frame_count == 1 &&
                                                  receipt.emitted_frame_extents.size() == 1 &&
                                                  receipt.emitted_frame_extents.front() ==
                                                      snow::image::EncodedFrameExtent{0, 0, 17, 9};
                             }
                             codecPreview = result.previewSource ==
                                            snow::image_viewer::ExactPreviewSource::codec_artifact;
                             correctAlpha =
                                 result.alphaContent == snow::image::AlphaContent::non_opaque;
                             secondId = controller.requestEdit(settings);
                         } else if (result.requestId == secondId) {
                             reused = result.artifact == firstArtifact;
                             loop.quit();
                         }
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QTimer::singleShot(15'000, &loop, &QEventLoop::quit);
    controller.setSource(sourcePath);
    loop.exec();
    if (!failure.isEmpty() || rasterRequests != 0 || !directStage || !correctReceipt ||
        !correctAlpha || !codecPreview || !reused) {
        std::cerr << "native WebP route: failure='" << failure.toStdString()
                  << "', rasterRequests=" << rasterRequests << ", directStage=" << directStage
                  << ", receipt=" << correctReceipt << ", alpha=" << correctAlpha
                  << ", codecPreview=" << codecPreview << ", reused=" << reused
                  << ", sourceRoute=" << (sourceRoute ? static_cast<int>(sourceRoute.value()) : -1)
                  << ", stages=" << performanceStages.join(QLatin1Char(',')).toStdString() << '\n';
    }
    require(sourceRoute && sourceRoute.value() == snow::image::RasterEncodeRoute::native &&
                failure.isEmpty() && rasterRequests == 0 && directStage && correctReceipt &&
                correctAlpha && codecPreview && reused,
            "identity WebP export uses native YUVA without GPU readback and reuses its artifact");
}

void testJpegNativePreviewAndArtifactReuse(const QString& directory) {
    snow::image_viewer::EditPipelineController controller;
    snow::image_viewer::EditExportSettings settings;
    settings.sourceSize = QSize(17, 9);
    settings.width = 17;
    settings.height = 9;
    settings.format = snow::image::Format::jpeg;
    settings.encode.format = settings.format;
    settings.encode.quality = 75;
    settings.encode.progressive = true;
    settings.encode.preserve_metadata = false;

    QEventLoop loop;
    quint64 firstId = 0;
    quint64 secondId = 0;
    std::shared_ptr<const snow::image_viewer::EncodedArtifact> firstArtifact;
    bool nativePreview = false;
    bool blackComposite = false;
    bool reused = false;
    QString failure;
    QStringList stages;
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::sourceReady, &loop,
                     [&]() { firstId = controller.requestEdit(settings); });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactRasterRequested,
                     &loop, [&](quint64 requestId, const auto& requested) {
                         stages.push_back(QStringLiteral("raster:%1").arg(requestId));
                         QImage pixels(requested.width, requested.height, QImage::Format_RGBA8888);
                         pixels.fill(QColor(255, 0, 0, 0));
                         controller.submitGpuResizeResult(requestId,
                                                          gpuReadback(std::move(pixels)));
                     });
    QObject::connect(&controller,
                     &snow::image_viewer::EditPipelineController::performanceStageCompleted, &loop,
                     [&](quint64, const QString& stage, qint64) { stages.push_back(stage); });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::exactReady, &loop,
                     [&](const snow::image_viewer::ExactEditResult& result) {
                         if (result.requestId == firstId) {
                             firstArtifact = result.artifact;
                             nativePreview = result.displayPreview &&
                                             result.displayPreview->sourceSize == QSize(17, 9) &&
                                             result.displayPreview->pixels.isNull() &&
                                             result.displayPreview->rasterStore &&
                                             result.displayPreview->tileStore && firstArtifact &&
                                             firstArtifact->receipt().jpeg_chroma_subsampling ==
                                                 snow::image::ChromaSubsampling::yuv420;
                             snow::image::Service service;
                             auto decoded = firstArtifact
                                                ? service.decode(firstArtifact->input())
                                                : snow::image::Result<snow::image::Document>(
                                                      snow::image::Status::error(
                                                          snow::image::ErrorCode::invalid_argument,
                                                          "JPEG artifact is unavailable."));
                             if (decoded) {
                                 const auto pixels = decoded.value().frames.front().image.pixels();
                                 blackComposite = pixels.size() >= 3 &&
                                                  std::to_integer<unsigned>(pixels[0]) <= 4 &&
                                                  std::to_integer<unsigned>(pixels[1]) <= 4 &&
                                                  std::to_integer<unsigned>(pixels[2]) <= 4;
                             }
                             secondId = controller.requestEdit(settings);
                         } else if (result.requestId == secondId) {
                             reused = result.artifact == firstArtifact;
                             loop.quit();
                         }
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::failed, &loop,
                     [&](const QString& message) {
                         failure = message;
                         loop.quit();
                     });
    QObject::connect(&controller, &snow::image_viewer::EditPipelineController::previewUnavailable,
                     &loop, [&](quint64, const QString& message) {
                         failure = QStringLiteral("preview unavailable: ") + message;
                         loop.quit();
                     });
    QTimer::singleShot(10'000, &loop, &QEventLoop::quit);
    controller.setGpuSource(directory + QStringLiteral("/sample.png"));
    loop.exec();
    if (!failure.isEmpty())
        std::cerr << "JPEG native preview error: " << failure.toStdString() << '\n';
    if (!nativePreview || !blackComposite || !reused) {
        std::cerr << "JPEG native preview state: preview=" << nativePreview
                  << ", black=" << blackComposite << ", reused=" << reused
                  << ", firstId=" << firstId << ", secondId=" << secondId
                  << ", restarts=" << controller.workerRestartCount()
                  << ", stages=" << stages.join(QLatin1Char(',')).toStdString() << '\n';
    }
    require(failure.isEmpty() && nativePreview && blackComposite && reused,
            "JPEG export uses black compositing, native exact preview, and artifact reuse");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        const snow::image_viewer::DecodeResult decoded =
            snow::image_viewer::ImageLoader::decodeSynchronously(path);
        require(decoded.succeeded(), "decode oversized smoke-test image");
        require(decoded.image.tileStore != nullptr,
                "oversized smoke-test image uses the bounded tile store");
        std::cout << "tiled image " << decoded.image.sourceSize.width() << 'x'
                  << decoded.image.sourceSize.height() << ", preview "
                  << decoded.image.pixels.width() << 'x' << decoded.image.pixels.height() << '\n';
        return 0;
    }
    QTemporaryDir directory;
    require(directory.isValid(), "create viewer test directory");
    testSuffixes();
    testGpuRasterTileContract();
    testTextureTilePlanning();
    testDecoder(directory.path());
    testSharedSourceRasterPipeline(directory.path());
    testSharedSourceRasterPreserves16Bit(directory.path());
    testPremultipliedRasterIsNotSourceExact();
    testEditSession(directory.path());
    testGpuEditEncoding(directory.path());
    testTiledGpuEditEncoding(directory.path());
    testPreparedPreviewSource(directory.path());
    testPngPreparationPreserves16Bit();
    testVerifiedAlphaPreparation();
    testInvalidGpuReadback(directory.path());
    testEffectiveEditCache(directory.path());
    testGpuRasterPreviewRecovery(directory.path());
    testProgressiveEditController(directory.path());
    testGpuResidentImageState();
    testAnimation(directory.path());
    testAnimatedExportCacheIsolation(directory.path());
    testTileStore();
    testWorkerProtocolAndPackages(directory.path());
    testMappedPreviewOwnership(directory.path());
    testSplitArtifactAndPreviewReadiness(directory.path());
    testCancelAfterArtifactPublication(directory.path());
    testResourceAdmission(directory.path());
    testNonCooperativeCancellation(directory.path());
    testCooperativeCancellation(directory.path());
    testJxlWorkerRetirement(directory.path());
    testWorkerFailureModes(directory.path());
    testSharedMemoryAttachRetry(directory.path());
    testExportKeyNormalization();
    testWebpNativeRoutingAndArtifactReuse(directory.path());
    testJpegNativePreviewAndArtifactReuse(directory.path());
    std::cout << "snow_image_viewer tests passed\n";
    return 0;
}
