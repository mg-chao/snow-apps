#include "snow_shot/presentation/components/settingspagewidget.h"

#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/apppermissionservice.h"
#include "widgets/alert.h"
#include "snow_shot/presentation/components/globalmouserow.h"
#include "snow_shot/presentation/components/sectionheaderwidget.h"
#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/presentation/components/formfields.h"
#include "snow_shot/presentation/components/settingspageutils.h"
#include "snow_shot/presentation/components/shortcutkeyrow.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/settings/settingsformfield.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include "widgets/button.h"
#include "widgets/message.h"
#include "widgets/divider.h"
#include "widgets/modal.h"
#include "widgets/scroll_area.h"

#include <QAbstractButton>
#include <QApplication>
#include "snow_shot/presentation/globalmousemanager.h"
#include <QEvent>
#include <QFileDialog>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHash>
#include <QHideEvent>
#include <QLabel>
#include <QPointer>

#include <QScrollBar>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QStyle>
#include <QSizePolicy>
#include <QTimer>

#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace {
namespace settings = snow_shot::presentation::settings;
namespace settings_ui = snow_shot::presentation::components;
namespace form_fields = snow_shot::presentation::components::form_fields;

std::optional<snow_shot::presentation::AppPermission>
permissionForRenderer(settings::SettingsCustomRenderer renderer) {
    using Permission = snow_shot::presentation::AppPermission;
    using Renderer = settings::SettingsCustomRenderer;
    switch (renderer) {
    case Renderer::PermissionScreenRecording:
        return Permission::ScreenRecording;
    case Renderer::PermissionAccessibility:
        return Permission::Accessibility;
    case Renderer::PermissionInputMonitoring:
        return Permission::InputMonitoring;
    case Renderer::PermissionMicrophone:
        return Permission::Microphone;
    case Renderer::TextTranslationConfigurations:
    case Renderer::CustomAiModels:
    case Renderer::McpStatus:
    case Renderer::StorageStatus:
    case Renderer::DrawingToolbarEditor:
    case Renderer::ScreenshotToolbarEditor:
    case Renderer::PinnedToolbarEditor:
    case Renderer::TrayMenuOptions:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

class SettingsPageWidget::Impl {
  public:
    struct RuntimeItem {
        const settings::SettingsFieldDescriptor* descriptor = nullptr;
        const settings::SettingsItemDefinition* definition = nullptr;
        QWidget* anchor = nullptr;
        QWidget* focusTarget = nullptr;
        QLabel* title = nullptr;
        QLabel* description = nullptr;
        settings::SettingsFormField* formField = nullptr;
        ShortcutKeyRow* shortcutControl = nullptr;
        GlobalMouseRow* globalMouseControl = nullptr;
        adqt::widgets::AdButton* actionControl = nullptr;
        adqt::widgets::AdButton* permissionControl = nullptr;
        std::optional<snow_shot::presentation::AppPermission> permission;
        SettingsCustomWidget* customControl = nullptr;
        std::optional<settings::SettingsCustomRenderer> deferredRenderer;
        QPointer<adqt::widgets::AdModal> modal;
        QPointer<form_fields::FormField> exportStyleField;
    };

    struct RuntimeSection {
        const settings::SettingsSectionDefinition* definition = nullptr;
        SectionHeaderWidget* header = nullptr;
        settings::SettingsSectionReset reset = settings::SettingsSectionReset::None;
        settings::SettingsSectionItemLayout itemLayout =
            settings::SettingsSectionItemLayout::VerticalList;
        const settings::SettingsSectionPlan* plan = nullptr;
        QWidget* list = nullptr;
        bool materialized = false;
    };

    Impl(SettingsPageWidget& owner, const settings::SettingsRegistry& sourceRegistry,
         const QString& sourcePageId, settings::SettingsRuntimeSession& sourceRuntimeSession)
        : q(owner), registry(sourceRegistry), catalog(sourceRegistry.catalog()),
          runtimeSession(sourceRuntimeSession), pagePlan(sourceRegistry.pagePlan(sourcePageId)),
          page(catalog.page(sourcePageId)),
          colorScheme(
              snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme()) {
        Q_ASSERT(page == nullptr || page->id == sourcePageId);
        build();
        connectServices();
        retranslateUi();
        applyTheme(colorScheme);
        initialized = true;
    }

    RuntimeItem* runtimeItem(const QString& itemId) {
        const auto found = itemIndexes.constFind(itemId);
        return found == itemIndexes.cend() ? nullptr : &items[found.value()];
    }

    RuntimeSection* runtimeSection(const QString& sectionId) {
        const auto found = sectionIndexes.constFind(sectionId);
        return found == sectionIndexes.cend() ? nullptr : &sections[found.value()];
    }

    void build() {
        if (page == nullptr) {
            q.setObjectName(settings::generatedObjectName(QStringLiteral("settings-page"),
                                                          QStringLiteral("invalid")));
            return;
        }
        const auto metric = colorScheme.metricAlias;
        q.setObjectName(settings::generatedObjectName(QStringLiteral("settings-page"), page->id));
        if (pagePlan != nullptr) {
            q.setProperty("settingsProviderId", pagePlan->providerId);
            q.setProperty("settingsPagePlanIndex", pagePlan->pageIndex);
        }
        q.setAutoFillBackground(false);

        auto* pageLayout = new QVBoxLayout(&q);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(0);

        auto* pageContainer = new PageContainerWidget(metric, &q);
        pageContainer->setObjectName(
            settings::generatedObjectName(QStringLiteral("settings-container"), page->id));
        scrollArea = pageContainer->scrollArea();
        scrollArea->setObjectName(
            settings::generatedObjectName(QStringLiteral("settings-scroll"), page->id));

        contentWidget = pageContainer->contentWidget();
        contentWidget->setObjectName(
            settings::generatedObjectName(QStringLiteral("settings-content"), page->id));
        contentLayout = pageContainer->contentLayout();
        contentLayout->setSpacing(0);

#ifdef Q_OS_MACOS
        if (page->id == QStringLiteral("global-mouse") ||
            page->id == QStringLiteral("global-hotkeys")) {
            const QMargins margins = contentLayout->contentsMargins();
            contentTopMarginWithoutPermissionBanner = margins.top();
            permissionBanner = new adqt::widgets::AdAlert(contentWidget);
            permissionBanner->setObjectName(QStringLiteral("appPermissionsAlert"));
            permissionBanner->setSeverity(adqt::widgets::AdAlert::Severity::Warning);
            permissionBanner->setIconMode(adqt::widgets::AdAlert::IconMode::Visible);
            permissionBanner->setAnimated(false);
            permissionButton = new adqt::widgets::AdButton(permissionBanner);
            permissionBanner->setActionsWidget(permissionButton);
            connect(permissionButton, &QAbstractButton::clicked, &q, [this] {
                auto* service = runtimeSession.appPermissions();
                if (!service)
                    return;
                const auto missing = relevantMissingPermissions();
                settings::SettingsCommand command;
                command.kind = settings::SettingsCommandKind::Navigate;
                command.location = {
                    QStringLiteral("app-permissions"), QStringLiteral("permissions"),
                    missing.isEmpty() ? QString()
                                      : snow_shot::presentation::appPermissionId(missing.first())};
                emit q.commandRequested(command);
            });
            if (auto* service = runtimeSession.appPermissions())
                connect(service, &snow_shot::presentation::AppPermissionService::changed, &q,
                        [this] { syncMousePermission(); });
            connect(&runtimeSession,
                    &settings::SettingsRuntimeSession::globalMousePermissionChanged, &q,
                    [this] { syncValues(); });
            connect(&runtimeSession, &settings::SettingsRuntimeSession::fieldChanged, &q,
                    [this] { syncMousePermission(); });
            contentLayout->addWidget(permissionBanner);
        }
#endif

        if (pagePlan != nullptr) {
            for (const settings::SettingsSectionPlan& sectionPlan : pagePlan->sectionPlans) {
                Q_ASSERT(sectionPlan.sectionIndex >= 0 &&
                         sectionPlan.sectionIndex < page->sections.size());
                if (sectionPlan.sectionIndex < 0 ||
                    sectionPlan.sectionIndex >= page->sections.size()) {
                    continue;
                }
                const settings::SettingsSectionDefinition& sectionDefinition =
                    page->sections.at(sectionPlan.sectionIndex);
                Q_ASSERT(sectionDefinition.id == sectionPlan.id);
                buildSection(sectionDefinition, &sectionPlan);
            }
        }
        pageLayout->addWidget(pageContainer, 1);
        if (!sections.isEmpty()) {
            materializeSection(sections.first());
        }
        scrollMarginX = metric.paddingSM;
        scrollMarginY = metric.paddingSM;
        requestVisibleSectionSync();
    }

    void buildSection(const settings::SettingsSectionDefinition& sectionDefinition,
                      const settings::SettingsSectionPlan* sectionPlan) {
        const auto metric = colorScheme.metricAlias;
        const settings::SettingsSectionReset reset =
            sectionPlan != nullptr ? sectionPlan->reset : sectionDefinition.reset;
        const settings::SettingsSectionItemLayout itemLayout =
            sectionPlan != nullptr ? sectionPlan->itemLayout : sectionDefinition.itemLayout;
        RuntimeSection runtimeSection;
        runtimeSection.definition = &sectionDefinition;
        runtimeSection.reset = reset;
        runtimeSection.itemLayout = itemLayout;
        runtimeSection.header =
            new SectionHeaderWidget(sectionDefinition.title.translated(), metric, contentWidget);
        runtimeSection.header->setObjectName(settings::generatedObjectName(
            QStringLiteral("settings-section"),
            QStringLiteral("%1-%2").arg(page->id, sectionDefinition.id)));
        runtimeSection.header->setResetVisible(reset != settings::SettingsSectionReset::None);
#ifdef Q_OS_MACOS
        if (page->id == QStringLiteral("app-permissions")) {
            runtimeSection.header->setTrailingAction(SectionHeaderWidget::TrailingAction::Refresh);
            connect(runtimeSection.header, &SectionHeaderWidget::refreshRequested, &q, [this] {
                if (auto* service = runtimeSession.appPermissions()) {
                    service->refresh();
                }
            });
        }
#endif
        contentLayout->addWidget(runtimeSection.header);

        auto* list = new QWidget(contentWidget);
        list->setObjectName(settings::generatedObjectName(
            QStringLiteral("settings-section-list"),
            QStringLiteral("%1-%2").arg(page->id, sectionDefinition.id)));
        list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        runtimeSection.plan = sectionPlan;
        runtimeSection.list = list;
        list->setAccessibleName(sectionDefinition.title.translated());
        // A focusable shell keeps deferred content reachable by keyboard and
        // accessibility clients. Entering it creates the real controls.
        list->setFocusPolicy(Qt::TabFocus);
        list->setFixedHeight(estimatedSectionHeight(runtimeSection));
        sections.push_back(runtimeSection);
        sectionIndexes.insert(sectionDefinition.id, static_cast<int>(sections.size()) - 1);
        contentLayout->addWidget(list);
    }

    int estimatedSectionHeight(const RuntimeSection& section) const {
        if (section.plan == nullptr) {
            return 0;
        }
        const auto& metric = colorScheme.metricAlias;
        const int rowHeight = metric.controlHeight + metric.fontSize + metric.paddingSM;
        int height = 0;
        for (int fieldIndex : section.plan->fieldIndexes) {
            height += registry.fields().at(fieldIndex).kind == settings::SettingsFieldKind::Custom
                          ? rowHeight * 4
                          : rowHeight;
        }
        if (section.itemLayout == settings::SettingsSectionItemLayout::TwoColumnGrid) {
            return (height + rowHeight) / 2 +
                   static_cast<int>(section.plan->fieldIndexes.size() / 2) * metric.marginLG;
        }
        return height +
               qMax(0, static_cast<int>(section.plan->fieldIndexes.size()) - 1) * metric.paddingLG;
    }

    void materializeSection(RuntimeSection& section) {
        if (section.materialized) {
            return;
        }
        section.materialized = true;
        QWidget* shell = section.list;
        // Populate a hidden body, then attach it once. Initializing each row in
        // an already visible shell would repeatedly run show/layout work.
        auto* list = new QWidget(shell);
        const bool wrappingCopy = section.reset == settings::SettingsSectionReset::Skin;
        list->setSizePolicy(QSizePolicy::Expanding,
                            wrappingCopy ? QSizePolicy::Preferred : QSizePolicy::Fixed);
        list->hide();
        const auto metric = colorScheme.metricAlias;
        const auto itemLayout = section.itemLayout;
        const auto& sectionDefinition = *section.definition;
        const QVector<int>* plannedFieldIndexes =
            section.plan != nullptr ? &section.plan->fieldIndexes : nullptr;
        const int firstItem = static_cast<int>(items.size());
        auto* listLayout = new QVBoxLayout(list);
        listLayout->setContentsMargins(0, 0, 0, 0);
        const bool twoColumnItemGrid =
            itemLayout == settings::SettingsSectionItemLayout::TwoColumnGrid;
        const bool dividedItemList = itemLayout == settings::SettingsSectionItemLayout::DividedList;
        listLayout->setSpacing(twoColumnItemGrid
                                   ? 0
                                   : (dividedItemList ? 0
                                      : (page->id == QStringLiteral("global-hotkeys") ||
                                         page->id == QStringLiteral("global-mouse"))
                                          ? metric.padding
                                          : metric.paddingLG));

        QGridLayout* itemGrid = nullptr;
        if (twoColumnItemGrid) {
            itemGrid = new QGridLayout;
            itemGrid->setContentsMargins(0, 0, 0, 0);
            itemGrid->setHorizontalSpacing(metric.marginLG);
            itemGrid->setVerticalSpacing(metric.marginLG);
            itemGrid->setColumnStretch(0, 1);
            itemGrid->setColumnStretch(1, 1);
            listLayout->addLayout(itemGrid);
        }
        if (plannedFieldIndexes != nullptr) {
            for (int itemIndex = 0; itemIndex < plannedFieldIndexes->size(); ++itemIndex) {
                const int fieldIndex = plannedFieldIndexes->at(itemIndex);
                Q_ASSERT(fieldIndex >= 0 && fieldIndex < registry.fields().size());
                if (fieldIndex < 0 || fieldIndex >= registry.fields().size()) {
                    continue;
                }
                const settings::SettingsFieldDescriptor& descriptor =
                    registry.fields().at(fieldIndex);
                Q_ASSERT(descriptor.pageId == page->id &&
                         descriptor.sectionId == sectionDefinition.id &&
                         descriptor.definition != nullptr);
                if (descriptor.pageId != page->id || descriptor.sectionId != sectionDefinition.id ||
                    descriptor.definition == nullptr) {
                    continue;
                }
                buildItem(*descriptor.definition, &descriptor, fieldIndex, list, listLayout,
                          itemLayout, itemGrid, itemIndex);
            }
        }
        if (initialized) {
            retranslateUi(firstItem);
            applyTheme(colorScheme, firstItem);
        }
        auto* shellLayout = new QVBoxLayout(shell);
        shellLayout->setContentsMargins(0, 0, 0, 0);
        shellLayout->addWidget(list);
        shell->setFocusPolicy(Qt::NoFocus);
        shell->setMinimumHeight(0);
        shell->setMaximumHeight(QWIDGETSIZE_MAX);
        if (wrappingCopy) {
            shell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        }
        list->show();
        rebuildTabOrder();
    }

    void buildItem(const settings::SettingsItemDefinition& definition,
                   const settings::SettingsFieldDescriptor* descriptor, int fieldIndex,
                   QWidget* list, QVBoxLayout* listLayout,
                   settings::SettingsSectionItemLayout itemLayout, QGridLayout* itemGrid = nullptr,
                   int itemIndex = 0) {
        RuntimeItem runtime;
        runtime.descriptor = descriptor;
        runtime.definition = &definition;

        const auto addItemWidget = [list, listLayout, itemGrid, itemIndex,
                                    itemLayout](QWidget* widget) {
            if (itemGrid != nullptr) {
                itemGrid->addWidget(widget, itemIndex / 2, itemIndex % 2);
            } else {
                if (itemLayout == settings::SettingsSectionItemLayout::DividedList &&
                    itemIndex > 0) {
                    auto* divider = new adqt::widgets::AdDivider(list);
                    divider->setObjectName(
                        QStringLiteral("settings-item-divider-%1").arg(itemIndex));
                    divider->setDividerSize(adqt::widgets::AdDivider::Size::Small);
                    listLayout->addWidget(divider);
                }
                listLayout->addWidget(widget);
            }
        };

        if (descriptor != nullptr && settings::SettingsFormField::supports(*descriptor)) {
            settings_ui::form_fields::Options options;
            options.presentation = settings_ui::form_fields::Presentation::SettingsRow;
            options.parent = list;
            runtime.formField =
                settings::SettingsFormField::create(*descriptor, runtimeSession, options);
            runtime.anchor = runtime.formField->viewWidget();
            runtime.focusTarget = runtime.formField->focusTarget();
            addItemWidget(runtime.anchor);
        } else {
            std::visit(
                [&](const auto& payload) {
                    using Payload = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<Payload,
                                                 settings::SettingsShortcutActionDefinition>) {
                        const auto shortcutState =
                            runtimeSession.shortcutState(payload.shortcutAction);
                        const auto metric = colorScheme.metricAlias;
                        const auto mainWindowMetric =
                            snow_shot::presentation::styles::buildMainWindowComponentMetricToken(
                                colorScheme);
                        ShortcutKeyRowConfig config;
                        config.title = definition.title.translated();
                        config.iconRef =
                            payload.iconFactory ? payload.iconFactory() : adqt::icons::IconRef();
                        config.shortcuts = shortcutState.shortcuts;
                        config.registrationState = shortcutState;
                        config.rowState = QStringLiteral("normal");
                        config.useStableBorder = true;
                        config.maxShortcutCount = 2;
                        config.shortcutValidator =
                            [this, action = payload.shortcutAction](const auto& shortcut) {
                                return runtimeSession.validateShortcut(action, shortcut);
                            };
                        config.suspendGlobalShortcuts = [this]() {
                            return runtimeSession.suspendGlobalShortcuts();
                        };
                        config.resumeGlobalShortcuts = [this](quint64 handle) {
                            runtimeSession.resumeGlobalShortcuts(handle);
                        };
                        config.adjustableDelay =
                            payload.adjustment ==
                            settings::SettingsShortcutAdjustment::ScreenshotDelaySeconds;
                        config.delaySeconds =
                            config.adjustableDelay
                                ? runtimeSession.integerValue(
                                      settings::SettingsIntegerBinding::ScreenshotDelaySeconds)
                                : 3;
                        config.delaySetter = [this](int value) {
                            return runtimeSession.applyIntegerValue(
                                settings::SettingsIntegerBinding::ScreenshotDelaySeconds, value);
                        };
                        auto* control = new ShortcutKeyRow(config, metric, mainWindowMetric, list);
                        control->setObjectName(settings::generatedObjectName(
                            QStringLiteral("settings-item"), definition.id));
                        runtime.shortcutControl = control;
                        runtime.focusTarget = control;
                        runtime.anchor = control;
                        addItemWidget(control);
                        connect(control, &ShortcutKeyRow::clicked, &q,
                                [this, command = payload.command]() {
                                    emit q.commandRequested(command);
                                });
                        connect(control, &ShortcutKeyRow::shortcutsChanged, &q,
                                [this, action = payload.shortcutAction](const auto& shortcuts) {
                                    if (!runtimeSession.applyShortcuts(action, shortcuts)) {
                                        syncValues();
                                    }
                                });
                    } else if constexpr (std::is_same_v<
                                             Payload, settings::SettingsLocalShortcutDefinition>) {
                        const snow_shot::shortcuts::ShortcutBindingList shortcuts =
                            runtimeSession.localShortcuts(payload.scope, payload.shortcutId);
                        snow_shot::presentation::GlobalShortcutRegistrationState displayState;
                        displayState.shortcuts = shortcuts;
                        displayState.status =
                            shortcuts.isEmpty()
                                ? snow_shot::presentation::GlobalShortcutStatus::Unset
                                : snow_shot::presentation::GlobalShortcutStatus::Registered;
                        const auto metric = colorScheme.metricAlias;
                        const auto mainWindowMetric =
                            snow_shot::presentation::styles::buildMainWindowComponentMetricToken(
                                colorScheme);
                        ShortcutKeyRowConfig config;
                        config.title = definition.title.translated();
                        config.iconRef =
                            payload.iconFactory ? payload.iconFactory() : adqt::icons::IconRef();
                        config.shortcuts = shortcuts;
                        config.registrationState = displayState;
                        config.rowState = QStringLiteral("normal");
                        config.useStableBorder = false;
                        config.maxShortcutCount = 2;
                        config.shortcutValidator = [this, scope = payload.scope,
                                                    shortcutId =
                                                        payload.shortcutId](const auto& shortcut) {
                            return runtimeSession.validateLocalShortcut(scope, shortcutId,
                                                                        shortcut);
                        };
                        config.showRegistrationStatus = false;
                        config.validationScope =
                            payload.scope == settings::SettingsLocalShortcutScope::Screenshot
                                ? ShortcutKeyRowConfig::ValidationScope::ScreenshotShortcut
                            : payload.scope == settings::SettingsLocalShortcutScope::Drawing
                                ? ShortcutKeyRowConfig::ValidationScope::DrawingShortcut
                            : payload.scope == settings::SettingsLocalShortcutScope::ScreenRecording
                                ? ShortcutKeyRowConfig::ValidationScope::RecordingShortcut
                                : ShortcutKeyRowConfig::ValidationScope::PinnedWindowShortcut;
                        config.allowModifierOnlyAlt =
                            payload.scope == settings::SettingsLocalShortcutScope::Screenshot &&
                            payload.shortcutId == QStringLiteral("toggle_guides");
                        config.presentation = ShortcutKeyRowConfig::Presentation::CompactFormField;
                        auto* control = new ShortcutKeyRow(config, metric, mainWindowMetric, list);
                        control->setObjectName(settings::generatedObjectName(
                            QStringLiteral("settings-item"), definition.id));
                        runtime.shortcutControl = control;
                        runtime.focusTarget = control;
                        runtime.anchor = control;
                        addItemWidget(control);
                        connect(
                            control, &ShortcutKeyRow::shortcutsChanged, &q,
                            [this, scope = payload.scope,
                             shortcutId = payload.shortcutId](const auto& next) {
                                if (!runtimeSession.applyLocalShortcuts(scope, shortcutId, next)) {
                                    syncValues();
                                }
                            });
                    } else if constexpr (std::is_same_v<
                                             Payload,
                                             settings::SettingsGlobalMouseActionDefinition>) {
                        auto* control =
                            new GlobalMouseRow(definition.title.translated(), payload.action,
                                               runtimeSession, colorScheme, list);
                        control->setObjectName(settings::generatedObjectName(
                            QStringLiteral("settings-item"), definition.id));
                        runtime.globalMouseControl = control;
                        runtime.focusTarget = control->configurationButton();
                        runtime.anchor = control;
                        addItemWidget(control);
                        connect(control, &GlobalMouseRow::dragRequested, &q,
                                [this](settings::SettingsGlobalMouseAction action) {
                                    settings::SettingsCommand command;
                                    command.kind =
                                        settings::SettingsCommandKind::BeginGlobalMouseDrag;
                                    command.globalMouseAction = action;
                                    emit q.commandRequested(command);
                                });
                    } else if constexpr (std::is_same_v<Payload,
                                                        settings::SettingsActionDefinition>) {
                        auto* control = new adqt::widgets::AdButton(list);
                        control->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
                        control->setAccentRole(payload.accent ==
                                                       settings::SettingsActionAccent::Danger
                                                   ? adqt::widgets::AdButton::AccentRole::Danger
                                                   : adqt::widgets::AdButton::AccentRole::Neutral);
                        control->setSizeClass(adqt::widgets::AdButton::SizeClass::Medium);
                        if (payload.iconFactory) {
                            control->setIconRef(payload.iconFactory());
                        }
                        runtime.actionControl = control;
                        runtime.focusTarget = control;
                        runtime.anchor = settings_ui::createSettingItemRow(
                            list, colorScheme.metricAlias, &runtime.title, &runtime.description,
                            control,
                            settings::generatedObjectName(QStringLiteral("settings-item"),
                                                          definition.id));
                        addItemWidget(runtime.anchor);
                        connect(control, &QAbstractButton::clicked, &q,
                                [this, itemId = definition.id]() { triggerAction(itemId); });
                    } else if constexpr (std::is_same_v<Payload,
                                                        settings::SettingsCustomDefinition>) {
                        if (const auto permission = permissionForRenderer(payload.renderer);
                            permission.has_value()) {
                            auto* control = new adqt::widgets::AdButton(list);
                            control->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
                            control->setSizeClass(adqt::widgets::AdButton::SizeClass::Medium);
                            control->setObjectName(
                                QStringLiteral("appPermission-%1-settings")
                                    .arg(snow_shot::presentation::appPermissionId(
                                        permission.value())));
                            runtime.permissionControl = control;
                            runtime.permission = permission;
                            runtime.anchor = settings_ui::createSettingItemRow(
                                list, colorScheme.metricAlias, &runtime.title, &runtime.description,
                                control,
                                settings::generatedObjectName(QStringLiteral("settings-item"),
                                                              definition.id));
                            runtime.anchor->setFocusPolicy(Qt::StrongFocus);
                            runtime.focusTarget = runtime.anchor;
                            addItemWidget(runtime.anchor);
                            connect(control, &QAbstractButton::clicked, &q,
                                    [this, permission = permission.value()] {
                                        auto* service = runtimeSession.appPermissions();
                                        if (service == nullptr ||
                                            service->snapshot().status(permission) ==
                                                snow_shot::presentation::AppPermissionStatus::
                                                    Checking ||
                                            service->snapshot().granted(permission)) {
                                            return;
                                        }
                                        if (!service->openSettings(permission)) {
                                            adqt::widgets::AdMessage::Request request;
                                            request.content =
                                                q.tr("Could not open System Settings. Open System "
                                                     "Settings > Privacy & Security > %1.")
                                                    .arg(snow_shot::presentation::appPermissionName(
                                                        permission));
                                            adqt::widgets::AdMessageService::error(
                                                std::move(request), &q);
                                        }
                                    });
                            return;
                        }
                        // Toolbar previews can be far below the viewport even in a
                        // visible section. Defer their drag surfaces and tool buttons
                        // independently from the section's ordinary settings rows.
                        if (payload.renderer ==
                                settings::SettingsCustomRenderer::DrawingToolbarEditor ||
                            payload.renderer ==
                                settings::SettingsCustomRenderer::ScreenshotToolbarEditor ||
                            payload.renderer ==
                                settings::SettingsCustomRenderer::PinnedToolbarEditor) {
                            runtime.anchor = new QWidget(list);
                            runtime.anchor->setObjectName(settings::generatedObjectName(
                                QStringLiteral("settings-item"), definition.id));
                            runtime.anchor->setFixedHeight(colorScheme.metricAlias.controlHeight *
                                                           4);
                            runtime.anchor->setFocusPolicy(Qt::TabFocus);
                            runtime.focusTarget = runtime.anchor;
                            runtime.deferredRenderer = payload.renderer;
                            if (initialized) {
                                runtime.anchor->installEventFilter(&q);
                            }
                            addItemWidget(runtime.anchor);
                            return;
                        }
                        auto* control = createSettingsCustomWidget(
                            payload.renderer, registry, definition, runtimeSession, list);
                        Q_ASSERT(control != nullptr);
                        if (control == nullptr) {
                            return;
                        }
                        control->setObjectName(settings::generatedObjectName(
                            QStringLiteral("settings-item"), definition.id));
                        runtime.customControl = control;
                        runtime.anchor = control;
                        runtime.focusTarget = control;
                        addItemWidget(control);
                    }
                },
                definition.payload);
        }

        if (descriptor != nullptr && descriptor->reset == settings::SettingsSectionReset::Skin &&
            runtime.anchor != nullptr) {
            // Skin copy can include a loading or error line. Fixed vertical
            // policies clamp height-for-width to the unwrapped size hint.
            runtime.anchor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        }
        if (runtime.formField == nullptr && runtime.focusTarget != nullptr &&
            runtime.focusTarget != runtime.anchor) {
            runtime.focusTarget->setObjectName(
                settings::generatedObjectName(QStringLiteral("settings-control"), definition.id));
        }
        if (descriptor != nullptr && runtime.anchor != nullptr) {
            runtime.anchor->setProperty("settingsFieldIndex", fieldIndex);
            runtime.anchor->setProperty("settingsFieldKind", static_cast<int>(descriptor->kind));
            runtime.anchor->setProperty("settingsProviderId", descriptor->providerId);
        }
        items.push_back(runtime);
        itemIndexes.insert(definition.id, static_cast<int>(items.size()) - 1);
    }

    void connectServices() {
        QObject::connect(&runtimeSession, &settings::SettingsRuntimeSession::operationMessage, &q,
                         [this](const QString& message, bool warning) {
                             if (!q.isVisible())
                                 return;
                             adqt::widgets::AdMessage::Request request;
                             request.content = message;
                             if (warning)
                                 adqt::widgets::AdMessageService::warning(std::move(request), &q);
                             else
                                 adqt::widgets::AdMessageService::error(std::move(request), &q);
                         });
        QObject::connect(
            &runtimeSession, &settings::SettingsRuntimeSession::actionFinished, &q,
            [this](settings::SettingsActionBinding action, bool success, const QString& error) {
                Q_UNUSED(error);
                if (!q.isVisible() || !success)
                    return;
                for (const RuntimeItem& item : items) {
                    if (item.definition == nullptr)
                        continue;
                    const auto* definition =
                        std::get_if<settings::SettingsActionDefinition>(&item.definition->payload);
                    if (definition == nullptr || definition->binding != action ||
                        !definition->successMessage.has_value()) {
                        continue;
                    }
                    adqt::widgets::AdMessage::Request request;
                    request.key =
                        QStringLiteral("settings-action-success-%1").arg(static_cast<int>(action));
                    request.content = definition->successMessage->translated();
                    adqt::widgets::AdMessageService::success(std::move(request), &q);
                    return;
                }
            });
        auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
        QObject::connect(&themeManager,
                         &snow_shot::presentation::styles::ThemeManager::themeChanged, &q,
                         [this](const auto& scheme) { applyTheme(scheme); });
        QObject::connect(&themeManager,
                         &snow_shot::presentation::styles::ThemeManager::themeModeChanged, &q,
                         [this](auto) { syncValues(); });
        QObject::connect(
            &runtimeSession, &settings::SettingsRuntimeSession::shortcutStateChanged, &q,
            [this](snow_shot::presentation::GlobalShortcutAction action,
                   const snow_shot::presentation::GlobalShortcutRegistrationState& state) {
                const auto* descriptor = registry.fieldForShortcut(action);
                RuntimeItem* item = descriptor == nullptr ? nullptr : runtimeItem(descriptor->id);
                if (item != nullptr && item->shortcutControl != nullptr) {
                    item->shortcutControl->setRegistrationState(state);
                }
            });
        QObject::connect(
            &runtimeSession, &settings::SettingsRuntimeSession::auxiliaryIntegerChanged, &q,
            [this](settings::SettingsIntegerBinding binding, int value) {
                if (binding != settings::SettingsIntegerBinding::ScreenshotDelaySeconds) {
                    return;
                }
                const auto* descriptor = registry.fieldForShortcut(
                    snow_shot::presentation::GlobalShortcutAction::ScreenshotDelay);
                RuntimeItem* item = descriptor == nullptr ? nullptr : runtimeItem(descriptor->id);
                if (item != nullptr && item->shortcutControl != nullptr) {
                    item->shortcutControl->setDelaySeconds(value);
                }
            });
        QObject::connect(&runtimeSession, &settings::SettingsRuntimeSession::fieldChanged, &q,
                         [this](const QString& fieldId, const settings::SettingsFieldState& state) {
                             RuntimeItem* item = runtimeItem(fieldId);
                             if (item == nullptr) {
                                 return;
                             }
                             syncField(*item, &state);
                         });
#ifdef Q_OS_MACOS
        if (page != nullptr && page->id == QStringLiteral("app-permissions")) {
            if (auto* service = runtimeSession.appPermissions()) {
                QObject::connect(service, &snow_shot::presentation::AppPermissionService::changed,
                                 &q, [this] { syncPermissionButtons(); });
            }
        }
#endif
        QObject::connect(&runtimeSession, &settings::SettingsRuntimeSession::storageStateChanged,
                         &q, [this](const snow_shot::storage::StorageStatus& status) {
                             for (RuntimeSection& section : sections) {
                                 if (section.definition != nullptr &&
                                     section.reset != settings::SettingsSectionReset::None) {
                                     const bool historyPolicyUpdate =
                                         section.reset ==
                                             settings::SettingsSectionReset::HistoryPolicy &&
                                         status.historyPolicyUpdating;
                                     section.header->setResetEnabled(status.writeAvailable &&
                                                                     !historyPolicyUpdate);
                                 }
                             }
                         });

        if (scrollArea != nullptr && scrollArea->verticalScrollBar() != nullptr) {
            QObject::connect(scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, &q,
                             [this](int) { requestVisibleSectionSync(); });
            QObject::connect(scrollArea->verticalScrollBar(), &QScrollBar::rangeChanged, &q,
                             [this](int, int) { requestVisibleSectionSync(); });
        }

        for (RuntimeSection& section : sections) {
            QObject::connect(section.header, &SectionHeaderWidget::resetRequested, &q,
                             [this, reset = section.reset]() { resetSection(reset); });
        }
    }

    void triggerAction(const QString& itemId) {
        RuntimeItem* item = runtimeItem(itemId);
        if (item == nullptr || item->definition == nullptr || item->modal != nullptr) {
            return;
        }
        const auto* action =
            std::get_if<settings::SettingsActionDefinition>(&item->definition->payload);
        if (action == nullptr) {
            return;
        }
        if (action->exportOptions.has_value()) {
            showExportOptions(*item, *action);
            return;
        }
        QString filePath;
        if (action->fileOpen.has_value()) {
            filePath =
                QFileDialog::getOpenFileName(&q, action->fileOpen->dialogTitle.translated(),
                                             QString(), action->fileOpen->fileFilter.translated());
            if (filePath.isEmpty()) {
                return;
            }
        }
        const auto runAction = [this, binding = action->binding, filePath]() {
            if (!runtimeSession.triggerAction(binding, filePath)) {
                syncValues();
            }
        };
        if (!action->confirmation.has_value()) {
            runAction();
            return;
        }
        confirmItemAction(*item, *action->confirmation, runAction);
    }

    void showExportOptions(RuntimeItem& item, const settings::SettingsActionDefinition& action) {
        auto* modal = new adqt::widgets::AdModal(&q);
        item.modal = modal;
        modal->setObjectName(
            settings::generatedObjectName(QStringLiteral("settings-modal"), item.definition->id));
        modal->setMode(adqt::widgets::AdModal::Mode::Overlay);
        modal->setOwnerWindow(q.window());
        modal->setCentered(true);
        modal->setPreferredWidth(580);
        modal->setPreset(adqt::widgets::AdModal::Preset::Plain);
        modal->setWindowTitle(item.definition->title.translated());
        modal->setAcceptText(action.buttonText.translated());
        modal->setRejectText(action.exportOptions->rejectText.translated());
        modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                                  adqt::widgets::AdModal::StandardButton::Cancel);

        auto* body = new QWidget;
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* form = new adqt::widgets::AdForm(body);
        form_fields::configureForm(form);
        form_fields::Options options;
        options.parent = form;
        options.form = form;
        const auto styleField = form_fields::switchField(
            {QStringLiteral("includeToolbarStyles"), action.exportOptions->styleFieldLabel,
             action.exportOptions->styleFieldDescription},
            options);
        item.exportStyleField = styleField.field;
        styleField.field->syncValue(false);
        layout->addWidget(form);
        modal->setContentWidget(body);

        QObject::connect(modal, &adqt::widgets::AdModal::accepted, &q,
                         [this, field = QPointer<form_fields::FormField>(styleField.field)] {
                             if (field != nullptr &&
                                 !runtimeSession.triggerAction(
                                     settings::SettingsActionBinding::ExportConfiguration, {},
                                     field->value().toBool())) {
                                 syncValues();
                             }
                         });
        QObject::connect(modal, &adqt::widgets::AdModal::finished, &q,
                         [this, itemId = item.definition->id](adqt::widgets::AdModal::DialogCode) {
                             RuntimeItem* finishedItem = runtimeItem(itemId);
                             if (finishedItem != nullptr && finishedItem->modal != nullptr) {
                                 finishedItem->modal->deleteLater();
                                 finishedItem->modal = nullptr;
                                 finishedItem->exportStyleField = nullptr;
                             }
                         });
        modal->setOpen(true);
    }

    void confirmItemAction(RuntimeItem& item,
                           const settings::SettingsConfirmationDefinition& confirmation,
                           const std::function<void()>& acceptedAction) {
        auto* modal = new adqt::widgets::AdModal(&q);
        item.modal = modal;
        modal->setObjectName(
            settings::generatedObjectName(QStringLiteral("settings-modal"), item.definition->id));
        modal->setMode(adqt::widgets::AdModal::Mode::Window);
        modal->setOwnerWindow(q.window());
        modal->setPreset(adqt::widgets::AdModal::Preset::Confirm);
        modal->setWindowTitle(confirmation.title.translated());
        modal->setText(confirmation.message.translated());
        modal->setAcceptText(confirmation.acceptText.translated());
        modal->setRejectText(confirmation.rejectText.translated());
        modal->setAcceptAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
        modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                                  adqt::widgets::AdModal::StandardButton::Cancel);
        QObject::connect(modal, &adqt::widgets::AdModal::accepted, &q, acceptedAction);
        QObject::connect(modal, &adqt::widgets::AdModal::finished, &q,
                         [this, itemId = item.definition->id](adqt::widgets::AdModal::DialogCode) {
                             RuntimeItem* finishedItem = runtimeItem(itemId);
                             if (finishedItem != nullptr && finishedItem->modal != nullptr) {
                                 finishedItem->modal->deleteLater();
                                 finishedItem->modal = nullptr;
                             }
                         });
        modal->setOpen(true);
    }

    void resetSection(settings::SettingsSectionReset reset) {
        if (!runtimeSession.reset(reset)) {
            syncValues();
        }
    }

    void syncField(RuntimeItem& runtime,
                   const settings::SettingsFieldState* providedState = nullptr) {
        if (runtime.formField != nullptr) {
            runtime.formField->sync(providedState);
            return;
        }
        if (runtime.definition == nullptr) {
            return;
        }
        const QString fieldId =
            runtime.descriptor != nullptr ? runtime.descriptor->id : runtime.definition->id;
        const settings::SettingsFieldState sessionState =
            providedState != nullptr ? *providedState : runtimeSession.state(fieldId);
        const bool fieldEnabled = sessionState.enabled;
        if (runtime.anchor != nullptr) {
            runtime.anchor->setVisible(sessionState.visible);
        }
        QWidget* stateTarget =
            runtime.focusTarget != nullptr ? runtime.focusTarget : runtime.anchor;
        if (stateTarget != nullptr) {
            bool changed = false;
            const auto setProperty = [&](const char* name, const QVariant& value) {
                if (stateTarget->property(name) != value) {
                    stateTarget->setProperty(name, value);
                    changed = true;
                }
            };
            setProperty("settingsDirty", sessionState.dirty);
            setProperty("settingsPending", sessionState.busy);
            setProperty("settingsConflicted", sessionState.conflicted);
            setProperty("settingsError", sessionState.error);
            if (changed && stateTarget->testAttribute(Qt::WA_WState_Polished)) {
                stateTarget->style()->unpolish(stateTarget);
                stateTarget->style()->polish(stateTarget);
                stateTarget->update();
            }
        }

        {
            if (runtime.shortcutControl != nullptr) {
                const auto* definition = std::get_if<settings::SettingsShortcutActionDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.shortcutControl->setRegistrationState(
                        runtimeSession.shortcutState(definition->shortcutAction));
                    if (definition->adjustment ==
                        settings::SettingsShortcutAdjustment::ScreenshotDelaySeconds) {
                        runtime.shortcutControl->setDelaySeconds(runtimeSession.integerValue(
                            settings::SettingsIntegerBinding::ScreenshotDelaySeconds));
                    }
                    runtime.shortcutControl->setEnabled(fieldEnabled);
                }
                const auto* local = std::get_if<settings::SettingsLocalShortcutDefinition>(
                    &runtime.definition->payload);
                if (local != nullptr) {
                    snow_shot::presentation::GlobalShortcutRegistrationState state;
                    state.shortcuts =
                        runtimeSession.localShortcuts(local->scope, local->shortcutId);
                    state.status = state.shortcuts.isEmpty()
                                       ? snow_shot::presentation::GlobalShortcutStatus::Unset
                                       : snow_shot::presentation::GlobalShortcutStatus::Registered;
                    runtime.shortcutControl->setRegistrationState(state);
                    runtime.shortcutControl->setEnabled(fieldEnabled);
                }
            }
            if (runtime.globalMouseControl != nullptr) {
                const auto* definition = std::get_if<settings::SettingsGlobalMouseActionDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.globalMouseControl->setCombination(
                        runtimeSession.globalMouseCombination(definition->action));
                    runtime.globalMouseControl->setEnabled(fieldEnabled);
                }
            }
            if (runtime.actionControl != nullptr) {
                const auto* definition =
                    std::get_if<settings::SettingsActionDefinition>(&runtime.definition->payload);
                if (definition != nullptr) {
                    const settings::SettingsActionState state =
                        runtimeSession.actionState(definition->binding);
                    runtime.actionControl->setBusy(state.busy);
                    runtime.actionControl->setEnabled(state.enabled);
                    if (!state.label.isEmpty())
                        runtime.actionControl->setText(state.label);
                    runtime.actionControl->setToolTip(state.hint);
                    if (!state.label.isEmpty())
                        runtime.actionControl->setAccentRole(
                            state.successAccent ? adqt::widgets::AdButton::AccentRole::Success
                                                : adqt::widgets::AdButton::AccentRole::Neutral);
                    if (!state.hint.isEmpty() && runtime.description)
                        runtime.description->setText(state.hint);
                }
            }
            if (runtime.permissionControl != nullptr) {
                syncPermissionButton(runtime);
            }
        }
    }

    void syncPermissionButton(RuntimeItem& runtime) {
        if (runtime.permissionControl == nullptr || !runtime.permission.has_value()) {
            return;
        }
        auto* service = runtimeSession.appPermissions();
        const auto permission = runtime.permission.value();
        const auto status = service != nullptr
                                ? service->snapshot().status(permission)
                                : snow_shot::presentation::AppPermissionStatus::Error;
        const bool checking = status == snow_shot::presentation::AppPermissionStatus::Checking;
        const bool granted = status == snow_shot::presentation::AppPermissionStatus::Granted;
        auto* button = runtime.permissionControl;
        if (checking) {
            button->setText(q.tr("Checking…"));
            button->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
        } else if (granted) {
            button->setText(q.tr("Authorized"));
            button->setAccentRole(adqt::widgets::AdButton::AccentRole::Success);
        } else {
            button->setText(q.tr("Go to Settings"));
            button->setAccentRole(permission == snow_shot::presentation::AppPermission::Microphone
                                      ? adqt::widgets::AdButton::AccentRole::Orange
                                      : adqt::widgets::AdButton::AccentRole::Danger);
        }
        const bool actionable = service != nullptr && !checking && !granted;
        button->setAttribute(Qt::WA_TransparentForMouseEvents, !actionable);
        button->setFocusPolicy(actionable ? Qt::TabFocus : Qt::NoFocus);
        button->setAccessibleName(button->text() + QStringLiteral(": ") +
                                  runtime.definition->title.translated());
        button->setAccessibleDescription(runtime.definition->description.translated());
    }

    void syncPermissionButtons() {
        for (RuntimeItem& runtime : items) {
            syncPermissionButton(runtime);
        }
    }

    void observePermissionPage(bool visible) {
#ifdef Q_OS_MACOS
        if (page != nullptr && page->id == QStringLiteral("app-permissions")) {
            if (auto* service = runtimeSession.appPermissions()) {
                service->observe(&q, visible);
            }
        }
#else
        Q_UNUSED(visible);
#endif
    }

    snow_shot::presentation::AppPermissions relevantMissingPermissions() const {
        auto* service = runtimeSession.appPermissions();
        return service
                   ? service->missing(snow_shot::presentation::pagePermissions(
                         page->id == QStringLiteral("global-mouse"), service->microphoneEnabled(),
                         runtimeSession.switchValue(
                             settings::SettingsSwitchBinding::TranslationPageEnabled)))
                   : snow_shot::presentation::AppPermissions{};
    }
    void syncMousePermission() {
        if (!permissionBanner)
            return;
        const auto missing = relevantMissingPermissions();
        const bool bannerVisible = !missing.isEmpty();
        const QMargins margins = contentLayout->contentsMargins();
        contentLayout->setContentsMargins(margins.left(),
                                          bannerVisible ? colorScheme.metricAlias.paddingLG
                                                        : contentTopMarginWithoutPermissionBanner,
                                          margins.right(), margins.bottom());
        QStringList names;
        for (auto permission : missing)
            names.append(snow_shot::presentation::appPermissionName(permission));
        permissionBanner->setText(q.tr("Permissions needed"));
        if (page->id == QStringLiteral("global-mouse")) {
            permissionBanner->setInformativeText(
                q.tr("Global mouse actions need access to: %1.").arg(names.join(q.tr(", "))));
        } else {
            QStringList actions;
            using Permission = snow_shot::presentation::AppPermission;
            if (missing.contains(Permission::ScreenRecording))
                actions.append(q.tr("screenshots and screen recording"));
            if (missing.contains(Permission::Accessibility))
                actions.append(q.tr("selected-text translation"));
            if (missing.contains(Permission::Microphone))
                actions.append(q.tr("microphone recording"));
            permissionBanner->setInformativeText(
                q.tr("To use %1, review access to: %2.")
                    .arg(actions.join(q.tr(", ")), names.join(q.tr(", "))));
        }
        permissionBanner->setVisible(bannerVisible);
    }

    void syncValues(int firstItem = 0) {
        const auto storageStatus = runtimeSession.storageStatus();
        for (int index = firstItem; index < items.size(); ++index) {
            syncField(items[index]);
        }
        for (RuntimeSection& runtime : sections) {
            if (runtime.reset != settings::SettingsSectionReset::None) {
                const bool historyPolicyUpdate =
                    runtime.reset == settings::SettingsSectionReset::HistoryPolicy &&
                    storageStatus.historyPolicyUpdating;
                runtime.header->setResetEnabled(storageStatus.writeAvailable &&
                                                !historyPolicyUpdate);
            }
        }
    }

    void retranslateUi(int firstItem = 0) {
        if (firstItem == 0) {
            for (RuntimeSection& runtime : sections) {
                runtime.header->setTitle(runtime.definition->title.translated());
                runtime.list->setAccessibleName(runtime.definition->title.translated());
            }
        }
        for (int runtimeIndex = firstItem; runtimeIndex < items.size(); ++runtimeIndex) {
            RuntimeItem& runtime = items[runtimeIndex];
            if (runtime.formField != nullptr) {
                runtime.formField->retranslateUi();
                continue;
            }
            const settings::SettingsItemDefinition& definition = *runtime.definition;
            const QString title = definition.title.translated();
            const QString description = definition.description.translated();
            if (runtime.title != nullptr) {
                runtime.title->setText(title);
            }
            if (runtime.description != nullptr) {
                runtime.description->setText(description);
            }
            if (runtime.focusTarget != nullptr) {
                runtime.focusTarget->setAccessibleName(title);
                runtime.focusTarget->setAccessibleDescription(description);
            }
            if (runtime.shortcutControl != nullptr) {
                runtime.shortcutControl->setTitle(title);
                runtime.shortcutControl->setAccessibleDescription(description);
                runtime.shortcutControl->retranslateUi();
            }
            if (runtime.globalMouseControl != nullptr) {
                runtime.globalMouseControl->setTitle(title);
                runtime.globalMouseControl->retranslateUi();
            }
            if (runtime.actionControl != nullptr) {
                const auto* action =
                    std::get_if<settings::SettingsActionDefinition>(&definition.payload);
                Q_ASSERT(action != nullptr);
                runtime.actionControl->setText(action->buttonText.translated());
                if (runtime.modal != nullptr && action->confirmation.has_value()) {
                    runtime.modal->setWindowTitle(action->confirmation->title.translated());
                    runtime.modal->setText(action->confirmation->message.translated());
                    runtime.modal->setAcceptText(action->confirmation->acceptText.translated());
                    runtime.modal->setRejectText(action->confirmation->rejectText.translated());
                } else if (runtime.modal != nullptr && action->exportOptions.has_value()) {
                    runtime.modal->setWindowTitle(definition.title.translated());
                    runtime.modal->setAcceptText(action->buttonText.translated());
                    runtime.modal->setRejectText(action->exportOptions->rejectText.translated());
                    if (runtime.exportStyleField != nullptr) {
                        runtime.exportStyleField->retranslateUi();
                    }
                }
            }
            if (runtime.customControl != nullptr) {
                runtime.customControl->retranslateUi();
            }
        }
        if (permissionButton) {
            permissionButton->setText(q.tr("Review permissions"));
            permissionButton->setAccessibleName(permissionButton->text());
            syncMousePermission();
        }
        syncValues(firstItem);
        requestVisibleSectionSync();
    }

    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme,
                    int firstItem = 0) {
        colorScheme = scheme;
        if (firstItem == 0) {
            for (RuntimeSection& runtime : sections) {
                // Headers subscribe to ThemeManager themselves.
                if (!runtime.materialized) {
                    runtime.list->setFixedHeight(estimatedSectionHeight(runtime));
                }
            }
        }
        for (int runtimeIndex = firstItem; runtimeIndex < items.size(); ++runtimeIndex) {
            RuntimeItem& runtime = items[runtimeIndex];
            if (runtime.formField != nullptr) {
                runtime.formField->controller()->applyTheme(scheme);
                continue;
            }
            if (runtime.title != nullptr && runtime.description != nullptr) {
                settings_ui::applySettingItemTheme(runtime.title, runtime.description, scheme);
            }
            if (runtime.shortcutControl != nullptr) {
                runtime.shortcutControl->applyTheme(scheme);
            }
            if (runtime.globalMouseControl != nullptr) {
                runtime.globalMouseControl->applyTheme(scheme);
            }
            if (runtime.customControl != nullptr) {
                runtime.customControl->applyTheme(scheme);
            }
        }
        requestVisibleSectionSync();
        q.update();
    }

    int sectionTop(const RuntimeSection& section) const {
        if (section.header == nullptr || contentWidget == nullptr) {
            return 0;
        }
        return section.header->mapTo(contentWidget, QPoint(0, 0)).y();
    }

    int sectionViewportInset() const {
        return contentLayout != nullptr ? contentLayout->contentsMargins().top() : 0;
    }

    QVector<QWidget*> tabWidgets(QWidget* parent) const {
        QVector<QWidget*> result;
        QWidget* widget = parent;
        do {
            if ((widget == parent || parent->isAncestorOf(widget)) &&
                (widget->focusPolicy() & Qt::TabFocus) != 0 && widget->focusProxy() == nullptr) {
                result.push_back(widget);
            }
            widget = widget->nextInFocusChain();
        } while (widget != parent);
        return result;
    }

    void rebuildTabOrder() {
        QWidget* previous = nullptr;
        for (const RuntimeSection& section : sections) {
            for (QWidget* parent : {static_cast<QWidget*>(section.header), section.list}) {
                const auto widgets = tabWidgets(parent);
                for (QWidget* widget : widgets) {
                    if (previous != nullptr) {
                        QWidget::setTabOrder(previous, widget);
                    }
                    previous = widget;
                }
            }
        }
    }

    void materializeCustom(RuntimeItem& item) {
        if (!item.deferredRenderer.has_value()) {
            return;
        }
        item.customControl = createSettingsCustomWidget(
            *item.deferredRenderer, registry, *item.definition, runtimeSession, item.anchor);
        Q_ASSERT(item.customControl != nullptr);
        item.deferredRenderer.reset();
        item.anchor->setFocusPolicy(Qt::NoFocus);
        item.anchor->setMinimumHeight(0);
        item.anchor->setMaximumHeight(QWIDGETSIZE_MAX);
        auto* layout = new QVBoxLayout(item.anchor);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(item.customControl);
        item.customControl->show();
        rebuildTabOrder();
    }

    void settleLayout() {
        if (contentWidget == nullptr || scrollArea == nullptr) {
            return;
        }
        // Match AdScrollArea's width-dependent sizing without pumping unrelated
        // events while controls are being created or a search target is revealed.
        const int width = scrollArea->viewport()->width();
        const int fitted = contentLayout->totalHeightForWidth(width);
        contentWidget->resize(width,
                              qMax(0, fitted >= 0 ? fitted : contentLayout->sizeHint().height()));
        contentLayout->activate();
    }

    void materializeVisibleSections() {
        if (!q.isVisible() || materializing || scrollArea == nullptr || sections.isEmpty()) {
            return;
        }
        const QScopedValueRollback<bool> guard(materializing, true);
        settleLayout();
        auto* bar = scrollArea->verticalScrollBar();
        const int viewportHeight = scrollArea->viewport()->height();
        // One control-height of overscan hides creation during ordinary scrolling,
        // without constructing another entire screen of expensive editors.
        const int overscan = colorScheme.metricAlias.controlHeight;
        for (RuntimeSection& section : sections) {
            if (section.materialized) {
                continue;
            }
            const int top = section.list->y();
            if (top > bar->value() + viewportHeight + overscan ||
                top + section.list->height() < bar->value() - overscan) {
                continue;
            }
            // Keep the section currently at the top of the viewport stationary
            // when an estimated height above it is replaced with its actual height.
            RuntimeSection* anchor = &sections.first();
            for (RuntimeSection& candidate : sections) {
                if (sectionTop(candidate) > bar->value()) {
                    break;
                }
                anchor = &candidate;
            }
            const int offset = bar->value() - sectionTop(*anchor);
            const bool atBottom = bar->maximum() > 0 && bar->value() == bar->maximum();
            materializeSection(section);
            settleLayout();
            bar->setValue(atBottom ? bar->maximum() : sectionTop(*anchor) + offset);
        }
        for (RuntimeItem& item : items) {
            if (!item.deferredRenderer.has_value()) {
                continue;
            }
            const int top = item.anchor->mapTo(contentWidget, QPoint()).y();
            if (top > bar->value() + viewportHeight + overscan ||
                top + item.anchor->height() < bar->value() - overscan) {
                continue;
            }
            RuntimeSection* anchor = &sections.first();
            for (RuntimeSection& candidate : sections) {
                if (sectionTop(candidate) > bar->value()) {
                    break;
                }
                anchor = &candidate;
            }
            const int offset = bar->value() - sectionTop(*anchor);
            const bool atBottom = bar->maximum() > 0 && bar->value() == bar->maximum();
            materializeCustom(item);
            settleLayout();
            bar->setValue(atBottom ? bar->maximum() : sectionTop(*anchor) + offset);
        }
    }

    bool filterEvent(QObject* watched, QEvent* event) {
        if ((watched == contentWidget && event->type() == QEvent::LayoutRequest) ||
            (scrollArea != nullptr && watched == scrollArea->viewport() &&
             event->type() == QEvent::Resize)) {
            requestVisibleSectionSync();
        }
        if (event->type() == QEvent::FocusIn) {
            for (RuntimeItem& item : items) {
                if (watched == item.anchor && item.deferredRenderer.has_value()) {
                    materializeCustom(item);
                    settleLayout();
                    focusContent(item.anchor, static_cast<QFocusEvent*>(event)->reason());
                    return true;
                }
            }
            for (RuntimeSection& section : sections) {
                if (watched != section.list || section.materialized) {
                    continue;
                }
                const auto reason = static_cast<QFocusEvent*>(event)->reason();
                materializeSection(section);
                settleLayout();
                focusContent(section.list, reason);
                return true;
            }
        }
        return false;
    }

    void focusContent(QWidget* parent, Qt::FocusReason reason) {
        const auto widgets = tabWidgets(parent);
        const bool backwards = reason == Qt::BacktabFocusReason;
        for (int i = 0; i < widgets.size(); ++i) {
            QWidget* widget = widgets.at(backwards ? widgets.size() - 1 - i : i);
            if (widget->isEnabled() && widget->isVisibleTo(&q)) {
                widget->setFocus(reason);
                scrollArea->ensureWidgetVisible(widget, scrollMarginX, scrollMarginY);
                return;
            }
        }
        q.focusNextPrevChild(!backwards);
    }

    void requestVisibleSectionSync() {
        if (visibleSectionSyncPending) {
            return;
        }
        visibleSectionSyncPending = true;
        QTimer::singleShot(0, &q, [this]() {
            visibleSectionSyncPending = false;
            materializeVisibleSections();
            syncVisibleSection();
        });
    }

    QString visibleSectionId() const {
        if (sections.isEmpty() || scrollArea == nullptr ||
            scrollArea->verticalScrollBar() == nullptr) {
            return {};
        }

        const QScrollBar* scrollBar = scrollArea->verticalScrollBar();
        int activeIndex = 0;
        if (scrollBar->maximum() > scrollBar->minimum() &&
            scrollBar->value() >= scrollBar->maximum()) {
            activeIndex = static_cast<int>(sections.size()) - 1;
        } else {
            const int activationLine = scrollBar->value() + sectionViewportInset() + 1;
            for (int index = 1; index < sections.size(); ++index) {
                if (sectionTop(sections.at(index)) > activationLine) {
                    break;
                }
                activeIndex = index;
            }
        }

        const RuntimeSection& section = sections.at(activeIndex);
        return section.definition != nullptr ? section.definition->id : QString();
    }

    void syncVisibleSection() {
        if (suppressVisibleSectionTracking || scrollArea == nullptr ||
            scrollArea->verticalScrollBar() == nullptr ||
            scrollArea->verticalScrollBar()->maximum() <=
                scrollArea->verticalScrollBar()->minimum()) {
            return;
        }
        const QString sectionId = visibleSectionId();
        if (sectionId.isEmpty() || sectionId == lastVisibleSectionId) {
            return;
        }
        lastVisibleSectionId = sectionId;
        emit q.visibleSectionChanged(sectionId);
    }

    void scrollToSection(const RuntimeSection& section) {
        if (scrollArea == nullptr || scrollArea->verticalScrollBar() == nullptr) {
            return;
        }
        QScrollBar* scrollBar = scrollArea->verticalScrollBar();
        const int target = sectionTop(section) - sectionViewportInset();
        scrollBar->setValue(qBound(scrollBar->minimum(), target, scrollBar->maximum()));
    }

    void reveal(const settings::SettingsLocation& requested) {
        if (page == nullptr) {
            return;
        }
        const settings::SettingsLocation location = catalog.resolveLocation(requested);
        if (location.pageId != page->id || scrollArea == nullptr) {
            return;
        }
        if (!requested.sectionId.isEmpty() || !requested.itemId.isEmpty()) {
            if (RuntimeSection* section = runtimeSection(location.sectionId)) {
                materializeSection(*section);
                if (RuntimeItem* item = runtimeItem(location.itemId)) {
                    materializeCustom(*item);
                }
                settleLayout();
            }
        }
        QWidget* target = nullptr;
        QWidget* focus = nullptr;
        RuntimeSection* targetSection = nullptr;
        const bool pageLevelNavigation =
            requested.sectionId.isEmpty() && requested.itemId.isEmpty();
        if (!pageLevelNavigation && !location.itemId.isEmpty()) {
            if (RuntimeItem* item = runtimeItem(location.itemId)) {
                target = item->anchor;
                focus = item->focusTarget;
            }
        }
        if (!pageLevelNavigation && target == nullptr) {
            targetSection = runtimeSection(location.sectionId);
            if (targetSection != nullptr) {
                target = targetSection->header;
            }
        }
        const bool previousSuppression = suppressVisibleSectionTracking;
        suppressVisibleSectionTracking = true;
        if (pageLevelNavigation) {
            QScrollBar* scrollBar = scrollArea->verticalScrollBar();
            scrollBar->setValue(scrollBar->minimum());
        } else if (target != nullptr) {
            if (targetSection != nullptr) {
                scrollToSection(*targetSection);
            } else {
                scrollArea->ensureWidgetVisible(target, scrollMarginX, scrollMarginY);
            }
        }
        // Finish viewport materialization before handing focus to an editor.
        // A later geometry change must not dismiss a popup opened at the target.
        materializeVisibleSections();
        if (targetSection != nullptr) {
            scrollToSection(*targetSection);
        } else if (target != nullptr) {
            scrollArea->ensureWidgetVisible(target, scrollMarginX, scrollMarginY);
        }
        if (focus != nullptr) {
            QWidget* focusWidget = focus->focusProxy() != nullptr ? focus->focusProxy() : focus;
            if (focusWidget->focusPolicy() != Qt::NoFocus) {
                focusWidget->setFocus(Qt::ShortcutFocusReason);
            }
        }
        suppressVisibleSectionTracking = previousSuppression;
        lastVisibleSectionId = visibleSectionId();
    }

    SettingsPageWidget& q;
    const settings::SettingsRegistry& registry;
    const settings::SettingsCatalog& catalog;
    settings::SettingsRuntimeSession& runtimeSession;
    const settings::SettingsPagePlan* pagePlan = nullptr;
    const settings::SettingsPageDefinition* page = nullptr;
    adqt::widgets::AdScrollArea* scrollArea = nullptr;
    QWidget* contentWidget = nullptr;
    QVBoxLayout* contentLayout = nullptr;
    QVector<RuntimeSection> sections;
    QVector<RuntimeItem> items;
    QHash<QString, int> sectionIndexes;
    QHash<QString, int> itemIndexes;
    snow_shot::presentation::styles::ThemeColorScheme colorScheme;
    int scrollMarginX = 0;
    int scrollMarginY = 0;
    QString lastVisibleSectionId;
    bool suppressVisibleSectionTracking = false;
    bool visibleSectionSyncPending = false;
    bool initialized = false;
    bool materializing = false;
    QPointer<adqt::widgets::AdAlert> permissionBanner;
    int contentTopMarginWithoutPermissionBanner = 0;
    QPointer<adqt::widgets::AdButton> permissionButton;
};

SettingsPageWidget::SettingsPageWidget(
    const snow_shot::presentation::settings::SettingsRegistry& registry, const QString& pageId,
    snow_shot::presentation::settings::SettingsRuntimeSession& runtimeSession, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>(*this, registry, pageId, runtimeSession)) {
    if (m_impl->scrollArea != nullptr) {
        m_impl->scrollArea->viewport()->installEventFilter(this);
        m_impl->contentWidget->installEventFilter(this);
        for (const auto& section : m_impl->sections) {
            section.list->installEventFilter(this);
        }
        for (const auto& item : m_impl->items) {
            if (item.deferredRenderer.has_value()) {
                item.anchor->installEventFilter(this);
            }
        }
    }
}

SettingsPageWidget::~SettingsPageWidget() = default;

QString SettingsPageWidget::pageId() const {
    return m_impl->page != nullptr ? m_impl->page->id : QString();
}

void SettingsPageWidget::reveal(
    const snow_shot::presentation::settings::SettingsLocation& location) {
    m_impl->reveal(location);
}

void SettingsPageWidget::applyTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    m_impl->applyTheme(scheme);
}

void SettingsPageWidget::retranslateUi() {
    m_impl->retranslateUi();
}

void SettingsPageWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void SettingsPageWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_impl->runtimeSession.refreshPlatformSettings();
    m_impl->observePermissionPage(true);
    m_impl->requestVisibleSectionSync();
}

bool SettingsPageWidget::eventFilter(QObject* watched, QEvent* event) {
    if (m_impl != nullptr && m_impl->filterEvent(watched, event)) {
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void SettingsPageWidget::hideEvent(QHideEvent* event) {
    m_impl->observePermissionPage(false);
    QWidget::hideEvent(event);
}
