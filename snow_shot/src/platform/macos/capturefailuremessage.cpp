#include "snow_shot/platform/macos/capturefailuremessage.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QPushButton>

#include <utility>

namespace snow_shot::platform::macos {
namespace {
bool isScreenCapturePermissionDenied(const QString& error) {
    // The native permission preflight emits this exact prefix. Other capture errors, including
    // unrelated permission failures, must retain their own diagnostics and no permission action.
    return error.startsWith(QStringLiteral("Allow Snow Shot in System Settings > Privacy & "
                                           "Security > Screen & System Audio Recording,"));
}

QString messageText(const char* source) {
    return QCoreApplication::translate("MacosCaptureFailureMessage", source);
}
} // namespace

CaptureFailureMessage::CaptureFailureMessage(std::function<bool(const QUrl&)> openUrl) {
    setOption(QMessageBox::Option::DontUseNativeDialog);
    setWindowModality(Qt::NonModal);
    setTextFormat(Qt::PlainText);
    setIcon(QMessageBox::Warning);
    setStandardButtons(QMessageBox::Close);
    m_settingsButton = addButton(messageText(QT_TRANSLATE_NOOP("MacosCaptureFailureMessage",
                                                               "Open Screen Recording Settings")),
                                 QMessageBox::ActionRole);
    m_settingsButton->setObjectName(QStringLiteral("openScreenRecordingSettings"));
    if (!openUrl) {
        openUrl = QDesktopServices::openUrl;
    }
    connect(m_settingsButton, &QPushButton::clicked, this, [openUrl = std::move(openUrl)]() {
        static_cast<void>(openUrl(QUrl(QStringLiteral(
            "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"))));
    });
    updateText();
}

void CaptureFailureMessage::present(const QString& error) {
    m_error = error;
    updateText();
    show();
    raise();
    activateWindow();
}

void CaptureFailureMessage::changeEvent(QEvent* event) {
    QMessageBox::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        updateText();
    }
}

void CaptureFailureMessage::updateText() {
    const QString title =
        messageText(QT_TRANSLATE_NOOP("MacosCaptureFailureMessage", "Screen capture failed"));
    setWindowTitle(title);
    const bool permissionDenied = isScreenCapturePermissionDenied(m_error);
    setText(
        permissionDenied
            ? messageText(QT_TRANSLATE_NOOP(
                  "MacosCaptureFailureMessage",
                  "Allow Snow Shot in System Settings > Privacy & Security > Screen & System "
                  "Audio Recording. If it is already enabled, quit and reopen Snow Shot. After "
                  "an update, you may need to remove Snow Shot from this list and add it again."))
            : (m_error.isEmpty() ? title : m_error));
    m_settingsButton->setText(messageText(
        QT_TRANSLATE_NOOP("MacosCaptureFailureMessage", "Open Screen Recording Settings")));
    m_settingsButton->setVisible(permissionDenied);
}
} // namespace snow_shot::platform::macos
