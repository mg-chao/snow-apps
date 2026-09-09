#ifndef SNOW_SHOT_PRESENTATION_TRANSLATIONPAGECONTROLLER_H
#define SNOW_SHOT_PRESENTATION_TRANSLATIONPAGECONTROLLER_H

#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/translation/translationservice.h"

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
    [[nodiscard]] const translation::TranslationPreferences& preferences() const {
        static const translation::TranslationPreferences empty;
        return m_service != nullptr ? m_service->preferences() : empty;
    }
    [[nodiscard]] const QVector<SnowShotChatModel>& models() const {
        static const QVector<SnowShotChatModel> empty;
        return m_service != nullptr ? m_service->models() : empty;
    }
    [[nodiscard]] bool loadingModels() const {
        return m_active && m_service != nullptr && m_service->loadingModels();
    }
    [[nodiscard]] bool translating() const {
        return m_job != nullptr && m_job->busy();
    }
    [[nodiscard]] bool active() const {
        return m_active;
    }
    [[nodiscard]] QString errorText() const;

  signals:
    // Stream content is separate from preferences, request lifecycle, and error state.
    void resultChanged();
    void stateChanged();

  private:
    void invalidateTranslation();
    void scheduleTranslation();
    void startTranslation();
    QPointer<translation::TranslationService> m_service;
    QPointer<translation::TranslationJob> m_job;
    QTimer m_debounce;
    QString m_source;
    QString m_result;
    bool m_active = false;
    bool m_composing = false;
    bool m_requestDue = false;
    bool m_retryRequired = false;
};
} // namespace snow_shot::presentation

#endif
