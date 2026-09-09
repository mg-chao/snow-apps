#include "snow_shot/presentation/components/actionrow.h"

#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/styles/actionrowstyle.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>

ActionRow::ActionRow(
    const ActionRowConfig& config,
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
    const snow_shot::presentation::styles::MainWindowComponentMetricToken& mainWindowMetric,
    const snow_shot::presentation::styles::ThemeColorScheme& scheme, QWidget* parent)
    : AdButton(parent), m_compactPresentation(config.compactPresentation), m_colorScheme(scheme),
      m_titleIconRef(config.iconRef), m_rowState(config.rowState),
      m_titleIconSize(metric.fontSizeLG + metric.borderRadiusXS),
      m_rowBorderWidth(metric.lineWidth), m_rowBorderRadius(mainWindowMetric.cardRadius),
      m_useStableBorder(config.useStableBorder) {
    setAccessibleName(config.title);
    setButtonStyle(m_compactPresentation ? ButtonStyle::Text : ButtonStyle::Outline);
    setAccentRole(AccentRole::Primary);
    setShape(Shape::Rounded);
    setFixedHeight(m_compactPresentation ? metric.controlHeight
                                         : metric.controlHeightLG + metric.paddingXXS);
    setCursor(m_compactPresentation ? Qt::ArrowCursor : Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setCheckable(false);
    setAttribute(Qt::WA_Hover, true);

    m_rowLayout = new QHBoxLayout(this);
    if (m_compactPresentation) {
        m_rowLayout->setContentsMargins(0, 0, 0, 0);
        m_rowLayout->setSpacing(metric.marginXS);
    } else {
        m_rowLayout->setContentsMargins(metric.padding, metric.paddingXXS, metric.padding,
                                        metric.paddingXXS);
        m_rowLayout->setSpacing(metric.marginXS + metric.borderRadiusXS);
    }

    auto* titleWrap = new QWidget(this);
    titleWrap->setAttribute(Qt::WA_TransparentForMouseEvents, !config.interactiveTitle);
    auto* titleLayout = new QHBoxLayout(titleWrap);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(metric.marginXS);
    m_titleLabel = new QLabel(config.title, titleWrap);
    m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, !config.interactiveTitle);
    titleLayout->addWidget(m_titleLabel, 0, Qt::AlignVCenter);
    if (!m_compactPresentation && adqt::icons::isValid(m_titleIconRef)) {
        m_titleIcon = new QLabel(titleWrap);
        m_titleIcon->setObjectName(QStringLiteral("actionRowTitleIcon"));
        m_titleIcon->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_titleIcon->setFixedSize(m_titleIconSize, m_titleIconSize);
        titleLayout->addWidget(m_titleIcon, 0, Qt::AlignVCenter);
    }
    if (!m_compactPresentation) {
        titleLayout->addStretch(1);
    }
    m_rowLayout->addWidget(titleWrap, m_compactPresentation ? 0 : 1);

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            [this](const auto& updatedScheme) { applyTheme(updatedScheme); });
    syncTitle();
}

void ActionRow::setConfigurationButton(adqt::widgets::AdButton* button) {
    m_configurationButton = button;
    button->installEventFilter(this);
    m_rowLayout->addWidget(button, 0, m_compactPresentation ? Qt::AlignVCenter : Qt::AlignRight);
    if (m_compactPresentation) {
        m_rowLayout->addStretch(1);
    }
    syncTitle();
}

void ActionRow::applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    m_colorScheme = scheme;
    syncTitle();
    update();
}

void ActionRow::paintEvent(QPaintEvent*) {
    if (m_compactPresentation) {
        return;
    }
    QPainter painter(this);
    const bool configurationActive = isConfigurationButtonActive();
    snow_shot::presentation::styles::paintActionRow(
        painter, size(), m_colorScheme.map, m_rowState, isDown() && !configurationActive,
        underMouse() && !configurationActive, m_rowBorderRadius, m_rowBorderWidth,
        m_useStableBorder);
}

bool ActionRow::event(QEvent* event) {
    const bool handled = AdButton::event(event);
    const auto type = event->type();
    if (type == QEvent::Enter || type == QEvent::Leave || type == QEvent::MouseButtonPress ||
        type == QEvent::MouseButtonRelease || type == QEvent::DevicePixelRatioChange) {
        syncTitle();
        update();
    }
    return handled;
}

bool ActionRow::eventFilter(QObject* watched, QEvent* event) {
    const auto type = event->type();
    if (watched == m_configurationButton &&
        (type == QEvent::Enter || type == QEvent::Leave || type == QEvent::MouseButtonPress ||
         type == QEvent::MouseButtonRelease)) {
        syncTitle();
        update();
    }
    return AdButton::eventFilter(watched, event);
}

bool ActionRow::isConfigurationButtonActive() const {
    return m_configurationButton != nullptr &&
           (m_configurationButton->underMouse() || m_configurationButton->isDown());
}

void ActionRow::syncTitle() {
    if (m_titleLabel == nullptr) {
        return;
    }
    const bool configurationActive = isConfigurationButtonActive();
    const QColor color = m_compactPresentation
                             ? m_colorScheme.map.colorText
                             : snow_shot::presentation::styles::actionRowColor(
                                   m_rowState, isDown() && !configurationActive,
                                   underMouse() && !configurationActive, m_colorScheme.map);
    const QString css = color.alpha() == 255 ? color.name(QColor::HexRgb)
                                             : QStringLiteral("rgba(%1, %2, %3, %4)")
                                                   .arg(color.red())
                                                   .arg(color.green())
                                                   .arg(color.blue())
                                                   .arg(color.alpha());
    m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(css));
    QFont labelFont = m_titleLabel->font();
    labelFont.setPixelSize(m_compactPresentation ? m_colorScheme.metricAlias.fontSize
                                                 : m_colorScheme.metricAlias.fontSizeLG);
    labelFont.setWeight(m_compactPresentation ? QFont::Normal : QFont::Medium);
    m_titleLabel->setFont(labelFont);
    if (m_titleIcon != nullptr) {
        m_titleIcon->setPixmap(snow_shot::presentation::icons::renderTintedIconPixmap(
            m_titleIconRef, QSize(m_titleIconSize, m_titleIconSize), devicePixelRatioF(), color));
    }
}
