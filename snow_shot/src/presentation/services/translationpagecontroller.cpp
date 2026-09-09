#include "snow_shot/presentation/translationpagecontroller.h"

#include <algorithm>
#include <utility>

namespace snow_shot::presentation {
TranslationPageController::TranslationPageController(SnowShotApiClient& client,
                                                     storage::ConfigurationStore& settings,
                                                     QLocale locale, QObject* parent,
                                                     int debounceMilliseconds)
    : QObject(parent),
      m_service(&translation::TranslationService::forClient(client, settings, locale)) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(std::max(0, debounceMilliseconds));
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        m_requestDue = true;
        startTranslation();
    });
    connect(m_service, &translation::TranslationService::preferencesChanged, this,
            [this](translation::TranslationService::ChangeReason reason) {
                if (reason == translation::TranslationService::ChangeReason::UserPreferences) {
                    scheduleTranslation();
                }
                emit stateChanged();
            });
    connect(m_service, &translation::TranslationService::catalogChanged, this, [this] {
        if (m_active)
            emit stateChanged();
    });
    connect(m_service, &translation::TranslationService::modelInvalidated, this,
            [this](const QString& id) {
                if (preferences().modelId != id || !m_active) {
                    return;
                }
                m_debounce.stop();
                m_requestDue = false;
                m_retryRequired = true;
                emit stateChanged();
            });
}

TranslationPageController::~TranslationPageController() {
    deactivate();
}

void TranslationPageController::activate() {
    if (m_active)
        return;
    m_active = true;
    if (m_service != nullptr)
        m_service->refreshModels();
}

void TranslationPageController::deactivate() {
    m_active = false;
    m_retryRequired = false;
    invalidateTranslation();
    m_source.clear();
}

void TranslationPageController::invalidateTranslation() {
    m_debounce.stop();
    m_requestDue = false;
    if (m_job != nullptr) {
        auto* job = m_job.data();
        m_job = nullptr;
        disconnect(job, nullptr, this, nullptr);
        job->cancel();
        job->deleteLater();
    }
    m_result.clear();
}

void TranslationPageController::scheduleTranslation() {
    m_retryRequired = false;
    invalidateTranslation();
    if (m_active && !m_composing && !m_source.trimmed().isEmpty())
        m_debounce.start();
    emit stateChanged();
}

void TranslationPageController::setSourceText(const QString& text) {
    if (text == m_source)
        return;
    m_source = text;
    scheduleTranslation();
}

void TranslationPageController::setComposing(bool composing) {
    if (m_composing == composing)
        return;
    m_composing = composing;
    scheduleTranslation();
}

bool TranslationPageController::setPreferences(const QString& source, const QString& target,
                                               const QString& model) {
    return m_service != nullptr && m_service->savePreferences({source, target, model});
}

void TranslationPageController::swapLanguages() {
    const auto current = preferences();
    if (current.sourceLanguage != QStringLiteral("auto") &&
        current.sourceLanguage != current.targetLanguage)
        setPreferences(current.targetLanguage, current.sourceLanguage, current.modelId);
}

void TranslationPageController::startTranslation() {
    if (m_service == nullptr || !m_active || m_composing || !m_requestDue ||
        m_source.trimmed().isEmpty())
        return;
    m_requestDue = false;
    m_job = m_service->createJob({m_source}, this);
    connect(m_job, &translation::TranslationJob::unitChanged, this, [this](int) {
        m_result = m_job->units().first().text;
        emit resultChanged();
    });
    connect(m_job, &translation::TranslationJob::stateChanged, this, [this] {
        m_result = m_job->units().first().text;
        emit stateChanged();
    });
    m_job->start();
}

void TranslationPageController::retry() {
    if (!m_active || m_service == nullptr)
        return;
    invalidateTranslation();
    m_retryRequired = false;
    if (m_service->models().isEmpty())
        m_service->refreshModels(true);
    m_requestDue = !m_source.trimmed().isEmpty();
    startTranslation();
    emit stateChanged();
}

void TranslationPageController::setLocale(const QLocale& locale) {
    if (m_service != nullptr)
        m_service->setLocale(locale);
}

QString TranslationPageController::errorText() const {
    if (m_retryRequired) {
        return translation::TranslationService::modelConfigurationChangedText();
    }
    if (m_service == nullptr)
        return tr("Translation service is unavailable");
    const QString serviceError = m_service->errorText();
    if (!serviceError.isEmpty())
        return serviceError;
    return m_job == nullptr ? QString() : m_job->errorText();
}
} // namespace snow_shot::presentation
