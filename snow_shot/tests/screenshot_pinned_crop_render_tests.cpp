// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_draw_engine_qt/snow_canvas_overlay_style.h"

#include <QGuiApplication>
#include <QImage>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QImage render(const QRectF& selection, qreal dpr) {
    QImage image(qCeil(120 * dpr), qCeil(100 * dpr), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    snowCanvasPaintSelectionMarquee(painter, selection);
    painter.end();
    return image;
}

void outlineCoverage() {
    for (qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0, 3.0}) {
        for (const QRectF& selection : {QRectF(10, 12, 61, 43), QRectF(10.25, 12.75, 61.5, 43.25),
                                        QRectF(10.75, 12.25, 61.25, 43.5)}) {
            const QImage image = render(selection, dpr);
            const int centerX = qFloor(selection.center().x() * dpr);
            const int centerY = qFloor(selection.center().y() * dpr);
            const QColor fill = image.pixelColor(centerX, centerY);
            require(fill.alpha() == 0x33 && std::abs(fill.red() - 0x40) <= 1 &&
                        std::abs(fill.green() - 0x96) <= 1 && fill.blue() == 0xff,
                    "crop selection retains its translucent blue fill");
            for (bool vertical : {false, true}) {
                for (bool trailing : {false, true}) {
                    const qreal edge =
                        (vertical ? (trailing ? selection.right() : selection.left())
                                  : (trailing ? selection.bottom() : selection.top())) *
                        dpr;
                    const int begin = qFloor(edge - 3 * dpr);
                    const int end = qCeil(edge + 3 * dpr);
                    qreal alpha = 0;
                    for (int position = begin; position < end; ++position) {
                        alpha += image
                                     .pixelColor(vertical ? position : centerX,
                                                 vertical ? centerY : position)
                                     .alphaF();
                    }
                    // Remove the ideal 20%-opaque interior. A centered one-logical-pixel
                    // stroke adds full coverage outside and replaces the fill inside.
                    const qreal interior = trailing ? edge - begin : end - edge;
                    const qreal coverage = alpha - interior * .2;
                    const qreal expected = dpr * .9;
                    if (std::abs(coverage - expected) > .04) {
                        std::cerr << "DPR " << dpr << ", vertical " << vertical << ", trailing "
                                  << trailing << ": coverage " << coverage << ", expected "
                                  << expected << '\n';
                        require(false,
                                "every crop edge has the same centered logical stroke width");
                    }
                }
            }
        }
    }
}

void emptySelection() {
    for (qreal dpr : {1.0, 1.25, 2.0}) {
        for (const QRectF& selection : {QRectF(), QRectF(10, 12, 0, 43), QRectF(10, 12, 61, 0)}) {
            const QImage image = render(selection, dpr);
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    require(qAlpha(image.pixel(x, y)) == 0,
                            "an empty crop selection does not paint an inverted rectangle");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    try {
        outlineCoverage();
        emptySelection();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
