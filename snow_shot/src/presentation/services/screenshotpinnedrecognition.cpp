// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotpinnedrecognition.h"
#include "snow_shot/presentation/screenshotimageconversionpersistence.h"
#include "snow_shot/presentation/screenshotrecognitionmodelpersistence.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrlayout.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include "snow_shot/presentation/screenshotocrtexttransform.h"
#include <QDataStream>
#include <QIODevice>

namespace {
constexpr quint32 kTextTranslationPayloadMarker = 0x53535452;
constexpr quint8 kTextTranslationPayloadVersion = 2;
QTransform imagePixelsToCanvasTransform(const QTransform& imageTransform,
                                        const QSize& transformedPixels, const QRectF& contentRect) {
    QTransform pixelsToCanvas;
    pixelsToCanvas.translate(contentRect.left(), contentRect.top());
    pixelsToCanvas.scale(
        transformedPixels.width() > 0 ? contentRect.width() / transformedPixels.width() : 1,
        transformedPixels.height() > 0 ? contentRect.height() / transformedPixels.height() : 1);
    return imageTransform * pixelsToCanvas;
}

} // namespace

std::shared_ptr<ScreenshotOcrPresentation> transformScreenshotPinnedOcrPresentation(
    const ScreenshotOcrPresentation& source, const QRectF& sourceRect, const QSize& sourcePixels,
    const QTransform& imageTransform, const QSize& transformedPixels, const QRectF& contentRect) {
    auto result = std::make_shared<ScreenshotOcrPresentation>();
    result->selection = contentRect.toAlignedRect();
    result->solidBackgroundFill = source.solidBackgroundFill;
    result->lines = source.lines;
    const qreal sourceScaleX =
        sourceRect.width() > 0 ? sourcePixels.width() / sourceRect.width() : 1;
    const qreal sourceScaleY =
        sourceRect.height() > 0 ? sourcePixels.height() / sourceRect.height() : 1;
    const QTransform pixelsToCanvas =
        imagePixelsToCanvasTransform(imageTransform, transformedPixels, contentRect);
    const auto mapPoint = [&](const QPointF& point) {
        const QPointF pixel((point.x() - sourceRect.left()) * sourceScaleX,
                            (point.y() - sourceRect.top()) * sourceScaleY);
        return pixelsToCanvas.map(pixel);
    };
    for (auto& line : result->lines) {
        for (auto& point : line.quad)
            point = mapPoint(point);
        for (auto& quad : line.sourceLineQuads)
            for (auto& point : quad)
                point = mapPoint(point);
    }
    result->prepareForRendering();
    return result;
}

ScreenshotRecognitionResults decodeScreenshotPinnedRecognition(const QByteArray& bytes,
                                                               const QFont& font) {
    Q_UNUSED(font);
    ScreenshotRecognitionResults results;
    if (bytes.isEmpty()) {
        return results;
    }
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_11);
    quint8 hasText = 0, hasTable = 0, hasQr = 0;
    stream >> results.key >> hasText >> hasTable >> hasQr;
    if (stream.status() != QDataStream::Ok) {
        return {};
    }
    if (hasText) {
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        qint64 lineCount = 0;
        QString textError;
        stream >> textError >> presentation->selection >> lineCount;
        if (lineCount < 0 || lineCount > 10000) {
            return {};
        }
        for (int index = 0; index < lineCount; ++index) {
            ScreenshotOcrLine line;
            quint8 direction = 0;
            stream >> line.text >> line.confidence >> line.quad >> direction;
            line.direction = direction != 0 ? ScreenshotOcrTextDirection::Vertical
                                            : ScreenshotOcrTextDirection::Horizontal;
            presentation->lines.push_back(std::move(line));
        }
        presentation->prepareForRendering();
        ScreenshotOcrRecognitionResult text;
        text.presentation = std::move(presentation);
        text.error = std::move(textError);
        results.text = std::move(text);
    }
    if (hasTable) {
        SnowShotTableResult table;
        stream >> table.html >> table.error >> table.code >> table.httpStatus;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        results.table = std::move(table);
#endif
    }
    if (hasQr) {
        ScreenshotQrRecognitionResult qr;
        stream >> qr.contents >> qr.error;
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
        results.qr = std::move(qr);
#endif
    }
    while (stream.status() == QDataStream::Ok && !stream.atEnd()) {
        quint32 marker = 0;
        quint8 version = 0;
        stream >> marker >> version;
        if (marker == snow_shot::presentation::kRecognitionModelPayloadMarker) {
            quint32 size = 0;
            stream >> size;
            if (stream.status() != QDataStream::Ok ||
                size > snow_shot::presentation::kMaximumRecognitionModelPayload ||
                size > stream.device()->bytesAvailable()) {
                return results;
            }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            QByteArray payload(static_cast<qsizetype>(size), Qt::Uninitialized);
            if (stream.readRawData(payload.data(), static_cast<int>(size)) !=
                static_cast<int>(size)) {
                return results;
            }
            if (version == snow_shot::presentation::kRecognitionModelPayloadVersion) {
                snow_shot::presentation::decodeRecognitionModels(payload, results);
            }
#else
            if (stream.skipRawData(static_cast<int>(size)) != static_cast<int>(size)) {
                return results;
            }
#endif
            continue;
        }
        if (marker == quint32(0x4C415458) && (version == 1 || version == 2)) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            SnowShotLatexResult latex;
            stream >> latex.latex >> results.visibleLatex;
            if (version == 2) {
                bool hasDraft = false;
                stream >> hasDraft;
                if (hasDraft) {
                    QString draft;
                    stream >> draft;
                    results.latexDraft = std::move(draft);
                }
            }
            if (stream.status() == QDataStream::Ok && latex.succeeded())
                results.latex = std::move(latex);
#else
            QString ignoredLatex;
            bool ignoredVisible = false;
            stream >> ignoredLatex >> ignoredVisible;
            if (version == 2) {
                bool hasDraft = false;
                stream >> hasDraft;
                if (hasDraft)
                    stream >> ignoredLatex;
            }
#endif
            continue;
        }
        if (marker == snow_shot::presentation::kImageConversionPayloadMarker) {
            quint32 size = 0;
            stream >> size;
            if (stream.status() != QDataStream::Ok ||
                size > snow_shot::presentation::kMaximumImageConversionPayload ||
                size > stream.device()->bytesAvailable()) {
                return results;
            }
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
            QByteArray payload(static_cast<qsizetype>(size), Qt::Uninitialized);
            if (stream.readRawData(payload.data(), static_cast<int>(size)) !=
                static_cast<int>(size)) {
                return results;
            }
            if (version == snow_shot::presentation::kImageConversionPayloadVersion) {
                snow_shot::presentation::decodeImageConversions(payload, results);
            }
#else
            if (stream.skipRawData(static_cast<int>(size)) != static_cast<int>(size)) {
                return results;
            }
#endif
            continue;
        }
        if (marker != kTextTranslationPayloadMarker ||
            (version != 1 && version != kTextTranslationPayloadVersion) ||
            !results.text.has_value() || !results.text->error.isEmpty() ||
            results.text->presentation == nullptr) {
            return {};
        }
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        results.translatedText = std::make_shared<ScreenshotOcrPresentation>();
        if (version == 1) {
            QStringList translatedLines;
            stream >> translatedLines;
            const auto& source = results.text->presentation;
            if (translatedLines.size() != source->lines.size()) {
                return {};
            }
            results.translatedText->selection = source->selection;
            results.translatedText->lines = source->lines;
            for (int index = 0; index < translatedLines.size(); ++index) {
                results.translatedText->lines[index].text = translatedLines[index];
            }
        } else {
            qint64 lineCount = 0;
            stream >> results.translatedText->selection >> lineCount;
            if (lineCount < 0 || lineCount > 10000) {
                return {};
            }
            for (int index = 0; index < lineCount; ++index) {
                ScreenshotOcrLine line;
                quint8 direction = 0;
                stream >> line.text >> line.confidence >> line.quad >> direction >>
                    line.paragraph >> line.sourceLineQuads;
                line.direction = direction != 0 ? ScreenshotOcrTextDirection::Vertical
                                                : ScreenshotOcrTextDirection::Horizontal;
                results.translatedText->lines.push_back(std::move(line));
            }
        }
        results.translatedText->prepareForRendering();
        if (const auto indices = snow_shot::presentation::ocrTranslationSourceIndices(
                *results.translatedText, *results.text->presentation)) {
            setScreenshotOcrTranslationFontCaps(results.translatedText->lines,
                                                results.text->presentation->lines, *indices, font);
        }
#else
        // Consume the legacy extension without constructing a translated presentation.
        if (version == 1) {
            QStringList ignoredLines;
            stream >> ignoredLines;
            if (ignoredLines.size() != results.text->presentation->lines.size()) {
                return {};
            }
        } else {
            QRect ignoredSelection;
            qint64 lineCount = 0;
            stream >> ignoredSelection >> lineCount;
            if (lineCount < 0 || lineCount > 10000) {
                return {};
            }
            for (int index = 0; index < lineCount; ++index) {
                ScreenshotOcrLine ignoredLine;
                quint8 ignoredDirection = 0;
                stream >> ignoredLine.text >> ignoredLine.confidence >> ignoredLine.quad >>
                    ignoredDirection >> ignoredLine.paragraph >> ignoredLine.sourceLineQuads;
            }
        }
#endif
    }
    if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
        return {};
    }
    snow_shot::presentation::promoteLegacyRecognitionModels(results);
    sanitizeEditionRecognitionResults(results);
    return results;
}

ScreenshotRecognitionResults
transformScreenshotPinnedRecognition(ScreenshotRecognitionResults results, const QRectF& sourceRect,
                                     const QSize& sourcePixels, const QTransform& imageTransform,
                                     const QSize& transformedPixels, const QRectF& contentRect) {
    sanitizeEditionRecognitionResults(results);
    if (results.text && results.text->presentation)
        results.text->presentation = transformScreenshotPinnedOcrPresentation(
            *results.text->presentation, sourceRect, sourcePixels, imageTransform,
            transformedPixels, contentRect);
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (results.translatedText)
        results.translatedText = transformScreenshotPinnedOcrPresentation(
            *results.translatedText, sourceRect, sourcePixels, imageTransform, transformedPixels,
            contentRect);
#endif
    return results;
}
