#pragma once

#include "core/image_types.h"
#include "core/view_transform.h"
#include "editing/edit_export_settings.h"
#include "editing/gpu_resize_readback.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QThreadPool>
#include <QWindow>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#if QT_CONFIG(vulkan) && __has_include(<QVulkanInstance>) && __has_include(<vulkan/vulkan.h>)
#define SNOW_IMAGE_VIEWER_HAS_QT_VULKAN 1
#else
#define SNOW_IMAGE_VIEWER_HAS_QT_VULKAN 0
#endif

QT_BEGIN_NAMESPACE
class QExposeEvent;
class QMouseEvent;
class QKeyEvent;
class QResizeEvent;
class QWheelEvent;
class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiSwapChain;
class QRhiTexture;
class QOffscreenSurface;
#if SNOW_IMAGE_VIEWER_HAS_QT_VULKAN
class QVulkanInstance;
#endif
QT_END_NAMESPACE

namespace snow::image_viewer {

enum class RhiBackend : std::uint8_t {
    platform_default,
    d3d11,
    opengl,
    vulkan,
    null_backend,
};

class RhiImageWindow final : public QWindow {
    Q_OBJECT

  public:
    explicit RhiImageWindow(QWindow* parent = nullptr,
                            RhiBackend backend = RhiBackend::platform_default);
    ~RhiImageWindow() override;

    void setImage(const DecodedImage& image);
    void setThumbnail(const ImageThumbnail& thumbnail);
    void clearImage();
    void setComparisonToOriginal();
    void setComparisonImage(const DecodedImage& preview);
    void clearComparison();
    bool comparisonActive() const {
        return comparisonActive_;
    }
    qreal comparisonRatio() const {
        return comparisonRatio_;
    }
    void setComparisonRatio(qreal ratio);
    void prewarm();
    int maximumTextureSize() const;
    QString backendName() const;
    QString adapterName() const;
    bool hasGpuOnlyStaticImage() const {
        return imageGpuOnlyResident_;
    }
    bool canGpuResize(const QSize& size) const;
    void requestMainImageReadback(quint64 requestId);
    void requestEditVisual(quint64 requestId, const EditExportSettings& settings);
    void requestEditResize(quint64 requestId, const EditExportSettings& settings);
    void clearExactRasterCacheForBenchmark();
    std::uint64_t exactRasterCacheBytes() const;
    std::uint64_t sourceTextureBytes() const;
    std::uint64_t sourceTextureCount() const;
    void cancelEditRequests();

    void setActualSize();
    void fitToWindow();
    void zoomBy(qreal factor, const QPointF& anchor = {});
    void rotateLeft();
    void rotateRight();
    qreal zoom() const;
    bool canZoomIn() const;
    bool canZoomOut() const;

    void setCanvasColors(const QColor& canvas, const QColor& checkerLight,
                         const QColor& checkerDark);
    void setNavigationState(bool canNavigatePrevious, bool canNavigateNext);
    void setNavigationColors(const QColor& background, const QColor& hoverBackground,
                             const QColor& pressedBackground, const QColor& icon);
    void setComparisonColors(const QColor& track, const QColor& thumb, const QColor& hoverThumb,
                             const QColor& pressedThumb, const QColor& leadingArrow,
                             const QColor& trailingArrow);

  signals:
    void zoomChanged(qreal zoom);
    void imageFrameSubmitted(const QString& filePath, bool thumbnail);
    void comparisonFrameSubmitted(const QString& filePath);
    void staticTextureResident(const QString& filePath);
    void gpuBackingLost(const QString& filePath);
    void mainImageReadbackReady(quint64 requestId, const QImage& pixels);
    void editVisualFrameSubmitted(quint64 requestId);
    void editPerformanceStageCompleted(quint64 requestId, const QString& stage, qint64 nanoseconds);
    void editResizeReadbackReady(quint64 requestId,
                                 const snow::image_viewer::GpuRasterResult& readback);
    void editResizeResourceCacheResult(quint64 requestId, bool cacheHit);
    void gpuOperationFailed(quint64 requestId, const QString& message);
    void renderError(const QString& message);
    void imageDropped(const QString& filePath);
    void contextMenuRequested(const QPoint& globalPosition);
    void previousRequested();
    void nextRequested();
    void comparisonRatioChanged(qreal ratio);
    void comparisonCloseRequested();

  protected:
    bool event(QEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    enum class OutputMode {
        Sdr,
        HdrScRgb,
    };

    enum class NavigationDirection {
        None,
        Previous,
        Next,
    };

    struct UniformData;
    struct OverlayUniformData;
    struct OverlayVertex;
    struct ImageTile;
    struct VirtualImageTexture;
    struct ExactRasterResources;
    struct ExactRasterJob;

    bool hasImage() const;
    void setImageInternal(const DecodedImage& image);
    bool isNavigationControlHit(NavigationDirection direction, const QPointF& position) const;
    void requestRender();
    void render();
    bool initializeRhi();
    bool initializeSwapChain();
    bool initializeStaticResources();
    bool rebuildImageResources();
    bool rebuildComparisonResources();
    bool appendComparisonTile(const QRectF& sourceRect, QImage pixels);
    void beginComparisonTileStaging(const QRectF& requested);
    void scheduleComparisonTileLoads();
    void cancelComparisonTileStaging();
    bool rebuildPipeline();
    bool rebuildEditProxyPipeline();
    bool rebuildNavigationPipeline();
    bool submitPendingExactRaster(QRhiCommandBuffer* commandBuffer);
    std::shared_ptr<ExactRasterResources>
    exactRasterResources(const QSize& targetSize, const QSize& fullTargetSize,
                         PixelEncoding encoding, snow::image::ResamplingMethod resampling,
                         bool tiled, bool* cacheHit);
    void completeExactRaster(const std::shared_ptr<ExactRasterJob>& job);
    void clearEditJobs();
    void releaseExactRasterResources();
    void releaseImageResources();
    void releaseComparisonResources();
    void releasePipelineResources();
    void releaseEditProxyResources();
    void releaseStaticResources();
    void releaseSwapChain();
    void releaseAllResources();
    void updateOutputMode();
    bool shouldUseStoredTiles() const;
    QRectF visibleSourceRect() const;
    void updateTileSelection();
    void updateNavigationHover(const QPointF& position);
    void appendNavigationVertices(std::vector<OverlayVertex>* vertices,
                                  const QSize& pixelSize) const;
    void appendComparisonVertices(std::vector<OverlayVertex>* vertices,
                                  const QSize& pixelSize) const;
    bool isComparisonHandleHit(const QPointF& position) const;
    UniformData buildUniformData(const DecodedImage& image, const QSize& pixelSize,
                                 const QRectF& sourceRect, const QSize& textureSize) const;
    OverlayUniformData buildOverlayUniformData(const QSize& pixelSize) const;
    QColor canvasClearColor() const;
    QPointF effectiveAnchor(const QPointF& anchor) const;

    RhiBackend backend_ = RhiBackend::platform_default;
    std::unique_ptr<QOffscreenSurface> fallbackSurface_;
#if SNOW_IMAGE_VIEWER_HAS_QT_VULKAN
    std::unique_ptr<QVulkanInstance> vulkanInstance_;
#endif
    std::unique_ptr<QRhi> rhi_;
    QRhiSwapChain* swapChain_ = nullptr;
    QRhiRenderPassDescriptor* renderPassDescriptor_ = nullptr;
    QRhiBuffer* vertexBuffer_ = nullptr;
    QRhiBuffer* overlayVertexBuffer_ = nullptr;
    QRhiBuffer* overlayUniformBuffer_ = nullptr;
    QRhiSampler* sampler_ = nullptr;
    QRhiShaderResourceBindings* overlayShaderResources_ = nullptr;
    QRhiGraphicsPipeline* pipeline_ = nullptr;
    QRhiGraphicsPipeline* virtualPipeline_ = nullptr;
    QRhiGraphicsPipeline* overlayPipeline_ = nullptr;
    QRhiBuffer* editProxyUniformBuffer_ = nullptr;
    QRhiSampler* editProxyNearestSampler_ = nullptr;
    QRhiSampler* editProxyLinearSampler_ = nullptr;
    QRhiShaderResourceBindings* editProxyNearestBindings_ = nullptr;
    QRhiShaderResourceBindings* editProxyLinearBindings_ = nullptr;
    QRhiGraphicsPipeline* editProxyPipeline_ = nullptr;

    DecodedImage image_;
    DecodedImage comparisonImage_;
    std::vector<std::unique_ptr<ImageTile>> imageTiles_;
    std::shared_ptr<VirtualImageTexture> imageVirtualTexture_;
    std::vector<std::unique_ptr<ImageTile>> comparisonTiles_;
    QThreadPool comparisonTilePool_;
    std::vector<QRect> comparisonTileLoadQueue_;
    std::size_t nextComparisonTileLoad_ = 0;
    int activeComparisonTileLoads_ = 0;
    quint64 comparisonTileGeneration_ = 0;
    bool comparisonTileLoadFailed_ = false;
    QRectF loadedTileRegion_;
    QRectF comparisonLoadedTileRegion_;
    bool usingStoredTiles_ = false;
    bool comparisonUsingStoredTiles_ = false;
    ViewTransform viewTransform_;
    QColor canvasColor_{QStringLiteral("#101216")};
    QColor checkerLight_{QStringLiteral("#2B2F36")};
    QColor checkerDark_{QStringLiteral("#20242A")};
    QColor navigationBackground_{Qt::transparent};
    QColor navigationHoverBackground_{Qt::transparent};
    QColor navigationPressedBackground_{Qt::transparent};
    QColor navigationIcon_{Qt::white};
    QColor comparisonTrack_{0, 0, 0, 153};
    QColor comparisonThumb_{Qt::black};
    QColor comparisonHoverThumb_{QStringLiteral("#20242A")};
    QColor comparisonPressedThumb_{QStringLiteral("#0E1014")};
    QColor comparisonLeadingArrow_{246, 248, 251, 242};
    QColor comparisonTrailingArrow_{QStringLiteral("#1677FF")};
    OutputMode outputMode_ = OutputMode::Sdr;
    float displayPeakNits_ = 100.0F;
    float sdrWhiteNits_ = 200.0F;
    bool sceneReferred_ = false;
    bool swapChainReady_ = false;
    bool staticResourcesReady_ = false;
    bool imageResourcesDirty_ = false;
    bool comparisonResourcesDirty_ = false;
    bool imageIsThumbnail_ = false;
    bool imageFrameSubmissionPending_ = false;
    bool comparisonFrameSubmissionPending_ = false;
    bool imageGpuOnlyEligible_ = false;
    bool imageGpuOnlyResident_ = false;
    bool vertexUploadPending_ = false;
    bool canNavigatePrevious_ = false;
    bool canNavigateNext_ = false;
    bool dragging_ = false;
    bool comparisonActive_ = false;
    bool comparisonHandleHovered_ = false;
    bool comparisonHandlePressed_ = false;
    bool pointerWithinWindow_ = false;
    bool renderPending_ = false;
    quint64 pendingVisualFrameRequestId_ = 0;
    std::optional<std::pair<quint64, EditExportSettings>> pendingExactRaster_;
    std::shared_ptr<ExactRasterJob> activeExactRaster_;
    std::vector<std::shared_ptr<ExactRasterJob>> retiredExactRasterJobs_;
    std::vector<std::shared_ptr<ExactRasterResources>> cachedExactRasterResources_;
    std::optional<EditExportSettings> editProxySettings_;
    std::vector<std::unique_ptr<ImageTile>> retiredEditSourceTiles_;
    QPointF lastPointerPosition_;
    QPointF navigationPointerPosition_;
    NavigationDirection navigationHovered_ = NavigationDirection::None;
    NavigationDirection navigationPressed_ = NavigationDirection::None;
    qreal comparisonDragOffsetX_ = 0.0;
    qreal comparisonRatio_ = 0.5;
};

} // namespace snow::image_viewer
