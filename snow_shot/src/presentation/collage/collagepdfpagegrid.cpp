// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/collagepdfpagegrid.h"

#include "snow_shot/presentation/components/canvasactiontoolbar.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/components/imagedropsurface.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/styles/sequencenumberbadgestyle.h"
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
#include <QFontMetrics>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

namespace snow_shot::presentation::components {
namespace {
using adqt::widgets::AdButton;
using adqt::widgets::AdImage;
using adqt::widgets::AdImageItem;
using adqt::widgets::AdImageItems;
using adqt::widgets::AdImageListModel;
using adqt::widgets::AdImageViewer;
using adqt::widgets::AdScrollArea;
using styles::SequenceNumberBadgeStyle;
namespace icons = adqt::icons::antd::outlined;

constexpr auto dragType = "application/x-snow-shot-collage-pdf-page";
constexpr int gridInset = 16;
constexpr int gridGap = 16;
constexpr int preferredCardWidth = 220;
constexpr int cardHeight = 332;
constexpr int edgeScrollZone = 48;

QString gridText(const char* source) {
    return QCoreApplication::translate("CollagePdfPageGrid", source);
}

QString imageName(const QString& label) {
    const auto name = QFileInfo(label).fileName();
    return name.isEmpty() ? label : name;
}

[[maybe_unused]] constexpr std::array translations{
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "PDF document pages"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid",
                      "Drag pages to reorder. Use the arrow keys to select a page and Enter to "
                      "preview the original image."),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Page %1: %2"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Page %1"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Replace image"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Delete image"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Rotate clockwise by 90°"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Rotate counterclockwise by 90°"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Flip horizontally"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Flip vertically"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Reset transform"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Preview original image"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Reverse page order"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Clear"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Add images"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Add images to create a PDF document"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Drag image files here, or choose Add images"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Drop images to add pages"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Add multiple image files at once"),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "The original image is unavailable."),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "Image preview was cancelled."),
    QT_TRANSLATE_NOOP("CollagePdfPageGrid", "The page preview shows the last valid settings."),
};

quint64 draggedId(const QMimeData* mime, const CollageDocument* document) {
    if (!mime || !mime->hasFormat(QString::fromLatin1(dragType)))
        return 0;
    const auto parts = mime->data(QString::fromLatin1(dragType)).split(':');
    if (parts.size() != 2 || parts.front().toULongLong() != reinterpret_cast<quintptr>(document))
        return 0;
    bool valid = false;
    const auto id = parts.back().toULongLong(&valid);
    return valid ? id : 0;
}

QStringList droppedFiles(const QMimeData* mime) {
    if (!mime || !mime->hasUrls() || mime->hasFormat(QString::fromLatin1(dragType)))
        return {};
    const auto supported = ScreenshotClipboardContentReader::supportedImageFileExtensions();
    QStringList paths;
    for (const auto& url : mime->urls()) {
        if (url.isLocalFile() &&
            supported.contains(QFileInfo(url.toLocalFile()).suffix().toLower()))
            paths.append(url.toLocalFile());
    }
    return paths;
}

class MemoryImageReply final : public adqt::widgets::AdImageReply {
  public:
    MemoryImageReply(QImage image, std::shared_ptr<void> owner, QObject* parent)
        : AdImageReply(parent), m_image(std::move(image)), m_owner(std::move(owner)) {
        // The Image component connects finished only after load() returns.
        QTimer::singleShot(0, this, [this] {
            if (isFinished())
                return;
            if (m_image.isNull())
                fail(gridText("The original image is unavailable."));
            else
                succeed(m_image);
        });
    }

    void abort() override {
        if (!isFinished())
            fail(gridText("Image preview was cancelled."));
    }

  private:
    QImage m_image;
    std::shared_ptr<void> m_owner;
};

class MemoryImageLoader final : public adqt::widgets::AdImageLoader {
  public:
    explicit MemoryImageLoader(QObject* parent) : AdImageLoader(parent) {}

    std::function<void()> thumbnailCacheChanged;

    void setSettings(const CollagePdfSettings& settings, quint64 revision) {
        m_settings = settings;
        m_settingsRevision = revision;
    }

    bool retainThumbnails(const QSet<quint64>& retained) {
        for (auto it = m_thumbnails.begin(); it != m_thumbnails.end();) {
            if (!retained.contains(it.key()))
                it = m_thumbnails.erase(it);
            else
                ++it;
        }
        // Account for both the cached QImage and the Image component's QPixmap.
        const auto maximum = std::max<qint64>(
            4096, (qint64{64} * 1024 * 1024) / (8 * std::max<qsizetype>(1, retained.size())));
        bool resized = false;
        if (maximum < m_maximumThumbnailPixels) {
            for (const auto& thumbnail : m_thumbnails) {
                if (qint64(thumbnail.size.width()) * thumbnail.size.height() > maximum) {
                    resized = true;
                    break;
                }
            }
        }
        m_maximumThumbnailPixels = maximum;
        if (thumbnailCacheChanged)
            thumbnailCacheChanged();
        return resized;
    }

    void clearThumbnails() {
        m_thumbnails.clear();
        if (thumbnailCacheChanged)
            thumbnailCacheChanged();
    }

    [[nodiscard]] qint64 thumbnailBytes() const {
        qint64 bytes = 0;
        for (const auto& thumbnail : m_thumbnails)
            bytes += thumbnail.image.sizeInBytes() * 2;
        return bytes;
    }

    void setItems(const QVector<CollageItem>& items) {
        // A completed reply retires immediately. This snapshot keeps original resource
        // reservations alive for as long as the viewer and card images retain their QImages.
        QSet<quint64> retained;
        retained.reserve(items.size());
        m_items.reserve(items.size());
        for (const auto& item : items) {
            retained.insert(item.id);
            const auto previous = m_items.constFind(item.id);
            if (previous == m_items.cend() || previous->revision != item.revision ||
                previous->label != item.label)
                m_items.insert(item.id, item);
        }
        for (auto it = m_items.begin(); it != m_items.end();) {
            if (!retained.contains(it.key()))
                it = m_items.erase(it);
            else
                ++it;
        }
    }

    adqt::widgets::AdImageReply* load(const QUrl& source,
                                      const adqt::widgets::AdImageLoadOptions& options,
                                      QObject* parent) override {
        const auto id = source.path().toULongLong();
        const auto entry = m_items.constFind(id);
        if (entry == m_items.cend())
            return new MemoryImageReply({}, {}, parent);
        const auto& item = entry.value();
        if (source.scheme() != QStringLiteral("collage-pdf-thumbnail"))
            return new MemoryImageReply(item.image, item.resourceOwner, parent);
        const auto layout = calculateCollagePdfPageLayout(transformedSize(item), m_settings);
        if (!layout.isValid())
            return new MemoryImageReply({}, {}, parent);
        QSize requested =
            options.targetPixelSize.isEmpty() ? QSize(220, 332) : options.targetPixelSize;
        if (requested.width() > 512 || requested.height() > 512)
            requested = requested.scaled(512, 512, Qt::KeepAspectRatio);
        QSize size = layout.pageMillimeters.scaled(QSizeF(requested), Qt::KeepAspectRatio).toSize();
        const qint64 pixels = qint64(size.width()) * size.height();
        if (pixels > m_maximumThumbnailPixels) {
            const qreal scale = std::sqrt(qreal(m_maximumThumbnailPixels) / qreal(pixels));
            size = QSize(std::max(1, qFloor(size.width() * scale)),
                         std::max(1, qFloor(size.height() * scale)));
        }
        size = size.expandedTo(QSize(1, 1));
        const auto cached = m_thumbnails.constFind(id);
        if (cached != m_thumbnails.cend() && cached->size == size &&
            cached->itemRevision == item.revision && cached->settingsRevision == m_settingsRevision)
            return new MemoryImageReply(cached->image, cached->owner, parent);
        auto reservation = reserveCollagePreview(qint64(size.width()) * size.height() * 8);
        if (!reservation)
            return new MemoryImageReply({}, {}, parent);
        QImage page(size, QImage::Format_RGB32);
        if (page.isNull())
            return new MemoryImageReply({}, {}, parent);
        page.fill(Qt::white);
        page.setColorSpace(QColorSpace::SRgb);
        QPainter painter(&page);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const qreal scaleX = qreal(size.width()) / layout.pageMillimeters.width();
        const qreal scaleY = qreal(size.height()) / layout.pageMillimeters.height();
        const auto mapped = [scaleX, scaleY](const QRectF& rect) {
            return QRectF(rect.x() * scaleX, rect.y() * scaleY, rect.width() * scaleX,
                          rect.height() * scaleY);
        };
        const auto target = mapped(layout.imageMillimeters);
        const auto transformed = item.transform.mapRect(QRectF(QPointF{}, item.preview.size()));
        if (!item.preview.isNull() && !transformed.isEmpty()) {
            painter.save();
            painter.translate(target.topLeft());
            painter.scale(target.width() / transformed.width(),
                          target.height() / transformed.height());
            painter.translate(-transformed.topLeft());
            painter.setTransform(item.transform, true);
            painter.drawImage(QPointF{}, item.preview);
            painter.restore();
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(180, 180, 180), 1, Qt::DashLine));
        painter.drawRect(mapped(layout.contentMillimeters));
        painter.end();
        m_thumbnails.insert(id, {size, item.revision, m_settingsRevision, page, reservation});
        if (thumbnailCacheChanged)
            thumbnailCacheChanged();
        return new MemoryImageReply(page, std::move(reservation), parent);
    }

  private:
    QHash<quint64, CollageItem> m_items;
    struct Thumbnail {
        QSize size;
        quint64 itemRevision = 0;
        quint64 settingsRevision = 0;
        QImage image;
        std::shared_ptr<void> owner;
    };
    QHash<quint64, Thumbnail> m_thumbnails;
    CollagePdfSettings m_settings;
    quint64 m_settingsRevision = 0;
    qint64 m_maximumThumbnailPixels = 256 * 256;
};

class InsertionMarker final : public QWidget {
  public:
    explicit InsertionMarker(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("collagePdfInsertionMarker"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(theme.colorPrimary, 3));
        painter.drawLine(QPointF(width() / 2.0, 4), QPointF(width() / 2.0, height() - 4));
        painter.setBrush(theme.colorPrimary);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(width() / 2.0, 4), 3, 3);
        painter.drawEllipse(QPointF(width() / 2.0, height() - 4), 3, 3);
    }
};

class PdfPageCard final : public QWidget {
  public:
    PdfPageCard(CollageDocument* document, AdImageViewer* viewer, MemoryImageLoader* loader,
                QWidget* parent)
        : QWidget(parent), m_document(document) {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        m_image = new AdImage(this);
        m_image->setObjectName(QStringLiteral("collagePdfPageImage"));
        m_image->setImageLoader(loader);
        m_image->setViewer(viewer);
        m_image->setPreviewEnabled(false);
        m_image->setLoadingPolicy(AdImage::LoadingPolicy::WhenVisible);
        m_image->setDecodePolicy(AdImage::DecodePolicy::FitWidget);
        AdImage::SemanticStyles imageStyle;
        imageStyle.root.borderColor = QColor(Qt::transparent);
        imageStyle.root.backgroundColor = QColor(Qt::white);
        m_image->setSemanticStyles(imageStyle);
        m_image->setFocusPolicy(Qt::NoFocus);
        m_image->installEventFilter(this);
        m_number = new QLabel(this);
        m_number->setObjectName(QStringLiteral("collagePdfPageNumber"));
        m_number->setAlignment(Qt::AlignCenter);
        m_number->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_number->setStyleSheet(SequenceNumberBadgeStyle::styleSheet());
    }

    std::function<void(quint64)> selected;
    std::function<void(quint64)> removeRequested;
    std::function<void(quint64)> previewRequested;
    std::function<void(quint64)> dragRequested;
    std::function<void(quint64, int)> navigateRequested;

    [[nodiscard]] quint64 id() const {
        return m_id;
    }

    [[nodiscard]] bool containsFocus() const {
        const auto* focus = QApplication::focusWidget();
        return focus && (focus == this || isAncestorOf(focus));
    }

    void releaseThumbnail() {
        // Clear the displayed pixmap before its cache reservation retires; the
        // QWidget itself can safely wait for DeferredDelete during input handling.
        m_image->setSource({});
    }

    void setEditingEnabled(bool editing) {
        m_editing = editing;
    }

    void setItem(const CollageItem& item, int row, const CollagePdfSettings& settings,
                 quint64 pageRevision) {
        const bool sourceChanged = m_id != item.id || m_revision != item.revision ||
                                   m_pageRevision != pageRevision || m_image->source().isEmpty();
        m_id = item.id;
        m_revision = item.revision;
        m_pageRevision = pageRevision;
        m_ordinal = row + 1;
        m_label = imageName(item.label);
        m_pageLayout = calculateCollagePdfPageLayout(transformedSize(item), settings);
        setObjectName(QStringLiteral("collagePdfPage_%1").arg(m_id));
        setProperty("collageItemId", QVariant::fromValue(m_id));
        setProperty("pageNumber", m_ordinal);
        m_image->setPreviewRow(row);
        positionChildren();
        if (sourceChanged) {
            QUrl source;
            source.setScheme(QStringLiteral("collage-pdf-thumbnail"));
            source.setPath(QString::number(m_id));
            source.setQuery(QStringLiteral("%1:%2").arg(m_revision).arg(m_pageRevision));
            m_image->setSource(source);
        }
        m_image->setAltText(m_label);
        m_number->setText(QString::number(m_ordinal));
        retranslate();
        update();
    }

    void retranslate() {
        setToolTip(m_label);
        setAccessibleName(gridText("Page %1: %2").arg(m_ordinal).arg(m_label));
        m_number->setAccessibleName(gridText("Page %1").arg(m_ordinal));
        setAccessibleDescription(gridText(
            "Drag pages to reorder. Use the arrow keys to select a page and Enter to preview the "
            "original image."));
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF outer = QRectF(rect()).adjusted(1, 1, -1, -1);
        const bool isSelected = m_document->selectedId() == m_id || containsFocus();
        painter.setPen(
            QPen(isSelected ? theme.colorPrimary : theme.colorBorderSecondary, isSelected ? 2 : 1));
        painter.setBrush(theme.colorFillAlter);
        painter.drawRoundedRect(outer, theme.borderRadiusSM, theme.borderRadiusSM);
        painter.setPen(QPen(theme.colorBorderSecondary, 1));
        painter.setBrush(Qt::white);
        painter.drawRect(m_paper);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(180, 180, 180), 1, Qt::DashLine));
        painter.drawRect(m_content);
        painter.setPen(theme.colorText);
        const QRect labelRect(10, height() - 29, width() - 20, 20);
        painter.drawText(labelRect, Qt::AlignHCenter | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_label, Qt::ElideRight, labelRect.width()));
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        positionChildren();
    }

    void focusInEvent(QFocusEvent* event) override {
        if (selected)
            selected(m_id);
        update();
        QWidget::focusInEvent(event);
    }

    void focusOutEvent(QFocusEvent* event) override {
        QTimer::singleShot(0, this, [this] { update(); });
        QWidget::focusOutEvent(event);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_image) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    pressed(m_image->mapTo(this, mouse->position().toPoint()));
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                moved(m_image->mapTo(this, mouse->position().toPoint()), mouse->buttons());
                return mouse->buttons().testFlag(Qt::LeftButton);
            } else if (event->type() == QEvent::MouseButtonRelease) {
                m_pressed = false;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            pressed(event->position());
            event->accept();
        } else {
            QWidget::mousePressEvent(event);
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        moved(event->position(), event->buttons());
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        m_pressed = false;
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        const auto key = event->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
            if (previewRequested)
                previewRequested(m_id);
        } else if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && m_editing) {
            if (removeRequested)
                removeRequested(m_id);
        } else if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
                   key == Qt::Key_Down || key == Qt::Key_Home || key == Qt::Key_End) {
            if (navigateRequested)
                navigateRequested(m_id, key);
        } else {
            QWidget::keyPressEvent(event);
            return;
        }
        event->accept();
    }

  private:
    void pressed(QPointF point) {
        m_press = point;
        m_pressed = true;
        setFocus(Qt::MouseFocusReason);
        if (selected)
            selected(m_id);
    }

    void moved(QPointF point, Qt::MouseButtons buttons) {
        if (m_pressed && m_editing && buttons.testFlag(Qt::LeftButton) &&
            (point - m_press).manhattanLength() >= QApplication::startDragDistance()) {
            m_pressed = false;
            if (dragRequested)
                dragRequested(m_id);
        }
    }

    void positionChildren() {
        if (!m_pageLayout.isValid()) {
            m_image->hide();
            return;
        }
        const auto paper = m_pageLayout.pageMillimeters;
        const QRectF available(10, 10, std::max(1, width() - 20), height() - 48);
        const auto fitted = paper.scaled(available.size(), Qt::KeepAspectRatio);
        m_paper =
            QRectF(available.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted);
        const qreal scale = fitted.width() / paper.width();
        const auto mapped = [this, scale](const QRectF& rect) {
            return QRectF(m_paper.topLeft() + rect.topLeft() * scale, rect.size() * scale);
        };
        m_content = mapped(m_pageLayout.contentMillimeters);
        const auto image = mapped(m_pageLayout.imageMillimeters);
        setProperty("paperRect", m_paper);
        setProperty("contentRect", m_content);
        setProperty("imageRect", image);
        // The full-page thumbnail preserves physical layout even when the bounded
        // source preview rounds a long image's short edge to one pixel. Compensate
        // for AdImage's final border pixel while keeping its border transparent.
        m_image->setGeometry(m_paper.toAlignedRect().adjusted(0, 0, 1, 1));
        m_image->show();
        m_number->setGeometry(
            QRect(QPoint(qRound(m_paper.left()), qRound(m_paper.top())) +
                      QPoint(SequenceNumberBadgeStyle::inset, SequenceNumberBadgeStyle::inset),
                  SequenceNumberBadgeStyle::size(fontMetrics(), QString::number(m_ordinal))));
        m_number->raise();
    }

    CollageDocument* m_document;
    quint64 m_id = 0;
    quint64 m_revision = 0;
    quint64 m_pageRevision = 0;
    int m_ordinal = 1;
    QString m_label;
    CollagePdfPageLayout m_pageLayout;
    QRectF m_paper;
    QRectF m_content;
    QPointF m_press;
    bool m_pressed = false;
    bool m_editing = true;
    AdImage* m_image = nullptr;
    QLabel* m_number = nullptr;
};
} // namespace

struct CollagePdfPageGrid::Impl {
    CollagePdfPageGrid* owner = nullptr;
    QPointer<CollageDocument> document;
    AdScrollArea* scroll = nullptr;
    QWidget* content = nullptr;
    CanvasActionToolbar* toolbar = nullptr;
    CanvasActionToolbar* selectionToolbar = nullptr;
    std::array<AdButton*, 8> selectionActions{};
    AdButton* reverse = nullptr;
    AdButton* add = nullptr;
    AdButton* clear = nullptr;
    ImageDropSurface* empty = nullptr;
    ImageDropSurface* dropHint = nullptr;
    QLabel* validationNotice = nullptr;
    InsertionMarker* marker = nullptr;
    AdImageViewer* viewer = nullptr;
    AdImageListModel* model = nullptr;
    MemoryImageLoader* loader = nullptr;
    QTimer* refreshTimer = nullptr;
    QTimer* scrollTimer = nullptr;
    QHash<quint64, PdfPageCard*> cards;
    QHash<quint64, int> indexById;
    QVector<quint64> order;
    AdImageItems previewItems;
    QVector<qint64> originalKeys;
    quint64 contentRevision = std::numeric_limits<quint64>::max();
    CollagePdfSettings settings;
    quint64 pageRevision = 0;
    int columns = 1;
    int width = preferredCardWidth;
    int leadingInset = gridInset;
    int insertion = -1;
    quint64 activeDragId = 0;
    QPointF dragPosition;
    bool loading = false;
    bool exporting = false;
    bool syncing = false;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    [[nodiscard]] QRect cardRect(int index) const {
        return QRect(leadingInset + (index % columns) * (width + gridGap),
                     gridInset + (index / columns) * (cardHeight + gridGap), width, cardHeight);
    }

    void positionSelectionToolbar() {
        const auto* card = cards.value(document->selectedId());
        const bool hasSelection = indexById.contains(document->selectedId());
        const bool editing = !exporting && hasSelection;
        for (auto* action : selectionActions)
            action->setEnabled(editing);
        selectionActions[5]->setEnabled(editing && !loading);
        selectionActions[6]->setEnabled(hasSelection);
        const auto viewport = scroll->viewport()->geometry().translated(scroll->pos());
        const auto selected = card ? QRect(card->mapTo(owner, QPoint()), card->size()) : QRect();
        const bool visible =
            hasSelection && card && card->isVisible() && selected.intersects(viewport);
        selectionToolbar->setVisible(visible);
        if (!visible)
            return;
        selectionToolbar->adjustSize();
        const int minimumX = viewport.left() + 8;
        const int minimumY = viewport.top() + 8;
        const int maximumX = std::max(minimumX, viewport.right() - selectionToolbar->width() - 7);
        const int maximumY = std::max(minimumY, viewport.bottom() - selectionToolbar->height() -
                                                    toolbar->height() - gridInset * 2);
        const int x = selected.center().x() - selectionToolbar->width() / 2;
        const int y = selected.top() - selectionToolbar->height() - 8;
        selectionToolbar->move(std::clamp(x, minimumX, maximumX),
                               std::clamp(y, minimumY, maximumY));
        selectionToolbar->raise();
    }

    void positionOverlays() {
        toolbar->adjustSize();
        const auto viewport = scroll->viewport()->geometry().translated(scroll->pos());
        toolbar->move(viewport.center().x() - toolbar->width() / 2,
                      viewport.bottom() - toolbar->height() - gridInset + 1);
        toolbar->raise();
        positionSelectionToolbar();
        empty->setGeometry(scroll->geometry());
        dropHint->setGeometry(owner->rect());
        dropHint->raise();
        if (insertion >= 0)
            positionMarker();
    }

    void enabled() {
        reverse->setEnabled(!loading && !exporting && document->items().size() > 1);
        add->setEnabled(!loading && !exporting);
        add->setBusy(loading);
        clear->setEnabled(!exporting && !document->items().isEmpty());
        for (auto* card : cards)
            card->setEditingEnabled(!exporting);
        positionSelectionToolbar();
        if (loading || exporting) {
            dropHint->hide();
            cancelDrag();
        }
    }

    void rebuildPreview() {
        AdImageItems next;
        QVector<qint64> keys;
        next.reserve(document->items().size());
        keys.reserve(document->items().size());
        for (const auto& item : document->items()) {
            QUrl source;
            source.setScheme(QStringLiteral("collage-pdf-original"));
            source.setPath(QString::number(item.id));
            next.push_back({source, imageName(item.label)});
            keys.push_back(item.image.cacheKey());
        }
        const bool changed = next != previewItems || keys != originalKeys;
        const bool wasVisible = changed && viewer->isVisible();
        quint64 viewedId = 0;
        if (wasVisible && viewer->currentRow() >= 0 && viewer->currentRow() < previewItems.size())
            viewedId = previewItems[viewer->currentRow()].source.path().toULongLong();
        if (changed)
            viewer->close();
        loader->setItems(document->items());
        if (changed) {
            previewItems = std::move(next);
            originalKeys = std::move(keys);
            model->setItems(previewItems);
            if (wasVisible && !order.isEmpty())
                viewer->openAt(
                    indexById.value(viewedId, indexById.value(document->selectedId(), 0)));
        }
    }

    void refresh() {
        if (syncing || !owner->isVisible())
            return;
        refreshTimer->stop();
        syncing = true;
        const int previousColumns = columns;
        const int previousTop = scroll->verticalScrollBar()->value();
        const int anchorIndex =
            std::max(0, (previousTop - gridInset) / (cardHeight + gridGap)) * columns;
        const quint64 anchorId = anchorIndex < order.size() ? order[anchorIndex] : 0;
        const int anchorOffset = previousTop - cardRect(anchorIndex).top();
        const auto nextSettings = document->lastValidPdfSettings();
        if (settings != nextSettings) {
            const auto previousPage = calculateCollagePdfPageLayout(QSize(1, 1), settings);
            const auto nextPage = calculateCollagePdfPageLayout(QSize(1, 1), nextSettings);
            if (previousPage.pageMillimeters != nextPage.pageMillimeters ||
                previousPage.contentMillimeters != nextPage.contentMillimeters)
                ++pageRevision;
            settings = nextSettings;
        }
        loader->setSettings(settings, pageRevision);
        validationNotice->setVisible(
            !validateCollagePdfSettings(document->pdfSettings()).isEmpty());
        owner->layout()->activate();
        if (contentRevision != document->contentRevision()) {
            order.clear();
            indexById.clear();
            order.reserve(document->items().size());
            indexById.reserve(document->items().size());
            for (const auto& item : document->items()) {
                indexById.insert(item.id, static_cast<int>(order.size()));
                order.append(item.id);
            }
            rebuildPreview();
            contentRevision = document->contentRevision();
        }
        const int available = std::max(1, scroll->viewport()->width() - gridInset * 2);
        columns = std::max(1, (available + gridGap) / (preferredCardWidth + gridGap));
        width = std::min(preferredCardWidth, available);
        const int occupied = columns * width + (columns - 1) * gridGap;
        leadingInset = std::max(0, (scroll->viewport()->width() - occupied) / 2);
        const qint64 rows = (qint64(order.size()) + columns - 1) / columns;
        const qint64 height =
            gridInset + rows * (cardHeight + gridGap) + toolbar->sizeHint().height() + gridInset;
        content->setFixedHeight(static_cast<int>(std::min<qint64>(height, QWIDGETSIZE_MAX)));
        if (previousColumns != columns && indexById.contains(anchorId))
            scroll->verticalScrollBar()->setValue(cardRect(indexById.value(anchorId)).top() +
                                                  anchorOffset);
        owner->setProperty("columnCount", columns);
        empty->setVisible(order.isEmpty());
        toolbar->setVisible(!order.isEmpty());
        scroll->show();
        enabled();
        syncing = false;
        refreshVisible();
        positionOverlays();
    }

    void refreshVisible() {
        if (syncing || !owner->isVisible())
            return;
        if (refreshTimer->isActive()) {
            refreshTimer->stop();
            refresh();
            return;
        }
        const int top = scroll->verticalScrollBar()->value();
        const int firstRow = std::max(0, (top - gridInset) / (cardHeight + gridGap) - 1);
        const int lastRow =
            std::max(firstRow, (top + scroll->viewport()->height()) / (cardHeight + gridGap) + 1);
        const int first = firstRow * columns;
        const int last = std::min(static_cast<int>(order.size()), (lastRow + 1) * columns);
        QSet<quint64> retained;
        for (int index = first; index < last; ++index)
            retained.insert(order[index]);
        for (auto it = cards.begin(); it != cards.end();) {
            const bool exists = indexById.contains(it.key());
            if (it.key() != activeDragId &&
                (!exists || (!retained.contains(it.key()) && !it.value()->containsFocus()))) {
                it.value()->hide();
                it.value()->releaseThumbnail();
                it.value()->deleteLater();
                it = cards.erase(it);
            } else {
                if (!exists) {
                    it.value()->hide();
                    it.value()->releaseThumbnail();
                }
                ++it;
            }
        }
        QSet<quint64> thumbnailIds = retained;
        for (auto it = cards.cbegin(); it != cards.cend(); ++it)
            thumbnailIds.insert(it.key());
        if (loader->retainThumbnails(thumbnailIds)) {
            for (auto* card : cards)
                card->releaseThumbnail();
            loader->clearThumbnails();
            ++pageRevision;
            loader->setSettings(settings, pageRevision);
        }
        for (int index = first; index < last; ++index) {
            const auto id = order[index];
            auto* card = cards.value(id);
            if (!card) {
                card = new PdfPageCard(document, viewer, loader, content);
                card->selected = [this](quint64 itemId) { document->select(itemId); };
                card->removeRequested = [this](quint64 itemId) { deleteItem(itemId); };
                card->previewRequested = [this](quint64 itemId) { openPreview(itemId); };
                card->navigateRequested = [this](quint64 itemId, int key) {
                    navigate(itemId, key);
                };
                card->dragRequested = [this](quint64 itemId) { startDrag(itemId); };
                cards.insert(id, card);
            }
        }
        for (auto it = cards.begin(); it != cards.end(); ++it) {
            const int index = indexById.value(it.key(), -1);
            if (index < 0)
                continue;
            it.value()->setGeometry(cardRect(index));
            it.value()->setItem(document->items()[index], index, settings, pageRevision);
            it.value()->setEditingEnabled(!exporting);
            it.value()->show();
        }
        owner->setProperty("materializedPageCount", cards.size());
        owner->setProperty("pageThumbnailBytes", loader->thumbnailBytes());
        positionSelectionToolbar();
        if (insertion >= 0)
            positionMarker();
    }

    void openPreview(quint64 id) {
        // Flush pending document changes before choosing an index or supplying originals.
        refreshTimer->stop();
        refresh();
        const int row = indexById.value(id, -1);
        if (row >= 0)
            viewer->openAt(row);
    }

    void deleteItem(quint64 id) {
        if (exporting)
            return;
        const auto* deleted = cards.value(id);
        const auto* focus = QApplication::focusWidget();
        const bool restoreFocus =
            (deleted && deleted->containsFocus()) ||
            (id == document->selectedId() && focus && selectionToolbar->isAncestorOf(focus));
        if (!document->remove(id))
            return;
        refreshTimer->stop();
        refresh();
        if (restoreFocus && !order.isEmpty()) {
            const auto next = document->selectedId();
            if (auto* nextCard = cards.value(next))
                nextCard->setFocus(Qt::OtherFocusReason);
        }
    }

    void navigate(quint64 id, int key) {
        int index = indexById.value(id, -1);
        if (index < 0 || order.isEmpty())
            return;
        if (key == Qt::Key_Left)
            --index;
        else if (key == Qt::Key_Right)
            ++index;
        else if (key == Qt::Key_Up)
            index -= columns;
        else if (key == Qt::Key_Down)
            index += columns;
        else if (key == Qt::Key_Home)
            index = 0;
        else if (key == Qt::Key_End)
            index = static_cast<int>(order.size()) - 1;
        index = std::clamp(index, 0, static_cast<int>(order.size()) - 1);
        const auto bounds = cardRect(index);
        auto* bar = scroll->verticalScrollBar();
        const int unobscuredHeight =
            std::max(1, scroll->viewport()->height() - toolbar->height() - gridInset * 2);
        if (bounds.top() < bar->value() + gridInset)
            bar->setValue(bounds.top() - gridInset);
        else if (bounds.bottom() > bar->value() + unobscuredHeight)
            bar->setValue(bounds.bottom() - unobscuredHeight);
        refreshVisible();
        if (auto* card = cards.value(order[index]))
            card->setFocus(Qt::OtherFocusReason);
    }

    void startDrag(quint64 id) {
        if (exporting || !indexById.contains(id))
            return;
        auto* card = cards.value(id);
        if (!card)
            return;
        activeDragId = id;
        const auto lifetime = alive;
        const QPointer<CollagePdfPageGrid> guardedOwner(owner);
        const QPointer<QDrag> drag(new QDrag(card));
        auto* mime = new QMimeData;
        mime->setData(QString::fromLatin1(dragType),
                      QByteArray::number(reinterpret_cast<quintptr>(document.data())) + ':' +
                          QByteArray::number(id));
        drag->setMimeData(mime);
        drag->setPixmap(
            card->grab().scaled(110, 166, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        drag->setHotSpot(QPoint(55, 50));
        drag->exec(Qt::MoveAction);
        if (!*lifetime || !guardedOwner)
            return;
        activeDragId = 0;
        cancelDrag();
        if (owner->isVisible()) {
            refreshVisible();
        } else if (auto* source = cards.take(id)) {
            source->deleteLater();
            owner->setProperty("materializedPageCount", cards.size());
        }
        if (drag)
            drag->deleteLater();
    }

    [[nodiscard]] bool acceptsInternal(const QMimeData* mime) const {
        const auto id = draggedId(mime, document);
        return !exporting && id != 0 && indexById.contains(id);
    }

    [[nodiscard]] bool acceptsFiles(const QMimeData* mime, Qt::DropActions actions) const {
        return !loading && !exporting && actions.testFlag(Qt::CopyAction) &&
               !droppedFiles(mime).isEmpty();
    }

    void updateInsertion(QPointF position) {
        dragPosition = position;
        const QPointF inContent = position + QPointF(0, scroll->verticalScrollBar()->value());
        const int row = std::max(
            0, static_cast<int>(std::floor((inContent.y() - gridInset) / (cardHeight + gridGap))));
        const qreal relativeX = inContent.x() - leadingInset;
        const int column =
            std::clamp(static_cast<int>(std::floor(relativeX / (width + gridGap))), 0, columns - 1);
        const int pointed = std::min(static_cast<int>(order.size()), row * columns + column);
        insertion = pointed;
        const qreal rowOffset = inContent.y() - gridInset - row * (cardHeight + gridGap);
        if (inContent.y() < gridInset)
            insertion = 0;
        else if (rowOffset >= cardHeight)
            insertion = std::min(static_cast<int>(order.size()), (row + 1) * columns);
        else if (pointed < order.size() && inContent.x() >= cardRect(pointed).center().x())
            ++insertion;
        insertion = std::clamp(insertion, 0, static_cast<int>(order.size()));
        owner->setProperty("insertionIndex", insertion);
        positionMarker();
        if (position.y() < edgeScrollZone ||
            position.y() > scroll->viewport()->height() - edgeScrollZone)
            scrollTimer->start();
        else
            scrollTimer->stop();
    }

    void positionMarker() {
        if (insertion < 0 || order.isEmpty()) {
            marker->hide();
            return;
        }
        const bool trailing = insertion == order.size();
        const auto bounds = cardRect(trailing ? insertion - 1 : insertion);
        const int x = trailing ? bounds.right() + gridGap / 2 : bounds.left() - gridGap / 2;
        marker->setGeometry(x - 4, bounds.top() - scroll->verticalScrollBar()->value(), 9,
                            bounds.height());
        marker->show();
        marker->raise();
    }

    void scrollDrag() {
        if (exporting || insertion < 0) {
            cancelDrag();
            return;
        }
        int delta = 0;
        if (dragPosition.y() < edgeScrollZone)
            delta = -std::clamp(qRound((edgeScrollZone - dragPosition.y()) / 2), 1, 24);
        else if (dragPosition.y() > scroll->viewport()->height() - edgeScrollZone)
            delta = std::clamp(
                qRound((dragPosition.y() - scroll->viewport()->height() + edgeScrollZone) / 2), 1,
                24);
        auto* bar = scroll->verticalScrollBar();
        bar->setValue(bar->value() + delta);
        updateInsertion(dragPosition);
    }

    void cancelDrag() {
        scrollTimer->stop();
        insertion = -1;
        owner->setProperty("insertionIndex", -1);
        marker->hide();
    }

    void dragEnter(QDragEnterEvent* event, QPointF position) {
        if (acceptsInternal(event->mimeData())) {
            dropHint->hide();
            updateInsertion(position);
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else if (acceptsFiles(event->mimeData(), event->possibleActions())) {
            cancelDrag();
            dropHint->show();
            dropHint->raise();
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            event->ignore();
        }
    }

    void dragMove(QDragMoveEvent* event, QPointF position) {
        if (acceptsInternal(event->mimeData())) {
            updateInsertion(position);
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else if (acceptsFiles(event->mimeData(), event->possibleActions())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            cancelDrag();
            dropHint->hide();
            event->ignore();
        }
    }

    void drop(QDropEvent* event, QPointF position) {
        dropHint->hide();
        if (acceptsInternal(event->mimeData())) {
            updateInsertion(position);
            const auto id = draggedId(event->mimeData(), document);
            const int source = indexById.value(id, -1);
            const int target = std::clamp(insertion > source ? insertion - 1 : insertion, 0,
                                          static_cast<int>(order.size()) - 1);
            document->moveItem(id, target);
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else if (acceptsFiles(event->mimeData(), event->possibleActions())) {
            const auto paths = droppedFiles(event->mimeData());
            event->setDropAction(Qt::CopyAction);
            event->accept();
            emit owner->imageFilesDropped(paths);
        } else {
            event->ignore();
        }
        cancelDrag();
    }
};

CollagePdfPageGrid::CollagePdfPageGrid(CollageDocument* document, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.document = document;
    setObjectName(QStringLiteral("collagePdfPageGrid"));
    setAcceptDrops(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    d.validationNotice = new QLabel(this);
    d.validationNotice->setObjectName(QStringLiteral("collagePdfValidationNotice"));
    d.validationNotice->setWordWrap(true);
    d.validationNotice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    d.validationNotice->hide();
    layout->addWidget(d.validationNotice);
    d.scroll = new AdScrollArea(this);
    d.scroll->setObjectName(QStringLiteral("collagePdfPagesScroll"));
    d.scroll->setFrameShape(QFrame::NoFrame);
    d.scroll->setWidgetResizable(true);
    d.scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d.scroll->viewport()->setAutoFillBackground(false);
    d.scroll->viewport()->setAcceptDrops(true);
    d.scroll->viewport()->installEventFilter(this);
    d.content = new QWidget;
    d.content->setObjectName(QStringLiteral("collagePdfPagesContent"));
    d.scroll->setContentWidget(d.content);
    d.content->setAutoFillBackground(false);
    d.content->installEventFilter(this);
    layout->addWidget(d.scroll);
    d.toolbar = new CanvasActionToolbar(this);
    d.toolbar->setObjectName(QStringLiteral("collagePdfCanvasToolbar"));
    d.reverse =
        d.toolbar->addAction(snow_shot::presentation::icons::custom::outlined::ReverseOrder(),
                             "collagePdfReversePageOrder");
    d.add = d.toolbar->addAction(icons::Plus(), "collagePdfAddImages");
    d.toolbar->addSeparator();
    d.clear = d.toolbar->addAction(icons::IconDelete(), "collagePdfClear");
    d.clear->setAccentRole(AdButton::AccentRole::Danger);
    d.selectionToolbar = new CanvasActionToolbar(this);
    d.selectionToolbar->setObjectName(QStringLiteral("collagePdfImageToolbar"));
    d.selectionToolbar->hide();
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
        "collagePdfRotateRight",  "collagePdfRotateLeft",     "collagePdfFlipHorizontal",
        "collagePdfFlipVertical", "collagePdfResetTransform", "collagePdfReplaceImage",
        "collagePdfPreviewImage", "collagePdfDeleteSelected"};
    for (std::size_t index = 0; index < d.selectionActions.size(); ++index) {
        if (index == 5)
            d.selectionToolbar->addSeparator();
        d.selectionActions[index] = d.selectionToolbar->addAction(refs[index], names[index]);
        connect(d.selectionActions[index], &AdButton::clicked, this, [this, index] {
            auto& state = *m_impl;
            const auto id = state.document->selectedId();
            if (!state.indexById.contains(id))
                return;
            if (index == 6) {
                state.openPreview(id);
                return;
            }
            if (state.exporting)
                return;
            if (index < 2)
                state.document->rotate(id, index == 0);
            else if (index < 4)
                state.document->flip(id, index == 2);
            else if (index == 4)
                state.document->resetTransform(id);
            else if (index == 5) {
                if (!state.loading)
                    emit replaceImageRequested(id);
            } else
                state.deleteItem(id);
        });
    }
    d.selectionActions.back()->setAccentRole(AdButton::AccentRole::Danger);
    connect(d.reverse, &AdButton::clicked, this, [this] {
        if (!m_impl->loading && !m_impl->exporting)
            m_impl->document->reverseItems();
    });
    connect(d.clear, &AdButton::clicked, this, &CollagePdfPageGrid::clearRequested);
    connect(d.add, &AdButton::clicked, this, &CollagePdfPageGrid::addImagesRequested);
    d.empty = new ImageDropSurface(false, this);
    d.empty->setObjectName(QStringLiteral("collagePdfEmptyState"));
    auto* emptyLayout = static_cast<QVBoxLayout*>(d.empty->layout());
    auto* emptyAdd = new AdButton(d.empty);
    emptyAdd->setObjectName(QStringLiteral("collagePdfEmptyAddImages"));
    emptyAdd->setIconRef(icons::Plus());
    emptyAdd->setButtonStyle(AdButton::ButtonStyle::Solid);
    emptyAdd->setAccentRole(AdButton::AccentRole::Primary);
    emptyAdd->setFocusPolicy(Qt::StrongFocus);
    emptyLayout->insertSpacing(emptyLayout->count() - 1, 8);
    emptyLayout->insertWidget(emptyLayout->count() - 1, emptyAdd, 0, Qt::AlignHCenter);
    connect(emptyAdd, &AdButton::clicked, this, &CollagePdfPageGrid::addImagesRequested);
    d.dropHint = new ImageDropSurface(true, this);
    d.dropHint->setObjectName(QStringLiteral("collagePdfDropHint"));
    d.dropHint->hide();
    d.marker = new InsertionMarker(d.scroll->viewport());
    d.viewer = new AdImageViewer(this);
    d.viewer->setObjectName(QStringLiteral("collagePdfOriginalViewer"));
    d.viewer->setOwnerWindow(this);
    d.loader = new MemoryImageLoader(this);
    d.loader->thumbnailCacheChanged = [this] {
        setProperty("pageThumbnailBytes", m_impl->loader->thumbnailBytes());
    };
    d.viewer->setImageLoader(d.loader);
    d.model = new AdImageListModel(this);
    d.model->setObjectName(QStringLiteral("collagePdfOriginalModel"));
    d.viewer->setModel(d.model);
    d.refreshTimer = new QTimer(this);
    d.refreshTimer->setSingleShot(true);
    d.refreshTimer->setInterval(0);
    connect(d.refreshTimer, &QTimer::timeout, this, [this] { m_impl->refresh(); });
    d.scrollTimer = new QTimer(this);
    d.scrollTimer->setInterval(16);
    connect(d.scrollTimer, &QTimer::timeout, this, [this] { m_impl->scrollDrag(); });
    connect(document, &CollageDocument::changed, this, [this] {
        if (isVisible())
            m_impl->refreshTimer->start();
    });
    connect(document, &CollageDocument::selectionChanged, this, [this] {
        for (auto* card : m_impl->cards)
            card->update();
        m_impl->positionSelectionToolbar();
    });
    connect(d.scroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { m_impl->refreshVisible(); });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this] {
                const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
                auto palette = m_impl->validationNotice->palette();
                palette.setColor(QPalette::WindowText, theme.colorTextSecondary);
                m_impl->validationNotice->setPalette(palette);
                m_impl->empty->refreshTheme();
                m_impl->dropHint->refreshTheme();
                for (auto* card : m_impl->cards)
                    card->update();
            });
    retranslateUi();
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
    auto noticePalette = d.validationNotice->palette();
    noticePalette.setColor(QPalette::WindowText, theme.colorTextSecondary);
    d.validationNotice->setPalette(noticePalette);
    d.refresh();
}

CollagePdfPageGrid::~CollagePdfPageGrid() {
    auto& d = *m_impl;
    *d.alive = false;
    d.refreshTimer->stop();
    d.scrollTimer->stop();
    disconnect(d.refreshTimer, nullptr, this, nullptr);
    disconnect(d.scrollTimer, nullptr, this, nullptr);
    disconnect(d.scroll->verticalScrollBar(), nullptr, this, nullptr);
    disconnect(&adqt::theme::ThemeManager::instance(), nullptr, this, nullptr);
    if (d.document)
        disconnect(d.document, nullptr, this, nullptr);
    // QObject destroys QWidget children after the Impl has retired. Focus can
    // move between those children during teardown, so detach their callbacks now.
    for (auto* card : d.cards) {
        card->selected = {};
        card->removeRequested = {};
        card->previewRequested = {};
        card->dragRequested = {};
        card->navigateRequested = {};
        card->releaseThumbnail();
    }
    d.viewer->close();
    d.loader->thumbnailCacheChanged = {};
    d.loader->clearThumbnails();
    d.loader->setItems({});
}

void CollagePdfPageGrid::setLoading(bool loading) {
    m_impl->loading = loading;
    m_impl->enabled();
    if (auto* add = findChild<AdButton*>(QStringLiteral("collagePdfEmptyAddImages"))) {
        add->setEnabled(!loading && !m_impl->exporting);
        add->setBusy(loading);
    }
}

void CollagePdfPageGrid::setExportBusy(bool busy) {
    m_impl->exporting = busy;
    m_impl->enabled();
    if (auto* add = findChild<AdButton*>(QStringLiteral("collagePdfEmptyAddImages")))
        add->setEnabled(!busy && !m_impl->loading);
}

void CollagePdfPageGrid::retranslateUi() {
    auto& d = *m_impl;
    setAccessibleName(gridText("PDF document pages"));
    d.validationNotice->setText(gridText("The page preview shows the last valid settings."));
    setAccessibleDescription(gridText(
        "Drag pages to reorder. Use the arrow keys to select a page and Enter to preview the "
        "original image."));
    d.reverse->setToolTip(gridText("Reverse page order"));
    d.add->setToolTip(gridText("Add images"));
    d.clear->setToolTip(gridText("Clear"));
    for (auto* action : {d.reverse, d.add, d.clear})
        action->setAccessibleName(action->toolTip());
    constexpr std::array<const char*, 8> actions{
        "Rotate clockwise by 90°", "Rotate counterclockwise by 90°",
        "Flip horizontally",       "Flip vertically",
        "Reset transform",         "Replace image",
        "Preview original image",  "Delete image"};
    for (std::size_t index = 0; index < d.selectionActions.size(); ++index) {
        d.selectionActions[index]->setToolTip(gridText(actions[index]));
        d.selectionActions[index]->setAccessibleName(d.selectionActions[index]->toolTip());
    }
    d.empty->title->setText(gridText("Add images to create a PDF document"));
    d.empty->description->setText(gridText("Drag image files here, or choose Add images"));
    if (auto* add = findChild<AdButton*>(QStringLiteral("collagePdfEmptyAddImages"))) {
        add->setText(gridText("Add images"));
        add->setAccessibleName(add->text());
    }
    d.dropHint->title->setText(gridText("Drop images to add pages"));
    d.dropHint->description->setText(gridText("Add multiple image files at once"));
    d.dropHint->setAccessibleName(d.dropHint->title->text());
    d.dropHint->setAccessibleDescription(d.dropHint->description->text());
    for (auto* card : d.cards)
        card->retranslate();
}

bool CollagePdfPageGrid::eventFilter(QObject* watched, QEvent* event) {
    auto& d = *m_impl;
    if (watched == d.scroll->viewport()) {
        if (event->type() == QEvent::DragEnter) {
            auto* drag = static_cast<QDragEnterEvent*>(event);
            d.dragEnter(drag, drag->position());
            return drag->isAccepted();
        }
        if (event->type() == QEvent::DragMove) {
            auto* drag = static_cast<QDragMoveEvent*>(event);
            d.dragMove(drag, drag->position());
            return drag->isAccepted();
        }
        if (event->type() == QEvent::DragLeave) {
            d.cancelDrag();
            d.dropHint->hide();
            event->accept();
            return true;
        }
        if (event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            d.drop(drop, drop->position());
            return drop->isAccepted();
        }
        if (event->type() == QEvent::Resize && d.refreshTimer)
            d.refreshTimer->start();
    }
    return QWidget::eventFilter(watched, event);
}

void CollagePdfPageGrid::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layout()->activate();
    m_impl->refresh();
}

void CollagePdfPageGrid::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_impl->refresh();
}

void CollagePdfPageGrid::hideEvent(QHideEvent* event) {
    m_impl->cancelDrag();
    m_impl->refreshTimer->stop();
    m_impl->dropHint->hide();
    m_impl->selectionToolbar->hide();
    m_impl->viewer->close();
    for (auto it = m_impl->cards.begin(); it != m_impl->cards.end();) {
        it.value()->hide();
        it.value()->releaseThumbnail();
        if (it.key() == m_impl->activeDragId) {
            ++it;
        } else {
            it.value()->deleteLater();
            it = m_impl->cards.erase(it);
        }
    }
    setProperty("materializedPageCount", m_impl->cards.size());
    m_impl->loader->setItems({});
    m_impl->loader->retainThumbnails({});
    m_impl->model->setItems({});
    m_impl->previewItems.clear();
    m_impl->originalKeys.clear();
    m_impl->contentRevision = std::numeric_limits<quint64>::max();
    QWidget::hideEvent(event);
}

void CollagePdfPageGrid::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void CollagePdfPageGrid::dragEnterEvent(QDragEnterEvent* event) {
    const auto viewport = m_impl->scroll->viewport()->mapTo(this, QPoint{});
    m_impl->dragEnter(event, event->position() - viewport);
}

void CollagePdfPageGrid::dragMoveEvent(QDragMoveEvent* event) {
    const auto viewport = m_impl->scroll->viewport()->mapTo(this, QPoint{});
    m_impl->dragMove(event, event->position() - viewport);
}

void CollagePdfPageGrid::dragLeaveEvent(QDragLeaveEvent* event) {
    m_impl->cancelDrag();
    m_impl->dropHint->hide();
    event->accept();
}

void CollagePdfPageGrid::dropEvent(QDropEvent* event) {
    const auto viewport = m_impl->scroll->viewport()->mapTo(this, QPoint{});
    m_impl->drop(event, event->position() - viewport);
}

} // namespace snow_shot::presentation::components
