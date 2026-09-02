#include "snow_shot/presentation/components/infotooltipicon.h"

#include "snow_shot/presentation/components/icons/iconrenderutils.h"

#include "antd_icons.h"
#include "widgets/tooltip.h"

#include <QPixmap>
#include <QPalette>
#include <QSize>

#include <algorithm>

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;
}

InfoTooltipIcon::InfoTooltipIcon(int iconSize, QWidget* parent)
    : QLabel(parent), m_tooltip(new adqt::widgets::AdTooltip(this)),
      m_iconSize(std::max(1, iconSize)) {
    setAlignment(Qt::AlignCenter);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(m_iconSize, m_iconSize);
    m_iconColor = palette().color(QPalette::WindowText);
    syncIcon();

    m_tooltip->setTargetWidget(this);
    m_tooltip->setAnchorWidget(this);
    m_tooltip->setPlacement(adqt::widgets::AdTooltip::Placement::Top);
    m_tooltip->setArrowPointAtCenter(true);
    m_tooltip->setHoverOpenDelayMs(0);
    m_tooltip->setEnabled(false);
}

QString InfoTooltipIcon::tooltipText() const {
    return m_tooltip->text();
}

void InfoTooltipIcon::setTooltipText(const QString& text) {
    const bool hasText = !text.trimmed().isEmpty();
    m_tooltip->setText(hasText ? text : QString());
    m_tooltip->setEnabled(hasText);
    setAccessibleDescription(hasText ? text : QString());

    if (!hasText) {
        m_tooltip->hide();
    }
}

void InfoTooltipIcon::setIconColor(const QColor& color) {
    if (m_iconColor == color) {
        return;
    }

    m_iconColor = color;
    syncIcon();
}

adqt::widgets::AdTooltip* InfoTooltipIcon::tooltipHost() const {
    return m_tooltip;
}

void InfoTooltipIcon::syncIcon() {
    const QPixmap pixmap = snow_shot::presentation::icons::renderTintedIconPixmap(
        outlined_icons::InfoCircle(), QSize(m_iconSize, m_iconSize), devicePixelRatioF(),
        m_iconColor);
    setPixmap(pixmap);
}
