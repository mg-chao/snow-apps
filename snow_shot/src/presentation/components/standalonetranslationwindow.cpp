#include "snow_shot/presentation/components/standalonetranslationwindow.h"
#include "snow_shot/presentation/components/translationpagewidget.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/modal.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

namespace snow_shot::presentation {
StandaloneTranslationWindow::StandaloneTranslationWindow(SnowShotApiClient* client, QObject* parent)
    : QObject(parent), m_client(client), m_modal(new adqt::widgets::AdModal(this)) {
    using adqt::widgets::AdModal;
    m_modal->setObjectName(QStringLiteral("standalone-translation-modal"));
    m_modal->setMode(AdModal::Mode::Window);
    m_modal->setWindowModeDetached(true);
    m_modal->setWindowModality(Qt::NonModal);
    m_modal->setWindowTitle(tr("Translation"));
    m_modal->setFooterVisible(false);
    m_modal->setCentered(true);
    m_modal->setWindowPreferredSize(QSize(960, 640));
    m_modal->setWindowMinimumSize(QSize(640, 480));
    m_modal->setWindowResizable(true);
    connect(m_modal, &AdModal::closed, this, [this] {
        if (m_page) {
            m_page->deactivate();
            // A page action can be on the stack. Detach now; destroy after that action returns.
            QWidget* content = m_modal->takeContentWidget();
            m_page.clear();
            if (content) {
                content->deleteLater();
            }
        }
    });
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged, this,
            [this] { m_modal->setWindowTitle(tr("Translation")); });
    connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, this,
            [this](const styles::ThemeColorScheme& scheme) {
                if (m_page) {
                    m_page->applyTheme(scheme);
                }
            });
}

StandaloneTranslationWindow::~StandaloneTranslationWindow() {
    close();
}

void StandaloneTranslationWindow::showTranslation(const QString& text, QScreen* screen) {
    if (!m_page) {
        if (!screen) {
            screen = QGuiApplication::screenAt(QCursor::pos());
        }
        if (!screen) {
            screen = QGuiApplication::primaryScreen();
        }
        m_modal->setWindowScreen(screen);
        m_page = new TranslationPageWidget(nullptr, m_client);
        m_page->setObjectName(QStringLiteral("standalone-translation-page"));
        m_modal->setContentWidget(m_page);
        connect(m_page, &TranslationPageWidget::closeWindowRequested, this,
                &StandaloneTranslationWindow::close);
    }
    m_modal->present();
    m_page->setSourceText(text);
}

void StandaloneTranslationWindow::close() {
    m_modal->close();
}
} // namespace snow_shot::presentation
