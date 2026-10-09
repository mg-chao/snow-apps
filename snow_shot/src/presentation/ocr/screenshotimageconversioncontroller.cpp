#include "snow_shot/presentation/screenshotimageconversioncontroller.h"

#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/modal.h"
#include "widgets/select.h"

#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
QString currentLocale() {
    return snow_shot::presentation::LanguageManager::instance().currentLocale().name();
}
using Settings = snow_shot::storage::ScreenshotImageConversionSettings;
} // namespace

ScreenshotImageConversionController::ScreenshotImageConversionController(QObject* parent)
    : QObject(parent) {
    m_previewTimer.setInterval(100);
    m_previewTimer.setSingleShot(true);
    connect(&m_previewTimer, &QTimer::timeout, this, &ScreenshotImageConversionController::changed);
}

ScreenshotImageConversionController::~ScreenshotImageConversionController() {
    invalidate();
}

void ScreenshotImageConversionController::setProvider(SnowShotApiClient* provider) {
    if (provider == m_api) {
        return;
    }
    if (m_modal)
        m_modal->reject();
    cancelRequests();
    if (m_api != nullptr) {
        m_api->cancel(m_settingsToken);
        disconnect(m_api, nullptr, this, nullptr);
    }
    m_settingsToken = 0;
    m_api = provider;
    m_cache.removeIf([this](const auto& cached) { return !entryMatchesProvider(cached.result); });
    if (m_active && (busy() || !restoreCachedResult(Settings().visionModel()))) {
        m_source.clear();
        fail(tr("Model configuration changed. Retry to use the updated settings."));
    }
    emit resultsChanged();
    if (provider != nullptr) {
        connect(provider, &SnowShotApiClient::baseUrlChanged, this, [this]() {
            m_cache.removeIf([](const auto& cached) {
                return !cached.result.model.startsWith(QStringLiteral("custom:"));
            });
            if (m_active && !m_requestModel.startsWith(QStringLiteral("custom:"))) {
                cancelRequests();
                m_source.clear();
                fail(tr("Model configuration changed. Retry to use the updated settings."));
            }
            emit resultsChanged();
        });
        connect(
            provider, &SnowShotApiClient::customModelInvalidated, this,
            [this](const QString& id, bool, bool vision) {
                if (!vision) {
                    return;
                }
                m_cache.removeIf([&id](const auto& cached) { return cached.result.model == id; });
                if (Settings().visionModel() == id || (m_active && m_requestModel == id)) {
                    cancelRequests();
                    m_source.clear();
                    if (Settings().visionModel() == id &&
                        (!m_api->isCustomModel(id) ||
                         std::none_of(m_api->cachedChatModels().cbegin(),
                                      m_api->cachedChatModels().cend(), [&id](const auto& model) {
                                          return model.id == id && model.supportsVision;
                                      }))) {
                        Settings().setVisionModel(m_api->fallbackModel(true));
                    }
                    if (m_active) {
                        fail(tr("Model configuration changed. Retry to use the updated settings."));
                    }
                }
                emit resultsChanged();
            });
        connect(provider, &QObject::destroyed, this, [this]() {
            m_modelsToken = m_conversionToken = m_settingsToken = 0;
            if (busy()) {
                fail(tr("The image conversion service is unavailable"));
            }
        });
    }
}

void ScreenshotImageConversionController::activate(QString key, QImage image,
                                                   SnowShotImageConversionFormat format) {
    if (m_active && m_key == key && m_format == format && busy() &&
        m_requestModel == Settings().visionModel()) {
        return;
    }
    cancelRequests();
    m_key = std::move(key);
    m_image = std::move(image);
    m_format = format;
    m_active = true;
    m_source.clear();
    m_error.clear();
    start(false);
}

bool ScreenshotImageConversionController::entryMatchesProvider(
    const ScreenshotImageConversionEntry& entry) const {
    if (!entry.isValid())
        return false;
    const bool custom = entry.model.startsWith(QStringLiteral("custom:"));
    // Legacy built-in results remain viewable offline, but cannot establish server identity.
    if (!m_api)
        return !custom;
    if (custom && (!m_api->isCustomModel(entry.model) ||
                   std::none_of(m_api->cachedChatModels().cbegin(),
                                m_api->cachedChatModels().cend(), [&entry](const auto& model) {
                                    return model.id == entry.model && model.supportsVision;
                                })))
        return false;
    return !entry.modelFingerprint.isEmpty() &&
           entry.modelFingerprint == m_api->modelFingerprint(entry.model);
}

bool ScreenshotImageConversionController::restoreCachedResult(const QString& model) {
    for (qsizetype index = m_cache.size(); index > 0; --index) {
        const auto& cached = m_cache.at(index - 1);
        if (cached.key != m_key || cached.result.format != m_format ||
            cached.result.model != model || !entryMatchesProvider(cached.result))
            continue;
        auto entry = m_cache.takeAt(index - 1);
        m_source = entry.result.source;
        m_requestModel = entry.result.model;
        m_cache.append(std::move(entry));
        m_state = State::Completed;
        emit changed();
        return true;
    }
    return false;
}

void ScreenshotImageConversionController::cacheResult(const QString& key,
                                                      ScreenshotImageConversionEntry entry) {
    m_cache.removeIf([&](const auto& cached) {
        return cached.key == key &&
               snow_shot::presentation::sameImageConversionRequest(cached.result, entry);
    });
    m_cache.append({key, std::move(entry)});
    trimCache();
}

void ScreenshotImageConversionController::trimCache() {
    const auto bytes = [](const CacheEntry& cached) {
        return (cached.key.size() + cached.result.model.size() + cached.result.source.size() +
                cached.result.modelFingerprint.size()) *
               static_cast<qsizetype>(sizeof(QChar));
    };
    qsizetype totalBytes = 0;
    for (const auto& cached : m_cache)
        totalBytes += bytes(cached);
    while (!m_cache.isEmpty() &&
           (m_cache.size() > snow_shot::presentation::kMaximumImageConversionEntries ||
            totalBytes > snow_shot::presentation::kMaximumImageConversionCacheBytes)) {
        totalBytes -= bytes(m_cache.first());
        m_cache.removeFirst();
    }
}

void ScreenshotImageConversionController::cancelRequests() {
    ++m_generation;
    m_previewTimer.stop();
    if (m_api != nullptr) {
        m_api->cancel(m_modelsToken);
        m_api->cancel(m_conversionToken);
    }
    m_modelsToken = m_conversionToken = 0;
}

void ScreenshotImageConversionController::deactivate() {
    cancelRequests();
    if (m_modal != nullptr) {
        m_modal->reject();
    }
    m_active = false;
    m_state = State::Idle;
    m_source.clear();
    m_error.clear();
    emit changed();
}

void ScreenshotImageConversionController::invalidate() {
    deactivate();
    if (m_modal != nullptr) {
        m_modal->reject();
    }
    if (m_api != nullptr) {
        m_api->cancel(m_settingsToken);
    }
    m_settingsToken = 0;
    m_cache.clear();
    m_image = {};
    m_key.clear();
}

void ScreenshotImageConversionController::seed(
    const QString& key, const QVector<ScreenshotImageConversionEntry>& entries) {
    if (key.isEmpty()) {
        return;
    }
    m_cache.removeIf([&key](const auto& cached) { return cached.key == key; });
    for (const auto& entry : entries) {
        if (entryMatchesProvider(entry))
            cacheResult(key, entry);
    }
}

QVector<ScreenshotImageConversionEntry>
ScreenshotImageConversionController::entries(const QString& key) const {
    QVector<ScreenshotImageConversionEntry> entries;
    for (const auto& cached : m_cache)
        if (cached.key == key && entryMatchesProvider(cached.result))
            entries.append(cached.result);
    return entries;
}

void ScreenshotImageConversionController::retry() {
    if (m_active && !busy()) {
        start(true);
    }
}

void ScreenshotImageConversionController::fail(const QString& message) {
    m_previewTimer.stop();
    m_error = message;
    m_state = State::Failed;
    emit changed();
}

void ScreenshotImageConversionController::start(bool refreshModels) {
    cancelRequests();
    m_source.clear();
    m_error.clear();
    m_refreshConversion = refreshModels;
    m_requestModel = Settings().visionModel();
    if (!refreshModels && restoreCachedResult(m_requestModel))
        return;
    if (m_api == nullptr || m_image.isNull() || m_key.isEmpty()) {
        fail(tr("The image conversion service is unavailable"));
        return;
    }
    m_state = State::LoadingModels;
    emit changed();
    if (m_api->isCustomModel(Settings().visionModel())) {
        startWithModels(m_api->cachedChatModels());
        return;
    }
    const quint64 generation = m_generation;
    m_modelsToken = m_api->ensureChatModels(
        currentLocale(), this,
        [this, generation](SnowShotChatModelsResult result) {
            if (generation != m_generation || !m_active) {
                return;
            }
            m_modelsToken = 0;
            if (!result.succeeded()) {
                fail(tr("Unable to load vision models"));
                return;
            }
            startWithModels(result.models);
        },
        refreshModels ? SnowShotApiClient::ChatModelsCachePolicy::Refresh
                      : SnowShotApiClient::ChatModelsCachePolicy::UseCached);
    if (m_modelsToken == 0) {
        fail(tr("Unable to load vision models"));
    }
}

void ScreenshotImageConversionController::startWithModels(
    const QVector<SnowShotChatModel>& models) {
    const QString saved = Settings().visionModel();
    auto selected = std::find_if(models.cbegin(), models.cend(), [&saved](const auto& model) {
        return model.supportsVision && model.id == saved;
    });
    if (selected == models.cend()) {
        selected = std::find_if(models.cbegin(), models.cend(),
                                [](const auto& model) { return model.supportsVision; });
    }
    if (selected == models.cend()) {
        fail(tr("No vision models are available"));
        return;
    }
    const QString model = selected->id;
    m_requestModel = model;
    Settings().setVisionModel(model);
    if (!m_refreshConversion && restoreCachedResult(model))
        return;
    const QString fingerprint = m_api->modelFingerprint(model);
    m_state = State::Converting;
    emit changed();
    const quint64 generation = m_generation;
    m_conversionToken = m_api->streamImageConversion(
        {model, m_image, m_format}, this,
        [this, generation](const QString& delta) {
            if (generation != m_generation || !m_active) {
                return;
            }
            m_source += delta;
            if (!m_previewTimer.isActive()) {
                m_previewTimer.start();
            }
        },
        [this, generation, model, fingerprint](SnowShotImageConversionResult result) {
            if (generation != m_generation || !m_active) {
                return;
            }
            m_conversionToken = 0;
            m_previewTimer.stop();
            if (!result.succeeded()) {
                if (result.code == QStringLiteral("model_not_found") && m_api != nullptr) {
                    m_modelsToken = m_api->ensureChatModels(
                        currentLocale(), this,
                        [this, generation](SnowShotChatModelsResult) {
                            if (generation == m_generation) {
                                m_modelsToken = 0;
                            }
                        },
                        SnowShotApiClient::ChatModelsCachePolicy::Refresh);
                }
                fail(result.error.isEmpty() ? tr("Image conversion failed") : result.error);
                return;
            }
            m_source = normalizedImageConversionSource(m_source, m_format);
            ScreenshotImageConversionEntry entry{m_format, model, m_source};
            entry.modelFingerprint = fingerprint;
            if (!entry.isValid()) {
                fail(tr("The model returned no usable content"));
                return;
            }
            if (!entryMatchesProvider(entry)) {
                m_source.clear();
                fail(tr("Model configuration changed. Retry to use the updated settings."));
                return;
            }
            cacheResult(m_key, std::move(entry));
            m_state = State::Completed;
            emit changed();
            emit resultsChanged();
        });
    if (m_conversionToken == 0) {
        fail(tr("The image could not be prepared for conversion"));
    }
}

void ScreenshotImageConversionController::openSettings(QWidget* owner) {
    using namespace adqt::widgets;
    if (owner == nullptr || m_modal != nullptr) {
        return;
    }
    auto* modal = new AdModal(this);
    m_modal = modal;
    modal->setObjectName(QStringLiteral("screenshotImageConversionSettingsModal"));
    modal->setOwnerWindow(owner);
    modal->setMode(AdModal::Mode::Window);
    modal->setWindowModality(Qt::ApplicationModal);
    modal->setCentered(true);
    modal->setPreferredWidth(440);
    modal->setMaskVisible(false);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    modal->setStandardButtons(AdModal::StandardButton::Ok | AdModal::StandardButton::Cancel);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* alert = new AdAlert(body);
    alert->setObjectName(QStringLiteral("screenshotVisionModelsError"));
    alert->setSeverity(AdAlert::Severity::Error);
    auto* retry = new AdButton(alert);
    retry->setObjectName(QStringLiteral("screenshotVisionModelsRetry"));
    retry->setButtonStyle(AdButton::ButtonStyle::Text);
    alert->setActionsWidget(retry);
    layout->addWidget(alert);
    auto* form = new AdForm(body);
    form->setFormLayout(AdForm::FormLayout::Vertical);
    auto* select = new AdSelect(form);
    select->setObjectName(QStringLiteral("screenshotVisionModel"));
    select->setPopupLayerMode(AdSelect::PopupLayerMode::QtTool);
    auto* field = form->addField(tr("Vision Model"), select, QStringLiteral("model"));
    layout->addWidget(form);
    modal->setContentWidget(body);
    modal->setInitialFocusWidget(select);
    const auto retranslate = [modal, field, select, retry]() {
        modal->setWindowTitle(tr("Image conversion settings"));
        modal->setAcceptText(tr("OK"));
        modal->setRejectText(tr("Cancel"));
        field->setLabel(tr("Vision Model"));
        select->setAccessibleName(tr("Vision Model"));
        retry->setText(tr("Retry"));
    };
    retranslate();
    const auto apply = [modal, select, alert](const QVector<SnowShotChatModel>& models) {
        const QString selected = select->currentValue().toString().isEmpty()
                                     ? Settings().visionModel()
                                     : select->currentValue().toString();
        QVector<AdSelect::Option> options;
        for (const auto& model : models) {
            if (model.supportsVision) {
                options.push_back({model.id, model.name});
            }
        }
        select->setOptions(options);
        const bool available = !options.isEmpty();
        select->setEnabled(available);
        select->setLoading(false);
        if (available) {
            const bool found =
                std::any_of(options.cbegin(), options.cend(), [&selected](const auto& item) {
                    return item.value.toString() == selected;
                });
            select->setCurrentValue(found ? selected : options.first().value);
        } else {
            select->setCurrentValue(QVariant());
        }
        alert->setText(tr("No vision models are available"));
        alert->setVisible(!available);
        if (modal->acceptButton() != nullptr) {
            modal->acceptButton()->setEnabled(available);
        }
    };
    const QPointer<AdModal> guard(modal);
    const auto load = [this, guard, select, alert, retry, apply](bool refreshModels) {
        if (guard == nullptr) {
            return;
        }
        if (m_api != nullptr) {
            m_api->cancel(m_settingsToken);
        }
        m_settingsToken = 0;
        const bool available = m_api != nullptr && !m_api->fallbackModel(true).isEmpty();
        select->setEnabled(available);
        select->setLoading(!available);
        retry->setBusy(true);
        alert->hide();
        if (guard->acceptButton() != nullptr) {
            guard->acceptButton()->setEnabled(available);
        }
        const auto failure = [select, alert, retry]() {
            select->setLoading(false);
            retry->setBusy(false);
            alert->setText(tr("Unable to load vision models"));
            alert->show();
        };
        if (m_api == nullptr) {
            failure();
            return;
        }
        m_settingsToken = m_api->ensureChatModels(
            currentLocale(), guard.data(),
            [this, guard, retry, apply, failure](SnowShotChatModelsResult result) {
                m_settingsToken = 0;
                if (guard == nullptr) {
                    return;
                }
                retry->setBusy(false);
                if (result.succeeded()) {
                    apply(result.models);
                } else {
                    failure();
                }
            },
            refreshModels ? SnowShotApiClient::ChatModelsCachePolicy::Refresh
                          : SnowShotApiClient::ChatModelsCachePolicy::UseCached);
        if (m_settingsToken == 0) {
            failure();
        }
    };
    connect(retry, &AdButton::clicked, modal, [load]() { load(true); });
    if (m_api != nullptr) {
        connect(m_api, &QObject::destroyed, modal, [load]() { load(false); });
        connect(m_api, &SnowShotApiClient::baseUrlChanged, modal, [load]() { load(false); });
        connect(m_api, &SnowShotApiClient::chatModelsChanged, modal,
                [this, apply]() { apply(m_api->cachedChatModels()); });
    }
    connect(owner, &QObject::destroyed, modal, [modal]() { modal->reject(); });
    connect(&snow_shot::presentation::LanguageManager::instance(),
            &snow_shot::presentation::LanguageManager::languageChanged, modal,
            [retranslate, load]() {
                retranslate();
                load(false);
            });
    connect(modal, &AdModal::closeRequested, modal,
            [this, modal, select](AdModal::CloseReason reason) {
                if (reason != AdModal::CloseReason::OkAction) {
                    modal->reject();
                    return;
                }
                if (!select->isEnabled() || select->currentValue().toString().isEmpty()) {
                    return;
                }
                const QString model = select->currentValue().toString();
                const bool changed = model != Settings().visionModel();
                Settings().setVisionModel(model);
                modal->accept();
                if (changed && m_active) {
                    start(false);
                }
            });
    connect(modal, &AdModal::finished, this, [this, modal]() {
        if (m_api != nullptr) {
            m_api->cancel(m_settingsToken);
        }
        m_settingsToken = 0;
        m_modal = nullptr;
        modal->deleteLater();
    });
    // Establish the initial content before the modal measures and shows its window.
    if (m_api != nullptr) {
        apply(m_api->cachedChatModels());
    }
    if (m_api == nullptr || !m_api->hasBuiltInModels(currentLocale())) {
        load(false);
    }
    modal->open();
    if (modal->acceptButton() != nullptr) {
        modal->acceptButton()->setEnabled(select->isEnabled());
    }
}
