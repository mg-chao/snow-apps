#include "snow_shot/presentation/screenshotselectionresizeworkflow.h"
#include "snow_shot/presentation/screenshotselectionresizemodalcontent.h"
#include "snow_shot/presentation/screenshotselectionsettingsstore.h"
#include "snow_shot/presentation/languagemanager.h"

#include "widgets/button.h"
#include "widgets/input_line_edit.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/input_number.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdlib>
#include <iostream>

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
    QWidget owner;
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

    modal->reject();
    flushEvents();
}

void resizingDoesNotReplacePreviousScreenshotSelection(
    ScreenshotSelectionResizeWorkflow& workflow, ScreenshotSelectionSettingsStore& settingsStore) {
    for (const bool hasPrevious : {false, true}) {
        settingsStore.clear();
        ScreenshotSelectionParams previous;
        previous.selection = QRect(5, 10, 100, 80);
        if (hasPrevious) {
            settingsStore.setPreviousSelectionParams(previous);
        }
        QWidget owner;
        ScreenshotSelectionResizeRequest request;
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
        require(settingsStore.hasPreviousSelectionParams() == hasPrevious,
                "resizing must not create a previous screenshot selection");
        if (hasPrevious) {
            require(settingsStore.previousSelectionParams() == previous,
                    "resizing must preserve the previous exported selection");
        }
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
    settings.setPreviousSelectionParams(previous);
    require(settings.previousSelectionParams() == previous, "Previous selection preserves holes");
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

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    ScreenshotSelectionSettingsStore settingsStore;
    settingsStore.clear();
    ScreenshotSelectionResizeWorkflow workflow(settingsStore);
    compoundPreviousSelectionKeepsPresetsRectangular(settingsStore);

    selectionResizeModalUsesApplicationModality(workflow);
    resizingDoesNotReplacePreviousScreenshotSelection(workflow, settingsStore);
    confirmedPresetCreationPersistsBeforeResizeCloses(workflow, settingsStore);
    presetCreateModalRetranslatesInPlace(workflow, settingsStore);

    settingsStore.clear();
    return 0;
}
