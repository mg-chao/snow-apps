#pragma once

#include <QImage>
#include <QByteArray>
#include <QList>
#include <QRectF>
#include <QSize>

#include <memory>

#include "snow_draw_engine_qt/snow_canvas_export_types.h"
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
    bool restoreDocumentSession(const QByteArray& payload);
    QByteArray serializeDocumentHistory() const;
    bool restoreDocumentHistory(const QByteArray& payload);
    bool restoreDocumentHistoryPreservingEditorStyles(const QByteArray& payload);
    bool clearDocumentPreservingViewports();
    bool setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools);
    void destroyAsync();
    QImage renderToImage(const QRectF& virtualSelectionRect, const QSize& outputSize,
                         const QList<CanvasExportSource>& sources);

  private:
    friend struct snow_canvas_runtime::Access;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
