#include "snow_shot/presentation/screenshotsavepreviewcanvas.h"
#include "theme/theme_manager.h"

#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kZoomReadoutInset = 8;
constexpr int kZoomReadoutDurationMs = 1000;
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
    m_readout = new QLabel(this);
    m_readout->setObjectName(QStringLiteral("savePreviewZoom"));
    m_readout->setAlignment(Qt::AlignCenter);
    m_readout->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_readout->setStyleSheet(QStringLiteral("QLabel#savePreviewZoom { "
                                            "color: white; background-color: rgba(0, 0, 0, 150); "
                                            "padding: 3px 6px; border-radius: 4px; }"));
    m_readout->hide();
    m_readoutTimer = new QTimer(this);
    m_readoutTimer->setObjectName(QStringLiteral("savePreviewZoomTimer"));
    m_readoutTimer->setSingleShot(true);
    m_readoutTimer->setInterval(kZoomReadoutDurationMs);
    connect(m_readoutTimer, &QTimer::timeout, m_readout, &QWidget::hide);
    m_busy = new QLabel(this);
    m_busy->setObjectName(QStringLiteral("savePreviewStatus"));
    m_busy->setStyleSheet(QStringLiteral(
        "color: white; background: rgba(0,0,0,160); padding: 4px 8px; border-radius: 4px;"));
    m_busy->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_busy->hide();
    updateReadout();
}
void ScreenshotSavePreviewCanvas::setSource(QImage image, QSize pixels) {
    m_original = std::move(image);
    m_output = {};
    m_sourcePixels = pixels;
    m_readoutTimer->stop();
    m_readout->hide();
    fitImage();
}
void ScreenshotSavePreviewCanvas::setPdfLayout(screenshot_pdf::Layout layout) {
    if (layout.pagePoints == m_pdfLayout.pagePoints &&
        layout.imagePoints == m_pdfLayout.imagePoints)
        return;
    m_pdfLayout = layout;
    update();
}
void ScreenshotSavePreviewCanvas::setOutput(QImage image) {
    m_output = std::move(image);
    update();
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
    update();
}
void ScreenshotSavePreviewCanvas::updateReadout() {
    m_readout->setText(tr("Scale: %1%").arg(m_zoom * 100, 0, 'f', m_zoom < 0.1 ? 1 : 0));
    m_readout->adjustSize();
    m_readout->move(kZoomReadoutInset,
                    std::max(0, height() - m_readout->height() - kZoomReadoutInset));
    m_readout->setAccessibleName(tr("Current zoom"));
    m_busy->setText(tr("Rendering preview"));
    m_busy->adjustSize();
    m_busy->move(12, 12);
    setAccessibleName(tr("Export comparison preview"));
    setProperty("zoom", m_zoom);
}
void ScreenshotSavePreviewCanvas::showReadout() {
    m_readout->show();
    m_readout->raise();
    m_readoutTimer->start();
}
void ScreenshotSavePreviewCanvas::setBusy(bool busy) {
    m_busy->setVisible(busy);
}
void ScreenshotSavePreviewCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QBrush(m_checkerboard));
    if (m_sourcePixels.isEmpty() || m_original.isNull())
        return;
    const QSizeF dimensions = QSizeF(m_sourcePixels) * m_zoom;
    const QPointF center = QRectF(rect()).center() + m_pan;
    const QRectF bounds(center.x() - dimensions.width() / 2, center.y() - dimensions.height() / 2,
                        dimensions.width(), dimensions.height());
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const double split = m_split * width();
    painter.save();
    painter.setClipRect(QRectF(0, 0, split, height()));
    painter.drawImage(bounds, m_original);
    painter.restore();
    painter.save();
    painter.setClipRect(QRectF(split, 0, width() - split, height()));
    if (!m_pdfLayout.pagePoints.isEmpty() && !m_pdfLayout.imagePoints.isEmpty()) {
        // Map paper into the shared image bounds; paper never owns the comparison viewport.
        const double scaleX = bounds.width() / m_pdfLayout.imagePoints.width();
        const double scaleY = bounds.height() / m_pdfLayout.imagePoints.height();
        const QRectF pageBounds(bounds.topLeft() - QPointF(m_pdfLayout.imagePoints.x() * scaleX,
                                                           m_pdfLayout.imagePoints.y() * scaleY),
                                QSizeF(m_pdfLayout.pagePoints.width() * scaleX,
                                       m_pdfLayout.pagePoints.height() * scaleY));
        painter.fillRect(pageBounds, Qt::white);
    }
    painter.drawImage(bounds, m_output.isNull() ? m_original : m_output);
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
    update();
}
bool ScreenshotSavePreviewCanvas::splitHandleContains(QPointF position) const {
    const QPointF delta = position - QPointF(width() * m_split, height() / 2.0);
    return std::abs(delta.x()) <= kSplitHitHalfWidth ||
           delta.x() * delta.x() + delta.y() * delta.y() <=
               kSplitThumbHitRadius * kSplitThumbHitRadius;
}
void ScreenshotSavePreviewCanvas::wheelEvent(QWheelEvent* event) {
    const double steps = !event->pixelDelta().isNull() ? event->pixelDelta().y() / 100.0
                                                       : event->angleDelta().y() / 120.0;
    if (steps != 0)
        zoomAt(m_zoom * std::pow(1.15, steps), event->position());
    event->accept();
}
void ScreenshotSavePreviewCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Left)
        setSplitRatio(m_split - 0.02);
    else if (event->key() == Qt::Key_Right)
        setSplitRatio(m_split + 0.02);
    else if (event->key() == Qt::Key_Home)
        fitImage(true);
    else if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)
        zoomAt(m_zoom * 1.15, QRectF(rect()).center());
    else if (event->key() == Qt::Key_Minus)
        zoomAt(m_zoom / 1.15, QRectF(rect()).center());
    else
        QWidget::keyPressEvent(event);
}
void ScreenshotSavePreviewCanvas::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        updateReadout();
    QWidget::changeEvent(event);
}
