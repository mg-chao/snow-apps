#include "snow_shot/presentation/screenshotcolorpickerwidget.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotguidelinerendering.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include <QFont>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QPainterPath>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kPreviewScale = 12;
constexpr int kPreviewPickerSize = 11;
constexpr int kPreviewCanvasSize = kPreviewScale * kPreviewPickerSize;
constexpr int kPanelPadding = 4;
constexpr int kPanelRadius = 6;
constexpr int kContentGap = 4;
constexpr int kTextVerticalOffset = 1;
constexpr int kTextHeight = 20;
constexpr int kColorTextHeight = 24;
constexpr int kCenterPixelBorderWidth = 1;
constexpr int kShadowMargin = 10;
constexpr int kShadowBlurRadius = 8;
constexpr int kCursorGap = 16;

const QColor kFallbackPanelBackground(255, 255, 255);
const QColor kFallbackPanelTextColor(38, 38, 38);
const QColor kShadowColor(0, 0, 0, 28);

QColor panelBackgroundForTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    if (scheme.alias.surfaceBg.isValid()) {
        return scheme.alias.surfaceBg;
    }
    if (scheme.map.colorBgContainer.isValid()) {
        return scheme.map.colorBgContainer;
    }
    return kFallbackPanelBackground;
}

QColor panelTextColorForTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    if (scheme.alias.textPrimary.isValid()) {
        return scheme.alias.textPrimary;
    }
    if (scheme.map.colorText.isValid()) {
        return scheme.map.colorText;
    }
    return kFallbackPanelTextColor;
}

struct RgbaPixel {
    int red = 0;
    int green = 0;
    int blue = 0;
    int alpha = 255;
};

int normalizedHue(qreal hue) {
    if (std::isnan(hue) || hue < 0.0) {
        return 0;
    }
    return qRound(hue);
}

QColor readableTextColor(const QColor& color) {
    if (!color.isValid()) {
        return QColor(255, 255, 255, 217);
    }

    return color.red() > 128 || color.green() > 128 || color.blue() > 128
               ? QColor(0, 0, 0, 224)
               : QColor(255, 255, 255, 217);
}

QString fitText(const QFont& font, const QString& text, int width) {
    QFontMetrics metrics(font);
    if (metrics.horizontalAdvance(text) <= width) {
        return text;
    }
    return metrics.elidedText(text, Qt::ElideRight, width);
}

RgbaPixel rgbaPixelAt(const QImage& image, int x, int y) {
    if (image.format() == QImage::Format_RGBA8888) {
        const uchar* pixel = image.constScanLine(y) + x * 4;
        return RgbaPixel{
            static_cast<int>(pixel[0]),
            static_cast<int>(pixel[1]),
            static_cast<int>(pixel[2]),
            static_cast<int>(pixel[3]),
        };
    }

    const QColor color = image.pixelColor(x, y).toRgb();
    return RgbaPixel{
        color.red(),
        color.green(),
        color.blue(),
        color.alpha(),
    };
}

int premultipliedComponent(int value, int alpha) {
    return (value * alpha + 127) / 255;
}

QRgb premultipliedArgb(const RgbaPixel& pixel) {
    return qRgba(premultipliedComponent(pixel.red, pixel.alpha),
                 premultipliedComponent(pixel.green, pixel.alpha),
                 premultipliedComponent(pixel.blue, pixel.alpha), pixel.alpha);
}

QPainterPath bottomRoundedRectPath(const QRectF& rect, qreal radius) {
    const qreal cornerRadius = std::min<qreal>(radius, std::min(rect.width(), rect.height()) / 2.0);

    QPainterPath path;
    path.moveTo(rect.topLeft());
    path.lineTo(rect.topRight());
    path.lineTo(rect.right(), rect.bottom() - cornerRadius);
    path.quadTo(rect.bottomRight(), QPointF(rect.right() - cornerRadius, rect.bottom()));
    path.lineTo(rect.left() + cornerRadius, rect.bottom());
    path.quadTo(rect.bottomLeft(), QPointF(rect.left(), rect.bottom() - cornerRadius));
    path.lineTo(rect.topLeft());
    path.closeSubpath();
    return path;
}

QPainterPath topRoundedRectPath(const QRectF& rect, qreal radius) {
    const qreal cornerRadius = std::min<qreal>(radius, std::min(rect.width(), rect.height()) / 2.0);

    QPainterPath path;
    path.moveTo(rect.left(), rect.top() + cornerRadius);
    path.quadTo(rect.topLeft(), QPointF(rect.left() + cornerRadius, rect.top()));
    path.lineTo(rect.right() - cornerRadius, rect.top());
    path.quadTo(rect.topRight(), QPointF(rect.right(), rect.top() + cornerRadius));
    path.lineTo(rect.bottomRight());
    path.lineTo(rect.bottomLeft());
    path.lineTo(rect.left(), rect.top() + cornerRadius);
    path.closeSubpath();
    return path;
}
} // namespace

ScreenshotColorPickerWidget::ScreenshotColorPickerWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFixedSize(sizeHint());

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    const auto applyTheme =
        [this](const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
            const QColor panelBackground = panelBackgroundForTheme(scheme);
            const QColor panelTextColor = panelTextColorForTheme(scheme);
            if (m_panelBackground == panelBackground && m_panelTextColor == panelTextColor) {
                return;
            }
            m_panelBackground = panelBackground;
            m_panelTextColor = panelTextColor;
            update();
        };
    applyTheme(themeManager.themeColorScheme());
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            applyTheme);

    m_opacityEffect = new QGraphicsOpacityEffect(this);
    m_opacityEffect->setOpacity(0.0);
    setGraphicsEffect(m_opacityEffect);
    hide();
}

void ScreenshotColorPickerWidget::resetForNewCapture() {
    m_captureImage = QImage();
    m_physicalRect = QRect();
    m_previewImage = QImage();
    m_currentPhysicalPoint = QPoint();
    m_currentColor = QColor();
    m_hasCurrentColor = false;
    m_colorFormat = ColorFormat::Hex;
    hidePicker();
}

void ScreenshotColorPickerWidget::setCaptureImage(const QImage& image, const QRect& physicalRect) {
    if (m_captureImage.cacheKey() == image.cacheKey() && m_physicalRect == physicalRect) {
        return;
    }

    m_captureImage = image;
    m_physicalRect = physicalRect;
    if (m_previewImage.size() != QSize(kPreviewPickerSize, kPreviewPickerSize) ||
        m_previewImage.format() != QImage::Format_ARGB32_Premultiplied) {
        m_previewImage =
            QImage(kPreviewPickerSize, kPreviewPickerSize, QImage::Format_ARGB32_Premultiplied);
    }
    m_hasCurrentColor = false;
}

void ScreenshotColorPickerWidget::updatePicker(const QPoint& physicalPoint,
                                               const QPointF& overlayLocalPosition, qreal opacity) {
    if (m_captureImage.isNull() || m_physicalRect.isNull()) {
        hidePicker();
        return;
    }

    opacity = std::clamp<qreal>(opacity, 0.0, 1.0);
    const bool previewChanged = updatePreview(physicalPoint);
    static_cast<void>(updatePosition(overlayLocalPosition));
    bool opacityChanged = false;
    if (m_opacityEffect != nullptr) {
        opacityChanged = std::abs(m_opacityEffect->opacity() - opacity) > 0.0001;
        if (opacityChanged) {
            m_opacityEffect->setOpacity(opacity);
        }
    }

    if (opacity <= 0.0) {
        hidePicker();
        return;
    }

    const bool wasVisible = isVisible();
    if (!wasVisible) {
        show();
        raise();
    }
    if (previewChanged || opacityChanged) {
        update();
    }
}

void ScreenshotColorPickerWidget::hidePicker() {
    if (m_opacityEffect != nullptr) {
        m_opacityEffect->setOpacity(0.0);
    }
    hide();
}

void ScreenshotColorPickerWidget::setCenterGuideLineColor(const QColor& color) {
    const QColor next = color.isValid() ? color : QColor(0, 0, 0, 0);
    if (m_centerGuideLineColor == next) {
        return;
    }
    m_centerGuideLineColor = next;
    update();
}

void ScreenshotColorPickerWidget::cycleColorFormat() {
    switch (m_colorFormat) {
    case ColorFormat::Hex:
        m_colorFormat = ColorFormat::Rgb;
        break;
    case ColorFormat::Rgb:
        m_colorFormat = ColorFormat::Hsl;
        break;
    case ColorFormat::Hsl:
    default:
        m_colorFormat = ColorFormat::Hex;
        break;
    }
    update();
}

QString ScreenshotColorPickerWidget::currentColorText() const {
    return m_hasCurrentColor ? formatColor(m_currentColor) : QString();
}

bool ScreenshotColorPickerWidget::hasCurrentColor() const {
    return m_hasCurrentColor;
}

QSize ScreenshotColorPickerWidget::sizeHint() const {
    const int panelWidth = kPanelPadding * 2 + kPreviewCanvasSize;
    const int panelHeight = kPanelPadding + kPreviewCanvasSize + kContentGap + kTextVerticalOffset +
                            kTextHeight + kContentGap + kTextVerticalOffset + kColorTextHeight +
                            kPanelPadding;
    return QSize(panelWidth + kShadowMargin * 2, panelHeight + kShadowMargin * 2);
}

void ScreenshotColorPickerWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF panel = panelRect();
    for (int step = kShadowBlurRadius; step >= 1; --step) {
        const qreal progress = static_cast<qreal>(kShadowBlurRadius - step + 1) /
                               static_cast<qreal>(kShadowBlurRadius);
        QColor shadow = kShadowColor;
        shadow.setAlphaF(static_cast<float>(std::clamp<qreal>(
            static_cast<qreal>(kShadowColor.alpha()) / 255.0 * std::pow(progress, 1.45), 0.0,
            1.0)));
        painter.setPen(QPen(shadow, 1.0));
        painter.setBrush(Qt::NoBrush);
        const QRectF shadowRect = panel.adjusted(-step, -step, step, step);
        painter.drawRoundedRect(shadowRect, kPanelRadius + step, kPanelRadius + step);
    }

    QPainterPath panelPath;
    panelPath.addRoundedRect(panel, kPanelRadius, kPanelRadius);
    painter.fillPath(panelPath, m_panelBackground);

    const QRectF preview = previewRect();
    const QPainterPath previewPath = topRoundedRectPath(preview, kPanelRadius);
    painter.save();
    painter.setClipPath(previewPath);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    if (!m_previewImage.isNull()) {
        painter.drawImage(preview, m_previewImage);
    }
    painter.restore();

    constexpr int kPreviewCenterOffset = (kPreviewPickerSize / 2) * kPreviewScale;
    const qreal previewCenterOffset = static_cast<qreal>(kPreviewCenterOffset);
    const QRectF centerRect(preview.left() + previewCenterOffset,
                            preview.top() + previewCenterOffset, kPreviewScale, kPreviewScale);
    paintScreenshotColorPickerCenterGuideLines(painter, preview, centerRect,
                                               m_centerGuideLineColor);
    const QColor markerColor = readableTextColor(m_currentColor);
    painter.setPen(QPen(markerColor, kCenterPixelBorderWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(centerRect.adjusted(0.5, 0.5, -0.5, -0.5), 2.0, 2.0);

    QFont textFont = font();
    textFont.setPixelSize(13);
    painter.setFont(textFont);
    painter.setPen(m_panelTextColor);
    const QString positionText = QStringLiteral("X: %1 Y: %2")
                                     .arg(m_currentPhysicalPoint.x())
                                     .arg(m_currentPhysicalPoint.y());
    painter.drawText(positionTextRect(), Qt::AlignCenter,
                     fitText(textFont, positionText, positionTextRect().toAlignedRect().width()));

    const QRectF colorRect = colorTextRect();
    QColor swatch = m_currentColor.isValid() ? m_currentColor : QColor(0, 0, 0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(swatch);
    painter.fillPath(bottomRoundedRectPath(colorRect, kPanelRadius), swatch);

    painter.setPen(readableTextColor(swatch));
    const QString colorText = currentColorText();
    painter.drawText(colorRect.adjusted(4.0, 0.0, -4.0, 0.0), Qt::AlignCenter,
                     fitText(textFont, colorText, colorRect.toAlignedRect().width() - 8));
}

bool ScreenshotColorPickerWidget::updatePreview(const QPoint& physicalPoint) {
    if (m_captureImage.isNull() || m_physicalRect.isNull()) {
        return false;
    }

    const int maxImageX = std::max(0, m_captureImage.width() - 1);
    const int maxImageY = std::max(0, m_captureImage.height() - 1);
    const int imageX = std::clamp(physicalPoint.x() - m_physicalRect.left(), 0, maxImageX);
    const int imageY = std::clamp(physicalPoint.y() - m_physicalRect.top(), 0, maxImageY);
    const QPoint nextPhysicalPoint(m_physicalRect.left() + imageX, m_physicalRect.top() + imageY);
    if (m_hasCurrentColor && m_currentPhysicalPoint == nextPhysicalPoint &&
        !m_previewImage.isNull()) {
        return false;
    }
    m_currentPhysicalPoint = nextPhysicalPoint;

    if (m_previewImage.isNull()) {
        m_previewImage =
            QImage(kPreviewPickerSize, kPreviewPickerSize, QImage::Format_ARGB32_Premultiplied);
    }

    const int halfPicker = kPreviewPickerSize / 2;
    for (int y = 0; y < kPreviewPickerSize; ++y) {
        auto* previewLine = reinterpret_cast<QRgb*>(m_previewImage.scanLine(y));
        for (int x = 0; x < kPreviewPickerSize; ++x) {
            const int sourceX = std::clamp(imageX + x - halfPicker, 0, maxImageX);
            const int sourceY = std::clamp(imageY + y - halfPicker, 0, maxImageY);
            previewLine[x] = premultipliedArgb(rgbaPixelAt(m_captureImage, sourceX, sourceY));
        }
    }

    const RgbaPixel centerPixel = rgbaPixelAt(m_captureImage, imageX, imageY);
    m_currentColor = QColor(centerPixel.red, centerPixel.green, centerPixel.blue, 255);
    m_hasCurrentColor = true;
    return true;
}

bool ScreenshotColorPickerWidget::updatePosition(const QPointF& overlayLocalPosition) {
    QWidget* parent = parentWidget();
    const QSize ownSize = size();
    const QSize panelSize(std::max(1, ownSize.width() - kShadowMargin * 2),
                          std::max(1, ownSize.height() - kShadowMargin * 2));
    const QRect bounds = parent != nullptr ? parent->rect() : QRect();
    const QPoint panelPosition = ScreenshotGeometryMapper::cursorPanelPosition(
        overlayLocalPosition.toPoint(), panelSize, bounds, kCursorGap);
    const QPoint targetPosition =
        panelPosition - QPoint(kShadowMargin, kShadowMargin);
    if (pos() == targetPosition) {
        return false;
    }

    move(targetPosition);
    return true;
}

QString ScreenshotColorPickerWidget::formatColor(const QColor& color) const {
    const QColor rgbColor = color.toRgb();
    switch (m_colorFormat) {
    case ColorFormat::Rgb:
        return QStringLiteral("rgb(%1, %2, %3)")
            .arg(rgbColor.red())
            .arg(rgbColor.green())
            .arg(rgbColor.blue());
    case ColorFormat::Hsl: {
        const QColor hsl = rgbColor.toHsl();
        const qreal saturation = static_cast<qreal>(hsl.hslSaturationF()) * 100.0;
        const qreal lightness = static_cast<qreal>(hsl.lightnessF()) * 100.0;
        return QStringLiteral("hsl(%1, %2%, %3%)")
            .arg(normalizedHue(static_cast<qreal>(hsl.hslHueF()) * 360.0))
            .arg(QString::number(saturation, 'f', 1))
            .arg(QString::number(lightness, 'f', 1));
    }
    case ColorFormat::Hex:
    default:
        return rgbColor.name(QColor::HexRgb).toUpper();
    }
}

QRectF ScreenshotColorPickerWidget::panelRect() const {
    return QRectF(kShadowMargin + 0.5, kShadowMargin + 0.5, width() - kShadowMargin * 2,
                  height() - kShadowMargin * 2);
}

QRectF ScreenshotColorPickerWidget::previewRect() const {
    const QRectF panel = panelRect();
    return QRectF(panel.left() + kPanelPadding, panel.top() + kPanelPadding, kPreviewCanvasSize,
                  kPreviewCanvasSize);
}

QRectF ScreenshotColorPickerWidget::positionTextRect() const {
    const QRectF preview = previewRect();
    return QRectF(preview.left(), preview.bottom() + kContentGap + kTextVerticalOffset,
                  kPreviewCanvasSize, kTextHeight);
}

QRectF ScreenshotColorPickerWidget::colorTextRect() const {
    const QRectF position = positionTextRect();
    return QRectF(position.left(), position.bottom() + kContentGap + kTextVerticalOffset,
                  kPreviewCanvasSize, kColorTextHeight);
}
