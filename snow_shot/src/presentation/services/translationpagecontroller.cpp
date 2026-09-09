#include "snow_shot/presentation/translationpagecontroller.h"

#include "snow_shot/presentation/translationlanguages.h"
#include "snow_shot/storage/configurationstore.h"

#include <QJsonValue>

#include <algorithm>
#include <utility>

namespace snow_shot::presentation {
namespace {
const QString kSourceKey = QStringLiteral("screenshot_translation/source_language");
const QString kTargetKey = QStringLiteral("screenshot_translation/target_language");
const QString kModelKey = QStringLiteral("screenshot_translation/model");
const QString kProxyKey = QStringLiteral("network/proxy");
} // namespace

TranslationPageController::TranslationPageController(SnowShotApiClient& client,
                                                     storage::ConfigurationStore& settings,
                                                     QLocale locale, QObject* parent,
                                                     int debounceMilliseconds)
    : QObject(parent), m_client(&client), m_settings(settings), m_locale(std::move(locale)),
      m_defaultTarget(defaultTranslationTargetLanguage(m_locale)) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(std::max(0, debounceMilliseconds));
    m_settingsSync.setSingleShot(true);
    m_settingsSync.setInterval(0);
    connect(&m_debounce, &QTimer::timeout, this, [this]() {
        m_requestDue = true;
        startTranslation();
    });
    connect(&m_settingsSync, &QTimer::timeout, this, &TranslationPageController::syncPreferences);
    connect(&settings, &storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
                if (!m_active) {
                    return;
                }
                if (key == kSourceKey || key == kTargetKey || key == kModelKey) {
                    m_settingsSync.start();
                } else if (key == kProxyKey && m_client != nullptr) {
                    m_client->setUseSystemProxy(m_settings.value(kProxyKey).toString() ==
                                                QStringLiteral("system"));
                }
            });
    syncPreferences();
}

TranslationPageController::~TranslationPageController() {
    deactivate();
}

void TranslationPageController::activate() {
    if (m_active) {
        return;
    }
    m_active = true;
    syncPreferences();
    if (m_client != nullptr) {
        m_client->setUseSystemProxy(m_settings.value(kProxyKey).toString() ==
                                    QStringLiteral("system"));
    }
    loadModels();
}

void TranslationPageController::cancelRequest(SnowShotApiClient::RequestToken& token) {
    const auto previous = std::exchange(token, 0);
    if (previous != 0 && m_client != nullptr) {
        m_client->cancel(previous);
    }
}

void TranslationPageController::deactivate() {
    m_active = false;
    m_settingsSync.stop();
    ++m_modelsGeneration;
    invalidateTranslation();
    cancelRequest(m_modelsToken);
    m_loadingModels = false;
    m_source.clear();
}

void TranslationPageController::invalidateTranslation() {
    ++m_generation;
    m_debounce.stop();
    m_requestDue = false;
    cancelRequest(m_translationToken);
    m_translating = false;
    m_result.clear();
    if (m_error == Error::Translation) {
        m_error = Error::None;
        m_errorDetail.clear();
    }
}

void TranslationPageController::scheduleTranslation() {
    invalidateTranslation();
    if (m_active && !m_composing && !m_source.trimmed().isEmpty()) {
        m_debounce.start();
    }
    emit stateChanged();
}

void TranslationPageController::setSourceText(const QString& text) {
    if (text == m_source) {
        return;
    }
    m_source = text;
    scheduleTranslation();
}

void TranslationPageController::setComposing(bool composing) {
    if (m_composing != composing) {
        m_composing = composing;
        scheduleTranslation();
    }
}

void TranslationPageController::syncPreferences() {
    auto next = m_preferences;
    next.sourceLanguage = m_settings.value(kSourceKey).toString();
    if (next.sourceLanguage.isEmpty()) {
        next.sourceLanguage = QStringLiteral("auto");
    }
    next.targetLanguage = m_settings.value(kTargetKey).toString();
    if (next.targetLanguage.isEmpty()) {
        next.targetLanguage = m_defaultTarget;
    }
    next.modelId = m_settings.value(kModelKey).toString();
    const int index = translationModelIndex(m_models, next.modelId);
    if (index >= 0) {
        next.modelId = m_models.at(index).id;
    }
    if (next != m_preferences) {
        m_preferences = next;
        scheduleTranslation();
    }
}

bool TranslationPageController::setPreferences(const QString& source, const QString& target,
                                               const QString& model) {
    const bool saved =
        m_settings.setValues({{kSourceKey, source}, {kTargetKey, target}, {kModelKey, model}});
    m_settingsSync.stop();
    if (!saved) {
        m_error = Error::Storage;
        m_errorDetail.clear();
    } else if (m_error == Error::Storage) {
        m_error = Error::None;
    }
    syncPreferences();
    emit stateChanged();
    return saved;
}

void TranslationPageController::swapLanguages() {
    if (m_preferences.sourceLanguage != QStringLiteral("auto") &&
        m_preferences.sourceLanguage != m_preferences.targetLanguage) {
        setPreferences(m_preferences.targetLanguage, m_preferences.sourceLanguage,
                       m_preferences.modelId);
    }
}

void TranslationPageController::loadModels() {
    if (!m_active || m_loadingModels) {
        return;
    }
    const quint64 generation = ++m_modelsGeneration;
    m_loadingModels = true;
    m_error = Error::None;
    m_errorDetail.clear();
    emit stateChanged();
    if (m_client != nullptr) {
        m_modelsToken = m_client->fetchChatModels(
            m_locale.name(), this, [this, generation](SnowShotChatModelsResult response) {
                if (!m_active || generation != m_modelsGeneration) {
                    return;
                }
                m_modelsToken = 0;
                m_loadingModels = false;
                m_models = response.models;
                if (!response.succeeded() || translationModelIndex(m_models, {}) < 0) {
                    m_error = Error::Models;
                    m_errorDetail = response.error;
                    emit stateChanged();
                    return;
                }
                // Resolving the initial default is not a user edit: keep the elapsed debounce.
                const int index = translationModelIndex(m_models, m_preferences.modelId);
                m_preferences.modelId = m_models.at(index).id;
                emit stateChanged();
                startTranslation();
            });
    }
    if (m_modelsToken == 0) {
        m_loadingModels = false;
        m_error = Error::Models;
        emit stateChanged();
    }
}

void TranslationPageController::startTranslation() {
    if (!m_active || m_composing || !m_requestDue || m_loadingModels ||
        m_source.trimmed().isEmpty() || m_error == Error::Models) {
        return;
    }
    const int index = translationModelIndex(m_models, m_preferences.modelId);
    if (m_client == nullptr || index < 0) {
        m_error = Error::Models;
        emit stateChanged();
        return;
    }
    m_requestDue = false;
    m_translating = true;
    const quint64 generation = m_generation;
    const auto& model = m_models.at(index);
    // Stable language codes work for both general models and provider-specific translation modes.
    const SnowShotTranslationRequest request{model.id, m_preferences.sourceLanguage,
                                             m_preferences.targetLanguage, m_source,
                                             model.translationMode};
    emit stateChanged();
    m_translationToken = m_client->streamTranslation(
        request, this,
        [this, generation](const QString& delta) {
            if (m_active && generation == m_generation) {
                m_result += delta;
                emit resultChanged();
            }
        },
        [this, generation](SnowShotTranslationResult response) {
            if (!m_active || generation != m_generation) {
                return;
            }
            m_translationToken = 0;
            m_translating = false;
            if (!response.succeeded()) {
                m_error = Error::Translation;
                m_errorDetail = response.error;
            }
            emit stateChanged();
        });
    if (m_translationToken == 0) {
        m_translating = false;
        m_error = Error::Translation;
        m_errorDetail.clear();
        emit stateChanged();
    }
}

void TranslationPageController::retry() {
    if (!m_active) {
        return;
    }
    const bool needModels = m_error == Error::Models || translationModelIndex(m_models, {}) < 0;
    invalidateTranslation();
    m_error = Error::None;
    m_errorDetail.clear();
    m_requestDue = !m_source.trimmed().isEmpty();
    if (needModels) {
        loadModels();
    } else {
        startTranslation();
    }
    emit stateChanged();
}

void TranslationPageController::setLocale(const QLocale& locale) {
    m_locale = locale;
}

QString TranslationPageController::errorText() const {
    switch (m_error) {
    case Error::None:
        return {};
    case Error::Storage:
        return tr(
            "Unable to save translation preferences. Your previous selections were restored.");
    case Error::Models:
        return m_errorDetail.isEmpty()
                   ? tr("No translation services are available.")
                   : tr("Unable to load translation services: %1").arg(m_errorDetail);
    case Error::Translation:
        return m_errorDetail.isEmpty() ? tr("Translation failed. Try again.")
                                       : tr("Translation failed: %1").arg(m_errorDetail);
    }
    return {};
}
} // namespace snow_shot::presentation
