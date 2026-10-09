#include "snow_shot/presentation/screenshotoriginalimagepreviewwindow.h"

#include <QApplication>
#include <QEventLoop>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QtMath>
#include <QWindow>

#include <cmath>
#include <array>
#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#endif

#ifdef Q_OS_MACOS
#include "snow_shot/platform/screenshotnative.h"
#include <QElapsedTimer>
#include <QTimer>
#import <AppKit/AppKit.h>
#endif

class ScreenshotOriginalImagePreviewWindowTestAccess final {
  public:
    static const QImage& raster(const ScreenshotOriginalImagePreviewWindow& window) {
        return window.m_viewportImage;
    }
    static const QImage& source(const ScreenshotOriginalImagePreviewWindow& window) {
        return window.m_sourceImage;
    }
    static quint64 generation(const ScreenshotOriginalImagePreviewWindow& window) {
        return window.m_rasterGeneration;
    }
    static QWidget* aboveSibling(const ScreenshotOriginalImagePreviewWindow& window) {
        return window.m_aboveSibling;
    }
    static bool beginDrag(ScreenshotOriginalImagePreviewWindow& window, const QPointF& cursor) {
        return window.beginDrag(cursor);
    }
    static void moveDrag(ScreenshotOriginalImagePreviewWindow& window, const QPointF& cursor) {
        window.moveDrag(cursor);
    }
    static void endDrag(ScreenshotOriginalImagePreviewWindow& window) {
        window.endDrag();
    }
};

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void settle() {
    for (int pass = 0; pass < 4; ++pass)
        QApplication::processEvents(QEventLoop::AllEvents);
}

QImage sourceImage(const QSize& size = QSize(16, 12)) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            row[x] = qRgb(x % 251, y % 251, (x + y) % 251);
    }
    return image;
}

ScreenshotOriginalImagePreviewState stateFor(QWidget* owner = nullptr) {
    ScreenshotOriginalImagePreviewState state;
    state.image = sourceImage();
    state.imageRectInViewport = QRectF(0, 0, 16, 12);
    state.resultRect = QRect(140, 140, 16, 12);
    state.transientOwner = owner;
    return state;
}

void originalImageStateDefaultsDisableFormulaPresentation() {
    const ScreenshotOriginalImagePreviewState state{
        sourceImage(), QRectF(0, 0, 16, 12), QRect(140, 140, 16, 12), nullptr, true, false};
    require(state.pinned && !state.staysOnTop && state.aboveSibling == nullptr && !state.formula &&
                !state.dimmed && !state.background.isValid() && !state.statusColor.isValid() &&
                state.status.isEmpty(),
            "original-image state must default to an undimmed preview without formula status");
}

class ResizeCounter final : public QObject {
  public:
    int count = 0;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize)
            ++count;
        return QObject::eventFilter(watched, event);
    }
};

bool sendMouseEvent(QWidget& widget, QEvent::Type type, const QPointF& globalPosition,
                    Qt::MouseButton button, Qt::MouseButtons buttons) {
    const QPointF localPosition = globalPosition - QPointF(widget.mapToGlobal(QPoint()));
    QMouseEvent event(type, localPosition, globalPosition, button, buttons, Qt::NoModifier);
    event.ignore();
    QApplication::sendEvent(&widget, &event);
    return event.isAccepted();
}

void placementHonorsTheResultMonitorAndPriority() {
    using Preview = ScreenshotOriginalImagePreviewWindow;
    const QRect work(0, 0, 1000, 800);
    require(Preview::placement(QRect(400, 300, 200, 100), work, 4) == QRect(196, 300, 200, 100),
            "left placement must have priority and align to the result top");
    require(Preview::placement(QRect(20, 300, 200, 100), work, 4) == QRect(20, 196, 200, 100),
            "above placement must follow insufficient space on the left");
    require(Preview::placement(QRect(20, 20, 200, 100), work, 4) == QRect(224, 20, 200, 100),
            "right placement must follow insufficient space on the left and above");
    require(Preview::placement(QRect(20, 20, 200, 100), QRect(0, 0, 230, 120), 4) ==
                QRect(-184, 20, 200, 100),
            "fallback must keep the left rectangle without shrinking or clamping");
    require(Preview::placement(QRect(204, 0, 200, 100), work, 4) == QRect(0, 0, 200, 100),
            "an exact fit on the work-area edge must be accepted");
    require(Preview::placement(QRect(20, 20, 200, 100), QRect(0, 0, 1000, 80), 4) ==
                QRect(-184, 20, 200, 100),
            "candidate fit must check both dimensions");
    require(Preview::placement(QRect(-1200, -200, 200, 100), QRect(-1600, -300, 1600, 900), 4) ==
                QRect(-1404, -200, 200, 100),
            "negative monitor coordinates must retain the same placement priority");
    require(Preview::placement(QRect(20, 20, 200, 100), QRect(0, 0, 1000, 800), -10) ==
                QRect(220, 20, 200, 100),
            "negative gaps must be normalized to zero");
    require(Preview::placement({}, work, 4).isEmpty(), "empty results must not produce a preview");
}

void viewportRasterPreservesClippingAndLetterboxing() {
    ScreenshotOriginalImagePreviewWindow preview;
    auto state = stateFor();
    state.resultRect.setSize(QSize(24, 20));
    state.imageRectInViewport = QRectF(4, 3, 16, 12);
    require(preview.present(state), "letterboxed image presentation failed");
    settle();
    const QImage& raster = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    const qreal dpr = preview.devicePixelRatioF();
    const qreal scale = ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry() ? 1.0 : dpr;
    require(raster.size() == QSize(qRound(24 * scale), qRound(20 * scale)) &&
                qFuzzyCompare(raster.devicePixelRatio(), dpr),
            "the raster must have viewport dimensions and the preview's actual output DPR");
    require(raster.pixelColor(0, 0).alpha() == 0 &&
                raster.pixelColor(raster.width() - 1, raster.height() - 1).alpha() == 0,
            "letterbox pixels must remain transparent");
    require(raster.pixelColor(qRound(6 * scale), qRound(5 * scale)).alpha() == 255,
            "image pixels must remain visible within the fitted image rectangle");
    require(ScreenshotOriginalImagePreviewWindowTestAccess::source(preview).constBits() ==
                state.image.constBits(),
            "the preview must retain a shared source image without copying its pixels");

    state.imageRectInViewport = QRectF(-4, -3, 16, 12);
    require(preview.present(state), "panned image presentation failed");
    settle();
    const QImage& panned = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    require(panned.pixelColor(0, 0).alpha() == 255 &&
                panned.pixelColor(qRound(15 * scale), qRound(15 * scale)).alpha() == 0,
            "panned images must clip to the viewport rather than fit the source anew");
    if (qFuzzyCompare(scale, qreal(1)))
        require(panned.pixel(0, 0) == state.image.pixel(4, 3),
                "panning must preserve the source-to-viewport pixel correspondence");
}

void largeSourceCoordinatesRetainVisiblePixels() {
    ScreenshotOriginalImagePreviewWindow preview;
    auto state = stateFor();
    state.image = sourceImage(QSize(40000, 2));
    state.resultRect.setSize(QSize(160, 40));
    state.imageRectInViewport = QRectF(-35000, 10, 40000, 2);
    require(preview.present(state), "large-source preview failed");
    settle();
    const QImage& raster = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    const qreal scale = ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry()
                            ? 1.0
                            : preview.devicePixelRatioF();
    require(raster.pixelColor(qRound(20 * scale), qRound(10 * scale)).alpha() == 255,
            "visible image slices beyond the raster coordinate limit must not disappear");
    if (qFuzzyCompare(scale, qreal(1))) {
        require(raster.pixel(20, 10) == state.image.pixel(35020, 0) &&
                    raster.pixel(100, 11) == state.image.pixel(35100, 1),
                "large-source clipping must retain the original pixels");
    }
    require(ScreenshotOriginalImagePreviewWindowTestAccess::source(preview).constBits() ==
                state.image.constBits(),
            "large-source slicing must keep source storage shared");
}

void sourceDprMetadataDoesNotAlterViewportMapping() {
    ScreenshotOriginalImagePreviewWindow preview;
    auto state = stateFor();
    require(preview.present(state), "untagged source preview failed");
    settle();
    const QImage expected = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    state.image.setDevicePixelRatio(2);
    require(preview.present(state), "DPR-tagged source preview failed");
    settle();
    require(ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview) == expected,
            "source DPR metadata must not scale the explicit source-to-viewport mapping");
}

void movementReusesTheRasterAndContentChangesInvalidateIt() {
    ScreenshotOriginalImagePreviewWindow preview;
    auto state = stateFor();
    require(preview.present(state), "initial cached preview failed");
    settle();
    const quint64 firstGeneration =
        ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
    const uchar* pixels =
        ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).constBits();
    for (int iteration = 0; iteration < 30; ++iteration) {
        state.resultRect.translate(1, 0);
        require(preview.present(state), "moving cached preview failed");
    }
    settle();
    require(
        ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) == firstGeneration &&
            ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).constBits() == pixels,
        "position changes must retain the same viewport raster");
    preview.hide();
    require(preview.present(state), "restoring cached preview failed");
    settle();
    require(ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) == firstGeneration,
            "hide and restore must reuse the existing viewport raster");
    QEvent backingChange(QEvent::DevicePixelRatioChange);
    QApplication::sendEvent(&preview, &backingChange);
    preview.hide();
    settle();
    require(!preview.isVisible() && ScreenshotOriginalImagePreviewWindowTestAccess::generation(
                                        preview) == firstGeneration,
            "a queued backing update must not rebuild or resurrect a drag-hidden preview");
    require(preview.present(state), "restoring after a queued backing update failed");
    state.imageRectInViewport.translate(1, 0);
    require(preview.present(state), "changing the viewport mapping failed");
    require(ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                firstGeneration + 1,
            "panning must invalidate the raster exactly once");
    state.resultRect.setSize(QSize(18, 14));
    require(preview.present(state), "resizing the preview failed");
    require(ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                firstGeneration + 2,
            "viewport size changes must invalidate the raster");
    state.image.setPixelColor(1, 1, Qt::red);
    require(preview.present(state), "changing source pixels failed");
    require(ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                firstGeneration + 3,
            "source revision changes must invalidate the raster");
}

void formulaStateSurvivesQueuedBackingRefreshes() {
    ScreenshotOriginalImagePreviewWindow preview;
    auto state = stateFor();
    state.resultRect.setSize(QSize(160, 100));
    state.image = QImage(24, 20, QImage::Format_ARGB32_Premultiplied);
    state.image.fill(QColor(210, 30, 40));
    state.imageRectInViewport = QRectF(16, 16, 24, 20);
    state.formula = true;
    state.dimmed = true;
    state.background = QColor(20, 50, 90);
    state.statusColor = QColor(245, 235, 220);
    state.status = QStringLiteral("Invalid formula source");
    require(preview.present(state), "presenting a dimmed formula preview failed");
    settle();
    const QImage initial = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    const quint64 generation = ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
    const qreal scale = ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry()
                            ? 1.0
                            : preview.devicePixelRatioF();
    require(initial.pixelColor(0, 0) == state.background &&
                initial.pixelColor(qRound(20 * scale), qRound(20 * scale)) !=
                    state.image.pixelColor(4, 4) &&
                preview.accessibleDescription() == state.status,
            "formula errors must retain an opaque themed background and dim the prior image");
    for (const QEvent::Type eventType :
         {QEvent::DevicePixelRatioChange, QEvent::ScreenChangeInternal, QEvent::WinIdChange}) {
        QEvent backingChange(eventType);
        QApplication::sendEvent(&preview, &backingChange);
        settle();
        require(preview.isVisible() && preview.accessibleDescription() == state.status &&
                    ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview) == initial &&
                    ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                        generation,
                "queued backing refreshes must preserve formula pixels, error status and cache");
    }
    state.background = QColor(90, 35, 20);
    state.statusColor = QColor(225, 245, 235);
    state.status = QStringLiteral("Renderer unavailable");
    require(preview.present(state), "changing the formula preview theme failed");
    settle();
    require(ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).pixelColor(0, 0) ==
                    state.background &&
                preview.accessibleDescription() == state.status &&
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                    generation + 1,
            "formula theme and status changes must invalidate the raster once");
}

void formulaPlaceholdersRetainNullImagesAndExactSmallExtents() {
    ScreenshotOriginalImagePreviewWindow preview;
    auto state = stateFor();
    state.image = {};
    state.resultRect.setSize(QSize(97, 65));
    state.imageRectInViewport = QRectF(QPointF(), QSizeF(state.resultRect.size()));
    state.formula = true;
    state.background = QColor(35, 75, 115);
    state.statusColor = QColor(245, 245, 245);
    state.status = QStringLiteral("Rendering formula...");
    require(preview.present(state), "presenting an image-free formula placeholder failed");
    settle();
    const QImage initial = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    const quint64 generation = ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
    require(!initial.isNull() && initial.pixelColor(0, 0) == state.background &&
                ScreenshotOriginalImagePreviewWindowTestAccess::source(preview).isNull() &&
                preview.accessibleDescription() == state.status,
            "formula loading must remain visible without a rendered source image");
    for (const QEvent::Type eventType :
         {QEvent::DevicePixelRatioChange, QEvent::ScreenChangeInternal}) {
        QEvent backingChange(eventType);
        QApplication::sendEvent(&preview, &backingChange);
        settle();
        require(preview.isVisible() && preview.accessibleDescription() == state.status &&
                    ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview) == initial &&
                    ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                        generation,
                "queued backing refreshes must preserve image-free formula placeholders");
    }
    state.resultRect.setSize(QSize(3, 2));
    state.imageRectInViewport = QRectF(QPointF(), QSizeF(state.resultRect.size()));
    state.status.clear();
    require(preview.present(state), "presenting a tiny formula placeholder failed");
    settle();
    const QImage& tiny = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    const qreal scale = ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry()
                            ? 1.0
                            : preview.devicePixelRatioF();
    require(tiny.size() == QSize(qRound(3 * scale), qRound(2 * scale)) &&
                tiny.pixelColor(0, 0) == state.background && preview.size() == QSize(3, 2),
            "placeholder rendering must retain exact tiny viewport dimensions");
}

void formulaErrorsFitTheCompleteImageAboveTheFooter() {
    ScreenshotOriginalImagePreviewWindow preview;
    auto state = stateFor();
    state.resultRect.setSize(QSize(200, 100));
    state.image = QImage(120, 80, QImage::Format_ARGB32_Premultiplied);
    state.image.fill(QColor(180, 180, 180));
    {
        QPainter painter(&state.image);
        painter.fillRect(QRect(0, 0, 12, 12), Qt::red);
        painter.fillRect(QRect(108, 0, 12, 12), Qt::green);
        painter.fillRect(QRect(0, 68, 12, 12), Qt::blue);
        painter.fillRect(QRect(108, 68, 12, 12), Qt::yellow);
    }
    state.imageRectInViewport = QRectF(40, 10, 120, 80);
    state.formula = true;
    state.background = QColor(20, 20, 20);
    state.statusColor = QColor(220, 220, 220);
    require(preview.present(state), "presenting the formula marker fixture failed");
    settle();
    const QImage valid = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    const qreal scale = ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry()
                            ? 1.0
                            : preview.devicePixelRatioF();
    require(valid.pixelColor(qRound(46 * scale), qRound(16 * scale)) == QColor(Qt::red) &&
                valid.pixelColor(qRound(154 * scale), qRound(84 * scale)) == QColor(Qt::yellow),
            "a valid formula retains its supplied source mapping");
    state.dimmed = true;
    state.status = QStringLiteral("Invalid formula source");
    require(preview.present(state), "presenting the retained formula and error footer failed");
    settle();
    const QImage error = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    std::array<int, 4> markerPixels{};
    QRect markerBounds;
    int brightestMarker = 0;
    for (int y = 0; y < error.height(); ++y) {
        for (int x = 0; x < error.width(); ++x) {
            const QColor color = error.pixelColor(x, y);
            int marker = -1;
            if (color.red() > color.green() + 35 && color.red() > color.blue() + 35)
                marker = 0;
            else if (color.green() > color.red() + 35 && color.green() > color.blue() + 35)
                marker = 1;
            else if (color.blue() > color.red() + 35 && color.blue() > color.green() + 35)
                marker = 2;
            else if (color.red() > color.blue() + 35 && color.green() > color.blue() + 35)
                marker = 3;
            if (marker >= 0) {
                ++markerPixels[static_cast<size_t>(marker)];
                markerBounds |= QRect(x, y, 1, 1);
                brightestMarker =
                    std::max({brightestMarker, color.red(), color.green(), color.blue()});
            }
        }
    }
    require(std::all_of(markerPixels.begin(), markerPixels.end(),
                        [](int count) { return count > 4; }) &&
                markerBounds.bottom() < error.height() * 0.6 &&
                std::abs(qreal(markerBounds.width()) / markerBounds.height() - 1.5) < 0.1 &&
                brightestMarker < 180 && preview.accessibleDescription() == state.status,
            "an error footer preserves all dimmed formula corners and their complete aspect ratio");
    const quint64 generation = ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
    state.resultRect.translate(20, 10);
    require(preview.present(state), "moving the formula marker fixture failed");
    require(ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) == generation &&
                ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview) == error,
            "moving a formula footer must reuse its composed raster");
    state.resultRect.setSize(QSize(31, 13));
    state.image = QImage(31, 13, QImage::Format_ARGB32_Premultiplied);
    state.image.fill(QColor(240, 80, 40));
    state.imageRectInViewport = QRectF(0, 0, 31, 13);
    state.statusColor = Qt::blue;
    require(preview.present(state), "presenting a tiny retained formula failed");
    const QImage tiny = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
    bool formulaVisible = false;
    for (int y = 0; y < tiny.height(); ++y) {
        for (int x = 0; x < tiny.width(); ++x) {
            const QColor color = tiny.pixelColor(x, y);
            require(color.blue() <= color.red(),
                    "a tiny formula viewport must omit its unreadable visual footer");
            formulaVisible = formulaVisible || color.red() > color.blue() + 35;
        }
    }
    require(
        formulaVisible && tiny.size() == QSize(qRound(31 * scale), qRound(13 * scale)) &&
            preview.accessibleDescription() == state.status,
        "tiny selections contract padding while retaining formula pixels and accessible errors");
}

void mouseDraggingPreservesTheViewportAndOwner() {
    if (ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry())
        return;
    QWidget owner;
    owner.setGeometry(100, 100, 200, 120);
    owner.show();
    owner.activateWindow();
    owner.setFocus();
    settle();
    ScreenshotOriginalImagePreviewWindow preview(&owner);
    auto state = stateFor(&owner);
    state.resultRect.setSize(QSize(97, 65));
    require(preview.present(state), "preparing the mouse drag fixture failed");
    settle();
    const QRect initial = preview.geometry();
    const QRect ownerGeometry = owner.geometry();
    QWidget* const focused = QApplication::focusWidget();
    const quint64 generation = ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
    const uchar* const pixels =
        ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).constBits();
    ResizeCounter resizeCounter;
    preview.installEventFilter(&resizeCounter);
    const QPointF cursor(initial.topLeft() + QPoint(7, 9));
    require(preview.cursor().shape() == Qt::OpenHandCursor,
            "a draggable preview must show the open hand cursor before dragging");
    sendMouseEvent(preview, QEvent::MouseButtonPress, cursor, Qt::RightButton, Qt::RightButton);
    sendMouseEvent(preview, QEvent::MouseMove, cursor + QPointF(30, 20), Qt::NoButton,
                   Qt::RightButton);
    sendMouseEvent(preview, QEvent::MouseButtonRelease, cursor + QPointF(30, 20), Qt::RightButton,
                   Qt::NoButton);
    require(preview.geometry() == initial && preview.cursor().shape() == Qt::OpenHandCursor,
            "right-button mouse input must not begin or move a preview drag");
    require(
        sendMouseEvent(preview, QEvent::MouseButtonPress, cursor, Qt::LeftButton, Qt::LeftButton) &&
            preview.cursor().shape() == Qt::ClosedHandCursor,
        "left-button input must start dragging and show the closed hand cursor");
    for (int step = 1; step <= 20; ++step) {
        const QPoint delta(3 * step, 2 * step);
        require(sendMouseEvent(preview, QEvent::MouseMove, cursor + delta, Qt::NoButton,
                               Qt::LeftButton),
                "left-button drag movement must be accepted");
        require(preview.geometry() == initial.translated(delta),
                "drag movement must preserve the pressed point's global cursor offset");
    }
    require(sendMouseEvent(preview, QEvent::MouseButtonRelease, cursor + QPointF(60, 40),
                           Qt::LeftButton, Qt::NoButton) &&
                preview.cursor().shape() == Qt::OpenHandCursor,
            "releasing the left button must end the drag and restore its open hand cursor");
    settle();
    const QRect dragged = initial.translated(60, 40);
    sendMouseEvent(preview, QEvent::MouseMove, cursor + QPointF(80, 50), Qt::NoButton,
                   Qt::NoButton);
    require(preview.geometry() == dragged && resizeCounter.count == 0 &&
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) == generation &&
                ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).constBits() ==
                    pixels,
            "dragging must only move the preview and reuse its unchanged viewport raster");
    require(owner.geometry() == ownerGeometry && QApplication::focusWidget() == focused &&
                preview.windowHandle()->transientParent() == owner.windowHandle(),
            "dragging must preserve the recognition owner's geometry, focus, and ownership");
}

void clickingDoesNotDetachAutomaticPlacement() {
    if (ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry())
        return;
    ScreenshotOriginalImagePreviewWindow preview;
    auto state = stateFor();
    require(preview.present(state), "preparing click-only placement failed");
    settle();
    const QRect initial = preview.geometry();
    const QPointF cursor(initial.topLeft() + QPoint(5, 4));
    const quint64 generation = ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
    require(
        sendMouseEvent(preview, QEvent::MouseButtonPress, cursor, Qt::LeftButton, Qt::LeftButton),
        "pressing a preview without moving failed");
    sendMouseEvent(preview, QEvent::MouseMove, cursor, Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(preview, QEvent::MouseButtonRelease, cursor, Qt::LeftButton, Qt::NoButton);
    state.resultRect.translate(30, 20);
    ScreenshotOriginalImagePreviewWindow automaticPreview;
    require(automaticPreview.present(state), "preparing the click-only automatic fixture failed");
    require(preview.present(state) && preview.geometry() == automaticPreview.geometry() &&
                preview.geometry() != initial &&
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) == generation,
            "clicking without moving must preserve automatic placement relative to the result");
}

void draggedPlacementSurvivesContentAndBackingRefreshes() {
    if (ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry())
        return;
    ScreenshotOriginalImagePreviewWindow preview;
    auto state = stateFor();
    require(preview.present(state), "preparing the retained placement fixture failed");
    settle();
    const QPointF cursor(preview.geometry().topLeft() + QPoint(5, 4));
    require(ScreenshotOriginalImagePreviewWindowTestAccess::beginDrag(preview, cursor),
            "starting the retained placement drag failed");
    ScreenshotOriginalImagePreviewWindowTestAccess::moveDrag(preview, cursor + QPointF(93, 67));
    ScreenshotOriginalImagePreviewWindowTestAccess::endDrag(preview);
    const QPoint draggedPosition = preview.geometry().topLeft();
    const quint64 generation = ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
    const uchar* const pixels =
        ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).constBits();
    state.resultRect.translate(200, 100);
    require(preview.present(state) && preview.geometry().topLeft() == draggedPosition &&
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) == generation &&
                ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).constBits() ==
                    pixels,
            "moving the recognition result must retain the user's preview position and pixels");
    QEvent screenChange(QEvent::ScreenChangeInternal);
    QApplication::sendEvent(&preview, &screenChange);
    QEvent backingChange(QEvent::DevicePixelRatioChange);
    QApplication::sendEvent(&preview, &backingChange);
    settle();
    require(preview.geometry().topLeft() == draggedPosition &&
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) == generation,
            "queued screen and backing refreshes must retain the dragged position and pixels");
    state.imageRectInViewport.translate(1, 0);
    require(preview.present(state) && preview.geometry().topLeft() == draggedPosition &&
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                    generation + 1,
            "changing the source mapping must refresh pixels without resetting user placement");
    state.resultRect.setSize(QSize(23, 19));
    require(preview.present(state) && preview.geometry() == QRect(draggedPosition, QSize(23, 19)) &&
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                    generation + 2,
            "resizing the recognition result must retain the dragged preview's top-left position");
    state.image.setPixelColor(1, 1, Qt::red);
    require(preview.present(state) && preview.geometry().topLeft() == draggedPosition &&
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                    generation + 3,
            "replacing source pixels must refresh the retained preview at its chosen position");
    ScreenshotOriginalImagePreviewWindow automaticPreview;
    require(automaticPreview.present(state), "preparing the automatic reset placement failed");
    settle();
    const QRect automaticGeometry = automaticPreview.geometry();
    preview.clear();
    require(preview.present(state) && preview.geometry() == automaticGeometry,
            "clearing the preview must reset user placement to automatic result placement");
}

void hidingAndLosingMouseGrabCancelDragging() {
    if (ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry())
        return;
    ScreenshotOriginalImagePreviewWindow preview;
    const auto state = stateFor();
    require(preview.present(state), "preparing drag cancellation failed");
    settle();
    const QPointF cursor(preview.geometry().topLeft() + QPoint(5, 4));
    require(
        sendMouseEvent(preview, QEvent::MouseButtonPress, cursor, Qt::LeftButton, Qt::LeftButton),
        "starting the hide cancellation drag failed");
    sendMouseEvent(preview, QEvent::MouseMove, cursor + QPointF(30, 20), Qt::NoButton,
                   Qt::LeftButton);
    const QRect dragged = preview.geometry();
    preview.hide();
    require(preview.cursor().shape() == Qt::OpenHandCursor,
            "hiding the preview must cancel its active drag");
    ScreenshotOriginalImagePreviewWindowTestAccess::moveDrag(preview, cursor + QPointF(60, 40));
    require(preview.present(state) && preview.geometry() == dragged,
            "restoring a hidden preview must retain placement without resuming a stale drag");
    const QPointF nextCursor(dragged.topLeft() + QPoint(5, 4));
    require(sendMouseEvent(preview, QEvent::MouseButtonPress, nextCursor, Qt::LeftButton,
                           Qt::LeftButton),
            "starting the lost-grab cancellation drag failed");
    QEvent lostGrab(QEvent::UngrabMouse);
    QApplication::sendEvent(&preview, &lostGrab);
    sendMouseEvent(preview, QEvent::MouseMove, nextCursor + QPointF(20, 10), Qt::NoButton,
                   Qt::LeftButton);
    require(preview.geometry() == dragged && preview.cursor().shape() == Qt::OpenHandCursor,
            "losing the mouse grab must cancel dragging before later move events arrive");
    require(sendMouseEvent(preview, QEvent::MouseButtonPress, nextCursor, Qt::LeftButton,
                           Qt::LeftButton),
            "starting the missing-button cancellation drag failed");
    sendMouseEvent(preview, QEvent::MouseMove, nextCursor + QPointF(20, 10), Qt::NoButton,
                   Qt::NoButton);
    require(preview.geometry() == dragged && preview.cursor().shape() == Qt::OpenHandCursor,
            "a move without the left button must cancel dragging without moving the preview");
    require(ScreenshotOriginalImagePreviewWindowTestAccess::beginDrag(preview, nextCursor),
            "starting the clear cancellation drag failed");
    preview.clear();
    require(!preview.isVisible() && preview.cursor().shape() == Qt::OpenHandCursor,
            "clearing the preview must cancel its active drag");
}

void passiveWindowKeepsOwnershipAndClearsInvalidState() {
    auto owner = std::make_unique<QWidget>();
    owner->setGeometry(100, 100, 200, 100);
    owner->show();
    owner->activateWindow();
    owner->setFocus();
    settle();
    QPointer<ScreenshotOriginalImagePreviewWindow> preview =
        new ScreenshotOriginalImagePreviewWindow(owner.get());
    auto state = stateFor(owner.get());
    require(preview->present(state), "owned preview presentation failed");
    settle();
    require(preview->isWindow() && preview->focusPolicy() == Qt::NoFocus &&
                preview->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus) &&
                preview->testAttribute(Qt::WA_ShowWithoutActivating) &&
                !preview->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                !preview->windowFlags().testFlag(Qt::WindowTransparentForInput),
            "the preview must accept dragging without accepting focus or activation");
    require(QApplication::focusWidget() != preview &&
                preview->windowHandle()->transientParent() == owner->windowHandle(),
            "presenting the preview must retain its owner without taking focus");
    auto sibling = std::make_unique<QWidget>(owner.get(), Qt::Tool | Qt::FramelessWindowHint);
    state.aboveSibling = sibling.get();
    require(preview->present(state), "retaining the optional toolbar sibling failed");
    QEvent siblingBackingChange(QEvent::DevicePixelRatioChange);
    QApplication::sendEvent(preview, &siblingBackingChange);
    settle();
    require(ScreenshotOriginalImagePreviewWindowTestAccess::aboveSibling(*preview) == sibling.get(),
            "scheduled backing refresh must preserve the optional toolbar sibling");
    sibling.reset();
    require(ScreenshotOriginalImagePreviewWindowTestAccess::aboveSibling(*preview) == nullptr,
            "destroying the optional sibling must clear its guarded pointer");
    state.aboveSibling = nullptr;
    state.staysOnTop = false;
    require(preview->present(state), "changing preview stacking failed");
    require(!preview->windowFlags().testFlag(Qt::WindowStaysOnTopHint),
            "logical-platform stacking must follow the state");
    state.imageRectInViewport.setX(std::numeric_limits<qreal>::quiet_NaN());
    require(!preview->present(state) && !preview->isVisible() &&
                ScreenshotOriginalImagePreviewWindowTestAccess::raster(*preview).isNull(),
            "invalid source geometry must hide and release the cached raster");
    state.imageRectInViewport = QRectF(1e308, 0, 1e308, 1);
    require(!preview->present(state) && !preview->isVisible(),
            "finite coordinates whose combined extent overflows must be rejected");
    state = stateFor(owner.get());
    require(preview->present(state), "reopening after invalid state failed");
    QEvent backingChange(QEvent::DevicePixelRatioChange);
    QApplication::sendEvent(preview, &backingChange);
    preview->clear();
    settle();
    require(!preview->isVisible() &&
                ScreenshotOriginalImagePreviewWindowTestAccess::source(*preview).isNull() &&
                ScreenshotOriginalImagePreviewWindowTestAccess::aboveSibling(*preview) == nullptr &&
                preview->windowHandle()->transientParent() == nullptr,
            "clearing must release source storage and transient ownership");
    require(preview->present(state), "reopening after clear failed");
    owner.reset();
    require(preview.isNull(), "destroying the owner must destroy the preview safely");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
bool nativeWindowIsAbove(HWND higher, HWND lower) {
    for (HWND window = GetWindow(lower, GW_HWNDPREV); window;
         window = GetWindow(window, GW_HWNDPREV)) {
        if (window == higher)
            return true;
    }
    return false;
}

void nativePreviewPreservesAlienRecognitionAndCanvasChildren() {
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.resize(360, 220);
    QWidget canvas(&owner);
    canvas.setGeometry(0, 0, 360, 220);
    QWidget recognition(&owner);
    recognition.setGeometry(20, 20, 180, 100);
    owner.show();
    settle();
    require(owner.internalWinId() && !canvas.internalWinId() && !recognition.internalWinId(),
            "the native-child fixture must start with alien recognition and canvas widgets");
    ScreenshotOriginalImagePreviewWindow preview(&recognition);
    auto state = stateFor(&owner);
    state.resultRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner);
    state.imageRectInViewport = QRectF(QPointF(), QSizeF(state.resultRect.size()));
    require(preview.present(state), "presenting the alien-owned native preview failed");
    preview.repaint();
    settle();
    require(preview.internalWinId() && !preview.testAttribute(Qt::WA_NativeWindow) &&
                !canvas.internalWinId() && !recognition.internalWinId() &&
                !canvas.testAttribute(Qt::WA_NativeWindow) &&
                !recognition.testAttribute(Qt::WA_NativeWindow),
            "creating the preview must preserve the owner's alien recognition/canvas siblings");
}

void nativeOverlappingSiblingRemainsAboveWithoutActivation() {
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.setGeometry(100, 100, 220, 120);
    owner.show();
    settle();
    ScreenshotOriginalImagePreviewWindow preview(&owner);
    auto state = stateFor(&owner);
    state.resultRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner);
    state.imageRectInViewport = QRectF(QPointF(), QSizeF(state.resultRect.size()));
    require(preview.present(state), "preparing the overlapping sibling fixture failed");
    settle();
    const QRect resultRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner);
    const QRect previewRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview);
    const HWND previewHwnd = reinterpret_cast<HWND>(preview.internalWinId());
    QWidget sibling(&owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                                Qt::WindowDoesNotAcceptFocus);
    sibling.setAttribute(Qt::WA_ShowWithoutActivating);
    sibling.resize(60, 24);
    sibling.show();
    settle();
    const HWND siblingHwnd = reinterpret_cast<HWND>(sibling.internalWinId());
    require(siblingHwnd != nullptr &&
                SetWindowPos(siblingHwnd, HWND_TOPMOST, previewRect.x(), previewRect.y(), 60, 24,
                             SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING) != FALSE,
            "positioning the overlapping passive sibling failed");
    settle();
    state.aboveSibling = &sibling;
    for (const bool staysOnTop : {true, false}) {
        state.staysOnTop = staysOnTop;
        require(SetWindowPos(siblingHwnd, staysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE,
                "setting the sibling's native stacking band failed");
        require(preview.present(state), "presenting with an overlapping sibling failed");
        settle();
        const QRect siblingRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&sibling);
        const HWND foreground = GetForegroundWindow();
        require(SetWindowPos(previewHwnd, staysOnTop ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE,
                "preparing the reversed overlap stacking failed");
        require(nativeWindowIsAbove(previewHwnd, siblingHwnd),
                "the overlap fixture must place the preview above the sibling first");
        preview.refreshStacking();
        settle();
        require(nativeWindowIsAbove(siblingHwnd, previewHwnd),
                "the overlapping same-band sibling must remain above the preview");
        require(GetForegroundWindow() == foreground &&
                    ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner) == resultRect &&
                    ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview) ==
                        previewRect &&
                    ScreenshotOriginalImagePreviewWindow::nativeClientRect(&sibling) == siblingRect,
                "repairing overlap stacking must preserve activation and all window rectangles");
    }
    sibling.hide();
    preview.refreshStacking();
    settle();
    require(!sibling.isVisible(), "refreshing stacking must not restore a hidden sibling");
}

void nativeWindowMatchesOddResultExtentsAfterRepaints() {
    require(ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry(),
            "native geometry tests require the Windows platform");
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "native geometry tests require a monitor");
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.setScreen(screen);
    owner.setGeometry(QRect(screen->geometry().topLeft() + QPoint(100, 100), QSize(421, 239)));
    owner.show();
    settle();
    std::cout << "native monitors=" << QGuiApplication::screens().size()
              << "; effective Qt DPR=" << owner.devicePixelRatioF() << '\n';
    if (qEnvironmentVariableIsSet("SNOW_PREVIEW_TEST_DPR")) {
        bool validFactor = false;
        const qreal factor = qEnvironmentVariable("SNOW_PREVIEW_TEST_DPR").toDouble(&validFactor);
        require(validFactor && qFuzzyCompare(owner.devicePixelRatioF(), factor),
                "the deterministic matrix must exercise the requested Qt DPR");
    }
    ScreenshotOriginalImagePreviewWindow preview(&owner);
    for (const QSize size : {QSize(421, 239), QSize(503, 277), QSize(97, 25)}) {
        const auto native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        require(native && GetMonitorInfoW(native->handle(), &info) != FALSE,
                "native monitor geometry unavailable");
        const HWND hwnd = reinterpret_cast<HWND>(owner.winId());
        require(SetWindowPos(hwnd, nullptr, info.rcWork.left + 101, info.rcWork.top + 113,
                             size.width(), size.height(), SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                "setting native result geometry failed");
        settle();
        if (ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner).size() != size) {
            require(SetWindowPos(hwnd, nullptr, info.rcWork.left + 101, info.rcWork.top + 113,
                                 size.width(), size.height(),
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING) != FALSE,
                    "setting the exact odd native result extent failed");
            settle();
        }
        require(ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner).size() == size,
                "the fixture result must expose the requested odd physical extent");
        auto state = stateFor(&owner);
        state.resultRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner);
        state.imageRectInViewport = QRectF(QPointF(), QSizeF(state.resultRect.size()));
        require(preview.present(state), "native preview presentation failed");
        for (int pass = 0; pass < 8; ++pass) {
            owner.repaint();
            preview.repaint();
            settle();
            require(ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner).size() == size,
                    "repainting must preserve the fixture's odd result extent");
            const QRect observed = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview);
            if (observed.size() != state.resultRect.size()) {
                std::cerr << "expected " << state.resultRect.width() << 'x'
                          << state.resultRect.height() << "; observed " << observed.width() << 'x'
                          << observed.height() << "; DPR " << preview.devicePixelRatioF()
                          << "; widget " << preview.width() << 'x' << preview.height() << "; pass "
                          << pass << '\n';
            }
            require(observed.size() == state.resultRect.size(),
                    "preview physical dimensions must match the result after every repaint");
        }
        const QImage painted = preview.grab().toImage();
        const QImage& raster = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
        if (painted.width() < size.width() || painted.height() < size.height()) {
            std::cerr << "painted " << painted.width() << 'x' << painted.height() << "; target "
                      << size.width() << 'x' << size.height() << "; DPR "
                      << preview.devicePixelRatioF() << "; widget " << preview.width() << 'x'
                      << preview.height() << '\n';
        }
        require(painted.width() >= size.width() && painted.height() >= size.height() &&
                    painted.pixel(0, 0) == raster.pixel(0, 0) &&
                    painted.pixel(size.width() - 1, size.height() - 1) ==
                        raster.pixel(size.width() - 1, size.height() - 1),
                "the native paint surface must retain the first and final visible image pixels");
        const QImage desktop = preview.windowHandle()->screen()->grabWindow(0).toImage();
        const QRect visible =
            ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview).translated(
                -QPoint(info.rcMonitor.left, info.rcMonitor.top));
        const QImage displayed = desktop.copy(visible);
        if (displayed.size() != size ||
            displayed.pixelColor(0, 0).rgb() != raster.pixelColor(0, 0).rgb() ||
            displayed.pixelColor(size.width() - 1, size.height() - 1).rgb() !=
                raster.pixelColor(size.width() - 1, size.height() - 1).rgb()) {
            std::cerr << "displayed " << displayed.width() << 'x' << displayed.height()
                      << "; first " << std::hex << displayed.pixelColor(0, 0).rgb() << '/'
                      << raster.pixelColor(0, 0).rgb() << "; last "
                      << displayed.pixelColor(size.width() - 1, size.height() - 1).rgb() << '/'
                      << raster.pixelColor(size.width() - 1, size.height() - 1).rgb() << std::dec
                      << '\n';
        }
        require(displayed.size() == size &&
                    displayed.pixelColor(0, 0).rgb() == raster.pixelColor(0, 0).rgb() &&
                    displayed.pixelColor(size.width() - 1, size.height() - 1).rgb() ==
                        raster.pixelColor(size.width() - 1, size.height() - 1).rgb(),
                "the native visible surface must retain the full physical image raster");
        require(ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).size() ==
                    state.resultRect.size(),
                "native raster size must match the exact visible result extent");
        preview.hide();
        require(preview.present(state), "restoring native preview failed");
        settle();
        require(ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview).size() ==
                    state.resultRect.size(),
                "restore must retain exact physical dimensions");
        const QImage retainedRaster =
            ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
        const QRgb retainedCorner = retainedRaster.pixel(size.width() - 1, size.height() - 1);
        preview.clear();
        require(!preview.isVisible() &&
                    ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).isNull() &&
                    retainedRaster.pixel(size.width() - 1, size.height() - 1) == retainedCorner,
                "clearing native storage must preserve the lifetime of shared raster snapshots");
        require(preview.present(state), "reopening after clearing native storage failed");
        settle();
    }
    auto state = stateFor(&owner);
    state.resultRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner);
    state.imageRectInViewport = QRectF(QPointF(), QSizeF(state.resultRect.size()));
    require(preview.present(state), "preparing cross-monitor image mapping failed");
    const quint64 crossMonitorGeneration =
        ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
    for (QScreen* destination : QGuiApplication::screens()) {
        const auto native = destination->nativeInterface<QNativeInterface::QWindowsScreen>();
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!native || GetMonitorInfoW(native->handle(), &info) == FALSE)
            continue;
        state.resultRect.moveTopLeft(QPoint(info.rcWork.left + 150, info.rcWork.top + 100));
        require(preview.present(state), "cross-monitor preview placement failed");
        preview.repaint();
        settle();
        require(ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview).size() ==
                    state.resultRect.size(),
                "cross-monitor DPI changes must preserve result-sized physical extents");
        require(ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                    crossMonitorGeneration,
                "physical placement changes must reuse the raster across monitor DPI grids");
        const int gap = qRound(4 * destination->devicePixelRatio());
        const QRect work(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
                         info.rcWork.bottom - info.rcWork.top);
        require(ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview) ==
                    ScreenshotOriginalImagePreviewWindow::placement(state.resultRect, work, gap),
                "placement must use the result monitor's physical work area");
    }
    for (QScreen* destination : QGuiApplication::screens()) {
        const auto native = destination->nativeInterface<QNativeInterface::QWindowsScreen>();
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!native || GetMonitorInfoW(native->handle(), &info) == FALSE)
            continue;
        const QRect work(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
                         info.rcWork.bottom - info.rcWork.top);
        const QSize largeOddExtent((work.width() / 2 + 1) | 1, (work.height() / 2 + 1) | 1);
        state.resultRect = QRect(work.topLeft() + QPoint(17, 19), largeOddExtent);
        state.imageRectInViewport = QRectF(QPointF(), QSizeF(largeOddExtent));
        const int gap = qRound(4 * destination->devicePixelRatio());
        const QRect fallback(work.left() + 17 - largeOddExtent.width() - gap, work.top() + 19,
                             largeOddExtent.width(), largeOddExtent.height());
        require(ScreenshotOriginalImagePreviewWindow::placement(state.resultRect, work, gap) ==
                        fallback &&
                    fallback.left() < work.left(),
                "an oversized result near the monitor corner must fall back to the left");
        require(preview.present(state), "native off-monitor fallback presentation failed");
        const quint64 fallbackGeneration =
            ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
        for (int pass = 0; pass < 4; ++pass) {
            preview.repaint();
            settle();
            require(ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview) == fallback,
                    "off-monitor fallback must retain the result monitor's exact native rectangle");
            require(ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                        fallbackGeneration,
                    "a fallback onto a different DPI grid must retain its viewport pixels");
        }
    }
}

void nativeFormulaPlaceholderPreservesOddPhysicalExtents() {
    require(ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry(),
            "native formula placeholders require Windows physical geometry");
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "native formula placeholders require a monitor");
    const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    require(native && GetMonitorInfoW(native->handle(), &info) != FALSE,
            "native formula placeholder work area is available");
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.setScreen(screen);
    owner.setGeometry(
        QRect(screen->availableGeometry().topLeft() + QPoint(500, 113), QSize(200, 120)));
    owner.show();
    settle();
    ScreenshotOriginalImagePreviewWindow preview(&owner);
    auto state = stateFor(&owner);
    state.image = {};
    state.formula = true;
    state.background = QColor(25, 75, 125);
    state.statusColor = QColor(245, 245, 245);
    for (const QSize size : {QSize(421, 239), QSize(97, 25), QSize(3, 2)}) {
        state.resultRect =
            QRect(info.rcWork.left + 500, info.rcWork.top + 113, size.width(), size.height());
        state.imageRectInViewport = QRectF(QPointF(), QSizeF(size));
        state.status = size.width() > 3 ? QStringLiteral("Rendering formula...") : QString{};
        require(preview.present(state), "presenting an odd native formula placeholder failed");
        settle();
        const quint64 generation =
            ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
        for (int pass = 0; pass < 4; ++pass) {
            QEvent backingChange(QEvent::DevicePixelRatioChange);
            QApplication::sendEvent(&preview, &backingChange);
            preview.repaint();
            settle();
            const QImage& raster = ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview);
            require(preview.isVisible() &&
                        ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview).size() ==
                            size &&
                        raster.size() == size && raster.pixelColor(0, 0) == state.background &&
                        raster.pixelColor(size.width() - 1, size.height() - 1) ==
                            state.background &&
                        preview.accessibleDescription() == state.status &&
                        ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                            generation,
                    "native placeholder refresh must preserve exact pixels, dimensions and status");
        }
    }
}

void nativeDraggingPreservesOddPhysicalExtentsAcrossMonitors() {
    require(ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry(),
            "native drag tests require Windows physical geometry");
    QScreen* primary = QGuiApplication::primaryScreen();
    require(primary != nullptr, "native drag tests require a monitor");
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.setScreen(primary);
    owner.setGeometry(
        QRect(primary->availableGeometry().topLeft() + QPoint(101, 113), QSize(180, 100)));
    owner.show();
    settle();
    const QRect ownerGeometry = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner);
    ScreenshotOriginalImagePreviewWindow preview(&owner);
    for (const QSize size : {QSize(421, 239), QSize(503, 277), QSize(97, 25)}) {
        preview.clear();
        auto state = stateFor(&owner);
        state.image = sourceImage(size);
        state.resultRect = QRect(ownerGeometry.topLeft(), size);
        state.imageRectInViewport = QRectF(QPointF(), QSizeF(size));
        require(preview.present(state), "preparing native cross-monitor dragging failed");
        settle();
        const quint64 generation =
            ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
        const uchar* const pixels =
            ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).constBits();
        const QPoint offset(11, 13);
        const QPointF cursor(
            ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview).topLeft() + offset);
        require(ScreenshotOriginalImagePreviewWindowTestAccess::beginDrag(preview, cursor),
                "starting a physical preview drag failed");
        const HWND foreground = GetForegroundWindow();
        for (QScreen* destination : QGuiApplication::screens()) {
            const auto native = destination->nativeInterface<QNativeInterface::QWindowsScreen>();
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            require(native && GetMonitorInfoW(native->handle(), &info) != FALSE,
                    "native drag destination geometry unavailable");
            const QPoint requested(info.rcWork.left + 113, info.rcWork.top + 127);
            ScreenshotOriginalImagePreviewWindowTestAccess::moveDrag(preview, requested + offset);
            preview.repaint();
            settle();
            const QRect dragged = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview);
            const int gridTolerance = qCeil(destination->devicePixelRatio());
            require(qAbs(dragged.x() - requested.x()) <= gridTolerance &&
                        qAbs(dragged.y() - requested.y()) <= gridTolerance &&
                        MonitorFromWindow(reinterpret_cast<HWND>(preview.internalWinId()),
                                          MONITOR_DEFAULTTONEAREST) == native->handle(),
                    "physical dragging must move the preview itself onto its destination grid");
            for (int pass = 0; pass < 4; ++pass) {
                require(preview.present(state), "refreshing the native dragged preview failed");
                QEvent backingChange(QEvent::DevicePixelRatioChange);
                QApplication::sendEvent(&preview, &backingChange);
                preview.repaint();
                settle();
                require(
                    ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview) == dragged &&
                        dragged.size() == size &&
                        ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).size() ==
                            size &&
                        ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                            generation &&
                        ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview)
                                .constBits() == pixels,
                    "cross-monitor dragging and refresh must retain exact odd physical extents "
                    "and the cached image raster");
                require(ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner) ==
                                ownerGeometry &&
                            GetForegroundWindow() == foreground,
                        "dragging a native preview must preserve its fixed recognition owner "
                        "and foreground window");
            }
        }
        ScreenshotOriginalImagePreviewWindowTestAccess::endDrag(preview);
        require(preview.cursor().shape() == Qt::OpenHandCursor,
                "ending a native drag must restore the open hand cursor");
        const QRect released = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview);
        QEvent backingChange(QEvent::DevicePixelRatioChange);
        QApplication::sendEvent(&preview, &backingChange);
        preview.repaint();
        settle();
        require(ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview) == released &&
                    released.size() == size &&
                    ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                        generation &&
                    ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).constBits() ==
                        pixels,
                "backing refresh after release must retain the chosen physical rectangle, odd "
                "extent, and cached image raster");
    }
}
#endif

#ifdef Q_OS_MACOS
NSWindow* macNativeWindow(QWidget& widget) {
    if (!widget.internalWinId())
        return nil;
    return reinterpret_cast<NSView*>(widget.internalWinId()).window;
}

void settleMacNative() {
    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
}

bool macWindowIsAbove(NSWindow* higher, NSWindow* lower) {
    NSArray<NSWindow*>* const windows = NSApp.orderedWindows;
    const NSUInteger higherIndex = [windows indexOfObjectIdenticalTo:higher];
    const NSUInteger lowerIndex = [windows indexOfObjectIdenticalTo:lower];
    return higherIndex != NSNotFound && lowerIndex != NSNotFound && higherIndex < lowerIndex;
}

void macStackingRejectsOffscreenSurfaces() {
    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
        return;
    QWidget lower(nullptr, Qt::Tool);
    QWidget higher(nullptr, Qt::Tool);
    require(!snow_shot::platform::stackScreenshotWindowBelow(&lower, &higher) &&
                !lower.internalWinId() && !higher.internalWinId(),
            "offscreen relative stacking must decline without creating native surfaces");
    lower.show();
    higher.show();
    settle();
    require(!snow_shot::platform::stackScreenshotWindowBelow(&lower, &higher),
            "offscreen window handles must never be interpreted as Cocoa views");
}

void macPreviewPreservesLogicalGeometryAndAlienChildren() {
    require(QGuiApplication::platformName() == QStringLiteral("cocoa") &&
                !ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry(),
            "macOS native preview tests require Cocoa logical geometry");
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.resize(421, 239);
    QWidget canvas(&owner);
    canvas.setGeometry(owner.rect());
    QWidget recognition(&owner);
    recognition.setGeometry(20, 20, 180, 100);
    owner.show();
    settleMacNative();
    require(owner.internalWinId() && !canvas.internalWinId() && !recognition.internalWinId(),
            "the Cocoa fixture must start with alien recognition and canvas widgets");
    ScreenshotOriginalImagePreviewWindow preview(&recognition);
    auto state = stateFor(&owner);
    for (const bool pinned : {false, true}) {
        state.pinned = pinned;
        for (QScreen* destination : QGuiApplication::screens()) {
            for (const QSize size : {QSize(421, 239), QSize(503, 277), QSize(97, 25)}) {
                owner.setScreen(destination);
                owner.setGeometry(
                    QRect(destination->availableGeometry().topLeft() + QPoint(101, 113), size));
                canvas.setGeometry(owner.rect());
                settleMacNative();
                state.resultRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner);
                state.imageRectInViewport = QRectF(QPointF(), QSizeF(state.resultRect.size()));
                require(preview.present(state), "presenting the logical Cocoa preview failed");
                for (int pass = 0; pass < 3; ++pass) {
                    preview.repaint();
                    settleMacNative();
                    require(preview.size() == state.resultRect.size() &&
                                ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview)
                                        .size() == state.resultRect.size(),
                            "Cocoa preview dimensions must match the result in logical pixels");
                    require(!preview.testAttribute(Qt::WA_NativeWindow) &&
                                !canvas.internalWinId() && !recognition.internalWinId() &&
                                !canvas.testAttribute(Qt::WA_NativeWindow) &&
                                !recognition.testAttribute(Qt::WA_NativeWindow),
                            "creating and moving the Cocoa preview must preserve alien siblings");
                }
            }
        }
    }
}

void macDraggingPreservesLogicalExtentsAcrossMonitors() {
    require(QGuiApplication::platformName() == QStringLiteral("cocoa") &&
                !ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry(),
            "Cocoa drag tests require logical geometry");
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.setGeometry(100, 100, 180, 100);
    owner.show();
    settleMacNative();
    const QRect ownerGeometry = owner.geometry();
    ScreenshotOriginalImagePreviewWindow preview(&owner);
    for (const QSize size : {QSize(421, 239), QSize(503, 277), QSize(97, 25)}) {
        preview.clear();
        auto state = stateFor(&owner);
        state.resultRect = QRect(ownerGeometry.topLeft(), size);
        state.imageRectInViewport = QRectF(QPointF(), QSizeF(size));
        require(preview.present(state), "preparing Cocoa cross-monitor dragging failed");
        settleMacNative();
        const QPoint offset(11, 13);
        const QPointF cursor(preview.geometry().topLeft() + offset);
        require(ScreenshotOriginalImagePreviewWindowTestAccess::beginDrag(preview, cursor),
                "starting a logical preview drag failed");
        for (QScreen* destination : QGuiApplication::screens()) {
            const QPoint requested = destination->availableGeometry().topLeft() + QPoint(113, 127);
            ScreenshotOriginalImagePreviewWindowTestAccess::moveDrag(preview, requested + offset);
            settleMacNative();
            const QRect dragged(requested, size);
            const BOOL active = [NSApp isActive];
            NSWindow* keyWindow = NSApp.keyWindow;
            const quint64 generation =
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
            const uchar* const pixels =
                ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).constBits();
            for (int pass = 0; pass < 3; ++pass) {
                require(preview.present(state), "refreshing the logical dragged preview failed");
                preview.repaint();
                settleMacNative();
                NSWindow* native = macNativeWindow(preview);
                require(preview.geometry() == dragged &&
                            ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview) ==
                                dragged &&
                            qRound(native.frame.size.width) == size.width() &&
                            qRound(native.frame.size.height) == size.height() &&
                            !native.ignoresMouseEvents,
                        "Cocoa dragging must retain the same logical extent on every monitor "
                        "and accept mouse input");
                require(owner.geometry() == ownerGeometry && [NSApp isActive] == active &&
                            NSApp.keyWindow == keyWindow &&
                            ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                                generation &&
                            ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview)
                                    .constBits() == pixels,
                        "Cocoa drag refresh must reuse stable backing pixels while preserving "
                        "owner geometry and activation");
            }
        }
        ScreenshotOriginalImagePreviewWindowTestAccess::endDrag(preview);
    }
}

void macOversizedPreviewRetainsTheUnconstrainedLeftFallback() {
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.resize(180, 100);
    owner.show();
    settleMacNative();
    for (QScreen* destination : QGuiApplication::screens()) {
        owner.setScreen(destination);
        owner.move(destination->availableGeometry().topLeft() + QPoint(11, 13));
        settleMacNative();
        const QRect ownerRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner);
        const NSRect ownerFrame = macNativeWindow(owner).frame;
        for (const bool pinned : {false, true}) {
            ScreenshotOriginalImagePreviewWindow preview(&owner);
            auto state = stateFor(&owner);
            state.pinned = pinned;
            // Model an oversized result without allocating another full-screen source image.
            state.resultRect =
                QRect(ownerRect.topLeft(), destination->geometry().size() + QSize(17, 17));
            state.imageRectInViewport = QRectF(QPointF(), QSizeF(state.resultRect.size()));
            const QRect fallback(state.resultRect.x() - state.resultRect.width() - 4,
                                 state.resultRect.y(), state.resultRect.width(),
                                 state.resultRect.height());
            require(preview.present(state), "presenting the oversized Cocoa fallback failed");
            for (int pass = 0; pass < 3; ++pass) {
                preview.repaint();
                settleMacNative();
                NSWindow* native = macNativeWindow(preview);
                const NSRect frame = native.frame;
                require(preview.geometry() == fallback &&
                            ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview) ==
                                fallback &&
                            !(native.styleMask & NSWindowStyleMaskTitled) &&
                            qRound(frame.size.width) == fallback.width() &&
                            qRound(frame.size.height) == fallback.height() &&
                            qRound(frame.origin.x - ownerFrame.origin.x) ==
                                fallback.x() - ownerRect.x() &&
                            qRound(NSMaxY(frame) - NSMaxY(ownerFrame)) ==
                                ownerRect.y() - fallback.y(),
                        "Cocoa must preserve the exact oversized borderless left fallback and "
                        "native frame");
            }
        }
    }
}

void macSiblingStackingPreservesActivationAndVisibility() {
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    QScreen* primary = QGuiApplication::primaryScreen();
    require(primary != nullptr, "the Cocoa stacking fixture requires a screen");
    const QRect available = primary->availableGeometry();
    owner.setGeometry(QRect(available.center() - QPoint(110, 60), QSize(220, 120)));
    owner.show();
    settleMacNative();
    ScreenshotOriginalImagePreviewWindow preview(&owner);
    auto state = stateFor(&owner);
    state.pinned = true;
    state.resultRect = ScreenshotOriginalImagePreviewWindow::nativeClientRect(&owner);
    state.imageRectInViewport = QRectF(QPointF(), QSizeF(state.resultRect.size()));
    require(preview.present(state), "preparing the Cocoa overlap fixture failed");
    settleMacNative();
    QWidget sibling(&owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    sibling.setAttribute(Qt::WA_ShowWithoutActivating);
    sibling.setGeometry(QRect(preview.geometry().topLeft(), QSize(60, 24)));
    sibling.show();
    settleMacNative();
    NSWindow* previewNative = macNativeWindow(preview);
    NSWindow* siblingNative = macNativeWindow(sibling);
    require(previewNative && siblingNative, "Cocoa overlap surfaces must have native windows");
    // Real pin surfaces stay visible while another application is active. Match that
    // behavior in this plain-widget fixture before testing inactive-app ordering.
    macNativeWindow(owner).hidesOnDeactivate = NO;
    siblingNative.hidesOnDeactivate = NO;
    QWidget uncreated(nullptr, Qt::Tool);
    require(!snow_shot::platform::stackScreenshotWindowBelow(&preview, &uncreated) &&
                !uncreated.internalWinId(),
            "relative ordering must not materialize an uncreated sibling");
    [NSApp deactivate];
    QElapsedTimer deactivation;
    deactivation.start();
    while ([NSApp isActive] && deactivation.elapsed() < 2000)
        settleMacNative();
    require(![NSApp isActive], "the Cocoa passivity fixture must deactivate before ordering");
    for (const bool staysOnTop : {true, false}) {
        state.staysOnTop = staysOnTop;
        state.aboveSibling = nullptr;
        require(preview.present(state), "preparing the preview's Cocoa stacking band failed");
        settleMacNative();
        siblingNative.level = previewNative.level;
        [previewNative orderWindow:NSWindowAbove relativeTo:siblingNative.windowNumber];
        require(macWindowIsAbove(previewNative, siblingNative),
                "the Cocoa overlap fixture must begin with reversed same-band order");
        const BOOL active = [NSApp isActive];
        NSWindow* keyWindow = NSApp.keyWindow;
        const NSInteger previewLevel = previewNative.level;
        const NSInteger siblingLevel = siblingNative.level;
        const QRect previewRect = preview.geometry();
        const QRect siblingRect = sibling.geometry();
        const QRect resultRect = owner.geometry();
        state.aboveSibling = &sibling;
        require(preview.present(state), "presenting with an overlapping Cocoa sibling failed");
        settleMacNative();
        require(macWindowIsAbove(siblingNative, previewNative) &&
                    previewNative.level == previewLevel && siblingNative.level == siblingLevel,
                "Cocoa relative stacking must preserve both levels and put the sibling above");
        require([NSApp isActive] == active && NSApp.keyWindow == keyWindow &&
                    owner.geometry() == resultRect && preview.geometry() == previewRect &&
                    sibling.geometry() == siblingRect && !previewNative.ignoresMouseEvents,
                "Cocoa preview stacking must preserve activation, key window, geometry and input");
        const quint64 generation =
            ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
        QEvent backingChange(QEvent::DevicePixelRatioChange);
        QApplication::sendEvent(&preview, &backingChange);
        preview.hide();
        settleMacNative();
        require(!preview.isVisible() && !previewNative.visible && [NSApp isActive] == active &&
                    NSApp.keyWindow == keyWindow,
                "queued Cocoa backing updates must not restore a suppressed preview");
        require(preview.present(state), "restoring the hidden Cocoa preview failed");
        settleMacNative();
        require(preview.isVisible() && previewNative.visible &&
                    macWindowIsAbove(siblingNative, previewNative) &&
                    ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) ==
                        generation &&
                    [NSApp isActive] == active && NSApp.keyWindow == keyWindow,
                "Cocoa restoration must reuse pixels and retain passive sibling ordering");
        sibling.hide();
        preview.refreshStacking();
        settleMacNative();
        require(!sibling.isVisible() && !siblingNative.visible &&
                    !snow_shot::platform::stackScreenshotWindowBelow(&preview, &sibling),
                "stacking refresh must not restore a hidden Cocoa sibling");
        sibling.show();
        siblingNative.hidesOnDeactivate = NO;
        settleMacNative();
    }
}
#endif
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        if (application.arguments().contains(QStringLiteral("--native-only"))) {
#if defined(Q_OS_WIN) || defined(_WIN32)
            nativePreviewPreservesAlienRecognitionAndCanvasChildren();
            nativeWindowMatchesOddResultExtentsAfterRepaints();
            nativeFormulaPlaceholderPreservesOddPhysicalExtents();
            nativeOverlappingSiblingRemainsAboveWithoutActivation();
            nativeDraggingPreservesOddPhysicalExtentsAcrossMonitors();
#elif defined(Q_OS_MACOS)
            macPreviewPreservesLogicalGeometryAndAlienChildren();
            macOversizedPreviewRetainsTheUnconstrainedLeftFallback();
            macSiblingStackingPreservesActivationAndVisibility();
            macDraggingPreservesLogicalExtentsAcrossMonitors();
#endif
        } else {
            originalImageStateDefaultsDisableFormulaPresentation();
            placementHonorsTheResultMonitorAndPriority();
            viewportRasterPreservesClippingAndLetterboxing();
            largeSourceCoordinatesRetainVisiblePixels();
            sourceDprMetadataDoesNotAlterViewportMapping();
            movementReusesTheRasterAndContentChangesInvalidateIt();
            formulaStateSurvivesQueuedBackingRefreshes();
            formulaPlaceholdersRetainNullImagesAndExactSmallExtents();
            formulaErrorsFitTheCompleteImageAboveTheFooter();
            mouseDraggingPreservesTheViewportAndOwner();
            clickingDoesNotDetachAutomaticPlacement();
            draggedPlacementSurvivesContentAndBackingRefreshes();
            hidingAndLosingMouseGrabCancelDragging();
            passiveWindowKeepsOwnershipAndClearsInvalidState();
#ifdef Q_OS_MACOS
            macStackingRejectsOffscreenSurfaces();
#endif
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
