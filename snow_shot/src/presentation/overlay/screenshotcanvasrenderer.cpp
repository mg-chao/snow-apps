#include "snow_shot/presentation/screenshotcanvasrenderer.h"

#include "snow_shot/presentation/screenshotguidelinerendering.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtextlayer.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QColorSpace>
#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRawFont>
#include <QRegion>
#include <QSizeF>
#include <QStyleOptionGraphicsItem>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <QTransform>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

class ScreenshotOcrGraphicsTextItem final : public QGraphicsItem {
  public:
    void configure(const QString& text, const QFont& font, const QColor& foreground,
                   const ScreenshotOcrTextRange& selection, ScreenshotOcrTextDirection direction,
                   qreal targetAspectRatio);
    void setSelection(const ScreenshotOcrTextRange& selection);
    [[nodiscard]] int cursorPositionAt(const QPointF& itemPosition) const;

    [[nodiscard]] QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget = nullptr) override;

  private:
    struct VerticalGlyph {
        int textStart = 0;
        int textLength = 0;
        bool rotated = false;
        std::unique_ptr<QTextLayout> layout;
        QRectF inkBounds;
    };

    QString m_text;
    QFont m_font;
    QColor m_foreground;
    ScreenshotOcrTextRange m_selection;
    ScreenshotOcrTextDirection m_direction = ScreenshotOcrTextDirection::Horizontal;
    std::unique_ptr<QTextLayout> m_layout;
    QTextLine m_line;
    std::vector<VerticalGlyph> m_verticalGlyphs;
    QVector<int> m_graphemeBoundaries;
    qreal m_verticalCellAdvance = 0.0;
    qreal m_targetAspectRatio = 0.0;
    QPointF m_layoutOrigin;
    QRectF m_bounds;
};

namespace {
constexpr double kSelectionBorderWidth = 2.0;
constexpr double kSelectionHandleRadius = 4.0;
constexpr double kSelectionHandleStrokeWidth = 1.5;
constexpr double kShowEndHandlesMinSize = 32.0;
constexpr double kShowMidHandlesMinSize = 64.0;
constexpr int kSelectionUpdatePadding = 10;
constexpr int kSelectionBorderUpdatePadding = 3;
constexpr int kSelectionHandleUpdatePadding = 6;
constexpr int kGuideLineUpdatePadding = 1;
constexpr qreal kOcrTextInkSafetyMargin = 1.0;
constexpr int kOcrTextLayoutMinPixelSize = 32;
constexpr qreal kOcrTextCrossAxisScale = 0.9;

#if defined(SNOW_SHOT_BENCH_INTERNALS)
thread_local QRegion g_selectionDamageRegion;
thread_local std::size_t g_selectionDamagePathFallbacks = 0;
thread_local QRegion g_guideLineDamageRegion;
thread_local std::size_t g_guideLineUpdateRequests = 0;

std::size_t regionPixelCount(const QRegion& region) {
    std::size_t pixels = 0;
    for (const QRect& rectangle : region) {
        pixels += static_cast<std::size_t>(rectangle.width()) *
                  static_cast<std::size_t>(rectangle.height());
    }
    return pixels;
}
#endif

qreal edgeLength(const QPointF& first, const QPointF& second) {
    return std::hypot(second.x() - first.x(), second.y() - first.y());
}

QPolygonF mappedQuad(const QPolygonF& quad, const QTransform& transform) {
    return transform.map(quad);
}

qreal linearColor(qreal value) {
    value /= 255.0;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

qreal relativeLuminance(const QColor& color) {
    return 0.2126 * linearColor(color.red()) + 0.7152 * linearColor(color.green()) +
           0.0722 * linearColor(color.blue());
}

qreal contrastRatio(const QColor& first, const QColor& second) {
    const qreal firstLuminance = relativeLuminance(first);
    const qreal secondLuminance = relativeLuminance(second);
    const qreal light = std::max(firstLuminance, secondLuminance);
    const qreal dark = std::min(firstLuminance, secondLuminance);
    return (light + 0.05) / (dark + 0.05);
}

QColor readableForeground(const ScreenshotOcrPresentation& presentation,
                          const ScreenshotOcrLine& line) {
    QColor background(Qt::white);
    const QPointF center =
        line.quad.boundingRect().center() - QPointF(presentation.selection.topLeft());
    const QPoint sample(qBound(0, qRound(center.x()), presentation.filledImage.width() - 1),
                        qBound(0, qRound(center.y()), presentation.filledImage.height() - 1));
    if (!presentation.filledImage.isNull() && presentation.filledImage.rect().contains(sample)) {
        background = presentation.filledImage.pixelColor(sample);
    }
    if (line.foreground.isValid() && contrastRatio(line.foreground, background) >= 4.5) {
        return line.foreground;
    }
    const QColor black(Qt::black);
    const QColor white(Qt::white);
    return contrastRatio(black, background) >= contrastRatio(white, background) ? black : white;
}

QRectF unitedValidBounds(const QRectF& current, const QRectF& candidate, bool* hasBounds) {
    if (hasBounds == nullptr || !candidate.isValid() || candidate.isEmpty()) {
        return current;
    }
    if (!*hasBounds) {
        *hasBounds = true;
        return candidate;
    }
    return current.united(candidate);
}

QRectF glyphRunInkBounds(const QGlyphRun& run) {
    // Logical run bounds include font metrics padding. Raw glyph bounds are
    // the painted ink authority, including bearings and variable-font shapes.
    const QRawFont rawFont = run.rawFont();
    if (rawFont.isValid()) {
        const QList<quint32> glyphIndexes = run.glyphIndexes();
        const QList<QPointF> positions = run.positions();
        const qsizetype count = std::min(glyphIndexes.size(), positions.size());
        QRectF bounds;
        bool hasBounds = false;
        for (qsizetype index = 0; index < count; ++index) {
            const QRectF glyphBounds =
                rawFont.boundingRect(glyphIndexes.at(index)).translated(positions.at(index));
            bounds = unitedValidBounds(bounds, glyphBounds, &hasBounds);
        }
        if (hasBounds) {
            return bounds;
        }
    }

    const QRectF runBounds = run.boundingRect();
    return runBounds.isValid() && !runBounds.isEmpty() ? runBounds : QRectF();
}

QRectF textLayoutInkBounds(const QTextLayout& layout) {
    QRectF bounds;
    bool hasBounds = false;
    for (const QGlyphRun& run : layout.glyphRuns()) {
        bounds = unitedValidBounds(bounds, glyphRunInkBounds(run), &hasBounds);
    }
    return hasBounds ? bounds : QRectF();
}

QVector<int> graphemeBoundaries(const QString& text) {
    QVector<int> boundaries;
    boundaries.push_back(0);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.toStart();
    while (true) {
        const qsizetype boundary = finder.toNextBoundary();
        if (boundary < 0) {
            break;
        }
        if (boundaries.constLast() != boundary) {
            boundaries.push_back(static_cast<int>(boundary));
        }
    }
    if (boundaries.constLast() != text.size()) {
        boundaries.push_back(static_cast<int>(text.size()));
    }
    return boundaries;
}

std::unique_ptr<QTextLayout> createSingleLineLayout(const QString& text, const QFont& font,
                                                    QTextLine* outLine = nullptr) {
    auto layout = std::make_unique<QTextLayout>(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setUseDesignMetrics(true);
    layout->setTextOption(option);
    layout->beginLayout();
    QTextLine line = layout->createLine();
    if (line.isValid()) {
        line.setLineWidth(std::numeric_limits<qreal>::max() / 4.0);
        line.setPosition(QPointF(0.0, 0.0));
    }
    layout->endLayout();
    if (outLine != nullptr) {
        *outLine = line;
    }
    return layout;
}

struct MeasuredSingleLineLayout {
    std::unique_ptr<QTextLayout> layout;
    QTextLine line;
    QRectF visualBounds;
};

MeasuredSingleLineLayout createMeasuredSingleLineLayout(const QString& text, const QFont& font) {
    MeasuredSingleLineLayout measured;
    measured.layout = createSingleLineLayout(text, font, &measured.line);
    measured.visualBounds = textLayoutInkBounds(*measured.layout);
    if (!measured.visualBounds.isValid() || measured.visualBounds.isEmpty()) {
        measured.visualBounds = measured.layout->boundingRect();
    }
    if (!measured.visualBounds.isValid() || measured.visualBounds.isEmpty()) {
        measured.visualBounds = QRectF(0.0, 0.0, 1.0, 1.0);
    }
    measured.visualBounds.adjust(-kOcrTextInkSafetyMargin, -kOcrTextInkSafetyMargin,
                                 kOcrTextInkSafetyMargin, kOcrTextInkSafetyMargin);
    return measured;
}

MeasuredSingleLineLayout createWidthExpandedSingleLineLayout(const QString& text,
                                                              const QFont& font,
                                                              qreal targetAspectRatio) {
    MeasuredSingleLineLayout baseline = createMeasuredSingleLineLayout(text, font);
    const qreal baselineWidth = baseline.visualBounds.width();
    const qreal baselineHeight = baseline.visualBounds.height();
    const QVector<int> boundaries = graphemeBoundaries(text);
    const int graphemeCount = std::max(0, static_cast<int>(boundaries.size()) - 1);
    if (graphemeCount < 2 || targetAspectRatio <= 0.0 || baselineHeight <= 0.0 ||
        baselineWidth / baselineHeight >= targetAspectRatio) {
        return baseline;
    }

    const qreal targetWidth = targetAspectRatio * baselineHeight;
    const qreal targetExtraWidth = targetWidth - baselineWidth;
    qreal letterSpacing = targetExtraWidth / static_cast<qreal>(graphemeCount - 1);
    qreal bestError = targetExtraWidth;
    MeasuredSingleLineLayout best = std::move(baseline);

    // Absolute letter spacing is nearly linear in Qt's text shaper, but the
    // number of positioned glyphs can differ from the grapheme count. A small
    // correction loop handles ligatures and combining sequences without
    // stretching the glyph outlines themselves.
    for (int attempt = 0; attempt < 3 && std::isfinite(letterSpacing) && letterSpacing > 0.0;
         ++attempt) {
        QFont spacedFont = font;
        spacedFont.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
        MeasuredSingleLineLayout candidate =
            createMeasuredSingleLineLayout(text, spacedFont);
        const qreal candidateWidth = candidate.visualBounds.width();
        const qreal candidateHeight = candidate.visualBounds.height();
        const qreal error =
            std::abs(candidateWidth - targetAspectRatio * candidateHeight);
        if (error < bestError) {
            bestError = error;
            best = std::move(candidate);
        }

        const qreal measuredExtraWidth = candidateWidth - baselineWidth;
        if (measuredExtraWidth <= 0.0) {
            letterSpacing *= 2.0;
            continue;
        }
        const qreal correctedSpacing = letterSpacing * targetExtraWidth / measuredExtraWidth;
        if (qFuzzyCompare(1.0 + correctedSpacing, 1.0 + letterSpacing)) {
            break;
        }
        letterSpacing = correctedSpacing;
    }
    return best;
}

char32_t verticalPresentationForm(char32_t character) {
    switch (character) {
    case U',':
    case U'\uff0c':
        return U'\ufe10';
    case U'\u3001':
        return U'\ufe11';
    case U'.':
    case U'\u3002':
        return U'\ufe12';
    case U':':
    case U'\uff1a':
        return U'\ufe13';
    case U';':
    case U'\uff1b':
        return U'\ufe14';
    case U'!':
    case U'\uff01':
        return U'\ufe15';
    case U'?':
    case U'\uff1f':
        return U'\ufe16';
    case U'\u3016':
        return U'\ufe17';
    case U'\u3017':
        return U'\ufe18';
    case U'\u2026':
        return U'\ufe19';
    case U'\u2014':
        return U'\ufe31';
    case U'\u2013':
        return U'\ufe32';
    case U'_':
    case U'\uff3f':
        return U'\ufe33';
    case U'(':
    case U'\uff08':
        return U'\ufe35';
    case U')':
    case U'\uff09':
        return U'\ufe36';
    case U'{':
    case U'\uff5b':
        return U'\ufe37';
    case U'}':
    case U'\uff5d':
        return U'\ufe38';
    case U'\u3014':
        return U'\ufe39';
    case U'\u3015':
        return U'\ufe3a';
    case U'\u3010':
        return U'\ufe3b';
    case U'\u3011':
        return U'\ufe3c';
    case U'\u300a':
        return U'\ufe3d';
    case U'\u300b':
        return U'\ufe3e';
    case U'\u3008':
        return U'\ufe3f';
    case U'\u3009':
        return U'\ufe40';
    case U'\u300c':
        return U'\ufe41';
    case U'\u300d':
        return U'\ufe42';
    case U'\u300e':
        return U'\ufe43';
    case U'\u300f':
        return U'\ufe44';
    case U'[':
    case U'\uff3b':
        return U'\ufe47';
    case U']':
    case U'\uff3d':
        return U'\ufe48';
    default:
        return 0;
    }
}

QString verticalDisplayText(const QString& grapheme, bool* rotated) {
    const QList<uint> codePoints = grapheme.toUcs4();
    if (codePoints.isEmpty()) {
        if (rotated != nullptr) {
            *rotated = false;
        }
        return grapheme;
    }

    if (codePoints.size() == 1) {
        const char32_t vertical = verticalPresentationForm(codePoints.constFirst());
        if (vertical != 0) {
            if (rotated != nullptr) {
                *rotated = false;
            }
            return QString::fromUcs4(&vertical, 1);
        }
    }

    const char32_t first = codePoints.constFirst();
    const bool rotate = first <= 0x02ff || (first >= 0x0370 && first <= 0x052f) ||
                        (first >= 0x0590 && first <= 0x10ff);
    if (rotated != nullptr) {
        *rotated = rotate;
    }
    return grapheme;
}

bool quadTransform(const QPolygonF& destination, qreal width, qreal height,
                   QTransform* outTransform) {
    if (outTransform == nullptr || destination.size() != 4 || width <= 0.0 || height <= 0.0) {
        return false;
    }
    const QPolygonF source({
        QPointF(0.0, 0.0),
        QPointF(width, 0.0),
        QPointF(width, height),
        QPointF(0.0, height),
    });
    return QTransform::quadToQuad(source, destination, *outTransform);
}

QPointF interpolatePoint(const QPointF& first, const QPointF& second, qreal amount) {
    return first + (second - first) * amount;
}

QPolygonF ocrTextFitQuad(const QPolygonF& quad, ScreenshotOcrTextDirection direction) {
    if (quad.size() != 4) {
        return quad;
    }

    // Keep the text centered while reserving 10% of its cross-axis region as
    // visual breathing room. The source quad remains unchanged for OCR fills
    // and hit testing.
    const qreal inset = (1.0 - kOcrTextCrossAxisScale) / 2.0;
    if (direction == ScreenshotOcrTextDirection::Vertical) {
        return QPolygonF({
            interpolatePoint(quad.at(0), quad.at(1), inset),
            interpolatePoint(quad.at(1), quad.at(0), inset),
            interpolatePoint(quad.at(2), quad.at(3), inset),
            interpolatePoint(quad.at(3), quad.at(2), inset),
        });
    }

    return QPolygonF({
        interpolatePoint(quad.at(0), quad.at(3), inset),
        interpolatePoint(quad.at(1), quad.at(2), inset),
        interpolatePoint(quad.at(2), quad.at(1), inset),
        interpolatePoint(quad.at(3), quad.at(0), inset),
    });
}

bool aspectFitQuadTransform(const QPolygonF& destination, qreal sourceWidth, qreal sourceHeight,
                            QTransform* outTransform) {
    if (destination.size() != 4 || sourceWidth <= 0.0 || sourceHeight <= 0.0 ||
        outTransform == nullptr) {
        return false;
    }

    const qreal destinationWidth = (edgeLength(destination.at(0), destination.at(1)) +
                                    edgeLength(destination.at(3), destination.at(2))) /
                                   2.0;
    const qreal destinationHeight = (edgeLength(destination.at(0), destination.at(3)) +
                                     edgeLength(destination.at(1), destination.at(2))) /
                                    2.0;
    if (destinationWidth <= 0.0 || destinationHeight <= 0.0) {
        return false;
    }

    const qreal scale = std::min(destinationWidth / sourceWidth, destinationHeight / sourceHeight);
    const qreal fittedWidth = sourceWidth * scale;
    const qreal fittedHeight = sourceHeight * scale;
    const QRectF fittedRect((destinationWidth - fittedWidth) / 2.0,
                            (destinationHeight - fittedHeight) / 2.0, fittedWidth, fittedHeight);

    QTransform destinationProjection;
    if (!quadTransform(destination, destinationWidth, destinationHeight, &destinationProjection)) {
        return false;
    }
    const QPolygonF fittedQuad = destinationProjection.map(QPolygonF({
        fittedRect.topLeft(),
        fittedRect.topRight(),
        fittedRect.bottomRight(),
        fittedRect.bottomLeft(),
    }));
    return quadTransform(fittedQuad, sourceWidth, sourceHeight, outTransform);
}

QColor selectionAccentColor(int alpha = 255) {
    return QColor(0x40, 0x96, 0xff, alpha);
}

QPainterPath selectionShapePath(const QRectF& selection, int cornerRadius,
                                const QTransform& canvasToViewTransform, qreal inset = 0.0) {
    QRectF viewRect = canvasToViewTransform.mapRect(selection.normalized());
    viewRect.adjust(inset, inset, -inset, -inset);

    QPainterPath path;
    if (!viewRect.isValid() || viewRect.isEmpty()) {
        return path;
    }

    const qreal canvasRadius = std::min<qreal>(
        std::max(0, cornerRadius), std::min(selection.width(), selection.height()) / 2.0);
    if (canvasRadius <= 0.0) {
        path.addRect(viewRect);
        return path;
    }

    const qreal horizontalScale =
        std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12());
    const qreal verticalScale =
        std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22());
    const qreal horizontalRadius =
        std::clamp(canvasRadius * horizontalScale - inset, 0.0, viewRect.width() / 2.0);
    const qreal verticalRadius =
        std::clamp(canvasRadius * verticalScale - inset, 0.0, viewRect.height() / 2.0);
    path.addRoundedRect(viewRect, horizontalRadius, verticalRadius, Qt::AbsoluteSize);
    return path;
}

QRectF snappedOutwardToDevicePixels(const QRectF& rect, qreal devicePixelRatio) {
    if (!rect.isValid() || rect.isEmpty() || devicePixelRatio <= 0.0) {
        return rect;
    }

    const qreal left = std::floor(rect.left() * devicePixelRatio) / devicePixelRatio;
    const qreal top = std::floor(rect.top() * devicePixelRatio) / devicePixelRatio;
    const qreal right = std::ceil(rect.right() * devicePixelRatio) / devicePixelRatio;
    const qreal bottom = std::ceil(rect.bottom() * devicePixelRatio) / devicePixelRatio;
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

void renderSelectionShadow(QPainter& painter, const SnowCanvasRenderContext& context,
                           const QRectF& selection, int cornerRadius, int shadowWidth,
                           const QColor& shadowColor) {
    if (shadowWidth <= 0) {
        return;
    }

    const qreal horizontalScale =
        std::hypot(context.canvasToViewTransform.m11(), context.canvasToViewTransform.m12());
    const qreal verticalScale =
        std::hypot(context.canvasToViewTransform.m21(), context.canvasToViewTransform.m22());
    const qreal scale = std::max<qreal>(1.0e-6, std::min(horizontalScale, verticalScale));
    const QRectF selectionView = context.canvasToViewTransform.mapRect(selection.normalized());
    if (!selectionView.isValid() || selectionView.isEmpty()) {
        return;
    }
    ScreenshotSelectionShadowRenderer::renderPreview(
        painter, selectionView, std::max(0, cornerRadius) * scale, shadowWidth * scale, shadowColor,
        context.devicePixelRatio);
}

bool hasSelectionBounds(const ScreenshotSelectionVisualState& state) {
    return state.present && state.bounds.isValid() && !state.bounds.isEmpty();
}

QRectF mappedSelectionBounds(const ScreenshotSelectionVisualState& state,
                             const QTransform& canvasToViewTransform) {
    return hasSelectionBounds(state) ? canvasToViewTransform.mapRect(state.bounds.normalized())
                                     : QRectF();
}

qreal viewScale(const QTransform& canvasToViewTransform) {
    return std::max<qreal>(
        1.0e-6, std::min(std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12()),
                         std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22())));
}

QRegion selectionStateDecorationRegion(const ScreenshotSelectionVisualState& state,
                                       const QRect& viewportRect,
                                       const QTransform& canvasToViewTransform) {
    const QRectF selectionBounds = mappedSelectionBounds(state, canvasToViewTransform);
    if (!selectionBounds.isValid() || selectionBounds.isEmpty()) {
        return {};
    }
    const qreal scale = viewScale(canvasToViewTransform);
    const qreal shadow = state.toolbarHovered ? std::max(0, state.shadowWidth) * scale : 0.0;
    const qreal padding =
        std::max<qreal>(kSelectionUpdatePadding, shadow + kSelectionBorderUpdatePadding);
    QRegion decoration(selectionBounds.adjusted(-padding, -padding, padding, padding)
                           .toAlignedRect()
                           .intersected(viewportRect));
    const QRect stableInterior = selectionBounds
                                     .adjusted(kSelectionUpdatePadding, kSelectionUpdatePadding,
                                               -kSelectionUpdatePadding, -kSelectionUpdatePadding)
                                     .toAlignedRect();
    if (!stableInterior.isEmpty()) {
        decoration -= QRegion(stableInterior.intersected(viewportRect));
    }

    if (!state.toolbarHovered && state.handlesVisible) {
        const double minSide = std::min(selectionBounds.width(), selectionBounds.height());
        std::array<QPointF, 8> handles{};
        std::size_t handleCount = 0;
        if (minSide > kShowEndHandlesMinSize) {
            handles[handleCount++] = selectionBounds.topLeft();
            handles[handleCount++] = selectionBounds.topRight();
            handles[handleCount++] = selectionBounds.bottomRight();
            handles[handleCount++] = selectionBounds.bottomLeft();
        }
        if (minSide > kShowMidHandlesMinSize) {
            handles[handleCount++] = QPointF(selectionBounds.center().x(), selectionBounds.top());
            handles[handleCount++] = QPointF(selectionBounds.right(), selectionBounds.center().y());
            handles[handleCount++] =
                QPointF(selectionBounds.center().x(), selectionBounds.bottom());
            handles[handleCount++] = QPointF(selectionBounds.left(), selectionBounds.center().y());
        }
        for (std::size_t index = 0; index < handleCount; ++index) {
            decoration +=
                QRegion(QRectF(handles[index].x() - kSelectionHandleUpdatePadding,
                               handles[index].y() - kSelectionHandleUpdatePadding,
                               kSelectionHandleUpdatePadding * 2, kSelectionHandleUpdatePadding * 2)
                            .toAlignedRect()
                            .intersected(viewportRect));
        }
    }
    return decoration;
}

QRegion selectionStateMaskRegion(const ScreenshotSelectionVisualState& state,
                                 const QRect& viewportRect,
                                 const QTransform& canvasToViewTransform) {
    if (!hasSelectionBounds(state)) {
        return {};
    }
    const QRectF mapped = canvasToViewTransform.mapRect(state.bounds.normalized());
    return QRegion(mapped.adjusted(-1.0, -1.0, 1.0, 1.0).toAlignedRect().intersected(viewportRect));
}

QRegion selectionStateRoundedCornerRegion(const ScreenshotSelectionVisualState& state,
                                          const QRect& viewportRect,
                                          const QTransform& canvasToViewTransform) {
    if (!hasSelectionBounds(state) || state.cornerRadius <= 0) {
        return {};
    }

    const QRectF mapped = mappedSelectionBounds(state, canvasToViewTransform);
    if (!mapped.isValid() || mapped.isEmpty()) {
        return {};
    }

    const qreal canvasRadius = std::min<qreal>(
        state.cornerRadius, std::min(state.bounds.width(), state.bounds.height()) / 2.0);
    const qreal horizontalScale =
        std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12());
    const qreal verticalScale =
        std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22());
    const qreal radiusX = std::min(mapped.width() / 2.0, canvasRadius * horizontalScale);
    const qreal radiusY = std::min(mapped.height() / 2.0, canvasRadius * verticalScale);
    constexpr qreal padding = 2.0;
    QRegion corners;
    corners += QRectF(mapped.left() - padding, mapped.top() - padding, radiusX + padding * 2.0,
                      radiusY + padding * 2.0)
                   .toAlignedRect();
    corners += QRectF(mapped.right() - radiusX - padding, mapped.top() - padding,
                      radiusX + padding * 2.0, radiusY + padding * 2.0)
                   .toAlignedRect();
    corners += QRectF(mapped.left() - padding, mapped.bottom() - radiusY - padding,
                      radiusX + padding * 2.0, radiusY + padding * 2.0)
                   .toAlignedRect();
    corners += QRectF(mapped.right() - radiusX - padding, mapped.bottom() - radiusY - padding,
                      radiusX + padding * 2.0, radiusY + padding * 2.0)
                   .toAlignedRect();
    return corners.intersected(viewportRect);
}

QColor normalizedGuideLineColor(const QColor& color) {
    return color.isValid() && color.alpha() > 0 ? color : QColor(0, 0, 0, 0);
}

QPoint guideLinePixelPosition(const QPointF& position) {
    return QPoint(qFloor(position.x()), qFloor(position.y()));
}

QRegion guideLineVerticalRegion(const QRect& viewportRect, int x) {
    if (viewportRect.isEmpty()) {
        return {};
    }

    return QRegion(QRect(x - kGuideLineUpdatePadding, viewportRect.top(),
                         kGuideLineUpdatePadding * 2 + 1, viewportRect.height()))
        .intersected(viewportRect);
}

QRegion guideLineHorizontalRegion(const QRect& viewportRect, int y) {
    if (viewportRect.isEmpty()) {
        return {};
    }

    return QRegion(QRect(viewportRect.left(), y - kGuideLineUpdatePadding, viewportRect.width(),
                         kGuideLineUpdatePadding * 2 + 1))
        .intersected(viewportRect);
}

QRegion guideLineCrosshairRegion(const QRect& viewportRect, const QPoint& center) {
    return guideLineVerticalRegion(viewportRect, center.x()) +
           guideLineHorizontalRegion(viewportRect, center.y());
}

QPoint monitorCenterGuideLinePosition(const QRect& viewportRect) {
    const QPointF center = QRectF(viewportRect).center();
    return guideLinePixelPosition(center);
}

QRegion planGuideLineDamage(const QRect& viewportRect, const QPoint& previousCursorPosition,
                            const QColor& previousCursorColor,
                            const QColor& previousMonitorCenterColor,
                            const QPoint& nextCursorPosition, const QColor& nextCursorColor,
                            const QColor& nextMonitorCenterColor) {
    QRegion dirtyRegion;
    const bool cursorColorChanged = previousCursorColor != nextCursorColor;
    if (cursorColorChanged || previousCursorPosition.x() != nextCursorPosition.x()) {
        if (previousCursorColor.alpha() > 0) {
            dirtyRegion += guideLineVerticalRegion(viewportRect, previousCursorPosition.x());
        }
        if (nextCursorColor.alpha() > 0) {
            dirtyRegion += guideLineVerticalRegion(viewportRect, nextCursorPosition.x());
        }
    }
    if (cursorColorChanged || previousCursorPosition.y() != nextCursorPosition.y()) {
        if (previousCursorColor.alpha() > 0) {
            dirtyRegion += guideLineHorizontalRegion(viewportRect, previousCursorPosition.y());
        }
        if (nextCursorColor.alpha() > 0) {
            dirtyRegion += guideLineHorizontalRegion(viewportRect, nextCursorPosition.y());
        }
    }
    if (previousMonitorCenterColor != nextMonitorCenterColor &&
        (previousMonitorCenterColor.alpha() > 0 || nextMonitorCenterColor.alpha() > 0)) {
        dirtyRegion +=
            guideLineCrosshairRegion(viewportRect, monitorCenterGuideLinePosition(viewportRect));
    }
    return dirtyRegion;
}

} // namespace

QRegion planScreenshotGuideLineDamage(const QRect& viewportRect,
                                      const QPoint& previousCursorPosition,
                                      const QColor& previousCursorColor,
                                      const QColor& previousMonitorCenterColor,
                                      const QPoint& nextCursorPosition,
                                      const QColor& nextCursorColor,
                                      const QColor& nextMonitorCenterColor) {
    return planGuideLineDamage(viewportRect, previousCursorPosition, previousCursorColor,
                               previousMonitorCenterColor, nextCursorPosition, nextCursorColor,
                               nextMonitorCenterColor);
}

QRegion planScreenshotSelectionDamage(const ScreenshotSelectionVisualState& previous,
                                      const ScreenshotSelectionVisualState& next,
                                      const QRect& viewportRect,
                                      const QTransform& canvasToViewTransform, bool maskVisible) {
    if (previous == next || viewportRect.isEmpty()) {
        return {};
    }
    if (!canvasToViewTransform.isInvertible()) {
        return QRegion(viewportRect);
    }
    QRegion dirtyRegion =
        selectionStateDecorationRegion(previous, viewportRect, canvasToViewTransform);
    dirtyRegion += selectionStateDecorationRegion(next, viewportRect, canvasToViewTransform);
    if (previous.bounds != next.bounds || previous.cornerRadius != next.cornerRadius ||
        previous.toolbarHovered != next.toolbarHovered) {
        dirtyRegion +=
            selectionStateRoundedCornerRegion(previous, viewportRect, canvasToViewTransform);
        dirtyRegion += selectionStateRoundedCornerRegion(next, viewportRect, canvasToViewTransform);
    }
    if (maskVisible) {
        dirtyRegion +=
            selectionStateMaskRegion(previous, viewportRect, canvasToViewTransform)
                .xored(selectionStateMaskRegion(next, viewportRect, canvasToViewTransform));
    }
    return dirtyRegion.intersected(viewportRect);
}

namespace {
QRectF sourcePixelsForImageLayer(const ScreenshotImageLayer& layer) {
    if (!layer.isValid()) {
        return {};
    }
    const qreal scaleX = layer.image.width() / layer.imageCanvasRect.width();
    const qreal scaleY = layer.image.height() / layer.imageCanvasRect.height();
    return QRectF((layer.destinationCanvasRect.left() - layer.imageCanvasRect.left()) * scaleX,
                  (layer.destinationCanvasRect.top() - layer.imageCanvasRect.top()) * scaleY,
                  layer.destinationCanvasRect.width() * scaleX,
                  layer.destinationCanvasRect.height() * scaleY);
}

constexpr qreal kMaximumRasterSourceCoordinate = 32768.0;
constexpr qreal kMaximumRasterSourceScale = 16384.0;
constexpr qreal kMaximumSourceChunkSpan = 2048.0;
constexpr qreal kMinimumSourceSamplingPadding = 1.0;

bool finiteRect(const QRectF& rect) {
    return std::isfinite(rect.left()) && std::isfinite(rect.top()) &&
           std::isfinite(rect.width()) && std::isfinite(rect.height());
}

QImage imageWindow(const QImage& image, const QRect& bounds) {
    if (image.isNull() || bounds.isEmpty() || !image.rect().contains(bounds)) {
        return {};
    }

    // A read-only QImage view keeps the source pixels shared while rebasing the coordinates.
    // Screenshot images are normally 32-bit; copy is retained for indexed and packed formats
    // whose palette or bit offset cannot be represented by the public QImage view constructor.
    const int depth = image.depth();
    if (depth > 0 && depth % 8 == 0 && image.colorTable().isEmpty()) {
        const int bytesPerPixel = depth / 8;
        const uchar* data = image.constScanLine(bounds.top()) +
                            static_cast<qsizetype>(bounds.left()) * bytesPerPixel;
        const qsizetype byteOffset = data - image.constBits();
        const qsizetype availableBytes = image.sizeInBytes() - byteOffset;
        const qsizetype requiredBytes = image.bytesPerLine() * bounds.height();
        const bool scanLinesAligned =
            reinterpret_cast<quintptr>(data) % alignof(quint32) == 0;
        if (scanLinesAligned && requiredBytes <= availableBytes) {
            QImage view(data, bounds.width(), bounds.height(), image.bytesPerLine(),
                        image.format());
            if (!view.isNull()) {
                view.setColorSpace(image.colorSpace());
                view.setDevicePixelRatio(1.0);
                return view;
            }
        }
    }

    QImage copy = image.copy(bounds);
    if (!copy.isNull()) {
        copy.setDevicePixelRatio(1.0);
    }
    return copy;
}

bool sourceMappingFitsRaster(const QRectF& source, qreal sourcePerTargetX,
                             qreal sourcePerTargetY) {
    return finiteRect(source) && source.left() >= 0.0 && source.top() >= 0.0 &&
           source.right() < kMaximumRasterSourceCoordinate &&
           source.bottom() < kMaximumRasterSourceCoordinate &&
           sourcePerTargetX > 0.0 && sourcePerTargetY > 0.0 &&
           sourcePerTargetX <= kMaximumRasterSourceScale &&
           sourcePerTargetY <= kMaximumRasterSourceScale;
}

void paintScaledSourceWindow(QPainter& painter, const QRectF& targetWindow,
                             const QImage& image, const QRect& sourceBounds) {
    if (targetWindow.isEmpty() || sourceBounds.isEmpty()) {
        return;
    }
    const QImage sourceWindow = imageWindow(image, sourceBounds);
    if (sourceWindow.isNull()) {
        return;
    }

    // Scale in QImage, whose transform path does not use the raster paint engine's 16.16
    // source-coordinate representation. The resulting image is deliberately bounded so the
    // final painter call itself remains inside that representation as well.
    const QRectF deviceWindow = painter.deviceTransform().mapRect(targetWindow);
    constexpr int kMaximumScaledDimension =
        static_cast<int>(kMaximumRasterSourceCoordinate) - 4;
    const int scaledWidth = std::clamp(qCeil(std::abs(deviceWindow.width())), 1,
                                       kMaximumScaledDimension);
    const int scaledHeight = std::clamp(qCeil(std::abs(deviceWindow.height())), 1,
                                        kMaximumScaledDimension);
    const Qt::TransformationMode mode =
        painter.testRenderHint(QPainter::SmoothPixmapTransform) ? Qt::SmoothTransformation
                                                                 : Qt::FastTransformation;
    QImage scaled = sourceWindow.scaled(QSize(scaledWidth, scaledHeight), Qt::IgnoreAspectRatio,
                                        mode);
    if (scaled.isNull()) {
        return;
    }
    scaled.setDevicePixelRatio(1.0);
    painter.drawImage(targetWindow, scaled, QRectF(scaled.rect()));
}

void paintExposedImageSlice(QPainter& painter, const QRectF& targetRect, const QImage& image,
                            const QRectF& sourceRect, const QRegion& exposedRegion) {
    if (image.isNull() || !finiteRect(targetRect) || !targetRect.isValid() ||
        targetRect.isEmpty() || !finiteRect(sourceRect) || !sourceRect.isValid() ||
        sourceRect.isEmpty() || exposedRegion.isEmpty()) {
        return;
    }

    const qreal sourcePerTargetX = sourceRect.width() / targetRect.width();
    const qreal sourcePerTargetY = sourceRect.height() / targetRect.height();
    if (!std::isfinite(sourcePerTargetX) || !std::isfinite(sourcePerTargetY) ||
        sourcePerTargetX <= 0.0 || sourcePerTargetY <= 0.0) {
        return;
    }

    const QRectF drawableSource = sourceRect.intersected(QRectF(image.rect()));
    if (!drawableSource.isValid() || drawableSource.isEmpty()) {
        return;
    }
    const QRectF drawableTarget(
        targetRect.left() + (drawableSource.left() - sourceRect.left()) / sourcePerTargetX,
        targetRect.top() + (drawableSource.top() - sourceRect.top()) / sourcePerTargetY,
        drawableSource.width() / sourcePerTargetX, drawableSource.height() / sourcePerTargetY);
    if (!drawableTarget.isValid() || drawableTarget.isEmpty()) {
        return;
    }

    // Keep the ordinary path identical to the old single draw. The explicit clip makes this
    // correct even when a caller supplies a damage region without clipping its painter first.
    if (sourceMappingFitsRaster(sourceRect, sourcePerTargetX, sourcePerTargetY)) {
        painter.save();
        painter.setClipRegion(exposedRegion, Qt::IntersectClip);
        painter.drawImage(targetRect, image, sourceRect);
        painter.restore();
        return;
    }

    const auto sourceForTarget = [&](const QRectF& target) {
        return QRectF(sourceRect.left() + (target.left() - targetRect.left()) * sourcePerTargetX,
                      sourceRect.top() + (target.top() - targetRect.top()) * sourcePerTargetY,
                      target.width() * sourcePerTargetX, target.height() * sourcePerTargetY);
    };
    const auto targetForSource = [&](const QRectF& source) {
        return QRectF(targetRect.left() + (source.left() - sourceRect.left()) / sourcePerTargetX,
                      targetRect.top() + (source.top() - sourceRect.top()) / sourcePerTargetY,
                      source.width() / sourcePerTargetX, source.height() / sourcePerTargetY);
    };
    const bool smoothSampling = painter.testRenderHint(QPainter::SmoothPixmapTransform);
    const qreal samplingPaddingX =
        smoothSampling ? std::max(kMinimumSourceSamplingPadding, sourcePerTargetX) : 0.0;
    const qreal samplingPaddingY =
        smoothSampling ? std::max(kMinimumSourceSamplingPadding, sourcePerTargetY) : 0.0;

    for (const QRect& exposedRectangle : exposedRegion) {
        const QRectF exposedTarget = drawableTarget.intersected(QRectF(exposedRectangle));
        if (!exposedTarget.isValid() || exposedTarget.isEmpty()) {
            continue;
        }

        const QRectF exposedSource = sourceForTarget(exposedTarget).intersected(drawableSource);
        if (!exposedSource.isValid() || exposedSource.isEmpty()) {
            continue;
        }

        // Keep each source chunk bounded in both dimensions. A ratio above the signed 16.16
        // increment limit is handled by QImage::scaled below, so that axis does not need to be
        // split into sub-pixel target cells.
        const int chunkCountX = sourcePerTargetX > kMaximumRasterSourceScale
                                    ? 1
                                    : qMax(1, qCeil(exposedSource.width() /
                                                    kMaximumSourceChunkSpan));
        const int chunkCountY = sourcePerTargetY > kMaximumRasterSourceScale
                                    ? 1
                                    : qMax(1, qCeil(exposedSource.height() /
                                                    kMaximumSourceChunkSpan));

        painter.save();
        painter.setClipRect(exposedRectangle, Qt::IntersectClip);
        for (int chunkY = 0; chunkY < chunkCountY; ++chunkY) {
            const qreal sourceTop =
                exposedSource.top() + exposedSource.height() * chunkY / chunkCountY;
            const qreal sourceBottom =
                exposedSource.top() + exposedSource.height() * (chunkY + 1) / chunkCountY;
            for (int chunkX = 0; chunkX < chunkCountX; ++chunkX) {
                const qreal sourceLeft =
                    exposedSource.left() + exposedSource.width() * chunkX / chunkCountX;
                const qreal sourceRight =
                    exposedSource.left() + exposedSource.width() * (chunkX + 1) / chunkCountX;
                const QRectF chunkSource(sourceLeft, sourceTop, sourceRight - sourceLeft,
                                         sourceBottom - sourceTop);
                const QRectF chunkTarget = targetForSource(chunkSource).intersected(exposedTarget);
                if (!chunkTarget.isValid() || chunkTarget.isEmpty()) {
                    continue;
                }

                const QRectF sampleSource =
                    chunkSource.adjusted(-samplingPaddingX, -samplingPaddingY, samplingPaddingX,
                                         samplingPaddingY)
                        .intersected(drawableSource);
                const QRect sourceBounds =
                    sampleSource.toAlignedRect().intersected(image.rect());
                if (!sampleSource.isValid() || sampleSource.isEmpty() || sourceBounds.isEmpty()) {
                    continue;
                }
                const QRectF sampleTarget = targetForSource(sampleSource);

                painter.save();
                painter.setClipRect(chunkTarget, Qt::IntersectClip);
                if (sourcePerTargetX > kMaximumRasterSourceScale ||
                    sourcePerTargetY > kMaximumRasterSourceScale) {
                    // This also handles a single target pixel representing tens of thousands of
                    // source pixels; splitting that target pixel cannot make the 16.16 increment
                    // finite, while QImage's scaler can reduce the source safely.
                    const QRectF targetWindow = targetForSource(QRectF(sourceBounds));
                    paintScaledSourceWindow(painter, targetWindow, image, sourceBounds);
                } else if (sourceMappingFitsRaster(sampleSource, sourcePerTargetX,
                                                   sourcePerTargetY)) {
                    painter.drawImage(sampleTarget, image, sampleSource);
                } else {
                    const QImage boundedSource = imageWindow(image, sourceBounds);
                    if (!boundedSource.isNull()) {
                        painter.drawImage(sampleTarget, boundedSource,
                                          sampleSource.translated(-sourceBounds.topLeft()));
                    }
                }
                painter.restore();
            }
        }
        painter.restore();
    }
}

void paintImageLayer(QPainter& painter, const ScreenshotImageLayer& layer,
                     const QTransform& canvasToTarget,
                     const QRegion* exposedRegion = nullptr) {
    const QRectF sourcePixels = sourcePixelsForImageLayer(layer);
    if (sourcePixels.isEmpty()) {
        return;
    }
    const QRectF targetRect = canvasToTarget.mapRect(layer.destinationCanvasRect);
    if (exposedRegion != nullptr) {
        paintExposedImageSlice(painter, targetRect, layer.image, sourcePixels, *exposedRegion);
    } else {
        painter.drawImage(targetRect, layer.image, sourcePixels);
    }
}
} // namespace

QImage materializeScreenshotImageSource(const ScreenshotImageSource& source,
                                        const QRectF& canvasRect, const QSize& pixelSize) {
    const QRectF normalizedCanvasRect = canvasRect.normalized();
    if (!source.isValid() || !normalizedCanvasRect.isValid() ||
        normalizedCanvasRect.isEmpty() || !pixelSize.isValid() || pixelSize.isEmpty()) {
        return {};
    }
    if (source.isMaterialized() && source.materializedCanvasRect == normalizedCanvasRect &&
        source.materializedImage.size() == pixelSize) {
        QImage image = source.materializedImage;
        if (image.format() != QImage::Format_ARGB32_Premultiplied) {
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        image.setDevicePixelRatio(1.0);
        return image;
    }

    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(1.0);
    image.fill(Qt::transparent);
    const qreal scaleX = pixelSize.width() / normalizedCanvasRect.width();
    const qreal scaleY = pixelSize.height() / normalizedCanvasRect.height();
    const QTransform canvasToImage(scaleX, 0.0, 0.0, scaleY,
                                   -normalizedCanvasRect.left() * scaleX,
                                   -normalizedCanvasRect.top() * scaleY);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (source.isMaterialized()) {
        paintImageLayer(painter,
                        ScreenshotImageLayer{source.materializedImage,
                                             source.materializedCanvasRect,
                                             source.materializedCanvasRect.intersected(
                                                 normalizedCanvasRect)},
                        canvasToImage);
    } else {
        for (const ScreenshotImageLayer& layer : source.layers) {
            ScreenshotImageLayer clipped = layer;
            clipped.destinationCanvasRect =
                clipped.destinationCanvasRect.intersected(normalizedCanvasRect);
            paintImageLayer(painter, clipped, canvasToImage);
        }
    }
    return image;
}

void ScreenshotOcrGraphicsTextItem::configure(const QString& text, const QFont& font,
                                               const QColor& foreground,
                                               const ScreenshotOcrTextRange& selection,
                                               ScreenshotOcrTextDirection direction,
                                               qreal targetAspectRatio) {
    targetAspectRatio = std::max<qreal>(0.0, targetAspectRatio);
    const bool selectionChanged =
        m_selection.start != selection.start || m_selection.length != selection.length;
    const bool hasLayout = direction == ScreenshotOcrTextDirection::Vertical
                               ? !m_verticalGlyphs.empty() || text.isEmpty()
                               : m_layout != nullptr;
    const bool aspectRatioMatches =
        qFuzzyCompare(1.0 + m_targetAspectRatio, 1.0 + targetAspectRatio);
    if (hasLayout && m_text == text && m_font == font && m_direction == direction &&
        aspectRatioMatches) {
        const bool foregroundChanged = m_foreground != foreground;
        m_foreground = foreground;
        m_selection = selection;
        if (foregroundChanged || selectionChanged) {
            update();
        }
        return;
    }

    prepareGeometryChange();
    m_text = text;
    m_font = font;
    m_foreground = foreground;
    m_selection = selection;
    m_direction = direction;
    m_targetAspectRatio = targetAspectRatio;
    m_layout.reset();
    m_line = QTextLine();
    m_verticalGlyphs.clear();
    m_graphemeBoundaries.clear();
    m_verticalCellAdvance = 0.0;

    if (direction == ScreenshotOcrTextDirection::Vertical) {
        m_graphemeBoundaries = graphemeBoundaries(text);
        const QFontMetricsF metrics(font);
        m_verticalCellAdvance = std::max<qreal>(1.0, metrics.height());
        qreal columnWidth = m_verticalCellAdvance;

        const int graphemeCount = std::max(0, static_cast<int>(m_graphemeBoundaries.size()) - 1);
        m_verticalGlyphs.reserve(static_cast<std::size_t>(graphemeCount));
        for (int index = 0; index < graphemeCount; ++index) {
            const int start = m_graphemeBoundaries.at(index);
            const int end = m_graphemeBoundaries.at(index + 1);
            bool rotated = false;
            const QString displayText = verticalDisplayText(text.mid(start, end - start), &rotated);
            auto glyphLayout = createSingleLineLayout(displayText, font);
            QRectF inkBounds = textLayoutInkBounds(*glyphLayout);
            if (!inkBounds.isValid() || inkBounds.isEmpty()) {
                inkBounds = glyphLayout->boundingRect();
            }
            if (!inkBounds.isValid() || inkBounds.isEmpty()) {
                inkBounds = QRectF(0.0, 0.0, 1.0, 1.0);
            }
            inkBounds.adjust(-kOcrTextInkSafetyMargin, -kOcrTextInkSafetyMargin,
                             kOcrTextInkSafetyMargin, kOcrTextInkSafetyMargin);
            const qreal orientedWidth = rotated ? inkBounds.height() : inkBounds.width();
            const qreal orientedHeight = rotated ? inkBounds.width() : inkBounds.height();
            columnWidth = std::max(columnWidth, orientedWidth);
            m_verticalCellAdvance = std::max(m_verticalCellAdvance, orientedHeight);
            m_verticalGlyphs.push_back(VerticalGlyph{
                start,
                end - start,
                rotated,
                std::move(glyphLayout),
                inkBounds,
            });
        }

        if (graphemeCount > 1 && targetAspectRatio > 0.0) {
            const qreal targetColumnHeight = columnWidth / targetAspectRatio;
            m_verticalCellAdvance =
                std::max(m_verticalCellAdvance,
                         targetColumnHeight / static_cast<qreal>(graphemeCount));
        }

        m_layoutOrigin = {};
        m_bounds = QRectF(0.0, 0.0, std::max<qreal>(1.0, columnWidth),
                          std::max<qreal>(1.0, m_verticalCellAdvance * graphemeCount));
    } else {
        MeasuredSingleLineLayout measured =
            createWidthExpandedSingleLineLayout(text, font, targetAspectRatio);
        m_layoutOrigin = -measured.visualBounds.topLeft();
        m_bounds = QRectF(0.0, 0.0, std::max<qreal>(1.0, measured.visualBounds.width()),
                          std::max<qreal>(1.0, measured.visualBounds.height()));
        m_line = measured.line;
        m_layout = std::move(measured.layout);
    }
    update();
}

void ScreenshotOcrGraphicsTextItem::setSelection(const ScreenshotOcrTextRange& selection) {
    if (m_selection.start == selection.start && m_selection.length == selection.length) {
        return;
    }
    m_selection = selection;
    update();
}

int ScreenshotOcrGraphicsTextItem::cursorPositionAt(const QPointF& itemPosition) const {
    if (m_direction == ScreenshotOcrTextDirection::Vertical) {
        if (m_graphemeBoundaries.isEmpty() || m_verticalCellAdvance <= 0.0) {
            return 0;
        }
        const int boundaryIndex = qBound(0, qRound(itemPosition.y() / m_verticalCellAdvance),
                                         static_cast<int>(m_graphemeBoundaries.size()) - 1);
        return m_graphemeBoundaries.at(boundaryIndex);
    }
    if (!m_line.isValid()) {
        return 0;
    }
    const qreal layoutX = itemPosition.x() - m_layoutOrigin.x();
    return qBound(0, m_line.xToCursor(layoutX, QTextLine::CursorBetweenCharacters),
                  static_cast<int>(m_text.size()));
}

QRectF ScreenshotOcrGraphicsTextItem::boundingRect() const {
    return m_bounds;
}

void ScreenshotOcrGraphicsTextItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
                                          QWidget* widget) {
    Q_UNUSED(option);
    if (painter == nullptr || m_text.isEmpty()) {
        return;
    }

    // AdTextEdit uses the standard Qt selection roles supplied by ant_design_qt's
    // theme palette. Read the same roles here so OCR selection has identical colors.
    const QPalette palette = widget != nullptr ? widget->palette() : QApplication::palette();
    const QColor selectionBackground = palette.highlight().color();
    const QColor selectionForeground = palette.highlightedText().color();

    painter->save();
    painter->setClipRect(m_bounds);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    painter->setPen(m_foreground);

    if (m_direction == ScreenshotOcrTextDirection::Vertical) {
        const int selectionStart = qBound(0, m_selection.start, static_cast<int>(m_text.size()));
        const int selectionEnd = qBound(selectionStart, selectionStart + m_selection.length,
                                        static_cast<int>(m_text.size()));
        for (std::size_t index = 0; index < m_verticalGlyphs.size(); ++index) {
            const VerticalGlyph& glyph = m_verticalGlyphs.at(index);
            const int glyphEnd = glyph.textStart + glyph.textLength;
            const bool selected = selectionStart < glyphEnd && selectionEnd > glyph.textStart;
            if (selected) {
                painter->fillRect(QRectF(0.0, static_cast<qreal>(index) * m_verticalCellAdvance,
                                         m_bounds.width(), m_verticalCellAdvance),
                                  selectionBackground);
            }
        }

        for (std::size_t index = 0; index < m_verticalGlyphs.size(); ++index) {
            const VerticalGlyph& glyph = m_verticalGlyphs.at(index);
            if (glyph.layout == nullptr) {
                continue;
            }
            const int glyphEnd = glyph.textStart + glyph.textLength;
            const bool selected = selectionStart < glyphEnd && selectionEnd > glyph.textStart;
            QVector<QTextLayout::FormatRange> formats;
            QTextLayout::FormatRange textFormat;
            textFormat.start = 0;
            textFormat.length = static_cast<int>(glyph.layout->text().size());
            textFormat.format.setForeground(QBrush(selected ? selectionForeground : m_foreground));
            formats.push_back(textFormat);

            painter->save();
            painter->translate(m_bounds.width() / 2.0,
                               (static_cast<qreal>(index) + 0.5) * m_verticalCellAdvance);
            if (glyph.rotated) {
                painter->rotate(90.0);
            }
            glyph.layout->draw(painter, -glyph.inkBounds.center(), formats);
            painter->restore();
        }
        painter->restore();
        return;
    }

    if (m_layout == nullptr) {
        painter->restore();
        return;
    }

    QVector<QTextLayout::FormatRange> formats;
    QTextLayout::FormatRange textFormat;
    textFormat.start = 0;
    textFormat.length = static_cast<int>(m_text.size());
    textFormat.format.setForeground(QBrush(m_foreground));
    formats.push_back(textFormat);
    if (!m_selection.empty()) {
        QTextLayout::FormatRange range;
        range.start = qBound(0, m_selection.start, static_cast<int>(m_text.size()));
        range.length = qBound(0, m_selection.length, static_cast<int>(m_text.size()) - range.start);
        range.format.setBackground(QBrush(selectionBackground));
        range.format.setForeground(QBrush(selectionForeground));
        formats.push_back(range);
    }

    m_layout->draw(painter, m_layoutOrigin, formats);
    painter->restore();
}

ScreenshotOcrTextLayer::ScreenshotOcrTextLayer(QWidget* parent)
    : QGraphicsView(parent), m_scene(new QGraphicsScene(this)) {
    setObjectName(QStringLiteral("snowShotOcrTextLayer"));
    setScene(m_scene);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setBackgroundBrush(Qt::NoBrush);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    viewport()->setAttribute(Qt::WA_NoSystemBackground, true);
    viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
    viewport()->setAutoFillBackground(false);
    setOptimizationFlag(QGraphicsView::DontSavePainterState, true);
    setStyleSheet(QStringLiteral("QGraphicsView#snowShotOcrTextLayer {"
                                 " background: transparent; border: none;"
                                 "}"));
    hide();
}

void ScreenshotOcrTextLayer::setPresentation(
    std::shared_ptr<ScreenshotOcrPresentation> presentation) {
    m_textItems.clear();
    m_scene->clear();
    m_presentation = std::move(presentation);
    if (m_presentation != nullptr) {
        m_presentation->prepareForRendering();
    }
    m_viewportRect = {};
    m_selectionAnchor = {};
    m_selectionFocus = {};
    m_selectionRevision = std::numeric_limits<quint64>::max();
    m_synchronized = false;
    rebuildTextItems();
    hide();
}

void ScreenshotOcrTextLayer::clearPresentation() {
    m_textItems.clear();
    m_scene->clear();
    m_presentation.reset();
    m_viewportRect = {};
    m_selectionAnchor = {};
    m_selectionFocus = {};
    m_selectionRevision = std::numeric_limits<quint64>::max();
    m_synchronized = false;
    hide();
}

void ScreenshotOcrTextLayer::synchronize(const QTransform& canvasToViewTransform,
                                         const QRect& viewportRect) {
    if (m_presentation == nullptr || m_textItems.empty() || viewportRect.isEmpty()) {
        hide();
        return;
    }

    const bool geometryChanged = !m_synchronized ||
                                 m_canvasToViewTransform != canvasToViewTransform ||
                                 m_viewportRect != viewportRect;
    if (geometryChanged && geometry() != viewportRect) {
        setGeometry(viewportRect);
    }
    if (geometryChanged) {
        setSceneRect(QRectF(QPointF(0.0, 0.0), viewportRect.size()));
        m_canvasToViewTransform = canvasToViewTransform;
        m_viewportRect = viewportRect;
        m_synchronized = true;
        for (TextItem& item : m_textItems) {
            synchronizeTextItem(item, canvasToViewTransform);
#if defined(SNOW_SHOT_BENCH_INTERNALS)
            ++m_geometrySynchronizationCount;
#endif
        }
    }
    updateSelection();
    show();
    raise();
    if (geometryChanged) {
        viewport()->update();
    }
}

void ScreenshotOcrTextLayer::updateSelection() {
    if (m_presentation == nullptr || m_selectionRevision == m_presentation->selectionRevision()) {
        return;
    }
    const ScreenshotOcrTextPosition nextAnchor = m_presentation->selectionAnchor();
    const ScreenshotOcrTextPosition nextFocus = m_presentation->selectionFocus();
    int firstLine = 0;
    int lastLine = static_cast<int>(m_textItems.size()) - 1;
    const bool anchorUnchanged = m_selectionAnchor.lineIndex == nextAnchor.lineIndex &&
                                 m_selectionAnchor.characterIndex == nextAnchor.characterIndex;
    if (m_selectionRevision != std::numeric_limits<quint64>::max() && anchorUnchanged &&
        m_selectionFocus.valid() && nextFocus.valid()) {
        firstLine = std::min(m_selectionFocus.lineIndex, nextFocus.lineIndex);
        lastLine = std::max(m_selectionFocus.lineIndex, nextFocus.lineIndex);
    }
    for (int lineIndex = firstLine; lineIndex <= lastLine; ++lineIndex) {
        TextItem& item = m_textItems.at(static_cast<std::size_t>(lineIndex));
        if (item.graphicsText != nullptr) {
            item.graphicsText->setSelection(m_presentation->textSelectionForLine(item.lineIndex));
        }
    }
    m_selectionAnchor = nextAnchor;
    m_selectionFocus = nextFocus;
    m_selectionRevision = m_presentation->selectionRevision();
}

ScreenshotOcrTextPosition ScreenshotOcrTextLayer::textPositionAt(const QPointF& canvasPosition,
                                                                 bool useClosestLine) const {
    if (m_presentation == nullptr) {
        return {};
    }

    ScreenshotOcrTextPosition position =
        m_presentation->textPositionAt(canvasPosition, useClosestLine);
    if (!position.valid() || !m_synchronized ||
        position.lineIndex >= static_cast<int>(m_textItems.size())) {
        return position;
    }

    const TextItem& item = m_textItems.at(static_cast<std::size_t>(position.lineIndex));
    if (item.graphicsText == nullptr || !item.graphicsText->isVisible()) {
        return position;
    }

    const QPointF viewPosition = m_canvasToViewTransform.map(canvasPosition);
    const QPointF itemPosition = item.graphicsText->mapFromScene(viewPosition);
    position.characterIndex = item.graphicsText->cursorPositionAt(itemPosition);
    return position;
}

void ScreenshotOcrTextLayer::rebuildTextItems() {
    if (m_presentation == nullptr) {
        return;
    }

    m_textItems.reserve(static_cast<std::size_t>(m_presentation->lines.size()));

    const auto appendTextItem = [this](int lineIndex) {
        auto* graphicsText = new ScreenshotOcrGraphicsTextItem;
        m_scene->addItem(graphicsText);
        m_textItems.push_back(TextItem{
            lineIndex,
            graphicsText,
        });
    };

    for (int lineIndex = 0; lineIndex < m_presentation->lines.size(); ++lineIndex) {
        appendTextItem(lineIndex);
    }
}

void ScreenshotOcrTextLayer::synchronizeTextItem(TextItem& item,
                                                  const QTransform& canvasToViewTransform) {
    if (m_presentation == nullptr || item.graphicsText == nullptr || item.lineIndex < 0 ||
        item.lineIndex >= m_presentation->lines.size()) {
        return;
    }

    const ScreenshotOcrLine& line = m_presentation->lines.at(item.lineIndex);
    const QString& text = line.text;
    const QPolygonF viewQuad = mappedQuad(line.quad, canvasToViewTransform);
    if (text.isEmpty() || viewQuad.size() != 4 ||
        !viewQuad.boundingRect().intersects(sceneRect())) {
        item.graphicsText->hide();
        return;
    }

    const qreal lineWidth = std::max(1.0, (edgeLength(viewQuad.at(0), viewQuad.at(1)) +
                                           edgeLength(viewQuad.at(3), viewQuad.at(2))) /
                                              2.0);
    const qreal lineHeight = std::max(1.0, (edgeLength(viewQuad.at(0), viewQuad.at(3)) +
                                            edgeLength(viewQuad.at(1), viewQuad.at(2))) /
                                               2.0);
    const qreal crossAxisSize =
        line.direction == ScreenshotOcrTextDirection::Vertical ? lineWidth : lineHeight;
    QFont font = QApplication::font();
    font.setPixelSize(std::max(kOcrTextLayoutMinPixelSize, qCeil(crossAxisSize * 2.0)));
    font.setHintingPreference(QFont::PreferNoHinting);
    const QPolygonF textFitQuad = ocrTextFitQuad(viewQuad, line.direction);
    const qreal targetWidth = (edgeLength(textFitQuad.at(0), textFitQuad.at(1)) +
                               edgeLength(textFitQuad.at(3), textFitQuad.at(2))) /
                              2.0;
    const qreal targetHeight = (edgeLength(textFitQuad.at(0), textFitQuad.at(3)) +
                                edgeLength(textFitQuad.at(1), textFitQuad.at(2))) /
                               2.0;
    const qreal targetAspectRatio = targetHeight > 0.0 ? targetWidth / targetHeight : 0.0;
    item.graphicsText->configure(text, font, readableForeground(*m_presentation, line),
                                  m_presentation->textSelectionForLine(item.lineIndex),
                                  line.direction, targetAspectRatio);

    const QRectF sourceBounds = item.graphicsText->boundingRect();
    QTransform transform;
    // Fit in the quad's local rectangle with one uniform scale, then project
    // that centered rectangle back into the rotated or perspective quad.
    if (!aspectFitQuadTransform(textFitQuad, sourceBounds.width(), sourceBounds.height(),
                                &transform)) {
        item.graphicsText->hide();
        return;
    }
    item.graphicsText->setPos(0.0, 0.0);
    item.graphicsText->setTransform(transform);
    item.graphicsText->show();
}

ScreenshotCanvasRenderer::ScreenshotCanvasRenderer(SnowCanvasWidget& canvas)
    : m_canvas(canvas) {}

ScreenshotCanvasRenderer::~ScreenshotCanvasRenderer() {
    delete m_ocrTextLayer.data();
}

void ScreenshotCanvasRenderer::setRenderMode(RenderMode mode) {
    if (m_renderMode == mode) {
        return;
    }
    m_renderMode = mode;
    invalidateCachedContent();
    if (m_renderMode == RenderMode::ScrollingCapture && m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->hide();
    }
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setImage(QImage image, const QRectF& canvasRect) {
    setImageSource(ScreenshotImageSource::fromImage(std::move(image), canvasRect));
}

void ScreenshotCanvasRenderer::setImageSource(ScreenshotImageSource source) {
    if (source.isMaterialized()) {
        source.materializedImage.setDevicePixelRatio(1.0);
    }
    m_imageSource = std::move(source);
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setImageViewportPhysicalSize(const QSize& size) {
    const QSize normalized = size.isValid() && !size.isEmpty() ? size : QSize();
    if (m_imageViewportPhysicalSize == normalized) {
        return;
    }
    m_imageViewportPhysicalSize = normalized;
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setPinnedResultSurface(
    const QRectF& contentCanvasRect, const QRectF& surfaceCanvasRect,
    const ScreenshotResultStyle& style) {
    m_pinnedContentCanvasRect = contentCanvasRect.normalized();
    m_pinnedSurfaceCanvasRect = surfaceCanvasRect.normalized();
    m_pinnedResultStyle = ScreenshotResultCompositor::normalizedStyle(style);
    setRenderMode(RenderMode::PinnedResult);
}

void ScreenshotCanvasRenderer::clearImage() {
    if (!m_imageSource.isValid() && m_imageViewportPhysicalSize.isEmpty()) {
        return;
    }
    m_imageSource = {};
    m_imageViewportPhysicalSize = QSize();
    m_pinnedContentCanvasRect = {};
    m_pinnedSurfaceCanvasRect = {};
    m_pinnedResultStyle = {};
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setMaskVisible(bool visible) {
    if (m_maskVisible == visible) {
        return;
    }
    m_maskVisible = visible;
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setMaskColor(const QColor& color) {
    const QColor next = color.isValid() ? color : QColor(0, 0, 0, 128);
    if (m_maskColor == next) {
        return;
    }
    m_maskColor = next;
    if (m_maskVisible) {
        m_canvas.update();
    }
}

void ScreenshotCanvasRenderer::setGuideLines(const QPointF& cursorPosition,
                                             const QColor& cursorColor,
                                             const QColor& monitorCenterColor) {
    const QColor nextCursorColor = normalizedGuideLineColor(cursorColor);
    const QColor nextMonitorColor = normalizedGuideLineColor(monitorCenterColor);
    const QPoint nextCursorPosition =
        nextCursorColor.alpha() > 0 ? guideLinePixelPosition(cursorPosition) : QPoint();
    const bool nextVisible = nextCursorColor.alpha() > 0 || nextMonitorColor.alpha() > 0;
    if (m_guideLineCursorPosition == nextCursorPosition &&
        m_cursorGuideLineColor == nextCursorColor &&
        m_monitorCenterGuideLineColor == nextMonitorColor && m_guideLinesVisible == nextVisible) {
        return;
    }
    const QRegion dirtyRegion = planScreenshotGuideLineDamage(
        m_canvas.rect(), m_guideLineCursorPosition, m_cursorGuideLineColor,
        m_monitorCenterGuideLineColor, nextCursorPosition, nextCursorColor, nextMonitorColor);
    m_guideLineCursorPosition = nextCursorPosition;
    m_cursorGuideLineColor = nextCursorColor;
    m_monitorCenterGuideLineColor = nextMonitorColor;
    m_guideLinesVisible = nextVisible;
    if (!dirtyRegion.isEmpty()) {
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        g_guideLineDamageRegion += dirtyRegion;
        ++g_guideLineUpdateRequests;
#endif
        m_canvas.update(dirtyRegion);
    }
}

void ScreenshotCanvasRenderer::clearGuideLines() {
    setGuideLines({}, Qt::transparent, Qt::transparent);
}

void ScreenshotCanvasRenderer::setSelection(const QRectF& selection, bool handlesVisible,
                                            int cornerRadius, int shadowWidth,
                                            const QColor& shadowColor) {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.bounds = selection.normalized();
    next.present = next.bounds.isValid() && !next.bounds.isEmpty();
    next.handlesVisible = handlesVisible;
    next.cornerRadius = next.present ? std::max(0, cornerRadius) : 0;
    next.shadowWidth = next.present ? std::max(0, shadowWidth) : 0;
    next.shadowColor = shadowColor.isValid() ? shadowColor : QColor(0x33, 0x33, 0x33);
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::applySelectionState(const ScreenshotSelectionVisualState& state) {
    ScreenshotSelectionVisualState next = state;
    next.bounds = next.bounds.normalized();
    next.present = next.present && next.bounds.isValid() && !next.bounds.isEmpty();
    next.cornerRadius = next.present ? std::max(0, next.cornerRadius) : 0;
    next.shadowWidth = next.present ? std::max(0, next.shadowWidth) : 0;
    if (!next.shadowColor.isValid()) {
        next.shadowColor = QColor(0x33, 0x33, 0x33);
    }
    if (next == m_selectionState) {
        return;
    }
    const ScreenshotSelectionVisualState previous = m_selectionState;
    m_selectionState = next;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    const QTransform canvasToViewTransform = m_canvas.canvasToViewTransform();
    if (!canvasToViewTransform.isInvertible()) {
        ++g_selectionDamagePathFallbacks;
    }
#else
    const QTransform canvasToViewTransform = m_canvas.canvasToViewTransform();
#endif
    const QRegion dirtyRegion = planScreenshotSelectionDamage(
        previous, m_selectionState, m_canvas.rect(), canvasToViewTransform, m_maskVisible);
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    g_selectionDamageRegion += dirtyRegion;
#endif
    if (!dirtyRegion.isEmpty()) {
        m_canvas.update(dirtyRegion);
    }
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
ScreenshotSelectionRenderDiagnostics selectionRenderDiagnosticsForCurrentThread() {
    return ScreenshotSelectionRenderDiagnostics{
        regionPixelCount(g_selectionDamageRegion),
        g_selectionDamagePathFallbacks,
    };
}

void resetSelectionRenderDiagnosticsForCurrentThread() {
    g_selectionDamageRegion = {};
    g_selectionDamagePathFallbacks = 0;
}

ScreenshotGuideLineRenderDiagnostics guideLineRenderDiagnosticsForCurrentThread() {
    return ScreenshotGuideLineRenderDiagnostics{
        regionPixelCount(g_guideLineDamageRegion),
        g_guideLineUpdateRequests,
    };
}

void resetGuideLineRenderDiagnosticsForCurrentThread() {
    g_guideLineDamageRegion = {};
    g_guideLineUpdateRequests = 0;
}
#endif

void ScreenshotCanvasRenderer::setSelectionToolbarHovered(bool hovered) {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.toolbarHovered = hovered;
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::setSelectionBorderVisible(bool visible) {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.borderVisible = visible;
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::clearSelection() {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.bounds = QRectF();
    next.present = false;
    next.handlesVisible = true;
    next.cornerRadius = 0;
    next.shadowWidth = 0;
    next.shadowColor = QColor(0x33, 0x33, 0x33);
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::setOcrPresentation(
    std::shared_ptr<ScreenshotOcrPresentation> presentation, OcrPresentationMode mode) {
    m_ocrPresentation = std::move(presentation);
    m_ocrPresentationMode = mode;
    invalidateCachedContent();
    if (m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText) {
        ensureOcrTextLayer()->setPresentation(m_ocrPresentation);
    } else if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->clearPresentation();
    }
    m_canvas.update();
}

void ScreenshotCanvasRenderer::updateOcrSelection() {
    if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->updateSelection();
    }
}

ScreenshotOcrTextPosition ScreenshotCanvasRenderer::ocrTextPositionAt(const QPointF& canvasPosition,
                                                                      bool useClosestLine) const {
    if (m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText &&
        m_ocrTextLayer != nullptr) {
        return m_ocrTextLayer->textPositionAt(canvasPosition, useClosestLine);
    }
    return m_ocrPresentation != nullptr
               ? m_ocrPresentation->textPositionAt(canvasPosition, useClosestLine)
               : ScreenshotOcrTextPosition{};
}

void ScreenshotCanvasRenderer::clearOcrPresentation() {
    if (m_ocrPresentation == nullptr) {
        return;
    }
    m_ocrPresentation.reset();
    m_ocrPresentationMode = OcrPresentationMode::BackgroundAndText;
    invalidateCachedContent();
    if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->clearPresentation();
    }
    m_canvas.update();
}

void ScreenshotCanvasRenderer::reset() {
    const bool hadCachedContent = m_imageSource.isValid() ||
                                  !m_imageViewportPhysicalSize.isEmpty() ||
                                  m_renderMode != RenderMode::Standard ||
                                  m_ocrPresentation != nullptr;
    const bool hadState = m_imageSource.isValid() || !m_imageViewportPhysicalSize.isEmpty() ||
                          m_renderMode != RenderMode::Standard || m_maskVisible ||
                          m_selectionState.present || m_selectionState.shadowWidth > 0 ||
                          m_selectionState.toolbarHovered || !m_selectionState.borderVisible ||
                          m_ocrPresentation != nullptr || m_guideLinesVisible;
    m_imageSource = {};
    m_imageViewportPhysicalSize = QSize();
    m_pinnedContentCanvasRect = {};
    m_pinnedSurfaceCanvasRect = {};
    m_pinnedResultStyle = {};
    m_selectionState = ScreenshotSelectionVisualState{};
    m_renderMode = RenderMode::Standard;
    m_maskVisible = false;
    m_guideLineCursorPosition = {};
    m_cursorGuideLineColor = QColor(0, 0, 0, 0);
    m_monitorCenterGuideLineColor = QColor(0, 0, 0, 0);
    m_guideLinesVisible = false;
    m_ocrPresentation.reset();
    m_ocrPresentationMode = OcrPresentationMode::BackgroundAndText;
    if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->clearPresentation();
    }
    if (hadCachedContent) {
        invalidateCachedContent();
    }
    if (hadState) {
        m_canvas.update();
    }
}

std::uint64_t ScreenshotCanvasRenderer::contentRevision() const {
    return m_contentRevision;
}

ScreenshotCanvasRenderer::RenderMode ScreenshotCanvasRenderer::renderMode() const {
    return m_renderMode;
}

bool ScreenshotCanvasRenderer::maskVisible() const {
    return m_maskVisible;
}

QColor ScreenshotCanvasRenderer::maskColor() const {
    return m_maskColor;
}

bool ScreenshotCanvasRenderer::guideLinesVisible() const {
    return m_guideLinesVisible;
}

bool ScreenshotCanvasRenderer::hasSelection() const {
    return m_selectionState.present;
}

bool ScreenshotCanvasRenderer::selectionHandlesVisible() const {
    return m_selectionState.handlesVisible;
}

int ScreenshotCanvasRenderer::selectionCornerRadius() const {
    return m_selectionState.cornerRadius;
}

int ScreenshotCanvasRenderer::selectionShadowWidth() const {
    return m_selectionState.shadowWidth;
}

bool ScreenshotCanvasRenderer::selectionToolbarHovered() const {
    return m_selectionState.toolbarHovered;
}

bool ScreenshotCanvasRenderer::selectionBorderVisible() const {
    return m_selectionState.borderVisible;
}

QRectF ScreenshotCanvasRenderer::selection() const {
    return m_selectionState.bounds;
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
quint64 ScreenshotCanvasRenderer::ocrGeometrySynchronizationCountForTesting() const {
    return m_ocrTextLayer != nullptr ? m_ocrTextLayer->geometrySynchronizationCount() : 0;
}

#endif

void ScreenshotCanvasRenderer::invalidateCachedContent() {
    ++m_contentRevision;
}

ScreenshotOcrTextLayer* ScreenshotCanvasRenderer::ensureOcrTextLayer() {
    if (m_ocrTextLayer == nullptr) {
        m_ocrTextLayer = new ScreenshotOcrTextLayer(&m_canvas);
    }
    return m_ocrTextLayer;
}

void ScreenshotCanvasRenderer::renderBeforeCanvas(QPainter& painter,
                                                  const SnowCanvasRenderContext& context) {
    if (m_renderMode == RenderMode::ScrollingCapture ||
        m_renderMode == RenderMode::PinnedResult) {
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(context.viewportRect, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (m_renderMode == RenderMode::ScrollingCapture) {
            return;
        }
    }

    if (!m_imageSource.isValid()) {
        return;
    }

    const bool physicalViewport = m_imageViewportPhysicalSize.isValid() &&
                                  !m_imageViewportPhysicalSize.isEmpty() &&
                                  context.devicePixelRatio > 0.0 &&
                                  m_imageSource.isMaterialized();
    if (physicalViewport) {
        const QRectF targetRect(
            QPointF(context.viewportRect.topLeft()),
            QSizeF(m_imageViewportPhysicalSize.width() / context.devicePixelRatio,
                   m_imageViewportPhysicalSize.height() / context.devicePixelRatio));
        if (!context.exposedRegion.intersects(targetRect.toAlignedRect())) {
            return;
        }
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              m_imageSource.materializedImage.size() !=
                                  m_imageViewportPhysicalSize);
        paintExposedImageSlice(painter, targetRect, m_imageSource.materializedImage,
                               QRectF(m_imageSource.materializedImage.rect()),
                               context.exposedRegion);
        painter.restore();
    } else if (m_imageSource.isMaterialized()) {
        const QRectF targetRect =
            m_renderMode == RenderMode::PinnedResult
                ? context.canvasToViewTransform.mapRect(m_imageSource.materializedCanvasRect)
                : snappedOutwardToDevicePixels(
                      context.canvasToViewTransform.mapRect(
                          m_imageSource.materializedCanvasRect),
                      context.devicePixelRatio);
        if (context.exposedRegion.intersects(targetRect.toAlignedRect())) {
            paintExposedImageSlice(painter, targetRect, m_imageSource.materializedImage,
                                   QRectF(m_imageSource.materializedImage.rect()),
                                   context.exposedRegion);
        }
    } else {
        for (const ScreenshotImageLayer& layer : m_imageSource.layers) {
            const QRectF targetRect =
                context.canvasToViewTransform.mapRect(layer.destinationCanvasRect);
            if (context.exposedRegion.intersects(targetRect.toAlignedRect())) {
                paintImageLayer(painter, layer, context.canvasToViewTransform,
                                &context.exposedRegion);
            }
        }
    }
    if (m_ocrPresentation != nullptr && !m_ocrPresentation->filledImage.isNull()) {
        const QRectF ocrTarget = snappedOutwardToDevicePixels(
            context.canvasToViewTransform.mapRect(QRectF(m_ocrPresentation->selection)),
            context.devicePixelRatio);
        painter.drawImage(ocrTarget, m_ocrPresentation->filledImage);
    }
}

void ScreenshotCanvasRenderer::renderAfterCanvas(QPainter& painter,
                                                 const SnowCanvasRenderContext& context) {
    if (m_renderMode == RenderMode::PinnedResult) {
        const QRectF contentView =
            context.canvasToViewTransform.mapRect(m_pinnedContentCanvasRect);
        ScreenshotResultCompositor::finishLiveSurface(
            painter, QRectF(context.viewportRect), contentView, m_pinnedResultStyle,
            context.devicePixelRatio,
            std::hypot(context.canvasToViewTransform.m11(),
                       context.canvasToViewTransform.m12()));
        if (m_ocrPresentation != nullptr &&
            m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText &&
            m_ocrTextLayer != nullptr) {
            m_ocrTextLayer->synchronize(context.canvasToViewTransform, context.viewportRect);
        }
        return;
    }
    if (!m_maskVisible && !m_selectionState.present && m_ocrPresentation == nullptr &&
        !m_guideLinesVisible) {
        return;
    }

    painter.save();
    const int visibleCornerRadius =
        m_renderMode == RenderMode::Standard ? m_selectionState.cornerRadius : 0;
    const int selectionBorderCornerRadius = m_ocrPresentation == nullptr ? visibleCornerRadius : 0;
    if (visibleCornerRadius > 0) {
        painter.setRenderHint(QPainter::Antialiasing, true);
    }

    if (m_maskVisible) {
        QPainterPath dimPath;
        dimPath.setFillRule(Qt::OddEvenFill);
        dimPath.addRect(QRectF(context.viewportRect));
        if (m_selectionState.present) {
            dimPath.addPath(selectionShapePath(m_selectionState.bounds, visibleCornerRadius,
                                               context.canvasToViewTransform));
        }
        painter.fillPath(dimPath, m_maskColor);
    }
    if (m_renderMode == RenderMode::Standard && m_guideLinesVisible) {
        const QRectF viewport(context.viewportRect);
        paintScreenshotGuideLines(painter, viewport, QPointF(m_guideLineCursorPosition),
                                  m_cursorGuideLineColor, m_monitorCenterGuideLineColor,
                                  &context.exposedRegion);
    }
    if (m_renderMode == RenderMode::Standard && m_selectionState.present) {
        const QRectF selectionView = context.canvasToViewTransform.mapRect(m_selectionState.bounds);
        if (m_selectionState.toolbarHovered) {
            renderSelectionShadow(painter, context, m_selectionState.bounds, visibleCornerRadius,
                                  m_selectionState.shadowWidth, m_selectionState.shadowColor);
        } else if (m_selectionState.borderVisible) {
            painter.setPen(QPen(selectionAccentColor(), kSelectionBorderWidth));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(selectionShapePath(m_selectionState.bounds,
                                                selectionBorderCornerRadius,
                                                context.canvasToViewTransform, 0.5));
        }

        const double minSide = std::min(selectionView.width(), selectionView.height());
        std::array<QPointF, 8> handles{};
        std::size_t handleCount = 0;
        if (!m_selectionState.toolbarHovered && m_selectionState.handlesVisible &&
            minSide > kShowEndHandlesMinSize) {
            handles[handleCount++] = selectionView.topLeft();
            handles[handleCount++] = selectionView.topRight();
            handles[handleCount++] = selectionView.bottomRight();
            handles[handleCount++] = selectionView.bottomLeft();
        }
        if (!m_selectionState.toolbarHovered && m_selectionState.handlesVisible &&
            minSide > kShowMidHandlesMinSize) {
            handles[handleCount++] = QPointF(selectionView.center().x(), selectionView.top());
            handles[handleCount++] = QPointF(selectionView.right(), selectionView.center().y());
            handles[handleCount++] = QPointF(selectionView.center().x(), selectionView.bottom());
            handles[handleCount++] = QPointF(selectionView.left(), selectionView.center().y());
        }
        if (handleCount != 0) {
            painter.setBrush(selectionAccentColor());
            painter.setPen(QPen(Qt::white, kSelectionHandleStrokeWidth));
            for (std::size_t index = 0; index < handleCount; ++index) {
                painter.drawEllipse(handles[index], kSelectionHandleRadius, kSelectionHandleRadius);
            }
        }
    }
    if (m_renderMode == RenderMode::Standard && m_ocrPresentation != nullptr &&
        m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText) {
        if (m_ocrTextLayer != nullptr) {
            m_ocrTextLayer->synchronize(context.canvasToViewTransform, context.viewportRect);
        }
    }
    painter.restore();
}
