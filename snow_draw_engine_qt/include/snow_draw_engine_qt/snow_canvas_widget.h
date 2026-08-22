#pragma once

#include <QRect>
#include <QRectF>
#include <QTransform>
#include <QVariant>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>

#include "snow_draw_engine_qt/snow_canvas_types.h"

class QCursor;
class QEnterEvent;
class QEvent;
class QFocusEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPointF;
class QResizeEvent;
class QWheelEvent;
class SnowCanvasRuntime;
class SnowCanvasCustomRenderer;

struct SnowCanvasDecorationRenderAreas {
    std::optional<QRectF> watermark;
    std::optional<QRectF> spotlight;
};

class SnowCanvasWidget : public QWidget {
    Q_OBJECT

  public:
    explicit SnowCanvasWidget(QWidget* parent = nullptr);
    explicit SnowCanvasWidget(SnowCanvasRuntime& runtime, QWidget* parent = nullptr);
    ~SnowCanvasWidget() override;

    SnowCanvasTool canvasTool() const;
    bool setCanvasTool(SnowCanvasTool tool);

    void setCursorForLayer(SnowCanvasCursorLayer layer, const QCursor& cursor);
    void clearCursorForLayer(SnowCanvasCursorLayer layer);

    SnowCanvasStyleToolbarState canvasStyleToolbarState() const;
    SnowCanvasSerialNumberToolbarState serialNumberToolbarState() const;
    SnowCanvasWatermarkConfig canvasWatermarkConfig() const;
    bool setCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config);
    void previewCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config);
    SnowCanvasSpotlightConfig canvasSpotlightConfig() const;
    bool setCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config);
    void previewCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config);
    bool setCanvasShapeStylePatch(const SnowCanvasShapeStyle& style, quint32 properties,
                                  SnowCanvasShapeKind kind);
    bool setCanvasFilterStyle(const SnowCanvasFilterStyle& style, quint32 properties);
    bool setCanvasRectangleShapeStyle(const SnowCanvasRectangleShapeStyle& style);
    bool setCanvasTextStyle(const SnowCanvasTextStyle& style);
    bool setCanvasSerialNumberStyle(const SnowCanvasSerialNumberStyle& style);

    SnowCanvasHistoryState canvasHistoryState() const;

    SnowCanvasSnapConfig canvasSnapConfig() const;
    bool setCanvasSnapConfig(const SnowCanvasSnapConfig& config);

    SnowCanvasGridConfig canvasGridConfig() const;
    bool setCanvasGridConfig(const SnowCanvasGridConfig& config);

    bool undo();
    bool redo();
    bool deleteSelected();
    bool duplicateSelected(const QPointF& offset = QPointF(12.0, 12.0));
    bool reorderSelected(SnowCanvasSelectionOrder order);
    bool setSelectedOpacity(double opacity);
    bool adjustSelectedSerialNumbers(qint64 delta);
    bool createSerialNumberText();
    // Commits active text, clears transient editing state and selection, and
    // restores the select tool.
    bool resetEditingState();
    // Discards an uncommitted inline text draft without adding it to history.
    bool cancelActiveTextEditing();
    [[nodiscard]] bool hasActiveTextEditing() const;
    // Keeps an active inline text draft alive while a text-style popup owns focus.
    void beginTextStylePopupInteraction();
    // Ends a text-style popup interaction and restores text input when appropriate.
    void endTextStylePopupInteraction(QWidget* focusScope);

    bool interactionEnabled() const;
    void setInteractionEnabled(bool enabled);
    bool wheelZoomEnabled() const;
    void setWheelZoomEnabled(bool enabled);
    // Controls engine-owned scene, overlay, editor, and auxiliary content.
    // Custom renderer passes and background clearing remain active.
    [[nodiscard]] bool canvasContentVisible() const;
    void setCanvasContentVisible(bool visible);
    [[nodiscard]] bool retainedSceneCacheEnabled() const;
    void setRetainedSceneCacheEnabled(bool enabled);
    // Releases renderer-owned caches while preserving the canvas runtime,
    // interaction state, and display model.
    void releaseRetainedRenderResources();
    bool clearBackgroundEnabled() const;
    void setClearBackgroundEnabled(bool enabled);

    bool showDirtyRects() const;
    std::uint64_t viewportId() const;

    bool setViewportCamera(double centerX, double centerY, double zoom);
    // Limits the viewport-anchored watermark to a canvas-space area. An empty
    // area renders no watermark; clearWatermarkRenderArea() restores the full
    // viewport behavior.
    bool hasWatermarkRenderArea() const;
    QRectF watermarkRenderArea() const;
    void setWatermarkRenderArea(const QRectF& canvasRect);
    void clearWatermarkRenderArea();
    void setDecorationRenderAreas(const SnowCanvasDecorationRenderAreas& areas);
    bool hasSpotlightRenderArea() const;
    QRectF spotlightRenderArea() const;
    void setSpotlightRenderArea(const QRectF& canvasRect);
    void clearSpotlightRenderArea();
    // The renderer is borrowed and must be detached before it is destroyed.
    SnowCanvasCustomRenderer* customRenderer() const;
    void setCustomRenderer(SnowCanvasCustomRenderer* renderer);
    [[nodiscard]] QTransform canvasToViewTransform() const;
    QRect viewRectForCanvasRect(const QRectF& canvasRect, int paddingPx = 0) const;
    void updateCanvasRect(const QRectF& canvasRect, int paddingPx = 0);

  public slots:
    void setShowDirtyRects(bool show);

  signals:
    void activeToolChanged();
    void styleToolbarStateChanged();
    void historyStateChanged();
    void snapConfigChanged();
    void gridConfigChanged();
    void watermarkPreviewApplied();
    void spotlightPreviewApplied();
    void freeDrawMoveBatchProcessed(quint32 inputCount, quint32 dispatchedCount);
    void eraserMoveFrameProcessed();
    void unhandledLeftDoubleClick();
    void unhandledMiddleClick();
    void showDirtyRectsChanged();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
