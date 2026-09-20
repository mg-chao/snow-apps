#pragma once
#include "snow_draw_engine_qt/snow_canvas_smart_erase.h"

#include <QRect>
#include <QRectF>
#include <QTransform>
#include <QVariant>
#include <QObject>
#include <QCursor>
#include <QFont>
#include <QRegion>
#include <functional>

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

class QWidget;
class QWindow;
class QPainter;

struct SnowCanvasHostCallbacks {
    std::function<void(const QRegion&)> repaint;
    std::function<void(Qt::FocusReason)> focus;
    std::function<void()> clearFocus;
    std::function<bool()> hasFocus;
    std::function<void(bool)> capture;
    std::function<void(const std::optional<QCursor>&)> cursor;
    std::function<void(bool)> inputMethodEnabled;
    std::function<QVariant(Qt::InputMethodQuery)> inputMethodQuery;
    std::function<QFont()> font;
    std::function<QWindow*()> window;
    std::function<QPointF(const QPointF&)> mapFromGlobal;
    std::function<QPointF(const QPointF&)> mapToGlobal;
};

struct SnowCanvasDecorationRenderAreas {
    std::optional<QRectF> watermark;
    std::optional<QRectF> spotlight;
};

class SnowCanvasView : public QObject {
    Q_OBJECT

  public:
    explicit SnowCanvasView(SnowCanvasHostCallbacks host = {}, QObject* parent = nullptr);
    explicit SnowCanvasView(SnowCanvasRuntime& runtime, SnowCanvasHostCallbacks host = {},
                            QObject* parent = nullptr);
    ~SnowCanvasView() override;
    bool setSurfaceMetrics(const QSize& physicalSize, qreal devicePixelRatio);
    void setLogicalSurfaceSize(const QSize& size, qreal devicePixelRatio);
    QSize physicalSize() const {
        return m_physicalSize;
    }
    QSizeF logicalExtent() const {
        return QSizeF(m_physicalSize) / m_devicePixelRatio;
    }
    QSize size() const {
        return m_size;
    }
    QRect rect() const {
        return QRect(QPoint(), m_size);
    }
    int width() const {
        return m_size.width();
    }
    int height() const {
        return m_size.height();
    }
    qreal devicePixelRatioF() const {
        return m_devicePixelRatio;
    }
    QFont font() const {
        return m_host.font ? m_host.font() : QFont();
    }
    QWindow* windowHandle() const {
        return m_host.window ? m_host.window() : nullptr;
    }
    QPointF mapFromGlobal(const QPointF& point) const {
        return m_host.mapFromGlobal ? m_host.mapFromGlobal(point) : point;
    }
    QPointF mapToGlobal(const QPointF& point) const {
        return m_host.mapToGlobal ? m_host.mapToGlobal(point) : point;
    }
    void clearFocus() {
        if (m_host.clearFocus)
            m_host.clearFocus();
    }
    void update();
    void update(const QRegion& region);
    void setFocus(Qt::FocusReason reason = Qt::OtherFocusReason);
    bool hasFocus() const {
        return m_host.hasFocus && m_host.hasFocus();
    }
    void grabMouse() {
        if (m_host.capture)
            m_host.capture(true);
    }
    void releaseMouse() {
        if (m_host.capture)
            m_host.capture(false);
    }
    void setCursor(const QCursor& cursor);
    void unsetCursor();
    QCursor cursor() const {
        return m_cursor.value_or(QCursor());
    }
    bool hasCursor() const {
        return m_cursor.has_value();
    }
    void setInputMethodEnabled(bool enabled);
    bool inputMethodEnabled() const {
        return m_inputMethodEnabled;
    }
    QVariant defaultInputMethodQuery(Qt::InputMethodQuery query) const {
        if (query == Qt::ImEnabled)
            return m_inputMethodEnabled;
        return m_host.inputMethodQuery ? m_host.inputMethodQuery(query) : QVariant();
    }
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const;
    bool render(QPainter& painter, const QRegion& dirty);

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
    bool setCanvasTextStyle(const SnowCanvasTextStyle& style);
    bool setCanvasSerialNumberStyle(const SnowCanvasSerialNumberStyle& style);

    SnowCanvasHistoryState canvasHistoryState() const;
    quint64 autoFilterGeneration() const;
    std::optional<SnowCanvasAutoFilterRecord> autoFilterRegions() const;
    bool setAutoFilterRegions(const std::optional<SnowCanvasAutoFilterRecord>& record);
    bool fillAutoFilterCategory(const QString& category);

    SnowCanvasSnapConfig canvasSnapConfig() const;
    bool setCanvasSnapConfig(const SnowCanvasSnapConfig& config);

    SnowCanvasGridConfig canvasGridConfig() const;
    bool setCanvasGridConfig(const SnowCanvasGridConfig& config);

    bool undo();
    bool redo();
    bool deleteSelected();
    // Deletes every document element as one undoable history entry, preserving viewports,
    // creation styles, history, and document-wide configuration.
    bool deleteAllElements();
    // Clears all document elements and history, preserving viewports and creation styles.
    bool clearDocument();
    bool duplicateSelected(const QPointF& offset = QPointF(12.0, 12.0));
    bool reorderSelected(SnowCanvasSelectionOrder order);
    // Aligns or distributes the selected elements to their shared bounds as one
    // undoable history entry. Requires at least two selected elements for the
    // align modes and three for the distribute modes.
    bool alignSelected(SnowCanvasSelectionAlignment alignment);
    bool setSelectedOpacity(double opacity);
    bool adjustSelectedSerialNumbers(qint64 delta);
    bool createSerialNumberText();
    // Opens the label of one selected arrow, or starts an uncommitted attached draft.
    bool editSelectedArrowText();
    // Commits active text, clears transient editing state and selection, and
    // restores the select tool.
    bool resetEditingState();
    // Commits active text and clears transient editing state and selection while
    // preserving the currently active canvas tool.
    bool resetEditingStatePreservingTool();
    // Discards an uncommitted inline text draft without adding it to history.
    bool cancelActiveTextEditing();
    // Releases renderer cache and scratch memory without changing document, view, styles, or
    // selection.
    void clearRenderState();
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
    void setBaseImageSources(const QList<SnowCanvasBaseImageSource>& sources);
    [[nodiscard]] QTransform canvasToViewTransform() const;
    QRect viewRectForCanvasRect(const QRectF& canvasRect, int paddingPx = 0) const;

  public slots:
    void setShowDirtyRects(bool show);

  signals:
    void serialNumberToolbarStateChanged();
    void overlayPaintRequested(QPainter* painter, const QRegion& dirty);
    void autoFilterRegionsChanged();
    void autoFilterInteractionStarting();
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
    bool event(QEvent* event) override;

  private:
    SnowCanvasHostCallbacks m_host;
    QSize m_physicalSize;
    QSize m_size;
    qreal m_devicePixelRatio = 1.0;
    bool m_inputMethodEnabled = false;
    std::optional<QCursor> m_cursor;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
