#include "snow_shot/presentation/components/updatenotice.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QPushButton>

namespace snow_shot::presentation {
UpdateNotice::UpdateNotice(QString version, QWidget* parent, LinkOpener opener)
    : QMessageBox(parent), m_version(std::move(version)) {
    setObjectName(QStringLiteral("automaticUpdateNotice"));
    setIcon(QMessageBox::Information);
    setWindowModality(Qt::NonModal);
    m_download = addButton(QString(), QMessageBox::AcceptRole);
    m_download->setObjectName(QStringLiteral("downloadFromWebsite"));
    m_later = addButton(QString(), QMessageBox::RejectRole);
    m_later->setObjectName(QStringLiteral("updateLater"));
    setDefaultButton(m_later);
    setEscapeButton(m_later);
    retranslate();
    connect(this, &QMessageBox::buttonClicked, this,
            [this, opener = std::move(opener)](QAbstractButton* button) {
                if (button == m_download) {
                    const QUrl url(QStringLiteral(SNOW_SHOT_WEBSITE_URL));
                    if (opener)
                        opener(url);
                    else
                        QDesktopServices::openUrl(url);
                }
            });
}
void UpdateNotice::retranslate() {
    setWindowTitle(QCoreApplication::translate("UpdateNotice", "Update available"));
    setText(QCoreApplication::translate("UpdateNotice",
                                        "Snow Shot %1 is available. Download the installation "
                                        "package from the official website.")
                .arg(m_version));
    m_download->setText(QCoreApplication::translate("UpdateNotice", "Download from website"));
    m_later->setText(QCoreApplication::translate("UpdateNotice", "Later"));
}
void UpdateNotice::changeEvent(QEvent* event) {
    QMessageBox::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslate();
}
} // namespace snow_shot::presentation
