// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "collagepreviewrows_p.h"
#include "snow_shot/presentation/components/collagepdfpagegrid.h"
#include "snow_shot/presentation/components/collagepdfsettingswidget.h"
#include "snow_shot/presentation/components/collagepdftemplatewidget.h"

#include "snow_shot/presentation/screenshotwheelinput.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/components/overlaycontrolbutton.h"
#include "snow_shot/presentation/components/canvasactiontoolbar.h"
#include "snow_shot/presentation/components/imagedropsurface.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "antd_icons.h"
#include "icon_renderer.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/divider.h"
#include "widgets/form.h"
#include "widgets/input_number.h"
#include "widgets/scroll_area.h"
#include "widgets/segmented.h"
#include "snowimageqtcodec.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QApplication>
#include <QCoreApplication>
#include <QColorSpace>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRgba64>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include "collageresourcebudget_p.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace snow_shot::presentation::components {
namespace {
using adqt::widgets::AdButton;
using adqt::widgets::AdColorPicker;
using adqt::widgets::AdColorValue;
using adqt::widgets::AdDivider;
using adqt::widgets::AdForm;
using adqt::widgets::AdFormItem;
using adqt::widgets::AdInputNumber;
using adqt::widgets::AdScrollArea;
using adqt::widgets::AdSegmented;
namespace icons = adqt::icons::antd::outlined;
constexpr int kSettingsFormWidth = 340;
constexpr int kSettingsFormRightMargin = 12;
constexpr auto kDragType = "application/x-snow-shot-collage-item";
const QSize kThumbnailSize{108, 84};
constexpr int kThumbnailMargin = 2;
constexpr qreal kSelectionFrameWidth = 3;
// Match snow-draw-engine-editor's selection colors in geometry/handles.rs.
const QColor kSelectionFrameColor{0x40, 0x96, 0xff};
const QColor kReplacementFillColor{0x40, 0x96, 0xff, 0x33};
constexpr std::array<const char*, 8> kColorValues{"#FFFFFF", "#000000", "#8C8C8C", "#FFCCC7",
                                                  "#D9F7BE", "#BAE0FF", "#FFF1B8", "#EFDBFF"};
// These strings also supply labels constructed dynamically by the form and toolbar.
[[maybe_unused]] constexpr std::array kTranslations{
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Replace image"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Delete image"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Image %1: %2"),
    QT_TRANSLATE_NOOP("CollageEditorWidget",
                      "Select to center this image; drag onto another image to swap"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Collage preview"),
    QT_TRANSLATE_NOOP("CollageEditorWidget",
                      "Drag with the middle mouse button to pan; scroll to zoom"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Rotate clockwise by 90°"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Rotate counterclockwise by 90°"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Flip horizontally"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Flip vertically"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Reset transform"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Add images"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Add images to create a collage"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Drag image files here, or choose add images"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Drop images to add to collage"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Add multiple image files at once"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "%1 × %2 px"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Total: %1 images"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Vertical"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Horizontal"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Custom"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Rows"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Columns"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Uniform size"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Original size"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Layout direction"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Image type"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Static image"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "PDF document"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "%1 × %2 mm"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Total: %1 pages"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Image sizing"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Outer border"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Image spacing"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Background color"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Outer border in output pixels"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Image spacing in output pixels"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Outer border unit"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Image spacing unit"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Outer border in percentage"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Image spacing in percentage"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Pixel"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Percentage"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Percentage of the widest image after sizing"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Percentage of the tallest image after sizing"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Percentage of the grid cell width"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Choose a background color, including transparency"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "White"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Black"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Gray"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Red"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Green"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Blue"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Yellow"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Purple"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Fit to canvas"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Clear all"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", "Reverse image order"),
    QT_TRANSLATE_NOOP("CollageEditorWidget", " px"),
};

QString text(const char* value) {
    return QCoreApplication::translate("CollageEditorWidget", value);
}

bool sameLayoutSettings(CollageSettings first, CollageSettings second) {
    first.background = second.background;
    return first == second;
}

// Large enough for Qt's parallel SIMD converter/scaler, independent of source raster height.
constexpr qint64 maximumAreaStripBytes = 8 * 1024 * 1024;

int areaStripRows(const QImage& image, QRect crop, QSize output) {
    if (output.width() > crop.width() / 4 || output.height() > crop.height() / 4)
        return 0;
    // The native crop, ICC conversion and high-precision horizontal result coexist.
    // Qt's native FP16 color transform expands to FP32 before restoring its input format.
    const bool halfFloat = image.format() == QImage::Format_RGBX16FPx4 ||
                           image.format() == QImage::Format_RGBA16FPx4 ||
                           image.format() == QImage::Format_RGBA16FPx4_Premultiplied;
    const qint64 row = qint64(crop.width()) * std::max(halfFloat ? 16 : 8, image.depth() / 8) * 3;
    return static_cast<int>(std::min<qint64>(64, maximumAreaStripBytes / row));
}

QImage areaDetail(const QImage& image, QRect crop, QSize output, int stripRows,
                  const ScreenshotExportCancellation& cancellation) {
    QImage reduced = snowCanvasAllocateImage(output, QImage::Format_RGBA64_Premultiplied);
    if (reduced.isNull())
        return {};
    std::vector<std::uint64_t> sums(size_t(output.width()) * 4, 0);
    int targetRow = 0;
    for (int first = 0; first < crop.height(); first += stripRows) {
        if (cancellation.isCancellationRequested())
            return {};
        const int rows = std::min(stripRows, crop.height() - first);
        QImage strip = image.copy(QRect(crop.left(), crop.top() + first, crop.width(), rows));
        if (strip.isNull())
            return {};
        if (strip.colorSpace().isValid() && strip.colorSpace() != QColorSpace(QColorSpace::SRgb))
            strip = strip.convertedToColorSpace(QColorSpace::SRgb);
        strip = strip.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        strip = strip.scaled(output.width(), rows, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (strip.isNull())
            return {};
        for (int local = 0; local < rows; ++local) {
            if (cancellation.isCancellationRequested())
                return {};
            const auto* pixels = reinterpret_cast<const QRgba64*>(strip.constScanLine(local));
            // Integer source/target overlap gives each output row exactly crop.height() weight.
            qint64 cursor = qint64(first + local) * output.height();
            const qint64 end = cursor + output.height();
            while (cursor < end) {
                const qint64 boundary = qint64(targetRow + 1) * crop.height();
                const auto weight = static_cast<std::uint64_t>(std::min(end, boundary) - cursor);
                for (int x = 0; x < output.width(); ++x) {
                    auto* sum = sums.data() + size_t(x) * 4;
                    sum[0] += std::uint64_t(pixels[x].red()) * weight;
                    sum[1] += std::uint64_t(pixels[x].green()) * weight;
                    sum[2] += std::uint64_t(pixels[x].blue()) * weight;
                    sum[3] += std::uint64_t(pixels[x].alpha()) * weight;
                }
                cursor += qint64(weight);
                if (cursor == boundary) {
                    auto* target = reinterpret_cast<QRgba64*>(reduced.scanLine(targetRow));
                    for (int x = 0; x < output.width(); ++x) {
                        const auto* sum = sums.data() + size_t(x) * 4;
                        const auto channel = [sum, height = crop.height()](int index) {
                            return static_cast<quint16>((sum[index] + unsigned(height) / 2) /
                                                        unsigned(height));
                        };
                        target[x] =
                            QRgba64::fromRgba64(channel(0), channel(1), channel(2), channel(3));
                    }
                    std::fill(sums.begin(), sums.end(), 0);
                    ++targetRow;
                }
            }
        }
    }
    return cancellation.isCancellationRequested()
               ? QImage{}
               : reduced.convertToFormat(QImage::Format_RGBA8888);
}

class DetailWorkingLifetime final {
  public:
    DetailWorkingLifetime(std::shared_ptr<void> reservation,
                          std::shared_ptr<std::atomic<qint64>> total, qint64 bytes,
                          CollagePreviewCanvas* owner)
        : m_reservation(std::move(reservation)), m_total(std::move(total)), m_bytes(bytes),
          m_owner(owner) {
        m_total->fetch_add(m_bytes);
    }
    ~DetailWorkingLifetime() {
        m_total->fetch_sub(m_bytes);
        // Work captures can retire after cancellation or window destruction, independently of
        // completion delivery. Keep diagnostics tied to the reservation's actual lifetime.
        if (m_owner)
            QMetaObject::invokeMethod(
                m_owner,
                [owner = m_owner, total = m_total] {
                    if (owner)
                        owner->setProperty("detailWorkingBytes", total->load());
                },
                Qt::QueuedConnection);
    }

  private:
    std::shared_ptr<void> m_reservation;
    std::shared_ptr<std::atomic<qint64>> m_total;
    qint64 m_bytes;
    QPointer<CollagePreviewCanvas> m_owner;
};

quint64 dragItem(const QMimeData* mime, const CollageDocument* document) {
    if (!mime || !mime->hasFormat(QString::fromLatin1(kDragType)))
        return 0;
    bool valid = false;
    const auto parts = mime->data(QString::fromLatin1(kDragType)).split(':');
    if (parts.size() != 2 || parts[0].toULongLong() != reinterpret_cast<quintptr>(document))
        return 0;
    const auto value = parts[1].toULongLong(&valid);
    return valid ? value : 0;
}

QStringList droppedImageFiles(const QMimeData* mime) {
    // Internal thumbnail drags belong to the existing swap interaction.
    if (!mime || !mime->hasUrls() || mime->hasFormat(QString::fromLatin1(kDragType)))
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

QPixmap checkerboard(const QWidget* widget) {
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(widget);
    QPixmap tile(20, 20);
    tile.fill(theme.colorBgContainer);
    QPainter painter(&tile);
    painter.fillRect(0, 0, 10, 10, theme.colorFillSecondary);
    painter.fillRect(10, 10, 10, 10, theme.colorFillSecondary);
    return tile;
}

void drawTransformedPreview(QPainter& painter, const QRectF& target, const QImage& image,
                            const QTransform& transform) {
    if (image.isNull() || target.isEmpty())
        return;
    const auto transformed = transform.mapRect(QRectF(QPointF{}, image.size()));
    painter.save();
    painter.translate(target.topLeft());
    painter.scale(target.width() / transformed.width(), target.height() / transformed.height());
    painter.translate(-transformed.topLeft());
    painter.setTransform(transform, true);
    painter.drawImage(QPointF{}, image);
    painter.restore();
}

class CollageColorPresetButton final : public AdButton {
  public:
    CollageColorPresetButton(const QColor& color, QWidget* parent)
        : AdButton(parent), m_color(color) {
        setSizeClass(AdButton::SizeClass::Small);
        setShape(AdButton::Shape::Rounded);
        setButtonStyle(AdButton::ButtonStyle::Text);
        setFixedSize(28, 28);
        setCheckable(true);
        setCheckedUsesActiveStyle(false);
        setFocusPolicy(Qt::StrongFocus);
    }

    void setSelected(bool selected) {
        setChecked(selected);
        setButtonStyle(selected ? AdButton::ButtonStyle::Tonal : AdButton::ButtonStyle::Text);
        setAccentRole(selected ? AdButton::AccentRole::Primary : AdButton::AccentRole::Neutral);
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        AdButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (!isEnabled())
            painter.setOpacity(.4);
        const QRectF swatch(QRectF(rect()).center() - QPointF(11, 11), QSizeF(22, 22));
        QPainterPath path;
        path.addRoundedRect(swatch, 3, 3);
        painter.fillPath(path, m_color);
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        painter.setPen(QPen(theme.colorBorder, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

  private:
    QColor m_color;
};

class CollageThumbnail final : public QWidget {
  public:
    CollageThumbnail(CollageDocument* document, quint64 id, QWidget* parent)
        : QWidget(parent), m_document(document), m_id(id) {
        setObjectName(QStringLiteral("collageThumbnail_%1").arg(id));
        setProperty("collageItemId", QVariant::fromValue(id));
        setFixedSize(kThumbnailSize);
        setFocusPolicy(Qt::StrongFocus);
        setAcceptDrops(true);
        setMouseTracking(true);
        m_replace = new OverlayControlButton(
            OverlayControlButton::Intent::Primary,
            snow_shot::presentation::icons::custom::outlined::ReplaceImage(), this);
        m_replace->setObjectName(QStringLiteral("collageReplaceImage"));
        m_remove = new OverlayControlButton(OverlayControlButton::Intent::Destructive,
                                            icons::IconDelete(), this);
        m_remove->setObjectName(QStringLiteral("collageDeleteImage"));
        for (auto* button : {m_replace, m_remove}) {
            button->setSizeClass(AdButton::SizeClass::Small);
            button->setFixedSize(28, 28);
            button->setIconSize(QSize(16, 16));
            button->setFocusPolicy(Qt::StrongFocus);
        }
        constexpr int actionMargin = 4;
        constexpr int actionSpacing = 4;
        m_remove->move(width() - m_remove->width() - actionMargin,
                       height() - m_remove->height() - actionMargin);
        m_replace->move(m_remove->x() - m_replace->width() - actionSpacing, m_remove->y());
        m_replace->installEventFilter(this);
        m_remove->installEventFilter(this);
        connect(m_replace, &QAbstractButton::clicked, this, [this] {
            if (editStarted)
                editStarted();
            if (replaceRequested)
                replaceRequested(m_id);
        });
        connect(m_remove, &QAbstractButton::clicked, this, [this] {
            if (editStarted)
                editStarted();
            m_document->remove(m_id);
        });
        retranslate();
        reveal();
    }
    std::function<void(quint64)> selected;
    std::function<void(quint64)> replaceRequested;
    std::function<void()> editStarted;
    void setEditingEnabled(bool enabled, bool replacementAllowed) {
        m_editing = enabled;
        m_replace->setEnabled(enabled && replacementAllowed);
        m_remove->setEnabled(enabled);
    }
    void setItem(const CollageItem& item, int ordinal, bool force = false) {
        if (!force && m_revision == item.revision && m_ordinal == ordinal && m_label == item.label)
            return;
        if (m_revision != item.revision || m_preview.isNull()) {
            m_revision = item.revision;
            m_preview = item.preview;
            m_transform = item.transform;
        }
        m_label = item.label;
        m_ordinal = ordinal;
        retranslate();
        update();
    }
    void retranslate() {
        m_replace->setToolTip(text("Replace image"));
        m_replace->setAccessibleName(m_replace->toolTip());
        m_remove->setToolTip(text("Delete image"));
        m_remove->setAccessibleName(m_remove->toolTip());
        setAccessibleName(text("Image %1: %2").arg(m_ordinal).arg(m_label));
        setAccessibleDescription(
            text("Select to center this image; drag onto another image to swap"));
        setToolTip(m_label);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF outer = QRectF(rect()).adjusted(1, 1, -1, -1);
        painter.setBrush(theme.colorBgContainer);
        painter.setPen(
            QPen(m_target ? kSelectionFrameColor : theme.colorBorderSecondary, m_target ? 3 : 1));
        painter.drawRoundedRect(outer, theme.borderRadiusSM, theme.borderRadiusSM);
        const QRectF imageArea = outer.adjusted(5, 5, -5, -20);
        painter.fillRect(imageArea, QBrush(checkerboard(this)));
        if (!m_preview.isNull()) {
            const auto size = m_transform.mapRect(QRectF(QPointF{}, m_preview.size()))
                                  .size()
                                  .scaled(imageArea.size(), Qt::KeepAspectRatio);
            const QRectF target(imageArea.center() - QPointF(size.width() / 2, size.height() / 2),
                                size);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            drawTransformedPreview(painter, target, m_preview, m_transform);
        }
        painter.setPen(theme.colorTextSecondary);
        painter.drawText(QRect(5, height() - 20, m_replace->x() - 10, 17), Qt::AlignCenter,
                         QString::number(m_ordinal));
        if (m_document->selectedId() == m_id || hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(kSelectionFrameColor, 2));
            painter.drawRoundedRect(outer, theme.borderRadiusSM, theme.borderRadiusSM);
        }
    }
    void enterEvent(QEnterEvent* event) override {
        m_hovered = true;
        reveal();
        QWidget::enterEvent(event);
    }
    void leaveEvent(QEvent* event) override {
        m_hovered = false;
        reveal();
        QWidget::leaveEvent(event);
    }
    void focusInEvent(QFocusEvent* event) override {
        reveal();
        update();
        QWidget::focusInEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        QTimer::singleShot(0, this, [this] {
            reveal();
            update();
        });
        QWidget::focusOutEvent(event);
    }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
            QTimer::singleShot(0, this, [this] { reveal(); });
        return QWidget::eventFilter(object, event);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_press = event->position();
            setFocus(Qt::MouseFocusReason);
            if (selected)
                selected(m_id);
            event->accept();
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton) || !m_editing ||
            (event->position() - m_press).manhattanLength() < QApplication::startDragDistance())
            return;
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData;
        mime->setData(QString::fromLatin1(kDragType),
                      QByteArray::number(reinterpret_cast<quintptr>(m_document)) + ':' +
                          QByteArray::number(m_id));
        drag->setMimeData(mime);
        drag->setPixmap(QPixmap::fromImage(m_preview).scaled(80, 64, Qt::KeepAspectRatio,
                                                             Qt::SmoothTransformation));
        drag->exec(Qt::MoveAction);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return) {
            if (selected)
                selected(m_id);
            event->accept();
        } else if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
                   m_editing) {
            m_document->remove(m_id);
            event->accept();
        } else {
            QWidget::keyPressEvent(event);
        }
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (m_editing && dragItem(event->mimeData(), m_document) != 0 &&
            dragItem(event->mimeData(), m_document) != m_id) {
            m_target = true;
            update();
            event->acceptProposedAction();
        }
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent*) override {
        m_target = false;
        update();
    }
    void dropEvent(QDropEvent* event) override {
        const auto source = dragItem(event->mimeData(), m_document);
        m_target = false;
        if (m_editing && source != 0 && m_document->swapItems(source, m_id))
            event->acceptProposedAction();
        update();
    }

  private:
    void reveal() {
        const bool visible =
            m_hovered || hasFocus() || m_replace->hasFocus() || m_remove->hasFocus();
        m_replace->setVisible(visible);
        m_remove->setVisible(visible);
    }
    CollageDocument* m_document;
    quint64 m_id;
    quint64 m_revision = std::numeric_limits<quint64>::max();
    QImage m_preview;
    QTransform m_transform;
    QString m_label;
    QPointF m_press;
    int m_ordinal = 1;
    bool m_hovered = false;
    bool m_target = false;
    bool m_editing = true;
    AdButton* m_replace = nullptr;
    AdButton* m_remove = nullptr;
};
} // namespace

struct CollagePreviewCanvas::Impl {
    struct Detail {
        QImage image;
        QRectF transformedPixels;
        QSize fullSize;
        quint64 revision = 0;
        quint64 used = 0;
        qreal resolution = 0;
        std::shared_ptr<void> reservation;
    };
    CollagePreviewCanvas* owner;
    CollageDocument* document;
    CollageLayout layout;
    CollageSettings cachedSettings;
    bool initialized = false;
    QVector<quint64> order;
    QHash<quint64, qsizetype> indexById;
    QHash<quint64, QImage> previews;
    QHash<quint64, QTransform> previewTransforms;
    QHash<quint64, quint64> revisions;
    QHash<quint64, Detail> details;
    struct DetailJob {
        ScreenshotExportJobHandle handle;
        quint64 ticket = 0;
        quint64 revision = 0;
    };
    QHash<quint64, DetailJob> detailJobs;
    QHash<quint64, qint64> pendingDetailBytes;
    qint64 basePreviewBytes = 0;
    qint64 detailCacheBytes = 0;
    qint64 pendingPreviewBytes = 0;
    std::shared_ptr<std::atomic<qint64>> workingBytes = std::make_shared<std::atomic<qint64>>(0);
    QTimer* detailTimer = nullptr;
    quint64 nextDetailTicket = 0;
    quint64 detailClock = 0;
    QPixmap checker;
    qreal zoom = 1;
    QPointF offset;
    QPointF last;
    QPointF press;
    quint64 pressedId = 0;
    quint64 targetId = 0;
    bool panning = false;
    bool dragging = false;
    bool automaticFit = true;
    bool editing = true;
    bool rendering = true;
    CanvasActionToolbar* toolbar = nullptr;
    std::array<AdButton*, 6> actions{};
    OverlayControlButton* fit = nullptr;

    void cancelDetails() {
        for (const auto& job : detailJobs)
            job.handle.cancel();
        detailJobs.clear();
    }
    qint64 previewBytes() const {
        return basePreviewBytes + detailCacheBytes + pendingPreviewBytes;
    }
    void previewStatisticsChanged() {
        owner->setProperty("detailPreviewBytes", previewBytes());
        owner->setProperty("detailTileCount", details.size());
        owner->setProperty("detailPendingCount", pendingDetailBytes.size());
        owner->setProperty("detailWorkingBytes", workingBytes->load());
    }
    QRectF detailViewport(const Detail& detail, const QRectF& item) const {
        return {
            item.left() + item.width() * detail.transformedPixels.left() / detail.fullSize.width(),
            item.top() + item.height() * detail.transformedPixels.top() / detail.fullSize.height(),
            item.width() * detail.transformedPixels.width() / detail.fullSize.width(),
            item.height() * detail.transformedPixels.height() / detail.fullSize.height()};
    }
    void requestDetails() {
        if (!rendering || document->outputType() == CollageOutputType::PdfDocument ||
            !layout.isValid())
            return;
        for (qsizetype index = 0; index < document->items().size(); ++index) {
            const auto& item = document->items()[index];
            const auto bounds = viewportRect(index);
            const auto visible = bounds.intersected(owner->rect());
            if (visible.isEmpty() || detailJobs.contains(item.id))
                continue;
            const auto fullSize = transformedSize(item);
            const qreal screenScale = bounds.width() / fullSize.width();
            const qreal screenScaleY = bounds.height() / fullSize.height();
            const qreal desiredScale =
                std::min(1.0, std::max(screenScale, screenScaleY) * owner->devicePixelRatioF());
            const auto smallSize =
                item.transform.mapRect(QRectF(QPointF{}, item.preview.size())).size();
            if (bounds.width() * owner->devicePixelRatioF() <= smallSize.width() * 1.25 &&
                bounds.height() * owner->devicePixelRatioF() <= smallSize.height() * 1.25)
                continue;
            // Cancelled requests still own preview storage until completion retires it.
            if (pendingDetailBytes.size() >= 2)
                break;
            const auto padded = visible.adjusted(-48, -48, 48, 48).intersected(bounds);
            const QRectF pixels((padded.left() - bounds.left()) / screenScale,
                                (padded.top() - bounds.top()) / screenScaleY,
                                padded.width() / screenScale, padded.height() / screenScaleY);
            const auto sourceBounds = item.transform.mapRect(QRectF(QPointF{}, item.image.size()));
            QTransform normalized = item.transform;
            normalized.setMatrix(normalized.m11(), normalized.m12(), normalized.m13(),
                                 normalized.m21(), normalized.m22(), normalized.m23(),
                                 normalized.dx() - sourceBounds.left(),
                                 normalized.dy() - sourceBounds.top(), normalized.m33());
            const QRect crop = normalized.inverted().mapRect(pixels).toAlignedRect().intersected(
                QRect(QPoint{}, item.image.size()));
            if (crop.isEmpty())
                continue;
            const QRectF transformedCrop = normalized.mapRect(QRectF(crop));
            const QSize wanted(std::max(1, qCeil(transformedCrop.width() * desiredScale)),
                               std::max(1, qCeil(transformedCrop.height() * desiredScale)));
            const auto tileSize = wanted.width() > 2048 || wanted.height() > 2048
                                      ? wanted.scaled(QSize(2048, 2048), Qt::KeepAspectRatio)
                                      : wanted;
            const qreal scale = std::min(qreal(tileSize.width()) / transformedCrop.width(),
                                         qreal(tileSize.height()) / transformedCrop.height());
            const QSize unrotated(std::max(1, qCeil(crop.width() * scale)),
                                  std::max(1, qCeil(crop.height() * scale)));
            const auto transformedTile =
                item.transform.mapRect(QRectF(QPointF{}, unrotated)).size();
            const qreal resolution = transformedTile.width() / transformedCrop.width();
            auto existing = details.find(item.id);
            if (existing != details.end() && existing->revision == item.revision &&
                existing->resolution >= resolution * .9 &&
                detailViewport(*existing, bounds).contains(visible)) {
                existing->used = ++detailClock;
                continue;
            }
            const qint64 bytes = qint64(unrotated.width()) * unrotated.height() * 4;
            while (!details.isEmpty() &&
                   previewBytes() + bytes > collage_detail::maximumWindowPreviewBytes) {
                auto oldest = details.begin();
                for (auto it = details.begin(); it != details.end(); ++it)
                    if (it->used < oldest->used)
                        oldest = it;
                detailCacheBytes -= oldest->image.sizeInBytes();
                details.erase(oldest);
            }
            if (previewBytes() + bytes > collage_detail::maximumWindowPreviewBytes)
                continue;
            const int stripRows = areaStripRows(item.image, crop, unrotated);
            const auto resizeBytes = stripRows > 0
                                         ? std::optional<qint64>(0)
                                         : image_codec::resizeWorkingBytes(crop.size(), unrotated);
            if (!resizeBytes)
                continue;
            // Area reduction retains native strips, a high-precision output and its accumulator.
            // Native resize retains its sRGB cache, Qt conversion tiles and a possible mono row.
            // Both paths also allow a transform result while it replaces the reduced raster.
            const qint64 scratchBytes =
                stripRows > 0
                    ? maximumAreaStripBytes + bytes * 3 + qint64(unrotated.width()) * 32
                    : *resizeBytes + 6 * 1024 * 1024 + qint64(crop.width() + 7) * 4 + bytes;
            auto reservation = reserveCollagePreview(bytes);
            auto working = collage_detail::reserveWorking(scratchBytes);
            if (!reservation || !working)
                continue;
            auto workLifetime = std::make_shared<DetailWorkingLifetime>(
                std::move(working), workingBytes, scratchBytes, owner);
            auto prepared = std::make_shared<Detail>();
            prepared->transformedPixels = transformedCrop;
            prepared->fullSize = fullSize;
            prepared->revision = item.revision;
            prepared->resolution = resolution;
            prepared->reservation = std::move(reservation);
            const quint64 ticket = ++nextDetailTicket;
            const auto job = ScreenshotExportCoordinator::shared().submit(
                owner, ScreenshotExportCoordinator::Priority::Background,
                [image = item.image, sourceOwner = item.resourceOwner, transform = item.transform,
                 crop, unrotated, stripRows, prepared,
                 workLifetime](const ScreenshotExportCancellation& cancellation) {
                    Q_UNUSED(sourceOwner);
                    Q_UNUSED(workLifetime);
                    if (cancellation.isCancellationRequested())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Cancelled, {});
                    QImage tile;
                    if (stripRows > 0) {
                        tile = areaDetail(image, crop, unrotated, stripRows, cancellation);
                    } else {
                        tile = snowCanvasAllocateImage(unrotated, QImage::Format_RGBA8888);
                        const auto rows = collage_detail::croppedRows(image, crop, cancellation);
                        if (!tile.isNull() &&
                            !image_codec::resizeToRgba8(rows, unrotated, tile.bits(),
                                                        tile.bytesPerLine(), tile.sizeInBytes()))
                            tile = {};
                    }
                    if (tile.isNull())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Render, {});
                    if (cancellation.isCancellationRequested())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Cancelled, {});
                    tile = snowCanvasTransformImage(tile, transform, Qt::FastTransformation);
                    if (tile.isNull())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Render, {});
                    tile.setDevicePixelRatio(1);
                    tile.setColorSpace(QColorSpace::SRgb);
                    if (cancellation.isCancellationRequested())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Cancelled, {});
                    prepared->image = std::move(tile);
                    return ScreenshotExportTaskResult{};
                },
                [this, id = item.id, ticket, prepared](const ScreenshotExportTaskResult& result) {
                    pendingPreviewBytes -= pendingDetailBytes.take(ticket);
                    const auto current = detailJobs.constFind(id);
                    const bool accepted = current != detailJobs.cend() && current->ticket == ticket;
                    if (accepted)
                        detailJobs.remove(id);
                    if (accepted && result.succeeded() &&
                        revisions.value(id) == prepared->revision && !prepared->image.isNull()) {
                        prepared->used = ++detailClock;
                        if (const auto cachedDetail = details.constFind(id);
                            cachedDetail != details.cend())
                            detailCacheBytes -= cachedDetail->image.sizeInBytes();
                        detailCacheBytes += prepared->image.sizeInBytes();
                        details.insert(id, std::move(*prepared));
                        owner->update();
                    }
                    previewStatisticsChanged();
                    if (!accepted || result.succeeded())
                        detailTimer->start();
                });
            if (job.isValid()) {
                detailJobs.insert(item.id, {job, ticket, item.revision});
                pendingDetailBytes.insert(ticket, bytes);
                pendingPreviewBytes += bytes;
            }
        }
        previewStatisticsChanged();
    }

    QRectF itemRect(quint64 id) const {
        const auto index = indexById.value(id, -1);
        return index >= 0 && index < layout.rects.size() ? QRectF(layout.rects[index]) : QRectF{};
    }
    QRectF viewportRect(quint64 id) const {
        const auto rect = itemRect(id);
        return rect.isEmpty() ? QRectF{}
                              : QRectF(offset + rect.topLeft() * zoom, rect.size() * zoom);
    }
    QRectF viewportRect(qsizetype index) const {
        if (index < 0 || index >= layout.rects.size())
            return {};
        const QRectF rect(layout.rects[index]);
        return {offset + rect.topLeft() * zoom, rect.size() * zoom};
    }
    quint64 hit(QPointF position) const {
        if (!layout.isValid() || zoom <= 0)
            return 0;
        const QPointF output = (position - offset) / zoom;
        for (qsizetype i = 0; i < layout.rects.size(); ++i)
            if (QRectF(layout.rects[i]).contains(output))
                return order[i];
        return 0;
    }
    void geometryChanged() {
        owner->setProperty("zoom", zoom);
        owner->setProperty("viewportOffset", offset);
        previewStatisticsChanged();
        const auto selected = viewportRect(document->selectedId());
        const bool layoutRecovery = !layout.isValid() && indexById.contains(document->selectedId());
        const bool showToolbar =
            editing &&
            (layoutRecovery || (!selected.isEmpty() && selected.intersects(owner->rect())));
        toolbar->setVisible(showToolbar);
        if (showToolbar) {
            toolbar->adjustSize();
            const int maximumX = std::max(8, owner->width() - toolbar->width() - 8);
            const int maximumY = std::max(8, owner->height() - toolbar->height() - 8);
            int x = layoutRecovery ? (owner->width() - toolbar->width()) / 2
                                   : qRound(selected.center().x() - toolbar->width() / 2.0);
            int y = layoutRecovery ? owner->height() - toolbar->height() - 16
                                   : qRound(selected.top() - toolbar->height() - 8);
            // Keep the image's top edge unobscured when a fitted image leaves a
            // useful side margin but no room above its floating controls.
            if (!layoutRecovery && y < 8 && selected.left() >= toolbar->width() + 16) {
                x = qRound(selected.left() - toolbar->width() - 8);
                y = qRound(selected.top());
            } else if (!layoutRecovery && y < 8 &&
                       selected.right() + toolbar->width() + 16 <= owner->width()) {
                x = qRound(selected.right() + 8);
                y = qRound(selected.top());
            }
            x = qBound(8, x, maximumX);
            y = qBound(8, y, maximumY);
            toolbar->move(x, y);
            toolbar->raise();
        }
        constexpr int fitInset = 12;
        fit->move(std::max(0, owner->width() - fit->width() - fitInset),
                  std::max(0, owner->height() - fit->height() - fitInset));
        fit->setEnabled(layout.isValid());
        fit->raise();
        owner->update();
        detailTimer->start();
    }
    void refresh() {
        if (!rendering || document->outputType() == CollageOutputType::PdfDocument) {
            detailTimer->stop();
            cancelDetails();
            return;
        }
        const bool firstRefresh = !initialized;
        const auto nextSettings = document->settings();
        if (initialized && nextSettings.background != cachedSettings.background &&
            sameLayoutSettings(nextSettings, cachedSettings) &&
            order.size() == document->items().size()) {
            cachedSettings = nextSettings;
            owner->update();
            return;
        }
        cachedSettings = nextSettings;
        initialized = true;
        const bool wasEmpty = order.isEmpty();
        const auto anchor = viewportRect(document->selectedId()).center();
        const bool anchored = !itemRect(document->selectedId()).isEmpty();
        const auto nextLayout = document->layout();
        const bool layoutChanged = nextLayout.size != layout.size ||
                                   nextLayout.rects != layout.rects ||
                                   nextLayout.error != layout.error;
        layout = nextLayout;
        QVector<quint64> nextOrder;
        nextOrder.reserve(document->items().size());
        QSet<quint64> retained;
        basePreviewBytes = 0;
        for (const auto& item : document->items()) {
            nextOrder.push_back(item.id);
            retained.insert(item.id);
            basePreviewBytes += item.preview.sizeInBytes();
            if (!revisions.contains(item.id) || revisions.value(item.id) != item.revision) {
                previews.insert(item.id, item.preview);
                previewTransforms.insert(item.id, item.transform);
                revisions.insert(item.id, item.revision);
            }
        }
        const bool orderChanged = nextOrder != order;
        if (orderChanged) {
            order = std::move(nextOrder);
            indexById.clear();
            for (qsizetype index = 0; index < order.size(); ++index)
                indexById.insert(order[index], index);
        }
        for (auto it = previews.begin(); it != previews.end();) {
            if (!retained.contains(it.key())) {
                revisions.remove(it.key());
                previewTransforms.remove(it.key());
                it = previews.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = details.begin(); it != details.end();) {
            if (!retained.contains(it.key()) || revisions.value(it.key()) != it->revision) {
                detailCacheBytes -= it->image.sizeInBytes();
                it = details.erase(it);
            } else
                ++it;
        }
        for (auto it = detailJobs.begin(); it != detailJobs.end();) {
            if (!retained.contains(it.key()) || revisions.value(it.key()) != it->revision) {
                it->handle.cancel();
                it = detailJobs.erase(it);
            } else {
                ++it;
            }
        }
        if (wasEmpty && !order.isEmpty())
            automaticFit = true;
        if (automaticFit && layoutChanged && layout.isValid()) {
            owner->fitToCanvas();
            return;
        } else if (!automaticFit && (layoutChanged || orderChanged) && anchored &&
                   !itemRect(document->selectedId()).isEmpty()) {
            offset += anchor - viewportRect(document->selectedId()).center();
        }
        if (document->items().isEmpty()) {
            automaticFit = true;
            offset = {};
            zoom = 1;
        }
        // Even an empty initial layout must project its state into newly created controls.
        if (firstRefresh || layoutChanged || orderChanged)
            this->geometryChanged();
        else {
            previewStatisticsChanged();
            owner->update();
            detailTimer->start();
        }
    }
};

CollagePreviewCanvas::CollagePreviewCanvas(CollageDocument* document, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.document = document;
    setObjectName(QStringLiteral("collagePreviewCanvas"));
    setMinimumSize(280, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    d.checker = checkerboard(this);
    d.detailTimer = new QTimer(this);
    d.detailTimer->setSingleShot(true);
    d.detailTimer->setInterval(70);
    connect(d.detailTimer, &QTimer::timeout, this, [this] { m_impl->requestDetails(); });
    d.toolbar = new CanvasActionToolbar(this);
    d.toolbar->setObjectName(QStringLiteral("collageImageToolbar"));
    const std::array<adqt::icons::IconRef, 6> refs{
        icons::RotateRight(),
        icons::RotateLeft(),
        snow_shot::presentation::icons::custom::outlined::FlipHorizontal(),
        snow_shot::presentation::icons::custom::outlined::FlipVertical(),
        icons::Reload(),
        icons::IconDelete()};
    const std::array<const char*, 6> names{
        "collageRotateClockwise", "collageRotateCounterclockwise", "collageFlipHorizontal",
        "collageFlipVertical",    "collageResetTransform",         "collageDeleteSelected"};
    for (std::size_t i = 0; i < d.actions.size(); ++i) {
        if (i == 5)
            d.toolbar->addSeparator();
        d.actions[i] = d.toolbar->addAction(refs[i], names[i]);
        connect(d.actions[i], &QAbstractButton::pressed, this, [this] { preserveViewport(); });
    }
    d.actions.back()->setAccentRole(AdButton::AccentRole::Danger);
    connect(d.actions[0], &QAbstractButton::clicked, this,
            [document] { document->rotate(document->selectedId(), true); });
    connect(d.actions[1], &QAbstractButton::clicked, this,
            [document] { document->rotate(document->selectedId(), false); });
    connect(d.actions[2], &QAbstractButton::clicked, this,
            [document] { document->flip(document->selectedId(), true); });
    connect(d.actions[3], &QAbstractButton::clicked, this,
            [document] { document->flip(document->selectedId(), false); });
    connect(d.actions[4], &QAbstractButton::clicked, this,
            [document] { document->resetTransform(document->selectedId()); });
    connect(d.actions[5], &QAbstractButton::clicked, this,
            [document] { document->remove(document->selectedId()); });
    d.fit =
        new OverlayControlButton(OverlayControlButton::Intent::Primary, icons::Fullscreen(), this);
    d.fit->setObjectName(QStringLiteral("collageFitToCanvas"));
    d.fit->setFocusPolicy(Qt::StrongFocus);
    connect(d.fit, &QAbstractButton::clicked, this, &CollagePreviewCanvas::fitToCanvas);
    connect(document, &CollageDocument::changed, this, [this] { m_impl->refresh(); });
    connect(document, &CollageDocument::selectionChanged, this,
            [this] { m_impl->geometryChanged(); });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this] {
                m_impl->checker = checkerboard(this);
                update();
            });
    retranslateUi();
    d.refresh();
}

CollagePreviewCanvas::~CollagePreviewCanvas() {
    m_impl->cancelDetails();
}
qreal CollagePreviewCanvas::zoom() const {
    return m_impl->zoom;
}
QPointF CollagePreviewCanvas::offset() const {
    return m_impl->offset;
}
QRectF CollagePreviewCanvas::imageViewportRect(quint64 id) const {
    return m_impl->viewportRect(id);
}
void CollagePreviewCanvas::fitToCanvas() {
    auto& d = *m_impl;
    if (!d.layout.isValid())
        return;
    d.automaticFit = true;
    d.zoom = std::min({1.0, qreal(std::max(1, width() - 48)) / d.layout.size.width(),
                       qreal(std::max(1, height() - 48)) / d.layout.size.height()});
    d.offset = (QPointF(width(), height()) -
                QPointF(d.layout.size.width(), d.layout.size.height()) * d.zoom) /
               2;
    d.geometryChanged();
}
void CollagePreviewCanvas::centerImage(quint64 id) {
    auto& d = *m_impl;
    if (!d.indexById.contains(id))
        return;
    d.document->select(id);
    const auto bounds = d.itemRect(id);
    if (bounds.isEmpty())
        return;
    d.automaticFit = false;
    d.zoom = std::min({d.zoom, qreal(std::max(1, width() - 48)) / bounds.width(),
                       qreal(std::max(1, height() - 48)) / bounds.height()});
    d.offset = QPointF(width() / 2.0, height() / 2.0) - bounds.center() * d.zoom;
    d.geometryChanged();
}
void CollagePreviewCanvas::preserveViewport() {
    m_impl->automaticFit = false;
}
void CollagePreviewCanvas::setEditingEnabled(bool enabled) {
    if (m_impl->editing == enabled)
        return;
    m_impl->editing = enabled;
    m_impl->pressedId = 0;
    m_impl->targetId = 0;
    m_impl->dragging = false;
    m_impl->geometryChanged();
}
void CollagePreviewCanvas::setRenderingEnabled(bool enabled) {
    auto& d = *m_impl;
    if (d.rendering == enabled)
        return;
    d.rendering = enabled;
    if (!enabled) {
        d.detailTimer->stop();
        d.cancelDetails();
        d.details.clear();
        d.detailCacheBytes = 0;
        d.previewStatisticsChanged();
    } else {
        d.refresh();
    }
}
void CollagePreviewCanvas::retranslateUi() {
    setAccessibleName(text("Collage preview"));
    setAccessibleDescription(text("Drag with the middle mouse button to pan; scroll to zoom"));
    const std::array<const char*, 6> labels{
        "Rotate clockwise by 90°", "Rotate counterclockwise by 90°",
        "Flip horizontally",       "Flip vertically",
        "Reset transform",         "Delete image"};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        m_impl->actions[i]->setToolTip(text(labels[i]));
        m_impl->actions[i]->setAccessibleName(m_impl->actions[i]->toolTip());
    }
    m_impl->fit->setToolTip(text("Fit to canvas"));
    m_impl->fit->setAccessibleName(m_impl->fit->toolTip());
    update();
}
void CollagePreviewCanvas::paintEvent(QPaintEvent*) {
    const auto& d = *m_impl;
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
    QPainter painter(this);
    painter.fillRect(rect(), QBrush(d.checker));
    if (d.document->items().isEmpty()) {
        return;
    }
    if (!d.layout.isValid()) {
        painter.setPen(theme.colorError);
        painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap,
                         d.layout.error);
        return;
    }
    const QRectF output(d.offset, QSizeF(d.layout.size) * d.zoom);
    painter.fillRect(output, d.document->settings().background);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (qsizetype index = 0; index < d.order.size(); ++index) {
        const auto id = d.order[index];
        const auto bounds = d.viewportRect(index);
        if (!bounds.intersects(rect()))
            continue;
        drawTransformedPreview(painter, bounds, d.previews.value(id),
                               d.previewTransforms.value(id));
        const auto detail = d.details.constFind(id);
        if (detail != d.details.cend() && detail->revision == d.revisions.value(id)) {
            const auto tileBounds = d.detailViewport(*detail, bounds);
            painter.fillRect(tileBounds, QBrush(d.checker));
            painter.fillRect(tileBounds, d.document->settings().background);
            painter.drawImage(tileBounds, detail->image);
        }
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(theme.colorBorder, 1));
    painter.drawRect(output);
    const auto selected = d.viewportRect(d.document->selectedId());
    if (!selected.isEmpty()) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(kSelectionFrameColor, kSelectionFrameWidth));
        painter.drawRect(selected);
        painter.restore();
    }
    if (d.targetId && d.targetId != d.pressedId) {
        const auto target = d.viewportRect(d.targetId);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(target, kReplacementFillColor);
        painter.setPen(QPen(kSelectionFrameColor, kSelectionFrameWidth, Qt::DashLine));
        const qreal inset = kSelectionFrameWidth / 2;
        painter.drawRect(target.adjusted(inset, inset, -inset, -inset));
        painter.restore();
    }
}
void CollagePreviewCanvas::resizeEvent(QResizeEvent*) {
    if (m_impl->automaticFit)
        fitToCanvas();
    m_impl->geometryChanged();
}
void CollagePreviewCanvas::mousePressEvent(QMouseEvent* event) {
    auto& d = *m_impl;
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::MiddleButton) {
        d.panning = true;
        d.automaticFit = false;
        d.last = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    } else if (event->button() == Qt::LeftButton) {
        d.automaticFit = false;
        const auto selected = d.hit(event->position());
        d.pressedId = d.editing ? selected : 0;
        d.press = event->position();
        d.document->select(selected);
        event->accept();
    }
}
void CollagePreviewCanvas::mouseMoveEvent(QMouseEvent* event) {
    auto& d = *m_impl;
    if (d.panning) {
        d.offset += event->position() - d.last;
        d.last = event->position();
        d.geometryChanged();
    } else if (d.pressedId && d.editing && (event->buttons() & Qt::LeftButton)) {
        if ((event->position() - d.press).manhattanLength() >= QApplication::startDragDistance())
            d.dragging = true;
        d.targetId = d.dragging ? d.hit(event->position()) : 0;
        if (d.dragging)
            setCursor(d.targetId && d.targetId != d.pressedId ? Qt::DragMoveCursor
                                                              : Qt::ForbiddenCursor);
        update();
    }
}
void CollagePreviewCanvas::mouseReleaseEvent(QMouseEvent* event) {
    auto& d = *m_impl;
    if (event->button() == Qt::MiddleButton)
        d.panning = false;
    if (event->button() == Qt::LeftButton) {
        const auto target = d.hit(event->position());
        if (d.dragging && d.editing && target && target != d.pressedId)
            d.document->swapItems(d.pressedId, target);
        d.pressedId = 0;
        d.targetId = 0;
        d.dragging = false;
    }
    unsetCursor();
    d.geometryChanged();
}
void CollagePreviewCanvas::wheelEvent(QWheelEvent* event) {
    auto& d = *m_impl;
    if (!d.layout.isValid()) {
        event->accept();
        return;
    }
    const double steps = usesPreciseWheelDelta(*event) ? event->pixelDelta().y() / 100.0
                                                       : event->angleDelta().y() / 120.0;
    if (steps != 0) {
        const double next = std::clamp(d.zoom * std::pow(1.15, steps), 0.000000001, 16.0);
        d.offset = event->position() - (event->position() - d.offset) * (next / d.zoom);
        d.zoom = next;
        d.automaticFit = false;
        d.geometryChanged();
    }
    event->accept();
}
void CollagePreviewCanvas::keyPressEvent(QKeyEvent* event) {
    auto& d = *m_impl;
    if (event->key() == Qt::Key_F || event->key() == Qt::Key_Home) {
        fitToCanvas();
    } else if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && d.editing) {
        d.document->remove(d.document->selectedId());
    } else if (event->key() == Qt::Key_Escape) {
        d.pressedId = 0;
        d.targetId = 0;
        d.dragging = false;
        d.document->select(0);
        unsetCursor();
    } else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Up ||
               event->key() == Qt::Key_Right || event->key() == Qt::Key_Down) {
        const auto index = d.indexById.value(d.document->selectedId(), -1);
        const int delta = event->key() == Qt::Key_Left || event->key() == Qt::Key_Up ? -1 : 1;
        if (!d.order.isEmpty())
            centerImage(d.order[qBound(qsizetype{0}, index + delta, d.order.size() - 1)]);
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}
void CollagePreviewCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (m_impl->editing && dragItem(event->mimeData(), m_impl->document))
        event->acceptProposedAction();
}
void CollagePreviewCanvas::dragMoveEvent(QDragMoveEvent* event) {
    m_impl->targetId = m_impl->hit(event->position());
    if (m_impl->targetId && m_impl->targetId != dragItem(event->mimeData(), m_impl->document))
        event->acceptProposedAction();
    else
        event->ignore();
    update();
}
void CollagePreviewCanvas::dragLeaveEvent(QDragLeaveEvent*) {
    m_impl->targetId = 0;
    update();
}
void CollagePreviewCanvas::dropEvent(QDropEvent* event) {
    const auto target = m_impl->hit(event->position());
    const auto source = dragItem(event->mimeData(), m_impl->document);
    m_impl->targetId = 0;
    if (m_impl->editing && target && source && m_impl->document->swapItems(source, target))
        event->acceptProposedAction();
    update();
}
void CollagePreviewCanvas::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

struct CollageEditorWidget::Impl {
    struct LengthEditor {
        QLabel* label = nullptr;
        AdInputNumber* input = nullptr;
        AdSegmented* unit = nullptr;
        bool percentage = false;
    };
    CollageEditorWidget* owner = nullptr;
    CollageDocument* document = nullptr;
    CollagePreviewCanvas* canvas = nullptr;
    CollagePdfPageGrid* pdfGrid = nullptr;
    CollagePdfTemplateWidget* pdfTemplate = nullptr;
    CollagePdfSettingsWidget* pdfSettings = nullptr;
    AdSegmented* outputType = nullptr;
    AdFormItem* outputTypeField = nullptr;
    AdFormItem* pdfSettingsField = nullptr;
    ImageDropSurface* emptyState = nullptr;
    QLabel* emptyPrompt = nullptr;
    AdButton* emptyAdd = nullptr;
    ImageDropSurface* dropHint = nullptr;
    QWidget* thumbnailRow = nullptr;
    QWidget* outputDescription = nullptr;
    QLabel* dimensions = nullptr;
    QLabel* total = nullptr;
    AdButton* add = nullptr;
    AdButton* clear = nullptr;
    AdButton* reverse = nullptr;
    CanvasActionToolbar* canvasToolbar = nullptr;
    AdScrollArea* strip = nullptr;
    QWidget* thumbnails = nullptr;
    QHBoxLayout* thumbnailLayout = nullptr;
    QHash<quint64, CollageThumbnail*> thumbnailById;
    quint64 selectedThumbnail = 0;
    AdForm* form = nullptr;
    AdSegmented* direction = nullptr;
    AdFormItem* gridField = nullptr;
    QLabel* rowsLabel = nullptr;
    QLabel* columnsLabel = nullptr;
    AdInputNumber* rows = nullptr;
    AdInputNumber* columns = nullptr;
    AdSegmented* sizing = nullptr;
    LengthEditor border;
    LengthEditor spacing;
    AdColorPicker* color = nullptr;
    std::array<AdFormItem*, 5> fields{};
    std::array<CollageColorPresetButton*, 8> presets{};
    CollageSettings cachedSettings;
    int referencePixels = 0;
    bool initialized = false;
    bool syncing = false;
    bool loading = false;
    bool exporting = false;

    void positionDropHint() {
        const auto* target = document->outputType() == CollageOutputType::PdfDocument
                                 ? (document->lastValidPdfSettings().stitchingMethod ==
                                            CollagePdfStitchingMethod::TemplateBased
                                        ? static_cast<QWidget*>(pdfTemplate)
                                        : static_cast<QWidget*>(pdfGrid))
                             : document->items().isEmpty() ? static_cast<QWidget*>(emptyState)
                                                           : static_cast<QWidget*>(canvas);
        dropHint->setGeometry(target->geometry());
        dropHint->raise();
        positionCanvasToolbar();
    }
    void positionCanvasToolbar() {
        const bool pdf = document->outputType() == CollageOutputType::PdfDocument;
        canvasToolbar->setVisible(!pdf);
        if (pdf)
            return;
        const auto* target = document->items().isEmpty() ? static_cast<QWidget*>(emptyState)
                                                         : static_cast<QWidget*>(canvas);
        canvasToolbar->adjustSize();
        canvasToolbar->move(target->x() + (target->width() - canvasToolbar->width()) / 2,
                            target->y() + target->height() - canvasToolbar->height() - 12);
        canvasToolbar->raise();
    }
    bool acceptsImageDrop(const QMimeData* mime, Qt::DropActions actions) const {
        return !loading && !exporting && actions.testFlag(Qt::CopyAction) &&
               !droppedImageFiles(mime).isEmpty();
    }
    int lengthReference() const {
        const auto settings = document->settings();
        int reference = 0;
        for (const auto& item : document->items()) {
            const auto size = transformedSize(item);
            reference = std::max(reference, settings.direction != CollageDirection::Horizontal
                                                ? size.width()
                                                : size.height());
            if (settings.sizing == CollageSizing::Uniform)
                break;
        }
        return reference;
    }
    void syncLengthInputs(int reference) {
        const auto settings = document->settings();
        const auto description = text(settings.direction == CollageDirection::Custom
                                          ? "Percentage of the grid cell width"
                                      : settings.direction == CollageDirection::Vertical
                                          ? "Percentage of the widest image after sizing"
                                          : "Percentage of the tallest image after sizing");
        const auto sync = [reference, &description](LengthEditor& editor, int pixels,
                                                    const char* pixelName,
                                                    const char* percentageName) {
            const QSignalBlocker blocker(editor.input);
            editor.input->setRange(0, editor.percentage && reference > 0
                                          ? 100.0 * std::numeric_limits<int>::max() / reference
                                          : std::numeric_limits<int>::max());
            editor.input->setSuffixText(editor.percentage ? QStringLiteral("%") : text(" px"));
            editor.input->setAccessibleName(text(editor.percentage ? percentageName : pixelName));
            editor.input->setToolTip(editor.percentage ? description : text(pixelName));
            editor.unit->setToolTip(description);
            editor.input->setEnabled(!editor.percentage || reference > 0);
            if (editor.percentage && reference == 0)
                editor.input->clear();
            else
                editor.input->setValue(editor.percentage ? 100.0 * pixels / reference : pixels);
        };
        // Unit and display rounding changes must never alter the stored output pixels.
        sync(border, settings.border, "Outer border in output pixels",
             "Outer border in percentage");
        sync(spacing, settings.spacing, "Image spacing in output pixels",
             "Image spacing in percentage");
    }
    void changeLengthUnit(LengthEditor& editor, const QVariant& value) {
        if (syncing)
            return;
        if (auto* focus = QApplication::focusWidget();
            focus && (focus == editor.input || editor.input->isAncestorOf(focus)))
            focus->clearFocus();
        editor.percentage = value.toString() == QStringLiteral("percentage");
        syncLengthInputs(lengthReference());
    }
    int lengthPixels(const LengthEditor& editor, double value) const {
        const double pixels = editor.percentage ? value * lengthReference() / 100.0 : value;
        return qRound(std::clamp(pixels, 0.0, double(std::numeric_limits<int>::max())));
    }
    void enabled() {
        if (loading || exporting)
            dropHint->hide();
        add->setEnabled(!loading && !exporting);
        add->setBusy(loading);
        clear->setEnabled(!exporting && !document->items().isEmpty());
        reverse->setEnabled(!loading && !exporting && document->items().size() > 1);
        form->setEnabled(!exporting);
        for (auto* thumbnail : thumbnailById)
            thumbnail->setEditingEnabled(!exporting, !loading);
        canvas->setEditingEnabled(!exporting);
        emptyAdd->setEnabled(!loading && !exporting);
        emptyAdd->setBusy(loading);
        pdfGrid->setLoading(loading);
        pdfGrid->setExportBusy(exporting);
        pdfTemplate->setLoading(loading);
        pdfTemplate->setExportBusy(exporting);
        positionCanvasToolbar();
    }
    void settingsChanged(const QString& field, const std::function<void(CollageSettings&)>& edit) {
        if (syncing)
            return;
        canvas->preserveViewport();
        auto settings = document->settings();
        const auto previousSettings = settings;
        edit(settings);
        document->setSettings(settings);
        const auto current = document->settings();
        // A too-small column count may normalize back to the existing settings without a signal.
        if (current == previousSettings)
            refresh(true);
        if (current.direction != previousSettings.direction ||
            (current.direction == CollageDirection::Custom &&
             (field == QStringLiteral("rows") || field == QStringLiteral("columns"))))
            canvas->fitToCanvas();
        emit owner->settingsEdited(field);
    }
    void refreshPdfDescription() {
        const auto settings = document->lastValidPdfSettings();
        const bool templateMode =
            settings.stitchingMethod == CollagePdfStitchingMethod::TemplateBased;
        QSize pixels(1, 1);
        if (!templateMode && settings.paperSize == CollagePdfPaperSize::ImageSize) {
            const auto selected = document->indexOf(document->selectedId());
            pixels = selected < 0 ? QSize{} : transformedSize(document->items()[selected]);
        }
        const auto page = calculateCollagePdfPageLayout(pixels, settings);
        const auto composed = templateMode
                                  ? calculateCollagePdfComposedPage(document->items(), settings, 0)
                                  : CollagePdfComposedPage{};
        const auto size =
            templateMode && composed.isValid() ? composed.pageMillimeters : page.pageMillimeters;
        dimensions->setText((templateMode ? composed.isValid() : page.isValid())
                                ? text("%1 × %2 mm")
                                      .arg(owner->locale().toString(size.width(), 'g', 6))
                                      .arg(owner->locale().toString(size.height(), 'g', 6))
                                : QString{});
        dimensions->setVisible(templateMode ? composed.isValid() : page.isValid());
        total->setText(
            text("Total: %1 pages").arg(collagePdfPageCount(document->items().size(), settings)));
    }
    void refresh(bool force = false) {
        syncing = true;
        const bool pdf = document->outputType() == CollageOutputType::PdfDocument;
        outputType->setCurrentValue(pdf ? QStringLiteral("pdf") : QStringLiteral("static"));
        pdfSettingsField->setVisible(pdf);
        pdfSettings->setVisible(pdf);
        const bool templateMode = pdf && document->lastValidPdfSettings().stitchingMethod ==
                                             CollagePdfStitchingMethod::TemplateBased;
        pdfGrid->setVisible(pdf && !templateMode);
        pdfTemplate->setVisible(templateMode);
        for (auto* field : fields)
            field->setVisible(!pdf);
        gridField->setVisible(!pdf && document->settings().direction == CollageDirection::Custom);
        canvas->setRenderingEnabled(!pdf);
        if (pdf) {
            canvas->hide();
            thumbnailRow->hide();
            emptyState->hide();
            dropHint->hide();
            refreshPdfDescription();
            syncing = false;
            enabled();
            return;
        }
        const auto nextSettings = document->settings();
        force = force || !initialized;
        const bool backgroundOnly = !force &&
                                    nextSettings.background != cachedSettings.background &&
                                    sameLayoutSettings(nextSettings, cachedSettings) &&
                                    thumbnailById.size() == document->items().size();
        if (force || nextSettings.background != cachedSettings.background) {
            color->setValue(AdColorValue::solid(nextSettings.background));
            for (std::size_t i = 0; i < presets.size(); ++i)
                presets[i]->setSelected(nextSettings.background ==
                                        QColor(QString::fromLatin1(kColorValues[i])));
        }
        if (backgroundOnly) {
            syncing = false;
            cachedSettings = nextSettings;
            return;
        }
        if (force || nextSettings.direction != cachedSettings.direction) {
            direction->setCurrentValue(nextSettings.direction == CollageDirection::Vertical
                                           ? QStringLiteral("vertical")
                                       : nextSettings.direction == CollageDirection::Horizontal
                                           ? QStringLiteral("horizontal")
                                           : QStringLiteral("custom"));
            gridField->setVisible(nextSettings.direction == CollageDirection::Custom);
        }
        if (force || nextSettings.rows != cachedSettings.rows)
            rows->setValue(nextSettings.rows);
        if (force || nextSettings.columns != cachedSettings.columns)
            columns->setValue(nextSettings.columns);
        if (force || nextSettings.sizing != cachedSettings.sizing)
            sizing->setCurrentValue(nextSettings.sizing == CollageSizing::Uniform
                                        ? QStringLiteral("uniform")
                                        : QStringLiteral("original"));
        const int reference = lengthReference();
        if (force || reference != referencePixels ||
            nextSettings.direction != cachedSettings.direction ||
            nextSettings.border != cachedSettings.border ||
            nextSettings.spacing != cachedSettings.spacing)
            syncLengthInputs(reference);
        referencePixels = reference;
        cachedSettings = nextSettings;
        initialized = true;
        syncing = false;
        QSet<quint64> retained;
        for (const auto& item : document->items())
            retained.insert(item.id);
        bool thumbnailsChanged = false;
        const bool countChanged = thumbnailById.size() != document->items().size();
        for (auto it = thumbnailById.begin(); it != thumbnailById.end();) {
            if (!retained.contains(it.key())) {
                thumbnailLayout->removeWidget(it.value());
                it.value()->hide();
                it.value()->deleteLater();
                it = thumbnailById.erase(it);
                thumbnailsChanged = true;
            } else {
                ++it;
            }
        }
        QHash<QWidget*, int> thumbnailPositions;
        for (int index = 0; index < thumbnailLayout->count(); ++index)
            thumbnailPositions.insert(thumbnailLayout->itemAt(index)->widget(), index);
        int ordinal = 0;
        for (const auto& item : document->items()) {
            auto* thumbnail = thumbnailById.value(item.id);
            if (!thumbnail) {
                thumbnail = new CollageThumbnail(document, item.id, thumbnails);
                thumbnail->selected = [this](quint64 id) { canvas->centerImage(id); };
                thumbnail->editStarted = [this] { canvas->preserveViewport(); };
                thumbnail->replaceRequested = [this](quint64 id) {
                    emit owner->replaceImageRequested(id);
                };
                thumbnailById.insert(item.id, thumbnail);
                thumbnail->setEditingEnabled(!exporting, !loading);
                thumbnailPositions.insert(thumbnail, thumbnailLayout->count());
                thumbnailLayout->addWidget(thumbnail);
                thumbnailsChanged = true;
            }
            thumbnail->setItem(item, ++ordinal, force);
            const int targetIndex = ordinal - 1;
            const int currentIndex = thumbnailPositions.value(thumbnail);
            if (currentIndex != targetIndex) {
                // Earlier slots are final, so currentIndex is always beyond targetIndex. Swap
                // two layout items directly instead of shifting every intervening thumbnail.
                auto* desired = thumbnailLayout->takeAt(currentIndex);
                auto* displaced = thumbnailLayout->takeAt(targetIndex);
                thumbnailLayout->insertItem(targetIndex, desired);
                thumbnailLayout->insertItem(currentIndex, displaced);
                thumbnailPositions.insert(thumbnail, targetIndex);
                thumbnailPositions.insert(displaced->widget(), currentIndex);
                thumbnailsChanged = true;
            }
        }
        if (thumbnailsChanged)
            thumbnails->adjustSize();
        const bool empty = document->items().isEmpty();
        canvas->setVisible(!empty);
        thumbnailRow->setVisible(!empty);
        emptyState->setVisible(empty);
        positionDropHint();
        const auto layout = document->layout();
        dimensions->setText(
            layout.isValid() ? text("%1 × %2 px").arg(layout.size.width()).arg(layout.size.height())
                             : QString{});
        dimensions->setVisible(layout.isValid());
        total->setText(text("Total: %1 images").arg(document->items().size()));
        if (countChanged || force)
            enabled();
    }
    void theme() {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(owner);
        auto labelFont = form->font();
        labelFont.setPixelSize(qMax(12, qRound(theme.fontSize)));
        labelFont.setWeight(QFont::Normal);
        for (auto* label : {border.label, spacing.label, rowsLabel, columnsLabel}) {
            label->setFont(labelFont);
            auto palette = label->palette();
            palette.setColor(QPalette::WindowText, theme.colorText);
            palette.setColor(QPalette::Disabled, QPalette::WindowText, theme.colorTextDisabled);
            label->setPalette(palette);
        }
        auto labelPalette = owner->palette();
        labelPalette.setColor(QPalette::WindowText, theme.colorTextSecondary);
        dimensions->setPalette(labelPalette);
        total->setPalette(labelPalette);
        emptyState->refreshTheme();
        dropHint->refreshTheme();
        const auto metric = styles::ThemeManager::instance().themeColorScheme().metricAlias;
        outputDescription->layout()->setSpacing(metric.marginSM);
        for (auto* label : {dimensions, total}) {
            QFont font = owner->font();
            font.setPixelSize(metric.fontSizeSM);
            label->setFont(font);
        }
        owner->update();
        for (auto* thumbnail : thumbnailById)
            thumbnail->update();
    }
};

CollageEditorWidget::CollageEditorWidget(CollageDocument* document, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.document = document;
    d.selectedThumbnail = document->selectedId();
    setObjectName(QStringLiteral("collageEditorWidget"));
    setAcceptDrops(true);
    const auto theme = styles::ThemeManager::instance().themeColorScheme().metricAlias;
    d.outputDescription = new QWidget(this);
    d.outputDescription->setObjectName(QStringLiteral("collageOutputDescription"));
    d.outputDescription->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    d.outputDescription->hide();
    auto* descriptionLayout = new QHBoxLayout(d.outputDescription);
    descriptionLayout->setContentsMargins(0, 0, 0, 0);
    descriptionLayout->setSpacing(theme.marginSM);
    d.dimensions = new QLabel(d.outputDescription);
    d.dimensions->setObjectName(QStringLiteral("collageOutputDimensions"));
    d.total = new QLabel(d.outputDescription);
    d.total->setObjectName(QStringLiteral("collageImageTotal"));
    for (auto* label : {d.dimensions, d.total}) {
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        descriptionLayout->addWidget(label, 0, Qt::AlignVCenter);
    }
    descriptionLayout->addStretch(1);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(theme.marginSM);
    auto* body = new QHBoxLayout;
    body->setSpacing(theme.marginMD);
    auto* previewColumn = new QVBoxLayout;
    previewColumn->setSpacing(theme.marginXS);
    d.canvas = new CollagePreviewCanvas(document, this);
    previewColumn->addWidget(d.canvas, 1);
    d.pdfGrid = new CollagePdfPageGrid(document, this);
    previewColumn->addWidget(d.pdfGrid, 1);
    d.pdfTemplate = new CollagePdfTemplateWidget(document, this);
    previewColumn->addWidget(d.pdfTemplate, 1);
    d.emptyState = new ImageDropSurface(false, this);
    d.emptyState->setObjectName(QStringLiteral("collageEmptyState"));
    auto* emptyLayout = static_cast<QVBoxLayout*>(d.emptyState->layout());
    d.emptyPrompt = d.emptyState->title;
    d.emptyPrompt->setObjectName(QStringLiteral("collageEmptyPrompt"));
    d.emptyState->description->setObjectName(QStringLiteral("collageEmptyDropDescription"));
    d.emptyAdd = new AdButton(d.emptyState);
    d.emptyAdd->setObjectName(QStringLiteral("collageEmptyAddImages"));
    d.emptyAdd->setIconRef(icons::Plus());
    d.emptyAdd->setButtonStyle(AdButton::ButtonStyle::Solid);
    d.emptyAdd->setAccentRole(AdButton::AccentRole::Primary);
    d.emptyAdd->setFocusPolicy(Qt::StrongFocus);
    emptyLayout->insertSpacing(emptyLayout->count() - 1, 8);
    emptyLayout->insertWidget(emptyLayout->count() - 1, d.emptyAdd, 0, Qt::AlignHCenter);
    previewColumn->addWidget(d.emptyState, 1);
    body->addLayout(previewColumn, 1);

    d.dropHint = new ImageDropSurface(true, this);
    d.dropHint->setObjectName(QStringLiteral("collageDropHint"));
    d.dropHint->title->setObjectName(QStringLiteral("collageDropTitle"));
    d.dropHint->description->setObjectName(QStringLiteral("collageDropDescription"));
    d.dropHint->hide();

    auto* formScroll = new AdScrollArea(this);
    formScroll->setObjectName(QStringLiteral("collageSettingsScroll"));
    formScroll->setFixedWidth(kSettingsFormWidth);
    formScroll->setWidgetResizable(true);
    formScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    formScroll->setFrameShape(QFrame::NoFrame);
    formScroll->viewport()->setAutoFillBackground(false);
    d.form = new AdForm;
    d.form->setObjectName(QStringLiteral("collageSettingsForm"));
    d.form->setFormLayout(AdForm::FormLayout::Vertical);
    d.form->setLabelAlign(AdForm::LabelAlign::Left);
    d.form->setColon(false);
    d.form->setRequiredMark(AdForm::RequiredMark::Hidden);
    d.form->setContentsMargins(0, 0, kSettingsFormRightMargin, 0);
    formScroll->setContentWidget(d.form);
    d.form->setAutoFillBackground(false);
    body->addWidget(formScroll);
    outer->addLayout(body, 1);

    const auto segmented = [&d](const char* name, const char* first, const char* second,
                                std::size_t index) {
        auto* control = new AdSegmented(d.form);
        control->setObjectName(QString::fromLatin1(name));
        control->setDistribution(AdSegmented::Distribution::Fill);
        control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        control->addOption({}, QString::fromLatin1(first));
        control->addOption({}, QString::fromLatin1(second));
        d.fields[index] = d.form->addField({}, control, QString::fromLatin1(name));
        return control;
    };
    d.outputType = new AdSegmented(d.form);
    d.outputType->setObjectName(QStringLiteral("collageImageType"));
    d.outputType->setDistribution(AdSegmented::Distribution::Fill);
    d.outputType->addOption({}, QStringLiteral("static"));
    d.outputType->addOption({}, QStringLiteral("pdf"));
    d.outputTypeField = d.form->addField({}, d.outputType, QStringLiteral("output_type"));
    d.direction = segmented("collageDirection", "vertical", "horizontal", 0);
    d.direction->addOption({}, QStringLiteral("custom"));
    auto* grid = new QWidget(d.form);
    grid->setObjectName(QStringLiteral("collageCustomGrid"));
    grid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* gridLayout = new QHBoxLayout(grid);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(theme.marginSM);
    const auto gridNumber = [grid, gridLayout](QLabel*& label, const char* name) {
        auto* field = new QWidget(grid);
        auto* layout = new QVBoxLayout(field);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        label = new QLabel(field);
        label->setObjectName(QString::fromLatin1(name) + QStringLiteral("Label"));
        label->setWordWrap(true);
        layout->addWidget(label);
        auto* input = new AdInputNumber(field);
        input->setObjectName(QString::fromLatin1(name));
        input->setDecimals(0);
        input->setRange(1, std::numeric_limits<int>::max());
        input->setSingleStep(1);
        input->setStepButtonLayout(AdInputNumber::StepButtonLayout::Compact);
        input->setKeyboardTracking(false);
        input->setWheelStepEnabled(false);
        input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        label->setBuddy(input);
        layout->addWidget(input);
        gridLayout->addWidget(field, 1);
        return input;
    };
    d.rows = gridNumber(d.rowsLabel, "collageGridRows");
    d.columns = gridNumber(d.columnsLabel, "collageGridColumns");
    d.gridField = d.form->addField({}, grid, QStringLiteral("collageCustomGrid"));
    d.sizing = segmented("collageSizing", "uniform", "original", 1);
    const auto number = [&d](Impl::LengthEditor& editor, const char* name, std::size_t index) {
        auto* field = new QWidget(d.form);
        field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* layout = new QVBoxLayout(field);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        auto* header = new QHBoxLayout;
        header->setSpacing(8);
        editor.label = new QLabel(field);
        editor.label->setObjectName(QString::fromLatin1(name) + QStringLiteral("Label"));
        editor.label->setWordWrap(true);
        header->addWidget(editor.label, 1);
        editor.unit = new AdSegmented(field);
        editor.unit->setObjectName(QString::fromLatin1(name) + QStringLiteral("UnitSegmented"));
        editor.unit->setControlSize(AdSegmented::ControlSize::Small);
        editor.unit->setDistribution(AdSegmented::Distribution::Content);
        editor.unit->addOption({}, QStringLiteral("pixels"));
        editor.unit->addOption({}, QStringLiteral("percentage"));
        editor.unit->setCurrentValue(QStringLiteral("pixels"));
        header->addWidget(editor.unit, 0, Qt::AlignRight | Qt::AlignVCenter);
        layout->addLayout(header);
        auto* input = new AdInputNumber(field);
        input->setObjectName(QString::fromLatin1(name));
        input->setDecimals(0);
        input->setRange(0, std::numeric_limits<int>::max());
        input->setSingleStep(1);
        input->setStepButtonLayout(AdInputNumber::StepButtonLayout::Compact);
        input->setSuffixText(text(" px"));
        input->setKeyboardTracking(false);
        input->setWheelStepEnabled(false);
        input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout->addWidget(input);
        d.fields[index] = d.form->addField({}, field, QString::fromLatin1(name));
        editor.input = input;
    };
    number(d.border, "collageOuterBorder", 2);
    number(d.spacing, "collageImageSpacing", 3);
    auto* colors = new QWidget(d.form);
    colors->setObjectName(QStringLiteral("collageBackgroundColors"));
    colors->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* colorLayout = new QHBoxLayout(colors);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    colorLayout->setSpacing(theme.marginXS);
    d.color = new AdColorPicker(colors);
    d.color->setObjectName(QStringLiteral("collageBackgroundColor"));
    d.color->setAlphaChannelEnabled(true);
    d.color->setAllowClear(true);
    d.color->setMode(AdColorPicker::Mode::Solid);
    d.color->setModeOptions({AdColorPicker::Mode::Solid});
    d.color->setTriggerTextVisible(false);
    d.color->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* presets = new QWidget(colors);
    auto* presetLayout = new QHBoxLayout(presets);
    presetLayout->setContentsMargins(0, 0, 0, 0);
    presetLayout->setSpacing(theme.marginXXS);
    for (std::size_t i = 0; i < d.presets.size(); ++i) {
        auto* button =
            new CollageColorPresetButton(QColor(QString::fromLatin1(kColorValues[i])), presets);
        d.presets[i] = button;
        button->setObjectName(QStringLiteral("collageColorPreset_%1").arg(i));
        presetLayout->addWidget(button);
        connect(button, &QAbstractButton::clicked, this, [this, i, button] {
            m_impl->settingsChanged(
                QStringLiteral("background_color"), [i](CollageSettings& settings) {
                    settings.background = QColor(QString::fromLatin1(kColorValues[i]));
                });
            button->setSelected(true);
        });
    }
    colorLayout->addWidget(presets, 0, Qt::AlignVCenter);
    auto* colorSeparator = new AdDivider(colors);
    colorSeparator->setObjectName(QStringLiteral("collageColorSeparator"));
    colorSeparator->setOrientation(AdDivider::Orientation::Vertical);
    AdDivider::MetricTokens separatorMetrics;
    separatorMetrics.verticalMarginInline = 1;
    colorSeparator->setComponentTokens({{}, separatorMetrics});
    colorSeparator->setFixedHeight(24);
    colorLayout->addWidget(colorSeparator, 0, Qt::AlignVCenter);
    colorLayout->addWidget(d.color, 0, Qt::AlignVCenter);
    colorLayout->addStretch(1);
    d.fields[4] = d.form->addField({}, colors, QStringLiteral("background"));
    d.pdfSettings = new CollagePdfSettingsWidget(document, d.form);
    d.pdfSettingsField = d.form->addField({}, d.pdfSettings, QStringLiteral("pdf_settings"));
    d.pdfSettingsField->setNoStyle(true);
    d.form->layout()->setAlignment(Qt::AlignTop);

    d.thumbnailRow = new QWidget(this);
    d.thumbnailRow->setObjectName(QStringLiteral("collageThumbnailRow"));
    auto* stripRow = new QHBoxLayout(d.thumbnailRow);
    stripRow->setContentsMargins(0, 0, 0, 0);
    stripRow->setSpacing(theme.marginSM);
    d.strip = new AdScrollArea(this);
    d.strip->setObjectName(QStringLiteral("collageThumbnailStrip"));
    d.strip->setFrameShape(QFrame::NoFrame);
    d.strip->setWidgetResizable(false);
    // AdScrollArea overlays its own bars; enabling Qt's bars also reserves a second row.
    d.strip->setFitToWidth(false);
    d.strip->setFixedHeight(kThumbnailSize.height() + 2 * kThumbnailMargin);
    d.thumbnails = new QWidget;
    d.thumbnails->setObjectName(QStringLiteral("collageThumbnails"));
    d.thumbnailLayout = new QHBoxLayout(d.thumbnails);
    d.thumbnailLayout->setContentsMargins(kThumbnailMargin, kThumbnailMargin, kThumbnailMargin,
                                          kThumbnailMargin);
    d.thumbnailLayout->setSpacing(theme.marginXS);
    d.thumbnailLayout->setSizeConstraint(QLayout::SetFixedSize);
    d.strip->setContentWidget(d.thumbnails);
    d.strip->setAutoFillBackground(false);
    d.strip->viewport()->setAutoFillBackground(false);
    // QScrollArea::setWidget enables background filling on the content widget.
    d.thumbnails->setAutoFillBackground(false);
    stripRow->addWidget(d.strip, 1);
    d.canvasToolbar = new CanvasActionToolbar(this);
    d.canvasToolbar->setObjectName(QStringLiteral("collageCanvasToolbar"));
    d.reverse = d.canvasToolbar->addAction(
        snow_shot::presentation::icons::custom::outlined::ReverseOrder(), "collageReverseImages");
    d.add = d.canvasToolbar->addAction(icons::Plus(), "collageAddImages");
    d.canvasToolbar->addSeparator();
    d.clear = d.canvasToolbar->addAction(icons::IconDelete(), "collageClearAll");
    d.clear->setAccentRole(AdButton::AccentRole::Danger);
    d.canvas->installEventFilter(this);
    d.emptyState->installEventFilter(this);
    previewColumn->addWidget(d.thumbnailRow);

    connect(d.add, &QAbstractButton::clicked, this, &CollageEditorWidget::addImagesRequested);
    connect(d.emptyAdd, &QAbstractButton::clicked, this, &CollageEditorWidget::addImagesRequested);
    connect(d.clear, &QAbstractButton::clicked, this, &CollageEditorWidget::clearRequested);
    connect(d.reverse, &QAbstractButton::clicked, this, [this] {
        if (!m_impl->loading && !m_impl->exporting) {
            m_impl->canvas->preserveViewport();
            m_impl->document->reverseItems();
        }
    });
    connect(d.outputType, &AdSegmented::currentValueChanged, this, [this](const QVariant& value) {
        if (!m_impl->syncing && !m_impl->exporting)
            m_impl->document->setOutputType(value.toString() == QStringLiteral("pdf")
                                                ? CollageOutputType::PdfDocument
                                                : CollageOutputType::StaticImage);
    });
    connect(d.pdfSettings, &CollagePdfSettingsWidget::settingsEdited, this,
            [this](const QString& field) { emit settingsEdited(QStringLiteral("pdf/") + field); });
    connect(d.pdfGrid, &CollagePdfPageGrid::addImagesRequested, this,
            &CollageEditorWidget::addImagesRequested);
    connect(d.pdfGrid, &CollagePdfPageGrid::imageFilesDropped, this,
            &CollageEditorWidget::imageFilesDropped);
    connect(d.pdfGrid, &CollagePdfPageGrid::replaceImageRequested, this,
            &CollageEditorWidget::replaceImageRequested);
    connect(d.pdfGrid, &CollagePdfPageGrid::clearRequested, this,
            &CollageEditorWidget::clearRequested);
    connect(d.pdfTemplate, &CollagePdfTemplateWidget::addImagesRequested, this,
            &CollageEditorWidget::addImagesRequested);
    connect(d.pdfTemplate, &CollagePdfTemplateWidget::replaceImageRequested, this,
            &CollageEditorWidget::replaceImageRequested);
    connect(d.pdfTemplate, &CollagePdfTemplateWidget::clearRequested, this,
            &CollageEditorWidget::clearRequested);
    connect(d.direction, &AdSegmented::currentValueChanged, this, [this](const QVariant& value) {
        m_impl->settingsChanged(QStringLiteral("direction"), [&value](CollageSettings& settings) {
            settings.direction =
                value.toString() == QStringLiteral("horizontal") ? CollageDirection::Horizontal
                : value.toString() == QStringLiteral("custom")   ? CollageDirection::Custom
                                                                 : CollageDirection::Vertical;
        });
    });
    connect(d.rows, &AdInputNumber::valueChanged, this, [this](double value) {
        m_impl->settingsChanged(QStringLiteral("rows"), [value](CollageSettings& settings) {
            settings.rows = qRound(value);
        });
    });
    connect(d.columns, &AdInputNumber::valueChanged, this, [this](double value) {
        m_impl->settingsChanged(QStringLiteral("columns"), [value](CollageSettings& settings) {
            settings.columns = qRound(value);
        });
    });
    connect(d.sizing, &AdSegmented::currentValueChanged, this, [this](const QVariant& value) {
        m_impl->settingsChanged(QStringLiteral("sizing_mode"), [&value](CollageSettings& settings) {
            settings.sizing = value.toString() == QStringLiteral("original")
                                  ? CollageSizing::Original
                                  : CollageSizing::Uniform;
        });
    });
    connect(d.border.unit, &AdSegmented::currentValueChanged, this,
            [this](const QVariant& value) { m_impl->changeLengthUnit(m_impl->border, value); });
    connect(d.spacing.unit, &AdSegmented::currentValueChanged, this,
            [this](const QVariant& value) { m_impl->changeLengthUnit(m_impl->spacing, value); });
    connect(d.border.input, &AdInputNumber::valueChanged, this, [this](double value) {
        const int pixels = m_impl->lengthPixels(m_impl->border, value);
        m_impl->settingsChanged(QStringLiteral("border_px"),
                                [pixels](CollageSettings& settings) { settings.border = pixels; });
    });
    connect(d.spacing.input, &AdInputNumber::valueChanged, this, [this](double value) {
        const int pixels = m_impl->lengthPixels(m_impl->spacing, value);
        m_impl->settingsChanged(QStringLiteral("spacing_px"),
                                [pixels](CollageSettings& settings) { settings.spacing = pixels; });
    });
    connect(d.color, &AdColorPicker::valueChanged, this, [this](const AdColorValue& value) {
        if (value.isGradient())
            return;
        QColor color = value.isNone() ? QColor(Qt::transparent) : value.solidColor;
        if (color.isValid())
            m_impl->settingsChanged(
                QStringLiteral("background_color"),
                [&color](CollageSettings& settings) { settings.background = color; });
    });
    connect(document, &CollageDocument::changed, this, [this] { m_impl->refresh(); });
    connect(document, &CollageDocument::selectionChanged, this, [this] {
        if (m_impl->document->outputType() == CollageOutputType::PdfDocument)
            m_impl->refreshPdfDescription();
        const auto selected = m_impl->document->selectedId();
        if (auto* thumbnail = m_impl->thumbnailById.value(m_impl->selectedThumbnail))
            thumbnail->update();
        if (auto* thumbnail = m_impl->thumbnailById.value(selected))
            thumbnail->update();
        m_impl->selectedThumbnail = selected;
    });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this] { m_impl->theme(); });
    connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, this,
            [this, outer, body, previewColumn, colorLayout, presetLayout, stripRow,
             gridLayout](const styles::ThemeColorScheme& scheme) {
                const auto& metric = scheme.metricAlias;
                outer->setSpacing(metric.marginSM);
                body->setSpacing(metric.marginMD);
                previewColumn->setSpacing(metric.marginXS);
                colorLayout->setSpacing(metric.marginXS);
                presetLayout->setSpacing(metric.marginXXS);
                stripRow->setSpacing(metric.marginSM);
                gridLayout->setSpacing(metric.marginSM);
                m_impl->thumbnailLayout->setSpacing(metric.marginXS);
                m_impl->theme();
            });
    retranslateUi();
    d.refresh(true);
    d.theme();
}
CollageEditorWidget::~CollageEditorWidget() = default;
void CollageEditorWidget::setLoading(bool loading) {
    m_impl->loading = loading;
    m_impl->enabled();
}
void CollageEditorWidget::setExportBusy(bool busy) {
    m_impl->exporting = busy;
    m_impl->enabled();
}
CollagePreviewCanvas* CollageEditorWidget::canvas() const {
    return m_impl->canvas;
}
CollagePdfPageGrid* CollageEditorWidget::pdfGrid() const {
    return m_impl->pdfGrid;
}
CollagePdfTemplateWidget* CollageEditorWidget::pdfTemplateWidget() const {
    return m_impl->pdfTemplate;
}
QWidget* CollageEditorWidget::outputDescriptionWidget() const {
    return m_impl->outputDescription;
}
void CollageEditorWidget::retranslateUi() {
    auto& d = *m_impl;
    d.syncing = true;
    d.outputTypeField->setLabel(text("Image type"));
    d.outputType->setAccessibleName(text("Image type"));
    d.outputType->setOptionLabel(0, text("Static image"));
    d.outputType->setOptionLabel(1, text("PDF document"));
    d.pdfSettings->retranslateUi();
    d.pdfGrid->retranslateUi();
    d.pdfTemplate->retranslateUi();
    d.direction->setOptionLabel(0, text("Vertical"));
    d.direction->setOptionLabel(1, text("Horizontal"));
    d.direction->setOptionLabel(2, text("Custom"));
    d.direction->setAccessibleName(text("Layout direction"));
    d.rowsLabel->setText(text("Rows"));
    d.columnsLabel->setText(text("Columns"));
    d.rows->setAccessibleName(text("Rows"));
    d.columns->setAccessibleName(text("Columns"));
    d.sizing->setOptionLabel(0, text("Uniform size"));
    d.sizing->setOptionLabel(1, text("Original size"));
    d.sizing->setAccessibleName(text("Image sizing"));
    const std::array<const char*, 5> labels{"Layout direction", "Image sizing", "Outer border",
                                            "Image spacing", "Background color"};
    for (std::size_t i = 0; i < labels.size(); ++i)
        if (i != 2 && i != 3)
            d.fields[i]->setLabel(text(labels[i]));
    d.border.label->setText(text("Outer border"));
    d.spacing.label->setText(text("Image spacing"));
    d.border.unit->setAccessibleName(text("Outer border unit"));
    d.spacing.unit->setAccessibleName(text("Image spacing unit"));
    for (auto* unit : {d.border.unit, d.spacing.unit}) {
        unit->setOptionLabel(0, text("Pixel"));
        unit->setOptionLabel(1, text("Percentage"));
    }
    d.color->setAccessibleName(text("Background color"));
    d.color->setToolTip(text("Choose a background color, including transparency"));
    constexpr std::array<const char*, 8> names{"White", "Black", "Gray",   "Red",
                                               "Green", "Blue",  "Yellow", "Purple"};
    for (std::size_t i = 0; i < d.presets.size(); ++i) {
        d.presets[i]->setToolTip(text(names[i]));
        d.presets[i]->setAccessibleName(text(names[i]));
    }
    d.add->setToolTip(text("Add images"));
    d.add->setAccessibleName(d.add->toolTip());
    d.reverse->setToolTip(text("Reverse image order"));
    d.reverse->setAccessibleName(d.reverse->toolTip());
    d.emptyPrompt->setText(text("Add images to create a collage"));
    d.emptyState->description->setText(text("Drag image files here, or choose add images"));
    d.dropHint->title->setText(text("Drop images to add to collage"));
    d.dropHint->description->setText(text("Add multiple image files at once"));
    d.dropHint->setAccessibleName(d.dropHint->title->text());
    d.dropHint->setAccessibleDescription(d.dropHint->description->text());
    d.emptyAdd->setText(text("Add images"));
    d.emptyAdd->setAccessibleName(d.emptyAdd->text());
    d.clear->setToolTip(text("Clear all"));
    d.clear->setAccessibleName(d.clear->toolTip());
    d.canvas->retranslateUi();
    d.syncing = false;
    d.refresh(true);
}
void CollageEditorWidget::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

bool CollageEditorWidget::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_impl->canvas || watched == m_impl->emptyState) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
         event->type() == QEvent::Show))
        m_impl->positionCanvasToolbar();
    return QWidget::eventFilter(watched, event);
}

void CollageEditorWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layout()->activate();
    m_impl->positionDropHint();
}

void CollageEditorWidget::hideEvent(QHideEvent* event) {
    m_impl->dropHint->hide();
    QWidget::hideEvent(event);
}

void CollageEditorWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (!m_impl->acceptsImageDrop(event->mimeData(), event->possibleActions())) {
        m_impl->dropHint->hide();
        event->ignore();
        return;
    }
    m_impl->positionDropHint();
    m_impl->dropHint->show();
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void CollageEditorWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (!m_impl->acceptsImageDrop(event->mimeData(), event->possibleActions())) {
        m_impl->dropHint->hide();
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void CollageEditorWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    m_impl->dropHint->hide();
    event->accept();
}

void CollageEditorWidget::dropEvent(QDropEvent* event) {
    m_impl->dropHint->hide();
    if (!m_impl->acceptsImageDrop(event->mimeData(), event->possibleActions())) {
        event->ignore();
        return;
    }
    const auto paths = droppedImageFiles(event->mimeData());
    event->setDropAction(Qt::CopyAction);
    event->accept();
    emit imageFilesDropped(paths);
}
} // namespace snow_shot::presentation::components
