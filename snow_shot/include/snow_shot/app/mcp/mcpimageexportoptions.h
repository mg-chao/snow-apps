#pragma once

#include "snow_shot/presentation/screenshotimagefileservice.h"
#include <QJsonObject>

namespace snow_shot::app::mcp {
inline bool imageExportOptions(const QJsonObject& params, ScreenshotImageEncodingOptions& encoding,
                               ScreenshotPdfOptions& pdf) {
    const auto quality = params.value(QStringLiteral("quality"));
    encoding.quality = quality.toInt(100);
    if ((!quality.isUndefined() &&
         (!quality.isDouble() || quality.toDouble() != encoding.quality)) ||
        encoding.quality < 1 || encoding.quality > 100)
        return false;
    const auto compression =
        params.value(QStringLiteral("compression_level")).toString(QStringLiteral("medium"));
    const auto page =
        params.value(QStringLiteral("pdf_page_size")).toString(QStringLiteral("a4_portrait"));
    if (!QStringList{QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}
             .contains(compression) ||
        !QStringList{QStringLiteral("image_size"), QStringLiteral("a4_portrait"),
                     QStringLiteral("a4_landscape")}
             .contains(page))
        return false;
    encoding.compressionLevel = ScreenshotImageFileService::compressionLevelForKey(compression);
    pdf.pageSize = screenshot_pdf::pageSizeForKey(page);
    pdf.quality = encoding.quality;
    pdf.title = params.value(QStringLiteral("pdf_title")).toString();
    return pdf.title.size() <= 1024;
}
} // namespace snow_shot::app::mcp
