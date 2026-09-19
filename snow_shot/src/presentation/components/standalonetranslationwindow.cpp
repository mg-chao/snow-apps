#include "snow_shot/presentation/components/standalonetranslationwindow.h"
#include "snow_shot/presentation/components/translationpagewidget.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/windowgeometrymemory.h"
#include "snow_shot/storage/persistedwindowgeometry.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/message.h"
#include "widgets/modal.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QSize>
#include <QWidget>

#include <optional>

namespace snow_shot::presentation {
namespace {
constexpr int TRANSLATION_WINDOW_DEFAULT_WIDTH = 720;
constexpr int TRANSLATION_WINDOW_DEFAULT_HEIGHT = 500;
constexpr int TRANSLATION_WINDOW_MIN_WIDTH = 650;
constexpr int TRANSLATION_WINDOW_MIN_HEIGHT = 400;
} // namespace

std::optional<QSize> restoredTranslationWindowSize(const QSize& minimumSize,
                                                   const QSize& maximumSize) {
    const std::optional<QSize> saved = storage::WindowMemorySettings().translationWindowSize();
    if (!saved.has_value()) {
        return std::nullopt;
    }
    return storage::clampWindowSize(*saved, minimumSize, maximumSize);
}

void rememberTranslationWindowSize(const QWidget& widget) {
    const QSize size = persistableWindowSize(widget);
    if (!size.isValid() || size.isEmpty()) {
        return;
    }
    storage::WindowMemorySettings().setTranslationWindowSize(size);
}

StandaloneTranslationWindow::StandaloneTranslationWindow(SnowShotApiClient* client, QObject* parent)
    : QObject(parent), m_client(client), m_modal(new adqt::widgets::AdModal(this)) {
    using adqt::widgets::AdModal;
    m_modal->setObjectName(QStringLiteral("standalone-translation-modal"));
    m_modal->setMode(AdModal::Mode::Window);
    m_modal->setWindowModeDetached(true);
    m_modal->setWindowModality(Qt::NonModal);
    m_modal->setWindowTitle(tr("Translation"));
    m_modal->setAcceptText(tr("Copy and Close"));
    m_modal->setRejectText(tr("Close"));
    m_modal->setCentered(true);
    m_modal->setWindowPreferredSize(
        QSize(TRANSLATION_WINDOW_DEFAULT_WIDTH, TRANSLATION_WINDOW_DEFAULT_HEIGHT));
    m_modal->setWindowMinimumSize(
        QSize(TRANSLATION_WINDOW_MIN_WIDTH, TRANSLATION_WINDOW_MIN_HEIGHT));
    m_modal->setWindowResizable(true);
    m_modal->setWindowTaskbarVisible(true);
    m_modal->setWindowMinimizeButtonVisible(true);
    m_modal->setWindowAlwaysOnTopButtonVisible(true);
    AdModal::ComponentTokens tokens;
    // The page carries its own padding; the modal chrome (header, footer) keeps its insets.
    tokens.contentPaddingHorizontal = 0;
    tokens.contentPaddingVertical = 0;
    // Drop the chrome gap below the title bar; the page's own top inset remains.
    tokens.headerMarginBottom = 0;
    m_modal->setComponentTokens(tokens);
    connect(m_modal, &AdModal::closed, this, [this] {
        if (m_page) {
            // The page is detached below; the shared surface is only reachable while
            // it still hosts the page.
            if (QWidget* surface = m_page->window()) {
                rememberTranslationWindowSize(*surface);
            }
            // The footer accept button doubles as "copy and close": copy before the
            // page is detached, while the controller result is still reachable.
            if (static_cast<AdModal::DialogCode>(m_modal->result()) ==
                AdModal::DialogCode::Accepted) {
                m_page->copyResult(false);
            }
            m_page->deactivate();
            // A page action can be on the stack. Detach now; destroy after that action returns.
            QWidget* content = m_modal->takeContentWidget();
            m_page.clear();
            if (content) {
                content->deleteLater();
            }
        }
    });
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged, this, [this] {
        m_modal->setWindowTitle(tr("Translation"));
        m_modal->setAcceptText(tr("Copy and Close"));
        m_modal->setRejectText(tr("Close"));
    });
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
        // Only the size is remembered: the window must keep opening centered on the
        // screen the translation was triggered from.
        if (const std::optional<QSize> rememberedSize = restoredTranslationWindowSize(
                QSize(TRANSLATION_WINDOW_MIN_WIDTH, TRANSLATION_WINDOW_MIN_HEIGHT),
                screen->availableGeometry().size())) {
            m_modal->setWindowPreferredSize(*rememberedSize);
        }
        m_page = new TranslationPageWidget(nullptr, m_client);
        m_page->setObjectName(QStringLiteral("standalone-translation-page"));
        m_modal->setContentWidget(m_page);
        connect(m_page, &TranslationPageWidget::closeWindowRequested, this,
                &StandaloneTranslationWindow::close);
    }
    m_modal->present();
    m_page->setSourceText(text);
    if (text.trimmed().isEmpty()) {
        adqt::widgets::AdMessage::Request request;
        request.key = QStringLiteral("standalone-translation-empty-selection");
        request.content = tr("Failed to retrieve selected text");
        adqt::widgets::AdMessageService::warning(std::move(request), m_page->window());
    }
}

void StandaloneTranslationWindow::close() {
    m_modal->close();
}
} // namespace snow_shot::presentation
