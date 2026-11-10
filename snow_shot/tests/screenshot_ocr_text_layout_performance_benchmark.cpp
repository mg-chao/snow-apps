#include "snow_shot/presentation/screenshotocrtextlayout.h"

#include <QElapsedTimer>
#include <QGuiApplication>

#include <algorithm>
#include <array>
#include <cstdio>

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    const QFont font = QGuiApplication::font();
    for (bool paragraph : {false, true}) {
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Recognized text with natural spacing and descenders Agyp");
        line.quad = {{0, 0}, {800, 0}, {800, 30}, {0, 30}};
        if (paragraph) {
            line.text = line.text.repeated(3);
            line.paragraph = true;
            line.quad = {{0, 0}, {800, 0}, {800, 120}, {0, 120}};
            for (int row = 0; row < 3; ++row) {
                const qreal y = row * 45;
                line.sourceLineQuads.push_back({{0, y}, {800, y}, {800, y + 30}, {0, y + 30}});
            }
        }
        std::array<double, 3> milliseconds;
        qreal checksum = 0;
        for (int run = -1; run < 3; ++run) {
            QElapsedTimer timer;
            timer.start();
            for (int index = 0; index < 500; ++index) {
                ScreenshotOcrTextLayout layout;
                QTransform transform;
                qreal size = 0;
                if (!configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {},
                                                      &transform, &size))
                    return 1;
                checksum += size;
            }
            if (run >= 0)
                milliseconds[static_cast<std::size_t>(run)] =
                    static_cast<double>(timer.nsecsElapsed()) / 1e6;
        }
        std::sort(milliseconds.begin(), milliseconds.end());
        std::printf("%s,500_layouts,median_ms=%.3f,min_ms=%.3f,max_ms=%.3f,checksum=%.3f\n",
                    paragraph ? "source_rows" : "single_line", milliseconds[1], milliseconds[0],
                    milliseconds[2], checksum);
    }
}
