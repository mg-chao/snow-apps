#include <QDir>
#include <QImage>
#include <QRect>

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "provide the generated macOS iconset directory");
    const QDir directory(QString::fromLocal8Bit(argv[1]));
    for (const int size : std::array{16, 32, 128, 256, 512}) {
        for (const int scale : std::array{1, 2}) {
            const QString name = QStringLiteral("icon_%1x%1%2.png")
                                     .arg(size)
                                     .arg(scale == 2 ? QStringLiteral("@2x") : QString());
            const QImage image(directory.filePath(name));
            const int pixels = size * scale;
            require(!image.isNull() && image.size() == QSize(pixels, pixels),
                    "every standard and Retina icon representation must exist");
            QRect artwork;
            bool green = false;
            bool purple = false;
            for (int y = 0; y < pixels; ++y) {
                for (int x = 0; x < pixels; ++x) {
                    const QColor color = image.pixelColor(x, y);
                    if (color.alpha() >= 128) {
                        artwork = artwork.united(QRect(x, y, 1, 1));
                        green |=
                            color.green() > color.red() + 20 && color.green() > color.blue() + 30;
                        purple |=
                            color.blue() > color.green() + 20 && color.red() > color.green() + 15;
                    }
                }
            }
            require(green && purple, "padding must preserve the application's colored artwork");
            const int minimumMargin = pixels * 7 / 100;
            require(artwork.left() >= minimumMargin && artwork.top() >= minimumMargin &&
                        artwork.right() < pixels - minimumMargin &&
                        artwork.bottom() < pixels - minimumMargin,
                    "Dock artwork must leave transparent space on every side");
            require(artwork.width() >= pixels * 3 / 4 && artwork.height() >= pixels * 3 / 4,
                    "Dock artwork must remain large enough to align with neighboring icons");
        }
    }
    return 0;
}
