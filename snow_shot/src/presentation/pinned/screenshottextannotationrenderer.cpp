// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshottextannotationrenderer.h"
#include "snow_shot/presentation/screenshottextannotationgeometry.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"
#include <QPainter>
#include <QHash>
#include <QCache>
#include <QDataStream>
#include <QGuiApplication>
#include <QIODevice>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <algorithm>
#include <array>
#include <cmath>

struct ScreenshotTextAnnotationStroke {
    ScreenshotTextAnnotationFragment fragment;
    ScreenshotTextAnnotationTool tool;
    qreal width;
    SnowCanvasStrokeStyle pattern;
    SnowCanvasStrokeRenderingStyle rendering;
    QRectF bounds;
    QPainterPath projectiveCoverage;
};

namespace {
struct StrokeKey {
    QRectF rect;
    QTransform localToSource;
    ScreenshotTextSelectionRange range;
    ScreenshotTextAnnotationTool tool;
    qreal width;
    SnowCanvasStrokeStyle pattern;
    SnowCanvasStrokeRenderingStyle rendering;
    std::array<qreal, 13> coordinates() const {
        return {rect.x(),
                rect.y(),
                rect.width(),
                rect.height(),
                localToSource.m11(),
                localToSource.m12(),
                localToSource.m13(),
                localToSource.m21(),
                localToSource.m22(),
                localToSource.m23(),
                localToSource.m31(),
                localToSource.m32(),
                localToSource.m33()};
    }
    // QRectF equality is fuzzy; hashing geometry requires exact coordinate equality.
    friend bool operator==(const StrokeKey& a, const StrokeKey& b) {
        return a.range == b.range && a.tool == b.tool && a.width == b.width &&
               a.pattern == b.pattern && a.rendering == b.rendering &&
               a.coordinates() == b.coordinates();
    }
    friend size_t qHash(const StrokeKey& key, size_t seed = 0) noexcept {
        const auto& t = key.localToSource;
        return qHashMulti(seed, key.rect.x(), key.rect.y(), key.rect.width(), key.rect.height(),
                          t.m11(), t.m12(), t.m13(), t.m21(), t.m22(), t.m23(), t.m31(), t.m32(),
                          t.m33(), key.range.sourceLine, key.range.start, key.range.end,
                          int(key.tool), key.width, int(key.pattern), int(key.rendering));
    }
};
struct FragmentKey {
    ScreenshotTextSelectionRange range;
    friend bool operator==(const FragmentKey&, const FragmentKey&) = default;
    friend size_t qHash(const FragmentKey& key, size_t seed = 0) noexcept {
        return qHashMulti(seed, key.range.sourceLine, key.range.start, key.range.end);
    }
};

struct Geometry {
    QHash<FragmentKey, QVector<ScreenshotTextAnnotationFragment>> fragments;
    QHash<StrokeKey, std::shared_ptr<const ScreenshotTextAnnotationStroke>> strokes;
    QByteArray shapeKey;
    QTransform transform;
    ScreenshotTextAnnotationRenderSnapshot snapshot;
    QVector<qsizetype> itemStyles;
};

using Diagnostics = ScreenshotTextAnnotationRenderer::Diagnostics;

quint32 fragmentSeed(const ScreenshotTextSelectionRange& range, ScreenshotTextAnnotationTool tool) {
    // Stable under repaint, recolouring, zoom and export. Each visual row has its own ink.
    quint32 seed = quint32(range.sourceLine) * 0x9e3779b9U;
    seed ^= quint32(range.start) * 0x85ebca6bU;
    seed ^= quint32(range.end) * 0xc2b2ae35U;
    return seed ^ quint32(tool);
}

QPainterPath sourceCoverage(const ScreenshotTextAnnotationStroke& stroke) {
    if (!stroke.projectiveCoverage.isEmpty())
        return stroke.projectiveCoverage;
    const auto& fragment = stroke.fragment;
    const auto tool = stroke.tool;
    const qreal width = stroke.width;
    const QRectF rect = fragment.rect;
    const QPointF center = rect.center();
    const QPointF origin = fragment.localToSource.map(center);
    const QPointF dx = fragment.localToSource.map(center + QPointF(1, 0)) - origin;
    const QPointF dy = fragment.localToSource.map(center + QPointF(0, 1)) - origin;
    const qreal advanceScale = std::hypot(dx.x(), dx.y());
    const qreal crossScale =
        advanceScale > 1e-9 ? std::abs(dx.x() * dy.y() - dx.y() * dy.x()) / advanceScale : 0;
    if (!(crossScale > 1e-9) || !std::isfinite(crossScale) || !std::isfinite(advanceScale))
        return {};
    const qreal localWidth = width / crossScale;
    const qreal amplitude = std::max(qreal(1), width * 0.75) / crossScale;
    const qreal y = tool == ScreenshotTextAnnotationTool::Strikethrough
                        ? rect.center().y()
                        : rect.bottom() + localWidth * 0.5 +
                              (tool == ScreenshotTextAnnotationTool::WavyUnderline ? amplitude : 0);
    QPainterPath centerline;
    centerline.moveTo(rect.left(), y);
    if (tool == ScreenshotTextAnnotationTool::WavyUnderline) {
        const qreal halfWave = std::max(qreal(3), width * 2) / advanceScale;
        qreal x = rect.left();
        qreal direction = -1;
        while (x < rect.right()) {
            const qreal next = std::min(rect.right(), x + halfWave);
            if (!(next > x))
                break;
            const qreal span = next - x;
            // Alternate smooth half waves. Short final segments stay inside the selection.
            centerline.cubicTo(x + span / 3, y + direction * amplitude * 4 / 3, x + span * 2 / 3,
                               y + direction * amplitude * 4 / 3, next, y);
            direction = -direction;
            x = next;
        }
    } else {
        centerline.lineTo(rect.right(), y);
    }
    auto ink =
        snowCanvasStrokeCoverage(fragment.localToSource.map(centerline), width, stroke.pattern,
                                 stroke.rendering, fragmentSeed(fragment.range, tool));
    if (tool != ScreenshotTextAnnotationTool::Strikethrough) {
        bool invertible = false;
        const auto sourceToLocal = fragment.localToSource.inverted(&invertible);
        if (invertible) {
            const qreal top = sourceToLocal.map(ink).boundingRect().top();
            if (top < rect.bottom()) {
                // Seeded hand-drawn excursions must also stay below the glyphs.
                const auto shift = sourceToLocal *
                                   QTransform::fromTranslate(0, rect.bottom() - top) *
                                   fragment.localToSource;
                ink = shift.map(ink);
            }
        }
    }
    return ink;
}

QRectF strokeBounds(const ScreenshotTextAnnotationStroke& stroke) {
    const auto& fragment = stroke.fragment;
    const auto& localToSource = fragment.localToSource;
    const auto origin = localToSource.map(QPointF());
    const auto dx = localToSource.map(QPointF(1, 0)) - origin;
    const auto dy = localToSource.map(QPointF(0, 1)) - origin;
    const qreal advanceScale = std::hypot(dx.x(), dx.y());
    const qreal crossScale =
        advanceScale > 1e-9 ? std::abs(dx.x() * dy.y() - dx.y() * dy.x()) / advanceScale : 0;
    if (!(crossScale > 1e-9) || !std::isfinite(crossScale) || !std::isfinite(advanceScale))
        return {};
    const auto rect = fragment.rect;
    const bool strike = stroke.tool == ScreenshotTextAnnotationTool::Strikethrough;
    const bool wave = stroke.tool == ScreenshotTextAnnotationTool::WavyUnderline;
    const qreal amplitude = wave ? std::max(qreal(1), stroke.width * .75) / crossScale : 0;
    const qreal y =
        strike ? rect.center().y() : rect.bottom() + stroke.width / crossScale * .5 + amplitude;
    // Cubics lie within their control polygon. The shared engine contract bounds every
    // hand-drawn displacement; round caps/joins add at most half the source-space width.
    const qreal radius = stroke.width * .5 + snowCanvasStrokeRenderingOutset(stroke.rendering);
    auto bounds = localToSource.mapRect(
        QRectF(rect.left(), y - amplitude * 4 / 3, rect.width(), amplitude * 8 / 3));
    bounds.adjust(-radius, -radius, radius, radius);
    if (!strike) {
        const auto sourceToLocal = localToSource.inverted();
        const qreal shift = std::max(qreal(0), rect.bottom() - sourceToLocal.mapRect(bounds).top());
        const auto maximumShift =
            sourceToLocal * QTransform::fromTranslate(0, shift) * localToSource;
        bounds = bounds.united(maximumShift.mapRect(bounds));
    }
    return bounds;
}

void appendItem(ScreenshotTextAnnotationRenderSnapshot& snapshot,
                ScreenshotTextAnnotationRenderItem item, Geometry& retained, qsizetype styleIndex) {
    if (!item.color.isValid() || item.color.alpha() == 0)
        return;
    const auto bounds = item.bounds();
    if (bounds.isEmpty())
        return;
    snapshot.bounds = snapshot.items.isEmpty() ? bounds : snapshot.bounds.united(bounds);
    snapshot.items.append(std::move(item));
    retained.itemStyles.append(styleIndex);
}

void appendFragment(ScreenshotTextAnnotationRenderSnapshot& snapshot,
                    const ScreenshotTextAnnotationRun& run,
                    const ScreenshotTextAnnotationFragment& fragment,
                    const QTransform& sourceToCanvas, const Geometry& previous, Geometry& retained,
                    Diagnostics& diagnostics, qsizetype styleIndex) {
    if (run.highlight) {
        QPainterPath path;
        path.addRect(fragment.rect);
        appendItem(
            snapshot,
            {(fragment.localToSource * sourceToCanvas).map(path), run.highlight->color, true},
            retained, styleIndex);
    }
    if (!run.line)
        return;
    const auto& style = run.line->style;
    if (!(style.width > 0) || !std::isfinite(style.width))
        return;
    const StrokeKey key{fragment.rect, fragment.localToSource, fragment.range,      run.line->tool,
                        style.width,   style.strokeStyle,      style.renderingStyle};
    std::shared_ptr<const ScreenshotTextAnnotationStroke> stroke;
    if (const auto cached = previous.strokes.constFind(key); cached != previous.strokes.cend()) {
        stroke = *cached;
    } else {
        auto prepared = std::make_shared<ScreenshotTextAnnotationStroke>();
        prepared->fragment = fragment;
        prepared->tool = run.line->tool;
        prepared->width = style.width;
        prepared->pattern = style.strokeStyle;
        prepared->rendering = style.renderingStyle;
        if (fragment.localToSource.isAffine()) {
            prepared->bounds = strokeBounds(*prepared);
        } else {
            // Projective OCR fitting can approach a horizon: retain exact coverage for this
            // uncommon case rather than using an affine bound that might clip valid ink.
            prepared->projectiveCoverage = sourceCoverage(*prepared);
            prepared->bounds = prepared->projectiveCoverage.boundingRect();
        }
        stroke = std::move(prepared);
        ++diagnostics.strokePreparations;
    }
    retained.strokes.insert(key, stroke);
    appendItem(snapshot, {{}, style.color, false, std::move(stroke), sourceToCanvas}, retained,
               styleIndex);
}

template <typename Fragments>
void appendRuns(ScreenshotTextAnnotationRenderSnapshot& snapshot,
                const QVector<ScreenshotTextAnnotationRun>& runs, const QTransform& sourceToCanvas,
                const Geometry& previous, Geometry& retained, Diagnostics& diagnostics,
                qsizetype styleOffset, Fragments fragments) {
    if (runs.isEmpty())
        return;
    QVector<ScreenshotTextSelectionRange> missing;
    for (const auto& run : runs) {
        const FragmentKey key{run.range};
        if (retained.fragments.contains(key))
            continue;
        const auto cached = previous.fragments.constFind(key);
        if (cached != previous.fragments.cend()) {
            retained.fragments.insert(key, *cached);
        } else {
            missing.append(run.range);
            // Retain empty results too: separators and inline objects have no annotation ink.
            retained.fragments.insert(key, {});
        }
    }
    if (!missing.isEmpty()) {
        // Batch only the misses so disjoint ranges in one OCR line share its text layout.
        auto prepared = fragments(missing);
        diagnostics.fragmentBuilds += quint64(missing.size());
        for (auto& fragment : prepared) {
            const auto range = std::lower_bound(missing.cbegin(), missing.cend(), fragment.range,
                                                [](const auto& item, const auto& value) {
                                                    return item.sourceLine < value.sourceLine ||
                                                           (item.sourceLine == value.sourceLine &&
                                                            item.end <= value.start);
                                                });
            if (range != missing.cend() && range->sourceLine == fragment.range.sourceLine &&
                range->start <= fragment.range.start && range->end >= fragment.range.end)
                retained.fragments[FragmentKey{*range}].append(std::move(fragment));
        }
    }
    for (qsizetype i = 0; i < runs.size(); ++i) {
        const auto& run = runs.at(i);
        const auto geometry = retained.fragments.constFind(FragmentKey{run.range});
        for (const auto& fragment : *geometry)
            appendFragment(snapshot, run, fragment, sourceToCanvas, previous, retained, diagnostics,
                           styleOffset + i);
    }
}

template <typename Fragments>
ScreenshotTextAnnotationRenderSnapshot
buildSnapshot(const ScreenshotTextAnnotationDocument& annotations, const QTransform& sourceToCanvas,
              Geometry& geometry, Diagnostics& diagnostics, Fragments fragments) {
    ScreenshotTextAnnotationRenderSnapshot snapshot;
    QVector<ScreenshotTextAnnotationRun> highlights;
    QVector<ScreenshotTextAnnotationRun> lines;
    const auto append = [](auto& channel, ScreenshotTextAnnotationRun run) {
        if (!channel.isEmpty() && channel.back().range.sourceLine == run.range.sourceLine &&
            channel.back().range.end == run.range.start &&
            channel.back().highlight == run.highlight && channel.back().line == run.line) {
            channel.back().range.end = run.range.end;
        } else {
            channel.append(std::move(run));
        }
    };
    for (const auto& run : annotations.runs()) {
        if (run.highlight) {
            auto highlight = run;
            highlight.line.reset();
            append(highlights, std::move(highlight));
        }
        if (run.line) {
            auto line = run;
            line.highlight.reset();
            append(lines, std::move(line));
        }
    }
    QByteArray shapeKey;
    QDataStream stream(&shapeKey, QIODevice::WriteOnly);
    QVector<QColor> colors;
    colors.reserve(highlights.size() + lines.size());
    stream << quint64(highlights.size()) << quint64(lines.size());
    const auto appendStyle = [&](const auto& run, const auto& style) {
        stream << run.range.sourceLine << run.range.start << run.range.end
               << (style.color.isValid() && style.color.alpha() != 0);
        colors.append(style.color);
    };
    for (const auto& run : highlights)
        appendStyle(run, *run.highlight);
    for (const auto& run : lines) {
        appendStyle(run, run.line->style);
        stream << quint8(run.line->tool) << run.line->style.width
               << quint8(run.line->style.strokeStyle) << quint8(run.line->style.renderingStyle);
    }
    if (geometry.shapeKey == shapeKey && geometry.transform == sourceToCanvas) {
        snapshot = geometry.snapshot;
        for (qsizetype i = 0; i < snapshot.items.size(); ++i) {
            const auto& color = colors.at(geometry.itemStyles.at(i));
            if (snapshot.items.at(i).color != color)
                snapshot.items[i].color = color;
        }
        geometry.snapshot = snapshot;
        return snapshot;
    }
    Geometry retained;
    retained.shapeKey = std::move(shapeKey);
    retained.transform = sourceToCanvas;
    // An unrelated highlight boundary must not restart an existing line's dash phase or seed.
    appendRuns(snapshot, highlights, sourceToCanvas, geometry, retained, diagnostics, 0, fragments);
    appendRuns(snapshot, lines, sourceToCanvas, geometry, retained, diagnostics, highlights.size(),
               fragments);
    retained.snapshot = snapshot;
    geometry = std::move(retained);
    return snapshot;
}
} // namespace

QRectF ScreenshotTextAnnotationRenderItem::bounds() const {
    if (!stroke)
        return path.boundingRect();
    return sourceToCanvas.isAffine() ? sourceToCanvas.mapRect(stroke->bounds)
                                     : coverage().boundingRect();
}

QPainterPath ScreenshotTextAnnotationRenderItem::coverage() const {
    return stroke ? sourceToCanvas.map(sourceCoverage(*stroke)) : path;
}

bool ScreenshotTextAnnotationRenderItem::sameGeometry(
    const ScreenshotTextAnnotationRenderItem& other) const {
    if (stroke || other.stroke)
        return stroke == other.stroke && sourceToCanvas == other.sourceToCanvas;
    return path == other.path;
}

struct ScreenshotTextAnnotationPaintCache::Private {
    struct Entry {
        // Owning the key prevents an evicted renderer description's address being reused.
        std::shared_ptr<const ScreenshotTextAnnotationStroke> stroke;
        QPainterPath source;
        QPainterPath canvas;
        QTransform transform;
        qsizetype bytes() const {
            return qsizetype(sizeof(Entry) + sizeof(ScreenshotTextAnnotationStroke)) +
                   (source.capacity() + (transform.isIdentity() ? 0 : canvas.capacity())) *
                       qsizetype(sizeof(QPainterPath::Element));
        }
    };
    QCache<const ScreenshotTextAnnotationStroke*, Entry> paths;
    quint64 builds = 0;
    explicit Private(qsizetype maximumBytes) : paths(std::max(qsizetype(0), maximumBytes)) {}
};

ScreenshotTextAnnotationPaintCache::ScreenshotTextAnnotationPaintCache(qsizetype maximumBytes)
    : d(std::make_unique<Private>(maximumBytes)) {}
ScreenshotTextAnnotationPaintCache::~ScreenshotTextAnnotationPaintCache() = default;

void ScreenshotTextAnnotationPaintCache::clear() {
    d->paths.clear();
}

ScreenshotTextAnnotationPaintCache::Diagnostics
ScreenshotTextAnnotationPaintCache::diagnostics() const {
    return {d->builds, d->paths.size(), d->paths.totalCost()};
}

QPainterPath
ScreenshotTextAnnotationPaintCache::coverage(const ScreenshotTextAnnotationRenderItem& item) {
    if (!item.stroke)
        return item.path;
    const auto* key = item.stroke.get();
    auto* entry = d->paths.object(key);
    if (entry && entry->transform == item.sourceToCanvas)
        return entry->canvas;
    if (entry) {
        entry = d->paths.take(key);
    } else {
        entry = new Private::Entry;
        entry->stroke = item.stroke;
        entry->source = sourceCoverage(*item.stroke);
        ++d->builds;
    }
    entry->transform = item.sourceToCanvas;
    entry->canvas = item.sourceToCanvas.map(entry->source);
    const auto result = entry->canvas;
    d->paths.insert(key, entry, entry->bytes());
    return result;
}

struct ScreenshotTextAnnotationRenderer::Private {
    QByteArray sourceKey;
    Geometry geometry;
    Diagnostics diagnostics;

    void bind(QByteArray key) {
        if (sourceKey != key) {
            geometry = {};
            sourceKey = std::move(key);
        }
    }
};

ScreenshotTextAnnotationRenderer::ScreenshotTextAnnotationRenderer() = default;
ScreenshotTextAnnotationRenderer::~ScreenshotTextAnnotationRenderer() = default;

void ScreenshotTextAnnotationRenderer::clear() {
    d.reset();
}

ScreenshotTextAnnotationRenderer::Diagnostics
ScreenshotTextAnnotationRenderer::diagnostics() const {
    if (!d)
        return {};
    auto result = d->diagnostics;
    result.fragmentSets = d->geometry.fragments.size();
    result.strokes = d->geometry.strokes.size();
    for (const auto& stroke : d->geometry.strokes)
        result.geometryBytes +=
            qint64(sizeof(ScreenshotTextAnnotationStroke)) +
            qint64(stroke->projectiveCoverage.capacity()) * qint64(sizeof(QPainterPath::Element));
    return result;
}

ScreenshotTextAnnotationRenderSnapshot
ScreenshotTextAnnotationRenderer::ocrSnapshot(const ScreenshotTextAnnotationDocument& annotations,
                                              const ScreenshotOcrPresentation& source,
                                              const QTransform& sourceToCanvas, const QFont& font) {
    if (annotations.empty()) {
        clear();
        return {};
    }
    if (!d)
        d = std::make_unique<Private>();
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << quint64(quintptr(&source)) << annotations.fingerprint()
           << quint8(ScreenshotTextAnnotationSource::Ocr) << font.resolve(QGuiApplication::font());
    d->bind(std::move(key));
    return buildSnapshot(annotations, sourceToCanvas, d->geometry, d->diagnostics,
                         [&](const auto& ranges) {
                             return screenshotOcrTextAnnotationFragments(source, ranges, font);
                         });
}

ScreenshotTextAnnotationRenderSnapshot ScreenshotTextAnnotationRenderer::formattedSnapshot(
    const ScreenshotTextAnnotationDocument& annotations, QTextDocument& source,
    const QTransform& sourceToCanvas) {
    if (annotations.empty()) {
        clear();
        return {};
    }
    if (!d)
        d = std::make_unique<Private>();
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << quint64(quintptr(&source)) << annotations.fingerprint()
           << quint8(ScreenshotTextAnnotationSource::Formatted) << source.revision()
           << source.defaultFont() << source.textWidth() << source.documentLayout()->documentSize();
    d->bind(std::move(key));
    return buildSnapshot(annotations, sourceToCanvas, d->geometry, d->diagnostics,
                         [&](const auto& ranges) {
                             return screenshotFormattedTextAnnotationFragments(source, ranges);
                         });
}

ScreenshotTextAnnotationRenderSnapshot
screenshotOcrTextAnnotationSnapshot(const ScreenshotTextAnnotationDocument& annotations,
                                    const ScreenshotOcrPresentation& source,
                                    const QTransform& sourceToCanvas, const QFont& font) {
    return ScreenshotTextAnnotationRenderer().ocrSnapshot(annotations, source, sourceToCanvas,
                                                          font);
}

ScreenshotTextAnnotationRenderSnapshot
screenshotFormattedTextAnnotationSnapshot(const ScreenshotTextAnnotationDocument& annotations,
                                          QTextDocument& source, const QTransform& sourceToCanvas) {
    return ScreenshotTextAnnotationRenderer().formattedSnapshot(annotations, source,
                                                                sourceToCanvas);
}

bool paintScreenshotTextAnnotations(QPainter& painter,
                                    const ScreenshotTextAnnotationRenderSnapshot& snapshot,
                                    const QTransform& canvasToPainter,
                                    ScreenshotTextAnnotationPaintCache* cache,
                                    const std::function<bool()>& cancelled) {
    if (snapshot.empty())
        return true;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(canvasToPainter, true);
    painter.setPen(Qt::NoPen);
    std::optional<QRectF> visible;
    const auto device = painter.deviceTransform();
    if (painter.hasClipping() && device.isAffine() && device.isInvertible()) {
        visible = painter.clipBoundingRect();
        if (visible->isEmpty()) {
            painter.restore();
            return true;
        }
        // Expand by one device pixel in each direction to retain antialiased edge coverage.
        const auto inverse = device.inverted();
        const qreal dx = std::abs(inverse.m11()) + std::abs(inverse.m21());
        const qreal dy = std::abs(inverse.m12()) + std::abs(inverse.m22());
        visible->adjust(-dx, -dy, dx, dy);
    }
    // Render highlights first so line marks remain visually independent of insertion order.
    for (const bool highlight : {true, false}) {
        painter.setCompositionMode(highlight ? QPainter::CompositionMode_Multiply
                                             : QPainter::CompositionMode_SourceOver);
        for (const auto& item : snapshot.items) {
            if (item.highlight != highlight)
                continue;
            // Cull compact descriptions before generating their coverage.
            if (visible && !item.bounds().intersects(*visible))
                continue;
            if (cancelled && cancelled()) {
                painter.restore();
                return false;
            }
            painter.setBrush(item.color);
            painter.drawPath(cache ? cache->coverage(item) : item.coverage());
        }
    }
    painter.restore();
    return true;
}

QRectF screenshotTextAnnotationDamage(const ScreenshotTextAnnotationRenderSnapshot& previous,
                                      const ScreenshotTextAnnotationRenderSnapshot& current) {
    const auto same = [](const auto& a, const auto& b) {
        return a.highlight == b.highlight && a.color == b.color && a.sameGeometry(b);
    };
    qsizetype start = 0;
    qsizetype oldEnd = previous.items.size();
    qsizetype newEnd = current.items.size();
    while (start < oldEnd && start < newEnd &&
           same(previous.items.at(start), current.items.at(start)))
        ++start;
    while (oldEnd > start && newEnd > start &&
           same(previous.items.at(oldEnd - 1), current.items.at(newEnd - 1))) {
        --oldEnd;
        --newEnd;
    }
    QRectF damage;
    const auto accumulate = [&](const auto& items, qsizetype end) {
        for (qsizetype i = start; i < end; ++i)
            damage = damage.united(items.at(i).bounds());
    };
    accumulate(previous.items, oldEnd);
    accumulate(current.items, newEnd);
    return damage;
}

QImage renderScreenshotTextAnnotations(const QImage& image, const QRectF& backgroundCanvasRect,
                                       const ScreenshotTextAnnotationRenderSnapshot& snapshot) {
    if (snapshot.empty())
        return image;
    if (image.isNull() || backgroundCanvasRect.isEmpty())
        return {};
    QImage result =
        image.depth() < 8 ? image.convertToFormat(QImage::Format_ARGB32_Premultiplied) : image;
    if (!snowCanvasDetachImage(result))
        return {};
    result.setDevicePixelRatio(1.0);
    QPainter painter(&result);
    if (!painter.isActive())
        return {};
    painter.setClipRect(result.rect());
    QTransform toPixels;
    toPixels.scale(result.width() / backgroundCanvasRect.width(),
                   result.height() / backgroundCanvasRect.height());
    toPixels.translate(-backgroundCanvasRect.left(), -backgroundCanvasRect.top());
    paintScreenshotTextAnnotations(painter, snapshot, toPixels);
    return result;
}
