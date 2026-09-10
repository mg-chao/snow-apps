#include "snow_shot/translation/translationservice.h"

#include "snow_shot/translation/translationlanguages.h"
#include "snow_shot/storage/configurationstore.h"

#include <QTimer>
#include <algorithm>
#include <utility>

namespace snow_shot::translation {
namespace {
const QString sourceKey = QStringLiteral("screenshot_translation/source_language");
const QString targetKey = QStringLiteral("screenshot_translation/target_language");
const QString modelKey = QStringLiteral("screenshot_translation/model");
const QString customKey = QStringLiteral("api_configuration/custom_models");
const QString proxyKey = QStringLiteral("network/proxy");
} // namespace

TranslationService& TranslationService::forClient(SnowShotApiClient& client,
                                                  storage::ConfigurationStore& settings,
                                                  const QLocale& locale) {
    for (auto* service :
         client.findChildren<TranslationService*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (service->m_settings == &settings) {
            return *service;
        }
    }
    return *new TranslationService(client, settings, locale);
}

TranslationService::TranslationService(SnowShotApiClient& client,
                                       storage::ConfigurationStore& settings, QLocale locale)
    : QObject(&client), m_client(&client), m_settings(&settings), m_locale(std::move(locale)),
      m_defaultTarget(defaultTranslationTargetLanguage(m_locale)) {
    client.setCustomModels(customAiModelsFromJson(settings.value(customKey)));
    client.setUseSystemProxy(settings.value(proxyKey).toString() == QStringLiteral("system"));
    connect(&settings, &QObject::destroyed, this, [this] { delete this; });
    connect(&settings, &storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
                if (m_client == nullptr || m_settings == nullptr)
                    return;
                if (key == customKey) {
                    m_client->setCustomModels(customAiModelsFromJson(m_settings->value(customKey)));
                } else if (key == proxyKey) {
                    m_client->setUseSystemProxy(m_settings->value(proxyKey).toString() ==
                                                QStringLiteral("system"));
                } else if (!m_saving && (key == sourceKey || key == targetKey || key == modelKey)) {
                    syncPreferences(ChangeReason::UserPreferences);
                }
            });
    connect(&client, &SnowShotApiClient::customModelInvalidated, this,
            [this](const QString& id, bool translation, bool) {
                if (translation) {
                    m_modelInvalidationPending = true;
                    emit modelInvalidated(id);
                }
            });
    connect(&client, &SnowShotApiClient::chatModelsChanged, this,
            [this] { publishModels(!loadingModels()); });
    connect(&client, &QObject::destroyed, this, [this] {
        m_modelsToken = 0;
        emit unavailable();
    });
    syncPreferences(ChangeReason::Catalog);
    publishModels(false);
}

TranslationService::~TranslationService() {
    if (m_client != nullptr)
        m_client->cancel(m_modelsToken);
}

void TranslationService::syncPreferences(ChangeReason reason) {
    if (m_settings == nullptr)
        return;
    TranslationPreferences next{m_settings->value(sourceKey).toString(),
                                m_settings->value(targetKey).toString(),
                                m_settings->value(modelKey).toString()};
    if (next.sourceLanguage.isEmpty())
        next.sourceLanguage = QStringLiteral("auto");
    if (next.targetLanguage.isEmpty())
        next.targetLanguage = m_defaultTarget;
    if (next != m_preferences) {
        m_preferences = next;
        emit preferencesChanged(reason);
    }
}

bool TranslationService::savePreferences(const TranslationPreferences& preferences) {
    if (m_settings == nullptr)
        return false;
    m_saving = true;
    const bool saved = m_settings->setValues({{sourceKey, preferences.sourceLanguage},
                                              {targetKey, preferences.targetLanguage},
                                              {modelKey, preferences.modelId}});
    m_saving = false;
    m_storageError = !saved;
    syncPreferences(ChangeReason::UserPreferences);
    emit catalogChanged();
    return saved;
}

void TranslationService::publishModels(bool resolveSelection) {
    if (m_client == nullptr)
        return;
    m_models.clear();
    for (const auto& model : m_client->cachedChatModels()) {
        if (model.supportsTranslation())
            m_models.append(model);
    }
    // Select renders a header for each contiguous group. Keep custom and server general
    // models together while preserving their order within each translation category.
    std::stable_partition(m_models.begin(), m_models.end(), [](const auto& model) {
        return model.translationMode == QStringLiteral("default");
    });
    const auto reason =
        m_modelInvalidationPending ? ChangeReason::ModelInvalidated : ChangeReason::Catalog;
    const int index = translationModelIndex(m_models, m_preferences.modelId);
    const bool missingCustom = m_preferences.modelId.startsWith(QStringLiteral("custom:")) &&
                               !m_client->isCustomModel(m_preferences.modelId);
    const bool localDefault = m_preferences.modelId.isEmpty() && index >= 0 &&
                              m_client->isCustomModel(m_models.at(index).id);
    if ((resolveSelection || missingCustom || localDefault) && (index >= 0 || missingCustom)) {
        const QString resolved = index >= 0 ? m_models.at(index).id : QString();
        if (resolved != m_preferences.modelId && m_settings != nullptr) {
            m_saving = true;
            m_storageError = !m_settings->setValue(modelKey, resolved);
            m_saving = false;
            syncPreferences(reason);
        }
    }
    m_modelInvalidationPending = false;
    emit catalogChanged();
}

void TranslationService::refreshModels(bool force) {
    if (m_client == nullptr || loadingModels())
        return;
    m_catalogAttempted = true;
    if (!force && m_client->hasBuiltInModels(m_locale.name())) {
        publishModels(true);
        return;
    }
    m_catalogError.clear();
    m_modelsToken =
        m_client->fetchChatModels(m_locale.name(), this, [this](SnowShotChatModelsResult result) {
            m_modelsToken = 0;
            m_catalogError = result.error;
            publishModels(result.succeeded());
        });
    if (m_modelsToken == 0)
        m_catalogError = tr("No translation services are available.");
    emit catalogChanged();
}

void TranslationService::setLocale(const QLocale& locale) {
    if (locale == m_locale)
        return;
    m_locale = locale;
    if (m_client != nullptr)
        m_client->cancel(std::exchange(m_modelsToken, 0));
    // The effective target belongs to the translation, not to the language of its UI.
    refreshModels();
}

QString TranslationService::errorText() const {
    if (m_storageError)
        return tr(
            "Unable to save translation preferences. Your previous selections were restored.");
    if (!m_models.isEmpty() || loadingModels() || !m_catalogAttempted)
        return {};
    return m_catalogError.isEmpty()
               ? tr("No translation services are available.")
               : tr("Unable to load translation services: %1").arg(m_catalogError);
}

QString TranslationService::modelConfigurationChangedText() {
    return tr("Model configuration changed. Retry to use the updated settings.");
}

TranslationJob* TranslationService::createJob(const QStringList& texts, QObject* owner) {
    return new TranslationJob(*this, texts, owner);
}

TranslationJob::TranslationJob(TranslationService& service, const QStringList& texts,
                               QObject* owner)
    : QObject(owner), m_service(&service), m_client(service.m_client),
      m_preferences(service.preferences()) {
    for (const auto& text : texts)
        m_units.append({text, {}, State::Idle});
    connect(&service, &TranslationService::catalogChanged, this, [this] {
        if (m_state == State::WaitingForModels)
            prepare();
    });
    connect(&service, &TranslationService::modelInvalidated, this,
            &TranslationJob::invalidateModel);
    connect(&service, &TranslationService::unavailable, this, [this] {
        if (busy())
            fail(tr("Translation service is unavailable"));
    });
    connect(&service, &QObject::destroyed, this, [this] {
        if (busy())
            fail(tr("Translation service is unavailable"));
    });
}

TranslationJob::~TranslationJob() {
    cancel();
}

void TranslationJob::cancel() {
    ++m_generation;
    const auto requests = std::exchange(m_requests, {});
    if (m_client != nullptr)
        for (auto token : requests)
            m_client->cancel(token);
    if (busy()) {
        m_state = State::Cancelled;
        for (auto& unit : m_units) {
            if (unit.state == State::Streaming || unit.state == State::Idle)
                unit.state = State::Cancelled;
        }
    }
}

void TranslationJob::start() {
    retry();
}

void TranslationJob::retry() {
    cancel();
    m_result = {};
    m_rateLimited = false;
    if (m_service == nullptr || m_client == nullptr) {
        fail(tr("Translation service is unavailable"));
        return;
    }
    const bool changed = m_preferences != m_service->preferences() || m_state == State::Invalidated;
    m_preferences = m_service->preferences();
    for (auto& unit : m_units) {
        if (changed) {
            unit.text.clear();
            unit.state = State::Idle;
        }
        if (unit.sourceText.trimmed().isEmpty())
            unit.state = State::Completed;
        else if (unit.state != State::Completed)
            unit.state = State::Idle;
    }
    m_state = State::WaitingForModels;
    const QPointer<TranslationJob> guard(this);
    emit stateChanged();
    if (guard == nullptr)
        return;
    prepare();
}

void TranslationJob::prepare() {
    if (m_state != State::WaitingForModels || m_service == nullptr)
        return;
    if (std::all_of(m_units.cbegin(), m_units.cend(),
                    [](const auto& unit) { return unit.state == State::Completed; })) {
        m_state = State::Completed;
        const QPointer<TranslationJob> guard(this);
        emit stateChanged();
        if (guard != nullptr && m_state == State::Completed)
            emit finished();
        return;
    }
    const auto& models = m_service->models();
    const auto it = std::find_if(models.cbegin(), models.cend(), [this](const auto& model) {
        return model.id == m_preferences.modelId;
    });
    if (it == models.cend()) {
        if (m_service->loadingModels())
            return;
        if (!m_service->m_catalogAttempted) {
            m_service->refreshModels();
            return;
        }
        const int index = translationModelIndex(models, m_service->preferences().modelId);
        if (index < 0) {
            fail(m_service->errorText());
            return;
        }
        m_preferences.modelId = models.at(index).id;
        m_translationMode = models.at(index).translationMode;
    } else {
        m_translationMode = it->translationMode;
    }
    m_state = State::Streaming;
    pump();
}

void TranslationJob::pump() {
    if (m_state != State::Streaming || m_client == nullptr)
        return;
    const auto generation = m_generation;
    for (int index = 0; index < m_units.size() && m_requests.size() < 4 && !m_rateLimited;
         ++index) {
        auto& unit = m_units[index];
        if (unit.state != State::Idle)
            continue;
        unit.state = State::Streaming;
        unit.text.clear();
        const SnowShotTranslationRequest request{
            m_preferences.modelId, m_preferences.sourceLanguage, m_preferences.targetLanguage,
            unit.sourceText, m_translationMode};
        const auto token = m_client->streamTranslation(
            request, this,
            [this, generation, index](const QString& delta) {
                if (generation != m_generation || m_state != State::Streaming)
                    return;
                m_units[index].text += delta;
                emit unitChanged(index);
            },
            [this, generation, index](SnowShotTranslationResult result) {
                if (generation != m_generation || !m_requests.remove(index))
                    return;
                auto& completed = m_units[index];
                if (result.succeeded() && completed.text.trimmed().isEmpty())
                    result.error = tr("The model returned no content");
                completed.state = result.succeeded() ? State::Completed : State::Failed;
                if (!result.succeeded())
                    m_result = result;
                if (result.httpStatus == 429) {
                    m_rateLimited = true;
                    for (auto& pending : m_units)
                        if (pending.state == State::Idle)
                            pending.state = State::Failed;
                }
                pump();
            });
        if (token != 0)
            m_requests.insert(index, token);
        else {
            unit.state = State::Failed;
            m_result.error = tr("Translation request could not be prepared");
        }
    }
    if (m_requests.isEmpty()) {
        m_state = std::any_of(m_units.cbegin(), m_units.cend(),
                              [](const auto& unit) { return unit.state == State::Failed; })
                      ? State::Failed
                      : State::Completed;
    }
    const QPointer<TranslationJob> guard(this);
    emit stateChanged();
    if (guard != nullptr && generation == m_generation && !busy())
        emit finished();
}

void TranslationJob::fail(const QString& error) {
    cancel();
    m_result.error = error.isEmpty() ? tr("Translation service is unavailable") : error;
    m_state = State::Failed;
    for (auto& unit : m_units)
        if (unit.state != State::Completed)
            unit.state = State::Failed;
    const QPointer<TranslationJob> guard(this);
    emit stateChanged();
    if (guard != nullptr)
        emit finished();
}

void TranslationJob::invalidateModel(const QString& id) {
    if (m_preferences.modelId != id)
        return;
    cancel();
    for (auto& unit : m_units) {
        unit.text.clear();
        unit.state = State::Idle;
    }
    m_state = State::Invalidated;
    m_result.error = TranslationService::modelConfigurationChangedText();
    const QPointer<TranslationJob> guard(this);
    emit stateChanged();
    if (guard != nullptr)
        emit invalidated();
}

QString TranslationJob::errorText() const {
    if (m_state == State::Invalidated) {
        return TranslationService::modelConfigurationChangedText();
    }
    return m_result.error;
}
} // namespace snow_shot::translation
