#include "snow_shot/presentation/components/shortcutconfigurationbutton.h"

#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/components/infotooltipicon.h"

#include "theme/theme.h"
#include "widgets/button_style.h"
#include "widgets/detail/button_rendering.h"

#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace {
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;

adqt::widgets::AdButton::AccentRole
shortcutAccentRole(snow_shot::presentation::GlobalShortcutStatus status) {
    switch (status) {
    case snow_shot::presentation::GlobalShortcutStatus::Registered:
        return adqt::widgets::AdButton::AccentRole::Green;
    case snow_shot::presentation::GlobalShortcutStatus::PartiallyRegistered:
        return adqt::widgets::AdButton::AccentRole::Orange;
    case snow_shot::presentation::GlobalShortcutStatus::Failed:
        return adqt::widgets::AdButton::AccentRole::Danger;
    case snow_shot::presentation::GlobalShortcutStatus::Unset:
        return adqt::widgets::AdButton::AccentRole::Neutral;
    }
    return adqt::widgets::AdButton::AccentRole::Danger;
}

adqt::widgets::detail::ButtonVisualStyle
resolveConfigurationButtonStyle(const ShortcutConfigurationButton& button) {
    adqt::widgets::detail::ButtonStyleInput input;
    input.buttonStyle = button.buttonStyle();
    input.accentRole = button.accentRole();
    input.sizeClass = button.sizeClass();
    input.flat = button.isFlat();
    input.defaultButton = button.isDefault();
    input.hasMenu = button.menu() != nullptr;
    input.baseFont = button.font();
    return adqt::widgets::detail::resolveButtonVisualStyle(
        input, adqt::theme::ThemeManager::instance().resolve(&button));
}

const adqt::widgets::detail::ButtonStateStyle&
resolvedState(const ShortcutConfigurationButton& button,
              const adqt::widgets::detail::ButtonVisualStyle& style, bool hovered) {
    if (!button.isEnabled()) {
        return style.disabled;
    }
    if (button.isDown()) {
        return style.active;
    }
    if (button.isChecked()) {
        return style.checked;
    }
    return hovered ? style.hover : style.normal;
}
} // namespace

ShortcutConfigurationButton::ShortcutConfigurationButton(
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric, int textMaxWidth,
    QWidget* parent)
    : ShortcutConfigurationButton(metric, textMaxWidth, custom_outlined_icons::Keyboard(), parent) {
}

ShortcutConfigurationButton::ShortcutConfigurationButton(
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric, int textMaxWidth,
    const adqt::icons::IconRef& contentIcon, QWidget* parent)
    : adqt::widgets::AdButton(parent), m_iconTextSpacing(metric.marginXS),
      m_textMaxWidth(std::max(0, textMaxWidth)) {
    setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Dashed);
    setAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
    setShape(adqt::widgets::AdButton::Shape::Rounded);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setMinimumWidth(0);
    setIconRef(contentIcon);

    QFont buttonFont = font();
    buttonFont.setPixelSize(metric.fontSize);
    buttonFont.setWeight(QFont::Normal);
    setFont(buttonFont);

    m_statusTooltipTrigger = new InfoTooltipIcon(metric.fontSize, this);
    m_statusTooltipTrigger->setObjectName(
        QStringLiteral("shortcutRegistrationStatusTooltipTrigger"));
    m_statusTooltipTrigger->setProperty("inlineGap", m_iconTextSpacing);
    m_statusTooltipTrigger->hide();
}

QSize ShortcutConfigurationButton::sizeHint() const {
    const auto style = resolveConfigurationButtonStyle(*this);
    const QFontMetricsF fontMetrics(style.metrics.font);
    const int textWidth = static_cast<int>(std::ceil(fontMetrics.horizontalAdvance(text())));
    const int cappedTextWidth = std::min(textWidth, m_textMaxWidth);
    const int iconSize = std::max(10, style.metrics.font.pixelSize());
    const int iconGap = text().isEmpty()
                            ? 0
                            : (m_status == snow_shot::presentation::GlobalShortcutStatus::Unset
                                   ? style.metrics.iconGap * 2
                                   : style.metrics.iconGap);
    const int horizontalFrameWidth =
        (style.metrics.horizontalPadding + style.metrics.borderWidth) * 2;
    return QSize(horizontalFrameWidth + iconSize + iconGap + cappedTextWidth +
                     statusTooltipReservationWidth(),
                 style.metrics.height);
}

QSize ShortcutConfigurationButton::minimumSizeHint() const {
    return QSize(0, resolveConfigurationButtonStyle(*this).metrics.height);
}

void ShortcutConfigurationButton::setRegistrationStatus(
    snow_shot::presentation::GlobalShortcutStatus status) {
    m_status = status;
    setProperty("registrationStatus", static_cast<int>(status));
    setAccentRole(shortcutAccentRole(status));
    syncStatusTooltipTrigger();
    update();
}

void ShortcutConfigurationButton::setRegistrationStatusTooltipVisible(bool visible) {
    if (m_statusTooltipVisible == visible) {
        return;
    }
    m_statusTooltipVisible = visible;
    syncStatusTooltipTrigger();
    updateGeometry();
    update();
}

InfoTooltipIcon* ShortcutConfigurationButton::registrationStatusTooltipTrigger() const {
    return m_statusTooltipTrigger;
}

void ShortcutConfigurationButton::setTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    m_colorScheme = scheme;
    syncStatusTooltipTrigger();
    update();
}

bool ShortcutConfigurationButton::event(QEvent* event) {
    const bool handled = adqt::widgets::AdButton::event(event);
    if (event->type() == QEvent::Enter) {
        m_hovered = true;
    } else if (event->type() == QEvent::Leave) {
        m_hovered = false;
    }
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
        event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        syncStatusTooltipTrigger();
        update();
    }
    return handled;
}

void ShortcutConfigurationButton::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    const auto style = resolveConfigurationButtonStyle(*this);
    const auto& state = resolvedState(*this, style, m_hovered);
    const auto& metrics = style.metrics;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF rawBorderRect =
        adqt::widgets::detail::joinedButtonBorderRect(rect(), metrics.borderWidth, false, false);
    const QRectF shapeRect = adqt::widgets::detail::resolveButtonShapeRect(rawBorderRect, shape());
    const auto corners = adqt::widgets::detail::resolveButtonCorners(
        shape(), shapeRect, metrics.borderRadius, false, false);
    const QPainterPath buttonPath = adqt::widgets::detail::roundedButtonPath(
        shapeRect, corners.topLeft, corners.topRight, corners.bottomRight, corners.bottomLeft);
    painter.fillPath(buttonPath, state.background);

    if (metrics.borderWidth > 0 && state.border.alpha() > 0) {
        const bool dashed = buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Dashed ||
                            buttonStyle() == adqt::widgets::AdButton::ButtonStyle::GhostDashed;
        QPen pen = adqt::widgets::detail::makeButtonBorderPen(
            state.border, metrics.borderWidth, dashed ? Qt::DashLine : state.borderStyle);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(buttonPath);
    }

    const int contentInset = metrics.horizontalPadding + metrics.borderWidth;
    const QRect contentRect =
        rect().adjusted(contentInset, metrics.borderWidth, -contentInset, -metrics.borderWidth);
    const int iconSize = std::max(10, metrics.font.pixelSize());
    const bool hasStatusTrigger = m_statusTooltipVisible && m_statusTooltipTrigger != nullptr;
    const int statusTriggerWidth = hasStatusTrigger ? m_statusTooltipTrigger->width() : 0;
    const int iconTextGap = text().isEmpty()
                                ? 0
                                : (m_status == snow_shot::presentation::GlobalShortcutStatus::Unset
                                       ? metrics.iconGap * 2
                                       : metrics.iconGap);
    const int statusGap = hasStatusTrigger && !text().isEmpty() ? metrics.iconGap : 0;
    const int availableTextWidth =
        std::max(0, contentRect.width() - iconSize - iconTextGap - statusGap - statusTriggerWidth);
    painter.setFont(metrics.font);
    const QFontMetrics fontMetrics(metrics.font);
    const QString displayText =
        text().isEmpty() ? QString()
                         : fontMetrics.elidedText(text(), Qt::ElideRight, availableTextWidth);
    const int textWidth = displayText.isEmpty() ? 0 : fontMetrics.horizontalAdvance(displayText);
    const int displayIconGap = displayText.isEmpty() ? 0 : iconTextGap;
    const int contentWidth = iconSize + displayIconGap + textWidth + statusGap + statusTriggerWidth;
    const int startX = contentRect.left() + std::max(0, (contentRect.width() - contentWidth) / 2);

    QColor iconColor = state.text;
    if (!m_hovered && !isDown()) {
        iconColor.setAlpha(
            static_cast<int>(std::lround(static_cast<double>(iconColor.alpha()) * 0.42)));
    }
    const QPixmap icon = snow_shot::presentation::icons::renderTintedIconPixmap(
        iconRef(), QSize(iconSize, iconSize), devicePixelRatioF(), iconColor);
    if (!icon.isNull()) {
        painter.drawPixmap(startX, (height() - iconSize) / 2, icon);
    }

    const int textX = startX + iconSize + displayIconGap;
    if (!displayText.isEmpty()) {
        painter.setPen(m_status == snow_shot::presentation::GlobalShortcutStatus::Unset
                           ? m_colorScheme.map.colorTextTertiary
                           : state.text);
        painter.drawText(QRect(textX, contentRect.top(), textWidth, contentRect.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, displayText);
    }
    if (hasStatusTrigger) {
        m_statusTooltipTrigger->setGeometry(
            textX + textWidth + statusGap, (height() - m_statusTooltipTrigger->height()) / 2,
            m_statusTooltipTrigger->width(), m_statusTooltipTrigger->height());
        m_statusTooltipTrigger->raise();
    }
}

int ShortcutConfigurationButton::statusTooltipReservationWidth() const {
    return m_statusTooltipVisible && m_statusTooltipTrigger != nullptr
               ? m_iconTextSpacing + m_statusTooltipTrigger->width()
               : 0;
}

void ShortcutConfigurationButton::syncStatusTooltipTrigger() {
    if (m_statusTooltipTrigger == nullptr) {
        return;
    }
    m_statusTooltipTrigger->setVisible(m_statusTooltipVisible);
    if (m_statusTooltipVisible) {
        m_statusTooltipTrigger->setIconColor(
            resolvedState(*this, resolveConfigurationButtonStyle(*this), m_hovered).text);
    }
}
