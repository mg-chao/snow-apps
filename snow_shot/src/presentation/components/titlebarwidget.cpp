#include "snow_shot/presentation/components/titlebarwidget.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "antd_icons.h"
#include "icon_renderer.h"
#include "widgets/button.h"

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
#include <QLabel>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <QPixmap>
#include <QWindow>

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;
namespace custom_icons = snow_shot::presentation::icons::custom;

QPixmap renderBrandLogo(int logicalHeight, const QColor& color, qreal devicePixelRatio) {
    if (logicalHeight <= 0 || !color.isValid()) {
        return {};
    }

    constexpr qreal aspectRatio = 95.0 / 17.0;
    const int logicalWidth =
        static_cast<int>(std::llround(static_cast<qreal>(logicalHeight) * aspectRatio));
    if (logicalWidth <= 0) {
        return {};
    }

    adqt::icons::IconRenderRequest request;
    request.logicalSize = QSize(logicalWidth, logicalHeight);
    request.devicePixelRatio = devicePixelRatio;
    return adqt::icons::renderIconPixmap(
        custom_icons::brand::SnowShotLogo(adqt::icons::IconColors::primary(color)), request);
}

#ifndef Q_OS_MACOS
enum class WindowButtonKind : std::uint8_t {
    Minimize,
    Maximize,
    Close,
};

#ifndef Q_OS_WIN
adqt::icons::IconRef windowControlIcon(WindowButtonKind kind) {
    switch (kind) {
    case WindowButtonKind::Minimize:
        return outlined_icons::Minus();
    case WindowButtonKind::Maximize:
        return outlined_icons::Border();
    case WindowButtonKind::Close:
        return outlined_icons::Close();
    default:
        return {};
    }
}

class WindowControlButton final : public adqt::widgets::AdButton {
  public:
    WindowControlButton(WindowButtonKind kind,
                        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
                        QWidget* parent = nullptr)
        : adqt::widgets::AdButton(parent), m_kind(kind) {
        setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        setSizeClass(adqt::widgets::AdButton::SizeClass::Small);
        setIconRef(windowControlIcon(m_kind));
        setIconSize(QSize(metric.fontSize, metric.fontSize));
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(metric.controlHeightSM, metric.controlHeightSM);
        syncAccentRole();
    }

  protected:
    void enterEvent(QEnterEvent* event) override {
        adqt::widgets::AdButton::enterEvent(event);
        syncAccentRole();
    }

    void leaveEvent(QEvent* event) override {
        adqt::widgets::AdButton::leaveEvent(event);
        syncAccentRole();
    }

    void mousePressEvent(QMouseEvent* event) override {
        adqt::widgets::AdButton::mousePressEvent(event);
        syncAccentRole();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        adqt::widgets::AdButton::mouseReleaseEvent(event);
        syncAccentRole();
    }

  private:
    void syncAccentRole() {
        const bool closeDangerState =
            m_kind == WindowButtonKind::Close && (underMouse() || isDown());
        setAccentRole(closeDangerState ? adqt::widgets::AdButton::AccentRole::Danger
                                       : adqt::widgets::AdButton::AccentRole::Neutral);
    }

    WindowButtonKind m_kind;
};

#else
// Windows caption geometry is independent of the app's content density.
constexpr int CAPTION_HEIGHT = 32;
constexpr int CAPTION_BUTTON_WIDTH = 46;
constexpr int CAPTION_ICON_SIZE = 10;

class WindowControlButton final : public QAbstractButton {
  public:
    WindowControlButton(WindowButtonKind kind,
                        const snow_shot::presentation::styles::ThemeAliasMetricToken&,
                        QWidget* parent = nullptr)
        : QAbstractButton(parent), m_kind(kind) {
        setFixedSize(CAPTION_BUTTON_WIDTH, CAPTION_HEIGHT);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        if (kind == WindowButtonKind::Maximize) {
            setProperty("snowWindowCaptionHit", HTMAXBUTTON);
        }
    }

    void setMaximized(bool maximized) {
        m_maximized = maximized;
        update();
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

    void paintEvent(QPaintEvent*) override {
        const auto& scheme =
            snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme();
        const bool hovered = underMouse() || property("snowNativeCaptionHover").toBool();
        const bool active = window()->isActiveWindow();
        const bool dark = scheme.map.colorBgContainer.lightness() < 128;
        QColor foreground = active ? scheme.map.colorText : scheme.map.colorTextTertiary;
        QPainter painter(this);
        if (isDown() || hovered) {
            if (m_kind == WindowButtonKind::Close) {
                painter.fillRect(rect(), isDown() ? QColor(196, 43, 28) : QColor(232, 17, 35));
                foreground = Qt::white;
            } else {
                painter.fillRect(rect(), QColor(dark ? 255 : 0, dark ? 255 : 0, dark ? 255 : 0,
                                                isDown() ? 20 : 30));
            }
        }
        // Work in physical pixels: a 1 DIP SVG stroke becomes a soft 1.25/1.5 px
        // edge at fractional scaling. Snap both the glyph and its stroke width.
        const qreal scale = devicePixelRatioF();
        const int extent = qRound(CAPTION_ICON_SIZE * scale);
        const int stroke = std::max(1, qRound(scale));
        const auto deviceTransform = painter.deviceTransform();
        const QPointF origin(qRound((width() * scale - extent) / 2.0 + deviceTransform.dx()) -
                                 deviceTransform.dx(),
                             qRound((height() * scale - extent) / 2.0 + deviceTransform.dy()) -
                                 deviceTransform.dy());
        painter.scale(1.0 / scale, 1.0 / scale);
        painter.translate(origin);
        painter.setPen(QPen(foreground, stroke, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        const qreal inset = stroke / 2.0;
        const QRectF box(inset, inset, extent - stroke, extent - stroke);
        switch (m_kind) {
        case WindowButtonKind::Minimize:
            painter.fillRect(QRectF(0, (extent - stroke) / 2, extent, stroke), foreground);
            break;
        case WindowButtonKind::Maximize:
            painter.setRenderHint(QPainter::Antialiasing);
            if (m_maximized) {
                const int offset = std::max(stroke + 1, qRound(2 * scale));
                QPainterPath back;
                back.moveTo(offset + inset, offset - stroke);
                back.lineTo(offset + inset, inset + 1);
                back.quadTo(offset + inset, inset, offset + inset + 1, inset);
                back.lineTo(extent - inset - 1, inset);
                back.quadTo(extent - inset, inset, extent - inset, inset + 1);
                back.lineTo(extent - inset, extent - offset - inset - 1);
                back.quadTo(extent - inset, extent - offset - inset, extent - inset - 1,
                            extent - offset - inset);
                back.lineTo(extent - offset + stroke, extent - offset - inset);
                painter.drawPath(back);
                painter.drawRoundedRect(QRectF(inset, offset + inset, extent - offset - stroke,
                                               extent - offset - stroke),
                                        1, 1);
            } else {
                painter.drawRoundedRect(box, 1, 1);
            }
            break;
        case WindowButtonKind::Close:
            painter.setRenderHint(QPainter::Antialiasing);
            painter.drawLine(box.topLeft(), box.bottomRight());
            painter.drawLine(box.topRight(), box.bottomLeft());
            break;
        }
    }

  private:
    WindowButtonKind m_kind;
    bool m_maximized = false;
};
#endif

void refreshWindowControlButtonTheme(QAbstractButton* button) {
    if (button != nullptr) {
        button->update();
    }
}
#endif
} // namespace

TitleBarWidget::TitleBarWidget(const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
                               QWidget* parent)
    : QFrame(parent), m_logoHeight(std::clamp(metric.fontSizeSM, 10, 14)) {
    setAutoFillBackground(true);
#ifdef Q_OS_WIN
    setFixedHeight(CAPTION_HEIGHT);
    m_applicationIcon = new QLabel(this);
    m_applicationIcon->setObjectName(QStringLiteral("windowSystemMenuIcon"));
    m_applicationIcon->setGeometry(16, 8, 16, 16);
    m_applicationIcon->setProperty("snowWindowCaptionHit", HTSYSMENU);
    window()->installEventFilter(this);
#else
    setFixedHeight(metric.controlHeight);
#endif

#ifndef Q_OS_MACOS
    m_minimizeButton = new WindowControlButton(WindowButtonKind::Minimize, metric, this);
    m_maximizeButton = new WindowControlButton(WindowButtonKind::Maximize, metric, this);
    m_closeButton = new WindowControlButton(WindowButtonKind::Close, metric, this);
    m_minimizeButton->setObjectName(QStringLiteral("minimizeWindowButton"));
    m_maximizeButton->setObjectName(QStringLiteral("maximizeWindowButton"));
    m_closeButton->setObjectName(QStringLiteral("closeWindowButton"));
#endif

    auto* layout = new QHBoxLayout(this);
#ifdef Q_OS_WIN
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->setDirection(QBoxLayout::LeftToRight);
#else
    layout->setContentsMargins(0, 0, metric.paddingSM + metric.borderRadiusXS, 0);
    layout->setSpacing(metric.marginXXS);
#endif

    retranslateUi();
    layout->addStretch();
#ifndef Q_OS_MACOS
    layout->addWidget(m_minimizeButton);
    layout->addWidget(m_maximizeButton);
    layout->addWidget(m_closeButton);
#endif

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            &TitleBarWidget::applyTheme);
    applyTheme(themeManager.themeColorScheme());
}

bool TitleBarWidget::event(QEvent* event) {
    const bool handled = QFrame::event(event);
#ifdef Q_OS_WIN
    if (event->type() == QEvent::DevicePixelRatioChange && m_applicationIcon != nullptr) {
        applyTheme(snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme());
    }
#endif
    return handled;
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

#ifdef Q_OS_WIN
    const QColor color = window()->isActiveWindow()
                             ? m_logoColor
                             : snow_shot::presentation::styles::ThemeManager::instance()
                                   .themeColorScheme()
                                   .map.colorTextTertiary;
    const QPixmap wordmark = renderBrandLogo(m_logoHeight, color, devicePixelRatioF());
    const qreal scale = devicePixelRatioF();
    const qreal y = qRound((height() * scale - wordmark.height()) / 2.0) / scale;
    painter.setClipRect(QRect(48, 0, std::max(0, m_minimizeButton->x() - 64), height()));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawPixmap(QPointF(48, y), wordmark);
#else
    const QPixmap logoPixmap = renderBrandLogo(m_logoHeight, m_logoColor, devicePixelRatioF());
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
#endif
}

bool TitleBarWidget::eventFilter(QObject* watched, QEvent* event) {
#ifdef Q_OS_WIN
    if (watched == window() && event->type() == QEvent::ActivationChange) {
        applyTheme(snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme());
    }
#endif
    return QFrame::eventFilter(watched, event);
}

void TitleBarWidget::applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, scheme.map.colorBgContainer);
    setPalette(palette);
    m_logoColor = scheme.map.colorText;
#ifdef Q_OS_WIN
    adqt::icons::IconRenderRequest request;
    request.logicalSize = QSize(16, 16);
    request.devicePixelRatio = devicePixelRatioF();
    QPixmap icon = adqt::icons::renderIconPixmap(custom_icons::app::ApplicationIcon(), request);
    if (!window()->isActiveWindow()) {
        QPainter painter(&icon);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.fillRect(icon.rect(), QColor(0, 0, 0, 128));
    }
    m_applicationIcon->setPixmap(icon);
#endif

#ifndef Q_OS_MACOS
    refreshWindowControlButtonTheme(m_minimizeButton);
    refreshWindowControlButtonTheme(m_maximizeButton);
    refreshWindowControlButtonTheme(m_closeButton);
#endif

    update();
}

void TitleBarWidget::setMaximized(bool maximized) {
    if (m_maximized == maximized) {
        return;
    }

    m_maximized = maximized;
#ifdef Q_OS_WIN
    static_cast<WindowControlButton*>(m_maximizeButton)->setMaximized(maximized);
#endif
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
