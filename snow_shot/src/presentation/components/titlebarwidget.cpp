#include "snow_shot/presentation/components/titlebarwidget.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "icon_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QWindow>

namespace {
namespace custom_icons = snow_shot::presentation::icons::custom;

#ifndef Q_OS_MACOS
constexpr int TRAFFIC_LIGHT_DIAMETER = 12;
constexpr int TRAFFIC_LIGHT_HIT_SIZE = 20;

constexpr QRgb CLOSE_COLOR = 0xffff5f57;
constexpr QRgb CLOSE_BORDER_COLOR = 0xffe0443e;
constexpr QRgb MINIMIZE_COLOR = 0xfffebc2e;
constexpr QRgb MINIMIZE_BORDER_COLOR = 0xffd89e24;
constexpr QRgb MAXIMIZE_COLOR = 0xff28c840;
constexpr QRgb MAXIMIZE_BORDER_COLOR = 0xff1aab29;
constexpr QRgb GLYPH_COLOR = 0x99000000;
#endif

QPixmap renderBrandLogo(int logicalHeight, const QColor& color) {
    if (logicalHeight <= 0 || !color.isValid()) {
        return {};
    }

    constexpr qreal aspectRatio = 95.0 / 17.0;
    const int logicalWidth =
        static_cast<int>(std::llround(static_cast<qreal>(logicalHeight) * aspectRatio));
    if (logicalWidth <= 0) {
        return {};
    }

    const qreal devicePixelRatio = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    adqt::icons::IconRenderRequest request;
    request.logicalSize = QSize(logicalWidth, logicalHeight);
    request.devicePixelRatio = devicePixelRatio;
    return adqt::icons::renderIconPixmap(
        custom_icons::brand::SnowShotLogo(adqt::icons::IconColors::primary(color)), request);
}

#ifndef Q_OS_MACOS
enum class WindowButtonKind : std::uint8_t {
    Close,
    Minimize,
    Maximize,
};

QColor windowControlColor(WindowButtonKind kind) {
    switch (kind) {
    case WindowButtonKind::Close:
        return QColor::fromRgba(CLOSE_COLOR);
    case WindowButtonKind::Minimize:
        return QColor::fromRgba(MINIMIZE_COLOR);
    case WindowButtonKind::Maximize:
        return QColor::fromRgba(MAXIMIZE_COLOR);
    default:
        return {};
    }
}

QColor windowControlBorderColor(WindowButtonKind kind) {
    switch (kind) {
    case WindowButtonKind::Close:
        return QColor::fromRgba(CLOSE_BORDER_COLOR);
    case WindowButtonKind::Minimize:
        return QColor::fromRgba(MINIMIZE_BORDER_COLOR);
    case WindowButtonKind::Maximize:
        return QColor::fromRgba(MAXIMIZE_BORDER_COLOR);
    default:
        return {};
    }
}

class WindowControlButton final : public QAbstractButton {
  public:
    explicit WindowControlButton(WindowButtonKind kind, QWidget* parent = nullptr)
        : QAbstractButton(parent), m_kind(kind) {
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(TRAFFIC_LIGHT_HIT_SIZE, TRAFFIC_LIGHT_HIT_SIZE);
        setCursor(Qt::ArrowCursor);
    }

  protected:
    void enterEvent(QEnterEvent* event) override {
        QAbstractButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }

    void paintEvent(QPaintEvent* event) override {
        (void)event;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QColor fill = windowControlColor(m_kind);
        QColor border = windowControlBorderColor(m_kind);
        if (isDown()) {
            fill = fill.darker(112);
            border = border.darker(112);
        }

        const QRectF circle((width() - TRAFFIC_LIGHT_DIAMETER) / 2.0,
                            (height() - TRAFFIC_LIGHT_DIAMETER) / 2.0, TRAFFIC_LIGHT_DIAMETER,
                            TRAFFIC_LIGHT_DIAMETER);
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(fill);
        painter.drawEllipse(circle);

        if (!underMouse()) {
            return;
        }

        painter.setPen(
            QPen(QColor::fromRgba(GLYPH_COLOR), 1.15, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF center = circle.center();
        switch (m_kind) {
        case WindowButtonKind::Close:
            painter.drawLine(center + QPointF(-2.0, -2.0), center + QPointF(2.0, 2.0));
            painter.drawLine(center + QPointF(2.0, -2.0), center + QPointF(-2.0, 2.0));
            break;
        case WindowButtonKind::Minimize:
            painter.drawLine(center + QPointF(-2.5, 0.0), center + QPointF(2.5, 0.0));
            break;
        case WindowButtonKind::Maximize: {
            QPainterPath glyph;
            glyph.moveTo(center.x() - 2.5, center.y() + 0.5);
            glyph.lineTo(center.x() - 0.5, center.y() + 2.5);
            glyph.lineTo(center.x() - 2.5, center.y() + 2.5);
            glyph.closeSubpath();
            glyph.moveTo(center.x() + 2.5, center.y() - 0.5);
            glyph.lineTo(center.x() + 0.5, center.y() - 2.5);
            glyph.lineTo(center.x() + 2.5, center.y() - 2.5);
            glyph.closeSubpath();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor::fromRgba(GLYPH_COLOR));
            painter.drawPath(glyph);
            break;
        }
        }
    }

  private:
    WindowButtonKind m_kind;
};
#endif
} // namespace

TitleBarWidget::TitleBarWidget(const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
                               QWidget* parent)
    : QFrame(parent), m_logoHeight(std::clamp(metric.fontSizeSM, 10, 14)) {
    setAutoFillBackground(true);
    setFixedHeight(metric.controlHeight);

#ifndef Q_OS_MACOS
    m_closeButton = new WindowControlButton(WindowButtonKind::Close, this);
    m_minimizeButton = new WindowControlButton(WindowButtonKind::Minimize, this);
    m_maximizeButton = new WindowControlButton(WindowButtonKind::Maximize, this);
    m_closeButton->setObjectName(QStringLiteral("closeWindowButton"));
    m_minimizeButton->setObjectName(QStringLiteral("minimizeWindowButton"));
    m_maximizeButton->setObjectName(QStringLiteral("maximizeWindowButton"));
#endif

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(metric.paddingXS, 0, metric.paddingXS, 0);
    layout->setSpacing(0);

    retranslateUi();
#ifndef Q_OS_MACOS
    layout->addWidget(m_closeButton);
    layout->addWidget(m_minimizeButton);
    layout->addWidget(m_maximizeButton);
#endif
    layout->addStretch();

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            &TitleBarWidget::applyTheme);
    applyTheme(themeManager.themeColorScheme());
}

void TitleBarWidget::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void TitleBarWidget::mousePressEvent(QMouseEvent* event) {
#ifdef Q_OS_MACOS
    if (event->button() == Qt::LeftButton) {
        QWidget* topLevelWindow = window();
        if (topLevelWindow != nullptr && topLevelWindow->windowHandle() != nullptr &&
            topLevelWindow->windowHandle()->startSystemMove()) {
            event->accept();
            return;
        }
    }
#endif
    QFrame::mousePressEvent(event);
}

void TitleBarWidget::retranslateUi() {
#ifndef Q_OS_MACOS
    m_closeButton->setToolTip(tr("Close"));
    m_closeButton->setAccessibleName(tr("Close"));
    m_minimizeButton->setToolTip(tr("Minimize"));
    m_minimizeButton->setAccessibleName(tr("Minimize"));
    const QString maximizeText = m_maximized ? tr("Restore") : tr("Maximize");
    m_maximizeButton->setToolTip(maximizeText);
    m_maximizeButton->setAccessibleName(maximizeText);
#endif
}

void TitleBarWidget::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QPixmap logoPixmap = renderBrandLogo(m_logoHeight, m_logoColor);
    if (!logoPixmap.isNull()) {
        const qreal devicePixelRatio =
            logoPixmap.devicePixelRatio() > 0.0 ? logoPixmap.devicePixelRatio() : 1.0;
        const int logoWidth = static_cast<int>(
            std::lround(static_cast<qreal>(logoPixmap.width()) / devicePixelRatio));
        const int logoHeight = static_cast<int>(
            std::lround(static_cast<qreal>(logoPixmap.height()) / devicePixelRatio));

        QWidget* topLevelWindow = window();
        const qreal windowCenterX = topLevelWindow != nullptr
                                        ? static_cast<qreal>(topLevelWindow->width()) / 2.0
                                        : static_cast<qreal>(width()) / 2.0;
        const qreal localCenterX =
            topLevelWindow != nullptr
                ? windowCenterX - static_cast<qreal>(mapTo(topLevelWindow, QPoint(0, 0)).x())
                : windowCenterX;

        painter.drawPixmap(
            QPointF(localCenterX - static_cast<qreal>(logoWidth) / 2.0,
                    (static_cast<qreal>(height()) - static_cast<qreal>(logoHeight)) / 2.0),
            logoPixmap);
    }
}

void TitleBarWidget::applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, scheme.map.colorBgContainer);
    setPalette(palette);
    m_logoColor = scheme.map.colorText;

    update();
}

void TitleBarWidget::setMaximized(bool maximized) {
    if (m_maximized == maximized) {
        return;
    }

    m_maximized = maximized;
    retranslateUi();
}

QAbstractButton* TitleBarWidget::minimizeButton() const {
    return m_minimizeButton;
}

QAbstractButton* TitleBarWidget::maximizeButton() const {
    return m_maximizeButton;
}

QAbstractButton* TitleBarWidget::closeButton() const {
    return m_closeButton;
}
