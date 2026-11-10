#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRPRESENTATION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRPRESENTATION_H

#include <QHash>
#include <QColor>
#include <QPolygonF>
#include <QRect>
#include <QString>
#include <QTransform>
#include <QVector>
#include <atomic>
#include <optional>

class QImage;

enum class ScreenshotOcrTextDirection {
    Horizontal,
    Vertical,
};

// Measurements of the rendered source row, in units of its fitted font size.
struct ScreenshotOcrSourceTextMetrics {
    qreal spacingEm = 0; // Extra letter spacing for a row, cell advance for a vertical column.
    qreal inkAscentEm = 0;
    qreal inkDescentEm = 0;
    bool operator==(const ScreenshotOcrSourceTextMetrics&) const = default;
};

struct ScreenshotOcrLine {
    QString text;
    qreal confidence = 0.0;
    QPolygonF quad;
    ScreenshotOcrTextDirection direction = ScreenshotOcrTextDirection::Horizontal;
    QVector<QPolygonF> sourceLineQuads{};
    bool paragraph = false;
    QColor backgroundFillColor{};
    // Derived translation-only limit: effective font size / quad cross-axis size.
    // Kept independent of translated text and reconstructed from source OCR on restore.
    std::optional<qreal> fontSizeCapRatio{};
    bool translationStreaming = false;
    std::optional<ScreenshotOcrSourceTextMetrics> sourceTextMetrics{};
    // Derived from original pixels. Invalid means insufficient evidence; never a render default.
    QColor sourceTextColor{};
    // Aligned with sourceLineQuads, including separate word boxes on the same visual row.
    QVector<QColor> sourceLineTextColors{};
};

struct ScreenshotOcrTextPosition {
    int lineIndex = -1;
    int characterIndex = 0;

    [[nodiscard]] bool valid() const {
        return lineIndex >= 0 && characterIndex >= 0;
    }
};

[[nodiscard]] QColor screenshotOcrContrastingTextColor(const QColor& background);

struct ScreenshotOcrTextRange {
    int start = 0;
    int length = 0;

    [[nodiscard]] bool empty() const {
        return length <= 0;
    }
};

enum class ScreenshotOcrLineSelectionPreview { None, Select, Deselect };

class ScreenshotOcrPresentation final {
  public:
    QRect selection;
    QVector<ScreenshotOcrLine> lines;
    // Explicit policy: missing color evidence must never select Blur in Background Fill mode.
    bool solidBackgroundFill = false;

    void prepareForRendering();
    void setLineText(int lineIndex, const QString& text, bool streaming = false);
    [[nodiscard]] bool empty() const;
    [[nodiscard]] int lineAt(const QPointF& canvasPosition) const;
    [[nodiscard]] ScreenshotOcrTextPosition textPositionAt(const QPointF& canvasPosition,
                                                           bool useClosestLine = false) const;
    [[nodiscard]] bool hasTextSelection() const;
    [[nodiscard]] bool textSelectionActive() const;
    [[nodiscard]] quint64 selectionRevision() const;
    [[nodiscard]] quint64 lineSelectionRevision(int lineIndex) const;
    // Null means the reader skipped a revision and must check individual line revisions.
    [[nodiscard]] const QVector<int>* selectionChangesSince(quint64 revision) const;
    [[nodiscard]] ScreenshotOcrTextPosition selectionAnchor() const;
    [[nodiscard]] ScreenshotOcrTextPosition selectionFocus() const;
    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] bool lineSelected(int lineIndex) const;
    [[nodiscard]] ScreenshotOcrTextRange textSelectionForLine(int lineIndex) const;
    [[nodiscard]] bool lineBoxSelectionActive() const;
    [[nodiscard]] QRectF lineBoxSelectionRect() const;
    [[nodiscard]] ScreenshotOcrLineSelectionPreview lineSelectionPreview(int lineIndex) const;

    void clearTextSelection();
    void selectAll();
    void beginTextSelection(const QPointF& canvasPosition);
    void beginTextSelection(const ScreenshotOcrTextPosition& position);
    void updateTextSelection(const QPointF& canvasPosition);
    void updateTextSelection(const ScreenshotOcrTextPosition& position);
    void finishTextSelection();
    void beginLineBoxSelection(const QPointF& origin);
    void updateLineBoxSelection(const QRectF& rectangle);
    void finishLineBoxSelection();
    void cancelLineBoxSelection();
    void copyCommittedSelectionFrom(const ScreenshotOcrPresentation& source);

  private:
    friend void prepareScreenshotOcrTextColors(ScreenshotOcrPresentation&, const QImage&,
                                               const QRectF&, const std::atomic_bool*);
    struct ColorGeometry {
        QPolygonF quad;
        QVector<QPolygonF> sourceLineQuads;
    };
    qint64 m_textColorImageKey = 0;
    QRectF m_textColorCanvasRect;
    QVector<ColorGeometry> m_textColorGeometry;

    struct CachedLineGeometry {
        QRectF bounds;
        QTransform canvasToNormalized;
        QVector<int> graphemeBoundaries;
        bool hasNormalizedTransform = false;
        bool hasPositiveQuadArea = false;
        bool axisAlignedQuad = false;
    };

    struct LineSelection {
        ScreenshotOcrTextRange range;
        bool fullLine = false;
    };

    [[nodiscard]] int closestLine(const QPointF& canvasPosition) const;
    [[nodiscard]] ScreenshotOcrTextPosition
    normalizedTextPosition(const ScreenshotOcrTextPosition& position) const;
    void beginSelectionChange();
    void markSelectionLineChanged(int lineIndex);
    void markCommittedSelectionChanged();
    void clearLineBoxPreview();
    void materializeSparseSelection();
    [[nodiscard]] bool lineFullySelected(int lineIndex) const;
    void findLinesIntersectingRectangle(const QRectF& rectangle);

    ScreenshotOcrTextPosition m_selectionAnchor;
    ScreenshotOcrTextPosition m_selectionFocus;
    QVector<CachedLineGeometry> m_cachedLineGeometry;
    QHash<quint64, QVector<int>> m_linesByCell;
    QVector<int> m_unindexedLines;
    QVector<int> m_wholeGeometryBoxCandidates;
    QHash<int, LineSelection> m_sparseSelection;
    QVector<quint8> m_boxDeselectAtPress;
    QVector<ScreenshotOcrLineSelectionPreview> m_boxPreviewActions;
    QVector<int> m_boxCandidates;
    QVector<int> m_boxCandidateScratch;
    QVector<quint64> m_lineSelectionRevisions;
    QVector<quint64> m_hitTestVisitRevisions;
    QVector<int> m_changedSelectionLines;
    QRectF m_lineBoxRectangle;
    QRectF m_lineGeometryBounds;
    quint64 m_selectionRevision = 0;
    quint64 m_previousSelectionRevision = 0;
    quint64 m_hitTestVisitRevision = 0;
    bool m_dragging = false;
    bool m_lineBoxDragging = false;
    bool m_sparseSelectionActive = false;
    bool m_allTextSelected = false;
    bool m_geometryPrepared = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRPRESENTATION_H
