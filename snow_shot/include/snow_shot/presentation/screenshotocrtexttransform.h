#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTTRANSFORM_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTTRANSFORM_H

#include <QString>

class ScreenshotOcrPresentation;

namespace snow_shot::presentation {

QString originalOcrText(const ScreenshotOcrPresentation& presentation);
QString removeOcrLineBreaks(const QString& text);
QString convertOcrPunctuation(const QString& text, bool fullWidth);
QString applyOcrTextTransforms(const QString& text, const QString& formatting,
                               const QString& punctuation);

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTTRANSFORM_H
