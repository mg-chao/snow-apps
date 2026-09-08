#include "snow_shot/presentation/styles/buttonborder.h"

#include <QImage>
#include <QPainter>
#include <QtMath>

#include <cstdlib>
#include <iostream>

namespace styles = snow_shot::presentation::styles;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void solidBordersRemainContinuousInsideWidgetClip() {
    for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 3.0}) {
        for (const QSize size : {QSize(600, 44), QSize(601, 45), QSize(602, 46), QSize(603, 47)}) {
            for (const QPoint origin : {QPoint(0, 0), QPoint(1, 3), QPoint(3, 1)}) {
                for (const int width : {1, 2}) {
                    const QRect deviceBounds(
                        QPoint(qRound(origin.x() * dpr), qRound(origin.y() * dpr)),
                        QPoint(qRound((origin.x() + size.width()) * dpr) - 1,
                               qRound((origin.y() + size.height()) * dpr) - 1));
                    QImage image(deviceBounds.right() + 5, deviceBounds.bottom() + 5,
                                 QImage::Format_ARGB32_Premultiplied);
                    image.fill(Qt::transparent);
                    // Model the backing store's rounded widget clip, including child origins
                    // whose device coordinates are fractional.
                    styles::ButtonBorderSpec spec;
                    spec.color = Qt::black;
                    spec.width = width;
                    spec.radius = 8;
                    image.setDevicePixelRatio(dpr);
                    QPainter painter(&image);
                    painter.setClipRect(QRectF(deviceBounds.x() / dpr, deviceBounds.y() / dpr,
                                               deviceBounds.width() / dpr,
                                               deviceBounds.height() / dpr));
                    painter.translate(origin);
                    styles::drawButtonBorder(&painter, size, spec);
                    painter.end();

                    const int inset = qCeil((spec.radius + width + 1) * dpr);
                    const int depth = qCeil((width + 1) * dpr);
                    const auto covered = [&](int x, int y, int dx, int dy) {
                        for (int i = 0; i < depth; ++i) {
                            if (qAlpha(image.pixel(x + i * dx, y + i * dy)) > 0) {
                                return true;
                            }
                        }
                        return false;
                    };
                    bool continuous = true;
                    for (int x = deviceBounds.left() + inset; x <= deviceBounds.right() - inset;
                         ++x) {
                        continuous &= covered(x, deviceBounds.top(), 0, 1);
                        continuous &= covered(x, deviceBounds.bottom(), 0, -1);
                    }
                    for (int y = deviceBounds.top() + inset; y <= deviceBounds.bottom() - inset;
                         ++y) {
                        continuous &= covered(deviceBounds.left(), y, 1, 0);
                        continuous &= covered(deviceBounds.right(), y, -1, 0);
                    }
                    if (!continuous) {
                        std::cerr << "DPR=" << dpr << " size=" << size.width() << 'x'
                                  << size.height() << " origin=" << origin.x() << ',' << origin.y()
                                  << " borderWidth=" << width << '\n';
                    }
                    require(continuous, "all four border edges must survive the widget clip");
                    require(qAlpha(image.pixel(deviceBounds.center())) == 0,
                            "the border must leave the content area transparent");
                    require(qAlpha(image.pixel(deviceBounds.topLeft())) == 0,
                            "rounded borders must leave the outer corner transparent");
                }
            }
        }
    }
}

void paintingPreservesCallerStateAndDashGaps() {
    for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        QImage image(qRound(200 * dpr), qRound(44 * dpr), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setPen(QPen(Qt::red, 3.0));
        painter.setBrush(Qt::blue);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QPen originalPen = painter.pen();
        const QBrush originalBrush = painter.brush();
        const auto originalHints = painter.renderHints();
        styles::ButtonBorderSpec spec;
        spec.color = Qt::black;
        spec.radius = 8;
        spec.pattern = styles::BorderPattern::Dashed;
        styles::drawButtonBorder(&painter, QSize(200, 44), spec);
        require(painter.pen() == originalPen && painter.brush() == originalBrush &&
                    painter.renderHints() == originalHints,
                "border painting must preserve the caller's painter state");
        painter.end();
        bool hasInk = false;
        bool hasGap = false;
        for (int x = qCeil(20 * dpr); x < qFloor(180 * dpr); ++x) {
            hasInk |= qAlpha(image.pixel(x, 0)) > 0;
            hasGap |= qAlpha(image.pixel(x, 0)) == 0;
        }
        require(hasInk && hasGap, "dashed borders must retain both strokes and gaps at every DPR");
    }
}
} // namespace

int main() {
    solidBordersRemainContinuousInsideWidgetClip();
    paintingPreservesCallerStateAndDashGaps();
    return 0;
}
