#include "screenshotpinnedgeometrymapping.h"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void physicalAndLogicalExtentsRemainDistinct() {
    const QRect client(-1921, -517, 1000, 667);
    for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        const QSize logical(qRound(client.width() / dpr), qRound(client.height() / dpr));
        const ScreenshotPinnedGeometryMapping mapping(client, logical, dpr);
        require(mapping.isValid(), "valid geometry was rejected");
        require(mapping.nativePosition({}) == client.topLeft(), "negative origin was lost");
        require(mapping.nativePosition(QPointF(logical.width(), logical.height())) ==
                    QPointF(-921, 150),
                "local extent must map to the exclusive physical edge");
        const QPointF fractional(12.375, 23.625);
        const QPointF roundTrip = mapping.localPosition(mapping.nativePosition(fractional));
        require(qAbs(roundTrip.x() - fractional.x()) < 1e-10 &&
                    qAbs(roundTrip.y() - fractional.y()) < 1e-10,
                "input mapping must retain fractional positions");
        require(mapping.containsNativePosition(client.topLeft()) &&
                    mapping.containsNativePosition(QPointF(-921.001, 149.999)) &&
                    !mapping.containsNativePosition(QPointF(-921, 149)) &&
                    !mapping.containsNativePosition(QPointF(-922, 150)),
                "client bounds must be half-open");
        require(mapping.logicalViewportSize() == QSizeF(1000.0 / dpr, 667.0 / dpr),
                "paint viewport must use DPR, not the rounded QWidget extent");
        const QSize covering = mapping.coveringLogicalSize();
        require(qRound(covering.width() * dpr) >= client.width() &&
                    qRound(covering.height() * dpr) >= client.height() &&
                    covering.width() - client.width() / dpr < 1.0 &&
                    covering.height() - client.height() / dpr < 1.0,
                "derived Qt extent must cover every pixel with less than one DIP of surplus");
        require(qAbs(mapping.viewportZoom(client.size()) * dpr - 1.0) < 1e-12,
                "100 percent content must render one source pixel per device pixel");
        const double zoom = mapping.viewportZoom(client.size());
        const QPointF camera = QPointF(500.0, 333.5) + mapping.viewportCenterOffset(zoom);
        require(
            qAbs(logical.width() / 2.0 - camera.x() * zoom) < 1e-10 &&
                qAbs(logical.height() / 2.0 - camera.y() * zoom) < 1e-10,
            "camera mapping must anchor content at physical pixel zero despite QWidget rounding");
        const QRect clipped = mapping.clippedDeviceRect(QRectF(0, 0, 1001, 668));
        require(clipped == QRect(0, 0, 1000, 667),
                "surplus backing-store pixels must not move the physical client edge");
        require(mapping.nativeHitSize(6).width() >= 6, "hit extent was scaled down unexpectedly");
    }
}

void fractionalWidthDoesNotGrowPastTheClient() {
    const ScreenshotPinnedGeometryMapping mapping(QRect(677, 395, 869, 937), QSizeF(579, 625),
                                                  1.5);
    const QSize covering = mapping.coveringLogicalSize();
    require(qRound(covering.width() * 1.5) == 869,
            "869 device pixels at 1.5x must stay on the logical size that rounds back to 869");
    require(qRound(covering.height() * 1.5) >= 937 && covering.height() <= 625,
            "937 device pixels at 1.5x must be covered without a larger logical height");
}

void edgesRoundIndependently() {
    const ScreenshotPinnedGeometryMapping mapping(QRect(-100, -100, 321, 181), QSizeF(257, 145),
                                                  1.25);
    require(mapping.clippedDeviceRect(QRectF(0.6, 0.6, 319.8, 179.8)) == QRect(1, 1, 319, 179),
            "edge rounding must not round width independently of the origin");
    require(mapping.clippedDeviceRect(QRectF(-2, -2, 325, 185)) == QRect(0, 0, 321, 181),
            "clipping must exclude every pixel outside the native client");
}

void invalidSnapshotsStayInvalid() {
    const ScreenshotPinnedGeometryMapping invalid[] = {
        {{}, QSizeF(10, 10), 1.0},
        {QRect(0, 0, 10, 10), QSizeF(0, 10), 1.0},
        {QRect(0, 0, 10, 10), QSizeF(10, 10), 0.0},
        {QRect(0, 0, 10, 10), QSizeF(10, 10), std::numeric_limits<qreal>::quiet_NaN()},
    };
    for (const auto& mapping : invalid) {
        require(!mapping.isValid() && !mapping.logicalViewportSize().isValid() &&
                    !mapping.clippedDeviceRect(QRectF(0, 0, 10, 10)).isValid() &&
                    !mapping.containsNativePosition({}),
                "invalid observations must not produce a valid physical extent");
    }
}
} // namespace

int main() {
    try {
        physicalAndLogicalExtentsRemainDistinct();
        fractionalWidthDoesNotGrowPastTheClient();
        edgesRoundIndependently();
        invalidSnapshotsStayInvalid();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "pinned geometry mapping tests passed\n";
    return 0;
}
