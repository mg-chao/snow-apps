#include "snow_shot/presentation/components/maincontentheaderwidget.h"

#include "snow_shot/presentation/components/applicationsearchwidget.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/settings/settingsregistry.h"

#include "widgets/tabs.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QPalette>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace {
constexpr int GLOBAL_SEARCH_MAX_WIDTH = 400;
constexpr int GLOBAL_SEARCH_MIN_WIDTH = 280;
} // namespace

MainContentHeaderWidget::MainContentHeaderWidget(
    const snow_shot::presentation::settings::SettingsRegistry& registry,
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric, QWidget* parent)
    : QFrame(parent), m_tabs(new adqt::widgets::AdTabs(this)),
      m_globalSearch(new ApplicationSearchWidget(registry, metric, this)) {
    setObjectName(QStringLiteral("mainContentHeader"));
    setFrameShape(QFrame::NoFrame);
    setAutoFillBackground(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(metric.padding, metric.paddingXS, metric.padding, 0);
    rootLayout->setSpacing(0);

    auto* searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(0);
    searchRow->addStretch(1);

    m_globalSearch->setObjectName(QStringLiteral("globalTopSearchBar"));
    m_globalSearch->setMinimumWidth(GLOBAL_SEARCH_MIN_WIDTH);
    m_globalSearch->setMaximumWidth(GLOBAL_SEARCH_MAX_WIDTH);
    m_globalSearch->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    searchRow->addWidget(m_globalSearch, 0, Qt::AlignHCenter | Qt::AlignVCenter);
    searchRow->addStretch(1);
    rootLayout->addLayout(searchRow);

    m_tabs->setObjectName(QStringLiteral("mainSectionTabs"));
    m_tabs->setType(adqt::widgets::AdTabs::Type::Line);
    m_tabs->setControlSize(adqt::widgets::AdTabs::ControlSize::Small);
    m_tabs->setTabPlacement(adqt::widgets::AdTabs::Placement::Top);
    m_tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_tabs->hide();
    rootLayout->addWidget(m_tabs, 0);

    connect(m_tabs, &adqt::widgets::AdTabs::tabClicked, this,
            [this](const QString& sectionId) { emit sectionRequested(sectionId); });
    connect(m_globalSearch, &ApplicationSearchWidget::locationActivated, this,
            &MainContentHeaderWidget::locationRequested);

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            &MainContentHeaderWidget::applyTheme);
    retranslateUi();
    applyTheme(themeManager.themeColorScheme());
}

QString MainContentHeaderWidget::currentSection() const {
    return m_tabs != nullptr ? m_tabs->currentKey() : QString();
}

void MainContentHeaderWidget::setSections(
    const QVector<snow_shot::presentation::settings::SettingsSectionSummary>& sections) {
    if (m_tabs == nullptr) {
        return;
    }

    const QSignalBlocker blocker(m_tabs);
    m_tabs->clear();
    m_sections = sections;
    for (const auto& section : m_sections) {
        if (section.id.trimmed().isEmpty()) {
            continue;
        }
        adqt::widgets::AdTabs::TabItem tab;
        tab.key = section.id;
        tab.label = section.label;
        tab.closable = false;
        m_tabs->addTab(tab);
    }
    if (m_tabs->count() > 0) {
        m_tabs->setCurrentIndex(0);
    }
    m_tabs->setVisible(m_tabs->count() > 0);
    updateLayoutMargins(
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias);
}

void MainContentHeaderWidget::setCurrentSection(const QString& sectionId) {
    if (m_tabs == nullptr || m_tabs->count() == 0) {
        return;
    }
    const int sectionIndex = m_tabs->indexOf(sectionId);
    const QSignalBlocker blocker(m_tabs);
    m_tabs->setCurrentIndex(sectionIndex >= 0 ? sectionIndex : 0);
}

void MainContentHeaderWidget::applyTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    QPalette headerPalette = palette();
    headerPalette.setColor(QPalette::Window, scheme.map.colorBgContainer);
    setPalette(headerPalette);
    updateLayoutMargins(scheme.metricAlias);
    if (m_globalSearch != nullptr) {
        m_globalSearch->applyTheme(scheme);
    }
    update();
}

void MainContentHeaderWidget::updateLayoutMargins(
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric) {
    if (layout() == nullptr) {
        return;
    }
    const int bottomMargin = m_tabs != nullptr && m_tabs->count() > 0 ? 0 : metric.paddingXS;
    layout()->setContentsMargins(metric.padding, metric.paddingXS, metric.padding, bottomMargin);
}

void MainContentHeaderWidget::retranslateUi() {
    if (m_globalSearch != nullptr) {
        m_globalSearch->setPlaceholderText(tr("Search settings and functions"));
    }
}

void MainContentHeaderWidget::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}
