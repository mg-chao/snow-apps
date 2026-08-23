#include "screenshottoolpalettebuttons.h"

#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include "widgets/radio.h"
#include "widgets/radio_button_group.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/slider.h"

#include <QAbstractButton>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QSize>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QVariant>

#include <algorithm>
#include <cmath>

ScreenshotToolPaletteTranslationText::ScreenshotToolPaletteTranslationText(const char* sourceText)
    : source(QString::fromUtf8(sourceText != nullptr ? sourceText : "")) {}

ScreenshotToolPaletteTranslationText::ScreenshotToolPaletteTranslationText(
    const QString& sourceText)
    : source(sourceText) {}

QString ScreenshotToolPaletteTranslationText::translated() const {
    if (source.isEmpty()) {
        return QString();
    }

    const QByteArray sourceUtf8 = source.toUtf8();
    QString translated =
        QCoreApplication::translate("ScreenshotToolPalette", sourceUtf8.constData());
    for (const QString& argument : arguments) {
        translated = translated.arg(argument);
    }
    return translated;
}

ScreenshotToolPaletteTranslationText
ScreenshotToolPaletteTranslationText::arg(const QString& value) const {
    ScreenshotToolPaletteTranslationText result(*this);
    result.arguments.push_back(value);
    return result;
}

ScreenshotToolPaletteTranslationText
ScreenshotToolPaletteTranslationText::arg(double value, int fieldWidth, char format,
                                          int precision) const {
    Q_UNUSED(fieldWidth);
    return arg(QString::number(value, format, precision));
}

namespace {
constexpr const char* kTooltipSourceProperty = "snowShotTranslationTooltipSource";
constexpr const char* kTooltipArgumentsProperty = "snowShotTranslationTooltipArguments";
constexpr const char* kAccessibleNameSourceProperty = "snowShotTranslationAccessibleNameSource";
constexpr const char* kAccessibleNameArgumentsProperty =
    "snowShotTranslationAccessibleNameArguments";
constexpr const char* kPlaceholderSourceProperty = "snowShotTranslationPlaceholderSource";
constexpr const char* kPlaceholderArgumentsProperty = "snowShotTranslationPlaceholderArguments";
constexpr int kItemTranslationSourceRole = Qt::UserRole + 1001;
constexpr int kItemTranslationContextRole = Qt::UserRole + 1002;
constexpr int kItemTranslationArgumentsRole = Qt::UserRole + 1003;
constexpr const char* kTranslationContext = "ScreenshotToolPalette";

QString translateSource(const QVariant& source, const QVariant& context,
                        const QVariant& arguments) {
    if (!source.isValid() || source.toString().isEmpty()) {
        return QString();
    }
    const QByteArray sourceUtf8 = source.toString().toUtf8();
    const QByteArray contextUtf8 = context.toString().isEmpty() ? QByteArray(kTranslationContext)
                                                                : context.toString().toUtf8();
    QString translated =
        QCoreApplication::translate(contextUtf8.constData(), sourceUtf8.constData());
    const QStringList formatArguments = arguments.toStringList();
    for (const QString& argument : formatArguments) {
        translated = translated.arg(argument);
    }
    return translated;
}

void setTranslationSource(QWidget* widget, const char* sourceProperty,
                          const char* argumentsProperty,
                          const ScreenshotToolPaletteTranslationText& text) {
    if (widget == nullptr || text.source.isEmpty()) {
        return;
    }
    widget->setProperty(sourceProperty, text.source);
    widget->setProperty(argumentsProperty, text.arguments);
}

constexpr int STYLE_ICON_SIZE = 18;
constexpr int STYLE_PREVIEW_ICON_SIZE = 22;
constexpr int CORNER_RADIUS_EDITOR_WIDTH = 56;
constexpr int WATERMARK_NUMERIC_EDITOR_WIDTH = 64;
constexpr int CORNER_RADIUS_ICON_TEXT_GAP = 6;
constexpr int STROKE_WIDTH_PREVIEW_HORIZONTAL_INSET = 6;
constexpr qreal STROKE_WIDTH_PREVIEW_VERTICAL_RESERVED = 16.0;
constexpr double DEFAULT_RECTANGLE_STROKE_WIDTH = 2.0;
constexpr double MIN_RECTANGLE_STROKE_WIDTH = 1.0;
constexpr double MAX_RECTANGLE_STROKE_WIDTH = 72.0;
constexpr const char* STYLE_RADIO_BASE_SIZE_PROPERTY = "snowShotStyleRadioBaseSize";
constexpr const char* STYLE_RADIO_BASE_ICON_SIZE_PROPERTY = "snowShotStyleRadioBaseIconSize";

adqt::icons::IconStatePalette styleRadioIconPalette(const adqt::widgets::AdRadio* radio,
                                                    const adqt::icons::IconRef& iconRef) {
    adqt::icons::IconStatePalette palette;
    if (radio == nullptr || !adqt::icons::isValid(iconRef) || !iconRef.colors().isEmpty() ||
        adqt::icons::describeIcon(iconRef).colorModel != adqt::icons::IconColorModel::Monochrome) {
        return palette;
    }

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    const bool solid = radio->buttonStyle() == adqt::widgets::AdRadio::ButtonStyle::Solid;
    const QColor checkedNormal = solid ? scheme.map.colorWhite : scheme.map.colorPrimary;
    const QColor checkedActive = solid ? scheme.map.colorWhite : scheme.map.colorPrimaryHover;
    const QColor checkedSelected = solid ? scheme.map.colorWhite : scheme.map.colorPrimaryActive;
    palette.set(QIcon::Normal, QIcon::Off, adqt::icons::IconColors::primary(scheme.map.colorText));
    palette.set(QIcon::Active, QIcon::Off,
                adqt::icons::IconColors::primary(scheme.map.colorPrimary));
    palette.set(QIcon::Selected, QIcon::Off,
                adqt::icons::IconColors::primary(scheme.map.colorPrimary));
    palette.set(QIcon::Disabled, QIcon::Off,
                adqt::icons::IconColors::primary(scheme.map.colorTextQuaternary));
    palette.set(QIcon::Normal, QIcon::On, adqt::icons::IconColors::primary(checkedNormal));
    palette.set(QIcon::Active, QIcon::On, adqt::icons::IconColors::primary(checkedActive));
    palette.set(QIcon::Selected, QIcon::On, adqt::icons::IconColors::primary(checkedSelected));
    palette.set(QIcon::Disabled, QIcon::On,
                adqt::icons::IconColors::primary(scheme.map.colorTextQuaternary));
    return palette;
}

class StyleRadioIconBinding final : public QObject {
  public:
    StyleRadioIconBinding(adqt::widgets::AdRadio* radio, const adqt::icons::IconRef& iconRef)
        : QObject(radio), m_radio(radio), m_iconRef(iconRef) {
        const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
        connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
                [this](const snow_shot::presentation::styles::ThemeColorScheme&) { refresh(); });
        refresh();
    }

    void setIconRef(const adqt::icons::IconRef& iconRef) {
        m_iconRef = iconRef;
        refresh();
    }

  private:
    void refresh() {
        if (m_radio == nullptr) {
            return;
        }
        m_radio->setIcon(
            adqt::icons::makeIcon(m_iconRef, styleRadioIconPalette(m_radio, m_iconRef)));
    }

    QPointer<adqt::widgets::AdRadio> m_radio;
    adqt::icons::IconRef m_iconRef;
};

// Style-toolbar action buttons use the same theme-aware icon color as the
// rest of the toolbar.  Keeping the original ref here lets us reapply the
// color when the application theme changes.
class StyleButtonIconBinding final : public QObject {
  public:
    StyleButtonIconBinding(adqt::widgets::AdButton* button,
                           const adqt::icons::IconRef& iconRef)
        : QObject(button), m_button(button), m_iconRef(iconRef) {
        const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
        connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
                [this](const snow_shot::presentation::styles::ThemeColorScheme&) { refresh(); });
        connect(m_button, &adqt::widgets::AdButton::buttonStyleChanged, this,
                [this](adqt::widgets::AdButton::ButtonStyle) { refresh(); });
        connect(m_button, &adqt::widgets::AdButton::accentRoleChanged, this,
                [this](adqt::widgets::AdButton::AccentRole) { refresh(); });
        m_button->installEventFilter(this);
        refresh();
    }

    void setIconRef(const adqt::icons::IconRef& iconRef) {
        m_iconRef = iconRef;
        refresh();
    }

  private:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_button && event != nullptr && event->type() == QEvent::EnabledChange) {
            refresh();
        }
        return QObject::eventFilter(watched, event);
    }

    void refresh() {
        if (m_button == nullptr) {
            return;
        }
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
        const bool solid =
            m_button->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid;
        const bool primary =
            m_button->accentRole() == adqt::widgets::AdButton::AccentRole::Primary;
        const QColor normal = solid && primary
                                  ? scheme.map.colorWhite
                                  : (primary ? scheme.map.colorPrimary : scheme.map.colorText);
        const QColor active = solid && primary
                                  ? scheme.map.colorWhite
                                  : (primary ? scheme.map.colorPrimaryHover : scheme.map.colorText);
        const QColor selected = solid && primary
                                    ? scheme.map.colorWhite
                                    : (primary ? scheme.map.colorPrimaryActive
                                               : scheme.map.colorText);

        adqt::icons::IconStatePalette palette;
        palette.set(QIcon::Normal, QIcon::Off, adqt::icons::IconColors::primary(normal));
        palette.set(QIcon::Active, QIcon::Off, adqt::icons::IconColors::primary(active));
        palette.set(QIcon::Selected, QIcon::Off, adqt::icons::IconColors::primary(selected));
        palette.set(QIcon::Disabled, QIcon::Off,
                    adqt::icons::IconColors::primary(scheme.map.colorTextQuaternary));
        m_button->setIconRef(m_iconRef);
        m_button->setIcon(adqt::icons::makeIcon(m_iconRef, palette));
    }

    QPointer<adqt::widgets::AdButton> m_button;
    adqt::icons::IconRef m_iconRef;
};

int scaledMetric(int value, qreal physicalScale) {
    if (value <= 0) {
        return 0;
    }
    return qMax(1, qRound(static_cast<qreal>(value) * physicalScale));
}

QColor strokeWidthPreviewColor(bool active) {
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    QColor color = active ? scheme.map.colorPrimary : scheme.map.colorTextSecondary;
    if (!color.isValid()) {
        color = QColor(QStringLiteral("#262626"));
    }
    return color;
}

void drawCompactValueText(QPainter* painter, const QRect& rect, const QFont& baseFont,
                          const QString& text, const QColor& color, qreal physicalScale) {
    if (painter == nullptr) {
        return;
    }

    const int textInset = std::max(1, qRound(2.0 * physicalScale));
    const QRect textRect = rect.adjusted(textInset, 0, -textInset, 0);
    QFont textFont = baseFont;
    if (textFont.pointSizeF() > 0.0) {
        const qreal maxPointSize = std::max<qreal>(1.0, 9.0 * physicalScale);
        const qreal minPointSize = std::max<qreal>(1.0, 6.5 * physicalScale);
        textFont.setPointSizeF(std::min<qreal>(textFont.pointSizeF(), maxPointSize));
        while (textFont.pointSizeF() > minPointSize &&
               QFontMetrics(textFont).horizontalAdvance(text) > textRect.width()) {
            textFont.setPointSizeF(textFont.pointSizeF() - 0.5);
        }
    } else {
        int pixelSize = textFont.pixelSize();
        if (pixelSize <= 0) {
            pixelSize = 11;
        }
        const int maxPixelSize = std::max(1, qRound(11.0 * physicalScale));
        const int minPixelSize = std::max(1, qRound(8.0 * physicalScale));
        textFont.setPixelSize(std::min(pixelSize, maxPixelSize));
        while (textFont.pixelSize() > minPixelSize &&
               QFontMetrics(textFont).horizontalAdvance(text) > textRect.width()) {
            textFont.setPixelSize(textFont.pixelSize() - 1);
        }
    }

    painter->setFont(textFont);
    painter->setPen(color);
    painter->drawText(textRect, Qt::AlignCenter, text);
}

void drawFillStyleIcon(QPainter* painter, const QRectF& iconRect, SnowCanvasFillStyle fillStyle,
                       const QColor& color) {
    if (painter == nullptr || iconRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->translate(iconRect.topLeft());
    painter->scale(iconRect.width() / 20.0, iconRect.height() / 20.0);

    const QRectF shapeRect(2.625, 2.625, 14.75, 14.75);
    QPainterPath shape;
    shape.addRoundedRect(shapeRect, fillStyle == SnowCanvasFillStyle::Solid ? 2.284 : 3.254,
                         fillStyle == SnowCanvasFillStyle::Solid ? 2.284 : 3.254);

    QPen pen(color, 1.25);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    if (fillStyle == SnowCanvasFillStyle::Solid) {
        painter->fillPath(shape, color);
        painter->drawPath(shape);
        painter->restore();
        return;
    }

    painter->setBrush(Qt::NoBrush);
    painter->drawPath(shape);
    painter->save();
    painter->setClipPath(shape);
    if (fillStyle == SnowCanvasFillStyle::Line) {
        painter->drawLine(QPointF(2.258, 15.156), QPointF(15.156, 2.258));
        painter->drawLine(QPointF(7.324, 20.222), QPointF(20.222, 7.325));
        painter->drawLine(QPointF(-0.222, 12.675), QPointF(12.675, -0.222));
        painter->drawLine(QPointF(4.518, 18.118), QPointF(17.416, 5.220));
    } else {
        painter->drawLine(QPointF(2.426, 15.044), QPointF(15.044, 2.426));
        painter->drawLine(QPointF(7.383, 20.000), QPointF(20.000, 7.383));
        painter->drawLine(QPointF(0.000, 12.617), QPointF(12.617, 0.000));
        painter->drawLine(QPointF(4.637, 17.941), QPointF(17.256, 5.324));
        painter->drawLine(QPointF(15.045, 17.574), QPointF(2.426, 4.956));
        painter->drawLine(QPointF(20.000, 12.617), QPointF(7.383, 0.000));
        painter->drawLine(QPointF(12.617, 20.000), QPointF(0.000, 7.383));
        painter->drawLine(QPointF(17.941, 15.363), QPointF(5.324, 2.745));
    }
    painter->restore();
    painter->restore();
}

void drawTransparentColorSwatch(QPainter* painter, const QRectF& swatchRect, qreal physicalScale,
                                const QColor& border, bool borderVisible) {
    if (painter == nullptr || swatchRect.isEmpty()) {
        return;
    }

    QPainterPath path;
    const qreal radius = std::max<qreal>(1.0, 3.0 * physicalScale);
    path.addRoundedRect(swatchRect, radius, radius);

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    const QColor baseColor =
        scheme.map.colorBgContainer.isValid() ? scheme.map.colorBgContainer : QColor(Qt::white);
    const QColor alternateColor = scheme.map.colorFillTertiary.isValid()
                                      ? scheme.map.colorFillTertiary
                                      : QColor(0xf0, 0xf0, 0xf0);
    const QColor errorColor =
        scheme.map.colorError.isValid() ? scheme.map.colorError : QColor(0xff, 0x4d, 0x4f);

    painter->fillPath(path, baseColor);
    painter->save();
    painter->setClipPath(path);
    const int tileSize = std::max(1, qRound(4.0 * physicalScale));
    for (int y = static_cast<int>(swatchRect.top()); y < swatchRect.bottom(); y += tileSize) {
        for (int x = static_cast<int>(swatchRect.left()); x < swatchRect.right(); x += tileSize) {
            const bool alternate = ((x / tileSize) + (y / tileSize)) % 2 == 0;
            painter->fillRect(QRectF(x, y, tileSize, tileSize),
                              alternate ? alternateColor : baseColor);
        }
    }
    painter->setPen(QPen(errorColor, std::max<qreal>(0.5, 1.5 * physicalScale)));
    painter->drawLine(swatchRect.bottomLeft(), swatchRect.topRight());
    painter->restore();

    if (borderVisible) {
        painter->setPen(QPen(border, std::max<qreal>(0.5, 1.0 * physicalScale)));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path);
    }
}

void drawStrokeStyleIcon(QPainter* painter, const QRectF& iconRect,
                         SnowCanvasArrowStrokeStyle strokeStyle, const QColor& color) {
    if (painter == nullptr || iconRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->translate(iconRect.topLeft());

    if (strokeStyle == SnowCanvasArrowStrokeStyle::Solid) {
        painter->scale(iconRect.width() / 20.0, iconRect.height() / 20.0);
        QPen pen(color, 1.25);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter->setPen(pen);
        painter->drawLine(QPointF(4.167, 10.0), QPointF(15.833, 10.0));
        painter->restore();
        return;
    }

    painter->scale(iconRect.width() / 24.0, iconRect.height() / 24.0);
    QPen pen(color, 2.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    if (strokeStyle == SnowCanvasArrowStrokeStyle::Dashed) {
        painter->drawLine(QPointF(5.0, 12.0), QPointF(7.0, 12.0));
        painter->drawLine(QPointF(11.0, 12.0), QPointF(13.0, 12.0));
        painter->drawLine(QPointF(17.0, 12.0), QPointF(19.0, 12.0));
    } else {
        for (qreal x : {4.0, 8.0, 12.0, 16.0, 20.0}) {
            painter->drawLine(QPointF(x, 12.0), QPointF(x, 12.01));
        }
    }
    painter->restore();
}

void applySharedButtonAccessibility(adqt::widgets::AdButton* button, const char* tooltip) {
    if (button == nullptr) {
        return;
    }
    if (tooltip != nullptr && tooltip[0] != '\0') {
        configureScreenshotToolPaletteTooltip(button, tooltip);
    }
    button->setFocusPolicy(Qt::NoFocus);
    button->setShape(adqt::widgets::AdButton::Shape::Rounded);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void drawNeutralButtonBorder(QPainter* painter, const QWidget* widget) {
    if (painter == nullptr || widget == nullptr) {
        return;
    }

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    QColor border = widget->isEnabled() ? scheme.map.colorBorder : scheme.map.colorBorderDisabled;
    if (!border.isValid()) {
        border = QColor(QStringLiteral("#d9d9d9"));
    }

    const qreal borderWidth = std::max<qreal>(1.0, scheme.metricMap.control.lineWidth);
    const qreal inset = borderWidth / 2.0;
    const qreal radius = std::max<qreal>(0.0, scheme.metricMap.radius.borderRadiusSM);
    const QRectF borderRect = QRectF(widget->rect()).adjusted(inset, inset, -inset, -inset);

    painter->save();
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(border, borderWidth));
    painter->drawRoundedRect(borderRect, radius, radius);
    painter->restore();
}

void drawMixedValueMark(QPainter* painter, const QRectF& bounds, const QColor& color,
                        qreal physicalScale) {
    if (painter == nullptr || bounds.isEmpty()) {
        return;
    }

    const qreal inset = std::max<qreal>(2.0, 6.0 * physicalScale);
    const QRectF markBounds = bounds.adjusted(inset, inset, -inset, -inset);
    QPen pen(color, std::max<qreal>(1.25, 1.75 * physicalScale));
    pen.setCapStyle(Qt::RoundCap);
    painter->save();
    painter->setPen(pen);
    painter->drawLine(markBounds.bottomLeft(), markBounds.topRight());
    painter->restore();
}

void drawStrokeStylePreview(QPainter* painter, const QWidget* widget, const QColor& color,
                            SnowCanvasArrowStrokeStyle strokeStyle, qreal physicalScale,
                            bool highlighted, bool outerBorderVisible) {
    if (painter == nullptr || widget == nullptr) {
        return;
    }

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    QColor contentColor;
    if (color.isValid()) {
        contentColor = color;
    } else if (highlighted) {
        contentColor = scheme.map.colorPrimary;
    } else {
        contentColor = scheme.map.colorTextSecondary;
    }
    if (!contentColor.isValid()) {
        contentColor = QColor(QStringLiteral("#595959"));
    }
    if (!widget->isEnabled()) {
        contentColor.setAlphaF(static_cast<float>(contentColor.alphaF() * 0.45));
    }

    const qreal inset = std::max<qreal>(1.0, 4.0 * physicalScale);
    const qreal iconSide = qMin<qreal>(STYLE_PREVIEW_ICON_SIZE * physicalScale,
                                       qMin(widget->width(), widget->height()) - inset);
    const QRectF iconRect(QRectF(widget->rect()).center().x() - iconSide / 2.0,
                          QRectF(widget->rect()).center().y() - iconSide / 2.0, iconSide, iconSide);

    if (outerBorderVisible) {
        drawNeutralButtonBorder(painter, widget);
    }
    drawStrokeStyleIcon(painter, iconRect, strokeStyle, contentColor);
}

void drawFillStylePreview(QPainter* painter, const QWidget* widget, const QColor& color,
                          SnowCanvasFillStyle fillStyle, qreal physicalScale, bool highlighted,
                          bool outerBorderVisible) {
    if (painter == nullptr || widget == nullptr) {
        return;
    }

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    const bool showTransparentColor = color.isValid() && color.alpha() == 0;
    QColor contentColor;
    if (color.isValid() && !showTransparentColor) {
        contentColor = color;
    } else if (highlighted) {
        contentColor = scheme.map.colorPrimary;
    } else {
        contentColor = scheme.map.colorTextSecondary;
    }
    if (!contentColor.isValid()) {
        contentColor = QColor(QStringLiteral("#595959"));
    }
    if (!widget->isEnabled()) {
        contentColor.setAlphaF(static_cast<float>(contentColor.alphaF() * 0.45));
    }

    const qreal inset = std::max<qreal>(1.0, 4.0 * physicalScale);
    const int iconSize = showTransparentColor ? STYLE_ICON_SIZE : STYLE_PREVIEW_ICON_SIZE;
    const qreal iconSide =
        qMin<qreal>(iconSize * physicalScale, qMin(widget->width(), widget->height()) - inset);
    const QRectF iconRect(QRectF(widget->rect()).center().x() - iconSide / 2.0,
                          QRectF(widget->rect()).center().y() - iconSide / 2.0, iconSide, iconSide);

    if (outerBorderVisible) {
        drawNeutralButtonBorder(painter, widget);
    }
    if (showTransparentColor) {
        QColor border = scheme.map.colorBorder;
        if (!border.isValid()) {
            border = QColor(QStringLiteral("#d9d9d9"));
        }
        drawTransparentColorSwatch(painter, iconRect, physicalScale, border, true);
    } else {
        drawFillStyleIcon(painter, iconRect, fillStyle, contentColor);
    }
}
} // namespace

StylePreviewButton::StylePreviewButton(QWidget* parent) : adqt::widgets::AdButton(parent) {
    setOutlined(true);
}

void StylePreviewButton::setOutlined(bool outlined) {
    setButtonStyle(outlined ? adqt::widgets::AdButton::ButtonStyle::Outline
                            : adqt::widgets::AdButton::ButtonStyle::Text);
    setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
}

StrokeWidthPreviewButton::StrokeWidthPreviewButton(QWidget* parent)
    : StylePreviewButton(parent), m_strokeWidth(DEFAULT_RECTANGLE_STROKE_WIDTH) {
    setAccessibleDescription(strokeWidthText());
}

void StrokeWidthPreviewButton::setStrokeWidth(double strokeWidth) {
    strokeWidth = std::clamp(strokeWidth, 0.0, MAX_RECTANGLE_STROKE_WIDTH);
    if (qFuzzyCompare(m_strokeWidth + 1.0, strokeWidth + 1.0)) {
        return;
    }
    m_strokeWidth = strokeWidth;
    setAccessibleDescription(m_mixed ? QCoreApplication::translate("ScreenshotToolPalette", "Mixed")
                                     : strokeWidthText());
    update();
}

void StrokeWidthPreviewButton::setActiveStrokeWidth(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    update();
}

void StrokeWidthPreviewButton::setMixed(bool mixed) {
    if (m_mixed == mixed) {
        return;
    }
    m_mixed = mixed;
    setAccessibleDescription(m_mixed ? QCoreApplication::translate("ScreenshotToolPalette", "Mixed")
                                     : strokeWidthText());
    update();
}

void StrokeWidthPreviewButton::setTextFallbackEnabled(bool enabled) {
    if (m_textFallbackEnabled == enabled) {
        return;
    }
    m_textFallbackEnabled = enabled;
    update();
}

void StrokeWidthPreviewButton::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }
    m_physicalScale = scale;
    update();
}

void StrokeWidthPreviewButton::paintEvent(QPaintEvent* event) {
    StylePreviewButton::paintEvent(event);

    const QColor lineColor = strokeWidthPreviewColor(m_active);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (m_mixed) {
        drawMixedValueMark(&painter, QRectF(rect()), lineColor, m_physicalScale);
        return;
    }
    const qreal minPreviewHeight =
        std::max<qreal>(0.5, MIN_RECTANGLE_STROKE_WIDTH * m_physicalScale);
    const qreal maxPreviewHeight = qMax<qreal>(
        minPreviewHeight, height() - STROKE_WIDTH_PREVIEW_VERTICAL_RESERVED * m_physicalScale);
    if (m_textFallbackEnabled && (m_strokeWidth <= 0.0 || m_strokeWidth > maxPreviewHeight)) {
        drawStrokeWidthText(&painter, lineColor);
        return;
    }

    const qreal previewHeight = std::clamp<qreal>(
        static_cast<qreal>(m_strokeWidth) * m_physicalScale, minPreviewHeight, maxPreviewHeight);
    const qreal horizontalInset =
        std::max<qreal>(1.0, STROKE_WIDTH_PREVIEW_HORIZONTAL_INSET * m_physicalScale);
    const QRectF previewRect(horizontalInset, QRectF(rect()).center().y() - previewHeight / 2.0,
                             qMax<qreal>(1.0, width() - horizontalInset * 2.0), previewHeight);
    const qreal radius = std::min<qreal>(previewHeight / 2.0, 3.0 * m_physicalScale);
    QPainterPath path;
    path.addRoundedRect(previewRect, radius, radius);
    painter.fillPath(path, lineColor);
}

QString StrokeWidthPreviewButton::strokeWidthText() const {
    const double rounded = std::round(m_strokeWidth);
    if (qFuzzyCompare(rounded + 1.0, m_strokeWidth + 1.0)) {
        return QStringLiteral("%1px").arg(static_cast<int>(rounded));
    }
    return QStringLiteral("%1px").arg(m_strokeWidth, 0, 'g', 3);
}

void StrokeWidthPreviewButton::drawStrokeWidthText(QPainter* painter, const QColor& color) const {
    drawCompactValueText(painter, rect(), font(), strokeWidthText(), color, m_physicalScale);
}

NumericValuePreviewButton::NumericValuePreviewButton(QWidget* parent)
    : StylePreviewButton(parent) {}

void NumericValuePreviewButton::setValue(double value) {
    if (!std::isfinite(value)) {
        value = 0.0;
    }
    if (qFuzzyCompare(m_value + 1.0, value + 1.0)) {
        return;
    }
    m_value = value;
    updateAccessibleValue();
    update();
}

void NumericValuePreviewButton::setSuffix(const QString& suffix) {
    if (m_suffix == suffix) {
        return;
    }
    m_suffix = suffix;
    updateAccessibleValue();
    update();
}

void NumericValuePreviewButton::setMixed(bool mixed) {
    if (m_mixed == mixed) {
        return;
    }
    m_mixed = mixed;
    updateAccessibleValue();
    update();
}

void NumericValuePreviewButton::setIntegerDisplay(bool integerDisplay) {
    if (m_integerDisplay == integerDisplay) {
        return;
    }
    m_integerDisplay = integerDisplay;
    updateAccessibleValue();
    update();
}

void NumericValuePreviewButton::setStrokeWidthPreviewEnabled(bool enabled) {
    if (m_strokeWidthPreviewEnabled == enabled) {
        return;
    }
    m_strokeWidthPreviewEnabled = enabled;
    update();
}

QString NumericValuePreviewButton::valueText() const {
    const double rounded = std::round(m_value);
    if (m_integerDisplay || qFuzzyCompare(rounded + 1.0, m_value + 1.0)) {
        return QString::number(static_cast<int>(rounded));
    }
    return QString::number(m_value, 'g', 3);
}

void NumericValuePreviewButton::updateAccessibleValue() {
    setAccessibleDescription(m_mixed ? QCoreApplication::translate("ScreenshotToolPalette", "Mixed")
                                     : valueText() + m_suffix);
}

void NumericValuePreviewButton::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }
    m_physicalScale = scale;
    update();
}

void NumericValuePreviewButton::paintEvent(QPaintEvent* event) {
    StylePreviewButton::paintEvent(event);

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    QColor textColor = isEnabled() ? scheme.map.colorPrimary : scheme.map.colorTextQuaternary;
    if (!textColor.isValid()) {
        textColor =
            isEnabled() ? QColor(QStringLiteral("#1677ff")) : QColor(QStringLiteral("#bfbfbf"));
    }
    QPainter painter(this);
    const qreal maxPreviewHeight =
        qMax<qreal>(0.5, height() - STROKE_WIDTH_PREVIEW_VERTICAL_RESERVED * m_physicalScale);
    if (m_strokeWidthPreviewEnabled && !m_mixed && m_value > 0.0 && m_value <= maxPreviewHeight) {
        const qreal previewHeight =
            std::clamp<qreal>(static_cast<qreal>(m_value) * m_physicalScale, 0.5, maxPreviewHeight);
        const qreal horizontalInset =
            std::max<qreal>(1.0, STROKE_WIDTH_PREVIEW_HORIZONTAL_INSET * m_physicalScale);
        const QRectF previewRect(horizontalInset, QRectF(rect()).center().y() - previewHeight / 2.0,
                                 qMax<qreal>(1.0, width() - horizontalInset * 2.0), previewHeight);
        const qreal radius = std::min<qreal>(previewHeight / 2.0, 3.0 * m_physicalScale);
        QPainterPath path;
        path.addRoundedRect(previewRect, radius, radius);
        painter.fillPath(path, textColor);
        return;
    }

    const QString text = m_mixed ? QStringLiteral("-") : valueText() + m_suffix;
    drawCompactValueText(&painter, rect(), font(), text, textColor, m_physicalScale);
}

ColorSwatchButton::ColorSwatchButton(QWidget* parent) : adqt::widgets::AdButton(parent) {}

IconValuePreviewTrigger::IconValuePreviewTrigger(QWidget* parent) : StylePreviewButton(parent) {}

void IconValuePreviewTrigger::setValueIconRef(const adqt::icons::IconRef& iconRef) {
    if (m_valueIconRef == iconRef) {
        return;
    }
    m_valueIconRef = iconRef;
    update();
}

void IconValuePreviewTrigger::setMixed(bool mixed) {
    if (m_mixed == mixed) {
        return;
    }

    m_mixed = mixed;
    update();
}

void IconValuePreviewTrigger::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }

    m_physicalScale = scale;
    update();
}

void IconValuePreviewTrigger::setIconSize(int size) {
    size = qMax(1, size);
    if (m_iconSize == size) {
        return;
    }

    m_iconSize = size;
    update();
}

void IconValuePreviewTrigger::paintEvent(QPaintEvent* event) {
    StylePreviewButton::paintEvent(event);

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    QColor color = scheme.map.colorText;
    if (!color.isValid()) {
        color = QColor(QStringLiteral("#141414"));
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (m_mixed) {
        drawMixedValueMark(&painter, QRectF(rect()), color, m_physicalScale);
        return;
    }

    const int iconSide = qMin(m_iconSize, qMin(width(), height()));
    const QRect iconRect((width() - iconSide) / 2, (height() - iconSide) / 2, iconSide, iconSide);
    const QPixmap icon = snow_shot::presentation::icons::renderTintedIconPixmap(
        m_valueIconRef, iconRect.size(), devicePixelRatioF(), color);
    if (!icon.isNull()) {
        painter.drawPixmap(iconRect, icon);
    }
}

void ColorSwatchButton::setSwatchColor(const QColor& color) {
    if (m_color == color) {
        return;
    }
    m_color = color;
    update();
}

void ColorSwatchButton::setSwatchBorderVisible(bool visible) {
    if (m_swatchBorderVisible == visible) {
        return;
    }
    m_swatchBorderVisible = visible;
    update();
}

void ColorSwatchButton::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }
    m_physicalScale = scale;
    update();
}

QColor ColorSwatchButton::swatchColor() const { return m_color; }

qreal ColorSwatchButton::swatchPhysicalScale() const { return m_physicalScale; }

void ColorSwatchButton::paintEvent(QPaintEvent* event) {
    adqt::widgets::AdButton::paintEvent(event);

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    QColor border = scheme.map.colorBorder;
    if (buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Solid) {
        border = scheme.map.colorWhite;
    }
    if (!border.isValid()) {
        border = QColor(QStringLiteral("#d9d9d9"));
    }

    const QColor swatch = m_color.isValid() ? m_color : QColor(Qt::transparent);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal inset = std::max<qreal>(1.0, 4.0 * m_physicalScale);
    const qreal swatchSide =
        qMin<qreal>(STYLE_ICON_SIZE * m_physicalScale, qMin(width(), height()) - inset);
    const QRectF swatchRect(QRectF(rect()).center().x() - swatchSide / 2.0,
                            QRectF(rect()).center().y() - swatchSide / 2.0, swatchSide, swatchSide);
    if (swatch.alpha() == 0) {
        drawTransparentColorSwatch(&painter, swatchRect, m_physicalScale, border,
                                   m_swatchBorderVisible);
        return;
    }

    QPainterPath path;
    const qreal radius = std::max<qreal>(1.0, 3.0 * m_physicalScale);
    path.addRoundedRect(swatchRect, radius, radius);
    painter.fillPath(path, swatch);
    if (m_swatchBorderVisible) {
        painter.setPen(QPen(border, std::max<qreal>(0.5, 1.0 * m_physicalScale)));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
}

ColorPickerSamplerButton::ColorPickerSamplerButton(QWidget* parent)
    : ColorSwatchButton(parent) {
    setSizeClass(adqt::widgets::AdButton::SizeClass::Small);
    setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
    setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    setCursor(Qt::PointingHandCursor);
}

void ColorPickerSamplerButton::paintEvent(QPaintEvent* event) {
    ColorSwatchButton::paintEvent(event);

    const QColor color = swatchColor();
    const int lightness = color.isValid() && color.alpha() > 0 ? qGray(color.rgb()) : 255;
    const QColor iconColor = lightness >= 136 ? QColor(Qt::black) : QColor(Qt::white);
    const int iconSide = qBound(10, qRound(14.0 * swatchPhysicalScale()),
                                qMin(width(), height()));
    const QRect iconRect((width() - iconSide) / 2, (height() - iconSide) / 2, iconSide, iconSide);
    const QPixmap icon = snow_shot::presentation::icons::renderTintedIconPixmap(
        snow_shot::presentation::icons::custom::outlined::ColorPicker(), iconRect.size(),
        devicePixelRatioF(), iconColor);
    if (icon.isNull()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawPixmap(iconRect, icon);
}

StrokeStylePreviewTrigger::StrokeStylePreviewTrigger(QWidget* parent)
    : StylePreviewButton(parent) {}

void StrokeStylePreviewTrigger::setStrokeColor(const QColor& color) {
    if (m_color == color) {
        return;
    }
    m_color = color;
    update();
}

void StrokeStylePreviewTrigger::setStrokeStyle(SnowCanvasArrowStrokeStyle strokeStyle) {
    if (m_strokeStyle == strokeStyle) {
        return;
    }
    m_strokeStyle = strokeStyle;
    update();
}

void StrokeStylePreviewTrigger::setMixed(bool mixed) {
    if (m_mixed == mixed) {
        return;
    }
    m_mixed = mixed;
    update();
}

void StrokeStylePreviewTrigger::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }
    m_physicalScale = scale;
    update();
}

void StrokeStylePreviewTrigger::paintEvent(QPaintEvent* event) {
    StylePreviewButton::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (m_mixed) {
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
        QColor color = scheme.map.colorTextSecondary;
        if (!color.isValid()) {
            color = QColor(QStringLiteral("#595959"));
        }
        drawMixedValueMark(&painter, QRectF(rect()), color, m_physicalScale);
        return;
    }
    drawStrokeStylePreview(&painter, this, m_color, m_strokeStyle, m_physicalScale, false, false);
}

StrokeStylePreviewButton::StrokeStylePreviewButton(QWidget* parent)
    : adqt::widgets::AdButton(parent) {}

void StrokeStylePreviewButton::setStrokeStyle(SnowCanvasArrowStrokeStyle strokeStyle) {
    if (m_strokeStyle == strokeStyle) {
        return;
    }
    m_strokeStyle = strokeStyle;
    update();
}

void StrokeStylePreviewButton::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }
    m_physicalScale = scale;
    update();
}

void StrokeStylePreviewButton::paintEvent(QPaintEvent* event) {
    adqt::widgets::AdButton::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawStrokeStylePreview(&painter, this, QColor(), m_strokeStyle, m_physicalScale,
                           buttonStyle() != adqt::widgets::AdButton::ButtonStyle::Text &&
                               accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
                           false);
}

FillStylePreviewTrigger::FillStylePreviewTrigger(QWidget* parent) : StylePreviewButton(parent) {}

void FillStylePreviewTrigger::setFillColor(const QColor& color) {
    if (m_color == color) {
        return;
    }
    m_color = color;
    update();
}

void FillStylePreviewTrigger::setFillStyle(SnowCanvasFillStyle fillStyle) {
    if (m_fillStyle == fillStyle) {
        return;
    }
    m_fillStyle = fillStyle;
    update();
}

void FillStylePreviewTrigger::setMixed(bool mixed) {
    if (m_mixed == mixed) {
        return;
    }
    m_mixed = mixed;
    update();
}

void FillStylePreviewTrigger::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }
    m_physicalScale = scale;
    update();
}

void FillStylePreviewTrigger::paintEvent(QPaintEvent* event) {
    StylePreviewButton::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (m_mixed) {
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
        QColor color = scheme.map.colorTextSecondary;
        if (!color.isValid()) {
            color = QColor(QStringLiteral("#595959"));
        }
        drawMixedValueMark(&painter, QRectF(rect()), color, m_physicalScale);
        return;
    }
    drawFillStylePreview(&painter, this, m_color, m_fillStyle, m_physicalScale, false, false);
}

FillStylePreviewButton::FillStylePreviewButton(QWidget* parent) : adqt::widgets::AdButton(parent) {}

void FillStylePreviewButton::setFillColor(const QColor& color) {
    if (m_color == color) {
        return;
    }
    m_color = color;
    update();
}

void FillStylePreviewButton::setFillStyle(SnowCanvasFillStyle fillStyle) {
    if (m_fillStyle == fillStyle) {
        return;
    }
    m_fillStyle = fillStyle;
    update();
}

void FillStylePreviewButton::setOuterBorderVisible(bool visible) {
    if (m_outerBorderVisible == visible) {
        return;
    }
    m_outerBorderVisible = visible;
    update();
}

void FillStylePreviewButton::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }
    m_physicalScale = scale;
    update();
}

void FillStylePreviewButton::paintEvent(QPaintEvent* event) {
    adqt::widgets::AdButton::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawFillStylePreview(&painter, this, m_color, m_fillStyle, m_physicalScale,
                         buttonStyle() != adqt::widgets::AdButton::ButtonStyle::Text &&
                             accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
                         m_outerBorderVisible);
}

IconNumericValuePreviewButton::IconNumericValuePreviewButton(QWidget* parent)
    : adqt::widgets::AdButton(parent), m_baseFont(font()) {}

int IconNumericValuePreviewButton::value() const {
    return m_value;
}

void IconNumericValuePreviewButton::setValue(int value) {
    if (m_value == value) {
        return;
    }
    m_value = value;
    update();
}

void IconNumericValuePreviewButton::setCornerRadius(int cornerRadius) {
    setValue(std::max(0, cornerRadius));
}

void IconNumericValuePreviewButton::setMixed(bool mixed) {
    if (m_mixed == mixed) {
        return;
    }
    m_mixed = mixed;
    update();
}

void IconNumericValuePreviewButton::setIconRef(const adqt::icons::IconRef& iconRef) {
    if (m_iconRef == iconRef) {
        return;
    }
    m_iconRef = iconRef;
    update();
}

void IconNumericValuePreviewButton::setCornerIconRef(const adqt::icons::IconRef& iconRef) {
    setIconRef(iconRef);
}

void IconNumericValuePreviewButton::setValueWidthReference(const QString& value) {
    if (value.isEmpty() || m_valueWidthReference == value) {
        return;
    }
    m_valueWidthReference = value;
    update();
}

void IconNumericValuePreviewButton::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }
    m_physicalScale = scale;

    QFont scaledFont = m_baseFont;
    if (scaledFont.pointSizeF() > 0.0) {
        scaledFont.setPointSizeF(scaledFont.pointSizeF() * m_physicalScale);
    } else if (scaledFont.pixelSize() > 0) {
        scaledFont.setPixelSize(
            std::max(1, qRound(static_cast<qreal>(scaledFont.pixelSize()) * m_physicalScale)));
    }
    setFont(scaledFont);
    update();
}

void IconNumericValuePreviewButton::paintEvent(QPaintEvent* event) {
    adqt::widgets::AdButton::paintEvent(event);

    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    QColor contentColor = scheme.map.colorText;
    if (!contentColor.isValid()) {
        contentColor = QColor(QStringLiteral("#262626"));
    }
    if (!isEnabled()) {
        contentColor.setAlphaF(static_cast<float>(contentColor.alphaF() * 0.45));
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawNeutralButtonBorder(&painter, this);
    painter.setFont(font());
    painter.setPen(contentColor);

    const QString valueText = m_mixed ? QStringLiteral("-") : QString::number(m_value);
    const int valueWidth = QFontMetrics(font()).horizontalAdvance(m_mixed ? QStringLiteral("-")
                                                                          : m_valueWidthReference);
    const int gap = scaledMetric(CORNER_RADIUS_ICON_TEXT_GAP, m_physicalScale);
    const QSize resolvedIconSize = iconSize();
    const int contentWidth = resolvedIconSize.width() + gap + valueWidth;
    const int contentLeft = (width() - contentWidth) / 2;
    const QRect iconRect(contentLeft, (height() - resolvedIconSize.height()) / 2,
                         resolvedIconSize.width(), resolvedIconSize.height());
    const QPixmap icon = snow_shot::presentation::icons::renderTintedIconPixmap(
        m_iconRef, resolvedIconSize, devicePixelRatioF(), contentColor);
    if (!icon.isNull()) {
        painter.drawPixmap(iconRect, icon);
    }

    const QRect valueRect(iconRect.right() + 1 + gap, 0, valueWidth, height());
    painter.drawText(valueRect, Qt::AlignHCenter | Qt::AlignVCenter, valueText);
}

void configureScreenshotToolPaletteTooltip(QWidget* trigger, const char* source) {
    if (source == nullptr || source[0] == '\0') {
        return;
    }

    configureScreenshotToolPaletteTooltip(trigger, ScreenshotToolPaletteTranslationText(source));
}

void configureScreenshotToolPaletteTooltip(QWidget* trigger,
                                           const ScreenshotToolPaletteTranslationText& text) {
    if (trigger == nullptr || text.source.isEmpty()) {
        return;
    }

    setTranslationSource(trigger, kTooltipSourceProperty, kTooltipArgumentsProperty, text);
    setTranslationSource(trigger, kAccessibleNameSourceProperty, kAccessibleNameArgumentsProperty,
                         text);
    const QString translated = text.translated();
    trigger->setToolTip(translated);
    trigger->setAccessibleName(translated);
}

void configureScreenshotToolPalettePopoverTrigger(QWidget* trigger, const char* source) {
    if (trigger == nullptr || source == nullptr || source[0] == '\0') {
        return;
    }

    setScreenshotToolPaletteAccessibleNameSource(trigger, source);
    trigger->setToolTip(QString());
    trigger->setAccessibleName(ScreenshotToolPaletteTranslationText(source).translated());
}

void setScreenshotToolPaletteTooltipSource(QWidget* widget, const char* source) {
    if (source == nullptr || source[0] == '\0') {
        return;
    }
    setTranslationSource(widget, kTooltipSourceProperty, kTooltipArgumentsProperty,
                         ScreenshotToolPaletteTranslationText(source));
}

void setScreenshotToolPaletteAccessibleNameSource(QWidget* widget, const char* source) {
    if (source == nullptr || source[0] == '\0') {
        return;
    }
    setTranslationSource(widget, kAccessibleNameSourceProperty, kAccessibleNameArgumentsProperty,
                         ScreenshotToolPaletteTranslationText(source));
}

void setScreenshotToolPalettePlaceholderSource(QWidget* widget, const char* source) {
    if (source == nullptr || source[0] == '\0') {
        return;
    }
    setTranslationSource(widget, kPlaceholderSourceProperty, kPlaceholderArgumentsProperty,
                         ScreenshotToolPaletteTranslationText(source));
}

void setScreenshotToolPaletteItemTranslationSource(QStandardItem* item, const char* source) {
    if (source == nullptr || source[0] == '\0') {
        return;
    }
    setScreenshotToolPaletteItemTranslationSource(item,
                                                  ScreenshotToolPaletteTranslationText(source));
}

void setScreenshotToolPaletteItemTranslationSource(
    QStandardItem* item, const ScreenshotToolPaletteTranslationText& text) {
    if (item == nullptr || text.source.isEmpty()) {
        return;
    }
    item->setData(text.source, kItemTranslationSourceRole);
    item->setData(text.arguments, kItemTranslationArgumentsRole);
    item->setData(QString::fromLatin1(kTranslationContext), kItemTranslationContextRole);
    item->setData(text.translated(), adqt::widgets::AdSelect::DefaultLabelRole);
}

void retranslateScreenshotToolPalette(QWidget* root) {
    if (root == nullptr) {
        return;
    }

    const auto retranslateWidget = [](QWidget* widget) {
        if (widget == nullptr) {
            return;
        }
        const QVariant context = widget->property("snowShotTranslationContext");
        const QVariant tooltipSource = widget->property(kTooltipSourceProperty);
        const QVariant tooltipArguments = widget->property(kTooltipArgumentsProperty);
        if (tooltipSource.isValid()) {
            widget->setToolTip(translateSource(tooltipSource, context, tooltipArguments));
        }
        const QVariant accessibleSource = widget->property(kAccessibleNameSourceProperty);
        const QVariant accessibleArguments = widget->property(kAccessibleNameArgumentsProperty);
        if (accessibleSource.isValid()) {
            widget->setAccessibleName(
                translateSource(accessibleSource, context, accessibleArguments));
        }
        const QVariant placeholderSource = widget->property(kPlaceholderSourceProperty);
        const QVariant placeholderArguments = widget->property(kPlaceholderArgumentsProperty);
        if (placeholderSource.isValid()) {
            const QString translated =
                translateSource(placeholderSource, context, placeholderArguments);
            if (auto* select = qobject_cast<adqt::widgets::AdSelect*>(widget)) {
                select->setPlaceholder(translated);
            } else if (auto* lineEdit = qobject_cast<QLineEdit*>(widget)) {
                lineEdit->setPlaceholderText(translated);
            }
        }
    };

    const auto retranslateSubtree = [&retranslateWidget](QWidget* subtree) {
        if (subtree == nullptr) {
            return;
        }

        retranslateWidget(subtree);
        for (QWidget* widget : subtree->findChildren<QWidget*>()) {
            retranslateWidget(widget);
        }

        for (QStandardItemModel* model : subtree->findChildren<QStandardItemModel*>()) {
            if (model == nullptr) {
                continue;
            }
            for (int row = 0; row < model->rowCount(); ++row) {
                for (int column = 0; column < model->columnCount(); ++column) {
                    QStandardItem* item = model->item(row, column);
                    if (item == nullptr) {
                        continue;
                    }
                    const QVariant source = item->data(kItemTranslationSourceRole);
                    if (!source.isValid()) {
                        continue;
                    }
                    const QVariant context = item->data(kItemTranslationContextRole);
                    const QVariant arguments = item->data(kItemTranslationArgumentsRole);
                    item->setData(translateSource(source, context, arguments),
                                  adqt::widgets::AdSelect::DefaultLabelRole);
                }
            }
        }
    };

    retranslateSubtree(root);
    for (adqt::widgets::AdPopover* popover : root->findChildren<adqt::widgets::AdPopover*>()) {
        retranslateSubtree(popover->contentWidget());
    }
}

bool screenshotToolPaletteMetricsApplyTo(const ScreenshotToolPaletteButtonMetrics& metrics,
                                         const QWidget* widget) {
    return widget != nullptr && (metrics.scope == nullptr || metrics.scope == widget ||
                                 metrics.scope->isAncestorOf(widget));
}

ScreenshotToolPaletteSliderEditor
createScreenshotToolPaletteSliderEditor(QBoxLayout* layout, QWidget* parent,
                                        const ScreenshotToolPaletteSliderEditorConfig& config,
                                        const ScreenshotToolPaletteButtonMetrics& metrics) {
    ScreenshotToolPaletteSliderEditor editor;
    if (layout == nullptr || parent == nullptr) {
        return editor;
    }

    editor.iconRef = config.iconRef;
    editor.baseIconSize = config.baseIconSize > 0 ? config.baseIconSize : metrics.iconSize;
    editor.baseSliderWidth = config.baseSliderWidth;
    editor.icon = new QLabel(parent);
    editor.icon->setObjectName(config.iconObjectName);
    editor.icon->setAlignment(Qt::AlignCenter);
    configureScreenshotToolPaletteTooltip(
        editor.icon, ScreenshotToolPaletteTranslationText(config.accessibleName));
    editor.icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(editor.icon);

    editor.slider = new adqt::widgets::AdSlider(parent);
    editor.slider->setFocusPolicy(Qt::NoFocus);
    editor.slider->setObjectName(config.sliderObjectName);
    editor.slider->setRange(0, 100);
    editor.slider->setSingleStep(1);
    editor.slider->setPageStep(5);
    editor.slider->setValue(config.initialValue);
    editor.slider->setTooltipFormatter(
        [](double value) { return QStringLiteral("%1%").arg(qRound(value)); });
    configureScreenshotToolPaletteTooltip(
        editor.slider, ScreenshotToolPaletteTranslationText(config.sliderTooltip));
    setScreenshotToolPaletteAccessibleNameSource(editor.slider,
                                                 config.accessibleName.toUtf8().constData());
    editor.slider->setAccessibleName(
        ScreenshotToolPaletteTranslationText(config.accessibleName).translated());
    editor.slider->setAccessibleDescription(QStringLiteral("%1%").arg(config.initialValue));
    layout->addWidget(editor.slider);
    configureScreenshotToolPaletteSliderEditor(editor, metrics);
    return editor;
}

ScreenshotToolPaletteSelectEditor
createScreenshotToolPaletteSelectEditor(QWidget* parent,
                                        const ScreenshotToolPaletteSelectEditorConfig& config,
                                        const ScreenshotToolPaletteButtonMetrics& metrics) {
    ScreenshotToolPaletteSelectEditor editor;
    if (parent == nullptr) {
        return editor;
    }

    editor.baseWidth = config.baseWidth;
    editor.select = new adqt::widgets::AdSelect(parent);
    editor.select->setFocusPolicy(Qt::NoFocus);
    editor.select->setObjectName(config.objectName);
    setScreenshotToolPaletteTooltipSource(editor.select, config.tooltip.toUtf8().constData());
    setScreenshotToolPaletteAccessibleNameSource(editor.select,
                                                 config.accessibleName.toUtf8().constData());
    setScreenshotToolPalettePlaceholderSource(editor.select,
                                              config.placeholder.toUtf8().constData());
    editor.select->setToolTip(ScreenshotToolPaletteTranslationText(config.tooltip).translated());
    editor.select->setAccessibleName(
        ScreenshotToolPaletteTranslationText(config.accessibleName).translated());
    editor.select->setMode(adqt::widgets::AdSelect::Mode::Single);
    editor.select->setPlaceholder(
        ScreenshotToolPaletteTranslationText(config.placeholder).translated());
    editor.select->setControlSize(adqt::widgets::AdSelect::ControlSize::Small);
    editor.select->setVariant(adqt::widgets::AdSelect::Variant::Borderless);
    editor.select->setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::QtTool);
    editor.select->setSearchEnabled(config.searchEnabled);
    editor.select->setPopupMatchSelectWidth(false);
    editor.select->setValueRole(adqt::widgets::AdSelect::DefaultValueRole);
    editor.select->setLabelRole(adqt::widgets::AdSelect::DefaultLabelRole);
    configureScreenshotToolPaletteSelectEditor(editor, metrics);
    return editor;
}

ScreenshotToolPaletteRadioEditor
createScreenshotToolPaletteRadioEditor(QWidget* parent,
                                       const ScreenshotToolPaletteRadioEditorConfig& config,
                                       const ScreenshotToolPaletteButtonMetrics& metrics) {
    ScreenshotToolPaletteRadioEditor editor;
    if (parent == nullptr) {
        return editor;
    }

    editor.container = new QWidget(parent);
    editor.container->setObjectName(config.objectName);
    auto* layout = new QHBoxLayout(editor.container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    editor.group = new adqt::widgets::AdRadioButtonGroup(editor.container);
    editor.group->setManagedLayout(layout);
    editor.group->setVariant(adqt::widgets::AdRadio::Variant::Button);
    editor.group->setControlSize(adqt::widgets::AdRadio::ControlSize::Small);
    for (const ScreenshotToolPaletteRadioOption& option : config.options) {
        auto* radio = new adqt::widgets::AdRadio(editor.container);
        radio->setFocusPolicy(Qt::NoFocus);
        configureScreenshotToolPaletteTooltip(radio, option.tooltip);
        setScreenshotToolPaletteStyleRadioIcon(radio, option.iconRef);
        radio->setIconSize(QSize(16, 16));
        editor.group->addButton(radio, option.id);
        layout->addWidget(radio);
        editor.buttons.push_back(radio);
    }
    configureScreenshotToolPaletteStyleRadioButtonGroup(editor.group, metrics,
                                                         config.useButtonMetrics);
    editor.group->setCheckedId(config.initialId);
    return editor;
}

ScreenshotToolPaletteOptionPopoverEditor createScreenshotToolPaletteOptionPopoverEditor(
    adqt::widgets::AdButton* trigger, QObject* receiver,
    const ScreenshotToolPaletteOptionPopoverEditorConfig& config,
    const std::function<void(int)>& activateValue,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    ScreenshotToolPaletteOptionPopoverEditor editor;
    if (trigger == nullptr || receiver == nullptr || config.options.isEmpty()) {
        return editor;
    }

    configureScreenshotToolPalettePopoverTrigger(trigger,
                                                 config.accessibleName.toUtf8().constData());
    editor.popover = new adqt::widgets::AdPopover(trigger);
    editor.popover->setSourceWidget(trigger);
    editor.popover->setTriggers(adqt::widgets::AdPopover::Trigger::Hover);
    editor.popover->setPlacement(adqt::widgets::AdPopover::Placement::Top);
    editor.popover->setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::QtTool);
    editor.popover->setArrowVisible(true);

    auto* content = new QWidget();
    content->setObjectName(config.contentObjectName);
    content->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(config.optionSpacing);
    for (const ScreenshotToolPaletteOptionPopoverOption& option : config.options) {
        auto* button =
            createScreenshotToolPaletteToolButton(content, nullptr, option.iconRef, metrics);
        configureScreenshotToolPaletteTooltip(button, option.tooltip);
        editor.buttons.push_back(button);
        editor.values.push_back(option.value);
        layout->addWidget(button);
        QObject::connect(button, &adqt::widgets::AdButton::clicked, receiver,
                         [popover = editor.popover, activateValue, value = option.value]() {
                             if (activateValue) {
                                 activateValue(value);
                             }
                             popover->hide();
                         });
    }
    editor.popover->setContentWidget(content);
    configureScreenshotToolPaletteOptionPopoverEditor(editor.popover, editor.buttons,
                                                      config.optionSpacing, metrics);
    return editor;
}

void configureScreenshotToolPaletteOptionPopoverEditor(
    adqt::widgets::AdPopover* popover, const QVector<adqt::widgets::AdButton*>& buttons,
    int optionSpacing, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (popover == nullptr) {
        return;
    }
    for (adqt::widgets::AdButton* button : buttons) {
        configureScreenshotToolPaletteBaseButton(button, nullptr, metrics);
    }
    if (QWidget* content = popover->contentWidget()) {
        if (QBoxLayout* layout = qobject_cast<QBoxLayout*>(content->layout())) {
            layout->setSpacing(optionSpacing);
            layout->invalidate();
            layout->activate();
            content->adjustSize();
        }
    }
}

void updateScreenshotToolPaletteOptionPopoverEditor(
    const QVector<adqt::widgets::AdButton*>& buttons, const QVector<int>& values, int activeValue) {
    for (int index = 0; index < buttons.size(); ++index) {
        adqt::widgets::AdButton* button = buttons.at(index);
        if (button == nullptr) {
            continue;
        }
        const bool active = index < values.size() && values.at(index) == activeValue;
        button->setButtonStyle(active ? adqt::widgets::AdButton::ButtonStyle::Solid
                                      : adqt::widgets::AdButton::ButtonStyle::Text);
        button->setAccentRole(active ? adqt::widgets::AdButton::AccentRole::Primary
                                     : adqt::widgets::AdButton::AccentRole::Neutral);
    }
}

void configureScreenshotToolPaletteSelectEditor(ScreenshotToolPaletteSelectEditor& editor,
                                                const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (editor.select == nullptr || !screenshotToolPaletteMetricsApplyTo(metrics, editor.select)) {
        return;
    }
    editor.select->setFixedSize(qMax(1, qRound(editor.baseWidth * metrics.physicalScale)),
                                qMax(1, qRound(metrics.buttonSize * metrics.physicalScale)));
}

void configureScreenshotToolPaletteSliderEditor(ScreenshotToolPaletteSliderEditor& editor,
                                                const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (editor.icon != nullptr && screenshotToolPaletteMetricsApplyTo(metrics, editor.icon)) {
        const int controlSize = qMax(1, qRound(metrics.buttonSize * metrics.physicalScale));
        const int iconSize = qMax(1, qRound(editor.baseIconSize * metrics.physicalScale));
        editor.icon->setFixedSize(controlSize, controlSize);
        editor.icon->setPixmap(snow_shot::presentation::icons::renderTintedIconPixmap(
            editor.iconRef, QSize(iconSize, iconSize), editor.icon->devicePixelRatioF(),
            snow_shot::presentation::styles::generateThemeColorScheme().map.colorText));
    }
    if (editor.slider != nullptr && screenshotToolPaletteMetricsApplyTo(metrics, editor.slider)) {
        editor.slider->setFixedSize(qMax(1, qRound(editor.baseSliderWidth * metrics.physicalScale)),
                                    qMax(1, qRound(metrics.buttonSize * metrics.physicalScale)));
    }
}

void configureScreenshotToolPaletteBaseButton(adqt::widgets::AdButton* button, const char* tooltip,
                                              const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
        return;
    }

    applySharedButtonAccessibility(button, tooltip);
    button->setSizeClass(adqt::widgets::AdButton::SizeClass::Medium);
    button->setIconSize(QSize(scaledMetric(metrics.iconSize, metrics.physicalScale),
                              scaledMetric(metrics.iconSize, metrics.physicalScale)));
    button->setFixedSize(scaledMetric(metrics.buttonSize, metrics.physicalScale),
                         scaledMetric(metrics.buttonSize, metrics.physicalScale));
}

void configureScreenshotToolPaletteStyleButton(adqt::widgets::AdButton* button, const char* tooltip,
                                               const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
        return;
    }

    applySharedButtonAccessibility(button, tooltip);
    button->setSizeClass(adqt::widgets::AdButton::SizeClass::Small);
    button->setIconSize(QSize(scaledMetric(metrics.iconSize, metrics.physicalScale),
                              scaledMetric(metrics.iconSize, metrics.physicalScale)));
    button->setFixedSize(scaledMetric(metrics.buttonSize, metrics.physicalScale),
                         scaledMetric(metrics.buttonSize, metrics.physicalScale));
}

void configureScreenshotToolPaletteStyleRadioButtonGroup(
    adqt::widgets::AdRadioButtonGroup* group, const ScreenshotToolPaletteButtonMetrics& metrics,
    bool useButtonMetrics) {
    if (group == nullptr) {
        return;
    }

    bool applied = false;
    for (QAbstractButton* button : group->buttons()) {
        auto* radio = qobject_cast<adqt::widgets::AdRadio*>(button);
        if (!screenshotToolPaletteMetricsApplyTo(metrics, radio)) {
            continue;
        }
        applied = true;

        QSize baseSize = radio->property(STYLE_RADIO_BASE_SIZE_PROPERTY).toSize();
        if (!baseSize.isValid()) {
            baseSize = useButtonMetrics ? QSize(metrics.buttonSize, metrics.buttonSize)
                                        : radio->sizeHint();
            radio->setProperty(STYLE_RADIO_BASE_SIZE_PROPERTY, baseSize);
        }

        QSize baseIconSize = radio->property(STYLE_RADIO_BASE_ICON_SIZE_PROPERTY).toSize();
        if (!baseIconSize.isValid()) {
            baseIconSize = useButtonMetrics ? QSize(metrics.iconSize, metrics.iconSize)
                                            : radio->iconSize();
            radio->setProperty(STYLE_RADIO_BASE_ICON_SIZE_PROPERTY, baseIconSize);
        }

        radio->setIconSize(QSize(scaledMetric(baseIconSize.width(), metrics.physicalScale),
                                 scaledMetric(baseIconSize.height(), metrics.physicalScale)));
        radio->setFixedSize(scaledMetric(baseSize.width(), metrics.physicalScale),
                            scaledMetric(baseSize.height(), metrics.physicalScale));
    }

    if (applied) {
        if (QBoxLayout* layout = group->managedLayout()) {
            layout->invalidate();
        }
    }
}

void setScreenshotToolPaletteStyleRadioIcon(adqt::widgets::AdRadio* radio,
                                            const adqt::icons::IconRef& iconRef) {
    if (radio == nullptr) {
        return;
    }

    for (QObject* child : radio->children()) {
        if (auto* binding = dynamic_cast<StyleRadioIconBinding*>(child)) {
            binding->setIconRef(iconRef);
            return;
        }
    }
    new StyleRadioIconBinding(radio, iconRef);
}

void setScreenshotToolPaletteToolButtonIcon(adqt::widgets::AdButton* button,
                                            const adqt::icons::IconRef& iconRef) {
    if (button == nullptr) {
        return;
    }

    for (QObject* child : button->children()) {
        if (auto* binding = dynamic_cast<StyleButtonIconBinding*>(child)) {
            binding->setIconRef(iconRef);
            return;
        }
    }
    button->setIconRef(iconRef);
}

adqt::widgets::AdButton*
createScreenshotToolPaletteToolButton(QWidget* parent, const char* tooltip,
                                      const adqt::icons::IconRef& iconRef,
                                      const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new adqt::widgets::AdButton(parent);
    configureScreenshotToolPaletteBaseButton(button, tooltip, metrics);
    new StyleButtonIconBinding(button, iconRef);
    button->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    return button;
}

adqt::widgets::AdButton* createScreenshotToolPaletteActionButton(
    QWidget* parent, const char* tooltip, const adqt::icons::IconRef& iconRef, bool danger,
    bool primary, const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new adqt::widgets::AdButton(parent);
    configureScreenshotToolPaletteBaseButton(button, tooltip, metrics);
    button->setIconRef(iconRef);
    button->setButtonStyle(primary ? adqt::widgets::AdButton::ButtonStyle::Solid
                                   : adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(primary ? adqt::widgets::AdButton::AccentRole::Primary
                                  : (danger ? adqt::widgets::AdButton::AccentRole::Danger
                                            : adqt::widgets::AdButton::AccentRole::Neutral));
    return button;
}

StrokeWidthPreviewButton*
createScreenshotToolPaletteStrokeWidthButton(QWidget* parent, const char* tooltip,
                                             double strokeWidth, bool summary,
                                             const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new StrokeWidthPreviewButton(parent);
    configureScreenshotToolPaletteStyleButton(button, tooltip, metrics);
    button->setOutlined(summary);
    button->setStrokeWidth(strokeWidth);
    button->setActiveStrokeWidth(summary);
    button->setTextFallbackEnabled(summary);
    button->setPhysicalScale(metrics.physicalScale);
    if (summary) {
        button->setCursor(Qt::SplitVCursor);
    }
    return button;
}

NumericValuePreviewButton*
createScreenshotToolPaletteNumericValueButton(QWidget* parent, const char* tooltip, double value,
                                              const QString& suffix,
                                              const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new NumericValuePreviewButton(parent);
    configureScreenshotToolPaletteStyleButton(button, tooltip, metrics);
    button->setValue(value);
    button->setSuffix(suffix);
    button->setPhysicalScale(metrics.physicalScale);
    button->setCursor(Qt::SplitVCursor);
    return button;
}

ColorSwatchButton*
createScreenshotToolPaletteColorButton(QWidget* parent, const char* tooltip, const QColor& color,
                                       bool summary, bool swatchBorderVisible,
                                       const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new ColorSwatchButton(parent);
    configureScreenshotToolPaletteStyleButton(button, tooltip, metrics);
    button->setButtonStyle(summary ? adqt::widgets::AdButton::ButtonStyle::Outline
                                   : adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    button->setSwatchColor(color);
    button->setSwatchBorderVisible(swatchBorderVisible);
    button->setPhysicalScale(metrics.physicalScale);
    return button;
}

ColorPickerSamplerButton* createScreenshotToolPaletteColorPickerSamplerButton(
    QWidget* parent, const QColor& color) {
    auto* button = new ColorPickerSamplerButton(parent);
    button->setSwatchColor(color);
    button->setSwatchBorderVisible(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

adqt::widgets::AdButton*
createScreenshotToolPaletteStyleActionButton(QWidget* parent, const char* tooltip,
                                             const adqt::icons::IconRef& iconRef,
                                             const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new adqt::widgets::AdButton(parent);
    configureScreenshotToolPaletteStyleButton(button, tooltip, metrics);
    new StyleButtonIconBinding(button, iconRef);
    button->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    return button;
}

IconValuePreviewTrigger* createScreenshotToolPaletteIconValuePreviewTrigger(
    QWidget* parent, const char* tooltip, const adqt::icons::IconRef& iconRef,
    const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* trigger = new IconValuePreviewTrigger(parent);
    trigger->setValueIconRef(iconRef);
    configureScreenshotToolPaletteStyleButton(trigger, tooltip, metrics);
    configureScreenshotToolPaletteIconValuePreviewTrigger(trigger, metrics);
    return trigger;
}

void configureScreenshotToolPaletteIconValuePreviewTrigger(
    IconValuePreviewTrigger* trigger, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, trigger)) {
        return;
    }

    configureScreenshotToolPaletteStyleButton(trigger, nullptr, metrics);
    trigger->setIconSize(scaledMetric(metrics.iconSize, metrics.physicalScale));
    trigger->setPhysicalScale(metrics.physicalScale);
}

StrokeStylePreviewTrigger* createScreenshotToolPaletteStrokeStyleTrigger(
    QWidget* parent, const char* tooltip, const QColor& color,
    SnowCanvasArrowStrokeStyle strokeStyle, const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* trigger = new StrokeStylePreviewTrigger(parent);
    trigger->setStrokeColor(color);
    trigger->setStrokeStyle(strokeStyle);
    configureScreenshotToolPaletteStyleButton(trigger, tooltip, metrics);
    configureScreenshotToolPaletteStrokeStyleTrigger(trigger, metrics);
    return trigger;
}

void configureScreenshotToolPaletteStrokeStyleTrigger(
    StrokeStylePreviewTrigger* trigger, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, trigger)) {
        return;
    }
    configureScreenshotToolPaletteStyleButton(trigger, nullptr, metrics);
    trigger->setPhysicalScale(metrics.physicalScale);
}

StrokeStylePreviewButton*
createScreenshotToolPaletteStrokeStyleButton(QWidget* parent, const char* tooltip,
                                             SnowCanvasArrowStrokeStyle strokeStyle,
                                             const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new StrokeStylePreviewButton(parent);
    configureScreenshotToolPaletteStyleButton(button, tooltip, metrics);
    button->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    button->setStrokeStyle(strokeStyle);
    button->setPhysicalScale(metrics.physicalScale);
    return button;
}

FillStylePreviewTrigger*
createScreenshotToolPaletteFillStyleTrigger(QWidget* parent, const char* tooltip,
                                            const QColor& color, SnowCanvasFillStyle fillStyle,
                                            const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* trigger = new FillStylePreviewTrigger(parent);
    trigger->setFillColor(color);
    trigger->setFillStyle(fillStyle);
    configureScreenshotToolPaletteStyleButton(trigger, tooltip, metrics);
    configureScreenshotToolPaletteFillStyleTrigger(trigger, metrics);
    return trigger;
}

void configureScreenshotToolPaletteFillStyleTrigger(
    FillStylePreviewTrigger* trigger, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, trigger)) {
        return;
    }
    configureScreenshotToolPaletteStyleButton(trigger, nullptr, metrics);
    trigger->setPhysicalScale(metrics.physicalScale);
}

FillStylePreviewButton* createScreenshotToolPaletteFillStyleButton(
    QWidget* parent, const char* tooltip, const QColor& color, SnowCanvasFillStyle fillStyle,
    bool summary, const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new FillStylePreviewButton(parent);
    configureScreenshotToolPaletteStyleButton(button, tooltip, metrics);
    button->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    button->setFillColor(color);
    button->setFillStyle(fillStyle);
    button->setOuterBorderVisible(summary);
    button->setPhysicalScale(metrics.physicalScale);
    return button;
}

CornerRadiusEditorButton*
createScreenshotToolPaletteCornerRadiusEditor(QWidget* parent, const char* tooltip,
                                              const adqt::icons::IconRef& iconRef, int cornerRadius,
                                              const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new IconNumericValuePreviewButton(parent);
    configureScreenshotToolPaletteCornerRadiusEditor(button, metrics);
    applySharedButtonAccessibility(button, tooltip);
    button->setCornerIconRef(iconRef);
    button->setCornerRadius(cornerRadius);
    button->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    button->setCursor(Qt::SplitVCursor);
    return button;
}

void configureScreenshotToolPaletteCornerRadiusEditor(
    CornerRadiusEditorButton* button, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
        return;
    }
    configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
    button->setFixedWidth(scaledMetric(CORNER_RADIUS_EDITOR_WIDTH, metrics.physicalScale));
    button->setPhysicalScale(metrics.physicalScale);
}

IconNumericValuePreviewButton* createScreenshotToolPaletteIconNumericValueButton(
    QWidget* parent, const char* tooltip, const adqt::icons::IconRef& iconRef, int value,
    const QString& valueWidthReference, const ScreenshotToolPaletteButtonMetrics& metrics) {
    auto* button = new IconNumericValuePreviewButton(parent);
    button->setIconRef(iconRef);
    button->setValue(value);
    button->setValueWidthReference(valueWidthReference);
    configureScreenshotToolPaletteIconNumericValueButton(button, metrics);
    applySharedButtonAccessibility(button, tooltip);
    button->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    button->setCursor(Qt::SplitVCursor);
    return button;
}

void configureScreenshotToolPaletteIconNumericValueButton(
    IconNumericValuePreviewButton* button, const ScreenshotToolPaletteButtonMetrics& metrics) {
    if (!screenshotToolPaletteMetricsApplyTo(metrics, button)) {
        return;
    }
    configureScreenshotToolPaletteStyleButton(button, nullptr, metrics);
    button->setFixedWidth(scaledMetric(WATERMARK_NUMERIC_EDITOR_WIDTH, metrics.physicalScale));
    button->setPhysicalScale(metrics.physicalScale);
}

void StrokeWidthPreviewButton::commitControlScale(
    const adqt::widgets::AdControlScaleContext& context) {
    adqt::widgets::AdButton::commitControlScale(context);
    setPhysicalScale(context.logicalScale);
}

void NumericValuePreviewButton::commitControlScale(
    const adqt::widgets::AdControlScaleContext& context) {
    adqt::widgets::AdButton::commitControlScale(context);
    setPhysicalScale(context.logicalScale);
}

void ColorSwatchButton::commitControlScale(const adqt::widgets::AdControlScaleContext& context) {
    adqt::widgets::AdButton::commitControlScale(context);
    setPhysicalScale(context.logicalScale);
}

void IconValuePreviewTrigger::commitControlScale(
    const adqt::widgets::AdControlScaleContext& context) {
    adqt::widgets::AdButton::commitControlScale(context);
    setPhysicalScale(context.logicalScale);
}

void StrokeStylePreviewTrigger::commitControlScale(
    const adqt::widgets::AdControlScaleContext& context) {
    adqt::widgets::AdButton::commitControlScale(context);
    setPhysicalScale(context.logicalScale);
}

void StrokeStylePreviewButton::commitControlScale(
    const adqt::widgets::AdControlScaleContext& context) {
    adqt::widgets::AdButton::commitControlScale(context);
    setPhysicalScale(context.logicalScale);
}

void FillStylePreviewTrigger::commitControlScale(
    const adqt::widgets::AdControlScaleContext& context) {
    adqt::widgets::AdButton::commitControlScale(context);
    setPhysicalScale(context.logicalScale);
}

void FillStylePreviewButton::commitControlScale(
    const adqt::widgets::AdControlScaleContext& context) {
    adqt::widgets::AdButton::commitControlScale(context);
    setPhysicalScale(context.logicalScale);
}

void IconNumericValuePreviewButton::commitControlScale(
    const adqt::widgets::AdControlScaleContext& context) {
    adqt::widgets::AdButton::commitControlScale(context);
    setPhysicalScale(context.logicalScale);
}
