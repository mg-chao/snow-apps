#include "snow_shot/presentation/components/settingspagewidget.h"

#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/components/sectionheaderwidget.h"
#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/presentation/components/settingspageutils.h"
#include "snow_shot/presentation/components/shortcutkeyrow.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/configurationschema.h"

#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/input_number.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_search_edit.h"
#include "widgets/modal.h"
#include "widgets/multi_select.h"
#include "widgets/radio.h"
#include "widgets/radio_button_group.h"
#include "widgets/scroll_area.h"
#include "widgets/select.h"
#include "widgets/slider.h"
#include "widgets/switch.h"

#include <QAbstractButton>
#include <QEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QStyle>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace {
namespace settings = snow_shot::presentation::settings;
namespace settings_ui = snow_shot::presentation::components;

adqt::widgets::AdSelect::Option selectOption(const QVariant& value, const QString& label) {
    adqt::widgets::AdSelect::Option result;
    result.value = value;
    result.label = label;
    return result;
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
        adqt::widgets::AdSelect* select = nullptr;
        adqt::widgets::AdMultiSelect* multiSelect = nullptr;
        adqt::widgets::AdSwitch* switchControl = nullptr;
        adqt::widgets::AdInputNumber* integerControl = nullptr;
        adqt::widgets::AdSlider* sliderControl = nullptr;
        QLabel* sliderValue = nullptr;
        adqt::widgets::AdColorPicker* colorControl = nullptr;
        adqt::widgets::AdRadioButtonGroup* radioGroup = nullptr;
        QVector<adqt::widgets::AdRadio*> radioButtons;
        QVector<QVariant> radioValues;
        adqt::widgets::AdSearchEdit* filePathControl = nullptr;
        adqt::widgets::AdSearchEdit* directoryPathControl = nullptr;
        adqt::widgets::AdLineEdit* textControl = nullptr;
        ShortcutKeyRow* shortcutControl = nullptr;
        adqt::widgets::AdButton* actionControl = nullptr;
        SettingsCustomWidget* customControl = nullptr;
        QPointer<adqt::widgets::AdModal> modal;
    };

    struct RuntimeSection {
        const settings::SettingsSectionDefinition* definition = nullptr;
        SectionHeaderWidget* header = nullptr;
        settings::SettingsSectionReset reset = settings::SettingsSectionReset::None;
        settings::SettingsSectionItemLayout itemLayout =
            settings::SettingsSectionItemLayout::VerticalList;
    };

    Impl(SettingsPageWidget& owner, const settings::SettingsRegistry& sourceRegistry,
         const QString& sourcePageId, settings::SettingsRuntimeSession& sourceRuntimeSession)
        : q(owner), registry(sourceRegistry), catalog(sourceRegistry.catalog()),
          runtimeSession(sourceRuntimeSession), pagePlan(sourceRegistry.pagePlan(sourcePageId)),
          page(catalog.page(sourcePageId)),
          colorScheme(snow_shot::presentation::styles::ThemeManager::instance()
                          .themeColorScheme()) {
        Q_ASSERT(page == nullptr || page->id == sourcePageId);
        build();
        connectServices();
        retranslateUi();
        syncValues();
        applyTheme(colorScheme);
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
        const QVector<int>* plannedFieldIndexes =
            sectionPlan != nullptr ? &sectionPlan->fieldIndexes : nullptr;
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
        contentLayout->addWidget(runtimeSection.header);
        sections.push_back(runtimeSection);
        sectionIndexes.insert(sectionDefinition.id, sections.size() - 1);

        auto* list = new QWidget(contentWidget);
        list->setObjectName(settings::generatedObjectName(
            QStringLiteral("settings-section-list"),
            QStringLiteral("%1-%2").arg(page->id, sectionDefinition.id)));
        list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* listLayout = new QVBoxLayout(list);
        listLayout->setContentsMargins(0, 0, 0, 0);
        const bool twoColumnItemGrid = itemLayout == settings::SettingsSectionItemLayout::TwoColumnGrid;
        listLayout->setSpacing(twoColumnItemGrid
                                   ? 0
                                   : (page->id == QStringLiteral("quick-functions")
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
                if (descriptor.pageId != page->id ||
                    descriptor.sectionId != sectionDefinition.id ||
                    descriptor.definition == nullptr) {
                    continue;
                }
                buildItem(*descriptor.definition, &descriptor, fieldIndex, list, listLayout,
                          itemGrid, itemIndex);
            }
        }
        contentLayout->addWidget(list);
    }

    void buildItem(const settings::SettingsItemDefinition& definition,
                   const settings::SettingsFieldDescriptor* descriptor, int fieldIndex,
                   QWidget* list, QVBoxLayout* listLayout,
                   QGridLayout* itemGrid = nullptr, int itemIndex = 0) {
        RuntimeItem runtime;
        runtime.descriptor = descriptor;
        runtime.definition = &definition;

        const auto addItemWidget = [listLayout, itemGrid, itemIndex](QWidget* widget) {
            if (itemGrid != nullptr) {
                itemGrid->addWidget(widget, itemIndex / 2, itemIndex % 2);
            } else {
                listLayout->addWidget(widget);
            }
        };

        std::visit(
            [&](const auto& payload) {
                using Payload = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, settings::SettingsSelectDefinition>) {
                    auto* select = new adqt::widgets::AdSelect(list);
                    select->setMode(adqt::widgets::AdSelect::Mode::Single);
                    select->setControlSize(adqt::widgets::AdSelect::ControlSize::Middle);
                    select->setFixedWidth(settings_ui::settingsControlWidth(colorScheme.metricAlias));
                    runtime.select = select;
                    runtime.focusTarget = select;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description, select,
                        settings::generatedObjectName(QStringLiteral("settings-item"), definition.id));
                    addItemWidget(runtime.anchor);
                    connect(select, &adqt::widgets::AdSelect::currentValueChanged, &q,
                            [this, itemId = definition.id](const QVariant& value) {
                                applySelectValue(itemId, value);
                            });
                } else if constexpr (std::is_same_v<
                                         Payload, settings::SettingsMultiSelectDefinition>) {
                    auto* control = new adqt::widgets::AdMultiSelect(list);
                    control->setControlSize(
                        adqt::widgets::AdMultiSelect::ControlSize::Middle);
                    control->setResponsiveMaxTagCount(true);
                    control->setFixedWidth(
                        settings_ui::settingsControlWidth(colorScheme.metricAlias));
                    runtime.multiSelect = control;
                    runtime.focusTarget = control;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description,
                        control, settings::generatedObjectName(QStringLiteral("settings-item"),
                                                               definition.id));
                    addItemWidget(runtime.anchor);
                    connect(control, &adqt::widgets::AdMultiSelect::selectedValuesChanged, &q,
                            [this, binding = payload.binding](const QVariantList& value) {
                                if (!synchronizingValues &&
                                    !runtimeSession.applyMultiSelectValue(binding, value)) {
                                    syncValues();
                                }
                            });
                } else if constexpr (std::is_same_v<Payload,
                                                    settings::SettingsSwitchDefinition>) {
                    auto* control = new adqt::widgets::AdSwitch(list);
                    control->setControlSize(adqt::widgets::AdSwitch::ControlSize::Medium);
                    runtime.switchControl = control;
                    runtime.focusTarget = control;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description, control,
                        settings::generatedObjectName(QStringLiteral("settings-item"), definition.id));
                    addItemWidget(runtime.anchor);
                    connect(control, &QAbstractButton::toggled, &q,
                            [this, binding = payload.binding](bool checked) {
                                applySwitchValue(binding, checked);
                            });
                } else if constexpr (std::is_same_v<Payload,
                                                     settings::SettingsIntegerDefinition>) {
                    auto* control = new adqt::widgets::AdInputNumber(list);
                    control->setControlSize(adqt::widgets::AdInputNumber::ControlSize::Medium);
                    control->setVariant(adqt::widgets::AdInputNumber::Variant::Outlined);
                    control->setStepButtonLayout(
                        adqt::widgets::AdInputNumber::StepButtonLayout::Compact);
                    control->setDecimals(0);
                    control->setFixedWidth(settings_ui::settingsControlWidth(colorScheme.metricAlias));
                    const auto* schemaEntry = snow_shot::storage::ConfigurationSchema::entry(
                        definition.configurationKey);
                    Q_ASSERT(schemaEntry != nullptr && schemaEntry->integerRange.has_value());
                    control->setRange(schemaEntry->integerRange->minimum,
                                      schemaEntry->integerRange->maximum);
                    control->setSingleStep(schemaEntry->integerRange->step);
                    runtime.integerControl = control;
                    runtime.focusTarget = control;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description, control,
                        settings::generatedObjectName(QStringLiteral("settings-item"), definition.id));
                    addItemWidget(runtime.anchor);
                    connect(control, &adqt::widgets::AdInputNumber::valueChanged, &q,
                            [this, binding = payload.binding](double value) {
                                applyIntegerValue(binding, static_cast<int>(value));
                            });
                } else if constexpr (std::is_same_v<Payload,
                                                     settings::SettingsSliderDefinition>) {
                    auto* container = new QWidget(list);
                    auto* layout = new QHBoxLayout(container);
                    layout->setContentsMargins(0, 0, 0, 0);
                    layout->setSpacing(0);
                    auto* control = new adqt::widgets::AdSlider(container);
                    const auto* schemaEntry = snow_shot::storage::ConfigurationSchema::entry(
                        definition.configurationKey);
                    Q_ASSERT(schemaEntry != nullptr && schemaEntry->integerRange.has_value());
                    control->setRange(schemaEntry->integerRange->minimum,
                                      schemaEntry->integerRange->maximum);
                    control->setSingleStep(schemaEntry->integerRange->step);
                    control->setPageStep(std::max(schemaEntry->integerRange->step, 10));
                    control->setTooltipEnabled(true);
                    auto* valueLabel = new QLabel(container);
                    valueLabel->setMinimumWidth(colorScheme.metricAlias.controlHeightLG);
                    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    layout->addWidget(control, 1);
                    layout->addWidget(valueLabel);
                    container->setFixedWidth(
                        settings_ui::settingsControlWidth(colorScheme.metricAlias));
                    runtime.sliderControl = control;
                    runtime.sliderValue = valueLabel;
                    runtime.focusTarget = control;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description,
                        container,
                        settings::generatedObjectName(QStringLiteral("settings-item"),
                                                      definition.id));
                    addItemWidget(runtime.anchor);
                    connect(control, &adqt::widgets::AdSlider::valueChanged, &q,
                            [this, binding = payload.binding, valueLabel,
                             suffix = payload.suffix](double value) {
                                const int integerValue = qRound(value);
                                valueLabel->setText(
                                    QStringLiteral("%1%2")
                                        .arg(integerValue)
                                        .arg(suffix.translated()));
                                if (!synchronizingValues &&
                                    !runtimeSession.applySliderValue(binding, integerValue)) {
                                    syncValues();
                                }
                            });
                } else if constexpr (std::is_same_v<Payload,
                                                     settings::SettingsColorDefinition>) {
                    auto* control = new adqt::widgets::AdColorPicker(list);
                    control->setSize(adqt::widgets::AdColorPicker::Size::Middle);
                    control->setModeOptions({adqt::widgets::AdColorPicker::Mode::Solid});
                    control->setMode(adqt::widgets::AdColorPicker::Mode::Solid);
                    control->setFormat(adqt::widgets::AdColorPicker::Format::Hex);
                    control->setAlphaChannelEnabled(payload.alphaChannelEnabled);
                    control->setAllowClear(false);
                    control->setTriggerTextVisible(true);
                    control->setFixedWidth(
                        settings_ui::settingsControlWidth(colorScheme.metricAlias));
                    if (auto* layout = qobject_cast<QHBoxLayout*>(control->layout());
                        layout != nullptr && layout->count() > 0 &&
                        layout->itemAt(0)->widget() != nullptr) {
                        layout->setAlignment(layout->itemAt(0)->widget(),
                                             Qt::AlignRight | Qt::AlignVCenter);
                    }
                    runtime.colorControl = control;
                    runtime.focusTarget = control;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description,
                        control,
                        settings::generatedObjectName(QStringLiteral("settings-item"),
                                                      definition.id));
                    addItemWidget(runtime.anchor);
                    connect(control, &adqt::widgets::AdColorPicker::valueChanged, &q,
                            [this, binding = payload.binding](
                                const adqt::widgets::AdColorValue& value) {
                                if (!synchronizingValues && value.isSolid() &&
                                    !runtimeSession.applyColorValue(binding,
                                                                    value.solidColor)) {
                                    syncValues();
                                }
                            });
                } else if constexpr (std::is_same_v<Payload,
                                                     settings::SettingsRadioDefinition>) {
                    auto* container = new QWidget(list);
                    auto* layout = new QHBoxLayout(container);
                    layout->setContentsMargins(0, 0, 0, 0);
                    layout->setSpacing(0);
                    auto* radioList = new QWidget(container);
                    radioList->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
                    auto* radioLayout = new QVBoxLayout(radioList);
                    radioLayout->setContentsMargins(0, 0, 0, 0);
                    radioLayout->setSpacing(colorScheme.metricAlias.marginXS);
                    auto* group = new adqt::widgets::AdRadioButtonGroup(container);
                    group->setManagedLayout(radioLayout);
                    group->setControlSize(adqt::widgets::AdRadio::ControlSize::Small);
                    for (int index = 0; index < payload.options.size(); ++index) {
                        const settings::SettingsRadioOptionDefinition& option =
                            payload.options.at(index);
                        auto* radio = new adqt::widgets::AdRadio(radioList);
                        radio->setIcon(QIcon(option.iconResource));
                        radio->setIconSize(QSize(24, 24));
                        group->addButton(radio, index);
                        radioLayout->addWidget(radio);
                        runtime.radioButtons.push_back(radio);
                        runtime.radioValues.push_back(option.value);
                    }
                    layout->addStretch(1);
                    layout->addWidget(radioList, 0, Qt::AlignRight);
                    container->setFixedWidth(
                        settings_ui::settingsControlWidth(colorScheme.metricAlias));
                    runtime.radioGroup = group;
                    runtime.focusTarget = runtime.radioButtons.isEmpty()
                                              ? static_cast<QWidget*>(container)
                                              : runtime.radioButtons.constFirst();
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description,
                        container,
                        settings::generatedObjectName(QStringLiteral("settings-item"),
                                                      definition.id));
                    addItemWidget(runtime.anchor);
                    connect(group, &adqt::widgets::AdRadioButtonGroup::checkedIdChanged, &q,
                            [this, binding = payload.binding,
                             values = runtime.radioValues](int id) {
                                if (!synchronizingValues && id >= 0 && id < values.size() &&
                                    !runtimeSession.applyRadioValue(binding,
                                                                    values.at(id))) {
                                    syncValues();
                                }
                            });
                } else if constexpr (std::is_same_v<Payload,
                                                     settings::SettingsFilePathDefinition>) {
                    auto* control = new adqt::widgets::AdSearchEdit(list);
                    control->setControlSize(adqt::widgets::AdSearchEdit::ControlSize::Medium);
                    control->setAllowClear(true);
                    control->setFixedWidth(
                        settings_ui::settingsControlWidth(colorScheme.metricAlias));
                    runtime.filePathControl = control;
                    runtime.focusTarget = control;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description,
                        control,
                        settings::generatedObjectName(QStringLiteral("settings-item"),
                                                      definition.id));
                    addItemWidget(runtime.anchor);
                    connect(control, &adqt::widgets::AdSearchEdit::editingFinished, &q,
                            [this, control, binding = payload.binding]() {
                                if (!synchronizingValues &&
                                    !runtimeSession.applyFilePathValue(binding,
                                                                        control->text())) {
                                    syncValues();
                                }
                            });
                    connect(control, &adqt::widgets::AdSearchEdit::searchRequested, &q,
                            [this, control, binding = payload.binding,
                             dialogTitle = payload.dialogTitle,
                             fileFilter = payload.fileFilter](
                                const QString& text,
                                adqt::widgets::AdSearchEdit::SearchReason reason) {
                                if (reason ==
                                    adqt::widgets::AdSearchEdit::SearchReason::ButtonClick) {
                                    const QString path = QFileDialog::getOpenFileName(
                                        &q, dialogTitle.translated(), text,
                                        fileFilter.translated());
                                    if (!path.isEmpty()) {
                                        control->setText(path);
                                        if (!runtimeSession.applyFilePathValue(binding, path)) {
                                            syncValues();
                                        }
                                    }
                                } else if (reason ==
                                           adqt::widgets::AdSearchEdit::SearchReason::ClearAction) {
                                    if (!runtimeSession.applyFilePathValue(binding, QString())) {
                                        syncValues();
                                    }
                                }
                            });
                } else if constexpr (std::is_same_v<
                                         Payload, settings::SettingsDirectoryPathDefinition>) {
                    auto* control = new adqt::widgets::AdSearchEdit(list);
                    control->setControlSize(adqt::widgets::AdSearchEdit::ControlSize::Medium);
                    control->setAllowClear(true);
                    control->setFixedWidth(
                        settings_ui::settingsControlWidth(colorScheme.metricAlias));
                    runtime.directoryPathControl = control;
                    runtime.focusTarget = control;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description,
                        control,
                        settings::generatedObjectName(QStringLiteral("settings-item"),
                                                      definition.id));
                    addItemWidget(runtime.anchor);
                    connect(control, &adqt::widgets::AdSearchEdit::editingFinished, &q,
                            [this, control, binding = payload.binding]() {
                                if (!synchronizingValues &&
                                    !runtimeSession.applyDirectoryPathValue(binding,
                                                                             control->text())) {
                                    syncValues();
                                }
                            });
                    connect(control, &adqt::widgets::AdSearchEdit::searchRequested, &q,
                            [this, control, binding = payload.binding,
                             dialogTitle = payload.dialogTitle](
                                const QString& text,
                                adqt::widgets::AdSearchEdit::SearchReason reason) {
                                if (reason ==
                                    adqt::widgets::AdSearchEdit::SearchReason::ButtonClick) {
                                    const QString path = QFileDialog::getExistingDirectory(
                                        &q, dialogTitle.translated(), text);
                                    if (!path.isEmpty()) {
                                        control->setText(path);
                                        if (!runtimeSession.applyDirectoryPathValue(binding,
                                                                                     path)) {
                                            syncValues();
                                        }
                                    }
                                } else if (reason ==
                                           adqt::widgets::AdSearchEdit::SearchReason::ClearAction) {
                                    if (!runtimeSession.applyDirectoryPathValue(binding,
                                                                                 QString())) {
                                        syncValues();
                                    }
                                }
                            });
                } else if constexpr (std::is_same_v<Payload,
                                                     settings::SettingsTextDefinition>) {
                    auto* control = new adqt::widgets::AdLineEdit(list);
                    control->setControlSize(adqt::widgets::AdLineEdit::ControlSize::Medium);
                    control->setAllowClear(false);
                    control->setFixedWidth(
                        settings_ui::settingsControlWidth(colorScheme.metricAlias));
                    runtime.textControl = control;
                    runtime.focusTarget = control;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description,
                        control,
                        settings::generatedObjectName(QStringLiteral("settings-item"),
                                                      definition.id));
                    addItemWidget(runtime.anchor);
                    connect(control, &QLineEdit::editingFinished, &q,
                            [this, control, binding = payload.binding]() {
                                if (!synchronizingValues &&
                                    !runtimeSession.applyTextValue(binding, control->text())) {
                                    syncValues();
                                }
                            });
                } else if constexpr (std::is_same_v<
                                         Payload, settings::SettingsShortcutActionDefinition>) {
                    const auto shortcutState = runtimeSession.shortcutState(payload.shortcutAction);
                    const auto metric = colorScheme.metricAlias;
                    const auto mainWindowMetric =
                        snow_shot::presentation::styles::buildMainWindowComponentMetricToken(
                            colorScheme);
                    ShortcutKeyRowConfig config{
                        definition.title.translated(),
                        payload.iconFactory ? payload.iconFactory() : adqt::icons::IconRef(),
                        shortcutState.shortcuts,
                        shortcutState,
                        QStringLiteral("normal"),
                        true,
                        2,
                        [this](const QString& shortcut) {
                            return runtimeSession.validateShortcut(shortcut);
                        },
                        payload.adjustment == settings::SettingsShortcutAdjustment::ScreenshotDelaySeconds,
                        payload.adjustment == settings::SettingsShortcutAdjustment::ScreenshotDelaySeconds
                            ? runtimeSession.integerValue(
                                  settings::SettingsIntegerBinding::ScreenshotDelaySeconds)
                            : 3,
                        [this](int value) {
                            return runtimeSession.applyIntegerValue(
                                settings::SettingsIntegerBinding::ScreenshotDelaySeconds, value);
                        },
                    };
                    auto* control = new ShortcutKeyRow(config, metric, mainWindowMetric, list);
                    control->setObjectName(settings::generatedObjectName(
                        QStringLiteral("settings-item"), definition.id));
                    runtime.shortcutControl = control;
                    runtime.focusTarget = control;
                    runtime.anchor = control;
                    addItemWidget(control);
                    connect(control, &ShortcutKeyRow::clicked, &q,
                            [this, command = payload.command]() { emit q.commandRequested(command); });
                    connect(control, &ShortcutKeyRow::shortcutsChanged, &q,
                            [this, action = payload.shortcutAction](const QStringList& shortcuts) {
                                 if (!runtimeSession.applyShortcuts(action, shortcuts)) {
                                     syncValues();
                                 }
                            });
                } else if constexpr (std::is_same_v<
                                         Payload, settings::SettingsLocalShortcutDefinition>) {
                    const QStringList shortcuts =
                        runtimeSession.localShortcuts(payload.scope, payload.shortcutId);
                    snow_shot::presentation::GlobalShortcutRegistrationState displayState;
                    displayState.shortcuts = shortcuts;
                    displayState.status = shortcuts.isEmpty()
                                              ? snow_shot::presentation::GlobalShortcutStatus::Unset
                                              : snow_shot::presentation::GlobalShortcutStatus::Registered;
                    const auto metric = colorScheme.metricAlias;
                    const auto mainWindowMetric =
                        snow_shot::presentation::styles::buildMainWindowComponentMetricToken(
                            colorScheme);
                    ShortcutKeyRowConfig config;
                    config.title = definition.title.translated();
                    config.iconRef = payload.iconFactory ? payload.iconFactory()
                                                         : adqt::icons::IconRef();
                    config.shortcuts = shortcuts;
                    config.registrationState = displayState;
                    config.rowState = QStringLiteral("normal");
                    config.useStableBorder = false;
                    config.maxShortcutCount = 2;
                    config.shortcutValidator =
                        [this, scope = payload.scope,
                         shortcutId = payload.shortcutId](const QString& shortcut) {
                            return runtimeSession.validateLocalShortcut(scope, shortcutId,
                                                                         shortcut);
                        };
                    config.showRegistrationStatus = false;
                    config.validationScope =
                        payload.scope == settings::SettingsLocalShortcutScope::Screenshot
                            ? ShortcutKeyRowConfig::ValidationScope::ScreenshotShortcut
                            : payload.scope == settings::SettingsLocalShortcutScope::Drawing
                                  ? ShortcutKeyRowConfig::ValidationScope::DrawingShortcut
                                  : ShortcutKeyRowConfig::ValidationScope::PinnedWindowShortcut;
                    config.presentation =
                        ShortcutKeyRowConfig::Presentation::CompactFormField;
                    auto* control = new ShortcutKeyRow(config, metric, mainWindowMetric, list);
                    control->setObjectName(settings::generatedObjectName(
                        QStringLiteral("settings-item"), definition.id));
                    runtime.shortcutControl = control;
                    runtime.focusTarget = control;
                    runtime.anchor = control;
                    addItemWidget(control);
                    connect(control, &ShortcutKeyRow::shortcutsChanged, &q,
                            [this, scope = payload.scope,
                             shortcutId = payload.shortcutId](const QStringList& next) {
                                if (!runtimeSession.applyLocalShortcuts(scope, shortcutId, next)) {
                                    syncValues();
                                }
                            });
                } else if constexpr (std::is_same_v<Payload,
                                                    settings::SettingsActionDefinition>) {
                    auto* control = new adqt::widgets::AdButton(list);
                    control->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
                    control->setAccentRole(
                        payload.accent == settings::SettingsActionAccent::Danger
                            ? adqt::widgets::AdButton::AccentRole::Danger
                            : adqt::widgets::AdButton::AccentRole::Neutral);
                    control->setSizeClass(adqt::widgets::AdButton::SizeClass::Medium);
                    if (payload.iconFactory) {
                        control->setIconRef(payload.iconFactory());
                    }
                    runtime.actionControl = control;
                    runtime.focusTarget = control;
                    runtime.anchor = settings_ui::createSettingItemRow(
                        list, colorScheme.metricAlias, &runtime.title, &runtime.description, control,
                        settings::generatedObjectName(QStringLiteral("settings-item"), definition.id));
                    addItemWidget(runtime.anchor);
                    connect(control, &QAbstractButton::clicked, &q,
                            [this, itemId = definition.id]() { triggerAction(itemId); });
                } else if constexpr (std::is_same_v<Payload,
                                                    settings::SettingsCustomDefinition>) {
                    auto* control =
                        createSettingsCustomWidget(payload.renderer, registry, definition,
                                                   runtimeSession, list);
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

        if (runtime.focusTarget != nullptr && runtime.focusTarget != runtime.anchor) {
            runtime.focusTarget->setObjectName(settings::generatedObjectName(
                QStringLiteral("settings-control"), definition.id));
        }
        if (descriptor != nullptr && runtime.anchor != nullptr) {
            runtime.anchor->setProperty("settingsFieldIndex", fieldIndex);
            runtime.anchor->setProperty("settingsFieldKind",
                                        static_cast<int>(descriptor->kind));
            runtime.anchor->setProperty("settingsProviderId", descriptor->providerId);
        }
        items.push_back(runtime);
        itemIndexes.insert(definition.id, items.size() - 1);
    }

    void connectServices() {
        auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
        QObject::connect(&themeManager,
                         &snow_shot::presentation::styles::ThemeManager::themeChanged, &q,
                         [this](const auto& scheme) { applyTheme(scheme); });
        QObject::connect(&themeManager,
                         &snow_shot::presentation::styles::ThemeManager::themeModeChanged, &q,
                         [this](auto) { syncValues(); });
        QObject::connect(&runtimeSession, &settings::SettingsRuntimeSession::shortcutStateChanged,
                              &q,
                              [this](
                                  snow_shot::presentation::GlobalShortcutAction action,
                                  const snow_shot::presentation::GlobalShortcutRegistrationState& state) {
                                   const auto* descriptor =
                                       registry.fieldForShortcut(action);
                                  RuntimeItem* item =
                                      descriptor == nullptr ? nullptr : runtimeItem(descriptor->id);
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
                RuntimeItem* item =
                    descriptor == nullptr ? nullptr : runtimeItem(descriptor->id);
                if (item != nullptr && item->shortcutControl != nullptr) {
                    item->shortcutControl->setDelaySeconds(value);
                }
            });
        QObject::connect(&runtimeSession, &settings::SettingsRuntimeSession::fieldChanged, &q,
                              [this](const QString& fieldId,
                                     const settings::SettingsFieldState& state) {
                                 RuntimeItem* item = runtimeItem(fieldId);
                                 if (item == nullptr) {
                                     return;
                                 }
                                  syncField(*item, &state);
                              });
        QObject::connect(&runtimeSession, &settings::SettingsRuntimeSession::optionsChanged, &q,
                             [this](const QString& fieldId,
                                    const settings::SettingsOptions& options) {
                                 RuntimeItem* item = runtimeItem(fieldId);
                                 if (item == nullptr) {
                                     return;
                                 }
                                 if (item->select != nullptr) {
                                     QList<adqt::widgets::AdSelect::Option> values;
                                     values.reserve(options.values.size());
                                     for (const settings::SettingsRuntimeOption& option :
                                          options.values) {
                                         values.push_back(selectOption(option.value, option.label));
                                     }
                                     const QSignalBlocker blocker(item->select);
                                     item->select->setOptions(values);
                                 }
                                 if (item->multiSelect != nullptr) {
                                     QVector<adqt::widgets::AdMultiSelect::Option> values;
                                     values.reserve(options.values.size());
                                     for (const settings::SettingsRuntimeOption& option :
                                          options.values) {
                                         values.push_back(selectOption(option.value, option.label));
                                     }
                                     const QSignalBlocker blocker(item->multiSelect);
                                     item->multiSelect->setOptions(values);
                                 }
                             });
        QObject::connect(&runtimeSession, &settings::SettingsRuntimeSession::storageStateChanged,
                              &q,
                                     [this](const snow_shot::storage::StorageStatus& status) {
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
                             [this](int) { syncVisibleSection(); });
            QObject::connect(scrollArea->verticalScrollBar(), &QScrollBar::rangeChanged, &q,
                             [this](int, int) { requestVisibleSectionSync(); });
        }

        for (RuntimeSection& section : sections) {
            QObject::connect(section.header, &SectionHeaderWidget::resetRequested, &q,
                             [this, reset = section.reset]() { resetSection(reset); });
        }
    }

    void applySelectValue(const QString& itemId, const QVariant& value) {
        const RuntimeItem* item = runtimeItem(itemId);
        if (item == nullptr || item->definition == nullptr) {
            return;
        }
        const auto* select =
            std::get_if<settings::SettingsSelectDefinition>(&item->definition->payload);
        const bool accepted =
            select != nullptr && runtimeSession.applySelectValue(select->binding, value);
        if (!accepted) {
            syncValues();
        }
    }

    void applySwitchValue(settings::SettingsSwitchBinding binding, bool checked) {
        if (synchronizingValues) {
            return;
        }
        if (!runtimeSession.applySwitchValue(binding, checked)) {
            syncValues();
        }
    }

    void applyIntegerValue(settings::SettingsIntegerBinding binding, int value) {
        if (!runtimeSession.applyIntegerValue(binding, value)) {
            syncValues();
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
        if (!action->confirmation.has_value()) {
            if (!runtimeSession.triggerAction(action->binding)) {
                syncValues();
            }
            return;
        }

        const settings::SettingsConfirmationDefinition& confirmation = *action->confirmation;
        auto* modal = new adqt::widgets::AdModal(&q);
        item->modal = modal;
        modal->setObjectName(settings::generatedObjectName(QStringLiteral("settings-modal"),
                                                           item->definition->id));
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
        QObject::connect(modal, &adqt::widgets::AdModal::accepted, &q, [this, binding = action->binding]() {
            if (!runtimeSession.triggerAction(binding)) {
                syncValues();
            }
        });
        QObject::connect(modal, &adqt::widgets::AdModal::finished, &q,
                         [this, itemId](adqt::widgets::AdModal::DialogCode) {
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

    QList<adqt::widgets::AdSelect::Option>
    selectOptions(const settings::SettingsSelectDefinition& definition) const {
        QList<adqt::widgets::AdSelect::Option> options;
        for (const settings::SettingsOptionDefinition& option : definition.options) {
            options.push_back(selectOption(option.value, option.label.translated()));
        }
        for (const settings::SettingsRuntimeOption& option :
             runtimeSession.dynamicSelectOptions(definition.binding)) {
            options.push_back(selectOption(option.value, option.label));
        }
        return options;
    }

    void syncField(RuntimeItem& runtime,
                   const settings::SettingsFieldState* providedState = nullptr) {
        const QScopedValueRollback<bool> synchronizationGuard(synchronizingValues, true);
        if (runtime.definition == nullptr) {
            return;
        }
        const QString fieldId = runtime.descriptor != nullptr ? runtime.descriptor->id
                                                               : runtime.definition->id;
        const settings::SettingsFieldState sessionState =
            providedState != nullptr ? *providedState : runtimeSession.state(fieldId);
        const bool fieldEnabled = sessionState.enabled;
        if (runtime.anchor != nullptr) {
            runtime.anchor->setVisible(sessionState.visible);
        }
        QWidget* stateTarget = runtime.focusTarget != nullptr ? runtime.focusTarget
                                                                : runtime.anchor;
        if (stateTarget != nullptr) {
            stateTarget->setProperty("settingsDirty", sessionState.dirty);
            stateTarget->setProperty("settingsPending", sessionState.busy);
            stateTarget->setProperty("settingsConflicted", sessionState.conflicted);
            stateTarget->setProperty("settingsError", sessionState.error);
            stateTarget->style()->unpolish(stateTarget);
            stateTarget->style()->polish(stateTarget);
            stateTarget->update();
        }

        {
            if (runtime.select != nullptr) {
                const QSignalBlocker blocker(runtime.select);
                const auto* definition = std::get_if<settings::SettingsSelectDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.select->setCurrentValue(runtimeSession.selectValue(definition->binding));
                }
                runtime.select->setEnabled(fieldEnabled);
            }
            if (runtime.multiSelect != nullptr) {
                const QSignalBlocker blocker(runtime.multiSelect);
                const auto* definition = std::get_if<settings::SettingsMultiSelectDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.multiSelect->setSelectedValues(
                        runtimeSession.multiSelectValue(definition->binding));
                }
                runtime.multiSelect->setEnabled(fieldEnabled);
            }
            if (runtime.switchControl != nullptr) {
                // AdSwitch refreshes its rendered thumb from toggled; the sync guard prevents
                // this programmatic update from being written back as a user change.
                const auto* definition = std::get_if<settings::SettingsSwitchDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    const QSignalBlocker blocker(runtime.switchControl);
                    runtime.switchControl->setChecked(
                        runtimeSession.switchValue(definition->binding));
                    runtime.switchControl->setEnabled(
                        fieldEnabled && definition != nullptr &&
                        runtimeSession.switchEnabled(definition->binding));
                }
            }
            if (runtime.integerControl != nullptr) {
                const QSignalBlocker blocker(runtime.integerControl);
                const auto* definition = std::get_if<settings::SettingsIntegerDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.integerControl->setValue(
                        runtimeSession.integerValue(definition->binding));
                }
                runtime.integerControl->setEnabled(fieldEnabled);
            }
            if (runtime.sliderControl != nullptr) {
                const QSignalBlocker blocker(runtime.sliderControl);
                const auto* definition = std::get_if<settings::SettingsSliderDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    const int value = runtimeSession.sliderValue(definition->binding);
                    runtime.sliderControl->setValue(value);
                    runtime.sliderValue->setText(
                        QStringLiteral("%1%2").arg(value).arg(definition->suffix.translated()));
                }
                runtime.sliderControl->setEnabled(fieldEnabled);
            }
            if (runtime.colorControl != nullptr) {
                const QSignalBlocker blocker(runtime.colorControl);
                const auto* definition = std::get_if<settings::SettingsColorDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.colorControl->setValue(adqt::widgets::AdColorValue::solid(
                        runtimeSession.colorValue(definition->binding)));
                }
                runtime.colorControl->setDisabled(!fieldEnabled);
            }
            if (runtime.radioGroup != nullptr) {
                const QSignalBlocker blocker(runtime.radioGroup);
                const auto* definition = std::get_if<settings::SettingsRadioDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    const QVariant current = runtimeSession.radioValue(definition->binding);
                    runtime.radioGroup->setCheckedId(runtime.radioValues.indexOf(current));
                }
                for (adqt::widgets::AdRadio* button : std::as_const(runtime.radioButtons)) {
                    button->setEnabled(fieldEnabled);
                }
            }
            if (runtime.filePathControl != nullptr) {
                const QSignalBlocker blocker(runtime.filePathControl);
                const auto* definition = std::get_if<settings::SettingsFilePathDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.filePathControl->setText(
                        runtimeSession.filePathValue(definition->binding));
                }
                runtime.filePathControl->setEnabled(fieldEnabled);
            }
            if (runtime.directoryPathControl != nullptr) {
                const QSignalBlocker blocker(runtime.directoryPathControl);
                const auto* definition =
                    std::get_if<settings::SettingsDirectoryPathDefinition>(
                        &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.directoryPathControl->setText(
                        runtimeSession.directoryPathValue(definition->binding));
                }
                runtime.directoryPathControl->setEnabled(fieldEnabled);
            }
            if (runtime.textControl != nullptr) {
                const QSignalBlocker blocker(runtime.textControl);
                const auto* definition = std::get_if<settings::SettingsTextDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.textControl->setText(
                        runtimeSession.textValue(definition->binding));
                }
                runtime.textControl->setEnabled(fieldEnabled);
            }
            if (runtime.shortcutControl != nullptr) {
                const auto* definition = std::get_if<settings::SettingsShortcutActionDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    runtime.shortcutControl->setRegistrationState(
                        runtimeSession.shortcutState(definition->shortcutAction));
                    if (definition->adjustment ==
                        settings::SettingsShortcutAdjustment::ScreenshotDelaySeconds) {
                        runtime.shortcutControl->setDelaySeconds(
                            runtimeSession.integerValue(
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
            if (runtime.actionControl != nullptr) {
                const auto* definition = std::get_if<settings::SettingsActionDefinition>(
                    &runtime.definition->payload);
                if (definition != nullptr) {
                    const settings::SettingsActionState state =
                        runtimeSession.actionState(definition->binding);
                    runtime.actionControl->setBusy(state.busy);
                    runtime.actionControl->setEnabled(state.enabled);
                }
            }
        }
    }

    void syncValues() {
        const auto storageStatus = runtimeSession.storageStatus();
        for (RuntimeItem& runtime : items) {
            syncField(runtime);
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

    void retranslateUi() {
        for (RuntimeSection& runtime : sections) {
            runtime.header->setTitle(runtime.definition->title.translated());
        }
        for (RuntimeItem& runtime : items) {
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
            if (runtime.select != nullptr) {
                const auto* select =
                    std::get_if<settings::SettingsSelectDefinition>(&definition.payload);
                Q_ASSERT(select != nullptr);
                const QSignalBlocker blocker(runtime.select);
                runtime.select->setOptions(selectOptions(*select));
            }
            if (runtime.multiSelect != nullptr) {
                const auto* multi =
                    std::get_if<settings::SettingsMultiSelectDefinition>(&definition.payload);
                Q_ASSERT(multi != nullptr);
                QVector<adqt::widgets::AdMultiSelect::Option> options;
                options.reserve(multi->options.size());
                for (const settings::SettingsOptionDefinition& option : multi->options) {
                    options.push_back(selectOption(option.value, option.label.translated()));
                }
                const QSignalBlocker blocker(runtime.multiSelect);
                runtime.multiSelect->setOptions(options);
            }
            if (runtime.integerControl != nullptr) {
                const auto* integer =
                    std::get_if<settings::SettingsIntegerDefinition>(&definition.payload);
                Q_ASSERT(integer != nullptr);
                runtime.integerControl->setSuffixText(integer->suffix.translated());
            }
            if (runtime.sliderControl != nullptr) {
                const auto* slider =
                    std::get_if<settings::SettingsSliderDefinition>(&definition.payload);
                Q_ASSERT(slider != nullptr);
                const int value = qRound(runtime.sliderControl->value());
                runtime.sliderValue->setText(
                    QStringLiteral("%1%2").arg(value).arg(slider->suffix.translated()));
                runtime.sliderControl->setTooltipFormatter(
                    [suffix = slider->suffix](double current) {
                        return QStringLiteral("%1%2")
                            .arg(qRound(current))
                            .arg(suffix.translated());
                    });
            }
            if (runtime.radioGroup != nullptr) {
                const auto* radio =
                    std::get_if<settings::SettingsRadioDefinition>(&definition.payload);
                Q_ASSERT(radio != nullptr);
                for (int index = 0;
                     index < radio->options.size() && index < runtime.radioButtons.size();
                     ++index) {
                    const QString label = radio->options.at(index).label.translated();
                    runtime.radioButtons.at(index)->setText(label);
                    runtime.radioButtons.at(index)->setAccessibleName(label);
                    runtime.radioButtons.at(index)->setAccessibleDescription(description);
                }
            }
            if (runtime.filePathControl != nullptr) {
                const auto* filePath =
                    std::get_if<settings::SettingsFilePathDefinition>(&definition.payload);
                Q_ASSERT(filePath != nullptr);
                runtime.filePathControl->setSearchButtonText(filePath->buttonText.translated());
                runtime.filePathControl->setPlaceholderText(
                    filePath->fileFilter.translated().section(QStringLiteral(";;"), 0, 0));
            }
            if (runtime.directoryPathControl != nullptr) {
                const auto* directoryPath =
                    std::get_if<settings::SettingsDirectoryPathDefinition>(&definition.payload);
                Q_ASSERT(directoryPath != nullptr);
                runtime.directoryPathControl->setSearchButtonText(
                    directoryPath->buttonText.translated());
            }
            if (runtime.shortcutControl != nullptr) {
                runtime.shortcutControl->setTitle(title);
                runtime.shortcutControl->setAccessibleDescription(description);
                runtime.shortcutControl->retranslateUi();
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
                }
            }
            if (runtime.customControl != nullptr) {
                runtime.customControl->retranslateUi();
            }
        }
        syncValues();
        requestVisibleSectionSync();
    }

    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
        colorScheme = scheme;
        for (RuntimeSection& runtime : sections) {
            runtime.header->applyTheme(scheme);
        }
        for (RuntimeItem& runtime : items) {
            if (runtime.title != nullptr && runtime.description != nullptr) {
                settings_ui::applySettingItemTheme(runtime.title, runtime.description, scheme);
            }
            if (runtime.shortcutControl != nullptr) {
                runtime.shortcutControl->applyTheme(scheme);
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

    void requestVisibleSectionSync() {
        if (visibleSectionSyncPending) {
            return;
        }
        visibleSectionSyncPending = true;
        QTimer::singleShot(0, &q, [this]() {
            visibleSectionSyncPending = false;
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
            activeIndex = sections.size() - 1;
        } else {
            const int activationLine =
                scrollBar->value() + sectionViewportInset() + 1;
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
        QWidget* target = nullptr;
        QWidget* focus = nullptr;
        RuntimeSection* targetSection = nullptr;
        if (!location.itemId.isEmpty()) {
            if (RuntimeItem* item = runtimeItem(location.itemId)) {
                target = item->anchor;
                focus = item->focusTarget;
            }
        }
        if (target == nullptr) {
            targetSection = runtimeSection(location.sectionId);
            if (targetSection != nullptr) {
                target = targetSection->header;
            }
        }
        const bool previousSuppression = suppressVisibleSectionTracking;
        suppressVisibleSectionTracking = true;
        if (target != nullptr) {
            if (targetSection != nullptr) {
                scrollToSection(*targetSection);
            } else {
                scrollArea->ensureWidgetVisible(target, scrollMarginX, scrollMarginY);
            }
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
    bool synchronizingValues = false;
    bool visibleSectionSyncPending = false;
};

SettingsPageWidget::SettingsPageWidget(
    const snow_shot::presentation::settings::SettingsRegistry& registry,
    const QString& pageId,
    snow_shot::presentation::settings::SettingsRuntimeSession& runtimeSession,
    QWidget* parent)
    : QWidget(parent),
      m_impl(std::make_unique<Impl>(*this, registry, pageId, runtimeSession)) {}

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
