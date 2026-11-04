#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTEXTANNOTATIONGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTEXTANNOTATIONGEOMETRY_H

#include "snow_shot/presentation/screenshottextannotation.h"
#include <QFont>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QVector>

class QTextDocument;
class QTextLayout;
class ScreenshotOcrPresentation;

[[nodiscard]] QByteArray
screenshotOcrTextAnnotationFingerprint(const ScreenshotOcrPresentation& presentation);
[[nodiscard]] QByteArray screenshotFormattedTextAnnotationFingerprint(QTextDocument& document);

// Local x follows the text advance, and local y follows its cross axis. Geometry is
// independent of the camera and the selection overlay, including for vertical text.
struct ScreenshotTextAnnotationFragment {
    ScreenshotTextSelectionRange range;
    QRectF rect;
    QTransform localToSource;
    qreal baseline = 0.0;
};

// Shared shaped-text extraction. Ranges are relative to layout.text(); returned range
// offsets include textOffset. Disjoint bidi fragments remain separate within each row.
[[nodiscard]] QVector<ScreenshotTextAnnotationFragment>
screenshotTextLayoutAnnotationFragments(const QTextLayout& layout, int start, int end,
                                        int textOffset = 0);
// QTextBlock-backed layouts do not expose their document text via QTextLayout::text().
[[nodiscard]] QVector<ScreenshotTextAnnotationFragment>
screenshotTextLayoutAnnotationFragments(const QTextLayout& layout, const QString& text, int start,
                                        int end, int textOffset = 0);

// OCR output uses the presentation's coordinates; formatted output uses document units.
// The caller applies source-to-image scaling (including a formatted pin's source DPR).
[[nodiscard]] QVector<ScreenshotTextAnnotationFragment>
screenshotOcrTextAnnotationFragments(const ScreenshotOcrPresentation& presentation,
                                     const QVector<ScreenshotTextSelectionRange>& ranges,
                                     const QFont& font = {});
[[nodiscard]] QVector<ScreenshotTextAnnotationFragment>
screenshotFormattedTextAnnotationFragments(QTextDocument& document,
                                           const QVector<ScreenshotTextSelectionRange>& ranges);
// Selection UI also needs an anchor for paragraph separators and empty selected blocks.
// Their caret-width bounds are not persistent annotation ink.
[[nodiscard]] QRectF
screenshotFormattedTextSelectionBounds(QTextDocument& document,
                                       const QVector<ScreenshotTextSelectionRange>& ranges,
                                       const QTransform& documentToOutput = {});

[[nodiscard]] QRectF
screenshotTextAnnotationBounds(const QVector<ScreenshotTextAnnotationFragment>& fragments,
                               const QTransform& sourceToOutput = {});

#endif
