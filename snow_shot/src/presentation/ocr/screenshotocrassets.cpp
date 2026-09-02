#include "snow_shot/presentation/screenshotocrassets.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <optional>
#include <mutex>

namespace {
constexpr auto kManifestName = "asset-manifest.json";
constexpr auto kRuntimeVersion = "1.0.0";
constexpr auto kPlatform = "windows-x64";
constexpr auto kModelId = "ppocrv6-small-463ea9f";
constexpr auto kDetectorName = "PP-OCRv6_det_small.onnx";
constexpr auto kRecognizerName = "PP-OCRv6_rec_small.onnx";
constexpr auto kDictionaryName = "ppocrv6_dict.txt";

struct FileDescriptor {
    QString name;
    QUrl url;
    qint64 size = -1;
    QByteArray sha256;
};

struct Descriptor {
    QString runtimeVersion;
    QString platform;
    FileDescriptor runtimeArchive;
    QList<FileDescriptor> runtimeFiles;
    QString modelId;
    QList<FileDescriptor> modelFiles;
};

QString formatError(const QString& context, const QString& detail = {}) {
    return detail.isEmpty() ? context : QStringLiteral("%1: %2").arg(context, detail);
}

bool safeRelativePath(const QString& path) {
    const QString normalized = QDir::fromNativeSeparators(path);
    if (normalized.isEmpty() || normalized.startsWith(u'/') ||
        normalized.contains(QStringLiteral("//")) || normalized.contains(u'\0') ||
        QDir::isAbsolutePath(normalized)) {
        return false;
    }
    const QStringList parts = normalized.split(u'/');
    return std::all_of(parts.cbegin(), parts.cend(), [](const QString& part) {
        return !part.isEmpty() && part != QStringLiteral(".") && part != QStringLiteral("..") &&
               !part.contains(u':');
    });
}

QByteArray sha256File(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (error != nullptr) *error = file.errorString();
        return {};
    }
    return hash.result();
}

bool verifyFile(const QString& path, const FileDescriptor& descriptor, QString* error) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink()) {
        if (error != nullptr) *error = QStringLiteral("missing file %1").arg(descriptor.name);
        return false;
    }
    if (descriptor.size < 0 || info.size() != descriptor.size) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid size for %1 (expected %2, got %3)")
                         .arg(descriptor.name)
                         .arg(descriptor.size)
                         .arg(info.size());
        }
        return false;
    }
    QString hashError;
    const QByteArray actual = sha256File(path, &hashError);
    if (actual.isEmpty() || actual != descriptor.sha256) {
        if (error != nullptr) {
            *error = hashError.isEmpty()
                         ? QStringLiteral("invalid SHA-256 for %1").arg(descriptor.name)
                         : formatError(QStringLiteral("could not hash %1").arg(descriptor.name),
                                       hashError);
        }
        return false;
    }
    return true;
}

std::optional<FileDescriptor> parseFile(const QJsonObject& object, bool requireUrl,
                                        QString* error) {
    FileDescriptor result;
    result.name = object.value(QStringLiteral("name")).toString();
    result.size = object.value(QStringLiteral("size")).toInteger(-1);
    result.sha256 = QByteArray::fromHex(
        object.value(QStringLiteral("sha256")).toString().toLatin1());
    result.url = QUrl(object.value(QStringLiteral("url")).toString());
    if (!safeRelativePath(result.name) || result.size < 0 || result.sha256.size() != 32 ||
        (requireUrl && (!result.url.isValid() || result.url.scheme() != QStringLiteral("https")))) {
        if (error != nullptr) *error = QStringLiteral("invalid OCR asset file descriptor");
        return std::nullopt;
    }
    return result;
}

std::optional<Descriptor> loadDescriptor(const QString& root, QString* error) {
    QFile file(QDir(root).filePath(QString::fromLatin1(kManifestName)));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = formatError(QStringLiteral("OCR asset manifest is missing"),
                                                   file.errorString());
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = formatError(QStringLiteral("OCR asset manifest is invalid"),
                                                   parseError.errorString());
        return std::nullopt;
    }
    const QJsonObject rootObject = document.object();
    const QJsonObject runtime = rootObject.value(QStringLiteral("runtime")).toObject();
    const QJsonObject model = rootObject.value(QStringLiteral("model")).toObject();
    Descriptor result;
    result.runtimeVersion = runtime.value(QStringLiteral("version")).toString();
    result.platform = runtime.value(QStringLiteral("platform")).toString();
    result.modelId = model.value(QStringLiteral("id")).toString();
    if (rootObject.value(QStringLiteral("schema")).toInt() != 1 ||
        result.runtimeVersion != QString::fromLatin1(kRuntimeVersion) ||
        result.platform != QString::fromLatin1(kPlatform) ||
        result.modelId != QString::fromLatin1(kModelId)) {
        if (error != nullptr) *error = QStringLiteral("unsupported OCR asset manifest version");
        return std::nullopt;
    }
    auto archive = parseFile(runtime.value(QStringLiteral("archive")).toObject(), true, error);
    if (!archive.has_value()) return std::nullopt;
    result.runtimeArchive = std::move(*archive);
    const auto parseFiles = [&](const QJsonArray& values, bool requireUrl,
                                QList<FileDescriptor>* destination) {
        for (const QJsonValue& value : values) {
            auto parsed = parseFile(value.toObject(), requireUrl, error);
            if (!parsed.has_value()) return false;
            if (std::any_of(destination->cbegin(), destination->cend(), [&](const auto& existing) {
                    return existing.name.compare(parsed->name, Qt::CaseInsensitive) == 0;
                })) {
                if (error != nullptr) *error = QStringLiteral("duplicate OCR asset descriptor");
                return false;
            }
            destination->push_back(std::move(*parsed));
        }
        return true;
    };
    if (!parseFiles(runtime.value(QStringLiteral("files")).toArray(), false,
                    &result.runtimeFiles) ||
        !parseFiles(model.value(QStringLiteral("files")).toArray(), true,
                    &result.modelFiles) ||
        result.runtimeFiles.size() != 3 || result.modelFiles.size() != 3) {
        if (error != nullptr && error->isEmpty()) *error = QStringLiteral("incomplete OCR asset manifest");
        return std::nullopt;
    }
    const QString expectedProcess = QStringLiteral("snow-ocr-process-%1-windows-x64.exe")
                                        .arg(result.runtimeVersion);
    const auto contains = [](const QList<FileDescriptor>& files, const QString& name) {
        return std::any_of(files.cbegin(), files.cend(), [&](const auto& item) {
            return item.name == name;
        });
    };
    if (!contains(result.runtimeFiles, expectedProcess) ||
        !contains(result.runtimeFiles, QStringLiteral("DirectML.dll")) ||
        !contains(result.runtimeFiles, QStringLiteral("runtime-manifest.json")) ||
        !contains(result.modelFiles, QString::fromLatin1(kDetectorName)) ||
        !contains(result.modelFiles, QString::fromLatin1(kRecognizerName)) ||
        !contains(result.modelFiles, QString::fromLatin1(kDictionaryName))) {
        if (error != nullptr) *error = QStringLiteral("OCR asset manifest has unexpected contents");
        return std::nullopt;
    }
    return result;
}

QString runtimeDirectory(const QString& root, const Descriptor& descriptor) {
    return QDir(root).filePath(QStringLiteral("runtimes/%1/%2")
                                   .arg(descriptor.runtimeVersion, descriptor.platform));
}

QString modelDirectory(const QString& root, const Descriptor& descriptor) {
    return QDir(root).filePath(QStringLiteral("models/%1").arg(descriptor.modelId));
}

bool validateCompletionMarker(const QString& directory, const QString& component,
                              QString* error) {
    QFile marker(QDir(directory).filePath(QStringLiteral(".complete.json")));
    if (!marker.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = QStringLiteral("missing completion marker for %1").arg(component);
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(marker.readAll());
    const QJsonObject object = document.object();
    if (!document.isObject() || object.value(QStringLiteral("schema")).toInt() != 1 ||
        object.value(QStringLiteral("component")).toString() != component) {
        if (error != nullptr) *error = QStringLiteral("invalid completion marker for %1").arg(component);
        return false;
    }
    return true;
}

bool validateComponent(const QString& directory, const QList<FileDescriptor>& files,
                       const QString& component, bool requireMarker, QString* error) {
    const bool filesValid = std::all_of(files.cbegin(), files.cend(), [&](const FileDescriptor& file) {
        return verifyFile(QDir(directory).filePath(file.name), file, error);
    });
    return filesValid && (!requireMarker || validateCompletionMarker(directory, component, error));
}

ScreenshotOcrResolvedAssets resolved(const QString& root, const Descriptor& descriptor,
                                     bool offline) {
    ScreenshotOcrResolvedAssets result;
    result.runtimeVersion = descriptor.runtimeVersion;
    result.runtimeDirectory = runtimeDirectory(root, descriptor);
    result.processPath = QDir(result.runtimeDirectory)
                             .filePath(QStringLiteral("snow-ocr-process-%1-windows-x64.exe")
                                           .arg(descriptor.runtimeVersion));
    const QString models = modelDirectory(root, descriptor);
    result.detectorModelPath = QDir(models).filePath(QString::fromLatin1(kDetectorName));
    result.recognizerModelPath = QDir(models).filePath(QString::fromLatin1(kRecognizerName));
    result.dictionaryPath = QDir(models).filePath(QString::fromLatin1(kDictionaryName));
    result.stateDirectory = QDir(root).filePath(
        QStringLiteral("state/%1").arg(descriptor.runtimeVersion));
    result.offline = offline;
    return result;
}

bool validateRoot(const QString& root, const Descriptor& descriptor, QString* error) {
    return validateComponent(runtimeDirectory(root, descriptor), descriptor.runtimeFiles,
                             descriptor.runtimeVersion, true, error) &&
           validateComponent(modelDirectory(root, descriptor), descriptor.modelFiles,
                             descriptor.modelId, true, error);
}

bool writeCompletionMarker(const QString& directory, const QString& component,
                           QString* error) {
    QJsonObject object{{QStringLiteral("schema"), 1},
                       {QStringLiteral("component"), component}};
    QSaveFile marker(QDir(directory).filePath(QStringLiteral(".complete.json")));
    if (!marker.open(QIODevice::WriteOnly) ||
        marker.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0 ||
        !marker.commit()) {
        if (error != nullptr) *error = marker.errorString();
        return false;
    }
    return true;
}

bool copyVerifiedFiles(const QString& source, const QString& staging,
                       const QList<FileDescriptor>& files, QString* error) {
    if (!QDir().mkpath(staging)) {
        if (error != nullptr) *error = QStringLiteral("could not create OCR staging directory");
        return false;
    }
    for (const FileDescriptor& descriptor : files) {
        const QString sourcePath = QDir(source).filePath(descriptor.name);
        if (!verifyFile(sourcePath, descriptor, error)) return false;
        const QString targetPath = QDir(staging).filePath(descriptor.name);
        if (!QFile::copy(sourcePath, targetPath) || !verifyFile(targetPath, descriptor, error)) {
            if (error != nullptr && error->isEmpty()) {
                *error = QStringLiteral("could not migrate %1").arg(descriptor.name);
            }
            return false;
        }
    }
    return true;
}

bool promoteDirectory(const QString& staging, const QString& destination, QString* error) {
    const QFileInfo destinationInfo(destination);
    QDir parent(destinationInfo.dir());
    if (!parent.mkpath(QStringLiteral("."))) {
        if (error != nullptr) *error = QStringLiteral("could not create OCR component directory");
        return false;
    }
    if (destinationInfo.exists() && !QDir(destination).removeRecursively()) {
        if (error != nullptr) *error = QStringLiteral("could not replace invalid OCR component");
        return false;
    }
    if (!parent.rename(staging, destination)) {
        if (error != nullptr) *error = QStringLiteral("could not activate OCR component");
        return false;
    }
    return true;
}

class CacheLock {
  public:
    explicit CacheLock(const QString& path) {
#ifdef Q_OS_WIN
        const QByteArray digest = QCryptographicHash::hash(
            QFileInfo(path).absoluteFilePath().toLower().toUtf8(), QCryptographicHash::Sha256)
                                      .toHex();
        const QString name = QStringLiteral("Local\\SnowShotOcrAssets-%1")
                                 .arg(QString::fromLatin1(digest.left(32)));
        m_handle = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(name.utf16()));
        if (m_handle != nullptr) {
            const DWORD result = WaitForSingleObject(m_handle, 120000);
            m_locked = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
#else
        Q_UNUSED(path);
        m_locked = true;
#endif
    }
    ~CacheLock() {
#ifdef Q_OS_WIN
        if (m_locked) ReleaseMutex(m_handle);
        if (m_handle != nullptr) CloseHandle(m_handle);
#endif
    }
    [[nodiscard]] bool locked() const { return m_locked; }

  private:
#ifdef Q_OS_WIN
    HANDLE m_handle = nullptr;
#endif
    bool m_locked = false;
};

bool configureProxy(QNetworkAccessManager* manager, const QString& proxyUrl, QString* error) {
    if (proxyUrl.trimmed().isEmpty()) return true;
    const QUrl url(proxyUrl);
    if (!url.isValid() || url.host().isEmpty() || url.port() <= 0) {
        if (error != nullptr) *error = QStringLiteral("invalid OCR download proxy");
        return false;
    }
    QNetworkProxy proxy(url.scheme().startsWith(QStringLiteral("socks"), Qt::CaseInsensitive)
                            ? QNetworkProxy::Socks5Proxy
                            : QNetworkProxy::HttpProxy,
                        url.host(), static_cast<quint16>(url.port()), url.userName(),
                        url.password());
    manager->setProxy(proxy);
    return true;
}

using Progress = std::function<void(qint64, qint64)>;

bool downloadOnce(QNetworkAccessManager* manager, const FileDescriptor& descriptor,
                  const QString& destination, const Progress& progress, QString* error,
                  bool* transient) {
    *transient = false;
    if (descriptor.url.scheme() != QStringLiteral("https")) {
        if (error != nullptr) *error = QStringLiteral("insecure OCR download URL rejected");
        return false;
    }
    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    QNetworkRequest request(descriptor.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("SnowShot/ocr-assets"));
    request.setRawHeader("Referer", "https://www.modelscope.cn/");
    QNetworkReply* reply = manager->get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(120000);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
        const QByteArray bytes = reply->readAll();
        if (file.write(bytes) != bytes.size()) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, progress);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start();
    loop.exec();
    const QByteArray remaining = reply->readAll();
    const bool wrote = remaining.isEmpty() || file.write(remaining) == remaining.size();
    file.flush();
    file.close();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl finalUrl = reply->url();
    const auto networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    reply->deleteLater();
    *transient = networkError == QNetworkReply::TimeoutError ||
                 networkError == QNetworkReply::TemporaryNetworkFailureError ||
                 networkError == QNetworkReply::RemoteHostClosedError || status == 408 ||
                 status == 429 || status >= 500;
    if (!wrote || networkError != QNetworkReply::NoError || status < 200 || status >= 300 ||
        finalUrl.scheme() != QStringLiteral("https")) {
        if (error != nullptr) {
            *error = !wrote ? QStringLiteral("could not write OCR download")
                            : formatError(QStringLiteral("OCR download failed"), networkErrorText);
        }
        QFile::remove(destination);
        return false;
    }
    if (!verifyFile(destination, descriptor, error)) {
        QFile::remove(destination);
        return false;
    }
    return true;
}

bool download(QNetworkAccessManager* manager, const FileDescriptor& descriptor,
              const QString& destination, const Progress& progress, QString* error) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool transient = false;
        if (downloadOnce(manager, descriptor, destination, progress, error, &transient)) return true;
        if (!transient) break;
    }
    return false;
}

bool extractRuntime(const QString& archive, const QString& staging,
                    const QList<FileDescriptor>& allowlist, QString* error) {
    void* reader = mz_zip_reader_create();
    const QByteArray archivePath = QFile::encodeName(archive);
    if (reader == nullptr || mz_zip_reader_open_file(reader, archivePath.constData()) != MZ_OK) {
        if (error != nullptr) *error = QStringLiteral("could not open OCR runtime archive");
        if (reader != nullptr) mz_zip_reader_delete(&reader);
        return false;
    }
    QHash<QString, FileDescriptor> expected;
    for (const auto& descriptor : allowlist) expected.insert(descriptor.name, descriptor);
    QSet<QString> seen;
    bool ok = mz_zip_reader_goto_first_entry(reader) == MZ_OK;
    while (ok) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || info == nullptr ||
            info->filename == nullptr) {
            if (error != nullptr) *error = QStringLiteral("invalid OCR runtime archive entry");
            ok = false;
            break;
        }
        const QString name = QDir::fromNativeSeparators(QString::fromUtf8(info->filename));
        const quint32 unixMode = static_cast<quint32>(info->external_fa >> 16);
        const bool specialUnixType = (unixMode & 0170000U) != 0U &&
                                     (unixMode & 0170000U) != 0100000U;
        const bool reparseLike = (info->external_fa & 0x0400U) != 0U;
        if (!safeRelativePath(name) || !expected.contains(name) || seen.contains(name) ||
            name.endsWith(u'/') || specialUnixType || reparseLike ||
            info->uncompressed_size != expected.value(name).size) {
            if (error != nullptr) {
                *error = QStringLiteral("unsafe or unexpected OCR runtime archive entry: %1")
                             .arg(name);
            }
            ok = false;
            break;
        }
        seen.insert(name);
        const QString outputPath = QDir(staging).filePath(name);
        if (!QDir().mkpath(QFileInfo(outputPath).dir().absolutePath()) ||
            mz_zip_reader_entry_open(reader) != MZ_OK) {
            if (error != nullptr) *error = QStringLiteral("could not extract OCR runtime");
            ok = false;
            break;
        }
        QFile output(outputPath);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) ok = false;
        char buffer[64 * 1024];
        while (ok) {
            const int32_t count = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer));
            if (count < 0) {
                ok = false;
                break;
            }
            if (count == 0) break;
            if (output.write(buffer, count) != count) {
                ok = false;
                break;
            }
        }
        output.close();
        if (mz_zip_reader_entry_close(reader) != MZ_OK) ok = false;
        if (!ok || !verifyFile(outputPath, expected.value(name), error)) {
            ok = false;
            break;
        }
        const int32_t next = mz_zip_reader_goto_next_entry(reader);
        if (next == MZ_END_OF_LIST) break;
        ok = next == MZ_OK;
    }
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    if (ok && seen.size() != expected.size()) {
        if (error != nullptr) *error = QStringLiteral("OCR runtime archive is incomplete");
        ok = false;
    }
    return ok;
}

void removeChildrenExcept(const QString& parentPath, const QSet<QString>& retained) {
    QDir parent(parentPath);
    for (const QFileInfo& child : parent.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!retained.contains(child.fileName())) QDir(child.absoluteFilePath()).removeRecursively();
    }
}
} // namespace

bool ScreenshotOcrResolvedAssets::valid() const {
    return !runtimeVersion.isEmpty() && QFileInfo(processPath).isFile() &&
           QFileInfo(detectorModelPath).isFile() && QFileInfo(recognizerModelPath).isFile() &&
           QFileInfo(dictionaryPath).isFile();
}

class ScreenshotOcrAssets::Impl final {
  public:
    Impl(ScreenshotOcrAssets* owner, Options options) : m_owner(owner), m_options(std::move(options)) {}
    ~Impl() {
        if (m_thread != nullptr) {
            m_thread->requestInterruption();
            m_thread->quit();
            m_thread->wait();
            delete m_thread;
        }
    }

    void prepare() {
        if (m_thread != nullptr && m_thread->isRunning()) return;
        if (m_thread != nullptr) {
            m_thread->wait();
            delete m_thread;
            m_thread = nullptr;
        }
        setStatus({ScreenshotOcrAssetPhase::Verifying, QStringLiteral("assets")});
        const Options options = m_options;
        QPointer<ScreenshotOcrAssets> owner(m_owner);
        m_thread = QThread::create([this, owner, options]() {
            QString error;
            ScreenshotOcrResolvedAssets assets;
            const bool success = acquire(options, &assets, &error);
            if (owner == nullptr) return;
            // Networking and ZIP work above owns thread-local Qt objects.
            // Destroy their deferred-delete queue before the worker exits.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QMetaObject::invokeMethod(owner, [this, owner, success, assets, error]() {
                if (owner == nullptr) return;
                if (success) {
                    m_assets = assets;
                    setStatus({assets.offline ? ScreenshotOcrAssetPhase::ReadyOffline
                                              : ScreenshotOcrAssetPhase::ReadyCached,
                               QStringLiteral("assets")});
                    emit owner->ready(assets);
                } else {
                    setStatus({ScreenshotOcrAssetPhase::Failed, QStringLiteral("assets"), 0, 0,
                               error});
                    emit owner->failed(error);
                }
            }, Qt::QueuedConnection);
        });
        QObject::connect(m_thread, &QThread::finished, m_owner, [this]() {
            if (m_thread == nullptr) return;
            m_thread->wait();
            delete m_thread;
            m_thread = nullptr;
        });
        m_thread->start();
    }

    void setProxyUrl(const QString& value) { m_options.proxyUrl = value; }
    ScreenshotOcrAssetStatus status() const {
        std::lock_guard lock(m_statusMutex);
        return m_status;
    }
    ScreenshotOcrResolvedAssets assets() const { return m_assets; }

  private:
    void setStatus(ScreenshotOcrAssetStatus status) {
        ScreenshotOcrAssetStatus current;
        {
            std::lock_guard lock(m_statusMutex);
            m_status = std::move(status);
            current = m_status;
        }
        emit m_owner->statusChanged(current);
    }

    void progress(const QString& component, qint64 received, qint64 total) {
        QPointer<ScreenshotOcrAssets> owner(m_owner);
        QMetaObject::invokeMethod(m_owner, [this, owner, component, received, total]() {
            if (owner != nullptr) {
                setStatus({ScreenshotOcrAssetPhase::Downloading, component, received, total});
            }
        }, Qt::QueuedConnection);
    }

    bool acquire(const Options& options, ScreenshotOcrResolvedAssets* assets, QString* error) {
        auto descriptor = loadDescriptor(options.offlineRoot, error);
        if (!descriptor.has_value()) return false;
        QString validationError;
        if (validateRoot(options.offlineRoot, *descriptor, &validationError)) {
            *assets = resolved(options.offlineRoot, *descriptor, true);
            const QString writableState = QDir(options.cacheRoot).filePath(
                QStringLiteral("state/%1").arg(descriptor->runtimeVersion));
            if (!options.cacheRoot.trimmed().isEmpty() && QDir().mkpath(writableState)) {
                assets->stateDirectory = writableState;
            } else {
                assets->stateDirectory.clear();
            }
            return true;
        }
        if (options.cacheRoot.trimmed().isEmpty()) {
            *error = QStringLiteral("OCR component storage is unavailable");
            return false;
        }
        CacheLock lock(options.cacheRoot);
        if (!lock.locked()) {
            *error = QStringLiteral("timed out waiting for OCR component storage");
            return false;
        }
        if (validateRoot(options.cacheRoot, *descriptor, &validationError)) {
            QDir().mkpath(resolved(options.cacheRoot, *descriptor, false).stateDirectory);
            *assets = resolved(options.cacheRoot, *descriptor, false);
            cleanup(options.cacheRoot, *descriptor);
            return true;
        }
        const QString stagingRoot = QDir(options.cacheRoot).filePath(QStringLiteral(".staging"));
        if (!QDir().mkpath(stagingRoot)) {
            *error = QStringLiteral("OCR component storage is read-only");
            return false;
        }
        // Remove abandoned/invalid component directories before staging the
        // replacement. The offline installation root is intentionally never
        // passed to this cleanup routine.
        cleanup(options.cacheRoot, *descriptor);
        if (!ensureModels(options, *descriptor, stagingRoot, error)) return false;
        if (!ensureRuntime(options, *descriptor, stagingRoot, error)) return false;
        if (!validateRoot(options.cacheRoot, *descriptor, error)) return false;
        const auto result = resolved(options.cacheRoot, *descriptor, false);
        if (!QDir().mkpath(result.stateDirectory)) {
            *error = QStringLiteral("could not create writable OCR state directory");
            return false;
        }
        cleanup(options.cacheRoot, *descriptor);
        *assets = result;
        return true;
    }

    bool ensureModels(const Options& options, const Descriptor& descriptor,
                      const QString& stagingRoot, QString* error) {
        const QString destination = modelDirectory(options.cacheRoot, descriptor);
        if (validateComponent(destination, descriptor.modelFiles, descriptor.modelId, true, error)) return true;
        const QString staging = QDir(stagingRoot).filePath(
            QStringLiteral("model-%1").arg(QUuid::createUuid().toString(QUuid::Id128)));
        QDir().mkpath(staging);
        // Migrate the pre-versioned model store only when all hashes match.
        if (validateComponent(options.cacheRoot, descriptor.modelFiles, descriptor.modelId, false,
                              nullptr)) {
            if (!copyVerifiedFiles(options.cacheRoot, staging, descriptor.modelFiles, error)) {
                QDir(staging).removeRecursively();
                return false;
            }
        } else {
            QNetworkAccessManager manager;
            if (!configureProxy(&manager, options.proxyUrl, error)) return false;
            for (const FileDescriptor& file : descriptor.modelFiles) {
                progress(QStringLiteral("models"), 0, file.size);
                const QString destinationPath = QDir(staging).filePath(file.name);
                const bool acquired = options.downloadOverride
                                          ? options.downloadOverride(file.url.toString(),
                                                                     destinationPath, error) &&
                                                verifyFile(destinationPath, file, error)
                                          : download(&manager, file, destinationPath,
                                                     [this](qint64 received, qint64 total) {
                                                         progress(QStringLiteral("models"), received,
                                                                  total);
                                                     },
                                                     error);
                if (!acquired) {
                    QDir(staging).removeRecursively();
                    return false;
                }
            }
        }
        if (!writeCompletionMarker(staging, descriptor.modelId, error) ||
            !promoteDirectory(staging, destination, error)) {
            QDir(staging).removeRecursively();
            return false;
        }
        return true;
    }

    bool ensureRuntime(const Options& options, const Descriptor& descriptor,
                       const QString& stagingRoot, QString* error) {
        const QString destination = runtimeDirectory(options.cacheRoot, descriptor);
        if (validateComponent(destination, descriptor.runtimeFiles, descriptor.runtimeVersion, true,
                              error)) return true;
        const QString token = QUuid::createUuid().toString(QUuid::Id128);
        const QString archive = QDir(stagingRoot).filePath(QStringLiteral("runtime-%1.zip").arg(token));
        const QString staging = QDir(stagingRoot).filePath(QStringLiteral("runtime-%1").arg(token));
        if (!QDir().mkpath(staging)) {
            *error = QStringLiteral("could not create OCR runtime staging directory");
            return false;
        }
        QNetworkAccessManager manager;
        const bool downloaded = options.downloadOverride
                                    ? options.downloadOverride(descriptor.runtimeArchive.url.toString(),
                                                               archive, error) &&
                                          verifyFile(archive, descriptor.runtimeArchive, error)
                                    : configureProxy(&manager, options.proxyUrl, error) &&
                                          download(&manager, descriptor.runtimeArchive, archive,
                                                   [this](qint64 received, qint64 total) {
                                                       progress(QStringLiteral("runtime"), received,
                                                                total);
                                                   },
                                                   error);
        const bool extracted = downloaded &&
                               (options.extractOverride
                                    ? options.extractOverride(archive, staging, error)
                                    : extractRuntime(archive, staging, descriptor.runtimeFiles,
                                                     error));
        if (!downloaded || !extracted ||
            !validateComponent(staging, descriptor.runtimeFiles, descriptor.runtimeVersion, false,
                               error) ||
            !writeCompletionMarker(staging, descriptor.runtimeVersion, error) ||
            !promoteDirectory(staging, destination, error)) {
            QFile::remove(archive);
            QDir(staging).removeRecursively();
            return false;
        }
        QFile::remove(archive);
        return true;
    }

    static void cleanup(const QString& root, const Descriptor& descriptor) {
        removeChildrenExcept(QDir(root).filePath(QStringLiteral("models")),
                             {descriptor.modelId});
        const QString runtimes = QDir(root).filePath(QStringLiteral("runtimes"));
        removeChildrenExcept(runtimes, {descriptor.runtimeVersion});
        removeChildrenExcept(QDir(runtimes).filePath(descriptor.runtimeVersion),
                             {descriptor.platform});
        QDir(QDir(root).filePath(QStringLiteral(".staging"))).removeRecursively();
        QDir().mkpath(QDir(root).filePath(QStringLiteral(".staging")));
    }

    ScreenshotOcrAssets* m_owner = nullptr;
    Options m_options;
    mutable std::mutex m_statusMutex;
    ScreenshotOcrAssetStatus m_status;
    ScreenshotOcrResolvedAssets m_assets;
    QThread* m_thread = nullptr;
};

ScreenshotOcrAssets::ScreenshotOcrAssets(Options options, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(this, std::move(options))) {}

ScreenshotOcrAssets::~ScreenshotOcrAssets() = default;

void ScreenshotOcrAssets::prepare() { m_impl->prepare(); }
void ScreenshotOcrAssets::setProxyUrl(const QString& proxyUrl) { m_impl->setProxyUrl(proxyUrl); }
ScreenshotOcrAssetStatus ScreenshotOcrAssets::status() const { return m_impl->status(); }
ScreenshotOcrResolvedAssets ScreenshotOcrAssets::resolvedAssets() const { return m_impl->assets(); }
