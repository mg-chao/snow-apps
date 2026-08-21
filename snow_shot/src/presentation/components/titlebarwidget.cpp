#include "snow_shot/presentation/components/titlebarwidget.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "antd_icons.h"
#include "icon_registry.h"
#include "widgets/button.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <QColor>
#include <QHBoxLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

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

enum class WindowButtonKind : std::uint8_t {
    Minimize,
    Close,
};

adqt::icons::IconRef windowControlIcon(WindowButtonKind kind) {
    switch (kind) {
    case WindowButtonKind::Minimize:
        return outlined_icons::Minus();
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

void refreshWindowControlButtonTheme(QAbstractButton* button) {
    if (button != nullptr) {
        button->update();
    }
}
} // namespace

TitleBarWidget::TitleBarWidget(const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
                               QWidget* parent)
    : QFrame(parent),
      m_minimizeButton(new WindowControlButton(WindowButtonKind::Minimize, metric, this)),
      m_closeButton(new WindowControlButton(WindowButtonKind::Close, metric, this)),
      m_logoHeight(std::clamp(metric.fontSizeSM, 10, 14)) {
    setAutoFillBackground(true);
    setFixedHeight(metric.controlHeight);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, metric.paddingSM + metric.borderRadiusXS, 0);
    layout->setSpacing(metric.marginXXS);
    layout->addStretch();

    retranslateUi();
    layout->addWidget(m_minimizeButton);

    layout->addWidget(m_closeButton);

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

void TitleBarWidget::retranslateUi() {
    m_minimizeButton->setToolTip(tr("Minimize"));
    m_minimizeButton->setAccessibleName(tr("Minimize"));
    m_closeButton->setToolTip(tr("Close"));
    m_closeButton->setAccessibleName(tr("Close"));
}

void TitleBarWidget::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qreal devicePixelRatio = devicePixelRatioF();
    if (m_logoPixmap.isNull() ||
        !qFuzzyCompare(m_logoPixmap.devicePixelRatio(), devicePixelRatio)) {
        m_logoPixmap = renderBrandLogo(m_logoHeight, m_logoColor, devicePixelRatio);
    }
    if (!m_logoPixmap.isNull()) {
        const qreal pixmapDevicePixelRatio =
            m_logoPixmap.devicePixelRatio() > 0.0 ? m_logoPixmap.devicePixelRatio() : 1.0;
        const int logoWidth = static_cast<int>(
            std::lround(static_cast<qreal>(m_logoPixmap.width()) / pixmapDevicePixelRatio));
        const int logoHeight = static_cast<int>(
            std::lround(static_cast<qreal>(m_logoPixmap.height()) / pixmapDevicePixelRatio));

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
            m_logoPixmap);
    }
}

void TitleBarWidget::applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, scheme.map.colorBgContainer);
    setPalette(palette);
    if (m_logoColor != scheme.map.colorText) {
        m_logoColor = scheme.map.colorText;
        m_logoPixmap = {};
    }

    refreshWindowControlButtonTheme(m_minimizeButton);
    refreshWindowControlButtonTheme(m_closeButton);

    update();
}

QAbstractButton* TitleBarWidget::minimizeButton() const {
    return m_minimizeButton;
}

QAbstractButton* TitleBarWidget::closeButton() const {
    return m_closeButton;
}
