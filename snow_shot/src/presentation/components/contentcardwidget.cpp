#include "snow_shot/presentation/components/contentcardwidget.h"

#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/presentation/settings/settingsruntimebindings.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include <QEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QVBoxLayout>

ContentCardWidget::ContentCardWidget(
    const snow_shot::presentation::settings::SettingsCatalog& catalog,
    snow_shot::presentation::settings::SettingsRuntimeBindings& runtimeBindings, QWidget* parent)
    : QFrame(parent), m_catalog(catalog), m_runtimeBindings(runtimeBindings),
      m_colorScheme(snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme()) {
    setFrameShape(QFrame::NoFrame);
    setLineWidth(0);
    setAutoFillBackground(false);

    auto* cardLayout = new QVBoxLayout(this);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    m_stack = new QStackedWidget(this);
    m_stack->setFrameShape(QFrame::NoFrame);
    m_stack->setLineWidth(0);
    m_stack->setAutoFillBackground(false);

    for (const auto& pageDefinition : m_catalog.pages()) {
        QWidget* routeWidget = nullptr;
        if (pageDefinition.kind ==
            snow_shot::presentation::settings::SettingsPageKind::ScreenshotHistory) {
            m_historyPlaceholder = new QWidget(m_stack);
            m_historyPlaceholder->setObjectName(
                QStringLiteral("screenshotHistoryRoutePlaceholder"));
            routeWidget = m_historyPlaceholder;
        } else {
            auto* page =
                new SettingsPageWidget(m_catalog, pageDefinition.id, m_runtimeBindings, m_stack);
            routeWidget = page;
            m_pagesById.insert(pageDefinition.id, page);
            connect(page, &SettingsPageWidget::commandRequested, this,
                    &ContentCardWidget::handleCommand);
            connect(page, &SettingsPageWidget::visibleSectionChanged, this,
                    [this, page](const QString& sectionId) {
                        if (sectionId.isEmpty() || m_stack == nullptr ||
                            m_stack->currentWidget() != page ||
                            m_currentLocation.pageId != page->pageId() ||
                            (m_currentLocation.sectionId == sectionId &&
                             m_currentLocation.itemId.isEmpty())) {
                            return;
                        }
                        m_currentLocation.sectionId = sectionId;
                        m_currentLocation.itemId.clear();
                        emit locationChanged(m_currentLocation);
                    });
        }
        m_routeWidgetsById.insert(pageDefinition.id, routeWidget);
        m_routePageIndices.insert(pageDefinition.route, m_stack->addWidget(routeWidget));
    }

    cardLayout->addWidget(m_stack, 1);
    navigateTo(m_catalog.defaultLocation());

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            &ContentCardWidget::applyTheme);
    applyTheme(m_colorScheme);
}

ContentCardWidget::~ContentCardWidget() = default;

QString ContentCardWidget::currentRoute() const {
    const auto* page = m_catalog.page(m_currentLocation.pageId);
    if (page != nullptr) {
        return page->route;
    }
    const auto* defaultPage = m_catalog.page(m_catalog.defaultLocation().pageId);
    return defaultPage != nullptr ? defaultPage->route : QStringLiteral("/");
}

snow_shot::presentation::settings::SettingsLocation ContentCardWidget::currentLocation() const {
    return m_currentLocation;
}

QVector<snow_shot::presentation::settings::SettingsSectionSummary>
ContentCardWidget::currentSections() const {
    return m_catalog.sectionSummaries(m_currentLocation.pageId);
}

void ContentCardWidget::setCurrentRoute(const QString& route) {
    const auto* page = m_catalog.pageForRoute(route);
    navigateTo(page != nullptr
                   ? snow_shot::presentation::settings::SettingsLocation{page->id, {}, {}}
                   : snow_shot::presentation::settings::SettingsLocation{});
}

void ContentCardWidget::activateSection(const QString& sectionId) {
    navigateTo({m_currentLocation.pageId, sectionId, {}});
}

void ContentCardWidget::navigateTo(
    const snow_shot::presentation::settings::SettingsLocation& requested) {
    const auto resolved = m_catalog.resolveLocation(requested);
    const auto* pageDefinition = m_catalog.page(resolved.pageId);
    if (pageDefinition != nullptr &&
        pageDefinition->kind ==
            snow_shot::presentation::settings::SettingsPageKind::ScreenshotHistory) {
        ensureHistoryPage(resolved.pageId);
    }
    QWidget* routeWidget = m_routeWidgetsById.value(resolved.pageId, nullptr);
    if (pageDefinition == nullptr || routeWidget == nullptr || m_stack == nullptr) {
        return;
    }

    const QString previousRoute = currentRoute();
    const QString previousPageId = m_currentLocation.pageId;
    const int pageIndex = m_routePageIndices.value(pageDefinition->route, -1);
    if (pageIndex < 0) {
        return;
    }

    if (m_historyPage != nullptr && previousPageId != resolved.pageId) {
        m_historyPage->setActive(false);
    }
    m_stack->setCurrentIndex(pageIndex);
    m_currentLocation = resolved;
    if (auto* page = m_pagesById.value(resolved.pageId, nullptr); page != nullptr) {
        page->reveal(resolved);
    } else if (routeWidget == m_historyPage && m_historyPage != nullptr) {
        m_historyPage->setActive(true);
    }

    if (previousPageId != resolved.pageId) {
        emit routeChanged(pageDefinition->route);
        emit sectionListChanged();
    } else if (previousRoute != pageDefinition->route) {
        emit routeChanged(pageDefinition->route);
    }
    emit locationChanged(m_currentLocation);
}

ScreenshotHistoryPageWidget* ContentCardWidget::ensureHistoryPage(const QString& pageId) {
    if (m_historyPage != nullptr || m_stack == nullptr || m_historyPlaceholder == nullptr) {
        return m_historyPage;
    }

    const int index = m_stack->indexOf(m_historyPlaceholder);
    if (index < 0) {
        return nullptr;
    }
    m_stack->removeWidget(m_historyPlaceholder);
    m_historyPlaceholder->deleteLater();
    m_historyPlaceholder = nullptr;

    m_historyPage = new ScreenshotHistoryPageWidget(m_stack);
    connect(m_historyPage, &ScreenshotHistoryPageWidget::editRequested, this,
            &ContentCardWidget::screenshotHistoryEditRequested);
    m_historyPage->applyTheme(m_colorScheme);
    m_historyPage->retranslateUi();
    m_stack->insertWidget(index, m_historyPage);
    m_routeWidgetsById.insert(pageId, m_historyPage);
    return m_historyPage;
}

void ContentCardWidget::showInterfaceSettings() {
    navigateTo({QStringLiteral("interface-settings"), QStringLiteral("general"), {}});
}

void ContentCardWidget::handleCommand(
    const snow_shot::presentation::settings::SettingsCommand& command) {
    switch (command.kind) {
    case snow_shot::presentation::settings::SettingsCommandKind::CaptureScreenshot:
        emit screenshotRequested();
        break;
    case snow_shot::presentation::settings::SettingsCommandKind::ExecuteQuickAction:
        emit quickActionRequested(command.shortcutAction);
        break;
    case snow_shot::presentation::settings::SettingsCommandKind::Navigate:
        navigateTo(command.location);
        break;
    }
}

void ContentCardWidget::applyTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    m_colorScheme = scheme;
    for (SettingsPageWidget* page : m_pagesById) {
        if (page != nullptr) {
            page->applyTheme(scheme);
        }
    }
    if (m_historyPage != nullptr) {
        m_historyPage->applyTheme(scheme);
    }
    update();
}

void ContentCardWidget::retranslateUi() {
    for (SettingsPageWidget* page : m_pagesById) {
        if (page != nullptr) {
            page->retranslateUi();
        }
    }
    if (m_historyPage != nullptr) {
        m_historyPage->retranslateUi();
    }
    emit sectionListChanged();
}

void ContentCardWidget::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void ContentCardWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto mainWindowMetric =
        snow_shot::presentation::styles::buildMainWindowComponentMetricToken(m_colorScheme);
    const QRectF cardRect = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_colorScheme.map.colorBgContainer);
    painter.drawRoundedRect(cardRect, static_cast<qreal>(mainWindowMetric.cardRadius),
                            static_cast<qreal>(mainWindowMetric.cardRadius));
}
