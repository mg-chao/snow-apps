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
    std::printf("width,height,lines,scenario,accepted,estimate_median_ms,estimate_p95_ms,"
                "render_median_ms,render_p95_ms\n");
    for (const QSize size : {QSize(1920, 1080), QSize(3840, 2160)}) {
        for (const int count : {20, 500}) {
            for (int scenario = 0; scenario < 4; ++scenario) {
                QImage source(size, QImage::Format_ARGB32_Premultiplied);
                source.fill(QColor(224, 232, 240));
                if (scenario == 1) {
                    for (int y = 0; y < size.height(); ++y) {
                        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
                        for (int x = 0; x < size.width(); ++x) {
                            const int noise = ((x * 17 + y * 29 + x * y) % 7) - 3;
                            row[x] = qRgb(224 + noise, 232 + noise, 240 + noise);
                        }
                    }
                }
                ScreenshotOcrPresentation presentation;
                const qreal cellWidth = size.width() / 5.0;
                const int rows = (count + 4) / 5;
                const qreal cellHeight = size.height() / static_cast<qreal>(rows);
                QPainter painter(&source);
                for (int i = 0; i < count; ++i) {
                    const qreal x = (i % 5) * cellWidth + 10;
                    const int row = i / 5;
                    const qreal y = row * cellHeight + 3;
                    const qreal w = cellWidth - 20;
                    const qreal h = std::min(24.0, cellHeight - 6);
                    ScreenshotOcrLine line;
                    line.quad = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
                    if (scenario == 3) {
                        // Mild skew keeps dense lines separate while increasing exclusion-region
                        // fragmentation. This exercises the cost hidden by axis-aligned pages.
                        line.quad[1].ry() += h * 0.4;
                        line.quad[2].ry() += h * 0.4;
                    }
                    presentation.lines.push_back(line);
                    if (scenario == 2 && i % 5 == 0) {
                        painter.fillRect(QRectF(x + w / 2, y - 3, w / 2 + 5, h + 6),
                                         QColor(32, 40, 48));
                    }
                    const int strokes = static_cast<int>((w - 8) / 12);
                    for (int stroke = 0; stroke < strokes; ++stroke) {
                        const qreal textX = x + 4 + stroke * 12;
                        const qreal textY = y + (scenario == 3 ? h * 0.4 * (textX - x) / w : 0);
                        painter.fillRect(QRectF(textX, textY + 1, 2, std::max(1.0, h - 2)),
                                         Qt::black);
                    }
                }
                painter.end();
                std::vector<double> estimates, renders;
                SnowCanvasRegionFilterScratch scratch;
                for (int run = 0; run < 23; ++run) {
                    QElapsedTimer timer;
                    timer.start();
                    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
                    const double estimateMs = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
                    timer.restart();
                    const QImage result = renderScreenshotOcrFilteredImage(
                        source, source.rect(), presentation, Qt::white, 1.0, nullptr, &scratch);
                    const double renderMs = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
                    if (result.isNull()) {
                        return 1;
                    }
                    if (run >= 3) {
                        estimates.push_back(estimateMs);
                        renders.push_back(renderMs);
                    }
                }
                const auto accepted =
                    std::count_if(presentation.lines.cbegin(), presentation.lines.cend(),
                                  [](const ScreenshotOcrLine& line) {
                                      return line.backgroundFillColor.isValid();
                                  });
                const int expectedAccepted = count;
                if (accepted != expectedAccepted) {
                    std::fprintf(stderr, "Unexpected acceptance count: %d/%d in scenario %d\n",
                                 static_cast<int>(accepted), count, scenario);
                    return 2;
                }
                std::sort(estimates.begin(), estimates.end());
                std::sort(renders.begin(), renders.end());
                const char* name = scenario == 0   ? "flat"
                                   : scenario == 1 ? "noise"
                                   : scenario == 2 ? "mixed"
                                                   : "rotated";
                std::printf("%d,%d,%d,%s,%d,%.6f,%.6f,%.6f,%.6f\n", size.width(), size.height(),
                            count, name, static_cast<int>(accepted),
                            (estimates[9] + estimates[10]) / 2.0, estimates[18],
                            (renders[9] + renders[10]) / 2.0, renders[18]);
                std::fflush(stdout);
            }
        }
    }
    return 0;
}
