#include "snow_shot/platform/macos/accessibilitypermissionmessage.h"

#include <QApplication>
#include <QEvent>
#include <QPushButton>
#include <QTranslator>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
using snow_shot::platform::macos::AccessibilityPermissionMessage;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QPushButton* button(AccessibilityPermissionMessage& message, const char* name) {
    auto* result = message.findChild<QPushButton*>(QString::fromLatin1(name));
    require(result != nullptr, "the permission dialog must expose its named action button");
    return result;
}

class TestTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* sourceText, const char*,
                      int) const override {
        if (QByteArray(context) != "MacosAccessibilityPermissionMessage") {
            return {};
        }
        const QByteArray source(sourceText);
        if (source == "Accessibility permission required") {
            return QStringLiteral("Translated permission title");
        }
        if (source == "Open System Settings") {
            return QStringLiteral("Translated settings action");
        }
        if (source == "Retry") {
            return QStringLiteral("Translated retry action");
        }
        return {};
    }
};

void selectedTextGuidanceDoesNotOfferAnUnsafeRetry() {
    int opened = 0;
    QUrl openedUrl;
    AccessibilityPermissionMessage message([&](const QUrl& url) {
        ++opened;
        openedUrl = url;
        return true;
    });
    message.present();
    auto* settings = button(message, "openAccessibilitySettings");
    auto* retry = button(message, "retryAccessibilityAction");
    require(message.isVisible() && message.isModal() && message.parentWidget() == nullptr &&
                !message.testOption(QMessageBox::Option::DontUseNativeDialog) &&
                message.textFormat() == Qt::PlainText,
            "permission guidance must use one independent, native-capable modal alert");
    require(message.buttons().contains(settings) && settings->isVisible() &&
                message.defaultButton() == settings &&
                message.escapeButton() == message.button(QMessageBox::Close) &&
                !message.buttons().contains(retry) && !retry->isVisible() &&
                message.informativeText().contains(QStringLiteral("read selected text")) &&
                message.informativeText().contains(QStringLiteral("original application")),
            "selected-text guidance must direct the user back to the original application without "
            "Retry");
    const auto windows = QApplication::topLevelWidgets().size();
    int ignoredRetry = 0;
    const auto description = message.informativeText();
    for (int index = 0; index < 48; ++index) {
        message.present([&] { ++ignoredRetry; });
    }
    require(QApplication::topLevelWidgets().size() == windows &&
                message.informativeText() == description && !message.buttons().contains(retry),
            "repeated permission errors must preserve the active native dialog and its mode");
    settings->click();
    require(!message.isVisible() && opened == 1 && ignoredRetry == 0 &&
                openedUrl ==
                    QUrl(QStringLiteral("x-apple.systempreferences:com.apple.preference.security?"
                                        "Privacy_Accessibility")),
            "the settings action must dismiss the alert and request only the Accessibility pane");
    for (int index = 0; index < 8; ++index) {
        message.present();
        require(message.isVisible() && !message.buttons().contains(retry),
                "closing a permission prompt must not permanently suppress subsequent requests");
        message.button(QMessageBox::Close)->click();
        require(!message.isVisible(), "Close must dismiss the alert without another action");
    }
    QCoreApplication::processEvents();
    require(opened == 1 && ignoredRetry == 0,
            "closing or reopening selected-text guidance must never invoke a stale mouse retry");
}

void retryRunsAfterDismissalAndCanPresentAnotherFailure() {
    AccessibilityPermissionMessage message([](const QUrl&) { return true; });
    int retries = 0;
    int replacementRetries = 0;
    bool dismissedBeforeRetry = false;
    message.present([&] {
        ++retries;
        dismissedBeforeRetry = !message.isVisible();
        message.present([&] { ++replacementRetries; });
    });
    auto* retry = button(message, "retryAccessibilityAction");
    require(message.buttons().contains(retry) && retry->isVisible() &&
                message.buttonRole(retry) == QMessageBox::AcceptRole &&
                message.informativeText().contains(QStringLiteral("global mouse gestures")),
            "mouse permission guidance must offer Retry with the matching explanation");
    const auto windows = QApplication::topLevelWidgets().size();
    int ignoredRetry = 0;
    for (int index = 0; index < 12; ++index) {
        message.present([&] { ++ignoredRetry; });
    }
    require(QApplication::topLevelWidgets().size() == windows,
            "repeated mouse failures must not create additional permission windows");
    retry->click();
    require(!message.isVisible() && retries == 0 && ignoredRetry == 0,
            "a click must close the old alert before scheduling the original retry callback");
    QCoreApplication::processEvents();
    require(retries == 1 && dismissedBeforeRetry && message.isVisible() && ignoredRetry == 0 &&
                QApplication::topLevelWidgets().size() == windows,
            "a still-denied retry must be able to reopen the same alert without losing guidance");
    retry->click();
    QCoreApplication::processEvents();
    require(retries == 1 && replacementRetries == 1 && ignoredRetry == 0 && !message.isVisible(),
            "reopened guidance must use its new callback exactly once");
    message.present();
    require(!message.buttons().contains(retry) && !retry->isVisible() &&
                message.informativeText().contains(QStringLiteral("read selected text")),
            "switching to selected-text guidance must remove the native Retry button");
    message.button(QMessageBox::Close)->click();
    message.present([&] { ++retries; });
    require(message.buttons().contains(retry) && retry->isVisible(),
            "switching back to mouse guidance must restore a visible native Retry button");
    message.button(QMessageBox::Close)->click();
    QCoreApplication::processEvents();
    require(retries == 1 && replacementRetries == 1,
            "Close must never perform the stored retry action");
}

void destructionCancelsQueuedRetry() {
    int retries = 0;
    auto message =
        std::make_unique<AccessibilityPermissionMessage>([](const QUrl&) { return true; });
    message->present([&] { ++retries; });
    button(*message, "retryAccessibilityAction")->click();
    message.reset();
    QCoreApplication::processEvents();
    require(retries == 0, "destroying the permission window must cancel its queued retry");
}

void languageChangesRetranslateAnExistingDialog(QApplication& application) {
    AccessibilityPermissionMessage message([](const QUrl&) { return true; });
    message.present([] {});
    TestTranslator translator;
    application.installTranslator(&translator);
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&message, &languageChange);
    require(message.text() == QStringLiteral("Translated permission title") &&
                button(message, "openAccessibilitySettings")->text() ==
                    QStringLiteral("Translated settings action") &&
                button(message, "retryAccessibilityAction")->text() ==
                    QStringLiteral("Translated retry action"),
            "existing permission guidance and its native button labels must retranslate together");
    application.removeTranslator(&translator);
    message.close();
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    selectedTextGuidanceDoesNotOfferAnUnsafeRetry();
    retryRunsAfterDismissalAndCanPresentAnotherFailure();
    destructionCancelsQueuedRetry();
    languageChangesRetranslateAnExistingDialog(application);
    return 0;
}
