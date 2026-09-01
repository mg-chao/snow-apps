#pragma once

#include "core/image_types.h"
#include "editing/encoded_artifact.h"
#include "editing/edit_export_settings.h"
#include "editing/gpu_resize_readback.h"

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QTimer>

#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include <snow/image/processing.h>

namespace snow::image {
struct Document;
}

class QProcess;
class QTemporaryDir;

namespace snow::image_viewer {

class MappedRasterPackage;
class RasterAsset;
class ImageRasterStore;

using EditRequestId = quint64;

enum class EditPipelineState {
    VisualPending,
    VisualReady,
    EncodingPending,
    ArtifactReady,
    ExactReady,
    Failed,
};

enum class RasterHandoffMode : quint8 {
    automatic,
    shared_memory,
    verified_file,
};

struct EditPipelineOptions final {
    int dimensionDebounceMs = 180;
    int continuousDebounceMs = 100;
    int discreteDebounceMs = 0;
    std::size_t memoryBudgetBytes = 0;
    std::size_t cacheBudgetBytes = 0;
    int workerTimeoutMs = 120000;
    QString workerExecutablePath;
    QString workerTestMode;
    bool allowSourceRasterReuse = true;
    RasterHandoffMode rasterHandoffMode = RasterHandoffMode::automatic;
};

enum class ExactPreviewSource : quint8 {
    gpu_raster,
    base_raster,
    prepared_raster,
    codec_artifact,
};

struct ExactEditResult final {
    EditRequestId requestId = 0;
    EditExportSettings settings;
    QString warning;
    std::optional<DecodedImage> displayPreview;
    ExactPreviewSource previewSource = ExactPreviewSource::base_raster;
    std::shared_ptr<const EncodedArtifact> artifact;
    RasterProvenance provenance = RasterProvenance::cpu_reference;
    bool exactPreviewAvailable = false;
    snow::image::AlphaContent alphaContent = snow::image::AlphaContent::non_opaque;

    bool isValid() const {
        return requestId != 0 && artifact && artifact->byteSize() > 0;
    }
};

struct EncodedEditResult final {
    EditRequestId requestId = 0;
    EditExportSettings settings;
    QString warning;
    std::shared_ptr<const EncodedArtifact> artifact;
    RasterProvenance provenance = RasterProvenance::cpu_reference;
    snow::image::AlphaContent alphaContent = snow::image::AlphaContent::non_opaque;

    bool isValid() const {
        return requestId != 0 && artifact && artifact->byteSize() > 0;
    }
};

class EditPipelineController final : public QObject {
    Q_OBJECT

  public:
    explicit EditPipelineController(QObject* parent = nullptr);
    EditPipelineController(EditPipelineOptions options, QObject* parent);
    ~EditPipelineController() override;

    void setSource(const QString& filePath,
                   const std::shared_ptr<ImageRasterStore>& rasterStore = {},
                   snow::image::RasterAnalysis analysis = {});
    void setGpuSource(const QString& filePath,
                      const std::shared_ptr<ImageRasterStore>& rasterStore = {},
                      snow::image::RasterAnalysis analysis = {});
    EditRequestId requestEdit(const EditExportSettings& settings,
                              EditChangeKind kind = EditChangeKind::discrete);
    void flushPendingExact();
    void submitVisualFrame(EditRequestId requestId);
    void submitGpuResizeResult(EditRequestId requestId, GpuRasterResult readback);
    void failGpuRequest(EditRequestId requestId, const QString& message);
    void cancel();
    void clearExactArtifactCacheForBenchmark();
    void clearExactPreviewCacheForBenchmark();
    void clearAllCachesForBenchmark();

    bool isBusy() const {
        return state_ == EditPipelineState::EncodingPending;
    }
    EditPipelineState state() const {
        return state_;
    }
    EditRequestId latestRequestId() const {
        return latestRequestId_;
    }
    bool hasExactResult(const EditExportSettings& settings) const;
    bool hasEncodedArtifact(const EditExportSettings& settings) const;
    bool hasExactPreview(const EditExportSettings& settings) const;
    bool isPreviewPending() const {
        return previewPending_;
    }
    bool hasCurrentPreview(const EditExportSettings& settings) const {
        return hasEncodedArtifact(settings);
    }
    std::shared_ptr<const EncodedArtifact> encodedArtifact() const;
    QString sourcePath() const {
        return sourcePath_;
    }
    bool usesGpuSource() const {
        return gpuSource_;
    }
    int cancellationCount() const {
        return cancellationCount_;
    }
    std::size_t cacheBytes() const {
        return cacheBytes_;
    }
    std::uint64_t mappedBytes() const;
    std::uint64_t sharedRasterBytes() const;
    std::uint64_t encodedBytes() const;
    std::uint64_t previewBytes() const;
    std::uint64_t readbackBytes() const {
        return lastReadbackBytes_;
    }
    std::uint64_t temporaryFileBytes() const;
    std::uint64_t temporaryFileCount() const;
    qint64 workerProcessId() const;
    qint64 workerStartupNanoseconds() const {
        return workerStartupNanoseconds_;
    }
    int workerRestartCount() const {
        return workerRestartCount_;
    }

  signals:
    void stateChanged(snow::image_viewer::EditPipelineState state);
    void busyChanged(bool busy);
    void performanceStageCompleted(EditRequestId requestId, const QString& stage,
                                   qint64 nanoseconds);
    void visualRequested(EditRequestId requestId,
                         const snow::image_viewer::EditExportSettings& settings);
    void exactRasterRequested(EditRequestId requestId,
                              const snow::image_viewer::EditExportSettings& settings);
    void sourceReady();
    void visualReady(EditRequestId requestId);
    void artifactReady(const snow::image_viewer::EncodedEditResult& result);
    // Emitted once the exact export is complete; displayPreview may still be
    // absent when the preview is pending or unavailable.
    void exactReady(const snow::image_viewer::ExactEditResult& result);
    void previewUnavailable(EditRequestId requestId, const QString& message);
    void failed(const QString& message);

  private:
    struct SourceResult;
    struct PreviewResult;
    struct PendingExact final {
        EditRequestId requestId = 0;
        EditExportSettings settings;
        std::shared_ptr<std::stop_source> cancellation;
        ExportKey key;
        std::size_t estimatedWorkingBytes = 0;
    };
    struct PendingPreview final {
        PendingExact request;
        std::shared_ptr<const EncodedArtifact> artifact;
        RasterProvenance provenance = RasterProvenance::cpu_reference;
        snow::image::AlphaContent alphaContent = snow::image::AlphaContent::non_opaque;
    };
    struct CacheEntry final {
        ExportKey key;
        ExactEditResult result;
        std::size_t artifactWeight = 0;
        std::size_t previewWeight = 0;
        std::size_t previewDiskWeight = 0;
        std::uint64_t artifactAccessSerial = 0;
        std::uint64_t previewAccessSerial = 0;
    };
    struct RasterCacheEntry final {
        BaseRasterKey key;
        std::shared_ptr<MappedRasterPackage> package;
        RasterProvenance provenance = RasterProvenance::cpu_reference;
        std::size_t weight = 0;
        std::uint64_t accessSerial = 0;
    };
    struct ActiveWorkerJob;

    void setState(EditPipelineState state);
    void startSourceDecode(EditRequestId generation,
                           const std::shared_ptr<std::stop_source>& cancellation);
    void scheduleExact();
    void startPendingExact();
    void startCpuExact(PendingExact request, std::shared_ptr<MappedRasterPackage> cachedRaster = {},
                       RasterProvenance provenance = RasterProvenance::cpu_reference);
    void startGpuEncode(PendingExact request, GpuRasterResult readback);
    void finishBasePreparation(PendingExact request, PreviewResult result);
    void dispatchWorker(PendingExact request, std::shared_ptr<MappedRasterPackage> package,
                        RasterProvenance provenance);
    void dispatchPreviewWorker(PendingExact request,
                               std::shared_ptr<const EncodedArtifact> artifact,
                               RasterProvenance provenance, snow::image::AlphaContent alphaContent);
    void startWorker();
    void sendActiveWorkerJob();
    void handleWorkerOutput();
    void handleWorkerExit(int exitCode);
    void cancelActiveWorker(bool restart);
    void retireIdleWorker();
    void cleanupWorkerJob(const ActiveWorkerJob& job);
    void finishWorker(const PendingExact& request, PreviewResult result);
    bool publishCacheHit(EditRequestId requestId, const EditExportSettings& settings,
                         const ExportKey& key);
    void cacheResult(const ExportKey& key, const ExactEditResult& result);
    std::shared_ptr<MappedRasterPackage> findRaster(const BaseRasterKey& key,
                                                    RasterProvenance* provenance);
    void cacheRaster(const BaseRasterKey& key, std::shared_ptr<MappedRasterPackage> package,
                     RasterProvenance provenance);
    void trimCache();
    bool evictOldestCachePortion();
    void clearCache();

    QThreadPool pool_;
    EditPipelineOptions options_;
    QTimer exactTimer_;
    QElapsedTimer exactDelayTimer_;
    QString sourcePath_;
    snow::image::RasterAnalysis sourceAnalysis_;
    std::shared_ptr<MappedRasterPackage> sourceRaster_;
    std::shared_ptr<snow::image::Document> sourceDocument_;
    std::shared_ptr<std::stop_source> sourceCancellation_;
    std::vector<std::shared_ptr<std::stop_source>> workerCancellations_;
    std::optional<PendingExact> pendingExact_;
    std::optional<PendingPreview> pendingPreview_;
    std::optional<ExactEditResult> exactResult_;
    EditExportSettings latestSettings_;
    EditRequestId latestRequestId_ = 0;
    EditRequestId sourceGeneration_ = 0;
    EditPipelineState state_ = EditPipelineState::VisualReady;
    int activeWorkerCount_ = 0;
    bool gpuSource_ = false;
    bool sourceReady_ = false;
    int cancellationCount_ = 0;
    std::vector<CacheEntry> cache_;
    std::vector<RasterCacheEntry> rasterCache_;
    // File-backed artifacts and raster stores are budgeted independently from
    // resident preview pixels and worker-private allocations.
    std::size_t diskCacheBytes_ = 0;
    std::size_t cacheBytes_ = 0;
    std::size_t reservedWorkerBytes_ = 0;
    std::size_t editingBudgetBytes_ = 0;
    std::shared_ptr<QTemporaryDir> artifactDirectory_;
    QString sharedMemorySessionKey_;
    QProcess* worker_ = nullptr;
    QByteArray workerOutput_;
    bool workerReady_ = false;
    bool shuttingDown_ = false;
    std::unique_ptr<ActiveWorkerJob> activeWorkerJob_;
    QTimer workerTimeout_;
    QElapsedTimer workerStartupTimer_;
    QElapsedTimer replacementDispatchTimer_;
    EditRequestId replacementDispatchRequestId_ = 0;
    qint64 workerStartupNanoseconds_ = 0;
    int workerRestartCount_ = 0;
    bool workerEverStarted_ = false;
    std::uint64_t accessSerial_ = 0;
    std::uint64_t lastReadbackBytes_ = 0;
    bool previewPending_ = false;
};

} // namespace snow::image_viewer

Q_DECLARE_METATYPE(snow::image_viewer::EditPipelineState)
Q_DECLARE_METATYPE(snow::image_viewer::ExactEditResult)
Q_DECLARE_METATYPE(snow::image_viewer::EncodedEditResult)
