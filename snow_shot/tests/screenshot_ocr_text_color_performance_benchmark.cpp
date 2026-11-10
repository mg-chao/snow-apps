#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPainter>

#include <algorithm>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    std::puts("width,height,lines,scenario,accepted,cold_median_ms,cold_p95_ms,cached_median_ms,"
              "cached_p95_ms");
    for (const QSize size : {QSize(1920, 1080), QSize(3840, 2160)}) {
        for (const int count : {100, 500, 1000}) {
            for (const bool rotated : {false, true}) {
                QImage image(size, QImage::Format_RGB32);
                image.fill(Qt::white);
                ScreenshotOcrPresentation base;
                QPainter painter(&image);
                const qreal cellWidth = size.width() / 5.0;
                const qreal cellHeight = size.height() / static_cast<qreal>((count + 4) / 5);
                for (int i = 0; i < count; ++i) {
                    const qreal width = cellWidth - 12;
                    const qreal height = std::min(24.0, cellHeight - 2);
                    QTransform transform;
                    transform.translate((i % 5) * cellWidth + 5, (i / 5) * cellHeight + 1);
                    if (rotated)
                        transform.shear(0, height * 0.15 / width);
                    ScreenshotOcrLine line;
                    line.quad =
                        transform.map(QPolygonF{{0, 0}, {width, 0}, {width, height}, {0, height}});
                    base.lines.push_back(line);
                    painter.save();
                    painter.setTransform(transform);
                    for (int x = 3; x < static_cast<int>(width) - 4; x += 9)
                        painter.fillRect(QRectF(x, 1, 2, height - 2), QColor(26, 13, 171));
                    painter.restore();
                }
                painter.end();
                std::vector<double> cold, cached;
                qsizetype accepted = 0;
                for (int run = 0; run < 23; ++run) {
                    auto result = base;
                    QElapsedTimer timer;
                    timer.start();
                    prepareScreenshotOcrTextColors(result, image, image.rect());
                    const double coldMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
                    timer.restart();
                    prepareScreenshotOcrTextColors(result, image, image.rect());
                    const double cachedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
                    accepted = std::count_if(result.lines.begin(), result.lines.end(),
                                             [](const auto& line) {
                                                 return line.sourceTextColor == QColor(26, 13, 171);
                                             });
                    if (accepted != count) {
                        std::fprintf(stderr, "Unexpected color acceptance %lld/%d\n",
                                     static_cast<long long>(accepted), count);
                        return 1;
                    }
                    if (run >= 3) {
                        cold.push_back(coldMs);
                        cached.push_back(cachedMs);
                    }
                }
                std::sort(cold.begin(), cold.end());
                std::sort(cached.begin(), cached.end());
                std::printf("%d,%d,%d,%s,%lld,%.4f,%.4f,%.4f,%.4f\n", size.width(), size.height(),
                            count, rotated ? "skewed" : "flat", static_cast<long long>(accepted),
                            (cold[9] + cold[10]) / 2, cold[18], (cached[9] + cached[10]) / 2,
                            cached[18]);
                std::fflush(stdout);
            }
        }
    }
}
