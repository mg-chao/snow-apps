#ifndef SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H
#define SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H

#include "snow_shot/customaimodelconfiguration.h"

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

enum class SnowShotModelOrigin { BuiltIn, Custom };

struct SnowShotChatModel {
    QString id;
    QString name;
    bool supportsReasoning = false;
    QString translationMode = QStringLiteral("default");
    bool supportsVision = false;
    SnowShotModelOrigin origin = SnowShotModelOrigin::BuiltIn;
    [[nodiscard]] bool supportsTranslation() const {
        return origin == SnowShotModelOrigin::Custom || !supportsVision;
    }
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
    QString translationMode = QStringLiteral("default");
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

enum class SnowShotImageConversionFormat { Markdown, Html };

struct SnowShotImageConversionRequest {
    QString model;
    QImage image;
    SnowShotImageConversionFormat format = SnowShotImageConversionFormat::Markdown;
};

using SnowShotImageConversionResult = SnowShotTranslationResult;

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

    [[nodiscard]] static QString configuredBaseUrl();

    [[nodiscard]] bool usesSystemProxy() const;
    void setUseSystemProxy(bool enabled);
    [[nodiscard]] const QVector<SnowShotChatModel>& cachedChatModels() const;
    [[nodiscard]] QString cachedChatModelsLocale() const {
        return m_cachedChatModelsLocale;
    }
    [[nodiscard]] RequestToken extractTable(const QImage& image, QObject* receiver,
                                            Completion completion);
    [[nodiscard]] RequestToken fetchChatModels(const QString& locale, QObject* receiver,
                                               ChatModelsCompletion completion);
    [[nodiscard]] RequestToken streamTranslation(const SnowShotTranslationRequest& request,
                                                 QObject* receiver, TranslationDelta delta,
                                                 TranslationCompletion completion);
    [[nodiscard]] RequestToken
    streamImageConversion(const SnowShotImageConversionRequest& request, QObject* receiver,
                          TranslationDelta delta,
                          std::function<void(SnowShotImageConversionResult)> completion);
    void cancel(RequestToken token);
    void setCustomModels(const snow_shot::CustomAiModels& models);
    [[nodiscard]] bool isCustomModel(const QString& id) const;
    [[nodiscard]] QString fallbackModel(bool vision) const;
    [[nodiscard]] bool hasBuiltInModels(const QString& locale) const;
    [[nodiscard]] QString modelFingerprint(const QString& id) const;

  signals:
    void chatModelsChanged();
    void customModelInvalidated(const QString& id, bool translation, bool vision);

  public:
    [[nodiscard]] static QImage prepareImage(const QImage& image);
    [[nodiscard]] static QByteArray encodeWebp(const QImage& image);
    [[nodiscard]] static QString formatFailure(int httpStatus, const QString& failureCode,
                                               const QString& description);

  private:
    friend class SnowShotApiClientTestAccess;
    std::function<QByteArray(const QImage&)> m_tableImagePreparation;
    int m_tableTimeoutMs = 35000;
    struct Request;
    void startTableUpload(RequestToken token, const QByteArray& webp);
    void cleanupRequest(Request* request);
    [[nodiscard]] QNetworkAccessManager* networkAccessManager();
    void finish(RequestToken token, SnowShotTableResult result);
    void finishChatModels(RequestToken token, SnowShotChatModelsResult result);
    void finishTranslation(RequestToken token, SnowShotTranslationResult result);
    void startChatStream(RequestToken token, const QByteArray& body);

    void rebuildAvailableModels();
    const snow_shot::CustomAiModelConfiguration* customModel(const QString& id) const;
    QString m_baseUrl;
    snow_shot::CustomAiModels m_customModels;
    QVector<SnowShotChatModel> m_availableModels;

    bool m_useSystemProxy = false;
    RequestToken m_nextToken = 0;
    QHash<RequestToken, Request*> m_requests;
    QVector<SnowShotChatModel> m_cachedChatModels;
    QString m_cachedChatModelsLocale;
};

#endif // SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H
