#include "snow_shot/presentation/components/globalmouserow.h"
#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/components/shortcutconfigurationbutton.h"
#include "snow_shot/presentation/components/shortcutkeyrow.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/presentation/globalshortcutmanager.h"

#include "icon_core.h"
#include "widgets/button.h"
#include "widgets/divider.h"
#include "widgets/modal.h"
#include "widgets/select.h"

#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QEnterEvent>
#include <QHash>
#include <QLabel>
#include <QLayout>
#include <QListView>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QTranslator>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <type_traits>

namespace settings = snow_shot::presentation::settings;
namespace presentation = snow_shot::presentation;
namespace styles = snow_shot::presentation::styles;
namespace storage = snow_shot::storage;

namespace {
static_assert(std::is_base_of_v<ActionRow, GlobalMouseRow>);
static_assert(std::is_base_of_v<ActionRow, ShortcutKeyRow>);

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class StableHeightObserver final : public QObject {
  public:
    explicit StableHeightObserver(QWidget& widget) : m_height(widget.height()) {
        widget.installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Resize) {
            require(static_cast<QResizeEvent*>(event)->size().height() == m_height,
                    "the displayed mouse editor must not resize even transiently");
        }
        return false;
    }

  private:
    int m_height;
};

void flushEvents() {
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

class FakeSettingsBackend final : public settings::SettingsBackend {
  public:
    QVariant selectValue(settings::SettingsSelectBinding) const override {
        return {};
    }
    QVector<settings::SettingsRuntimeOption>
    dynamicSelectOptions(settings::SettingsSelectBinding) const override {
        return {};
    }
    bool applySelectValue(settings::SettingsSelectBinding, const QVariant&) override {
        return false;
    }
    bool switchValue(settings::SettingsSwitchBinding) const override {
        return false;
    }
    bool applySwitchValue(settings::SettingsSwitchBinding, bool) override {
        return false;
    }
    QVariantList multiSelectValue(settings::SettingsMultiSelectBinding) const override {
        return {};
    }
    bool applyMultiSelectValue(settings::SettingsMultiSelectBinding, const QVariantList&) override {
        return false;
    }
    int integerValue(settings::SettingsIntegerBinding) const override {
        return 0;
    }
    bool applyIntegerValue(settings::SettingsIntegerBinding, int) override {
        return false;
    }
    int sliderValue(settings::SettingsSliderBinding) const override {
        return 0;
    }
    bool applySliderValue(settings::SettingsSliderBinding, int) override {
        return false;
    }
    QColor colorValue(settings::SettingsColorBinding) const override {
        return {};
    }
    bool applyColorValue(settings::SettingsColorBinding, const QColor&) override {
        return false;
    }
    QVariant radioValue(settings::SettingsRadioBinding) const override {
        return {};
    }
    bool applyRadioValue(settings::SettingsRadioBinding, const QVariant&) override {
        return false;
    }
    QString filePathValue(settings::SettingsFilePathBinding) const override {
        return {};
    }
    bool applyFilePathValue(settings::SettingsFilePathBinding, const QString&) override {
        return false;
    }
    QString directoryPathValue(settings::SettingsDirectoryPathBinding) const override {
        return {};
    }
    bool applyDirectoryPathValue(settings::SettingsDirectoryPathBinding, const QString&) override {
        return false;
    }
    QString textValue(settings::SettingsTextBinding) const override {
        return {};
    }
    bool applyTextValue(settings::SettingsTextBinding, const QString&) override {
        return false;
    }
    storage::ScreenshotToolbarLayout
    toolbarLayout(storage::ScreenshotToolbarLayoutKind) const override {
        return {};
    }
    bool applyToolbarLayout(storage::ScreenshotToolbarLayoutKind,
                            const storage::ScreenshotToolbarLayout&) override {
        return false;
    }
    presentation::GlobalShortcutRegistrationState
    shortcutState(presentation::GlobalShortcutAction action) const override {
        presentation::GlobalShortcutRegistrationState state;
        state.action = action;
        return state;
    }
    presentation::GlobalShortcutValidationResult
    validateShortcut(const QString& shortcut) const override {
        return {shortcut, true, presentation::GlobalShortcutFailureReason::None};
    }
    bool applyShortcuts(presentation::GlobalShortcutAction, const QStringList&) override {
        return false;
    }
    QStringList localShortcuts(settings::SettingsLocalShortcutScope,
                               const QString&) const override {
        return {};
    }
    presentation::GlobalShortcutValidationResult
    validateLocalShortcut(settings::SettingsLocalShortcutScope, const QString&,
                          const QString& shortcut) const override {
        return {shortcut, true, presentation::GlobalShortcutFailureReason::None};
    }
    bool applyLocalShortcuts(settings::SettingsLocalShortcutScope, const QString&,
                             const QStringList&) override {
        return false;
    }
    settings::SettingsGlobalMouseCombination
    globalMouseCombination(settings::SettingsGlobalMouseAction action) const override {
        return m_combinations.value(static_cast<int>(action));
    }
    bool applyGlobalMouseCombination(
        settings::SettingsGlobalMouseAction action,
        const settings::SettingsGlobalMouseCombination& combination) override {
        m_combinations.insert(static_cast<int>(action), combination);
        return true;
    }
    settings::SettingsActionState actionState(settings::SettingsActionBinding) const override {
        return {true, false};
    }
    bool triggerAction(settings::SettingsActionBinding) override {
        return false;
    }
    storage::StorageStatus storageStatus() const override {
        storage::StorageStatus status;
        status.writeAvailable = true;
        return status;
    }
    bool resetSection(settings::SettingsSectionReset reset) override {
        if (reset == settings::SettingsSectionReset::GlobalMouse) {
            m_combinations.clear();
            return true;
        }
        return false;
    }

  private:
    QHash<int, settings::SettingsGlobalMouseCombination> m_combinations;
};

class GlobalMouseTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }

    QString translate(const char* context, const char* sourceText, const char*,
                      int) const override {
        if (QString::fromLatin1(context) != QStringLiteral("GlobalMouseRow")) {
            return {};
        }
        const QHash<QString, QString> translations{
            {QStringLiteral("Mouse configuration for \"%1\""),
             QStringLiteral("Mauskonfiguration fuer \"%1\"")},
            {QStringLiteral("Activation key"), QStringLiteral("Aktivierungstaste")},
            {QStringLiteral("Mouse button"), QStringLiteral("Maustaste")},
            {QStringLiteral("Ctrl"), QStringLiteral("Strg")},
            {QStringLiteral("None"), QStringLiteral("Keine")},
            {QStringLiteral("Right-button drag"), QStringLiteral("Rechts ziehen")},
        };
        return translations.value(QString::fromUtf8(sourceText));
    }
};

QStringList optionValues(const adqt::widgets::AdSelect& select) {
    QStringList values;
    for (const auto& option : select.options()) {
        values.push_back(option.value.toString());
    }
    return values;
}

ShortcutConfigurationButton* configurationButton(GlobalMouseRow& row) {
    return row.configurationButton();
}

GlobalMouseRow* rowForTitle(SettingsPageWidget& page, const QString& title) {
    for (GlobalMouseRow* row : page.findChildren<GlobalMouseRow*>()) {
        auto* label = row->findChild<QLabel*>(QStringLiteral("globalMouseActionLabel"));
        if (label != nullptr && label->text() == title) {
            return row;
        }
    }
    return nullptr;
}

template <typename Widget> Widget* namedWidget(const QString& objectName) {
    for (QWidget* widget : QApplication::allWidgets()) {
        if (widget->objectName() == objectName) {
            if (auto* result = dynamic_cast<Widget*>(widget); result != nullptr) {
                return result;
            }
        }
    }
    return nullptr;
}

template <typename Object> Object* namedObject(QObject& parent, const QString& objectName) {
    return dynamic_cast<Object*>(parent.findChild<QObject*>(objectName));
}

int selectCount(const QWidget& parent) {
    int count = 0;
    for (QWidget* child : parent.findChildren<QWidget*>()) {
        if (dynamic_cast<adqt::widgets::AdSelect*>(child) != nullptr) {
            ++count;
        }
    }
    return count;
}

void globalMousePageRendersActionButtons() {
    const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, QStringLiteral("global-mouse"), session);
    page.resize(900, 720);
    page.show();
    flushEvents();

    const auto rows = page.findChildren<GlobalMouseRow*>();
    const auto dividers = page.findChildren<adqt::widgets::AdDivider*>();
    require(rows.size() == 7, "Global mouse must render seven action rows");
    require(dividers.isEmpty(), "Global mouse action buttons must use the quick-action layout");

    QStringList remainingTitles{
        QStringLiteral("Copy to clipboard"), QStringLiteral("Pin to screen"),
        QStringLiteral("Text recognition"),  QStringLiteral("Text translation"),
        QStringLiteral("Save as file"),      QStringLiteral("Quick save"),
        QStringLiteral("Screen recording")};
    for (GlobalMouseRow* row : rows) {
        auto* label = row->findChild<QLabel*>(QStringLiteral("globalMouseActionLabel"));
        auto* icon = row->findChild<QLabel*>(QStringLiteral("globalMouseActionIcon"));
        auto* button = configurationButton(*row);
        QLayout* layout = row->layout();
        require(dynamic_cast<QAbstractButton*>(row) != nullptr,
                "Global mouse action rows must be button containers");
        const bool correctlyOrdered =
            label != nullptr && icon != nullptr && button != nullptr && layout != nullptr &&
            label->mapTo(row, QPoint()).x() + label->width() <= icon->mapTo(row, QPoint()).x() &&
            icon->mapTo(row, QPoint()).x() + icon->width() <= button->x();
        if (!correctlyOrdered) {
            std::cerr << "row=" << row->objectName().toStdString()
                      << " label=" << (label != nullptr) << " button=" << (button != nullptr)
                      << " layout=" << (layout != nullptr)
                      << " count=" << (layout != nullptr ? layout->count() : -1) << '\n';
        }
        require(correctlyOrdered,
                "Global mouse rows must place the action label left of the configuration button");
        require(!icon->pixmap().isNull() && icon->testAttribute(Qt::WA_TransparentForMouseEvents),
                "Every global mouse action must display an icon without intercepting clicks");
        if (label->text() == QStringLiteral("Quick save")) {
            const QPixmap expected = presentation::icons::renderTintedIconPixmap(
                presentation::icons::custom::outlined::QuickSave(), icon->size(),
                row->devicePixelRatioF(), label->palette().color(QPalette::WindowText));
            require(icon->pixmap().toImage() == expected.toImage(),
                    "Quick save must display the supplied quick-save icon");
        }
        require(remainingTitles.removeOne(label->text()),
                "Global mouse rows must expose each declared action label exactly once");
        require(
            button->buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Dashed &&
                adqt::icons::describeIcon(button->iconRef()).key.name ==
                    QStringLiteral("wheel-mouse") &&
                button->accessibleName().contains(label->text()),
            "Global mouse buttons must reuse the dashed trigger with a mouse icon and action name");
    }
    require(remainingTitles.isEmpty(), "Global mouse page is missing a declared action label");
}

void globalMouseModalEditsOnlyOnAcceptedUniquePairs() {
    using Action = settings::SettingsGlobalMouseAction;
    using Combination = settings::SettingsGlobalMouseCombination;
    const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, QStringLiteral("global-mouse"), session);
    page.resize(900, 720);
    page.show();
    flushEvents();

    GlobalMouseRow* copyRow = rowForTitle(page, QStringLiteral("Copy to clipboard"));
    require(copyRow != nullptr, "Copy to clipboard row must exist");
    auto* copyButton = configurationButton(*copyRow);
    copyButton->click();
    auto* initialContent = namedWidget<QWidget>(QStringLiteral("globalMouseConfigurationContent"));
    StableHeightObserver initialHeightObserver(*initialContent);
    const int initialHeight = initialContent->height();
    for (int i = 0; i < 8; ++i) {
        flushEvents();
        require(initialContent->height() == initialHeight,
                "opening the mouse modal must not change its content height after display");
    }
    flushEvents();

    auto* modal = namedObject<adqt::widgets::AdModal>(
        *copyRow, QStringLiteral("globalMouseConfigurationModal"));
    auto* activation =
        namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseActivationKeySelect"));
    auto* mouseButton =
        namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseButtonSelect"));
    require(modal != nullptr && modal->contentWidget() != nullptr && activation != nullptr &&
                mouseButton != nullptr && selectCount(*modal->contentWidget()) == 2 &&
                modal->contentWidget()->findChild<QWidget*>(
                    QStringLiteral("shortcutConfigContent")) == nullptr &&
                modal->contentWidget()->findChild<QWidget*>(
                    QStringLiteral("shortcutConfigKeyButton")) == nullptr,
            "mouse configuration must contain exactly two selects and no key recorder");
    require(optionValues(*activation) == QStringList{QStringLiteral("windows"),
                                                     QStringLiteral("ctrl"), QStringLiteral("alt"),
                                                     QStringLiteral("shift")} &&
                optionValues(*mouseButton) ==
                    QStringList{QStringLiteral("left_drag"), QStringLiteral("right_drag"),
                                QStringLiteral("wheel_drag"), QStringLiteral("side_button_1_drag"),
                                QStringLiteral("side_button_2_drag"), QStringLiteral("none")} &&
                activation->currentValues() == QVariantList{QStringLiteral("windows")} &&
                mouseButton->currentValues().isEmpty() && !modal->acceptButton()->isEnabled(),
            "an Unset mouse modal must default only Windows and require a mouse selection");

    require(activation->mode() == adqt::widgets::AdSelect::Mode::Multiple &&
                modal->contentWidget()->findChild<QWidget*>(
                    QStringLiteral("globalMouseConfigurationForm")) != nullptr,
            "activation keys must be a multiple select in a form");
    for (QLabel* label : initialContent->findChildren<QLabel*>()) {
        if (label->buddy() == activation || label->buddy() == mouseButton) {
            const int gap = label->buddy()->mapTo(initialContent, QPoint()).y() -
                            label->mapTo(initialContent, QPoint()).y() - label->height();
            require(gap == styles::ThemeManager::instance().themeColorScheme().metricAlias.marginXS,
                    "field hints must use the compact theme gap above their selects");
        }
    }
    activation->setCurrentValues({QStringLiteral("alt")});
    mouseButton->setCurrentValue(QStringLiteral("wheel_drag"));
    modal->rejectButton()->click();
    flushEvents();
    require(session.globalMouseCombination(Action::ScreenshotCopy).isUnset() &&
                copyButton->text() == QStringLiteral("Unset"),
            "Cancel must leave an Unset global mouse field unchanged");

    copyButton->click();
    flushEvents();
    modal = namedObject<adqt::widgets::AdModal>(*copyRow,
                                                QStringLiteral("globalMouseConfigurationModal"));
    activation =
        namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseActivationKeySelect"));
    mouseButton = namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseButtonSelect"));
    activation->setCurrentValues({QStringLiteral("shift"), QStringLiteral("ctrl")});
    mouseButton->setCurrentValue(QStringLiteral("right_drag"));
    modal->acceptButton()->click();
    flushEvents();
    const Combination saved{{QStringLiteral("ctrl"), QStringLiteral("shift")},
                            QStringLiteral("right_drag")};
    require(session.globalMouseCombination(Action::ScreenshotCopy) == saved &&
                copyButton->text() == QStringLiteral("Ctrl + Shift + Right-button drag"),
            "OK must persist the structured pair and refresh the button label");

    GlobalMouseRow* fixedRow = rowForTitle(page, QStringLiteral("Pin to screen"));
    require(fixedRow != nullptr, "Pin to screen row must exist");
    auto* fixedButton = configurationButton(*fixedRow);
    fixedButton->click();
    flushEvents();
    modal = namedObject<adqt::widgets::AdModal>(*fixedRow,
                                                QStringLiteral("globalMouseConfigurationModal"));
    activation =
        namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseActivationKeySelect"));
    mouseButton = namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseButtonSelect"));
    auto* validation = namedWidget<QLabel>(QStringLiteral("globalMouseValidationMessage"));
    const int editorHeight = modal->contentWidget()->height();
    StableHeightObserver validationHeightObserver(*modal->contentWidget());
    activation->setCurrentValues({QStringLiteral("ctrl"), QStringLiteral("shift")});
    mouseButton->setCurrentValue(saved.mouseButton);
    flushEvents();
    require(validation != nullptr && validation->isVisible() && !modal->acceptButton()->isEnabled(),
            "a duplicate combination must show validation and disable OK");
    require(modal->contentWidget()->height() == editorHeight,
            "a wrapped validation message must not resize the displayed mouse editor");
    mouseButton->setCurrentValue(QStringLiteral("wheel_drag"));
    flushEvents();
    require(!validation->isVisible() && modal->acceptButton()->isEnabled(),
            "choosing a unique pair must clear duplicate validation");
    require(modal->contentWidget()->height() == editorHeight,
            "clearing validation must preserve the mouse editor height");
    activation->setCurrentValues({});
    flushEvents();
    require(validation->isVisible() && !modal->acceptButton()->isEnabled(),
            "an empty activation-key selection must not be saved");
    require(modal->contentWidget()->height() == editorHeight,
            "changing validation messages must preserve the mouse editor height");
    modal->rejectButton()->click();
    flushEvents();
}

void noneClearsAssignedMouseBinding() {
    using Action = settings::SettingsGlobalMouseAction;
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    const settings::SettingsGlobalMouseCombination assigned{{QStringLiteral("windows")},
                                                            QStringLiteral("left_drag")};
    require(session.applyGlobalMouseCombination(Action::ScreenshotCopy, assigned),
            "None fixture must assign a binding");
    SettingsPageWidget page(registry, QStringLiteral("global-mouse"), session);
    page.resize(900, 720);
    page.show();
    flushEvents();
    auto* row = rowForTitle(page, QStringLiteral("Copy to clipboard"));
    for (const bool accept : {false, true}) {
        row->configurationButton()->click();
        flushEvents();
        auto* modal = namedObject<adqt::widgets::AdModal>(
            *row, QStringLiteral("globalMouseConfigurationModal"));
        auto* mouse =
            namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseButtonSelect"));
        auto* keys =
            namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseActivationKeySelect"));
        bool hasNone = false;
        for (const auto& item : mouse->options()) {
            hasNone |= item.value == QStringLiteral("none") && item.label == QStringLiteral("None");
        }
        require(hasNone, "mouse actions must offer None");
        mouse->setCurrentValue(QStringLiteral("none"));
        require(modal->acceptButton()->isEnabled() && !keys->isEnabled(),
                "None must ignore existing activation keys and disable their editor");
        keys->setCurrentValues({});
        mouse->setCurrentValue(QStringLiteral("left_drag"));
        require(keys->isEnabled() && !modal->acceptButton()->isEnabled(),
                "switching back to a drag must restore activation-key validation");
        mouse->setCurrentValue(QStringLiteral("none"));
        flushEvents();
        require(modal->acceptButton()->isEnabled(), "None must be valid without activation keys");
        (accept ? modal->acceptButton() : modal->rejectButton())->click();
        flushEvents();
        require(accept ? session.globalMouseCombination(Action::ScreenshotCopy).isUnset()
                       : session.globalMouseCombination(Action::ScreenshotCopy) == assigned,
                "None must clear the binding only when accepted");
    }
    require(row->configurationButton()->text() == QStringLiteral("Unset"),
            "clearing a binding must refresh its row");
}

void globalMouseAndGlobalHotkeyRowsSharePresentation() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    auto scheme = styles::ThemeManager::instance().themeColorScheme();
    scheme.map.colorBgContainer = QColor(QStringLiteral("#e1eee5"));
    GlobalMouseRow mouse(QStringLiteral("Action"),
                         settings::SettingsGlobalMouseAction::ScreenshotCopy, session, scheme);
    SettingsPageWidget globalHotkeysPage(registry, QStringLiteral("global-hotkeys"), session);
    auto* quickRow = globalHotkeysPage.findChild<ShortcutKeyRow*>();
    require(quickRow != nullptr, "Quick Actions must provide an action row for comparison");
    // Isolate the real row from its page's layout and background during pixel comparison.
    quickRow->setParent(nullptr);
    const std::unique_ptr<ShortcutKeyRow> quickRowOwner(quickRow);
    auto& shortcut = *quickRow;
    shortcut.applyTheme(scheme);
    mouse.resize(601, mouse.height());
    shortcut.resize(mouse.size());
    const auto renderSurface = [](QWidget& widget, qreal dpr) {
        QImage image(QSize(qRound(widget.width() * dpr), qRound(widget.height() * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        widget.render(&image, QPoint(), QRegion(), QWidget::DrawWindowBackground);
        return image;
    };
    auto* shortcutButton = shortcut.findChild<ShortcutConfigurationButton*>();
    require(shortcutButton != nullptr, "Quick Actions must provide a configuration button");
    for (const bool hovered : {false, true}) {
        mouse.setAttribute(Qt::WA_UnderMouse, hovered);
        shortcut.setAttribute(Qt::WA_UnderMouse, hovered);
        for (const bool pressed : {false, true}) {
            mouse.setDown(pressed);
            shortcut.setDown(pressed);
            for (const bool configurationActive : {false, true}) {
                mouse.configurationButton()->setDown(configurationActive);
                shortcutButton->setDown(configurationActive);
                for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
                    const QImage mouseSurface = renderSurface(mouse, dpr);
                    const QImage shortcutSurface = renderSurface(shortcut, dpr);
                    if (mouseSurface != shortcutSurface) {
                        std::cerr << "Row comparison: hovered=" << hovered << " pressed=" << pressed
                                  << " configurationActive=" << configurationActive
                                  << " mouse=" << mouseSurface.width() << 'x'
                                  << mouseSurface.height() << " quick=" << shortcutSurface.width()
                                  << 'x' << shortcutSurface.height() << '\n';
                    }
                    require(
                        mouseSurface == shortcutSurface,
                        "global mouse and global-hotkey action surfaces must render identically "
                        "in normal, hovered, pressed, and configuration-active states");
                }
            }
        }
    }
}

void globalMouseAndHotkeyTitlesRenderIdentically() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    auto scheme = styles::ThemeManager::instance().themeColorScheme();
    GlobalMouseRow mouse(QStringLiteral("Copy to clipboard"),
                         settings::SettingsGlobalMouseAction::ScreenshotCopy, session, scheme);
    ShortcutKeyRowConfig config;
    config.title = QStringLiteral("Copy to clipboard");
    config.iconRef = presentation::icons::custom::outlined::ScreenshotCopy();
    config.useStableBorder = true;
    ShortcutKeyRow shortcut(config, scheme.metricAlias,
                            styles::buildMainWindowComponentMetricToken(scheme));
    auto* mouseTitle = mouse.findChild<QLabel*>(QStringLiteral("globalMouseActionLabel"));
    auto* shortcutTitle = shortcut.findChild<QLabel*>(QStringLiteral("shortcutTitleLabel"));
    auto* shortcutButton = shortcut.findChild<ShortcutConfigurationButton*>();
    require(mouseTitle != nullptr && shortcutTitle != nullptr && shortcutButton != nullptr,
            "Both action rows must expose a title and configuration button");
    // Keep configuration widths equal, but retain their different icons and behavior.
    mouse.configurationButton()->setFixedWidth(180);
    shortcutButton->setFixedWidth(180);
    mouse.resize(601, mouse.height());
    shortcut.resize(mouse.size());
    mouse.show();
    shortcut.show();
    flushEvents();

    const auto sendHover = [](QWidget& widget, bool hovered) {
        widget.setAttribute(Qt::WA_UnderMouse, hovered);
        if (hovered) {
            QEnterEvent event(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
            QApplication::sendEvent(&widget, &event);
        } else {
            QEvent event(QEvent::Leave);
            QApplication::sendEvent(&widget, &event);
        }
    };
    const auto renderTitleArea = [](QWidget& widget, qreal dpr) {
        QImage image(QSize(qRound(widget.width() * dpr), qRound(widget.height() * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        widget.render(&image); // Include the title and icon child widgets.
        return image.copy(0, 0, qRound(350 * dpr), image.height());
    };
    for (const bool dark : {false, true}) {
        scheme.map.colorBgContainer = QColor(dark ? "#171d23" : "#f3f7fa");
        scheme.map.colorText = QColor(dark ? "#f0f2f4" : "#17212a");
        scheme.map.colorPrimaryHover = QColor(dark ? "#85beff" : "#246ac7");
        scheme.map.colorPrimaryActive = QColor(dark ? "#549be8" : "#174b9a");
        mouse.applyTheme(scheme);
        shortcut.applyTheme(scheme);
        for (const bool hovered : {false, true}) {
            for (const bool pressed : {false, true}) {
                for (const int configurationState : {0, 1, 2}) {
                    mouse.setDown(pressed);
                    shortcut.setDown(pressed);
                    mouse.configurationButton()->setDown(configurationState == 2);
                    shortcutButton->setDown(configurationState == 2);
                    sendHover(*mouse.configurationButton(), configurationState == 1);
                    sendHover(*shortcutButton, configurationState == 1);
                    sendHover(mouse, hovered);
                    sendHover(shortcut, hovered);
                    const QColor expected = configurationState != 0 ? scheme.map.colorText
                                            : pressed               ? scheme.map.colorPrimaryActive
                                            : hovered               ? scheme.map.colorPrimaryHover
                                                                    : scheme.map.colorText;
                    require(mouseTitle->palette().color(QPalette::WindowText) == expected &&
                                shortcutTitle->palette().color(QPalette::WindowText) == expected,
                            "Both titles must use the hotkey color for every interaction state");
                    for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
                        require(renderTitleArea(mouse, dpr) == renderTitleArea(shortcut, dpr),
                                "Action titles, icons, and backgrounds must render identically "
                                "across themes, hover/press states, and scale factors");
                    }
                }
            }
        }
    }
}

void globalMouseLanguageAndThemeChangesRefreshOpenUi() {
    using Action = settings::SettingsGlobalMouseAction;
    const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    require(session.applyGlobalMouseCombination(
                Action::ScreenshotCopy, {{QStringLiteral("ctrl")}, QStringLiteral("right_drag")}),
            "language test fixture must save a mouse combination");
    SettingsPageWidget page(registry, QStringLiteral("global-mouse"), session);
    page.resize(900, 720);
    page.show();
    flushEvents();

    GlobalMouseRow* row = rowForTitle(page, QStringLiteral("Copy to clipboard"));
    require(row != nullptr, "Copy to clipboard row must exist for language updates");
    auto* button = configurationButton(*row);
    button->click();
    flushEvents();
    auto* modal =
        namedObject<adqt::widgets::AdModal>(*row, QStringLiteral("globalMouseConfigurationModal"));
    auto* activation =
        namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseActivationKeySelect"));
    auto* mouseButton =
        namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseButtonSelect"));

    GlobalMouseTranslator translator;
    require(QApplication::installTranslator(&translator), "Global mouse translator must install");
    flushEvents();
    require(button->text() == QStringLiteral("Strg + Rechts ziehen") &&
                modal->windowTitle() ==
                    QStringLiteral("Mauskonfiguration fuer \"Copy to clipboard\"") &&
                activation->options().at(1).label == QStringLiteral("Strg") &&
                mouseButton->options().at(1).label == QStringLiteral("Rechts ziehen") &&
                mouseButton->options().last().label == QStringLiteral("Keine"),
            "language changes must refresh button and open-modal text without losing selection");

    styles::ThemeColorScheme scheme = styles::ThemeManager::instance().themeColorScheme();
    scheme.map.colorText = QColor(QStringLiteral("#123456"));
    page.applyTheme(scheme);
    auto* label = row->findChild<QLabel*>(QStringLiteral("globalMouseActionLabel"));
    require(label != nullptr &&
                label->palette().color(QPalette::WindowText) == scheme.map.colorText,
            "theme changes must refresh Global mouse row presentation");

    QApplication::removeTranslator(&translator);
    flushEvents();
    modal->rejectButton()->click();
    flushEvents();
}

void actionButtonPressDispatchesBeforeRelease() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, QStringLiteral("global-mouse"), session);
    page.resize(900, 720);
    page.show();
    flushEvents();
    int requests = 0;
    settings::SettingsGlobalMouseAction action =
        settings::SettingsGlobalMouseAction::ScreenshotCopy;
    QObject::connect(&page, &SettingsPageWidget::commandRequested, &page,
                     [&](const settings::SettingsCommand& command) {
                         require(command.kind ==
                                     settings::SettingsCommandKind::BeginGlobalMouseDrag,
                                 "the action button must dispatch a direct drag command");
                         action = command.globalMouseAction;
                         ++requests;
                     });
    auto* row = rowForTitle(page, QStringLiteral("Text recognition"));
    require(row != nullptr, "OCR action button must exist");
    const QPointF local(20, row->height() / 2);
    const auto sendMouse = [&](QWidget* target, QEvent::Type type, Qt::MouseButton button,
                               Qt::MouseButtons buttons) {
        QMouseEvent event(type, local, target->mapToGlobal(local.toPoint()), button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };
    sendMouse(row, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    require(requests == 1 && action == settings::SettingsGlobalMouseAction::ScreenshotOcr,
            "left press must start the correct action before releasing, with no activation key");
    sendMouse(row, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    require(requests == 1, "releasing the action button must not dispatch another action");
    sendMouse(row, QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton);
    sendMouse(row, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton);
    require(requests == 1, "right press must not start a direct drag");
    sendMouse(row->configurationButton(), QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    sendMouse(row->configurationButton(), QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    flushEvents();
    require(requests == 1, "configuration button must not trigger the enclosing action");
    auto* modal =
        namedObject<adqt::widgets::AdModal>(*row, QStringLiteral("globalMouseConfigurationModal"));
    require(modal != nullptr, "configuration button must open its modal");
    modal->reject();
    flushEvents();
    auto* saveRow = rowForTitle(page, QStringLiteral("Save as file"));
    auto* quickSaveRow = rowForTitle(page, QStringLiteral("Quick save"));
    require(saveRow != nullptr && quickSaveRow != nullptr &&
                saveRow->mapTo(&page, QPoint()).y() < quickSaveRow->mapTo(&page, QPoint()).y(),
            "Save as file must appear above Quick save");
    sendMouse(saveRow, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    require(requests == 2 && action == settings::SettingsGlobalMouseAction::ScreenshotSave,
            "Save as file must dispatch its distinct capture action");
    sendMouse(saveRow, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    require(requests == 2, "Save as file must dispatch only once per drag");
}

void globalMousePopupStaysAboveModalDuringLayout() {
    const auto& registry = settings::builtInSettingsRegistry();
    FakeSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, QStringLiteral("global-mouse"), session);
    page.resize(900, 720);
    page.show();
    flushEvents();
    auto* row = rowForTitle(page, QStringLiteral("Copy to clipboard"));
    require(row != nullptr, "popup fixture requires the copy row");
    row->configurationButton()->click();
    flushEvents();
    auto* select =
        namedWidget<adqt::widgets::AdSelect>(QStringLiteral("globalMouseActivationKeySelect"));
    require(select != nullptr, "popup fixture requires activation select");
    select->showPopup();
    flushEvents();
    QWidget* viewport = select->view()->viewport();
    for (int i = 0; i < 3; ++i) {
        const QPointF local = viewport->rect().center();
        QEnterEvent hover(local, viewport->mapTo(viewport->window(), local.toPoint()),
                          viewport->mapToGlobal(local.toPoint()));
        QApplication::sendEvent(viewport, &hover);
        QEvent layout(QEvent::LayoutRequest);
        QApplication::sendEvent(&page, &layout);
        QWidget* immediateHit = page.childAt(viewport->mapTo(&page, viewport->rect().center()));
        require(immediateHit == viewport || viewport->isAncestorOf(immediateHit),
                "modal relayout must not temporarily cover the open select popup");
        flushEvents();
        QWidget* hit = page.childAt(viewport->mapTo(&page, viewport->rect().center()));
        require(select->popupVisible() && (hit == viewport || viewport->isAncestorOf(hit)),
                "select popup must remain above the modal during hover and layout updates");
    }
    select->hidePopup();
    auto* modal =
        namedObject<adqt::widgets::AdModal>(*row, QStringLiteral("globalMouseConfigurationModal"));
    modal->reject();
    flushEvents();
}

void builtInGlobalMouseResetRestoresDefaults() {
    using Action = settings::SettingsGlobalMouseAction;
    using Combination = settings::SettingsGlobalMouseCombination;
    QTemporaryDir temporary;
    require(temporary.isValid(), "reset fixture requires temporary storage");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    applicationStorage.shutdown();
    static_cast<void>(applicationStorage.initialize(
        {temporary.filePath(QStringLiteral("bin")), temporary.path()}));
    require(applicationStorage.isInitialized(), "reset fixture storage must initialize");
    {
        snow_shot::presentation::GlobalShortcutManager shortcuts;
        settings::BuiltInSettingsBackend backend(shortcuts);
        const Combination custom{{QStringLiteral("alt")}, QStringLiteral("side_button_1_drag")};
        require(backend.applyGlobalMouseCombination(Action::ScreenshotCopy, {}) &&
                    backend.globalMouseCombination(Action::ScreenRecording).isUnset() &&
                    backend.applyGlobalMouseCombination(Action::ScreenRecording, custom) &&
                    backend.globalMouseCombination(Action::ScreenRecording) == custom &&
                    backend.applyGlobalMouseCombination(Action::ScreenshotSave, custom) &&
                    backend.globalMouseCombination(Action::ScreenshotSave) == custom,
                "Save as file must persist through the built-in backend");
        require(backend.resetSection(settings::SettingsSectionReset::GlobalMouse),
                "built-in global mouse reset must succeed");
        require(backend.globalMouseCombination(Action::ScreenshotCopy) ==
                        Combination{{QStringLiteral("windows")}, QStringLiteral("left_drag")} &&
                    backend.globalMouseCombination(Action::ScreenshotFixed) ==
                        Combination{{QStringLiteral("windows")}, QStringLiteral("wheel_drag")} &&
                    backend.globalMouseCombination(Action::ScreenshotOcr) ==
                        Combination{{QStringLiteral("windows")}, QStringLiteral("right_drag")} &&
                    backend.globalMouseCombination(Action::ScreenshotTranslation).isUnset() &&
                    backend.globalMouseCombination(Action::ScreenshotSave).isUnset() &&
                    backend.globalMouseCombination(Action::ScreenshotQuickSave).isUnset() &&
                    backend.globalMouseCombination(Action::ScreenRecording).isUnset(),
                "reset must restore the three Windows bindings and clear the other actions");
    }
    applicationStorage.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("global-mouse-page-tests"));
    globalMousePageRendersActionButtons();
    noneClearsAssignedMouseBinding();
    globalMouseAndGlobalHotkeyRowsSharePresentation();
    globalMouseAndHotkeyTitlesRenderIdentically();
    globalMouseModalEditsOnlyOnAcceptedUniquePairs();
    globalMouseLanguageAndThemeChangesRefreshOpenUi();
    globalMousePopupStaysAboveModalDuringLayout();
    actionButtonPressDispatchesBeforeRelease();
    builtInGlobalMouseResetRestoresDefaults();
    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
