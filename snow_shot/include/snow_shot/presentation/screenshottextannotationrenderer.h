// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTEXTANNOTATIONRENDERER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTEXTANNOTATIONRENDERER_H

#include "snow_shot/presentation/screenshottextannotation.h"
#include <QFont>
#include <QImage>
#include <QPainterPath>
#include <QTransform>
#include <memory>
#include <functional>

class QPainter;
class QTextDocument;
class ScreenshotOcrPresentation;
struct ScreenshotTextAnnotationStroke;

struct ScreenshotTextAnnotationRenderItem final {
    // Explicit canvas-space ink (highlights and imported paths). Line annotations instead
    // retain a compact immutable stroke description; coverage() materializes either form.
    QPainterPath path;
    QColor color;
    bool highlight = false;
    std::shared_ptr<const ScreenshotTextAnnotationStroke> stroke = {};
    QTransform sourceToCanvas = {};
    [[nodiscard]] QRectF bounds() const;
    [[nodiscard]] QPainterPath coverage() const;
    [[nodiscard]] bool sameGeometry(const ScreenshotTextAnnotationRenderItem& other) const;
};

// Immutable vector descriptions; safe to share with asynchronous exports. No source document
// or mutable layout is retained, and no stroke expansion is required to create a snapshot.
struct ScreenshotTextAnnotationRenderSnapshot final {
    QVector<ScreenshotTextAnnotationRenderItem> items;
    QRectF bounds;
    [[nodiscard]] bool empty() const {
        return items.isEmpty();
    }
};

// Owns only the current source's active geometry. Recolouring and camera changes reuse
// immutable stroke descriptions without allocating or comparing expanded paths.
class ScreenshotTextAnnotationRenderer final {
  public:
    struct Diagnostics {
        // Number of selection ranges whose fragments were generated, excluding cache hits.
        quint64 fragmentBuilds = 0;
        quint64 strokePreparations = 0;
        qsizetype fragmentSets = 0;
        qsizetype strokes = 0;
        qint64 geometryBytes = 0;
    };

    ScreenshotTextAnnotationRenderer();
    ~ScreenshotTextAnnotationRenderer();
    ScreenshotTextAnnotationRenderer(const ScreenshotTextAnnotationRenderer&) = delete;
    ScreenshotTextAnnotationRenderer& operator=(const ScreenshotTextAnnotationRenderer&) = delete;

    ScreenshotTextAnnotationRenderSnapshot
    ocrSnapshot(const ScreenshotTextAnnotationDocument& annotations,
                const ScreenshotOcrPresentation& source, const QTransform& sourceToCanvas = {},
                const QFont& font = {});
    ScreenshotTextAnnotationRenderSnapshot
    formattedSnapshot(const ScreenshotTextAnnotationDocument& annotations, QTextDocument& source,
                      const QTransform& sourceToCanvas = {});
    void clear();
    [[nodiscard]] Diagnostics diagnostics() const;

  private:
    struct Private;
    std::unique_ptr<Private> d;
};

// A canvas-owned, bounded cache for visible coverage. Exports omit it and release each row's
// expanded path immediately. This object is confined to its painting thread.
class ScreenshotTextAnnotationPaintCache final {
  public:
    struct Diagnostics {
        quint64 coverageBuilds = 0;
        qsizetype paths = 0;
        qsizetype bytes = 0;
    };
    explicit ScreenshotTextAnnotationPaintCache(qsizetype maximumBytes = 8 * 1024 * 1024);
    ~ScreenshotTextAnnotationPaintCache();
    void clear();
    [[nodiscard]] Diagnostics diagnostics() const;
    [[nodiscard]] QPainterPath coverage(const ScreenshotTextAnnotationRenderItem& item);

  private:
    struct Private;
    std::unique_ptr<Private> d;
};

ScreenshotTextAnnotationRenderSnapshot
screenshotOcrTextAnnotationSnapshot(const ScreenshotTextAnnotationDocument& annotations,
                                    const ScreenshotOcrPresentation& source,
                                    const QTransform& sourceToCanvas = {}, const QFont& font = {});
ScreenshotTextAnnotationRenderSnapshot
screenshotFormattedTextAnnotationSnapshot(const ScreenshotTextAnnotationDocument& annotations,
                                          QTextDocument& source,
                                          const QTransform& sourceToCanvas = {});
bool paintScreenshotTextAnnotations(QPainter& painter,
                                    const ScreenshotTextAnnotationRenderSnapshot& snapshot,
                                    const QTransform& canvasToPainter = {},
                                    ScreenshotTextAnnotationPaintCache* cache = nullptr,
                                    const std::function<bool()>& cancelled = {});
// Bounds of changed ink, excluding an unchanged prefix/suffix of the ordered snapshots.
[[nodiscard]] QRectF
screenshotTextAnnotationDamage(const ScreenshotTextAnnotationRenderSnapshot& previous,
                               const ScreenshotTextAnnotationRenderSnapshot& current);
QImage renderScreenshotTextAnnotations(const QImage& image, const QRectF& backgroundCanvasRect,
                                       const ScreenshotTextAnnotationRenderSnapshot& snapshot);

#endif
