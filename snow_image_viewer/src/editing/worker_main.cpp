#include "editing/worker_core.h"
#include "editing/worker_protocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

using snow::image_viewer::EditExportSettings;
namespace protocol = snow::image_viewer::worker_protocol;
namespace worker_core = snow::image_viewer::worker_core;

std::mutex outputMutex;

void writeFrame(protocol::MessageType type, const QJsonObject& object) {
    const std::lock_guard lock(outputMutex);
    const QByteArray bytes = protocol::encodeFrame(type, object);
    std::cout.write(bytes.constData(), bytes.size());
    std::cout.flush();
}

std::ptrdiff_t readInput(char* data, std::size_t size) {
#if defined(Q_OS_WIN)
    return static_cast<std::ptrdiff_t>(
        _read(_fileno(stdin), data, static_cast<unsigned int>(size)));
#else
    return static_cast<std::ptrdiff_t>(::read(STDIN_FILENO, data, size));
#endif
}

bool applyMemoryLimit(qulonglong bytes, bool initialize = false) {
    if (bytes == 0)
        return false;
#if defined(Q_OS_WIN)
    Q_UNUSED(initialize);
    static HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit =
        static_cast<SIZE_T>(std::min<qulonglong>(bytes, std::numeric_limits<SIZE_T>::max()));
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return false;
    static bool assigned = false;
    if (!assigned) {
        if (!AssignProcessToJobObject(job, GetCurrentProcess()))
            return false;
        assigned = true;
    }
#else
    rlimit limit{};
    if (getrlimit(RLIMIT_AS, &limit) != 0)
        return false;
    const rlim_t requested =
        static_cast<rlim_t>(std::min<qulonglong>(bytes, std::numeric_limits<rlim_t>::max()));
    if (initialize)
        limit.rlim_max = requested;
    limit.rlim_cur = std::min(requested, limit.rlim_max);
    if (setrlimit(RLIMIT_AS, &limit) != 0)
        return false;
#endif
    return true;
}

bool isJobPath(const QString& sessionDirectory, const QString& path, const QString& nonce,
               bool mustExist, bool requireNonce = true) {
    const QFileInfo sessionInfo(sessionDirectory);
    const QFileInfo pathInfo(path);
    const QString session = QDir::cleanPath(sessionInfo.absoluteFilePath());
    const QString parent = QDir::cleanPath(pathInfo.absoluteDir().absolutePath());
    return sessionInfo.isDir() && session == parent &&
           (!requireNonce || pathInfo.fileName().startsWith(nonce)) &&
           (!mustExist || pathInfo.isFile());
}

QJsonObject runJob(const QJsonObject& job, std::stop_token stop, bool* artifactPublished) {
    const QString requestId = job.value(QStringLiteral("requestId")).toString();
    const QString nonce = job.value(QStringLiteral("nonce")).toString();
    const QString sessionDirectory = job.value(QStringLiteral("sessionDirectory")).toString();
    const QJsonObject baseRaster = job.value(QStringLiteral("baseRaster")).toObject();
    const QString baseKind = baseRaster.value(QStringLiteral("kind")).toString();
    const QString basePath = baseRaster.value(QStringLiteral("path")).toString();
    const QString artifactPath = job.value(QStringLiteral("artifactPath")).toString();
    const QString previewPath = job.value(QStringLiteral("previewPath")).toString();
    const QString preparedPath = job.value(QStringLiteral("preparedPath")).toString();
    const QJsonObject preparedRaster = job.value(QStringLiteral("preparedRaster")).toObject();
    QJsonObject result{{QStringLiteral("protocolVersion"), static_cast<int>(protocol::kVersion)},
                       {QStringLiteral("success"), false}};
    QString error;
    EditExportSettings settings;
    const bool fileTransport = baseKind == QStringLiteral("verified_file");
    const bool sharedTransport = baseKind == QStringLiteral("shared_memory");
    if (nonce.size() < 32 || (!fileTransport && !sharedTransport) ||
        (fileTransport && !isJobPath(sessionDirectory, basePath, nonce, true, false)) ||
        !isJobPath(sessionDirectory, artifactPath, nonce, false) ||
        !isJobPath(sessionDirectory, previewPath, nonce, false) ||
        (!preparedPath.isEmpty() && !isJobPath(sessionDirectory, preparedPath, nonce, false)) ||
        (!preparedRaster.isEmpty() &&
         (preparedRaster.value(QStringLiteral("kind")).toString() !=
              QStringLiteral("verified_file") ||
          !isJobPath(sessionDirectory, preparedRaster.value(QStringLiteral("path")).toString(),
                     nonce, true, false))) ||
        !protocol::settingsFromJson(job.value(QStringLiteral("settings")), &settings, &error)) {
        if (error.isEmpty())
            error = QStringLiteral("The worker job paths are invalid.");
        result.insert(QStringLiteral("error"), error);
        return result;
    }

    bool memoryLimitValid = false;
    const qulonglong memoryLimit =
        job.value(QStringLiteral("memoryLimit")).toString().toULongLong(&memoryLimitValid);
    if (!memoryLimitValid || !applyMemoryLimit(memoryLimit)) {
        result.insert(QStringLiteral("error"),
                      QStringLiteral("The worker memory ceiling could not be applied."));
        return result;
    }

    const QString testMode = job.value(QStringLiteral("testMode")).toString();
    if (testMode == QStringLiteral("block-noncooperative")) {
        for (;;)
            QThread::msleep(1000);
    }
    if (testMode == QStringLiteral("block-cooperative")) {
        while (!stop.stop_requested())
            QThread::msleep(1);
        result.insert(QStringLiteral("error"), QStringLiteral("The export was cancelled."));
        return result;
    }
    if (testMode == QStringLiteral("crash"))
        std::_Exit(86);
    if (testMode == QStringLiteral("partial-artifact")) {
        QFile partial(artifactPath + QStringLiteral(".partial"));
        if (partial.open(QIODevice::WriteOnly))
            partial.write("partial", 7);
        result.insert(QStringLiteral("error"),
                      QStringLiteral("Simulated partial artifact failure."));
        return result;
    }

    if (testMode == QStringLiteral("shared-attach-failure") && sharedTransport) {
        // Keep the transport syntactically valid while naming a segment that does
        // not exist. The controller should classify this as a one-time retriable
        // attach failure and resend the same job with a verified file transport.
        QJsonObject retryJob = job;
        QJsonObject invalidTransport = baseRaster;
        invalidTransport.insert(QStringLiteral("key"),
                                QStringLiteral("snow-edit-v1-") + QString(32, QLatin1Char('0')) +
                                    QLatin1Char('-') + QString(32, QLatin1Char('1')));
        retryJob.insert(QStringLiteral("baseRaster"), invalidTransport);
        return worker_core::executeEncodeJob(
            retryJob, settings,
            [requestId, nonce](QJsonObject publication) {
                publication.insert(QStringLiteral("protocolVersion"),
                                   static_cast<int>(protocol::kVersion));
                publication.insert(QStringLiteral("requestId"), requestId);
                publication.insert(QStringLiteral("nonce"), nonce);
                writeFrame(protocol::MessageType::artifact_ready, publication);
            },
            stop, artifactPublished);
    }

    return worker_core::executeEncodeJob(
        job, settings,
        [requestId, nonce](QJsonObject publication) {
            publication.insert(QStringLiteral("protocolVersion"),
                               static_cast<int>(protocol::kVersion));
            publication.insert(QStringLiteral("requestId"), requestId);
            publication.insert(QStringLiteral("nonce"), nonce);
            writeFrame(protocol::MessageType::artifact_ready, publication);
        },
        stop, artifactPublished);
}

void handleJob(const QJsonObject& job, std::stop_token stop) {
    if (job.value(QStringLiteral("testMode")).toString() == QStringLiteral("stale-result")) {
        writeFrame(protocol::MessageType::artifact_ready,
                   {{QStringLiteral("requestId"), QStringLiteral("0")},
                    {QStringLiteral("nonce"), QStringLiteral("stale")},
                    {QStringLiteral("artifactPath"), QStringLiteral("stale.invalid")}});
    }

    bool artifactPublished = false;
    QJsonObject result = runJob(job, stop, &artifactPublished);
    const QString artifactPath = job.value(QStringLiteral("artifactPath")).toString();
    const QString previewPath = job.value(QStringLiteral("previewPath")).toString();
    QFile::remove(artifactPath + QStringLiteral(".partial"));
    QFile::remove(previewPath + QStringLiteral(".partial"));
    const QString preparedPath = job.value(QStringLiteral("preparedPath")).toString();
    if (!preparedPath.isEmpty())
        QFile::remove(preparedPath + QStringLiteral(".partial"));
    if (stop.stop_requested()) {
        if (!artifactPublished)
            QFile::remove(artifactPath);
        if (!artifactPublished && !preparedPath.isEmpty())
            QFile::remove(preparedPath);
        QFile::remove(previewPath);
        return;
    }
    if (!result.value(QStringLiteral("success")).toBool()) {
        if (!artifactPublished)
            QFile::remove(artifactPath);
        if (!artifactPublished && !preparedPath.isEmpty())
            QFile::remove(preparedPath);
        QFile::remove(previewPath);
    }

    if (job.value(QStringLiteral("testMode")).toString() == QStringLiteral("stale-result")) {
        writeFrame(protocol::MessageType::preview_ready,
                   {{QStringLiteral("requestId"), QStringLiteral("0")},
                    {QStringLiteral("nonce"), QStringLiteral("stale")}});
    }
    result.insert(QStringLiteral("requestId"), job.value(QStringLiteral("requestId")).toString());
    result.insert(QStringLiteral("nonce"), job.value(QStringLiteral("nonce")).toString());
    writeFrame(result.value(QStringLiteral("success")).toBool()
                   ? protocol::MessageType::preview_ready
                   : protocol::MessageType::job_failed,
               result);
}

void handlePreviewJob(const QJsonObject& job, std::stop_token stop) {
    const QString requestId = job.value(QStringLiteral("requestId")).toString();
    const QString nonce = job.value(QStringLiteral("nonce")).toString();
    const QString sessionDirectory = job.value(QStringLiteral("sessionDirectory")).toString();
    const QString artifactPath = job.value(QStringLiteral("artifactPath")).toString();
    const QString previewPath = job.value(QStringLiteral("previewPath")).toString();
    QString error;
    EditExportSettings settings;
    const auto fail = [&](const QString& message) {
        writeFrame(protocol::MessageType::job_failed, {{QStringLiteral("requestId"), requestId},
                                                       {QStringLiteral("nonce"), nonce},
                                                       {QStringLiteral("artifactPublished"), true},
                                                       {QStringLiteral("error"), message}});
    };
    if (nonce.size() < 32 || !isJobPath(sessionDirectory, artifactPath, nonce, true) ||
        !isJobPath(sessionDirectory, previewPath, nonce, false) ||
        !protocol::settingsFromJson(job.value(QStringLiteral("settings")), &settings, &error)) {
        fail(error.isEmpty() ? QStringLiteral("The preview job paths are invalid.") : error);
        return;
    }
    bool memoryLimitValid = false;
    const qulonglong memoryLimit =
        job.value(QStringLiteral("memoryLimit")).toString().toULongLong(&memoryLimitValid);
    if (!memoryLimitValid || !applyMemoryLimit(memoryLimit)) {
        fail(QStringLiteral("The worker memory ceiling could not be applied."));
        return;
    }

    QJsonObject result =
        worker_core::executePreviewJob(artifactPath, previewPath, settings.format, stop);
    QFile::remove(previewPath + QStringLiteral(".partial"));
    if (stop.stop_requested()) {
        QFile::remove(previewPath);
        return;
    }
    result.insert(QStringLiteral("requestId"), requestId);
    result.insert(QStringLiteral("nonce"), nonce);
    writeFrame(result.value(QStringLiteral("success")).toBool()
                   ? protocol::MessageType::preview_ready
                   : protocol::MessageType::job_failed,
               result);
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    Q_UNUSED(application);
    bool memoryLimitValid = false;
    const qulonglong memoryLimit =
        qEnvironmentVariable("SNOW_IMAGE_WORKER_MEMORY_LIMIT").toULongLong(&memoryLimitValid);
    if (!memoryLimitValid || !applyMemoryLimit(memoryLimit, true))
        return 4;
#if defined(Q_OS_WIN)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    writeFrame(protocol::MessageType::ready,
               {{QStringLiteral("protocolVersion"), static_cast<int>(protocol::kVersion)}});
    QByteArray input;
    std::array<char, 64 * 1024> buffer{};
    std::jthread jobThread;
    QString jobNonce;
    std::atomic_bool jobDone = true;
    for (;;) {
        const std::ptrdiff_t count = readInput(buffer.data(), buffer.size());
        if (count <= 0)
            break;
        input.append(buffer.data(), static_cast<qsizetype>(count));
        for (;;) {
            protocol::Frame frame;
            QString frameError;
            if (!protocol::takeFrame(&input, &frame, &frameError)) {
                if (!frameError.isEmpty())
                    return 3;
                break;
            }
            const QJsonObject object = frame.payload;
            if (frame.type == protocol::MessageType::shutdown) {
                if (jobThread.joinable())
                    jobThread.request_stop();
                return 0;
            }
            if (frame.type == protocol::MessageType::cancel) {
                if (jobThread.joinable() && !jobDone.load() &&
                    object.value(QStringLiteral("nonce")).toString() == jobNonce) {
                    jobThread.request_stop();
                    // An acknowledgement means the worker is quiescent and can accept the
                    // replacement job immediately. The controller enforces the bounded
                    // cancellation deadline and terminates us if a codec does not stop.
                    jobThread.join();
                    writeFrame(protocol::MessageType::cancelled,
                               {{QStringLiteral("requestId"),
                                 object.value(QStringLiteral("requestId")).toString()},
                                {QStringLiteral("nonce"), jobNonce}});
                }
                continue;
            }
            if (frame.type != protocol::MessageType::encode_job &&
                frame.type != protocol::MessageType::preview_job)
                return 3;
            if (jobThread.joinable()) {
                QElapsedTimer completion;
                completion.start();
                while (!jobDone.load() && completion.elapsed() < 100)
                    QThread::yieldCurrentThread();
                if (!jobDone.load())
                    return 3;
                jobThread.join();
            }
            jobNonce = object.value(QStringLiteral("nonce")).toString();
            jobDone.store(false);
            const protocol::MessageType jobType = frame.type;
            jobThread = std::jthread([object, jobType, &jobDone](std::stop_token stop) {
                if (jobType == protocol::MessageType::preview_job)
                    handlePreviewJob(object, stop);
                else
                    handleJob(object, stop);
                jobDone.store(true);
            });
        }
    }
    return 0;
}
