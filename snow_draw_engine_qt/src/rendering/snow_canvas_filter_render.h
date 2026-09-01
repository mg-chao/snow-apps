#pragma once

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRegion>

#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>

namespace snow_canvas_filter_render {

struct ImageView {
    std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    qsizetype stride = 0;
};

struct ConstImageView {
    const std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    qsizetype stride = 0;
};

struct AlphaView {
    const std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    qsizetype stride = 0;
};

enum class SimdBackend {
    Scalar,
    Avx2,
};

struct Parameters {
    std::uint32_t type = 0;
    double strength = 1.0;
    double logicalBlockSize = 1.0;
    double logicalSigma = 0.0;
    double logicalSamplingRadius = 0.0;
    qreal devicePixelRatio = 1.0;
    QPointF gridOriginInImage;
};

struct ExecutionOptions {
    bool forceScalar = false;
    bool singleThreaded = false;
    bool forceDenseMask = false;
};

struct MaskSpan {
    int y = 0;
    int beginX = 0;
    int endX = 0;
};

struct GaussianBlurPlan {
    int reductionFactor = 1;
    int passCount = 0;
    int radii[3] = {0, 0, 0};
    int physicalSupportRadius = 0;
};

struct KernelDiagnostics {
    SimdBackend backend = SimdBackend::Scalar;
    std::size_t allocatedBytes = 0;
    std::size_t copiedBytes = 0;
    std::size_t scratchReuseCount = 0;
    std::size_t parallelJobs = 0;
    int adaptiveBlurFactor = 1;
    std::size_t retainedBytes = 0;
    std::size_t gaussianPasses = 0;
    std::size_t gaussianAvx2Executions = 0;
    std::size_t gaussianDownsampleAvx2Executions = 0;
    std::size_t gaussianReconstructionAvx2Executions = 0;
    std::uint64_t downsampleNanoseconds = 0;
    std::uint64_t reducedBlurNanoseconds = 0;
    std::uint64_t reconstructionNanoseconds = 0;
};

class RenderWorkspace {
  public:
    explicit RenderWorkspace(std::size_t retainedByteLimit = 128u * 1024u * 1024u);
    ~RenderWorkspace();

    RenderWorkspace(const RenderWorkspace&) = delete;
    RenderWorkspace& operator=(const RenderWorkspace&) = delete;

    QImage& argbScratchA(const QSize& size, qreal devicePixelRatio = 1.0);
    QImage& argbScratchB(const QSize& size, qreal devicePixelRatio = 1.0);
    QImage& sceneScratch(const QSize& size, qreal devicePixelRatio = 1.0);
    QImage& preLayerScratch(const QSize& size, qreal devicePixelRatio = 1.0);
    QImage& alphaScratch(const QSize& size, qreal devicePixelRatio = 1.0);
    std::vector<QRgb>& mosaicSampleScratch(std::size_t count);
    // Releases all canvas-owned scratch images and sample storage.
    void clear();
    void finishFrame(bool releaseAll = false);
    const KernelDiagnostics& diagnostics() const;
    void resetDiagnostics();
    std::size_t retainedBytes() const;
    void setAllocationFailureForTests(bool fail);

  private:
    struct PoolEntry;
    QImage& ensureImage(QImage& image, PoolEntry*& entry, int lease, const QSize& size,
                        QImage::Format format, qreal dpr);
    void releaseLease(PoolEntry*& entry, QImage& image);

    std::size_t m_retainedByteLimit;
    std::list<PoolEntry> m_pool;
    std::uint64_t m_poolClock = 0;
    QImage m_argbA;
    QImage m_argbB;
    QImage m_scene;
    QImage m_preLayer;
    QImage m_alpha;
    PoolEntry* m_argbAEntry = nullptr;
    PoolEntry* m_argbBEntry = nullptr;
    PoolEntry* m_sceneEntry = nullptr;
    PoolEntry* m_preLayerEntry = nullptr;
    PoolEntry* m_alphaEntry = nullptr;
    std::vector<QRgb> m_mosaicSamples;
    KernelDiagnostics m_diagnostics;
    bool m_failAllocationsForTests = false;
};

ConstImageView view(const QImage& image);
ImageView view(QImage& image);
AlphaView alphaView(const QImage& image);
SimdBackend selectedSimdBackend();
const char* simdBackendName(SimdBackend backend);
int samplingRadiusPixels(const Parameters& parameters);
GaussianBlurPlan gaussianBlurPlan(const Parameters& parameters);
void apply(QImage& image, const Parameters& parameters, RenderWorkspace* workspace = nullptr,
           const ExecutionOptions& options = {});
bool applyMasked(const QImage& source, QImage& destination, const QImage& mask,
                 const QRect& destinationPixels, const Parameters& parameters,
                 RenderWorkspace* workspace = nullptr, const ExecutionOptions& options = {});
bool applyMaskedSparse(const QImage& source, QImage& destination, const QImage& croppedMask,
                       const QPoint& maskOriginPixels, const QRect& destinationPixels,
                       const std::vector<MaskSpan>& spans, const std::vector<QRect>& occupiedBlocks,
                       const Parameters& parameters, RenderWorkspace* workspace = nullptr,
                       const ExecutionOptions& options = {});
bool applyMasked(const QImage& source, QImage& destination, const QImage& croppedMask,
                 const QPoint& maskOriginPixels, const QRect& destinationPixels,
                 const Parameters& parameters, RenderWorkspace* workspace = nullptr,
                 const ExecutionOptions& options = {});
bool applyRect(const QImage& source, QImage& destination, const QRect& destinationPixels,
               double opacity, const Parameters& parameters, RenderWorkspace* workspace = nullptr,
               const ExecutionOptions& options = {});
bool applyRegion(const QImage& source, QImage& destination, const QRegion& destinationPixels,
                 const Parameters& parameters, RenderWorkspace* workspace = nullptr,
                 const ExecutionOptions& options = {});
void blendOverSource(QImage& filtered, const QImage& source, double opacity,
                     const ExecutionOptions& options = {},
                     KernelDiagnostics* diagnostics = nullptr);

} // namespace snow_canvas_filter_render
