#pragma once

#include <QImage>
#include <QByteArray>
#include <QList>
#include <QJsonArray>
#include <QRectF>
#include <QSize>
#include <QSizeF>

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
    // Restorable committed document/configuration without undo/redo or input drafts.
    // Cost and size depend only on current content, never on accumulated history.
    // Retains the persisted-session limit of 16 MiB for current content.
    QByteArray serializeDocumentSnapshot() const;
    // Canonical current-document JSON bytes, not a hash or a restorable session.
    // Excludes history, editor defaults, revisions and unused trailing element slots.
    QByteArray documentCheckpoint() const;
    QByteArray serializeSelectedDrawTemplate() const;
    QJsonArray selectedElementIds() const;
    bool restoreDocumentSession(const QByteArray& payload);
    QByteArray serializeDocumentHistory() const;
    bool restoreDocumentHistory(const QByteArray& payload);
    bool restoreDocumentHistoryPreservingEditorStyles(const QByteArray& payload);
    bool clearDocumentPreservingViewports();
    // Releases derived drawing caches without changing the document, history, or viewports.
    void clearRenderState();
    bool setAutomaticallySelectNewlyCreatedElements(bool enabled);
    bool setAutomaticSelectionExcludedTools(const QSet<SnowCanvasTool>& tools);
    bool setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools);
    QByteArray applyAnnotationTransaction(const QByteArray& payload);
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;
    // direction: -1 previews undo, 0 reads the current state, +1 previews redo.
    quint64 hostHistoryState(int direction = 0) const;
    bool commitHostHistoryState(quint64 state);
    quint64 documentRevision() const;
    // Includes hidden elements and watermark content, but excludes creation styles and history.
    bool hasDocumentContent() const;
    void setDocumentChangedHandler(std::function<void()> handler);
    // Detaches clients immediately. Engine storage is released by the process cleanup worker,
    // without waiting in the runtime owner's destructor.
    void destroyAsync();
    void setBaseImageSources(const QList<SnowCanvasBaseImageSource>& sources);
    SnowCanvasSmartEraseSnapshot smartEraseSnapshot() const;
    // Reuses completed results for the same base pixels and canvas coordinates while keeping
    // reconstruction synchronized with subsequent document edits.
    void seedSmartEraseSnapshot(const SnowCanvasSmartEraseSnapshot& snapshot);
    // Freezes captured appearance for worker exports; subsequent document edits are not tracked.
    void restoreSmartEraseSnapshot(const SnowCanvasSmartEraseSnapshot& snapshot);
    // Complete missing reconstruction on the calling export worker, reusing matching seed pixels.
    // Only erasures affecting outputCanvasRect are required. Failure/cancellation leaves the
    // existing snapshot unchanged; success freezes the completed snapshot for rendering.
    bool prepareSmartEraseForExport(const SnowCanvasSmartEraseSnapshot& seed,
                                    const QList<SnowCanvasBaseImageSource>& sources,
                                    const QRectF& outputCanvasRect,
                                    const std::function<bool()>& cancelled = {});
    // Projects source layers with the same sampling and clipping as document exports.
    // Requires no runtime and can be called on any thread. The result has DPR 1
    // and no color profile; callers supply metadata for their source color space.
    static QImage renderImageSourcesToImage(const QRectF& canvasRect, const QSize& outputSize,
                                            const QList<CanvasExportSource>& sources);
    // An optional output-sized background replaces capture painting, while sources
    // remain available to magnifiers and background restoration.
    QImage renderToImage(const QRectF& virtualSelectionRect, const QSize& outputSize,
                         const QList<CanvasExportSource>& sources, const QImage& background = {});
    // Reuse one synchronized scene and the original coordinate frame while painting
    // disjoint pixel rectangles into a compact destination. Filtering uses bounded halos.
    QImage renderTilesToImage(const QRectF& sourceFrame, const QSize& sourcePixelSize,
                              const QSize& outputSize, const QList<CanvasExportTile>& tiles,
                              const QList<CanvasExportSource>& sources,
                              const QImage& background = {});

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
    bool setDistanceStyleFromToolbar(const SnowCanvasDistanceStyle&, quint32);
    bool setAngleStyleFromToolbar(const SnowCanvasAngleStyle&, quint32);
    bool setBracketStyleFromToolbar(const SnowCanvasBracketStyle&, quint32);
    bool setMagnifierStyleFromToolbar(const SnowCanvasMagnifierStyle&, quint32,
                                      bool creationDefaults = true);
    bool adjustAngleValue(double deltaRadians);
    bool setDistanceCreationPixelScale(const QSizeF&);
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
