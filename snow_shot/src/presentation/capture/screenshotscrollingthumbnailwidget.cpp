#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"
#include "screenshotscrollingperfinstrumentation.h"

#include "widgets/scroll_area.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <QtMath>

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

    createScrollBar();
    updateWidgetMetrics();
}

void ScreenshotScrollingThumbnailWidget::createScrollBar() {
    delete m_scrollBar;
    // AdScrollBar fixes the cross-axis size in its constructor; changing the base orientation
    // leaves those constraints attached to the old axis.
    m_scrollBar =
        new adqt::widgets::AdScrollBar(horizontal(m_mode) ? Qt::Horizontal : Qt::Vertical, this);
    m_scrollBar->setFocusPolicy(Qt::NoFocus);
    m_scrollBar->setSingleStep(24);
    m_scrollBar->setPageStep(120);
    m_scrollBar->hide();
    m_scrollBar->installEventFilter(this);
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this]() {
        updateHover();
        update();
    });
}

void ScreenshotScrollingThumbnailWidget::releaseNativeSurface() {
    hide();
    destroy(true, true);
}

void ScreenshotScrollingThumbnailWidget::reset() {
    clearHover();
    cancelDrag();
    m_previewTiles.clear();
    m_previewExtent = 0;
    m_tileDirection = TileDirection::None;
    m_sourceSize = {};
    if (!m_captureViewportSizeConfigured) {
        m_captureViewportSize = {};
    }
    m_highlightedRows = {};
    m_captureImageExtent = 0;
    m_trim->top = 0;
    m_trim->bottom = 0;
    if (m_scrollBar != nullptr) {
        m_scrollBar->setRange(0, 0);
        m_scrollBar->setValue(0);
        m_scrollBar->hide();
    }
    updateWidgetMetrics();
    update();
}

void ScreenshotScrollingThumbnailWidget::cancelDrag() {
    if (m_dragHandle != DragHandle::None) {
        m_dragHandle = DragHandle::None;
        if (QWidget::mouseGrabber() == this) {
            releaseMouse();
        }
    }
    unsetCursor();
}

bool ScreenshotScrollingThumbnailWidget::event(QEvent* event) {
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::UngrabMouse) {
        cancelDrag();
        clearHover();
    } else if (event->type() == QEvent::Move) {
        clearHover();
    }
    return QWidget::event(event);
}

bool ScreenshotScrollingThumbnailWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_scrollBar && event->type() == QEvent::Enter &&
        m_dragHandle == DragHandle::None) {
        clearHover();
    }
    return QWidget::eventFilter(watched, event);
}

void ScreenshotScrollingThumbnailWidget::setRecognitionMode(
    ScreenshotScrollingRecognitionMode mode) {
    if (m_mode == mode) {
        return;
    }
    reset();
    m_mode = mode;
    createScrollBar();
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

void ScreenshotScrollingThumbnailWidget::setCaptureViewportSize(const QSize& size) {
    const QSize viewport = size.isEmpty() ? QSize() : size;
    m_captureViewportSizeConfigured = !viewport.isEmpty();
    if (m_captureViewportSize == viewport) {
        return;
    }
    clearHover();
    m_captureViewportSize = viewport;
    if (!viewport.isEmpty()) {
        m_captureImageExtent = horizontal(m_mode) ? viewport.width() : viewport.height();
    }
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
    const int oldTrimEnd = m_trim->bottom;
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
    if (m_captureViewportSize.isEmpty()) {
        m_captureViewportSize = sourceSize;
    }
    if (change == ScreenshotScrollingStitchChange::Initial || m_captureImageExtent <= 0) {
        m_captureImageExtent =
            horizontal(m_mode) ? m_captureViewportSize.width() : m_captureViewportSize.height();
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
        m_trim->top = std::clamp(m_trim->top, 0, std::max(0, currentExtent - 1));
        m_trim->bottom = currentExtent;
    } else if (canPreserveTrim && prepend) {
        m_trim->top = 0;
        m_trim->bottom = std::clamp(oldTrimEnd + std::max(0, addedRows), 1, currentExtent);
    } else {
        m_trim->top = 0;
        m_trim->bottom = currentExtent;
    }
    if (m_trim->bottom <= m_trim->top) {
        m_trim->top = 0;
        m_trim->bottom = currentExtent;
    }

    const QSignalBlocker blocker(m_scrollBar);
    updateWidgetMetrics(false);
    if (m_scrollBar != nullptr) {
        if (append) {
            m_scrollBar->setValue(m_scrollBar->maximum());
        } else if (prepend || change == ScreenshotScrollingStitchChange::Initial) {
            m_scrollBar->setValue(0);
        }
    }
    updateHover();
    update();
}

bool ScreenshotScrollingThumbnailWidget::hasPreview() const {
    return m_previewExtent > 0 && !m_previewTiles.empty();
}

void ScreenshotScrollingThumbnailWidget::replacePreview(const QImage& image) {
    SNOW_SCROLL_DETAIL_SCOPE(ThumbnailTiles);
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
        m_previewTiles.push_back({std::move(tile), 0, span, start});
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
            tile.firstPosition += remaining;
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
    SNOW_SCROLL_DETAIL_SCOPE(ThumbnailTiles);
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
            const qint64 firstPosition =
                m_previewTiles.empty()
                    ? 0
                    : m_previewTiles.back().firstPosition + m_previewTiles.back().spanCount;
            m_previewTiles.push_back({std::move(tile), 0, 0, firstPosition});
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
    SNOW_SCROLL_DETAIL_SCOPE(ThumbnailTiles);
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
            const qint64 firstPosition =
                m_previewTiles.empty() ? 0 : m_previewTiles.front().firstPosition;
            m_previewTiles.push_front({std::move(tile), kPreviewTileSpan, 0, firstPosition});
        }
        PreviewTile& tile = m_previewTiles.front();
        const int span = std::min(tile.firstSpan, remaining);
        const int sourceStart = remaining - span;
        const int destinationStart = tile.firstSpan - span;
        copyPreviewSpan(normalized, sourceStart, tile.image, destinationStart, span, m_mode);
        tile.firstSpan = destinationStart;
        tile.spanCount += span;
        tile.firstPosition -= span;
        remaining -= span;
        m_previewExtent += span;
    }
}

void ScreenshotScrollingThumbnailWidget::drawPreviewTiles(QPainter& painter,
                                                          const QRectF& imageTarget,
                                                          const QRectF& visibleRect) const {
    SNOW_SCROLL_DETAIL_SCOPE(ThumbnailTilePaint);
    if (visibleRect.isEmpty()) {
        return;
    }
    const qreal tileScale = (horizontal(m_mode) ? imageTarget.width() : imageTarget.height()) /
                            static_cast<qreal>(m_previewExtent);
    const qint64 firstPosition = m_previewTiles.front().firstPosition;
    const qreal targetStart = horizontal(m_mode) ? imageTarget.left() : imageTarget.top();
    const qreal visibleStart = horizontal(m_mode) ? visibleRect.left() : visibleRect.top();
    const qreal visibleEnd = horizontal(m_mode) ? visibleRect.right() : visibleRect.bottom();
    const qreal firstVisibleSpan =
        static_cast<qreal>(firstPosition) + (visibleStart - targetStart) / tileScale;
    const qreal lastVisibleSpan =
        static_cast<qreal>(firstPosition) + (visibleEnd - targetStart) / tileScale;
    auto iterator = std::lower_bound(m_previewTiles.begin(), m_previewTiles.end(), firstVisibleSpan,
                                     [](const PreviewTile& tile, qreal position) {
                                         return static_cast<qreal>(tile.firstPosition +
                                                                   tile.spanCount) <= position;
                                     });
    for (; iterator != m_previewTiles.end() &&
           static_cast<qreal>(iterator->firstPosition) < lastVisibleSpan;
         ++iterator) {
        const PreviewTile& tile = *iterator;
        const qreal previewStart = static_cast<qreal>(tile.firstPosition - firstPosition);
        const QRectF target =
            horizontal(m_mode)
                ? QRectF(imageTarget.left() + previewStart * tileScale, imageTarget.top(),
                         tile.spanCount * tileScale, imageTarget.height())
                : QRectF(imageTarget.left(), imageTarget.top() + previewStart * tileScale,
                         imageTarget.width(), tile.spanCount * tileScale);
        if (target.intersects(visibleRect)) {
            const QRectF source = horizontal(m_mode)
                                      ? QRectF(tile.firstSpan, 0, tile.spanCount, kThumbnailExtent)
                                      : QRectF(0, tile.firstSpan, kThumbnailExtent, tile.spanCount);
            painter.drawImage(target, tile.image, source);
        }
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

QRectF ScreenshotScrollingThumbnailWidget::hoverPreviewRectForTesting() const {
    return m_hoverPreviewRect;
}

QRect ScreenshotScrollingThumbnailWidget::hoverSourceRectForTesting() const {
    return m_hoverSourceRect;
}
#endif

int ScreenshotScrollingThumbnailWidget::trimTop() const {
    return m_trim->top;
}

int ScreenshotScrollingThumbnailWidget::trimBottom() const {
    return m_trim->bottom;
}

QRect ScreenshotScrollingThumbnailWidget::previewRect() const {
    return rect();
}

qreal ScreenshotScrollingThumbnailWidget::imageScale() const {
    const int sourceCross = horizontal(m_mode) ? m_sourceSize.height() : m_sourceSize.width();
    if (!hasPreview() || sourceCross <= 0) {
        return 1.0;
    }
    const qreal crossScale = static_cast<qreal>(kThumbnailExtent) / sourceCross;
    const int captureExtent =
        horizontal(m_mode) ? m_captureViewportSize.width() : m_captureViewportSize.height();
    return captureExtent > 0
               ? std::min(crossScale, static_cast<qreal>(m_maximumPreviewExtent) / captureExtent)
               : crossScale;
}

QRectF ScreenshotScrollingThumbnailWidget::imageTargetRect() const {
    const qreal scale = imageScale();
    const qreal viewportExtent = horizontal(m_mode) ? width() : height();
    const qreal maximumOffset = std::max(0.0, sourceExtent() * scale - viewportExtent);
    const qreal offset = m_scrollBar != nullptr
                             ? std::min(static_cast<qreal>(m_scrollBar->value()), maximumOffset)
                             : 0.0;
    const QPointF origin = horizontal(m_mode) ? QPointF(-offset, 0) : QPointF(0, -offset);
    return QRectF(origin, QSizeF(m_sourceSize) * scale);
}

int ScreenshotScrollingThumbnailWidget::scaledImageExtent() const {
    return hasPreview() ? qCeil(sourceExtent() * imageScale()) : 0;
}

int ScreenshotScrollingThumbnailWidget::sourceExtent() const {
    return horizontal(m_mode) ? m_sourceSize.width() : m_sourceSize.height();
}

int ScreenshotScrollingThumbnailWidget::previewPosition(const QPointF& position) const {
    return qRound(horizontal(m_mode) ? position.x() : position.y());
}

int ScreenshotScrollingThumbnailWidget::handlePosition(int sourcePosition) const {
    const QRectF target = imageTargetRect();
    const qreal origin = horizontal(m_mode) ? target.left() : target.top();
    const qreal position = origin + static_cast<qreal>(sourcePosition) * imageScale();
    if (hasPreview() && sourcePosition == sourceExtent()) {
        // Match the rounded-up widget extent before selecting the last pixel of the image.
        return qCeil(position) - 1;
    }
    return qRound(position);
}

int ScreenshotScrollingThumbnailWidget::sourcePositionForPreviewPosition(int position) const {
    if (!hasPreview()) {
        return 0;
    }
    const qreal scale = imageScale();
    if (scale <= 0.0) {
        return 0;
    }
    const QRectF target = imageTargetRect();
    const qreal start = horizontal(m_mode) ? target.left() : target.top();
    return std::clamp(qRound((position - start) / scale), 0, sourceExtent());
}

bool ScreenshotScrollingThumbnailWidget::isTrimHandleAtPosition(int position) const {
    return hasPreview() &&
           (std::abs(position - handlePosition(m_trim->top)) <= kHandleHitRadius ||
            std::abs(position - handlePosition(m_trim->bottom)) <= kHandleHitRadius);
}

void ScreenshotScrollingThumbnailWidget::updateWidgetMetrics(bool refreshHover) {
    SNOW_SCROLL_DETAIL_SCOPE(ThumbnailMetrics);
    const QScopedValueRollback<bool> metricsGuard(m_updatingMetrics, true);
    const int extent = std::max(1, std::min(m_maximumPreviewExtent, scaledImageExtent()));
    const int sourceCross = horizontal(m_mode) ? m_sourceSize.height() : m_sourceSize.width();
    const int crossExtent =
        hasPreview() ? std::max(1, qCeil(sourceCross * imageScale())) : kThumbnailExtent;
    setFixedSize(horizontal(m_mode) ? QSize(extent, crossExtent) : QSize(crossExtent, extent));
    const int maximum = std::max(0, scaledImageExtent() - extent);
    if (m_scrollBar != nullptr) {
        const QSignalBlocker blocker(m_scrollBar);
        m_scrollBar->setRange(0, maximum);
        m_scrollBar->setPageStep(extent);
        m_scrollBar->setVisible(maximum > 0);
    }
    updateScrollBarGeometry();
    if (refreshHover) {
        updateHover();
    }
    update();
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
    const qreal scale = imageScale();
    const QRectF imageTarget = imageTargetRect();
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

    if (m_trim->top > 0) {
        const QRectF mask = horizontal(m_mode) ? QRectF(imageTarget.left(), imageTarget.top(),
                                                        m_trim->top * scale, imageTarget.height())
                                               : QRectF(imageTarget.left(), imageTarget.top(),
                                                        imageTarget.width(), m_trim->top * scale);
        painter.fillRect(mask, QColor(0, 0, 0, 178));
    }
    if (m_trim->bottom < sourceExtent()) {
        const QRectF mask =
            horizontal(m_mode)
                ? QRectF(imageTarget.left() + m_trim->bottom * scale, imageTarget.top(),
                         (sourceExtent() - m_trim->bottom) * scale, imageTarget.height())
                : QRectF(imageTarget.left(), imageTarget.top() + m_trim->bottom * scale,
                         imageTarget.width(), (sourceExtent() - m_trim->bottom) * scale);
        painter.fillRect(mask, QColor(0, 0, 0, 178));
    }
    if (!m_hoverPreviewRect.isEmpty()) {
        painter.fillRect(m_hoverPreviewRect, QColor(22, 119, 255, 64));
    }
    drawTrimHandle(painter, handlePosition(m_trim->top), true);
    drawTrimHandle(painter, handlePosition(m_trim->bottom), false);
    painter.restore();
}

void ScreenshotScrollingThumbnailWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateScrollBarGeometry();
    if (!m_updatingMetrics) {
        updateHover();
    }
}

void ScreenshotScrollingThumbnailWidget::leaveEvent(QEvent* event) {
    if (m_dragHandle == DragHandle::None) {
        clearHover();
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
    const int headDistance = std::abs(position - handlePosition(m_trim->top));
    const int tailDistance = std::abs(position - handlePosition(m_trim->bottom));
    if (isTrimHandleAtPosition(position)) {
        m_dragHandle = headDistance <= tailDistance ? DragHandle::Head : DragHandle::Tail;
        m_hoverPosition = event->position();
        setCursor(horizontal(m_mode) ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        grabMouse();
        updateHover();
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
    if (m_dragHandle != DragHandle::None && !event->buttons().testFlag(Qt::LeftButton)) {
        cancelDrag();
    }
    updateCursorForPosition(position);
    if (m_dragHandle == DragHandle::None) {
        if (event->buttons() == Qt::NoButton) {
            m_hoverPosition = event->position();
            updateHover();
        } else {
            clearHover();
        }
        QWidget::mouseMoveEvent(event);
        return;
    }
    m_hoverPosition = event->position();
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
        m_hoverPosition = event->position();
        updateHover();
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

void ScreenshotScrollingThumbnailWidget::clearHover() {
    m_hoverPosition.reset();
    setHoverRect({}, {});
}

void ScreenshotScrollingThumbnailWidget::setHoverRect(const QRectF& preview, const QRect& source,
                                                      bool cropping) {
    if (m_hoverPreviewRect != preview) {
        const QRectF dirty = m_hoverPreviewRect.united(preview).adjusted(-1.0, -1.0, 1.0, 1.0);
        m_hoverPreviewRect = preview;
        update(dirty.toAlignedRect());
    }
    if (m_hoverSourceRect != source || m_cropPreviewActive != cropping) {
        m_hoverSourceRect = source;
        m_cropPreviewActive = cropping;
        emit hoverSourceRectChanged(source, cropping);
    }
}

void ScreenshotScrollingThumbnailWidget::updateHover() {
    const bool cropping = m_dragHandle != DragHandle::None;
    if ((!m_hoverPosition && !cropping) || !hasPreview() || m_captureViewportSize.isEmpty() ||
        m_sourceSize.isEmpty()) {
        setHoverRect({}, {});
        return;
    }
    const QRectF imageTarget = imageTargetRect();
    const qreal scale = imageScale();
    if (cropping) {
        const int boundary = m_dragHandle == DragHandle::Head ? m_trim->top : m_trim->bottom;
        const int captureExtent =
            horizontal(m_mode) ? m_captureViewportSize.width() : m_captureViewportSize.height();
        const int start = boundary - captureExtent / 2;
        const QPoint sourceOrigin = horizontal(m_mode) ? QPoint(start, 0) : QPoint(0, start);
        const QRect source(sourceOrigin, m_captureViewportSize);
        setHoverRect(QRectF(imageTarget.topLeft() + QPointF(sourceOrigin) * scale,
                            QSizeF(m_captureViewportSize) * scale),
                     source, true);
        return;
    }
    const QPointF position = *m_hoverPosition;
    const QRectF visible = imageTarget.intersected(QRectF(previewRect()));
    if (!visible.contains(position) || (m_scrollBar != nullptr && m_scrollBar->isVisible() &&
                                        QRectF(m_scrollBar->geometry()).contains(position))) {
        setHoverRect({}, {});
        return;
    }

    const QSize sourceSize(std::min(m_captureViewportSize.width(), m_sourceSize.width()),
                           std::min(m_captureViewportSize.height(), m_sourceSize.height()));
    const QSizeF previewSize = QSizeF(sourceSize) * scale;
    // Clamp in the visible viewport first: a scrolled thumbnail must not select hidden rows.
    const qreal left = std::clamp(position.x() - previewSize.width() / 2.0, visible.left(),
                                  std::max(visible.left(), visible.right() - previewSize.width()));
    const qreal top = std::clamp(position.y() - previewSize.height() / 2.0, visible.top(),
                                 std::max(visible.top(), visible.bottom() - previewSize.height()));
    const QPoint sourceOrigin(std::clamp(qRound((left - imageTarget.left()) / scale), 0,
                                         m_sourceSize.width() - sourceSize.width()),
                              std::clamp(qRound((top - imageTarget.top()) / scale), 0,
                                         m_sourceSize.height() - sourceSize.height()));
    setHoverRect(QRectF(QPointF(left, top), previewSize), QRect(sourceOrigin, sourceSize));
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
        m_trim->top = std::clamp(sourcePosition, 0, std::max(0, m_trim->bottom - 1));
    } else {
        m_trim->bottom =
            std::clamp(sourcePosition, std::min(sourceExtent(), m_trim->top + 1), sourceExtent());
    }
    updateHover();
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

void ScreenshotScrollingThumbnailWidget::setTrimModel(
    std::shared_ptr<ScreenshotScrollingTrimRange> trim) {
    m_trim = std::move(trim);
    update();
}
