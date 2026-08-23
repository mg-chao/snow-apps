#include "screenshotselectiontoolbarwidgets.h"

#include "snow_shot/presentation/components/icons/iconrenderutils.h"

#include <QEnterEvent>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>

namespace {
constexpr int PanelSeparatorWidth = 1;
constexpr int PanelSeparatorHeight = 12;
constexpr int IconTextSpacing = 4;
constexpr int ValueHorizontalInset = 2;
constexpr int LockIconHorizontalMargin = 4;
constexpr int SeparatorLeftMargin = 8;
constexpr int SeparatorRightMargin = 8;
constexpr int ShadowHoverBlurRadius = 4;
constexpr int ShadowHoverAlpha = 84;
constexpr double ShadowHoverFalloffPower = 2.4;
const QColor PanelBackground(0, 0, 0, 115);
const QColor PanelTextColor(255, 255, 255);
const QColor PanelHoverColor(22, 119, 255, 107);
const QColor PanelSeparatorColor(217, 217, 217, 107);
const QColor PanelShadowColor(64, 150, 255);
const QColor PanelPrimaryColor(22, 119, 255);
} // namespace

QColor screenshot_selection_toolbar::panelTextColor() {
    return PanelTextColor;
}

QColor screenshot_selection_toolbar::panelPrimaryColor() {
    return PanelPrimaryColor;
}

void screenshot_selection_toolbar::paintToolbarShadow(QPainter* painter, const QRectF& panelRect,
                                                      bool hovered) {
    if (painter == nullptr || panelRect.isEmpty() || !hovered) {
        return;
    }

    painter->setBrush(Qt::NoBrush);

    for (int step = ShadowHoverBlurRadius; step >= 1; --step) {
        const qreal progress = static_cast<qreal>(ShadowHoverBlurRadius - step + 1) /
                               static_cast<qreal>(ShadowHoverBlurRadius);
        QColor color = PanelShadowColor;
        color.setAlpha(
            std::clamp(static_cast<int>(std::lround(
                           static_cast<double>(ShadowHoverAlpha) *
                           std::pow(static_cast<double>(progress), ShadowHoverFalloffPower))),
                       0, 255));

        painter->setPen(QPen(color, 1.0));
        const qreal outset = static_cast<qreal>(step);
        const QRectF shadowRect = panelRect.adjusted(-outset, -outset, outset, outset);
        painter->drawRoundedRect(shadowRect, screenshot_selection_toolbar::PanelRadius + outset,
                                 screenshot_selection_toolbar::PanelRadius + outset);
    }
}

QPixmap screenshot_selection_toolbar::renderToolbarIcon(QWidget* widget,
                                                        const adqt::icons::IconRef& iconRef,
                                                        QColor color) {
    if (widget == nullptr) {
        return QPixmap();
    }

    if (!color.isValid()) {
        color = PanelTextColor;
    }

    return snow_shot::presentation::icons::renderTintedIconPixmap(
        iconRef, QSize(IconSize, IconSize), widget->devicePixelRatioF(), color);
}

SelectionToolbarPanel::SelectionToolbarPanel(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("screenshotSelectionToolbarPanel"));
    setAttribute(Qt::WA_StyledBackground, false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
}

void SelectionToolbarPanel::enterEvent(QEnterEvent* event) {
    emit hoverChanged(true);
    QFrame::enterEvent(event);
}

void SelectionToolbarPanel::leaveEvent(QEvent* event) {
    emit hoverChanged(false);
    QFrame::leaveEvent(event);
}

void SelectionToolbarPanel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(PanelBackground);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            screenshot_selection_toolbar::PanelRadius,
                            screenshot_selection_toolbar::PanelRadius);
}

SelectionToolbarValueLabel::SelectionToolbarValueLabel(QWidget* parent) : QLabel(parent) {
    setAlignment(Qt::AlignCenter);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedHeight(screenshot_selection_toolbar::PanelHeight -
                   screenshot_selection_toolbar::PanelVerticalPadding * 2);
}

void SelectionToolbarValueLabel::setLeadingIcon(const QPixmap& icon) {
    m_leadingIcon = icon;
    m_iconOnly = false;
    updateGeometry();
    update();
}

void SelectionToolbarValueLabel::setIconOnlyPixmap(const QPixmap& icon) {
    m_leadingIcon = icon;
    m_iconOnly = true;
    updateGeometry();
    update();
}

void SelectionToolbarValueLabel::setLockAspectRatioControl(bool enabled) {
    if (m_lockAspectRatioControl == enabled) {
        return;
    }
    m_lockAspectRatioControl = enabled;
    update();
}

void SelectionToolbarValueLabel::setPointerInteractionEnabled(bool enabled) {
    setAttribute(Qt::WA_TransparentForMouseEvents, !enabled);
    if (!enabled && m_hovered) {
        m_hovered = false;
        update();
    }
}

void SelectionToolbarValueLabel::enterEvent(QEnterEvent* event) {
    if (!testAttribute(Qt::WA_TransparentForMouseEvents) && !m_hovered) {
        m_hovered = true;
        update();
    }
    QLabel::enterEvent(event);
}

void SelectionToolbarValueLabel::leaveEvent(QEvent* event) {
    if (m_hovered) {
        m_hovered = false;
        update();
    }
    QLabel::leaveEvent(event);
}

QSize SelectionToolbarValueLabel::sizeHint() const {
    const int textWidth = text().isEmpty() ? 0 : QFontMetrics(font()).horizontalAdvance(text());
    int width = textWidth + ValueHorizontalInset * 2;
    if (!m_leadingIcon.isNull()) {
        const int iconWidth = screenshot_selection_toolbar::IconSize;
        if (m_iconOnly) {
            width = iconWidth + LockIconHorizontalMargin * 2;
        } else {
            width += iconWidth + IconTextSpacing;
        }
    }
    return QSize(std::max(1, width), screenshot_selection_toolbar::PanelHeight -
                                         screenshot_selection_toolbar::PanelVerticalPadding * 2);
}

QSize SelectionToolbarValueLabel::minimumSizeHint() const {
    return sizeHint();
}

void SelectionToolbarValueLabel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    if (!testAttribute(Qt::WA_TransparentForMouseEvents) && m_hovered) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(PanelHoverColor);
        QRectF hoverRect = QRectF(rect()).adjusted(0.5, 1.0, -0.5, -1.0);
        if (m_lockAspectRatioControl) {
            hoverRect.adjust(LockIconHorizontalMargin / 2.0, 0.0, -LockIconHorizontalMargin / 2.0,
                             0.0);
        }
        painter.drawRoundedRect(hoverRect, screenshot_selection_toolbar::PanelRadius,
                                screenshot_selection_toolbar::PanelRadius);
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font());
    painter.setPen(PanelTextColor);

    int x = ValueHorizontalInset;
    if (!m_leadingIcon.isNull()) {
        if (m_iconOnly) {
            x = LockIconHorizontalMargin;
        }
        const QRect iconRect(x, (height() - screenshot_selection_toolbar::IconSize) / 2 + 1,
                             screenshot_selection_toolbar::IconSize,
                             screenshot_selection_toolbar::IconSize);
        painter.drawPixmap(iconRect, m_leadingIcon);
        x += screenshot_selection_toolbar::IconSize + (m_iconOnly ? 0 : IconTextSpacing);
    }

    if (!m_iconOnly) {
        const QRect textRect(x, 0, std::max(1, width() - x - ValueHorizontalInset), height());
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text());
    }
}

SelectionToolbarSeparator::SelectionToolbarSeparator(QWidget* parent) : QWidget(parent) {
    setFixedSize(sizeHint());
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize SelectionToolbarSeparator::sizeHint() const {
    return QSize(PanelSeparatorWidth + SeparatorLeftMargin + SeparatorRightMargin,
                 PanelSeparatorHeight);
}

QSize SelectionToolbarSeparator::minimumSizeHint() const {
    return sizeHint();
}

void SelectionToolbarSeparator::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    const QRect lineRect(SeparatorLeftMargin, (height() - PanelSeparatorHeight) / 2,
                         PanelSeparatorWidth, PanelSeparatorHeight);
    painter.fillRect(lineRect, PanelSeparatorColor);
}
