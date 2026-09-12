#include "snow_shot/presentation/selectedtexttranslationcoordinator.h"
#include "snow_shot/presentation/selectedtexttranslationcontroller.h"
#include "snow_shot/presentation/components/standalonetranslationwindow.h"
#include "snow_shot/storage/configurationstore.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

namespace snow_shot::presentation {
namespace {
const QString kMasterKey = QStringLiteral("extended_features/translation_page_enabled");
const QString kStandaloneKey = QStringLiteral("extended_features/standalone_translation_window");
} // namespace

SelectedTextTranslationCoordinator::SelectedTextTranslationCoordinator(
    storage::ConfigurationStore& configuration, SnowShotApiClient* client, QObject* parent,
    std::unique_ptr<SelectedTextCaptureBackend> backend, ScreenProvider screenProvider)
    : QObject(parent), m_configuration(configuration),
      m_capture(backend ? new SelectedTextTranslationController(std::move(backend), this)
                        : new SelectedTextTranslationController(this)),
      m_window(new StandaloneTranslationWindow(client, this)),
      m_screenProvider(screenProvider ? std::move(screenProvider)
                                      : [] { return QGuiApplication::screenAt(QCursor::pos()); }) {
    connect(m_capture, &SelectedTextTranslationController::textReady, this,
            [this](const QString& text) {
                if (m_shutdown || !m_configuration.value(kMasterKey).toBool()) {
                    return;
                }
                if (m_standalone) {
                    m_window->showTranslation(text, m_screen);
                } else {
                    emit mainTranslationRequested(text);
                }
            });
    connect(m_capture, &SelectedTextTranslationController::operationFailed, this,
            &SelectedTextTranslationCoordinator::operationFailed);
    connect(&configuration, &storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
                if (key != kMasterKey && key != kStandaloneKey) {
                    return;
                }
                m_capture->cancel();
                m_screen.clear();
                if (!m_configuration.value(kMasterKey).toBool() ||
                    !m_configuration.value(kStandaloneKey).toBool()) {
                    m_window->close();
                }
            });
}

SelectedTextTranslationCoordinator::~SelectedTextTranslationCoordinator() {
    shutdown();
}

void SelectedTextTranslationCoordinator::capture() {
    if (m_shutdown || m_capture->pending() || !m_configuration.value(kMasterKey).toBool()) {
        return;
    }
    m_standalone = m_configuration.value(kStandaloneKey).toBool();
    m_screen = m_screenProvider();
    m_capture->capture();
}

void SelectedTextTranslationCoordinator::shutdown() {
    m_shutdown = true;
    m_capture->shutdown();
    m_window->close();
}
} // namespace snow_shot::presentation
