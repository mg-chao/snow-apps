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
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringView>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QJsonObject;
class QTimer;
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

[[nodiscard]] inline bool snowShotExpertModelId(QStringView id) {
    return id == QStringLiteral("expert-model");
}

struct SnowShotChatModel {
    QString id;
    QString name;
    bool supportsReasoning = false;
    QString translationMode = QStringLiteral("default");
    bool supportsVision = false;

    [[nodiscard]] bool operator==(const SnowShotChatModel&) const = default;
};

[[nodiscard]] inline bool snowShotLegacyAiModelId(const QString& id) {
    return id == QStringLiteral("qwen3.8-flash") || id == QStringLiteral("qwen-mt-flash");
}

// Historical selections and results can recognize an alias without a catalog. New
// requests must still resolve the selection against a successful server catalog.
[[nodiscard]] inline bool snowShotBuiltInModelAlias(const QString& legacyId,
                                                    const QString& canonicalId) {
    return snowShotLegacyAiModelId(legacyId) && canonicalId == QStringLiteral("ai-model");
}

// Resolve only against a successful server catalog. Custom IDs and historical
// result identities are never rewritten by this selection helper.
[[nodiscard]] inline QString
snowShotResolvedBuiltInModelId(const QVector<SnowShotChatModel>& models, const QString& id) {
    if (snowShotLegacyAiModelId(id)) {
        for (const auto& model : models)
            if (snowShotBuiltInModelAlias(id, model.id))
                return model.id;
    }
    return id;
}

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

// Immutable after preparation. Corrections append turns without re-encoding the image or
// changing the model's cached conversation prefix. Never persist this image-bearing context.
class SnowShotTableVisionConversation final {
    friend class SnowShotApiClient;
    QByteArray m_body;
    QString m_model;
    QString m_fingerprint;
};

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

    enum class ChatModelsCachePolicy { UseCached, Refresh };

    explicit SnowShotApiClient(QString baseUrl, QObject* parent = nullptr);
    ~SnowShotApiClient() override;

    [[nodiscard]] static QString configuredBaseUrl(const QString& savedUrl = {});
    [[nodiscard]] QString baseUrl() const {
        return m_baseUrl;
    }
    [[nodiscard]] bool setBaseUrl(const QString& baseUrl);

    [[nodiscard]] bool usesSystemProxy() const;
    void setUseSystemProxy(bool enabled);
    // Applies only to official requests; custom provider credentials remain independent.
    void setAccountAccessToken(const QByteArray& token, bool sameAccountRefresh = false);
    void setProxyConfiguration(const QString& mode, const QString& address);
    void configureNetworkProxy(QNetworkAccessManager& manager) const;
    [[nodiscard]] const QVector<SnowShotChatModel>& cachedChatModels() const;
    [[nodiscard]] QString cachedChatModelsLocale() const {
        return m_cachedChatModelsLocale;
    }
    [[nodiscard]] RequestToken extractTable(const QImage& image, QObject* receiver,
                                            Completion completion);
    [[nodiscard]] RequestToken extractLatex(const QImage& image, QObject* receiver,
                                            LatexCompletion completion);
    [[nodiscard]] RequestToken extractTableVision(const QImage& image, const QString& model,
                                                  QObject* receiver, Completion completion);
    [[nodiscard]] RequestToken
    streamTableVision(const QImage& image, const QString& model,
                      std::shared_ptr<SnowShotTableVisionConversation> conversation,
                      QObject* receiver, TranslationDelta delta, TranslationCompletion completion);
    [[nodiscard]] RequestToken
    correctTableVision(std::shared_ptr<SnowShotTableVisionConversation> conversation,
                       const QString& source, QObject* receiver, TranslationDelta delta,
                       TranslationCompletion completion);
    // Optional progress receives raw content chunks; only completion publishes validated LaTeX.
    [[nodiscard]] RequestToken extractLatexVision(const QImage& image, const QString& model,
                                                  QObject* receiver, LatexCompletion completion,
                                                  TranslationDelta delta = {});
    [[nodiscard]] RequestToken fetchChatModels(const QString& locale, QObject* receiver,
                                               ChatModelsCompletion completion);
    // Discovery reuses the cache unless refreshing. Concurrent subscribers, including normal
    // discovery during a refresh, share one request. Each token cancels only its subscriber.
    [[nodiscard]] RequestToken
    ensureChatModels(const QString& locale, QObject* receiver, ChatModelsCompletion completion,
                     ChatModelsCachePolicy cachePolicy = ChatModelsCachePolicy::UseCached);
    [[nodiscard]] RequestToken streamTranslation(const SnowShotTranslationRequest& request,
                                                 QObject* receiver, TranslationDelta delta,
                                                 TranslationCompletion completion);
    [[nodiscard]] RequestToken
    streamImageConversion(const SnowShotImageConversionRequest& request, QObject* receiver,
                          TranslationDelta delta,
                          std::function<void(SnowShotImageConversionResult)> completion);
    void cancel(RequestToken token);
    [[nodiscard]] bool isQueued(RequestToken token) const;
    void setTextTranslationConfigurations(const snow_shot::TextTranslationConfigurations& values);
    [[nodiscard]] bool isTextTranslation(const QString& id) const;
    [[nodiscard]] int translationConcurrency(const QString& id) const;
    void setCustomModels(const snow_shot::CustomAiModels& models);
    [[nodiscard]] bool isCustomModel(const QString& id) const;
    [[nodiscard]] QString fallbackModel(bool vision) const;
    [[nodiscard]] QString builtInVisionModel() const;
    [[nodiscard]] bool hasBuiltInModels(const QString& locale) const;
    [[nodiscard]] QString modelFingerprint(const QString& id) const;

  signals:
    void proxyConfigurationChanged();
    void baseUrlChanged();
    void chatModelsChanged();
    void officialCallSettled();
    void customModelInvalidated(const QString& id, bool translation, bool vision);

  public:
    [[nodiscard]] static QImage prepareImage(const QImage& image);
    [[nodiscard]] static QByteArray encodeWebp(const QImage& image);
    [[nodiscard]] static QString formatFailure(int httpStatus, const QString& failureCode,
                                               const QString& description, QStringView model = {});

  private:
    friend class SnowShotApiClientTestAccess;
    std::function<QByteArray(const QImage&)> m_tableImagePreparation;
    int m_latexTimeoutMs = 65000;
    int m_tableTimeoutMs = 35000;
    int m_visionTimeoutMs = 120000;
    struct Request;
    enum class ImageExtractionKind { Table, Latex };
    struct ImageExtractionResult;
    RequestToken extractImage(ImageExtractionKind kind, const QImage& image, QObject* receiver,
                              Completion completion, LatexCompletion latexCompletion);
    void startImageUpload(RequestToken token, const QByteArray& webp);
    void finishImageExtraction(RequestToken token, ImageExtractionResult result);
    void finishLatex(RequestToken token, SnowShotLatexResult result);
    void cleanupRequest(Request* request);
    [[nodiscard]] QNetworkAccessManager* networkAccessManager();
    void finish(RequestToken token, SnowShotTableResult result);
    void finishChatModels(RequestToken token, SnowShotChatModelsResult result);
    void finishTranslation(RequestToken token, SnowShotTranslationResult result);
    bool finishExpiredImageRequest(RequestToken token);
    void submitChatStream(RequestToken token, QByteArray body);
    void pumpCustomChatStreams();
    void pumpBuiltInChatStreams();
    bool retryBuiltInTranslation(RequestToken token, const QJsonObject& problem);
    void startChatStream(RequestToken token, const QByteArray& body);
    void drainChatStream(RequestToken token);
    void completeChatStream(RequestToken token);
    RequestToken
    streamImageRequest(const QString& model, const QImage& image, const QString& format,
                       const QString& prompt, QObject* receiver, TranslationDelta delta,
                       TranslationCompletion completion, bool absoluteDeadline,
                       std::shared_ptr<SnowShotTableVisionConversation> conversation = {},
                       std::optional<QString> correction = std::nullopt);
    void finishModelCatalogSubscribers(RequestToken owner, SnowShotChatModelsResult result);
    void noteChatModelsLocale(const QString& locale);

    const snow_shot::TextTranslationConfiguration* textTranslation(const QString& id) const;
    RequestToken enqueueTextTranslation(const SnowShotTranslationRequest& input, QObject* receiver,
                                        TranslationDelta delta, TranslationCompletion completion);
    void pumpTextTranslations();
    void startTextTranslation(RequestToken token);
    snow_shot::TextTranslationConfigurations m_textTranslations;
    QList<RequestToken> m_translationQueue;
    QList<RequestToken> m_customChatQueue;
    QList<RequestToken> m_builtinChatQueue;
    QElapsedTimer m_builtinChatClock;
    QTimer* m_builtinChatTimer = nullptr;
    qint64 m_builtinChatNextStartMs = 0;
    QHash<QString, qint64> m_builtinChatResumeTimes;
    void rebuildAvailableModels();
    void appendLocalModels(QVector<SnowShotChatModel>& models) const;
    const snow_shot::CustomAiModelConfiguration* customModel(const QString& id) const;
    QString m_baseUrl;
    QString m_freeTranslationBaseUrl;
    quint64 m_serverGeneration = 0;
    quint64 m_chatModelsLocaleGeneration = 0;
    QString m_requestedChatModelsLocale;
    snow_shot::CustomAiModels m_customModels;
    QVector<SnowShotChatModel> m_availableModels;

    QByteArray m_accountAccessToken;
    QString m_proxyMode = QStringLiteral("none");
    QString m_proxyAddress;
    RequestToken m_nextToken = 0;
    QHash<RequestToken, Request*> m_requests;
    QHash<QString, RequestToken> m_pendingModelCatalogs;
    QHash<RequestToken, QList<RequestToken>> m_modelCatalogSubscribers;
    QVector<SnowShotChatModel> m_cachedChatModels;
    QString m_cachedChatModelsLocale;
};
#endif

#endif // SNOW_SHOT_NETWORK_SNOWSHOTAPICLIENT_H
