#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEROWSOURCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEROWSOURCE_H

#include <QSize>
#include <QtGlobal>

#include <functional>

struct ScreenshotImageRowSource final {
    QSize size;
    std::function<bool(int firstRow, int rowCount, qsizetype destinationStride, uchar* destination,
                       qsizetype destinationSize)>
        readRows;
    std::function<bool()> cancellationRequested;

    [[nodiscard]] bool isValid() const {
        return size.isValid() && !size.isEmpty() && static_cast<bool>(readRows);
    }
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEROWSOURCE_H
