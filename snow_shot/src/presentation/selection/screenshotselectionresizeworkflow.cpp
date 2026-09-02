#include "snow_shot/presentation/screenshotselectionresizeworkflow.h"

#include "snow_shot/presentation/screenshotselectionresizemodalcontent.h"
#include "snow_shot/presentation/screenshotselectionsettingsstore.h"
#include "snow_shot/presentation/languagemanager.h"

#include "widgets/modal.h"

#include <QObject>
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QVector>

#include <utility>

namespace {
void persistPreviousSelectionParams(ScreenshotSelectionSettingsStore& settingsStore,
                                    const ScreenshotSelectionParams& params, const QRect& bounds) {
    if (bounds.isEmpty()) {
        settingsStore.setPreviousSelectionParams(params);
        return;
    }

    settingsStore.setPreviousSelectionParams(clampScreenshotSelectionParams(params, bounds));
}
} // namespace

ScreenshotSelectionResizeWorkflow::ScreenshotSelectionResizeWorkflow(
    ScreenshotSelectionSettingsStore& settingsStore)
    : m_settingsStore(settingsStore) {}

bool ScreenshotSelectionResizeWorkflow::open(QObject* modalParent,
                                             const ScreenshotSelectionResizeRequest& request,
                                             ApplySelectionCallback applySelection) const {
    if (request.selectionBounds.isEmpty() || request.currentParams.selection.width() < 1 ||
        request.currentParams.selection.height() < 1 || !applySelection) {
        return false;
    }

    auto* modal = new adqt::widgets::AdModal(modalParent);

    ScreenshotSelectionParams previousParams;
    const bool hasPreviousParams = m_settingsStore.hasPreviousSelectionParams();
    if (hasPreviousParams) {
        previousParams = clampScreenshotSelectionParams(m_settingsStore.previousSelectionParams(),
                                                        request.selectionBounds);
    }

    auto* content = new ScreenshotSelectionResizeModalContent(
        request.currentParams, request.selectionBounds, hasPreviousParams, previousParams,
        m_settingsStore.presets(), nullptr);
    const QPointer<ScreenshotSelectionResizeModalContent> contentGuard(content);
    ScreenshotSelectionSettingsStore* settingsStore = &m_settingsStore;
    QObject::connect(content, &ScreenshotSelectionResizeModalContent::presetsUpdated, modal,
                     [settingsStore](const QVector<ScreenshotSelectionPreset>& presets) {
                         settingsStore->setPresets(presets);
                     });

    modal->setOwnerWindow(request.ownerWindow);
    modal->setMode(adqt::widgets::AdModal::Mode::Window);
    modal->setWindowModality(Qt::ApplicationModal);
    modal->setWindowTitle(tr("Resize selection"));
    modal->setCentered(true);
    modal->setPreferredWidth(500);
    modal->setMaskVisible(false);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(adqt::widgets::AdModal::ClosePolicy::Manual);
    modal->setAcceptText(tr("OK"));
    modal->setRejectText(tr("Cancel"));
    modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                              adqt::widgets::AdModal::StandardButton::Cancel);
    modal->setContentWidget(content);
    modal->setInitialFocusWidget(content->initialFocusWidget());

    QObject::connect(&snow_shot::presentation::LanguageManager::instance(),
                     &snow_shot::presentation::LanguageManager::languageChanged, modal,
                     [modal, content](const QString&, const QLocale&) {
                         modal->setWindowTitle(QCoreApplication::translate(
                             "ScreenshotSelectionResizeWorkflow", "Resize selection"));
                         modal->setAcceptText(QCoreApplication::translate(
                             "ScreenshotSelectionResizeWorkflow", "OK"));
                         modal->setRejectText(QCoreApplication::translate(
                             "ScreenshotSelectionResizeWorkflow", "Cancel"));
                         QEvent languageChange(QEvent::LanguageChange);
                         QCoreApplication::sendEvent(content, &languageChange);
                     });

    const QRect selectionBounds = request.selectionBounds;
    QObject::connect(
        modal, &adqt::widgets::AdModal::closeRequested, modal,
        [modal, contentGuard, applySelection = std::move(applySelection), settingsStore,
         selectionBounds](adqt::widgets::AdModal::CloseReason reason) {
            if (reason != adqt::widgets::AdModal::CloseReason::OkAction) {
                modal->reject();
                return;
            }

            auto* contentPtr = contentGuard.data();
            if (contentPtr == nullptr) {
                modal->reject();
                return;
            }

            ScreenshotSelectionParams params;
            QVector<ScreenshotSelectionPreset> presets;
            bool presetsChanged = false;
            const ScreenshotSelectionResizeModalContent::CommitResult result =
                contentPtr->commit(&params, &presets, &presetsChanged);
            if (result == ScreenshotSelectionResizeModalContent::CommitResult::Invalid) {
                return;
            }
            if (presetsChanged) {
                settingsStore->setPresets(presets);
            }
            if (result == ScreenshotSelectionResizeModalContent::CommitResult::ApplySelection) {
                applySelection(params);
                persistPreviousSelectionParams(*settingsStore, params, selectionBounds);
            }
            modal->accept();
        });
    QObject::connect(modal, &adqt::widgets::AdModal::finished, modal,
                     [onFinished = request.onFinished](adqt::widgets::AdModal::DialogCode) {
                         if (onFinished) {
                             onFinished();
                         }
                     });
    QObject::connect(modal, &adqt::widgets::AdModal::finished, modal, &QObject::deleteLater);

    modal->open();
    return true;
}
