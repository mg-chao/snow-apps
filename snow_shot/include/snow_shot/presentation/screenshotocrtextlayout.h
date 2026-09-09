#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYOUT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYOUT_H

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include <QFont>
#include <QTextLayout>
#include <memory>
#include <vector>

class QPainter;

// Owns text-shaping data on the calling thread; independent of widgets and theme state.
class ScreenshotOcrTextLayout final {
  public:
    void configure(const QString& text, const QFont& font, const QColor& textColor,
                   const ScreenshotOcrTextRange& selection, ScreenshotOcrTextDirection direction,
                   qreal targetAspectRatio, bool paragraph);
    void setSelection(const ScreenshotOcrTextRange& selection);
    [[nodiscard]] int cursorPositionAt(const QPointF& itemPosition) const;

    [[nodiscard]] QRectF boundingRect() const;
    void paint(QPainter* painter, const QColor& selectionBackground = {},
               const QColor& selectionForeground = {}) const;

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
    QColor m_textColor;
    ScreenshotOcrTextRange m_selection;
    ScreenshotOcrTextDirection m_direction = ScreenshotOcrTextDirection::Horizontal;
    std::unique_ptr<QTextLayout> m_layout;
    QTextLine m_line;
    std::vector<VerticalGlyph> m_verticalGlyphs;
    QVector<int> m_graphemeBoundaries;
    qreal m_verticalCellAdvance = 0.0;
    qreal m_targetAspectRatio = 0.0;
    bool m_paragraph = false;
    QPointF m_layoutOrigin;
    QRectF m_bounds;
};

[[nodiscard]] bool configureScreenshotOcrTextLayout(ScreenshotOcrTextLayout& layout,
                                                    const ScreenshotOcrLine& line,
                                                    const QTransform& canvasToOutput, QFont font,
                                                    const QColor& textColor,
                                                    const ScreenshotOcrTextRange& selection,
                                                    QTransform* textToOutput);

#endif
