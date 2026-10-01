#ifndef SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H
#define SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H

#include "snow_shot/app/edition.h"
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_LATEX_RECOGNITION || SNOW_SHOT_ENABLE_IMAGE_CONVERSION ||                     \
    SNOW_SHOT_ENABLE_API_CONFIGURATION
#include "snow_shot/customaimodelconfiguration.h"
#include "snow_shot/texttranslationconfiguration.h"
#endif

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

class QNetworkAccessManager;
class SnowShotApiClient;

struct SnowShotTableResult {
    QString html;
    QString error;
    QString code;
    int httpStatus = 0;

    [[nodiscard]] bool succeeded() const {
        return !html.trimmed().isEmpty() && error.isEmpty();
    }
};

struct SnowShotLatexResult {
    QString latex;
    QString error;
    QString code;
    int httpStatus = 0;

    [[nodiscard]] bool succeeded() const {
        return !latex.trimmed().isEmpty() && error.isEmpty();
    }
};

struct SnowShotChatModel {
    QString id;
    QString name;
    bool supportsReasoning = false;
    QString translationMode = QStringLiteral("default");
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

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_LATEX_RECOGNITION || SNOW_SHOT_ENABLE_IMAGE_CONVERSION ||                     \
    SNOW_SHOT_ENABLE_API_CONFIGURATION
class SnowShotApiClient final : public QObject {
    Q_OBJECT

  public:
    using RequestToken = quint64;
    using Completion = std::function<void(SnowShotTableResult)>;
    using LatexCompletion = std::function<void(SnowShotLatexResult)>;
    using ChatModelsCompletion = std::function<void(SnowShotChatModelsResult)>;
    using TranslationDelta = std::function<void(const QString&)>;
    using TranslationCompletion = std::function<void(SnowShotTranslationResult)>;

    explicit SnowShotApiClient(QString baseUrl, QObject* parent = nullptr);
    ~SnowShotApiClient() override;

    [[nodiscard]] static QString configuredBaseUrl(const QString& savedUrl = {});
    [[nodiscard]] QString baseUrl() const {
        return m_baseUrl;
    }
    [[nodiscard]] bool setBaseUrl(const QString& baseUrl);

    [[nodiscard]] bool usesSystemProxy() const;
    void setUseSystemProxy(bool enabled);
    [[nodiscard]] const QVector<SnowShotChatModel>& cachedChatModels() const;
    [[nodiscard]] QString cachedChatModelsLocale() const {
        return m_cachedChatModelsLocale;
    }
    [[nodiscard]] RequestToken extractTable(const QImage& image, QObject* receiver,
                                            Completion completion);
    [[nodiscard]] RequestToken extractLatex(const QImage& image, QObject* receiver,
                                            LatexCompletion completion);
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
    void setTextTranslationConfigurations(const snow_shot::TextTranslationConfigurations& values);
    [[nodiscard]] bool isTextTranslation(const QString& id) const;
    [[nodiscard]] int translationConcurrency(const QString& id) const;
    void setCustomModels(const snow_shot::CustomAiModels& models);
    [[nodiscard]] bool isCustomModel(const QString& id) const;
    [[nodiscard]] QString fallbackModel(bool vision) const;
    [[nodiscard]] bool hasBuiltInModels(const QString& locale) const;
    [[nodiscard]] QString modelFingerprint(const QString& id) const;

  signals:
    void baseUrlChanged();
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
    int m_latexTimeoutMs = 65000;
    int m_tableTimeoutMs = 35000;
    struct Request;
    void startLatexUpload(RequestToken token, const QByteArray& webp);
    void finishLatex(RequestToken token, SnowShotLatexResult result);
    void startTableUpload(RequestToken token, const QByteArray& webp);
    void cleanupRequest(Request* request);
    [[nodiscard]] QNetworkAccessManager* networkAccessManager();
    void finish(RequestToken token, SnowShotTableResult result);
    void finishChatModels(RequestToken token, SnowShotChatModelsResult result);
    void finishTranslation(RequestToken token, SnowShotTranslationResult result);
    void submitChatStream(RequestToken token, QByteArray body);
    void pumpCustomChatStreams();
    void startChatStream(RequestToken token, const QByteArray& body);

    const snow_shot::TextTranslationConfiguration* textTranslation(const QString& id) const;
    RequestToken enqueueTextTranslation(const SnowShotTranslationRequest& input, QObject* receiver,
                                        TranslationDelta delta, TranslationCompletion completion);
    void pumpTextTranslations();
    void startTextTranslation(RequestToken token);
    snow_shot::TextTranslationConfigurations m_textTranslations;
    QList<RequestToken> m_translationQueue;
    QList<RequestToken> m_customChatQueue;
    void rebuildAvailableModels();
    const snow_shot::CustomAiModelConfiguration* customModel(const QString& id) const;
    QString m_baseUrl;
    quint64 m_serverGeneration = 0;
    snow_shot::CustomAiModels m_customModels;
    QVector<SnowShotChatModel> m_availableModels;

    bool m_useSystemProxy = false;
    RequestToken m_nextToken = 0;
    QHash<RequestToken, Request*> m_requests;
    QVector<SnowShotChatModel> m_cachedChatModels;
    QString m_cachedChatModelsLocale;
};
#endif

#endif // SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H
