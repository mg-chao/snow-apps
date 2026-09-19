#include "snow_shot/presentation/components/smartselectionpermissionwidget.h"
#include <QApplication>
#include <QAbstractButton>
#include <QEvent>
#include <QLabel>
#include <cstdlib>
#include <iostream>

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bool granted = false;
    int prompts = 0, opened = 0, checks = 0;
    SmartSelectionPermissionWidget widget(nullptr, {[&](bool prompt) {
                                                        ++checks;
                                                        if (prompt)
                                                            ++prompts;
                                                        return granted;
                                                    },
                                                    [&] { ++opened; }});
    auto* request =
        widget.findChild<QAbstractButton*>(QStringLiteral("smartSelectionPermissionRequest"));
    auto* retry =
        widget.findChild<QAbstractButton*>(QStringLiteral("smartSelectionPermissionRetry"));
    auto* settings =
        widget.findChild<QAbstractButton*>(QStringLiteral("smartSelectionPermissionSettings"));
    auto* label = widget.findChild<QLabel*>(QStringLiteral("smartSelectionPermissionStatus"));
    require(request && retry && settings && label, "permission actions must be discoverable");
    require(prompts == 0 && checks == 1 && request->isEnabled(),
            "initial status must never request TCC");
    widget.refresh();
    retry->click();
    settings->click();
    require(prompts == 0 && opened == 1 && checks == 3, "retry and settings must not prompt");
    request->click();
    require(prompts == 1, "only the explicit request action may prompt");
    granted = true;
    retry->click();
    require(widget.property("accessibilityGranted").toBool() && !request->isEnabled() &&
                !retry->isEnabled() && settings->isEnabled(),
            "granted permission must update action state");
    granted = false;
    widget.refresh();
    require(request->isEnabled() && retry->isEnabled(),
            "permission revocation must restore actions");
    const QString text = label->text();
    label->clear();
    QEvent change(QEvent::LanguageChange);
    QApplication::sendEvent(&widget, &change);
    require(label->text() == text, "cached status must retranslate on language change");
}
