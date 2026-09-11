#include "snow_shot/platform/macos/accessibilitypermissionmessage.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QPushButton>
#include <QTimer>
#include <utility>

namespace snow_shot::platform::macos {
namespace {
QString translatedText(const char* source) {
    return QCoreApplication::translate("MacosAccessibilityPermissionMessage", source);
}
} // namespace

AccessibilityPermissionMessage::AccessibilityPermissionMessage(
    std::function<bool(const QUrl&)> openUrl) {
    setWindowModality(Qt::ApplicationModal);
    setTextFormat(Qt::PlainText);
    setIcon(QMessageBox::Warning);
    setStandardButtons(QMessageBox::Close);
    m_settings = new QPushButton(this);
    m_settings->setObjectName(QStringLiteral("openAccessibilitySettings"));
    addButton(m_settings, QMessageBox::AcceptRole);
    m_retry = new QPushButton(this);
    m_retry->setObjectName(QStringLiteral("retryAccessibilityAction"));
    if (!openUrl)
        openUrl = QDesktopServices::openUrl;
    connect(m_settings, &QPushButton::clicked, this, [openUrl = std::move(openUrl)] {
        static_cast<void>(openUrl(QUrl(QStringLiteral(
            "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"))));
    });
    connect(m_retry, &QPushButton::clicked, this, [this] {
        const auto action = m_retryAction;
        if (action) {
            // The message box handles this click after our connection. Retry only once it
            // has closed, so a still-denied operation can present fresh permission guidance.
            QTimer::singleShot(0, this, action);
        }
    });
    updateText();
}

void AccessibilityPermissionMessage::present(std::function<void()> retry) {
    if (isVisible())
        return;
    m_retryAction = std::move(retry);
    if (m_retryAction) {
        if (!buttons().contains(m_retry))
            addButton(m_retry, QMessageBox::AcceptRole);
        m_retry->show();
    } else {
        removeButton(m_retry);
        // QMessageBox detaches removed buttons. Keep ownership while this mode omits Retry.
        m_retry->setParent(this);
        m_retry->hide();
    }
    updateText();
    show();
    raise();
    activateWindow();
}

void AccessibilityPermissionMessage::changeEvent(QEvent* event) {
    QMessageBox::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        updateText();
}

void AccessibilityPermissionMessage::updateText() {
    const QString title = translatedText(QT_TRANSLATE_NOOP("MacosAccessibilityPermissionMessage",
                                                           "Accessibility permission required"));
    // macOS ignores QMessageBox window titles; the visible heading must use the text itself.
    setWindowTitle(title);
    setText(title);
    setInformativeText(
        m_retryAction
            ? translatedText(
                  QT_TRANSLATE_NOOP("MacosAccessibilityPermissionMessage",
                                    "Allow Snow Shot in System Settings > Privacy & Security > "
                                    "Accessibility to use global mouse gestures. After granting "
                                    "access, click Retry or restart Snow Shot."))
            : translatedText(QT_TRANSLATE_NOOP(
                  "MacosAccessibilityPermissionMessage",
                  "Allow Snow Shot in System Settings > Privacy & Security > Accessibility to read "
                  "selected text. Then return to the original application, select text and use the "
                  "shortcut again.")));
    m_settings->setText(translatedText(
        QT_TRANSLATE_NOOP("MacosAccessibilityPermissionMessage", "Open System Settings")));
    m_retry->setText(
        translatedText(QT_TRANSLATE_NOOP("MacosAccessibilityPermissionMessage", "Retry")));
    setDefaultButton(m_settings);
    setEscapeButton(QMessageBox::Close);
}
} // namespace snow_shot::platform::macos
