#include "snow_draw_engine_qt/snow_canvas_view.h"
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
#include "snow_canvas_smart_erase.h"
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

std::optional<SnowCursorStyle> baselineCursorForCanvasTool(SnowCanvasTool tool) {
    switch (tool) {
    case SnowCanvasTool::Shape:
    case SnowCanvasTool::Arrow:
    case SnowCanvasTool::Line:
    case SnowCanvasTool::RectangleHighlight:
    case SnowCanvasTool::RectangleFilter:
    case SnowCanvasTool::Spotlight:
    case SnowCanvasTool::SerialNumber:
        return SNOW_CURSOR_STYLE_CROSSHAIR;
    case SnowCanvasTool::FreeDraw:
    case SnowCanvasTool::PenHighlight:
    case SnowCanvasTool::PenFilter:
        return SNOW_CURSOR_STYLE_STROKE;
    case SnowCanvasTool::Eraser:
        return SNOW_CURSOR_STYLE_ERASER;
    case SnowCanvasTool::Text:
        return SNOW_CURSOR_STYLE_TEXT;
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

bool isPointerInput(const SnowInputEvent& input, SnowPointerEventType eventType) {
    return input.kind == SNOW_INPUT_EVENT_POINTER && input.pointer.event_type == eventType;
}

bool focusWidgetIsWithin(const QWidget* focusWidget, const QWidget* scope) {
    for (const QWidget* candidate = focusWidget; candidate != nullptr;
         candidate = candidate->parentWidget()) {
        if (candidate == scope) {
            return true;
        }
    }
    return false;
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
    // Transient watermark previews currently modify presentation properties only. Keep the
    // engine-resolved template text from the retained display cache so a color preview cannot
    // temporarily expose the raw watermark text.
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

qreal painterDevicePixelRatio(const QPainter& painter, const SnowCanvasView& widget) {
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

bool hitsSelectedText(SnowRuntime runtime, SnowViewport viewport, const QPointF& canvasPoint) {
    SnowElementId id{};
    std::uint8_t hit = 0;
    std::uint8_t selected = 0;
    return snow_viewport_hit_text(runtime, viewport, canvasPoint.x(), canvasPoint.y(), &id, &hit) ==
               SNOW_OK &&
           hit != 0 &&
           snow_viewport_is_element_selected(runtime, viewport, id, &selected) == SNOW_OK &&
           selected != 0;
}

} // namespace

struct SnowCanvasView::Impl : public snow_canvas_runtime::Client {
    explicit Impl(SnowCanvasView& widget)
        : widget(widget), cursorController(widget), inputHandler(widget, cursorController),
          textInteraction(widget, cursorController) {}

    Impl(SnowCanvasView& widget, SnowCanvasRuntime& runtime)
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
    void setBaseImageSources(const QList<SnowCanvasBaseImageSource>& sources);
    void smartEraseChanged() override {
        clearRenderState();
        widget.update();
    }
    void clearRetainedDisplayState();
    bool hasViewport() const;
    void syncAfterEngineMutation() override;
    void syncAfterEngineMutation(bool emitSignals);
    quint64 autoFilterGeneration = 0;
    void refreshStateFromEngine(bool emitSignals) override;
    void syncChangedViewports(SnowChangedViewportList changedViewports);
    bool setSurfaceSizeAndSync(const QSize& size, bool emitSignals);
    bool setCameraAndSync(double centerX, double centerY, double zoom);
    void shutdown();
    void refreshSerialNumberToolbar();

    SnowCanvasTool canvasTool() const;
    bool setCanvasTool(SnowCanvasTool tool);
    void setCursorForLayer(SnowCanvasCursorLayer layer, const QCursor& cursor);
    void clearCursorForLayer(SnowCanvasCursorLayer layer);
    void refreshCursorDevicePixelRatio() {
        cursorController.refreshDevicePixelRatio();
    }
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
    quint64 readAutoFilterGeneration() const;
    std::optional<SnowCanvasAutoFilterRecord> autoFilterRegions() const;
    bool setAutoFilterRegions(const std::optional<SnowCanvasAutoFilterRecord>& record);
    bool fillAutoFilterCategory(const QString& category);
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
    bool deleteAllElements();
    bool clearDocument();
    bool duplicateSelected(const QPointF& offset);
    bool reorderSelected(SnowCanvasSelectionOrder order);
    bool alignSelected(SnowCanvasSelectionAlignment alignment);
    bool setSelectedOpacity(double opacity);
    bool adjustSelectedSerialNumbers(qint64 delta);
    bool createSerialNumberText();
    bool beginArrowText(const QPointF& viewPosition, bool selected);
    bool resetEditingState(bool restoreSelectTool) override;
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
    enum class FontWheelTarget { Text, SerialNumber };
    FontWheelTarget fontWheelTarget() const;
    bool stepSerialNumberFontSize(bool increase);
    bool stepTextFontSize(bool increase);
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
    void refreshToolCursorStyle();
    void refocusWidget();

    SnowCanvasView& widget;
    SnowCanvasWidgetRuntimeBinding runtimeBinding;
    SnowCanvasCursorController cursorController;
    SnowCanvasWidgetInputHandler inputHandler;
    SnowCanvasWidgetDisplayState displayState;
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

SnowCanvasWidgetInputHandler::Context SnowCanvasView::Impl::inputHandlerContext() {
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

void SnowCanvasView::Impl::applyInputProcessResult(
    const SnowCanvasWidgetInputHandler::ProcessResult& result) {
    if (!result.success) {
        return;
    }
    syncChangedViewports(result.changedViewports.get());
}

bool SnowCanvasView::Impl::applyActiveTextResizeMeasurementIfNeeded() {
    SnowCanvasWidgetTextInteraction::ActiveResizeMeasurementResult result =
        textInteraction.applyActiveResizeMeasurementIfNeeded(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache());
    if (!result.success) {
        return false;
    }
    syncChangedViewports(result.changedViewports.get());
    return true;
}

bool SnowCanvasView::Impl::prepareActiveTextResizeMeasurementForPointerUp(
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
    const auto tool = canvasTool();
    const bool geometryTool = tool == SnowCanvasTool::Select || tool == SnowCanvasTool::Arrow ||
                              tool == SnowCanvasTool::Shape;
    if (!state.active &&
        (!geometryTool || snow_runtime_arrow_text_count(runtimeBinding.engine()) == 0)) {
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

bool SnowCanvasView::Impl::dispatchInput(QEvent* event, const SnowInputEvent& input) {
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

bool SnowCanvasView::Impl::processInput(const SnowInputEvent& input) {
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

void SnowCanvasView::Impl::initializeWidget() {
    widget.setInputMethodEnabled(false);
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
    QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, &widget,
                     [this](Qt::ApplicationState state) { handleApplicationStateChanged(state); });
}

void SnowCanvasView::Impl::initializeViewport() {
    displayState.resetRetainedState();
    runtimeBinding.registerClient(*this);
    runtimeBinding.initializeAttachment(displayState);
    setSurfaceSizeAndSync(widget.size(), false);
    refreshSerialNumberToolbar();
}

void SnowCanvasView::Impl::refreshSerialNumberToolbar() {
    emit widget.serialNumberToolbarStateChanged();
}

void SnowCanvasView::Impl::shutdown() {
    if (pendingLiveStrokePreservesEverySample) {
        flushLiveStrokeMoves();
    }
    clearTextStylePopupInteraction();
    detachRuntime();
    runtimeBinding.unregisterClient(*this);
}

SnowCanvasView::~SnowCanvasView() {
    m_impl->shutdown();
}

SnowCanvasTool SnowCanvasView::Impl::canvasTool() const {
    const SnowCanvasTool engineTool =
        snow_canvas_types::toCanvasTool(displayState.snapshot().activeTool);
    return requestedCanvasTool == SnowCanvasTool::Line && engineTool == SnowCanvasTool::Arrow
               ? SnowCanvasTool::Line
               : engineTool;
}

SnowCanvasTool SnowCanvasView::canvasTool() const {
    return m_impl->canvasTool();
}

bool SnowCanvasView::Impl::setCanvasTool(SnowCanvasTool tool) {
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

bool SnowCanvasView::setCanvasTool(SnowCanvasTool tool) {
    return m_impl->setCanvasTool(tool);
}

void SnowCanvasView::Impl::setCursorForLayer(SnowCanvasCursorLayer layer, const QCursor& cursor) {
    cursorController.setCursor(layer, cursor);
}

void SnowCanvasView::setCursorForLayer(SnowCanvasCursorLayer layer, const QCursor& cursor) {
    m_impl->setCursorForLayer(layer, cursor);
}

void SnowCanvasView::Impl::clearCursorForLayer(SnowCanvasCursorLayer layer) {
    cursorController.clearCursor(layer);
}

void SnowCanvasView::clearCursorForLayer(SnowCanvasCursorLayer layer) {
    m_impl->clearCursorForLayer(layer);
}

SnowCanvasStyleToolbarState SnowCanvasView::Impl::canvasStyleToolbarState() const {
    SnowCanvasStyleToolbarState state =
        snow_canvas_types::toCanvasStyleToolbarState(displayState.snapshot().styleToolbarState);
    SnowTextElementInfo arrowText{};
    SnowTextStyle arrowTextStyle{};
    std::uint8_t foundArrow = 0;
    state.canEditArrowText = hasViewport() && interactionEnabled() && !textInteraction.isActive() &&
                             snow_viewport_get_arrow_text_target(
                                 runtimeBinding.engine(), runtimeBinding.viewportHandle(), 0, 0.0,
                                 0.0, &arrowText, &arrowTextStyle, &foundArrow) == SNOW_OK &&
                             foundArrow != 0;
    if (textInteraction.isActive()) {
        if (textInteraction.session().arrowId().generation != 0) {
            state.source = SnowCanvasStyleToolbarSource::SelectedText;
        }
        state.textStyle = snow_canvas_types::toCanvasTextStyle(textInteraction.currentTextStyle());
        state.textStyleMixed = 0;
    }
    return state;
}

SnowCanvasStyleToolbarState SnowCanvasView::canvasStyleToolbarState() const {
    return m_impl->canvasStyleToolbarState();
}

SnowCanvasSerialNumberToolbarState SnowCanvasView::Impl::serialNumberToolbarState() const {
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

SnowCanvasSerialNumberToolbarState SnowCanvasView::serialNumberToolbarState() const {
    return m_impl->serialNumberToolbarState();
}

bool SnowCanvasView::Impl::setCanvasShapeStylePatch(const SnowCanvasShapeStyle& style,
                                                    quint32 properties, SnowCanvasShapeKind kind) {
    return applyMutationResult(snow_canvas_commands::setShapeStylePatch(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        snow_canvas_types::toEngineShapeStyle(style), properties,
        snow_canvas_types::toEngineShapeKind(kind)));
}

SnowCanvasWatermarkConfig SnowCanvasView::Impl::canvasWatermarkConfig() const {
    SnowWatermarkConfig engineConfig{};
    if (snow_viewport_get_watermark_config(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                           &engineConfig) != SNOW_OK) {
        return {};
    }
    return snow_canvas_types::toCanvasWatermarkConfig(engineConfig);
}

SnowCanvasWatermarkConfig SnowCanvasView::canvasWatermarkConfig() const {
    return m_impl->canvasWatermarkConfig();
}

bool SnowCanvasView::Impl::setCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    // A persistent update owns the display state from this point onward. Any
    // queued transient value must not be allowed to overwrite the synced store.
    const QRegion previousPreviewRegion = watermarkViewRenderRegion();
    pendingWatermarkPreview.reset();
    watermarkPreview.reset();
    watermarkPreviewTimer.stop();
    watermarkPreviewScheduled = false;
    const SnowWatermarkConfig engineConfig = snow_canvas_types::toEngineWatermarkConfig(config);
    const bool applied = applyMutationResult(snow_canvas_commands::setWatermarkConfig(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), engineConfig));
    if (applied && !previousPreviewRegion.isEmpty()) {
        widget.update(previousPreviewRegion);
    }
    return applied;
}

bool SnowCanvasView::setCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    return m_impl->setCanvasWatermarkConfig(config);
}

void SnowCanvasView::Impl::previewCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    pendingWatermarkPreview = config;
    if (watermarkPreviewScheduled) {
        return;
    }
    watermarkPreviewScheduled = true;
    applyPendingWatermarkPreview();
    watermarkPreviewTimer.start(watermarkPreviewFrameInterval());
}

void SnowCanvasView::Impl::applyPendingWatermarkPreview() {
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

int SnowCanvasView::Impl::watermarkPreviewFrameInterval() const {
    QScreen* screen = widget.windowHandle() != nullptr ? widget.windowHandle()->screen()
                                                       : QGuiApplication::primaryScreen();
    const qreal refreshRate = screen != nullptr ? screen->refreshRate() : 60.0;
    const int interval =
        refreshRate > 1.0 && std::isfinite(refreshRate) ? qRound(1000.0 / refreshRate) : 16;
    return std::clamp(interval, 4, 16);
}

void SnowCanvasView::previewCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    m_impl->previewCanvasWatermarkConfig(config);
}

SnowCanvasSpotlightConfig SnowCanvasView::Impl::canvasSpotlightConfig() const {
    SnowSpotlightConfig config{};
    if (snow_viewport_get_spotlight_config(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                           &config) != SNOW_OK) {
        return {};
    }
    return snow_canvas_types::toCanvasSpotlightConfig(config);
}

SnowCanvasSpotlightConfig SnowCanvasView::canvasSpotlightConfig() const {
    return m_impl->canvasSpotlightConfig();
}

bool SnowCanvasView::Impl::setCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
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

bool SnowCanvasView::setCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    return m_impl->setCanvasSpotlightConfig(config);
}

void SnowCanvasView::Impl::previewCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
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

void SnowCanvasView::Impl::applyPendingSpotlightPreview() {
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

void SnowCanvasView::previewCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    m_impl->previewCanvasSpotlightConfig(config);
}

bool SnowCanvasView::setCanvasShapeStylePatch(const SnowCanvasShapeStyle& style, quint32 properties,
                                              SnowCanvasShapeKind kind) {
    return m_impl->setCanvasShapeStylePatch(style, properties, kind);
}

bool SnowCanvasView::Impl::setCanvasFilterStyle(const SnowCanvasFilterStyle& style,
                                                quint32 properties) {
    SnowFilterStyle engineStyle{};
    engineStyle.filter_type = static_cast<SnowFilterType>(style.type);
    engineStyle.strength = style.strength;
    engineStyle.opacity = style.opacity;
    engineStyle.stroke_width = style.strokeWidth;
    return applyMutationResult(snow_canvas_commands::setFilterStyle(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), engineStyle, properties));
}

bool SnowCanvasView::setCanvasFilterStyle(const SnowCanvasFilterStyle& style, quint32 properties) {
    return m_impl->setCanvasFilterStyle(style, properties);
}

bool SnowCanvasView::Impl::setCanvasTextStyle(const SnowCanvasTextStyle& style) {
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

bool SnowCanvasView::setCanvasTextStyle(const SnowCanvasTextStyle& style) {
    return m_impl->setCanvasTextStyle(style);
}

bool SnowCanvasView::Impl::setCanvasSerialNumberStyle(const SnowCanvasSerialNumberStyle& style) {
    return applyMutationResult(snow_canvas_commands::setSerialNumberStyle(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        snow_canvas_types::toEngineSerialNumberStyle(style)));
}

bool SnowCanvasView::setCanvasSerialNumberStyle(const SnowCanvasSerialNumberStyle& style) {
    return m_impl->setCanvasSerialNumberStyle(style);
}

SnowCanvasHistoryState SnowCanvasView::Impl::canvasHistoryState() const {
    return snow_canvas_types::toCanvasHistoryState(displayState.snapshot().historyState);
}

SnowCanvasHistoryState SnowCanvasView::canvasHistoryState() const {
    return m_impl->canvasHistoryState();
}

SnowCanvasSnapConfig SnowCanvasView::Impl::canvasSnapConfig() const {
    return snow_canvas_types::toCanvasSnapConfig(displayState.snapshot().snapConfig);
}

SnowCanvasSnapConfig SnowCanvasView::canvasSnapConfig() const {
    return m_impl->canvasSnapConfig();
}

bool SnowCanvasView::Impl::setCanvasSnapConfig(const SnowCanvasSnapConfig& config) {
    return applyPairedMutationResult(snow_canvas_commands::setSnapConfig(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        snow_canvas_types::toEngineSnapConfig(config)));
}

bool SnowCanvasView::setCanvasSnapConfig(const SnowCanvasSnapConfig& config) {
    return m_impl->setCanvasSnapConfig(config);
}

SnowCanvasGridConfig SnowCanvasView::Impl::canvasGridConfig() const {
    return snow_canvas_types::toCanvasGridConfig(displayState.snapshot().gridConfig);
}

SnowCanvasGridConfig SnowCanvasView::canvasGridConfig() const {
    return m_impl->canvasGridConfig();
}

bool SnowCanvasView::Impl::setCanvasGridConfig(const SnowCanvasGridConfig& config) {
    return applyPairedMutationResult(snow_canvas_commands::setGridConfig(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        snow_canvas_types::toEngineGridConfig(config)));
}

bool SnowCanvasView::setCanvasGridConfig(const SnowCanvasGridConfig& config) {
    return m_impl->setCanvasGridConfig(config);
}

bool SnowCanvasView::Impl::interactionEnabled() const {
    return inputHandler.interactionEnabled();
}

bool SnowCanvasView::interactionEnabled() const {
    return m_impl->interactionEnabled();
}

void SnowCanvasView::Impl::setInteractionEnabled(bool enabled) {
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

void SnowCanvasView::setInteractionEnabled(bool enabled) {
    m_impl->setInteractionEnabled(enabled);
}

bool SnowCanvasView::Impl::wheelZoomEnabled() const {
    return m_wheelZoomEnabled;
}

bool SnowCanvasView::wheelZoomEnabled() const {
    return m_impl->wheelZoomEnabled();
}

void SnowCanvasView::Impl::setWheelZoomEnabled(bool enabled) {
    m_wheelZoomEnabled = enabled;
}

void SnowCanvasView::setWheelZoomEnabled(bool enabled) {
    m_impl->setWheelZoomEnabled(enabled);
}

bool SnowCanvasView::Impl::canvasContentVisible() const {
    return canvasContentIsVisible;
}

bool SnowCanvasView::canvasContentVisible() const {
    return m_impl->canvasContentVisible();
}

void SnowCanvasView::Impl::setCanvasContentVisible(bool visible) {
    if (canvasContentIsVisible == visible) {
        return;
    }
    canvasContentIsVisible = visible;
    snow_canvas_filter_tile_cache::invalidateNamespace(&widget);
    refreshSerialNumberToolbar();
    widget.update();
}

void SnowCanvasView::setCanvasContentVisible(bool visible) {
    m_impl->setCanvasContentVisible(visible);
}

bool SnowCanvasView::Impl::clearBackgroundEnabled() const {
    return canvasClearBackgroundEnabled;
}

bool SnowCanvasView::clearBackgroundEnabled() const {
    return m_impl->clearBackgroundEnabled();
}

void SnowCanvasView::Impl::setClearBackgroundEnabled(bool enabled) {
    if (canvasClearBackgroundEnabled == enabled) {
        return;
    }
    canvasClearBackgroundEnabled = enabled;
    widget.update();
}

void SnowCanvasView::setClearBackgroundEnabled(bool enabled) {
    m_impl->setClearBackgroundEnabled(enabled);
}

bool SnowCanvasView::Impl::showDirtyRects() const {
    return dirtyRectsVisible;
}

bool SnowCanvasView::showDirtyRects() const {
    return m_impl->showDirtyRects();
}

std::uint64_t SnowCanvasView::Impl::viewportId() const {
    return runtimeBinding.viewportId();
}

std::uint64_t SnowCanvasView::Impl::runtimeViewportId() const {
    return viewportId();
}

std::uint64_t SnowCanvasView::viewportId() const {
    return m_impl->viewportId();
}

bool SnowCanvasView::Impl::setViewportCamera(double centerX, double centerY, double zoom) {
    return setCameraAndSync(centerX, centerY, zoom);
}

bool SnowCanvasView::setViewportCamera(double centerX, double centerY, double zoom) {
    return m_impl->setViewportCamera(centerX, centerY, zoom);
}

bool SnowCanvasView::Impl::hasWatermarkRenderArea() const {
    return configuredWatermarkRenderArea.has_value();
}

bool SnowCanvasView::hasWatermarkRenderArea() const {
    return m_impl->hasWatermarkRenderArea();
}

QRectF SnowCanvasView::Impl::watermarkRenderArea() const {
    return configuredWatermarkRenderArea.value_or(QRectF());
}

QRectF SnowCanvasView::watermarkRenderArea() const {
    return m_impl->watermarkRenderArea();
}

QRectF SnowCanvasView::Impl::watermarkViewRenderArea() const {
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

QRegion SnowCanvasView::Impl::watermarkViewRenderRegion() const {
    if (!configuredWatermarkRenderArea.has_value()) {
        return QRegion(widget.rect());
    }
    const QRectF viewArea = watermarkViewRenderArea();
    return viewArea.isValid() && !viewArea.isEmpty()
               ? QRegion(viewArea.toAlignedRect().intersected(widget.rect()))
               : QRegion();
}

bool SnowCanvasView::Impl::watermarkEffectivelyVisible() const {
    updateWatermarkPresentationInfo();
    const WatermarkDisplayInfo& info = watermarkPresentationInfo;
    return info.watermark_text_len != 0 && info.watermark_color.a != 0 &&
           std::isfinite(info.watermark_font_size) && info.watermark_font_size > 0.0 &&
           std::isfinite(info.watermark_opacity) && info.watermark_opacity > 0.0;
}

bool SnowCanvasView::Impl::spotlightEffectivelyVisible() const {
    spotlightPresentationInfo = displayState.displayCache().spotlightInfo();
    if (spotlightPreview.has_value()) {
        spotlightPresentationInfo.color = snow_canvas_types::toEngineColor(spotlightPreview->color);
        spotlightPresentationInfo.opacity = spotlightPreview->opacity;
    }
    const SpotlightDisplayInfo& info = spotlightPresentationInfo;
    return info.active && info.color.a != 0 && std::isfinite(info.opacity) && info.opacity > 0.0;
}

void SnowCanvasView::Impl::setDecorationRenderAreas(const SnowCanvasDecorationRenderAreas& areas) {
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

void SnowCanvasView::Impl::setWatermarkRenderArea(const QRectF& canvasRect) {
    setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        std::optional<QRectF>(canvasRect.normalized()),
        configuredSpotlightRenderArea,
    });
}

void SnowCanvasView::setWatermarkRenderArea(const QRectF& canvasRect) {
    m_impl->setWatermarkRenderArea(canvasRect);
}

void SnowCanvasView::setDecorationRenderAreas(const SnowCanvasDecorationRenderAreas& areas) {
    m_impl->setDecorationRenderAreas(areas);
}

void SnowCanvasView::Impl::clearWatermarkRenderArea() {
    setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        std::nullopt,
        configuredSpotlightRenderArea,
    });
}

void SnowCanvasView::clearWatermarkRenderArea() {
    m_impl->clearWatermarkRenderArea();
}

bool SnowCanvasView::Impl::hasSpotlightRenderArea() const {
    return configuredSpotlightRenderArea.has_value();
}

bool SnowCanvasView::hasSpotlightRenderArea() const {
    return m_impl->hasSpotlightRenderArea();
}

QRectF SnowCanvasView::Impl::spotlightRenderArea() const {
    return configuredSpotlightRenderArea.value_or(QRectF());
}

QRectF SnowCanvasView::spotlightRenderArea() const {
    return m_impl->spotlightRenderArea();
}

QRectF SnowCanvasView::Impl::spotlightViewRenderArea() const {
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

QRegion SnowCanvasView::Impl::spotlightViewRenderRegion() const {
    const QRectF area = spotlightViewRenderArea();
    return area.isValid() && !area.isEmpty()
               ? QRegion(area.toAlignedRect().intersected(widget.rect()))
               : QRegion();
}

void SnowCanvasView::Impl::setSpotlightRenderArea(const QRectF& canvasRect) {
    setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        configuredWatermarkRenderArea,
        std::optional<QRectF>(canvasRect.normalized()),
    });
}

void SnowCanvasView::setSpotlightRenderArea(const QRectF& canvasRect) {
    m_impl->setSpotlightRenderArea(canvasRect);
}

void SnowCanvasView::Impl::clearSpotlightRenderArea() {
    setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        configuredWatermarkRenderArea,
        std::nullopt,
    });
}

void SnowCanvasView::clearSpotlightRenderArea() {
    m_impl->clearSpotlightRenderArea();
}

SnowCanvasCustomRenderer* SnowCanvasView::Impl::customRenderer() const {
    return installedCustomRenderer;
}

SnowCanvasCustomRenderer* SnowCanvasView::customRenderer() const {
    return m_impl->customRenderer();
}

void SnowCanvasView::Impl::setCustomRenderer(SnowCanvasCustomRenderer* renderer) {
    if (installedCustomRenderer == renderer) {
        return;
    }
    installedCustomRenderer = renderer;
    snow_canvas_filter_tile_cache::invalidateNamespace(&widget);
    widget.update();
}

void SnowCanvasView::setCustomRenderer(SnowCanvasCustomRenderer* renderer) {
    m_impl->setCustomRenderer(renderer);
}

QTransform SnowCanvasView::Impl::canvasToViewTransform() const {
    const SnowCanvasDisplayCache& cache = displayState.displayCache();
    return displayCacheHasSynced(cache) ? ::canvasToViewTransform(cache.sceneInfo())
                                        : QTransform(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0);
}

QTransform SnowCanvasView::canvasToViewTransform() const {
    return m_impl->canvasToViewTransform();
}

QRect SnowCanvasView::Impl::viewRectForCanvasRect(const QRectF& canvasRect, int paddingPx) const {
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

QRect SnowCanvasView::viewRectForCanvasRect(const QRectF& canvasRect, int paddingPx) const {
    return m_impl->viewRectForCanvasRect(canvasRect, paddingPx);
}

void SnowCanvasView::Impl::detachRuntime() {
    clearRetainedDisplayState();
    runtimeBinding.detachAttachment();
}

void SnowCanvasView::Impl::detachRuntimeForReplacement() {
    detachRuntime();
}

void SnowCanvasView::Impl::detachRuntimeOwner(SnowCanvasRuntime* owner) {
    if (!runtimeBinding.isRuntimeOwner(owner)) {
        return;
    }

    clearRetainedDisplayState();
    runtimeBinding.detachRuntimeOwner(owner);
}

void SnowCanvasView::Impl::attachRuntime(SnowRuntime runtime) {
    runtimeBinding.attachRuntime(runtime, displayState);
    setSurfaceSizeAndSync(widget.size(), false);
    refreshSerialNumberToolbar();
    widget.update();
}

void SnowCanvasView::Impl::clearRetainedDisplayState() {
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

void SnowCanvasView::Impl::clearRenderState() {
    snow_canvas_filter_tile_cache::invalidateNamespace(&widget);
    filterWorkspace.clear();
    penMaskAtlas.clear();
}

bool SnowCanvasView::Impl::hasViewport() const {
    return runtimeBinding.hasViewport();
}

bool SnowCanvasView::Impl::undo() {
    return applyMutationResult(snow_canvas_commands::undo(runtimeBinding.engine()));
}

bool SnowCanvasView::undo() {
    return m_impl->undo();
}

bool SnowCanvasView::Impl::redo() {
    return applyMutationResult(snow_canvas_commands::redo(runtimeBinding.engine()));
}

bool SnowCanvasView::redo() {
    return m_impl->redo();
}

bool SnowCanvasView::Impl::deleteSelected() {
    return applyMutationResult(snow_canvas_commands::deleteSelected(
        runtimeBinding.engine(), runtimeBinding.viewportHandle()));
}

bool SnowCanvasView::deleteSelected() {
    return m_impl->deleteSelected();
}

bool SnowCanvasView::Impl::deleteAllElements() {
    return applyMutationResult(snow_canvas_commands::deleteAllElements(
        runtimeBinding.engine(), runtimeBinding.viewportHandle()));
}

bool SnowCanvasView::deleteAllElements() {
    return m_impl->deleteAllElements();
}

bool SnowCanvasView::Impl::clearDocument() {
    const SnowCanvasTool previousTool = canvasTool();
    return runtimeBinding.clearDocumentPreservingViewports() && setCanvasTool(previousTool);
}

bool SnowCanvasView::clearDocument() {
    return m_impl->clearDocument();
}

bool SnowCanvasView::Impl::duplicateSelected(const QPointF& offset) {
    return applyMutationResult(snow_canvas_commands::duplicateSelected(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), offset.x(), offset.y()));
}

bool SnowCanvasView::duplicateSelected(const QPointF& offset) {
    return m_impl->duplicateSelected(offset);
}

bool SnowCanvasView::Impl::reorderSelected(SnowCanvasSelectionOrder order) {
    return applyMutationResult(snow_canvas_commands::reorderSelected(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        static_cast<std::uint32_t>(order)));
}

bool SnowCanvasView::reorderSelected(SnowCanvasSelectionOrder order) {
    return m_impl->reorderSelected(order);
}

bool SnowCanvasView::Impl::alignSelected(SnowCanvasSelectionAlignment alignment) {
    return applyMutationResult(snow_canvas_commands::alignSelected(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        static_cast<std::uint32_t>(alignment)));
}

bool SnowCanvasView::alignSelected(SnowCanvasSelectionAlignment alignment) {
    return m_impl->alignSelected(alignment);
}

bool SnowCanvasView::Impl::setSelectedOpacity(double opacity) {
    return applyMutationResult(snow_canvas_commands::setSelectedOpacity(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), opacity));
}

bool SnowCanvasView::setSelectedOpacity(double opacity) {
    return m_impl->setSelectedOpacity(opacity);
}

bool SnowCanvasView::Impl::adjustSelectedSerialNumbers(qint64 delta) {
    return applyMutationResult(snow_canvas_commands::adjustSelectedSerialNumbers(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(),
        static_cast<std::int64_t>(delta)));
}

bool SnowCanvasView::adjustSelectedSerialNumbers(qint64 delta) {
    return m_impl->adjustSelectedSerialNumbers(delta);
}

bool SnowCanvasView::Impl::createSerialNumberText() {
    const SnowStyleToolbarState& styleState = displayState.snapshot().styleToolbarState;
    snow_canvas_commands::CreateSerialNumberTextResult createResult =
        textInteraction.createSerialNumberText(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(), styleState.text_style,
            styleState.serial_number_style);
    if (!createResult.success) {
        return false;
    }

    // The editor session must begin from the label's styled scene item, the
    // same authority the serial drag's release path uses. Sync the creation
    // into the display cache first; beginning before the sync leaves the
    // editor a style-less preview whose commit strips the label's fill,
    // color, and stroke.
    syncChangedViewports(createResult.changedViewports.get());
    SnowCanvasWidgetTextInteraction::BeginResult beginResult =
        textInteraction.beginCreatedText(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                         createResult, displayState.displayCache());
    syncChangedViewports(beginResult.firstChangedViewports.get());
    if (beginResult.started) {
        refocusWidget();
    }
    return true;
}

bool SnowCanvasView::Impl::beginArrowText(const QPointF& viewPosition, bool selected) {
    if (!interactionEnabled() || textInteraction.isActive()) {
        return false;
    }
    auto result =
        textInteraction.beginArrow(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                   displayState.displayCache(), viewPosition, selected);
    syncChangedViewports(result.firstChangedViewports.get());
    syncChangedViewports(result.secondChangedViewports.get());
    if (result.started) {
        emit widget.styleToolbarStateChanged();
    }
    return result.started;
}

bool SnowCanvasView::editSelectedArrowText() {
    return m_impl->beginArrowText(QPointF(), true);
}

bool SnowCanvasView::createSerialNumberText() {
    return m_impl->createSerialNumberText();
}

bool SnowCanvasView::Impl::resetEditingState(bool restoreSelectTool) {
    const SnowCanvasTool previousTool = canvasTool();
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
    inputHandler.clearTransientState();
    if (restoreSelectTool) {
        requestedCanvasTool = SnowCanvasTool::Select;
        applyCanvasToolCursor(SnowCanvasTool::Select);
    } else if (previousTool != SnowCanvasTool::Select) {
        // The engine reset also selects the Select tool. Export callers need
        // the editing cleanup while preserving the user's active tool.
        if (!setCanvasTool(previousTool)) {
            return false;
        }
    }
    refreshSerialNumberToolbar();
    if (!previewRegion.isEmpty()) {
        widget.update(previewRegion);
    }
    return true;
}

bool SnowCanvasView::resetEditingState() {
    return m_impl->resetEditingState(true);
}

bool SnowCanvasView::resetEditingStatePreservingTool() {
    return m_impl->resetEditingState(false);
}

void SnowCanvasView::clearRenderState() {
    m_impl->clearRenderState();
}

bool SnowCanvasView::Impl::cancelActiveTextEditing() {
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

bool SnowCanvasView::cancelActiveTextEditing() {
    return m_impl->cancelActiveTextEditing();
}

bool SnowCanvasView::Impl::hasActiveTextEditing() const {
    return textInteraction.isActive();
}

bool SnowCanvasView::hasActiveTextEditing() const {
    return m_impl->hasActiveTextEditing();
}

void SnowCanvasView::Impl::beginTextStylePopupInteraction() {
    ++textStylePopupInteractionDepth;
    ++textStylePopupFocusRestoreGeneration;
}

void SnowCanvasView::beginTextStylePopupInteraction() {
    m_impl->beginTextStylePopupInteraction();
}

void SnowCanvasView::Impl::endTextStylePopupInteraction(QWidget* focusScope) {
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

void SnowCanvasView::endTextStylePopupInteraction(QWidget* focusScope) {
    m_impl->endTextStylePopupInteraction(focusScope);
}

void SnowCanvasView::Impl::clearTextStylePopupInteraction() {
    textStylePopupInteractionDepth = 0;
    textStylePopupFocusScope = nullptr;
    ++textStylePopupFocusRestoreGeneration;
}

void SnowCanvasView::Impl::handleApplicationStateChanged(Qt::ApplicationState state) {
    if (state == Qt::ApplicationActive) {
        return;
    }

    clearTextStylePopupInteraction();
    if (textInteraction.isActive()) {
        commitText(false);
    }
}

SnowCanvasWidgetTextInteraction::CommitResult
SnowCanvasView::Impl::commitText(bool refocusWidget, bool restoreExistingSelection) {
    SnowCanvasWidgetTextInteraction::CommitResult result =
        textInteraction.commit(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                               hasViewport(), displayState.displayCache(), refocusWidget);
    applyTextCommitResult(result, restoreExistingSelection);
    return result;
}

void SnowCanvasView::Impl::applyTextCommitResult(
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

void SnowCanvasView::Impl::applyTextEditorEventResult(
    SnowCanvasWidgetTextInteraction::EditorEventResult& result) {
    syncChangedViewports(result.changedViewports.get());
    restoreFinishedExistingTextSelection(result.finishedExistingEdit);
    if (result.sessionEnded) {
        clearTextStylePopupInteraction();
        emit widget.styleToolbarStateChanged();
    }
}

bool SnowCanvasView::Impl::restoreFinishedExistingTextSelection(
    const SnowCanvasWidgetTextInteraction::FinishedExistingEdit& edit) {
    SnowCanvasWidgetTextInteraction::SelectionRestoreResult result =
        textInteraction.restoreFinishedExistingSelection(runtimeBinding.engine(),
                                                         runtimeBinding.viewportHandle(), edit);
    if (result.restored) {
        syncChangedViewports(result.changedViewports.get());
    }
    return result.restored;
}

bool SnowCanvasView::Impl::beginText(const QPointF& viewPosition, bool allowCreate) {
    commitText();

    const SnowTextStyle textStyle = displayState.snapshot().styleToolbarState.text_style;
    SnowCanvasWidgetTextInteraction::BeginResult result =
        textInteraction.beginAt(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                displayState.displayCache(), viewPosition, textStyle, allowCreate);
    syncChangedViewports(result.firstChangedViewports.get());
    syncChangedViewports(result.secondChangedViewports.get());
    return result.started;
}

bool SnowCanvasView::Impl::beginSelectedText(const QPointF& viewPosition,
                                             bool requireSerialBoundText) {
    commitText();
    SnowCanvasWidgetTextInteraction::BeginResult result = textInteraction.beginSelectedAt(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache(),
        viewPosition, requireSerialBoundText);
    syncChangedViewports(result.firstChangedViewports.get());
    syncChangedViewports(result.secondChangedViewports.get());
    return result.started;
}

void SnowCanvasView::Impl::refreshStateFromEngine(bool emitSignals) {
    if (!hasViewport()) {
        return;
    }

    const quint64 generation = widget.autoFilterGeneration();
    if (autoFilterGeneration != generation) {
        autoFilterGeneration = generation;
        if (emitSignals) {
            emit widget.autoFilterRegionsChanged();
        }
    }
    snow_canvas_state::Changes changes;
    if (!displayState.refreshState(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                                   &changes)) {
        return;
    }

    refreshToolCursorStyle();
    if (emitSignals) {
        emitChangedStateSignals(changes);
    }
    refreshSerialNumberToolbar();
}

void SnowCanvasView::Impl::syncAfterEngineMutation() {
    syncAfterEngineMutation(true);
}

void SnowCanvasView::Impl::syncAfterEngineMutation(bool emitSignals) {
    if (auto* owner = runtimeBinding.runtimeOwner())
        snow_canvas_runtime::Access::smartErase(*owner).sync(runtimeBinding.engine());
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
    refreshToolCursorStyle();
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

void SnowCanvasView::Impl::syncChangedViewports(SnowChangedViewportList changedViewports) {
    const auto labels =
        textInteraction.measureArrowText(runtimeBinding.engine(), runtimeBinding.viewportHandle());
    runtimeBinding.syncChangedViewports(labels.changedViewports.get());
    const auto serialLabels = textInteraction.measureSerialLabelLayout(
        runtimeBinding.engine(), runtimeBinding.viewportHandle());
    runtimeBinding.syncChangedViewports(serialLabels.changedViewports.get());
    runtimeBinding.syncChangedViewports(changedViewports);
}

bool SnowCanvasView::Impl::applyMutationResult(const snow_canvas_commands::MutationResult& result) {
    if (!result.success) {
        return false;
    }

    syncChangedViewports(result.changedViewports.get());
    refocusWidget();
    return true;
}

bool SnowCanvasView::Impl::applyPairedMutationResult(
    const snow_canvas_commands::PairedMutationResult& result) {
    syncChangedViewports(result.firstChangedViewports.get());
    if (!result.success) {
        return false;
    }

    syncChangedViewports(result.secondChangedViewports.get());
    refocusWidget();
    return true;
}

void SnowCanvasView::Impl::emitChangedStateSignals(const snow_canvas_state::Changes& changes) {
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

void SnowCanvasView::Impl::refreshToolCursorStyle() {
    const auto& cursorStyle = displayState.snapshot().styleToolbarState;
    const bool filterCursor = displayState.snapshot().activeTool == SNOW_ACTIVE_TOOL_PEN_FILTER;
    const auto& stroke = cursorStyle.shape_style.stroke;
    cursorController.configureStrokeCursor(
        (filterCursor ? cursorStyle.filter_style.stroke_width
                      : cursorStyle.shape_style.stroke_width) *
            displayState.displayCache().sceneInfo().camera_zoom,
        filterCursor ? std::nullopt
                     : std::optional<QColor>(QColor(stroke.r, stroke.g, stroke.b, stroke.a)));
}

void SnowCanvasView::Impl::applyCanvasToolCursor(SnowCanvasTool tool) {
    const auto cursor = baselineCursorForCanvasTool(tool);
    if (cursor.has_value()) {
        cursorController.setEngineCursor(*cursor);
        return;
    }
    cursorController.clearCursor(SnowCanvasCursorLayer::CanvasTool);
}

void SnowCanvasView::Impl::refocusWidget() {
    widget.setFocus(Qt::OtherFocusReason);
}

bool SnowCanvasView::Impl::setSurfaceSizeAndSync(const QSize& size, bool emitSignals) {
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

bool SnowCanvasView::Impl::setCameraAndSync(double centerX, double centerY, double zoom) {
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

bool SnowCanvasView::Impl::paint(QPainter& painter, const QRegion& exposedRegion) {
    if (!hasExposedRect(exposedRegion, widget.rect())) {
        return false;
    }

    const SnowCanvasDisplayCache& cache = displayState.displayCache();

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(exposedRegion, Qt::IntersectClip);
    snow_canvas_compositor::Frame frame = buildPaintFrame();
    if (canvasContentIsVisible) {
        const SceneDisplayInfo& sceneInfo = cache.sceneInfo();
        const std::uint64_t contentKey =
            sceneCacheContentKey(sceneInfo, installedCustomRenderer, canvasClearBackgroundEnabled,
                                 painterDevicePixelRatio(painter, widget), widget.size());

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
        if (auto* owner = runtimeBinding.runtimeOwner())
            sceneRequest.smartErase = owner->smartEraseSnapshot();
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

snow_canvas_compositor::Frame SnowCanvasView::Impl::buildPaintFrame() const {
    const SnowCanvasDisplayCache& cache = displayState.displayCache();
    snow_canvas_compositor::Frame frame =
        snow_canvas_widget_paint_frame::build(snow_canvas_widget_paint_frame::Request{
            &cache,
            widget.rect(),
            canvasClearBackgroundEnabled,
            dirtyRectsVisible,
            const_cast<snow_canvas_filter_render::RenderWorkspace*>(&filterWorkspace),
        });
    frame.penMaskAtlas = &const_cast<SnowCanvasView::Impl*>(this)->penMaskAtlas;
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

void SnowCanvasView::Impl::updateWatermarkPresentationInfo() const {
    watermarkPresentationInfo = displayState.displayCache().watermarkInfo();
    if (watermarkPreview.has_value()) {
        applyWatermarkConfig(watermarkPresentationInfo, *watermarkPreview);
    }
}

SnowCanvasRenderContext SnowCanvasView::Impl::renderContext(QPainter& painter,
                                                            const QRegion& exposedRegion) const {
    const SceneDisplayInfo& sceneInfo = displayState.displayCache().sceneInfo();
    return SnowCanvasRenderContext{
        widget.rect(),
        exposedRegion.intersected(widget.rect()),
        ::canvasToViewTransform(sceneInfo),
        painterDevicePixelRatio(painter, widget),
    };
}

void SnowCanvasView::Impl::renderBeforeCanvas(QPainter& painter,
                                              const SnowCanvasRenderContext& context) const {
    if (installedCustomRenderer == nullptr) {
        return;
    }
    painter.save();
    installedCustomRenderer->renderBeforeCanvas(painter, context);
    painter.restore();
}

void SnowCanvasView::Impl::renderAfterCanvas(QPainter& painter,
                                             const SnowCanvasRenderContext& context) const {
    if (installedCustomRenderer == nullptr) {
        return;
    }
    painter.save();
    installedCustomRenderer->renderAfterCanvas(painter, context);
    painter.restore();
}

bool SnowCanvasView::Impl::handleMousePress(QMouseEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }

    clearTextStylePopupInteraction();

    const bool textEditorActive = textInteraction.isActive();
    const bool pointerInsideTextEditor =
        textInteraction.editorContains(displayState.displayCache(), event->position());
    const auto selectionTarget = snow_canvas_widget_selection_hit_testing::selectionInteractionAt(
        displayState.displayCache(), event->position());
    using snow_canvas_widget_selection_hit_testing::SelectionInteractionTarget;
    const bool pointerOverSelectionInteraction =
        selectionTarget != SelectionInteractionTarget::None &&
        (!textEditorActive || textInteraction.hasSelectionInteraction());
    const bool pointerHitsSelectedText =
        snow_canvas_widget_pointer_flow::isSelectedTextCopyGesture(event->button(),
                                                                   event->modifiers()) &&
        selectionTarget != SelectionInteractionTarget::Handle &&
        hitsSelectedText(runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                         widget.canvasToViewTransform().inverted().map(event->position()));
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
            pointerHitsSelectedText,
            selectionTarget,
        });
    if (plan.shouldHandleEditorPress) {
        return textInteraction.handleEditorMousePress(event, displayState.displayCache(),
                                                      widget.font());
    }
    if (!plan.shouldFocusWidget) {
        return false;
    }

    widget.setFocus(Qt::MouseFocusReason);
    suppressNextTextToolCreate = false;
    restoredSelectionForNextTextToolPress = false;
    bool canDispatchAfterTextCommit = true;
    if (plan.shouldCommitTextEditor) {
        const SnowCanvasWidgetTextInteraction::CommitResult commitResult =
            commitText(true, plan.shouldRestoreSelectionOnCommit);
        if (plan.dispatchAfterCommitRequiresRestoredSelection) {
            canDispatchAfterTextCommit = commitResult.restoredExistingSelection ||
                                         plan.suppressedTextCreateRestoredSelection;
        }
    }

    if (plan.shouldBeginText) {
        if (plan.allowCreateText && beginArrowText(event->position(), false)) {
            event->accept();
            return true;
        }
        if (beginText(event->position(), false)) {
            event->accept();
            return true;
        }
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
    const bool handled =
        dispatchInput(event, snow_canvas_input::makePointerInput(*event, SNOW_POINTER_EVENT_DOWN));
    if (handled && plan.shouldBeginText && plan.allowCreateText) {
        const SnowTextStyle textStyle = displayState.snapshot().styleToolbarState.text_style;
        SnowCanvasWidgetTextInteraction::BeginResult draft =
            textInteraction.beginRequestedNewTextDraft(
                runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                displayState.displayCache(), event->position(), textStyle);
        syncChangedViewports(draft.firstChangedViewports.get());
        syncChangedViewports(draft.secondChangedViewports.get());
    }
    return handled;
}

bool SnowCanvasView::Impl::handleMouseMove(QMouseEvent* event) {
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

bool SnowCanvasView::Impl::queueEraserMove(QMouseEvent* event, const SnowInputEvent& input) {
    pendingEraserMove = input;
    if (!eraserMoveFlushScheduled) {
        eraserMoveFlushScheduled = true;
        QTimer::singleShot(0, &widget, [this]() { flushEraserMove(); });
    }
    event->accept();
    return true;
}

void SnowCanvasView::Impl::flushEraserMove() {
    eraserMoveFlushScheduled = false;
    if (!pendingEraserMove.has_value()) {
        return;
    }
    const SnowInputEvent input = *pendingEraserMove;
    pendingEraserMove.reset();
    static_cast<void>(processInput(input));
    emit widget.eraserMoveFrameProcessed();
}

bool SnowCanvasView::Impl::handleMouseDoubleClick(QMouseEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }
    if (textInteraction.isActive()) {
        event->accept();
        return true;
    }
    flushLiveStrokeMoves();
    flushEraserMove();
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier &&
        beginArrowText(event->position(), false)) {
        event->accept();
        return true;
    }
    return dispatchInput(
        event, snow_canvas_input::makePointerInput(*event, SNOW_POINTER_EVENT_DOUBLE_CLICK));
}

bool SnowCanvasView::Impl::queueLiveStrokeMove(QMouseEvent* event, const SnowInputEvent& input,
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

void SnowCanvasView::Impl::flushLiveStrokeMoves() {
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

bool SnowCanvasView::Impl::handleMouseRelease(QMouseEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }
    if (textInteraction.handleEditorMouseRelease(event)) {
        return true;
    }
    flushLiveStrokeMoves();
    flushEraserMove();
    const bool handled =
        dispatchInput(event, snow_canvas_input::makePointerInput(*event, SNOW_POINTER_EVENT_UP));
    if (handled && event->button() == Qt::LeftButton) {
        auto result = textInteraction.beginRequestedTextEdit(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache());
        syncChangedViewports(result.firstChangedViewports.get());
        if (result.started) {
            refocusWidget();
            emit widget.styleToolbarStateChanged();
        }
    }
    return handled;
}

void SnowCanvasView::Impl::beginMiddleClickTracking(const QPointF& position) {
    middleClickPressPosition = position;
    middleClickCandidate = true;
}

void SnowCanvasView::Impl::updateMiddleClickTracking(const QPointF& position) {
    if (middleClickCandidate && middleClickPressPosition.has_value() &&
        QLineF(*middleClickPressPosition, position).length() > QApplication::startDragDistance()) {
        middleClickCandidate = false;
    }
}

bool SnowCanvasView::Impl::finishMiddleClickTracking() {
    const bool candidate = middleClickCandidate;
    middleClickCandidate = false;
    middleClickPressPosition.reset();
    return candidate;
}

void SnowCanvasView::Impl::cancelMiddleClickTracking() {
    middleClickCandidate = false;
    middleClickPressPosition.reset();
}

bool SnowCanvasView::Impl::handleEnter(QEnterEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }
    return dispatchInput(event,
                         snow_canvas_input::makePointerInput(
                             event->position(), Qt::NoButton, QGuiApplication::mouseButtons(),
                             QGuiApplication::keyboardModifiers(), SNOW_POINTER_EVENT_ENTER));
}

bool SnowCanvasView::Impl::handleLeave(QEvent* event) {
    if (!interactionEnabled()) {
        return false;
    }
    return dispatchInput(
        event, snow_canvas_input::makePointerInput(widget.mapFromGlobal(QCursor::pos()),
                                                   Qt::NoButton, QGuiApplication::mouseButtons(),
                                                   QGuiApplication::keyboardModifiers(),
                                                   SNOW_POINTER_EVENT_LEAVE));
}

bool SnowCanvasView::Impl::handleWheel(QWheelEvent* event) {
    if (event == nullptr || !interactionEnabled()) {
        return false;
    }

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
        return fontWheelTarget() == FontWheelTarget::SerialNumber
                   ? stepSerialNumberFontSize(plan.increase)
                   : stepTextFontSize(plan.increase);
    }

    if (!wheelZoomEnabled()) {
        event->accept();
        return true;
    }
    return dispatchInput(event, snow_canvas_input::makeWheelInput(*event));
}

// Font-size wheel steps follow the style toolbar source so the wheel and the
// toolbar agree: an active text draft, selected text, or selected serial
// badges. With no text or badge selected, that source is the active tool's
// default style. Other selected kinds fall back to the active font tool.
SnowCanvasView::Impl::FontWheelTarget SnowCanvasView::Impl::fontWheelTarget() const {
    if (textInteraction.isActive()) {
        return FontWheelTarget::Text;
    }
    switch (displayState.snapshot().styleToolbarState.source) {
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_TEXT:
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_TEXT:
        return FontWheelTarget::Text;
    case SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_SERIAL_NUMBER:
    case SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_SERIAL_NUMBER:
        return FontWheelTarget::SerialNumber;
    default:
        break;
    }
    return canvasTool() == SnowCanvasTool::SerialNumber ? FontWheelTarget::SerialNumber
                                                        : FontWheelTarget::Text;
}

bool SnowCanvasView::Impl::stepSerialNumberFontSize(bool increase) {
    SnowSerialNumberStyle style = displayState.snapshot().styleToolbarState.serial_number_style;
    const double nextFontSize =
        snow_canvas_text_measurement::steppedFontSize(style.font_size, increase);
    if (std::abs(nextFontSize - style.font_size) <= std::numeric_limits<double>::epsilon()) {
        return true;
    }

    style.font_size = nextFontSize;
    return applyMutationResult(snow_canvas_commands::setSerialNumberStyle(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), style));
}

bool SnowCanvasView::Impl::stepTextFontSize(bool increase) {
    SnowCanvasWidgetTextInteraction::StyleChangeResult result = textInteraction.stepFontSize(
        runtimeBinding.engine(), runtimeBinding.viewportHandle(), displayState.displayCache(),
        displayState.snapshot().styleToolbarState.text_style, increase);
    if (!result.success) {
        return false;
    }

    syncChangedViewports(result.changedViewports.get());
    if (result.toolbarStateChanged) {
        emit widget.styleToolbarStateChanged();
    }
    return true;
}

bool SnowCanvasView::Impl::handleKeyPress(QKeyEvent* event) {
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

    if (event != nullptr && !event->isAutoRepeat() && event->modifiers() == Qt::NoModifier &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        beginArrowText(QPointF(), true)) {
        event->accept();
        return true;
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

bool SnowCanvasView::Impl::handleKeyRelease(QKeyEvent* event) {
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

bool SnowCanvasView::Impl::handleInputMethodEvent(QInputMethodEvent* event) {
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

QVariant SnowCanvasView::Impl::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (!interactionEnabled()) {
        return widget.defaultInputMethodQuery(query);
    }
    QVariant value =
        textInteraction.inputMethodQuery(query, displayState.displayCache(), widget.font());
    if (value.isValid()) {
        return value;
    }
    return widget.defaultInputMethodQuery(query);
}

QVariant SnowCanvasView::inputMethodQuery(Qt::InputMethodQuery query) const {
    return m_impl->inputMethodQuery(query);
}

void SnowCanvasView::Impl::handleFocusOut() {
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

void SnowCanvasView::Impl::handleResize(const QSize& size) {
    snow_canvas_filter_tile_cache::invalidateNamespace(&widget);
    setSurfaceSizeAndSync(size, true);
}

void SnowCanvasView::Impl::setShowDirtyRects(bool show) {
    if (dirtyRectsVisible == show) {
        return;
    }
    dirtyRectsVisible = show;

    emit widget.showDirtyRectsChanged();

    const QRegion repaintRegion = displayState.dirtyVisualizationRegion();
    snow_canvas_widget_repaint::updateCoalesced(widget, repaintRegion);
}

void SnowCanvasView::setShowDirtyRects(bool show) {
    m_impl->setShowDirtyRects(show);
}

namespace {
SnowAutoFilterBounds autoFilterBounds(const QRectF& r) {
    return {r.left(), r.top(), r.right(), r.bottom()};
}
QRectF autoFilterRect(const SnowAutoFilterBounds& r) {
    return QRectF(QPointF(r.left, r.top), QPointF(r.right, r.bottom));
}
} // namespace
quint64 SnowCanvasView::Impl::readAutoFilterGeneration() const {
    uint64_t generation = 0;
    uint8_t identified = 0;
    SnowAutoFilterBounds bounds{};
    size_t count = 0;
    snow_runtime_get_auto_filter_regions(runtimeBinding.engine(), &generation, &identified, &bounds,
                                         nullptr, 0, &count);
    return generation;
}
std::optional<SnowCanvasAutoFilterRecord> SnowCanvasView::Impl::autoFilterRegions() const {
    uint64_t generation = 0;
    uint8_t identified = 0;
    SnowAutoFilterBounds bounds{};
    size_t count = 0;
    const auto runtime = runtimeBinding.engine();
    if (snow_runtime_get_auto_filter_regions(runtime, &generation, &identified, &bounds, nullptr, 0,
                                             &count) != SNOW_OK ||
        !identified) {
        return std::nullopt;
    }
    std::vector<SnowAutoFilterRegion> regions(count);
    if (snow_runtime_get_auto_filter_regions(runtime, &generation, &identified, &bounds,
                                             regions.data(), regions.size(), &count) != SNOW_OK) {
        return std::nullopt;
    }
    SnowCanvasAutoFilterRecord record;
    record.sourceBounds = autoFilterRect(bounds);
    for (const auto& region : regions) {
        record.regions.append(
            {region.id, autoFilterRect(region.bounds),
             QString::fromUtf8(reinterpret_cast<const char*>(region.category),
                               static_cast<qsizetype>(
                                   strnlen(reinterpret_cast<const char*>(region.category), 128)))});
    }
    return record;
}
bool SnowCanvasView::Impl::setAutoFilterRegions(
    const std::optional<SnowCanvasAutoFilterRecord>& record) {
    std::vector<SnowAutoFilterRegion> regions;
    SnowAutoFilterBounds bounds{};
    if (record) {
        bounds = autoFilterBounds(record->sourceBounds);
        for (const auto& region : record->regions) {
            SnowAutoFilterRegion value{};
            value.id = region.id;
            value.bounds = autoFilterBounds(region.bounds);
            const QByteArray category = region.category.toUtf8();
            if (category.size() > 127) {
                return false;
            }
            std::memcpy(value.category, category.constData(), static_cast<size_t>(category.size()));
            regions.push_back(value);
        }
    }
    SnowChangedViewportList changed = nullptr;
    snow_canvas_commands::MutationResult result;
    result.success =
        snow_viewport_set_auto_filter_regions(
            runtimeBinding.engine(), runtimeBinding.viewportHandle(), record ? &bounds : nullptr,
            regions.data(), regions.size(), &changed) == SNOW_OK;
    result.changedViewports.reset(changed);
    return applyMutationResult(std::move(result));
}
bool SnowCanvasView::Impl::fillAutoFilterCategory(const QString& category) {
    const QByteArray bytes = category.toUtf8();
    SnowChangedViewportList changed = nullptr;
    snow_canvas_commands::MutationResult result;
    result.success = snow_viewport_fill_auto_filter_category(
                         runtimeBinding.engine(), runtimeBinding.viewportHandle(),
                         reinterpret_cast<const uint8_t*>(bytes.constData()),
                         static_cast<size_t>(bytes.size()), &changed) == SNOW_OK;
    result.changedViewports.reset(changed);
    return applyMutationResult(std::move(result));
}

quint64 SnowCanvasView::autoFilterGeneration() const {
    return m_impl->readAutoFilterGeneration();
}
std::optional<SnowCanvasAutoFilterRecord> SnowCanvasView::autoFilterRegions() const {
    return m_impl->autoFilterRegions();
}
bool SnowCanvasView::setAutoFilterRegions(const std::optional<SnowCanvasAutoFilterRecord>& record) {
    return m_impl->setAutoFilterRegions(record);
}
bool SnowCanvasView::fillAutoFilterCategory(const QString& category) {
    return m_impl->fillAutoFilterCategory(category);
}

void SnowCanvasView::Impl::setBaseImageSources(const QList<SnowCanvasBaseImageSource>& sources) {
    if (auto* owner = runtimeBinding.runtimeOwner()) {
        auto& coordinator = snow_canvas_runtime::Access::smartErase(*owner);
        coordinator.setSources(this, sources);
        coordinator.sync(runtimeBinding.engine());
        smartEraseChanged();
    }
}

void SnowCanvasView::setBaseImageSources(const QList<SnowCanvasBaseImageSource>& sources) {
    m_impl->setBaseImageSources(sources);
}

SnowCanvasView::SnowCanvasView(SnowCanvasHostCallbacks host, QObject* parent)
    : QObject(parent), m_host(std::move(host)), m_impl(std::make_unique<Impl>(*this)) {
    m_impl->initializeWidget();
    m_impl->initializeViewport();
}

SnowCanvasView::SnowCanvasView(SnowCanvasRuntime& runtime, SnowCanvasHostCallbacks host,
                               QObject* parent)
    : QObject(parent), m_host(std::move(host)), m_impl(std::make_unique<Impl>(*this, runtime)) {
    m_impl->initializeWidget();
    m_impl->initializeViewport();
}

bool SnowCanvasView::setSurfaceMetrics(const QSize& physicalSize, qreal ratio) {
    if (physicalSize.width() < 0 || physicalSize.height() < 0 || !std::isfinite(ratio) ||
        ratio <= 0)
        return false;
    const QSize logicalSize(qCeil(physicalSize.width() / ratio),
                            qCeil(physicalSize.height() / ratio));
    if (physicalSize == m_physicalSize && ratio == m_devicePixelRatio && logicalSize == m_size)
        return true;
    m_physicalSize = physicalSize;
    m_devicePixelRatio = ratio;
    m_size = logicalSize;
    m_impl->handleResize(m_size);
    m_impl->refreshCursorDevicePixelRatio();
    update();
    return true;
}

void SnowCanvasView::setLogicalSurfaceSize(const QSize& size, qreal ratio) {
    m_size = size;
    m_devicePixelRatio = ratio;
    m_physicalSize = QSize(qRound(size.width() * ratio), qRound(size.height() * ratio));
    m_impl->handleResize(size);
    m_impl->refreshCursorDevicePixelRatio();
}

bool SnowCanvasView::render(QPainter& painter, const QRegion& dirty) {
    if (!painter.isActive())
        return false;
    painter.save();
    painter.setClipRect(QRectF(QPointF(), logicalExtent()), Qt::IntersectClip);
    const bool painted = m_impl->paint(painter, dirty);
    if (painted)
        emit overlayPaintRequested(&painter, dirty);
    painter.restore();
    return painted;
}

bool SnowCanvasView::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (canvasTool() == SnowCanvasTool::AutoFilter && mouse->button() == Qt::LeftButton)
            emit autoFilterInteractionStarting();
        if (mouse->button() == Qt::MiddleButton)
            m_impl->beginMiddleClickTracking(mouse->position());
        const bool handled = m_impl->handleMousePress(mouse);
        if (mouse->button() == Qt::MiddleButton && handled)
            m_impl->cancelMiddleClickTracking();
        return handled;
    }
    case QEvent::MouseButtonDblClick: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (m_impl->handleMouseDoubleClick(mouse))
            return true;
        if (mouse->button() == Qt::LeftButton) {
            event->accept();
            emit unhandledLeftDoubleClick();
            return true;
        }
        return false;
    }
    case QEvent::MouseMove: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        m_impl->updateMiddleClickTracking(mouse->position());
        return m_impl->handleMouseMove(mouse);
    }
    case QEvent::MouseButtonRelease: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const bool middleClick =
            mouse->button() == Qt::MiddleButton && m_impl->finishMiddleClickTracking();
        const bool handled = m_impl->handleMouseRelease(mouse);
        if (middleClick && !handled) {
            event->accept();
            emit unhandledMiddleClick();
            return true;
        }
        return handled;
    }
    case QEvent::Enter:
        return m_impl->handleEnter(static_cast<QEnterEvent*>(event));
    case QEvent::Leave:
        return m_impl->handleLeave(event);
    case QEvent::Wheel:
        return m_impl->handleWheel(static_cast<QWheelEvent*>(event));
    case QEvent::KeyPress:
        return m_impl->handleKeyPress(static_cast<QKeyEvent*>(event));
    case QEvent::KeyRelease:
        return m_impl->handleKeyRelease(static_cast<QKeyEvent*>(event));
    case QEvent::InputMethod:
        return m_impl->handleInputMethodEvent(static_cast<QInputMethodEvent*>(event));
    case QEvent::FocusOut:
        m_impl->handleFocusOut();
        return false;
    case QEvent::InputMethodQuery: {
        auto* query = static_cast<QInputMethodQueryEvent*>(event);
        for (quint32 bit = 1; bit != 0; bit <<= 1) {
            const auto item = static_cast<Qt::InputMethodQuery>(bit);
            if (query->queries().testFlag(item))
                query->setValue(item, inputMethodQuery(item));
        }
        return true;
    }
    default:
        return QObject::event(event);
    }
}

void SnowCanvasView::update(const QRegion& region) {
    if (m_host.repaint)
        m_host.repaint(region.intersected(rect()));
}
void SnowCanvasView::update() {
    update(QRegion(rect()));
}
void SnowCanvasView::setFocus(Qt::FocusReason reason) {
    if (m_host.focus)
        m_host.focus(reason);
}
void SnowCanvasView::setCursor(const QCursor& cursor) {
    m_cursor = cursor;
    if (m_host.cursor)
        m_host.cursor(m_cursor);
}
void SnowCanvasView::unsetCursor() {
    m_cursor.reset();
    if (m_host.cursor)
        m_host.cursor(m_cursor);
}
void SnowCanvasView::setInputMethodEnabled(bool enabled) {
    m_inputMethodEnabled = enabled;
    if (m_host.inputMethodEnabled)
        m_host.inputMethodEnabled(enabled);
}
