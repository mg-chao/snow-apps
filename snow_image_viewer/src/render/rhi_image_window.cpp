#include "render/rhi_image_window.h"

#include "core/image_tile_store.h"
#include "render/texture_size.h"

#include <QEvent>
#include <QElapsedTimer>
#include <QExposeEvent>
#include <QFile>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QDropEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QKeyEvent>
#include <QMimeData>
#include <QOffscreenSurface>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QUrl>
#if SNOW_IMAGE_VIEWER_HAS_QT_VULKAN
#include <QVulkanInstance>
#endif
#include <QWheelEvent>
#include <QtConcurrentRun>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace snow::image_viewer {
namespace {

RhiBackend resolveBackend(RhiBackend backend) {
    if (backend != RhiBackend::platform_default)
        return backend;
#if defined(Q_OS_WIN)
    return RhiBackend::d3d11;
#else
    return RhiBackend::opengl;
#endif
}

QString backendLabel(RhiBackend backend) {
    switch (backend) {
    case RhiBackend::platform_default:
        return QStringLiteral("platform default");
    case RhiBackend::d3d11:
        return QStringLiteral("D3D11");
    case RhiBackend::opengl:
        return QStringLiteral("OpenGL");
    case RhiBackend::vulkan:
        return QStringLiteral("Vulkan");
    case RhiBackend::null_backend:
        return QStringLiteral("Null");
    }
    return QStringLiteral("unknown");
}

QShader loadShader(const QString& resourcePath) {
    static QHash<QString, QShader> cache;
    const auto cached = cache.constFind(resourcePath);
    if (cached != cache.cend()) {
        return cached.value();
    }
    QFile file(resourcePath);
    const QShader shader =
        file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader();
    cache.insert(resourcePath, shader);
    return shader;
}

QString droppedLocalFile(const QMimeData* mimeData) {
    if (!mimeData || !mimeData->hasUrls()) {
        return {};
    }

    for (const QUrl& url : mimeData->urls()) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
            return url.toLocalFile();
        }
    }
    return {};
}

float srgbToLinear(float value) {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

std::array<float, 4> linearColor(const QColor& color, float scale = 1.0F) {
    return {srgbToLinear(color.redF()) * scale, srgbToLinear(color.greenF()) * scale,
            srgbToLinear(color.blueF()) * scale, color.alphaF()};
}

bool startTopLevelSystemMove(QWindow* window) {
    QWindow* topLevel = window;
    while (auto* parentWindow = qobject_cast<QWindow*>(topLevel->parent())) {
        topLevel = parentWindow;
    }
    return topLevel->startSystemMove();
}

constexpr std::array<float, 16> kQuadVertices = {
    0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
};

constexpr qreal kNavigationControlSize = 42.0;
constexpr qreal kNavigationControlOffset = 12.0;
constexpr qreal kNavigationHoverWidth = 88.0;
constexpr qreal kNavigationArrowHalfHeight = 8.0;
constexpr qreal kNavigationArrowHalfWidth = 5.0;
constexpr qreal kNavigationArrowStrokeWidth = 1.6;
constexpr qreal kNavigationAntialiasWidth = 0.75;
constexpr int kNavigationVertexCapacity = 12;
constexpr qreal kComparisonHitHalfWidth = 15.0;
constexpr qreal kComparisonTrackWidth = 8.0;
constexpr qreal kComparisonThumbRadius = 24.0;
constexpr qreal kComparisonThumbOutlineWidth = 1.0;
constexpr qreal kComparisonThumbHitRadius = 30.0;
constexpr qreal kComparisonArrowHalfHeight = 8.0;
constexpr qreal kComparisonArrowWidth = 7.0;
constexpr int kComparisonVertexCapacity = 30;
constexpr int kOverlayVertexCapacity =
    std::max(kNavigationVertexCapacity, kComparisonVertexCapacity);

QImage imageTilePixels(const QImage& source, const QRect& tileRect) {
    const int bytesPerPixel = source.depth() / 8;
    if (source.isNull() || bytesPerPixel <= 0 || !source.rect().contains(tileRect)) {
        return {};
    }

    auto* tileData =
        const_cast<uchar*>(source.constScanLine(tileRect.y())) + tileRect.x() * bytesPerPixel;
    QImage tile(tileData, tileRect.width(), tileRect.height(), source.bytesPerLine(),
                source.format());
    tile.setColorSpace(source.colorSpace());
    return tile;
}

QImage paddedArrayLayer(const QImage& source, const QRect& tileRect, const QSize& layerSize) {
    const int bytesPerPixel = source.depth() / 8;
    if (source.isNull() || bytesPerPixel <= 0 || !layerSize.isValid() ||
        !source.rect().contains(tileRect) || tileRect.width() > layerSize.width() ||
        tileRect.height() > layerSize.height()) {
        return {};
    }
    QImage layer(layerSize, source.format());
    if (layer.isNull())
        return {};
    layer.setColorSpace(source.colorSpace());
    const std::size_t pixelBytes = static_cast<std::size_t>(bytesPerPixel);
    const std::size_t sourceRowBytes = static_cast<std::size_t>(tileRect.width()) * pixelBytes;
    for (int y = 0; y < tileRect.height(); ++y) {
        const uchar* sourceRow = source.constScanLine(tileRect.y() + y) +
                                 static_cast<std::size_t>(tileRect.x()) * pixelBytes;
        uchar* destinationRow = layer.scanLine(y);
        std::memcpy(destinationRow, sourceRow, sourceRowBytes);
        const uchar* edgePixel = destinationRow + sourceRowBytes - pixelBytes;
        for (int x = tileRect.width(); x < layerSize.width(); ++x) {
            std::memcpy(destinationRow + static_cast<std::size_t>(x) * pixelBytes, edgePixel,
                        pixelBytes);
        }
    }
    const std::size_t layerRowBytes = static_cast<std::size_t>(layerSize.width()) * pixelBytes;
    for (int y = tileRect.height(); y < layerSize.height(); ++y) {
        std::memcpy(layer.scanLine(y), layer.constScanLine(tileRect.height() - 1), layerRowBytes);
    }
    return layer;
}

QRectF sourceRectForTextureTile(const QRect& textureRect, const QSize& textureSize,
                                const QSize& sourceSize) {
    if (!textureSize.isValid() || !sourceSize.isValid()) {
        return {};
    }

    const qreal scaleX = static_cast<qreal>(sourceSize.width()) / textureSize.width();
    const qreal scaleY = static_cast<qreal>(sourceSize.height()) / textureSize.height();
    return QRectF(textureRect.x() * scaleX, textureRect.y() * scaleY, textureRect.width() * scaleX,
                  textureRect.height() * scaleY);
}

} // namespace

struct RhiImageWindow::UniformData {
    float mvp[16]{};
    float checkerLight[4]{};
    float checkerDark[4]{};
    float outputParameters[4]{};
    float sourceParameters[4]{};
    float textureParameters[4]{};
};

struct ResizeUniformData {
    float mvp[16]{};
    float sourceAndTarget[4]{};
    float options[4]{};
    float scaleAndAxis[4]{};
    int targetTile[4]{};
    float backendParameters[4]{};
};

struct RhiImageWindow::OverlayUniformData {
    float mvp[16]{};
};

struct RhiImageWindow::OverlayVertex {
    float position[2]{};
    float color[4]{};
    float shape[4]{};
};

struct RhiImageWindow::ImageTile {
    QRectF sourceRect;
    QImage sourcePixels;
    QImage pixels;
    QSize textureSize;
    QRhiTexture* texture = nullptr;
    QRhiBuffer* uniformBuffer = nullptr;
    QRhiShaderResourceBindings* shaderResources = nullptr;
    bool uploadPending = true;
    bool mipGenerationPending = false;
};

struct RhiImageWindow::VirtualImageTexture final {
    ~VirtualImageTexture() {
        delete shaderResources;
        delete uniformBuffer;
        delete texture;
    }

    TextureArrayTilePlan plan;
    std::vector<QImage> layerPixels;
    QRhiTexture* texture = nullptr;
    QRhiBuffer* uniformBuffer = nullptr;
    QRhiShaderResourceBindings* shaderResources = nullptr;
    bool uploadPending = true;
    bool mipGenerationPending = true;
};

struct RhiImageWindow::ExactRasterResources final {
    ~ExactRasterResources() {
        delete pipeline;
        delete intermediatePipeline;
        delete intermediateBindings;
        delete nearestBindings;
        delete linearBindings;
        delete renderPass;
        delete intermediateRenderPass;
        delete target;
        delete intermediateTarget;
        delete nearestSampler;
        delete linearSampler;
        delete uniformBuffer;
        delete secondPassUniformBuffer;
        delete texture;
        delete intermediateTexture;
    }

    QRhiTexture* sourceTexture = nullptr;
    std::shared_ptr<VirtualImageTexture> virtualSource;
    QSize targetSize;
    QSize fullTargetSize;
    PixelEncoding encoding = PixelEncoding::Srgb8;
    snow::image::ResamplingMethod resampling = snow::image::ResamplingMethod::lanczos3;
    bool tiled = false;
    QRhiTexture* texture = nullptr;
    QRhiTexture* intermediateTexture = nullptr;
    QSize intermediateSize;
    int mipLevel = 0;
    int firstAxis = 0;
    int secondAxis = 0;
    QRhiBuffer* uniformBuffer = nullptr;
    QRhiBuffer* secondPassUniformBuffer = nullptr;
    QRhiSampler* nearestSampler = nullptr;
    QRhiSampler* linearSampler = nullptr;
    QRhiTextureRenderTarget* target = nullptr;
    QRhiTextureRenderTarget* intermediateTarget = nullptr;
    QRhiRenderPassDescriptor* renderPass = nullptr;
    QRhiRenderPassDescriptor* intermediateRenderPass = nullptr;
    QRhiShaderResourceBindings* nearestBindings = nullptr;
    QRhiShaderResourceBindings* linearBindings = nullptr;
    QRhiShaderResourceBindings* intermediateBindings = nullptr;
    QRhiGraphicsPipeline* pipeline = nullptr;
    QRhiGraphicsPipeline* intermediatePipeline = nullptr;
};

struct RhiImageWindow::ExactRasterJob final {
    struct TileReadback final {
        QRect pixelRect;
        QRhiReadbackResult result;
    };

    quint64 requestId = 0;
    EditExportSettings settings;
    PixelEncoding encoding = PixelEncoding::Srgb8;
    ColorMetadata color;
    std::vector<std::shared_ptr<ExactRasterResources>> resources;
    std::vector<TileReadback> readbacks;
    std::size_t completedReadbacks = 0;
    QElapsedTimer gpuTimer;
};

RhiImageWindow::RhiImageWindow(QWindow* parent, RhiBackend backend)
    : QWindow(parent), backend_(resolveBackend(backend)) {
    comparisonTilePool_.setMaxThreadCount(2);
    switch (backend_) {
    case RhiBackend::d3d11:
        setSurfaceType(QSurface::Direct3DSurface);
        break;
    case RhiBackend::opengl:
        setSurfaceType(QSurface::OpenGLSurface);
        break;
    case RhiBackend::vulkan:
        setSurfaceType(QSurface::VulkanSurface);
#if SNOW_IMAGE_VIEWER_HAS_QT_VULKAN
        vulkanInstance_ = std::make_unique<QVulkanInstance>();
        vulkanInstance_->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (vulkanInstance_->create())
            setVulkanInstance(vulkanInstance_.get());
#endif
        break;
    case RhiBackend::null_backend:
        setSurfaceType(QSurface::RasterSurface);
        break;
    case RhiBackend::platform_default:
        break;
    }
    setTitle(QStringLiteral("Snow Image Viewer canvas"));
    connect(this, &QWindow::screenChanged, this, [this]() {
        viewTransform_.setDevicePixelRatio(devicePixelRatio());
        updateTileSelection();
        emit zoomChanged(viewTransform_.zoom());
        releaseSwapChain();
        requestRender();
    });
}

RhiImageWindow::~RhiImageWindow() {
    disconnect(this, nullptr, nullptr, nullptr);
    cancelComparisonTileStaging();
    comparisonTilePool_.clear();
    comparisonTilePool_.waitForDone();
    releaseAllResources();
    // QWindow's base destructor runs after members. Release the native Vulkan
    // surface while its instance member is still alive.
    destroy();
#if SNOW_IMAGE_VIEWER_HAS_QT_VULKAN
    vulkanInstance_.reset();
#endif
    fallbackSurface_.reset();
}

void RhiImageWindow::setImage(const DecodedImage& image) {
    imageIsThumbnail_ = false;
    setImageInternal(image);
}

void RhiImageWindow::setImageInternal(const DecodedImage& image) {
    const bool preserveGeometry = image_.isValid() && image_.filePath == image.filePath &&
                                  image_.sourceSize == image.sourceSize;
    image_ = image;
    imageGpuOnlyEligible_ = false;
    imageGpuOnlyResident_ = false;
    loadedTileRegion_ = {};
    usingStoredTiles_ = false;
    viewTransform_.setDevicePixelRatio(devicePixelRatio());
    if (!preserveGeometry) {
        viewTransform_.setImageSize(image.sourceSize);
    }
    imageResourcesDirty_ = true;
    imageFrameSubmissionPending_ = true;
    updateOutputMode();
    emit zoomChanged(viewTransform_.zoom());
    requestRender();
    QTimer::singleShot(0, this, [this]() { requestRender(); });
    QTimer::singleShot(50, this, [this]() { requestRender(); });
}

void RhiImageWindow::setThumbnail(const ImageThumbnail& thumbnail) {
    if (!thumbnail.isValid()) {
        return;
    }

    DecodedImage image;
    image.filePath = thumbnail.filePath;
    image.sourceSize = thumbnail.sourceSize;
    image.pixels = thumbnail.pixels;
    image.pixelEncoding = PixelEncoding::Srgb8;
    image.color.sourceColorSpace = QColorSpace(QColorSpace::SRgb);
    image.color.description = QStringLiteral("sRGB system thumbnail");
    image.color.transferDescription = QStringLiteral("sRGB");
    image.decoderName = QStringLiteral("System thumbnail");
    imageIsThumbnail_ = true;
    setImageInternal(image);
}

void RhiImageWindow::clearImage() {
    clearEditJobs();
    image_ = {};
    comparisonImage_ = {};
    comparisonActive_ = false;
    comparisonHandlePressed_ = false;
    comparisonDragOffsetX_ = 0.0;
    imageFrameSubmissionPending_ = false;
    comparisonFrameSubmissionPending_ = false;
    imageGpuOnlyEligible_ = false;
    imageGpuOnlyResident_ = false;
    releaseImageResources();
    updateOutputMode();
    emit zoomChanged(viewTransform_.zoom());
    requestRender();
}

void RhiImageWindow::setComparisonToOriginal() {
    if (!image_.isValid())
        return;
    editProxySettings_.reset();
    comparisonImage_ = {};
    comparisonActive_ = true;
    releaseComparisonResources();
    comparisonResourcesDirty_ = false;
    requestRender();
}

void RhiImageWindow::setComparisonImage(const DecodedImage& preview) {
    if (!image_.isValid() || !preview.isValid())
        return;
    editProxySettings_.reset();
    comparisonImage_ = preview;
    comparisonActive_ = true;
    comparisonResourcesDirty_ = true;
    comparisonFrameSubmissionPending_ = true;
    requestRender();
}

void RhiImageWindow::clearComparison() {
    editProxySettings_.reset();
    if (!comparisonActive_ && !comparisonImage_.isValid())
        return;
    comparisonActive_ = false;
    comparisonImage_ = {};
    comparisonFrameSubmissionPending_ = false;
    comparisonHandleHovered_ = false;
    comparisonHandlePressed_ = false;
    comparisonDragOffsetX_ = 0.0;
    releaseComparisonResources();
    comparisonResourcesDirty_ = false;
    unsetCursor();
    requestRender();
}

void RhiImageWindow::setComparisonRatio(qreal ratio) {
    const qreal normalized = std::clamp(ratio, 0.0, 1.0);
    if (qFuzzyCompare(comparisonRatio_ + 1.0, normalized + 1.0))
        return;
    comparisonRatio_ = normalized;
    emit comparisonRatioChanged(comparisonRatio_);
    requestRender();
}

void RhiImageWindow::prewarm() {
    if (initializeRhi()) {
        requestRender();
    }
}

bool RhiImageWindow::hasImage() const {
    return image_.isValid();
}

bool RhiImageWindow::isNavigationControlHit(NavigationDirection direction,
                                            const QPointF& position) const {
    if (direction == NavigationDirection::None || width() <= 0 || height() <= 0) {
        return false;
    }

    const qreal radius = kNavigationControlSize * 0.5;
    const qreal centerX = direction == NavigationDirection::Previous
                              ? kNavigationControlOffset + radius
                              : width() - kNavigationControlOffset - radius;
    const QPointF center(centerX, height() * 0.5);
    const QPointF delta = position - center;
    return delta.x() * delta.x() + delta.y() * delta.y() <= radius * radius;
}

void RhiImageWindow::updateNavigationHover(const QPointF& position) {
    navigationPointerPosition_ = position;
    const NavigationDirection previous = navigationHovered_;
    NavigationDirection next = NavigationDirection::None;
    if (pointerWithinWindow_ && hasImage() && !comparisonActive_) {
        if (position.x() < kNavigationHoverWidth && canNavigatePrevious_) {
            next = NavigationDirection::Previous;
        } else if (position.x() >= width() - kNavigationHoverWidth && canNavigateNext_) {
            next = NavigationDirection::Next;
        }
    }

    navigationHovered_ = next;
    if (!comparisonActive_ && !dragging_ && navigationPressed_ == NavigationDirection::None) {
        if (next == NavigationDirection::None) {
            unsetCursor();
        } else {
            setCursor(Qt::PointingHandCursor);
        }
    }
    if (previous != next || previous != NavigationDirection::None ||
        next != NavigationDirection::None) {
        requestRender();
    }
}

void RhiImageWindow::appendNavigationVertices(std::vector<OverlayVertex>* vertices,
                                              const QSize& pixelSize) const {
    if (!vertices || navigationHovered_ == NavigationDirection::None || width() <= 0 ||
        pixelSize.isEmpty()) {
        return;
    }

    const qreal pixelRatio = static_cast<qreal>(pixelSize.width()) / width();
    const qreal radius = kNavigationControlSize * 0.5;
    const qreal centerX = navigationHovered_ == NavigationDirection::Previous
                              ? kNavigationControlOffset + radius
                              : width() - kNavigationControlOffset - radius;
    const QPointF center(centerX, height() * 0.5);
    const bool controlHovered =
        isNavigationControlHit(navigationHovered_, navigationPointerPosition_);
    const QColor background =
        navigationPressed_ == navigationHovered_
            ? navigationPressedBackground_
            : (controlHovered ? navigationHoverBackground_ : navigationBackground_);
    const float whiteScale =
        outputMode_ == OutputMode::HdrScRgb && sceneReferred_ ? sdrWhiteNits_ / 80.0F : 1.0F;
    const std::array<float, 4> backgroundColor = linearColor(background, whiteScale);
    const std::array<float, 4> iconColor = linearColor(navigationIcon_, whiteScale);

    const auto appendVertex = [vertices,
                               pixelRatio](const QPointF& position, const QPointF& localPosition,
                                           const std::array<float, 4>& color, qreal direction) {
        OverlayVertex vertex;
        vertex.position[0] = static_cast<float>(position.x() * pixelRatio);
        vertex.position[1] = static_cast<float>(position.y() * pixelRatio);
        std::copy(color.cbegin(), color.cend(), vertex.color);
        vertex.shape[0] = static_cast<float>(localPosition.x());
        vertex.shape[1] = static_cast<float>(localPosition.y());
        vertex.shape[2] = static_cast<float>(pixelRatio);
        vertex.shape[3] = static_cast<float>(direction);
        vertices->push_back(vertex);
    };
    const auto appendQuad = [&appendVertex, &center](const QPointF& halfExtent,
                                                     const std::array<float, 4>& color,
                                                     qreal direction) {
        const QPointF topLeft(-halfExtent.x(), -halfExtent.y());
        const QPointF topRight(halfExtent.x(), -halfExtent.y());
        const QPointF bottomLeft(-halfExtent.x(), halfExtent.y());
        const QPointF bottomRight(halfExtent.x(), halfExtent.y());
        appendVertex(center + topLeft, topLeft, color, direction);
        appendVertex(center + bottomLeft, bottomLeft, color, direction);
        appendVertex(center + topRight, topRight, color, direction);
        appendVertex(center + topRight, topRight, color, direction);
        appendVertex(center + bottomLeft, bottomLeft, color, direction);
        appendVertex(center + bottomRight, bottomRight, color, direction);
    };

    const qreal antialiasWidth = kNavigationAntialiasWidth / pixelRatio;
    appendQuad(QPointF(radius + antialiasWidth, radius + antialiasWidth), backgroundColor, 0.0);

    const qreal arrowHalfStroke = kNavigationArrowStrokeWidth * 0.5;
    const QPointF arrowExtent(kNavigationArrowHalfWidth + arrowHalfStroke + antialiasWidth,
                              kNavigationArrowHalfHeight + arrowHalfStroke + antialiasWidth);
    const qreal direction = navigationHovered_ == NavigationDirection::Previous ? -1.0 : 1.0;
    appendQuad(arrowExtent, iconColor, direction);
}

void RhiImageWindow::setActualSize() {
    viewTransform_.setActualSize();
    updateTileSelection();
    emit zoomChanged(viewTransform_.zoom());
    requestRender();
}

void RhiImageWindow::fitToWindow() {
    viewTransform_.fitToWindow();
    updateTileSelection();
    emit zoomChanged(viewTransform_.zoom());
    requestRender();
}

void RhiImageWindow::zoomBy(qreal factor, const QPointF& anchor) {
    viewTransform_.zoomBy(factor, effectiveAnchor(anchor));
    updateTileSelection();
    emit zoomChanged(viewTransform_.zoom());
    requestRender();
}

void RhiImageWindow::rotateLeft() {
    viewTransform_.rotateLeft();
    updateTileSelection();
    emit zoomChanged(viewTransform_.zoom());
    requestRender();
}

void RhiImageWindow::rotateRight() {
    viewTransform_.rotateRight();
    updateTileSelection();
    emit zoomChanged(viewTransform_.zoom());
    requestRender();
}

qreal RhiImageWindow::zoom() const {
    return viewTransform_.zoom();
}

bool RhiImageWindow::canZoomIn() const {
    return !viewTransform_.isAtMaximumZoom();
}

bool RhiImageWindow::canZoomOut() const {
    return !viewTransform_.isAtMinimumZoom();
}

void RhiImageWindow::setCanvasColors(const QColor& canvas, const QColor& checkerLight,
                                     const QColor& checkerDark) {
    canvasColor_ = canvas;
    checkerLight_ = checkerLight;
    checkerDark_ = checkerDark;
    requestRender();
}

void RhiImageWindow::setNavigationState(bool canNavigatePrevious, bool canNavigateNext) {
    if (canNavigatePrevious_ == canNavigatePrevious && canNavigateNext_ == canNavigateNext) {
        return;
    }

    canNavigatePrevious_ = canNavigatePrevious;
    canNavigateNext_ = canNavigateNext;
    updateNavigationHover(navigationPointerPosition_);
    requestRender();
}

void RhiImageWindow::setNavigationColors(const QColor& background, const QColor& hoverBackground,
                                         const QColor& pressedBackground, const QColor& icon) {
    if (navigationBackground_ == background && navigationHoverBackground_ == hoverBackground &&
        navigationPressedBackground_ == pressedBackground && navigationIcon_ == icon) {
        return;
    }

    navigationBackground_ = background;
    navigationHoverBackground_ = hoverBackground;
    navigationPressedBackground_ = pressedBackground;
    navigationIcon_ = icon;
    requestRender();
}

int RhiImageWindow::maximumTextureSize() const {
    return rhi_ ? rhi_->resourceLimit(QRhi::TextureSizeMax) : 0;
}

QString RhiImageWindow::backendName() const {
    if (!rhi_)
        return QStringLiteral("uninitialized");
    switch (rhi_->backend()) {
    case QRhi::Vulkan:
        return QStringLiteral("Vulkan");
    case QRhi::OpenGLES2:
        return QStringLiteral("OpenGL");
    case QRhi::D3D11:
        return QStringLiteral("D3D11");
    case QRhi::D3D12:
        return QStringLiteral("D3D12");
    case QRhi::Metal:
        return QStringLiteral("Metal");
    case QRhi::Null:
        return QStringLiteral("Null");
    }
    return QStringLiteral("unknown");
}

QString RhiImageWindow::adapterName() const {
    return rhi_ ? QString::fromUtf8(rhi_->driverInfo().deviceName) : QString();
}

bool RhiImageWindow::canGpuResize(const QSize& size) const {
    const int limit = maximumTextureSize();
    return imageGpuOnlyResident_ && (imageVirtualTexture_ || imageTiles_.size() == 1) &&
           size.isValid() && limit > 0;
}

void RhiImageWindow::requestMainImageReadback(quint64 requestId) {
    if (!imageGpuOnlyResident_ || (!imageVirtualTexture_ && imageTiles_.size() != 1) || !rhi_) {
        emit gpuOperationFailed(requestId,
                                QStringLiteral("The image texture is not available for readback."));
        return;
    }
    QRhiCommandBuffer* commandBuffer = nullptr;
    if (rhi_->beginOffscreenFrame(&commandBuffer) != QRhi::FrameOpSuccess) {
        emit gpuOperationFailed(requestId, QStringLiteral("Could not begin the image readback."));
        return;
    }
    const std::size_t readbackCount =
        imageVirtualTexture_ ? imageVirtualTexture_->plan.tiles.size() : 1U;
    std::vector<QRhiReadbackResult> results(readbackCount);
    QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
    for (std::size_t layer = 0; layer < readbackCount; ++layer) {
        QRhiReadbackDescription description(imageVirtualTexture_ ? imageVirtualTexture_->texture
                                                                 : imageTiles_.front()->texture);
        description.setLayer(static_cast<int>(layer));
        updates->readBackTexture(description, &results[layer]);
    }
    commandBuffer->resourceUpdate(updates);
    const QRhi::FrameOpResult frameResult = rhi_->endOffscreenFrame();
    if (frameResult != QRhi::FrameOpSuccess || results.front().data.isEmpty()) {
        emit gpuOperationFailed(requestId,
                                QStringLiteral("The image texture could not be read back."));
        return;
    }
    const QRhiTexture::Format textureFormat = image_.pixelEncoding == PixelEncoding::LinearScRgb16F
                                                  ? QRhiTexture::RGBA16F
                                                  : QRhiTexture::RGBA8;
    const QImage::Format format =
        textureFormat == QRhiTexture::RGBA16F ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA8888;
    const std::size_t bytesPerPixel = format == QImage::Format_RGBA16FPx4 ? 8U : 4U;
    const auto validPayload = [textureFormat, bytesPerPixel](const QRhiReadbackResult& result,
                                                             const QSize& expectedSize) {
        if (result.format != textureFormat || result.pixelSize != expectedSize ||
            !expectedSize.isValid()) {
            return false;
        }
        const std::size_t width = static_cast<std::size_t>(expectedSize.width());
        const std::size_t height = static_cast<std::size_t>(expectedSize.height());
        if (width > std::numeric_limits<std::size_t>::max() / bytesPerPixel)
            return false;
        const std::size_t stride = width * bytesPerPixel;
        if (height > std::numeric_limits<std::size_t>::max() / stride)
            return false;
        const std::size_t required = stride * height;
        return required <= static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()) &&
               result.data.size() >= static_cast<qsizetype>(required);
    };
    QImage pixels;
    if (imageVirtualTexture_) {
        pixels = QImage(image_.sourceSize, format);
        bool valid = !pixels.isNull();
        for (std::size_t layer = 0; valid && layer < results.size(); ++layer) {
            const QRhiReadbackResult& result = results[layer];
            const QRect& rect = imageVirtualTexture_->plan.tiles[layer];
            const std::size_t sourceStride =
                static_cast<std::size_t>(result.pixelSize.width()) * bytesPerPixel;
            valid = validPayload(result, imageVirtualTexture_->plan.layerSize);
            for (int row = 0; valid && row < rect.height(); ++row) {
                std::memcpy(pixels.scanLine(rect.y() + row) +
                                static_cast<std::size_t>(rect.x()) * bytesPerPixel,
                            result.data.constData() + static_cast<std::size_t>(row) * sourceStride,
                            static_cast<std::size_t>(rect.width()) * bytesPerPixel);
            }
        }
        if (!valid)
            pixels = {};
    } else {
        const QRhiReadbackResult& result = results.front();
        if (validPayload(result, imageTiles_.front()->textureSize)) {
            const qsizetype bytesPerLine = static_cast<qsizetype>(
                static_cast<std::size_t>(result.pixelSize.width()) * bytesPerPixel);
            QImage view(reinterpret_cast<const uchar*>(result.data.constData()),
                        result.pixelSize.width(), result.pixelSize.height(), bytesPerLine, format);
            pixels = view.copy();
        }
    }
    if (pixels.isNull()) {
        emit gpuOperationFailed(requestId,
                                QStringLiteral("The image texture readback was incomplete."));
        return;
    }
    pixels.setColorSpace(image_.pixelEncoding == PixelEncoding::LinearScRgb16F
                             ? QColorSpace(QColorSpace::SRgbLinear)
                             : QColorSpace(QColorSpace::SRgb));
    emit mainImageReadbackReady(requestId, pixels);
}

void RhiImageWindow::requestEditVisual(quint64 requestId, const EditExportSettings& settings) {
    if (!imageGpuOnlyResident_ || (!imageVirtualTexture_ && imageTiles_.empty())) {
        emit gpuOperationFailed(requestId, QStringLiteral("The source texture is unavailable."));
        return;
    }
    // The resident source texture remains visible while exact export work is pending.
    // The request ID is published only after this state reaches the swapchain.
    comparisonActive_ = true;
    comparisonImage_ = {};
    editProxySettings_ = settings;
    pendingVisualFrameRequestId_ = requestId;
    requestRender();
}

void RhiImageWindow::requestEditResize(quint64 requestId, const EditExportSettings& settings) {
    if (!canGpuResize(QSize(settings.width, settings.height))) {
        emit gpuOperationFailed(
            requestId, QStringLiteral("The requested size is not eligible for GPU resizing."));
        return;
    }
    pendingExactRaster_ = std::pair{requestId, settings};
    requestRender();
}

void RhiImageWindow::clearExactRasterCacheForBenchmark() {
    if (!activeExactRaster_)
        releaseExactRasterResources();
}

std::uint64_t RhiImageWindow::exactRasterCacheBytes() const {
    std::uint64_t total = 0;
    for (const auto& resources : cachedExactRasterResources_) {
        const std::uint64_t bytesPerPixel =
            resources->encoding == PixelEncoding::LinearScRgb16F ? 8U : 4U;
        const auto bytes = [bytesPerPixel](const QSize& size) {
            return size.isValid() ? static_cast<std::uint64_t>(size.width()) *
                                        static_cast<std::uint64_t>(size.height()) * bytesPerPixel
                                  : 0U;
        };
        total += bytes(resources->targetSize) + bytes(resources->intermediateSize);
    }
    return total;
}

std::uint64_t RhiImageWindow::sourceTextureCount() const {
    return imageVirtualTexture_
               ? static_cast<std::uint64_t>(imageVirtualTexture_->plan.tiles.size())
               : static_cast<std::uint64_t>(imageTiles_.size());
}

std::uint64_t RhiImageWindow::sourceTextureBytes() const {
    const std::uint64_t bytesPerPixel =
        image_.pixelEncoding == PixelEncoding::LinearScRgb16F ? 8U : 4U;
    std::uint64_t total = 0;
    if (imageVirtualTexture_) {
        int width = imageVirtualTexture_->plan.layerSize.width();
        int height = imageVirtualTexture_->plan.layerSize.height();
        for (;;) {
            total += static_cast<std::uint64_t>(width) * height * bytesPerPixel *
                     imageVirtualTexture_->plan.tiles.size();
            if (width == 1 && height == 1)
                break;
            width = std::max(1, width / 2);
            height = std::max(1, height / 2);
        }
        return total;
    }
    for (const std::unique_ptr<ImageTile>& tile : imageTiles_) {
        int width = tile->textureSize.width();
        int height = tile->textureSize.height();
        for (;;) {
            total += static_cast<std::uint64_t>(std::max(0, width)) *
                     static_cast<std::uint64_t>(std::max(0, height)) * bytesPerPixel;
            if (imageIsThumbnail_ || (width == 1 && height == 1))
                break;
            width = std::max(1, width / 2);
            height = std::max(1, height / 2);
        }
    }
    return total;
}

void RhiImageWindow::cancelEditRequests() {
    clearEditJobs();
    requestRender();
}

std::shared_ptr<RhiImageWindow::ExactRasterResources> RhiImageWindow::exactRasterResources(
    const QSize& targetSize, const QSize& fullTargetSize, PixelEncoding encoding,
    snow::image::ResamplingMethod resampling, bool tiled, bool* cacheHit) {
    QRhiTexture* sourceTexture = imageVirtualTexture_  ? imageVirtualTexture_->texture
                                 : imageTiles_.empty() ? nullptr
                                                       : imageTiles_.front()->texture;
    for (const auto& cached : cachedExactRasterResources_) {
        if (cached->sourceTexture == sourceTexture && cached->targetSize == targetSize &&
            cached->fullTargetSize == fullTargetSize && cached->encoding == encoding &&
            cached->resampling == resampling && cached->tiled == tiled) {
            if (cacheHit)
                *cacheHit = true;
            return cached;
        }
    }
    if (cacheHit)
        *cacheHit = false;
    if (!rhi_ || !sourceTexture || !targetSize.isValid())
        return {};

    if (!cachedExactRasterResources_.empty()) {
        const auto& first = cachedExactRasterResources_.front();
        if (first->sourceTexture != sourceTexture || first->fullTargetSize != fullTargetSize ||
            first->encoding != encoding || first->resampling != resampling ||
            first->tiled != tiled) {
            cachedExactRasterResources_.clear();
        }
    }

    const QShader vertexShader = loadShader(QStringLiteral(":/shaders/edit_resize.vert.qsb"));
    const QShader fragmentShader =
        loadShader(imageVirtualTexture_ ? QStringLiteral(":/shaders/edit_resize_array.frag.qsb")
                                        : QStringLiteral(":/shaders/edit_resize.frag.qsb"));
    if (!vertexShader.isValid() || !fragmentShader.isValid())
        return {};

    auto resources = std::make_shared<ExactRasterResources>();
    resources->sourceTexture = sourceTexture;
    resources->virtualSource = imageVirtualTexture_;
    resources->targetSize = targetSize;
    resources->fullTargetSize = fullTargetSize;
    resources->encoding = encoding;
    resources->resampling = resampling;
    resources->tiled = tiled;
    const QSize sourceSize =
        imageVirtualTexture_ ? image_.sourceSize : imageTiles_.front()->textureSize;
    const bool horizontal = sourceSize.width() != fullTargetSize.width();
    const bool vertical = sourceSize.height() != fullTargetSize.height();
    if (resampling == snow::image::ResamplingMethod::lanczos3) {
        if (tiled) {
            const double reductionX =
                static_cast<double>(sourceSize.width()) / std::max(1, fullTargetSize.width());
            const double reductionY =
                static_cast<double>(sourceSize.height()) / std::max(1, fullTargetSize.height());
            const double sharedReduction = std::min(reductionX, reductionY);
            if (horizontal && vertical && sharedReduction >= 4.0) {
                resources->mipLevel =
                    std::max(0, static_cast<int>(std::floor(std::log2(sharedReduction))) - 1);
            }
        } else if (horizontal && vertical) {
            const double reductionX =
                static_cast<double>(sourceSize.width()) / std::max(1, fullTargetSize.width());
            const double reductionY =
                static_cast<double>(sourceSize.height()) / std::max(1, fullTargetSize.height());
            const double sharedReduction = std::min(reductionX, reductionY);
            if (sharedReduction >= 4.0) {
                resources->mipLevel =
                    std::max(0, static_cast<int>(std::floor(std::log2(sharedReduction))) - 1);
            }
            if (resources->virtualSource && resources->mipLevel > 0) {
                const int largestLayerDimension =
                    std::max(resources->virtualSource->plan.layerSize.width(),
                             resources->virtualSource->plan.layerSize.height());
                resources->mipLevel = std::min(
                    resources->mipLevel,
                    static_cast<int>(std::floor(std::log2(std::max(1, largestLayerDimension)))));
            }
            const QSize mipSize(std::max(1, sourceSize.width() >> resources->mipLevel),
                                std::max(1, sourceSize.height() >> resources->mipLevel));
            const std::uint64_t horizontalFirstPixels =
                static_cast<std::uint64_t>(targetSize.width()) * mipSize.height();
            const std::uint64_t verticalFirstPixels =
                static_cast<std::uint64_t>(mipSize.width()) * targetSize.height();
            const bool horizontalFirst = horizontalFirstPixels <= verticalFirstPixels;
            resources->firstAxis = horizontalFirst ? 1 : 2;
            resources->secondAxis = horizontalFirst ? 2 : 1;
            resources->intermediateSize = horizontalFirst
                                              ? QSize(fullTargetSize.width(), mipSize.height())
                                              : QSize(mipSize.width(), fullTargetSize.height());
        } else {
            resources->firstAxis = horizontal ? 1 : vertical ? 2 : 0;
        }
    }
    if (resources->virtualSource && resources->mipLevel > 0) {
        const int largestLayerDimension =
            std::max(resources->virtualSource->plan.layerSize.width(),
                     resources->virtualSource->plan.layerSize.height());
        const int maximumLayerMip =
            static_cast<int>(std::floor(std::log2(std::max(1, largestLayerDimension))));
        resources->mipLevel = std::min(resources->mipLevel, maximumLayerMip);
    }
    if (rhi_->backend() == QRhi::OpenGLES2 && resources->firstAxis != 0 &&
        resources->secondAxis != 0) {
        resources->firstAxis = 0;
        resources->secondAxis = 0;
        resources->intermediateSize = {};
    }
    const QRhiTexture::Format format =
        encoding == PixelEncoding::LinearScRgb16F ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
    QRhiTexture::Flags flags = QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource;
    if (encoding == PixelEncoding::Srgb8)
        flags |= QRhiTexture::sRGB;
    resources->texture = rhi_->newTexture(format, targetSize, 1, flags);
    resources->uniformBuffer = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                               static_cast<quint32>(sizeof(ResizeUniformData)));
    resources->nearestSampler =
        rhi_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                         QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    resources->linearSampler =
        rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
                         QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!resources->texture->create() || !resources->uniformBuffer->create() ||
        !resources->nearestSampler->create() || !resources->linearSampler->create()) {
        return {};
    }
    if (resources->secondAxis != 0) {
        resources->secondPassUniformBuffer =
            rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                            static_cast<quint32>(sizeof(ResizeUniformData)));
        QRhiTexture::Flags intermediateFlags = QRhiTexture::RenderTarget;
        if (encoding == PixelEncoding::Srgb8)
            intermediateFlags |= QRhiTexture::sRGB;
        resources->intermediateTexture =
            rhi_->newTexture(format, resources->intermediateSize, 1, intermediateFlags);
        if (!resources->secondPassUniformBuffer->create() ||
            !resources->intermediateTexture->create())
            return {};
    }
    resources->target = rhi_->newTextureRenderTarget(
        QRhiTextureRenderTargetDescription(QRhiColorAttachment(resources->texture)));
    resources->renderPass = resources->target->newCompatibleRenderPassDescriptor();
    resources->target->setRenderPassDescriptor(resources->renderPass);
    if (resources->intermediateTexture) {
        resources->intermediateTarget =
            rhi_->newTextureRenderTarget(QRhiTextureRenderTargetDescription(
                QRhiColorAttachment(resources->intermediateTexture)));
        resources->intermediateRenderPass =
            resources->intermediateTarget->newCompatibleRenderPassDescriptor();
        resources->intermediateTarget->setRenderPassDescriptor(resources->intermediateRenderPass);
    }
    const auto createBindings = [this, &resources](QRhiSampler* sampler) {
        QRhiShaderResourceBindings* bindings = rhi_->newShaderResourceBindings();
        bindings->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0,
                                                     QRhiShaderResourceBinding::VertexStage |
                                                         QRhiShaderResourceBinding::FragmentStage,
                                                     resources->uniformBuffer),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      resources->sourceTexture, sampler),
        });
        if (!bindings->create()) {
            delete bindings;
            return static_cast<QRhiShaderResourceBindings*>(nullptr);
        }
        return bindings;
    };
    resources->nearestBindings = createBindings(resources->nearestSampler);
    resources->linearBindings = createBindings(resources->linearSampler);
    if (resources->intermediateTexture) {
        resources->intermediateBindings = rhi_->newShaderResourceBindings();
        resources->intermediateBindings->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0,
                                                     QRhiShaderResourceBinding::VertexStage |
                                                         QRhiShaderResourceBinding::FragmentStage,
                                                     resources->secondPassUniformBuffer),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      resources->intermediateTexture,
                                                      resources->linearSampler),
        });
        if (!resources->intermediateBindings->create())
            return {};
    }
    if (!resources->nearestBindings || !resources->linearBindings)
        return {};
    resources->pipeline = rhi_->newGraphicsPipeline();
    resources->pipeline->setShaderStages(
        {{QRhiShaderStage::Vertex, vertexShader}, {QRhiShaderStage::Fragment, fragmentShader}});
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({QRhiVertexInputBinding(4 * sizeof(float))});
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)),
    });
    resources->pipeline->setVertexInputLayout(inputLayout);
    resources->pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    resources->pipeline->setShaderResourceBindings(resources->linearBindings);
    resources->pipeline->setRenderPassDescriptor(resources->renderPass);
    if (resources->intermediateTexture && resources->virtualSource) {
        const QShader intermediateFragment =
            loadShader(QStringLiteral(":/shaders/edit_resize.frag.qsb"));
        if (!intermediateFragment.isValid())
            return {};
        resources->intermediatePipeline = rhi_->newGraphicsPipeline();
        resources->intermediatePipeline->setShaderStages(
            {{QRhiShaderStage::Vertex, vertexShader},
             {QRhiShaderStage::Fragment, intermediateFragment}});
        resources->intermediatePipeline->setVertexInputLayout(inputLayout);
        resources->intermediatePipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        resources->intermediatePipeline->setShaderResourceBindings(resources->intermediateBindings);
        resources->intermediatePipeline->setRenderPassDescriptor(resources->renderPass);
    }
    if (!resources->target->create() ||
        (resources->intermediateTarget && !resources->intermediateTarget->create()) ||
        !resources->pipeline->create() ||
        (resources->intermediatePipeline && !resources->intermediatePipeline->create()))
        return {};

    cachedExactRasterResources_.push_back(resources);
    return resources;
}

bool RhiImageWindow::submitPendingExactRaster(QRhiCommandBuffer* commandBuffer) {
    if (!pendingExactRaster_ || activeExactRaster_ || !commandBuffer ||
        (!imageVirtualTexture_ && imageTiles_.empty())) {
        return true;
    }
    const auto [requestId, settings] = *pendingExactRaster_;
    QElapsedTimer submissionTimer;
    submissionTimer.start();
    pendingExactRaster_.reset();
    const QSize targetSize(settings.width, settings.height);
    const QSize sourceSize =
        imageVirtualTexture_ ? image_.sourceSize : imageTiles_.front()->textureSize;
    const bool identityReadback =
        imageTiles_.size() == 1 && image_.pixelEncoding == PixelEncoding::Srgb8 &&
        imageTiles_.front()->textureSize == targetSize && imageTiles_.front()->texture;
    if (identityReadback) {
        auto job = std::make_shared<ExactRasterJob>();
        job->requestId = requestId;
        job->settings = settings;
        job->encoding = image_.pixelEncoding;
        job->color = image_.color;
        job->readbacks.resize(1);
        job->readbacks.front().pixelRect = QRect(QPoint(0, 0), targetSize);
        const QPointer<RhiImageWindow> window(this);
        const std::weak_ptr<ExactRasterJob> weakJob(job);
        job->readbacks.front().result.completed = [window, weakJob]() {
            const auto completedJob = weakJob.lock();
            if (!window || !completedJob)
                return;
            QMetaObject::invokeMethod(
                window,
                [window, completedJob]() {
                    if (window)
                        window->completeExactRaster(completedJob);
                },
                Qt::QueuedConnection);
        };
        QRhiResourceUpdateBatch* readbackBatch = rhi_->nextResourceUpdateBatch();
        readbackBatch->readBackTexture(QRhiReadbackDescription(imageTiles_.front()->texture),
                                       &job->readbacks.front().result);
        commandBuffer->resourceUpdate(readbackBatch);
        job->gpuTimer.start();
        activeExactRaster_ = std::move(job);
        emit editResizeResourceCacheResult(requestId, false);
        emit editPerformanceStageCompleted(requestId, QStringLiteral("exact.gpu_identity_readback"),
                                           submissionTimer.nsecsElapsed());
        emit editPerformanceStageCompleted(requestId, QStringLiteral("exact.gpu_pass_count"), 0);
        return true;
    }

    const int textureLimit = maximumTextureSize();
    const std::vector<QRect> tileRects = textureTilesForLimit(targetSize, textureLimit);
    if (tileRects.empty()) {
        emit gpuOperationFailed(requestId, QStringLiteral("The GPU resize tile plan is invalid."));
        return false;
    }
    const bool tiled = tileRects.size() > 1;

    struct TilePlan final {
        QRect pixelRect;
        std::shared_ptr<ExactRasterResources> resources;
    };
    std::vector<TilePlan> plans;
    plans.reserve(tileRects.size());
    bool allCacheHits = true;
    for (const QRect& rect : tileRects) {
        bool cacheHit = false;
        auto resources = exactRasterResources(rect.size(), targetSize, image_.pixelEncoding,
                                              settings.resampling, tiled, &cacheHit);
        if (!resources) {
            emit gpuOperationFailed(
                requestId, QStringLiteral("The GPU resize tile resources could not be created."));
            return false;
        }
        allCacheHits = allCacheHits && cacheHit;
        plans.push_back({rect, std::move(resources)});
    }
    emit editResizeResourceCacheResult(requestId, allCacheHits);

    auto job = std::make_shared<ExactRasterJob>();
    job->requestId = requestId;
    job->settings = settings;
    job->encoding = image_.pixelEncoding;
    job->color = image_.color;
    job->readbacks.resize(plans.size());
    job->resources.reserve(cachedExactRasterResources_.size());
    for (const auto& cached : cachedExactRasterResources_)
        job->resources.push_back(cached);

    QMatrix4x4 projection;
    projection.ortho(0.0F, 1.0F, 1.0F, 0.0F, -1.0F, 1.0F);
    const QMatrix4x4 mvp = rhi_->clipSpaceCorrMatrix() * projection;
    const QRhiCommandBuffer::VertexInput vertexInput(vertexBuffer_, 0);
    std::uint64_t passCount = 0;
    for (std::size_t index = 0; index < plans.size(); ++index) {
        const TilePlan& plan = plans[index];
        const auto& resources = plan.resources;
        ResizeUniformData uniforms;
        std::memcpy(uniforms.mvp, mvp.constData(), sizeof(uniforms.mvp));
        uniforms.sourceAndTarget[0] = static_cast<float>(sourceSize.width());
        uniforms.sourceAndTarget[1] = static_cast<float>(sourceSize.height());
        if (resources->virtualSource) {
            uniforms.scaleAndAxis[2] =
                static_cast<float>(resources->virtualSource->plan.layerSize.width());
            uniforms.scaleAndAxis[3] =
                static_cast<float>(resources->virtualSource->plan.layerSize.height());
            uniforms.options[3] = static_cast<float>(resources->virtualSource->plan.columns);
        }
        const bool identity = sourceSize == targetSize;
        uniforms.options[0] = identity                                                        ? 0.0F
                              : settings.resampling == snow::image::ResamplingMethod::nearest ? 0.0F
                              : settings.resampling == snow::image::ResamplingMethod::linear  ? 1.0F
                                                                                             : 2.0F;
        uniforms.options[1] = settings.linearRgb ? 1.0F : 0.0F;
        uniforms.options[2] = settings.premultiplyAlpha ? 1.0F : 0.0F;
        uniforms.options[3] = 0.0F;
        uniforms.backendParameters[0] = 0.0F;
        uniforms.backendParameters[1] = 0.0F;
        uniforms.backendParameters[2] = 1.0F;
        uniforms.backendParameters[3] =
            rhi_->backend() == QRhi::OpenGLES2 && resources->encoding == PixelEncoding::Srgb8
                ? 1.0F
                : 0.0F;
        uniforms.scaleAndAxis[0] = static_cast<float>(resources->mipLevel);
        uniforms.scaleAndAxis[1] = static_cast<float>(resources->firstAxis);
        if (resources->mipLevel > 0) {
            uniforms.sourceAndTarget[0] =
                static_cast<float>(std::max(1, sourceSize.width() >> resources->mipLevel));
            uniforms.sourceAndTarget[1] =
                static_cast<float>(std::max(1, sourceSize.height() >> resources->mipLevel));
        }

        const QSize firstPassSize =
            resources->intermediateTarget ? resources->intermediateSize : plan.pixelRect.size();
        uniforms.sourceAndTarget[2] = static_cast<float>(firstPassSize.width());
        uniforms.sourceAndTarget[3] = static_cast<float>(firstPassSize.height());
        if (resources->intermediateTarget) {
            uniforms.targetTile[2] = firstPassSize.width();
            uniforms.targetTile[3] = firstPassSize.height();
        } else {
            uniforms.targetTile[0] = plan.pixelRect.x();
            uniforms.targetTile[1] = plan.pixelRect.y();
            uniforms.targetTile[2] = targetSize.width();
            uniforms.targetTile[3] = targetSize.height();
        }

        QRhiResourceUpdateBatch* initial = rhi_->nextResourceUpdateBatch();
        if (vertexUploadPending_) {
            initial->uploadStaticBuffer(vertexBuffer_, kQuadVertices.data());
            vertexUploadPending_ = false;
        }
        initial->updateDynamicBuffer(resources->uniformBuffer, 0, sizeof(uniforms), &uniforms);
        QRhiTextureRenderTarget* firstTarget =
            resources->intermediateTarget ? resources->intermediateTarget : resources->target;
        commandBuffer->beginPass(firstTarget, Qt::transparent, {1.0F, 0}, initial);
        commandBuffer->setGraphicsPipeline(resources->pipeline);
        commandBuffer->setViewport(QRhiViewport(0, 0, static_cast<float>(firstPassSize.width()),
                                                static_cast<float>(firstPassSize.height())));
        commandBuffer->setShaderResources(settings.resampling ==
                                                  snow::image::ResamplingMethod::nearest
                                              ? resources->nearestBindings
                                              : resources->linearBindings);
        commandBuffer->setVertexInput(0, 1, &vertexInput);
        commandBuffer->draw(4);
        commandBuffer->endPass();
        ++passCount;

        if (resources->intermediateTarget) {
            ResizeUniformData secondUniforms = uniforms;
            secondUniforms.sourceAndTarget[0] =
                static_cast<float>(resources->intermediateSize.width());
            secondUniforms.sourceAndTarget[1] =
                static_cast<float>(resources->intermediateSize.height());
            secondUniforms.sourceAndTarget[2] = static_cast<float>(targetSize.width());
            secondUniforms.sourceAndTarget[3] = static_cast<float>(targetSize.height());
            secondUniforms.scaleAndAxis[0] = 0.0F;
            secondUniforms.scaleAndAxis[1] = static_cast<float>(resources->secondAxis);
            secondUniforms.options[3] = 0.0F;
            secondUniforms.backendParameters[0] = 0.0F;
            secondUniforms.backendParameters[1] = 0.0F;
            secondUniforms.targetTile[0] = 0;
            secondUniforms.targetTile[1] = 0;
            secondUniforms.targetTile[2] = targetSize.width();
            secondUniforms.targetTile[3] = targetSize.height();
            QRhiResourceUpdateBatch* secondUpdate = rhi_->nextResourceUpdateBatch();
            secondUpdate->updateDynamicBuffer(resources->secondPassUniformBuffer, 0,
                                              sizeof(secondUniforms), &secondUniforms);
            commandBuffer->beginPass(resources->target, Qt::transparent, {1.0F, 0}, secondUpdate);
            commandBuffer->setGraphicsPipeline(resources->intermediatePipeline
                                                   ? resources->intermediatePipeline
                                                   : resources->pipeline);
            commandBuffer->setViewport(QRhiViewport(0, 0, static_cast<float>(targetSize.width()),
                                                    static_cast<float>(targetSize.height())));
            commandBuffer->setShaderResources(resources->intermediateBindings);
            commandBuffer->setVertexInput(0, 1, &vertexInput);
            commandBuffer->draw(4);
            commandBuffer->endPass();
            ++passCount;
        }

        job->readbacks[index].pixelRect = plan.pixelRect;
        const QPointer<RhiImageWindow> window(this);
        const std::weak_ptr<ExactRasterJob> weakJob(job);
        job->readbacks[index].result.completed = [window, weakJob]() {
            const auto completedJob = weakJob.lock();
            if (!window || !completedJob)
                return;
            QMetaObject::invokeMethod(
                window,
                [window, completedJob]() {
                    if (window)
                        window->completeExactRaster(completedJob);
                },
                Qt::QueuedConnection);
        };
        QRhiResourceUpdateBatch* readbackBatch = rhi_->nextResourceUpdateBatch();
        readbackBatch->readBackTexture(QRhiReadbackDescription(resources->texture),
                                       &job->readbacks[index].result);
        commandBuffer->resourceUpdate(readbackBatch);
    }

    job->gpuTimer.start();
    activeExactRaster_ = std::move(job);
    emit editPerformanceStageCompleted(requestId, QStringLiteral("exact.gpu_submission"),
                                       submissionTimer.nsecsElapsed());
    emit editPerformanceStageCompleted(requestId, QStringLiteral("exact.gpu_pass_count"),
                                       static_cast<qint64>(passCount));
    return true;
}

// Qt signal delivery owns any queued copies of the shared GPU readback storage.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void RhiImageWindow::completeExactRaster(const std::shared_ptr<ExactRasterJob>& job) {
    if (!job || job->completedReadbacks >= job->readbacks.size())
        return;
    ++job->completedReadbacks;
    if (job->completedReadbacks < job->readbacks.size())
        return;
    if (job != activeExactRaster_) {
        const auto retired =
            std::find(retiredExactRasterJobs_.begin(), retiredExactRasterJobs_.end(), job);
        if (retired != retiredExactRasterJobs_.end())
            retiredExactRasterJobs_.erase(retired);
        if (retiredExactRasterJobs_.empty()) {
            for (const auto& tile : retiredEditSourceTiles_) {
                delete tile->shaderResources;
                delete tile->uniformBuffer;
                delete tile->texture;
            }
            retiredEditSourceTiles_.clear();
        }
        return;
    }
    emit editPerformanceStageCompleted(job->requestId, QStringLiteral("exact.gpu_readback"),
                                       job->gpuTimer.nsecsElapsed());
    GpuRasterResult readback;
    readback.pixelSize = QSize(job->settings.width, job->settings.height);
    readback.encoding = job->encoding;
    readback.color = job->color;
    readback.requestId = job->requestId;
    readback.provenance = RasterProvenance::gpu_approximate;
    const std::size_t bytesPerPixel = readback.bytesPerPixel();
    bool dimensionsValid = true;
    for (const auto& tile : job->readbacks) {
        dimensionsValid = dimensionsValid && tile.result.pixelSize == tile.pixelRect.size();
    }
    if (job->readbacks.size() == 1) {
        auto& tile = job->readbacks.front();
        readback.storage = std::make_shared<QByteArray>(std::move(tile.result.data));
        readback.rowStride = static_cast<std::size_t>(tile.pixelRect.width()) * bytesPerPixel;
    } else {
        readback.tiles.reserve(job->readbacks.size());
        for (auto& tile : job->readbacks) {
            readback.tiles.push_back(
                {std::make_shared<QByteArray>(std::move(tile.result.data)), tile.pixelRect,
                 static_cast<std::size_t>(tile.pixelRect.width()) * bytesPerPixel});
        }
    }
    const quint64 requestId = job->requestId;
    activeExactRaster_.reset();
    if (retiredExactRasterJobs_.empty()) {
        for (const auto& tile : retiredEditSourceTiles_) {
            delete tile->shaderResources;
            delete tile->uniformBuffer;
            delete tile->texture;
        }
        retiredEditSourceTiles_.clear();
    }
    if (!dimensionsValid || !readback.isValid()) {
        emit gpuOperationFailed(requestId, QStringLiteral("The GPU resize readback failed."));
    } else {
        emit editResizeReadbackReady(requestId, readback);
    }
    if (pendingExactRaster_)
        requestRender();
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

void RhiImageWindow::clearEditJobs() {
    pendingExactRaster_.reset();
    pendingVisualFrameRequestId_ = 0;
    if (activeExactRaster_) {
        retiredExactRasterJobs_.push_back(std::move(activeExactRaster_));
    }
    if (retiredExactRasterJobs_.empty()) {
        for (const auto& tile : retiredEditSourceTiles_) {
            delete tile->shaderResources;
            delete tile->uniformBuffer;
            delete tile->texture;
        }
        retiredEditSourceTiles_.clear();
    }
    releaseExactRasterResources();
}

void RhiImageWindow::releaseExactRasterResources() {
    cachedExactRasterResources_.clear();
}

bool RhiImageWindow::isComparisonHandleHit(const QPointF& position) const {
    if (!comparisonActive_ || width() <= 0 || height() <= 0)
        return false;
    const qreal split = comparisonRatio_ * width();
    const qreal horizontalDistance = std::abs(position.x() - split);
    if (horizontalDistance <= kComparisonHitHalfWidth)
        return true;

    const QPointF thumbDelta = position - QPointF(split, height() * 0.5);
    return thumbDelta.x() * thumbDelta.x() + thumbDelta.y() * thumbDelta.y() <=
           kComparisonThumbHitRadius * kComparisonThumbHitRadius;
}

void RhiImageWindow::appendComparisonVertices(std::vector<OverlayVertex>* vertices,
                                              const QSize& pixelSize) const {
    if (!vertices || !comparisonActive_ || width() <= 0 || pixelSize.isEmpty())
        return;
    const qreal pixelRatio = static_cast<qreal>(pixelSize.width()) / width();
    const qreal split = comparisonRatio_ * width();
    const float whiteScale =
        outputMode_ == OutputMode::HdrScRgb && sceneReferred_ ? sdrWhiteNits_ / 80.0F : 1.0F;
    const auto trackColor = linearColor(comparisonTrack_, whiteScale);
    const auto thumbColor = linearColor(comparisonThumb_, whiteScale);
    const auto outlineColor = linearColor(QColor(246, 248, 251, 66), whiteScale);
    const auto leadingArrowColor = linearColor(comparisonLeadingArrow_, whiteScale);
    const auto trailingArrowColor = linearColor(comparisonTrailingArrow_, whiteScale);
    const QPointF thumbCenter(split, height() * 0.5);

    const auto appendVertex = [vertices,
                               pixelRatio](const QPointF& position, const QPointF& localPosition,
                                           const std::array<float, 4>& color, qreal shapeMode) {
        OverlayVertex vertex;
        vertex.position[0] = static_cast<float>(position.x() * pixelRatio);
        vertex.position[1] = static_cast<float>(position.y() * pixelRatio);
        std::copy(color.cbegin(), color.cend(), vertex.color);
        vertex.shape[0] = static_cast<float>(localPosition.x());
        vertex.shape[1] = static_cast<float>(localPosition.y());
        vertex.shape[2] = static_cast<float>(pixelRatio);
        vertex.shape[3] = static_cast<float>(shapeMode);
        vertices->push_back(vertex);
    };
    const auto appendRect = [&appendVertex](const QRectF& rectangle,
                                            const std::array<float, 4>& color) {
        const std::array<QPointF, 6> points{rectangle.topLeft(),    rectangle.bottomLeft(),
                                            rectangle.topRight(),   rectangle.topRight(),
                                            rectangle.bottomLeft(), rectangle.bottomRight()};
        for (const QPointF& point : points) {
            appendVertex(point, {}, color, 3.0);
        }
    };
    const auto appendCircle = [&appendVertex, pixelRatio](const QPointF& center, qreal radius,
                                                          const std::array<float, 4>& color) {
        const qreal antialiasExtent = kNavigationAntialiasWidth / pixelRatio;
        const qreal extent = radius + antialiasExtent;
        const qreal localScale = kComparisonThumbRadius / radius;
        const std::array<QPointF, 6> offsets{QPointF(-extent, -extent), QPointF(-extent, extent),
                                             QPointF(extent, -extent),  QPointF(extent, -extent),
                                             QPointF(-extent, extent),  QPointF(extent, extent)};
        for (const QPointF& offset : offsets) {
            appendVertex(center + offset, offset * localScale, color, 2.0);
        }
    };
    const auto appendTriangle = [&appendVertex, pixelRatio](const QPointF& baseCenter,
                                                            qreal direction,
                                                            const std::array<float, 4>& color) {
        const qreal antialiasExtent = kNavigationAntialiasWidth / pixelRatio;
        const qreal tipX = direction * kComparisonArrowWidth;
        const QRectF localBounds(QPointF(std::min(0.0, tipX) - antialiasExtent,
                                         -kComparisonArrowHalfHeight - antialiasExtent),
                                 QPointF(std::max(0.0, tipX) + antialiasExtent,
                                         kComparisonArrowHalfHeight + antialiasExtent));
        const std::array<QPointF, 6> offsets{localBounds.topLeft(),    localBounds.bottomLeft(),
                                             localBounds.topRight(),   localBounds.topRight(),
                                             localBounds.bottomLeft(), localBounds.bottomRight()};
        const qreal shapeMode = direction < 0.0 ? 4.0 : 5.0;
        for (const QPointF& offset : offsets) {
            appendVertex(baseCenter + offset, offset, color, shapeMode);
        }
    };

    appendRect(QRectF(split - kComparisonTrackWidth * 0.5, 0.0, kComparisonTrackWidth, height()),
               trackColor);

    appendCircle(thumbCenter, kComparisonThumbRadius + kComparisonThumbOutlineWidth, outlineColor);
    appendCircle(thumbCenter, kComparisonThumbRadius, thumbColor);

    const qreal innerEdge = kComparisonTrackWidth * 0.5;
    appendTriangle(QPointF(split - innerEdge, thumbCenter.y()), -1.0, leadingArrowColor);
    appendTriangle(QPointF(split + innerEdge, thumbCenter.y()), 1.0, trailingArrowColor);
}

void RhiImageWindow::setComparisonColors(const QColor& track, const QColor& thumb,
                                         const QColor& hoverThumb, const QColor& pressedThumb,
                                         const QColor& leadingArrow, const QColor& trailingArrow) {
    comparisonTrack_ = track;
    comparisonThumb_ = thumb;
    comparisonHoverThumb_ = hoverThumb;
    comparisonPressedThumb_ = pressedThumb;
    comparisonLeadingArrow_ = leadingArrow;
    comparisonTrailingArrow_ = trailingArrow;
    requestRender();
}

bool RhiImageWindow::event(QEvent* event) {
    if (event->type() == QEvent::ContextMenu && hasImage()) {
        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        emit contextMenuRequested(contextEvent->globalPos());
        contextEvent->accept();
        return true;
    }
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto* dragEvent = static_cast<QDropEvent*>(event);
        if (!droppedLocalFile(dragEvent->mimeData()).isEmpty()) {
            dragEvent->acceptProposedAction();
            return true;
        }
    }
    if (event->type() == QEvent::Drop) {
        auto* dropEvent = static_cast<QDropEvent*>(event);
        const QString filePath = droppedLocalFile(dropEvent->mimeData());
        if (!filePath.isEmpty()) {
            emit imageDropped(filePath);
            dropEvent->acceptProposedAction();
            return true;
        }
    }
    if (event->type() == QEvent::UpdateRequest) {
        renderPending_ = false;
        render();
        return true;
    }
    if (event->type() == QEvent::PlatformSurface) {
        const auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event);
        if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            releaseSwapChain();
        }
    }
    if (event->type() == QEvent::Leave) {
        pointerWithinWindow_ = false;
        navigationHovered_ = NavigationDirection::None;
        navigationPressed_ = NavigationDirection::None;
        comparisonHandleHovered_ = false;
        if (!dragging_ && !comparisonHandlePressed_) {
            unsetCursor();
        }
        requestRender();
    }
    return QWindow::event(event);
}

void RhiImageWindow::exposeEvent(QExposeEvent* event) {
    Q_UNUSED(event)
    if (isVisible()) {
        requestRender();
        QTimer::singleShot(0, this, [this]() { requestRender(); });
    }
}

void RhiImageWindow::resizeEvent(QResizeEvent* event) {
    QWindow::resizeEvent(event);
    viewTransform_.setDevicePixelRatio(devicePixelRatio());
    viewTransform_.setViewportSize(event->size());
    updateTileSelection();
    emit zoomChanged(viewTransform_.zoom());
    requestRender();
}

void RhiImageWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && hasImage()) {
        pointerWithinWindow_ = true;
        if (isComparisonHandleHit(event->position())) {
            comparisonHandlePressed_ = true;
            comparisonHandleHovered_ = true;
            comparisonDragOffsetX_ = comparisonRatio_ * width() - event->position().x();
            setCursor(Qt::SplitHCursor);
            event->accept();
            return;
        }
        updateNavigationHover(event->position());
        if (navigationHovered_ != NavigationDirection::None) {
            navigationPressed_ = navigationHovered_;
            requestRender();
            event->accept();
            return;
        }
        if (viewTransform_.isAtMinimumZoom() && startTopLevelSystemMove(this)) {
            event->accept();
            return;
        }
        dragging_ = true;
        lastPointerPosition_ = event->position();
        setCursor(Qt::ClosedHandCursor);
    }
    QWindow::mousePressEvent(event);
}

void RhiImageWindow::mouseMoveEvent(QMouseEvent* event) {
    pointerWithinWindow_ = true;
    if (comparisonHandlePressed_) {
        setCursor(Qt::SplitHCursor);
        setComparisonRatio((event->position().x() + comparisonDragOffsetX_) / std::max(1, width()));
        event->accept();
        return;
    }
    if (comparisonActive_) {
        const bool hovered = isComparisonHandleHit(event->position());
        if (hovered != comparisonHandleHovered_) {
            comparisonHandleHovered_ = hovered;
        }
        if (!dragging_) {
            if (hovered)
                setCursor(Qt::SplitHCursor);
            else
                unsetCursor();
        }
    }
    updateNavigationHover(event->position());
    if (navigationPressed_ != NavigationDirection::None) {
        QWindow::mouseMoveEvent(event);
        return;
    }
    if (dragging_) {
        const QPointF delta = event->position() - lastPointerPosition_;
        lastPointerPosition_ = event->position();
        viewTransform_.panBy(delta);
        updateTileSelection();
        requestRender();
    }
    QWindow::mouseMoveEvent(event);
}

void RhiImageWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && comparisonHandlePressed_) {
        comparisonHandlePressed_ = false;
        comparisonDragOffsetX_ = 0.0;
        comparisonHandleHovered_ = isComparisonHandleHit(event->position());
        if (!comparisonHandleHovered_)
            unsetCursor();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && navigationPressed_ != NavigationDirection::None) {
        const NavigationDirection activated = navigationPressed_;
        navigationPressed_ = NavigationDirection::None;
        updateNavigationHover(event->position());
        requestRender();
        event->accept();
        if (navigationHovered_ == activated) {
            if (activated == NavigationDirection::Previous) {
                emit previousRequested();
            } else {
                emit nextRequested();
            }
        }
        return;
    }
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        unsetCursor();
    }
    QWindow::mouseReleaseEvent(event);
}

void RhiImageWindow::wheelEvent(QWheelEvent* event) {
    if (hasImage() && !event->angleDelta().isNull()) {
        const qreal steps = static_cast<qreal>(event->angleDelta().y()) / 120.0;
        zoomBy(std::pow(1.2, steps), event->position());
        event->accept();
        return;
    }
    QWindow::wheelEvent(event);
}

void RhiImageWindow::requestRender() {
    if (!renderPending_) {
        renderPending_ = true;
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    }
}

bool RhiImageWindow::initializeRhi() {
    if (rhi_) {
        return true;
    }
    switch (backend_) {
    case RhiBackend::d3d11: {
#if defined(Q_OS_WIN)
        QRhiD3D11InitParams params;
        rhi_.reset(QRhi::create(QRhi::D3D11, &params));
#endif
        break;
    }
    case RhiBackend::opengl: {
#if QT_CONFIG(opengl)
        QRhiGles2InitParams params;
        params.window = this;
        fallbackSurface_.reset(QRhiGles2InitParams::newFallbackSurface(params.format));
        params.fallbackSurface = fallbackSurface_.get();
        rhi_.reset(QRhi::create(QRhi::OpenGLES2, &params));
#endif
        break;
    }
    case RhiBackend::vulkan: {
#if SNOW_IMAGE_VIEWER_HAS_QT_VULKAN
        if (vulkanInstance_ && vulkanInstance_->isValid()) {
            QRhiVulkanInitParams params;
            params.inst = vulkanInstance_.get();
            params.window = this;
            rhi_.reset(QRhi::create(QRhi::Vulkan, &params));
        }
#endif
        break;
    }
    case RhiBackend::null_backend: {
        QRhiNullInitParams params;
        rhi_.reset(QRhi::create(QRhi::Null, &params));
        break;
    }
    case RhiBackend::platform_default:
        break;
    }
    if (!rhi_) {
        emit renderError(QStringLiteral("The %1 QRhi graphics device could not be created.")
                             .arg(backendLabel(backend_)));
        return false;
    }
    return true;
}

bool RhiImageWindow::initializeSwapChain() {
    if (swapChainReady_) {
        return true;
    }
    if (!initializeRhi()) {
        return false;
    }
    if (!swapChain_) {
        swapChain_ = rhi_->newSwapChain();
        swapChain_->setWindow(this);
    }

    const bool wantsHdr = image_.isValid() && image_.color.dynamicRange == DynamicRange::High &&
                          swapChain_->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear);
    outputMode_ = wantsHdr ? OutputMode::HdrScRgb : OutputMode::Sdr;
    swapChain_->setFormat(wantsHdr ? QRhiSwapChain::HDRExtendedSrgbLinear : QRhiSwapChain::SDR);
    swapChain_->setFlags(wantsHdr ? QRhiSwapChain::Flags{} : QRhiSwapChain::sRGB);

    delete renderPassDescriptor_;
    renderPassDescriptor_ = swapChain_->newCompatibleRenderPassDescriptor();
    swapChain_->setRenderPassDescriptor(renderPassDescriptor_);
    if (!swapChain_->createOrResize()) {
        emit renderError(QStringLiteral("The image swapchain could not be created."));
        return false;
    }
    swapChainReady_ = true;

    const QRhiSwapChainHdrInfo hdrInfo = swapChain_->hdrInfo();
    sdrWhiteNits_ = hdrInfo.sdrWhiteLevel > 0.0F ? hdrInfo.sdrWhiteLevel : 200.0F;
    sceneReferred_ = hdrInfo.luminanceBehavior == QRhiSwapChainHdrInfo::SceneReferred;
    if (hdrInfo.limitsType == QRhiSwapChainHdrInfo::LuminanceInNits) {
        displayPeakNits_ = std::max(100.0F, hdrInfo.limits.luminanceInNits.maxLuminance);
    } else {
        displayPeakNits_ = std::max(
            100.0F, hdrInfo.limits.colorComponentValue.maxColorComponentValue * sdrWhiteNits_);
    }
    return true;
}

bool RhiImageWindow::initializeStaticResources() {
    if (staticResourcesReady_) {
        return true;
    }
    vertexBuffer_ = rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                    static_cast<quint32>(sizeof(kQuadVertices)));
    overlayVertexBuffer_ =
        rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                        static_cast<quint32>(sizeof(OverlayVertex) * kOverlayVertexCapacity));
    overlayUniformBuffer_ = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                            static_cast<quint32>(sizeof(OverlayUniformData)));
    sampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!vertexBuffer_->create() || !overlayVertexBuffer_->create() ||
        !overlayUniformBuffer_->create() || !sampler_->create()) {
        emit renderError(QStringLiteral("The QRhi image resources could not be created."));
        return false;
    }
    vertexUploadPending_ = true;
    staticResourcesReady_ = true;
    return true;
}

bool RhiImageWindow::rebuildImageResources() {
    imageResourcesDirty_ = false;
    releaseImageResources();
    if (!image_.isValid()) {
        return true;
    }
    const int maximumTextureSize = rhi_->resourceLimit(QRhi::TextureSizeMax);
    if (maximumTextureSize <= 0) {
        emit renderError(
            QStringLiteral("The graphics device does not report a usable texture limit."));
        return false;
    }
    QRhiTexture::Flags flags;
    if (!imageIsThumbnail_) {
        flags |= QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips |
                 QRhiTexture::UsedAsTransferSource;
    }
    const QRhiTexture::Format format = image_.pixelEncoding == PixelEncoding::LinearScRgb16F
                                           ? QRhiTexture::RGBA16F
                                           : QRhiTexture::RGBA8;
    if (image_.pixelEncoding == PixelEncoding::Srgb8) {
        flags |= QRhiTexture::sRGB;
    }

    const bool virtualSourceCandidate =
        !imageIsThumbnail_ && !image_.isAnimated() && !image_.usesStoredTiles() &&
        image_.pixels.size() == image_.sourceSize && rhi_->isFeatureSupported(QRhi::TextureArrays);
    const TextureArrayTilePlan virtualPlan =
        virtualSourceCandidate
            ? textureArrayTilePlan(image_.pixels.size(), maximumTextureSize,
                                   rhi_->resourceLimit(QRhi::TextureArraySizeMax))
            : TextureArrayTilePlan{};
    if (virtualPlan.isValid() && virtualPlan.tiles.size() > 1) {
        auto source = std::make_shared<VirtualImageTexture>();
        source->plan = virtualPlan;
        source->layerPixels.reserve(virtualPlan.tiles.size());
        for (const QRect& tileRect : virtualPlan.tiles) {
            QImage layer = paddedArrayLayer(image_.pixels, tileRect, virtualPlan.layerSize);
            if (layer.isNull()) {
                emit renderError(
                    QStringLiteral("The virtual image texture layer could not be prepared."));
                return false;
            }
            source->layerPixels.push_back(std::move(layer));
        }
        source->texture = rhi_->newTextureArray(format, static_cast<int>(virtualPlan.tiles.size()),
                                                virtualPlan.layerSize, 1, flags);
        source->uniformBuffer = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                                static_cast<quint32>(sizeof(UniformData)));
        if (!source->texture->create() || !source->uniformBuffer->create()) {
            emit renderError(QStringLiteral("The virtual image texture could not be created."));
            return false;
        }
        source->shaderResources = rhi_->newShaderResourceBindings();
        source->shaderResources->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0,
                                                     QRhiShaderResourceBinding::VertexStage |
                                                         QRhiShaderResourceBinding::FragmentStage,
                                                     source->uniformBuffer),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      source->texture, sampler_),
        });
        if (!source->shaderResources->create()) {
            emit renderError(
                QStringLiteral("The virtual image texture bindings could not be created."));
            return false;
        }
        imageVirtualTexture_ = std::move(source);
        usingStoredTiles_ = false;
        loadedTileRegion_ = QRectF(QPointF(0.0, 0.0), image_.sourceSize);
#if defined(Q_OS_WIN)
        imageGpuOnlyEligible_ = true;
#else
        imageGpuOnlyEligible_ = false;
#endif
        if (!rebuildComparisonResources())
            return false;
        return rebuildPipeline();
    }

    struct PreparedTile final {
        QRectF sourceRect;
        QImage pixels;
    };
    std::vector<PreparedTile> preparedTiles;
    const bool useStoredTiles = shouldUseStoredTiles();
    QRectF loadedRegion;
    if (useStoredTiles) {
        QRectF requested = visibleSourceRect().adjusted(-512.0, -512.0, 512.0, 512.0);
        requested = requested.intersected(QRectF(QPointF(0.0, 0.0), image_.sourceSize));
        const std::vector<StoredImageTile> storedTiles =
            image_.tileStore->tilesIntersecting(requested);
        preparedTiles.reserve(storedTiles.size());
        for (const StoredImageTile& stored : storedTiles) {
            QString error;
            QImage pixels = image_.tileStore->load(stored, &error);
            if (pixels.isNull()) {
                emit renderError(error);
                return false;
            }
            const QRectF sourceRect(stored.sourceRect);
            loadedRegion = loadedRegion.isNull() ? sourceRect : loadedRegion.united(sourceRect);
            preparedTiles.push_back({sourceRect, std::move(pixels)});
        }
    } else {
        const std::vector<QRect> textureTiles =
            textureTilesForLimit(image_.pixels.size(), maximumTextureSize);
        preparedTiles.reserve(textureTiles.size());
        for (const QRect& textureRect : textureTiles) {
            QImage pixels = imageTilePixels(image_.pixels, textureRect);
            if (pixels.isNull()) {
                emit renderError(
                    QStringLiteral("The image tile could not be prepared for upload."));
                return false;
            }
            preparedTiles.push_back(
                {sourceRectForTextureTile(textureRect, image_.pixels.size(), image_.sourceSize),
                 std::move(pixels)});
        }
        loadedRegion = QRectF(QPointF(0.0, 0.0), image_.sourceSize);
    }
    if (preparedTiles.empty()) {
        emit renderError(QStringLiteral("The image could not be divided into graphics textures."));
        return false;
    }

    usingStoredTiles_ = useStoredTiles;
    loadedTileRegion_ = loadedRegion;
    imageTiles_.reserve(preparedTiles.size());
    for (PreparedTile& prepared : preparedTiles) {
        auto tile = std::make_unique<ImageTile>();
        tile->sourcePixels = std::move(prepared.pixels);
        tile->sourceRect = prepared.sourceRect;
        tile->pixels = tile->sourcePixels;
        tile->textureSize = tile->pixels.size();
        tile->mipGenerationPending = !imageIsThumbnail_;

        tile->texture = rhi_->newTexture(format, tile->pixels.size(), 1, flags);
        tile->uniformBuffer = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                              static_cast<quint32>(sizeof(UniformData)));
        if (!tile->texture->create() || !tile->uniformBuffer->create()) {
            delete tile->texture;
            delete tile->uniformBuffer;
            releaseImageResources();
            emit renderError(QStringLiteral("The image tile resources could not be created."));
            return false;
        }

        tile->shaderResources = rhi_->newShaderResourceBindings();
        tile->shaderResources->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0,
                                                     QRhiShaderResourceBinding::VertexStage |
                                                         QRhiShaderResourceBinding::FragmentStage,
                                                     tile->uniformBuffer),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      tile->texture, sampler_),
        });
        if (!tile->shaderResources->create()) {
            delete tile->shaderResources;
            delete tile->texture;
            delete tile->uniformBuffer;
            releaseImageResources();
            emit renderError(
                QStringLiteral("The image tile shader bindings could not be created."));
            return false;
        }
        imageTiles_.push_back(std::move(tile));
    }
#if defined(Q_OS_WIN)
    imageGpuOnlyEligible_ =
        !imageIsThumbnail_ && !image_.isAnimated() && !image_.usesStoredTiles() &&
        imageTiles_.size() == 1 &&
        imageTiles_.front()->sourceRect == QRectF(QPointF(0.0, 0.0), image_.sourceSize);
#else
    imageGpuOnlyEligible_ = false;
#endif
    if (!rebuildComparisonResources())
        return false;
    return rebuildPipeline();
}

bool RhiImageWindow::rebuildComparisonResources() {
    comparisonResourcesDirty_ = false;
    releaseComparisonResources();
    if (!comparisonActive_ || !comparisonImage_.isValid())
        return true;
    const int maximumTextureSize = rhi_->resourceLimit(QRhi::TextureSizeMax);
    if (maximumTextureSize <= 0 ||
        (!comparisonImage_.tileStore && comparisonImage_.pixels.isNull()))
        return false;
    if (comparisonImage_.tileStore) {
        QRectF requested = visibleSourceRect().adjusted(-512.0, -512.0, 512.0, 512.0);
        requested = requested.intersected(QRectF(QPointF(0.0, 0.0), comparisonImage_.sourceSize));
        comparisonUsingStoredTiles_ = true;
        beginComparisonTileStaging(requested);
        return true;
    }

    const std::vector<QRect> previewRects =
        textureTilesForLimit(comparisonImage_.pixels.size(), maximumTextureSize);
    for (const QRect& textureRect : previewRects) {
        QImage pixels = imageTilePixels(comparisonImage_.pixels, textureRect);
        if (!appendComparisonTile(sourceRectForTextureTile(textureRect,
                                                           comparisonImage_.pixels.size(),
                                                           comparisonImage_.sourceSize),
                                  std::move(pixels))) {
            return false;
        }
    }
    comparisonLoadedTileRegion_ = QRectF(QPointF(0.0, 0.0), comparisonImage_.sourceSize);
    return true;
}

bool RhiImageWindow::appendComparisonTile(const QRectF& sourceRect, QImage pixels) {
    if (pixels.isNull()) {
        emit renderError(QStringLiteral("The comparison image could not be prepared."));
        return false;
    }
    QRhiTexture::Flags flags = QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips |
                               QRhiTexture::UsedAsTransferSource;
    if (comparisonImage_.pixelEncoding == PixelEncoding::Srgb8)
        flags |= QRhiTexture::sRGB;
    const QRhiTexture::Format format =
        comparisonImage_.pixelEncoding == PixelEncoding::LinearScRgb16F ? QRhiTexture::RGBA16F
                                                                        : QRhiTexture::RGBA8;
    auto tile = std::make_unique<ImageTile>();
    tile->pixels = std::move(pixels);
    tile->textureSize = tile->pixels.size();
    tile->sourceRect = sourceRect;
    tile->mipGenerationPending = true;
    tile->texture = rhi_->newTexture(format, tile->textureSize, 1, flags);
    tile->uniformBuffer = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                          static_cast<quint32>(sizeof(UniformData)));
    if (!tile->texture->create() || !tile->uniformBuffer->create()) {
        delete tile->texture;
        delete tile->uniformBuffer;
        emit renderError(QStringLiteral("The comparison image resources could not be created."));
        return false;
    }
    tile->shaderResources = rhi_->newShaderResourceBindings();
    tile->shaderResources->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            tile->uniformBuffer),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  tile->texture, sampler_),
    });
    if (!tile->shaderResources->create()) {
        delete tile->shaderResources;
        delete tile->texture;
        delete tile->uniformBuffer;
        emit renderError(QStringLiteral("The comparison shader bindings could not be created."));
        return false;
    }
    comparisonTiles_.push_back(std::move(tile));
    if (!pipeline_ && !rebuildPipeline()) {
        std::unique_ptr<ImageTile> failed = std::move(comparisonTiles_.back());
        comparisonTiles_.pop_back();
        delete failed->shaderResources;
        delete failed->uniformBuffer;
        delete failed->texture;
        return false;
    }
    return true;
}

void RhiImageWindow::beginComparisonTileStaging(const QRectF& requested) {
    cancelComparisonTileStaging();
    if (!comparisonImage_.tileStore)
        return;
    const auto stored = comparisonImage_.tileStore->tilesIntersecting(requested);
    comparisonTileLoadQueue_.reserve(stored.size());
    for (const StoredImageTile& tile : stored)
        comparisonTileLoadQueue_.push_back(tile.sourceRect);
    comparisonTileLoadFailed_ = false;
    scheduleComparisonTileLoads();
}

// QFutureWatcher is owned by this window and schedules deletion when its future finishes.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void RhiImageWindow::scheduleComparisonTileLoads() {
    constexpr int kMaximumConcurrentLoads = 2;
    const quint64 generation = comparisonTileGeneration_;
    const auto store = comparisonImage_.tileStore;
    while (store && activeComparisonTileLoads_ < kMaximumConcurrentLoads &&
           nextComparisonTileLoad_ < comparisonTileLoadQueue_.size()) {
        const QRect sourceRect = comparisonTileLoadQueue_[nextComparisonTileLoad_++];
        ++activeComparisonTileLoads_;
        using LoadResult = std::pair<QImage, QString>;
        auto* watcher = new QFutureWatcher<LoadResult>(this);
        connect(watcher, &QFutureWatcher<LoadResult>::finished, this,
                [this, watcher, store, sourceRect, generation]() {
                    --activeComparisonTileLoads_;
                    LoadResult loaded = watcher->result();
                    watcher->deleteLater();
                    if (generation != comparisonTileGeneration_ ||
                        store != comparisonImage_.tileStore) {
                        scheduleComparisonTileLoads();
                        return;
                    }
                    if (loaded.first.isNull()) {
                        if (!comparisonTileLoadFailed_) {
                            comparisonTileLoadFailed_ = true;
                            emit renderError(
                                loaded.second.isEmpty()
                                    ? QStringLiteral("The comparison tile could not be loaded.")
                                    : loaded.second);
                        }
                        comparisonTileLoadQueue_.clear();
                        nextComparisonTileLoad_ = 0;
                    } else if (appendComparisonTile(QRectF(sourceRect), std::move(loaded.first))) {
                        const QRectF rect(sourceRect);
                        comparisonLoadedTileRegion_ =
                            comparisonLoadedTileRegion_.isNull()
                                ? rect
                                : comparisonLoadedTileRegion_.united(rect);
                        requestRender();
                    }
                    scheduleComparisonTileLoads();
                });
        watcher->setFuture(
            QtConcurrent::run(&comparisonTilePool_, [store, sourceRect]() -> LoadResult {
                QString error;
                QImage pixels = store->load(StoredImageTile{sourceRect}, &error);
                return {std::move(pixels), std::move(error)};
            }));
    }
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

void RhiImageWindow::cancelComparisonTileStaging() {
    ++comparisonTileGeneration_;
    comparisonTileLoadQueue_.clear();
    nextComparisonTileLoad_ = 0;
    comparisonTileLoadFailed_ = false;
}

void RhiImageWindow::keyPressEvent(QKeyEvent* event) {
    if (comparisonActive_) {
        qreal delta = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.10 : 0.01;
        if (event->key() == Qt::Key_Left) {
            setComparisonRatio(comparisonRatio_ - delta);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right) {
            setComparisonRatio(comparisonRatio_ + delta);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Home) {
            setComparisonRatio(0.0);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_End) {
            setComparisonRatio(1.0);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            emit comparisonCloseRequested();
            event->accept();
            return;
        }
    }
    QWindow::keyPressEvent(event);
}

bool RhiImageWindow::shouldUseStoredTiles() const {
    if (!image_.tileStore || !image_.sourceSize.isValid() || image_.pixels.isNull()) {
        return false;
    }
    const qreal previewScale =
        std::max(static_cast<qreal>(image_.pixels.width()) / image_.sourceSize.width(),
                 static_cast<qreal>(image_.pixels.height()) / image_.sourceSize.height());
    return viewTransform_.zoom() > previewScale * 1.25;
}

QRectF RhiImageWindow::visibleSourceRect() const {
    if (!image_.sourceSize.isValid() || width() <= 0 || height() <= 0) {
        return {};
    }
    const std::array<QPointF, 4> corners{QPointF(0.0, 0.0), QPointF(width(), 0.0),
                                         QPointF(0.0, height()), QPointF(width(), height())};
    QPointF first = viewTransform_.mapViewportToImage(corners.front());
    qreal minimumX = first.x();
    qreal maximumX = first.x();
    qreal minimumY = first.y();
    qreal maximumY = first.y();
    for (const QPointF& corner : corners) {
        const QPointF mapped = viewTransform_.mapViewportToImage(corner);
        minimumX = std::min(minimumX, mapped.x());
        maximumX = std::max(maximumX, mapped.x());
        minimumY = std::min(minimumY, mapped.y());
        maximumY = std::max(maximumY, mapped.y());
    }
    return QRectF(QPointF(minimumX, minimumY), QPointF(maximumX, maximumY))
        .normalized()
        .intersected(QRectF(QPointF(0.0, 0.0), image_.sourceSize));
}

void RhiImageWindow::updateTileSelection() {
    if (!image_.tileStore) {
        return;
    }
    const bool desiredStoredTiles = shouldUseStoredTiles();
    if (desiredStoredTiles != usingStoredTiles_ ||
        (desiredStoredTiles && !loadedTileRegion_.contains(visibleSourceRect()))) {
        imageResourcesDirty_ = true;
    }
    const bool comparisonTileStaging =
        activeComparisonTileLoads_ > 0 || nextComparisonTileLoad_ < comparisonTileLoadQueue_.size();
    const QRectF comparisonVisible =
        visibleSourceRect().intersected(QRectF(QPointF(0.0, 0.0), comparisonImage_.sourceSize));
    if (comparisonActive_ && comparisonImage_.tileStore && !comparisonTileStaging &&
        (!comparisonUsingStoredTiles_ ||
         !comparisonLoadedTileRegion_.contains(comparisonVisible))) {
        comparisonResourcesDirty_ = true;
    }
}

bool RhiImageWindow::rebuildPipeline() {
    if (imageTiles_.empty() && comparisonTiles_.empty() && !imageVirtualTexture_) {
        return true;
    }
    if ((!imageTiles_.empty() || !comparisonTiles_.empty()) && !pipeline_) {
        const QShader vertexShader = loadShader(QStringLiteral(":/shaders/image.vert.qsb"));
        const QShader fragmentShader = loadShader(QStringLiteral(":/shaders/image.frag.qsb"));
        if (!vertexShader.isValid() || !fragmentShader.isValid()) {
            emit renderError(QStringLiteral("The embedded QRhi shaders are invalid."));
            return false;
        }

        pipeline_ = rhi_->newGraphicsPipeline();
        pipeline_->setShaderStages(
            {{QRhiShaderStage::Vertex, vertexShader}, {QRhiShaderStage::Fragment, fragmentShader}});
        QRhiVertexInputLayout inputLayout;
        inputLayout.setBindings({QRhiVertexInputBinding(4 * sizeof(float))});
        inputLayout.setAttributes({
            QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
            QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)),
        });
        pipeline_->setVertexInputLayout(inputLayout);
        pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        pipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
        pipeline_->setShaderResourceBindings(!imageTiles_.empty()
                                                 ? imageTiles_.front()->shaderResources
                                                 : comparisonTiles_.front()->shaderResources);
        pipeline_->setRenderPassDescriptor(renderPassDescriptor_);
        if (!pipeline_->create()) {
            delete pipeline_;
            pipeline_ = nullptr;
            emit renderError(QStringLiteral("The QRhi image pipeline could not be created."));
            return false;
        }
    }
    if (imageVirtualTexture_ && !virtualPipeline_) {
        const QShader vertexShader = loadShader(QStringLiteral(":/shaders/image.vert.qsb"));
        const QShader fragmentShader = loadShader(QStringLiteral(":/shaders/image_array.frag.qsb"));
        if (!vertexShader.isValid() || !fragmentShader.isValid()) {
            emit renderError(QStringLiteral("The embedded virtual-image shaders are invalid."));
            return false;
        }
        virtualPipeline_ = rhi_->newGraphicsPipeline();
        virtualPipeline_->setShaderStages(
            {{QRhiShaderStage::Vertex, vertexShader}, {QRhiShaderStage::Fragment, fragmentShader}});
        QRhiVertexInputLayout inputLayout;
        inputLayout.setBindings({QRhiVertexInputBinding(4 * sizeof(float))});
        inputLayout.setAttributes({
            QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
            QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)),
        });
        virtualPipeline_->setVertexInputLayout(inputLayout);
        virtualPipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        virtualPipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
        virtualPipeline_->setShaderResourceBindings(imageVirtualTexture_->shaderResources);
        virtualPipeline_->setRenderPassDescriptor(renderPassDescriptor_);
        if (!virtualPipeline_->create()) {
            delete virtualPipeline_;
            virtualPipeline_ = nullptr;
            emit renderError(QStringLiteral("The virtual-image pipeline could not be created."));
            return false;
        }
    }
    return true;
}

bool RhiImageWindow::rebuildEditProxyPipeline() {
    if (!imageVirtualTexture_ && imageTiles_.size() != 1)
        return true;
    if (editProxyPipeline_)
        return true;
    const QShader vertexShader = loadShader(QStringLiteral(":/shaders/edit_resize.vert.qsb"));
    const QShader fragmentShader =
        loadShader(imageVirtualTexture_ ? QStringLiteral(":/shaders/edit_resize_array.frag.qsb")
                                        : QStringLiteral(":/shaders/edit_resize.frag.qsb"));
    if (!vertexShader.isValid() || !fragmentShader.isValid())
        return false;

    editProxyUniformBuffer_ = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                              static_cast<quint32>(sizeof(ResizeUniformData)));
    editProxyNearestSampler_ =
        rhi_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                         QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    editProxyLinearSampler_ =
        rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                         QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!editProxyUniformBuffer_->create() || !editProxyNearestSampler_->create() ||
        !editProxyLinearSampler_->create()) {
        releaseEditProxyResources();
        return false;
    }
    const auto createBindings = [this](QRhiSampler* sampler) {
        QRhiShaderResourceBindings* bindings = rhi_->newShaderResourceBindings();
        bindings->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0,
                                                     QRhiShaderResourceBinding::VertexStage |
                                                         QRhiShaderResourceBinding::FragmentStage,
                                                     editProxyUniformBuffer_),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                imageVirtualTexture_ ? imageVirtualTexture_->texture : imageTiles_.front()->texture,
                sampler),
        });
        if (!bindings->create()) {
            delete bindings;
            return static_cast<QRhiShaderResourceBindings*>(nullptr);
        }
        return bindings;
    };
    editProxyNearestBindings_ = createBindings(editProxyNearestSampler_);
    editProxyLinearBindings_ = createBindings(editProxyLinearSampler_);
    if (!editProxyNearestBindings_ || !editProxyLinearBindings_) {
        releaseEditProxyResources();
        return false;
    }
    editProxyPipeline_ = rhi_->newGraphicsPipeline();
    editProxyPipeline_->setShaderStages(
        {{QRhiShaderStage::Vertex, vertexShader}, {QRhiShaderStage::Fragment, fragmentShader}});
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({QRhiVertexInputBinding(4 * sizeof(float))});
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)),
    });
    editProxyPipeline_->setVertexInputLayout(inputLayout);
    editProxyPipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    editProxyPipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
    editProxyPipeline_->setShaderResourceBindings(editProxyLinearBindings_);
    editProxyPipeline_->setRenderPassDescriptor(renderPassDescriptor_);
    if (!editProxyPipeline_->create()) {
        releaseEditProxyResources();
        return false;
    }
    return true;
}

bool RhiImageWindow::rebuildNavigationPipeline() {
    if (!overlayShaderResources_) {
        overlayShaderResources_ = rhi_->newShaderResourceBindings();
        overlayShaderResources_->setBindings({QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage, overlayUniformBuffer_)});
        if (!overlayShaderResources_->create()) {
            emit renderError(
                QStringLiteral("The navigation shader bindings could not be created."));
            return false;
        }
    }
    if (overlayPipeline_) {
        return true;
    }

    const QShader vertexShader = loadShader(QStringLiteral(":/shaders/navigation.vert.qsb"));
    const QShader fragmentShader = loadShader(QStringLiteral(":/shaders/navigation.frag.qsb"));
    if (!vertexShader.isValid() || !fragmentShader.isValid()) {
        emit renderError(QStringLiteral("The embedded navigation shaders are invalid."));
        return false;
    }

    overlayPipeline_ = rhi_->newGraphicsPipeline();
    overlayPipeline_->setShaderStages(
        {{QRhiShaderStage::Vertex, vertexShader}, {QRhiShaderStage::Fragment, fragmentShader}});
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({QRhiVertexInputBinding(sizeof(OverlayVertex))});
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, 2 * sizeof(float)),
        QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float4, 6 * sizeof(float)),
    });
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    overlayPipeline_->setVertexInputLayout(inputLayout);
    overlayPipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
    overlayPipeline_->setCullMode(QRhiGraphicsPipeline::None);
    overlayPipeline_->setTargetBlends({blend});
    overlayPipeline_->setShaderResourceBindings(overlayShaderResources_);
    overlayPipeline_->setRenderPassDescriptor(renderPassDescriptor_);
    if (!overlayPipeline_->create()) {
        delete overlayPipeline_;
        overlayPipeline_ = nullptr;
        emit renderError(QStringLiteral("The navigation graphics pipeline could not be created."));
        return false;
    }
    return true;
}

void RhiImageWindow::releaseImageResources() {
    releaseEditProxyResources();
    releaseExactRasterResources();
    const auto release = [](std::vector<std::unique_ptr<ImageTile>>* tiles) {
        for (const std::unique_ptr<ImageTile>& tile : *tiles) {
            delete tile->shaderResources;
            delete tile->uniformBuffer;
            delete tile->texture;
        }
        tiles->clear();
    };
    if (activeExactRaster_) {
        for (auto& tile : imageTiles_)
            retiredEditSourceTiles_.push_back(std::move(tile));
        imageTiles_.clear();
    } else {
        release(&imageTiles_);
    }
    imageVirtualTexture_.reset();
    releaseComparisonResources();
}

void RhiImageWindow::releaseComparisonResources() {
    cancelComparisonTileStaging();
    for (const std::unique_ptr<ImageTile>& tile : comparisonTiles_) {
        delete tile->shaderResources;
        delete tile->uniformBuffer;
        delete tile->texture;
    }
    comparisonTiles_.clear();
    comparisonLoadedTileRegion_ = {};
    comparisonUsingStoredTiles_ = false;
}

void RhiImageWindow::releasePipelineResources() {
    releaseEditProxyResources();
    delete pipeline_;
    pipeline_ = nullptr;
    delete virtualPipeline_;
    virtualPipeline_ = nullptr;
    delete overlayPipeline_;
    overlayPipeline_ = nullptr;
    delete overlayShaderResources_;
    overlayShaderResources_ = nullptr;
}

void RhiImageWindow::releaseEditProxyResources() {
    delete editProxyPipeline_;
    editProxyPipeline_ = nullptr;
    delete editProxyNearestBindings_;
    editProxyNearestBindings_ = nullptr;
    delete editProxyLinearBindings_;
    editProxyLinearBindings_ = nullptr;
    delete editProxyNearestSampler_;
    editProxyNearestSampler_ = nullptr;
    delete editProxyLinearSampler_;
    editProxyLinearSampler_ = nullptr;
    delete editProxyUniformBuffer_;
    editProxyUniformBuffer_ = nullptr;
}

void RhiImageWindow::releaseStaticResources() {
    delete sampler_;
    sampler_ = nullptr;
    delete overlayUniformBuffer_;
    overlayUniformBuffer_ = nullptr;
    delete vertexBuffer_;
    vertexBuffer_ = nullptr;
    delete overlayVertexBuffer_;
    overlayVertexBuffer_ = nullptr;
    staticResourcesReady_ = false;
}

void RhiImageWindow::releaseSwapChain() {
    releasePipelineResources();
    if (swapChain_ && swapChainReady_) {
        swapChain_->destroy();
    }
    swapChainReady_ = false;
    delete renderPassDescriptor_;
    renderPassDescriptor_ = nullptr;
}

void RhiImageWindow::releaseAllResources() {
    const QString lostPath = imageGpuOnlyResident_ ? image_.filePath : QString();
    clearEditJobs();
    retiredExactRasterJobs_.clear();
    for (const auto& tile : retiredEditSourceTiles_) {
        delete tile->shaderResources;
        delete tile->uniformBuffer;
        delete tile->texture;
    }
    retiredEditSourceTiles_.clear();
    releaseSwapChain();
    releaseImageResources();
    releaseStaticResources();
    delete swapChain_;
    swapChain_ = nullptr;
    rhi_.reset();
    imageGpuOnlyResident_ = false;
    imageGpuOnlyEligible_ = false;
    imageResourcesDirty_ = image_.isValid() && image_.hasCpuPixels();
    if (!lostPath.isEmpty())
        emit gpuBackingLost(lostPath);
}

void RhiImageWindow::updateOutputMode() {
    const OutputMode desired =
        image_.color.dynamicRange == DynamicRange::High ? OutputMode::HdrScRgb : OutputMode::Sdr;
    if (swapChainReady_ && desired != outputMode_) {
        releaseSwapChain();
    }
}

RhiImageWindow::UniformData RhiImageWindow::buildUniformData(const DecodedImage& image,
                                                             const QSize& pixelSize,
                                                             const QRectF& sourceRect,
                                                             const QSize& textureSize) const {
    UniformData data;
    const float dpr = static_cast<float>(devicePixelRatio());
    QMatrix4x4 projection;
    projection.ortho(0.0F, static_cast<float>(pixelSize.width()),
                     static_cast<float>(pixelSize.height()), 0.0F, -1.0F, 1.0F);
    QMatrix4x4 model;
    model.translate(static_cast<float>((width() / 2.0 + viewTransform_.pan().x()) * dpr),
                    static_cast<float>((height() / 2.0 + viewTransform_.pan().y()) * dpr));
    model.rotate(static_cast<float>(viewTransform_.quarterTurns() * 90), 0.0F, 0.0F, 1.0F);
    model.scale(static_cast<float>(image_.sourceSize.width() * viewTransform_.zoom()),
                static_cast<float>(image_.sourceSize.height() * viewTransform_.zoom()));
    model.translate(static_cast<float>(sourceRect.x() / image_.sourceSize.width() - 0.5),
                    static_cast<float>(sourceRect.y() / image_.sourceSize.height() - 0.5));
    model.scale(static_cast<float>(sourceRect.width() / image_.sourceSize.width()),
                static_cast<float>(sourceRect.height() / image_.sourceSize.height()));
    const QMatrix4x4 mvp = rhi_->clipSpaceCorrMatrix() * projection * model;
    std::memcpy(data.mvp, mvp.constData(), sizeof(data.mvp));

    const float whiteScale =
        outputMode_ == OutputMode::HdrScRgb && sceneReferred_ ? sdrWhiteNits_ / 80.0F : 1.0F;
    const auto light = linearColor(checkerLight_, whiteScale);
    const auto dark = linearColor(checkerDark_, whiteScale);
    std::copy(light.cbegin(), light.cend(), data.checkerLight);
    std::copy(dark.cbegin(), dark.cend(), data.checkerDark);
    data.outputParameters[0] = outputMode_ == OutputMode::HdrScRgb ? 1.0F : 0.0F;
    data.outputParameters[1] = 10.0F * dpr;
    data.outputParameters[2] = whiteScale;
    data.outputParameters[3] = displayPeakNits_ / 80.0F;
    data.sourceParameters[0] = image.color.linearScaleToScRgb;
    data.sourceParameters[1] = image.color.dynamicRange == DynamicRange::High ? 1.0F : 0.0F;
    data.sourceParameters[2] = std::max(1.0F, sdrWhiteNits_ / 80.0F);
    data.sourceParameters[3] = image.pixelsPremultiplied ? 0.0F : 1.0F;
    data.textureParameters[0] = static_cast<float>(textureSize.width());
    data.textureParameters[1] = static_cast<float>(textureSize.height());
    data.textureParameters[2] = rhi_->isYUpInFramebuffer() ? -1.0F : 0.0F;
    return data;
}

RhiImageWindow::OverlayUniformData
RhiImageWindow::buildOverlayUniformData(const QSize& pixelSize) const {
    OverlayUniformData data;
    QMatrix4x4 projection;
    projection.ortho(0.0F, static_cast<float>(pixelSize.width()),
                     static_cast<float>(pixelSize.height()), 0.0F, -1.0F, 1.0F);
    const QMatrix4x4 mvp = rhi_->clipSpaceCorrMatrix() * projection;
    std::memcpy(data.mvp, mvp.constData(), sizeof(data.mvp));
    return data;
}

QColor RhiImageWindow::canvasClearColor() const {
    const float scale =
        outputMode_ == OutputMode::HdrScRgb && sceneReferred_ ? sdrWhiteNits_ / 80.0F : 1.0F;
    const auto linear = linearColor(canvasColor_, scale);
    return QColor::fromRgbF(linear[0], linear[1], linear[2], 1.0F);
}

QPointF RhiImageWindow::effectiveAnchor(const QPointF& anchor) const {
    if (!anchor.isNull()) {
        return anchor;
    }
    return QPointF(width() / 2.0, height() / 2.0);
}

void RhiImageWindow::render() {
    if (!isVisible() || size().isEmpty() || !initializeSwapChain() ||
        !initializeStaticResources()) {
        return;
    }
    if (swapChain_->currentPixelSize() != swapChain_->surfacePixelSize() &&
        !swapChain_->createOrResize()) {
        return;
    }
    if (imageResourcesDirty_ && !rebuildImageResources()) {
        imageResourcesDirty_ = false;
    }
    if (comparisonResourcesDirty_ && !rebuildComparisonResources()) {
        comparisonResourcesDirty_ = false;
        emit renderError(QStringLiteral("The comparison image resources could not be created."));
    }
    if (((!pipeline_ && (!imageTiles_.empty() || !comparisonTiles_.empty())) ||
         (!virtualPipeline_ && imageVirtualTexture_)) &&
        !rebuildPipeline()) {
        return;
    }
    if (imageGpuOnlyEligible_ && (imageVirtualTexture_ || imageTiles_.size() == 1) &&
        !editProxyPipeline_ && !rebuildEditProxyPipeline()) {
        emit renderError(QStringLiteral("The GPU edit pipeline could not be prewarmed."));
    }
    if (editProxySettings_ && !rebuildEditProxyPipeline()) {
        const quint64 requestId = pendingVisualFrameRequestId_;
        editProxySettings_.reset();
        if (requestId) {
            pendingVisualFrameRequestId_ = 0;
            emit gpuOperationFailed(requestId,
                                    QStringLiteral("The GPU edit preview could not be created."));
        }
    }
    if ((navigationHovered_ != NavigationDirection::None || comparisonActive_) &&
        !overlayPipeline_ && !rebuildNavigationPipeline()) {
        return;
    }

    const QRhi::FrameOpResult frameResult = rhi_->beginFrame(swapChain_);
    if (frameResult == QRhi::FrameOpSwapChainOutOfDate) {
        swapChainReady_ = swapChain_->createOrResize();
        requestRender();
        return;
    }
    if (frameResult == QRhi::FrameOpDeviceLost) {
        releaseAllResources();
        emit renderError(QStringLiteral("The graphics device was reset; rebuilding the viewer."));
        requestRender();
        return;
    }
    if (frameResult != QRhi::FrameOpSuccess) {
        emit renderError(QStringLiteral("QRhi could not begin an image frame."));
        return;
    }

    QRhiCommandBuffer* commandBuffer = swapChain_->currentFrameCommandBuffer();
    submitPendingExactRaster(commandBuffer);
    QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
    if (vertexUploadPending_) {
        updates->uploadStaticBuffer(vertexBuffer_, kQuadVertices.data());
        vertexUploadPending_ = false;
    }
    const QSize pixelSize = swapChain_->currentPixelSize();
    if ((pipeline_ || virtualPipeline_) && image_.isValid()) {
        const auto updateTiles = [&](const DecodedImage& image,
                                     const std::vector<std::unique_ptr<ImageTile>>& tiles) {
            for (const std::unique_ptr<ImageTile>& tile : tiles) {
                if (tile->uploadPending) {
                    updates->uploadTexture(tile->texture, tile->pixels);
                    if (tile->mipGenerationPending) {
                        updates->generateMips(tile->texture);
                        tile->mipGenerationPending = false;
                    }
                    tile->uploadPending = false;
                }
                const UniformData uniforms =
                    buildUniformData(image, pixelSize, tile->sourceRect, tile->textureSize);
                updates->updateDynamicBuffer(tile->uniformBuffer, 0, sizeof(uniforms), &uniforms);
            }
        };
        updateTiles(image_, imageTiles_);
        if (comparisonActive_ && comparisonImage_.isValid()) {
            updateTiles(comparisonImage_, comparisonTiles_);
        }
        if (imageVirtualTexture_) {
            if (imageVirtualTexture_->uploadPending) {
                for (std::size_t layer = 0; layer < imageVirtualTexture_->layerPixels.size();
                     ++layer) {
                    updates->uploadTexture(imageVirtualTexture_->texture,
                                           QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                               static_cast<int>(layer), 0,
                                               QRhiTextureSubresourceUploadDescription(
                                                   imageVirtualTexture_->layerPixels[layer]))));
                }
                if (imageVirtualTexture_->mipGenerationPending) {
                    updates->generateMips(imageVirtualTexture_->texture);
                    imageVirtualTexture_->mipGenerationPending = false;
                }
                imageVirtualTexture_->uploadPending = false;
            }
            UniformData uniforms = buildUniformData(
                image_, pixelSize, QRectF(QPointF(0.0, 0.0), image_.sourceSize), image_.sourceSize);
            uniforms.textureParameters[2] =
                static_cast<float>(imageVirtualTexture_->plan.layerSize.width());
            uniforms.textureParameters[3] =
                static_cast<float>(imageVirtualTexture_->plan.layerSize.height());
            updates->updateDynamicBuffer(imageVirtualTexture_->uniformBuffer, 0, sizeof(uniforms),
                                         &uniforms);
        }
    }
    if (editProxyPipeline_ && editProxySettings_ &&
        (imageVirtualTexture_ || imageTiles_.size() == 1)) {
        ResizeUniformData uniforms;
        const QSize sourceSize =
            imageVirtualTexture_ ? image_.sourceSize : imageTiles_.front()->textureSize;
        const QRectF sourceRect = imageVirtualTexture_
                                      ? QRectF(QPointF(0.0, 0.0), image_.sourceSize)
                                      : imageTiles_.front()->sourceRect;
        const UniformData viewerUniforms =
            buildUniformData(image_, pixelSize, sourceRect, sourceSize);
        std::memcpy(uniforms.mvp, viewerUniforms.mvp, sizeof(uniforms.mvp));
        uniforms.sourceAndTarget[0] = static_cast<float>(sourceSize.width());
        uniforms.sourceAndTarget[1] = static_cast<float>(sourceSize.height());
        uniforms.sourceAndTarget[2] = static_cast<float>(editProxySettings_->width);
        uniforms.sourceAndTarget[3] = static_cast<float>(editProxySettings_->height);
        const bool identity =
            sourceSize == QSize(editProxySettings_->width, editProxySettings_->height);
        uniforms.options[0] =
            identity                                                                   ? 0.0F
            : editProxySettings_->resampling == snow::image::ResamplingMethod::nearest ? 0.0F
            : editProxySettings_->resampling == snow::image::ResamplingMethod::linear  ? 1.0F
                                                                                       : 2.0F;
        uniforms.options[1] = editProxySettings_->linearRgb ? 1.0F : 0.0F;
        uniforms.options[2] = editProxySettings_->premultiplyAlpha ? 1.0F : 0.0F;
        uniforms.options[3] = 0.0F;
        uniforms.backendParameters[0] = 0.0F;
        uniforms.backendParameters[1] =
            !imageVirtualTexture_ && rhi_->isYUpInFramebuffer() ? 1.0F : 0.0F;
        uniforms.targetTile[2] = editProxySettings_->width;
        uniforms.targetTile[3] = editProxySettings_->height;
        if (imageVirtualTexture_) {
            uniforms.scaleAndAxis[2] =
                static_cast<float>(imageVirtualTexture_->plan.layerSize.width());
            uniforms.scaleAndAxis[3] =
                static_cast<float>(imageVirtualTexture_->plan.layerSize.height());
            uniforms.options[3] = static_cast<float>(imageVirtualTexture_->plan.columns);
        }
        updates->updateDynamicBuffer(editProxyUniformBuffer_, 0, sizeof(uniforms), &uniforms);
    }
    std::vector<OverlayVertex> overlayVertices;
    if (overlayPipeline_ &&
        (navigationHovered_ != NavigationDirection::None || comparisonActive_)) {
        overlayVertices.reserve(comparisonActive_ ? kComparisonVertexCapacity
                                                  : kNavigationVertexCapacity);
        if (comparisonActive_)
            appendComparisonVertices(&overlayVertices, pixelSize);
        else
            appendNavigationVertices(&overlayVertices, pixelSize);
        if (!overlayVertices.empty()) {
            const OverlayUniformData uniforms = buildOverlayUniformData(pixelSize);
            updates->updateDynamicBuffer(overlayUniformBuffer_, 0, sizeof(uniforms), &uniforms);
            updates->updateDynamicBuffer(
                overlayVertexBuffer_, 0,
                static_cast<quint32>(overlayVertices.size() * sizeof(OverlayVertex)),
                overlayVertices.data());
        }
    }

    commandBuffer->beginPass(swapChain_->currentFrameRenderTarget(), canvasClearColor(), {1.0F, 0},
                             updates);
    if ((imageVirtualTexture_ || !imageTiles_.empty()) && image_.isValid()) {
        commandBuffer->setGraphicsPipeline(imageVirtualTexture_ ? virtualPipeline_ : pipeline_);
        commandBuffer->setViewport(QRhiViewport(0, 0, static_cast<float>(pixelSize.width()),
                                                static_cast<float>(pixelSize.height())));
        const QRhiCommandBuffer::VertexInput vertexInput(vertexBuffer_, 0);
        commandBuffer->setVertexInput(0, 1, &vertexInput);
        const auto drawOriginal = [&]() {
            if (imageVirtualTexture_) {
                commandBuffer->setGraphicsPipeline(virtualPipeline_);
                commandBuffer->setShaderResources(imageVirtualTexture_->shaderResources);
                commandBuffer->draw(4);
            } else {
                commandBuffer->setGraphicsPipeline(pipeline_);
                for (const std::unique_ptr<ImageTile>& tile : imageTiles_) {
                    commandBuffer->setShaderResources(tile->shaderResources);
                    commandBuffer->draw(4);
                }
            }
        };
        const int splitPixel =
            std::clamp(static_cast<int>(std::lround(comparisonRatio_ * pixelSize.width())), 0,
                       pixelSize.width());
        commandBuffer->setScissor(comparisonActive_
                                      ? QRhiScissor(0, 0, splitPixel, pixelSize.height())
                                      : QRhiScissor(0, 0, pixelSize.width(), pixelSize.height()));
        drawOriginal();
        if (comparisonActive_) {
            commandBuffer->setScissor(
                QRhiScissor(splitPixel, 0, pixelSize.width() - splitPixel, pixelSize.height()));
            if (editProxyPipeline_ && editProxySettings_ &&
                (imageVirtualTexture_ || imageTiles_.size() == 1)) {
                commandBuffer->setGraphicsPipeline(editProxyPipeline_);
                commandBuffer->setShaderResources(editProxySettings_->resampling ==
                                                          snow::image::ResamplingMethod::nearest
                                                      ? editProxyNearestBindings_
                                                      : editProxyLinearBindings_);
                commandBuffer->draw(4);
            } else {
                if (comparisonImage_.isValid() && pipeline_ && !comparisonTiles_.empty()) {
                    commandBuffer->setGraphicsPipeline(pipeline_);
                    for (const std::unique_ptr<ImageTile>& tile : comparisonTiles_) {
                        commandBuffer->setShaderResources(tile->shaderResources);
                        commandBuffer->draw(4);
                    }
                } else {
                    drawOriginal();
                }
            }
        }
    }
    if (overlayPipeline_ && !overlayVertices.empty()) {
        commandBuffer->setGraphicsPipeline(overlayPipeline_);
        commandBuffer->setViewport(QRhiViewport(0, 0, static_cast<float>(pixelSize.width()),
                                                static_cast<float>(pixelSize.height())));
        commandBuffer->setShaderResources(overlayShaderResources_);
        const QRhiCommandBuffer::VertexInput vertexInput(overlayVertexBuffer_, 0);
        commandBuffer->setVertexInput(0, 1, &vertexInput);
        commandBuffer->draw(static_cast<quint32>(overlayVertices.size()));
    }
    commandBuffer->endPass();
    const QRhi::FrameOpResult endFrameResult = rhi_->endFrame(swapChain_);
    if (endFrameResult == QRhi::FrameOpDeviceLost) {
        releaseAllResources();
        requestRender();
    } else if (endFrameResult == QRhi::FrameOpSuccess) {
        // Some QRhi backends dispatch asynchronous readback completion while
        // advancing a subsequent frame. Keep frames flowing only while a job is active.
        if (activeExactRaster_)
            requestRender();
        if (pendingVisualFrameRequestId_ != 0) {
            const quint64 requestId = pendingVisualFrameRequestId_;
            pendingVisualFrameRequestId_ = 0;
            emit editVisualFrameSubmitted(requestId);
        }
        if (imageGpuOnlyEligible_ && !imageGpuOnlyResident_) {
            imageGpuOnlyResident_ = true;
            image_.pixels = {};
            for (const std::unique_ptr<ImageTile>& tile : imageTiles_) {
                tile->sourcePixels = {};
                tile->pixels = {};
            }
            if (imageVirtualTexture_)
                imageVirtualTexture_->layerPixels.clear();
            emit staticTextureResident(image_.filePath);
        }
        if (imageFrameSubmissionPending_ && image_.isValid()) {
            imageFrameSubmissionPending_ = false;
            emit imageFrameSubmitted(image_.filePath, imageIsThumbnail_);
        }
        const bool comparisonStagingComplete =
            !comparisonImage_.tileStore ||
            (!comparisonTileLoadFailed_ && activeComparisonTileLoads_ == 0 &&
             nextComparisonTileLoad_ >= comparisonTileLoadQueue_.size());
        if (comparisonFrameSubmissionPending_ && comparisonImage_.isValid() &&
            comparisonStagingComplete &&
            (!comparisonImage_.tileStore || !comparisonTiles_.empty())) {
            comparisonFrameSubmissionPending_ = false;
            emit comparisonFrameSubmitted(comparisonImage_.filePath);
        }
        if (comparisonImage_.isValid() && !comparisonImage_.isAnimated()) {
            comparisonImage_.pixels = {};
            for (const std::unique_ptr<ImageTile>& tile : comparisonTiles_) {
                if (!tile->uploadPending) {
                    tile->sourcePixels = {};
                    tile->pixels = {};
                }
            }
        }
    }
}

} // namespace snow::image_viewer
