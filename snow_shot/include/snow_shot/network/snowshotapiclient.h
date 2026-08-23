#ifndef SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H
#define SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

class QNetworkAccessManager;

struct SnowShotTableResult {
    QString html;
    QString error;
    QString code;
    int httpStatus = 0;

    [[nodiscard]] bool succeeded() const {
        return !html.trimmed().isEmpty() && error.isEmpty();
    }
};

struct SnowShotChatModel {
    QString id;
    QString name;
    bool thinking = false;
    bool supportsVision = false;
};

struct SnowShotChatModelsResult {
    QVector<SnowShotChatModel> models;
    QString error;
    QString code;
    int httpStatus = 0;

    [[nodiscard]] bool succeeded() const {
        return !models.isEmpty() && error.isEmpty();
    }
};

struct SnowShotTranslationRequest {
    QString model;
    QString sourceLanguage;
    QString targetLanguage;
    QString text;
};

struct SnowShotTranslationResult {
    QString error;
    QString code;
    int httpStatus = 0;
    bool cancelled = false;

    [[nodiscard]] bool succeeded() const {
        return error.isEmpty() && !cancelled;
    }
};

class SnowShotApiClient final : public QObject {
    Q_OBJECT

  public:
    using RequestToken = quint64;
    using Completion = std::function<void(SnowShotTableResult)>;
    using ChatModelsCompletion = std::function<void(SnowShotChatModelsResult)>;
    using TranslationDelta = std::function<void(const QString&)>;
    using TranslationCompletion = std::function<void(SnowShotTranslationResult)>;

    explicit SnowShotApiClient(QString baseUrl, QObject* parent = nullptr);
    ~SnowShotApiClient() override;

    [[nodiscard]] const QString& baseUrl() const;
    [[nodiscard]] bool usesSystemProxy() const;
    void setUseSystemProxy(bool enabled);
    [[nodiscard]] const QVector<SnowShotChatModel>& cachedChatModels() const;
    [[nodiscard]] RequestToken extractTable(const QImage& image, QObject* receiver,
                                             Completion completion);
    [[nodiscard]] RequestToken fetchChatModels(const QString& locale, QObject* receiver,
                                               ChatModelsCompletion completion);
    [[nodiscard]] RequestToken streamTranslation(const SnowShotTranslationRequest& request,
                                                 QObject* receiver, TranslationDelta delta,
                                                 TranslationCompletion completion);
    void cancel(RequestToken token);

    [[nodiscard]] static QImage prepareImage(const QImage& image);
    [[nodiscard]] static QByteArray encodeWebp(const QImage& image);
    [[nodiscard]] static QString formatFailure(int httpStatus, const QString& failureCode,
                                               const QString& description);

  private:
    struct Request;
    [[nodiscard]] QNetworkAccessManager* networkAccessManager();
    void finish(RequestToken token, SnowShotTableResult result);
    void finishChatModels(RequestToken token, SnowShotChatModelsResult result);
    void finishTranslation(RequestToken token, SnowShotTranslationResult result);

    QString m_baseUrl;
    bool m_useSystemProxy = false;
    RequestToken m_nextToken = 0;
    QHash<RequestToken, Request*> m_requests;
    QVector<SnowShotChatModel> m_cachedChatModels;
};

#endif // SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H
