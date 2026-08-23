#include "snow_shot/presentation/screenshottoolbarmainpanel.h"

#include "screenshottoolpalettebuttons.h"
#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include "antd_icons.h"

#include <QBoxLayout>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPixmap>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QSet>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;

constexpr int kButtonSize = 32;
constexpr int kIconSize = 24;
constexpr int kPanelHorizontalMargin = 12;
constexpr int kPanelMarginTop = 4;
constexpr int kPanelMarginBottom = 4;
constexpr int kDragHandleWidth = 18;
constexpr int kDragHandleIconSize = 18;
constexpr int kDragHandleTrailingSpacing = 4;
constexpr int kSeparatorHeight = 16;
constexpr int kSeparatorWidth = 1;
constexpr int kSeparatorSideSpacing = 12;
constexpr int kPanelRadius = 8;
constexpr qreal kShadowBlurRadius = 18.0;
constexpr qreal kShadowOffsetX = 0.0;
constexpr qreal kShadowOffsetY = 3.0;
constexpr QColor kShadowColor(0, 0, 0, 90);

QColor toolbarSurfaceColor() {
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    return scheme.map.colorBgContainer.isValid() ? scheme.map.colorBgContainer : QColor(Qt::white);
}

QColor toolbarSeparatorColor() {
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    return scheme.map.colorBorder.isValid() ? scheme.map.colorBorder : QColor(0xd9, 0xd9, 0xd9);
}

QColor toolbarDragHandleColor() {
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    return scheme.map.colorTextQuaternary.isValid() ? scheme.map.colorTextQuaternary
                                                    : QColor(0xbf, 0xbf, 0xbf);
}

QString cssColor(const QColor& color) {
    if (color.alpha() == 255) {
        return color.name(QColor::HexRgb);
    }

    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

int scaledMetric(int value, qreal scale) {
    return value <= 0 ? 0 : qMax(1, qRound(static_cast<qreal>(value) * scale));
}

ScreenshotToolPaletteButtonMetrics buttonMetrics(qreal scale) {
    return ScreenshotToolPaletteButtonMetrics{
        kButtonSize,
        kIconSize,
        scale,
    };
}
} // namespace

ScreenshotToolbarMainPanel::ScreenshotToolbarMainPanel(const Options& options, QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("screenshotToolbarMainPanel"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(this);
    m_layout = layout;

    if (options.showDragHandle) {
        auto* handle = new QLabel(this);
        handle->setObjectName(QStringLiteral("screenshotToolbarDragHandle"));
        configureScreenshotToolPaletteTooltip(handle, "Drag toolbar");
        handle->setFocusPolicy(Qt::NoFocus);
        handle->setCursor(Qt::SizeAllCursor);
        handle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        handle->setAttribute(Qt::WA_Hover, true);
        handle->setAlignment(Qt::AlignCenter);
        m_dragHandle = handle;
        layout->addWidget(handle);
        addSpacing(kDragHandleTrailingSpacing);
    }

    applyMetrics();

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            [this](const snow_shot::presentation::styles::ThemeColorScheme&) {
                updatePanelStyle();
                updateDragHandle(m_dragHandle);
                updateDragHandle(m_trailingDragHandle);
            });
}

QBoxLayout* ScreenshotToolbarMainPanel::contentLayout() const {
    return m_layout;
}

QWidget* ScreenshotToolbarMainPanel::dragHandle() const {
    return m_dragHandle;
}

QWidget* ScreenshotToolbarMainPanel::trailingDragHandle() const {
    return m_trailingDragHandle;
}

int ScreenshotToolbarMainPanel::buttonSize() const {
    return scaledMetric(kButtonSize, m_physicalScale);
}

qreal ScreenshotToolbarMainPanel::physicalScale() const {
    return m_physicalScale;
}

QMargins ScreenshotToolbarMainPanel::shadowMargins() {
    return QMargins(24, 24, 24, 28);
}

adqt::widgets::AdButton*
ScreenshotToolbarMainPanel::createToolButton(const char* tooltip,
                                             const adqt::icons::IconRef& iconRef) {
    auto* button = createScreenshotToolPaletteToolButton(this, tooltip, iconRef,
                                                         buttonMetrics(m_physicalScale));
    m_buttons.push_back(button);
    return button;
}

adqt::widgets::AdButton* ScreenshotToolbarMainPanel::createActionButton(
    const char* tooltip, const adqt::icons::IconRef& iconRef, bool danger, bool primary) {
    auto* button = createScreenshotToolPaletteActionButton(this, tooltip, iconRef, danger, primary,
                                                           buttonMetrics(m_physicalScale));
    m_buttons.push_back(button);
    return button;
}

void ScreenshotToolbarMainPanel::addSpacing(int baseSpacing) {
    if (m_layout == nullptr) {
        return;
    }

    auto* spacer = new QSpacerItem(scaledMetric(baseSpacing, m_physicalScale), 0,
                                   QSizePolicy::Fixed, QSizePolicy::Minimum);
    m_layout->addSpacerItem(spacer);
    m_spacingItems.push_back(SpacingItem{spacer, baseSpacing});
}

void ScreenshotToolbarMainPanel::addSeparator() {
    if (m_layout == nullptr) {
        return;
    }

    addSpacing(kSeparatorSideSpacing);
    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::NoFrame);
    separator->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    updateSeparatorStyle(separator);
    m_layout->addWidget(separator);
    m_separatorFrames.push_back(separator);
    addSpacing(kSeparatorSideSpacing);
    applyMetrics();
}

void ScreenshotToolbarMainPanel::resetContentLayout() {
    if (m_layout == nullptr) {
        return;
    }

    const QSet<QFrame*> separators(m_separatorFrames.cbegin(), m_separatorFrames.cend());
    while (QLayoutItem* item = m_layout->takeAt(0)) {
        QWidget* widget = item->widget();
        if (widget != nullptr) {
            widget->hide();
            if (QFrame* separator = qobject_cast<QFrame*>(widget);
                separator != nullptr && separators.contains(separator)) {
                delete separator;
            }
        }
        delete item;
    }
    m_separatorFrames.clear();
    m_spacingItems.clear();

    if (m_dragHandle != nullptr) {
        m_dragHandle->show();
        m_layout->addWidget(m_dragHandle);
        addSpacing(kDragHandleTrailingSpacing);
    }
    applyMetrics();
}

void ScreenshotToolbarMainPanel::addTrailingDragHandle() {
    if (m_layout == nullptr) {
        return;
    }

    if (m_trailingDragHandle != nullptr) {
        if (m_layout->indexOf(m_trailingDragHandle) < 0) {
            m_layout->addWidget(m_trailingDragHandle);
        }
        m_trailingDragHandle->show();
        return;
    }

    auto* handle = new QLabel(this);
    handle->setObjectName(QStringLiteral("screenshotToolbarTrailingDragHandle"));
    configureScreenshotToolPaletteTooltip(handle, "Drag toolbar");
    handle->setFocusPolicy(Qt::NoFocus);
    handle->setCursor(Qt::SizeAllCursor);
    handle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    handle->setAttribute(Qt::WA_Hover, true);
    handle->setAlignment(Qt::AlignCenter);
    m_trailingDragHandle = handle;
    m_layout->addWidget(handle);
    updateDragHandle(handle);
}

void ScreenshotToolbarMainPanel::setPhysicalScale(qreal scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    scale = std::clamp<qreal>(scale, 0.25, 4.0);
    if (qFuzzyCompare(m_physicalScale + 1.0, scale + 1.0)) {
        return;
    }

    m_physicalScale = scale;
    applyMetrics();
}

void ScreenshotToolbarMainPanel::changeEvent(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QFrame::changeEvent(event);
}

void ScreenshotToolbarMainPanel::retranslateUi() {
    retranslateScreenshotToolPalette(this);
}

void ScreenshotToolbarMainPanel::applyMetrics() {
    if (m_layout == nullptr) {
        return;
    }

    m_layout->setContentsMargins(scaledMetric(kPanelHorizontalMargin, m_physicalScale),
                                 scaledMetric(kPanelMarginTop, m_physicalScale),
                                 scaledMetric(kPanelHorizontalMargin, m_physicalScale),
                                 scaledMetric(kPanelMarginBottom, m_physicalScale));
    m_layout->setSpacing(0);
    m_layout->invalidate();

    const ScreenshotToolPaletteButtonMetrics metrics = buttonMetrics(m_physicalScale);
    for (adqt::widgets::AdButton* button : m_buttons) {
        configureScreenshotToolPaletteBaseButton(button, nullptr, metrics);
    }

    for (QFrame* separator : m_separatorFrames) {
        if (separator != nullptr) {
            separator->setFixedSize(scaledMetric(kSeparatorWidth, m_physicalScale),
                                    scaledMetric(kSeparatorHeight, m_physicalScale));
        }
    }

    for (const SpacingItem& item : std::as_const(m_spacingItems)) {
        if (item.item != nullptr) {
            item.item->changeSize(scaledMetric(item.baseSpacing, m_physicalScale), 0,
                                  QSizePolicy::Fixed, QSizePolicy::Minimum);
        }
    }

    updateDragHandle(m_dragHandle);
    updateDragHandle(m_trailingDragHandle);
    updatePanelStyle();
}

void ScreenshotToolbarMainPanel::updatePanelStyle() {
    for (QFrame* separator : std::as_const(m_separatorFrames)) {
        updateSeparatorStyle(separator);
    }

    auto* shadow = qobject_cast<QGraphicsDropShadowEffect*>(graphicsEffect());
    if (shadow == nullptr) {
        shadow = new QGraphicsDropShadowEffect(this);
        setGraphicsEffect(shadow);
    }
    shadow->setBlurRadius(kShadowBlurRadius * m_physicalScale);
    shadow->setOffset(kShadowOffsetX * m_physicalScale, kShadowOffsetY * m_physicalScale);
    shadow->setColor(kShadowColor);
}

void ScreenshotToolbarMainPanel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(toolbarSurfaceColor());
    const qreal radius = scaledMetric(kPanelRadius, m_physicalScale);
    painter.drawRoundedRect(QRectF(rect()), radius, radius);
}

void ScreenshotToolbarMainPanel::updateSeparatorStyle(QFrame* separator) {
    if (separator == nullptr) {
        return;
    }

    separator->setAttribute(Qt::WA_StyledBackground, true);
    separator->setStyleSheet(QStringLiteral("QFrame { background: %1; border: 0px; }")
                                 .arg(cssColor(toolbarSeparatorColor())));
}

void ScreenshotToolbarMainPanel::updateDragHandle(QWidget* handle) {
    if (handle == nullptr) {
        return;
    }

    handle->setFixedSize(scaledMetric(kDragHandleWidth, m_physicalScale), buttonSize());
    const QPixmap icon = snow_shot::presentation::icons::renderTintedIconPixmap(
        outlined_icons::Holder(),
        QSize(scaledMetric(kDragHandleIconSize, m_physicalScale),
              scaledMetric(kDragHandleIconSize, m_physicalScale)),
        handle->devicePixelRatioF(), toolbarDragHandleColor());
    if (!icon.isNull()) {
        if (auto* label = qobject_cast<QLabel*>(handle)) {
            label->setPixmap(icon);
        }
    }
}
