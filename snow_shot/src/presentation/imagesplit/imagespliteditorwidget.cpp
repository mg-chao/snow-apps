// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/imagespliteditorwidget.h"

#include "snow_shot/presentation/components/canvasactiontoolbar.h"
#include "snow_shot/presentation/components/imagedropsurface.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_draw_engine_qt/snow_canvas_overlay_style.h"
#include "antd_icons.h"
#include "snow_shot/presentation/screenshotwheelinput.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/styles/sequencenumberbadgestyle.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/shortcuts/shortcutbinding.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/input_number.h"
#include "widgets/scroll_area.h"
#include "widgets/segmented.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRegion>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace snow_shot::presentation::components {
namespace {
using adqt::widgets::AdButton;
using adqt::widgets::AdForm;
using adqt::widgets::AdFormItem;
using adqt::widgets::AdInputNumber;
using adqt::widgets::AdScrollArea;
using adqt::widgets::AdSegmented;
using styles::SequenceNumberBadgeStyle;
constexpr qreal kRulerHeight = 28;
constexpr qreal kRulerWidth = 38;
constexpr qreal kPositionBadgeHeight = 22;
constexpr qreal kPositionBadgeSpacing = (kRulerHeight - kPositionBadgeHeight) / 2;
constexpr qreal kLineHitDistance = 6;
constexpr qreal kGuideSpacing = 2;
// Match snow-draw-engine-editor's selection color in geometry/handles.rs.
const QColor kSplitLineColor{0x40, 0x96, 0xff};

class OverlapWidthTextPolicy final : public adqt::widgets::AdInputNumberTextPolicy {
  public:
    explicit OverlapWidthTextPolicy(QObject* parent) : AdInputNumberTextPolicy(parent) {}

    QString formatText(const QString& canonicalText, bool, const QString&) const override {
        // Group separators inserted by the numeric control would invalidate whole-number drafts.
        return canonicalText;
    }
};

QString editorText(const char* source) {
    return QCoreApplication::translate("ImageSplitEditorWidget", source);
}
QString canvasText(const char* source) {
    return QCoreApplication::translate("ImageSplitPreviewCanvas", source);
}
[[maybe_unused]] constexpr std::array kEditorTranslations{
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Split direction"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Vertical"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Horizontal"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Grid"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Split type"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Overlap width"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget",
                      "Each image extends this many pixels past each split line"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Equal split"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Precise split"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Number of images"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Rows"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Columns"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Width per image"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Height per image"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Pixels"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Percentage"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", " px"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Split size unit"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Enter a positive integer"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Enter a non-negative integer"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Enter an integer of at least 16 pixels"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Undo"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Redo"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Fit to canvas"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Reload"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "%1 × %2 px"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Total: %1 images"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Select an image to split"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Drag an image file here, or choose select image"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Select image"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Drop one image to split"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Drop one image to replace the current image"),
    QT_TRANSLATE_NOOP("ImageSplitEditorWidget", "Drop one image file at a time"),
};
[[maybe_unused]] constexpr std::array kCanvasTranslations{
    QT_TRANSLATE_NOOP("ImageSplitPreviewCanvas", "Image splitting preview"),
    QT_TRANSLATE_NOOP(
        "ImageSplitPreviewCanvas",
        "Click a ruler to add a split line; drag a line to move it. Select a line and press Delete "
        "to remove it. Drag with the middle mouse button to pan; scroll to zoom"),
    QT_TRANSLATE_NOOP("ImageSplitPreviewCanvas", "X: %1 px"),
    QT_TRANSLATE_NOOP("ImageSplitPreviewCanvas", "Y: %1 px"),
};

QStringList droppedPaths(const QMimeData* mime) {
    QStringList paths;
    if (!mime || !mime->hasUrls())
        return paths;
    for (const auto& url : mime->urls()) {
        if (!url.isLocalFile())
            return {};
        paths.append(url.toLocalFile());
    }
    return paths;
}

int roundedCoordinate(qreal coordinate) {
    return static_cast<int>(
        std::clamp(std::floor(coordinate + .5), 0.0, qreal(std::numeric_limits<int>::max())));
}

QFont positionFont(const QWidget* canvas) {
    auto font = canvas->font();
    font.setPixelSize(12);
    return font;
}

QSizeF positionBadgeSize(const QWidget* canvas, const QString& label) {
    return {qreal(QFontMetrics(positionFont(canvas)).horizontalAdvance(label) + 12),
            kPositionBadgeHeight};
}
} // namespace

struct ImageSplitPreviewCanvas::Impl {
    using PositionLine = std::optional<std::pair<bool, int>>;
    struct OverlayState {
        PositionLine selected;
        PositionLine hovered;
        PositionLine ruler;
        bool operator==(const OverlayState&) const = default;
    };
    ImageSplitPreviewCanvas* owner = nullptr;
    ImageSplitDocument* document = nullptr;
    qreal zoom = 1;
    QPointF offset;
    QPointF last;
    bool automaticFit = true;
    bool panning = false;
    bool dragging = false;
    bool editing = true;
    bool selectedVertical = true;
    int selectedIndex = -1;
    bool hoveredVertical = true;
    int hoveredIndex = -1;
    std::optional<QPointF> hoverPosition;
    qint64 imageKey = 0;
    ImageSplitSettings previousSettings;
    QPixmap checkerTile;
    QColor checkerBackground;
    QColor checkerFill;
    // Declared last so callbacks disconnect before their captured state is destroyed.
    QObject callbackContext;

    QRectF viewport() const {
        // Reserve room for the complete Y hint even when the image is panned past the left edge.
        const qreal gutter = std::max(
            kRulerWidth,
            positionBadgeSize(owner, canvasText("Y: %1 px").arg(document->image().height()))
                    .width() +
                kPositionBadgeSpacing);
        return QRectF(gutter, kRulerHeight, std::max(1.0, owner->width() - gutter),
                      std::max(1.0, owner->height() - kRulerHeight));
    }
    QRectF imageRect() const {
        return QRectF(offset, QSizeF(document->image().size()) * zoom);
    }
    QRectF visibleImageRect() const {
        return imageRect().intersected(viewport());
    }
    QRectF rulerRect(bool horizontal) const {
        const auto visible = visibleImageRect();
        if (visible.isEmpty())
            return {};
        return horizontal ? QRectF(visible.left(), visible.top() - kRulerHeight, visible.width(),
                                   kRulerHeight)
                          : QRectF(visible.left() - kRulerWidth, visible.top(), kRulerWidth,
                                   visible.height());
    }
    bool axisAllowed(bool vertical) const {
        const auto direction = document->settings().direction;
        return direction == ImageSplitDirection::Grid ||
               (vertical ? direction == ImageSplitDirection::Vertical
                         : direction == ImageSplitDirection::Horizontal);
    }
    const auto& lines(const ImageSplitLayout& layout, bool vertical) const {
        return vertical ? layout.verticalLines : layout.horizontalLines;
    }
    std::optional<std::pair<bool, int>> rulerLineAt(QPointF point) const {
        if (!editing || !document->layout().isValid())
            return std::nullopt;
        const auto pixel = owner->viewportToImage(point);
        for (const bool vertical : {true, false}) {
            if (!axisAllowed(vertical) || !rulerRect(vertical).contains(point))
                continue;
            const int coordinate = roundedCoordinate(vertical ? pixel.x() : pixel.y());
            const int dimension = vertical ? document->image().width() : document->image().height();
            if (coordinate > 0 && coordinate < dimension)
                return std::pair{vertical, coordinate};
        }
        return std::nullopt;
    }
    std::optional<std::pair<bool, int>> hoveredRulerLine() const {
        if (!hoverPosition || dragging || panning)
            return std::nullopt;
        // Resolve against the current transform so zoom and pan cannot leave a stale preview.
        return rulerLineAt(*hoverPosition);
    }
    OverlayState overlayState() const {
        const auto layout = document->layout();
        const auto coordinate = [&](bool vertical, int index) -> PositionLine {
            const auto& boundaries = lines(layout, vertical);
            return index >= 0 && index < boundaries.size()
                       ? PositionLine{std::pair{vertical, boundaries[index]}}
                       : std::nullopt;
        };
        return {coordinate(selectedVertical, selectedIndex),
                coordinate(hoveredVertical, hoveredIndex), hoveredRulerLine()};
    }
    PositionLine positionLine(const OverlayState& state) const {
        return state.ruler ? state.ruler : state.hovered ? state.hovered : state.selected;
    }
    QString positionText(const PositionLine& line) const {
        return (line->first ? canvasText("X: %1 px") : canvasText("Y: %1 px")).arg(line->second);
    }
    QRectF positionBadgeRect(const PositionLine& line) const {
        if (!line)
            return {};
        const auto [vertical, coordinate] = *line;
        const auto ruler = rulerRect(vertical);
        const auto point = vertical ? owner->imageToViewport({qreal(coordinate), 0})
                                    : owner->imageToViewport({0, qreal(coordinate)});
        const auto center = vertical ? QPointF(point.x(), ruler.center().y())
                                     : QPointF(ruler.center().x(), point.y());
        if (!ruler.contains(center))
            return {};
        const auto badge = positionBadgeSize(owner, positionText(line));
        // Painting and overlay damage share the same bounds outside the visible image.
        const qreal left = vertical ? center.x() - badge.width() / 2
                                    : ruler.right() - kPositionBadgeSpacing - badge.width();
        const qreal top = vertical ? ruler.bottom() - kPositionBadgeSpacing - badge.height()
                                   : center.y() - badge.height() / 2;
        return {QPointF(std::clamp(left, 0.0, std::max(0.0, owner->width() - badge.width())),
                        std::clamp(top, 0.0, std::max(0.0, owner->height() - badge.height()))),
                badge};
    }
    QRegion overlayDamage(const OverlayState& state) const {
        QRegion result;
        const auto visible = visibleImageRect();
        for (const auto& line : {state.selected, state.hovered, state.ruler}) {
            if (!line || visible.isEmpty())
                continue;
            const auto [vertical, coordinate] = *line;
            const qreal position = vertical ? owner->imageToViewport({qreal(coordinate), 0}).x()
                                            : owner->imageToViewport({0, qreal(coordinate)}).y();
            const QRectF strip =
                vertical ? QRectF(position - 2, visible.top() - 2, 4, visible.height() + 4)
                         : QRectF(visible.left() - 2, position - 2, visible.width() + 4, 4);
            result += strip.intersected(owner->rect()).toAlignedRect();
        }
        const auto badge = positionBadgeRect(positionLine(state));
        if (!badge.isEmpty())
            result += badge.adjusted(-2, -2, 2, 2).toAlignedRect();
        return result.intersected(owner->rect());
    }
    void updateOverlay(const OverlayState& before) {
        const auto after = overlayState();
        if (before == after)
            return;
        const auto damage = overlayDamage(before) + overlayDamage(after);
        if (!damage.isEmpty())
            owner->update(damage);
    }
    std::pair<bool, int> hit(QPointF point) const {
        const auto layout = document->layout();
        if (!viewport().contains(point) || rulerRect(true).contains(point) ||
            rulerRect(false).contains(point))
            return {true, -1};
        if (!imageRect()
                 .adjusted(-kLineHitDistance, -kLineHitDistance, kLineHitDistance, kLineHitDistance)
                 .contains(point))
            return {true, -1};
        bool axis = true;
        int result = -1;
        qreal closest = kLineHitDistance;
        const auto imagePoint = owner->viewportToImage(point);
        for (const bool vertical : {true, false}) {
            if (!axisAllowed(vertical))
                continue;
            const auto& boundaries = lines(layout, vertical);
            const qreal value = vertical ? imagePoint.x() : imagePoint.y();
            const auto candidate = boundaries.coordinateLowerBound(value);
            for (int shift : {-1, 0}) {
                const int index = static_cast<int>(candidate) + shift;
                if (index < 0 || index >= boundaries.size())
                    continue;
                const qreal distance = std::abs(boundaries[index] - value) * zoom;
                if (distance <= closest) {
                    closest = distance;
                    result = index;
                    axis = vertical;
                }
            }
        }
        return {axis, result};
    }
    void refresh() {
        const auto key = document->image().cacheKey();
        const auto settings = document->settings();
        if (key != imageKey) {
            imageKey = key;
            selectedIndex = -1;
            hoveredIndex = -1;
            hoverPosition.reset();
            owner->fitToCanvas();
        } else if (!(settings == previousSettings)) {
            selectedIndex = -1;
            hoveredIndex = -1;
        }
        previousSettings = settings;
        const auto layout = document->layout();
        if (selectedIndex >= lines(layout, selectedVertical).size())
            selectedIndex = -1;
        if (hoveredIndex >= lines(layout, hoveredVertical).size())
            hoveredIndex = -1;
        owner->update();
    }
};

ImageSplitPreviewCanvas::ImageSplitPreviewCanvas(ImageSplitDocument* document, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.document = document;
    setObjectName(QStringLiteral("imageSplitCanvas"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(180, 160);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    connect(document, &ImageSplitDocument::lineDragIndexChanged, &d.callbackContext,
            [this](int index) {
                auto& d = *m_impl;
                if (d.dragging) {
                    d.selectedIndex = index;
                    d.hoveredIndex = -1;
                }
            });
    connect(document, &ImageSplitDocument::changed, &d.callbackContext,
            [this] { m_impl->refresh(); });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
            &d.callbackContext, [this] { update(); });
    retranslateUi();
    d.refresh();
}
ImageSplitPreviewCanvas::~ImageSplitPreviewCanvas() = default;
qreal ImageSplitPreviewCanvas::zoom() const {
    return m_impl->zoom;
}
QPointF ImageSplitPreviewCanvas::offset() const {
    return m_impl->offset;
}
QPointF ImageSplitPreviewCanvas::imageToViewport(QPointF point) const {
    return m_impl->offset + point * m_impl->zoom;
}
QPointF ImageSplitPreviewCanvas::viewportToImage(QPointF point) const {
    return (point - m_impl->offset) / m_impl->zoom;
}
void ImageSplitPreviewCanvas::fitToCanvas() {
    auto& d = *m_impl;
    if (d.document->image().isNull())
        return;
    const auto area = d.viewport().adjusted(24, 24, -24, -24);
    const auto size = d.document->image().size();
    d.automaticFit = true;
    d.zoom = std::min({1.0, std::max(1.0, area.width()) / size.width(),
                       std::max(1.0, area.height()) / size.height()});
    d.offset = d.viewport().center() - QPointF(size.width(), size.height()) * d.zoom / 2;
    update();
}
void ImageSplitPreviewCanvas::setEditingEnabled(bool enabled) {
    auto& d = *m_impl;
    if (d.editing == enabled)
        return;
    const auto before = d.overlayState();
    if (!enabled && d.dragging)
        d.document->cancelLineDrag();
    d.dragging = false;
    d.editing = enabled;
    d.hoveredIndex = -1;
    d.hoverPosition.reset();
    unsetCursor();
    d.updateOverlay(before);
}
void ImageSplitPreviewCanvas::retranslateUi() {
    setAccessibleName(canvasText("Image splitting preview"));
    setAccessibleDescription(canvasText(
        "Click a ruler to add a split line; drag a line to move it. Select a line and press Delete "
        "to remove it. Drag with the middle mouse button to pan; scroll to zoom"));
    if (m_impl->automaticFit)
        fitToCanvas();
    update();
}

void ImageSplitPreviewCanvas::paintEvent(QPaintEvent* event) {
    auto& d = *m_impl;
    const auto damage = event->region();
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto viewport = d.viewport();
    const auto size = d.document->image().size();
    const auto layout = d.document->layout();
    const auto rulerLine = d.hoveredRulerLine();
    painter.save();
    painter.setClipRect(viewport);
    const auto imageRect = d.imageRect();
    if (!size.isEmpty()) {
        // The checker is a small logical-pixel tile, independent of image size and canvas zoom.
        if (d.checkerTile.isNull() || d.checkerBackground != theme.colorBgContainer ||
            d.checkerFill != theme.colorFillSecondary) {
            d.checkerBackground = theme.colorBgContainer;
            d.checkerFill = theme.colorFillSecondary;
            d.checkerTile = QPixmap(20, 20);
            d.checkerTile.fill(d.checkerBackground);
            QPainter checker(&d.checkerTile);
            checker.fillRect(0, 0, 10, 10, d.checkerFill);
            checker.fillRect(10, 10, 10, 10, d.checkerFill);
        }
        painter.fillRect(imageRect, QBrush(d.checkerTile));
        const auto& preview = d.document->preview();
        const qreal previewScale = preview.isNull()
                                       ? 0
                                       : std::min(qreal(preview.width()) / size.width(),
                                                  qreal(preview.height()) / size.height());
        const qreal displayScale = d.zoom * devicePixelRatioF();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, displayScale < 1);
        if (!preview.isNull() && displayScale < previewScale) {
            painter.drawImage(imageRect, preview);
        } else {
            // Draw only the visible source pixels when the preview no longer has enough detail.
            // This avoids allocating a full-size pixmap or scaling the source on the GUI thread.
            const QRectF visiblePixels =
                QRectF(viewportToImage(viewport.topLeft()), viewportToImage(viewport.bottomRight()))
                    .intersected(QRectF(QPointF{}, QSizeF(size)));
            if (!visiblePixels.isEmpty()) {
                const QRectF visibleTarget(imageToViewport(visiblePixels.topLeft()),
                                           visiblePixels.size() * d.zoom);
                painter.drawImage(visibleTarget, d.document->image(), visiblePixels);
            }
        }
        if (layout.overlapWidth > 0) {
            const auto visible = imageRect.intersected(viewport);
            QPainterPath bands;
            bands.setFillRule(Qt::WindingFill);
            const auto appendBands = [&](bool vertical) {
                const auto& boundaries = d.lines(layout, vertical);
                if (boundaries.isEmpty() || visible.isEmpty())
                    return;
                const qreal first = vertical ? visible.left() : visible.top();
                const qreal last = vertical ? visible.right() : visible.bottom();
                const qreal origin = vertical ? d.offset.x() : d.offset.y();
                const auto transform = painter.deviceTransform();
                const qreal scale =
                    std::max(1.0, std::abs(vertical ? transform.m11() : transform.m22()));
                const int firstPixel = static_cast<int>(std::floor(first * scale));
                const int lastPixel = static_cast<int>(std::ceil(last * scale));
                std::optional<qreal> start;
                const auto append = [&](qreal end) {
                    if (!start)
                        return;
                    bands.addRect(
                        vertical ? QRectF(*start, visible.top(), end - *start, visible.height())
                                 : QRectF(visible.left(), *start, visible.width(), end - *start));
                    start.reset();
                };
                // Rasterize the union at display-pixel resolution. Binary searches keep even
                // millions of dense or overlapping guides bounded by the visible canvas size.
                // Outward rounding retains bands narrower than a display pixel when zoomed out.
                for (int pixel = firstPixel; pixel < lastPixel; ++pixel) {
                    const qreal begin = std::max(first, pixel / scale);
                    const qreal end = std::min(last, (pixel + 1) / scale);
                    const qreal sourceBegin = (begin - origin) / d.zoom;
                    const qreal sourceEnd = (end - origin) / d.zoom;
                    const auto index =
                        boundaries.coordinateUpperBound(sourceBegin - layout.overlapWidth);
                    const bool covered = index < boundaries.size() &&
                                         boundaries[index] < sourceEnd + layout.overlapWidth;
                    if (covered) {
                        if (!start)
                            start = begin;
                    } else {
                        append(begin);
                    }
                }
                append(last);
            };
            appendBands(true);
            appendBands(false);
            auto color = kSplitLineColor;
            color.setAlphaF(0.2F);
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.fillPath(bands, color);
            painter.restore();
        }
        painter.setPen(QPen(theme.colorBorder, 1));
        painter.drawRect(imageRect);
        const auto drawLines = [&](bool vertical) {
            const auto& lines = d.lines(layout, vertical);
            const int selected = d.selectedVertical == vertical ? d.selectedIndex : -1;
            const int hovered = d.hoveredVertical == vertical ? d.hoveredIndex : -1;
            const int preview = rulerLine && rulerLine->first == vertical ? rulerLine->second : -1;
            const qreal visibleFirst = vertical ? viewportToImage(viewport.topLeft()).x()
                                                : viewportToImage(viewport.topLeft()).y();
            const qreal visibleLast = vertical ? viewportToImage(viewport.bottomRight()).x()
                                               : viewportToImage(viewport.bottomRight()).y();
            const auto drawLine = [&](int coordinate, bool highlighted) {
                const qreal position = vertical ? imageToViewport({qreal(coordinate), 0}).x()
                                                : imageToViewport({0, qreal(coordinate)}).y();
                const QRectF strip =
                    vertical ? QRectF(position - 2, imageRect.top() - 2, 4, imageRect.height() + 4)
                             : QRectF(imageRect.left() - 2, position - 2, imageRect.width() + 4, 4);
                if (!damage.intersects(strip.intersected(viewport).toAlignedRect()))
                    return;
                QPen pen(kSplitLineColor, 1.0);
                if (highlighted)
                    pen.setCapStyle(Qt::FlatCap);
                else
                    snowCanvasApplySelectionDashStyle(pen);
                painter.setPen(pen);
                if (vertical)
                    painter.drawLine(QPointF(position, imageRect.top()),
                                     QPointF(position, imageRect.bottom()));
                else
                    painter.drawLine(QPointF(imageRect.left(), position),
                                     QPointF(imageRect.right(), position));
            };
            auto index = lines.coordinateLowerBound(visibleFirst);
            while (index < lines.size() && lines[index] <= visibleLast) {
                const int coordinate = lines[index];
                if (index != selected && index != hovered && coordinate != preview)
                    drawLine(coordinate, false);
                // Jump over subpixel guides instead of traversing a potentially dense axis.
                const qreal nextCoordinate = coordinate + std::max(1.0, kGuideSpacing / d.zoom);
                index = lines.coordinateLowerBound(nextCoordinate);
            }
            const auto drawHighlighted = [&](int index) {
                if (index >= 0 && index < lines.size() && lines[index] != preview &&
                    lines[index] >= visibleFirst && lines[index] <= visibleLast)
                    drawLine(lines[index], true);
            };
            drawHighlighted(selected);
            if (hovered != selected)
                drawHighlighted(hovered);
            if (preview >= 0)
                drawLine(preview, true);
        };
        drawLines(true);
        drawLines(false);
        painter.setFont(font());
        const QFontMetrics metrics(font());
        constexpr int minimumCellHeight =
            SequenceNumberBadgeStyle::height + SequenceNumberBadgeStyle::inset + 2;
        const auto readableCells = [&](bool horizontal, qreal minimumSpan) {
            QVector<int> result;
            const auto& lines = horizontal ? layout.verticalLines : layout.horizontalLines;
            const int dimension = horizontal ? layout.size.width() : layout.size.height();
            const qreal first = horizontal ? viewport.left() : viewport.top();
            const qreal last = horizontal ? viewport.right() : viewport.bottom();
            const qreal origin = horizontal ? d.offset.x() : d.offset.y();
            const qreal step = minimumSpan / 2;
            int previous = -1;
            const auto sample = [&](qreal position) {
                const qreal coordinate = (position - origin) / d.zoom;
                if (coordinate < 0 || coordinate > dimension)
                    return;
                const int index = static_cast<int>(lines.coordinateUpperBound(coordinate));
                if (index == previous)
                    return;
                previous = index;
                const int start = index == 0 ? 0 : lines[index - 1];
                const int end = index == lines.size() ? dimension : lines[index];
                if ((end - start) * d.zoom >= minimumSpan)
                    result.append(index);
            };
            // A readable cell contains a sample, even after thousands of subpixel cells. Work is
            // bounded by viewport pixels, with no allocation or traversal of the region product.
            for (qreal position = first; position < last; position += step)
                sample(position);
            sample(last);
            return result;
        };
        const auto readableColumns = readableCells(
            true, SequenceNumberBadgeStyle::size(metrics, QStringLiteral("1")).width() +
                      SequenceNumberBadgeStyle::inset + 2);
        const auto readableRows = readableCells(false, minimumCellHeight);
        const qint64 columns = layout.verticalLines.size() + 1;
        for (const int row : readableRows) {
            for (const int column : readableColumns) {
                const auto index = row * columns + column;
                const auto region = layout.baseRegionAt(index);
                const QRectF target(imageToViewport(region.topLeft()),
                                    QSizeF(region.size()) * d.zoom);
                // An ordinal stays at the top of its cell. Avoid preparing text for badges that
                // cannot intersect a narrow guide or coordinate-hint repaint.
                if (!damage.intersects(
                        QRectF(target.topLeft(), QSizeF(target.width(), minimumCellHeight))
                            .toAlignedRect()))
                    continue;
                const auto label = QString::number(index + 1);
                const auto badge = SequenceNumberBadgeStyle::size(metrics, label);
                if (target.width() < badge.width() + SequenceNumberBadgeStyle::inset + 2)
                    continue;
                const QRectF badgeRect(target.topLeft() + QPointF(SequenceNumberBadgeStyle::inset,
                                                                  SequenceNumberBadgeStyle::inset),
                                       QSizeF(badge));
                if (!badgeRect.intersects(viewport) ||
                    !damage.intersects(badgeRect.adjusted(-1, -1, 1, 1).toAlignedRect()))
                    continue;
                SequenceNumberBadgeStyle::paint(painter, badgeRect, label);
            }
        }
    }
    painter.restore();
    const auto visibleImageRect = d.visibleImageRect();
    if (visibleImageRect.isEmpty())
        return;
    QFont rulerFont = font();
    rulerFont.setPixelSize(10);
    painter.setFont(rulerFont);
    const QFontMetrics rulerMetrics(rulerFont);
    for (const bool horizontal : {true, false}) {
        const auto ruler = d.rulerRect(horizontal);
        if (!damage.intersects(ruler.toAlignedRect()))
            continue;
        painter.save();
        painter.setClipRect(ruler);
        painter.fillRect(ruler, theme.colorBgContainer);
        painter.setPen(QPen(theme.colorBorderSecondary, 1.0));
        painter.drawLine(horizontal ? ruler.bottomLeft() : ruler.topRight(), ruler.bottomRight());
        const qreal desired = 80 / d.zoom;
        const qreal magnitude = std::pow(10.0, std::floor(std::log10(std::max(1.0, desired))));
        qreal major = magnitude;
        for (qreal factor : {1.0, 2.0, 5.0, 10.0}) {
            if (magnitude * factor >= desired) {
                major = magnitude * factor;
                break;
            }
        }
        major = std::max(1.0, major);
        const qreal minor = std::max(1.0, major / 5);
        const qreal first = horizontal ? viewportToImage(visibleImageRect.topLeft()).x()
                                       : viewportToImage(visibleImageRect.topLeft()).y();
        const qreal last = horizontal ? viewportToImage(visibleImageRect.bottomRight()).x()
                                      : viewportToImage(visibleImageRect.bottomRight()).y();
        const qreal dimension = horizontal ? size.width() : size.height();
        const qreal begin = std::max(0.0, std::ceil(first / minor) * minor);
        for (qreal coordinate = begin; coordinate <= std::min(last, dimension);
             coordinate += minor) {
            const auto point =
                imageToViewport(horizontal ? QPointF(coordinate, 0) : QPointF(0, coordinate));
            const bool majorTick =
                std::abs(coordinate / major - std::round(coordinate / major)) < .001;
            painter.setPen(QPen(theme.colorTextTertiary, 1.0));
            if (horizontal) {
                painter.drawLine(QPointF(point.x(), ruler.bottom()),
                                 QPointF(point.x(), ruler.bottom() - (majorTick ? 8 : 4)));
                if (majorTick) {
                    const auto label = QString::number(roundedCoordinate(coordinate));
                    const qreal span =
                        std::min(qreal(rulerMetrics.horizontalAdvance(label) + 6), ruler.width());
                    const qreal left = std::clamp(point.x() - span / 2, ruler.left(),
                                                  std::max(ruler.left(), ruler.right() - span));
                    painter.drawText(QRectF(left, ruler.top() + 1, span, 16), Qt::AlignCenter,
                                     label);
                }
            } else {
                painter.drawLine(QPointF(ruler.right(), point.y()),
                                 QPointF(ruler.right() - (majorTick ? 8 : 4), point.y()));
                if (majorTick) {
                    painter.save();
                    const auto label = QString::number(roundedCoordinate(coordinate));
                    const qreal span =
                        std::min(qreal(rulerMetrics.horizontalAdvance(label) + 6), ruler.height());
                    const qreal top = std::clamp(point.y() - span / 2, ruler.top(),
                                                 std::max(ruler.top(), ruler.bottom() - span));
                    painter.translate(ruler.left() + 16, top + span / 2);
                    painter.rotate(-90);
                    painter.drawText(QRectF(-span / 2, -8, span, 16), Qt::AlignCenter, label);
                    painter.restore();
                }
            }
        }
        painter.restore();
    }
    const auto positionLine = d.positionLine(d.overlayState());
    const auto badgeRect = d.positionBadgeRect(positionLine);
    if (!badgeRect.isEmpty() &&
        damage.intersects(badgeRect.adjusted(-1, -1, 1, 1).toAlignedRect())) {
        painter.setFont(positionFont(this));
        const auto label = d.positionText(positionLine);
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.colorPrimary);
        painter.drawRoundedRect(badgeRect, 3, 3);
        painter.setPen(theme.colorTextLightSolid);
        painter.drawText(badgeRect, Qt::AlignCenter, label);
        painter.restore();
    }
}

void ImageSplitPreviewCanvas::resizeEvent(QResizeEvent*) {
    if (m_impl->automaticFit)
        fitToCanvas();
    update();
}
void ImageSplitPreviewCanvas::mousePressEvent(QMouseEvent* event) {
    auto& d = *m_impl;
    const auto before = d.overlayState();
    d.hoverPosition = event->position();
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::MiddleButton) {
        d.panning = true;
        d.automaticFit = false;
        d.last = event->position();
        setCursor(Qt::ClosedHandCursor);
        d.updateOverlay(before);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !d.editing || d.document->image().isNull()) {
        d.updateOverlay(before);
        return;
    }
    const auto point = event->position();
    const auto create = [&d](bool vertical, int coordinate) {
        static_cast<void>(d.document->createLine(vertical, coordinate));
        const auto layout = d.document->layout();
        const auto& lines = d.lines(layout, vertical);
        const auto index = lines.coordinateLowerBound(coordinate);
        if (index < lines.size() && lines[index] == coordinate) {
            d.selectedVertical = vertical;
            d.selectedIndex = static_cast<int>(index);
            d.hoveredIndex = -1;
        }
    };
    if (const auto line = d.rulerLineAt(point)) {
        create(line->first, line->second);
    } else {
        const auto [vertical, index] = d.hit(point);
        d.selectedVertical = vertical;
        d.selectedIndex = index;
        d.hoveredIndex = -1;
        if (index >= 0 && d.document->beginLineDrag(vertical, index)) {
            d.dragging = true;
            setCursor(vertical ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        }
    }
    d.updateOverlay(before);
    event->accept();
}
void ImageSplitPreviewCanvas::mouseMoveEvent(QMouseEvent* event) {
    auto& d = *m_impl;
    const auto before = d.overlayState();
    d.hoverPosition = event->position();
    if (d.panning) {
        d.offset += event->position() - d.last;
        d.last = event->position();
        update();
    } else if (d.dragging) {
        const auto point = viewportToImage(event->position());
        d.document->moveDraggedLine(roundedCoordinate(d.selectedVertical ? point.x() : point.y()));
    } else if (d.editing) {
        const auto [vertical, index] = d.hit(event->position());
        d.hoveredVertical = vertical;
        d.hoveredIndex = index;
        if (index >= 0)
            setCursor(vertical ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        else if (d.hoveredRulerLine())
            setCursor(Qt::CrossCursor);
        else
            unsetCursor();
        d.updateOverlay(before);
    }
}
void ImageSplitPreviewCanvas::mouseReleaseEvent(QMouseEvent* event) {
    auto& d = *m_impl;
    const auto before = d.overlayState();
    d.hoverPosition = event->position();
    if (event->button() == Qt::MiddleButton)
        d.panning = false;
    if (event->button() == Qt::LeftButton && d.dragging) {
        d.document->endLineDrag();
        d.dragging = false;
    }
    unsetCursor();
    d.updateOverlay(before);
}
void ImageSplitPreviewCanvas::wheelEvent(QWheelEvent* event) {
    auto& d = *m_impl;
    const auto before = d.overlayState();
    d.hoverPosition = event->position();
    if (d.document->image().isNull())
        return;
    const double steps = usesPreciseWheelDelta(*event) ? event->pixelDelta().y() / 100.0
                                                       : event->angleDelta().y() / 120.0;
    if (steps != 0) {
        const qreal next = std::clamp(d.zoom * std::pow(1.15, steps), 0.000000001, 16.0);
        d.offset = event->position() - (event->position() - d.offset) * (next / d.zoom);
        d.zoom = next;
        d.automaticFit = false;
        update();
    } else {
        d.updateOverlay(before);
    }
    event->accept();
}
void ImageSplitPreviewCanvas::keyPressEvent(QKeyEvent* event) {
    auto& d = *m_impl;
    if (event->key() == Qt::Key_Escape && d.dragging) {
        d.document->cancelLineDrag();
        d.dragging = false;
        unsetCursor();
    } else if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && d.editing) {
        if (d.selectedIndex >= 0)
            d.document->removeLine(d.selectedVertical, d.selectedIndex);
        d.selectedIndex = -1;
        d.hoveredIndex = -1;
    } else if (event->key() == Qt::Key_F || event->key() == Qt::Key_Home) {
        fitToCanvas();
    } else if (d.editing && shortcuts::matchesStandardShortcut(*event, QKeySequence::Undo)) {
        d.document->undo();
    } else if (d.editing && shortcuts::matchesStandardShortcut(*event, QKeySequence::Redo)) {
        d.document->redo();
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
    update();
}
void ImageSplitPreviewCanvas::leaveEvent(QEvent*) {
    const auto before = m_impl->overlayState();
    m_impl->hoveredIndex = -1;
    m_impl->hoverPosition.reset();
    if (!m_impl->dragging && !m_impl->panning)
        unsetCursor();
    m_impl->updateOverlay(before);
}
void ImageSplitPreviewCanvas::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    else if (event->type() == QEvent::FontChange && m_impl->automaticFit)
        fitToCanvas();
    QWidget::changeEvent(event);
}

struct ImageSplitEditorWidget::Impl {
    ImageSplitEditorWidget* owner = nullptr;
    ImageSplitDocument* document = nullptr;
    ImageSplitPreviewCanvas* canvas = nullptr;
    AdForm* form = nullptr;
    AdSegmented* direction = nullptr;
    AdSegmented* type = nullptr;
    AdSegmented* unit = nullptr;
    AdFormItem* directionField = nullptr;
    AdFormItem* typeField = nullptr;
    AdFormItem* overlapField = nullptr;
    AdFormItem* countField = nullptr;
    AdFormItem* rowsField = nullptr;
    AdFormItem* columnsField = nullptr;
    AdFormItem* sizeField = nullptr;
    AdInputNumber* count = nullptr;
    AdInputNumber* overlap = nullptr;
    AdInputNumber* rows = nullptr;
    AdInputNumber* columns = nullptr;
    AdInputNumber* size = nullptr;
    ImageDropSurface* emptyState = nullptr;
    AdButton* emptySelect = nullptr;
    ImageDropSurface* dropHint = nullptr;
    QWidget* description = nullptr;
    QLabel* dimensions = nullptr;
    QLabel* total = nullptr;
    std::array<AdButton*, 4> actions{};
    QMap<QString, QString> drafts;
    bool syncing = false;
    bool loading = false;
    bool exporting = false;
    bool valid = false;
    qint64 imageKey = 0;
    ImageSplitSettings lastSettings;
    ImageSplitUnit displayedUnit = ImageSplitUnit::Percentage;
    mutable std::optional<ImageSplitSettings> validatedSettings;
    mutable QSize validatedSize;
    mutable QMap<QString, QString> cachedErrors;
    // Retire callbacks before the captured state dies and QWidget tears down focused inputs.
    QObject callbackContext;

    bool busy() const {
        return loading || exporting;
    }
    bool acceptsImageDrop(const QMimeData* mime, Qt::DropActions dropActions) const {
        return !busy() && dropActions.testFlag(Qt::CopyAction) && !droppedPaths(mime).isEmpty();
    }
    void positionDropHint() {
        const auto* target = document->image().isNull() ? static_cast<QWidget*>(emptyState)
                                                        : static_cast<QWidget*>(canvas);
        dropHint->setGeometry(target->geometry());
        dropHint->raise();
    }
    QString sizeKey() const {
        return unit->currentValue() == QStringLiteral("pixels") ? QStringLiteral("pixels")
                                                                : QStringLiteral("percentage");
    }
    std::pair<ImageSplitSettings, QMap<QString, QString>> draftSettings() const {
        auto settings = document->settings();
        settings.direction = direction->currentValue() == QStringLiteral("horizontal")
                                 ? ImageSplitDirection::Horizontal
                             : direction->currentValue() == QStringLiteral("grid")
                                 ? ImageSplitDirection::Grid
                                 : ImageSplitDirection::Vertical;
        settings.type = type->currentValue() == QStringLiteral("precise") ? ImageSplitType::Precise
                                                                          : ImageSplitType::Equal;
        settings.unit = unit->currentValue() == QStringLiteral("pixels")
                            ? ImageSplitUnit::Pixels
                            : ImageSplitUnit::Percentage;
        QMap<QString, QString> errors;
        const auto integer = [&](const QString& key, int& destination, int minimum) {
            const auto draft = drafts.value(key).trimmed();
            static const QRegularExpression pattern(QStringLiteral("^[0-9]+$"));
            bool ok = false;
            const qlonglong value = draft.toLongLong(&ok);
            if (!pattern.match(draft).hasMatch() || !ok || value < minimum ||
                value > std::numeric_limits<int>::max()) {
                errors.insert(key, minimum == 0 ? editorText("Enter a non-negative integer")
                                   : minimum == 16
                                       ? editorText("Enter an integer of at least 16 pixels")
                                       : editorText("Enter a positive integer"));
            } else {
                destination = static_cast<int>(value);
            }
        };
        integer(QStringLiteral("overlapWidth"), settings.overlapWidth, 0);
        if (settings.direction == ImageSplitDirection::Grid) {
            integer(QStringLiteral("rows"), settings.rows, 1);
            integer(QStringLiteral("columns"), settings.columns, 1);
        } else if (settings.type == ImageSplitType::Equal) {
            integer(QStringLiteral("count"), settings.count, 1);
        } else if (settings.unit == ImageSplitUnit::Pixels) {
            integer(QStringLiteral("pixels"), settings.pixels, 16);
        } else {
            // The model validates and calculates the complete decimal exactly. Only conversion
            // from pixels formats two decimal places; user-entered precision remains intact.
            settings.percentage = drafts.value(QStringLiteral("percentage")).trimmed();
        }
        if (errors.isEmpty()) {
            if (!validatedSettings || !(*validatedSettings == settings) ||
                validatedSize != document->image().size()) {
                validatedSettings = settings;
                validatedSize = document->image().size();
                cachedErrors = calculateImageSplitLayout(validatedSize, settings).errors;
            }
            errors = cachedErrors;
        }
        return {settings, errors};
    }
    void displayError(AdFormItem* field, AdInputNumber* input, const QString& key,
                      const QMap<QString, QString>& errors) {
        const auto error = errors.value(key);
        field->setErrorMessages(error.isEmpty() ? QStringList{} : QStringList{error});
        field->setValidateStatus(error.isEmpty() ? AdFormItem::ValidateStatus::None
                                                 : AdFormItem::ValidateStatus::Error);
        input->setStatus(error.isEmpty() ? AdInputNumber::Status::None
                                         : AdInputNumber::Status::Error);
    }
    void validate() {
        const auto [settings, errors] = draftSettings();
        Q_UNUSED(settings);
        displayError(countField, count, QStringLiteral("count"), errors);
        displayError(overlapField, overlap, QStringLiteral("overlapWidth"), errors);
        displayError(rowsField, rows, QStringLiteral("rows"), errors);
        displayError(columnsField, columns, QStringLiteral("columns"), errors);
        displayError(sizeField, size, sizeKey(), errors);
        const bool next = errors.isEmpty() && document->layout().isValid();
        if (valid != next) {
            valid = next;
            emit owner->validationChanged();
        }
        updateActions();
    }
    void commit() {
        if (syncing || busy())
            return;
        const auto [settings, errors] = draftSettings();
        if (errors.isEmpty() && !(settings == document->settings()))
            document->setSettings(settings);
        validate();
    }
    void updateActions() {
        const bool available = !busy() && !document->image().isNull();
        actions[0]->setEnabled(available && document->canUndo());
        actions[1]->setEnabled(available && document->canRedo());
        actions[2]->setEnabled(!document->image().isNull());
        actions[3]->setEnabled(!busy());
        emptySelect->setEnabled(!busy());
        emptySelect->setBusy(loading);
        form->setEnabled(!busy());
        const auto displayedDirection = direction->currentValue() == QStringLiteral("horizontal")
                                            ? ImageSplitDirection::Horizontal
                                        : direction->currentValue() == QStringLiteral("grid")
                                            ? ImageSplitDirection::Grid
                                            : ImageSplitDirection::Vertical;
        canvas->setEditingEnabled(available && document->layout().isValid() &&
                                  displayedDirection == document->settings().direction);
        if (busy())
            dropHint->hide();
    }
    void updateVisibility() {
        const bool grid = direction->currentValue() == QStringLiteral("grid");
        const bool equal = type->currentValue() == QStringLiteral("equal");
        typeField->setItemHidden(grid);
        countField->setItemHidden(grid || !equal);
        sizeField->setItemHidden(grid || equal);
        rowsField->setItemHidden(!grid);
        columnsField->setItemHidden(!grid);
        sizeField->setLabel(direction->currentValue() == QStringLiteral("horizontal")
                                ? editorText("Height per image")
                                : editorText("Width per image"));
        size->setAccessibleName(sizeField->label());
    }
    void sync() {
        syncing = true;
        const auto settings = document->settings();
        direction->setCurrentValue(
            settings.direction == ImageSplitDirection::Vertical     ? QStringLiteral("vertical")
            : settings.direction == ImageSplitDirection::Horizontal ? QStringLiteral("horizontal")
                                                                    : QStringLiteral("grid"));
        type->setCurrentValue(settings.type == ImageSplitType::Equal ? QStringLiteral("equal")
                                                                     : QStringLiteral("precise"));
        unit->setCurrentValue(settings.unit == ImageSplitUnit::Pixels
                                  ? QStringLiteral("pixels")
                                  : QStringLiteral("percentage"));
        count->setValue(settings.count);
        overlap->setValue(settings.overlapWidth);
        rows->setValue(settings.rows);
        columns->setValue(settings.columns);
        size->setDecimals(-1);
        size->setValueMode(settings.unit == ImageSplitUnit::Pixels
                               ? AdInputNumber::ValueMode::Number
                               : AdInputNumber::ValueMode::ExactDecimal);
        size->setRange(settings.unit == ImageSplitUnit::Pixels ? 16 : 5,
                       settings.unit == ImageSplitUnit::Pixels ? std::numeric_limits<int>::max()
                                                               : 100);
        size->setSuffixText(settings.unit == ImageSplitUnit::Pixels ? editorText(" px")
                                                                    : QStringLiteral("%"));
        if (settings.unit == ImageSplitUnit::Pixels)
            size->setValue(settings.pixels);
        else
            size->setExactValue(settings.percentage);
        drafts.insert(QStringLiteral("count"), QString::number(settings.count));
        drafts.insert(QStringLiteral("overlapWidth"), QString::number(settings.overlapWidth));
        drafts.insert(QStringLiteral("rows"), QString::number(settings.rows));
        drafts.insert(QStringLiteral("columns"), QString::number(settings.columns));
        drafts.insert(QStringLiteral("pixels"), QString::number(settings.pixels));
        drafts.insert(QStringLiteral("percentage"), settings.percentage);
        updateVisibility();
        lastSettings = settings;
        displayedUnit = settings.unit;
        imageKey = document->image().cacheKey();
        syncing = false;
    }
    void refresh() {
        if (!(document->settings() == lastSettings) || document->image().cacheKey() != imageKey)
            sync();
        const bool empty = document->image().isNull();
        canvas->setVisible(!empty);
        emptyState->setVisible(empty);
        // Restore the canvas geometry before callers fit or retain its viewport.
        owner->layout()->activate();
        positionDropHint();
        dropHint->title->setText(editorText(empty ? "Drop one image to split"
                                                  : "Drop one image to replace the current image"));
        dropHint->setAccessibleName(dropHint->title->text());
        const auto layout = document->layout();
        dimensions->setText(
            editorText("%1 × %2 px").arg(layout.size.width()).arg(layout.size.height()));
        dimensions->setVisible(!empty);
        total->setText(editorText("Total: %1 images").arg(layout.outputCount()));
        validate();
        updateActions();
    }
    void changeUnit(const QVariant& value) {
        if (syncing || busy())
            return;
        const bool toPixels = value.toString() == QStringLiteral("pixels");
        if (toPixels == (displayedUnit == ImageSplitUnit::Pixels))
            return;
        const auto sourceSize = document->image().size();
        const int dimension = direction->currentValue() == QStringLiteral("horizontal")
                                  ? sourceSize.height()
                                  : sourceSize.width();
        QString converted;
        if (toPixels) {
            const auto source = drafts.value(QStringLiteral("percentage")).trimmed();
            if (const auto pixels = imageSplitPercentagePixels(dimension, source))
                converted = QString::number(*pixels);
            drafts.insert(QStringLiteral("pixels"), converted);
        } else {
            bool ok = false;
            const qint64 pixels = drafts.value(QStringLiteral("pixels")).trimmed().toLongLong(&ok);
            if (ok && pixels >= 0 && pixels <= std::numeric_limits<int>::max() && dimension > 0) {
                const qint64 hundredths = (pixels * 10000 + dimension / 2) / dimension;
                converted = QStringLiteral("%1.%2")
                                .arg(hundredths / 100)
                                .arg(hundredths % 100, 2, 10, QChar(u'0'));
            }
            drafts.insert(QStringLiteral("percentage"), converted);
        }
        displayedUnit = toPixels ? ImageSplitUnit::Pixels : ImageSplitUnit::Percentage;
        syncing = true;
        size->setValueMode(toPixels ? AdInputNumber::ValueMode::Number
                                    : AdInputNumber::ValueMode::ExactDecimal);
        size->setRange(toPixels ? 16 : 5, toPixels ? std::numeric_limits<int>::max() : 100);
        size->setSuffixText(toPixels ? editorText(" px") : QStringLiteral("%"));
        if (converted.isEmpty())
            size->clear();
        else if (toPixels)
            size->setValue(converted.toDouble());
        else
            size->setExactValue(converted);
        syncing = false;
        commit();
    }
    void theme() {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(owner);
        QPalette palette = owner->palette();
        palette.setColor(QPalette::WindowText, theme.colorTextSecondary);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, theme.colorTextDisabled);
        for (auto* label : {dimensions, total})
            label->setPalette(palette);
        emptyState->refreshTheme();
        dropHint->refreshTheme();
        owner->update();
    }
};

ImageSplitEditorWidget::ImageSplitEditorWidget(ImageSplitDocument* document, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.document = document;
    setObjectName(QStringLiteral("imageSplitEditorWidget"));
    setAcceptDrops(true);
    const auto metric = styles::ThemeManager::instance().themeColorScheme().metricAlias;
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(metric.marginMD);
    d.canvas = new ImageSplitPreviewCanvas(document, this);
    outer->addWidget(d.canvas, 1);
    auto* toolbar = new CanvasActionToolbar(d.canvas);
    toolbar->setObjectName(QStringLiteral("imageSplitCanvasToolbar"));
    auto* overlay = new QVBoxLayout(d.canvas);
    overlay->setContentsMargins(16, 16, 16, 16);
    overlay->addStretch(1);
    overlay->addWidget(toolbar, 0, Qt::AlignHCenter);
    namespace icons = adqt::icons::antd::outlined;
    const std::array<adqt::icons::IconRef, 4> refs{icons::Undo(), icons::Redo(),
                                                   icons::Fullscreen(), icons::Reload()};
    const std::array<const char*, 4> names{"imageSplitUndo", "imageSplitRedo", "imageSplitFit",
                                           "imageSplitReload"};
    for (std::size_t i = 0; i < d.actions.size(); ++i) {
        if (i == 3)
            toolbar->addSeparator();
        d.actions[i] = toolbar->addAction(refs[i], names[i]);
    }
    d.emptyState = new ImageDropSurface(false, this);
    d.emptyState->setObjectName(QStringLiteral("imageSplitEmptyState"));
    d.emptyState->title->setObjectName(QStringLiteral("imageSplitEmptyPrompt"));
    d.emptyState->description->setObjectName(QStringLiteral("imageSplitEmptyDescription"));
    d.emptySelect = new AdButton(d.emptyState);
    d.emptySelect->setObjectName(QStringLiteral("imageSplitEmptySelectImage"));
    d.emptySelect->setIconRef(icons::Plus());
    d.emptySelect->setButtonStyle(AdButton::ButtonStyle::Solid);
    d.emptySelect->setAccentRole(AdButton::AccentRole::Primary);
    d.emptySelect->setFocusPolicy(Qt::StrongFocus);
    auto* emptyLayout = static_cast<QVBoxLayout*>(d.emptyState->layout());
    emptyLayout->insertSpacing(emptyLayout->count() - 1, 8);
    emptyLayout->insertWidget(emptyLayout->count() - 1, d.emptySelect, 0, Qt::AlignHCenter);
    outer->addWidget(d.emptyState, 1);
    auto* scroll = new AdScrollArea(this);
    scroll->setObjectName(QStringLiteral("imageSplitSettingsScroll"));
    scroll->setFixedWidth(340);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);
    d.form = new AdForm;
    d.form->setObjectName(QStringLiteral("imageSplitSettingsForm"));
    d.form->setFormLayout(AdForm::FormLayout::Vertical);
    d.form->setLabelAlign(AdForm::LabelAlign::Left);
    d.form->setColon(false);
    d.form->setRequiredMark(AdForm::RequiredMark::Hidden);
    d.form->setContentsMargins(0, 0, 12, 0);
    scroll->setContentWidget(d.form);
    // QScrollArea::setWidget enables background filling on the content widget.
    d.form->setAutoFillBackground(false);
    outer->addWidget(scroll);
    const auto segmented = [&d](const char* name, const QStringList& values) {
        auto* control = new AdSegmented(d.form);
        control->setObjectName(QString::fromLatin1(name));
        control->setDistribution(AdSegmented::Distribution::Fill);
        control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        for (const auto& value : values)
            control->addOption({}, value);
        return control;
    };
    d.direction =
        segmented("imageSplitDirection", {QStringLiteral("vertical"), QStringLiteral("horizontal"),
                                          QStringLiteral("grid")});
    d.directionField = d.form->addField({}, d.direction, QStringLiteral("direction"));
    d.type = segmented("imageSplitType", {QStringLiteral("equal"), QStringLiteral("precise")});
    d.typeField = d.form->addField({}, d.type, QStringLiteral("type"));
    const auto number = [&d](const char* name) {
        auto* input = new AdInputNumber(d.form);
        input->setObjectName(QString::fromLatin1(name));
        // Retain fractional drafts so invalid integers receive inline feedback rather than
        // rounding.
        input->setDecimals(-1);
        input->setRangeMode(AdInputNumber::RangeMode::Permissive);
        input->setRange(1, std::numeric_limits<int>::max());
        input->setSingleStep(1);
        input->setKeyboardTracking(false);
        input->setWheelStepEnabled(false);
        input->setStepButtonLayout(AdInputNumber::StepButtonLayout::Compact);
        input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        input->setLocale(QLocale::c());
        return input;
    };
    d.overlap = number("imageSplitOverlapWidth");
    d.overlap->setTextPolicy(new OverlapWidthTextPolicy(d.overlap));
    d.overlap->setRange(0, std::numeric_limits<int>::max());
    d.overlapField = d.form->addField({}, d.overlap, QStringLiteral("overlapWidth"));
    d.count = number("imageSplitCount");
    d.countField = d.form->addField({}, d.count, QStringLiteral("count"));
    d.rows = number("imageSplitRows");
    d.rowsField = d.form->addField({}, d.rows, QStringLiteral("rows"));
    d.columns = number("imageSplitColumns");
    d.columnsField = d.form->addField({}, d.columns, QStringLiteral("columns"));
    d.unit = segmented("imageSplitUnit", {QStringLiteral("pixels"), QStringLiteral("percentage")});
    d.unit->setControlSize(AdSegmented::ControlSize::Small);
    d.unit->setDistribution(AdSegmented::Distribution::Content);
    d.size = number("imageSplitSize");
    d.sizeField = d.form->addField({}, d.size, QStringLiteral("size"));
    d.sizeField->setLabelAccessoryWidget(d.unit);
    // Split validation owns errors, including uncommitted drafts. Generic form validation would
    // clear them after a numeric value/hasValue signal without checking the split constraints.
    for (auto* field : d.form->items())
        field->setValidateOnChange(false);
    // Register all fields before arranging the grid; registration rebuilds the form layout.
    auto* formLayout = static_cast<QVBoxLayout*>(d.form->layout());
    const int gridIndex = formLayout->indexOf(d.rowsField);
    formLayout->removeWidget(d.rowsField);
    formLayout->removeWidget(d.columnsField);
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(metric.marginLG);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->addWidget(d.rowsField, 0, 0, Qt::AlignTop);
    grid->addWidget(d.columnsField, 0, 1, Qt::AlignTop);
    formLayout->insertLayout(gridIndex, grid);
    formLayout->setAlignment(Qt::AlignTop);
    d.description = new QWidget(this);
    d.description->setObjectName(QStringLiteral("imageSplitOutputDescription"));
    d.description->hide();
    auto* descriptionLayout = new QHBoxLayout(d.description);
    descriptionLayout->setContentsMargins(0, 0, 0, 0);
    descriptionLayout->setSpacing(metric.marginSM);
    d.dimensions = new QLabel(d.description);
    d.dimensions->setObjectName(QStringLiteral("imageSplitDimensions"));
    d.total = new QLabel(d.description);
    d.total->setObjectName(QStringLiteral("imageSplitOutputCount"));
    descriptionLayout->addWidget(d.dimensions);
    descriptionLayout->addWidget(d.total);
    descriptionLayout->addStretch(1);
    d.dropHint = new ImageDropSurface(true, this);
    d.dropHint->setObjectName(QStringLiteral("imageSplitDropHint"));
    d.dropHint->title->setObjectName(QStringLiteral("imageSplitDropTitle"));
    d.dropHint->description->setObjectName(QStringLiteral("imageSplitDropDescription"));
    d.dropHint->hide();
    const auto wireNumber = [this, context = &d.callbackContext](AdInputNumber* input,
                                                                 const QString& key) {
        connect(input, &AdInputNumber::textChanged, context, [this, key](const QString& value) {
            auto& state = *m_impl;
            if (state.syncing)
                return;
            state.drafts.insert(key.isEmpty() ? state.sizeKey() : key, value);
            state.validate();
        });
        connect(input, &AdInputNumber::valueChanged, context, [this](double) { m_impl->commit(); });
        connect(input, &QAbstractSpinBox::editingFinished, context, [this] { m_impl->commit(); });
    };
    wireNumber(d.count, QStringLiteral("count"));
    wireNumber(d.overlap, QStringLiteral("overlapWidth"));
    wireNumber(d.rows, QStringLiteral("rows"));
    wireNumber(d.columns, QStringLiteral("columns"));
    wireNumber(d.size, {});
    connect(d.direction, &AdSegmented::currentValueChanged, &d.callbackContext,
            [this](const QVariant& value) {
                auto& state = *m_impl;
                if (state.syncing || state.busy())
                    return;
                Q_UNUSED(value);
                state.updateVisibility();
                state.commit();
            });
    connect(d.type, &AdSegmented::currentValueChanged, &d.callbackContext,
            [this](const QVariant& value) {
                auto& state = *m_impl;
                if (state.syncing || state.busy())
                    return;
                Q_UNUSED(value);
                state.updateVisibility();
                state.commit();
            });
    connect(d.unit, &AdSegmented::currentValueChanged, &d.callbackContext,
            [this](const QVariant& value) { m_impl->changeUnit(value); });
    connect(d.actions[0], &AdButton::clicked, document, &ImageSplitDocument::undo);
    connect(d.actions[1], &AdButton::clicked, document, &ImageSplitDocument::redo);
    connect(d.actions[2], &AdButton::clicked, d.canvas, &ImageSplitPreviewCanvas::fitToCanvas);
    connect(d.actions[3], &AdButton::clicked, this, &ImageSplitEditorWidget::reloadRequested);
    connect(d.emptySelect, &AdButton::clicked, this, &ImageSplitEditorWidget::reloadRequested);
    connect(document, &ImageSplitDocument::changed, &d.callbackContext,
            [this] { m_impl->refresh(); });
    connect(document, &ImageSplitDocument::historyChanged, &d.callbackContext,
            [this] { m_impl->updateActions(); });
    auto* shortcuts = new WindowShortcutManager(this);
    shortcuts->addScopeWindow(this);
    for (const bool undo : {true, false}) {
        WindowShortcutManager::Binding binding;
        binding.id = undo ? QStringLiteral("imageSplitUndo") : QStringLiteral("imageSplitRedo");
        for (const auto& sequence :
             QKeySequence::keyBindings(undo ? QKeySequence::Undo : QKeySequence::Redo)) {
            if (sequence.count() == 1)
                binding.keyCombinations.append(sequence[0]);
        }
        binding.priority = WindowShortcutManager::StandardPriority::WindowCommand;
        binding.canActivate = [this, undo](const auto& context) {
            return !m_impl->busy() &&
                   !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget) &&
                   (undo ? m_impl->document->canUndo() : m_impl->document->canRedo());
        };
        binding.activate = [this, undo](const auto&) {
            if (undo)
                m_impl->document->undo();
            else
                m_impl->document->redo();
            return true;
        };
        static_cast<void>(shortcuts->addBinding(&d.callbackContext, std::move(binding)));
    }
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
            &d.callbackContext, [this] { m_impl->theme(); });
    connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged,
            &d.callbackContext,
            [this, outer, descriptionLayout, grid](const styles::ThemeColorScheme& scheme) {
                outer->setSpacing(scheme.metricAlias.marginMD);
                descriptionLayout->setSpacing(scheme.metricAlias.marginSM);
                grid->setHorizontalSpacing(scheme.metricAlias.marginLG);
                m_impl->theme();
            });
    d.sync();
    retranslateUi();
    d.refresh();
    d.theme();
}
ImageSplitEditorWidget::~ImageSplitEditorWidget() = default;
ImageSplitPreviewCanvas* ImageSplitEditorWidget::canvas() const {
    return m_impl->canvas;
}
bool ImageSplitEditorWidget::configurationValid() const {
    return m_impl->valid;
}
bool ImageSplitEditorWidget::commitPendingSettings() {
    if (m_impl->busy())
        return false;
    m_impl->commit();
    return m_impl->valid && m_impl->draftSettings().first == m_impl->document->settings();
}
QWidget* ImageSplitEditorWidget::outputDescriptionWidget() const {
    return m_impl->description;
}
void ImageSplitEditorWidget::setLoading(bool loading) {
    m_impl->loading = loading;
    m_impl->updateActions();
}
void ImageSplitEditorWidget::setExportBusy(bool busy) {
    m_impl->exporting = busy;
    m_impl->updateActions();
}
void ImageSplitEditorWidget::retranslateUi() {
    auto& d = *m_impl;
    d.validatedSettings.reset();
    const std::array<const char*, 3> directions{"Vertical", "Horizontal", "Grid"};
    for (std::size_t i = 0; i < directions.size(); ++i)
        d.direction->setOptionLabel(static_cast<int>(i), editorText(directions[i]));
    d.directionField->setLabel(editorText("Split direction"));
    d.direction->setAccessibleName(d.directionField->label());
    d.type->setOptionLabel(0, editorText("Equal split"));
    d.type->setOptionLabel(1, editorText("Precise split"));
    d.typeField->setLabel(editorText("Split type"));
    d.type->setAccessibleName(d.typeField->label());
    d.overlapField->setLabel(editorText("Overlap width"));
    d.overlap->setAccessibleName(d.overlapField->label());
    d.overlap->setSuffixText(editorText(" px"));
    const auto overlapDescription =
        editorText("Each image extends this many pixels past each split line");
    d.overlap->setToolTip(overlapDescription);
    d.overlap->setAccessibleDescription(overlapDescription);
    for (const auto& item : {std::pair{d.countField, d.count}, std::pair{d.rowsField, d.rows},
                             std::pair{d.columnsField, d.columns}}) {
        const auto source = item.first == d.countField  ? "Number of images"
                            : item.first == d.rowsField ? "Rows"
                                                        : "Columns";
        item.first->setLabel(editorText(source));
        item.second->setAccessibleName(item.first->label());
    }
    d.unit->setOptionLabel(0, editorText("Pixels"));
    d.unit->setOptionLabel(1, editorText("Percentage"));
    d.unit->setAccessibleName(editorText("Split size unit"));
    d.size->setSuffixText(d.sizeKey() == QStringLiteral("pixels") ? editorText(" px")
                                                                  : QStringLiteral("%"));
    d.updateVisibility();
    const std::array<const char*, 4> actions{"Undo", "Redo", "Fit to canvas", "Reload"};
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto label = editorText(actions[i]);
        d.actions[i]->setToolTip(label);
        d.actions[i]->setAccessibleName(label);
    }
    d.emptyState->title->setText(editorText("Select an image to split"));
    d.emptyState->description->setText(
        editorText("Drag an image file here, or choose select image"));
    d.emptyState->setAccessibleName(d.emptyState->title->text());
    d.emptyState->setAccessibleDescription(d.emptyState->description->text());
    d.emptySelect->setText(editorText("Select image"));
    d.emptySelect->setAccessibleName(d.emptySelect->text());
    d.dropHint->description->setText(editorText("Drop one image file at a time"));
    d.dropHint->setAccessibleDescription(d.dropHint->description->text());
    d.canvas->retranslateUi();
    d.refresh();
}
void ImageSplitEditorWidget::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}
void ImageSplitEditorWidget::dragEnterEvent(QDragEnterEvent* event) {
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
void ImageSplitEditorWidget::dragMoveEvent(QDragMoveEvent* event) {
    if (!m_impl->acceptsImageDrop(event->mimeData(), event->possibleActions())) {
        m_impl->dropHint->hide();
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}
void ImageSplitEditorWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    m_impl->dropHint->hide();
    event->accept();
}
void ImageSplitEditorWidget::dropEvent(QDropEvent* event) {
    m_impl->dropHint->hide();
    if (!m_impl->acceptsImageDrop(event->mimeData(), event->possibleActions())) {
        event->ignore();
        return;
    }
    const auto paths = droppedPaths(event->mimeData());
    event->setDropAction(Qt::CopyAction);
    event->accept();
    emit imageFilesDropped(paths);
}
void ImageSplitEditorWidget::hideEvent(QHideEvent* event) {
    m_impl->dropHint->hide();
    QWidget::hideEvent(event);
}
void ImageSplitEditorWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layout()->activate();
    m_impl->positionDropHint();
}

} // namespace snow_shot::presentation::components
