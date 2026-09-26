#pragma once

#include "snow_shot/app/mcp/screenshotmcpserver.h"
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotrecognitionresults.h"
#include "snow_draw_engine_qt/snow_canvas_export_types.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"
#include <QImage>
#include <memory>

class ScreenshotOcrRecognitionPort;
class ScreenshotQrRecognitionPort;
class SnowShotApiClient;

namespace snow_shot::app::mcp {
class McpJobRegistry;
class McpDocumentService final : public QObject {
    Q_OBJECT
  public:
    struct Source {
        QImage image;
        QList<CanvasExportSource> images;
        QRectF canvasBounds;
        QByteArray documentSession;
        QByteArray documentHistory;
        std::optional<ScreenshotSelectionParams> selection;
        QJsonObject metadata;
        ScreenshotClipboardOriginalContent originalContent;
        ScreenshotRecognitionResults recognitionResults;
        QString tool = QStringLiteral("select");
    };
    struct Ports {
        McpJobRegistry* jobs = nullptr;
        ScreenshotOcrRecognitionPort* recognition = nullptr;
        ScreenshotQrRecognitionPort* qrRecognition = nullptr;
        SnowShotApiClient* api = nullptr;
        using SourceCompletion = std::function<void(Source, QString)>;
        using SourceBudget = std::function<bool(qint64)>;
        // Non-file sources resolve through existing application repositories/services.
        std::function<void(const ScreenshotMcpRequest&, SourceCompletion, SourceBudget)>
            resolveSource;
        std::function<void(quint64, const QString&)> cancelSource;
        std::function<bool(QImage, std::function<void(bool)>)> pin;
        std::function<bool(Source, std::function<void(bool)>)> present;
        std::function<bool(Source, QImage, std::function<void(bool)>)> pinDocument;
        std::function<void(QImage, std::function<void(QList<SnowCanvasAutoFilterRegion>, QString)>)>
            autoFilter;
        std::function<qint64()> clock;
        // Optional deterministic test instrumentation; called on the owning worker lane.
        std::function<void(const ScreenshotMcpRequest&)> beforeWorkerRequest;
        std::function<void(const ScreenshotMcpRequest&)> beforeWorkerDecode;
        std::function<void(const QString& documentId, const QString& operation)> workObserved;
        qsizetype artifactCacheBytes = 64 * 1024 * 1024;
    };
    explicit McpDocumentService(Ports ports, QObject* parent = nullptr);
    ~McpDocumentService() override;
    [[nodiscard]] bool handles(const QString& method) const;
    [[nodiscard]] QStringList capabilities() const;
    QJsonObject storeArtifact(quint64 owner, QByteArray bytes, const QString& mimeType);
    void storeFileArtifact(quint64 owner, const QString& sourcePath, const QString& mimeType,
                           std::function<void(QJsonObject, QString)> completion);
    void request(const ScreenshotMcpRequest&, ScreenshotMcpServer::Completion);
    void disconnected(quint64 owner);
    bool cancelRequest(quint64 owner, const QString& requestId);
    void shutdown();
  signals:
    void changed(quint64 owner, const QString& documentId);
    void artifactChanged(quint64 owner, const QString& artifactId);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::app::mcp
