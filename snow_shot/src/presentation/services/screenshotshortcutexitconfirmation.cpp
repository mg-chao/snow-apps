#include "snow_shot/presentation/screenshotshortcutexitconfirmation.h"

#include "snow_shot/presentation/windowshortcutmanager.h"

#include "widgets/button.h"
#include "widgets/modal.h"

#include <QCoreApplication>
#include <QPointer>

#include <utility>

namespace snow_shot::presentation {

using adqt::widgets::AdButton;
using adqt::widgets::AdModal;
using adqt::widgets::AdModalService;

struct ScreenshotShortcutExitConfirmation::Impl {
    Impl(ScreenshotShortcutExitConfirmation& owner, WindowShortcutManager& manager, ExitAction exit,
         RestoreOwner restore)
        : q(owner), shortcutManager(manager), exitAction(std::move(exit)),
          restoreOwner(std::move(restore)) {}

    bool request(bool confirmationRequired, QWidget* owner) {
        if (!confirmationRequired || owner == nullptr) {
            if (exitAction) {
                exitAction();
                return true;
            }
            return false;
        }
        if (modal != nullptr) {
            return true;
        }

        dialogOwner = owner;
        suspension = shortcutManager.suspendInput();

        AdModalService::Request request;
        request.mode = AdModal::Mode::Overlay;
        request.title = QCoreApplication::translate("ScreenshotController", "Exit screenshot?");
        request.text = QCoreApplication::translate("ScreenshotController",
                                                   "Your current screenshot will be discarded.");
        request.acceptText = QCoreApplication::translate("ScreenshotController", "Exit");
        request.rejectText = QCoreApplication::translate("ScreenshotController", "Cancel");
        request.acceptAccentRole = AdButton::AccentRole::Danger;
        request.closeOnEscape = true;

        const QPointer<ScreenshotShortcutExitConfirmation> receiver(&q);
        request.onAccept = [receiver](AdModal* acceptedModal) {
            if (receiver == nullptr) {
                acceptedModal->reject();
                return;
            }
            receiver->m_impl->accept(acceptedModal);
        };
        request.onReject = [](AdModal* rejectedModal) { rejectedModal->reject(); };

        modal = AdModalService::showConfirm(request, owner);
        if (modal == nullptr) {
            complete(false);
            return false;
        }
        modal->setObjectName(QStringLiteral("screenshotShortcutExitConfirmation"));
        QObject::connect(modal, &AdModal::finished, &q, [receiver](AdModal::DialogCode code) {
            if (receiver != nullptr) {
                receiver->m_impl->complete(code == AdModal::DialogCode::Accepted);
            }
        });
        QObject::connect(modal, &QObject::destroyed, &q, [receiver]() {
            if (receiver != nullptr) {
                receiver->m_impl->complete(false);
            }
        });
        return true;
    }

    void accept(AdModal* acceptedModal) {
        acceptedModal->accept();
        if (exitAction) {
            exitAction();
        }
    }

    void dismiss() {
        if (modal != nullptr) {
            modal->reject();
            return;
        }
        complete(false);
    }

    void complete(bool accepted) {
        if (suspension == 0) {
            return;
        }
        const auto completedSuspension = std::exchange(suspension, 0);
        const QPointer<QWidget> completedOwner = std::exchange(dialogOwner, {});
        modal.clear();
        shortcutManager.resumeInput(completedSuspension);
        if (!accepted && completedOwner != nullptr && restoreOwner) {
            restoreOwner(completedOwner);
        }
    }

    ScreenshotShortcutExitConfirmation& q;
    WindowShortcutManager& shortcutManager;
    ExitAction exitAction;
    RestoreOwner restoreOwner;
    QPointer<AdModal> modal;
    QPointer<QWidget> dialogOwner;
    WindowShortcutManager::InputSuspensionHandle suspension = 0;
};

ScreenshotShortcutExitConfirmation::ScreenshotShortcutExitConfirmation(
    WindowShortcutManager& shortcutManager, ExitAction exitAction, RestoreOwner restoreOwner,
    QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, shortcutManager, std::move(exitAction),
                                                     std::move(restoreOwner))) {}

ScreenshotShortcutExitConfirmation::~ScreenshotShortcutExitConfirmation() {
    m_impl->dismiss();
}

bool ScreenshotShortcutExitConfirmation::request(bool confirmationRequired, QWidget* owner) {
    return m_impl->request(confirmationRequired, owner);
}

void ScreenshotShortcutExitConfirmation::dismiss() {
    m_impl->dismiss();
}

} // namespace snow_shot::presentation
