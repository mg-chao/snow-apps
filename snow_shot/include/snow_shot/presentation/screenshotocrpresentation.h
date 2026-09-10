#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRPRESENTATION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRPRESENTATION_H

#include <QHash>
#include <QColor>
#include <QPolygonF>
#include <QRect>
#include <QString>
#include <QTransform>
#include <QVector>

enum class ScreenshotOcrTextDirection {
    Horizontal,
    Vertical,
};

struct ScreenshotOcrLine {
    QString text;
    qreal confidence = 0.0;
    QPolygonF quad;
    ScreenshotOcrTextDirection direction = ScreenshotOcrTextDirection::Horizontal;
    QVector<QPolygonF> sourceLineQuads;
    bool paragraph = false;
    QColor backgroundFillColor;
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

class ScreenshotOcrPresentation final {
  public:
    QRect selection;
    QVector<ScreenshotOcrLine> lines;
    // Explicit policy: missing color evidence must never select Blur in Background Fill mode.
    bool solidBackgroundFill = false;

    void prepareForRendering();
    void setLineText(int lineIndex, const QString& text);
    [[nodiscard]] bool empty() const;
    [[nodiscard]] int lineAt(const QPointF& canvasPosition) const;
    [[nodiscard]] ScreenshotOcrTextPosition textPositionAt(const QPointF& canvasPosition,
                                                           bool useClosestLine = false) const;
    [[nodiscard]] bool hasTextSelection() const;
    [[nodiscard]] bool textSelectionActive() const;
    [[nodiscard]] quint64 selectionRevision() const;
    [[nodiscard]] ScreenshotOcrTextPosition selectionAnchor() const;
    [[nodiscard]] ScreenshotOcrTextPosition selectionFocus() const;
    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] bool lineSelected(int lineIndex) const;
    [[nodiscard]] ScreenshotOcrTextRange textSelectionForLine(int lineIndex) const;

    void clearTextSelection();
    void selectAll();
    void beginTextSelection(const QPointF& canvasPosition);
    void beginTextSelection(const ScreenshotOcrTextPosition& position);
    void updateTextSelection(const QPointF& canvasPosition);
    void updateTextSelection(const ScreenshotOcrTextPosition& position);
    void finishTextSelection();

  private:
    struct CachedLineGeometry {
        QRectF bounds;
        QTransform canvasToNormalized;
        QVector<int> graphemeBoundaries;
        bool hasNormalizedTransform = false;
    };

    [[nodiscard]] int closestLine(const QPointF& canvasPosition) const;
    [[nodiscard]] ScreenshotOcrTextPosition
    normalizedTextPosition(const ScreenshotOcrTextPosition& position) const;

    ScreenshotOcrTextPosition m_selectionAnchor;
    ScreenshotOcrTextPosition m_selectionFocus;
    QVector<CachedLineGeometry> m_cachedLineGeometry;
    QHash<quint64, QVector<int>> m_linesByCell;
    QVector<int> m_unindexedLines;
    quint64 m_selectionRevision = 0;
    bool m_dragging = false;
    bool m_geometryPrepared = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRPRESENTATION_H
