#include "snow_shot/presentation/screenshotselectionresizeworkflow.h"
#include "snow_shot/presentation/screenshotselectionresizemodalcontent.h"
#include "snow_shot/presentation/screenshotselectionsettingsstore.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "physical_key_test_support.h"

#include "widgets/button.h"
#include "widgets/input_line_edit.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/segmented.h"
#include "widgets/form.h"
#include "widgets/color_picker.h"
#include "widgets/input_number.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPainterPath>
#include <QPointer>
#include <QRadioButton>
#include <QString>
#include <QWidget>
#include <QWindow>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void selectionResizeModalUsesApplicationModality(ScreenshotSelectionResizeWorkflow& workflow) {
    QWidget owner(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    owner.setAttribute(Qt::WA_TranslucentBackground);
    owner.resize(900, 700);
    owner.show();
    flushEvents();

    ScreenshotSelectionResizeRequest request;
    request.currentParams.selection = QRect(20, 30, 320, 180);
    request.selectionBounds = QRect(0, 0, 1920, 1080);
    request.ownerWindow = &owner;

    require(workflow.open(&owner, request, [](const ScreenshotSelectionParams&) {}),
            "valid resize request should open its modal");

    auto* modal = owner.findChild<adqt::widgets::AdModal*>();
    require(modal != nullptr, "resize workflow should create its modal");
    require(modal->windowModality() == Qt::ApplicationModal,
            "resize modal should request application modality");

    QWidget* overlay = owner.findChild<QWidget*>(QStringLiteral("ad-modal-overlay"));
    require(overlay != nullptr, "resize modal should create its window surface");
    require(overlay->windowModality() == Qt::ApplicationModal && overlay->isModal(),
            "resize modal surface should block every application window");
    require(overlay->windowHandle()->transientParent() == owner.windowHandle(),
            "resize modal must retain native capture-window ownership");
    flushEvents();
    require(overlay->isVisible() && overlay->windowHandle()->isExposed() &&
                !overlay->windowHandle()->format().hasAlpha() &&
                modal->contentWidget()->isVisible(),
            "resize modal must expose an opaque window and its content above a capture overlay");

    modal->reject();
    flushEvents();
}

void selectionResizeModalBlocksOwnerShortcuts(ScreenshotSelectionResizeWorkflow& workflow) {
    QWidget owner;
    owner.resize(900, 700);
    owner.show();
    snow_shot::presentation::WindowShortcutManager shortcuts;
    shortcuts.addScopeWindow(&owner);
    int commands = 0;
    snow_shot::presentation::WindowShortcutManager::Binding command;
    command.id = QStringLiteral("screenshot-command");
    command.keyCombinations = {QKeyCombination(Qt::Key_F8)};
    command.activate = [&](const auto&) {
        ++commands;
        return true;
    };
    require(shortcuts.addBinding(&owner, std::move(command)) != 0,
            "the screenshot owner should register its shortcut");
    const auto press = [](QWidget* receiver, Qt::Key key) {
        PhysicalKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(receiver, &press);
        PhysicalKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QCoreApplication::sendEvent(receiver, &release);
    };
    press(&owner, Qt::Key_F8);
    require(commands == 1, "the screenshot shortcut should work before editing the selection");

    ScreenshotSelectionResizeRequest request;
    request.currentParams.selection = QRect(20, 30, 320, 180);
    request.selectionBounds = QRect(0, 0, 1920, 1080);
    request.ownerWindow = &owner;
    require(workflow.open(&owner, request, [](const ScreenshotSelectionParams&) {}),
            "selection editing should open its modal");
    flushEvents();
    auto* modal = owner.findChild<adqt::widgets::AdModal*>();
    require(modal != nullptr && modal->rejectButton() != nullptr,
            "selection editing should expose its cancel action");
    modal->rejectButton()->setFocus();
    press(modal->rejectButton(), Qt::Key_F8);
    press(&owner, Qt::Key_F8);
    require(commands == 1 && shortcuts.inputSuspended(),
            "selection editing should block owner shortcuts from modal controls and the owner");

    auto* addPreset = modal->contentWidget()->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("selectionPresetAddButton"));
    require(addPreset != nullptr, "selection editing should expose its nested preset modal");
    addPreset->click();
    flushEvents();
    auto* nested = modal->contentWidget()->findChild<adqt::widgets::AdModal*>(
        QStringLiteral("selectionPresetCreateModal"));
    require(nested != nullptr && nested->rejectButton() != nullptr,
            "adding a preset should open a nested modal");
    nested->rejectButton()->setFocus();
    press(nested->rejectButton(), Qt::Key_F8);
    nested->reject();
    flushEvents();
    modal->contentWidget()->window()->activateWindow();
    modal->rejectButton()->setFocus();
    flushEvents();
    press(modal->rejectButton(), Qt::Key_F8);
    require(commands == 1 && shortcuts.inputSuspended(),
            "closing a nested modal must keep selection editing shortcuts blocked");

    press(modal->rejectButton(), Qt::Key_Escape);
    flushEvents();
    require(!shortcuts.inputSuspended(), "Escape should close selection editing normally");
    press(&owner, Qt::Key_F8);
    require(commands == 2, "screenshot shortcuts should resume after selection editing closes");
}

void resizingDoesNotReplacePreviousScreenshotSelection(
    ScreenshotSelectionResizeWorkflow& workflow, ScreenshotSelectionSettingsStore& settingsStore) {
    for (const bool hasPrevious : {false, true}) {
        settingsStore.clear();
        ScreenshotSelectionParams previous;
        previous.selection = QRect(5, 10, 100, 80);
        QWidget owner;
        ScreenshotSelectionResizeRequest request;
        if (hasPrevious)
            request.previousParams = previous;
        request.currentParams.selection = QRect(20, 30, 320, 180);
        request.selectionBounds = QRect(0, 0, 1920, 1080);
        request.ownerWindow = &owner;
        bool applied = false;
        require(workflow.open(&owner, request,
                              [&](const ScreenshotSelectionParams& params) {
                                  applied = params.selection == request.currentParams.selection;
                              }),
                "resize should open for an unexported selection");
        auto* modal = owner.findChild<adqt::widgets::AdModal*>();
        require(modal != nullptr && modal->acceptButton() != nullptr,
                "resize should expose its confirmation action");
        modal->acceptButton()->click();
        flushEvents();
        require(applied, "confirming resize should apply the current selection");
        require(request.previousParams.has_value() == hasPrevious &&
                    (!hasPrevious || request.previousParams == previous),
                "resizing must preserve the history-derived previous selection");
    }
    settingsStore.clear();
}

void unavailablePreviousGeometryIsDisabled(ScreenshotSelectionResizeWorkflow& workflow,
                                           ScreenshotSelectionSettingsStore& settingsStore) {
    for (const bool custom : {false, true}) {
        settingsStore.clear();
        ScreenshotSelectionParams previous;
        previous.selection = QRect(500, 500, 100, 80);
        if (custom) {
            QPainterPath path;
            path.addEllipse(QRectF(previous.selection));
            previous.region = ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve);
        } else {
            previous.region = QRegion(previous.selection).subtracted(QRect(530, 520, 20, 20));
        }
        QWidget owner;
        ScreenshotSelectionResizeRequest request;
        request.previousParams = previous;
        request.currentParams.selection = QRect(20, 30, 160, 100);
        request.selectionBounds = QRect(0, 0, 300, 200);
        request.ownerWindow = &owner;
        require(workflow.open(&owner, request, [](const ScreenshotSelectionParams&) {}),
                "resize must open even when Previous is unavailable in the current canvas");
        auto* modal = owner.findChild<adqt::widgets::AdModal*>();
        require(modal != nullptr, "resize workflow must create its modal");
        auto* quickSets = modal->contentWidget()->findChild<adqt::widgets::AdSelect*>(
            QStringLiteral("selectionPresetSelect"));
        require(quickSets != nullptr, "resize modal must expose selection presets");
        bool previousDisabled = false;
        for (const auto& option : quickSets->options()) {
            if (option.value == QStringLiteral("previous"))
                previousDisabled = option.disabled;
        }
        require(previousDisabled, "an empty clipped Previous must be disabled in the resize modal");
        modal->reject();
        flushEvents();
        require(request.previousParams == previous,
                "canvas-specific availability must preserve the stored previous geometry");
    }
    settingsStore.clear();
}

void confirmedPresetCreationPersistsBeforeResizeCloses(
    ScreenshotSelectionResizeWorkflow& workflow, ScreenshotSelectionSettingsStore& settingsStore) {
    QWidget owner;
    owner.resize(900, 700);
    owner.show();
    flushEvents();

    ScreenshotSelectionResizeRequest request;
    request.currentParams.selection = QRect(20, 30, 640, 360);
    request.selectionBounds = QRect(0, 0, 1920, 1080);
    request.ownerWindow = &owner;
    require(workflow.open(&owner, request, [](const ScreenshotSelectionParams&) {}),
            "valid resize request should open its modal");
    auto* resizeModal = owner.findChild<adqt::widgets::AdModal*>();
    auto* content =
        resizeModal == nullptr
            ? nullptr
            : qobject_cast<ScreenshotSelectionResizeModalContent*>(resizeModal->contentWidget());
    require(content != nullptr, "resize modal should own its resize form content");
    auto* addButton =
        content->findChild<adqt::widgets::AdButton*>(QStringLiteral("selectionPresetAddButton"));
    require(addButton != nullptr, "resize form should expose the preset Add action");
    addButton->click();
    flushEvents();

    auto* createModal =
        content->findChild<adqt::widgets::AdModal*>(QStringLiteral("selectionPresetCreateModal"));
    auto* nameInput = createModal == nullptr
                          ? nullptr
                          : createModal->contentWidget()->findChild<adqt::widgets::AdLineEdit*>(
                                QStringLiteral("selectionPresetNameInput"));
    require(createModal != nullptr && nameInput != nullptr,
            "preset Add action should open its required-name form");
    nameInput->setText(QStringLiteral("Persisted 640 x 360"));
    createModal->acceptButton()->click();
    flushEvents();

    const QVector<ScreenshotSelectionPreset> savedPresets = settingsStore.presets();
    require(savedPresets.size() == 1 &&
                savedPresets.constFirst().name == QStringLiteral("Persisted 640 x 360"),
            "confirming preset creation should persist it immediately");

    resizeModal->reject();
    flushEvents();
    require(settingsStore.presets() == savedPresets,
            "canceling resize should not roll back a separately confirmed preset creation");
}

void presetCreateModalRetranslatesInPlace(ScreenshotSelectionResizeWorkflow& workflow,
                                          ScreenshotSelectionSettingsStore& settingsStore) {
    auto& languageManager = snow_shot::presentation::LanguageManager::instance();
    settingsStore.clear();
    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English should be active before opening the preset modal");

    QWidget owner;
    owner.resize(900, 700);
    owner.show();
    flushEvents();

    ScreenshotSelectionResizeRequest request;
    request.currentParams.selection = QRect(20, 30, 640, 360);
    request.selectionBounds = QRect(0, 0, 1920, 1080);
    request.ownerWindow = &owner;
    require(workflow.open(&owner, request, [](const ScreenshotSelectionParams&) {}),
            "valid resize request should open its modal for retranslation");

    auto* resizeModal = owner.findChild<adqt::widgets::AdModal*>();
    auto* content =
        resizeModal == nullptr
            ? nullptr
            : qobject_cast<ScreenshotSelectionResizeModalContent*>(resizeModal->contentWidget());
    require(content != nullptr, "resize modal should expose its content");

    auto* addButton =
        content->findChild<adqt::widgets::AdButton*>(QStringLiteral("selectionPresetAddButton"));
    require(addButton != nullptr, "resize form should expose the preset Add action");
    addButton->click();
    flushEvents();

    auto* createModal =
        content->findChild<adqt::widgets::AdModal*>(QStringLiteral("selectionPresetCreateModal"));
    auto* nameInput = createModal == nullptr
                          ? nullptr
                          : createModal->contentWidget()->findChild<adqt::widgets::AdLineEdit*>(
                                QStringLiteral("selectionPresetNameInput"));
    require(createModal != nullptr && nameInput != nullptr,
            "preset Add should open the create modal");
    nameInput->setText(QStringLiteral("Keep this text"));
    const QPointer<adqt::widgets::AdModal> modalGuard(createModal);

    require(languageManager.setLanguage(QStringLiteral("zh_CN")),
            "Simplified Chinese should load while the preset modal is open");
    flushEvents();

    require(modalGuard != nullptr && modalGuard.data() == createModal,
            "language switching should keep the existing preset modal instance");
    require(createModal->windowTitle() == QStringLiteral("\u6dfb\u52a0\u9884\u8bbe") &&
                createModal->acceptButton() != nullptr &&
                createModal->acceptButton()->text() == QStringLiteral("\u6dfb\u52a0") &&
                createModal->rejectButton() != nullptr &&
                createModal->rejectButton()->text() == QStringLiteral("\u53d6\u6d88"),
            "the open preset modal should retranslate its title and actions");
    require(nameInput->text() == QStringLiteral("Keep this text"),
            "retranslation should preserve the entered preset name");

    createModal->reject();
    flushEvents();
    resizeModal->reject();
    flushEvents();
    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "English should be restorable after the retranslation test");
    settingsStore.clear();
}
} // namespace

void compoundPreviousSelectionKeepsPresetsRectangular(ScreenshotSelectionSettingsStore& settings) {
    ScreenshotSelectionParams previous;
    previous.selection = QRect(10, 10, 100, 80);
    previous.region = QRegion(previous.selection).subtracted(QRect(40, 30, 20, 20));
    settings.setPresets({{QStringLiteral("Rectangle"), previous}});
    require(settings.presets().size() == 1 && !settings.presets().first().params.region,
            "named presets must store rectangular bounds");
    ScreenshotSelectionResizeModalContent content(previous, QRect(0, 0, 300, 300), true, previous,
                                                  settings.presets());
    int disabled = 0;
    for (auto* input : content.findChildren<adqt::widgets::AdInputNumber*>())
        disabled += input->isEnabled() ? 0 : 1;
    require(disabled == 4, "complex selection must disable numeric position and size controls");
    auto* presets =
        content.findChild<adqt::widgets::AdSelect*>(QStringLiteral("selectionPresetSelect"));
    require(presets && presets->isEnabled(), "preset selection must remain usable");
    presets->selected(QStringLiteral("preset:0"), QString());
    ScreenshotSelectionParams selected;
    require(content.commit(&selected, nullptr, nullptr) ==
                    ScreenshotSelectionResizeModalContent::CommitResult::ApplySelection &&
                !selected.region,
            "choosing a preset must replace the compound region");
    presets->selected(QStringLiteral("previous"), QString());
    require(content.commit(&selected, nullptr, nullptr) ==
                    ScreenshotSelectionResizeModalContent::CommitResult::ApplySelection &&
                selected.region == previous.region,
            "Previous selection restores full geometry through the same dialog");
    settings.clear();
}

void borderTypeKeepsIndependentValues() {
    ScreenshotSelectionParams params;
    params.selection = QRect(10, 20, 200, 100);
    params.shadowWidth = 8;
    params.shadowColor = QColor(20, 30, 40);
    ScreenshotSelectionResizeModalContent content(params, QRect(0, 0, 800, 600), true, params, {});
    auto* type =
        content.findChild<adqt::widgets::AdSegmented*>(QStringLiteral("selectionBorderType"));
    auto* form = content.findChild<adqt::widgets::AdForm*>(QStringLiteral("selectionResizeForm"));
    require(type && form && type->currentValue() == QStringLiteral("shadow"),
            "legacy modal defaults to Shadow");
    type->setCurrentValue(QStringLiteral("border"));
    ScreenshotSelectionParams selected;
    static_cast<void>(content.commit(&selected, nullptr, nullptr));
    require(selected.borderType == ScreenshotSelectionBorderType::Border &&
                selected.borderWidth == 0 && selected.borderColor == QColor(Qt::white) &&
                selected.shadowWidth == 8,
            "first Border selection is disabled white and preserves shadow settings");
    form->setValues({{QStringLiteral("effectWidth"), 5},
                     {QStringLiteral("effectColor"),
                      QVariant::fromValue(adqt::widgets::AdColorValue::solid(QColor(Qt::red)))}});
    type->setCurrentValue(QStringLiteral("shadow"));
    static_cast<void>(content.commit(&selected, nullptr, nullptr));
    require(selected.shadowWidth == 8 && selected.shadowColor == params.shadowColor &&
                selected.borderWidth == 5 && selected.borderColor == QColor(Qt::red),
            "switching back restores independent shadow settings");
    type->setCurrentValue(QStringLiteral("border"));
    require(form->values().value(QStringLiteral("effectWidth")).toInt() == 5,
            "border width is remembered");
    bool found = false;
    for (const auto* item : form->items())
        found |= item->label() == QStringLiteral("Border width");
    require(found, "active effect labels describe borders");
    content.show();
    content.adjustSize();
    flushEvents();
    if (const auto path = qEnvironmentVariable("SNOW_TEST_SELECTION_BORDER_PREVIEW");
        !path.isEmpty())
        require(content.grab().save(path), "border dialog preview must save");
    auto& languages = snow_shot::presentation::LanguageManager::instance();
    require(languages.setLanguage(QStringLiteral("zh_CN")),
            "border controls support language changes");
    QEvent translatedEvent(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&content, &translatedEvent);
    found = false;
    for (const auto* item : form->items())
        found |= item->label() == QStringLiteral("\u8fb9\u6846\u5bbd\u5ea6");
    require(found && type->currentValue() == QStringLiteral("border"),
            "border labels translate without changing the selected type");
    require(languages.setLanguage(QStringLiteral("en_US")),
            "restore English after border translation test");

    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&content, &languageChange);
    require(type->currentValue() == QStringLiteral("border"),
            "retranslation preserves active effect");
    static_cast<void>(content.commit(&selected, nullptr, nullptr));
    require(selected.borderWidth == 5 && selected.shadowWidth == 8,
            "retranslation preserves both widths");
}

void restoredBorderTypeMatchesControl() {
    ScreenshotSelectionParams current;
    current.selection = QRect(10, 20, 200, 100);
    current.borderType = ScreenshotSelectionBorderType::Border;
    current.borderWidth = 5;
    current.borderColor = QColor(Qt::red);
    current.shadowWidth = 8;
    current.shadowColor = QColor(Qt::blue);
    auto previous = current;
    previous.borderType = ScreenshotSelectionBorderType::Shadow;
    previous.shadowWidth = 12;
    auto preset = current;
    preset.borderWidth = 17;
    ScreenshotSelectionParams committed;
    {
        ScreenshotSelectionResizeModalContent content(current, QRect(0, 0, 800, 600), true,
                                                      previous,
                                                      {{QStringLiteral("Border preset"), preset}});
        auto* type =
            content.findChild<adqt::widgets::AdSegmented*>(QStringLiteral("selectionBorderType"));
        auto* form =
            content.findChild<adqt::widgets::AdForm*>(QStringLiteral("selectionResizeForm"));
        auto* presets =
            content.findChild<adqt::widgets::AdSelect*>(QStringLiteral("selectionPresetSelect"));
        require(type && form && presets, "resize controls exist");
        const auto check = [&](const ScreenshotSelectionParams& expected) {
            const auto value = screenshotSelectionBorderTypeId(expected.borderType);
            require(type->currentValue() == value &&
                        form->value(QStringLiteral("borderType")) == value,
                    "restored type must match both the form and Segmented selection");
            const auto buttons = type->findChildren<QRadioButton*>();
            require(buttons.size() == 2, "Segmented exposes both accessible options");
            for (const auto* button : buttons) {
                require(button->isChecked() ==
                            (button->property("optionIndex").toInt() == type->currentIndex()),
                        "restored type must match the visible and accessible selected option");
            }
            require(content.commit(&committed, nullptr, nullptr) ==
                            ScreenshotSelectionResizeModalContent::CommitResult::ApplySelection &&
                        committed.borderType == expected.borderType &&
                        committed.borderWidth == expected.borderWidth &&
                        committed.borderColor == expected.borderColor &&
                        committed.shadowWidth == expected.shadowWidth &&
                        committed.shadowColor == expected.shadowColor,
                    "restoring a type preserves both effect value sets");
        };
        check(current);
        content.show();
        content.adjustSize();
        flushEvents();
        check(current);
        presets->selected(QStringLiteral("previous"), QString());
        check(previous);
        presets->selected(QStringLiteral("preset:0"), QString());
        check(preset);
        presets->selected(QStringLiteral("current"), QString());
        check(current);
    }
    ScreenshotSelectionResizeModalContent reopened(committed, QRect(0, 0, 800, 600), true, previous,
                                                   {});
    auto* type =
        reopened.findChild<adqt::widgets::AdSegmented*>(QStringLiteral("selectionBorderType"));
    require(type && type->currentValue() == QStringLiteral("border"),
            "reopening the committed Border selection must select Border");
    type->setCurrentValue(QStringLiteral("shadow"));
    static_cast<void>(reopened.commit(&committed, nullptr, nullptr));
    require(committed.borderType == ScreenshotSelectionBorderType::Shadow &&
                committed.shadowWidth == current.shadowWidth &&
                committed.borderWidth == current.borderWidth,
            "switching a reopened Border selection restores the saved shadow width");
}

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    restoredBorderTypeMatchesControl();
    borderTypeKeepsIndependentValues();
    ScreenshotSelectionSettingsStore settingsStore;
    settingsStore.clear();
    ScreenshotSelectionResizeWorkflow workflow(settingsStore);
    compoundPreviousSelectionKeepsPresetsRectangular(settingsStore);

    selectionResizeModalUsesApplicationModality(workflow);
    selectionResizeModalBlocksOwnerShortcuts(workflow);
    resizingDoesNotReplacePreviousScreenshotSelection(workflow, settingsStore);
    unavailablePreviousGeometryIsDisabled(workflow, settingsStore);
    confirmedPresetCreationPersistsBeforeResizeCloses(workflow, settingsStore);
    presetCreateModalRetranslatesInPlace(workflow, settingsStore);

    settingsStore.clear();
    return 0;
}
