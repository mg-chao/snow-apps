#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"

#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr int kPanelWidth = 208;
constexpr int kPanelHeight = 64;
constexpr int kShadowMargin = 10;
constexpr int kShadowBlurRadius = 8;
constexpr int kPanelRadius = 6;
constexpr int kPanelPadding = 11;
constexpr int kPreviewPickerSize = 7;
constexpr int kPreviewScale = 6;
constexpr int kPreviewCanvasSize = kPreviewPickerSize * kPreviewScale;
constexpr int kPreviewRadius = 4;
constexpr int kCenterPixelBorderWidth = 1;
constexpr int kContentGap = 10;
constexpr int kCursorGap = 14;
constexpr int kCursorCanvasSize = 32;
constexpr int kCursorIconSize = 24;

const QColor kFallbackPanelBackground(255, 255, 255);
const QColor kFallbackPanelBorder(217, 217, 217);
const QColor kFallbackTextColor(38, 38, 38);
const QColor kFallbackSecondaryTextColor(140, 140, 140);
const QColor kShadowColor(0, 0, 0, 28);

QColor validOr(const QColor& preferred, const QColor& fallback) {
    return preferred.isValid() ? preferred : fallback;
}

QRectF panelRectForWindow(const QWidget& window) {
    return QRectF(kShadowMargin + 0.5, kShadowMargin + 0.5, window.width() - kShadowMargin * 2,
                  window.height() - kShadowMargin * 2);
}

QString rgbText(const QColor& color) {
    const QColor rgb = color.toRgb();
    return QStringLiteral("RGB %1, %2, %3").arg(rgb.red()).arg(rgb.green()).arg(rgb.blue());
}

QColor readableMarkerColor(const QColor& color) {
    if (!color.isValid()) {
        return QColor(255, 255, 255, 217);
    }
    return color.red() > 128 || color.green() > 128 || color.blue() > 128
               ? QColor(0, 0, 0, 224)
               : QColor(255, 255, 255, 217);
}
} // namespace

ScreenshotCanvasColorSamplerWindow::ScreenshotCanvasColorSamplerWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                          Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput) {
    setObjectName(QStringLiteral("screenshotCanvasColorSamplerWindow"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(sizeHint());

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    const auto applyTheme = [this](
                                const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
        m_panelBackground = validOr(scheme.map.colorBgElevated,
                                    validOr(scheme.alias.surfaceBg, kFallbackPanelBackground));
        m_previewBorder = validOr(scheme.map.colorBorderSecondary,
                                  validOr(scheme.alias.subtleBorder, kFallbackPanelBorder));
        m_textColor =
            validOr(scheme.map.colorText, validOr(scheme.alias.textPrimary, kFallbackTextColor));
        m_secondaryTextColor =
            validOr(scheme.map.colorTextSecondary,
                    validOr(scheme.alias.textMuted, kFallbackSecondaryTextColor));
        update();
    };
    applyTheme(themeManager.themeColorScheme());
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            applyTheme);
    hide();
}

void ScreenshotCanvasColorSamplerWindow::beginSampling() {
    m_sampling = true;
    m_previewImage = QImage();
    m_currentColor = QColor();
    hide();
}

void ScreenshotCanvasColorSamplerWindow::updateSample(const QImage& previewImage,
                                                      const QPoint& globalCursorPosition) {
    if (!m_sampling || previewImage.isNull()) {
        return;
    }

    m_previewImage = previewImage;
    m_currentColor =
        m_previewImage.pixelColor(m_previewImage.width() / 2, m_previewImage.height() / 2).toRgb();
    if (!m_currentColor.isValid()) {
        return;
    }
    updatePosition(globalCursorPosition);
    if (!isVisible()) {
        show();
        raise();
    }
    update();
}

void ScreenshotCanvasColorSamplerWindow::endSampling() {
    m_sampling = false;
    m_previewImage = QImage();
    m_currentColor = QColor();
    hide();
}

QCursor ScreenshotCanvasColorSamplerWindow::samplingCursor() {
    QScreen* cursorScreen = QGuiApplication::screenAt(QCursor::pos());
    if (cursorScreen == nullptr) {
        cursorScreen = QGuiApplication::primaryScreen();
    }
    const qreal devicePixelRatio =
        cursorScreen != nullptr ? std::max<qreal>(1.0, cursorScreen->devicePixelRatio()) : 1.0;
    QPixmap cursorPixmap(QSize(kCursorCanvasSize, kCursorCanvasSize) * devicePixelRatio);
    cursorPixmap.setDevicePixelRatio(devicePixelRatio);
    cursorPixmap.fill(Qt::transparent);

    namespace custom_icons = snow_shot::presentation::icons::custom;
    const auto icon = custom_icons::outlined::ColorPicker();
    const QPixmap outline = snow_shot::presentation::icons::renderTintedIconPixmap(
        icon, QSize(kCursorIconSize, kCursorIconSize), devicePixelRatio, QColor(255, 255, 255));
    const QPixmap foreground = snow_shot::presentation::icons::renderTintedIconPixmap(
        icon, QSize(kCursorIconSize, kCursorIconSize), devicePixelRatio, QColor(0, 0, 0));
    if (outline.isNull() || foreground.isNull()) {
        return QCursor(Qt::CrossCursor);
    }

    QPainter painter(&cursorPixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    constexpr QPointF iconPosition(4.0, 2.0);
    constexpr std::array<QPointF, 8> outlineOffsets = {
        QPointF(-1.0, -1.0), QPointF(0.0, -1.0), QPointF(1.0, -1.0), QPointF(-1.0, 0.0),
        QPointF(1.0, 0.0),   QPointF(-1.0, 1.0), QPointF(0.0, 1.0),  QPointF(1.0, 1.0),
    };
    for (const QPointF& offset : outlineOffsets) {
        painter.drawPixmap(iconPosition + offset, outline);
    }
    painter.drawPixmap(iconPosition, foreground);
    painter.end();

    return QCursor(cursorPixmap, 5, 25);
}

QSize ScreenshotCanvasColorSamplerWindow::sizeHint() const {
    return QSize(kPanelWidth + kShadowMargin * 2, kPanelHeight + kShadowMargin * 2);
}

void ScreenshotCanvasColorSamplerWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (!m_currentColor.isValid()) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF panel = panelRectForWindow(*this);
    for (int step = kShadowBlurRadius; step >= 1; --step) {
        const qreal progress = static_cast<qreal>(kShadowBlurRadius - step + 1) /
                               static_cast<qreal>(kShadowBlurRadius);
        QColor shadow = kShadowColor;
        shadow.setAlphaF(static_cast<float>(std::clamp<qreal>(
            static_cast<qreal>(kShadowColor.alpha()) / 255.0 * std::pow(progress, 1.45), 0.0,
            1.0)));
        painter.setPen(QPen(shadow, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(panel.adjusted(-step, -step, step, step), kPanelRadius + step,
                                kPanelRadius + step);
    }

    QPainterPath panelPath;
    panelPath.addRoundedRect(panel, kPanelRadius, kPanelRadius);
    painter.fillPath(panelPath, m_panelBackground);

    const QRectF preview(panel.left() + kPanelPadding,
                         panel.top() + (panel.height() - kPreviewCanvasSize) / 2.0,
                         kPreviewCanvasSize, kPreviewCanvasSize);
    QPainterPath previewPath;
    previewPath.addRoundedRect(preview, kPreviewRadius, kPreviewRadius);
    painter.save();
    painter.setClipPath(previewPath);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(preview, m_previewImage);
    painter.restore();
    painter.setPen(QPen(m_previewBorder, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(preview.adjusted(0.5, 0.5, -0.5, -0.5), kPreviewRadius, kPreviewRadius);

    constexpr int centerOffset = (kPreviewPickerSize / 2) * kPreviewScale;
    const QRectF centerPixel(preview.left() + centerOffset, preview.top() + centerOffset,
                             kPreviewScale, kPreviewScale);
    painter.setPen(QPen(readableMarkerColor(m_currentColor), kCenterPixelBorderWidth));
    painter.drawRoundedRect(centerPixel.adjusted(0.5, 0.5, -0.5, -0.5), 1.5, 1.5);

    const qreal textLeft = preview.right() + kContentGap;
    const qreal textWidth = panel.right() - kPanelPadding - textLeft;
    QFont primaryFont = font();
    primaryFont.setPixelSize(14);
    primaryFont.setWeight(QFont::DemiBold);
    painter.setFont(primaryFont);
    painter.setPen(m_textColor);
    painter.drawText(QRectF(textLeft, panel.top() + 10.0, textWidth, 22.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     m_currentColor.name(QColor::HexRgb).toUpper());

    QFont secondaryFont = font();
    secondaryFont.setPixelSize(12);
    painter.setFont(secondaryFont);
    painter.setPen(m_secondaryTextColor);
    painter.drawText(QRectF(textLeft, panel.top() + 32.0, textWidth, 20.0),
                     Qt::AlignLeft | Qt::AlignVCenter, rgbText(m_currentColor));
}

void ScreenshotCanvasColorSamplerWindow::updatePosition(const QPoint& globalCursorPosition) {
    QScreen* screen = QGuiApplication::screenAt(globalCursorPosition);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect bounds = screen != nullptr ? screen->geometry() : QRect();
    const QPoint target = ScreenshotGeometryMapper::cursorPanelPosition(globalCursorPosition,
                                                                        size(), bounds, kCursorGap);
    if (pos() != target) {
        move(target);
    }
}
