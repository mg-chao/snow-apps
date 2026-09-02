#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"

#include "widgets/scroll_area.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int kThumbnailExtent = 128;
constexpr int kPreviewTileSpan = 256;
constexpr int kHandleHitRadius = 9;
constexpr int kHandleThickness = 4;
constexpr int kHandleStrokeWidth = 2;
constexpr int kHandleTabExtent = 30;
constexpr int kAutoScrollMargin = 18;
constexpr int kAutoScrollStep = 20;

bool horizontal(ScreenshotScrollingRecognitionMode mode) {
    return mode == ScreenshotScrollingRecognitionMode::Horizontal;
}

int imageExtent(const QImage& image, ScreenshotScrollingRecognitionMode mode) {
    return horizontal(mode) ? image.width() : image.height();
}

int imageCrossExtent(const QImage& image, ScreenshotScrollingRecognitionMode mode) {
    return horizontal(mode) ? image.height() : image.width();
}

void copyPreviewSpan(const QImage& source, int sourceStart, QImage& destination,
                     int destinationStart, int span, ScreenshotScrollingRecognitionMode mode) {
    if (!horizontal(mode)) {
        constexpr size_t rowBytes = static_cast<size_t>(kThumbnailExtent) * 4;
        for (int row = 0; row < span; ++row) {
            std::memcpy(destination.scanLine(destinationStart + row),
                        source.constScanLine(sourceStart + row), rowBytes);
        }
        return;
    }

    const size_t bytes = static_cast<size_t>(span) * 4;
    for (int row = 0; row < kThumbnailExtent; ++row) {
        std::memcpy(destination.scanLine(row) + destinationStart * 4,
                    source.constScanLine(row) + sourceStart * 4, bytes);
    }
}
} // namespace

ScreenshotScrollingThumbnailWidget::ScreenshotScrollingThumbnailWidget(QWidget& parent)
    : QWidget(&parent) {
    setObjectName(QStringLiteral("screenshot-scrolling-thumbnail"));
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);

    m_scrollBar = new adqt::widgets::AdScrollBar(Qt::Vertical, this);
    m_scrollBar->setFocusPolicy(Qt::NoFocus);
    m_scrollBar->setSingleStep(24);
    m_scrollBar->setPageStep(120);
    m_scrollBar->hide();
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this]() { update(); });
    updateWidgetMetrics();
}

void ScreenshotScrollingThumbnailWidget::reset() {
    if (m_dragHandle != DragHandle::None) {
        m_dragHandle = DragHandle::None;
        if (QWidget::mouseGrabber() == this) {
            releaseMouse();
        }
    }
    unsetCursor();
    m_previewTiles.clear();
    m_previewExtent = 0;
    m_tileDirection = TileDirection::None;
    m_sourceSize = {};
    m_highlightedRows = {};
    m_captureImageExtent = 0;
    m_trimTop = 0;
    m_trimBottom = 0;
    if (m_scrollBar != nullptr) {
        m_scrollBar->setRange(0, 0);
        m_scrollBar->setValue(0);
        m_scrollBar->hide();
    }
    updateWidgetMetrics();
    update();
}

void ScreenshotScrollingThumbnailWidget::setRecognitionMode(
    ScreenshotScrollingRecognitionMode mode) {
    if (m_mode == mode) {
        return;
    }
    reset();
    m_mode = mode;
    if (m_scrollBar != nullptr) {
        m_scrollBar->setOrientation(horizontal(m_mode) ? Qt::Horizontal : Qt::Vertical);
    }
    updateWidgetMetrics();
}

void ScreenshotScrollingThumbnailWidget::setMaximumPreviewHeight(int height) {
    setMaximumPreviewExtent(height);
}

void ScreenshotScrollingThumbnailWidget::setMaximumPreviewExtent(int extent) {
    const int clamped = std::max(1, extent);
    if (m_maximumPreviewExtent == clamped) {
        return;
    }
    m_maximumPreviewExtent = clamped;
    updateWidgetMetrics();
}

void ScreenshotScrollingThumbnailWidget::setStitchedImage(const QImage& previewImage,
                                                          const QSize& sourceSize,
                                                          ScreenshotScrollingStitchChange change,
                                                          int addedRows, bool replacePreviewImage,
                                                          int replacedPreviewRows) {
    if (sourceSize.isEmpty() ||
        (previewImage.isNull() &&
         (replacePreviewImage || change == ScreenshotScrollingStitchChange::Initial ||
          change == ScreenshotScrollingStitchChange::Replaced))) {
        return;
    }

    const int oldExtent = sourceExtent();
    const int oldCross = horizontal(m_mode) ? m_sourceSize.height() : m_sourceSize.width();
    const int oldTrimEnd = m_trimBottom;
    if (!previewImage.isNull()) {
        if (replacePreviewImage || change == ScreenshotScrollingStitchChange::Initial ||
            change == ScreenshotScrollingStitchChange::Replaced) {
            replacePreview(previewImage);
        } else if (change == ScreenshotScrollingStitchChange::AppendedDown ||
                   change == ScreenshotScrollingStitchChange::AppendedRight) {
            discardPreviewBack(replacedPreviewRows);
            appendPreview(previewImage);
        } else if (change == ScreenshotScrollingStitchChange::PrependedUp ||
                   change == ScreenshotScrollingStitchChange::PrependedLeft) {
            discardPreviewFront(replacedPreviewRows);
            prependPreview(previewImage);
        }
    }
    m_sourceSize = sourceSize;
    const int currentExtent = sourceExtent();
    const int currentCross = horizontal(m_mode) ? sourceSize.height() : sourceSize.width();
    if (change == ScreenshotScrollingStitchChange::Initial || m_captureImageExtent <= 0) {
        m_captureImageExtent = currentExtent;
    }

    const int highlightExtent = std::min(m_captureImageExtent, currentExtent);
    if (highlightExtent <= 0) {
        m_highlightedRows = {};
    } else if (horizontal(m_mode)) {
        const int left = change == ScreenshotScrollingStitchChange::AppendedRight
                             ? currentExtent - highlightExtent
                             : 0;
        m_highlightedRows = QRect(left, 0, highlightExtent, sourceSize.height());
    } else {
        const int top = change == ScreenshotScrollingStitchChange::AppendedDown
                            ? currentExtent - highlightExtent
                            : 0;
        m_highlightedRows = QRect(0, top, sourceSize.width(), highlightExtent);
    }

    const bool append = change == ScreenshotScrollingStitchChange::AppendedDown ||
                        change == ScreenshotScrollingStitchChange::AppendedRight;
    const bool prepend = change == ScreenshotScrollingStitchChange::PrependedUp ||
                         change == ScreenshotScrollingStitchChange::PrependedLeft;
    const bool canPreserveTrim = oldExtent > 0 && oldCross == currentCross;
    if (canPreserveTrim && append) {
        m_trimTop = std::clamp(m_trimTop, 0, std::max(0, currentExtent - 1));
        m_trimBottom = currentExtent;
    } else if (canPreserveTrim && prepend) {
        m_trimTop = 0;
        m_trimBottom = std::clamp(oldTrimEnd + std::max(0, addedRows), 1, currentExtent);
    } else {
        m_trimTop = 0;
        m_trimBottom = currentExtent;
    }
    if (m_trimBottom <= m_trimTop) {
        m_trimTop = 0;
        m_trimBottom = currentExtent;
    }

    updateWidgetMetrics();
    if (m_scrollBar != nullptr) {
        if (append) {
            m_scrollBar->setValue(m_scrollBar->maximum());
        } else if (prepend || change == ScreenshotScrollingStitchChange::Initial) {
            m_scrollBar->setValue(0);
        }
    }
    update();
}

bool ScreenshotScrollingThumbnailWidget::hasPreview() const {
    return m_previewExtent > 0 && !m_previewTiles.empty();
}

void ScreenshotScrollingThumbnailWidget::replacePreview(const QImage& image) {
    m_previewTiles.clear();
    m_previewExtent = 0;
    m_tileDirection = TileDirection::None;
    if (image.isNull() || imageCrossExtent(image, m_mode) != kThumbnailExtent) {
        return;
    }
    const QImage normalized = image.format() == QImage::Format_RGBA8888
                                  ? image
                                  : image.convertToFormat(QImage::Format_RGBA8888);
    if (normalized.isNull()) {
        return;
    }
    const int extent = imageExtent(normalized, m_mode);
    for (int start = 0; start < extent; start += kPreviewTileSpan) {
        const int span = std::min(kPreviewTileSpan, extent - start);
        QImage tile = horizontal(m_mode) ? normalized.copy(start, 0, span, kThumbnailExtent)
                                         : normalized.copy(0, start, kThumbnailExtent, span);
        if (tile.isNull()) {
            m_previewTiles.clear();
            m_previewExtent = 0;
            return;
        }
        m_previewTiles.push_back({std::move(tile), 0, span});
        m_previewExtent += span;
    }
}

void ScreenshotScrollingThumbnailWidget::discardPreviewBack(int span) {
    int remaining = std::clamp(span, 0, m_previewExtent);
    m_previewExtent -= remaining;
    while (remaining > 0 && !m_previewTiles.empty()) {
        PreviewTile& tile = m_previewTiles.back();
        if (remaining >= tile.spanCount) {
            remaining -= tile.spanCount;
            m_previewTiles.pop_back();
        } else {
            tile.spanCount -= remaining;
            remaining = 0;
        }
    }
}

void ScreenshotScrollingThumbnailWidget::discardPreviewFront(int span) {
    int remaining = std::clamp(span, 0, m_previewExtent);
    m_previewExtent -= remaining;
    while (remaining > 0 && !m_previewTiles.empty()) {
        PreviewTile& tile = m_previewTiles.front();
        if (remaining >= tile.spanCount) {
            remaining -= tile.spanCount;
            m_previewTiles.pop_front();
        } else {
            tile.firstSpan += remaining;
            tile.spanCount -= remaining;
            remaining = 0;
        }
    }
}

void ScreenshotScrollingThumbnailWidget::prepareTileDirection(TileDirection direction) {
    if (m_tileDirection == direction) {
        return;
    }
    compactActiveTile();
    m_tileDirection = direction;
}

void ScreenshotScrollingThumbnailWidget::compactActiveTile() {
    if (m_previewTiles.empty() || m_tileDirection == TileDirection::None) {
        return;
    }
    PreviewTile& tile =
        m_tileDirection == TileDirection::Append ? m_previewTiles.back() : m_previewTiles.front();
    if (tile.firstSpan == 0 && tile.spanCount == imageExtent(tile.image, m_mode)) {
        return;
    }
    QImage compacted = horizontal(m_mode)
                           ? tile.image.copy(tile.firstSpan, 0, tile.spanCount, kThumbnailExtent)
                           : tile.image.copy(0, tile.firstSpan, kThumbnailExtent, tile.spanCount);
    if (!compacted.isNull()) {
        tile.image = std::move(compacted);
        tile.firstSpan = 0;
    }
}

void ScreenshotScrollingThumbnailWidget::appendPreview(const QImage& image) {
    if (image.isNull() || imageCrossExtent(image, m_mode) != kThumbnailExtent) {
        return;
    }
    const QImage normalized = image.format() == QImage::Format_RGBA8888
                                  ? image
                                  : image.convertToFormat(QImage::Format_RGBA8888);
    if (normalized.isNull()) {
        return;
    }
    prepareTileDirection(TileDirection::Append);
    int sourceStart = 0;
    const int sourceExtentValue = imageExtent(normalized, m_mode);
    while (sourceStart < sourceExtentValue) {
        if (m_previewTiles.empty() ||
            imageExtent(m_previewTiles.back().image, m_mode) != kPreviewTileSpan ||
            m_previewTiles.back().firstSpan + m_previewTiles.back().spanCount >= kPreviewTileSpan) {
            QImage tile(horizontal(m_mode) ? QSize(kPreviewTileSpan, kThumbnailExtent)
                                           : QSize(kThumbnailExtent, kPreviewTileSpan),
                        QImage::Format_RGBA8888);
            if (tile.isNull()) {
                return;
            }
            m_previewTiles.push_back({std::move(tile), 0, 0});
        }
        PreviewTile& tile = m_previewTiles.back();
        const int destinationStart = tile.firstSpan + tile.spanCount;
        const int span =
            std::min(kPreviewTileSpan - destinationStart, sourceExtentValue - sourceStart);
        copyPreviewSpan(normalized, sourceStart, tile.image, destinationStart, span, m_mode);
        tile.spanCount += span;
        sourceStart += span;
        m_previewExtent += span;
    }
}

void ScreenshotScrollingThumbnailWidget::prependPreview(const QImage& image) {
    if (image.isNull() || imageCrossExtent(image, m_mode) != kThumbnailExtent) {
        return;
    }
    const QImage normalized = image.format() == QImage::Format_RGBA8888
                                  ? image
                                  : image.convertToFormat(QImage::Format_RGBA8888);
    if (normalized.isNull()) {
        return;
    }
    prepareTileDirection(TileDirection::Prepend);
    int remaining = imageExtent(normalized, m_mode);
    while (remaining > 0) {
        if (m_previewTiles.empty() ||
            imageExtent(m_previewTiles.front().image, m_mode) != kPreviewTileSpan ||
            m_previewTiles.front().firstSpan == 0) {
            QImage tile(horizontal(m_mode) ? QSize(kPreviewTileSpan, kThumbnailExtent)
                                           : QSize(kThumbnailExtent, kPreviewTileSpan),
                        QImage::Format_RGBA8888);
            if (tile.isNull()) {
                return;
            }
            m_previewTiles.push_front({std::move(tile), kPreviewTileSpan, 0});
        }
        PreviewTile& tile = m_previewTiles.front();
        const int span = std::min(tile.firstSpan, remaining);
        const int sourceStart = remaining - span;
        const int destinationStart = tile.firstSpan - span;
        copyPreviewSpan(normalized, sourceStart, tile.image, destinationStart, span, m_mode);
        tile.firstSpan = destinationStart;
        tile.spanCount += span;
        remaining -= span;
        m_previewExtent += span;
    }
}

void ScreenshotScrollingThumbnailWidget::drawPreviewTiles(QPainter& painter,
                                                          const QRectF& imageTarget,
                                                          const QRectF& visibleRect) const {
    int previewStart = 0;
    for (const PreviewTile& tile : m_previewTiles) {
        const QRectF target = horizontal(m_mode)
                                  ? QRectF(imageTarget.left() + previewStart, imageTarget.top(),
                                           tile.spanCount, imageTarget.height())
                                  : QRectF(imageTarget.left(), imageTarget.top() + previewStart,
                                           imageTarget.width(), tile.spanCount);
        if (target.intersects(visibleRect)) {
            const QRectF source = horizontal(m_mode)
                                      ? QRectF(tile.firstSpan, 0, tile.spanCount, kThumbnailExtent)
                                      : QRectF(0, tile.firstSpan, kThumbnailExtent, tile.spanCount);
            painter.drawImage(target, tile.image, source);
        }
        previewStart += tile.spanCount;
    }
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
QImage ScreenshotScrollingThumbnailWidget::previewImageForTesting() const {
    if (!hasPreview()) {
        return {};
    }
    QImage output(horizontal(m_mode) ? QSize(m_previewExtent, kThumbnailExtent)
                                     : QSize(kThumbnailExtent, m_previewExtent),
                  QImage::Format_RGBA8888);
    if (output.isNull()) {
        return {};
    }
    int targetStart = 0;
    for (const PreviewTile& tile : m_previewTiles) {
        copyPreviewSpan(tile.image, tile.firstSpan, output, targetStart, tile.spanCount, m_mode);
        targetStart += tile.spanCount;
    }
    return output;
}

qsizetype ScreenshotScrollingThumbnailWidget::previewLogicalBytesForTesting() const {
    return static_cast<qsizetype>(m_previewExtent) * kThumbnailExtent * 4;
}

qsizetype ScreenshotScrollingThumbnailWidget::previewAllocatedBytesForTesting() const {
    qsizetype bytes = 0;
    for (const PreviewTile& tile : m_previewTiles) {
        bytes += tile.image.sizeInBytes();
    }
    return bytes;
}

QRect ScreenshotScrollingThumbnailWidget::highlightedRowsForTesting() const {
    return m_highlightedRows;
}
#endif

int ScreenshotScrollingThumbnailWidget::trimTop() const {
    return m_trimTop;
}

int ScreenshotScrollingThumbnailWidget::trimBottom() const {
    return m_trimBottom;
}

QRect ScreenshotScrollingThumbnailWidget::previewRect() const {
    return rect();
}

qreal ScreenshotScrollingThumbnailWidget::imageScale() const {
    return hasPreview() && sourceExtent() > 0
               ? static_cast<qreal>(m_previewExtent) / static_cast<qreal>(sourceExtent())
               : 1.0;
}

int ScreenshotScrollingThumbnailWidget::scaledImageExtent() const {
    return hasPreview() ? m_previewExtent : 0;
}

int ScreenshotScrollingThumbnailWidget::sourceExtent() const {
    return horizontal(m_mode) ? m_sourceSize.width() : m_sourceSize.height();
}

int ScreenshotScrollingThumbnailWidget::previewPosition(const QPointF& position) const {
    return qRound(horizontal(m_mode) ? position.x() : position.y());
}

int ScreenshotScrollingThumbnailWidget::handlePosition(int sourcePosition) const {
    const int offset = m_scrollBar != nullptr ? m_scrollBar->value() : 0;
    const QRect viewport = previewRect();
    int position = (horizontal(m_mode) ? viewport.left() : viewport.top()) +
                   qRound(static_cast<qreal>(sourcePosition) * imageScale()) - offset;
    if (hasPreview() && sourcePosition == sourceExtent()) {
        --position;
    }
    return position;
}

int ScreenshotScrollingThumbnailWidget::sourcePositionForPreviewPosition(int position) const {
    if (!hasPreview()) {
        return 0;
    }
    const int offset = m_scrollBar != nullptr ? m_scrollBar->value() : 0;
    const qreal scale = imageScale();
    if (scale <= 0.0) {
        return 0;
    }
    const QRect viewport = previewRect();
    const int start = horizontal(m_mode) ? viewport.left() : viewport.top();
    return std::clamp(qRound(static_cast<qreal>(position - start + offset) / scale), 0,
                      sourceExtent());
}

bool ScreenshotScrollingThumbnailWidget::isTrimHandleAtPosition(int position) const {
    return hasPreview() && (std::abs(position - handlePosition(m_trimTop)) <= kHandleHitRadius ||
                            std::abs(position - handlePosition(m_trimBottom)) <= kHandleHitRadius);
}

void ScreenshotScrollingThumbnailWidget::updateWidgetMetrics() {
    const int extent = std::max(1, std::min(m_maximumPreviewExtent, scaledImageExtent()));
    setFixedSize(horizontal(m_mode) ? QSize(extent, kThumbnailExtent)
                                    : QSize(kThumbnailExtent, extent));
    const int maximum = std::max(0, scaledImageExtent() - extent);
    if (m_scrollBar != nullptr) {
        m_scrollBar->setRange(0, maximum);
        m_scrollBar->setPageStep(extent);
        m_scrollBar->setVisible(maximum > 0);
    }
    updateScrollBarGeometry();
}

void ScreenshotScrollingThumbnailWidget::updateScrollBarGeometry() {
    if (m_scrollBar != nullptr) {
        m_scrollBar->setOverlayBounds(rect());
    }
}

void ScreenshotScrollingThumbnailWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (!hasPreview()) {
        return;
    }

    const QRect viewport = previewRect();
    const int offset = m_scrollBar != nullptr ? m_scrollBar->value() : 0;
    const qreal scale = imageScale();
    const QRectF imageTarget = horizontal(m_mode) ? QRectF(viewport.left() - offset, viewport.top(),
                                                           scaledImageExtent(), kThumbnailExtent)
                                                  : QRectF(viewport.left(), viewport.top() - offset,
                                                           kThumbnailExtent, scaledImageExtent());
    painter.save();
    painter.setClipRect(viewport);
    drawPreviewTiles(painter, imageTarget, viewport);
    painter.fillRect(imageTarget, QColor(0, 0, 0, 132));

    if (!m_highlightedRows.isEmpty()) {
        const QRectF highlighted =
            horizontal(m_mode)
                ? QRectF(imageTarget.left() + m_highlightedRows.left() * scale, imageTarget.top(),
                         m_highlightedRows.width() * scale, imageTarget.height())
                : QRectF(imageTarget.left(), imageTarget.top() + m_highlightedRows.top() * scale,
                         imageTarget.width(), m_highlightedRows.height() * scale);
        painter.save();
        painter.setClipRect(highlighted, Qt::IntersectClip);
        drawPreviewTiles(painter, imageTarget, highlighted.intersected(viewport));
        painter.restore();
    }

    if (m_trimTop > 0) {
        const QRectF mask = horizontal(m_mode) ? QRectF(imageTarget.left(), imageTarget.top(),
                                                        m_trimTop * scale, imageTarget.height())
                                               : QRectF(imageTarget.left(), imageTarget.top(),
                                                        imageTarget.width(), m_trimTop * scale);
        painter.fillRect(mask, QColor(0, 0, 0, 178));
    }
    if (m_trimBottom < sourceExtent()) {
        const QRectF mask =
            horizontal(m_mode)
                ? QRectF(imageTarget.left() + m_trimBottom * scale, imageTarget.top(),
                         (sourceExtent() - m_trimBottom) * scale, imageTarget.height())
                : QRectF(imageTarget.left(), imageTarget.top() + m_trimBottom * scale,
                         imageTarget.width(), (sourceExtent() - m_trimBottom) * scale);
        painter.fillRect(mask, QColor(0, 0, 0, 178));
    }
    drawTrimHandle(painter, handlePosition(m_trimTop), true);
    drawTrimHandle(painter, handlePosition(m_trimBottom), false);
    painter.restore();
}

void ScreenshotScrollingThumbnailWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateScrollBarGeometry();
}

void ScreenshotScrollingThumbnailWidget::leaveEvent(QEvent* event) {
    if (m_dragHandle == DragHandle::None) {
        unsetCursor();
    }
    QWidget::leaveEvent(event);
}

void ScreenshotScrollingThumbnailWidget::mousePressEvent(QMouseEvent* event) {
    if (event == nullptr || event->button() != Qt::LeftButton || !hasPreview()) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int position = previewPosition(event->position());
    const int headDistance = std::abs(position - handlePosition(m_trimTop));
    const int tailDistance = std::abs(position - handlePosition(m_trimBottom));
    if (isTrimHandleAtPosition(position)) {
        m_dragHandle = headDistance <= tailDistance ? DragHandle::Head : DragHandle::Tail;
        setCursor(horizontal(m_mode) ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        grabMouse();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ScreenshotScrollingThumbnailWidget::mouseMoveEvent(QMouseEvent* event) {
    if (event == nullptr) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const int position = previewPosition(event->position());
    updateCursorForPosition(position);
    if (m_dragHandle == DragHandle::None) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    updateTrimFromPosition(position);
    event->accept();
}

void ScreenshotScrollingThumbnailWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event != nullptr && event->button() == Qt::LeftButton && m_dragHandle != DragHandle::None) {
        const int position = previewPosition(event->position());
        updateTrimFromPosition(position);
        m_dragHandle = DragHandle::None;
        releaseMouse();
        updateCursorForPosition(position);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ScreenshotScrollingThumbnailWidget::wheelEvent(QWheelEvent* event) {
    if (event != nullptr && m_scrollBar != nullptr && m_scrollBar->isVisible()) {
        int delta = horizontal(m_mode) ? event->pixelDelta().x() : event->pixelDelta().y();
        if (delta == 0) {
            delta = event->pixelDelta().y();
        }
        if (delta == 0) {
            delta = (horizontal(m_mode) && event->angleDelta().x() != 0)
                        ? event->angleDelta().x() / 2
                        : event->angleDelta().y() / 2;
        }
        if (delta != 0) {
            m_scrollBar->setValue(m_scrollBar->value() - delta);
            event->accept();
            return;
        }
    }
    QWidget::wheelEvent(event);
}

void ScreenshotScrollingThumbnailWidget::updateCursorForPosition(int position) {
    if (m_dragHandle != DragHandle::None || isTrimHandleAtPosition(position)) {
        setCursor(horizontal(m_mode) ? Qt::SizeHorCursor : Qt::SizeVerCursor);
    } else {
        unsetCursor();
    }
}

void ScreenshotScrollingThumbnailWidget::updateTrimFromPosition(int position) {
    if (!hasPreview() || m_dragHandle == DragHandle::None) {
        return;
    }
    const QRect viewport = previewRect();
    const int start = horizontal(m_mode) ? viewport.left() : viewport.top();
    const int end = horizontal(m_mode) ? viewport.right() : viewport.bottom();
    if (m_scrollBar != nullptr && m_scrollBar->isVisible()) {
        if (position < start + kAutoScrollMargin) {
            m_scrollBar->setValue(m_scrollBar->value() - kAutoScrollStep);
        } else if (position > end - kAutoScrollMargin) {
            m_scrollBar->setValue(m_scrollBar->value() + kAutoScrollStep);
        }
    }
    const int sourcePosition = sourcePositionForPreviewPosition(position);
    if (m_dragHandle == DragHandle::Head) {
        m_trimTop = std::clamp(sourcePosition, 0, std::max(0, m_trimBottom - 1));
    } else {
        m_trimBottom =
            std::clamp(sourcePosition, std::min(sourceExtent(), m_trimTop + 1), sourceExtent());
    }
    update();
}

void ScreenshotScrollingThumbnailWidget::drawTrimHandle(QPainter& painter, int position,
                                                        bool head) const {
    const QRect viewport = previewRect();
    const int viewportStart = horizontal(m_mode) ? viewport.left() : viewport.top();
    const int viewportEnd = horizontal(m_mode) ? viewport.right() : viewport.bottom();
    if (position < viewportStart - kHandleHitRadius || position > viewportEnd + kHandleHitRadius) {
        return;
    }
    const int visual = std::clamp(position, viewportStart, viewportEnd);
    const QColor color(QStringLiteral("#faad14"));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    if (horizontal(m_mode)) {
        const int lineWidth = std::min(kHandleStrokeWidth, viewport.width());
        const int lineLeft =
            std::clamp(visual - lineWidth / 2, viewport.left(), viewport.right() - lineWidth + 1);
        painter.fillRect(QRect(lineLeft, viewport.top(), lineWidth, viewport.height()), color);
        const int tabWidth = std::min(kHandleThickness, viewport.width());
        const int tabLeft = std::clamp(head ? visual : visual - tabWidth + 1, viewport.left(),
                                       viewport.right() - tabWidth + 1);
        painter.drawRoundedRect(QRectF(tabLeft, viewport.center().y() - kHandleTabExtent / 2,
                                       tabWidth, kHandleTabExtent),
                                2.0, 2.0);
        return;
    }

    const int lineHeight = std::min(kHandleStrokeWidth, viewport.height());
    const int lineTop =
        std::clamp(visual - lineHeight / 2, viewport.top(), viewport.bottom() - lineHeight + 1);
    painter.fillRect(QRect(viewport.left(), lineTop, viewport.width(), lineHeight), color);
    const int tabHeight = std::min(kHandleThickness, viewport.height());
    const int tabTop = std::clamp(head ? visual : visual - tabHeight + 1, viewport.top(),
                                  viewport.bottom() - tabHeight + 1);
    painter.drawRoundedRect(
        QRectF(viewport.center().x() - kHandleTabExtent / 2, tabTop, kHandleTabExtent, tabHeight),
        2.0, 2.0);
}
