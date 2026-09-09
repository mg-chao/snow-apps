#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include <QGuiApplication>
#include <QFontDatabase>
#include <QElapsedTimer>
#include <algorithm>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
#if defined(Q_OS_WIN)
    if (QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) < 0 ||
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) < 0)
        return 3;
    QGuiApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif

    std::cout
        << "width,height,lines,snapshot_median_us,render_median_ms,render_p95_ms,image_bytes\n";
    for (const QSize size : {QSize(1920, 1080), QSize(3840, 2160)}) {
        for (const int count : {20, 500}) {
            ScreenshotRecognitionImageSnapshot snapshot;
            snapshot.image = QImage(size, QImage::Format_ARGB32_Premultiplied);
            snapshot.image.fill(Qt::white);
            snapshot.canvasRect = QRectF(QPointF(), QSizeF(size));
            snapshot.font = QGuiApplication::font();
            snapshot.textColor = Qt::black;
            for (int i = 0; i < count; ++i) {
                const qreal x = (i % 5) * size.width() / 5.0;
                const qreal y = (i / 5) * size.height() / 100.0;
                ScreenshotOcrLine line;
                line.text = QStringLiteral("Recognized translation %1").arg(i);
                line.quad = {{x, y},
                             {x + size.width() / 5.0, y},
                             {x + size.width() / 5.0, y + 20},
                             {x, y + 20}};
                snapshot.lines.push_back(line);
            }
            std::vector<double> copies, renders;
            for (int run = 0; run < 22; ++run) {
                QElapsedTimer timer;
                timer.start();
                auto frozen = snapshot;
                const double copyUs = timer.nsecsElapsed() / 1000.0;
                if (frozen.image.constBits() != snapshot.image.constBits() ||
                    frozen.lines.constData() != snapshot.lines.constData())
                    return 2;
                timer.restart();
                const QImage result = renderScreenshotRecognitionImage(frozen);
                const double renderMs = timer.nsecsElapsed() / 1000000.0;
                if (result.isNull())
                    return 1;
                if (run >= 2) {
                    copies.push_back(copyUs);
                    renders.push_back(renderMs);
                }
            }
            std::sort(copies.begin(), copies.end());
            std::sort(renders.begin(), renders.end());
            std::cout << size.width() << ',' << size.height() << ',' << count << ',' << copies[10]
                      << ',' << renders[10] << ',' << renders[18] << ','
                      << snapshot.image.sizeInBytes() << '\n';
        }
    }
    return 0;
}
