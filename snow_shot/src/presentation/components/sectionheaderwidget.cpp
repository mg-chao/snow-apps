#include "snow_shot/presentation/components/sectionheaderwidget.h"

#include "snow_shot/presentation/components/themedheadericonbutton.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "antd_icons.h"
#include "widgets/button.h"
#include "widgets/popconfirm.h"

#include <QFont>
#include <QHBoxLayout>
#include <QEvent>
#include <QLabel>
#include <QPalette>
#include <QSizePolicy>

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;
} // namespace

SectionHeaderWidget::SectionHeaderWidget(
    const QString& title, const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
    QWidget* parent)
    : QFrame(parent), m_title(title), m_titleLabel(new QLabel(title, this)),
      m_resetButton(new ThemedHeaderIconButton(metric, outlined_icons::Reload(), this)),
      m_expandButton(new ThemedHeaderIconButton(metric, outlined_icons::Down(), this)),
      m_resetPopconfirm(new adqt::widgets::AdPopconfirm(this)) {
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* headerLayout = new QHBoxLayout(this);
    headerLayout->setContentsMargins(0, metric.marginMD, 0, metric.marginMD);
    headerLayout->setSpacing(metric.marginXS);

    m_expandButton->setObjectName(QStringLiteral("sectionExpandButton"));
    m_expandButton->setCheckable(true);
    m_expandButton->setFocusPolicy(Qt::StrongFocus);
    m_expandButton->setChecked(true);
    m_expandButton->hide();
    headerLayout->addWidget(m_expandButton);
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addStretch();

    m_resetButton->setObjectName(QStringLiteral("sectionResetButton"));
    m_resetButton->setToolTip(tr("Reset"));
    m_resetButton->setAccessibleName(tr("Reset"));
    headerLayout->addWidget(m_resetButton);

    m_resetPopconfirm->setObjectName(QStringLiteral("sectionResetPopconfirm"));
    m_resetPopconfirm->setSourceWidget(m_resetButton);
    connect(m_resetPopconfirm, &adqt::widgets::AdPopconfirm::accepted, this,
            &SectionHeaderWidget::resetRequested);
    connect(m_resetButton, &QAbstractButton::clicked, this, [this] {
        if (m_trailingAction == TrailingAction::Refresh) {
            emit refreshRequested();
        }
    });
    connect(m_expandButton, &QAbstractButton::toggled, this, [this](bool expanded) {
        m_expandButton->setIconRef(expanded ? outlined_icons::Down() : outlined_icons::Right());
        retranslateUi();
        emit expandedChanged(expanded);
    });

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            &SectionHeaderWidget::applyTheme);
    retranslateUi();
    applyTheme(themeManager.themeColorScheme());
}

void SectionHeaderWidget::setTitle(const QString& title) {
    m_title = title;
    if (m_titleLabel != nullptr) {
        m_titleLabel->setText(m_title);
    }
    retranslateUi();
}

void SectionHeaderWidget::setCollapsible(bool collapsible) {
    m_expandButton->setVisible(collapsible);
}

void SectionHeaderWidget::setExpanded(bool expanded) {
    m_expandButton->setChecked(expanded);
}

void SectionHeaderWidget::setTrailingAction(TrailingAction action) {
    m_trailingAction = action;
    m_resetButton->setVisible(action != TrailingAction::None);
    m_resetButton->setObjectName(action == TrailingAction::Refresh
                                     ? QStringLiteral("sectionRefreshButton")
                                     : QStringLiteral("sectionResetButton"));
    m_resetPopconfirm->setEnabled(action == TrailingAction::Reset && m_resetButton->isEnabled());
    retranslateUi();
}

void SectionHeaderWidget::setResetVisible(bool visible) {
    setTrailingAction(visible ? TrailingAction::Reset : TrailingAction::None);
}

void SectionHeaderWidget::setResetEnabled(bool enabled) {
    m_resetButton->setEnabled(enabled);
    m_resetPopconfirm->setEnabled(m_trailingAction == TrailingAction::Reset && enabled);
}

void SectionHeaderWidget::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void SectionHeaderWidget::retranslateUi() {
    const QString actionText =
        m_trailingAction == TrailingAction::Refresh ? tr("Refresh") : tr("Reset");
    m_resetButton->setToolTip(actionText);
    m_resetButton->setAccessibleName(actionText);
    const QString expansionText =
        m_expandButton->isChecked() ? tr("Collapse %1").arg(m_title) : tr("Expand %1").arg(m_title);
    m_expandButton->setAccessibleName(expansionText);
    m_expandButton->setToolTip(expansionText);
    updateResetConfirmationText();
}

void SectionHeaderWidget::updateResetConfirmationText() {
    m_resetPopconfirm->setText(tr("Reset \"%1\" to default settings?").arg(m_title));
}

void SectionHeaderWidget::applyTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, Qt::transparent);
    setPalette(palette);

    QPalette labelPalette = m_titleLabel->palette();
    labelPalette.setColor(QPalette::WindowText, scheme.map.colorText);
    m_titleLabel->setPalette(labelPalette);

    QFont titleFont = m_titleLabel->font();
    titleFont.setPixelSize(scheme.metricAlias.fontSizeXL);
    titleFont.setWeight(QFont::Bold);
    m_titleLabel->setFont(titleFont);

    update();
}
