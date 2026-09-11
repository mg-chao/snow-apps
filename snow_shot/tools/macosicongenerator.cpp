#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <array>
#include <iostream>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc != 3) {
        std::cerr << "Usage: macos-icon-generator source.svg output.iconset\n";
        return 1;
    }
    QSvgRenderer renderer(QString::fromLocal8Bit(argv[1]));
    const QDir output(QString::fromLocal8Bit(argv[2]));
    if (!renderer.isValid() || !QDir().mkpath(output.absolutePath())) {
        return 1;
    }
    for (int size : std::array{16, 32, 128, 256, 512}) {
        for (int scale : std::array{1, 2}) {
            QImage image(size * scale, size * scale, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            renderer.render(&painter);
            painter.end();
            const QString name = QStringLiteral("icon_%1x%1%2.png")
                                     .arg(size)
                                     .arg(scale == 2 ? QStringLiteral("@2x") : QString());
            if (!image.save(output.filePath(name))) {
                return 1;
            }
        }
    }
    return 0;
}
