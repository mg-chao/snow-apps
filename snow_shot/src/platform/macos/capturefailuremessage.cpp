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
    // Cocoa presents an NSAlert with native buttons. Non-modal message boxes fall back
    // to Qt widgets, and window-modal alerts are unsupported by Qt on macOS Tahoe.
    setWindowModality(Qt::ApplicationModal);
    setTextFormat(Qt::PlainText);
    setIcon(QMessageBox::Warning);
    setStandardButtons(QMessageBox::Close);
    m_settingsButton = new QPushButton(this);
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
    // Native alerts cannot update their content while shown. Keep one active alert,
    // then rebuild its content on the next failure after it has been dismissed.
    if (isVisible()) {
        return;
    }
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
    setText(permissionDenied
                ? messageText(QT_TRANSLATE_NOOP("MacosCaptureFailureMessage",
                                                "Screen recording permission required"))
                : title);
    setInformativeText(
        permissionDenied
            ? messageText(QT_TRANSLATE_NOOP(
                  "MacosCaptureFailureMessage",
                  "Allow Snow Shot in System Settings > Privacy & Security > Screen & System "
                  "Audio Recording.\n\nIf it is already enabled, quit and reopen Snow Shot. After "
                  "an update, you may need to remove Snow Shot from this list and add it again."))
            : m_error);
    m_settingsButton->setText(
        messageText(QT_TRANSLATE_NOOP("MacosCaptureFailureMessage", "Open System Settings")));
    // Hiding a button does not remove it from an NSAlert's button list.
    if (permissionDenied) {
        if (!buttons().contains(m_settingsButton)) {
            addButton(m_settingsButton, QMessageBox::AcceptRole);
        }
        m_settingsButton->show();
        setDefaultButton(m_settingsButton);
    } else {
        removeButton(m_settingsButton);
        m_settingsButton->hide();
        setDefaultButton(QMessageBox::Close);
    }
    setEscapeButton(QMessageBox::Close);
}
} // namespace snow_shot::platform::macos
