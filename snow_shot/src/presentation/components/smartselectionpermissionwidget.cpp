#include "snow_shot/presentation/components/smartselectionpermissionwidget.h"
#include "widgets/button.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QUrl>
#ifdef Q_OS_MACOS
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace {
bool checkPermission(bool prompt) {
#ifdef Q_OS_MACOS
    if (!prompt)
        return AXIsProcessTrusted();
    const void* keys[] = {kAXTrustedCheckOptionPrompt};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef options =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    const bool granted = AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
    return granted;
#else
    Q_UNUSED(prompt);
    return true;
#endif
}
QString permissionText(const char* source) {
    return QCoreApplication::translate("SmartSelectionPermissionWidget", source);
}
} // namespace

SmartSelectionPermissionWidget::SmartSelectionPermissionWidget(
    QWidget* parent, SmartSelectionPermissionActions actions)
    : QWidget(parent), m_actions(std::move(actions)), m_status(new QLabel(this)),
      m_open(new adqt::widgets::AdButton(this)), m_request(new adqt::widgets::AdButton(this)),
      m_retry(new adqt::widgets::AdButton(this)) {
    if (!m_actions.check)
        m_actions.check = checkPermission;
    if (!m_actions.openSettings)
        m_actions.openSettings = [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral(
                "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")));
        };
    setObjectName(QStringLiteral("smartSelectionPermission"));
    m_status->setObjectName(QStringLiteral("smartSelectionPermissionStatus"));
    m_status->setWordWrap(true);
    m_open->setObjectName(QStringLiteral("smartSelectionPermissionSettings"));
    m_request->setObjectName(QStringLiteral("smartSelectionPermissionRequest"));
    m_retry->setObjectName(QStringLiteral("smartSelectionPermissionRetry"));
    auto* layout = new QHBoxLayout(this);
    layout->addWidget(m_status, 1);
    layout->addWidget(m_open);
    layout->addWidget(m_request);
    layout->addWidget(m_retry);
    connect(m_open, &QAbstractButton::clicked, this, [this] { m_actions.openSettings(); });
    connect(m_request, &QAbstractButton::clicked, this, [this] {
        m_granted = m_actions.check(true);
        retranslate();
    });
    connect(m_retry, &QAbstractButton::clicked, this, [this] { refresh(); });
    auto* timer = new QTimer(this);
    timer->setInterval(1500);
    connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible())
            refresh();
    });
    timer->start();
    refresh();
}
void SmartSelectionPermissionWidget::refresh() {
    m_granted = m_actions.check(false);
    retranslate();
}
void SmartSelectionPermissionWidget::retranslate() {
    m_status->setText(m_granted ? permissionText(QT_TRANSLATE_NOOP(
                                      "SmartSelectionPermissionWidget",
                                      "Accessibility access is enabled for Smart selection."))
                                : permissionText(QT_TRANSLATE_NOOP(
                                      "SmartSelectionPermissionWidget",
                                      "Allow Accessibility access to select window elements. Smart "
                                      "selection currently selects whole windows.")));
    m_open->setText(permissionText(
        QT_TRANSLATE_NOOP("SmartSelectionPermissionWidget", "Open System Settings")));
    m_request->setText(permissionText(
        QT_TRANSLATE_NOOP("SmartSelectionPermissionWidget", "Request Accessibility Access")));
    m_retry->setText(permissionText(QT_TRANSLATE_NOOP("SmartSelectionPermissionWidget", "Retry")));
    for (auto* button : {m_open, m_request, m_retry})
        button->setAccessibleName(button->text());
    m_request->setEnabled(!m_granted);
    m_retry->setEnabled(!m_granted);
    setProperty("accessibilityGranted", m_granted);
}
void SmartSelectionPermissionWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslate();
}
void SmartSelectionPermissionWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
}
