#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYOUT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYOUT_H

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshottextannotationgeometry.h"
#include <QFont>
#include <QTextLayout>
#include <memory>
#include <vector>

class QPainter;

// Owns text-shaping data on the calling thread; independent of widgets and theme state.
class ScreenshotOcrTextLayout final {
  public:
    enum class PaintMode { TextAndSelection, SelectionOnly, TextOnly };

    struct StreamingLayout {
        qreal width;
        std::optional<ScreenshotOcrSourceTextMetrics> source;
        bool operator==(const StreamingLayout&) const = default;
    };

    void configure(const QString& text, const QFont& font, const QColor& textColor,
                   const ScreenshotOcrTextRange& selection, ScreenshotOcrTextDirection direction,
                   qreal targetAspectRatio, bool paragraph, const QVector<QRectF>& sourceRows = {},
                   std::optional<qreal> fontSizeCapRatio = {},
                   std::optional<StreamingLayout> streaming = {});
    void setSelection(const ScreenshotOcrTextRange& selection);
    [[nodiscard]] int cursorPositionAt(const QPointF& itemPosition) const;
    [[nodiscard]] QVector<ScreenshotTextAnnotationFragment>
    rangeFragments(const ScreenshotOcrTextRange& range) const;

    [[nodiscard]] bool usesSourceRows() const {
        return !m_fittedRows.empty();
    }

    [[nodiscard]] bool glyphRasterCacheable() const {
        return m_glyphRasterCacheable;
    }

    [[nodiscard]] QRectF boundingRect() const;
    // Font size in layout coordinates, including the internal source-row transform.
    [[nodiscard]] qreal fontPixelSize() const {
        return m_font.pixelSize() * m_sourceRowScale;
    }
    [[nodiscard]] ScreenshotOcrSourceTextMetrics sourceTextMetrics() const;
    [[nodiscard]] QRectF glyphRasterBounds() const {
        return m_glyphRasterBounds;
    }
    void paint(QPainter* painter, const QColor& selectionBackground = {},
               const QColor& selectionForeground = {},
               PaintMode mode = PaintMode::TextAndSelection) const;

  private:
    struct FittedRow {
        int textStart = 0;
        int textLength = 0;
        std::unique_ptr<QTextLayout> layout;
        QTransform transform;
        QRectF bounds;
    };
    bool fitSourceRows(qreal aspectRatio);
    void updateGlyphRasterCacheability();

    struct VerticalGlyph {
        int textStart = 0;
        int textLength = 0;
        bool rotated = false;
        std::unique_ptr<QTextLayout> layout;
        QRectF inkBounds;
    };

    QString m_text;
    QFont m_font;
    QColor m_textColor;
    ScreenshotOcrTextRange m_selection;
    ScreenshotOcrTextDirection m_direction = ScreenshotOcrTextDirection::Horizontal;
    std::unique_ptr<QTextLayout> m_layout;
    QTextLine m_line;
    std::vector<VerticalGlyph> m_verticalGlyphs;
    std::vector<FittedRow> m_fittedRows;
    QVector<QRectF> m_sourceRows;
    QVector<int> m_graphemeBoundaries;
    qreal m_verticalCellAdvance = 0.0;
    qreal m_targetAspectRatio = 0.0;
    qreal m_sourceRowScale = 1.0;
    std::optional<qreal> m_fontSizeCapRatio;
    std::optional<StreamingLayout> m_streaming;
    bool m_paragraph = false;
    bool m_glyphRasterCacheable = true;
    QPointF m_layoutOrigin;
    QRectF m_bounds;
    QRectF m_glyphRasterBounds;
};

[[nodiscard]] bool
configureScreenshotOcrTextLayout(ScreenshotOcrTextLayout& layout, const ScreenshotOcrLine& line,
                                 const QTransform& canvasToOutput, QFont font,
                                 const QColor& textColor, const ScreenshotOcrTextRange& selection,
                                 QTransform* textToOutput, qreal* fittedFontPixelSize = nullptr);

// Capture source font sizes and spacing once for translation fitting and streaming.
void setScreenshotOcrTranslationFontCaps(QVector<ScreenshotOcrLine>& translation,
                                         const QVector<ScreenshotOcrLine>& source,
                                         const QVector<QVector<int>>& sourceIndices,
                                         const QFont& font);

#endif
