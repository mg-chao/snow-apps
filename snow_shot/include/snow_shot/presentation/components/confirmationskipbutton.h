#pragma once

#include "widgets/button.h"
#include "widgets/modal.h"

#include <QBoxLayout>
#include <QCoreApplication>
#include <QEvent>

#include <functional>

namespace snow_shot::presentation {

class ConfirmationSkipButton final : public adqt::widgets::AdButton {
  public:
    ConfirmationSkipButton(adqt::widgets::AdModal& modal,
                           const std::function<bool()>& disableConfirmation)
        : AdButton(modal.rejectButton()->parentWidget()) {
        setObjectName(QStringLiteral("confirmationDontAskAgainButton"));
        setButtonStyle(ButtonStyle::Outline);
        setAccentRole(AccentRole::Neutral);
        setAutoDefault(false);
        retranslateUi();
        // Keep the modal's standard actions, focus, styling, and acceptance callbacks.
        auto* actions = qobject_cast<QBoxLayout*>(parentWidget()->layout());
        actions->insertWidget(0, this);
        connect(this, &AdButton::clicked, &modal, [&modal, disableConfirmation] {
            disableConfirmation();
            modal.acceptButton()->click();
        });
        show();
    }

  protected:
    void changeEvent(QEvent* event) override {
        if (event->type() == QEvent::LanguageChange)
            retranslateUi();
        AdButton::changeEvent(event);
    }

  private:
    void retranslateUi() {
        setText(QCoreApplication::translate("ConfirmationSkipButton", "Don't ask again"));
    }
};

} // namespace snow_shot::presentation
