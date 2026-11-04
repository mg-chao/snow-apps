#include "snow_shot/shortcuts/shortcutbinding.h"
#include "snow_shot/presentation/screenshotsavepreviewcanvas.h"
#include "screenshotexportmemory_p.h"
#include "snow_shot/presentation/canvasstatusreadout.h"
#include "snow_shot/presentation/screenshotwheelinput.h"
#include "theme/theme_manager.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr qreal kSplitHitHalfWidth = 15.0;
constexpr qreal kSplitTrackWidth = 8.0;
constexpr qreal kSplitThumbRadius = 24.0;
constexpr qreal kSplitThumbOutlineWidth = 1.0;
constexpr qreal kSplitThumbHitRadius = 30.0;
constexpr qreal kSplitArrowHalfHeight = 8.0;
constexpr qreal kSplitArrowWidth = 7.0;
} // namespace

ScreenshotSavePreviewCanvas::ScreenshotSavePreviewCanvas(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("savePreviewCanvas"));
    setMinimumSize(180, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    m_checkerboard = QPixmap(24, 24);
    m_checkerboard.fill(Qt::white);
    QPainter tilePainter(&m_checkerboard);
    tilePainter.fillRect(0, 0, 12, 12, QColor(0, 0, 0, 28));
    tilePainter.fillRect(12, 12, 12, 12, QColor(0, 0, 0, 28));
    m_readout = new CanvasStatusReadout(this);
    m_readout->setObjectName(QStringLiteral("savePreviewZoom"));
    m_readout->dismissalTimer()->setObjectName(QStringLiteral("savePreviewZoomTimer"));
    m_busy = new CanvasStatusReadout(this);
    m_busy->setObjectName(QStringLiteral("savePreviewStatus"));
    m_busy->setMessage(CanvasStatusReadout::Message::RenderingPreview);
    m_detailTimer.setSingleShot(true);
    m_detailTimer.setInterval(60);
    connect(&m_detailTimer, &QTimer::timeout, this, [this] { requestDetails(); });
    updateReadout();
}
ScreenshotSavePreviewCanvas::~ScreenshotSavePreviewCanvas() {
    m_detailJob.cancel();
}
void ScreenshotSavePreviewCanvas::setSource(QImage image, QSize pixels, DetailRenderer renderer) {
    m_original = std::move(image);
    m_output = {};
    m_sourcePixels = pixels;
    m_sourceRenderer = std::move(renderer);
    m_outputRenderer = {};
    m_sourceDetail = {};
    m_outputDetail = {};
    m_outputPixels = {};
    m_readout->dismiss();
    fitImage();
}
void ScreenshotSavePreviewCanvas::setPdfLayout(screenshot_pdf::Layout layout) {
    if (layout.pagePoints == m_pdfLayout.pagePoints &&
        layout.imagePoints == m_pdfLayout.imagePoints)
        return;
    m_pdfLayout = layout;
    update();
}
void ScreenshotSavePreviewCanvas::setOutput(QImage image, QSize pixels, DetailRenderer renderer) {
    m_outputPixels = pixels.isEmpty() ? image.size() : pixels;
    m_output = std::move(image);
    m_outputRenderer = std::move(renderer);
    m_outputDetail = {};
    scheduleDetails();
    update();
}

QRectF ScreenshotSavePreviewCanvas::imageBounds() const {
    const QSizeF dimensions = QSizeF(m_sourcePixels) * m_zoom;
    const QPointF center = QRectF(rect()).center() + m_pan;
    return {center.x() - dimensions.width() / 2, center.y() - dimensions.height() / 2,
            dimensions.width(), dimensions.height()};
}
QRect ScreenshotSavePreviewCanvas::detailRequest(const QImage& preview, QSize pixels,
                                                 const DetailRenderer& renderer,
                                                 const Detail& detail) const {
    if (!renderer || pixels.isEmpty() || preview.isNull())
        return {};
    const QRectF bounds = imageBounds();
    if (bounds.isEmpty() ||
        (preview.width() >= qMin(qreal(pixels.width()), bounds.width() * devicePixelRatioF()) &&
         preview.height() >= qMin(qreal(pixels.height()), bounds.height() * devicePixelRatioF())))
        return {};
    const QRectF visible = bounds.intersected(QRectF(rect()));
    if (visible.isEmpty())
        return {};
    const qreal scaleX = pixels.width() / bounds.width();
    const qreal scaleY = pixels.height() / bounds.height();
    const QRect request =
        QRectF((visible.left() - bounds.left()) * scaleX, (visible.top() - bounds.top()) * scaleY,
               visible.width() * scaleX, visible.height() * scaleY)
            .toAlignedRect()
            .adjusted(-2, -2, 2, 2)
            .intersected(QRect(QPoint{}, pixels));
    if (detail.region.contains(request) && !detail.image.isNull() &&
        detail.image.width() >=
            detail.region.width() * qMin(qreal{1}, devicePixelRatioF() / scaleX) &&
        detail.image.height() >=
            detail.region.height() * qMin(qreal{1}, devicePixelRatioF() / scaleY))
        return {};
    return request;
}
void ScreenshotSavePreviewCanvas::scheduleDetails() {
    ++m_detailSerial;
    m_detailJob.cancel();
    m_detailJob = {};
    if (m_rendering)
        m_detailTimer.stop();
    else
        m_detailTimer.start();
}
void ScreenshotSavePreviewCanvas::requestDetails() {
    const QRect original =
        detailRequest(m_original, m_sourcePixels, m_sourceRenderer, m_sourceDetail);
    const QRect output = detailRequest(m_output, m_outputPixels, m_outputRenderer, m_outputDetail);
    if (original.isEmpty() && output.isEmpty())
        return;
    struct Result {
        QImage original;
        QImage output;
    };
    auto images = std::make_shared<Result>();
    const quint64 serial = m_detailSerial;
    qint64 workingBytes = std::max(
        original.isEmpty() ? 0 : screenshot_export_memory::previewWorkingBytes(m_sourcePixels),
        output.isEmpty() ? 0 : screenshot_export_memory::previewWorkingBytes(m_outputPixels));
    if (!screenshot_pinned_render::addBytes(workingBytes, qint64{16} * 1024 * 1024))
        workingBytes = std::numeric_limits<qint64>::max();
    m_detailJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [original, output, sourceRenderer = m_sourceRenderer, outputRenderer = m_outputRenderer,
         images](const ScreenshotExportCancellation& cancellation) {
            ScreenshotExportTaskResult result;
            if (!original.isEmpty())
                images->original = sourceRenderer(original, cancellation, &result.error);
            if (!output.isEmpty() && !cancellation.isCancellationRequested())
                images->output = outputRenderer(output, cancellation, &result.error);
            if ((!original.isEmpty() && images->original.isNull()) ||
                (!output.isEmpty() && images->output.isNull()))
                result.failureStage = ScreenshotExportFailureStage::Render;
            return result;
        },
        [this, serial, original, output, images](ScreenshotExportTaskResult result) {
            if (serial != m_detailSerial)
                return;
            m_detailJob = {};
            if (!result.succeeded()) {
                setProperty("detailError", result.error);
                return;
            }
            if (!original.isEmpty())
                m_sourceDetail = {std::move(images->original), original};
            if (!output.isEmpty())
                m_outputDetail = {std::move(images->output), output};
            setProperty("detailGeneration", QVariant::fromValue(serial));
            update();
        },
        workingBytes);
}
void ScreenshotSavePreviewCanvas::setSplitRatio(double value) {
    m_split = std::clamp(value, 0.0, 1.0);
    setProperty("splitRatio", m_split);
    update();
}
void ScreenshotSavePreviewCanvas::fitImage(bool showZoomReadout) {
    if (m_sourcePixels.isEmpty())
        return;
    const double previousZoom = m_zoom;
    m_fit = true;
    m_pan = {};
    m_zoom = std::min({1.0, double(std::max(1, width() - 40)) / m_sourcePixels.width(),
                       double(std::max(1, height() - 40)) / m_sourcePixels.height()});
    updateReadout();
    if (showZoomReadout && !qFuzzyCompare(previousZoom, m_zoom))
        showReadout();
    scheduleDetails();
    update();
}
void ScreenshotSavePreviewCanvas::updateReadout() {
    m_readout->setMessage(CanvasStatusReadout::Message::Scale, m_zoom * 100, m_zoom < 0.1 ? 1 : 0);
    m_readout->layoutIn(rect());
    m_busy->layoutIn(rect(), CanvasStatusReadout::Corner::TopLeft);
    setAccessibleName(tr("Export comparison preview"));
    setProperty("zoom", m_zoom);
}
void ScreenshotSavePreviewCanvas::showReadout() {
    m_readout->showTransient();
}
void ScreenshotSavePreviewCanvas::setBusy(bool busy) {
    m_rendering = busy;
    scheduleDetails();
    if (busy)
        m_busy->showPersistent();
    else
        m_busy->dismiss();
}
void ScreenshotSavePreviewCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QBrush checkerboard(m_checkerboard);
    const auto drawBacking = [&](QRectF area, QRectF paper = {}) {
        painter.save();
        // Preserve the same crisp checker pixels when restoring detail at any device scale.
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.fillRect(area, checkerboard);
        if (!paper.isEmpty())
            painter.fillRect(area.intersected(paper), Qt::white);
        painter.restore();
    };
    drawBacking(rect());
    if (m_sourcePixels.isEmpty() || m_original.isNull())
        return;
    const QRectF bounds = imageBounds();
    const auto drawDetail = [&](const Detail& detail, QSize pixels, QRectF paper = {}) {
        if (detail.image.isNull() || pixels.isEmpty())
            return;
        const QRectF detailBounds(bounds.x() + detail.region.x() * bounds.width() / pixels.width(),
                                  bounds.y() +
                                      detail.region.y() * bounds.height() / pixels.height(),
                                  detail.region.width() * bounds.width() / pixels.width(),
                                  detail.region.height() * bounds.height() / pixels.height());
        // Detail replaces the preview, so restore its backing before compositing alpha once.
        drawBacking(detailBounds, paper);
        painter.drawImage(detailBounds, detail.image);
    };
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const double split = m_split * width();
    painter.save();
    painter.setClipRect(QRectF(0, 0, split, height()));
    painter.drawImage(bounds, m_original);
    drawDetail(m_sourceDetail, m_sourcePixels);
    painter.restore();
    painter.save();
    painter.setClipRect(QRectF(split, 0, width() - split, height()));
    QRectF pageBounds;
    if (!m_pdfLayout.pagePoints.isEmpty() && !m_pdfLayout.imagePoints.isEmpty()) {
        // Map paper into the shared image bounds; paper never owns the comparison viewport.
        const double scaleX = bounds.width() / m_pdfLayout.imagePoints.width();
        const double scaleY = bounds.height() / m_pdfLayout.imagePoints.height();
        pageBounds = QRectF(bounds.topLeft() - QPointF(m_pdfLayout.imagePoints.x() * scaleX,
                                                       m_pdfLayout.imagePoints.y() * scaleY),
                            QSizeF(m_pdfLayout.pagePoints.width() * scaleX,
                                   m_pdfLayout.pagePoints.height() * scaleY));
        painter.fillRect(pageBounds, Qt::white);
    }
    painter.drawImage(bounds, m_output.isNull() ? m_original : m_output);
    drawDetail(m_outputDetail, m_outputPixels, pageBounds);
    painter.restore();

    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(7, 9, 12, 158));
    painter.drawRect(QRectF(split - kSplitTrackWidth / 2.0, 0, kSplitTrackWidth, height()));

    const QPointF thumbCenter(split, height() / 2.0);
    painter.setBrush(QColor(246, 248, 251, 66));
    painter.drawEllipse(thumbCenter, kSplitThumbRadius + kSplitThumbOutlineWidth,
                        kSplitThumbRadius + kSplitThumbOutlineWidth);
    QColor thumbColor(Qt::black);
    if (m_splitDragging) {
        thumbColor = theme.scheme == adqt::theme::ThemeScheme::Dark ? QColor(10, 12, 16, 252)
                                                                    : QColor(12, 15, 20, 250);
    } else if (m_splitHovered) {
        thumbColor = theme.scheme == adqt::theme::ThemeScheme::Dark ? QColor(31, 35, 42, 252)
                                                                    : QColor(32, 36, 43, 250);
    }
    painter.setBrush(thumbColor);
    painter.drawEllipse(thumbCenter, kSplitThumbRadius, kSplitThumbRadius);

    painter.setBrush(QColor(246, 248, 251, 242));
    painter.drawPolygon(QPolygonF{
        QPointF(split - kSplitTrackWidth / 2.0, thumbCenter.y() - kSplitArrowHalfHeight),
        QPointF(split - kSplitTrackWidth / 2.0 - kSplitArrowWidth, thumbCenter.y()),
        QPointF(split - kSplitTrackWidth / 2.0, thumbCenter.y() + kSplitArrowHalfHeight),
    });
    painter.setBrush(theme.colorPrimary);
    painter.drawPolygon(QPolygonF{
        QPointF(split + kSplitTrackWidth / 2.0, thumbCenter.y() - kSplitArrowHalfHeight),
        QPointF(split + kSplitTrackWidth / 2.0 + kSplitArrowWidth, thumbCenter.y()),
        QPointF(split + kSplitTrackWidth / 2.0, thumbCenter.y() + kSplitArrowHalfHeight),
    });
}
void ScreenshotSavePreviewCanvas::resizeEvent(QResizeEvent*) {
    if (m_fit)
        fitImage();
    updateReadout();
    scheduleDetails();
}
void ScreenshotSavePreviewCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    setFocus();
    m_last = event->position();
    m_dragging = true;
    m_splitDragging = splitHandleContains(m_last);
    m_splitHovered = m_splitDragging;
    setCursor(m_splitDragging ? Qt::SplitHCursor : Qt::ClosedHandCursor);
    update();
}
void ScreenshotSavePreviewCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) {
        const bool hovered = splitHandleContains(event->position());
        if (hovered != m_splitHovered) {
            m_splitHovered = hovered;
            update();
        }
        setCursor(hovered ? Qt::SplitHCursor : Qt::OpenHandCursor);
        return;
    }
    if (m_splitDragging)
        setSplitRatio(event->position().x() / std::max(1, width()));
    else {
        m_fit = false;
        m_pan += event->position() - m_last;
        scheduleDetails();
        update();
    }
    m_last = event->position();
}
void ScreenshotSavePreviewCanvas::mouseReleaseEvent(QMouseEvent* event) {
    m_dragging = false;
    m_splitDragging = false;
    m_splitHovered = splitHandleContains(event->position());
    setCursor(m_splitHovered ? Qt::SplitHCursor : Qt::OpenHandCursor);
    update();
}
void ScreenshotSavePreviewCanvas::leaveEvent(QEvent* event) {
    if (!m_splitDragging && m_splitHovered) {
        m_splitHovered = false;
        update();
    }
    QWidget::leaveEvent(event);
}
void ScreenshotSavePreviewCanvas::mouseDoubleClickEvent(QMouseEvent*) {
    fitImage(true);
}
void ScreenshotSavePreviewCanvas::zoomAt(double value, QPointF position) {
    const double zoom = std::clamp(value, 0.001, 8.0);
    if (m_sourcePixels.isEmpty() || qFuzzyCompare(zoom, m_zoom))
        return;
    const QPointF offset = position - QRectF(rect()).center();
    m_pan = offset - (offset - m_pan) * (zoom / m_zoom);
    m_zoom = zoom;
    m_fit = false;
    updateReadout();
    showReadout();
    scheduleDetails();
    update();
}
bool ScreenshotSavePreviewCanvas::splitHandleContains(QPointF position) const {
    const QPointF delta = position - QPointF(width() * m_split, height() / 2.0);
    return std::abs(delta.x()) <= kSplitHitHalfWidth ||
           delta.x() * delta.x() + delta.y() * delta.y() <=
               kSplitThumbHitRadius * kSplitThumbHitRadius;
}
void ScreenshotSavePreviewCanvas::wheelEvent(QWheelEvent* event) {
    const double steps = snow_shot::presentation::usesPreciseWheelDelta(*event)
                             ? event->pixelDelta().y() / 100.0
                             : event->angleDelta().y() / 120.0;
    if (steps != 0)
        zoomAt(m_zoom * std::pow(1.15, steps), event->position());
    event->accept();
}
void ScreenshotSavePreviewCanvas::keyPressEvent(QKeyEvent* event) {
    if (snow_shot::shortcuts::commandKey(*event) == Qt::Key_Left)
        setSplitRatio(m_split - 0.02);
    else if (snow_shot::shortcuts::commandKey(*event) == Qt::Key_Right)
        setSplitRatio(m_split + 0.02);
    else if (snow_shot::shortcuts::commandKey(*event) == Qt::Key_Home)
        fitImage(true);
    else if (snow_shot::shortcuts::commandKey(*event) == Qt::Key_Plus ||
             snow_shot::shortcuts::commandKey(*event) == Qt::Key_Equal)
        zoomAt(m_zoom * 1.15, QRectF(rect()).center());
    else if (snow_shot::shortcuts::commandKey(*event) == Qt::Key_Minus)
        zoomAt(m_zoom / 1.15, QRectF(rect()).center());
    else
        QWidget::keyPressEvent(event);
}
void ScreenshotSavePreviewCanvas::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        setAccessibleName(tr("Export comparison preview"));
    QWidget::changeEvent(event);
}
