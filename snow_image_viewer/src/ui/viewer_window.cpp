#include "ui/viewer_window.h"

#include "core/image_raster_store.h"
#include "platform/windows_background.h"
#include "platform/windows_file_explorer.h"
#include "platform/windows_open_with.h"
#include "platform/windows_print.h"
#include "platform/windows_share.h"
#include "render/rhi_image_window.h"
#include "editing/edit_pipeline_controller.h"
#include "ui/app_icons.h"
#include "ui/edit_size_format_window.h"

#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/context_menu.h"
#include "widgets/modal.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorSpace>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMimeData>
#include <QPalette>
#include <QSettings>
#include <QSaveFile>
#include <QScreen>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace snow::image_viewer {
namespace {

using AdButton = adqt::widgets::AdButton;
using AdContextMenu = adqt::widgets::AdContextMenu;
using AdModal = adqt::widgets::AdModal;
namespace outlined = adqt::icons::antd::outlined;

constexpr int kToolbarHeight = 48;
constexpr int kToolbarButtonSize = 36;
constexpr int kToolbarIconSize = 22;
constexpr int kMinimumWindowWidth = 520;
constexpr int kMinimumWindowHeight = 360;
constexpr int kMinimumPreviewExtent = 256;

std::uint64_t processPeakRss(qint64 processId) {
    if (processId <= 0)
        return 0;
#if defined(Q_OS_WIN)
    const HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                                       static_cast<DWORD>(processId));
    if (!process)
        return 0;
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    const bool read = GetProcessMemoryInfo(process, &counters, sizeof(counters)) != FALSE;
    CloseHandle(process);
    return read ? static_cast<std::uint64_t>(counters.PeakWorkingSetSize) : 0;
#elif defined(Q_OS_LINUX)
    QFile status(QStringLiteral("/proc/%1/status").arg(processId));
    if (!status.open(QIODevice::ReadOnly))
        return 0;
    while (!status.atEnd()) {
        const QByteArray line = status.readLine();
        if (!line.startsWith("VmHWM:"))
            continue;
        const QList<QByteArray> parts = line.simplified().split(' ');
        bool okay = false;
        const qulonglong kibibytes = parts.size() >= 2 ? parts[1].toULongLong(&okay) : 0;
        return okay ? kibibytes * 1024ULL : 0;
    }
    return 0;
#else
    return 0;
#endif
}

std::uint64_t processCurrentRss(qint64 processId) {
    if (processId <= 0)
        return 0;
#if defined(Q_OS_WIN)
    const HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                                       static_cast<DWORD>(processId));
    if (!process)
        return 0;
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    const bool read = GetProcessMemoryInfo(process, &counters, sizeof(counters)) != FALSE;
    CloseHandle(process);
    return read ? static_cast<std::uint64_t>(counters.WorkingSetSize) : 0;
#elif defined(Q_OS_LINUX)
    QFile status(QStringLiteral("/proc/%1/status").arg(processId));
    if (!status.open(QIODevice::ReadOnly))
        return 0;
    while (!status.atEnd()) {
        const QByteArray line = status.readLine();
        if (!line.startsWith("VmRSS:"))
            continue;
        const QList<QByteArray> parts = line.simplified().split(' ');
        bool okay = false;
        const qulonglong kibibytes = parts.size() >= 2 ? parts[1].toULongLong(&okay) : 0;
        return okay ? kibibytes * 1024ULL : 0;
    }
    return 0;
#else
    return 0;
#endif
}

QString imageDialogFilter() {
    QStringList patterns;
    for (const QString& suffix : FolderSequence::supportedSuffixes()) {
        patterns.append(QStringLiteral("*.%1").arg(suffix));
    }
    return QStringLiteral("Images (%1);;All files (*)").arg(patterns.join(QLatin1Char(' ')));
}

} // namespace

ViewerWindow::ViewerWindow(QWidget* parent)
    : QMainWindow(parent), ownedLoader_(std::make_unique<ImageLoader>()),
      loader_(ownedLoader_.get()) {
    initializeWindow({});
}

ViewerWindow::ViewerWindow(ImageLoader& startupLoader, const QString& initialImagePath,
                           QWidget* parent)
    : QMainWindow(parent), loader_(&startupLoader) {
    initializeWindow(initialImagePath);
}

ViewerWindow::~ViewerWindow() = default;

void ViewerWindow::startEditModePerformanceTest(qint64 applicationStartupNanoseconds,
                                                EditPerformanceOptions options) {
    if (performanceTestActive_)
        return;
    performanceTestActive_ = true;
    performanceGpuPath_ = false;
    performanceCpuPixelsReleased_ = false;
    performanceRequestId_ = 0;
    performanceReusedGpuPixels_ = false;
    performancePreviewSource_ = ExactPreviewSource::base_raster;
    performanceAlphaContent_ = QStringLiteral("unknown");
    performanceVisualRequestNanoseconds_ = 0;
    performanceOptions_ = options;
    performanceIterationsTarget_ = std::clamp(options.iterations, 1, 1000);
    performanceWarmupTarget_ = std::clamp(options.warmupIterations, 1, 100);
    performanceIterationsCompleted_ = 0;
    performanceScenarioPhase_ = 0;
    performanceResourceCacheHits_ = 0;
    performanceResourceCacheMisses_ = 0;
    performanceWarmupTimings_.clear();
    performanceColdTimings_.clear();
    performanceTimings_.clear();
    performanceArtifactHitTimings_.clear();
    performancePreviewOnlyTimings_.clear();
    performanceRapidTimings_.clear();
    performanceSettings_.reset();
    performanceMilestones_.clear();
    performanceScenarioSnapshots_.clear();
    performanceTimer_.start();
    recordPerformanceMilestone(QStringLiteral("application.start_to_window_shown_elapsed"),
                               applicationStartupNanoseconds);
    QTimer::singleShot(900000, this, [this]() {
        if (performanceTestActive_) {
            finishEditModePerformanceTest(
                false, QStringLiteral("Timed out waiting for edit benchmark completion."));
        }
    });
}

void ViewerWindow::recordPerformanceTiming(const QString& name, qint64 nanoseconds) {
    if (!performanceTestActive_)
        return;
    if (performanceScenarioPhase_ == 0)
        performanceWarmupTimings_[name].push_back(nanoseconds);
    else if (performanceScenarioPhase_ == 1)
        performanceColdTimings_[name].push_back(nanoseconds);
    else if (performanceScenarioPhase_ == 2)
        performanceTimings_[name].push_back(nanoseconds);
    else if (performanceScenarioPhase_ == 3)
        performanceArtifactHitTimings_[name].push_back(nanoseconds);
    else if (performanceScenarioPhase_ == 4)
        performancePreviewOnlyTimings_[name].push_back(nanoseconds);
    else
        performanceRapidTimings_[name].push_back(nanoseconds);
}

void ViewerWindow::recordPerformanceMilestone(const QString& name, qint64 nanoseconds) {
    if (performanceTestActive_)
        performanceMilestones_[name] = nanoseconds;
}

void ViewerWindow::capturePerformanceScenarioSnapshot(int phase) {
    if (!performanceTestActive_ || !editSession_ || !rhiWindow_)
        return;
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("artifact_bytes"),
                    static_cast<double>(performanceEncodedBytes_));
    snapshot.insert(QStringLiteral("provenance"),
                    performanceProvenance_ == RasterProvenance::gpu_approximate
                        ? QStringLiteral("gpu_approximate")
                    : performanceProvenance_ == RasterProvenance::source_exact
                        ? QStringLiteral("source_exact")
                        : QStringLiteral("cpu_reference"));
    snapshot.insert(QStringLiteral("preview_source"),
                    performancePreviewSource_ == ExactPreviewSource::gpu_raster
                        ? QStringLiteral("gpu_raster")
                    : performancePreviewSource_ == ExactPreviewSource::base_raster
                        ? QStringLiteral("base_raster")
                    : performancePreviewSource_ == ExactPreviewSource::prepared_raster
                        ? QStringLiteral("prepared_raster")
                        : QStringLiteral("codec_artifact"));
    snapshot.insert(QStringLiteral("alpha_content"), performanceAlphaContent_);
    const auto samplingName = [](snow::image::ChromaSubsampling sampling) {
        switch (sampling) {
        case snow::image::ChromaSubsampling::none:
            return QStringLiteral("grayscale");
        case snow::image::ChromaSubsampling::yuv444:
            return QStringLiteral("4:4:4");
        case snow::image::ChromaSubsampling::yuv422:
            return QStringLiteral("4:2:2");
        case snow::image::ChromaSubsampling::yuv420:
            return QStringLiteral("4:2:0");
        default:
            return QStringLiteral("other");
        }
    };
    const auto artifact = editSession_->encodedArtifact();
    snapshot.insert(QStringLiteral("resolved_chroma_subsampling"),
                    artifact && artifact->receipt().jpeg_chroma_subsampling
                        ? samplingName(*artifact->receipt().jpeg_chroma_subsampling)
                        : QStringLiteral("not_applicable"));
    snapshot.insert(QStringLiteral("streamed_encoder_bytes"),
                    static_cast<double>(artifact ? artifact->byteSize() : 0U));
    snow::image::Service performanceService;
    const auto* encoder = performanceSettings_
                              ? performanceService.encoder_info(performanceSettings_->format)
                              : nullptr;
    snapshot.insert(QStringLiteral("worker_cancellation_mode"),
                    encoder && encoder->cancellation == snow::image::CodecCancellation::cooperative
                        ? QStringLiteral("cooperative")
                        : QStringLiteral("terminate_worker"));
    if (editPreviewImage_.rasterStore && editPreviewImage_.rasterStore->store()) {
        const auto& descriptor = editPreviewImage_.rasterStore->store()->descriptor();
        QString layout = QStringLiteral("unknown");
        if (!descriptor.frames.empty()) {
            const auto& rasterLayout = descriptor.frames.front().layout;
            layout =
                rasterLayout.color_model == snow::image::ColorModel::gray ? QStringLiteral("gray")
                : rasterLayout.color_model == snow::image::ColorModel::ycbcr
                    ? QStringLiteral("ycbcr_%1").arg(samplingName(rasterLayout.chroma_subsampling))
                    : QStringLiteral("packed");
        }
        snapshot.insert(QStringLiteral("preview_raster_layout"), layout);
        snapshot.insert(QStringLiteral("preview_backing_bytes"),
                        static_cast<double>(editPreviewImage_.rasterStore->store()->file_bytes()));
    } else {
        snapshot.insert(QStringLiteral("preview_raster_layout"), QStringLiteral("packed"));
        snapshot.insert(QStringLiteral("preview_backing_bytes"),
                        static_cast<double>(editPreviewImage_.pixels.sizeInBytes()));
    }
    snapshot.insert(QStringLiteral("preview_resident_staging_bytes"),
                    static_cast<double>(editPreviewImage_.pixels.sizeInBytes()));
    snapshot.insert(QStringLiteral("parent_current_rss_bytes"),
                    static_cast<double>(processCurrentRss(QCoreApplication::applicationPid())));
    snapshot.insert(QStringLiteral("worker_current_rss_bytes"),
                    static_cast<double>(processCurrentRss(editSession_->workerProcessId())));
    snapshot.insert(QStringLiteral("mapped_bytes"),
                    static_cast<double>(editSession_->mappedBytes()));
    snapshot.insert(QStringLiteral("shared_raster_bytes"),
                    static_cast<double>(editSession_->sharedRasterBytes()));
    snapshot.insert(QStringLiteral("cache_bytes"), static_cast<double>(editSession_->cacheBytes()));
    snapshot.insert(QStringLiteral("encoded_cache_bytes"),
                    static_cast<double>(editSession_->encodedBytes()));
    snapshot.insert(QStringLiteral("preview_cache_bytes"),
                    static_cast<double>(editSession_->previewBytes()));
    snapshot.insert(QStringLiteral("readback_bytes"),
                    static_cast<double>(editSession_->readbackBytes()));
    snapshot.insert(QStringLiteral("gpu_resize_cache_bytes"),
                    static_cast<double>(rhiWindow_->exactRasterCacheBytes()));
    snapshot.insert(QStringLiteral("source_texture_count"),
                    static_cast<double>(rhiWindow_->sourceTextureCount()));
    snapshot.insert(QStringLiteral("source_texture_bytes"),
                    static_cast<double>(rhiWindow_->sourceTextureBytes()));
    snapshot.insert(QStringLiteral("temporary_file_bytes"),
                    static_cast<double>(editSession_->temporaryFileBytes()));
    snapshot.insert(QStringLiteral("temporary_file_count"),
                    static_cast<double>(editSession_->temporaryFileCount()));
    performanceScenarioSnapshots_[phase] = std::move(snapshot);
}

void ViewerWindow::finishEditModePerformanceTest(bool succeeded, const QString& error) {
    if (!performanceTestActive_)
        return;
    recordPerformanceMilestone(QStringLiteral("test.total_elapsed"),
                               performanceTimer_.nsecsElapsed());
    performanceTestActive_ = false;

    const auto timingObject = [](const QMap<QString, QVector<qint64>>& source) {
        QJsonObject timings;
        for (auto timing = source.cbegin(); timing != source.cend(); ++timing) {
            QVector<qint64> sorted = timing.value();
            if (sorted.isEmpty())
                continue;
            std::sort(sorted.begin(), sorted.end());
            const auto percentile = [&sorted](double fraction) {
                const qsizetype index =
                    std::clamp<qsizetype>(static_cast<qsizetype>(std::ceil(
                                              fraction * static_cast<double>(sorted.size()))) -
                                              1,
                                          0, sorted.size() - 1);
                return sorted[index];
            };
            QJsonObject values;
            values.insert(QStringLiteral("count"), sorted.size());
            values.insert(QStringLiteral("min_nanoseconds"), static_cast<double>(sorted.front()));
            values.insert(QStringLiteral("max_nanoseconds"), static_cast<double>(sorted.back()));
            values.insert(QStringLiteral("median_nanoseconds"),
                          static_cast<double>(percentile(0.5)));
            values.insert(QStringLiteral("p95_nanoseconds"), static_cast<double>(percentile(0.95)));
            QJsonArray samples;
            for (const qint64 sample : timing.value())
                samples.append(static_cast<double>(sample));
            values.insert(QStringLiteral("samples_nanoseconds"), samples);
            timings.insert(timing.key(), values);
        }
        return timings;
    };
    const QJsonObject warmupTimings = timingObject(performanceWarmupTimings_);
    const QJsonObject coldTimings = timingObject(performanceColdTimings_);
    const QJsonObject timings = timingObject(performanceTimings_);
    const QJsonObject artifactHitTimings = timingObject(performanceArtifactHitTimings_);
    const QJsonObject previewOnlyTimings = timingObject(performancePreviewOnlyTimings_);
    const QJsonObject rapidTimings = timingObject(performanceRapidTimings_);
    QJsonObject milestones;
    for (auto milestone = performanceMilestones_.cbegin();
         milestone != performanceMilestones_.cend(); ++milestone) {
        QJsonObject values;
        values.insert(QStringLiteral("nanoseconds"), static_cast<double>(milestone.value()));
        values.insert(QStringLiteral("milliseconds"),
                      static_cast<double>(milestone.value()) / 1000000.0);
        milestones.insert(milestone.key(), values);
    }
    QJsonObject image;
    image.insert(QStringLiteral("path"), currentImage_.filePath);
    image.insert(QStringLiteral("width"), currentImage_.sourceSize.width());
    image.insert(QStringLiteral("height"), currentImage_.sourceSize.height());
    image.insert(QStringLiteral("encoded_bytes"), static_cast<double>(performanceEncodedBytes_));
    image.insert(QStringLiteral("pipeline"),
                 performanceGpuPath_ ? QStringLiteral("gpu") : QStringLiteral("cpu"));
    image.insert(QStringLiteral("source_cpu_bytes_after_upload"),
                 static_cast<double>(currentImage_.pixels.sizeInBytes()));

    QJsonObject request;
    request.insert(QStringLiteral("id"), static_cast<double>(performanceRequestId_));
    request.insert(QStringLiteral("cancellations"),
                   editSession_ ? editSession_->cancellationCount() : 0);
    request.insert(QStringLiteral("iterations_requested"), performanceIterationsTarget_);
    request.insert(QStringLiteral("iterations_completed"), performanceIterationsCompleted_);
    request.insert(QStringLiteral("gpu_resource_cache_hits"), performanceResourceCacheHits_);
    request.insert(QStringLiteral("gpu_resource_cache_misses"), performanceResourceCacheMisses_);
    request.insert(QStringLiteral("final_preview"),
                   performancePreviewSource_ == ExactPreviewSource::gpu_raster
                       ? QStringLiteral("gpu_raster")
                   : performancePreviewSource_ == ExactPreviewSource::base_raster
                       ? QStringLiteral("base_raster")
                   : performancePreviewSource_ == ExactPreviewSource::prepared_raster
                       ? QStringLiteral("prepared_raster")
                       : QStringLiteral("codec_artifact"));
    if (editWindow_) {
        const EditExportSettings settings = activeEditSettings();
        request.insert(QStringLiteral("width"), settings.width);
        request.insert(QStringLiteral("height"), settings.height);
        request.insert(QStringLiteral("format"),
                       QString::fromUtf8(snow::image::format_name(settings.format).data(),
                                         static_cast<qsizetype>(
                                             snow::image::format_name(settings.format).size())));
        request.insert(QStringLiteral("animation_policy"),
                       animationPolicyForFormat(settings.format) ==
                               snow::image::AnimationPolicy::preserve
                           ? QStringLiteral("preserve")
                           : QStringLiteral("first_frame"));
        request.insert(QStringLiteral("preserve_metadata"), settings.encode.preserve_metadata);
    }

    QJsonObject gpu;
    gpu.insert(QStringLiteral("backend"), rhiWindow_->backendName());
    gpu.insert(QStringLiteral("adapter"), rhiWindow_->adapterName());

    QJsonObject benchmark;
    benchmark.insert(QStringLiteral("iterations"), performanceIterationsTarget_);
    benchmark.insert(QStringLiteral("warmup_iterations"), performanceWarmupTarget_);
    benchmark.insert(QStringLiteral("measured_iterations"), performanceIterationsTarget_);
    benchmark.insert(QStringLiteral("cache_layer_cleared"), QStringLiteral("per_scenario"));
    benchmark.insert(QStringLiteral("report_version"), 1);
    benchmark.insert(QStringLiteral("worker_startup_nanoseconds"),
                     editSession_ ? static_cast<double>(editSession_->workerStartupNanoseconds())
                                  : 0.0);
    benchmark.insert(QStringLiteral("worker_restarts"),
                     editSession_ ? editSession_->workerRestartCount() : 0);
    benchmark.insert(QStringLiteral("scale"), performanceOptions_.scale);
    benchmark.insert(QStringLiteral("forced_cpu"), performanceOptions_.forceCpu);
    benchmark.insert(QStringLiteral("rapid_superseding"), performanceOptions_.rapidSuperseding);
    benchmark.insert(QStringLiteral("preserve_metadata"), performanceOptions_.preserveMetadata);
    benchmark.insert(QStringLiteral("warmup_timings"), warmupTimings);
    const std::string_view pngBackend =
        snow::image::compression_backend_version(snow::image::Format::png);
    benchmark.insert(
        QStringLiteral("png_compression_backend"),
        QString::fromUtf8(pngBackend.data(), static_cast<qsizetype>(pngBackend.size())));

    QJsonObject artifactCopy;
    if (editSession_) {
        const std::shared_ptr<const EncodedArtifact> artifact = editSession_->encodedArtifact();
        if (artifact) {
            QByteArray copied;
            QBuffer destination(&copied);
            destination.open(QIODevice::WriteOnly);
            QElapsedTimer copyTimer;
            copyTimer.start();
            QString copyError;
            const bool copiedOkay = artifact->copyTo(destination, &copyError);
            const qint64 elapsed = copyTimer.nsecsElapsed();
            artifactCopy.insert(QStringLiteral("succeeded"), copiedOkay);
            artifactCopy.insert(QStringLiteral("error"), copyError);
            artifactCopy.insert(QStringLiteral("bytes"), static_cast<double>(copied.size()));
            artifactCopy.insert(QStringLiteral("nanoseconds"), static_cast<double>(elapsed));
            artifactCopy.insert(QStringLiteral("bytes_per_second"),
                                elapsed > 0 ? static_cast<double>(copied.size()) * 1.0e9 /
                                                  static_cast<double>(elapsed)
                                            : 0.0);
        }
    }
    benchmark.insert(QStringLiteral("artifact_copy"), artifactCopy);

    const auto scenarioObject = [&](const QString& name, const QJsonObject& scenarioTimings,
                                    int phase) {
        QJsonObject scenario = performanceScenarioSnapshots_.value(phase);
        scenario.insert(QStringLiteral("name"), name);
        scenario.insert(QStringLiteral("timings"), scenarioTimings);
        const QString clearedLayer =
            name == QStringLiteral("cold-full-pipeline") || name == QStringLiteral("cpu-fallback")
                ? QStringLiteral("base_rasters,exact_artifacts,gpu_resize_resources")
            : name == QStringLiteral("base-raster-reuse") ? QStringLiteral("exact_artifacts")
            : name == QStringLiteral("preview-only-cache-recovery")
                ? QStringLiteral("exact_previews")
            : name == QStringLiteral("rapid-superseding-request")
                ? QStringLiteral("exact_artifacts,active_worker")
                : QStringLiteral("none");
        scenario.insert(QStringLiteral("cache_layer_cleared"), clearedLayer);
        scenario.insert(QStringLiteral("animation_policy"),
                        request.value(QStringLiteral("animation_policy")));
        return scenario;
    };
    QJsonObject scenarios;
    scenarios.insert(QStringLiteral("cold-full-pipeline"),
                     scenarioObject(QStringLiteral("cold-full-pipeline"), coldTimings, 1));
    scenarios.insert(QStringLiteral("base-raster-reuse"),
                     scenarioObject(QStringLiteral("base-raster-reuse"), timings, 2));
    scenarios.insert(QStringLiteral("exact-artifact-hit"),
                     scenarioObject(QStringLiteral("exact-artifact-hit"), artifactHitTimings, 3));
    scenarios.insert(
        QStringLiteral("preview-only-cache-recovery"),
        scenarioObject(QStringLiteral("preview-only-cache-recovery"), previewOnlyTimings, 4));
    if (performanceOptions_.forceCpu) {
        scenarios.insert(QStringLiteral("cpu-fallback"),
                         scenarioObject(QStringLiteral("cpu-fallback"), coldTimings, 1));
    }
    if (performanceOptions_.rapidSuperseding) {
        scenarios.insert(
            QStringLiteral("rapid-superseding-request"),
            scenarioObject(QStringLiteral("rapid-superseding-request"), rapidTimings, 5));
    }

    QJsonObject report;
    report.insert(QStringLiteral("schema"),
                  QStringLiteral("snow-image-viewer.edit-mode-performance.v1"));
    report.insert(QStringLiteral("succeeded"), succeeded);
    report.insert(QStringLiteral("error"), error);
    report.insert(QStringLiteral("image"), image);
    report.insert(QStringLiteral("request"), request);
    report.insert(QStringLiteral("gpu"), gpu);
    report.insert(QStringLiteral("benchmark"), benchmark);
    report.insert(QStringLiteral("scenarios"), scenarios);
    report.insert(QStringLiteral("timings"), timings);
    report.insert(QStringLiteral("milestones"), milestones);
    report.insert(
        QStringLiteral("lifetime_peak_rss"),
        QJsonObject{{QStringLiteral("parent_bytes"),
                     static_cast<double>(processPeakRss(QCoreApplication::applicationPid()))},
                    {QStringLiteral("worker_bytes"),
                     static_cast<double>(
                         processPeakRss(editSession_ ? editSession_->workerProcessId() : 0))}});
    emit editModePerformanceTestFinished(succeeded,
                                         QJsonDocument(report).toJson(QJsonDocument::Indented));
}

EditExportSettings ViewerWindow::activeEditSettings() const {
    if (performanceSettings_)
        return *performanceSettings_;
    return editWindow_ ? editWindow_->settings() : EditExportSettings{};
}

void ViewerWindow::requestRapidPerformanceIteration() {
    if (!performanceTestActive_ || !performanceOptions_.rapidSuperseding || !editSession_ ||
        !performanceSettings_)
        return;
    editSession_->clearExactArtifactCacheForBenchmark();
    EditExportSettings superseded = *performanceSettings_;
    switch (superseded.format) {
    case snow::image::Format::png:
        superseded.encode.interlaced = !superseded.encode.interlaced;
        break;
    case snow::image::Format::jpeg:
    case snow::image::Format::heif:
    case snow::image::Format::avif:
    case snow::image::Format::jxl:
        superseded.encode.quality = std::clamp(superseded.encode.quality + 1, 1, 100);
        if (superseded.encode.quality == performanceSettings_->encode.quality)
            superseded.encode.quality = std::max(1, superseded.encode.quality - 1);
        break;
    case snow::image::Format::webp:
        if (superseded.encode.lossless) {
            superseded.encode.lossless_effort =
                std::clamp(superseded.encode.lossless_effort + 1, 0, 9);
            if (superseded.encode.lossless_effort == performanceSettings_->encode.lossless_effort) {
                superseded.encode.lossless_effort =
                    std::max(0, superseded.encode.lossless_effort - 1);
            }
        } else {
            superseded.encode.quality = std::clamp(superseded.encode.quality + 1, 1, 100);
            if (superseded.encode.quality == performanceSettings_->encode.quality)
                superseded.encode.quality = std::max(1, superseded.encode.quality - 1);
        }
        break;
    default:
        superseded.encode.preserve_metadata = !superseded.encode.preserve_metadata;
        break;
    }
    editSession_->requestEdit(superseded);
    editSession_->flushPendingExact();
    QTimer::singleShot(10, this, [this]() {
        if (!performanceTestActive_ || !editSession_ || !performanceSettings_)
            return;
        editSession_->requestEdit(*performanceSettings_);
        editSession_->flushPendingExact();
    });
}

void ViewerWindow::initializeWindow(const QString& initialImagePath) {
    setObjectName(QStringLiteral("viewerWindow"));
    setWindowTitle(QStringLiteral("Snow Image Viewer"));
    setAcceptDrops(true);
    setMinimumSize(kMinimumWindowWidth, kMinimumWindowHeight);
    resize(1100, 720);
    folderSequencePool_.setMaxThreadCount(1);
    folderSequencePool_.setExpiryTimeout(-1);
    animationTimer_ = new QTimer(this);
    animationTimer_->setSingleShot(true);
    connect(animationTimer_, &QTimer::timeout, this, &ViewerWindow::advanceCurrentAnimation);

    buildInterface();
    connectSignals();
    restoreSettings();
    refreshTheme();
    if (!initialImagePath.isEmpty()) {
        attachInitialImageLoad(initialImagePath);
    }
    updateImageControls();
}

void ViewerWindow::attachInitialImageLoad(const QString& filePath) {
    const QFileInfo info(filePath);
    if (!info.isFile()) {
        showLoadError(filePath, QStringLiteral("The selected file does not exist."));
        return;
    }

    const QString absolutePath = info.absoluteFilePath();
    lastDirectory_ = info.absolutePath();
    loadingFilePath_ = absolutePath;
    updateWindowTitle();
    deferFolderSequenceLoad(absolutePath);
    prepareCanvasForLoading();
    rhiWindow_->prewarm();
    emptyOpenButton_->setBusy(loader_->isLoading());
}

void ViewerWindow::openImage(const QString& filePath) {
    if (editingActive_)
        closeSizeFormatEditor();
    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        showLoadError(filePath, QStringLiteral("The selected file does not exist."));
        return;
    }

    errorAlert_->hide();
    stopCurrentAnimation();
    const QString absolutePath = info.absoluteFilePath();
    lastDirectory_ = info.absolutePath();
    loadingFilePath_ = absolutePath;
    updateWindowTitle();
    const int canvasExtent = std::max(canvasContainer_->width(), canvasContainer_->height());
    const int thumbnailExtent =
        std::clamp(static_cast<int>(std::ceil(canvasExtent * rhiWindow_->devicePixelRatio())),
                   kMinimumPreviewExtent, kSystemThumbnailMaximumExtent);
    loader_->load(absolutePath, thumbnailExtent);
    deferFolderSequenceLoad(absolutePath);
    prepareCanvasForLoading();
    rhiWindow_->prewarm();
    emptyOpenButton_->setBusy(true);
    updateImageControls();
}

void ViewerWindow::buildInterface() {
    root_ = new QWidget(this);
    root_->setObjectName(QStringLiteral("viewerRoot"));
    auto* rootLayout = new QVBoxLayout(root_);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    setCentralWidget(root_);

    errorAlert_ = new adqt::widgets::AdAlert(root_);
    errorAlert_->setObjectName(QStringLiteral("errorAlert"));
    errorAlert_->setSeverity(adqt::widgets::AdAlert::Severity::Error);
    errorAlert_->setDisplayMode(adqt::widgets::AdAlert::DisplayMode::Banner);
    errorAlert_->setIconMode(adqt::widgets::AdAlert::IconMode::Visible);
    errorAlert_->setClosable(true);
    errorAlert_->setAnimated(false);
    errorAlert_->hide();
    rootLayout->addWidget(errorAlert_);

    contentStack_ = new QStackedWidget(root_);
    contentStack_->setObjectName(QStringLiteral("contentStack"));

    emptyState_ = new QWidget(contentStack_);
    emptyState_->setObjectName(QStringLiteral("emptyState"));
    auto* emptyLayout = new QVBoxLayout(emptyState_);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->setSpacing(9);
    emptyLayout->addStretch(1);
    emptyIcon_ = new QLabel(emptyState_);
    emptyIcon_->setObjectName(QStringLiteral("emptyIcon"));
    emptyIcon_->setAlignment(Qt::AlignCenter);
    emptyIcon_->setFixedHeight(58);
    auto* emptyTitle = new QLabel(QStringLiteral("Open an image"), emptyState_);
    emptyTitle->setObjectName(QStringLiteral("emptyTitle"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    auto* emptyHint =
        new QLabel(QStringLiteral("Drop a file here or choose one from disk"), emptyState_);
    emptyHint->setObjectName(QStringLiteral("emptyHint"));
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyOpenButton_ = new AdButton(QStringLiteral("Choose image"), emptyState_);
    emptyOpenButton_->setObjectName(QStringLiteral("emptyOpenButton"));
    emptyOpenButton_->setIconRef(outlined::FolderOpen());
    emptyOpenButton_->setButtonStyle(AdButton::ButtonStyle::Solid);
    emptyOpenButton_->setAccentRole(AdButton::AccentRole::Primary);
    emptyOpenButton_->setShape(AdButton::Shape::Rounded);
    emptyOpenButton_->setSizeClass(AdButton::SizeClass::Medium);
    emptyOpenButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    emptyLayout->addWidget(emptyIcon_, 0, Qt::AlignHCenter);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyHint);
    emptyLayout->addSpacing(5);
    emptyLayout->addWidget(emptyOpenButton_, 0, Qt::AlignHCenter);
    emptyLayout->addStretch(1);
    contentStack_->addWidget(emptyState_);

    canvasPage_ = new QWidget(contentStack_);
    canvasPage_->setObjectName(QStringLiteral("canvasPage"));
    auto* canvasLayout = new QVBoxLayout(canvasPage_);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    canvasLayout->setSpacing(0);

    rhiWindow_ = new RhiImageWindow();
    canvasContainer_ = QWidget::createWindowContainer(rhiWindow_, canvasPage_);
    canvasContainer_->setObjectName(QStringLiteral("canvasContainer"));
    canvasContainer_->setFocusPolicy(Qt::StrongFocus);
    canvasLayout->addWidget(canvasContainer_, 1);
    contentStack_->addWidget(canvasPage_);
    contentStack_->setCurrentWidget(emptyState_);
    rootLayout->addWidget(contentStack_, 1);

    bottomToolbar_ = new QWidget(root_);
    bottomToolbar_->setObjectName(QStringLiteral("viewerBottomToolbar"));
    bottomToolbar_->setFixedHeight(kToolbarHeight);
    auto* toolbarLayout = new QHBoxLayout(bottomToolbar_);
    toolbarLayout->setContentsMargins(8, 0, 8, 0);
    toolbarLayout->setSpacing(6);

    actualSizeButton_ = makeIconButton(QStringLiteral("Show actual size"),
                                       QStringLiteral("Actual size"), bottomToolbar_);
    updateActualSizeButtonAction();
    zoomInButton_ =
        makeIconButton(QStringLiteral("Zoom in"), QStringLiteral("Zoom in"), bottomToolbar_);
    zoomInButton_->setIconRef(outlined::ZoomIn());
    zoomOutButton_ =
        makeIconButton(QStringLiteral("Zoom out"), QStringLiteral("Zoom out"), bottomToolbar_);
    zoomOutButton_->setIconRef(outlined::ZoomOut());
    previousButton_ = makeIconButton(QStringLiteral("Previous image"),
                                     QStringLiteral("Previous image"), bottomToolbar_);
    previousButton_->setIconRef(outlined::ArrowLeft());
    nextButton_ =
        makeIconButton(QStringLiteral("Next image"), QStringLiteral("Next image"), bottomToolbar_);
    nextButton_->setIconRef(outlined::ArrowRight());
    rotateLeftButton_ = makeIconButton(QStringLiteral("Rotate left"), QStringLiteral("Rotate left"),
                                       bottomToolbar_);
    rotateLeftButton_->setIconRef(outlined::RotateLeft());
    rotateRightButton_ = makeIconButton(QStringLiteral("Rotate right"),
                                        QStringLiteral("Rotate right"), bottomToolbar_);
    rotateRightButton_->setIconRef(outlined::RotateRight());
    deleteButton_ = makeIconButton(QStringLiteral("Delete image"), QStringLiteral("Delete image"),
                                   bottomToolbar_);
    deleteButton_->setObjectName(QStringLiteral("deleteButton"));
    deleteButton_->setIconRef(outlined::IconDelete());
    deleteButton_->setAccentRole(AdButton::AccentRole::Danger);
    editButton_ = makeIconButton(QStringLiteral("Edit size and format"),
                                 QStringLiteral("Edit size and format"), bottomToolbar_);
    editButton_->setObjectName(QStringLiteral("editSizeFormatButton"));
    editButton_->setIconRef(icons::outlined::ResizeImage());

    deleteConfirm_ = new AdModal(this);
    deleteConfirm_->setObjectName(QStringLiteral("deleteConfirmModal"));
    deleteConfirm_->setOwnerWindow(this);
    deleteConfirm_->setMode(AdModal::Mode::Window);
    deleteConfirm_->setWindowModeDetached(true);
    // Retain application modality without creating a native parent or transient window.
    deleteConfirm_->setWindowModality(Qt::ApplicationModal);
    deleteConfirm_->setPreset(AdModal::Preset::Confirm);
    deleteConfirm_->setWindowTitle(QStringLiteral("Move this image to the Recycle Bin?"));
    deleteConfirm_->setText(QStringLiteral("You can restore it from the Recycle Bin."));
    deleteConfirm_->setAcceptText(QStringLiteral("Delete"));
    deleteConfirm_->setAcceptAccentRole(AdButton::AccentRole::Danger);
    deleteConfirm_->setCentered(true);
    deleteConfirm_->setPreferredWidth(416);

    imageContextMenu_ = new AdContextMenu(this);
    imageContextMenu_->setObjectName(QStringLiteral("imageContextMenu"));
    copyImageAction_ =
        imageContextMenu_->addItem(QStringLiteral("Copy"), outlined::Copy(), QKeySequence::Copy);
    copyImageAction_->setObjectName(QStringLiteral("copyImageAction"));
    copyImagePathAction_ =
        imageContextMenu_->addItem(QStringLiteral("Copy as path"), outlined::FileText());
    copyImagePathAction_->setObjectName(QStringLiteral("copyImagePathAction"));
#if defined(Q_OS_WIN)
    printImageAction_ = imageContextMenu_->addItem(QStringLiteral("Print"), outlined::Printer());
    printImageAction_->setObjectName(QStringLiteral("printImageAction"));
    shareImageAction_ = imageContextMenu_->addItem(QStringLiteral("Share"), outlined::ShareAlt());
    shareImageAction_->setObjectName(QStringLiteral("shareImageAction"));
    setAsMenu_ = imageContextMenu_->addSubMenu(QStringLiteral("Set as"), outlined::Picture());
    setAsMenu_->setObjectName(QStringLiteral("setAsContextMenu"));
    setAsMenu_->menuAction()->setObjectName(QStringLiteral("setAsImageAction"));
    setAsLockScreenAction_ =
        setAsMenu_->addItem(QStringLiteral("Lock screen background"), outlined::Lock());
    setAsLockScreenAction_->setObjectName(QStringLiteral("setAsLockScreenAction"));
    setAsBackgroundAction_ = setAsMenu_->addItem(QStringLiteral("Background"), outlined::Desktop());
    setAsBackgroundAction_->setObjectName(QStringLiteral("setAsBackgroundAction"));
#endif
    imageContextMenu_->addSeparator();
    openWithAction_ = imageContextMenu_->addItem(QStringLiteral("Open with"), outlined::Appstore());
    openWithAction_->setObjectName(QStringLiteral("openWithAction"));
    revealInExplorerAction_ =
        imageContextMenu_->addItem(QStringLiteral("Open in File Explorer"), outlined::FolderOpen());
    revealInExplorerAction_->setObjectName(QStringLiteral("revealInExplorerAction"));
    imageContextMenu_->addSeparator();
    deleteImageAction_ =
        imageContextMenu_->addItem(QStringLiteral("Delete"), outlined::IconDelete());
    deleteImageAction_->setObjectName(QStringLiteral("deleteImageAction"));
    imageContextMenu_->setActionDanger(deleteImageAction_);

    toolbarLayout->addStretch(1);
    toolbarLayout->addWidget(actualSizeButton_);
    toolbarLayout->addWidget(zoomInButton_);
    toolbarLayout->addWidget(zoomOutButton_);
    toolbarLayout->addWidget(previousButton_);
    toolbarLayout->addWidget(nextButton_);
    toolbarLayout->addWidget(rotateLeftButton_);
    toolbarLayout->addWidget(rotateRightButton_);
    toolbarLayout->addWidget(editButton_);
    toolbarLayout->addWidget(deleteButton_);
    toolbarLayout->addStretch(1);
    rootLayout->addWidget(bottomToolbar_);
}

void ViewerWindow::prepareCanvasForLoading() {
    contentStack_->setCurrentWidget(canvasPage_);
    emptyState_->hide();
    canvasContainer_->show();
    canvasContainer_->raise();
    rhiWindow_->show();
}

void ViewerWindow::deferFolderSequenceLoad(const QString& filePath) {
    desiredSequenceFilePath_ = QFileInfo(filePath).absoluteFilePath();
    folderSequenceLoadAllowed_ = false;
}

void ViewerWindow::startPendingFolderSequenceLoad() {
    if (!folderSequenceLoadAllowed_ || folderSequenceLoadInFlight_ ||
        desiredSequenceFilePath_.isEmpty()) {
        return;
    }

    const QString filePath = desiredSequenceFilePath_;
    FolderSequence baseSequence = sequence_;
    folderSequenceLoadInFlight_ = true;
    auto* watcher = new QFutureWatcher<FolderSequence>(this);
    connect(watcher, &QFutureWatcher<FolderSequence>::finished, this, [this, watcher, filePath]() {
        FolderSequence loadedSequence = watcher->result();
        watcher->deleteLater();
        folderSequenceLoadInFlight_ = false;
        if (filePath == desiredSequenceFilePath_ &&
            (filePath == loadingFilePath_ || filePath == currentImage_.filePath)) {
            sequence_ = std::move(loadedSequence);
            desiredSequenceFilePath_.clear();
            folderSequenceLoadAllowed_ = false;
            updateWindowTitle();
            updateImageControls();
            prefetchCurrentNeighbors();
        }
        startPendingFolderSequenceLoad();
    });
    watcher->setFuture(QtConcurrent::run(
        &folderSequencePool_, [baseSequence = std::move(baseSequence), filePath]() mutable {
            baseSequence.setCurrentFile(filePath);
            return baseSequence;
        }));
}

void ViewerWindow::prefetchCurrentNeighbors() {
    if (!currentImage_.isValid() || sequence_.currentFile() != currentImage_.filePath ||
        loader_->isLoading()) {
        return;
    }
    loader_->prefetchThumbnails({sequence_.previousFile(), sequence_.nextFile()});
}

void ViewerWindow::connectSignals() {
    connect(emptyOpenButton_, &AdButton::clicked, this, &ViewerWindow::chooseImage);
    connect(actualSizeButton_, &AdButton::clicked, this, [this]() {
        if (loader_->isLoading()) {
            return;
        }

        if (fitToWindowAction_) {
            rhiWindow_->fitToWindow();
        } else {
            rhiWindow_->setActualSize();
        }
        fitToWindowAction_ = !fitToWindowAction_;
        updateActualSizeButtonAction();
    });
    connect(zoomInButton_, &AdButton::clicked, this, [this]() {
        if (!loader_->isLoading()) {
            rhiWindow_->zoomBy(1.2);
        }
    });
    connect(zoomOutButton_, &AdButton::clicked, this, [this]() {
        if (!loader_->isLoading()) {
            rhiWindow_->zoomBy(1.0 / 1.2);
        }
    });
    connect(previousButton_, &AdButton::clicked, this, &ViewerWindow::openPrevious);
    connect(nextButton_, &AdButton::clicked, this, &ViewerWindow::openNext);
    connect(rhiWindow_, &RhiImageWindow::imageDropped, this, [this](const QString& filePath) {
        if (!loader_->isLoading()) {
            openImage(filePath);
        }
    });
    connect(rhiWindow_, &RhiImageWindow::previousRequested, this, &ViewerWindow::openPrevious);
    connect(rhiWindow_, &RhiImageWindow::nextRequested, this, &ViewerWindow::openNext);
    connect(rhiWindow_, &RhiImageWindow::contextMenuRequested, this,
            &ViewerWindow::showImageContextMenu);
    connect(rotateLeftButton_, &AdButton::clicked, this, [this]() {
        if (!loader_->isLoading()) {
            rhiWindow_->rotateLeft();
        }
    });
    connect(rotateRightButton_, &AdButton::clicked, this, [this]() {
        if (!loader_->isLoading()) {
            rhiWindow_->rotateRight();
        }
    });
    connect(deleteButton_, &AdButton::clicked, deleteConfirm_, &AdModal::open);
    connect(editButton_, &AdButton::clicked, this, &ViewerWindow::openSizeFormatEditor);
    connect(copyImageAction_, &QAction::triggered, this, &ViewerWindow::copyCurrentImage);
    connect(copyImagePathAction_, &QAction::triggered, this, &ViewerWindow::copyCurrentImagePath);
#if defined(Q_OS_WIN)
    connect(printImageAction_, &QAction::triggered, this, &ViewerWindow::printCurrentImage);
    connect(shareImageAction_, &QAction::triggered, this, &ViewerWindow::shareCurrentImage);
    connect(setAsLockScreenAction_, &QAction::triggered, this,
            &ViewerWindow::setCurrentImageAsLockScreenBackground);
    connect(setAsBackgroundAction_, &QAction::triggered, this,
            &ViewerWindow::setCurrentImageAsBackground);
#endif
    connect(openWithAction_, &QAction::triggered, this, &ViewerWindow::openCurrentImageWith);
    connect(revealInExplorerAction_, &QAction::triggered, this, &ViewerWindow::revealCurrentImage);
    connect(deleteImageAction_, &QAction::triggered, deleteConfirm_, &AdModal::open);
    connect(deleteConfirm_, &AdModal::accepted, this, &ViewerWindow::deleteCurrentImage);

    connect(loader_, &ImageLoader::loadingChanged, this, [this](bool loading) {
        emptyOpenButton_->setBusy(loading);
        updateImageControls();
    });
    connect(loader_, &ImageLoader::thumbnailLoaded, this, [this](const ImageThumbnail& thumbnail) {
        if (thumbnail.filePath != loadingFilePath_) {
            return;
        }
        prepareCanvasForLoading();
        rhiWindow_->setThumbnail(thumbnail);
        canvasContainer_->setFocus();
    });
    connect(loader_, &ImageLoader::imageLoaded, this, [this](const DecodedImage& image) {
        loadingFilePath_.clear();
        currentImage_ = image;
        fitToWindowAction_ = false;
        updateActualSizeButtonAction();
        deleteConfirm_->setText(QStringLiteral("%1 can be restored from the Recycle Bin.")
                                    .arg(QFileInfo(image.filePath).fileName()));
        prepareCanvasForLoading();
        rhiWindow_->setImage(image);
        startCurrentAnimation();
        errorAlert_->hide();
        updateWindowTitle();
        updateImageControls();
        canvasContainer_->setFocus();
        prefetchCurrentNeighbors();
    });
    connect(loader_, &ImageLoader::loadFailed, this,
            [this](const QString& path, const QString& error) {
                if (QFileInfo(path).absoluteFilePath() == loadingFilePath_) {
                    loadingFilePath_.clear();
                    if (currentImage_.isValid()) {
                        if (currentImage_.hasCpuPixels()) {
                            rhiWindow_->setImage(currentImage_);
                            startCurrentAnimation();
                            deferFolderSequenceLoad(currentImage_.filePath);
                            folderSequenceLoadAllowed_ = true;
                            startPendingFolderSequenceLoad();
                        } else if (!rhiWindow_->hasGpuOnlyStaticImage() &&
                                   QFileInfo(path).absoluteFilePath() != currentImage_.filePath &&
                                   QFileInfo::exists(currentImage_.filePath)) {
                            const QString restorePath = currentImage_.filePath;
                            QTimer::singleShot(0, this,
                                               [this, restorePath]() { openImage(restorePath); });
                        }
                    } else {
                        sequence_ = {};
                        desiredSequenceFilePath_.clear();
                        folderSequenceLoadAllowed_ = false;
                        rhiWindow_->clearImage();
                    }
                }
                showLoadError(path, error);
                updateWindowTitle();
            });

    connect(rhiWindow_, &RhiImageWindow::zoomChanged, this, [this](qreal) {
        updateWindowTitle();
        updateImageControls();
    });
    connect(rhiWindow_, &RhiImageWindow::renderError, this,
            [this](const QString& message) { showLoadError(currentImage_.filePath, message); });
    connect(rhiWindow_, &RhiImageWindow::staticTextureResident, this,
            [this](const QString& filePath) {
                if (currentImage_.filePath != filePath || currentImage_.isAnimated() ||
                    currentImage_.usesStoredTiles()) {
                    return;
                }
                currentImage_.pixels = {};
                performanceCpuPixelsReleased_ = true;
                recordPerformanceMilestone(
                    QStringLiteral("image.cpu_pixels_released_elapsed"),
                    performanceTimer_.isValid() ? performanceTimer_.nsecsElapsed() : 0);
            });
    connect(rhiWindow_, &RhiImageWindow::gpuBackingLost, this, [this](const QString& filePath) {
        if (filePath == currentImage_.filePath && QFileInfo::exists(filePath)) {
            openImage(filePath);
        } else if (!filePath.isEmpty()) {
            showLoadError(
                filePath,
                QStringLiteral(
                    "The GPU image was lost and the source file is no longer available."));
        }
    });
    connect(rhiWindow_, &RhiImageWindow::mainImageReadbackReady, this,
            [this](quint64 requestId, const QImage& pixels) {
                if (!pixelReadbackInFlight_ || requestId != pixelReadbackRequest_)
                    return;
                const PixelReadbackPurpose purpose = pixelReadbackPurpose_;
                pixelReadbackInFlight_ = false;
                pixelReadbackPurpose_ = PixelReadbackPurpose::None;
                updateImageControls();
                consumeCurrentPixels(purpose, pixels);
            });
    connect(rhiWindow_, &RhiImageWindow::gpuOperationFailed, this,
            [this](quint64 requestId, const QString& message) {
                if (!pixelReadbackInFlight_ || requestId != pixelReadbackRequest_)
                    return;
                pixelReadbackInFlight_ = false;
                pixelReadbackPurpose_ = PixelReadbackPurpose::None;
                updateImageControls();
                showLoadError(currentImage_.filePath, message);
            });
    connect(rhiWindow_, &RhiImageWindow::comparisonCloseRequested, this,
            &ViewerWindow::closeSizeFormatEditor);
    connect(rhiWindow_, &RhiImageWindow::imageFrameSubmitted, this,
            [this](const QString& filePath, bool thumbnail) {
                if (filePath == desiredSequenceFilePath_) {
                    folderSequenceLoadAllowed_ = true;
                    startPendingFolderSequenceLoad();
                }
                if (performanceTestActive_ && !thumbnail && !editingActive_ &&
                    filePath == currentImage_.filePath) {
                    recordPerformanceMilestone(QStringLiteral("image.first_render_elapsed"),
                                               performanceTimer_.nsecsElapsed());
                    performanceEditStartNanoseconds_ = performanceTimer_.nsecsElapsed();
                    QTimer::singleShot(0, this, [this]() {
                        if (performanceTestActive_)
                            openSizeFormatEditor();
                    });
                }
            });
    connect(rhiWindow_, &RhiImageWindow::comparisonFrameSubmitted, this,
            [this](const QString& filePath) {
                if (!editPreviewImage_.isAnimated() && editPreviewImage_.filePath == filePath) {
                    editPreviewImage_.pixels = {};
                }
                Q_UNUSED(filePath)
            });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            &ViewerWindow::refreshTheme);
}

void ViewerWindow::chooseImage() {
    if (loader_->isLoading()) {
        return;
    }

    const QString startDirectory = lastDirectory_.isEmpty() ? QDir::homePath() : lastDirectory_;
    const QString filePath = QFileDialog::getOpenFileName(this, QStringLiteral("Open image"),
                                                          startDirectory, imageDialogFilter());
    if (!filePath.isEmpty()) {
        openImage(filePath);
    }
}

void ViewerWindow::openPrevious() {
    if (sequence_.hasPrevious()) {
        openImage(sequence_.previousFile());
    }
}

void ViewerWindow::openNext() {
    if (sequence_.hasNext()) {
        openImage(sequence_.nextFile());
    }
}

void ViewerWindow::copyCurrentImage() {
    if (!currentImage_.isValid() || loader_->isLoading()) {
        return;
    }
    requestCurrentPixels(PixelReadbackPurpose::Clipboard);
}

void ViewerWindow::requestCurrentPixels(PixelReadbackPurpose purpose) {
    if (!currentImage_.isValid() || loader_->isLoading() || pixelReadbackInFlight_)
        return;
    if (currentImage_.hasCpuPixels()) {
        consumeCurrentPixels(purpose, currentImage_.pixels);
        return;
    }
    if (!rhiWindow_->hasGpuOnlyStaticImage()) {
        showLoadError(currentImage_.filePath,
                      QStringLiteral("The image pixels are not available."));
        return;
    }
    pixelReadbackInFlight_ = true;
    pixelReadbackPurpose_ = purpose;
    pixelReadbackRequest_ = (quint64{1} << 63U) | (++pixelReadbackRequest_ & ~(quint64{1} << 63U));
    updateImageControls();
    rhiWindow_->requestMainImageReadback(pixelReadbackRequest_);
}

void ViewerWindow::consumeCurrentPixels(PixelReadbackPurpose purpose, const QImage& pixels) {
    if (pixels.isNull())
        return;
    if (purpose == PixelReadbackPurpose::Clipboard) {
        QImage clipboardImage = pixels.convertedToColorSpace(
            QColorSpace(QColorSpace::SRgb), QImage::Format_RGBA8888_Premultiplied, Qt::AutoColor);
        if (clipboardImage.isNull()) {
            clipboardImage = pixels.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        }
        QApplication::clipboard()->setImage(clipboardImage);
    } else if (purpose == PixelReadbackPurpose::LockScreen) {
        setWindowsBackgroundFromPixels(true, pixels);
    } else if (purpose == PixelReadbackPurpose::Wallpaper) {
        setWindowsBackgroundFromPixels(false, pixels);
    } else if (purpose == PixelReadbackPurpose::Print) {
        printPixels(pixels);
    }
}

void ViewerWindow::copyCurrentImagePath() {
    if (!currentImage_.isValid() || loader_->isLoading()) {
        return;
    }
    QApplication::clipboard()->setText(QDir::toNativeSeparators(currentImage_.filePath));
}

void ViewerWindow::setCurrentImageAsLockScreenBackground() {
    setCurrentImageAsWindowsBackground(true);
}

void ViewerWindow::setCurrentImageAsBackground() {
    setCurrentImageAsWindowsBackground(false);
}

void ViewerWindow::setCurrentImageAsWindowsBackground(bool lockScreen) {
#if defined(Q_OS_WIN)
    if (!currentImage_.isValid() || loader_->isLoading() || windowsBackgroundChangeInFlight_) {
        return;
    }
    requestCurrentPixels(lockScreen ? PixelReadbackPurpose::LockScreen
                                    : PixelReadbackPurpose::Wallpaper);
#else
    Q_UNUSED(lockScreen)
#endif
}

void ViewerWindow::setWindowsBackgroundFromPixels(bool lockScreen, const QImage& pixels) {
#if defined(Q_OS_WIN)
    if (pixels.isNull() || windowsBackgroundChangeInFlight_)
        return;

    if (!windowsBackgroundController_) {
        windowsBackgroundController_ = std::make_unique<WindowsBackgroundController>();
    }
    const auto target = lockScreen ? WindowsBackgroundController::Target::LockScreen
                                   : WindowsBackgroundController::Target::Wallpaper;
    windowsBackgroundChangeInFlight_ = true;
    updateImageControls();
    auto* watcher = new QFutureWatcher<WindowsBackgroundController::Result>(this);
    connect(watcher, &QFutureWatcher<WindowsBackgroundController::Result>::finished, this,
            [this, watcher, lockScreen]() {
                const WindowsBackgroundController::Result result = watcher->result();
                watcher->deleteLater();
                windowsBackgroundChangeInFlight_ = false;
                updateImageControls();
                if (result.changed) {
                    return;
                }

                errorAlert_->setText(
                    lockScreen ? QStringLiteral("Could not set the lock screen background")
                               : QStringLiteral("Could not set the desktop background"));
                errorAlert_->setInformativeText(result.errorMessage);
                errorAlert_->show();
            });
    watcher->setFuture(windowsBackgroundController_->setImage(target, pixels));
#else
    Q_UNUSED(lockScreen)
    Q_UNUSED(pixels)
#endif
}

void ViewerWindow::printCurrentImage() {
#if defined(Q_OS_WIN)
    if (!currentImage_.isValid() || loader_->isLoading() || windowsPrintInFlight_) {
        return;
    }

    requestCurrentPixels(PixelReadbackPurpose::Print);
#endif
}

void ViewerWindow::printPixels(const QImage& pixels) {
#if defined(Q_OS_WIN)
    if (pixels.isNull() || windowsPrintInFlight_)
        return;
    if (!windowsPrintController_) {
        windowsPrintController_ = std::make_unique<WindowsPrintController>();
    }
    windowsPrintInFlight_ = true;
    updateImageControls();
    auto* watcher = new QFutureWatcher<WindowsPrintController::Result>(this);
    connect(watcher, &QFutureWatcher<WindowsPrintController::Result>::finished, this,
            [this, watcher]() {
                const WindowsPrintController::Result result = watcher->result();
                watcher->deleteLater();
                windowsPrintInFlight_ = false;
                updateImageControls();
                if (result.completion != WindowsPrintController::Result::Completion::Failed) {
                    return;
                }

                errorAlert_->setText(QStringLiteral("Could not print image"));
                errorAlert_->setInformativeText(result.errorMessage);
                errorAlert_->show();
            });
    watcher->setFuture(windowsPrintController_->showPrintUI(
        static_cast<quintptr>(winId()), QFileInfo(currentImage_.filePath).fileName(), pixels));
#else
    Q_UNUSED(pixels)
#endif
}

void ViewerWindow::shareCurrentImage() {
#if defined(Q_OS_WIN)
    if (!currentImage_.isValid() || loader_->isLoading()) {
        return;
    }

    if (!windowsShareController_) {
        windowsShareController_ = std::make_unique<WindowsShareController>();
    }
    QString errorMessage;
    if (!windowsShareController_->showShareUI(static_cast<quintptr>(winId()),
                                              currentImage_.filePath, &errorMessage)) {
        errorAlert_->setText(QStringLiteral("Could not open Windows sharing"));
        errorAlert_->setInformativeText(errorMessage);
        errorAlert_->show();
    }
#endif
}

void ViewerWindow::openCurrentImageWith() {
    if (!currentImage_.isValid() || loader_->isLoading()) {
        return;
    }

    QString errorMessage;
#if defined(Q_OS_WIN)
    const bool opened = showWindowsOpenWithDialog(static_cast<quintptr>(winId()),
                                                  currentImage_.filePath, &errorMessage);
#else
    const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(currentImage_.filePath));
    if (!opened) {
        errorMessage = QFileInfo(currentImage_.filePath).fileName();
    }
#endif
    if (!opened) {
        errorAlert_->setText(QStringLiteral("Could not open the app chooser"));
        errorAlert_->setInformativeText(errorMessage);
        errorAlert_->show();
    }
}

void ViewerWindow::revealCurrentImage() {
    if (!currentImage_.isValid() || loader_->isLoading() || revealInExplorerInFlight_) {
        return;
    }

#if defined(Q_OS_WIN)
    revealInExplorerInFlight_ = true;
    updateImageControls();

    auto* watcher = new QFutureWatcher<WindowsFileExplorerResult>(this);
    connect(watcher, &QFutureWatcher<WindowsFileExplorerResult>::finished, this, [this, watcher]() {
        const WindowsFileExplorerResult result = watcher->result();
        watcher->deleteLater();
        revealInExplorerInFlight_ = false;
        updateImageControls();
        if (result.opened) {
            return;
        }

        errorAlert_->setText(QStringLiteral("Could not open File Explorer"));
        errorAlert_->setInformativeText(result.errorMessage);
        errorAlert_->show();
    });
    watcher->setFuture(openInWindowsFileExplorer(currentImage_.filePath));
#else
    if (!QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(currentImage_.filePath).absolutePath()))) {
        errorAlert_->setText(QStringLiteral("Could not open the file manager"));
        errorAlert_->setInformativeText(QFileInfo(currentImage_.filePath).absolutePath());
        errorAlert_->show();
    }
#endif
}

void ViewerWindow::showImageContextMenu(const QPoint& globalPosition) {
    if (!currentImage_.isValid() || loader_->isLoading() || editingActive_) {
        return;
    }
    imageContextMenu_->popupAt(globalPosition);
}

void ViewerWindow::deleteCurrentImage() {
    if (!currentImage_.isValid() || loader_->isLoading()) {
        return;
    }

    const QString currentPath = currentImage_.filePath;
    const QString replacementPath =
        sequence_.hasNext() ? sequence_.nextFile() : sequence_.previousFile();
    QFile file(currentPath);
    if (!file.moveToTrash()) {
        errorAlert_->setText(QStringLiteral("Could not move %1 to the Recycle Bin")
                                 .arg(QFileInfo(currentPath).fileName()));
        errorAlert_->setInformativeText(file.errorString());
        errorAlert_->show();
        return;
    }

    currentImage_ = {};
    stopCurrentAnimation();
    sequence_ = {};
    rhiWindow_->clearImage();

    if (!replacementPath.isEmpty()) {
        openImage(replacementPath);
        return;
    }

    errorAlert_->hide();
    contentStack_->setCurrentWidget(emptyState_);
    updateWindowTitle();
    updateImageControls();
    emptyOpenButton_->setFocus();
}

void ViewerWindow::openSizeFormatEditor() {
    if (!currentImage_.isValid() || loader_->isLoading())
        return;
    if (editWindow_) {
        editWindow_->show();
        editWindow_->raise();
        editWindow_->activateWindow();
        return;
    }
    stopCurrentAnimation();
    QElapsedTimer editorConstructionTimer;
    editorConstructionTimer.start();
    editingActive_ = true;
    saveAfterArtifact_ = false;
    rhiWindow_->setComparisonRatio(0.5);
    rhiWindow_->setComparisonToOriginal();
    EditPipelineOptions pipelineOptions;
    pipelineOptions.allowSourceRasterReuse =
        !(performanceTestActive_ && performanceOptions_.forceCpu);
    editSession_ = new EditPipelineController(pipelineOptions, this);
    editWindow_ = new EditSizeFormatWindow(currentImage_.filePath, currentImage_.sourceSize,
                                           currentImage_.isAnimated());
    if (performanceTestActive_) {
        EditExportSettings settings = editWindow_->settings();
        settings.width =
            std::max(1, qRound(settings.sourceSize.width() * performanceOptions_.scale));
        settings.height =
            std::max(1, qRound(settings.sourceSize.height() * performanceOptions_.scale));
        if (performanceOptions_.format != snow::image::Format::unknown)
            settings.format = performanceOptions_.format;
        settings.encode = {};
        settings.encode.format = settings.format;
        settings.encode.preserve_metadata = performanceOptions_.preserveMetadata;
        settings.encode.compression_level = 6;
        settings.encode.lossless = performanceOptions_.lossless;
        if (settings.format == snow::image::Format::jpeg) {
            settings.encode.quality = 75;
            settings.encode.progressive = true;
        } else if (settings.format == snow::image::Format::webp) {
            settings.encode.quality = 75;
            settings.encode.effort = 4;
        } else if (settings.format == snow::image::Format::heif ||
                   settings.format == snow::image::Format::avif) {
            settings.encode.quality = 50;
            settings.encode.effort = 6;
        } else if (settings.format == snow::image::Format::jxl) {
            settings.encode.quality = 75;
            settings.encode.effort = 7;
            settings.encode.progressive = true;
        }
        performanceSettings_ = settings;
    }
    recordPerformanceTiming(QStringLiteral("edit.editor_construct_duration"),
                            editorConstructionTimer.nsecsElapsed());
    editWindow_->setAttribute(Qt::WA_DeleteOnClose);
    connect(editWindow_, &QObject::destroyed, this, [this]() {
        editWindow_ = nullptr;
        if (editingActive_)
            closeSizeFormatEditor();
    });
    connect(editWindow_, &EditSizeFormatWindow::editorClosed, this,
            &ViewerWindow::closeSizeFormatEditor);
    connect(
        editWindow_, &EditSizeFormatWindow::settingsChanged, this,
        [this](const EditExportSettings& settings, EditChangeKind kind) {
            if (!editSession_)
                return;
            if (performanceTestActive_)
                return;
            const bool useGpu = !currentImage_.isAnimated() && !currentImage_.usesStoredTiles() &&
                                rhiWindow_->canGpuResize(QSize(settings.width, settings.height));
            if (useGpu != editSession_->usesGpuSource()) {
                if (useGpu)
                    editSession_->setGpuSource(currentImage_.filePath, currentImage_.rasterStore,
                                               currentImage_.analysis);
                else
                    editSession_->setSource(currentImage_.filePath, currentImage_.rasterStore,
                                            currentImage_.analysis);
                return;
            }
            editSession_->requestEdit(settings, kind);
        });
    connect(editWindow_, &EditSizeFormatWindow::saveRequested, this,
            &ViewerWindow::saveEditedImage);
    connect(editSession_, &EditPipelineController::busyChanged, editWindow_,
            &EditSizeFormatWindow::setBusy);
    connect(editSession_, &EditPipelineController::performanceStageCompleted, this,
            [this](EditRequestId requestId, const QString& stage, qint64 nanoseconds) {
                if (performanceTestActive_ && requestId != 0 && requestId != performanceRequestId_)
                    return;
                recordPerformanceTiming(stage, nanoseconds);
            });
    connect(rhiWindow_, &RhiImageWindow::editPerformanceStageCompleted, this,
            [this](EditRequestId requestId, const QString& stage, qint64 nanoseconds) {
                if (performanceTestActive_ && requestId != performanceRequestId_)
                    return;
                recordPerformanceTiming(stage, nanoseconds);
            });
    connect(editSession_, &EditPipelineController::visualRequested, this,
            [this](EditRequestId requestId, const EditExportSettings& settings) {
                if (!editSession_)
                    return;
                performanceRequestId_ = requestId;
                if (performanceTestActive_) {
                    performanceVisualRequestNanoseconds_ = performanceTimer_.nsecsElapsed();
                }
                if (editSession_->usesGpuSource()) {
                    rhiWindow_->requestEditVisual(requestId, settings);
                } else {
                    editSession_->submitVisualFrame(requestId);
                }
            });
    connect(editSession_, &EditPipelineController::artifactReady, this,
            [this](const EncodedEditResult& result) {
                if (!editWindow_ || !editSession_ ||
                    result.requestId != editSession_->latestRequestId())
                    return;
                const qint64 byteCount =
                    result.artifact ? static_cast<qint64>(result.artifact->byteSize()) : 0;
                performanceEncodedBytes_ = byteCount;
                performanceProvenance_ = result.provenance;
                performanceAlphaContent_ = result.alphaContent == snow::image::AlphaContent::opaque
                                               ? QStringLiteral("opaque")
                                               : QStringLiteral("non_opaque");
                editWindow_->setPreviewInfo(byteCount, QFileInfo(currentImage_.filePath).size(),
                                            result.warning);
                if (saveAfterArtifact_) {
                    saveAfterArtifact_ = false;
                    QTimer::singleShot(0, this, &ViewerWindow::saveEditedImage);
                }
                if (performanceTestActive_ && result.requestId == performanceRequestId_) {
                    recordPerformanceTiming(QStringLiteral("edit.request_to_artifact_ready"),
                                            performanceTimer_.nsecsElapsed() -
                                                performanceVisualRequestNanoseconds_);
                }
            });
    connect(
        editSession_, &EditPipelineController::exactReady, this,
        [this](const ExactEditResult& result) {
            if (!editWindow_ || !editSession_ ||
                result.requestId != editSession_->latestRequestId())
                return;
            performanceReusedGpuPixels_ = result.previewSource == ExactPreviewSource::gpu_raster;
            performancePreviewSource_ = result.previewSource;
            performanceProvenance_ = result.provenance;
            if (result.displayPreview) {
                stopCurrentAnimation();
                editPreviewImage_ = *result.displayPreview;
                if (currentImage_.isAnimated()) {
                    currentImage_.pixels = currentImage_.animationFrames.front().pixels;
                    rhiWindow_->setImage(currentImage_);
                }
                if (editPreviewImage_.isAnimated()) {
                    editPreviewImage_.pixels = editPreviewImage_.animationFrames.front().pixels;
                }
                rhiWindow_->setComparisonImage(editPreviewImage_);
                if (currentImage_.isAnimated() && editPreviewImage_.isAnimated()) {
                    animationTimer_->start(static_cast<int>(std::clamp<qint64>(
                        currentImage_.animationFrames.front().durationMilliseconds, 1,
                        std::numeric_limits<int>::max())));
                }
            }
            if (!performanceTestActive_ || result.requestId != performanceRequestId_)
                return;
            recordPerformanceTiming(QStringLiteral("edit.request_to_exact_ready"),
                                    performanceTimer_.nsecsElapsed() -
                                        performanceVisualRequestNanoseconds_);
            const auto queueEdit = [this](bool clearAll, bool clearExact, bool clearPreview) {
                if (clearAll) {
                    editSession_->clearAllCachesForBenchmark();
                    rhiWindow_->clearExactRasterCacheForBenchmark();
                } else if (clearExact) {
                    editSession_->clearExactArtifactCacheForBenchmark();
                } else if (clearPreview) {
                    editSession_->clearExactPreviewCacheForBenchmark();
                }
                QTimer::singleShot(0, this, [this]() {
                    if (performanceTestActive_ && editWindow_ && editSession_)
                        editSession_->requestEdit(activeEditSettings());
                });
            };
            if (performanceScenarioPhase_ == 0) {
                ++performanceIterationsCompleted_;
                if (performanceIterationsCompleted_ < performanceWarmupTarget_) {
                    queueEdit(true, false, false);
                    return;
                }
                performanceIterationsCompleted_ = 0;
                performanceResourceCacheHits_ = 0;
                performanceResourceCacheMisses_ = 0;
                if (performanceOptions_.rapidSuperseding) {
                    performanceScenarioPhase_ = 5;
                    QTimer::singleShot(0, this, &ViewerWindow::requestRapidPerformanceIteration);
                    return;
                }
                performanceScenarioPhase_ = 1;
                queueEdit(true, false, false);
                return;
            }
            if (performanceScenarioPhase_ == 1) {
                ++performanceIterationsCompleted_;
                if (performanceIterationsCompleted_ < performanceIterationsTarget_) {
                    queueEdit(true, false, false);
                    return;
                }
                capturePerformanceScenarioSnapshot(1);
                performanceScenarioPhase_ = 2;
                performanceIterationsCompleted_ = 0;
                queueEdit(false, true, false);
                return;
            }
            if (performanceScenarioPhase_ == 2) {
                ++performanceIterationsCompleted_;
                if (performanceIterationsCompleted_ < performanceIterationsTarget_) {
                    queueEdit(false, true, false);
                    return;
                }
                capturePerformanceScenarioSnapshot(2);
                performanceScenarioPhase_ = 3;
                performanceIterationsCompleted_ = 0;
                queueEdit(false, false, false);
                return;
            }
            if (performanceScenarioPhase_ == 3) {
                ++performanceIterationsCompleted_;
                if (performanceIterationsCompleted_ < performanceIterationsTarget_) {
                    queueEdit(false, false, false);
                    return;
                }
                capturePerformanceScenarioSnapshot(3);
                performanceScenarioPhase_ = 4;
                performanceIterationsCompleted_ = 0;
                queueEdit(false, false, true);
                return;
            } else if (performanceScenarioPhase_ == 4) {
                ++performanceIterationsCompleted_;
                if (performanceIterationsCompleted_ < performanceIterationsTarget_) {
                    queueEdit(false, false, true);
                    return;
                }
                capturePerformanceScenarioSnapshot(4);
            } else if (performanceScenarioPhase_ == 5) {
                ++performanceIterationsCompleted_;
                if (performanceIterationsCompleted_ < performanceIterationsTarget_) {
                    QTimer::singleShot(0, this, &ViewerWindow::requestRapidPerformanceIteration);
                    return;
                }
                capturePerformanceScenarioSnapshot(5);
            }
            const bool residencyValid =
                performanceOptions_.forceCpu
                    ? !performanceGpuPath_ &&
                          performanceProvenance_ == RasterProvenance::cpu_reference
                    : performanceGpuPath_ && performanceCpuPixelsReleased_ &&
                          currentImage_.pixels.isNull() &&
                          (performanceProvenance_ == RasterProvenance::gpu_approximate ||
                           performanceProvenance_ == RasterProvenance::source_exact);
            finishEditModePerformanceTest(
                residencyValid,
                residencyValid
                    ? QString()
                    : QStringLiteral("The edit benchmark did not retain the requested pipeline."));
        });
    connect(editSession_, &EditPipelineController::exactRasterRequested, rhiWindow_,
            &RhiImageWindow::requestEditResize);
    connect(rhiWindow_, &RhiImageWindow::editVisualFrameSubmitted, editSession_,
            &EditPipelineController::submitVisualFrame);
    connect(rhiWindow_, &RhiImageWindow::editResizeReadbackReady, editSession_,
            [this](quint64 generation, const GpuRasterResult& readback) {
                if (editSession_) {
                    editSession_->submitGpuResizeResult(generation, readback);
                }
            });
    connect(rhiWindow_, &RhiImageWindow::editResizeResourceCacheResult, this,
            [this](quint64 requestId, bool cacheHit) {
                if (!performanceTestActive_ || requestId != performanceRequestId_)
                    return;
                if (cacheHit)
                    ++performanceResourceCacheHits_;
                else
                    ++performanceResourceCacheMisses_;
            });
    connect(rhiWindow_, &RhiImageWindow::gpuOperationFailed, editSession_,
            [this](quint64 generation, const QString& message) {
                if (!editSession_ || generation != editSession_->latestRequestId())
                    return;
                if (editSession_->usesGpuSource() && QFileInfo::exists(currentImage_.filePath)) {
                    editSession_->setSource(currentImage_.filePath, currentImage_.rasterStore,
                                            currentImage_.analysis);
                    return;
                }
                editSession_->failGpuRequest(generation, message);
            });
    connect(editSession_, &EditPipelineController::visualReady, this, [this](EditRequestId) {
        performanceGpuPath_ = editSession_ && editSession_->usesGpuSource();
        if (performanceTestActive_) {
            recordPerformanceTiming(QStringLiteral("edit.request_to_visual_frame"),
                                    performanceTimer_.nsecsElapsed() -
                                        performanceVisualRequestNanoseconds_);
        }
    });
    connect(editSession_, &EditPipelineController::sourceReady, this, [this]() {
        if (performanceTestActive_) {
            recordPerformanceTiming(QStringLiteral("edit.request_to_source_ready"),
                                    performanceTimer_.nsecsElapsed() -
                                        performanceEditStartNanoseconds_);
        }
        if (editWindow_ && editSession_)
            editSession_->requestEdit(activeEditSettings());
    });
    connect(editSession_, &EditPipelineController::failed, this, [this](const QString& message) {
        if (editWindow_)
            editWindow_->setError(message);
        finishEditModePerformanceTest(false, message);
    });
    QElapsedTimer editorShowTimer;
    editorShowTimer.start();
    editWindow_->show();
    positionSizeFormatEditor();
    recordPerformanceTiming(QStringLiteral("edit.editor_show_duration"),
                            editorShowTimer.nsecsElapsed());
    const EditExportSettings initialSettings = activeEditSettings();
    if (!performanceOptions_.forceCpu && !currentImage_.isAnimated() &&
        !currentImage_.usesStoredTiles() &&
        rhiWindow_->canGpuResize(QSize(initialSettings.width, initialSettings.height))) {
        editSession_->setGpuSource(currentImage_.filePath, currentImage_.rasterStore,
                                   currentImage_.analysis);
    } else {
        // Forced-CPU benchmarking intentionally measures the decode/transform
        // reference baseline instead of the production source-raster fast path.
        editSession_->setSource(currentImage_.filePath,
                                performanceTestActive_ && performanceOptions_.forceCpu
                                    ? std::shared_ptr<ImageRasterStore>{}
                                    : currentImage_.rasterStore,
                                currentImage_.analysis);
    }
    updateImageControls();
}

void ViewerWindow::closeSizeFormatEditor() {
    if (!editingActive_ && !editWindow_ && !editSession_)
        return;
    editingActive_ = false;
    saveAfterArtifact_ = false;
    editWindowMinimizedWithViewer_ = false;
    editPreviewImage_ = {};
    rhiWindow_->cancelEditRequests();
    rhiWindow_->clearComparison();
    if (editSession_) {
        editSession_->cancel();
        editSession_->deleteLater();
        editSession_ = nullptr;
    }
    if (editWindow_) {
        EditSizeFormatWindow* closing = editWindow_;
        editWindow_ = nullptr;
        closing->disconnect(this);
        closing->hide();
        closing->deleteLater();
    }
    if (currentImage_.isAnimated()) {
        currentImage_.pixels = currentImage_.animationFrames.front().pixels;
        rhiWindow_->setImage(currentImage_);
    }
    startCurrentAnimation();
    updateImageControls();
    canvasContainer_->setFocus();
}

void ViewerWindow::saveEditedImage() {
    if (!editWindow_ || !editSession_)
        return;
    if (!editSession_->hasEncodedArtifact(editWindow_->settings())) {
        saveAfterArtifact_ = true;
        editSession_->flushPendingExact();
        return;
    }
    const EditExportSettings current = editWindow_->settings();
    const auto extensions = snow::image::format_extensions(current.format);
    const QString extension =
        extensions.empty() ? QStringLiteral("img")
                           : QString::fromUtf8(extensions.front().data(),
                                               static_cast<qsizetype>(extensions.front().size()));
    const QFileInfo source(currentImage_.filePath);
    const QString suggested = source.absoluteDir().filePath(
        QStringLiteral("%1-edited.%2").arg(source.completeBaseName(), extension));
    const QString filter =
        QStringLiteral("%1 (*.%2)")
            .arg(QString::fromUtf8(
                     snow::image::format_name(current.format).data(),
                     static_cast<qsizetype>(snow::image::format_name(current.format).size())),
                 extension);
    const QString path = QFileDialog::getSaveFileName(editWindow_, QStringLiteral("Save image as"),
                                                      suggested, filter);
    if (path.isEmpty())
        return;
    const auto artifact = editSession_->encodedArtifact();
    if (!artifact) {
        editWindow_->setError(QStringLiteral("The encoded artifact is unavailable."));
        return;
    }
    QSaveFile file(path);
    QString copyError;
    const qint64 byteCount = static_cast<qint64>(artifact->byteSize());
    if (!file.open(QIODevice::WriteOnly) || !artifact->copyTo(file, &copyError) || !file.commit()) {
        editWindow_->setError(!copyError.isEmpty() ? copyError
                              : file.errorString().isEmpty()
                                  ? QStringLiteral("The exported image could not be saved.")
                                  : file.errorString());
        return;
    }
    editWindow_->setPreviewInfo(byteCount, QFileInfo(currentImage_.filePath).size(),
                                QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()));
}

void ViewerWindow::positionSizeFormatEditor() {
    if (!editWindow_)
        return;
    QScreen* targetScreen = windowHandle() ? windowHandle()->screen() : nullptr;
    if (!targetScreen)
        targetScreen = QGuiApplication::screenAt(frameGeometry().center());
    if (!targetScreen)
        targetScreen = QGuiApplication::primaryScreen();
    if (!targetScreen)
        return;
    const QRect available = targetScreen->availableGeometry();
    const int width = std::min(editWindow_->width(), available.width());
    const int height = std::min(editWindow_->height(), std::max(1, available.height() - 24));
    editWindow_->resize(width, height);
    const QRect viewerFrame = frameGeometry();
    const int desiredX = viewerFrame.right() + 13;
    const int desiredY = viewerFrame.top();
    const int x = std::clamp(desiredX, available.left(), available.right() - width + 1);
    const int y = std::clamp(desiredY, available.top(), available.bottom() - height + 1);
    editWindow_->move(x, y);
}

void ViewerWindow::startCurrentAnimation() {
    stopCurrentAnimation();
    if (!currentImage_.isAnimated()) {
        return;
    }
    animationFrameIndex_ = 0;
    completedAnimationLoops_ = 0;
    currentImage_.pixels = currentImage_.animationFrames.front().pixels;
    animationTimer_->start(static_cast<int>(
        std::clamp<qint64>(currentImage_.animationFrames.front().durationMilliseconds, 1,
                           std::numeric_limits<int>::max())));
}

void ViewerWindow::stopCurrentAnimation() {
    if (animationTimer_) {
        animationTimer_->stop();
    }
    animationFrameIndex_ = 0;
    completedAnimationLoops_ = 0;
}

void ViewerWindow::advanceCurrentAnimation() {
    if (!currentImage_.isAnimated()) {
        return;
    }
    if (editingActive_) {
        if (!editPreviewImage_.isAnimated())
            return;
        const int sourceFrameCount = static_cast<int>(currentImage_.animationFrames.size());
        const int previewFrameCount = static_cast<int>(editPreviewImage_.animationFrames.size());
        animationFrameIndex_ = (animationFrameIndex_ + 1) % sourceFrameCount;
        const int previewIndex = animationFrameIndex_ % previewFrameCount;
        const DecodedAnimationFrame& sourceFrame =
            currentImage_.animationFrames.at(animationFrameIndex_);
        currentImage_.pixels = sourceFrame.pixels;
        editPreviewImage_.pixels = editPreviewImage_.animationFrames.at(previewIndex).pixels;
        rhiWindow_->setImage(currentImage_);
        rhiWindow_->setComparisonImage(editPreviewImage_);
        animationTimer_->start(static_cast<int>(std::clamp<qint64>(
            sourceFrame.durationMilliseconds, 1, std::numeric_limits<int>::max())));
        return;
    }
    ++animationFrameIndex_;
    const int frameCount = static_cast<int>(currentImage_.animationFrames.size());
    if (animationFrameIndex_ >= frameCount) {
        ++completedAnimationLoops_;
        if (currentImage_.loopCount != 0 && completedAnimationLoops_ >= currentImage_.loopCount) {
            animationFrameIndex_ = frameCount - 1;
            return;
        }
        animationFrameIndex_ = 0;
    }
    const DecodedAnimationFrame& frame = currentImage_.animationFrames.at(animationFrameIndex_);
    currentImage_.pixels = frame.pixels;
    rhiWindow_->setImage(currentImage_);
    animationTimer_->start(static_cast<int>(
        std::clamp<qint64>(frame.durationMilliseconds, 1, std::numeric_limits<int>::max())));
}

void ViewerWindow::showLoadError(const QString& path, const QString& message) {
    const QString fileName = QFileInfo(path).fileName();
    errorAlert_->setText(fileName.isEmpty() ? QStringLiteral("Image could not be opened")
                                            : QStringLiteral("Could not open %1").arg(fileName));
    errorAlert_->setInformativeText(message);
    errorAlert_->show();
    if (!currentImage_.isValid()) {
        contentStack_->setCurrentWidget(emptyState_);
        updateWindowTitle();
    }
    updateImageControls();
}

void ViewerWindow::updateImageControls() {
    const bool hasImage = currentImage_.isValid();
    const bool canUseImage = hasImage && !loader_->isLoading();
    const bool hasPrevious = sequence_.hasPrevious();
    const bool hasNext = sequence_.hasNext();
    previousButton_->setEnabled(hasPrevious && !editingActive_);
    nextButton_->setEnabled(hasNext && !editingActive_);
    rhiWindow_->setNavigationState(hasPrevious && !editingActive_, hasNext && !editingActive_);
    zoomOutButton_->setEnabled(hasImage && rhiWindow_->canZoomOut());
    zoomInButton_->setEnabled(hasImage && rhiWindow_->canZoomIn());
    actualSizeButton_->setEnabled(hasImage);
    rotateLeftButton_->setEnabled(hasImage && !editingActive_);
    rotateRightButton_->setEnabled(hasImage && !editingActive_);
    deleteButton_->setEnabled(hasImage && !editingActive_);
    editButton_->setEnabled(canUseImage && !editingActive_);
    copyImageAction_->setEnabled(canUseImage && !editingActive_ && !pixelReadbackInFlight_);
    copyImagePathAction_->setEnabled(canUseImage && !editingActive_);
#if defined(Q_OS_WIN)
    const bool canSetWindowsBackground = canUseImage && !editingActive_ &&
                                         !pixelReadbackInFlight_ &&
                                         !windowsBackgroundChangeInFlight_;
    printImageAction_->setEnabled(canUseImage && !editingActive_ && !pixelReadbackInFlight_ &&
                                  !windowsPrintInFlight_);
    shareImageAction_->setEnabled(canUseImage && !editingActive_);
    setAsMenu_->menuAction()->setEnabled(canSetWindowsBackground);
    setAsLockScreenAction_->setEnabled(canSetWindowsBackground);
    setAsBackgroundAction_->setEnabled(canSetWindowsBackground);
#endif
    openWithAction_->setEnabled(canUseImage && !editingActive_);
    revealInExplorerAction_->setEnabled(canUseImage && !editingActive_ &&
                                        !revealInExplorerInFlight_);
    deleteImageAction_->setEnabled(canUseImage && !editingActive_);
}

void ViewerWindow::updateActualSizeButtonAction() {
    if (fitToWindowAction_) {
        actualSizeButton_->setAccessibleName(QStringLiteral("Fit to window"));
        actualSizeButton_->setToolTip(QStringLiteral("Fit to window"));
        actualSizeButton_->setIconRef(outlined::Fullscreen());
        return;
    }

    actualSizeButton_->setAccessibleName(QStringLiteral("Show actual size"));
    actualSizeButton_->setToolTip(QStringLiteral("Actual size"));
    actualSizeButton_->setIconRef(outlined::OneToOne());
}

void ViewerWindow::updateWindowTitle() {
    if (!loadingFilePath_.isEmpty()) {
        const QFileInfo fileInfo(loadingFilePath_);
        setWindowTitle(QStringLiteral("%1 (Loading...), %2")
                           .arg(fileInfo.fileName())
                           .arg(QLocale().formattedDataSize(fileInfo.size())));
        return;
    }

    if (!currentImage_.isValid()) {
        setWindowTitle(QStringLiteral("Snow Image Viewer"));
        return;
    }

    const QFileInfo fileInfo(currentImage_.filePath);
    setWindowTitle(QStringLiteral("%1 | %2 \u00d7 %3, %4 | %5% - [%6/%7]")
                       .arg(fileInfo.fileName())
                       .arg(currentImage_.sourceSize.width())
                       .arg(currentImage_.sourceSize.height())
                       .arg(QLocale().formattedDataSize(fileInfo.size()))
                       .arg(std::lround(rhiWindow_->zoom() * 100.0))
                       .arg(sequence_.currentIndex() + 1)
                       .arg(sequence_.count()));
}

void ViewerWindow::refreshTheme() {
    const auto& manager = adqt::theme::ThemeManager::instance();
    const auto& theme = manager.theme();
    const auto& colors = theme.palette;
    const bool dark = theme.scheme == adqt::theme::ThemeScheme::Dark;
    const QColor canvas = colors.colorBgContainer;
    const QColor checkerLight =
        dark ? QColor(QStringLiteral("#343941")) : QColor(QStringLiteral("#4B5159"));
    const QColor checkerDark =
        dark ? QColor(QStringLiteral("#252A31")) : QColor(QStringLiteral("#383E45"));
    const QColor navigationBase = dark ? QColor(7, 10, 14, 166) : QColor(17, 22, 29, 150);
    const QColor navigationHover = dark ? QColor(7, 10, 14, 204) : QColor(17, 22, 29, 195);
    const QColor navigationPressed = dark ? QColor(4, 6, 9, 225) : QColor(10, 13, 18, 220);
    const QColor navigationIcon(255, 255, 255, 242);
    const QColor comparisonTrack(7, 9, 12, 158);
    const QColor comparisonThumb(Qt::black);
    const QColor comparisonHoverThumb = dark ? QColor(31, 35, 42, 252) : QColor(32, 36, 43, 250);
    const QColor comparisonPressedThumb = dark ? QColor(10, 12, 16, 252) : QColor(12, 15, 20, 250);
    const QColor comparisonLeadingArrow(246, 248, 251, 242);

    QPalette errorAlertPalette = manager.globalPalette();
    for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
        errorAlertPalette.setColor(group, QPalette::Window, colors.colorErrorBg);
        errorAlertPalette.setColor(group, QPalette::Mid, colors.colorErrorBorder);
        errorAlertPalette.setColor(group, QPalette::WindowText, colors.colorErrorText);
        errorAlertPalette.setColor(group, QPalette::Text, colors.colorText);
        errorAlertPalette.setColor(group, QPalette::ButtonText, colors.colorTextTertiary);
        errorAlertPalette.setColor(group, QPalette::AlternateBase, colors.colorErrorBgHover);
        errorAlertPalette.setColor(group, QPalette::Button, colors.colorErrorBgActive);
        errorAlertPalette.setColor(group, QPalette::Highlight, colors.colorErrorBorderHover);
    }
    errorAlert_->setPalette(errorAlertPalette);

    const QString style =
        QStringLiteral("#viewerRoot { background: %1; }"
                       "#viewerBottomToolbar { background: %2; border-top: 1px solid %3; }"
                       "#emptyState { background: %4; }"
                       "#emptyTitle { color: %5; font-size: 17px; font-weight: 600; }"
                       "#emptyHint { color: %6; font-size: 12px; }"
                       "#contentStack, #canvasPage { border: 0; background: %4; }")
            .arg(colors.colorBgLayout.name(QColor::HexArgb),
                 colors.colorBgContainer.name(QColor::HexArgb),
                 colors.colorBorderSecondary.name(QColor::HexArgb), canvas.name(QColor::HexRgb),
                 colors.colorText.name(QColor::HexArgb),
                 colors.colorTextSecondary.name(QColor::HexArgb));
    root_->setStyleSheet(style);
    rhiWindow_->setCanvasColors(canvas, checkerLight, checkerDark);
    rhiWindow_->setNavigationColors(navigationBase, navigationHover, navigationPressed,
                                    navigationIcon);
    rhiWindow_->setComparisonColors(comparisonTrack, comparisonThumb, comparisonHoverThumb,
                                    comparisonPressedThumb, comparisonLeadingArrow,
                                    colors.colorPrimary);

    adqt::icons::IconRenderRequest emptyIconRequest;
    emptyIconRequest.logicalSize = QSize(48, 48);
    emptyIconRequest.devicePixelRatio = devicePixelRatioF();
    emptyIcon_->setPixmap(adqt::icons::renderIconPixmap(
        outlined::Picture(adqt::icons::IconColors::primary(colors.colorTextTertiary)),
        emptyIconRequest));
}

void ViewerWindow::restoreSettings() {
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    lastDirectory_ = settings.value(QStringLiteral("files/lastDirectory")).toString();
}

void ViewerWindow::saveSettings() const {
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("files/lastDirectory"), lastDirectory_);
}

AdButton* ViewerWindow::makeIconButton(const QString& accessibleName, const QString& tooltipText,
                                       QWidget* parent) {
    auto* button = new AdButton(parent);
    button->setButtonStyle(AdButton::ButtonStyle::Text);
    button->setShape(AdButton::Shape::Rounded);
    button->setSizeClass(AdButton::SizeClass::Medium);
    button->setFixedSize(kToolbarButtonSize, kToolbarButtonSize);
    button->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    button->setAccessibleName(accessibleName);
    button->setToolTip(tooltipText);
    return button;
}

bool ViewerWindow::event(QEvent* event) {
    const QEvent::Type eventType = event ? event->type() : QEvent::None;
    const bool editorWasMinimized = editWindow_ && editWindow_->isMinimized();
    const bool handled = QMainWindow::event(event);
    if (!event || !editWindow_) {
        return handled;
    }

    if (eventType == QEvent::WindowStateChange) {
        if (isMinimized()) {
            editWindowMinimizedWithViewer_ = !editorWasMinimized;
            if (editWindowMinimizedWithViewer_) {
                editWindow_->showMinimized();
            }
        } else if (editWindowMinimizedWithViewer_) {
            editWindowMinimizedWithViewer_ = false;
            editWindow_->showNormal();
            scheduleSizeFormatEditorReveal();
        }
    } else if (eventType == QEvent::WindowActivate && !isMinimized()) {
        scheduleSizeFormatEditorReveal();
    }

    return handled;
}

bool ViewerWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#if defined(Q_OS_WIN)
    Q_UNUSED(eventType)
    if (message) {
        const auto* nativeMessage = static_cast<const MSG*>(message);
        if (nativeMessage->message == WM_ACTIVATE && LOWORD(nativeMessage->wParam) != WA_INACTIVE) {
            scheduleSizeFormatEditorReveal();
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void ViewerWindow::scheduleSizeFormatEditorReveal() {
    if (!editWindow_ || isMinimized() || editWindowRevealScheduled_)
        return;
    editWindowRevealScheduled_ = true;
    QTimer::singleShot(0, this, [this]() {
        editWindowRevealScheduled_ = false;
        revealSizeFormatEditor();
    });
}

void ViewerWindow::revealSizeFormatEditor() {
    if (!editWindow_ || isMinimized())
        return;
    if (editWindow_->isMinimized()) {
        editWindow_->showNormal();
    }
    positionSizeFormatEditor();
    editWindow_->raise();

#if defined(Q_OS_WIN)
    // Qt transports the native HWND through its integer-valued WId type.
    const HWND editorHandle =
        reinterpret_cast<HWND>(editWindow_->winId()); // NOLINT(performance-no-int-to-ptr)
    if (editorHandle) {
        SetWindowPos(editorHandle, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
#endif
}

void ViewerWindow::closeEvent(QCloseEvent* event) {
    closeSizeFormatEditor();
    saveSettings();
    QMainWindow::closeEvent(event);
}

void ViewerWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QMainWindow::dragEnterEvent(event);
}

void ViewerWindow::dropEvent(QDropEvent* event) {
    if (loader_->isLoading()) {
        event->ignore();
        return;
    }

    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
            openImage(url.toLocalFile());
            event->acceptProposedAction();
            return;
        }
    }
    QMainWindow::dropEvent(event);
}

} // namespace snow::image_viewer
