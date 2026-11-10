#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTABLECONVERSIONCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTABLECONVERSIONCONTROLLER_H

#include "snow_shot/app/edition.h"
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/screenshottabledocument.h"
#include <QTimer>
#include <atomic>

// Owns one recognition plus at most one format correction. Preview documents never enter
// the recognition cache or an editable session until the final stream has succeeded.
class ScreenshotTableConversionController final : public QObject {
    Q_OBJECT
  public:
    explicit ScreenshotTableConversionController(QObject* parent = nullptr);
    ~ScreenshotTableConversionController() override;
    void start(SnowShotApiClient* api, const QImage& image, const QString& model);
    void cancel();
    [[nodiscard]] bool busy() const {
        return m_busy;
    }
    [[nodiscard]] bool correcting() const {
        return m_correcting;
    }
    [[nodiscard]] const ScreenshotTableDocument& preview() const {
        return m_preview;
    }

  signals:
    void changed();
    void completed(ScreenshotTableDocument document, QString error);

  private:
    void sendAttempt(const QImage& image, const QString& model, const QString& failedSource = {});
    void parse();
    void fail(const QString& error);
    QPointer<SnowShotApiClient> m_api;
    QMetaObject::Connection m_providerDestroyed;
    SnowShotApiClient::RequestToken m_request = 0;
    std::shared_ptr<SnowShotTableVisionConversation> m_conversation;
    QTimer m_previewTimer;
    std::shared_ptr<std::atomic_bool> m_parseCancellation;
    quint64 m_generation = 0;
    quint64 m_revision = 0;
    bool m_busy = false;
    bool m_correcting = false;
    bool m_finished = false;
    bool m_parsing = false;
    QString m_source;
    ScreenshotTableDocument m_preview;
};
#endif
#endif
