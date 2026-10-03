#include "snow_shot/presentation/components/maincontentheaderwidget.h"

#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/settings/settingsregistry.h"

#include "widgets/tabs.h"

#include <QPalette>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

MainContentHeaderWidget::MainContentHeaderWidget(
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric, QWidget* parent)
    : QFrame(parent), m_tabs(new adqt::widgets::AdTabs(this)) {
    setObjectName(QStringLiteral("mainContentHeader"));
    setFrameShape(QFrame::NoFrame);
    setAutoFillBackground(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(metric.padding, metric.paddingXS, metric.padding, 0);
    rootLayout->setSpacing(0);

    m_tabs->setObjectName(QStringLiteral("mainSectionTabs"));
    m_tabs->setType(adqt::widgets::AdTabs::Type::Line);
    m_tabs->setControlSize(adqt::widgets::AdTabs::ControlSize::Small);
    m_tabs->setTabPlacement(adqt::widgets::AdTabs::Placement::Top);
    m_tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_tabs->hide();
    rootLayout->addWidget(m_tabs, 0);

    connect(m_tabs, &adqt::widgets::AdTabs::tabClicked, this,
            [this](const QString& sectionId) { emit sectionRequested(sectionId); });

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            &MainContentHeaderWidget::applyTheme);
    hide();
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
    setVisible(m_tabs->count() > 0);
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
    m_surfaceColor = scheme.map.colorBgContainer;
    updateSkinMask();
    updateLayoutMargins(scheme.metricAlias);
    update();
}

void MainContentHeaderWidget::setSkinMaskOpacity(qreal opacity) {
    const qreal normalized = std::isfinite(opacity) ? std::clamp(opacity, 0.0, 1.0) : 1.0;
    if (m_skinMaskOpacity == normalized) {
        return;
    }
    m_skinMaskOpacity = normalized;
    updateSkinMask();
}

void MainContentHeaderWidget::updateSkinMask() {
    QPalette headerPalette = palette();
    QColor background = m_surfaceColor;
    if (m_skinMaskOpacity != 1.0) {
        background.setAlphaF(background.alphaF() * static_cast<float>(m_skinMaskOpacity));
    }
    headerPalette.setColor(QPalette::Window, background);
    setPalette(headerPalette);
    update();
}

void MainContentHeaderWidget::updateLayoutMargins(
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric) {
    if (layout() == nullptr) {
        return;
    }
    layout()->setContentsMargins(metric.padding, metric.paddingXS, metric.padding, 0);
}
