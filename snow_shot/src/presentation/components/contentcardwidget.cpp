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
    if (pageDefinition == nullptr || m_stack == nullptr) {
        return;
    }

    const QString previousRoute = currentRoute();
    const QString previousPageId = m_currentLocation.pageId;

    const bool sameActivePage = m_activePage != nullptr && m_activePageId == resolved.pageId;
    if (!sameActivePage) {
        destroyActivePage();
        QWidget* routeWidget = createPage(*pageDefinition);
        if (routeWidget == nullptr) {
            return;
        }
        m_activePage = routeWidget;
        m_activePageId = resolved.pageId;
        m_stack->addWidget(routeWidget);
        m_stack->setCurrentWidget(routeWidget);
        if (auto* historyPage = dynamic_cast<ScreenshotHistoryPageWidget*>(routeWidget);
            historyPage != nullptr) {
            historyPage->setActive(true);
        }
    }

    m_currentLocation = resolved;
    if (auto* page = dynamic_cast<SettingsPageWidget*>(m_activePage.data()); page != nullptr) {
        page->reveal(resolved);
    }

    if (previousPageId != resolved.pageId) {
        emit routeChanged(pageDefinition->route);
        emit sectionListChanged();
    } else if (previousRoute != pageDefinition->route) {
        emit routeChanged(pageDefinition->route);
    }
    emit locationChanged(m_currentLocation);
}

QWidget* ContentCardWidget::createPage(
    const snow_shot::presentation::settings::SettingsPageDefinition& definition) {
    QWidget* page = nullptr;
    if (definition.kind ==
        snow_shot::presentation::settings::SettingsPageKind::ScreenshotHistory) {
        auto* historyPage = new ScreenshotHistoryPageWidget(m_stack);
        connect(historyPage, &ScreenshotHistoryPageWidget::editRequested, this,
                &ContentCardWidget::screenshotHistoryEditRequested);
        page = historyPage;
    } else {
        auto* settingsPage =
            new SettingsPageWidget(m_catalog, definition.id, m_runtimeBindings, m_stack);
        QPointer<SettingsPageWidget> pageGuard(settingsPage);
        connect(settingsPage, &SettingsPageWidget::commandRequested, this,
                &ContentCardWidget::handleCommand);
        connect(settingsPage, &SettingsPageWidget::visibleSectionChanged, this,
                [this, pageGuard](const QString& sectionId) {
                    if (pageGuard == nullptr || sectionId.isEmpty() || m_stack == nullptr ||
                        m_activePage != pageGuard || m_stack->currentWidget() != pageGuard ||
                        m_currentLocation.pageId != pageGuard->pageId() ||
                        (m_currentLocation.sectionId == sectionId &&
                         m_currentLocation.itemId.isEmpty())) {
                        return;
                    }
                    m_currentLocation.sectionId = sectionId;
                    m_currentLocation.itemId.clear();
                    emit locationChanged(m_currentLocation);
                });
        page = settingsPage;
    }
    return page;
}

void ContentCardWidget::destroyActivePage() {
    QWidget* page = m_activePage.data();
    if (page == nullptr) {
        m_activePageId.clear();
        return;
    }

    if (auto* historyPage = dynamic_cast<ScreenshotHistoryPageWidget*>(page);
        historyPage != nullptr) {
        historyPage->setActive(false);
    }
    if (m_stack != nullptr && m_stack->indexOf(page) >= 0) {
        m_stack->removeWidget(page);
    }
    // Detach the page from services immediately. This prevents a widget that is
    // waiting for deferred deletion from processing theme, language, or runtime
    // updates after it has left the active route.
    page->disconnect();
    QObject::disconnect(&m_runtimeBindings, nullptr, page, nullptr);
    QObject::disconnect(&snow_shot::presentation::styles::ThemeManager::instance(), nullptr, page,
                        nullptr);
    for (QObject* source : page->findChildren<QObject*>()) {
        QObject::disconnect(source, nullptr, page, nullptr);
    }
    m_activePage.clear();
    m_activePageId.clear();
    page->setParent(nullptr);
    page->deleteLater();
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
    if (auto* page = dynamic_cast<SettingsPageWidget*>(m_activePage.data()); page != nullptr) {
        page->applyTheme(scheme);
    } else if (auto* historyPage =
                   dynamic_cast<ScreenshotHistoryPageWidget*>(m_activePage.data());
               historyPage != nullptr) {
        historyPage->applyTheme(scheme);
    }
    update();
}

void ContentCardWidget::retranslateUi() {
    if (auto* page = dynamic_cast<SettingsPageWidget*>(m_activePage.data()); page != nullptr) {
        page->retranslateUi();
    } else if (auto* historyPage =
                   dynamic_cast<ScreenshotHistoryPageWidget*>(m_activePage.data());
               historyPage != nullptr) {
        historyPage->retranslateUi();
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
