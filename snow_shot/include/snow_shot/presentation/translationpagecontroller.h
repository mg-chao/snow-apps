#ifndef SNOW_SHOT_PRESENTATION_TRANSLATIONPAGECONTROLLER_H
#define SNOW_SHOT_PRESENTATION_TRANSLATIONPAGECONTROLLER_H

#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QLocale>
#include <QObject>
#include <QTimer>

namespace snow_shot::storage {
class ConfigurationStore;
}

namespace snow_shot::presentation {
class TranslationPageController final : public QObject {
    Q_OBJECT

  public:
    TranslationPageController(SnowShotApiClient& client, storage::ConfigurationStore& settings,
                              QLocale locale, QObject* parent = nullptr,
                              int debounceMilliseconds = 1500);
    ~TranslationPageController() override;

    void activate();
    void deactivate();
    void setSourceText(const QString& text);
    void setComposing(bool composing);
    bool setPreferences(const QString& source, const QString& target, const QString& model);
    void swapLanguages();
    void retry();
    void setLocale(const QLocale& locale);

    [[nodiscard]] const QString& sourceText() const {
        return m_source;
    }
    [[nodiscard]] const QString& resultText() const {
        return m_result;
    }
    [[nodiscard]] const storage::ScreenshotTranslationConfiguration& preferences() const {
        return m_preferences;
    }
    [[nodiscard]] const QVector<SnowShotChatModel>& models() const {
        return m_models;
    }
    [[nodiscard]] bool loadingModels() const {
        return m_loadingModels;
    }
    [[nodiscard]] bool translating() const {
        return m_translating;
    }
    [[nodiscard]] bool active() const {
        return m_active;
    }
    [[nodiscard]] QString errorText() const;

  signals:
    void stateChanged();

  private:
    enum class Error { None, Models, Translation, Storage };
    void syncPreferences();
    void loadModels();
    void invalidateTranslation();
    void scheduleTranslation();
    void startTranslation();
    void cancelRequest(SnowShotApiClient::RequestToken& token);

    QPointer<SnowShotApiClient> m_client;
    storage::ConfigurationStore& m_settings;
    QLocale m_locale;
    QString m_defaultTarget;
    storage::ScreenshotTranslationConfiguration m_preferences;
    QVector<SnowShotChatModel> m_models;
    QTimer m_debounce;
    QTimer m_settingsSync;
    QString m_source;
    QString m_result;
    QString m_errorDetail;
    Error m_error = Error::None;
    SnowShotApiClient::RequestToken m_modelsToken = 0;
    SnowShotApiClient::RequestToken m_translationToken = 0;
    quint64 m_generation = 0;
    quint64 m_modelsGeneration = 0;
    bool m_active = false;
    bool m_composing = false;
    bool m_loadingModels = false;
    bool m_translating = false;
    bool m_requestDue = false;
};
} // namespace snow_shot::presentation

#endif
