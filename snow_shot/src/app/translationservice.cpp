#include "snow_shot/translation/translationservice.h"
#include "snow_shot/translation/freetranslation.h"

#include "snow_shot/translation/translationlanguages.h"
#include "snow_shot/storage/configurationstore.h"

#include <QTimer>
#include <algorithm>
#include <utility>

namespace snow_shot::translation {
namespace {
const QString sourceKey = QStringLiteral("screenshot_translation/source_language");
const QString targetKey = QStringLiteral("screenshot_translation/target_language");
const QString secondaryTargetKey =
    QStringLiteral("screenshot_translation/secondary_target_language");
const QString confidenceThresholdKey =
    QStringLiteral("screenshot_translation/language_detection_confidence_threshold");
const QString modelKey = QStringLiteral("screenshot_translation/model");
const QString customKey = QStringLiteral("api_configuration/custom_models");
const QString textKey = QStringLiteral("api_configuration/text_translation");
const QString proxyKey = QStringLiteral("network/proxy");
const QString httpProxyKey = QStringLiteral("network/http_proxy");
const QString serverKey = QStringLiteral("api_configuration/server_url");
} // namespace

TranslationService& TranslationService::forClient(SnowShotApiClient& client,
                                                  storage::ConfigurationStore& settings,
                                                  const QLocale& locale,
                                                  TranslationLanguageDetector languageDetector) {
    for (auto* service :
         client.findChildren<TranslationService*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (service->m_settings == &settings) {
            return *service;
        }
    }
    return *new TranslationService(client, settings, locale, std::move(languageDetector));
}

TranslationService::TranslationService(SnowShotApiClient& client,
                                       storage::ConfigurationStore& settings, QLocale locale,
                                       TranslationLanguageDetector languageDetector)
    : QObject(&client), m_client(&client), m_settings(&settings), m_locale(std::move(locale)),
      m_defaultTarget(defaultTranslationTargetLanguage(m_locale)),
      m_languageDetector(languageDetector
                             ? std::move(languageDetector)
                             : TranslationLanguageDetector(detectTranslationLanguage)) {
    client.setCustomModels(customAiModelsFromJson(settings.value(customKey)));
    client.setTextTranslationConfigurations(
        textTranslationConfigurationsFromJson(settings.value(textKey)));
    client.setProxyConfiguration(settings.value(proxyKey).toString(),
                                 settings.value(httpProxyKey).toString());
    connect(&settings, &QObject::destroyed, this, [this] { delete this; });
    connect(&settings, &storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
                if (m_client == nullptr || m_settings == nullptr)
                    return;
                if (key == serverKey) {
                    static_cast<void>(m_client->setBaseUrl(SnowShotApiClient::configuredBaseUrl(
                        m_settings->value(serverKey).toString())));
                } else if (key == customKey) {
                    m_client->setCustomModels(customAiModelsFromJson(m_settings->value(customKey)));
                } else if (key == textKey) {
                    m_client->setTextTranslationConfigurations(
                        textTranslationConfigurationsFromJson(m_settings->value(textKey)));
                } else if (key == proxyKey || key == httpProxyKey) {
                    m_client->setProxyConfiguration(m_settings->value(proxyKey).toString(),
                                                    m_settings->value(httpProxyKey).toString());
                } else if (!m_saving &&
                           (key == sourceKey || key == targetKey || key == secondaryTargetKey ||
                            key == confidenceThresholdKey || key == modelKey)) {
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
    connect(&client, &SnowShotApiClient::baseUrlChanged, this, [this] {
        m_client->cancel(std::exchange(m_modelsToken, 0));
        refreshModels(true);
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
    TranslationPreferences next{
        m_settings->value(sourceKey).toString(), m_settings->value(targetKey).toString(),
        m_settings->value(modelKey).toString(), m_settings->value(secondaryTargetKey).toString(),
        m_settings->value(confidenceThresholdKey).toInt(60)};
    if (next.sourceLanguage.isEmpty())
        next.sourceLanguage = QStringLiteral("auto");
    if (next.targetLanguage.isEmpty())
        next.targetLanguage = m_defaultTarget;
    if (next.secondaryTargetLanguage.isEmpty())
        next.secondaryTargetLanguage = QStringLiteral("en");
    if (next != m_preferences) {
        m_preferences = next;
        emit preferencesChanged(reason);
    }
}

std::optional<bool> TranslationService::writePreferences(const QMap<QString, QJsonValue>& values,
                                                         ChangeReason reason) {
    if (m_settings == nullptr)
        return std::nullopt;
    const QPointer<TranslationService> lifetime(this);
    const quint64 generation = ++m_preferencesGeneration;
    const bool previousSaving = std::exchange(m_saving, true);
    const bool saved = m_settings->setValues(values);
    if (!lifetime)
        return std::nullopt;
    m_saving = previousSaving;
    if (generation != m_preferencesGeneration)
        return std::nullopt;
    m_storageError = !saved;
    syncPreferences(reason);
    if (!lifetime || generation != m_preferencesGeneration)
        return std::nullopt;
    return saved;
}

bool TranslationService::savePreferences(const TranslationPreferences& preferences) {
    const auto saved = writePreferences(
        {{sourceKey, preferences.sourceLanguage},
         {targetKey, preferences.targetLanguage},
         {secondaryTargetKey, preferences.secondaryTargetLanguage},
         {confidenceThresholdKey, preferences.languageDetectionConfidenceThreshold},
         {modelKey, preferences.modelId}},
        ChangeReason::UserPreferences);
    if (!saved.has_value())
        return false;
    const QPointer<TranslationService> lifetime(this);
    const quint64 generation = m_preferencesGeneration;
    emit catalogChanged();
    return lifetime && generation == m_preferencesGeneration && *saved;
}

void TranslationService::publishModels(bool resolveSelection) {
    if (m_client == nullptr)
        return;
    m_models = m_client->cachedChatModels();
    m_models.removeIf([](const auto& model) { return model.id == freeTranslationId; });
    m_models.prepend({freeTranslationId, tr("Free translation"), false,
                      QStringLiteral("free-translation"), false});
    // Select renders a header for each contiguous group. Keep custom and server general
    // models together while preserving their order within each translation category.
    std::stable_partition(m_models.begin(), m_models.end(), [](const auto& model) {
        return model.translationMode == QStringLiteral("default");
    });
    const auto reason =
        m_modelInvalidationPending ? ChangeReason::ModelInvalidated : ChangeReason::Catalog;
    // A catalog can change inside a settings write, before the preference cache is synchronized.
    const QString selectedModel =
        m_settings != nullptr ? m_settings->value(modelKey).toString() : m_preferences.modelId;
    const int index = translationModelIndex(m_models, selectedModel);
    const bool unresolvedLegacy =
        snowShotLegacyAiModelId(selectedModel) &&
        snowShotResolvedBuiltInModelId(m_models, selectedModel) == selectedModel &&
        std::none_of(m_models.cbegin(), m_models.cend(),
                     [&selectedModel](const auto& model) { return model.id == selectedModel; });
    const bool missingCustom = (selectedModel.startsWith(QStringLiteral("custom:")) &&
                                !m_client->isCustomModel(selectedModel)) ||
                               (selectedModel.startsWith(QStringLiteral("translation:")) &&
                                !m_client->isTextTranslation(selectedModel));
    const bool localDefault = selectedModel.isEmpty() && index >= 0 &&
                              (m_models.at(index).id == freeTranslationId ||
                               m_client->isCustomModel(m_models.at(index).id) ||
                               m_client->isTextTranslation(m_models.at(index).id));
    if (!unresolvedLegacy && (resolveSelection || missingCustom || localDefault) &&
        (index >= 0 || missingCustom)) {
        const QString resolved = index >= 0 ? m_models.at(index).id : QString();
        if (resolved != selectedModel && m_settings != nullptr) {
            if (!writePreferences({{modelKey, resolved}}, reason).has_value())
                return;
        }
    }
    m_modelInvalidationPending = false;
    emit catalogChanged();
}

void TranslationService::refreshModels(bool force) {
    if (m_client == nullptr || loadingModels())
        return;
    m_catalogAttempted = true;
    m_catalogError.clear();
    m_modelsToken = m_client->ensureChatModels(
        m_locale.name(), this,
        [this](SnowShotChatModelsResult result) {
            m_modelsToken = 0;
            m_catalogError = result.error;
            publishModels(result.succeeded());
        },
        force ? SnowShotApiClient::ChatModelsCachePolicy::Refresh
              : SnowShotApiClient::ChatModelsCachePolicy::UseCached);
    if (m_modelsToken == 0)
        m_catalogError = tr("No translation services are available");
    emit catalogChanged();
}

void TranslationService::setLocale(const QLocale& locale) {
    if (locale == m_locale) {
        // LanguageChange can arrive before the language manager publishes its new locale.
        publishModels(false);
        return;
    }
    m_locale = locale;
    const QPointer<TranslationService> guard(this);
    publishModels(false);
    if (!guard)
        return;
    if (m_client != nullptr)
        m_client->cancel(std::exchange(m_modelsToken, 0));
    // The effective target belongs to the translation, not to the language of its UI.
    refreshModels();
}

QString TranslationService::errorText() const {
    if (m_storageError)
        return tr("Unable to save translation preferences. Your previous selections were restored");
    if (!m_models.isEmpty() || loadingModels() || !m_catalogAttempted)
        return {};
    return m_catalogError.isEmpty()
               ? tr("No translation services are available")
               : tr("Unable to load translation services: %1").arg(m_catalogError);
}

QString TranslationService::modelConfigurationChangedText() {
    return tr("Model configuration changed. Retry to use the updated settings");
}

TranslationJob* TranslationService::createJob(const QStringList& texts, QObject* owner) {
    return new TranslationJob(*this, texts, owner);
}

TranslationJob* TranslationService::createJob(const QStringList& texts,
                                              const TranslationPreferences& preferences,
                                              QObject* owner) {
    auto* job = new TranslationJob(*this, texts, owner);
    job->m_preferences = preferences;
    job->m_fixedPreferences = true;
    return job;
}

TranslationJob::TranslationJob(TranslationService& service, const QStringList& texts,
                               QObject* owner)
    : QObject(owner), m_service(&service), m_client(service.m_client),
      m_preferences(service.preferences()) {
    QHash<QString, int> groupsBySource;
    for (const auto& text : texts) {
        const int index = static_cast<int>(m_units.size());
        m_units.append({text, {}, State::Idle});
        const auto existing = groupsBySource.constFind(text);
        if (existing == groupsBySource.cend()) {
            groupsBySource.insert(text, static_cast<int>(m_unitGroups.size()));
            m_unitGroups.append(QVector<int>{index});
        } else {
            m_unitGroups[*existing].append(index);
        }
    }
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
    const bool changed = (!m_fixedPreferences && m_preferences != m_service->preferences()) ||
                         m_state == State::Invalidated;
    if (!m_fixedPreferences)
        m_preferences = m_service->preferences();
    bool hasPendingUnits = false;
    for (auto& unit : m_units) {
        if (changed) {
            unit.text.clear();
            unit.state = State::Idle;
        }
        if (unit.sourceText.trimmed().isEmpty())
            unit.state = State::Completed;
        else if (unit.state != State::Completed)
            unit.state = State::Idle;
        hasPendingUnits |= unit.state == State::Idle;
    }
    // Detect lazily so unchanged targets and empty jobs avoid joining and analyzing text.
    // A later retry can enable routing; its first detection still includes every original unit.
    if (hasPendingUnits && !m_languageDetectionAttempted &&
        !m_preferences.secondaryTargetLanguage.isEmpty() &&
        m_preferences.secondaryTargetLanguage != m_preferences.targetLanguage) {
        QStringList sources;
        sources.reserve(m_units.size());
        for (const auto& unit : m_units)
            sources.append(unit.sourceText);
        m_languageDetectionAttempted = true;
        m_detectedLanguage = m_service->m_languageDetector(sources.join(u'\n'));
    }
    m_effectiveTargetLanguage = resolveTranslationTarget(m_preferences, m_detectedLanguage);
    m_state = State::WaitingForModels;
    const auto generation = m_generation;
    const QPointer<TranslationJob> guard(this);
    emit stateChanged();
    if (!guard || generation != m_generation || m_state != State::WaitingForModels)
        return;
    prepare();
}

void TranslationJob::prepare() {
    if (m_state != State::WaitingForModels || m_service == nullptr)
        return;
    if (std::all_of(m_units.cbegin(), m_units.cend(),
                    [](const auto& unit) { return unit.state == State::Completed; })) {
        m_state = State::Completed;
        publishState();
        return;
    }
    const auto& models = m_service->models();
    m_preferences.modelId = snowShotResolvedBuiltInModelId(models, m_preferences.modelId);
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
        if (!m_service->m_catalogError.isEmpty() && !m_preferences.modelId.isEmpty()) {
            fail(m_service->m_catalogError);
            return;
        }
        if (m_fixedPreferences && !m_preferences.modelId.isEmpty()) {
            fail(m_service->errorText());
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
    for (int group = 0;
         group < m_unitGroups.size() &&
         m_requests.size() < (m_client->isTextTranslation(m_preferences.modelId) ? 16 : 8) &&
         !m_rateLimited;
         ++group) {
        const auto indices = m_unitGroups[group];
        const auto& unit = m_units[indices.first()];
        if (unit.state != State::Idle)
            continue;
        for (const int index : indices) {
            m_units[index].state = State::Streaming;
            m_units[index].text.clear();
        }
        const SnowShotTranslationRequest request{
            m_preferences.modelId, m_preferences.sourceLanguage, m_effectiveTargetLanguage,
            unit.sourceText, m_translationMode};
        const auto token = m_client->streamTranslation(
            request, this,
            [this, generation, indices](const QString& delta) {
                if (generation != m_generation || m_state != State::Streaming)
                    return;
                // Publish one coherent snapshot before observers see any original position.
                QString text = m_units[indices.first()].text;
                text += delta;
                for (const int index : indices)
                    m_units[index].text = text;
                const QPointer<TranslationJob> guard(this);
                for (const int index : indices) {
                    emit unitChanged(index);
                    if (!guard || generation != m_generation || m_state != State::Streaming)
                        return;
                }
            },
            [this, generation, group, indices](SnowShotTranslationResult result) {
                if (generation != m_generation || !m_requests.remove(group))
                    return;
                if (result.succeeded() && m_units[indices.first()].text.trimmed().isEmpty())
                    result.error = tr("The model returned no content");
                for (const int index : indices)
                    m_units[index].state = result.succeeded() ? State::Completed : State::Failed;
                if (!result.succeeded())
                    m_result = result;
                if (result.httpStatus == 429) {
                    m_rateLimited = true;
                    // A terminal limit must also retire requests in the shared built-in
                    // queue. Already dispatched chat streams can finish without losing
                    // their partial output or incurring a duplicate provider call.
                    const auto requests = m_requests;
                    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
                        if (m_client->isTextTranslation(m_preferences.modelId) ||
                            m_client->isQueued(it.value())) {
                            m_requests.remove(it.key());
                            m_client->cancel(it.value());
                            for (const int index : m_unitGroups[it.key()])
                                m_units[index].state = State::Failed;
                        }
                    }
                    for (auto& pending : m_units)
                        if (pending.state == State::Idle)
                            pending.state = State::Failed;
                }
                pump();
            });
        if (token != 0)
            m_requests.insert(group, token);
        else {
            for (const int index : indices)
                m_units[index].state = State::Failed;
            m_result.error = tr("Translation request could not be prepared");
        }
    }
    if (m_requests.isEmpty()) {
        m_state = std::any_of(m_units.cbegin(), m_units.cend(),
                              [](const auto& unit) { return unit.state == State::Failed; })
                      ? State::Failed
                      : State::Completed;
    }
    if (generation == m_generation)
        publishState();
}

void TranslationJob::publishState() {
    const auto generation = m_generation;
    const auto state = m_state;
    const QPointer<TranslationJob> guard(this);
    emit stateChanged();
    // A listener may retire this transition even when its retry reaches the same state.
    if (!guard || generation != m_generation || state != m_state)
        return;
    if (state == State::Completed || state == State::Failed)
        emit finished();
    else if (state == State::Invalidated)
        emit invalidated();
}

void TranslationJob::fail(const QString& error) {
    cancel();
    m_result.error = error.isEmpty() ? tr("Translation service is unavailable") : error;
    m_state = State::Failed;
    for (auto& unit : m_units)
        if (unit.state != State::Completed)
            unit.state = State::Failed;
    publishState();
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
    publishState();
}

QString TranslationJob::errorText() const {
    if (m_state == State::Invalidated) {
        return TranslationService::modelConfigurationChangedText();
    }
    if (m_preferences.modelId == freeTranslationId && !m_result.error.isEmpty())
        return freeTranslationErrorText();
    return m_result.error;
}
} // namespace snow_shot::translation
