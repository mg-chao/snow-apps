#include "snow_shot/platform/macos/capturefailuremessage.h"

#include <QApplication>
#include <QEvent>
#include <QPushButton>
#include <QTranslator>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class TestTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }

    QString translate(const char* context, const char* sourceText, const char*,
                      int) const override {
        if (QByteArray(context) == "MacosCaptureFailureMessage" &&
            QByteArray(sourceText) == "Screen capture failed") {
            return QStringLiteral("Translated capture failure");
        }
        return {};
    }
};
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    int opened = 0;
    QUrl openedUrl;
    snow_shot::platform::macos::CaptureFailureMessage message([&](const QUrl& url) {
        ++opened;
        openedUrl = url;
        return true;
    });
    const auto permissionError = QStringLiteral("Allow Snow Shot in System Settings > Privacy & "
                                                "Security > Screen & System Audio Recording, "
                                                "then try again.");
    message.present(permissionError);
    auto* settings = message.findChild<QPushButton*>(QStringLiteral("openScreenRecordingSettings"));
    require(message.isVisible() && message.isModal() && message.parentWidget() == nullptr &&
                !message.testOption(QMessageBox::Option::DontUseNativeDialog),
            "failure feedback must remain visible independently of a closed capture overlay");
    require(settings != nullptr && settings->isVisible() &&
                message.text() == QStringLiteral("Screen recording permission required") &&
                message.informativeText().contains(QStringLiteral("quit and reopen")) &&
                message.informativeText().contains(QStringLiteral("remove Snow Shot")) &&
                message.defaultButton() == settings &&
                message.escapeButton() == message.button(QMessageBox::Close),
            "permission failure must offer a settings action and recovery instructions");
    const auto windows = QApplication::topLevelWidgets().size();
    for (int index = 0; index < 48; ++index) {
        message.present(permissionError);
    }
    require(QApplication::topLevelWidgets().size() == windows,
            "repeated failures must reuse one message window");
    settings->click();
    require(opened == 1 &&
                openedUrl ==
                    QUrl(QStringLiteral("x-apple.systempreferences:com.apple.preference.security?"
                                        "Privacy_ScreenCapture")),
            "the permission action must open the screen capture settings pane");
    message.close();
    for (const auto& error : {QStringLiteral("ScreenCapture timed out"),
                              QStringLiteral("permission denied while opening output file"),
                              QStringLiteral("<b>Invalid screenshot buffer size</b>")}) {
        message.present(error);
        require(message.isVisible() && !message.buttons().contains(settings) &&
                    message.informativeText() == error && message.textFormat() == Qt::PlainText,
                "other failures must show their literal diagnostics without a permission action");
        message.close();
    }
    for (int index = 0; index < 8; ++index) {
        message.present(permissionError);
        require(message.isVisible() && message.buttons().contains(settings) &&
                    message.buttonRole(settings) == QMessageBox::AcceptRole,
                "dismissing a previous permission failure must not suppress the next one");
        settings->click();
        require(!message.isVisible(), "the settings action must dismiss the alert");
    }
    require(opened == 9, "the settings action must work on every reopened permission alert");
    message.present({});
    require(!message.text().isEmpty() && !message.buttons().contains(settings),
            "missing native diagnostics must still produce visible failure feedback");
    TestTranslator translator;
    application.installTranslator(&translator);
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&message, &languageChange);
    require(message.text() == QStringLiteral("Translated capture failure"),
            "an existing failure window must update when the application language changes");
    application.removeTranslator(&translator);
    message.close();
    return 0;
}
