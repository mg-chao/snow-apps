#include "editing/edit_pipeline_controller.h"

#include "core/image_raster_store.h"
#include "core/image_tile_store.h"
#include "decoding/snow_image_decoder.h"
#include "editing/raster_asset.h"
#include "editing/raster_package.h"
#include "editing/temporary_file_lease.h"
#include "editing/worker_protocol.h"

#include <snow/image/io.h>
#include <snow/image/resource_plan.h>
#include <snow/image/service.h>

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace snow::image_viewer {
namespace {

struct PerformanceTiming final {
    QString stage;
    qint64 nanoseconds = 0;
};

void recordTiming(QVector<PerformanceTiming>* timings, const QString& stage, QElapsedTimer* timer) {
    timings->push_back({stage, timer->nsecsElapsed()});
    timer->restart();
}

std::filesystem::path nativePath(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

QString statusText(const snow::image::Status& status) {
    const QString message = QString::fromStdString(status.message).trimmed();
    return message.isEmpty() ? QStringLiteral("The image operation failed.") : message;
}

QString extensionFor(snow::image::Format format) {
    const auto extensions = snow::image::format_extensions(format);
    return extensions.empty()
               ? QStringLiteral("img")
               : QString::fromUtf8(extensions.front().data(),
                                   static_cast<qsizetype>(extensions.front().size()));
}

QString uuidHex() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
}

snow::image::TransformOptions transformOptions(const EditExportSettings& settings,
                                               std::uint32_t maximumThreads = 0) {
    snow::image::TransformOptions options;
    options.resize = snow::image::ResizeOptions{static_cast<std::uint32_t>(settings.width),
                                                static_cast<std::uint32_t>(settings.height),
                                                settings.resampling,
                                                settings.premultiplyAlpha,
                                                settings.linearRgb,
                                                maximumThreads};
    options.animation_policy = animationPolicyForFormat(settings.format);
    return options;
}

std::shared_ptr<MappedRasterPackage>
packageForBackingStore(const std::shared_ptr<ImageRasterStore>& backingStore,
                       const std::shared_ptr<QTemporaryDir>& artifactDirectory,
                       snow::image::RasterAnalysis analysis = {}) {
    if (!backingStore || !backingStore->isValid() || !backingStore->store()->complete() ||
        !artifactDirectory || !artifactDirectory->isValid()) {
        return {};
    }
    const QString publishedPath =
        artifactDirectory->filePath(QStringLiteral("source-shared-%1.raster")
                                        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    std::error_code linkError;
    std::filesystem::create_hard_link(nativePath(backingStore->filePath()),
                                      nativePath(publishedPath), linkError);
    if (linkError) {
        std::error_code copyError;
        if (!std::filesystem::copy_file(
                nativePath(backingStore->filePath()), nativePath(publishedPath),
                std::filesystem::copy_options::overwrite_existing, copyError)) {
            return {};
        }
    }
    QString leaseError;
    auto lease = TemporaryFileLease::adopt(publishedPath, artifactDirectory, &leaseError);
    if (!lease) {
        QFile::remove(publishedPath);
        return {};
    }
    struct PublishedRasterOwner final {
        std::shared_ptr<ImageRasterStore> backing;
        std::shared_ptr<const TemporaryFileLease> publication;
    };
    auto owner = std::make_shared<PublishedRasterOwner>(
        PublishedRasterOwner{backingStore, std::move(lease)});
    QString error;
    return MappedRasterPackage::adoptStore(
        publishedPath, backingStore->store(), &error, std::move(owner),
        analysis.alpha_content ? analysis.alpha_content : backingStore->verifiedAlphaContent());
}

bool canReuseSourceRaster(const EditExportSettings& settings,
                          const std::shared_ptr<MappedRasterPackage>& package) {
    if (!package || settings.width <= 0 || settings.height <= 0)
        return false;
    const snow::image::DocumentDescriptor& descriptor = package->source().descriptor();
    if (descriptor.canvas_width != static_cast<std::uint32_t>(settings.width) ||
        descriptor.canvas_height != static_cast<std::uint32_t>(settings.height) ||
        descriptor.frames.size() != 1) {
        return false;
    }
    const snow::image::RasterFrameDescriptor& frame = descriptor.frames.front();
    if (frame.width != descriptor.canvas_width || frame.height != descriptor.canvas_height ||
        frame.x != 0 || frame.y != 0)
        return false;
    if (!settings.reducePalette) {
        snow::image::Service service;
        const auto route = service.raster_encode_route(descriptor, settings.encode);
        if (route && route.value() == snow::image::RasterEncodeRoute::native)
            return true;
    }
    if (frame.layout.planes.size() != 1)
        return false;
    const snow::image::PlaneDescriptor& plane = frame.layout.planes.front();
    return frame.layout.alpha != snow::image::AlphaMode::premultiplied &&
           plane.semantic == snow::image::PlaneSemantic::packed &&
           plane.format.alpha != snow::image::AlphaMode::premultiplied;
}

snow::image::DocumentDescriptor syntheticSourceDescriptor(const EditExportSettings& settings) {
    const bool hdr = exportKey(settings, 0).colorIntent == OutputColorIntent::hdr_preserve;
    const std::uint32_t width = static_cast<std::uint32_t>(
        settings.sourceSize.width() > 0 ? settings.sourceSize.width() : settings.width);
    const std::uint32_t height = static_cast<std::uint32_t>(
        settings.sourceSize.height() > 0 ? settings.sourceSize.height() : settings.height);
    const snow::image::PixelFormat format = hdr ? snow::image::kRgba16Float : snow::image::kRgba8;
    snow::image::DocumentDescriptor descriptor;
    descriptor.canvas_width = width;
    descriptor.canvas_height = height;
    snow::image::RasterFrameDescriptor frame;
    frame.width = width;
    frame.height = height;
    frame.layout.alpha = format.alpha;
    frame.layout.planes.push_back({snow::image::PlaneSemantic::packed, width, height, format,
                                   static_cast<std::uint8_t>(hdr ? 16 : 8)});
    descriptor.frames.push_back(std::move(frame));
    return descriptor;
}

snow::image::Result<snow::image::DocumentDescriptor>
planningSourceDescriptor(const EditExportSettings& settings,
                         const std::shared_ptr<MappedRasterPackage>& raster,
                         const std::shared_ptr<snow::image::Document>& document) {
    if (raster)
        return raster->source().descriptor();
    if (document)
        return snow::image::describe_document(*document);
    snow::image::DocumentDescriptor descriptor = syntheticSourceDescriptor(settings);
    const auto valid = descriptor.validate();
    if (!valid)
        return valid.error();
    return descriptor;
}

snow::image::Result<snow::image::DocumentDescriptor>
planningOutputDescriptor(const EditExportSettings& settings,
                         const snow::image::DocumentDescriptor& source) {
    snow::image::DocumentDescriptor output = source;
    output.format = settings.format;
    output.kind = snow::image::DocumentKind::raster;
    output.canvas_width = static_cast<std::uint32_t>(settings.width);
    output.canvas_height = static_cast<std::uint32_t>(settings.height);
    if (animationPolicyForFormat(settings.format) == snow::image::AnimationPolicy::first_frame &&
        output.frames.size() > 1) {
        output.frames.resize(1);
    }
    std::size_t maximum_bytes_per_pixel = 0;
    for (const snow::image::RasterFrameDescriptor& frame : source.frames) {
        for (const snow::image::PlaneDescriptor& plane : frame.layout.planes) {
            const auto bytes = plane.format.bytes_per_pixel();
            if (!bytes)
                return bytes.error();
            maximum_bytes_per_pixel = std::max(maximum_bytes_per_pixel, bytes.value());
        }
    }
    const bool hdr = exportKey(settings, 0).colorIntent == OutputColorIntent::hdr_preserve;
    const bool high_precision = hdr || maximum_bytes_per_pixel > 4;
    const snow::image::PixelFormat format =
        high_precision ? snow::image::kRgba16Float : snow::image::kRgba8;
    for (snow::image::RasterFrameDescriptor& frame : output.frames) {
        frame.width = output.canvas_width;
        frame.height = output.canvas_height;
        frame.x = 0;
        frame.y = 0;
        frame.layout = {};
        frame.layout.alpha = format.alpha;
        frame.layout.planes.push_back({snow::image::PlaneSemantic::packed, output.canvas_width,
                                       output.canvas_height, format,
                                       static_cast<std::uint8_t>(high_precision ? 16 : 8)});
    }
    const auto valid = output.validate();
    if (!valid)
        return valid.error();
    return output;
}

snow::image::Result<snow::image::ResourcePlan>
editResourcePlan(const EditExportSettings& settings, const snow::image::DocumentDescriptor& source,
                 std::uint64_t private_memory_budget, bool gpu_transform) {
    snow::image::Service service;
    const snow::image::EncoderInfo* encoder = service.encoder_info(settings.format);
    if (!encoder)
        return snow::image::Status::error(snow::image::ErrorCode::codec_unavailable,
                                          "The selected encoder is unavailable.");
    auto normalized = snow::image::normalize_encode_options(*encoder, settings.encode);
    if (!normalized)
        return normalized.error();
    auto selectedRoute = service.raster_encode_route(source, normalized.value());
    if (!selectedRoute)
        return selectedRoute.error();
    const snow::image::RasterEncodeRoute route = settings.reducePalette
                                                     ? snow::image::RasterEncodeRoute::materialized
                                                     : selectedRoute.value();
    snow::image::Result<snow::image::DocumentDescriptor> output =
        route == snow::image::RasterEncodeRoute::native
            ? snow::image::Result<snow::image::DocumentDescriptor>(source)
            : planningOutputDescriptor(settings, source);
    if (!output)
        return output.error();
    output.value().format = settings.format;
    snow::image::ResourcePlanRequest request;
    request.source = source;
    request.output = std::move(output).value();
    request.encode_options = std::move(normalized).value();
    request.raster_route = route;
    request.budgets.private_memory_bytes = private_memory_budget;
    request.budgets.gpu_bytes = std::numeric_limits<std::uint64_t>::max();
    request.maximum_cpu_threads = std::max(1U, std::thread::hardware_concurrency());
    request.gpu_transform = gpu_transform;
    switch (settings.resampling) {
    case snow::image::ResamplingMethod::nearest:
        request.resampling_support = 0.5;
        break;
    case snow::image::ResamplingMethod::linear:
        request.resampling_support = 1.0;
        break;
    default:
        request.resampling_support = 3.0;
        break;
    }
    return snow::image::plan_resources(request);
}

std::uint64_t phasePrivateBytes(const snow::image::ResourcePlan& plan,
                                snow::image::ResourcePhase phase) {
    for (const snow::image::PhaseResourcePlan& candidate : plan.phases) {
        if (candidate.phase == phase)
            return candidate.footprint.private_memory_bytes;
    }
    return std::numeric_limits<std::uint64_t>::max();
}

std::size_t defaultEditingBudget() {
    std::uint64_t total = 0;
    std::uint64_t available = 0;
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        total = status.ullTotalPhys;
        available = status.ullAvailPhys;
    }
#elif defined(Q_OS_UNIX)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long availablePages = sysconf(_SC_AVPHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        total = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
    }
    if (availablePages > 0 && pageSize > 0) {
        available =
            static_cast<std::uint64_t>(availablePages) * static_cast<std::uint64_t>(pageSize);
    }
#endif
    constexpr std::uint64_t kMiB = 1024U * 1024U;
    constexpr std::uint64_t kMaximum = std::uint64_t{2} << 30U;
    if (total == 0 || available == 0)
        return 256U * kMiB;
    return static_cast<std::size_t>(
        std::max<std::uint64_t>(64U * kMiB, std::min({total / 8U, available / 3U, kMaximum})));
}

std::size_t documentResidentBytes(const snow::image::Document& document) {
    std::uint64_t bytes = 0;
    for (const snow::image::Frame& frame : document.frames) {
        if (frame.image.pixels().size() > std::numeric_limits<std::uint64_t>::max() - bytes)
            return std::numeric_limits<std::size_t>::max();
        bytes += frame.image.pixels().size();
    }
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(bytes, std::numeric_limits<std::size_t>::max()));
}

std::size_t artifactWeight(const ExactEditResult& result) {
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(result.artifact ? result.artifact->byteSize() : 0U,
                                std::numeric_limits<std::size_t>::max()));
}

std::size_t previewWeight(const ExactEditResult& result) {
    std::uint64_t bytes = 0;
    if (result.displayPreview) {
        bytes = static_cast<std::uint64_t>(result.displayPreview->pixels.sizeInBytes());
        for (const DecodedAnimationFrame& frame : result.displayPreview->animationFrames) {
            const std::uint64_t frameBytes = static_cast<std::uint64_t>(frame.pixels.sizeInBytes());
            if (frameBytes > std::numeric_limits<std::uint64_t>::max() - bytes) {
                bytes = std::numeric_limits<std::uint64_t>::max();
                break;
            }
            bytes += frameBytes;
        }
    }
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(bytes, std::numeric_limits<std::size_t>::max()));
}

std::size_t previewDiskWeight(const ExactEditResult& result) {
    if (!result.displayPreview || !result.displayPreview->rasterStore ||
        !result.displayPreview->rasterStore->store()) {
        return 0;
    }
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(result.displayPreview->rasterStore->store()->file_bytes(),
                                std::numeric_limits<std::size_t>::max()));
}

} // namespace

struct EditPipelineController::SourceResult final {
    std::shared_ptr<MappedRasterPackage> raster;
    snow::image::RasterAnalysis analysis;
    QString error;
    QVector<PerformanceTiming> timings;
};

struct EditPipelineController::PreviewResult final {
    std::optional<DecodedImage> displayPreview;
    ExactPreviewSource previewSource = ExactPreviewSource::base_raster;
    std::shared_ptr<const EncodedArtifact> artifact;
    QString warning;
    QString error;
    QVector<PerformanceTiming> timings;
    std::shared_ptr<MappedRasterPackage> raster;
    RasterProvenance provenance = RasterProvenance::cpu_reference;
    snow::image::AlphaContent alphaContent = snow::image::AlphaContent::non_opaque;
};

struct EditPipelineController::ActiveWorkerJob final {
    PendingExact request;
    std::shared_ptr<RasterAsset> baseAsset;
    RasterProvenance provenance = RasterProvenance::cpu_reference;
    QString nonce;
    QString artifactPath;
    QString previewPath;
    QString warning;
    QString previewKind;
    QString testMode;
    std::shared_ptr<const EncodedArtifact> artifact;
    snow::image::AlphaContent alphaContent = snow::image::AlphaContent::non_opaque;
    bool previewOnly = false;
    bool sharedMemoryRetryAttempted = false;
    bool sent = false;
    bool cooperative = false;
    bool cancelling = false;
    QElapsedTimer timer;
    QElapsedTimer artifactTimer;
    QElapsedTimer cancellationTimer;
};

EditPipelineController::EditPipelineController(QObject* parent)
    : EditPipelineController(EditPipelineOptions{}, parent) {}

EditPipelineController::EditPipelineController(EditPipelineOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
    editingBudgetBytes_ =
        options_.memoryBudgetBytes == 0 ? defaultEditingBudget() : options_.memoryBudgetBytes;
    const std::size_t maximumCache = editingBudgetBytes_ / 3U;
    if (options_.cacheBudgetBytes == 0)
        options_.cacheBudgetBytes = maximumCache;
    options_.cacheBudgetBytes = std::min(options_.cacheBudgetBytes, maximumCache);
    options_.workerTimeoutMs = std::max(100, options_.workerTimeoutMs);
    pool_.setMaxThreadCount(1);
    pool_.setExpiryTimeout(-1);
    exactTimer_.setSingleShot(true);
    exactTimer_.setTimerType(Qt::PreciseTimer);
    connect(&exactTimer_, &QTimer::timeout, this, &EditPipelineController::scheduleExact);
    workerTimeout_.setSingleShot(true);
    connect(&workerTimeout_, &QTimer::timeout, this, [this]() {
        if (!activeWorkerJob_)
            return;
        const PendingExact request = activeWorkerJob_->request;
        const bool wasCancelling = activeWorkerJob_->cancelling;
        if (wasCancelling)
            activeWorkerJob_->cooperative = false;
        cancelActiveWorker(true);
        if (!wasCancelling && request.requestId == latestRequestId_) {
            setState(EditPipelineState::Failed);
            emit failed(QStringLiteral("The image export worker timed out."));
        }
    });
    artifactDirectory_ = std::make_shared<QTemporaryDir>(
        QDir::temp().filePath(QStringLiteral("snow-image-edit-XXXXXX")));
    sharedMemorySessionKey_ = QStringLiteral("snow-edit-v1-") + uuidHex();
    if (artifactDirectory_->isValid()) {
        QFile::setPermissions(artifactDirectory_->path(), QFileDevice::ReadOwner |
                                                              QFileDevice::WriteOwner |
                                                              QFileDevice::ExeOwner);
    }
    startWorker();
}

EditPipelineController::~EditPipelineController() {
    shuttingDown_ = true;
    cancel();
    if (worker_ && worker_->state() != QProcess::NotRunning) {
        worker_->write(worker_protocol::encodeFrame(worker_protocol::MessageType::shutdown, {}));
        worker_->closeWriteChannel();
        if (!worker_->waitForFinished(100)) {
            worker_->kill();
            worker_->waitForFinished(1000);
        }
    }
    pool_.waitForDone();
    exactResult_.reset();
    clearCache();
    artifactDirectory_.reset();
}

void EditPipelineController::setSource(const QString& filePath,
                                       const std::shared_ptr<ImageRasterStore>& rasterStore,
                                       snow::image::RasterAnalysis analysis) {
    cancel();
    clearCache();
    sourcePath_ = QFileInfo(filePath).absoluteFilePath();
    sourceAnalysis_ = analysis;
    gpuSource_ = false;
    sourceReady_ = false;
    sourceRaster_.reset();
    sourceDocument_.reset();
    sourceRaster_ = packageForBackingStore(rasterStore, artifactDirectory_, analysis);
    if (sourceRaster_) {
        if (const snow::image::Document* document = sourceRaster_->document())
            sourceDocument_ = std::make_shared<snow::image::Document>(*document);
        sourceReady_ = true;
        const EditRequestId generation = ++sourceGeneration_;
        QTimer::singleShot(0, this, [this, generation]() {
            if (generation == sourceGeneration_ && sourceReady_ && !gpuSource_)
                emit sourceReady();
        });
        return;
    }
    sourceCancellation_ = std::make_shared<std::stop_source>();
    const EditRequestId generation = ++sourceGeneration_;
    startSourceDecode(generation, sourceCancellation_);
}

void EditPipelineController::setGpuSource(const QString& filePath,
                                          const std::shared_ptr<ImageRasterStore>& rasterStore,
                                          snow::image::RasterAnalysis analysis) {
    cancel();
    clearCache();
    ++sourceGeneration_;
    sourcePath_ = QFileInfo(filePath).absoluteFilePath();
    sourceAnalysis_ = analysis;
    sourceRaster_.reset();
    sourceDocument_.reset();
    sourceRaster_ = packageForBackingStore(rasterStore, artifactDirectory_, analysis);
    if (sourceRaster_) {
        if (const snow::image::Document* document = sourceRaster_->document())
            sourceDocument_ = std::make_shared<snow::image::Document>(*document);
    }
    gpuSource_ = true;
    sourceReady_ = true;
    QTimer::singleShot(0, this, [this]() {
        if (gpuSource_ && sourceReady_)
            emit sourceReady();
    });
}

EditRequestId EditPipelineController::requestEdit(const EditExportSettings& settings,
                                                  EditChangeKind kind) {
    QElapsedTimer handlingTimer;
    handlingTimer.start();
    if (!settings.isValid() || !sourceReady_)
        return 0;
    EditExportSettings normalizedSettings = settings;
    snow::image::Service service;
    const snow::image::EncoderInfo* encoder = service.encoder_info(settings.format);
    if (!encoder)
        return 0;
    auto normalizedOptions = snow::image::normalize_encode_options(*encoder, settings.encode);
    if (!normalizedOptions)
        return 0;
    const EditRequestId requestId = ++latestRequestId_;
    latestSettings_ = normalizedSettings;
    exactResult_.reset();
    previewPending_ = false;
    pendingExact_.reset();
    if (pendingPreview_)
        pendingPreview_->request.cancellation->request_stop();
    pendingPreview_.reset();
    if (activeWorkerJob_)
        cancelActiveWorker(true);
    for (const auto& cancellation : workerCancellations_) {
        if (cancellation && !cancellation->stop_requested()) {
            cancellation->request_stop();
            ++cancellationCount_;
        }
    }
    exactDelayTimer_.restart();
    int continuousDelay = options_.continuousDebounceMs;
    if (continuousDelay == 100) {
        if (normalizedSettings.format == snow::image::Format::webp) {
            continuousDelay = 250;
        } else if (normalizedSettings.format == snow::image::Format::avif ||
                   normalizedSettings.format == snow::image::Format::heif ||
                   normalizedSettings.format == snow::image::Format::jxl) {
            continuousDelay = 350;
        }
    }
    const int delay = kind == EditChangeKind::dimension_typing ? options_.dimensionDebounceMs
                      : kind == EditChangeKind::continuous     ? continuousDelay
                                                               : options_.discreteDebounceMs;
    setState(EditPipelineState::VisualPending);
    const ExportKey key = exportKey(normalizedSettings, sourceGeneration_);
    const bool cached = publishCacheHit(requestId, normalizedSettings, key);
    emit visualRequested(requestId, normalizedSettings);
    emit performanceStageCompleted(requestId, QStringLiteral("controller.request_handling"),
                                   handlingTimer.nsecsElapsed());
    if (!cached)
        exactTimer_.start(std::max(0, delay));
    return requestId;
}

void EditPipelineController::flushPendingExact() {
    if (!latestRequestId_ || hasEncodedArtifact(latestSettings_))
        return;
    exactTimer_.stop();
    scheduleExact();
}

void EditPipelineController::submitVisualFrame(EditRequestId requestId) {
    if (requestId != latestRequestId_)
        return;
    if (state_ == EditPipelineState::VisualPending)
        setState(EditPipelineState::VisualReady);
    emit visualReady(requestId);
}

void EditPipelineController::scheduleExact() {
    if (!latestRequestId_ || !latestSettings_.isValid())
        return;
    emit performanceStageCompleted(latestRequestId_, QStringLiteral("exact.debounce_delay"),
                                   exactDelayTimer_.nsecsElapsed());
    QElapsedTimer planningTimer;
    planningTimer.start();
    const std::size_t sourceBytes =
        sourceDocument_ && !sourceRaster_ ? documentResidentBytes(*sourceDocument_) : 0;
    const auto sourceDescriptor =
        planningSourceDescriptor(latestSettings_, sourceRaster_, sourceDocument_);
    const bool sourceReuse =
        options_.allowSourceRasterReuse && canReuseSourceRaster(latestSettings_, sourceRaster_);
    const auto plan =
        sourceDescriptor
            ? editResourcePlan(latestSettings_, sourceDescriptor.value(),
                               sourceBytes < editingBudgetBytes_ ? editingBudgetBytes_ - sourceBytes
                                                                 : 0,
                               gpuSource_ && !sourceReuse)
            : snow::image::Result<snow::image::ResourcePlan>(sourceDescriptor.error());
    const std::uint64_t plannedBytes =
        sourceReuse ? 0
        : plan      ? phasePrivateBytes(plan.value(), snow::image::ResourcePhase::transform)
                    : std::numeric_limits<std::uint64_t>::max();
    const std::size_t workingBytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(plannedBytes, std::numeric_limits<std::size_t>::max()));
    while (workingBytes != std::numeric_limits<std::size_t>::max() &&
           sourceBytes <= editingBudgetBytes_ &&
           cacheBytes_ > editingBudgetBytes_ - sourceBytes -
                             std::min(workingBytes, editingBudgetBytes_ - sourceBytes) &&
           (!cache_.empty() || !rasterCache_.empty()) && evictOldestCachePortion()) {
    }
    emit performanceStageCompleted(latestRequestId_, QStringLiteral("exact.resource_planning"),
                                   planningTimer.nsecsElapsed());
    if (workingBytes == std::numeric_limits<std::size_t>::max() ||
        sourceBytes > editingBudgetBytes_ ||
        workingBytes > editingBudgetBytes_ - std::min(sourceBytes, editingBudgetBytes_)) {
        setState(EditPipelineState::Failed);
        emit failed(
            QStringLiteral("Exact export requires at least %1 bytes; %2 bytes are available. "
                           "The live preview remains available.")
                .arg(QString::number(workingBytes),
                     QString::number(editingBudgetBytes_ > sourceBytes
                                         ? editingBudgetBytes_ - sourceBytes
                                         : 0)));
        return;
    }
    auto cancellation = std::make_shared<std::stop_source>();
    pendingExact_ = PendingExact{latestRequestId_, latestSettings_, cancellation,
                                 exportKey(latestSettings_, sourceGeneration_), workingBytes};
    setState(EditPipelineState::EncodingPending);
    if (activeWorkerCount_ >= 1)
        return;
    startPendingExact();
}

void EditPipelineController::startPendingExact() {
    if (activeWorkerCount_ >= 1)
        return;
    if (pendingExact_) {
        PendingExact request = std::move(*pendingExact_);
        pendingExact_.reset();
        if (request.requestId != latestRequestId_)
            return;
        RasterProvenance provenance = RasterProvenance::cpu_reference;
        const auto cachedRaster = findRaster(request.key.base, &provenance);
        if (cachedRaster) {
            emit performanceStageCompleted(request.requestId,
                                           QStringLiteral("exact.raster_cache_hit"), 0);
            startCpuExact(std::move(request), cachedRaster, provenance);
        } else if (options_.allowSourceRasterReuse &&
                   canReuseSourceRaster(request.settings, sourceRaster_)) {
            emit performanceStageCompleted(request.requestId,
                                           QStringLiteral("exact.source_raster_reuse"), 0);
            cacheRaster(request.key.base, sourceRaster_, RasterProvenance::source_exact);
            startCpuExact(std::move(request), sourceRaster_, RasterProvenance::source_exact);
        } else if (gpuSource_) {
            emit exactRasterRequested(request.requestId, request.settings);
        } else {
            startCpuExact(std::move(request));
        }
        return;
    }
    if (!pendingPreview_)
        return;
    PendingPreview preview = std::move(*pendingPreview_);
    pendingPreview_.reset();
    if (preview.request.requestId != latestRequestId_ ||
        preview.request.cancellation->stop_requested())
        return;
    dispatchPreviewWorker(std::move(preview.request), std::move(preview.artifact),
                          preview.provenance, preview.alphaContent);
}

void EditPipelineController::submitGpuResizeResult(EditRequestId requestId,
                                                   GpuRasterResult readback) {
    if (!gpuSource_ || requestId != latestRequestId_)
        return;
    if (!readback.isValid() ||
        readback.pixelSize != QSize(latestSettings_.width, latestSettings_.height)) {
        failGpuRequest(requestId, QStringLiteral("The GPU resize readback buffer is invalid."));
        return;
    }
    PendingExact request{requestId, latestSettings_, std::make_shared<std::stop_source>(),
                         exportKey(latestSettings_, sourceGeneration_), 0};
    const auto sourceDescriptor =
        planningSourceDescriptor(latestSettings_, sourceRaster_, sourceDocument_);
    const auto plan =
        sourceDescriptor
            ? editResourcePlan(latestSettings_, sourceDescriptor.value(), editingBudgetBytes_, true)
            : snow::image::Result<snow::image::ResourcePlan>(sourceDescriptor.error());
    if (!plan) {
        failGpuRequest(requestId, statusText(plan.error()));
        return;
    }
    request.estimatedWorkingBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
        phasePrivateBytes(plan.value(), snow::image::ResourcePhase::transform),
        std::numeric_limits<std::size_t>::max()));
    startGpuEncode(std::move(request), std::move(readback));
}

void EditPipelineController::failGpuRequest(EditRequestId requestId, const QString& message) {
    if (requestId != latestRequestId_)
        return;
    Q_UNUSED(message);
    if (!gpuSource_)
        return;
    gpuSource_ = false;
    sourceReady_ = false;
    sourceCancellation_ = std::make_shared<std::stop_source>();
    emit performanceStageCompleted(requestId, QStringLiteral("exact.gpu_cpu_fallback"), 0);
    startSourceDecode(sourceGeneration_, sourceCancellation_);
}

void EditPipelineController::cancel() {
    exactTimer_.stop();
    ++latestRequestId_;
    if (sourceCancellation_)
        sourceCancellation_->request_stop();
    if (activeWorkerJob_)
        cancelActiveWorker(!shuttingDown_);
    for (const auto& cancellation : workerCancellations_) {
        if (cancellation && !cancellation->stop_requested()) {
            cancellation->request_stop();
            ++cancellationCount_;
        }
    }
    sourceCancellation_.reset();
    pendingExact_.reset();
    if (pendingPreview_)
        pendingPreview_->request.cancellation->request_stop();
    pendingPreview_.reset();
    exactResult_.reset();
    previewPending_ = false;
}

bool EditPipelineController::hasExactResult(const EditExportSettings& settings) const {
    return hasEncodedArtifact(settings) && hasExactPreview(settings);
}

bool EditPipelineController::hasEncodedArtifact(const EditExportSettings& settings) const {
    return exactResult_ && exactResult_->isValid() && exactResult_->requestId == latestRequestId_ &&
           exactResult_->settings == settings;
}

bool EditPipelineController::hasExactPreview(const EditExportSettings& settings) const {
    return exactResult_ && hasEncodedArtifact(settings) && exactResult_->exactPreviewAvailable;
}

std::shared_ptr<const EncodedArtifact> EditPipelineController::encodedArtifact() const {
    return exactResult_ ? exactResult_->artifact : nullptr;
}

void EditPipelineController::clearExactArtifactCacheForBenchmark() {
    for (const CacheEntry& entry : cache_) {
        diskCacheBytes_ -= std::min(diskCacheBytes_, entry.artifactWeight);
        diskCacheBytes_ -= std::min(diskCacheBytes_, entry.previewDiskWeight);
        cacheBytes_ -= std::min(cacheBytes_, entry.previewWeight);
    }
    cache_.clear();
    exactResult_.reset();
}

void EditPipelineController::clearExactPreviewCacheForBenchmark() {
    for (CacheEntry& entry : cache_) {
        cacheBytes_ -= std::min(cacheBytes_, entry.previewWeight);
        diskCacheBytes_ -= std::min(diskCacheBytes_, entry.previewDiskWeight);
        entry.previewWeight = 0;
        entry.previewDiskWeight = 0;
        entry.previewAccessSerial = 0;
        entry.result.displayPreview.reset();
        entry.result.exactPreviewAvailable = false;
    }
    exactResult_.reset();
    previewPending_ = false;
}

void EditPipelineController::clearAllCachesForBenchmark() {
    exactResult_.reset();
    clearCache();
}

void EditPipelineController::setState(EditPipelineState state) {
    if (state_ == state)
        return;
    const bool wasBusy = isBusy();
    if (state != EditPipelineState::ArtifactReady)
        previewPending_ = false;
    state_ = state;
    emit stateChanged(state_);
    if (wasBusy != isBusy())
        emit busyChanged(isBusy());
}

void EditPipelineController::startSourceDecode(
    EditRequestId generation, const std::shared_ptr<std::stop_source>& cancellation) {
    auto* watcher = new QFutureWatcher<SourceResult>(this);
    connect(watcher, &QFutureWatcher<SourceResult>::finished, this,
            [this, watcher, generation, cancellation]() {
                SourceResult result = watcher->result();
                watcher->deleteLater();
                if (generation != sourceGeneration_ || cancellation->stop_requested())
                    return;
                if (!result.error.isEmpty()) {
                    emit failed(result.error);
                    return;
                }
                for (const PerformanceTiming& timing : result.timings) {
                    emit performanceStageCompleted(0, timing.stage, timing.nanoseconds);
                }
                sourceAnalysis_ = result.analysis;
                sourceRaster_ = std::move(result.raster);
                if (!sourceRaster_) {
                    emit failed(QStringLiteral("The decoded source raster is unavailable."));
                    return;
                }
                if (const snow::image::Document* document = sourceRaster_->document())
                    sourceDocument_ = std::make_shared<snow::image::Document>(*document);
                else
                    sourceDocument_.reset();
                sourceReady_ = true;
                const bool resumeExact = state_ == EditPipelineState::EncodingPending &&
                                         latestSettings_.isValid() && !pendingExact_ &&
                                         !activeWorkerJob_;
                if (resumeExact) {
                    exactTimer_.start(0);
                } else {
                    emit sourceReady();
                }
            });
    const auto artifactDirectory = artifactDirectory_;
    const snow::image::RasterAnalysis providedAnalysis = sourceAnalysis_;
    watcher->setFuture(QtConcurrent::run(&pool_, [path = sourcePath_, cancellation,
                                                  artifactDirectory, providedAnalysis]() {
        SourceResult result;
        QElapsedTimer timer;
        timer.start();
        auto input = snow::image::file_input(nativePath(path));
        recordTiming(&result.timings, QStringLiteral("source.file_input"), &timer);
        if (!input) {
            result.error = statusText(input.error());
            return result;
        }
        snow::image::DecodeOptions options;
        options.orientation = snow::image::OrientationPolicy::apply;
        snow::image::Service service;
        const auto detected = service.detect(input.value(), cancellation->get_token());
        if (!detected) {
            result.error = statusText(detected.error());
            return result;
        }
        if (detected.value() == snow::image::Format::jpeg ||
            detected.value() == snow::image::Format::webp) {
            options.raster_layout = snow::image::RasterLayoutPolicy::native;
        }
        if (!artifactDirectory || !artifactDirectory->isValid()) {
            result.error = QStringLiteral("The source raster directory is unavailable.");
            return result;
        }
        const QString rasterPath = artifactDirectory->filePath(
            QStringLiteral("source-%1.raster")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        auto decoded = service.decode_to_store(input.value(), nativePath(rasterPath), options, {},
                                               cancellation->get_token());
        recordTiming(&result.timings, QStringLiteral("source.decode_to_store"), &timer);
        if (!decoded) {
            if (decoded.error().code != snow::image::ErrorCode::cancelled)
                result.error = statusText(decoded.error());
            return result;
        }
        std::shared_ptr<snow::image::RasterStore> decodedStore = std::move(decoded).value();
        snow::image::RasterAnalysis analysis = providedAnalysis;
        if (providedAnalysis.alpha_content) {
            result.analysis = providedAnalysis;
        } else {
            analysis.alpha_content = snow::image::AlphaContent::opaque;
            bool analysisKnown = true;
            for (std::uint32_t frameIndex = 0;
                 frameIndex < decodedStore->descriptor().frames.size(); ++frameIndex) {
                const auto& frame = decodedStore->descriptor().frames[frameIndex];
                for (std::uint32_t planeIndex = 0; planeIndex < frame.layout.planes.size();
                     ++planeIndex) {
                    const auto& plane = frame.layout.planes[planeIndex];
                    if (plane.semantic == snow::image::PlaneSemantic::alpha) {
                        if (plane.format != snow::image::kGray8) {
                            analysisKnown = false;
                            continue;
                        }
                        const auto mapped = decodedStore->map_plane(frameIndex, planeIndex);
                        if (!mapped) {
                            analysisKnown = false;
                            continue;
                        }
                        for (std::uint32_t row = 0; row < plane.height; ++row) {
                            if (cancellation->get_token().stop_requested()) {
                                analysisKnown = false;
                                break;
                            }
                            const std::byte* pixels =
                                mapped.value().pixels.data() +
                                static_cast<std::size_t>(row) * mapped.value().row_stride;
                            if (std::any_of(pixels, pixels + plane.width, [](std::byte alpha) {
                                    return alpha != std::byte{0xff};
                                })) {
                                analysis.alpha_content = snow::image::AlphaContent::non_opaque;
                                break;
                            }
                        }
                        continue;
                    }
                    if (plane.format.alpha == snow::image::AlphaMode::none)
                        continue;
                    if (plane.semantic != snow::image::PlaneSemantic::packed) {
                        analysisKnown = false;
                        continue;
                    }
                    const auto mapped = decodedStore->map_plane(frameIndex, planeIndex);
                    if (!mapped) {
                        analysisKnown = false;
                        continue;
                    }
                    const auto classified = snow::image::classify_alpha(
                        snow::image::ImageView{plane.width, plane.height, plane.format,
                                               mapped.value().row_stride, mapped.value().pixels},
                        cancellation->get_token());
                    if (!classified) {
                        analysisKnown = false;
                        continue;
                    }
                    if (classified.value() == snow::image::AlphaContent::non_opaque)
                        analysis.alpha_content = classified.value();
                }
            }
            if (!analysisKnown)
                analysis = {};
            result.analysis = analysis;
        }
        const auto lease = TemporaryFileLease::adopt(rasterPath, artifactDirectory, &result.error);
        if (lease) {
            result.raster =
                MappedRasterPackage::adoptStore(rasterPath, std::move(decodedStore), &result.error,
                                                lease, result.analysis.alpha_content);
        } else {
            decodedStore.reset();
            QFile::remove(rasterPath);
        }
        if (!result.raster && !decodedStore)
            QFile::remove(rasterPath);
        recordTiming(&result.timings, QStringLiteral("source.map_store"), &timer);
        return result;
    }));
}

// QFutureWatcher is owned by this QObject and schedules deletion when its future finishes.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void EditPipelineController::startCpuExact(PendingExact request,
                                           std::shared_ptr<MappedRasterPackage> cachedRaster,
                                           RasterProvenance provenance) {
    if (cachedRaster) {
        lastReadbackBytes_ = 0;
        dispatchWorker(std::move(request), std::move(cachedRaster), provenance);
        return;
    }
    lastReadbackBytes_ = 0;
    const auto source = sourceDocument_;
    const auto sourceRaster = sourceRaster_;
    if (!source && sourceRaster) {
        dispatchWorker(std::move(request), sourceRaster, RasterProvenance::cpu_reference);
        return;
    }
    const RasterHandoffMode handoffMode = options_.rasterHandoffMode;
    const QString sharedSessionKey = sharedMemorySessionKey_;
    const std::uint64_t sourceBytes =
        source && !sourceRaster_ ? static_cast<std::uint64_t>(documentResidentBytes(*source)) : 0;
    const snow::image::RasterAnalysis sourceAnalysis = sourceAnalysis_;
    const std::uint64_t resident =
        sourceBytes >
                std::numeric_limits<std::uint64_t>::max() - cacheBytes_ -
                    std::min<std::size_t>(reservedWorkerBytes_,
                                          std::numeric_limits<std::uint64_t>::max() - cacheBytes_)
            ? std::numeric_limits<std::uint64_t>::max()
            : sourceBytes + cacheBytes_ + reservedWorkerBytes_;
    const std::uint64_t transformBudget =
        resident < editingBudgetBytes_ ? editingBudgetBytes_ - resident : 0;
    std::uint32_t maximumThreads = 1;
    const auto sourceDescriptor = planningSourceDescriptor(request.settings, sourceRaster_, source);
    if (sourceDescriptor) {
        const auto plan =
            editResourcePlan(request.settings, sourceDescriptor.value(), transformBudget, false);
        if (plan) {
            maximumThreads = plan.value().cpu_threads;
            request.estimatedWorkingBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                phasePrivateBytes(plan.value(), snow::image::ResourcePhase::transform),
                std::numeric_limits<std::size_t>::max()));
        }
    }
    ++activeWorkerCount_;
    reservedWorkerBytes_ += request.estimatedWorkingBytes;
    workerCancellations_.push_back(request.cancellation);
    const auto artifactDirectory = artifactDirectory_;
    auto* watcher = new QFutureWatcher<PreviewResult>(this);
    connect(watcher, &QFutureWatcher<PreviewResult>::finished, this,
            [this, watcher, request]() mutable {
                PreviewResult result = watcher->result();
                watcher->deleteLater();
                finishBasePreparation(std::move(request), std::move(result));
            });
    watcher->setFuture(QtConcurrent::run(&pool_, [source, sourceRaster, artifactDirectory, request,
                                                  maximumThreads, handoffMode, sharedSessionKey,
                                                  sourceAnalysis]() mutable {
        PreviewResult result;
        QElapsedTimer timer;
        timer.start();
        if (!source || !artifactDirectory || !artifactDirectory->isValid()) {
            result.error = QStringLiteral("The decoded edit source is unavailable.");
            return result;
        }
        const auto sourceAlpha =
            sourceRaster ? sourceRaster->verifiedAlphaContent() : sourceAnalysis.alpha_content;
        const std::optional<snow::image::AlphaContent> propagatedAlpha =
            sourceAlpha == snow::image::AlphaContent::opaque
                ? std::optional<snow::image::AlphaContent>(snow::image::AlphaContent::opaque)
                : std::nullopt;
        const QString sharedKey = sharedSessionKey + QLatin1Char('-') + uuidHex();
        std::shared_ptr<MappedRasterPackage> package;
        snow::image::Result<void> transformed = snow::image::Status::error(
            snow::image::ErrorCode::internal_error, "The base raster was not prepared.");
        QString preparationError;
        if (handoffMode != RasterHandoffMode::verified_file) {
            SharedRasterSink sharedSink(sharedKey, propagatedAlpha);
            transformed = snow::image::transform_to_sink(
                *source, transformOptions(request.settings, maximumThreads), sharedSink,
                request.cancellation->get_token());
            if (transformed)
                package = sharedSink.takePackage(&preparationError);
            sharedSink.discard();
            if (package) {
                recordTiming(&result.timings, QStringLiteral("exact.shared_raster"), &timer);
                result.raster = std::move(package);
                result.provenance = RasterProvenance::cpu_reference;
                recordTiming(&result.timings, QStringLiteral("exact.base_package"), &timer);
                return result;
            }
            if (request.cancellation->stop_requested()) {
                result.error.clear();
                return result;
            }
            if (handoffMode == RasterHandoffMode::shared_memory) {
                result.error =
                    preparationError.isEmpty()
                        ? transformed
                              ? QStringLiteral("The shared raster package could not be produced.")
                              : statusText(transformed.error())
                        : preparationError;
                return result;
            }
        }
        const QString path =
            artifactDirectory->filePath(QStringLiteral("base-%1.raster").arg(uuidHex()));
        MappedRasterSink sink(path, propagatedAlpha);
        transformed = snow::image::transform_to_sink(
            *source, transformOptions(request.settings, maximumThreads), sink,
            request.cancellation->get_token());
        recordTiming(&result.timings, QStringLiteral("exact.direct_mapped_transform"), &timer);
        if (!transformed) {
            sink.discard();
            if (transformed.error().code != snow::image::ErrorCode::cancelled)
                result.error = statusText(transformed.error());
            return result;
        }
        const auto lease = TemporaryFileLease::adopt(path, artifactDirectory, &result.error);
        if (lease)
            result.raster = sink.takePackage(&result.error, lease);
        if (!result.raster)
            sink.discard();
        result.provenance = RasterProvenance::cpu_reference;
        recordTiming(&result.timings, QStringLiteral("exact.base_package_open"), &timer);
        return result;
    }));
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

void EditPipelineController::startGpuEncode(PendingExact request, GpuRasterResult readback) {
    lastReadbackBytes_ = readback.storageBytes();
    ++activeWorkerCount_;
    reservedWorkerBytes_ += request.estimatedWorkingBytes;
    workerCancellations_.push_back(request.cancellation);
    const auto artifactDirectory = artifactDirectory_;
    const RasterHandoffMode handoffMode = options_.rasterHandoffMode;
    const QString sharedSessionKey = sharedMemorySessionKey_;
    const auto sourceAlpha =
        sourceRaster_ ? sourceRaster_->verifiedAlphaContent() : sourceAnalysis_.alpha_content;
    const std::optional<snow::image::AlphaContent> propagatedAlpha =
        sourceAlpha == snow::image::AlphaContent::opaque
            ? std::optional<snow::image::AlphaContent>(snow::image::AlphaContent::opaque)
            : std::nullopt;
    auto* watcher = new QFutureWatcher<PreviewResult>(this);
    connect(watcher, &QFutureWatcher<PreviewResult>::finished, this,
            [this, watcher, request]() mutable {
                PreviewResult result = watcher->result();
                watcher->deleteLater();
                finishBasePreparation(std::move(request), std::move(result));
            });
    watcher->setFuture(QtConcurrent::run(&pool_, [readback = std::move(readback), request,
                                                  artifactDirectory, handoffMode, sharedSessionKey,
                                                  propagatedAlpha]() mutable {
        PreviewResult result;
        QElapsedTimer timer;
        timer.start();
        const bool fp = readback.encoding == PixelEncoding::LinearScRgb16F;
        const std::size_t bytesPerPixel = fp ? 8U : 4U;
        if (!readback.isValid() || readback.pixelSize.width() <= 0 ||
            readback.pixelSize.height() <= 0 ||
            static_cast<std::size_t>(readback.pixelSize.width()) >
                std::numeric_limits<std::size_t>::max() / bytesPerPixel) {
            result.error = QStringLiteral("The GPU resize readback buffer is invalid.");
            return result;
        }
        const std::size_t packedRowBytes =
            static_cast<std::size_t>(readback.pixelSize.width()) * bytesPerPixel;
        if (static_cast<std::size_t>(readback.pixelSize.height()) >
            std::numeric_limits<std::size_t>::max() / packedRowBytes) {
            result.error = QStringLiteral("The GPU resize output size overflows.");
            return result;
        }
        const std::size_t outputBytes =
            packedRowBytes * static_cast<std::size_t>(readback.pixelSize.height());
        if (!artifactDirectory || !artifactDirectory->isValid()) {
            result.error = QStringLiteral("The raster-package directory is unavailable.");
            return result;
        }
        const QString packagePath =
            artifactDirectory->filePath(QStringLiteral("base-%1.raster").arg(uuidHex()));
        const QString sharedKey = sharedSessionKey + QLatin1Char('-') + uuidHex();

        snow::image::DocumentInfo info;
        info.canvas_width = static_cast<std::uint32_t>(readback.pixelSize.width());
        info.canvas_height = static_cast<std::uint32_t>(readback.pixelSize.height());
        info.color.primaries = snow::image::ColorPrimaries::srgb;
        info.color.transfer =
            fp ? snow::image::TransferFunction::linear : snow::image::TransferFunction::srgb;
        info.color.dynamic_range = readback.color.dynamicRange == DynamicRange::High
                                       ? snow::image::DynamicRange::high
                                       : snow::image::DynamicRange::standard;
        info.color.source_peak_nits = readback.color.sourcePeakNits;
        info.color.diffuse_white_nits = readback.color.diffuseWhiteNits;
        info.frames.push_back({info.canvas_width,
                               info.canvas_height,
                               0,
                               0,
                               {},
                               fp ? snow::image::kRgba16Float : snow::image::kRgba8,
                               true,
                               {},
                               info.color});

        std::unique_ptr<MappedRasterSink> fileSink;
        std::unique_ptr<SharedRasterSink> sharedSink;
        snow::image::PixelSink* sink = nullptr;
        if (handoffMode != RasterHandoffMode::verified_file) {
            sharedSink = std::make_unique<SharedRasterSink>(sharedKey, propagatedAlpha);
            sink = sharedSink.get();
        } else {
            fileSink = std::make_unique<MappedRasterSink>(packagePath, propagatedAlpha);
            sink = fileSink.get();
        }
        auto status = sink->begin(info);
        if (!status && handoffMode == RasterHandoffMode::automatic) {
            sharedSink.reset();
            fileSink = std::make_unique<MappedRasterSink>(packagePath, propagatedAlpha);
            sink = fileSink.get();
            status = sink->begin(info);
        }
        if (status)
            status = sink->begin_frame(0, info.frames.front());
        std::span<std::byte> destination;
        if (status)
            destination = sink->frame_storage(0, packedRowBytes, outputBytes);
        if (!status || destination.size() != outputBytes) {
            if (fileSink)
                fileSink->discard();
            if (sharedSink)
                sharedSink->discard();
            result.error =
                status ? QStringLiteral("The GPU raster package could not map its output plane.")
                       : statusText(status.error());
            return result;
        }

        const auto copyRows = [&](const QByteArray& source, std::size_t sourceStride,
                                  const QRect& rect) {
            const std::size_t tileRowBytes = static_cast<std::size_t>(rect.width()) * bytesPerPixel;
            for (int row = 0; row < rect.height(); ++row) {
                if (request.cancellation->stop_requested())
                    return false;
                std::memcpy(destination.data() +
                                static_cast<std::size_t>(rect.y() + row) * packedRowBytes +
                                static_cast<std::size_t>(rect.x()) * bytesPerPixel,
                            source.constData() + static_cast<std::size_t>(row) * sourceStride,
                            tileRowBytes);
            }
            return true;
        };
        bool copied = true;
        if (readback.storage) {
            copied = copyRows(*readback.storage, readback.rowStride,
                              QRect(QPoint(0, 0), readback.pixelSize));
            readback.storage.reset();
        } else {
            for (std::size_t tileIndex = 0; tileIndex < readback.tiles.size(); ++tileIndex) {
                const GpuRasterTile& tile = readback.tiles[tileIndex];
                if (!tile.storage || !copyRows(*tile.storage, tile.rowStride, tile.pixelRect)) {
                    readback.tiles[tileIndex].storage.reset();
                    copied = false;
                    break;
                }
                // A tile is no longer needed once its final row has reached the
                // destination plane; keep no GPU readback ownership across dispatch.
                readback.tiles[tileIndex].storage.reset();
                if (request.cancellation->stop_requested()) {
                    copied = false;
                    break;
                }
            }
        }
        if (!copied) {
            if (fileSink)
                fileSink->discard();
            if (sharedSink)
                sharedSink->discard();
            return result;
        }
        status = sink->end_frame(0);
        if (status)
            status = sink->end();
        if (!status) {
            if (fileSink)
                fileSink->discard();
            if (sharedSink)
                sharedSink->discard();
            result.error = statusText(status.error());
            return result;
        }
        recordTiming(&result.timings, QStringLiteral("exact.raw_buffer_prepare"), &timer);

        if (fileSink) {
            const auto lease =
                TemporaryFileLease::adopt(packagePath, artifactDirectory, &result.error);
            if (lease)
                result.raster = fileSink->takePackage(&result.error, lease);
            if (!result.raster)
                fileSink->discard();
        } else {
            result.raster = sharedSink->takePackage(&result.error);
            if (!result.raster)
                sharedSink->discard();
        }
        readback = {};
        result.provenance = RasterProvenance::gpu_approximate;
        recordTiming(&result.timings, QStringLiteral("exact.base_package"), &timer);
        return result;
    }));
}

void EditPipelineController::finishBasePreparation(PendingExact request, PreviewResult result) {
    activeWorkerCount_ = std::max(0, activeWorkerCount_ - 1);
    reservedWorkerBytes_ -= std::min(reservedWorkerBytes_, request.estimatedWorkingBytes);
    std::erase(workerCancellations_, request.cancellation);
    lastReadbackBytes_ = 0;
    const bool current = request.requestId == latestRequestId_ &&
                         request.settings == latestSettings_ &&
                         !request.cancellation->stop_requested();
    if (!result.error.isEmpty()) {
        if (current) {
            setState(EditPipelineState::Failed);
            emit failed(result.error);
        }
        startPendingExact();
        return;
    }
    if (!result.raster) {
        if (current) {
            setState(EditPipelineState::Failed);
            emit failed(QStringLiteral("The base raster package was not produced."));
        }
        startPendingExact();
        return;
    }
    for (const PerformanceTiming& timing : result.timings) {
        emit performanceStageCompleted(request.requestId, timing.stage, timing.nanoseconds);
    }
    cacheRaster(request.key.base, result.raster, result.provenance);
    if (!current) {
        startPendingExact();
        return;
    }
    dispatchWorker(std::move(request), std::move(result.raster), result.provenance);
}

void EditPipelineController::dispatchWorker(PendingExact request,
                                            std::shared_ptr<MappedRasterPackage> package,
                                            RasterProvenance provenance) {
    if (!package || !artifactDirectory_ || !artifactDirectory_->isValid()) {
        setState(EditPipelineState::Failed);
        emit failed(QStringLiteral("The image export package is unavailable."));
        return;
    }
    auto plan = editResourcePlan(request.settings, package->source().descriptor(),
                                 editingBudgetBytes_, false);
    constexpr std::uint64_t kRuntimeHeadroom = std::uint64_t{64} << 20U;
    std::uint64_t required = std::numeric_limits<std::uint64_t>::max();
    if (plan) {
        std::uint64_t phasePrivate = 0;
        for (const snow::image::PhaseResourcePlan& phase : plan.value().phases) {
            if (phase.phase == snow::image::ResourcePhase::encode ||
                phase.phase == snow::image::ResourcePhase::validate ||
                phase.phase == snow::image::ResourcePhase::preview) {
                phasePrivate = std::max(phasePrivate, phase.footprint.private_memory_bytes);
            }
        }
        if (phasePrivate <= std::numeric_limits<std::uint64_t>::max() - kRuntimeHeadroom) {
            required = phasePrivate + kRuntimeHeadroom;
        }
    }
    const std::size_t sourceBytes =
        sourceDocument_ && !sourceRaster_ ? documentResidentBytes(*sourceDocument_) : 0;
    auto residentBytes = [&]() -> std::uint64_t {
        return static_cast<std::uint64_t>(sourceBytes) + cacheBytes_ + reservedWorkerBytes_;
    };
    while ((!plan || required > editingBudgetBytes_ ||
            residentBytes() >
                editingBudgetBytes_ - std::min<std::uint64_t>(required, editingBudgetBytes_)) &&
           (!cache_.empty() || !rasterCache_.empty()) && evictOldestCachePortion()) {
    }
    const std::uint64_t resident = residentBytes();
    if (!plan || required > editingBudgetBytes_ || resident > editingBudgetBytes_ ||
        required > editingBudgetBytes_ - resident) {
        const std::uint64_t available =
            resident < editingBudgetBytes_ ? editingBudgetBytes_ - resident : 0;
        setState(EditPipelineState::Failed);
        emit failed(QStringLiteral("Exact export requires %1 bytes; %2 bytes are available. "
                                   "The live preview remains available.")
                        .arg(QString::number(required), QString::number(available)));
        startPendingExact();
        return;
    }
    request.estimatedWorkingBytes = static_cast<std::size_t>(required);
    ++activeWorkerCount_;
    reservedWorkerBytes_ += request.estimatedWorkingBytes;
    activeWorkerJob_ = std::make_unique<ActiveWorkerJob>();
    activeWorkerJob_->request = std::move(request);
    activeWorkerJob_->baseAsset = package->isSharedMemory()
                                      ? RasterAsset::sharedMemory(std::move(package))
                                      : RasterAsset::fileBacked(std::move(package));
    activeWorkerJob_->provenance = provenance;
    activeWorkerJob_->testMode = std::move(options_.workerTestMode);
    options_.workerTestMode.clear();
    snow::image::Service service;
    const snow::image::EncoderInfo* encoder =
        service.encoder_info(activeWorkerJob_->request.settings.format);
    activeWorkerJob_->cooperative =
        encoder && encoder->cancellation == snow::image::CodecCancellation::cooperative;
    activeWorkerJob_->nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString baseName = activeWorkerJob_->nonce;
    activeWorkerJob_->artifactPath = artifactDirectory_->filePath(QStringLiteral("%1.%2").arg(
        baseName, extensionFor(activeWorkerJob_->request.settings.format)));
    activeWorkerJob_->previewPath =
        artifactDirectory_->filePath(QStringLiteral("%1.preview.raster").arg(baseName));
    activeWorkerJob_->timer.start();
    if (!worker_ || worker_->state() == QProcess::NotRunning)
        startWorker();
    sendActiveWorkerJob();
}

void EditPipelineController::dispatchPreviewWorker(PendingExact request,
                                                   std::shared_ptr<const EncodedArtifact> artifact,
                                                   RasterProvenance provenance,
                                                   snow::image::AlphaContent alphaContent) {
    if (!artifact || !artifactDirectory_ || !artifactDirectory_->isValid())
        return;
    request.cancellation = std::make_shared<std::stop_source>();
    const auto sourceDescriptor = planningSourceDescriptor(request.settings, {}, {});
    const auto plan =
        sourceDescriptor ? editResourcePlan(request.settings, sourceDescriptor.value(),
                                            editingBudgetBytes_, false)
                         : snow::image::Result<snow::image::ResourcePlan>(sourceDescriptor.error());
    request.estimatedWorkingBytes =
        plan ? static_cast<std::size_t>(std::min<std::uint64_t>(
                   phasePrivateBytes(plan.value(), snow::image::ResourcePhase::preview) +
                       (std::uint64_t{64} << 20U),
                   std::numeric_limits<std::size_t>::max()))
             : editingBudgetBytes_;
    if (activeWorkerCount_ >= 1 || activeWorkerJob_) {
        if (pendingPreview_)
            pendingPreview_->request.cancellation->request_stop();
        pendingPreview_ =
            PendingPreview{std::move(request), std::move(artifact), provenance, alphaContent};
        return;
    }
    ++activeWorkerCount_;
    reservedWorkerBytes_ += request.estimatedWorkingBytes;
    activeWorkerJob_ = std::make_unique<ActiveWorkerJob>();
    activeWorkerJob_->request = std::move(request);
    activeWorkerJob_->provenance = provenance;
    activeWorkerJob_->alphaContent = alphaContent;
    activeWorkerJob_->artifact = std::move(artifact);
    activeWorkerJob_->artifactPath = activeWorkerJob_->artifact->path();
    activeWorkerJob_->nonce = QFileInfo(activeWorkerJob_->artifactPath).completeBaseName();
    activeWorkerJob_->previewKind = QStringLiteral("codec");
    activeWorkerJob_->previewOnly = true;
    activeWorkerJob_->previewPath = artifactDirectory_->filePath(
        QStringLiteral("%1.preview-recovery-%2.raster").arg(activeWorkerJob_->nonce, uuidHex()));
    activeWorkerJob_->timer.start();
    if (!worker_ || worker_->state() == QProcess::NotRunning)
        startWorker();
    sendActiveWorkerJob();
}

void EditPipelineController::startWorker() {
    if (shuttingDown_ || (worker_ && worker_->state() != QProcess::NotRunning))
        return;
    if (worker_) {
        worker_->deleteLater();
        worker_ = nullptr;
    }
    workerOutput_.clear();
    workerReady_ = false;
    worker_ = new QProcess(this);
    worker_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(worker_, &QProcess::readyReadStandardOutput, this,
            &EditPipelineController::handleWorkerOutput);
    connect(worker_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { handleWorkerExit(exitCode); });
    connect(worker_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            handleWorkerExit(-1);
    });
    QString executable = options_.workerExecutablePath;
    const QString name = QStringLiteral("snow_image_edit_worker") +
#if defined(Q_OS_WIN)
                         QStringLiteral(".exe");
#else
                         QString();
#endif
    if (executable.isEmpty()) {
        const QString appDirectory = QCoreApplication::applicationDirPath();
        executable = QDir(appDirectory).filePath(name);
        if (!QFileInfo(executable).isExecutable()) {
            const QString visualStudioCandidate =
                QDir(appDirectory).absoluteFilePath(QStringLiteral("../../Release/%1").arg(name));
            if (QFileInfo(visualStudioCandidate).isExecutable())
                executable = visualStudioCandidate;
        }
    }
    worker_->setProgram(executable);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SNOW_IMAGE_WORKER_MEMORY_LIMIT"),
                       QString::number(editingBudgetBytes_));
    worker_->setProcessEnvironment(environment);
    if (workerEverStarted_)
        ++workerRestartCount_;
    workerEverStarted_ = true;
    workerStartupTimer_.start();
    worker_->start(QIODevice::ReadWrite);
}

void EditPipelineController::sendActiveWorkerJob() {
    if (!activeWorkerJob_ || activeWorkerJob_->sent || !workerReady_ || !worker_ ||
        worker_->state() != QProcess::Running)
        return;
    EditExportSettings workerSettings = activeWorkerJob_->request.settings;
    snow::image::Service service;
    const snow::image::EncoderInfo* encoder = service.encoder_info(workerSettings.format);
    const auto normalizedOptions =
        encoder ? snow::image::normalize_encode_options(*encoder, workerSettings.encode)
                : snow::image::Result<snow::image::EncodeOptions>(
                      snow::image::Status::error(snow::image::ErrorCode::unsupported_format,
                                                 "The requested export encoder is unavailable."));
    if (!normalizedOptions) {
        const PendingExact request = activeWorkerJob_->request;
        cancelActiveWorker(true);
        if (request.requestId == latestRequestId_) {
            setState(EditPipelineState::Failed);
            emit failed(statusText(normalizedOptions.error()));
        }
        return;
    }
    workerSettings.encode = normalizedOptions.value();
    QJsonObject job{
        {QStringLiteral("requestId"), QString::number(activeWorkerJob_->request.requestId)},
        {QStringLiteral("nonce"), activeWorkerJob_->nonce},
        {QStringLiteral("sessionDirectory"), artifactDirectory_->path()},
        {QStringLiteral("artifactPath"), activeWorkerJob_->artifactPath},
        {QStringLiteral("previewPath"), activeWorkerJob_->previewPath},
        {QStringLiteral("settings"), worker_protocol::settingsToJson(workerSettings)},
        {QStringLiteral("provenance"), static_cast<int>(activeWorkerJob_->provenance)},
        {QStringLiteral("sourceFrameCount"),
         sourceDocument_ ? static_cast<int>(sourceDocument_->frames.size()) : 1},
        {QStringLiteral("memoryLimit"),
         QString::number(activeWorkerJob_->request.estimatedWorkingBytes)}};
    if (!activeWorkerJob_->previewOnly) {
        if (!activeWorkerJob_->baseAsset) {
            cancelActiveWorker(true);
            setState(EditPipelineState::Failed);
            emit failed(QStringLiteral("The image export raster asset is unavailable."));
            return;
        }
        job.insert(QStringLiteral("baseRaster"), activeWorkerJob_->baseAsset->workerTransport());
        if (const auto alpha = activeWorkerJob_->baseAsset->verifiedAlphaContent()) {
            job.insert(QStringLiteral("verifiedAlphaContent"), static_cast<int>(*alpha));
        }
    }
    if (!activeWorkerJob_->testMode.isEmpty()) {
        job.insert(QStringLiteral("testMode"), activeWorkerJob_->testMode);
    }
    const QByteArray frame = worker_protocol::encodeFrame(
        activeWorkerJob_->previewOnly ? worker_protocol::MessageType::preview_job
                                      : worker_protocol::MessageType::encode_job,
        job);
    if (worker_->write(frame) != frame.size()) {
        const PendingExact request = activeWorkerJob_->request;
        cancelActiveWorker(true);
        if (request.requestId == latestRequestId_) {
            setState(EditPipelineState::Failed);
            emit failed(QStringLiteral("The image export job could not be sent to the worker."));
        }
        return;
    }
    activeWorkerJob_->sent = true;
    emit performanceStageCompleted(activeWorkerJob_->request.requestId,
                                   QStringLiteral("worker.job_dispatched"), 0);
    if (replacementDispatchTimer_.isValid() &&
        activeWorkerJob_->request.requestId == replacementDispatchRequestId_) {
        emit performanceStageCompleted(activeWorkerJob_->request.requestId,
                                       QStringLiteral("worker.replacement_dispatch"),
                                       replacementDispatchTimer_.nsecsElapsed());
        replacementDispatchTimer_.invalidate();
        replacementDispatchRequestId_ = 0;
    }
    workerTimeout_.start(options_.workerTimeoutMs);
}

void EditPipelineController::handleWorkerOutput() {
    if (!worker_)
        return;
    workerOutput_.append(worker_->readAllStandardOutput());
    for (;;) {
        worker_protocol::Frame frame;
        QString frameError;
        if (!worker_protocol::takeFrame(&workerOutput_, &frame, &frameError)) {
            if (!frameError.isEmpty()) {
                const bool affected =
                    activeWorkerJob_ && activeWorkerJob_->request.requestId == latestRequestId_;
                cancelActiveWorker(true);
                if (affected) {
                    setState(EditPipelineState::Failed);
                    emit failed(frameError);
                }
            }
            return;
        }
        const QJsonObject& object = frame.payload;
        if (frame.type == worker_protocol::MessageType::ready) {
            if (object.value(QStringLiteral("protocolVersion")).toInt() !=
                static_cast<int>(worker_protocol::kVersion)) {
                cancelActiveWorker(false);
                return;
            }
            workerStartupNanoseconds_ =
                workerStartupTimer_.isValid() ? workerStartupTimer_.nsecsElapsed() : 0;
            emit performanceStageCompleted(latestRequestId_, QStringLiteral("worker.startup"),
                                           workerStartupNanoseconds_);
            workerReady_ = true;
            sendActiveWorkerJob();
            continue;
        }
        if (frame.type == worker_protocol::MessageType::cancelled && activeWorkerJob_ &&
            object.value(QStringLiteral("requestId")).toString() ==
                QString::number(activeWorkerJob_->request.requestId) &&
            object.value(QStringLiteral("nonce")).toString() == activeWorkerJob_->nonce) {
            workerTimeout_.stop();
            auto job = std::move(activeWorkerJob_);
            activeWorkerCount_ = std::max(0, activeWorkerCount_ - 1);
            reservedWorkerBytes_ -=
                std::min(reservedWorkerBytes_, job->request.estimatedWorkingBytes);
            cleanupWorkerJob(*job);
            emit performanceStageCompleted(
                latestRequestId_, QStringLiteral("worker.cooperative_cancellation"),
                job->cancellationTimer.isValid() ? job->cancellationTimer.nsecsElapsed() : 0);
            startPendingExact();
            continue;
        }
        if (!activeWorkerJob_)
            continue;
        if (object.value(QStringLiteral("requestId")).toString() !=
                QString::number(activeWorkerJob_->request.requestId) ||
            object.value(QStringLiteral("nonce")).toString() != activeWorkerJob_->nonce) {
            continue;
        }
        if (frame.type == worker_protocol::MessageType::artifact_ready) {
            if (object.value(QStringLiteral("artifactPath")).toString() !=
                activeWorkerJob_->artifactPath) {
                cancelActiveWorker(true);
                setState(EditPipelineState::Failed);
                emit failed(
                    QStringLiteral("The image export worker returned a mismatched artifact."));
                continue;
            }
            QString artifactError;
            snow::image::EncodedArtifactReceipt receipt;
            if (!worker_protocol::receiptFromJson(object.value(QStringLiteral("receipt")), &receipt,
                                                  &artifactError) ||
                receipt.format != activeWorkerJob_->request.settings.format) {
                cancelActiveWorker(true);
                setState(EditPipelineState::Failed);
                emit failed(
                    artifactError.isEmpty()
                        ? QStringLiteral(
                              "The image export worker returned an invalid artifact receipt.")
                        : artifactError);
                continue;
            }
            bool artifactBytesValid = false;
            const std::uint64_t artifactBytes = object.value(QStringLiteral("artifactBytes"))
                                                    .toString()
                                                    .toULongLong(&artifactBytesValid);
            const QFileInfo artifactInfo(activeWorkerJob_->artifactPath);
            if (!artifactBytesValid || artifactBytes == 0 || !artifactInfo.isFile() ||
                static_cast<std::uint64_t>(artifactInfo.size()) != artifactBytes) {
                cancelActiveWorker(true);
                setState(EditPipelineState::Failed);
                emit failed(QStringLiteral(
                    "The image export worker returned an artifact with an invalid size."));
                continue;
            }
            const auto lease = TemporaryFileLease::adopt(activeWorkerJob_->artifactPath,
                                                         artifactDirectory_, &artifactError);
            activeWorkerJob_->artifact = EncodedArtifact::adopt(
                activeWorkerJob_->request.settings.format, activeWorkerJob_->artifactPath,
                std::move(receipt), lease, &artifactError);
            if (!activeWorkerJob_->artifact) {
                cancelActiveWorker(true);
                setState(EditPipelineState::Failed);
                emit failed(artifactError);
                continue;
            }
            activeWorkerJob_->warning = object.value(QStringLiteral("warning")).toString();
            activeWorkerJob_->previewKind = object.value(QStringLiteral("previewKind")).toString();
            const int alphaContent = object.value(QStringLiteral("alphaContent")).toInt(-1);
            if (alphaContent < static_cast<int>(snow::image::AlphaContent::opaque) ||
                alphaContent > static_cast<int>(snow::image::AlphaContent::non_opaque)) {
                cancelActiveWorker(true);
                setState(EditPipelineState::Failed);
                emit failed(
                    QStringLiteral("The image export worker returned invalid alpha metadata."));
                continue;
            }
            activeWorkerJob_->alphaContent = static_cast<snow::image::AlphaContent>(alphaContent);
            const PendingExact request = activeWorkerJob_->request;
            const QString jobNonce = activeWorkerJob_->nonce;
            const qint64 requestToArtifact = activeWorkerJob_->timer.nsecsElapsed();
            activeWorkerJob_->artifactTimer.start();
            ExactEditResult cached;
            cached.requestId = request.requestId;
            cached.settings = request.settings;
            cached.warning = activeWorkerJob_->warning;
            if (activeWorkerJob_->previewKind == QStringLiteral("base")) {
                cached.previewSource =
                    activeWorkerJob_->provenance != RasterProvenance::cpu_reference
                        ? ExactPreviewSource::gpu_raster
                        : ExactPreviewSource::base_raster;
            } else if (activeWorkerJob_->previewKind == QStringLiteral("prepared")) {
                cached.previewSource = ExactPreviewSource::prepared_raster;
            } else {
                cached.previewSource = ExactPreviewSource::codec_artifact;
            }
            cached.artifact = activeWorkerJob_->artifact;
            cached.provenance = activeWorkerJob_->provenance;
            cached.alphaContent = activeWorkerJob_->alphaContent;
            cacheResult(request.key, cached);
            if (request.requestId == latestRequestId_ && request.settings == latestSettings_) {
                exactResult_ = cached;
                previewPending_ = true;
                setState(EditPipelineState::ArtifactReady);
                EncodedEditResult encoded{request.requestId, request.settings,
                                          cached.warning,    cached.artifact,
                                          cached.provenance, cached.alphaContent};
                emit artifactReady(encoded);
                emit performanceStageCompleted(request.requestId,
                                               QStringLiteral("request_to_artifact_ready"),
                                               requestToArtifact);
            }
            const auto addTiming = [&](const char* key, const char* stage) {
                bool okay = false;
                const qint64 value =
                    object.value(QString::fromLatin1(key)).toString().toLongLong(&okay);
                if (okay)
                    emit performanceStageCompleted(request.requestId, QString::fromLatin1(stage),
                                                   value);
            };
            addTiming("prepareNs", "exact.prepare_export");
            addTiming("alphaClassificationNs", "exact.classify_alpha");
            addTiming("encodeNs", "exact.encode");
            addTiming("directNativeEncodeNs", "exact.direct_native_encode");
            addTiming("validateReceiptNs", "exact.validate_receipt");
            if (activeWorkerJob_ && activeWorkerJob_->nonce == jobNonce)
                workerTimeout_.start(options_.workerTimeoutMs);
            continue;
        }
        if (frame.type == worker_protocol::MessageType::job_failed) {
            workerTimeout_.stop();
            if (activeWorkerJob_ && !activeWorkerJob_->previewOnly &&
                !activeWorkerJob_->sharedMemoryRetryAttempted && activeWorkerJob_->baseAsset &&
                object.value(QStringLiteral("retriableSharedMemoryAttach")).toBool()) {
                QString materializeError;
                const QString fallbackPath = artifactDirectory_->filePath(
                    QStringLiteral("base-retry-%1.raster").arg(uuidHex()));
                const snow::image::Document* retryDocument =
                    activeWorkerJob_->baseAsset->document();
                std::shared_ptr<MappedRasterPackage> fallback =
                    retryDocument ? MappedRasterPackage::create(
                                        fallbackPath, *retryDocument, &materializeError, {},
                                        activeWorkerJob_->baseAsset->verifiedAlphaContent())
                                  : nullptr;
                if (fallback) {
                    const auto lease = TemporaryFileLease::adopt(fallbackPath, artifactDirectory_,
                                                                 &materializeError);
                    if (lease) {
                        fallback->retainCleanupOwner(lease);
                        activeWorkerJob_->baseAsset = RasterAsset::fileBacked(std::move(fallback));
                        activeWorkerJob_->sharedMemoryRetryAttempted = true;
                        activeWorkerJob_->sent = false;
                        activeWorkerJob_->cancelling = false;
                        QFile::remove(activeWorkerJob_->artifactPath + QStringLiteral(".partial"));
                        QFile::remove(activeWorkerJob_->previewPath + QStringLiteral(".partial"));
                        sendActiveWorkerJob();
                        continue;
                    }
                }
            }
            auto job = std::move(activeWorkerJob_);
            const QString error = object.value(QStringLiteral("error"))
                                      .toString(QStringLiteral("The image export worker failed."));
            if (job->artifact) {
                activeWorkerCount_ = std::max(0, activeWorkerCount_ - 1);
                reservedWorkerBytes_ -=
                    std::min(reservedWorkerBytes_, job->request.estimatedWorkingBytes);
                std::erase(workerCancellations_, job->request.cancellation);
                QFile::remove(job->previewPath + QStringLiteral(".partial"));
                QFile::remove(job->previewPath);
                if (job->request.settings.format == snow::image::Format::jxl)
                    retireIdleWorker();
                if (job->request.requestId == latestRequestId_) {
                    previewPending_ = false;
                    setState(EditPipelineState::ArtifactReady);
                    emit previewUnavailable(job->request.requestId, error);
                }
                startPendingExact();
            } else {
                PreviewResult result;
                result.error = error;
                result.provenance = job->provenance;
                cleanupWorkerJob(*job);
                finishWorker(job->request, std::move(result));
            }
            continue;
        }
        if (frame.type != worker_protocol::MessageType::preview_ready)
            continue;
        workerTimeout_.stop();
        auto job = std::move(activeWorkerJob_);
        PreviewResult result;
        result.provenance = job->provenance;
        result.alphaContent = job->alphaContent;
        result.artifact = job->artifact;
        result.warning = job->warning;
        QElapsedTimer previewMapping;
        previewMapping.start();
        std::shared_ptr<MappedRasterPackage> previewPackage;
        if (job->previewKind == QStringLiteral("base")) {
            if (job->provenance != RasterProvenance::cpu_reference) {
                result.previewSource = ExactPreviewSource::gpu_raster;
            } else {
                result.previewSource = ExactPreviewSource::base_raster;
                previewPackage = job->baseAsset ? job->baseAsset->packageShared() : nullptr;
            }
        } else if (job->previewKind == QStringLiteral("prepared") ||
                   job->previewKind == QStringLiteral("codec")) {
            const QString previewPath = object.value(QStringLiteral("previewPath")).toString();
            if (previewPath != job->previewPath) {
                result.error = QStringLiteral("The worker returned a mismatched preview package.");
            } else {
                const auto previewLease =
                    TemporaryFileLease::adopt(previewPath, artifactDirectory_, &result.error);
                if (previewLease)
                    previewPackage =
                        MappedRasterPackage::open(previewPath, &result.error, previewLease);
            }
            result.previewSource = job->previewKind == QStringLiteral("prepared")
                                       ? ExactPreviewSource::prepared_raster
                                       : ExactPreviewSource::codec_artifact;
        } else {
            result.error = QStringLiteral("The worker returned an invalid preview source.");
        }
        if (result.error.isEmpty() && previewPackage) {
            if (const snow::image::Document* previewDocument = previewPackage->document()) {
                DecodeCancellation cancellation;
                const std::string_view formatName =
                    snow::image::format_name(job->request.settings.format);
                DecodeResult prepared = prepareMappedSnowDocument(
                    sourcePath_, *previewDocument,
                    QString::fromUtf8(formatName.data(), static_cast<qsizetype>(formatName.size())),
                    cancellation);
                if (!prepared.succeeded())
                    result.error = prepared.error;
                else
                    result.displayPreview = std::move(prepared.image);
            } else if (previewPackage->store() && previewPackage->store()->complete()) {
                const auto& descriptor = previewPackage->source().descriptor();
                const QSize fullSize(static_cast<int>(descriptor.canvas_width),
                                     static_cast<int>(descriptor.canvas_height));
                auto backing = ImageRasterStore::retain(previewPackage->path(),
                                                        previewPackage->store(), previewPackage);
                if (!backing || !fullSize.isValid()) {
                    result.error = QStringLiteral("The native preview raster is invalid.");
                } else {
                    DecodedImage preview;
                    preview.filePath = sourcePath_;
                    preview.sourceSize = fullSize;
                    preview.pixelEncoding = PixelEncoding::Srgb8;
                    preview.pixelsPremultiplied = true;
                    preview.color.sourceColorSpace = QColorSpace(QColorSpace::SRgb);
                    preview.color.description = QStringLiteral("sRGB");
                    preview.decoderName = QStringLiteral("libjpeg-turbo native raster");
                    preview.analysis = backing->analysis();
                    preview.rasterStore = backing;
                    preview.tileStore = std::make_shared<ImageTileStore>(backing, fullSize,
                                                                         QImage{}, QSize(512, 512));
                    result.displayPreview = std::move(preview);
                }
            } else {
                result.error = QStringLiteral("The preview raster is unavailable.");
            }
        }
        result.timings.push_back(
            {QStringLiteral("exact.preview_mapping"), previewMapping.nsecsElapsed()});
        result.timings.push_back(
            {QStringLiteral("artifact_to_preview_ready"),
             job->artifactTimer.isValid() ? job->artifactTimer.nsecsElapsed() : 0});
        QFile::remove(job->previewPath + QStringLiteral(".partial"));
        finishWorker(job->request, std::move(result));
    }
}

void EditPipelineController::handleWorkerExit(int exitCode) {
    Q_UNUSED(exitCode);
    if (shuttingDown_)
        return;
    const bool affected =
        activeWorkerJob_ && activeWorkerJob_->request.requestId == latestRequestId_;
    const bool artifactPublished = activeWorkerJob_ && activeWorkerJob_->artifact;
    if (activeWorkerJob_) {
        activeWorkerCount_ = std::max(0, activeWorkerCount_ - 1);
        reservedWorkerBytes_ -=
            std::min(reservedWorkerBytes_, activeWorkerJob_->request.estimatedWorkingBytes);
        cleanupWorkerJob(*activeWorkerJob_);
        activeWorkerJob_.reset();
    }
    workerTimeout_.stop();
    if (worker_) {
        worker_->deleteLater();
        worker_ = nullptr;
    }
    workerReady_ = false;
    if (affected) {
        if (artifactPublished) {
            previewPending_ = false;
            setState(EditPipelineState::ArtifactReady);
            emit previewUnavailable(
                latestRequestId_,
                QStringLiteral(
                    "The exact comparison preview is unavailable because the worker exited."));
        } else {
            setState(EditPipelineState::Failed);
            emit failed(QStringLiteral("The image export worker exited unexpectedly."));
        }
    }
    startWorker();
    startPendingExact();
}

void EditPipelineController::cleanupWorkerJob(const ActiveWorkerJob& job) {
    QFile::remove(job.artifactPath + QStringLiteral(".partial"));
    QFile::remove(job.previewPath + QStringLiteral(".partial"));
    if (!job.artifact)
        QFile::remove(job.artifactPath);
    QFile::remove(job.previewPath);
}

void EditPipelineController::cancelActiveWorker(bool restart) {
    if (!activeWorkerJob_ && (!worker_ || worker_->state() == QProcess::NotRunning)) {
        if (restart)
            startWorker();
        return;
    }
    workerTimeout_.stop();
    QElapsedTimer cancellation;
    cancellation.start();
    if (activeWorkerJob_ && !activeWorkerJob_->sent) {
        activeWorkerJob_->request.cancellation->request_stop();
        activeWorkerCount_ = std::max(0, activeWorkerCount_ - 1);
        reservedWorkerBytes_ -=
            std::min(reservedWorkerBytes_, activeWorkerJob_->request.estimatedWorkingBytes);
        auto job = std::move(activeWorkerJob_);
        cleanupWorkerJob(*job);
        ++cancellationCount_;
        emit performanceStageCompleted(latestRequestId_,
                                       QStringLiteral("worker.cooperative_cancellation"),
                                       cancellation.nsecsElapsed());
        if (restart && (!worker_ || worker_->state() == QProcess::NotRunning))
            startWorker();
        return;
    }
    if (activeWorkerJob_ && activeWorkerJob_->cooperative && activeWorkerJob_->sent &&
        !activeWorkerJob_->cancelling && restart && worker_ &&
        worker_->state() == QProcess::Running) {
        activeWorkerJob_->request.cancellation->request_stop();
        activeWorkerJob_->cancelling = true;
        activeWorkerJob_->cancellationTimer.start();
        const QByteArray frame = worker_protocol::encodeFrame(
            worker_protocol::MessageType::cancel,
            {{QStringLiteral("requestId"), QString::number(activeWorkerJob_->request.requestId)},
             {QStringLiteral("nonce"), activeWorkerJob_->nonce}});
        if (worker_->write(frame) == frame.size()) {
            ++cancellationCount_;
            replacementDispatchRequestId_ = latestRequestId_;
            replacementDispatchTimer_.start();
            workerTimeout_.start(100);
            return;
        }
        activeWorkerJob_->cooperative = false;
    }
    std::unique_ptr<ActiveWorkerJob> job;
    if (activeWorkerJob_) {
        activeWorkerJob_->request.cancellation->request_stop();
        activeWorkerCount_ = std::max(0, activeWorkerCount_ - 1);
        reservedWorkerBytes_ -=
            std::min(reservedWorkerBytes_, activeWorkerJob_->request.estimatedWorkingBytes);
        job = std::move(activeWorkerJob_);
        ++cancellationCount_;
    }
    if (worker_) {
        worker_->disconnect(this);
        if (worker_->state() != QProcess::NotRunning) {
            worker_->kill();
            worker_->waitForFinished(25);
        }
        worker_->deleteLater();
        worker_ = nullptr;
    }
    if (job)
        cleanupWorkerJob(*job);
    workerReady_ = false;
    workerOutput_.clear();
    replacementDispatchTimer_.invalidate();
    replacementDispatchRequestId_ = 0;
    emit performanceStageCompleted(latestRequestId_, QStringLiteral("worker.cancellation_latency"),
                                   cancellation.nsecsElapsed());
    if (restart && !shuttingDown_)
        startWorker();
}

void EditPipelineController::retireIdleWorker() {
    if (activeWorkerJob_ || !worker_)
        return;
    worker_->disconnect(this);
    if (worker_->state() != QProcess::NotRunning) {
        worker_->kill();
        worker_->waitForFinished(100);
    }
    worker_->deleteLater();
    worker_ = nullptr;
    workerReady_ = false;
    workerOutput_.clear();
    if (!shuttingDown_)
        startWorker();
}

void EditPipelineController::finishWorker(const PendingExact& request, PreviewResult result) {
    activeWorkerCount_ = std::max(0, activeWorkerCount_ - 1);
    reservedWorkerBytes_ -= std::min(reservedWorkerBytes_, request.estimatedWorkingBytes);
    std::erase(workerCancellations_, request.cancellation);
    if (result.raster) {
        cacheRaster(request.key.base, result.raster, result.provenance);
    }
    // libjxl can retain a large committed heap after an encode. Retire the isolated
    // worker before another JPEG XL job receives a fresh per-job memory ceiling.
    if (request.settings.format == snow::image::Format::jxl)
        retireIdleWorker();
    const bool current = request.requestId == latestRequestId_ &&
                         request.settings == latestSettings_ &&
                         !request.cancellation->stop_requested();
    if (current) {
        if (!result.error.isEmpty()) {
            if (result.artifact && result.artifact->byteSize() > 0) {
                previewPending_ = false;
                setState(EditPipelineState::ArtifactReady);
                emit previewUnavailable(request.requestId, result.error);
            } else {
                setState(EditPipelineState::Failed);
                emit failed(result.error);
            }
        } else if (result.artifact && result.artifact->byteSize() > 0) {
            for (const auto& timing : result.timings) {
                emit performanceStageCompleted(request.requestId, timing.stage, timing.nanoseconds);
            }
            ExactEditResult exact;
            exact.requestId = request.requestId;
            exact.settings = request.settings;
            exact.warning = result.warning;
            exact.displayPreview = std::move(result.displayPreview);
            exact.previewSource = result.previewSource;
            exact.artifact = std::move(result.artifact);
            exact.provenance = result.provenance;
            exact.exactPreviewAvailable = true;
            exact.alphaContent = result.alphaContent;
            cacheResult(request.key, exact);
            exactResult_ = exact;
            setState(EditPipelineState::ExactReady);
            // The result is passed by value; a handler may synchronously clear
            // exactResult_ without affecting other receivers.
            emit exactReady(exact);
        }
    }
    if (!current && result.artifact && result.artifact->byteSize() > 0) {
        ExactEditResult cached;
        cached.settings = request.settings;
        cached.warning = result.warning;
        cached.displayPreview = std::move(result.displayPreview);
        cached.previewSource = result.previewSource;
        cached.artifact = std::move(result.artifact);
        cached.provenance = result.provenance;
        cached.exactPreviewAvailable = true;
        cached.alphaContent = result.alphaContent;
        cacheResult(request.key, cached);
    }
    startPendingExact();
}

// QTimer owns the queued callback and its captured shared cancellation state.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
bool EditPipelineController::publishCacheHit(EditRequestId requestId,
                                             const EditExportSettings& settings,
                                             const ExportKey& key) {
    const auto found = std::find_if(cache_.begin(), cache_.end(),
                                    [&](const CacheEntry& entry) { return entry.key == key; });
    if (found == cache_.end())
        return false;
    found->artifactAccessSerial = ++accessSerial_;
    setState(EditPipelineState::EncodingPending);
    QTimer::singleShot(0, this, [this, requestId, settings]() {
        if (requestId != latestRequestId_ || settings != latestSettings_ || cache_.empty())
            return;
        const ExportKey key = exportKey(settings, sourceGeneration_);
        const auto found = std::find_if(cache_.rbegin(), cache_.rend(),
                                        [&](const CacheEntry& entry) { return entry.key == key; });
        if (found == cache_.rend())
            return;
        found->artifactAccessSerial = ++accessSerial_;
        if (found->result.exactPreviewAvailable)
            found->previewAccessSerial = found->artifactAccessSerial;
        ExactEditResult exact = found->result;
        exact.requestId = requestId;
        exact.settings = settings;
        exactResult_ = exact;
        emit performanceStageCompleted(requestId, QStringLiteral("exact.cache_hit"), 0);
        previewPending_ = !exact.exactPreviewAvailable;
        setState(exact.exactPreviewAvailable ? EditPipelineState::ExactReady
                                             : EditPipelineState::ArtifactReady);
        EncodedEditResult encoded{requestId,      settings,         exact.warning,
                                  exact.artifact, exact.provenance, exact.alphaContent};
        emit artifactReady(encoded);
        if (exact.exactPreviewAvailable) {
            emit exactReady(exact);
        } else {
            PendingExact previewRequest{requestId, settings, std::make_shared<std::stop_source>(),
                                        key, 0};
            const auto sourceDescriptor = planningSourceDescriptor(settings, {}, {});
            const auto previewPlan =
                sourceDescriptor
                    ? editResourcePlan(settings, sourceDescriptor.value(), editingBudgetBytes_,
                                       false)
                    : snow::image::Result<snow::image::ResourcePlan>(sourceDescriptor.error());
            previewRequest.estimatedWorkingBytes =
                previewPlan ? static_cast<std::size_t>(std::min<std::uint64_t>(
                                  phasePrivateBytes(previewPlan.value(),
                                                    snow::image::ResourcePhase::preview) +
                                      (std::uint64_t{64} << 20U),
                                  std::numeric_limits<std::size_t>::max()))
                            : editingBudgetBytes_;
            QElapsedTimer recoveryTimer;
            recoveryTimer.start();
            bool recovered = false;
            const ExactPreviewSource originalSource = exact.previewSource;
            if (originalSource == ExactPreviewSource::gpu_raster ||
                originalSource == ExactPreviewSource::base_raster) {
                RasterProvenance rasterProvenance = RasterProvenance::cpu_reference;
                const std::shared_ptr<MappedRasterPackage> raster =
                    findRaster(key.base, &rasterProvenance);
                if (raster && rasterProvenance == exact.provenance) {
                    if (originalSource == ExactPreviewSource::gpu_raster &&
                        ((gpuSource_ && rasterProvenance == RasterProvenance::gpu_approximate) ||
                         rasterProvenance == RasterProvenance::source_exact)) {
                        exact.displayPreview.reset();
                        recovered = true;
                    } else if (originalSource == ExactPreviewSource::base_raster &&
                               rasterProvenance == RasterProvenance::cpu_reference) {
                        DecodeCancellation cancellation;
                        const std::string_view formatName =
                            snow::image::format_name(settings.format);
                        const snow::image::Document* rasterDocument = raster->document();
                        DecodeResult prepared =
                            rasterDocument
                                ? prepareMappedSnowDocument(
                                      sourcePath_, *rasterDocument,
                                      QString::fromUtf8(formatName.data(),
                                                        static_cast<qsizetype>(formatName.size())),
                                      cancellation)
                                : DecodeResult::failure(
                                      QStringLiteral("The cached raster is not packed."));
                        if (prepared.succeeded()) {
                            exact.displayPreview = std::move(prepared.image);
                            recovered = true;
                        }
                    }
                }
            }
            if (recovered) {
                exact.exactPreviewAvailable = true;
                exactResult_ = exact;
                previewPending_ = false;
                cacheResult(key, exact);
                setState(EditPipelineState::ExactReady);
                emit performanceStageCompleted(requestId,
                                               QStringLiteral("exact.raster_preview_recovery"),
                                               recoveryTimer.nsecsElapsed());
                emit exactReady(exact);
            } else {
                dispatchPreviewWorker(std::move(previewRequest), exact.artifact, exact.provenance,
                                      exact.alphaContent);
            }
        }
    });
    return true;
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

void EditPipelineController::cacheResult(const ExportKey& key, const ExactEditResult& result) {
    if (!result.artifact || result.artifact->byteSize() == 0)
        return;
    const auto duplicate = std::find_if(cache_.begin(), cache_.end(),
                                        [&](const CacheEntry& entry) { return entry.key == key; });
    if (duplicate != cache_.end()) {
        diskCacheBytes_ -= std::min(diskCacheBytes_, duplicate->artifactWeight);
        diskCacheBytes_ -= std::min(diskCacheBytes_, duplicate->previewDiskWeight);
        cacheBytes_ -= std::min(cacheBytes_, duplicate->previewWeight);
        cache_.erase(duplicate);
    }
    const std::size_t artifactBytes = artifactWeight(result);
    if (artifactBytes == 0 || artifactBytes > options_.cacheBudgetBytes)
        return;
    const std::size_t previewBytes = previewWeight(result);
    const std::size_t previewDiskBytes = previewDiskWeight(result);
    const std::uint64_t serial = ++accessSerial_;
    cache_.push_back({key, result, artifactBytes, previewBytes, previewDiskBytes, serial,
                      previewBytes > 0 || previewDiskBytes > 0 ? serial : 0});
    if (artifactBytes <= std::numeric_limits<std::size_t>::max() - diskCacheBytes_)
        diskCacheBytes_ += artifactBytes;
    else
        diskCacheBytes_ = std::numeric_limits<std::size_t>::max();
    if (previewDiskBytes <= std::numeric_limits<std::size_t>::max() - diskCacheBytes_)
        diskCacheBytes_ += previewDiskBytes;
    else
        diskCacheBytes_ = std::numeric_limits<std::size_t>::max();
    if (previewBytes <= std::numeric_limits<std::size_t>::max() - cacheBytes_)
        cacheBytes_ += previewBytes;
    else
        cacheBytes_ = std::numeric_limits<std::size_t>::max();
    trimCache();
}

std::shared_ptr<MappedRasterPackage>
EditPipelineController::findRaster(const BaseRasterKey& key, RasterProvenance* provenance) {
    const auto found =
        std::find_if(rasterCache_.begin(), rasterCache_.end(),
                     [&](const RasterCacheEntry& entry) { return entry.key == key; });
    if (found == rasterCache_.end())
        return {};
    found->accessSerial = ++accessSerial_;
    if (provenance)
        *provenance = found->provenance;
    return found->package;
}

void EditPipelineController::cacheRaster(const BaseRasterKey& key,
                                         std::shared_ptr<MappedRasterPackage> package,
                                         RasterProvenance provenance) {
    if (!package)
        return;
    const auto duplicate =
        std::find_if(rasterCache_.begin(), rasterCache_.end(),
                     [&](const RasterCacheEntry& entry) { return entry.key == key; });
    if (duplicate != rasterCache_.end()) {
        diskCacheBytes_ -= std::min(diskCacheBytes_, duplicate->weight);
        rasterCache_.erase(duplicate);
    }
    const std::size_t weight = static_cast<std::size_t>(
        std::min<std::uint64_t>(package->mappedBytes(), std::numeric_limits<std::size_t>::max()));
    if (weight > options_.cacheBudgetBytes)
        return;
    rasterCache_.push_back({key, std::move(package), provenance, weight, ++accessSerial_});
    if (weight <= std::numeric_limits<std::size_t>::max() - diskCacheBytes_)
        diskCacheBytes_ += weight;
    else
        diskCacheBytes_ = std::numeric_limits<std::size_t>::max();
    trimCache();
}

void EditPipelineController::trimCache() {
    while (
        (cacheBytes_ > options_.cacheBudgetBytes || diskCacheBytes_ > options_.cacheBudgetBytes) &&
        evictOldestCachePortion()) {
    }
    if (cache_.empty() && rasterCache_.empty()) {
        cacheBytes_ = 0;
        diskCacheBytes_ = 0;
    }
}

bool EditPipelineController::evictOldestCachePortion() {
    enum class Kind : std::uint8_t { none, resident_preview, native_preview, artifact, raster };
    Kind kind = Kind::none;
    std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
    auto exact = cache_.end();
    for (auto entry = cache_.begin(); entry != cache_.end(); ++entry) {
        // A tie favors dropping the recoverable preview before its artifact.
        if (entry->previewWeight > 0 && entry->previewAccessSerial <= oldest) {
            kind = Kind::resident_preview;
            oldest = entry->previewAccessSerial;
            exact = entry;
        }
        if (entry->previewDiskWeight > 0 && entry->previewAccessSerial < oldest) {
            kind = Kind::native_preview;
            oldest = entry->previewAccessSerial;
            exact = entry;
        }
        if (entry->artifactWeight > 0 && entry->artifactAccessSerial < oldest) {
            kind = Kind::artifact;
            oldest = entry->artifactAccessSerial;
            exact = entry;
        }
    }
    auto raster = rasterCache_.end();
    for (auto entry = rasterCache_.begin(); entry != rasterCache_.end(); ++entry) {
        if (entry->accessSerial < oldest) {
            kind = Kind::raster;
            oldest = entry->accessSerial;
            raster = entry;
        }
    }
    if (kind == Kind::resident_preview) {
        cacheBytes_ -= std::min(cacheBytes_, exact->previewWeight);
        exact->previewWeight = 0;
        exact->previewAccessSerial = 0;
        exact->result.displayPreview.reset();
        exact->result.exactPreviewAvailable = false;
        return true;
    }
    if (kind == Kind::native_preview) {
        diskCacheBytes_ -= std::min(diskCacheBytes_, exact->previewDiskWeight);
        exact->previewDiskWeight = 0;
        exact->previewAccessSerial = 0;
        exact->result.displayPreview.reset();
        exact->result.exactPreviewAvailable = false;
        return true;
    }
    if (kind == Kind::artifact) {
        diskCacheBytes_ -= std::min(diskCacheBytes_, exact->artifactWeight);
        diskCacheBytes_ -= std::min(diskCacheBytes_, exact->previewDiskWeight);
        cacheBytes_ -= std::min(cacheBytes_, exact->previewWeight);
        cache_.erase(exact);
        return true;
    }
    if (kind == Kind::raster) {
        diskCacheBytes_ -= std::min(diskCacheBytes_, raster->weight);
        rasterCache_.erase(raster);
        return true;
    }
    return false;
}

void EditPipelineController::clearCache() {
    cache_.clear();
    rasterCache_.clear();
    cacheBytes_ = 0;
    diskCacheBytes_ = 0;
}

std::uint64_t EditPipelineController::mappedBytes() const {
    std::uint64_t bytes = 0;
    std::vector<const void*> counted;
    const auto add = [&](const std::shared_ptr<MappedRasterPackage>& package) {
        if (!package)
            return true;
        const void* identity = package->identity();
        if (std::find(counted.begin(), counted.end(), identity) != counted.end())
            return true;
        counted.push_back(identity);
        if (package->mappedBytes() > std::numeric_limits<std::uint64_t>::max() - bytes) {
            return false;
        }
        bytes += package->mappedBytes();
        return true;
    };
    if (!add(sourceRaster_))
        return std::numeric_limits<std::uint64_t>::max();
    for (const RasterCacheEntry& entry : rasterCache_) {
        if (!add(entry.package)) {
            return std::numeric_limits<std::uint64_t>::max();
        }
    }
    if (activeWorkerJob_ && activeWorkerJob_->baseAsset) {
        if (!add(activeWorkerJob_->baseAsset->packageShared()))
            return std::numeric_limits<std::uint64_t>::max();
    }
    return bytes;
}

std::uint64_t EditPipelineController::sharedRasterBytes() const {
    std::uint64_t bytes = 0;
    std::vector<const void*> counted;
    const auto add = [&](const std::shared_ptr<MappedRasterPackage>& package) {
        if (!package || !package->isSharedMemory())
            return true;
        const void* identity = package->identity();
        if (std::find(counted.begin(), counted.end(), identity) != counted.end())
            return true;
        counted.push_back(identity);
        if (package->mappedBytes() > std::numeric_limits<std::uint64_t>::max() - bytes)
            return false;
        bytes += package->mappedBytes();
        return true;
    };
    if (!add(sourceRaster_))
        return std::numeric_limits<std::uint64_t>::max();
    for (const RasterCacheEntry& entry : rasterCache_) {
        if (!add(entry.package))
            return std::numeric_limits<std::uint64_t>::max();
    }
    if (activeWorkerJob_ &&
        !add(activeWorkerJob_->baseAsset ? activeWorkerJob_->baseAsset->packageShared() : nullptr))
        return std::numeric_limits<std::uint64_t>::max();
    return bytes;
}

std::uint64_t EditPipelineController::encodedBytes() const {
    std::uint64_t bytes = 0;
    for (const CacheEntry& entry : cache_) {
        if (!entry.result.artifact)
            continue;
        const std::uint64_t size = entry.result.artifact->byteSize();
        if (size > std::numeric_limits<std::uint64_t>::max() - bytes)
            return std::numeric_limits<std::uint64_t>::max();
        bytes += size;
    }
    return bytes;
}

std::uint64_t EditPipelineController::previewBytes() const {
    std::uint64_t bytes = 0;
    for (const CacheEntry& entry : cache_) {
        if (!entry.result.displayPreview)
            continue;
        std::uint64_t size =
            static_cast<std::uint64_t>(entry.result.displayPreview->pixels.sizeInBytes());
        for (const DecodedAnimationFrame& frame : entry.result.displayPreview->animationFrames) {
            const std::uint64_t frameBytes = static_cast<std::uint64_t>(frame.pixels.sizeInBytes());
            if (frameBytes > std::numeric_limits<std::uint64_t>::max() - size)
                return std::numeric_limits<std::uint64_t>::max();
            size += frameBytes;
        }
        if (size > std::numeric_limits<std::uint64_t>::max() - bytes)
            return std::numeric_limits<std::uint64_t>::max();
        bytes += size;
    }
    return bytes;
}

std::uint64_t EditPipelineController::temporaryFileBytes() const {
    if (!artifactDirectory_ || !artifactDirectory_->isValid())
        return 0;
    std::uint64_t bytes = 0;
    const QFileInfoList files =
        QDir(artifactDirectory_->path()).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& file : files) {
        const std::uint64_t size = static_cast<std::uint64_t>(std::max<qint64>(0, file.size()));
        if (size > std::numeric_limits<std::uint64_t>::max() - bytes)
            return std::numeric_limits<std::uint64_t>::max();
        bytes += size;
    }
    return bytes;
}

std::uint64_t EditPipelineController::temporaryFileCount() const {
    if (!artifactDirectory_ || !artifactDirectory_->isValid())
        return 0;
    return static_cast<std::uint64_t>(
        QDir(artifactDirectory_->path()).entryInfoList(QDir::Files | QDir::NoDotAndDotDot).size());
}

qint64 EditPipelineController::workerProcessId() const {
    return worker_ ? worker_->processId() : 0;
}

} // namespace snow::image_viewer
