#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool close(qreal first, qreal second) {
    return std::abs(first - second) < 0.00001;
}

void mouse(ScreenshotScrollingThumbnailWidget& thumbnail, QEvent::Type type,
           const QPointF& position, Qt::MouseButton button = Qt::NoButton,
           Qt::MouseButtons buttons = Qt::NoButton) {
    QMouseEvent event(type, position, thumbnail.mapToGlobal(position.toPoint()), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(&thumbnail, &event);
}

void hoverGeometryAndNavigation(ScreenshotScrollingRecognitionMode mode) {
    const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    thumbnail.setRecognitionMode(mode);
    const QSize capture = horizontal ? QSize(400, 640) : QSize(640, 400);
    const QSize source = horizontal ? QSize(2400, 640) : QSize(640, 2400);
    thumbnail.setCaptureViewportSize(capture);
    thumbnail.reset();
    thumbnail.setMaximumPreviewExtent(260);
    QImage preview(horizontal ? QSize(480, 128) : QSize(128, 480), QImage::Format_RGBA8888);
    preview.fill(QColor(80, 100, 120));
    thumbnail.setStitchedImage(preview, source, ScreenshotScrollingStitchChange::Initial, 0);
    parent.show();
    thumbnail.show();
    QApplication::processEvents();
    auto* scrollBar = thumbnail.findChild<QScrollBar*>();
    require(scrollBar != nullptr && scrollBar->isVisible(), "long preview needs a scrollbar");
    require(scrollBar->geometry().isValid(), "oriented scrollbar must have a usable hit area");
    scrollBar->setValue(0);
    int notifications = 0;
    QRect notifiedSource;
    QObject::connect(&thumbnail, &ScreenshotScrollingThumbnailWidget::hoverSourceRectChanged,
                     &thumbnail, [&](const QRect& sourceRect) {
                         ++notifications;
                         notifiedSource = sourceRect;
                     });
    const auto position = [horizontal](qreal axis) {
        return horizontal ? QPointF(axis, 64) : QPointF(64, axis);
    };
    mouse(thumbnail, QEvent::MouseMove, position(130));
    QRectF box = thumbnail.hoverPreviewRectForTesting();
    require(close(box.width(), capture.width() * 0.2) &&
                close(box.height(), capture.height() * 0.2),
            "hover rectangle must use the complete capture viewport at uniform scale");
    require(close(horizontal ? box.left() : box.top(), 90), "hover rectangle must be centered");
    require(notifiedSource == (horizontal ? QRect(450, 0, 400, 640) : QRect(0, 450, 640, 400)),
            "hover source crop must match the displayed rectangle");
    mouse(thumbnail, QEvent::MouseMove, position(130.01));
    require(notifications == 1, "subpixel moves inside the same source crop must be coalesced");
    scrollBar->setValue(53);
    require(thumbnail.hoverSourceRectForTesting() ==
                (horizontal ? QRect(715, 0, 400, 640) : QRect(0, 715, 640, 400)),
            "scrolling with a stationary cursor must update the source crop");
    mouse(thumbnail, QEvent::MouseMove, position(1));
    box = thumbnail.hoverPreviewRectForTesting();
    require(close(horizontal ? box.left() : box.top(), 0), "hover must clamp at the near edge");
    require(thumbnail.hoverSourceRectForTesting() ==
                (horizontal ? QRect(265, 0, 400, 640) : QRect(0, 265, 640, 400)),
            "edge clamping must happen before mapping the scrolled source crop");
    mouse(thumbnail, QEvent::MouseMove, position(259));
    box = thumbnail.hoverPreviewRectForTesting();
    require(close(horizontal ? box.right() : box.bottom(), 260),
            "hover must stay entirely inside the far edge");
    require(QRectF(thumbnail.rect()).contains(box), "the hover rectangle must fit the thumbnail");

    mouse(thumbnail, QEvent::MouseMove, QPointF(scrollBar->geometry().center()));
    require(thumbnail.hoverSourceRectForTesting().isEmpty(),
            "scrollbar hover must suppress preview");
    mouse(thumbnail, QEvent::MouseMove, position(130));
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&thumbnail, &leave);
    require(thumbnail.hoverSourceRectForTesting().isEmpty() && notifiedSource.isEmpty(),
            "leaving must emit an empty source crop");
    const int afterLeave = notifications;
    scrollBar->setValue(20);
    require(notifications == afterLeave, "scrolling after leaving must not reactivate hover");
}

void constrainedViewportPreservesAspectAndSourceBounds(ScreenshotScrollingRecognitionMode mode) {
    const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    thumbnail.setRecognitionMode(mode);
    const QSize capture = horizontal ? QSize(400, 64) : QSize(64, 400);
    const QSize source = horizontal ? QSize(2417, 64) : QSize(64, 2417);
    thumbnail.setCaptureViewportSize(capture);
    thumbnail.setMaximumPreviewExtent(127);
    QImage preview(horizontal ? QSize(4834, 128) : QSize(128, 4834), QImage::Format_RGBA8888);
    preview.fill(Qt::white);
    thumbnail.setStitchedImage(preview, source, ScreenshotScrollingStitchChange::Initial, 0);
    parent.show();
    thumbnail.show();
    QApplication::processEvents();
    auto* scrollBar = thumbnail.findChild<QScrollBar*>();
    require(scrollBar != nullptr, "constrained preview needs a scrollbar");
    scrollBar->setValue(scrollBar->maximum());
    const QPointF point = horizontal ? QPointF(60, 4) : QPointF(4, 60);
    mouse(thumbnail, QEvent::MouseMove, point);
    const QRectF box = thumbnail.hoverPreviewRectForTesting();
    require(!box.isEmpty() && close(box.width() / box.height(),
                                    static_cast<qreal>(capture.width()) / capture.height()),
            "constrained thumbnail must retain capture aspect ratio");
    require(QRectF(thumbnail.rect()).contains(box),
            "a narrow capture rectangle must fit the visible thumbnail");
    require(close(horizontal ? box.width() : box.height(), 127),
            "uniform shrink must make the complete capture fit the available extent");
    const QRect expected = horizontal ? QRect(2017, 0, 400, 64) : QRect(0, 2017, 64, 400);
    require(thumbnail.hoverSourceRectForTesting() == expected,
            "fractional display extent at the scrollbar maximum must reach the final source pixel");
}

void lifecycleAndTrimPriority() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    thumbnail.setCaptureViewportSize(QSize(128, 100));
    QImage preview(128, 400, QImage::Format_RGBA8888);
    preview.fill(Qt::white);
    thumbnail.setStitchedImage(preview, preview.size(), ScreenshotScrollingStitchChange::Initial,
                               0);
    parent.show();
    thumbnail.show();
    QApplication::processEvents();
    bool cropping = false;
    QObject::connect(&thumbnail, &ScreenshotScrollingThumbnailWidget::hoverSourceRectChanged,
                     &thumbnail, [&](const QRect&, bool value) { cropping = value; });
    mouse(thumbnail, QEvent::MouseMove, QPointF(64, 100));
    require(!thumbnail.hoverSourceRectForTesting().isEmpty(), "hover should start before trimming");
    mouse(thumbnail, QEvent::MouseButtonPress, QPointF(64, 0), Qt::LeftButton, Qt::LeftButton);
    require(cropping && thumbnail.hoverSourceRectForTesting() == QRect(0, -50, 128, 100),
            "trim press must preview the viewport centered on the crop boundary");
    mouse(thumbnail, QEvent::MouseMove, QPointF(64, 20), Qt::NoButton, Qt::LeftButton);
    require(thumbnail.trimTop() == 20 && cropping &&
                thumbnail.hoverSourceRectForTesting() == QRect(0, -30, 128, 100),
            "active trim drag must update the crop and its boundary-centered preview");
    mouse(thumbnail, QEvent::MouseButtonRelease, QPointF(64, 20), Qt::LeftButton);
    require(!cropping && !thumbnail.hoverSourceRectForTesting().isEmpty(),
            "trim release must restore ordinary hover without the crop guide");
    for (const auto type : {QEvent::Hide, QEvent::WindowDeactivate, QEvent::UngrabMouse}) {
        mouse(thumbnail, QEvent::MouseMove, QPointF(64, 100));
        QEvent interruption(type);
        QApplication::sendEvent(&thumbnail, &interruption);
        require(thumbnail.hoverSourceRectForTesting().isEmpty(),
                "lifecycle event must clear hover");
    }
    mouse(thumbnail, QEvent::MouseMove, QPointF(64, 100));
    thumbnail.move(thumbnail.pos() + QPoint(1, 1));
    require(thumbnail.hoverSourceRectForTesting().isEmpty(),
            "moving the thumbnail must clear hover");
    mouse(thumbnail, QEvent::MouseMove, QPointF(64, 100));
    thumbnail.setCaptureViewportSize(QSize(128, 80));
    require(thumbnail.hoverSourceRectForTesting().isEmpty(),
            "capture-area changes must clear hover");
    mouse(thumbnail, QEvent::MouseMove, QPointF(64, 100));
    thumbnail.reset();
    require(thumbnail.hoverSourceRectForTesting().isEmpty(), "reset must clear hover");
    thumbnail.setStitchedImage(preview, preview.size(), ScreenshotScrollingStitchChange::Initial,
                               0);
    mouse(thumbnail, QEvent::MouseMove, QPointF(64, 100));
    require(thumbnail.hoverSourceRectForTesting().height() == 80,
            "reset must preserve an explicitly configured capture viewport");
    thumbnail.setRecognitionMode(ScreenshotScrollingRecognitionMode::Horizontal);
    require(thumbnail.hoverSourceRectForTesting().isEmpty(),
            "recognition changes must clear hover");
}

void cropPreviewTracksBothHandlesAndThumbnailScrolling(ScreenshotScrollingRecognitionMode mode) {
    const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
    const QSize capture = horizontal ? QSize(400, 650) : QSize(650, 400);
    const QSize source = horizontal ? QSize(2417, 650) : QSize(650, 2417);
    const qreal scale = 128.0 / 650;
    for (const bool head : {true, false}) {
        QWidget parent;
        ScreenshotScrollingThumbnailWidget thumbnail(parent);
        thumbnail.setRecognitionMode(mode);
        thumbnail.setCaptureViewportSize(capture);
        thumbnail.setMaximumPreviewExtent(260);
        QImage preview(horizontal ? QSize(476, 128) : QSize(128, 476), QImage::Format_RGBA8888);
        preview.fill(Qt::white);
        thumbnail.setStitchedImage(preview, source, ScreenshotScrollingStitchChange::Initial, 0);
        parent.show();
        thumbnail.show();
        QApplication::processEvents();
        auto* scrollBar = thumbnail.findChild<QScrollBar*>();
        require(scrollBar != nullptr, "crop navigation requires a scrollbar");
        scrollBar->setValue(head ? 0 : scrollBar->maximum());
        bool cropping = false;
        QObject::connect(&thumbnail, &ScreenshotScrollingThumbnailWidget::hoverSourceRectChanged,
                         &thumbnail, [&](const QRect&, bool value) { cropping = value; });
        const auto position = [horizontal](qreal axis, qreal cross = 64) {
            return horizontal ? QPointF(axis, cross) : QPointF(cross, axis);
        };
        const auto checkBoundary = [&] {
            const int boundary = head ? thumbnail.trimTop() : thumbnail.trimBottom();
            const QRect expected = horizontal ? QRect(boundary - 200, 0, 400, 650)
                                              : QRect(0, boundary - 200, 650, 400);
            require(cropping && thumbnail.hoverSourceRectForTesting() == expected,
                    "crop preview must follow the actual trim boundary at full capture size");
            const QRectF box = thumbnail.hoverPreviewRectForTesting();
            const qreal offset =
                std::min(static_cast<qreal>(scrollBar->value()), 2417 * scale - 260);
            require(
                close(horizontal ? box.center().x() : box.center().y(), boundary * scale - offset),
                "thumbnail crop highlight must stay centered on its boundary after scrolling");
        };
        mouse(thumbnail, QEvent::MouseButtonPress, position(head ? 0 : 259), Qt::LeftButton,
              Qt::LeftButton);
        checkBoundary();
        require(!QRect(QPoint(), source).contains(thumbnail.hoverSourceRectForTesting()),
                "crop windows at either result edge must extend beyond the result");
        mouse(thumbnail, QEvent::MouseMove, position(120), Qt::NoButton, Qt::LeftButton);
        checkBoundary();
        scrollBar->setValue(head ? 70 : scrollBar->maximum() - 70);
        checkBoundary();
        const int oldOffset = scrollBar->value();
        mouse(thumbnail, QEvent::MouseMove, position(head ? -40 : 300, -100), Qt::NoButton,
              Qt::LeftButton);
        checkBoundary();
        require(head ? scrollBar->value() < oldOffset : scrollBar->value() > oldOffset,
                "dragging outside the thumbnail must preserve trim auto-scrolling");
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&thumbnail, &leave);
        checkBoundary();
        QEvent enterScrollBar(QEvent::Enter);
        QApplication::sendEvent(scrollBar, &enterScrollBar);
        checkBoundary();
        mouse(thumbnail, QEvent::MouseButtonRelease, position(head ? -40 : 300), Qt::LeftButton);
        require(!cropping && thumbnail.hoverSourceRectForTesting().isEmpty(),
                "releasing outside the thumbnail must remove crop preview and its guide");

        scrollBar->setValue(head ? 0 : scrollBar->maximum());
        const int boundary = head ? thumbnail.trimTop() : thumbnail.trimBottom();
        const qreal offset = std::min(static_cast<qreal>(scrollBar->value()), 2417 * scale - 260);
        mouse(thumbnail, QEvent::MouseButtonPress, position(boundary * scale - offset),
              Qt::LeftButton, Qt::LeftButton);
        require(cropping, "a second trim press must activate crop preview again");
        QEvent interruption(QEvent::WindowDeactivate);
        QApplication::sendEvent(&thumbnail, &interruption);
        require(!cropping && thumbnail.hoverSourceRectForTesting().isEmpty(),
                "interrupting a crop drag must remove its transient preview");
    }
}

void cropPreviewUsesClampedTrimAndFullCaptureSize() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    thumbnail.setCaptureViewportSize(QSize(128, 100));
    QImage preview(128, 50, QImage::Format_RGBA8888);
    preview.fill(Qt::white);
    thumbnail.setStitchedImage(preview, preview.size(), ScreenshotScrollingStitchChange::Initial,
                               0);
    mouse(thumbnail, QEvent::MouseButtonPress, QPointF(64, 0), Qt::LeftButton, Qt::LeftButton);
    mouse(thumbnail, QEvent::MouseMove, QPointF(64, 100), Qt::NoButton, Qt::LeftButton);
    require(thumbnail.trimTop() == 49 &&
                thumbnail.hoverSourceRectForTesting() == QRect(0, -1, 128, 100),
            "crossing the opposite trim must center on the clamped boundary without shrinking");
    mouse(thumbnail, QEvent::MouseButtonRelease, QPointF(64, 100), Qt::LeftButton);
}

void blueHoverFillKeepsTrimHandlesVisible() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    thumbnail.setCaptureViewportSize(QSize(128, 100));
    QImage preview(128, 200, QImage::Format_RGBA8888);
    preview.fill(QColor(80, 100, 120));
    thumbnail.setStitchedImage(preview, preview.size(), ScreenshotScrollingStitchChange::Initial,
                               0);
    QImage before(thumbnail.size(), QImage::Format_ARGB32_Premultiplied);
    before.fill(Qt::transparent);
    thumbnail.render(&before);
    mouse(thumbnail, QEvent::MouseMove, QPointF(64, 25));
    QImage after(thumbnail.size(), QImage::Format_ARGB32_Premultiplied);
    after.fill(Qt::transparent);
    thumbnail.render(&after);
    const QColor base = before.pixelColor(64, 25);
    const QColor hovered = after.pixelColor(64, 25);
    require(hovered.blue() > base.blue() && hovered.red() < base.red(),
            "hover must add a semi-transparent blue fill");
    require(after.pixelColor(64, 0) == QColor(QStringLiteral("#faad14")),
            "orange trim handles must be painted above hover fill");
}

void terminalTrimHandleReachesRasterEdge(ScreenshotScrollingRecognitionMode mode) {
    const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
    for (const int sourceExtent : {1001, 1003, 1300}) {
        QWidget parent;
        ScreenshotScrollingThumbnailWidget thumbnail(parent);
        thumbnail.setRecognitionMode(mode);
        const QSize source = horizontal ? QSize(sourceExtent, 650) : QSize(650, sourceExtent);
        thumbnail.setCaptureViewportSize(source);
        const int previewExtent = qRound(sourceExtent * 128.0 / 650);
        QImage preview(horizontal ? QSize(previewExtent, 128) : QSize(128, previewExtent),
                       QImage::Format_RGBA8888);
        preview.fill(Qt::white);
        thumbnail.setStitchedImage(preview, source, ScreenshotScrollingStitchChange::Initial, 0);
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            QImage rendered(
                QSize(qRound(thumbnail.width() * dpr), qRound(thumbnail.height() * dpr)),
                QImage::Format_ARGB32_Premultiplied);
            rendered.setDevicePixelRatio(dpr);
            rendered.fill(Qt::transparent);
            thumbnail.render(&rendered);
            const int crossExtent = horizontal ? rendered.height() : rendered.width();
            for (int cross = 0; cross < crossExtent; ++cross) {
                const QColor edge = horizontal ? rendered.pixelColor(rendered.width() - 1, cross)
                                               : rendered.pixelColor(cross, rendered.height() - 1);
                require(edge == QColor(QStringLiteral("#faad14")),
                        "terminal trim handle must reach the raster edge without a black strip");
            }
        }
    }
}

void tilePaintingPreservesOffsetsAfterEdgePatches(ScreenshotScrollingRecognitionMode mode) {
    const bool horizontal = mode == ScreenshotScrollingRecognitionMode::Horizontal;
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    thumbnail.setRecognitionMode(mode);
    thumbnail.setCaptureViewportSize(horizontal ? QSize(100, 128) : QSize(128, 100));
    thumbnail.setMaximumPreviewExtent(400);
    const auto image = [horizontal](int extent, const QColor& color) {
        QImage value(horizontal ? QSize(extent, 128) : QSize(128, extent), QImage::Format_RGBA8888);
        value.fill(color);
        return value;
    };
    const QColor initialColor(20, 40, 60);
    const QColor appendedColor(80, 100, 120);
    const QColor prependedColor(140, 160, 180);
    const QImage initial = image(520, initialColor);
    thumbnail.setStitchedImage(initial, initial.size(), ScreenshotScrollingStitchChange::Initial,
                               0);
    thumbnail.setStitchedImage(image(180, appendedColor),
                               horizontal ? QSize(600, 128) : QSize(128, 600),
                               horizontal ? ScreenshotScrollingStitchChange::AppendedRight
                                          : ScreenshotScrollingStitchChange::AppendedDown,
                               80, false, 100);
    thumbnail.setStitchedImage(image(150, prependedColor),
                               horizontal ? QSize(660, 128) : QSize(128, 660),
                               horizontal ? ScreenshotScrollingStitchChange::PrependedLeft
                                          : ScreenshotScrollingStitchChange::PrependedUp,
                               60, false, 90);
    parent.show();
    thumbnail.show();
    QApplication::processEvents();
    auto* scrollBar = thumbnail.findChild<QScrollBar*>();
    require(scrollBar != nullptr, "edge-patch paint needs its scrollbar");
    const auto paint = [&] {
        QImage rendered(thumbnail.size(), QImage::Format_ARGB32_Premultiplied);
        rendered.fill(Qt::transparent);
        thumbnail.render(&rendered);
        return rendered;
    };
    const auto pixel = [horizontal](const QImage& rendered, int axis) {
        return horizontal ? rendered.pixelColor(axis, 64) : rendered.pixelColor(64, axis);
    };
    const QImage first = paint();
    require(pixel(first, 20) == prependedColor,
            "paint index must include the new partially filled leading tile");
    require(pixel(first, 80) == prependedColor,
            "paint index must cross the refreshed leading overlap correctly");
    scrollBar->setValue(scrollBar->maximum());
    const QImage last = paint();
    const QColor initialDimmed = pixel(last, 20);
    const QColor appendedDimmed = pixel(last, 300);
    require(initialDimmed.red() < appendedDimmed.red() &&
                initialDimmed.blue() < appendedDimmed.blue(),
            "paint index must keep source order across appended tiles at the viewport end");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    for (const auto mode : {ScreenshotScrollingRecognitionMode::Vertical,
                            ScreenshotScrollingRecognitionMode::Horizontal}) {
        hoverGeometryAndNavigation(mode);
        constrainedViewportPreservesAspectAndSourceBounds(mode);
        tilePaintingPreservesOffsetsAfterEdgePatches(mode);
        terminalTrimHandleReachesRasterEdge(mode);
        cropPreviewTracksBothHandlesAndThumbnailScrolling(mode);
    }
    lifecycleAndTrimPriority();
    cropPreviewUsesClampedTrimAndFullCaptureSize();
    blueHoverFillKeepsTrimHandlesVisible();
    std::cout << "scrolling hover geometry tests passed\n";
    return 0;
}
