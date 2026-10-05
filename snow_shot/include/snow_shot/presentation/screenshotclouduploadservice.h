#ifndef SNOW_SHOT_SCREENSHOTCLOUDUPLOADSERVICE_H
#define SNOW_SHOT_SCREENSHOTCLOUDUPLOADSERVICE_H

#include "snow_shot/clouduploadconfiguration.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include <QPointer>

struct ScreenshotCloudUploadOptions {
    ScreenshotImageFileFormat format = ScreenshotImageFileFormat::Png;
    ScreenshotImageEncodingOptions encoding;
    ScreenshotPdfOptions pdf;
    QString filenameFormat;
    int transferTimeoutMs = 60000;
};
struct ScreenshotCloudUploadResult {
    enum class Status { Succeeded, Failed, Cancelled };
    Status status = Status::Failed;
    QUrl url;
    QString error;
};
class ScreenshotCloudUploadJob final : public QObject {
    Q_OBJECT
  public:
    ~ScreenshotCloudUploadJob() override;
    void cancel();
    [[nodiscard]] QString preparedPath() const;
  signals:
    void progress(int percent);
    void finished(const ScreenshotCloudUploadResult& result);

  private:
    friend class ScreenshotCloudUploadService;
    ScreenshotCloudUploadJob(std::shared_ptr<ScreenshotExportArtifact> artifact,
                             snow_shot::CloudUploadConfiguration config,
                             ScreenshotCloudUploadOptions options, QObject* parent);
    void prepare();
    void upload(const QByteArray& hash);
    void complete(ScreenshotCloudUploadResult result);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
class ScreenshotCloudUploadService final {
  public:
    using Completion = std::function<void(const ScreenshotCloudUploadResult&)>;
    // The receiver owns the job. It cancels on destruction and deletes itself after completion.
    [[nodiscard]] static ScreenshotCloudUploadJob*
    upload(std::shared_ptr<ScreenshotExportArtifact> artifact,
           snow_shot::CloudUploadConfiguration config, ScreenshotCloudUploadOptions options,
           QObject* receiver, Completion completion = {});
    [[nodiscard]] static ScreenshotCloudUploadOptions currentOptions();
    [[nodiscard]] static std::optional<snow_shot::CloudUploadConfiguration> configuration();
};
#endif
