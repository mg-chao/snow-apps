#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "icons/draw_engine_icons.h"
#include "icon_renderer.h"

#include "snow_canvas_commands.h"
#include "snow_canvas_cursor_controller.h"
#include "snow_canvas_changed_viewports.h"
#include "snow_canvas_compositor.h"
#include "snow_canvas_display_cache.h"
#include "snow_canvas_event_flow.h"
#include "snow_canvas_input_adapter.h"
#include "snow_canvas_lifecycle.h"
#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_state.h"
#include "snow_canvas_text_editor_input.h"
#include "snow_canvas_text_measurement.h"
#include "snow_canvas_filter_tile_cache.h"
#include "snow_canvas_type_conversions.h"
#include "snow_canvas_widget_display_state.h"
#include "snow_canvas_widget_input_handler.h"
#include "snow_canvas_widget_keyboard_flow.h"
#include "snow_canvas_widget_paint_frame.h"
#include "snow_canvas_widget_pointer_flow.h"
#include "snow_canvas_widget_repaint.h"
#include "snow_canvas_widget_runtime_binding.h"
#include "snow_canvas_widget_selection_hit_testing.h"
#include "snow_canvas_widget_sync.h"
#include "snow_canvas_widget_text_interaction.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QBitmap>
#include <QByteArray>
#include <QApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputMethodEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QPixmap>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QWindow>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

std::optional<QCursor> baselineCursorForCanvasTool(SnowCanvasTool tool) {
    switch (tool) {
    case SnowCanvasTool::Shape:
    case SnowCanvasTool::Arrow:
    case SnowCanvasTool::Line:
    case SnowCanvasTool::FreeDraw:
    case SnowCanvasTool::RectangleHighlight:
    case SnowCanvasTool::RectangleFilter:
    case SnowCanvasTool::PenHighlight:
    case SnowCanvasTool::PenFilter:
    case SnowCanvasTool::Spotlight:
    case SnowCanvasTool::SerialNumber:
        return QCursor(Qt::CrossCursor);
    case SnowCanvasTool::Eraser:
        return QCursor(Qt::BlankCursor);
    case SnowCanvasTool::Text:
        return QCursor(Qt::IBeamCursor);
    case SnowCanvasTool::Select:
    case SnowCanvasTool::Watermark:
    default:
        return std::nullopt;
    }
}

bool hasExposedRect(const QRegion& exposedRegion, const QRect& widgetRect) {
    return !exposedRegion.boundingRect().intersected(widgetRect).isEmpty();
}

bool hasFilter(const SnowCanvasSceneItem* sceneItems, std::uint32_t sceneItemCount) {
    if (sceneItems == nullptr) {
        return false;
    }
    for (std::uint32_t index = 0; index < sceneItemCount; ++index) {
        if (sceneItems[index].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER) {
            return true;
        }
    }
    return false;
}

std::uint64_t sceneCacheContentKey(const SceneDisplayInfo& sceneInfo,
                                   const SnowCanvasCustomRenderer* customRenderer,
                                   bool clearBackgroundEnabled, double devicePixelRatio,
                                   const QSize& widgetSize) {
    std::size_t key = 1469598103934665603ull;
    const auto hashCombine = [&key](std::size_t value) {
        key ^= value;
        key *= 1099511628211ull;
    };
    hashCombine(std::hash<double>{}(sceneInfo.surface_width));
    hashCombine(std::hash<double>{}(sceneInfo.surface_height));
    hashCombine(std::hash<double>{}(sceneInfo.camera_center_x));
    hashCombine(std::hash<double>{}(sceneInfo.camera_center_y));
    hashCombine(std::hash<double>{}(sceneInfo.camera_zoom));
    hashCombine(static_cast<std::size_t>(sceneInfo.clear_color.r) |
                (static_cast<std::size_t>(sceneInfo.clear_color.g) << 8) |
                (static_cast<std::size_t>(sceneInfo.clear_color.b) << 16) |
                (static_cast<std::size_t>(sceneInfo.clear_color.a) << 24));
    hashCombine(reinterpret_cast<std::size_t>(customRenderer));
    if (customRenderer != nullptr) {
        hashCombine(std::hash<std::uint64_t>{}(customRenderer->contentRevision()));
    }
    hashCombine(clearBackgroundEnabled ? 1u : 0u);
    hashCombine(std::hash<double>{}(devicePixelRatio));
    hashCombine(static_cast<std::size_t>(widgetSize.width()));
    hashCombine(static_cast<std::size_t>(widgetSize.height()));
    return static_cast<std::uint64_t>(key);
}

QRect filterAffectedViewRect(QRect affected, const SnowCanvasDisplayCache& cache,
                             const QRect& viewport) {
    const SceneDisplayInfo& displayInfo = cache.sceneInfo();
    const double zoom = qMax(0.0, displayInfo.camera_zoom);
    const SnowCanvasSceneItem* items = cache.sceneItems();
    for (std::uint32_t index = 0; items != nullptr && index < cache.sceneItemCount();) {
        if (items[index].kind != SNOW_SCENE_DISPLAY_ITEM_FILTER) {
            ++index;
            continue;
        }
        const QRect entering = affected;
        while (index < cache.sceneItemCount() &&
               items[index].kind == SNOW_SCENE_DISPLAY_ITEM_FILTER) {
            const SnowCanvasSceneItem& filter = items[index++];
            if (filter.opacity <= 0.0) {
                continue;
            }
            const double radius = qMax(0.0, filter.filter.sampling_radius) * zoom;
            const QRect filterBounds = snow_canvas_render_geometry::alignedRectForBounds(
                snow_canvas_render_geometry::sceneItemBounds(displayInfo, filter));
            const QRect propagated =
                entering.adjusted(-qCeil(radius), -qCeil(radius), qCeil(radius), qCeil(radius))
                    .intersected(filterBounds);
            if (!propagated.isEmpty()) {
                affected = affected.united(propagated);
            }
        }
    }
    return affected.intersected(viewport);
}

bool isPointerInput(const SnowInputEvent& input, SnowPointerEventType eventType) {
    return input.kind == SNOW_INPUT_EVENT_POINTER && input.pointer.event_type == eventType;
}

constexpr int kSerialToolbarButtonSize = 24;
constexpr int kSerialToolbarIconSize = 14;
constexpr int kSerialToolbarWidth = kSerialToolbarButtonSize * 3;
constexpr int kSerialToolbarHeight = kSerialToolbarButtonSize;
constexpr int kSerialToolbarRadius = 10;

bool focusWidgetIsWithin(const QWidget* focusWidget, const QWidget* scope) {
    for (const QWidget* candidate = focusWidget; candidate != nullptr;
         candidate = candidate->parentWidget()) {
        if (candidate == scope) {
            return true;
        }
    }
    return false;
}

enum class SerialToolbarIcon : std::uint8_t {
    Decrease,
    Increase,
    TextFields,
};

QIcon serialToolbarIcon(SerialToolbarIcon icon) {
    adqt::icons::IconRef ref;
    switch (icon) {
    case SerialToolbarIcon::Decrease:
        ref = snow::draw_engine::icons::toolbar::SerialDecrease();
        break;
    case SerialToolbarIcon::Increase:
        ref = snow::draw_engine::icons::toolbar::SerialIncrease();
        break;
    case SerialToolbarIcon::TextFields:
        ref = snow::draw_engine::icons::toolbar::SerialTextFields();
        break;
    }
    adqt::icons::IconStatePalette palette;
    palette.set(QIcon::Normal, QIcon::Off, adqt::icons::IconColors::primary(QColor(29, 27, 32)));
    palette.set(QIcon::Disabled, QIcon::Off,
                adqt::icons::IconColors::primary(QColor(73, 69, 79, 128)));
    return adqt::icons::makeIcon(ref, palette);
}

void applySerialToolbarMask(QWidget* toolbar) {
    if (toolbar == nullptr || toolbar->size().isEmpty()) {
        return;
    }

    QBitmap mask(toolbar->size());
    mask.fill(Qt::color0);

    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::color1);
    painter.drawRoundedRect(QRectF(0.0, 0.0, toolbar->width(), toolbar->height()),
                            kSerialToolbarRadius, kSerialToolbarRadius);
    toolbar->setMask(mask);
}

QToolButton* createSerialToolbarButton(QWidget* parent, const QString& objectName,
                                       SerialToolbarIcon icon, const QString& toolTip) {
    QToolButton* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setText(QString());
    button->setIcon(serialToolbarIcon(icon));
    button->setIconSize(QSize(kSerialToolbarIconSize, kSerialToolbarIconSize));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setToolTip(toolTip);
    button->setToolTipDuration(-1);
    button->setAccessibleName(toolTip);
    button->setAutoRaise(true);
    button->setCursor(Qt::ArrowCursor);
    button->setFixedSize(kSerialToolbarButtonSize, kSerialToolbarButtonSize);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

bool displayCacheHasSynced(const SnowCanvasDisplayCache& displayCache) {
    const SnowPatchCursor& cursor = displayCache.patchCursor();
    return cursor.scene_revision != 0 || cursor.decoration_revision != 0 ||
           cursor.overlay_revision != 0;
}

void applyWatermarkConfig(WatermarkDisplayInfo& displayInfo,
                          const SnowCanvasWatermarkConfig& config) {
    displayInfo.watermark_color = SnowColorRgba8{
        static_cast<std::uint8_t>(config.color.red()),
        static_cast<std::uint8_t>(config.color.green()),
        static_cast<std::uint8_t>(config.color.blue()),
        static_cast<std::uint8_t>(config.color.alpha()),
    };
    const QByteArray text = config.text.trimmed().toUtf8().left(SNOW_WATERMARK_TEXT_CAPACITY);
    displayInfo.watermark_text.fill(0);
    std::copy(text.begin(), text.end(), displayInfo.watermark_text.begin());
    displayInfo.watermark_text_len = static_cast<std::uint16_t>(text.size());
    displayInfo.watermark_font_size = config.fontSize;
    const QByteArray family =
        config.fontFamily.trimmed().toUtf8().left(SNOW_WATERMARK_FONT_FAMILY_CAPACITY);
    displayInfo.watermark_font_family.fill(0);
    std::copy(family.begin(), family.end(), displayInfo.watermark_font_family.begin());
    displayInfo.watermark_font_family_len = static_cast<std::uint16_t>(family.size());
    displayInfo.watermark_angle = config.angle;
    displayInfo.watermark_gap = config.gap;
    displayInfo.watermark_opacity = config.opacity;
}

bool surfaceSizeMatches(const SnowCanvasDisplayCache& displayCache, const QSize& size) {
    if (!displayCacheHasSynced(displayCache)) {
        return false;
    }

    const SceneDisplayInfo& sceneInfo = displayCache.sceneInfo();
    return sceneInfo.surface_width == static_cast<double>(size.width()) &&
           sceneInfo.surface_height == static_cast<double>(size.height());
}

bool cameraMatches(const SnowCanvasDisplayCache& displayCache, double centerX, double centerY,
                   double zoom) {
    if (!displayCacheHasSynced(displayCache)) {
        return false;
    }

    const SceneDisplayInfo& sceneInfo = displayCache.sceneInfo();
    return sceneInfo.camera_center_x == centerX && sceneInfo.camera_center_y == centerY &&
           sceneInfo.camera_zoom == zoom;
}

qreal painterDevicePixelRatio(const QPainter& painter, const QWidget& widget) {
    if (painter.device() != nullptr && painter.device()->devicePixelRatioF() > 0.0) {
        return painter.device()->devicePixelRatioF();
    }
    return widget.devicePixelRatioF() > 0.0 ? widget.devicePixelRatioF() : 1.0;
}

QTransform canvasToViewTransform(const SceneDisplayInfo& sceneInfo) {
    const qreal zoom = sceneInfo.camera_zoom > 0.0 ? sceneInfo.camera_zoom : 1.0;
    return QTransform(zoom, 0.0, 0.0, zoom,
                      sceneInfo.surface_width / 2.0 - sceneInfo.camera_center_x * zoom,
                      sceneInfo.surface_height / 2.0 - sceneInfo.camera_center_y * zoom);
}

void accept(QKeyEvent& event) {
    event.accept();
}

} // namespace

struct SnowCanvasWidget::Impl : public snow_canvas_runtime::Client {
    explicit Impl(SnowCanvasWidget& widget)
        : widget(widget), cursorController(widget), inputHandler(widget, cursorController),
          textInteraction(widget, cursorController) {}

    Impl(SnowCanvasWidget& widget, SnowCanvasRuntime& runtime)
        : widget(widget), runtimeBinding(runtime), cursorController(widget),
          inputHandler(widget, cursorController), textInteraction(widget, cursorController) {}

    void initializeWidget();
    void initializeViewport();
    std::uint64_t runtimeViewportId() const override;
    void detachRuntimeForReplacement() override;
    void detachRuntime();
    void attachRuntime(SnowRuntime runtime) override;
    void detachRuntimeOwner(SnowCanvasRuntime* owner) override;
    void clearRenderState() override;
    void clearRetainedDisplayState();
    bool hasViewport() const;
    void syncAfterEngineMutation() override;
    void syncAfterEngineMutation(bool emitSignals);
    void refreshStateFromEngine(bool emitSignals) override;
    void syncChangedViewports(SnowChangedViewportList changedViewports);
    bool setSurfaceSizeAndSync(const QSize& size, bool emitSignals);
    bool setCameraAndSync(double centerX, double centerY, double zoom);
    void shutdown();
    void initializeSerialNumberToolbar();
    void refreshSerialNumberToolbar();

    SnowCanvasTool canvasTool() const;
    bool setCanvasTool(SnowCanvasTool tool);
    void setCursorForLayer(SnowCanvasCursorLayer layer, const QCursor& cursor);
    void clearCursorForLayer(SnowCanvasCursorLayer layer);
    SnowCanvasStyleToolbarState canvasStyleToolbarState() const;
    SnowCanvasSerialNumberToolbarState serialNumberToolbarState() const;
    SnowCanvasWatermarkConfig canvasWatermarkConfig() const;
    bool setCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config);
    void previewCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config);
    void applyPendingWatermarkPreview();
    int watermarkPreviewFrameInterval() const;
    void updateWatermarkPresentationInfo() const;
    SnowCanvasSpotlightConfig canvasSpotlightConfig() const;
    bool setCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config);
    void previewCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config);
    void applyPendingSpotlightPreview();
    bool setCanvasShapeStylePatch(const SnowCanvasShapeStyle& style, quint32 properties,
                                  SnowCanvasShapeKind kind);
    bool setCanvasFilterStyle(const SnowCanvasFilterStyle& style, quint32 properties);
    bool setCanvasTextStyle(const SnowCanvasTextStyle& style);
    bool setCanvasSerialNumberStyle(const SnowCanvasSerialNumberStyle& style);
    SnowCanvasHistoryState canvasHistoryState() const;
    SnowCanvasSnapConfig canvasSnapConfig() const;
    bool setCanvasSnapConfig(const SnowCanvasSnapConfig& config);
    SnowCanvasGridConfig canvasGridConfig() const;
    bool setCanvasGridConfig(const SnowCanvasGridConfig& config);
    bool interactionEnabled() const;
    void setInteractionEnabled(bool enabled);
    bool wheelZoomEnabled() const;
    void setWheelZoomEnabled(bool enabled);
    bool canvasContentVisible() const;
    void setCanvasContentVisible(bool visible);
    bool clearBackgroundEnabled() const;
    void setClearBackgroundEnabled(bool enabled);
    bool showDirtyRects() const;
    void setShowDirtyRects(bool show);
    std::uint64_t viewportId() const;
    bool setViewportCamera(double centerX, double centerY, double zoom);
    bool hasWatermarkRenderArea() const;
    QRectF watermarkRenderArea() const;
    void setWatermarkRenderArea(const QRectF& canvasRect);
    void clearWatermarkRenderArea();
    void setDecorationRenderAreas(const SnowCanvasDecorationRenderAreas& areas);
    QRectF watermarkViewRenderArea() const;
    QRegion watermarkViewRenderRegion() const;
    bool hasSpotlightRenderArea() const;
    QRectF spotlightRenderArea() const;
    void setSpotlightRenderArea(const QRectF& canvasRect);
    void clearSpotlightRenderArea();
    QRectF spotlightViewRenderArea() const;
    QRegion spotlightViewRenderRegion() const;
    bool watermarkEffectivelyVisible() const;
    bool spotlightEffectivelyVisible() const;
    SnowCanvasCustomRenderer* customRenderer() const;
    void setCustomRenderer(SnowCanvasCustomRenderer* renderer);
    QTransform canvasToViewTransform() const;
    QRect viewRectForCanvasRect(const QRectF& canvasRect, int paddingPx) const;

    bool undo();
    bool redo();
    bool deleteSelected();
    bool duplicateSelected(const QPointF& offset);
    bool reorderSelected(SnowCanvasSelectionOrder order);
    bool setSelectedOpacity(double opacity);
    bool adjustSelectedSerialNumbers(qint64 delta);
    bool createSerialNumberText();
    bool resetEditingState();
    bool cancelActiveTextEditing();
    bool hasActiveTextEditing() const;
    SnowCanvasWidgetTextInteraction::CommitResult commitText(bool refocusWidget = true,
                                                             bool restoreExistingSelection = true);
    void applyTextCommitResult(SnowCanvasWidgetTextInteraction::CommitResult& result,
                               bool restoreExistingSelection);
    void applyTextEditorEventResult(SnowCanvasWidgetTextInteraction::EditorEventResult& result);
    bool restoreFinishedExistingTextSelection(
        const SnowCanvasWidgetTextInteraction::FinishedExistingEdit& edit);
    bool beginText(const QPointF& viewPosition, bool allowCreate);
    bool beginSelectedText(const QPointF& viewPosition, bool requireSerialBoundText);

    void paintEvent(QPaintEvent* event);
    bool paint(QPainter& painter, const QRegion& exposedRegion);
    snow_canvas_compositor::Frame buildPaintFrame() const;
    SnowCanvasRenderContext renderContext(QPainter& painter, const QRegion& exposedRegion) const;
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) const;
    void renderAfterCanvas(QPainter& painter, const SnowCanvasRenderContext& context) const;
    bool handleMousePress(QMouseEvent* event);
    bool handleMouseDoubleClick(QMouseEvent* event);
    bool handleMouseMove(QMouseEvent* event);
    bool handleMouseRelease(QMouseEvent* event);
    void beginMiddleClickTracking(const QPointF& position);
    void updateMiddleClickTracking(const QPointF& position);
    bool finishMiddleClickTracking();
    void cancelMiddleClickTracking();
    bool handleEnter(QEnterEvent* event);
    bool handleLeave(QEvent* event);
    bool handleWheel(QWheelEvent* event);
    bool handleKeyPress(QKeyEvent* event);
    bool handleKeyRelease(QKeyEvent* event);
    bool handleInputMethodEvent(QInputMethodEvent* event);
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const;
    void handleFocusOut();
    void handleResize(const QSize& size);
    void beginTextStylePopupInteraction();
    void endTextStylePopupInteraction(QWidget* focusScope);
    void clearTextStylePopupInteraction();
    void handleApplicationStateChanged(Qt::ApplicationState state);

  private:
    SnowCanvasWidgetInputHandler::Context inputHandlerContext();
    void applyInputProcessResult(const SnowCanvasWidgetInputHandler::ProcessResult& result);
    bool applyActiveTextResizeMeasurementIfNeeded();
    bool prepareActiveTextResizeMeasurementForPointerUp(const SnowInputEvent& input);
    bool dispatchInput(QEvent* event, const SnowInputEvent& input);
    bool processInput(const SnowInputEvent& input);
    bool queueLiveStrokeMove(QMouseEvent* event, const SnowInputEvent& input,
                             bool preserveEverySample);
    void flushLiveStrokeMoves();
    bool queueEraserMove(QMouseEvent* event, const SnowInputEvent& input);
    void flushEraserMove();

    bool m_wheelZoomEnabled = true;
    bool applyMutationResult(const snow_canvas_commands::MutationResult& result);
    bool applyPairedMutationResult(const snow_canvas_commands::PairedMutationResult& result);
    void emitChangedStateSignals(const snow_canvas_state::Changes& changes);
    void applyCanvasToolCursor(SnowCanvasTool tool);
    void refocusWidget();

    SnowCanvasWidget& widget;
    SnowCanvasWidgetRuntimeBinding runtimeBinding;
    SnowCanvasCursorController cursorController;
    SnowCanvasWidgetInputHandler inputHandler;
    SnowCanvasWidgetDisplayState displayState;
    QWidget* serialNumberToolbar = nullptr;
    QToolButton* serialNumberDecreaseButton = nullptr;
    QToolButton* serialNumberIncreaseButton = nullptr;
    QToolButton* serialNumberCreateTextButton = nullptr;
    bool dirtyRectsVisible = false;
    bool canvasContentIsVisible = true;
    bool canvasClearBackgroundEnabled = true;
    bool suppressNextTextToolCreate = false;
    bool restoredSelectionForNextTextToolPress = false;
    SnowCanvasTool requestedCanvasTool = SnowCanvasTool::Select;
    SnowCanvasCustomRenderer* installedCustomRenderer = nullptr;
    std::optional<QRectF> configuredWatermarkRenderArea;
    std::optional<QRectF> configuredSpotlightRenderArea;
    snow_canvas_filter_render::RenderWorkspace filterWorkspace;
    snow_canvas_pen_mask::PenMaskAtlas penMaskAtlas;
    SnowCanvasWidgetTextInteraction textInteraction;
    std::optional<QPointF> middleClickPressPosition;
    bool middleClickCandidate = false;
    std::vector<SnowInputEvent> pendingLiveStrokeMoves;
    bool liveStrokeMoveFlushScheduled = false;
    quint64 liveStrokeMoveFlushGeneration = 0;
    bool pendingLiveStrokePreservesEverySample = false;
    std::optional<SnowInputEvent> pendingEraserMove;
    bool eraserMoveFlushScheduled = false;
    std::optional<SnowCanvasWatermarkConfig> pendingWatermarkPreview;
    std::optional<SnowCanvasWatermarkConfig> watermarkPreview;
    QTimer watermarkPreviewTimer;
    bool watermarkPreviewScheduled = false;
    mutable WatermarkDisplayInfo watermarkPresentationInfo;
    std::optional<SnowCanvasSpotlightConfig> spotlightPreview;
    std::optional<SnowCanvasSpotlightConfig> pendingSpotlightPreview;
    QTimer spotlightPreviewTimer;
    bool spotlightPreviewScheduled = false;
    mutable SpotlightDisplayInfo spotlightPresentationInfo;
    int textStylePopupInteractionDepth = 0;
    quint64 textStylePopupFocusRestoreGeneration = 0;
    QPointer<QWidget> textStylePopupFocusScope;
};

SnowCanvasWidget::SnowCanvasWidget(QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>(*this)) {
    m_impl->initializeWidget();
    m_impl->initializeViewport();
}

SnowCanvasWidget::SnowCanvasWidget(SnowCanvasRuntime& runtime, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>(*this, runtime)) {
    m_impl->initializeWidget();
    m_impl->initializeViewport();
}

SnowCanvasWidgetInputHandler::Context SnowCanvasWidget::Impl::inputHandlerContext() {
    SnowCanvasWidgetInputHandler::Context context;
    context.hasViewport = hasViewport();
    context.processInput = [this](const SnowInputEvent& input) {
        return snow_canvas_commands::processInput(runtimeBinding.engine(),
                                                  runtimeBinding.viewportHandle(), input);
    };
    context.processInputBatch = [this](const std::vector<SnowInputEvent>& inputs) {
        return snow_canvas_commands::processPointerMoveBatch(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(), inputs);
    };
    return context;
}

void SnowCanvasWidget::Impl::applyInputProcessResult(
    const SnowCanvasWidgetInputHandler::ProcessResult& result) {
    if (!result.success) {
        return;
    }
    syncChangedViewports(result.changedViewports.get());
}

bool SnowCanvasWidget::Impl::applyActiveTextResizeMeasurementIfNeeded() {
    SnowCanvasWidgetTextInteraction::ActiveResizeMeasurementResult result =
        textInteraction.applyActiveResizeMeasurementIfNeeded(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache());
    if (!result.success) {
        return false;
    }
    syncChangedViewports(result.changedViewports.get());
    return true;
}

bool SnowCanvasWidget::Impl::prepareActiveTextResizeMeasurementForPointerUp(
    const SnowInputEvent& input) {
    if (!isPointerInput(input, SNOW_POINTER_EVENT_UP)) {
        return true;
    }

    const SnowCanvasWidgetTextInteraction::ActiveResizeMeasurementState state =
        textInteraction.activeResizeMeasurementState(runtimeBinding.engine(),
                                                     runtimeBinding.viewportHandle());
    if (!state.success) {
        return false;
    }
    if (!state.active) {
        return true;
    }

    SnowInputEvent moveInput = input;
    moveInput.pointer.event_type = SNOW_POINTER_EVENT_MOVE;
    moveInput.pointer.button = SNOW_POINTER_BUTTON_NONE;
    moveInput.pointer.buttons |= 0b0000'0001;
    SnowCanvasWidgetInputHandler::ProcessResult moveResult =
        inputHandler.process(inputHandlerContext(), moveInput);
    applyInputProcessResult(moveResult);
    if (!moveResult.success) {
        return false;
    }
    return applyActiveTextResizeMeasurementIfNeeded();
}

bool SnowCanvasWidget::Impl::dispatchInput(QEvent* event, const SnowInputEvent& input) {
    if (!isPointerInput(input, SNOW_POINTER_EVENT_MOVE)) {
        flushLiveStrokeMoves();
        flushEraserMove();
    }
    if (!prepareActiveTextResizeMeasurementForPointerUp(input)) {
        return false;
    }
    SnowCanvasWidgetInputHandler::DispatchResult result =
        inputHandler.dispatch(event, inputHandlerContext(), input);
    applyInputProcessResult(result.process);
    if (result.process.success && isPointerInput(input, SNOW_POINTER_EVENT_MOVE)) {
        if (!applyActiveTextResizeMeasurementIfNeeded()) {
            return false;
        }
    }
    return result.accepted;
}

bool SnowCanvasWidget::Impl::processInput(const SnowInputEvent& input) {
    if (!prepareActiveTextResizeMeasurementForPointerUp(input)) {
        return false;
    }
    SnowCanvasWidgetInputHandler::ProcessResult result =
        inputHandler.process(inputHandlerContext(), input);
    applyInputProcessResult(result);
    if (result.success && isPointerInput(input, SNOW_POINTER_EVENT_MOVE)) {
        if (!applyActiveTextResizeMeasurementIfNeeded()) {
            return false;
        }
    }
    return result.success;
}

void SnowCanvasWidget::Impl::initializeWidget() {
    widget.setAttribute(Qt::WA_OpaquePaintEvent);
    widget.setAttribute(Qt::WA_InputMethodEnabled, false);
    widget.setMouseTracking(true);
    widget.setFocusPolicy(Qt::StrongFocus);
    watermarkPreviewTimer.setSingleShot(true);
    QObject::connect(&watermarkPreviewTimer, &QTimer::timeout, &widget, [this]() {
        if (!pendingWatermarkPreview.has_value()) {
            watermarkPreviewScheduled = false;
            return;
        }
        applyPendingWatermarkPreview();
        watermarkPreviewTimer.start(watermarkPreviewFrameInterval());
    });
    spotlightPreviewTimer.setSingleShot(true);
    QObject::connect(&spotlightPreviewTimer, &QTimer::timeout, &widget, [this]() {
        if (!pendingSpotlightPreview.has_value()) {
            spotlightPreviewScheduled = false;
            return;
        }
        applyPendingSpotlightPreview();
        if (pendingSpotlightPreview.has_value()) {
            spotlightPreviewTimer.start(watermarkPreviewFrameInterval());
        } else {
            spotlightPreviewScheduled = false;
        }
    });
    initializeSerialNumberToolbar();
    QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, &widget,
                     [this](Qt::ApplicationState state) { handleApplicationStateChanged(state); });
}

void SnowCanvasWidget::Impl::initializeViewport() {
    displayState.resetRetainedState();
    runtimeBinding.registerClient(*this);
    runtimeBinding.initializeAttachment(displayState);
    setSurfaceSizeAndSync(widget.size(), false);
    refreshSerialNumberToolbar();
}

void SnowCanvasWidget::Impl::initializeSerialNumberToolbar() {
    serialNumberToolbar = new QWidget(&widget);
    serialNumberToolbar->setObjectName(QStringLiteral("snowSerialNumberToolbar"));
    serialNumberToolbar->setAttribute(Qt::WA_StyledBackground, true);
    serialNumberToolbar->setFixedSize(kSerialToolbarWidth, kSerialToolbarHeight);
    applySerialToolbarMask(serialNumberToolbar);
    serialNumberToolbar->setStyleSheet(
        QStringLiteral("#snowSerialNumberToolbar {"
                       "background: white;"
                       "border: none;"
                       "border-radius: 10px;"
                       "}"
                       "#snowSerialNumberToolbar QToolButton {"
                       "border: 0;"
                       "border-radius: 0;"
                       "padding: 0;"
                       "}"
                       "#snowSerialNumberToolbarDecreaseButton {"
                       "border-top-left-radius: 10px;"
                       "border-bottom-left-radius: 10px;"
                       "}"
                       "#snowSerialNumberToolbarCreateTextButton {"
                       "border-top-right-radius: 10px;"
                       "border-bottom-right-radius: 10px;"
                       "}"
                       "#snowSerialNumberToolbar QToolButton:hover:enabled {"
                       "background: rgba(30, 30, 30, 18);"
                       "}"
                       "#snowSerialNumberToolbar QToolButton:pressed:enabled {"
                       "background: rgba(30, 30, 30, 32);"
                       "}"
                       "#snowSerialNumberToolbar QToolButton:disabled {"
                       "color: rgba(30, 30, 30, 76);"
                       "}"));

    auto* shadow = new QGraphicsDropShadowEffect(serialNumberToolbar);
    shadow->setBlurRadius(10.0);
    shadow->setOffset(0.0, 2.0);
    shadow->setColor(QColor(0, 0, 0, 48));
    serialNumberToolbar->setGraphicsEffect(shadow);

    auto* layout = new QHBoxLayout(serialNumberToolbar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    serialNumberDecreaseButton = createSerialToolbarButton(
        serialNumberToolbar, QStringLiteral("snowSerialNumberToolbarDecreaseButton"),
        SerialToolbarIcon::Decrease, QStringLiteral("Decrease"));
    serialNumberIncreaseButton = createSerialToolbarButton(
        serialNumberToolbar, QStringLiteral("snowSerialNumberToolbarIncreaseButton"),
        SerialToolbarIcon::Increase, QStringLiteral("Increase"));
    serialNumberCreateTextButton = createSerialToolbarButton(
        serialNumberToolbar, QStringLiteral("snowSerialNumberToolbarCreateTextButton"),
        SerialToolbarIcon::TextFields, QStringLiteral("Create Text"));

    layout->addWidget(serialNumberDecreaseButton);
    layout->addWidget(serialNumberIncreaseButton);
    layout->addWidget(serialNumberCreateTextButton);

    QObject::connect(serialNumberDecreaseButton, &QToolButton::clicked, &widget, [this]() {
        adjustSelectedSerialNumbers(-1);
        refreshSerialNumberToolbar();
    });
    QObject::connect(serialNumberIncreaseButton, &QToolButton::clicked, &widget, [this]() {
        adjustSelectedSerialNumbers(1);
        refreshSerialNumberToolbar();
    });
    QObject::connect(serialNumberCreateTextButton, &QToolButton::clicked, &widget, [this]() {
        createSerialNumberText();
        refreshSerialNumberToolbar();
    });

    serialNumberToolbar->hide();
}

void SnowCanvasWidget::Impl::refreshSerialNumberToolbar() {
    if (serialNumberToolbar == nullptr) {
        return;
    }

    const SnowCanvasSerialNumberToolbarState state = serialNumberToolbarState();
    if (!canvasContentIsVisible || !state.visible) {
        serialNumberToolbar->hide();
        return;
    }

    const bool enabled = interactionEnabled();
    serialNumberDecreaseButton->setEnabled(enabled && state.canDecrease);
    serialNumberIncreaseButton->setEnabled(enabled && state.canIncrease);
    serialNumberCreateTextButton->setEnabled(enabled && state.canCreateText);
    serialNumberToolbar->setGeometry(state.geometry.toAlignedRect());
    applySerialToolbarMask(serialNumberToolbar);
    serialNumberToolbar->raise();
    serialNumberToolbar->show();
}

void SnowCanvasWidget::Impl::shutdown() {
    if (pendingLiveStrokePreservesEverySample) {
        flushLiveStrokeMoves();
    }
    clearTextStylePopupInteraction();
    detachRuntime();
    runtimeBinding.unregisterClient(*this);
}

SnowCanvasWidget::~SnowCanvasWidget() {
    m_impl->shutdown();
}

SnowCanvasTool SnowCanvasWidget::Impl::canvasTool() const {
    const SnowCanvasTool engineTool =
        snow_canvas_types::toCanvasTool(displayState.snapshot().activeTool);
    return requestedCanvasTool == SnowCanvasTool::Line && engineTool == SnowCanvasTool::Arrow
               ? SnowCanvasTool::Line
               : engineTool;
}

SnowCanvasTool SnowCanvasWidget::canvasTool() const {
    return m_impl->canvasTool();
}

bool SnowCanvasWidget::Impl::setCanvasTool(SnowCanvasTool tool) {
    const SnowCanvasTool previousTool = canvasTool();
    if (previousTool != tool) {
        if (pendingLiveStrokePreservesEverySample) {
            flushLiveStrokeMoves();
        } else {
            pendingLiveStrokeMoves.clear();
        }
        pendingEraserMove.reset();
    }
    if (textInteraction.isActive() && previousTool != tool) {
        clearTextStylePopupInteraction();
        commitText(false, false);
    }

    const bool engineToolChanged =
        snow_canvas_types::toEngineTool(previousTool) != snow_canvas_types::toEngineTool(tool);
    if (!applyMutationResult(snow_canvas_commands::setActiveTool(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(),
            snow_canvas_types::toEngineTool(tool)))) {
        return false;
    }

    requestedCanvasTool = tool;
    if (previousTool != tool && !engineToolChanged) {
        emit widget.activeToolChanged();
    }
    applyCanvasToolCursor(tool);
    return true;
}

bool SnowCanvasWidget::setCanvasTool(SnowCanvasTool tool) {
    return m_impl->setCanvasTool(tool);
}

void SnowCanvasWidget::Impl::setCursorForLayer(SnowCanvasCursorLayer layer,
                                               const QCursor& cursor) {
    cursorController.setCursor(layer, cursor);
}

void SnowCanvasWidget::setCursorForLayer(SnowCanvasCursorLayer layer, const QCursor& cursor) {
    m_impl->setCursorForLayer(layer, cursor);
}

void SnowCanvasWidget::Impl::clearCursorForLayer(SnowCanvasCursorLayer layer) {
    cursorController.clearCursor(layer);
}

void SnowCanvasWidget::clearCursorForLayer(SnowCanvasCursorLayer layer) {
    m_impl->clearCursorForLayer(layer);
}

SnowCanvasStyleToolbarState SnowCanvasWidget::Impl::canvasStyleToolbarState() const {
    SnowCanvasStyleToolbarState state =
        snow_canvas_types::toCanvasStyleToolbarState(displayState.snapshot().styleToolbarState);
    if (textInteraction.isActive()) {
        state.textStyle = snow_canvas_types::toCanvasTextStyle(textInteraction.currentTextStyle());
        state.textStyleMixed = 0;
    }
    return state;
}

SnowCanvasStyleToolbarState SnowCanvasWidget::canvasStyleToolbarState() const {
    return m_impl->canvasStyleToolbarState();
}

SnowCanvasSerialNumberToolbarState SnowCanvasWidget::Impl::serialNumberToolbarState() const {
    SnowCanvasSerialNumberToolbarState state;
    if (!hasViewport()) {
        return state;
    }

    SnowSerialNumberToolbarState engineState{};
    if (snow_viewport_get_serial_number_toolbar_state(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(), &engineState) != SNOW_OK) {
        return state;
    }

    state.visible = engineState.visible != 0;
    state.geometry =
        QRectF(engineState.left, engineState.top, engineState.width, engineState.height);
    state.canDecrease = engineState.can_decrease != 0;
    state.canIncrease = engineState.can_increase != 0;
    state.canCreateText = engineState.can_create_text != 0;
    return state;
}

SnowCanvasSerialNumberToolbarState SnowCanvasWidget::serialNumberToolbarState() const {
    return m_impl->serialNumberToolbarState();
}

bool SnowCanvasWidget::Impl::setCanvasShapeStylePatch(const SnowCanvasShapeStyle& style,
                                                      quint32 properties,
                                                      SnowCanvasShapeKind kind) {
    return applyMutationResult(snow_canvas_commands::setShapeStylePatch(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        snow_canvas_types::toEngineShapeStyle(style), properties,
        snow_canvas_types::toEngineShapeKind(kind)));
}

SnowCanvasWatermarkConfig SnowCanvasWidget::Impl::canvasWatermarkConfig() const {
    SnowWatermarkConfig engineConfig{};
    SnowCanvasWatermarkConfig config;
    if (snow_viewport_get_watermark_config(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                           &engineConfig) != SNOW_OK) {
        return config;
    }
    config.color = QColor(engineConfig.color.r, engineConfig.color.g, engineConfig.color.b,
                          engineConfig.color.a);
    config.text =
        QString::fromUtf8(engineConfig.text_utf8, static_cast<int>(engineConfig.text_utf8_len));
    config.fontSize = engineConfig.font_size;
    config.fontFamily = QString::fromUtf8(engineConfig.font_family_utf8,
                                          static_cast<int>(engineConfig.font_family_utf8_len));
    config.angle = engineConfig.angle;
    config.gap = engineConfig.gap;
    config.opacity = engineConfig.opacity;
    return config;
}

SnowCanvasWatermarkConfig SnowCanvasWidget::canvasWatermarkConfig() const {
    return m_impl->canvasWatermarkConfig();
}

bool SnowCanvasWidget::Impl::setCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    // A persistent update owns the display state from this point onward. Any
    // queued transient value must not be allowed to overwrite the synced store.
    const QRegion previousPreviewRegion = watermarkViewRenderRegion();
    pendingWatermarkPreview.reset();
    watermarkPreview.reset();
    watermarkPreviewTimer.stop();
    watermarkPreviewScheduled = false;
    SnowWatermarkConfig engineConfig{};
    engineConfig.color = SnowColorRgba8{static_cast<std::uint8_t>(config.color.red()),
                                        static_cast<std::uint8_t>(config.color.green()),
                                        static_cast<std::uint8_t>(config.color.blue()),
                                        static_cast<std::uint8_t>(config.color.alpha())};
    const QByteArray text = config.text.trimmed().toUtf8().left(SNOW_WATERMARK_TEXT_CAPACITY);
    engineConfig.text_utf8_len = static_cast<std::uint32_t>(text.size());
    std::copy(text.begin(), text.end(), engineConfig.text_utf8);
    engineConfig.font_size = config.fontSize;
    const QByteArray family = config.fontFamily.trimmed().toUtf8().left(128);
    engineConfig.font_family_utf8_len = static_cast<std::uint32_t>(family.size());
    std::copy(family.begin(), family.end(), engineConfig.font_family_utf8);
    engineConfig.angle = config.angle;
    engineConfig.gap = config.gap;
    engineConfig.opacity = config.opacity;
    const bool applied = applyMutationResult(snow_canvas_commands::setWatermarkConfig(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), engineConfig));
    if (applied && !previousPreviewRegion.isEmpty()) {
        widget.update(previousPreviewRegion);
    }
    return applied;
}

bool SnowCanvasWidget::setCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    return m_impl->setCanvasWatermarkConfig(config);
}

void SnowCanvasWidget::Impl::previewCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    pendingWatermarkPreview = config;
    if (watermarkPreviewScheduled) {
        return;
    }
    watermarkPreviewScheduled = true;
    applyPendingWatermarkPreview();
    watermarkPreviewTimer.start(watermarkPreviewFrameInterval());
}

void SnowCanvasWidget::Impl::applyPendingWatermarkPreview() {
    if (!pendingWatermarkPreview.has_value()) {
        return;
    }
    const QRegion previousRegion = watermarkViewRenderRegion();
    watermarkPreview = *pendingWatermarkPreview;
    pendingWatermarkPreview.reset();
    updateWatermarkPresentationInfo();
    const QRegion changedRegion = previousRegion.united(watermarkViewRenderRegion());
    if (!changedRegion.isEmpty()) {
        widget.update(changedRegion);
    }
    emit widget.watermarkPreviewApplied();
}

int SnowCanvasWidget::Impl::watermarkPreviewFrameInterval() const {
    QScreen* screen = widget.windowHandle() != nullptr ? widget.windowHandle()->screen()
                                                       : QGuiApplication::primaryScreen();
    const qreal refreshRate = screen != nullptr ? screen->refreshRate() : 60.0;
    const int interval =
        refreshRate > 1.0 && std::isfinite(refreshRate) ? qRound(1000.0 / refreshRate) : 16;
    return std::clamp(interval, 4, 16);
}

void SnowCanvasWidget::previewCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    m_impl->previewCanvasWatermarkConfig(config);
}

SnowCanvasSpotlightConfig SnowCanvasWidget::Impl::canvasSpotlightConfig() const {
    SnowSpotlightConfig config{};
    if (snow_viewport_get_spotlight_config(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                           &config) != SNOW_OK) {
        return {};
    }
    return snow_canvas_types::toCanvasSpotlightConfig(config);
}

SnowCanvasSpotlightConfig SnowCanvasWidget::canvasSpotlightConfig() const {
    return m_impl->canvasSpotlightConfig();
}

bool SnowCanvasWidget::Impl::setCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    const QRegion previousPreviewRegion = spotlightViewRenderRegion();
    pendingSpotlightPreview.reset();
    spotlightPreview.reset();
    spotlightPreviewTimer.stop();
    spotlightPreviewScheduled = false;
    const bool applied = applyMutationResult(snow_canvas_commands::setSpotlightConfig(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        snow_canvas_types::toEngineSpotlightConfig(config)));
    if (applied && !previousPreviewRegion.isEmpty()) {
        widget.update(previousPreviewRegion);
    }
    return applied;
}

bool SnowCanvasWidget::setCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    return m_impl->setCanvasSpotlightConfig(config);
}

void SnowCanvasWidget::Impl::previewCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    const SnowCanvasSpotlightConfig current =
        spotlightPreview.has_value() ? *spotlightPreview : canvasSpotlightConfig();
    if (config == current) {
        pendingSpotlightPreview.reset();
        // Keep an already scheduled refresh boundary alive so a later value
        // in the same burst remains coalesced instead of applying immediately.
        if (!spotlightPreviewScheduled) {
            spotlightPreviewTimer.stop();
        }
        return;
    }
    if (pendingSpotlightPreview.has_value() && *pendingSpotlightPreview == config) {
        return;
    }
    pendingSpotlightPreview = config;
    if (spotlightPreviewScheduled) {
        return;
    }
    spotlightPreviewScheduled = true;
    applyPendingSpotlightPreview();
    spotlightPreviewTimer.start(watermarkPreviewFrameInterval());
}

void SnowCanvasWidget::Impl::applyPendingSpotlightPreview() {
    if (!pendingSpotlightPreview.has_value()) {
        return;
    }
    const QRegion previousRegion = spotlightViewRenderRegion();
    spotlightPreview = *pendingSpotlightPreview;
    pendingSpotlightPreview.reset();
    const QRegion changedRegion = previousRegion.united(spotlightViewRenderRegion());
    if (!changedRegion.isEmpty()) {
        widget.update(changedRegion);
    }
    emit widget.spotlightPreviewApplied();
}

void SnowCanvasWidget::previewCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    m_impl->previewCanvasSpotlightConfig(config);
}

bool SnowCanvasWidget::setCanvasShapeStylePatch(const SnowCanvasShapeStyle& style,
                                                quint32 properties, SnowCanvasShapeKind kind) {
    return m_impl->setCanvasShapeStylePatch(style, properties, kind);
}

bool SnowCanvasWidget::Impl::setCanvasFilterStyle(const SnowCanvasFilterStyle& style,
                                                  quint32 properties) {
    SnowFilterStyle engineStyle{};
    engineStyle.filter_type = static_cast<SnowFilterType>(style.type);
    engineStyle.strength = style.strength;
    engineStyle.opacity = style.opacity;
    engineStyle.stroke_width = style.strokeWidth;
    return applyMutationResult(snow_canvas_commands::setFilterStyle(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), engineStyle, properties));
}

bool SnowCanvasWidget::setCanvasFilterStyle(const SnowCanvasFilterStyle& style,
                                            quint32 properties) {
    return m_impl->setCanvasFilterStyle(style, properties);
}

bool SnowCanvasWidget::Impl::setCanvasTextStyle(const SnowCanvasTextStyle& style) {
    const SnowTextStyle engineStyle = snow_canvas_types::toEngineTextStyle(style);
    SnowCanvasWidgetTextInteraction::StyleChangeResult result =
        textInteraction.applyTextStyle(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                       displayState.displayCache(), engineStyle);
    if (!result.success) {
        return false;
    }

    syncChangedViewports(result.changedViewports.get());
    if (result.toolbarStateChanged) {
        emit widget.styleToolbarStateChanged();
    }
    return true;
}

bool SnowCanvasWidget::setCanvasTextStyle(const SnowCanvasTextStyle& style) {
    return m_impl->setCanvasTextStyle(style);
}

bool SnowCanvasWidget::Impl::setCanvasSerialNumberStyle(const SnowCanvasSerialNumberStyle& style) {
    return applyMutationResult(snow_canvas_commands::setSerialNumberStyle(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        snow_canvas_types::toEngineSerialNumberStyle(style)));
}

bool SnowCanvasWidget::setCanvasSerialNumberStyle(const SnowCanvasSerialNumberStyle& style) {
    return m_impl->setCanvasSerialNumberStyle(style);
}

SnowCanvasHistoryState SnowCanvasWidget::Impl::canvasHistoryState() const {
    return snow_canvas_types::toCanvasHistoryState(displayState.snapshot().historyState);
}

SnowCanvasHistoryState SnowCanvasWidget::canvasHistoryState() const {
    return m_impl->canvasHistoryState();
}

SnowCanvasSnapConfig SnowCanvasWidget::Impl::canvasSnapConfig() const {
    return snow_canvas_types::toCanvasSnapConfig(displayState.snapshot().snapConfig);
}

SnowCanvasSnapConfig SnowCanvasWidget::canvasSnapConfig() const {
    return m_impl->canvasSnapConfig();
}

bool SnowCanvasWidget::Impl::setCanvasSnapConfig(const SnowCanvasSnapConfig& config) {
    return applyPairedMutationResult(snow_canvas_commands::setSnapConfig(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        snow_canvas_types::toEngineSnapConfig(config)));
}

bool SnowCanvasWidget::setCanvasSnapConfig(const SnowCanvasSnapConfig& config) {
    return m_impl->setCanvasSnapConfig(config);
}

SnowCanvasGridConfig SnowCanvasWidget::Impl::canvasGridConfig() const {
    return snow_canvas_types::toCanvasGridConfig(displayState.snapshot().gridConfig);
}

SnowCanvasGridConfig SnowCanvasWidget::canvasGridConfig() const {
    return m_impl->canvasGridConfig();
}

bool SnowCanvasWidget::Impl::setCanvasGridConfig(const SnowCanvasGridConfig& config) {
    return applyPairedMutationResult(snow_canvas_commands::setGridConfig(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        snow_canvas_types::toEngineGridConfig(config)));
}

bool SnowCanvasWidget::setCanvasGridConfig(const SnowCanvasGridConfig& config) {
    return m_impl->setCanvasGridConfig(config);
}

bool SnowCanvasWidget::Impl::interactionEnabled() const {
    return inputHandler.interactionEnabled();
}

bool SnowCanvasWidget::interactionEnabled() const {
    return m_impl->interactionEnabled();
}

void SnowCanvasWidget::Impl::setInteractionEnabled(bool enabled) {
    if (!enabled) {
        ++liveStrokeMoveFlushGeneration;
        liveStrokeMoveFlushScheduled = false;
        pendingLiveStrokeMoves.clear();
        pendingLiveStrokePreservesEverySample = false;
        pendingEraserMove.reset();
    }
    inputHandler.setInteractionEnabled(enabled);
    refreshSerialNumberToolbar();
}

void SnowCanvasWidget::setInteractionEnabled(bool enabled) {
    m_impl->setInteractionEnabled(enabled);
}

bool SnowCanvasWidget::Impl::wheelZoomEnabled() const {
    return m_wheelZoomEnabled;
}

bool SnowCanvasWidget::wheelZoomEnabled() const {
    return m_impl->wheelZoomEnabled();
}

void SnowCanvasWidget::Impl::setWheelZoomEnabled(bool enabled) {
    m_wheelZoomEnabled = enabled;
}

void SnowCanvasWidget::setWheelZoomEnabled(bool enabled) {
    m_impl->setWheelZoomEnabled(enabled);
}

bool SnowCanvasWidget::Impl::canvasContentVisible() const {
    return canvasContentIsVisible;
}

bool SnowCanvasWidget::canvasContentVisible() const {
    return m_impl->canvasContentVisible();
}

void SnowCanvasWidget::Impl::setCanvasContentVisible(bool visible) {
    if (canvasContentIsVisible == visible) {
        return;
    }
    canvasContentIsVisible = visible;
    snow_canvas_filter_tile_cache::invalidateNamespace(&widget);
    refreshSerialNumberToolbar();
    widget.update();
}

void SnowCanvasWidget::setCanvasContentVisible(bool visible) {
    m_impl->setCanvasContentVisible(visible);
}

bool SnowCanvasWidget::Impl::clearBackgroundEnabled() const {
    return canvasClearBackgroundEnabled;
}

bool SnowCanvasWidget::clearBackgroundEnabled() const {
    return m_impl->clearBackgroundEnabled();
}

void SnowCanvasWidget::Impl::setClearBackgroundEnabled(bool enabled) {
    if (canvasClearBackgroundEnabled == enabled) {
        return;
    }
    canvasClearBackgroundEnabled = enabled;
    widget.update();
}

void SnowCanvasWidget::setClearBackgroundEnabled(bool enabled) {
    m_impl->setClearBackgroundEnabled(enabled);
}

bool SnowCanvasWidget::Impl::showDirtyRects() const {
    return dirtyRectsVisible;
}

bool SnowCanvasWidget::showDirtyRects() const {
    return m_impl->showDirtyRects();
}

std::uint64_t SnowCanvasWidget::Impl::viewportId() const {
    return runtimeBinding.viewportId();
}

std::uint64_t SnowCanvasWidget::Impl::runtimeViewportId() const {
    return viewportId();
}

std::uint64_t SnowCanvasWidget::viewportId() const {
    return m_impl->viewportId();
}

bool SnowCanvasWidget::Impl::setViewportCamera(double centerX, double centerY, double zoom) {
    return setCameraAndSync(centerX, centerY, zoom);
}

bool SnowCanvasWidget::setViewportCamera(double centerX, double centerY, double zoom) {
    return m_impl->setViewportCamera(centerX, centerY, zoom);
}

bool SnowCanvasWidget::Impl::hasWatermarkRenderArea() const {
    return configuredWatermarkRenderArea.has_value();
}

bool SnowCanvasWidget::hasWatermarkRenderArea() const {
    return m_impl->hasWatermarkRenderArea();
}

QRectF SnowCanvasWidget::Impl::watermarkRenderArea() const {
    return configuredWatermarkRenderArea.value_or(QRectF());
}

QRectF SnowCanvasWidget::watermarkRenderArea() const {
    return m_impl->watermarkRenderArea();
}

QRectF SnowCanvasWidget::Impl::watermarkViewRenderArea() const {
    if (!configuredWatermarkRenderArea.has_value()) {
        return QRectF(widget.rect());
    }
    const QRectF canvasArea = configuredWatermarkRenderArea->normalized();
    if (!canvasArea.isValid() || canvasArea.isEmpty()) {
        return {};
    }
    const SnowCanvasDisplayCache& cache = displayState.displayCache();
    if (!displayCacheHasSynced(cache)) {
        return {};
    }
    return ::canvasToViewTransform(cache.sceneInfo()).mapRect(canvasArea);
}

QRegion SnowCanvasWidget::Impl::watermarkViewRenderRegion() const {
    if (!configuredWatermarkRenderArea.has_value()) {
        return QRegion(widget.rect());
    }
    const QRectF viewArea = watermarkViewRenderArea();
    return viewArea.isValid() && !viewArea.isEmpty()
               ? QRegion(viewArea.toAlignedRect().intersected(widget.rect()))
               : QRegion();
}

bool SnowCanvasWidget::Impl::watermarkEffectivelyVisible() const {
    updateWatermarkPresentationInfo();
    const WatermarkDisplayInfo& info = watermarkPresentationInfo;
    return info.watermark_text_len != 0 && info.watermark_color.a != 0 &&
           std::isfinite(info.watermark_font_size) && info.watermark_font_size > 0.0 &&
           std::isfinite(info.watermark_opacity) && info.watermark_opacity > 0.0;
}

bool SnowCanvasWidget::Impl::spotlightEffectivelyVisible() const {
    spotlightPresentationInfo = displayState.displayCache().spotlightInfo();
    if (spotlightPreview.has_value()) {
        spotlightPresentationInfo.color = snow_canvas_types::toEngineColor(spotlightPreview->color);
        spotlightPresentationInfo.opacity = spotlightPreview->opacity;
    }
    const SpotlightDisplayInfo& info = spotlightPresentationInfo;
    return info.active && info.color.a != 0 && std::isfinite(info.opacity) && info.opacity > 0.0;
}

void SnowCanvasWidget::Impl::setDecorationRenderAreas(
    const SnowCanvasDecorationRenderAreas& areas) {
    const QRegion previousWatermark = watermarkViewRenderRegion();
    const QRegion previousSpotlight = spotlightViewRenderRegion();
    const bool watermarkWasVisible = watermarkEffectivelyVisible();
    const bool spotlightWasVisible = spotlightEffectivelyVisible();

    configuredWatermarkRenderArea = areas.watermark.has_value()
                                        ? std::optional<QRectF>(areas.watermark->normalized())
                                        : std::nullopt;
    configuredSpotlightRenderArea = areas.spotlight.has_value()
                                        ? std::optional<QRectF>(areas.spotlight->normalized())
                                        : std::nullopt;

    const QRegion nextWatermark = watermarkViewRenderRegion();
    const QRegion nextSpotlight = spotlightViewRenderRegion();
    const bool watermarkIsVisible = watermarkEffectivelyVisible();
    const bool spotlightIsVisible = spotlightEffectivelyVisible();
    QRegion changed;
    if (watermarkWasVisible || watermarkIsVisible) {
        changed += previousWatermark.united(nextWatermark);
    }
    if (spotlightWasVisible || spotlightIsVisible) {
        changed += previousSpotlight.xored(nextSpotlight);
    }
    if (!changed.isEmpty()) {
        widget.update(changed);
    }
}

void SnowCanvasWidget::Impl::setWatermarkRenderArea(const QRectF& canvasRect) {
    setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        std::optional<QRectF>(canvasRect.normalized()),
        configuredSpotlightRenderArea,
    });
}

void SnowCanvasWidget::setWatermarkRenderArea(const QRectF& canvasRect) {
    m_impl->setWatermarkRenderArea(canvasRect);
}

void SnowCanvasWidget::setDecorationRenderAreas(const SnowCanvasDecorationRenderAreas& areas) {
    m_impl->setDecorationRenderAreas(areas);
}

void SnowCanvasWidget::Impl::clearWatermarkRenderArea() {
    setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        std::nullopt,
        configuredSpotlightRenderArea,
    });
}

void SnowCanvasWidget::clearWatermarkRenderArea() {
    m_impl->clearWatermarkRenderArea();
}

bool SnowCanvasWidget::Impl::hasSpotlightRenderArea() const {
    return configuredSpotlightRenderArea.has_value();
}

bool SnowCanvasWidget::hasSpotlightRenderArea() const {
    return m_impl->hasSpotlightRenderArea();
}

QRectF SnowCanvasWidget::Impl::spotlightRenderArea() const {
    return configuredSpotlightRenderArea.value_or(QRectF());
}

QRectF SnowCanvasWidget::spotlightRenderArea() const {
    return m_impl->spotlightRenderArea();
}

QRectF SnowCanvasWidget::Impl::spotlightViewRenderArea() const {
    if (!configuredSpotlightRenderArea.has_value()) {
        return QRectF(widget.rect());
    }
    const QRectF canvasArea = configuredSpotlightRenderArea->normalized();
    if (!canvasArea.isValid() || canvasArea.isEmpty()) {
        return {};
    }
    const SnowCanvasDisplayCache& cache = displayState.displayCache();
    return displayCacheHasSynced(cache)
               ? ::canvasToViewTransform(cache.sceneInfo()).mapRect(canvasArea)
               : QRectF();
}

QRegion SnowCanvasWidget::Impl::spotlightViewRenderRegion() const {
    const QRectF area = spotlightViewRenderArea();
    return area.isValid() && !area.isEmpty()
               ? QRegion(area.toAlignedRect().intersected(widget.rect()))
               : QRegion();
}

void SnowCanvasWidget::Impl::setSpotlightRenderArea(const QRectF& canvasRect) {
    setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        configuredWatermarkRenderArea,
        std::optional<QRectF>(canvasRect.normalized()),
    });
}

void SnowCanvasWidget::setSpotlightRenderArea(const QRectF& canvasRect) {
    m_impl->setSpotlightRenderArea(canvasRect);
}

void SnowCanvasWidget::Impl::clearSpotlightRenderArea() {
    setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        configuredWatermarkRenderArea,
        std::nullopt,
    });
}

void SnowCanvasWidget::clearSpotlightRenderArea() {
    m_impl->clearSpotlightRenderArea();
}

SnowCanvasCustomRenderer* SnowCanvasWidget::Impl::customRenderer() const {
    return installedCustomRenderer;
}

SnowCanvasCustomRenderer* SnowCanvasWidget::customRenderer() const {
    return m_impl->customRenderer();
}

void SnowCanvasWidget::Impl::setCustomRenderer(SnowCanvasCustomRenderer* renderer) {
    if (installedCustomRenderer == renderer) {
        return;
    }
    installedCustomRenderer = renderer;
    snow_canvas_filter_tile_cache::invalidateNamespace(&widget);
    widget.update();
}

void SnowCanvasWidget::setCustomRenderer(SnowCanvasCustomRenderer* renderer) {
    m_impl->setCustomRenderer(renderer);
}

QTransform SnowCanvasWidget::Impl::canvasToViewTransform() const {
    const SnowCanvasDisplayCache& cache = displayState.displayCache();
    return displayCacheHasSynced(cache) ? ::canvasToViewTransform(cache.sceneInfo())
                                        : QTransform(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0);
}

QTransform SnowCanvasWidget::canvasToViewTransform() const {
    return m_impl->canvasToViewTransform();
}

QRect SnowCanvasWidget::Impl::viewRectForCanvasRect(const QRectF& canvasRect, int paddingPx) const {
    const QRectF normalized = canvasRect.normalized();
    if (!normalized.isValid() || normalized.isEmpty()) {
        return {};
    }

    const SnowCanvasDisplayCache& cache = displayState.displayCache();
    if (!displayCacheHasSynced(cache)) {
        return {};
    }
    return snow_canvas_render_geometry::alignedRectForBounds(
        ::canvasToViewTransform(cache.sceneInfo()).mapRect(normalized), paddingPx);
}

QRect SnowCanvasWidget::viewRectForCanvasRect(const QRectF& canvasRect, int paddingPx) const {
    return m_impl->viewRectForCanvasRect(canvasRect, paddingPx);
}

void SnowCanvasWidget::Impl::detachRuntime() {
    clearRetainedDisplayState();
    runtimeBinding.detachAttachment();
}

void SnowCanvasWidget::Impl::detachRuntimeForReplacement() {
    detachRuntime();
}

void SnowCanvasWidget::Impl::detachRuntimeOwner(SnowCanvasRuntime* owner) {
    if (!runtimeBinding.isRuntimeOwner(owner)) {
        return;
    }

    clearRetainedDisplayState();
    runtimeBinding.detachRuntimeOwner(owner);
}

void SnowCanvasWidget::Impl::attachRuntime(SnowRuntime runtime) {
    runtimeBinding.attachRuntime(runtime, displayState);
    setSurfaceSizeAndSync(widget.size(), false);
    refreshSerialNumberToolbar();
    widget.update();
}

void SnowCanvasWidget::Impl::clearRetainedDisplayState() {
    if (pendingLiveStrokePreservesEverySample && hasViewport()) {
        flushLiveStrokeMoves();
    }
    clearTextStylePopupInteraction();
    pendingLiveStrokeMoves.clear();
    pendingLiveStrokePreservesEverySample = false;
    pendingEraserMove.reset();
    pendingWatermarkPreview.reset();
    watermarkPreview.reset();
    watermarkPreviewTimer.stop();
    watermarkPreviewScheduled = false;
    pendingSpotlightPreview.reset();
    spotlightPreview.reset();
    spotlightPreviewTimer.stop();
    spotlightPreviewScheduled = false;
    const bool textSessionWasActive = textInteraction.isActive();
    const SnowCanvasWidgetTextInteraction::CancelResult cancelResult = textInteraction.cancel(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache());
    if (cancelResult.sessionEnded) {
        syncChangedViewports(cancelResult.changedViewports.get());
    }
    displayState.resetRetainedState();
    if (textSessionWasActive) {
        emit widget.styleToolbarStateChanged();
    }
    inputHandler.clearTransientState();
    refreshSerialNumberToolbar();
    clearRenderState();
}

void SnowCanvasWidget::Impl::clearRenderState() {
    snow_canvas_filter_tile_cache::invalidateNamespace(&widget);
    filterWorkspace.clear();
    penMaskAtlas.clear();
}

bool SnowCanvasWidget::Impl::hasViewport() const {
    return runtimeBinding.hasViewport();
}

bool SnowCanvasWidget::Impl::undo() {
    return applyMutationResult(snow_canvas_commands::undo(runtimeBinding.engine()));
}

bool SnowCanvasWidget::undo() {
    return m_impl->undo();
}

bool SnowCanvasWidget::Impl::redo() {
    return applyMutationResult(snow_canvas_commands::redo(runtimeBinding.engine()));
}

bool SnowCanvasWidget::redo() {
    return m_impl->redo();
}

bool SnowCanvasWidget::Impl::deleteSelected() {
    return applyMutationResult(snow_canvas_commands::deleteSelected(
        runtimeBinding.engine(), runtimeBinding.viewportHandle()));
}

bool SnowCanvasWidget::deleteSelected() {
    return m_impl->deleteSelected();
}

bool SnowCanvasWidget::Impl::duplicateSelected(const QPointF& offset) {
    return applyMutationResult(snow_canvas_commands::duplicateSelected(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), offset.x(), offset.y()));
}

bool SnowCanvasWidget::duplicateSelected(const QPointF& offset) {
    return m_impl->duplicateSelected(offset);
}

bool SnowCanvasWidget::Impl::reorderSelected(SnowCanvasSelectionOrder order) {
    return applyMutationResult(snow_canvas_commands::reorderSelected(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        static_cast<std::uint32_t>(order)));
}

bool SnowCanvasWidget::reorderSelected(SnowCanvasSelectionOrder order) {
    return m_impl->reorderSelected(order);
}

bool SnowCanvasWidget::Impl::setSelectedOpacity(double opacity) {
    return applyMutationResult(snow_canvas_commands::setSelectedOpacity(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), opacity));
}

bool SnowCanvasWidget::setSelectedOpacity(double opacity) {
    return m_impl->setSelectedOpacity(opacity);
}

bool SnowCanvasWidget::Impl::adjustSelectedSerialNumbers(qint64 delta) {
    return applyMutationResult(snow_canvas_commands::adjustSelectedSerialNumbers(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        static_cast<std::int64_t>(delta)));
}

bool SnowCanvasWidget::adjustSelectedSerialNumbers(qint64 delta) {
    return m_impl->adjustSelectedSerialNumbers(delta);
}

bool SnowCanvasWidget::Impl::createSerialNumberText() {
    const SnowStyleToolbarState& styleState = displayState.snapshot().styleToolbarState;
    SnowCanvasWidgetTextInteraction::SerialTextCreationResult result =
        textInteraction.createSerialNumberText(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache(),
            styleState.text_style, styleState.serial_number_style);
    if (!result.success) {
        return false;
    }

    syncChangedViewports(result.firstChangedViewports.get());
    syncChangedViewports(result.secondChangedViewports.get());
    if (result.shouldRefocus) {
        refocusWidget();
    }
    return true;
}

bool SnowCanvasWidget::createSerialNumberText() {
    return m_impl->createSerialNumberText();
}

bool SnowCanvasWidget::Impl::resetEditingState() {
    if (pendingLiveStrokePreservesEverySample) {
        flushLiveStrokeMoves();
    } else {
        pendingLiveStrokeMoves.clear();
    }
    pendingEraserMove.reset();

    clearTextStylePopupInteraction();
    if (textInteraction.isActive()) {
        commitText(false, false);
    }

    const QRegion previewRegion = watermarkViewRenderRegion().united(spotlightViewRenderRegion());
    pendingWatermarkPreview.reset();
    watermarkPreview.reset();
    watermarkPreviewTimer.stop();
    watermarkPreviewScheduled = false;
    pendingSpotlightPreview.reset();
    spotlightPreviewTimer.stop();
    spotlightPreviewScheduled = false;
    spotlightPreview.reset();

    if (!applyMutationResult(snow_canvas_commands::resetEditingState(
            runtimeBinding.engine(), runtimeBinding.viewportHandle()))) {
        return false;
    }

    clearRenderState();
    requestedCanvasTool = SnowCanvasTool::Select;
    inputHandler.clearTransientState();
    applyCanvasToolCursor(SnowCanvasTool::Select);
    refreshSerialNumberToolbar();
    if (!previewRegion.isEmpty()) {
        widget.update(previewRegion);
    }
    return true;
}

bool SnowCanvasWidget::resetEditingState() {
    return m_impl->resetEditingState();
}

void SnowCanvasWidget::clearRenderState() {
    m_impl->clearRenderState();
}

bool SnowCanvasWidget::Impl::cancelActiveTextEditing() {
    SnowCanvasWidgetTextInteraction::CancelResult result = textInteraction.cancel(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache());
    if (!result.sessionEnded) {
        return false;
    }

    clearTextStylePopupInteraction();
    syncChangedViewports(result.changedViewports.get());
    emit widget.styleToolbarStateChanged();
    return true;
}

bool SnowCanvasWidget::cancelActiveTextEditing() {
    return m_impl->cancelActiveTextEditing();
}

bool SnowCanvasWidget::Impl::hasActiveTextEditing() const {
    return textInteraction.isActive();
}

bool SnowCanvasWidget::hasActiveTextEditing() const {
    return m_impl->hasActiveTextEditing();
}

void SnowCanvasWidget::Impl::beginTextStylePopupInteraction() {
    ++textStylePopupInteractionDepth;
    ++textStylePopupFocusRestoreGeneration;
}

void SnowCanvasWidget::beginTextStylePopupInteraction() {
    m_impl->beginTextStylePopupInteraction();
}

void SnowCanvasWidget::Impl::endTextStylePopupInteraction(QWidget* focusScope) {
    if (textStylePopupInteractionDepth <= 0) {
        return;
    }

    --textStylePopupInteractionDepth;
    if (textStylePopupInteractionDepth != 0) {
        return;
    }

    textStylePopupFocusScope = focusScope;
    const quint64 generation = ++textStylePopupFocusRestoreGeneration;
    QTimer::singleShot(0, &widget, [this, generation]() {
        if (generation != textStylePopupFocusRestoreGeneration ||
            textStylePopupInteractionDepth != 0 || !textInteraction.isActive() ||
            QGuiApplication::applicationState() != Qt::ApplicationActive) {
            return;
        }

        QWidget* focusWidget = QApplication::focusWidget();
        const bool focusMayBeRestored =
            focusWidget == nullptr || !focusWidget->isVisible() ||
            focusWidgetIsWithin(focusWidget, textStylePopupFocusScope.data());
        if (focusMayBeRestored) {
            widget.setFocus(Qt::OtherFocusReason);
        }
    });
}

void SnowCanvasWidget::endTextStylePopupInteraction(QWidget* focusScope) {
    m_impl->endTextStylePopupInteraction(focusScope);
}

void SnowCanvasWidget::Impl::clearTextStylePopupInteraction() {
    textStylePopupInteractionDepth = 0;
    textStylePopupFocusScope = nullptr;
    ++textStylePopupFocusRestoreGeneration;
}

void SnowCanvasWidget::Impl::handleApplicationStateChanged(Qt::ApplicationState state) {
    if (state == Qt::ApplicationActive) {
        return;
    }

    clearTextStylePopupInteraction();
    if (textInteraction.isActive()) {
        commitText(false);
    }
}

SnowCanvasWidgetTextInteraction::CommitResult
SnowCanvasWidget::Impl::commitText(bool refocusWidget, bool restoreExistingSelection) {
    SnowCanvasWidgetTextInteraction::CommitResult result =
        textInteraction.commit(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                               hasViewport(), displayState.displayCache(), refocusWidget);
    applyTextCommitResult(result, restoreExistingSelection);
    return result;
}

void SnowCanvasWidget::Impl::applyTextCommitResult(
    SnowCanvasWidgetTextInteraction::CommitResult& result, bool restoreExistingSelection) {
    syncChangedViewports(result.changedViewports.get());
    if (restoreExistingSelection) {
        result.restoredExistingSelection =
            restoreFinishedExistingTextSelection(result.finishedExistingEdit);
    }
    if (result.sessionEnded) {
        clearTextStylePopupInteraction();
        emit widget.styleToolbarStateChanged();
    }
}

void SnowCanvasWidget::Impl::applyTextEditorEventResult(
    SnowCanvasWidgetTextInteraction::EditorEventResult& result) {
    syncChangedViewports(result.changedViewports.get());
    restoreFinishedExistingTextSelection(result.finishedExistingEdit);
    if (result.sessionEnded) {
        clearTextStylePopupInteraction();
        emit widget.styleToolbarStateChanged();
    }
}

bool SnowCanvasWidget::Impl::restoreFinishedExistingTextSelection(
    const SnowCanvasWidgetTextInteraction::FinishedExistingEdit& edit) {
    SnowCanvasWidgetTextInteraction::SelectionRestoreResult result =
        textInteraction.restoreFinishedExistingSelection(runtimeBinding.engine(),
                                                         runtimeBinding.viewportHandle(), edit);
    if (result.restored) {
        syncChangedViewports(result.changedViewports.get());
    }
    return result.restored;
}

bool SnowCanvasWidget::Impl::beginText(const QPointF& viewPosition, bool allowCreate) {
    commitText();

    const SnowTextStyle textStyle = displayState.snapshot().styleToolbarState.text_style;
    SnowCanvasWidgetTextInteraction::BeginResult result =
        textInteraction.beginAt(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                displayState.displayCache(), viewPosition, textStyle, allowCreate);
    syncChangedViewports(result.firstChangedViewports.get());
    syncChangedViewports(result.secondChangedViewports.get());
    return result.started;
}

bool SnowCanvasWidget::Impl::beginSelectedText(const QPointF& viewPosition,
                                               bool requireSerialBoundText) {
    commitText();
    SnowCanvasWidgetTextInteraction::BeginResult result = textInteraction.beginSelectedAt(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache(),
        viewPosition, requireSerialBoundText);
    syncChangedViewports(result.firstChangedViewports.get());
    syncChangedViewports(result.secondChangedViewports.get());
    return result.started;
}

void SnowCanvasWidget::Impl::refreshStateFromEngine(bool emitSignals) {
    if (!hasViewport()) {
        return;
    }

    snow_canvas_state::Changes changes;
    if (!displayState.refreshState(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                   &changes)) {
        return;
    }

    if (emitSignals) {
        emitChangedStateSignals(changes);
    }
    refreshSerialNumberToolbar();
}

void SnowCanvasWidget::Impl::syncAfterEngineMutation() {
    syncAfterEngineMutation(true);
}

void SnowCanvasWidget::Impl::syncAfterEngineMutation(bool emitSignals) {
    const std::uint64_t previousSceneRevision =
        displayState.displayCache().patchCursor().scene_revision;
    const snow_canvas_widget_sync::Result result =
        snow_canvas_widget_sync::syncAfterEngineMutation(snow_canvas_widget_sync::Request{
            &displayState,
            &textInteraction.session(),
            runtimeBinding.engine(),
            runtimeBinding.viewportHandle(),
            hasViewport(),
            widget.rect(),
            widget.font(),
            dirtyRectsVisible,
            emitSignals,
            QRegion(),
        });

    const SnowCanvasDisplayCache& cache = displayState.displayCache();
    if (cache.patchCursor().scene_revision != previousSceneRevision) {
        // Paint events can coalesce patches, so consume each patch's dirty region
        // before the next display-cache sync replaces it.
        snow_canvas_filter_tile_cache::invalidateRegion(
            &widget,
            snow_canvas_display::dirtyRectsToRegion(cache.sceneDirtyRects(),
                                                    cache.sceneDirtyRectCount(), widget.rect()),
            widget.devicePixelRatioF());
    }
    if (result.shouldEmitStateSignals) {
        emitChangedStateSignals(result.stateChanges);
    }
    snow_canvas_widget_repaint::updateCoalesced(widget, result.repaintRegion);
    refreshSerialNumberToolbar();
}

void SnowCanvasWidget::Impl::syncChangedViewports(SnowChangedViewportList changedViewports) {
    runtimeBinding.syncChangedViewports(changedViewports);
}

bool SnowCanvasWidget::Impl::applyMutationResult(
    const snow_canvas_commands::MutationResult& result) {
    if (!result.success) {
        return false;
    }

    syncChangedViewports(result.changedViewports.get());
    refocusWidget();
    return true;
}

bool SnowCanvasWidget::Impl::applyPairedMutationResult(
    const snow_canvas_commands::PairedMutationResult& result) {
    syncChangedViewports(result.firstChangedViewports.get());
    if (!result.success) {
        return false;
    }

    syncChangedViewports(result.secondChangedViewports.get());
    refocusWidget();
    return true;
}

void SnowCanvasWidget::Impl::emitChangedStateSignals(const snow_canvas_state::Changes& changes) {
    if (changes.activeToolChanged) {
        emit widget.activeToolChanged();
    }
    if (changes.styleToolbarChanged) {
        emit widget.styleToolbarStateChanged();
    }
    if (changes.historyChanged) {
        emit widget.historyStateChanged();
    }
    if (changes.snapConfigChanged) {
        emit widget.snapConfigChanged();
    }
    if (changes.gridConfigChanged) {
        emit widget.gridConfigChanged();
    }
}

void SnowCanvasWidget::Impl::applyCanvasToolCursor(SnowCanvasTool tool) {
    const std::optional<QCursor> cursor = baselineCursorForCanvasTool(tool);
    if (cursor.has_value()) {
        cursorController.setCursor(SnowCanvasCursorLayer::CanvasTool, *cursor);
        return;
    }
    cursorController.clearCursor(SnowCanvasCursorLayer::CanvasTool);
}

void SnowCanvasWidget::Impl::refocusWidget() {
    widget.setFocus(Qt::OtherFocusReason);
}

bool SnowCanvasWidget::Impl::setSurfaceSizeAndSync(const QSize& size, bool emitSignals) {
    if (!hasViewport()) {
        return false;
    }
    if (surfaceSizeMatches(displayState.displayCache(), size)) {
        return true;
    }
    if (!snow_canvas_lifecycle::setSurfaceSize(runtimeBinding.engine(), runtimeBinding.viewport(),
                                               size)) {
        return false;
    }

    syncAfterEngineMutation(emitSignals);
    return true;
}

bool SnowCanvasWidget::Impl::setCameraAndSync(double centerX, double centerY, double zoom) {
    if (!hasViewport()) {
        return false;
    }
    if (cameraMatches(displayState.displayCache(), centerX, centerY, zoom)) {
        return true;
    }
    if (!snow_canvas_lifecycle::setCamera(runtimeBinding.engine(), runtimeBinding.viewport(),
                                          centerX, centerY, zoom)) {
        return false;
    }

    syncAfterEngineMutation(true);
    return true;
}

void SnowCanvasWidget::Impl::paintEvent(QPaintEvent* event) {
    const QRegion exposedRegion = event != nullptr ? event->region() : QRegion(widget.rect());
    if (!hasExposedRect(exposedRegion, widget.rect())) {
        return;
    }

    QPainter painter(&widget);
    paint(painter, exposedRegion);
}

bool SnowCanvasWidget::Impl::paint(QPainter& painter, const QRegion& exposedRegion) {
    if (!hasExposedRect(exposedRegion, widget.rect())) {
        return false;
    }

    const SnowCanvasDisplayCache& cache = displayState.displayCache();

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(exposedRegion);
    snow_canvas_compositor::Frame frame = buildPaintFrame();
    if (canvasContentIsVisible) {
        const SceneDisplayInfo& sceneInfo = cache.sceneInfo();
        const std::uint64_t contentKey =
            sceneCacheContentKey(sceneInfo, installedCustomRenderer, canvasClearBackgroundEnabled,
                                 painterDevicePixelRatio(painter, widget), widget.size());

        snow_canvas_renderer::resetFilterRenderDiagnosticsForCurrentThread();
        const SnowCanvasRenderContext tileContext = renderContext(painter, exposedRegion);
        const bool filterVisible = hasFilter(frame.sceneItems, frame.sceneItemCount);
        if (!filterVisible) {
            snow_canvas_compositor::clearSurface(painter, frame);
            renderBeforeCanvas(painter, tileContext);
        }
        snow_canvas_renderer::SceneRenderRequest sceneRequest{
            &painter,
            &cache.sceneInfo(),
            frame.sceneItems,
            frame.sceneItemCount,
            exposedRegion,
            nullptr,
            0,
            frame.backgroundImage,
            filterVisible ? installedCustomRenderer : nullptr,
            filterVisible ? &tileContext : nullptr,
            frame.displayCache,
            frame.workspace,
            {},
            nullptr,
            &widget,
            frame.penMaskAtlas,
            true,
            static_cast<std::uint64_t>(contentKey),
            QPoint(),
            canvasClearBackgroundEnabled,
        };
        snow_canvas_renderer::renderSceneItemsTiled(sceneRequest);
        painter.save();
        snow_canvas_compositor::renderDocumentDecorations(painter, frame);
        snow_canvas_compositor::renderEditorOverlays(painter, frame);
        textInteraction.renderEditorOverlay(painter, widget.font(), cache);
        painter.restore();
    } else {
        const SnowCanvasRenderContext context = renderContext(painter, exposedRegion);
        snow_canvas_compositor::clearSurface(painter, frame);
        renderBeforeCanvas(painter, context);
    }
    const SnowCanvasRenderContext context = renderContext(painter, exposedRegion);
    renderAfterCanvas(painter, context);

    return true;
}

snow_canvas_compositor::Frame SnowCanvasWidget::Impl::buildPaintFrame() const {
    const SnowCanvasDisplayCache& cache = displayState.displayCache();
    snow_canvas_compositor::Frame frame =
        snow_canvas_widget_paint_frame::build(snow_canvas_widget_paint_frame::Request{
            &cache,
            &widget,
            canvasClearBackgroundEnabled,
            dirtyRectsVisible,
            const_cast<snow_canvas_filter_render::RenderWorkspace*>(&filterWorkspace),
        });
    frame.penMaskAtlas = &const_cast<SnowCanvasWidget::Impl*>(this)->penMaskAtlas;
    updateWatermarkPresentationInfo();
    frame.watermarkInfo = &watermarkPresentationInfo;
    frame.watermarkRenderArea = watermarkViewRenderArea();
    frame.hasWatermarkRenderArea = configuredWatermarkRenderArea.has_value();
    spotlightPresentationInfo = cache.spotlightInfo();
    if (spotlightPreview.has_value()) {
        spotlightPresentationInfo.color = snow_canvas_types::toEngineColor(spotlightPreview->color);
        spotlightPresentationInfo.opacity = spotlightPreview->opacity;
    }
    frame.spotlightInfo = &spotlightPresentationInfo;
    frame.spotlightRenderArea = spotlightViewRenderArea();
    frame.hasSpotlightRenderArea = configuredSpotlightRenderArea.has_value();
    return frame;
}

void SnowCanvasWidget::Impl::updateWatermarkPresentationInfo() const {
    watermarkPresentationInfo = displayState.displayCache().watermarkInfo();
    if (watermarkPreview.has_value()) {
        applyWatermarkConfig(watermarkPresentationInfo, *watermarkPreview);
    }
}

SnowCanvasRenderContext SnowCanvasWidget::Impl::renderContext(QPainter& painter,
                                                              const QRegion& exposedRegion) const {
    const SceneDisplayInfo& sceneInfo = displayState.displayCache().sceneInfo();
    return SnowCanvasRenderContext{
        widget.rect(),
        exposedRegion.intersected(widget.rect()),
        ::canvasToViewTransform(sceneInfo),
        painterDevicePixelRatio(painter, widget),
    };
}

void SnowCanvasWidget::Impl::renderBeforeCanvas(QPainter& painter,
                                                const SnowCanvasRenderContext& context) const {
    if (installedCustomRenderer == nullptr) {
        return;
    }
    painter.save();
    installedCustomRenderer->renderBeforeCanvas(painter, context);
    painter.restore();
}

void SnowCanvasWidget::Impl::renderAfterCanvas(QPainter& painter,
                                               const SnowCanvasRenderContext& context) const {
    if (installedCustomRenderer == nullptr) {
        return;
    }
    painter.save();
    installedCustomRenderer->renderAfterCanvas(painter, context);
    painter.restore();
}

void SnowCanvasWidget::paintEvent(QPaintEvent* event) {
    m_impl->paintEvent(event);
}

bool SnowCanvasWidget::eventFilter(QObject* watched, QEvent* event) {
    return QWidget::eventFilter(watched, event);
}

bool SnowCanvasWidget::Impl::handleMousePress(QMouseEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }

    clearTextStylePopupInteraction();

    const bool textEditorActive = textInteraction.isActive();
    const bool pointerInsideTextEditor =
        textInteraction.editorContains(displayState.displayCache(), event->position());
    if (textInteraction.handleEditorMousePress(event, displayState.displayCache(), widget.font())) {
        return true;
    }
    const bool pointerOverTextEditorSelectionInteraction =
        textInteraction.selectionInteractionContains(displayState.displayCache(),
                                                     event->position());
    const bool pointerOverSelectionInteraction =
        pointerOverTextEditorSelectionInteraction ||
        (!textEditorActive &&
         snow_canvas_widget_selection_hit_testing::pointerHitsSelectionInteraction(
             displayState.displayCache(), event->position()));
    const snow_canvas_widget_pointer_flow::PressPlan plan =
        snow_canvas_widget_pointer_flow::planPress(snow_canvas_widget_pointer_flow::PressRequest{
            true,
            textEditorActive,
            pointerInsideTextEditor,
            canvasTool(),
            event->button(),
            event->modifiers(),
            pointerOverSelectionInteraction,
            suppressNextTextToolCreate,
            restoredSelectionForNextTextToolPress,
        });
    if (!plan.shouldFocusWidget) {
        return false;
    }

    widget.setFocus(Qt::MouseFocusReason);
    suppressNextTextToolCreate = false;
    restoredSelectionForNextTextToolPress = false;
    bool canDispatchAfterTextCommit = true;
    if (plan.shouldCommitTextEditor) {
        const SnowCanvasWidgetTextInteraction::CommitResult commitResult =
            commitText(true, pointerOverSelectionInteraction);
        if (plan.dispatchAfterCommitRequiresRestoredSelection) {
            canDispatchAfterTextCommit = commitResult.restoredExistingSelection ||
                                         plan.suppressedTextCreateRestoredSelection;
        }
    }

    if (plan.shouldBeginText && beginText(event->position(), plan.allowCreateText)) {
        event->accept();
        return true;
    }
    if (plan.shouldBeginSelectedText && !pointerOverSelectionInteraction &&
        beginSelectedText(event->position(), canvasTool() == SnowCanvasTool::SerialNumber)) {
        event->accept();
        return true;
    }
    if (plan.shouldAcceptIfTextBeginFails) {
        event->accept();
        return true;
    }
    if (plan.shouldAcceptSuppressedTextCreate) {
        event->accept();
        return true;
    }
    if (!plan.shouldDispatchToEngine) {
        return false;
    }
    if (!canDispatchAfterTextCommit) {
        event->accept();
        return true;
    }
    return dispatchInput(event,
                         snow_canvas_input::makePointerInput(*event, SNOW_POINTER_EVENT_DOWN));
}

void SnowCanvasWidget::mousePressEvent(QMouseEvent* event) {
    if (event != nullptr && event->button() == Qt::MiddleButton) {
        m_impl->beginMiddleClickTracking(event->position());
    }
    const bool handled = m_impl->handleMousePress(event);
    if (event != nullptr && event->button() == Qt::MiddleButton && handled) {
        m_impl->cancelMiddleClickTracking();
    }
    if (handled) {
        return;
    }
    QWidget::mousePressEvent(event);
}

bool SnowCanvasWidget::Impl::handleMouseMove(QMouseEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }
    if (textInteraction.handleEditorMouseMove(event, displayState.displayCache(), widget.font())) {
        return true;
    }
    const SnowInputEvent input =
        snow_canvas_input::makePointerInput(*event, SNOW_POINTER_EVENT_MOVE);
    if ((canvasTool() == SnowCanvasTool::FreeDraw || canvasTool() == SnowCanvasTool::PenFilter) &&
        (event->buttons() & Qt::LeftButton) != 0) {
        return queueLiveStrokeMove(event, input, canvasTool() == SnowCanvasTool::PenFilter);
    }
    if (canvasTool() == SnowCanvasTool::Eraser && (event->buttons() & Qt::LeftButton) != 0) {
        return queueEraserMove(event, input);
    }
    flushLiveStrokeMoves();
    flushEraserMove();
    return dispatchInput(event, input);
}

bool SnowCanvasWidget::Impl::queueEraserMove(QMouseEvent* event, const SnowInputEvent& input) {
    pendingEraserMove = input;
    if (!eraserMoveFlushScheduled) {
        eraserMoveFlushScheduled = true;
        QTimer::singleShot(0, &widget, [this]() { flushEraserMove(); });
    }
    event->accept();
    return true;
}

void SnowCanvasWidget::Impl::flushEraserMove() {
    eraserMoveFlushScheduled = false;
    if (!pendingEraserMove.has_value()) {
        return;
    }
    const SnowInputEvent input = *pendingEraserMove;
    pendingEraserMove.reset();
    static_cast<void>(processInput(input));
    emit widget.eraserMoveFrameProcessed();
}

bool SnowCanvasWidget::Impl::handleMouseDoubleClick(QMouseEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }
    if (textInteraction.isActive()) {
        event->accept();
        return true;
    }
    flushLiveStrokeMoves();
    flushEraserMove();
    return dispatchInput(
        event, snow_canvas_input::makePointerInput(*event, SNOW_POINTER_EVENT_DOUBLE_CLICK));
}

void SnowCanvasWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_impl->handleMouseDoubleClick(event)) {
        return;
    }
    if (event != nullptr && event->button() == Qt::LeftButton) {
        event->accept();
        emit unhandledLeftDoubleClick();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool SnowCanvasWidget::Impl::queueLiveStrokeMove(QMouseEvent* event, const SnowInputEvent& input,
                                                 bool preserveEverySample) {
    if (!pendingLiveStrokeMoves.empty() &&
        pendingLiveStrokePreservesEverySample != preserveEverySample) {
        flushLiveStrokeMoves();
    }
    pendingLiveStrokePreservesEverySample = preserveEverySample;
    pendingLiveStrokeMoves.push_back(input);
    if (!liveStrokeMoveFlushScheduled) {
        liveStrokeMoveFlushScheduled = true;
        const quint64 generation = ++liveStrokeMoveFlushGeneration;
        QTimer::singleShot(8, &widget, [this, generation]() {
            if (generation == liveStrokeMoveFlushGeneration) {
                flushLiveStrokeMoves();
            }
        });
    }
    event->accept();
    return true;
}

void SnowCanvasWidget::Impl::flushLiveStrokeMoves() {
    ++liveStrokeMoveFlushGeneration;
    liveStrokeMoveFlushScheduled = false;
    if (pendingLiveStrokeMoves.empty()) {
        return;
    }
    const bool preserveEverySample = pendingLiveStrokePreservesEverySample;
    pendingLiveStrokePreservesEverySample = false;
    constexpr std::size_t kMaximumSamplesPerFrame = 96;
    const std::size_t pendingCount = pendingLiveStrokeMoves.size();
    const std::size_t dispatchCount =
        preserveEverySample ? pendingCount : std::min(pendingCount, kMaximumSamplesPerFrame);
    if (dispatchCount < pendingCount) {
        for (std::size_t outputIndex = 0; outputIndex < dispatchCount; ++outputIndex) {
            const std::size_t inputIndex =
                dispatchCount == 1 ? pendingCount - 1
                                   : outputIndex * (pendingCount - 1) / (dispatchCount - 1);
            if (outputIndex != inputIndex) {
                pendingLiveStrokeMoves[outputIndex] = pendingLiveStrokeMoves[inputIndex];
            }
        }
        pendingLiveStrokeMoves.resize(dispatchCount);
    }
    const SnowCanvasWidgetInputHandler::ProcessResult result =
        inputHandler.processBatch(inputHandlerContext(), pendingLiveStrokeMoves);
    if (result.success) {
        runtimeBinding.syncChangedViewports(result.changedViewports.get());
    }
    if (!preserveEverySample) {
        emit widget.freeDrawMoveBatchProcessed(static_cast<quint32>(pendingCount),
                                               static_cast<quint32>(dispatchCount));
    }
    pendingLiveStrokeMoves.clear();
}

void SnowCanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (event != nullptr) {
        m_impl->updateMiddleClickTracking(event->position());
    }
    if (m_impl->handleMouseMove(event)) {
        return;
    }
    QWidget::mouseMoveEvent(event);
}

bool SnowCanvasWidget::Impl::handleMouseRelease(QMouseEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }
    if (textInteraction.handleEditorMouseRelease(event)) {
        return true;
    }
    flushLiveStrokeMoves();
    flushEraserMove();
    return dispatchInput(event, snow_canvas_input::makePointerInput(*event, SNOW_POINTER_EVENT_UP));
}

void SnowCanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    const bool middleClick = event != nullptr && event->button() == Qt::MiddleButton &&
                             m_impl->finishMiddleClickTracking();
    const bool handled = m_impl->handleMouseRelease(event);
    if (middleClick && !handled) {
        event->accept();
        emit unhandledMiddleClick();
        return;
    }
    if (handled) {
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void SnowCanvasWidget::Impl::beginMiddleClickTracking(const QPointF& position) {
    middleClickPressPosition = position;
    middleClickCandidate = true;
}

void SnowCanvasWidget::Impl::updateMiddleClickTracking(const QPointF& position) {
    if (middleClickCandidate && middleClickPressPosition.has_value() &&
        QLineF(*middleClickPressPosition, position).length() > QApplication::startDragDistance()) {
        middleClickCandidate = false;
    }
}

bool SnowCanvasWidget::Impl::finishMiddleClickTracking() {
    const bool candidate = middleClickCandidate;
    middleClickCandidate = false;
    middleClickPressPosition.reset();
    return candidate;
}

void SnowCanvasWidget::Impl::cancelMiddleClickTracking() {
    middleClickCandidate = false;
    middleClickPressPosition.reset();
}

bool SnowCanvasWidget::Impl::handleEnter(QEnterEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }
    return dispatchInput(event,
                         snow_canvas_input::makePointerInput(
                             event->position(), Qt::NoButton, QGuiApplication::mouseButtons(),
                             QGuiApplication::keyboardModifiers(), SNOW_POINTER_EVENT_ENTER));
}

void SnowCanvasWidget::enterEvent(QEnterEvent* event) {
    if (m_impl->handleEnter(event)) {
        return;
    }
    QWidget::enterEvent(event);
}

bool SnowCanvasWidget::Impl::handleLeave(QEvent* event) {
    if (!interactionEnabled()) {
        return false;
    }
    return dispatchInput(
        event, snow_canvas_input::makePointerInput(widget.mapFromGlobal(QCursor::pos()),
                                                   Qt::NoButton, QGuiApplication::mouseButtons(),
                                                   QGuiApplication::keyboardModifiers(),
                                                   SNOW_POINTER_EVENT_LEAVE));
}

void SnowCanvasWidget::leaveEvent(QEvent* event) {
    if (m_impl->handleLeave(event)) {
        return;
    }
    QWidget::leaveEvent(event);
}

bool SnowCanvasWidget::Impl::handleWheel(QWheelEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }

    if (canvasTool() == SnowCanvasTool::SerialNumber) {
        const snow_canvas_text_editor_input::FontSizeWheelPlan plan =
            snow_canvas_text_editor_input::planFontSizeWheel(
                snow_canvas_text_editor_input::FontSizeWheelRequest{
                    true,
                    canvasTool(),
                    event->modifiers(),
                    event->pixelDelta().y(),
                    event->angleDelta().y(),
                });
        if (plan.matchedToolWheel) {
            if (!plan.shouldStepFontSize) {
                return false;
            }

            SnowSerialNumberStyle style =
                displayState.snapshot().styleToolbarState.serial_number_style;
            const double nextFontSize =
                snow_canvas_text_measurement::steppedFontSize(style.font_size, plan.increase);
            if (std::abs(nextFontSize - style.font_size) <=
                std::numeric_limits<double>::epsilon()) {
                return true;
            }

            style.font_size = nextFontSize;
            return applyMutationResult(snow_canvas_commands::setSerialNumberStyle(
                runtimeBinding.engine(), runtimeBinding.viewportHandle(), style));
        }
    }

    SnowCanvasWidgetTextInteraction::WheelFontSizeResult textWheelResult =
        textInteraction.handleFontSizeWheel(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache(),
            displayState.snapshot().styleToolbarState.text_style, canvasTool(), event);
    if (textWheelResult.matchedToolWheel) {
        if (!textWheelResult.handled) {
            return false;
        }

        syncChangedViewports(textWheelResult.changedViewports.get());
        if (textWheelResult.toolbarStateChanged) {
            emit widget.styleToolbarStateChanged();
        }
        return true;
    }
    if (!wheelZoomEnabled()) {
        event->accept();
        return true;
    }
    return dispatchInput(event, snow_canvas_input::makeWheelInput(*event));
}

void SnowCanvasWidget::wheelEvent(QWheelEvent* event) {
    if (m_impl->handleWheel(event)) {
        return;
    }
    QWidget::wheelEvent(event);
}

bool SnowCanvasWidget::Impl::handleKeyPress(QKeyEvent* event) {
    if (!interactionEnabled()) {
        return false;
    }
    if (textInteraction.isActive()) {
        SnowCanvasWidgetTextInteraction::EditorEventResult result = textInteraction.handleKeyPress(
            event, runtimeBinding.engine(), runtimeBinding.viewportHandle(), hasViewport(),
            displayState.displayCache());
        if (result.handled) {
            applyTextEditorEventResult(result);
            return true;
        }
    }

    const snow_canvas_widget_keyboard_flow::KeyPlan plan =
        snow_canvas_widget_keyboard_flow::planPress(event);
    if (!plan.hasEvent) {
        return false;
    }

    switch (plan.effect) {
    case snow_canvas_widget_keyboard_flow::KeyEffect::ToggleDirtyRects:
        setShowDirtyRects(!dirtyRectsVisible);
        accept(*event);
        return true;
    case snow_canvas_widget_keyboard_flow::KeyEffect::AcceptOnly:
        accept(*event);
        return true;
    case snow_canvas_widget_keyboard_flow::KeyEffect::DispatchToEngine:
    default:
        break;
    }

    return dispatchInput(event, snow_canvas_input::makeKeyInput(*event, SNOW_KEY_EVENT_DOWN));
}

void SnowCanvasWidget::keyPressEvent(QKeyEvent* event) {
    if (m_impl->handleKeyPress(event)) {
        return;
    }
    QWidget::keyPressEvent(event);
}

bool SnowCanvasWidget::Impl::handleKeyRelease(QKeyEvent* event) {
    if (!interactionEnabled()) {
        return false;
    }
    if (textInteraction.handleKeyRelease(event)) {
        return true;
    }

    const snow_canvas_widget_keyboard_flow::KeyPlan plan =
        snow_canvas_widget_keyboard_flow::planRelease(event);
    if (!plan.hasEvent) {
        return false;
    }

    if (plan.effect == snow_canvas_widget_keyboard_flow::KeyEffect::AcceptOnly) {
        accept(*event);
        return true;
    }
    return dispatchInput(event, snow_canvas_input::makeKeyInput(*event, SNOW_KEY_EVENT_UP));
}

void SnowCanvasWidget::keyReleaseEvent(QKeyEvent* event) {
    if (m_impl->handleKeyRelease(event)) {
        return;
    }
    QWidget::keyReleaseEvent(event);
}

bool SnowCanvasWidget::Impl::handleInputMethodEvent(QInputMethodEvent* event) {
    if (!interactionEnabled()) {
        return false;
    }
    SnowCanvasWidgetTextInteraction::InputMethodEventResult result =
        textInteraction.handleInputMethodEvent(event, runtimeBinding.engine(),
                                               runtimeBinding.viewportHandle(),
                                               displayState.displayCache(), widget.font());
    syncChangedViewports(result.changedViewports.get());
    if (result.handled) {
        return true;
    }
    return false;
}

void SnowCanvasWidget::inputMethodEvent(QInputMethodEvent* event) {
    if (m_impl->handleInputMethodEvent(event)) {
        return;
    }
    QWidget::inputMethodEvent(event);
}

QVariant SnowCanvasWidget::Impl::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (!interactionEnabled()) {
        return widget.QWidget::inputMethodQuery(query);
    }
    QVariant value =
        textInteraction.inputMethodQuery(query, displayState.displayCache(), widget.font());
    if (value.isValid()) {
        return value;
    }
    return widget.QWidget::inputMethodQuery(query);
}

QVariant SnowCanvasWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    return m_impl->inputMethodQuery(query);
}

void SnowCanvasWidget::Impl::handleFocusOut() {
    if (pendingLiveStrokePreservesEverySample) {
        flushLiveStrokeMoves();
    } else {
        pendingLiveStrokeMoves.clear();
    }
    pendingEraserMove.reset();
    const bool retainTextEditing = textStylePopupInteractionDepth > 0;
    if (textInteraction.isActive() && !retainTextEditing) {
        commitText(false);
    }
    if (!retainTextEditing) {
        processInput(snow_canvas_event_flow::focusLostInput());
    }
}

void SnowCanvasWidget::focusOutEvent(QFocusEvent* event) {
    m_impl->handleFocusOut();
    QWidget::focusOutEvent(event);
}

void SnowCanvasWidget::Impl::handleResize(const QSize& size) {
    snow_canvas_filter_tile_cache::invalidateNamespace(&widget);
    setSurfaceSizeAndSync(size, true);
}

void SnowCanvasWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    m_impl->handleResize(event->size());
}

void SnowCanvasWidget::Impl::setShowDirtyRects(bool show) {
    if (dirtyRectsVisible == show) {
        return;
    }
    dirtyRectsVisible = show;

    emit widget.showDirtyRectsChanged();

    const QRegion repaintRegion = displayState.dirtyVisualizationRegion();
    snow_canvas_widget_repaint::updateCoalesced(widget, repaintRegion);
}

void SnowCanvasWidget::setShowDirtyRects(bool show) {
    m_impl->setShowDirtyRects(show);
}
