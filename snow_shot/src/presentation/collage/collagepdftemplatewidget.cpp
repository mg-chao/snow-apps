// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/collagepdftemplatewidget.h"

#include "snow_shot/presentation/components/canvasactiontoolbar.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_draw_engine_qt/snow_canvas_overlay_style.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snowimageqtcodec.h"
#include "collageresourcebudget_p.h"
#include "collagepreviewrows_p.h"
#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/image.h"
#include "widgets/scroll_area.h"

#include <QApplication>
#include <QCoreApplication>
#include <QColorSpace>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

namespace snow_shot::presentation::components {
namespace {
using adqt::widgets::AdButton;
using adqt::widgets::AdImageListModel;
using adqt::widgets::AdImageViewer;
using adqt::widgets::AdScrollArea;
namespace icons = adqt::icons::antd::outlined;
constexpr auto dragType = "application/x-snow-shot-collage-item";
constexpr int thumbnailWidth = 108;
constexpr int thumbnailHeight = 84;
constexpr int thumbnailStep = thumbnailWidth + 8;

QString templateText(const char* source) {
    return QCoreApplication::translate("CollagePdfTemplateWidget", source);
}

[[maybe_unused]] constexpr std::array translations{
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Template stitching preview"),
    QT_TRANSLATE_NOOP(
        "CollagePdfTemplateWidget",
        "Drag an image to move or swap it; drag its selection handles to scale around its center"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Add images"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Clear all"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Reverse image order"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Rotate clockwise by 90°"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Rotate counterclockwise by 90°"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Flip horizontally"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Flip vertically"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Reset image"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Replace image"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Preview original image"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Delete image"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Add images to create a PDF document"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Image %1: %2"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget",
                      "Select to show this image's page; drag onto another image to swap"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Input images"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Previous page"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Next page"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Page %1 of %2"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "No pages"),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget",
                      "The page preview shows the last valid settings."),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "The original image is unavailable."),
    QT_TRANSLATE_NOOP("CollagePdfTemplateWidget", "Image preview was cancelled."),
};

quint64 draggedId(const QMimeData* mime, const CollageDocument* document) {
    if (!mime || !mime->hasFormat(QString::fromLatin1(dragType)))
        return 0;
    const auto parts = mime->data(QString::fromLatin1(dragType)).split(':');
    if (parts.size() != 2 || parts.front().toULongLong() != reinterpret_cast<quintptr>(document))
        return 0;
    bool valid = false;
    const auto id = parts.back().toULongLong(&valid);
    return valid && document->indexOf(id) >= 0 ? id : 0;
}

struct TemplateDetail final {
    QImage image;
    qint64 sourceKey = 0;
    QRect sourcePixels;
    qreal resolution = 0;
    std::shared_ptr<void> reservation;
};

void paintImage(QPainter& painter, const QRectF& target, const CollageItem& item,
                const TemplateDetail* detail = nullptr) {
    if (target.isEmpty() || item.preview.isNull())
        return;
    // Use original dimensions: a 256-pixel preview can round a very thin image to
    // one pixel, but that must not change the physical aspect ratio of the page.
    const auto transformed = item.transform.mapRect(QRectF(QPointF{}, item.image.size()));
    if (transformed.isEmpty())
        return;
    painter.save();
    painter.translate(target.topLeft());
    painter.scale(target.width() / transformed.width(), target.height() / transformed.height());
    painter.translate(-transformed.topLeft());
    painter.setTransform(item.transform, true);
    const QRectF source(QPointF{}, item.image.size());
    if (detail) {
        // Do not draw the low-resolution alpha a second time under the detailed tile.
        QPainterPath remaining;
        remaining.addRect(source);
        QPainterPath tile;
        tile.addRect(detail->sourcePixels);
        painter.save();
        painter.setClipPath(remaining.subtracted(tile), Qt::IntersectClip);
        painter.drawImage(source, item.preview);
        painter.restore();
        painter.drawImage(QRectF(detail->sourcePixels), detail->image);
    } else {
        painter.drawImage(source, item.preview);
    }
    painter.restore();
}

Qt::CursorShape resizeCursor(SnowCanvasSelectionHandle handle) {
    switch (handle) {
    case SnowCanvasSelectionHandle::TopLeft:
    case SnowCanvasSelectionHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case SnowCanvasSelectionHandle::TopRight:
    case SnowCanvasSelectionHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case SnowCanvasSelectionHandle::Top:
    case SnowCanvasSelectionHandle::Bottom:
        return Qt::SizeVerCursor;
    case SnowCanvasSelectionHandle::Left:
    case SnowCanvasSelectionHandle::Right:
        return Qt::SizeHorCursor;
    case SnowCanvasSelectionHandle::None:
        return Qt::ArrowCursor;
    }
    return Qt::ArrowCursor;
}

class OriginalReply final : public adqt::widgets::AdImageReply {
  public:
    OriginalReply(const CollageItem* item, QObject* parent)
        : AdImageReply(parent), m_image(item ? item->image : QImage{}),
          m_owner(item ? item->resourceOwner : nullptr) {
        QTimer::singleShot(0, this, [this] {
            if (!isFinished()) {
                if (m_image.isNull())
                    fail(templateText("The original image is unavailable."));
                else
                    succeed(m_image);
            }
        });
    }
    void abort() override {
        if (!isFinished())
            fail(templateText("Image preview was cancelled."));
    }

  private:
    QImage m_image;
    std::shared_ptr<void> m_owner;
};

class OriginalLoader final : public adqt::widgets::AdImageLoader {
  public:
    OriginalLoader(CollageDocument* document, QObject* parent)
        : AdImageLoader(parent), m_document(document) {}
    adqt::widgets::AdImageReply* load(const QUrl& source, const adqt::widgets::AdImageLoadOptions&,
                                      QObject* parent) override {
        const int index = m_document ? m_document->indexOf(source.path().toULongLong()) : -1;
        return new OriginalReply(index < 0 ? nullptr : &m_document->items()[index], parent);
    }

  private:
    QPointer<CollageDocument> m_document;
};

class TemplateThumbnail final : public QWidget {
  public:
    TemplateThumbnail(CollageDocument* document, quint64 id, QWidget* parent)
        : QWidget(parent), m_document(document), m_id(id) {
        setObjectName(QStringLiteral("collagePdfTemplateThumbnail_%1").arg(id));
        setProperty("collageItemId", QVariant::fromValue(id));
        setFixedSize(thumbnailWidth, thumbnailHeight);
        setFocusPolicy(Qt::StrongFocus);
        setAcceptDrops(true);
        retranslate();
    }
    std::function<void(quint64)> previewRequested;
    std::function<void(quint64, bool)> draggingChanged;
    bool editing = true;
    void retranslate() {
        const int index = m_document->indexOf(m_id);
        if (index < 0)
            return;
        const auto label = QFileInfo(m_document->items()[index].label).fileName();
        setAccessibleName(templateText("Image %1: %2").arg(index + 1).arg(label));
        setAccessibleDescription(
            templateText("Select to show this image's page; drag onto another image to swap"));
        setToolTip(label);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const int index = m_document->indexOf(m_id);
        if (index < 0)
            return;
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        const bool selected = m_document->selectedId() == m_id || hasFocus();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(m_target || selected ? theme.colorPrimary : theme.colorBorderSecondary,
                            m_target || selected ? 2 : 1));
        painter.setBrush(theme.colorBgContainer);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
        const QRectF available(6, 6, width() - 12, height() - 26);
        const auto& item = m_document->items()[index];
        const auto size =
            QSizeF(transformedSize(item)).scaled(available.size(), Qt::KeepAspectRatio);
        const QRectF image(available.center() - QPointF(size.width() / 2, size.height() / 2), size);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        paintImage(painter, image, item);
        painter.setPen(theme.colorTextSecondary);
        painter.drawText(QRect(4, height() - 20, width() - 8, 16), Qt::AlignCenter,
                         QString::number(index + 1));
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton)
            return;
        m_press = event->position();
        m_pressed = true;
        setFocus(Qt::MouseFocusReason);
        m_document->select(m_id);
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        m_pressed = false;
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!editing || !m_pressed || !event->buttons().testFlag(Qt::LeftButton) ||
            (event->position() - m_press).manhattanLength() < QApplication::startDragDistance())
            return;
        m_pressed = false;
        const QPointer<TemplateThumbnail> guard(this);
        if (draggingChanged)
            draggingChanged(m_id, true);
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData;
        mime->setData(QString::fromLatin1(dragType),
                      QByteArray::number(reinterpret_cast<quintptr>(m_document)) + ':' +
                          QByteArray::number(m_id));
        drag->setMimeData(mime);
        drag->setPixmap(grab().scaled(80, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        drag->exec(Qt::MoveAction);
        if (guard && draggingChanged)
            draggingChanged(m_id, false);
    }
    void keyPressEvent(QKeyEvent* event) override {
        const int index = m_document->indexOf(m_document->selectedId());
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (previewRequested)
                previewRequested(m_id);
        } else if (event->key() == Qt::Key_Space) {
            m_document->select(m_id);
        } else if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
                   editing) {
            m_document->remove(m_id);
        } else if (index >= 0 && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
            const auto next = std::clamp(index + (event->key() == Qt::Key_Left ? -1 : 1), 0,
                                         int(m_document->items().size()) - 1);
            m_document->select(m_document->items()[next].id);
        } else {
            QWidget::keyPressEvent(event);
            return;
        }
        event->accept();
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        m_target = editing && draggedId(event->mimeData(), m_document) != 0 &&
                   draggedId(event->mimeData(), m_document) != m_id;
        if (m_target)
            event->acceptProposedAction();
        update();
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (m_target)
            event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent*) override {
        m_target = false;
        update();
    }
    void dropEvent(QDropEvent* event) override {
        m_target = false;
        const auto source = draggedId(event->mimeData(), m_document);
        if (editing && source && m_document->swapItems(source, m_id))
            event->acceptProposedAction();
        update();
    }
    void focusInEvent(QFocusEvent* event) override {
        m_document->select(m_id);
        QWidget::focusInEvent(event);
        update();
    }
    void focusOutEvent(QFocusEvent* event) override {
        QWidget::focusOutEvent(event);
        update();
    }

  private:
    CollageDocument* m_document;
    quint64 m_id;
    QPointF m_press;
    bool m_pressed = false;
    bool m_target = false;
};
} // namespace

struct CollagePdfTemplateCanvas::Impl {
    using Detail = TemplateDetail;
    CollagePdfTemplateCanvas* owner = nullptr;
    CollageDocument* document = nullptr;
    CollagePdfComposedPage page;
    int pageIndex = 0;
    QRectF paper;
    qreal viewScale = 1;
    CanvasActionToolbar* toolbar = nullptr;
    CanvasActionToolbar* selectionToolbar = nullptr;
    std::array<AdButton*, 8> actions{};
    AdButton* add = nullptr;
    AdButton* clear = nullptr;
    AdButton* reverse = nullptr;
    bool loading = false;
    bool exporting = false;
    quint64 gestureId = 0;
    int gestureCell = -1;
    quint64 targetId = 0;
    SnowCanvasSelectionHandle handle = SnowCanvasSelectionHandle::None;
    QPointF press;
    QRectF initialImage;
    CollagePdfPlacement pendingPlacement;
    bool moved = false;
    QTimer* detailTimer = nullptr;
    ScreenshotExportQueue* detailQueue = nullptr;
    QHash<quint64, Detail> details;
    QSet<quint64> pendingDetails;
    int detailCapacity = 0;
    quint64 detailGeneration = 0;
    QHash<quint64, qint64> pendingDetailBytes;

    void detailStatistics() {
        qint64 bytes = 0;
        for (const auto& detail : details)
            bytes += detail.image.sizeInBytes();
        for (const auto bytesInFlight : pendingDetailBytes)
            bytes += bytesInFlight;
        owner->setProperty("detailBytes", bytes);
        if (owner->parentWidget())
            owner->parentWidget()->setProperty("detailBytes", bytes);
    }
    void cancelDetails() {
        detailTimer->stop();
        ++detailGeneration;
        detailQueue->cancel();
        pendingDetails.clear();
        pendingDetailBytes.clear();
        details.clear();
        detailStatistics();
    }
    const Detail* detailFor(const CollageItem& item) const {
        const auto entry = details.constFind(item.id);
        return entry != details.cend() && entry->sourceKey == item.image.cacheKey() ? &*entry
                                                                                    : nullptr;
    }
    void requestDetails() {
        if (!owner->isVisible() || gestureId || exporting || !page.isValid())
            return;
        QSet<quint64> retained;
        int occupied = 0;
        for (const auto& cell : page.cells)
            if (cell.itemId) {
                retained.insert(cell.itemId);
                ++occupied;
            }
        for (auto it = details.begin(); it != details.end();) {
            if (!retained.contains(it.key()))
                it = details.erase(it);
            else
                ++it;
        }
        // A page owns at most 32 MiB of detailed display rasters, independent of
        // input count, source dimensions, or oversized placement adjustments.
        const qreal maximumPixels = qreal(qint64{32} * 1024 * 1024 / 4) / std::max(1, occupied);
        if (detailCapacity != occupied) {
            cancelDetails();
            detailCapacity = occupied;
        }
        for (const auto& cell : page.cells) {
            if (pendingDetails.size() >= 2)
                break;
            if (!cell.itemId || pendingDetails.contains(cell.itemId))
                continue;
            const auto& item = document->items()[cell.itemIndex];
            const auto oriented = transformedSize(item);
            const auto imageBounds = mapped(cell.imageMillimeters);
            const QRectF viewport(owner->rect());
            const auto visible = imageBounds.intersected(viewport).intersected(
                item.id == document->selectedId() ? viewport : mapped(cell.clipMillimeters));
            if (visible.isEmpty())
                continue;
            const qreal screenScale = imageBounds.width() / oriented.width();
            const auto transformed = item.transform.mapRect(QRectF(QPointF{}, item.image.size()));
            QTransform normalized = item.transform;
            normalized.setMatrix(normalized.m11(), normalized.m12(), normalized.m13(),
                                 normalized.m21(), normalized.m22(), normalized.m23(),
                                 normalized.dx() - transformed.left(),
                                 normalized.dy() - transformed.top(), normalized.m33());
            const auto sourceRectangle = [&](const QRectF& view) {
                const QRectF pixels((view.left() - imageBounds.left()) / screenScale,
                                    (view.top() - imageBounds.top()) / screenScale,
                                    view.width() / screenScale, view.height() / screenScale);
                return normalized.inverted().mapRect(pixels).toAlignedRect().intersected(
                    QRect(QPoint{}, item.image.size()));
            };
            const QRect required = sourceRectangle(visible);
            const QRect crop =
                sourceRectangle(visible.adjusted(-48, -48, 48, 48).intersected(imageBounds));
            if (crop.isEmpty())
                continue;
            const qreal scale = std::min(1.0, screenScale * owner->devicePixelRatioF());
            QSize wanted(std::max(1, qCeil(crop.width() * scale)),
                         std::max(1, qCeil(crop.height() * scale)));
            if (wanted.width() > 2048 || wanted.height() > 2048)
                wanted = wanted.scaled(2048, 2048, Qt::KeepAspectRatio);
            if (qreal(wanted.width()) * wanted.height() > maximumPixels) {
                const qreal reduction =
                    std::sqrt(maximumPixels / (qreal(wanted.width()) * wanted.height()));
                wanted = QSize(std::max(1, qFloor(wanted.width() * reduction)),
                               std::max(1, qFloor(wanted.height() * reduction)));
            }
            const qreal resolution = std::min(qreal(wanted.width()) / crop.width(),
                                              qreal(wanted.height()) / crop.height());
            const auto old = details.constFind(item.id);
            if (old != details.cend() && old->sourceKey == item.image.cacheKey() &&
                old->sourcePixels.contains(required) && old->resolution >= resolution * .9)
                continue;
            const qreal previewResolution =
                std::min(qreal(item.preview.width()) / item.image.width(),
                         qreal(item.preview.height()) / item.image.height());
            if (resolution <= previewResolution * 1.05)
                continue;
            details.remove(item.id);
            const qint64 bytes = qint64(wanted.width()) * wanted.height() * 4;
            const auto scratch = image_codec::resizeWorkingBytes(crop.size(), wanted);
            if (!scratch)
                continue;
            auto reservation = collage_detail::reservePreview(bytes);
            auto working = collage_detail::reserveWorking(*scratch + 6 * 1024 * 1024 +
                                                          qint64(crop.width() + 7) * 4);
            if (!reservation || !working)
                continue;
            auto prepared = std::make_shared<Detail>();
            prepared->sourceKey = item.image.cacheKey();
            prepared->sourcePixels = crop;
            prepared->resolution = resolution;
            prepared->reservation = std::move(reservation);
            const auto id = item.id;
            const auto sourceKey = item.image.cacheKey();
            const auto generation = detailGeneration;
            pendingDetails.insert(id);
            pendingDetailBytes.insert(id, bytes);
            const bool queued = detailQueue->enqueue(
                ScreenshotExportCoordinator::Priority::Background,
                [image = item.image, sourceOwner = item.resourceOwner, crop, wanted, prepared,
                 working](const ScreenshotExportCancellation& cancellation) {
                    Q_UNUSED(sourceOwner);
                    Q_UNUSED(working);
                    ScreenshotExportTaskResult result;
                    if (cancellation.isCancellationRequested())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Cancelled, {});
                    const auto rows = collage_detail::croppedRows(image, crop, cancellation);
                    auto raster = snowCanvasAllocateImage(wanted, QImage::Format_RGBA8888);
                    if (raster.isNull() ||
                        !image_codec::resizeToRgba8(rows, wanted, raster.bits(),
                                                    raster.bytesPerLine(), raster.sizeInBytes()) ||
                        cancellation.isCancellationRequested())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Render, {});
                    raster.setDevicePixelRatio(1);
                    raster.setColorSpace(QColorSpace::SRgb);
                    prepared->image = std::move(raster);
                    return result;
                },
                [this, id, sourceKey, generation,
                 prepared](const ScreenshotExportTaskResult& result) {
                    if (generation != detailGeneration)
                        return;
                    pendingDetails.remove(id);
                    pendingDetailBytes.remove(id);
                    const int index = document->indexOf(id);
                    if (result.succeeded() && cellFor(id) && index >= 0 &&
                        document->items()[index].image.cacheKey() == sourceKey &&
                        !prepared->image.isNull()) {
                        details.insert(id, std::move(*prepared));
                        detailStatistics();
                        owner->update();
                    }
                    detailTimer->start();
                });
            if (!queued) {
                pendingDetails.remove(id);
                pendingDetailBytes.remove(id);
            }
        }
        detailStatistics();
    }

    QRectF mapped(const QRectF& value) const {
        return {paper.topLeft() + value.topLeft() * viewScale, value.size() * viewScale};
    }
    const CollagePdfCell* cellFor(quint64 id) const {
        if (!id)
            return nullptr;
        for (const auto& cell : page.cells)
            if (cell.itemId == id)
                return &cell;
        return nullptr;
    }
    quint64 hitCell(const QPointF& position) const {
        for (const auto& cell : page.cells)
            if (cell.itemId && mapped(cell.clipMillimeters).contains(position))
                return cell.itemId;
        return 0;
    }
    quint64 hitImage(const QPointF& position) const {
        for (const auto& cell : page.cells)
            if (cell.itemId && mapped(cell.clipMillimeters).contains(position) &&
                mapped(cell.imageMillimeters).contains(position))
                return cell.itemId;
        return 0;
    }
    void cancelGesture() {
        if (!gestureId && !targetId)
            return;
        gestureId = 0;
        gestureCell = -1;
        targetId = 0;
        moved = false;
        handle = SnowCanvasSelectionHandle::None;
        owner->unsetCursor();
        page = calculateCollagePdfComposedPage(document->items(), document->lastValidPdfSettings(),
                                               pageIndex);
        owner->setProperty("swapTargetId", QVariant::fromValue(quint64{0}));
        owner->update();
    }
    void geometry() {
        const QRectF available(24, 24, std::max(1, owner->width() - 48),
                               std::max(1, owner->height() - 24 - 56));
        if (page.isValid()) {
            const auto fitted = page.pageMillimeters.scaled(available.size(), Qt::KeepAspectRatio);
            paper = {available.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted};
            viewScale = fitted.width() / page.pageMillimeters.width();
        } else {
            paper = {};
            viewScale = 1;
        }
        toolbar->adjustSize();
        toolbar->move((owner->width() - toolbar->width()) / 2,
                      std::max(4, owner->height() - toolbar->height() - 12));
        toolbar->raise();
        const auto* selected = cellFor(document->selectedId());
        selectionToolbar->setVisible(selected != nullptr);
        if (selected) {
            const auto bounds = mapped(selected->imageMillimeters);
            selectionToolbar->adjustSize();
            const int maximumX = std::max(8, owner->width() - selectionToolbar->width() - 8);
            const int maximumY = std::max(8, toolbar->y() - selectionToolbar->height() - 8);
            selectionToolbar->move(
                std::clamp(qRound(bounds.center().x() - selectionToolbar->width() / 2.0), 8,
                           maximumX),
                std::clamp(qRound(bounds.top() - selectionToolbar->height() - 12), 8, maximumY));
            selectionToolbar->raise();
        }
        owner->setProperty("paperRect", paper);
        owner->setProperty("pageIndex", pageIndex);
        if (detailTimer)
            detailTimer->start();
        owner->update();
    }
    void enabled() {
        const bool selected = cellFor(document->selectedId()) != nullptr;
        for (auto* action : actions)
            action->setEnabled(selected && !exporting);
        actions[5]->setEnabled(selected && !loading && !exporting);
        actions[6]->setEnabled(selected);
        add->setEnabled(!loading && !exporting);
        add->setBusy(loading);
        clear->setEnabled(!exporting && !document->items().isEmpty());
        reverse->setEnabled(!loading && !exporting && document->items().size() > 1);
        geometry();
    }
    void refresh(bool followSelection = true) {
        cancelGesture();
        const int previous = pageIndex;
        const auto settings = document->lastValidPdfSettings();
        const int count = static_cast<int>(collagePdfPageCount(document->items().size(), settings));
        const int selectedIndex = document->indexOf(document->selectedId());
        if (followSelection && selectedIndex >= 0)
            pageIndex = selectedIndex / collagePdfTemplateCapacity(settings.templateType);
        pageIndex = std::clamp(pageIndex, 0, std::max(0, count - 1));
        if (pageIndex != previous)
            cancelDetails();
        page = calculateCollagePdfComposedPage(document->items(), settings, pageIndex);
        enabled();
        if (pageIndex != previous)
            emit owner->pageChanged(pageIndex);
    }
    void original(quint64 id) {
        emit owner->previewOriginalRequested(id);
    }
};

CollagePdfTemplateCanvas::CollagePdfTemplateCanvas(CollageDocument* document, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.document = document;
    setObjectName(QStringLiteral("collagePdfTemplateCanvas"));
    setMinimumSize(280, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    d.detailTimer = new QTimer(this);
    d.detailTimer->setSingleShot(true);
    d.detailTimer->setInterval(70);
    d.detailQueue = new ScreenshotExportQueue(ScreenshotExportCoordinator::shared(), this);
    connect(d.detailTimer, &QTimer::timeout, this, [this] { m_impl->requestDetails(); });
    d.toolbar = new CanvasActionToolbar(this);
    d.toolbar->setObjectName(QStringLiteral("collagePdfTemplateCanvasToolbar"));
    d.reverse =
        d.toolbar->addAction(snow_shot::presentation::icons::custom::outlined::ReverseOrder(),
                             "collagePdfTemplateReverseImages");
    d.add = d.toolbar->addAction(icons::Plus(), "collagePdfTemplateAddImages");
    d.toolbar->addSeparator();
    d.clear = d.toolbar->addAction(icons::IconDelete(), "collagePdfTemplateClear");
    d.clear->setAccentRole(AdButton::AccentRole::Danger);
    connect(d.add, &AdButton::clicked, this, &CollagePdfTemplateCanvas::addImagesRequested);
    connect(d.clear, &AdButton::clicked, this, &CollagePdfTemplateCanvas::clearRequested);
    connect(d.reverse, &AdButton::clicked, this, [this] {
        if (!m_impl->loading && !m_impl->exporting)
            m_impl->document->reverseItems();
    });
    d.selectionToolbar = new CanvasActionToolbar(this);
    d.selectionToolbar->setObjectName(QStringLiteral("collagePdfTemplateImageToolbar"));
    const std::array<adqt::icons::IconRef, 8> refs{
        icons::RotateRight(),
        icons::RotateLeft(),
        snow_shot::presentation::icons::custom::outlined::FlipHorizontal(),
        snow_shot::presentation::icons::custom::outlined::FlipVertical(),
        icons::Reload(),
        snow_shot::presentation::icons::custom::outlined::ReplaceImage(),
        icons::Eye(),
        icons::IconDelete()};
    constexpr std::array<const char*, 8> names{
        "collagePdfTemplateRotateRight",    "collagePdfTemplateRotateLeft",
        "collagePdfTemplateFlipHorizontal", "collagePdfTemplateFlipVertical",
        "collagePdfTemplateResetImage",     "collagePdfTemplateReplaceImage",
        "collagePdfTemplatePreviewImage",   "collagePdfTemplateDeleteImage"};
    for (std::size_t index = 0; index < d.actions.size(); ++index) {
        if (index == 5)
            d.selectionToolbar->addSeparator();
        d.actions[index] = d.selectionToolbar->addAction(refs[index], names[index]);
        connect(d.actions[index], &AdButton::clicked, this, [this, index] {
            auto& state = *m_impl;
            state.cancelGesture();
            const auto id = state.document->selectedId();
            if (!state.cellFor(id))
                return;
            if (index == 6)
                state.original(id);
            else if (!state.exporting) {
                if (index < 2)
                    state.document->rotate(id, index == 0);
                else if (index < 4)
                    state.document->flip(id, index == 2);
                else if (index == 4)
                    state.document->resetTransform(id);
                else if (index == 5 && !state.loading)
                    emit replaceImageRequested(id);
                else if (index == 7)
                    state.document->remove(id);
            }
        });
    }
    d.actions.back()->setAccentRole(AdButton::AccentRole::Danger);
    connect(document, &CollageDocument::changed, this, [this] { m_impl->refresh(); });
    connect(document, &CollageDocument::selectionChanged, this, [this] { m_impl->refresh(); });
    connect(document, &CollageDocument::pdfPlacementChanged, this,
            [this](quint64) { m_impl->refresh(false); });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this] { update(); });
    retranslateUi();
    d.refresh();
}
CollagePdfTemplateCanvas::~CollagePdfTemplateCanvas() {
    m_impl->cancelDetails();
}
int CollagePdfTemplateCanvas::pageIndex() const {
    return m_impl->pageIndex;
}
void CollagePdfTemplateCanvas::setPageIndex(int index) {
    auto& d = *m_impl;
    const auto settings = d.document->lastValidPdfSettings();
    const int count = static_cast<int>(collagePdfPageCount(d.document->items().size(), settings));
    index = std::clamp(index, 0, std::max(0, count - 1));
    const int first = index * collagePdfTemplateCapacity(settings.templateType);
    if (first < d.document->items().size()) {
        const int selectedIndex = d.document->indexOf(d.document->selectedId());
        if (selectedIndex < first ||
            selectedIndex >= first + collagePdfTemplateCapacity(settings.templateType))
            d.document->select(d.document->items()[first].id);
    }
    d.pageIndex = index;
    d.refresh();
    emit pageChanged(d.pageIndex);
}
QRectF CollagePdfTemplateCanvas::pageViewportRect() const {
    return m_impl->paper;
}
QRectF CollagePdfTemplateCanvas::imageViewportRect(quint64 id) const {
    const auto* cell = m_impl->cellFor(id);
    return cell ? m_impl->mapped(cell->imageMillimeters) : QRectF{};
}
QRectF CollagePdfTemplateCanvas::cellViewportRect(quint64 id) const {
    const auto* cell = m_impl->cellFor(id);
    return cell ? m_impl->mapped(cell->clipMillimeters) : QRectF{};
}
void CollagePdfTemplateCanvas::setLoading(bool loading) {
    m_impl->cancelGesture();
    m_impl->loading = loading;
    m_impl->enabled();
}
void CollagePdfTemplateCanvas::setExportBusy(bool busy) {
    m_impl->cancelGesture();
    if (busy)
        m_impl->cancelDetails();
    m_impl->exporting = busy;
    m_impl->enabled();
}
void CollagePdfTemplateCanvas::retranslateUi() {
    auto& d = *m_impl;
    setAccessibleName(templateText("Template stitching preview"));
    setAccessibleDescription(templateText(
        "Drag an image to move or swap it; drag its selection handles to scale around its center"));
    constexpr std::array<const char*, 8> labels{"Rotate clockwise by 90°",
                                                "Rotate counterclockwise by 90°",
                                                "Flip horizontally",
                                                "Flip vertically",
                                                "Reset image",
                                                "Replace image",
                                                "Preview original image",
                                                "Delete image"};
    for (std::size_t index = 0; index < labels.size(); ++index) {
        d.actions[index]->setToolTip(templateText(labels[index]));
        d.actions[index]->setAccessibleName(d.actions[index]->toolTip());
    }
    for (const auto& [button, label] : std::array<std::pair<AdButton*, const char*>, 3>{
             {{d.add, "Add images"}, {d.clear, "Clear all"}, {d.reverse, "Reverse image order"}}}) {
        button->setToolTip(templateText(label));
        button->setAccessibleName(button->toolTip());
    }
    update();
}
void CollagePdfTemplateCanvas::paintEvent(QPaintEvent*) {
    const auto& d = *m_impl;
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
    QPainter painter(this);
    painter.fillRect(rect(), theme.colorFillAlter);
    if (d.document->items().isEmpty()) {
        painter.setPen(theme.colorTextSecondary);
        painter.drawText(rect().adjusted(24, 24, -24, -56), Qt::AlignCenter | Qt::TextWordWrap,
                         templateText("Add images to create a PDF document"));
        return;
    }
    if (!d.page.isValid()) {
        painter.setPen(theme.colorError);
        painter.drawText(rect().adjusted(24, 24, -24, -56), Qt::AlignCenter | Qt::TextWordWrap,
                         d.page.error);
        return;
    }
    painter.fillRect(d.paper, Qt::white);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (const auto* selected = d.cellFor(d.document->selectedId())) {
        QPainterPath outside;
        outside.addRect(QRectF(rect()));
        QPainterPath inside;
        inside.addRect(d.mapped(selected->clipMillimeters));
        painter.save();
        painter.setClipPath(outside.subtracted(inside));
        painter.setOpacity(.25);
        paintImage(painter, d.mapped(selected->imageMillimeters),
                   d.document->items()[selected->itemIndex],
                   d.detailFor(d.document->items()[selected->itemIndex]));
        painter.restore();
    }
    for (const auto& cell : d.page.cells) {
        const auto bounds = d.mapped(cell.clipMillimeters);
        painter.save();
        painter.setClipRect(bounds);
        if (cell.itemId)
            paintImage(painter, d.mapped(cell.imageMillimeters),
                       d.document->items()[cell.itemIndex],
                       d.detailFor(d.document->items()[cell.itemIndex]));
        painter.restore();
        painter.setPen(QPen(QColor(190, 190, 190), 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bounds);
    }
    painter.setPen(QPen(theme.colorBorder, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(d.paper);
    if (d.targetId && d.targetId != d.gestureId) {
        if (const auto* target = d.cellFor(d.targetId))
            snowCanvasPaintSelectionMarquee(painter, d.mapped(target->clipMillimeters));
    }
    if (const auto* selected = d.cellFor(d.document->selectedId()))
        snowCanvasPaintImageSelection(painter, d.mapped(selected->imageMillimeters));
}
void CollagePdfTemplateCanvas::resizeEvent(QResizeEvent*) {
    m_impl->cancelGesture();
    m_impl->geometry();
}
void CollagePdfTemplateCanvas::mousePressEvent(QMouseEvent* event) {
    auto& d = *m_impl;
    if (event->button() != Qt::LeftButton || d.loading || d.exporting)
        return;
    setFocus(Qt::MouseFocusReason);
    d.cancelGesture();
    const auto selected = d.document->selectedId();
    const auto bounds = imageViewportRect(selected);
    const auto handle = snowCanvasImageSelectionHandleAt(bounds, event->position());
    const auto id =
        handle == SnowCanvasSelectionHandle::None ? d.hitImage(event->position()) : selected;
    d.document->select(id);
    if (!id) {
        event->accept();
        return;
    }
    d.gestureId = id;
    d.handle = handle;
    d.press = event->position();
    for (int index = 0; index < d.page.cells.size(); ++index) {
        const auto& cell = d.page.cells[index];
        if (cell.itemId != id)
            continue;
        d.gestureCell = index;
        d.initialImage = cell.imageMillimeters;
        const auto& item = d.document->items()[cell.itemIndex];
        d.pendingPlacement = item.pdfPlacement;
        d.pendingPlacement.centerOffsetMillimeters =
            cell.imageMillimeters.center() - cell.clipMillimeters.center();
        d.pendingPlacement.millimetersPerPixel =
            cell.imageMillimeters.width() / transformedSize(item).width();
        break;
    }
    setCursor(handle == SnowCanvasSelectionHandle::None ? Qt::ClosedHandCursor
                                                        : resizeCursor(handle));
    event->accept();
}
void CollagePdfTemplateCanvas::mouseMoveEvent(QMouseEvent* event) {
    auto& d = *m_impl;
    if (!d.gestureId || !event->buttons().testFlag(Qt::LeftButton)) {
        const auto handle = snowCanvasImageSelectionHandleAt(
            imageViewportRect(d.document->selectedId()), event->position());
        if (handle == SnowCanvasSelectionHandle::None)
            unsetCursor();
        else
            setCursor(resizeCursor(handle));
        return;
    }
    if (!d.moved &&
        (event->position() - d.press).manhattanLength() < QApplication::startDragDistance())
        return;
    d.moved = true;
    auto& cell = d.page.cells[d.gestureCell];
    const auto delta = (event->position() - d.press) / d.viewScale;
    if (d.handle == SnowCanvasSelectionHandle::None) {
        cell.imageMillimeters = d.initialImage.translated(delta);
        d.pendingPlacement.centerOffsetMillimeters =
            cell.imageMillimeters.center() - cell.clipMillimeters.center();
        const auto target = d.hitCell(event->position());
        d.targetId = target != d.gestureId ? target : 0;
        setCursor(d.targetId ? Qt::DragMoveCursor : Qt::ClosedHandCursor);
    } else {
        const auto half = d.initialImage.size() / 2;
        QPointF vector;
        switch (d.handle) {
        case SnowCanvasSelectionHandle::TopLeft:
            vector = {-half.width(), -half.height()};
            break;
        case SnowCanvasSelectionHandle::TopRight:
            vector = {half.width(), -half.height()};
            break;
        case SnowCanvasSelectionHandle::BottomRight:
            vector = {half.width(), half.height()};
            break;
        case SnowCanvasSelectionHandle::BottomLeft:
            vector = {-half.width(), half.height()};
            break;
        case SnowCanvasSelectionHandle::Top:
            vector = {0, -half.height()};
            break;
        case SnowCanvasSelectionHandle::Right:
            vector = {half.width(), 0};
            break;
        case SnowCanvasSelectionHandle::Bottom:
            vector = {0, half.height()};
            break;
        case SnowCanvasSelectionHandle::Left:
            vector = {-half.width(), 0};
            break;
        case SnowCanvasSelectionHandle::None:
            break;
        }
        const qreal denominator = QPointF::dotProduct(vector, vector);
        const qreal maximumEdge =
            std::max(d.initialImage.width(), d.initialImage.height()) * d.viewScale;
        const qreal scale = std::clamp(1 + QPointF::dotProduct(delta, vector) / denominator,
                                       2.0 / maximumEdge, 1000000.0 / maximumEdge);
        const auto size = d.initialImage.size() * scale;
        cell.imageMillimeters = {
            d.initialImage.center() - QPointF(size.width() / 2, size.height() / 2), size};
        const auto& item = d.document->items()[cell.itemIndex];
        d.pendingPlacement.millimetersPerPixel = size.width() / transformedSize(item).width();
        d.targetId = 0;
    }
    setProperty("swapTargetId", QVariant::fromValue(d.targetId));
    d.geometry();
    event->accept();
}
void CollagePdfTemplateCanvas::mouseReleaseEvent(QMouseEvent* event) {
    auto& d = *m_impl;
    if (event->button() != Qt::LeftButton || !d.gestureId)
        return;
    if (d.moved && !d.loading && !d.exporting) {
        QMouseEvent finalMove(QEvent::MouseMove, event->position(), event->globalPosition(),
                              Qt::NoButton, Qt::LeftButton, event->modifiers());
        mouseMoveEvent(&finalMove);
    }
    const auto id = d.gestureId;
    const auto releasedOver = d.hitCell(event->position());
    const auto target =
        d.handle == SnowCanvasSelectionHandle::None && releasedOver != id ? releasedOver : 0;
    const auto placement = d.pendingPlacement;
    const bool commit = d.moved && !d.loading && !d.exporting;
    d.cancelGesture();
    if (commit) {
        if (target)
            d.document->swapItems(id, target);
        else
            d.document->setPdfPlacement(id, placement);
    }
    d.geometry();
    event->accept();
}
void CollagePdfTemplateCanvas::keyPressEvent(QKeyEvent* event) {
    auto& d = *m_impl;
    if (event->key() == Qt::Key_Escape) {
        d.cancelGesture();
        d.geometry();
    } else if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
               !d.exporting) {
        d.cancelGesture();
        d.document->remove(d.document->selectedId());
    } else if (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown) {
        setPageIndex(d.pageIndex + (event->key() == Qt::Key_PageUp ? -1 : 1));
    } else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
               event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
        const int index = d.document->indexOf(d.document->selectedId());
        if (!d.document->items().isEmpty()) {
            const int delta = event->key() == Qt::Key_Left || event->key() == Qt::Key_Up ? -1 : 1;
            const int next = std::clamp(index + delta, 0, int(d.document->items().size()) - 1);
            d.document->select(d.document->items()[next].id);
        }
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}
void CollagePdfTemplateCanvas::hideEvent(QHideEvent* event) {
    m_impl->cancelGesture();
    m_impl->cancelDetails();
    QWidget::hideEvent(event);
}
void CollagePdfTemplateCanvas::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_impl->refresh();
}
void CollagePdfTemplateCanvas::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}
void CollagePdfTemplateCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (!m_impl->loading && !m_impl->exporting && draggedId(event->mimeData(), m_impl->document))
        event->acceptProposedAction();
}
void CollagePdfTemplateCanvas::dragMoveEvent(QDragMoveEvent* event) {
    auto& d = *m_impl;
    const auto source = draggedId(event->mimeData(), d.document);
    const auto target = d.hitCell(event->position());
    d.targetId = source && target != source ? target : 0;
    setProperty("swapTargetId", QVariant::fromValue(d.targetId));
    if (!d.loading && !d.exporting && d.targetId)
        event->acceptProposedAction();
    else
        event->ignore();
    update();
}
void CollagePdfTemplateCanvas::dragLeaveEvent(QDragLeaveEvent*) {
    m_impl->targetId = 0;
    setProperty("swapTargetId", QVariant::fromValue(quint64{0}));
    update();
}
void CollagePdfTemplateCanvas::dropEvent(QDropEvent* event) {
    auto& d = *m_impl;
    const auto source = draggedId(event->mimeData(), d.document);
    const auto target = d.hitCell(event->position());
    d.targetId = 0;
    if (!d.loading && !d.exporting && source && target && d.document->swapItems(source, target))
        event->acceptProposedAction();
    setProperty("swapTargetId", QVariant::fromValue(quint64{0}));
    update();
}

struct CollagePdfTemplateWidget::Impl {
    CollagePdfTemplateWidget* owner = nullptr;
    CollageDocument* document = nullptr;
    CollagePdfTemplateCanvas* canvas = nullptr;
    QLabel* notice = nullptr;
    QLabel* pageLabel = nullptr;
    AdButton* previous = nullptr;
    AdButton* next = nullptr;
    AdScrollArea* strip = nullptr;
    QWidget* content = nullptr;
    QHash<quint64, TemplateThumbnail*> thumbnails;
    quint64 activeDrag = 0;
    quint64 previousSelected = 0;
    AdImageViewer* viewer = nullptr;
    AdImageListModel* model = nullptr;
    bool loading = false;
    bool exporting = false;
    quint64 revision = std::numeric_limits<quint64>::max();
    void navigation() {
        const int count = static_cast<int>(
            collagePdfPageCount(document->items().size(), document->lastValidPdfSettings()));
        pageLabel->setText(
            count ? templateText("Page %1 of %2").arg(canvas->pageIndex() + 1).arg(count)
                  : templateText("No pages"));
        previous->setEnabled(canvas->pageIndex() > 0);
        next->setEnabled(canvas->pageIndex() + 1 < count);
    }
    void openOriginal(quint64 id) {
        const int index = document->indexOf(id);
        if (index < 0)
            return;
        if (revision != document->contentRevision()) {
            revision = document->contentRevision();
            adqt::widgets::AdImageItems items;
            items.reserve(document->items().size());
            for (const auto& item : document->items()) {
                QUrl source;
                source.setScheme(QStringLiteral("collage-template-original"));
                source.setPath(QString::number(item.id));
                source.setQuery(QString::number(item.image.cacheKey()));
                items.push_back({source, QFileInfo(item.label).fileName()});
            }
            model->setItems(items);
        }
        viewer->openAt(index);
    }
    void visibleThumbnails() {
        if (!owner->isVisible())
            return;
        const auto& items = document->items();
        const int first = std::max(0, strip->horizontalScrollBar()->value() / thumbnailStep - 1);
        const int last =
            std::min(int(items.size()), first + strip->viewport()->width() / thumbnailStep + 4);
        QSet<quint64> retained;
        for (int index = first; index < last; ++index)
            retained.insert(items[index].id);
        if (activeDrag)
            retained.insert(activeDrag);
        for (auto it = thumbnails.begin(); it != thumbnails.end();) {
            if (!retained.contains(it.key())) {
                it.value()->hide();
                it.value()->deleteLater();
                it = thumbnails.erase(it);
            } else {
                ++it;
            }
        }
        for (int index = first; index < last; ++index) {
            const auto id = items[index].id;
            auto* thumbnail = thumbnails.value(id);
            if (!thumbnail) {
                thumbnail = new TemplateThumbnail(document, id, content);
                thumbnail->previewRequested = [this](quint64 imageId) { openOriginal(imageId); };
                thumbnail->draggingChanged = [this](quint64 imageId, bool dragging) {
                    activeDrag = dragging ? imageId : 0;
                    visibleThumbnails();
                };
                thumbnails.insert(id, thumbnail);
            }
            thumbnail->editing = !loading && !exporting;
            thumbnail->move(index * thumbnailStep + 2, 2);
            thumbnail->retranslate();
            thumbnail->show();
            thumbnail->update();
        }
        owner->setProperty("materializedThumbnailCount", thumbnails.size());
        // Thumbnail widgets share the document's bounded source previews and own no rasters.
        owner->setProperty("thumbnailBytes", qint64{0});
    }
    void selection() {
        const bool thumbnailFocused =
            std::any_of(thumbnails.cbegin(), thumbnails.cend(),
                        [](const auto* thumbnail) { return thumbnail->hasFocus(); });
        const auto selected = document->selectedId();
        if (auto* thumbnail = thumbnails.value(previousSelected))
            thumbnail->update();
        previousSelected = selected;
        const int index = document->indexOf(selected);
        if (index >= 0) {
            const int left = index * thumbnailStep;
            const int current = strip->horizontalScrollBar()->value();
            const int viewport = strip->viewport()->width();
            if (left < current)
                strip->horizontalScrollBar()->setValue(left);
            else if (left + thumbnailWidth + 4 > current + viewport)
                strip->horizontalScrollBar()->setValue(left + thumbnailWidth + 4 - viewport);
        }
        visibleThumbnails();
        if (thumbnailFocused) {
            if (auto* thumbnail = thumbnails.value(selected))
                thumbnail->setFocus(Qt::OtherFocusReason);
        }
        navigation();
    }
    void refresh() {
        if (!owner->isVisible())
            return;
        if (viewer->isVisible() && revision != document->contentRevision()) {
            quint64 viewedId = document->selectedId();
            const auto originals = model->items();
            if (viewer->currentRow() >= 0 && viewer->currentRow() < originals.size())
                viewedId = originals[viewer->currentRow()].source.path().toULongLong();
            viewer->close();
            openOriginal(document->indexOf(viewedId) >= 0 ? viewedId : document->selectedId());
        }
        const auto width =
            std::max<qint64>(1, qint64(document->items().size()) * thumbnailStep + 4);
        content->setFixedSize(int(std::min<qint64>(width, QWIDGETSIZE_MAX)), thumbnailHeight + 4);
        strip->setVisible(!document->items().isEmpty());
        notice->setVisible(!validateCollagePdfSettings(document->pdfSettings()).isEmpty());
        selection();
    }
};

CollagePdfTemplateWidget::CollagePdfTemplateWidget(CollageDocument* document, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.document = document;
    setObjectName(QStringLiteral("collagePdfTemplateWidget"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    d.notice = new QLabel(this);
    d.notice->setObjectName(QStringLiteral("collagePdfTemplateValidationNotice"));
    d.notice->setWordWrap(true);
    layout->addWidget(d.notice);
    d.canvas = new CollagePdfTemplateCanvas(document, this);
    layout->addWidget(d.canvas, 1);
    auto* navigation = new QHBoxLayout;
    navigation->addStretch();
    d.previous = new AdButton(this);
    d.previous->setObjectName(QStringLiteral("collagePdfTemplatePreviousPage"));
    d.previous->setIconRef(icons::Left());
    d.previous->setButtonStyle(AdButton::ButtonStyle::Text);
    navigation->addWidget(d.previous);
    d.pageLabel = new QLabel(this);
    d.pageLabel->setObjectName(QStringLiteral("collagePdfTemplatePageNumber"));
    navigation->addWidget(d.pageLabel);
    d.next = new AdButton(this);
    d.next->setObjectName(QStringLiteral("collagePdfTemplateNextPage"));
    d.next->setIconRef(icons::Right());
    d.next->setButtonStyle(AdButton::ButtonStyle::Text);
    navigation->addWidget(d.next);
    navigation->addStretch();
    layout->addLayout(navigation);
    d.strip = new AdScrollArea(this);
    d.strip->setObjectName(QStringLiteral("collagePdfTemplateThumbnailStrip"));
    d.strip->setFrameShape(QFrame::NoFrame);
    d.strip->setWidgetResizable(false);
    d.strip->setFitToWidth(false);
    d.strip->setFixedHeight(thumbnailHeight + 4);
    d.strip->viewport()->installEventFilter(this);
    d.content = new QWidget;
    d.content->setObjectName(QStringLiteral("collagePdfTemplateThumbnails"));
    d.strip->setContentWidget(d.content);
    layout->addWidget(d.strip);
    d.viewer = new AdImageViewer(this);
    d.viewer->setObjectName(QStringLiteral("collagePdfTemplateOriginalViewer"));
    d.viewer->setOwnerWindow(this);
    d.viewer->setImageLoader(new OriginalLoader(document, this));
    d.model = new AdImageListModel(this);
    d.viewer->setModel(d.model);
    connect(d.canvas, &CollagePdfTemplateCanvas::addImagesRequested, this,
            &CollagePdfTemplateWidget::addImagesRequested);
    connect(d.canvas, &CollagePdfTemplateCanvas::clearRequested, this,
            &CollagePdfTemplateWidget::clearRequested);
    connect(d.canvas, &CollagePdfTemplateCanvas::replaceImageRequested, this,
            &CollagePdfTemplateWidget::replaceImageRequested);
    connect(d.canvas, &CollagePdfTemplateCanvas::previewOriginalRequested, this,
            [this](quint64 id) { m_impl->openOriginal(id); });
    connect(d.canvas, &CollagePdfTemplateCanvas::pageChanged, this,
            [this] { m_impl->navigation(); });
    connect(d.previous, &AdButton::clicked, this,
            [this] { m_impl->canvas->setPageIndex(m_impl->canvas->pageIndex() - 1); });
    connect(d.next, &AdButton::clicked, this,
            [this] { m_impl->canvas->setPageIndex(m_impl->canvas->pageIndex() + 1); });
    connect(d.strip->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { m_impl->visibleThumbnails(); });
    connect(document, &CollageDocument::changed, this, [this] { m_impl->refresh(); });
    connect(document, &CollageDocument::selectionChanged, this, [this] { m_impl->selection(); });
    retranslateUi();
    d.refresh();
}
CollagePdfTemplateWidget::~CollagePdfTemplateWidget() = default;
CollagePdfTemplateCanvas* CollagePdfTemplateWidget::canvas() const {
    return m_impl->canvas;
}
void CollagePdfTemplateWidget::setLoading(bool loading) {
    m_impl->loading = loading;
    m_impl->canvas->setLoading(loading);
    m_impl->visibleThumbnails();
}
void CollagePdfTemplateWidget::setExportBusy(bool busy) {
    m_impl->exporting = busy;
    m_impl->canvas->setExportBusy(busy);
    m_impl->visibleThumbnails();
}
void CollagePdfTemplateWidget::retranslateUi() {
    auto& d = *m_impl;
    d.canvas->retranslateUi();
    d.notice->setText(templateText("The page preview shows the last valid settings."));
    d.strip->setAccessibleName(templateText("Input images"));
    d.previous->setToolTip(templateText("Previous page"));
    d.previous->setAccessibleName(d.previous->toolTip());
    d.next->setToolTip(templateText("Next page"));
    d.next->setAccessibleName(d.next->toolTip());
    for (auto* thumbnail : d.thumbnails)
        thumbnail->retranslate();
    d.navigation();
}
bool CollagePdfTemplateWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_impl->strip->viewport() && event->type() == QEvent::Resize)
        m_impl->visibleThumbnails();
    return QWidget::eventFilter(watched, event);
}
void CollagePdfTemplateWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_impl->refresh();
}
void CollagePdfTemplateWidget::hideEvent(QHideEvent* event) {
    if (m_impl->activeDrag)
        QDrag::cancel();
    m_impl->viewer->close();
    QWidget::hideEvent(event);
}
void CollagePdfTemplateWidget::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}
} // namespace snow_shot::presentation::components
