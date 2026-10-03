#pragma once

#include <QImage>
#include <QByteArray>
#include <QList>
#include <QJsonArray>
#include <QRectF>
#include <QSize>

#include <memory>
#include <functional>

#include "snow_draw_engine_qt/snow_canvas_export_types.h"
#include "snow_draw_engine_qt/snow_canvas_smart_erase.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

namespace snow_canvas_runtime {
struct Access;
}

class SnowCanvasRuntime {
  public:
    SnowCanvasRuntime();
    explicit SnowCanvasRuntime(const SnowCanvasRuntimeConfig& config);
    ~SnowCanvasRuntime();

    SnowCanvasRuntime(const SnowCanvasRuntime&) = delete;
    SnowCanvasRuntime& operator=(const SnowCanvasRuntime&) = delete;

    // Thread-affine: construct, use, and destroy a runtime on the same thread.
    // Cross-thread calls that can fail return false or an empty image.
    bool isOwnerThread() const;
    bool isValid() const;
    bool reset();
    bool cloneDocumentSessionFrom(const SnowCanvasRuntime& source);
    QByteArray serializeDocumentSession() const;
    QByteArray serializeSelectedDrawTemplate() const;
    QJsonArray selectedElementIds() const;
    bool restoreDocumentSession(const QByteArray& payload);
    QByteArray serializeDocumentHistory() const;
    bool restoreDocumentHistory(const QByteArray& payload);
    bool restoreDocumentHistoryPreservingEditorStyles(const QByteArray& payload);
    bool clearDocumentPreservingViewports();
    // Releases derived drawing caches without changing the document, history, or viewports.
    void clearRenderState();
    bool setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools);
    QByteArray applyAnnotationTransaction(const QByteArray& payload);
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;
    quint64 documentRevision() const;
    void setDocumentChangedHandler(std::function<void()> handler);
    // Detaches clients immediately. Engine storage is released by the process cleanup worker,
    // without waiting in the runtime owner's destructor.
    void destroyAsync();
    void setBaseImageSources(const QList<SnowCanvasBaseImageSource>& sources);
    SnowCanvasSmartEraseSnapshot smartEraseSnapshot() const;
    void restoreSmartEraseSnapshot(const SnowCanvasSmartEraseSnapshot& snapshot);
    QImage renderToImage(const QRectF& virtualSelectionRect, const QSize& outputSize,
                         const QList<CanvasExportSource>& sources);

  private:
    friend struct snow_canvas_runtime::Access;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// A short-lived editor viewport without QWidget, rendering surfaces, or event processing.
// It must be destroyed before its runtime is reset/restored and used on the runtime owner thread.
class SnowCanvasRuntimeEditor final {
  public:
    explicit SnowCanvasRuntimeEditor(SnowCanvasRuntime& runtime,
                                     SnowCanvasTool tool = SnowCanvasTool::Select);
    ~SnowCanvasRuntimeEditor();
    SnowCanvasRuntimeEditor(const SnowCanvasRuntimeEditor&) = delete;
    SnowCanvasRuntimeEditor& operator=(const SnowCanvasRuntimeEditor&) = delete;
    bool isValid() const;
    bool setActiveTool(SnowCanvasTool tool);
    SnowCanvasStyleToolbarState canvasStyleToolbarState() const;
    SnowCanvasWatermarkConfig canvasWatermarkConfig() const;
    SnowCanvasSpotlightConfig canvasSpotlightConfig() const;
    bool setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32, SnowCanvasShapeKind);
    bool setTextStyleFromToolbar(const SnowCanvasTextStyle&,
                                 quint32 properties = SnowCanvasTextStyleAllProperties);
    bool setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&);
    bool setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&, quint32 properties);
    bool setFilterStyleFromToolbar(const SnowCanvasFilterStyle&, quint32);
    bool
    setBrushEraserCreationStyle(const SnowCanvasBrushEraserStyle&,
                                quint32 properties = SnowCanvasBrushEraserStylePropertyStrokeWidth);
    bool setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig&);
    bool setSpotlightConfigFromToolbar(const SnowCanvasSpotlightConfig&);
    bool select(quint32 index, quint32 generation);
    bool deleteSelected();
    bool deleteAllElements();
    // Rectangle Eraser uses two corners; Brush Eraser accepts a stroke or a single dot.
    bool erasePath(const QList<QPointF>& points,
                   SnowCanvasTool eraserTool = SnowCanvasTool::Eraser);
    bool duplicateSelected(QPointF offset = QPointF(20, 20));
    bool reorderSelected(SnowCanvasSelectionOrder);
    bool alignSelected(SnowCanvasSelectionAlignment);
    bool setSelectedOpacity(double);
    bool adjustSelectedSerialNumbers(qint64);
    bool createSerialNumberText();
    bool insertDrawTemplate(const QByteArray&, QPointF center);
    bool setAutoFilterRegions(const SnowCanvasAutoFilterRecord&);
    bool fillAutoFilterCategory(const QString& category);
    bool succeeded() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
