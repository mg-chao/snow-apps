#include "snow_shot/storage/capturehistoryrepository.h"

#include "snow_shot/storage/persistedselectioncodec.h"
#include "snow_shot/storage/storagelogging.h"
#include "snowimageqtcodec.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace snow_shot::storage {
namespace {
constexpr qint64 kMiB = 1024 * 1024;
constexpr qint64 kMaximumCanvasBytes = 16 * kMiB;
constexpr qint64 kMaximumPixelsPerImage = 64'000'000;
constexpr qint64 kMaximumPixelsPerRecord = 128'000'000;
constexpr int kMaximumDisplays = 32;
constexpr int kIndexVersion = 1;
// Bounds arithmetic on persisted sizes without consulting payload files.
constexpr qint64 kMaximumStoredBytes = 1LL << 40;

struct StoredRecord {
    CaptureHistoryRecord record;
    QString canvasFileName = QStringLiteral("canvas_history.json");
    QVector<QString> displayFileNames;
    std::optional<QString> resultFileName;
};

struct Snapshot {
    QVector<StoredRecord> records;
    QMap<QString, qint64> pendingDeletions;
};

struct EncodedDraft {
    StoredRecord stored;
    QMap<QString, QByteArray> files;
};

template <typename Result> std::shared_future<Result> readyFuture(Result result) {
    std::promise<Result> promise;
    promise.set_value(std::move(result));
    return promise.get_future().share();
}

bool validUuid(const QString& id) {
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
}

bool integer(const QJsonValue& value, qint64 minimum, qint64 maximum, qint64* result) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble() ||
        value.toDouble() < static_cast<double>(minimum) ||
        value.toDouble() > static_cast<double>(maximum)) {
        return false;
    }
    *result = value.toInteger();
    return true;
}

bool validCanvas(const QByteArray& bytes) {
    if (bytes.isEmpty() || bytes.size() > kMaximumCanvasBytes) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    return document.isObject() || document.isArray();
}

QString sourceText(CaptureHistorySource source) {
    switch (source) {
    case CaptureHistorySource::CopiedToClipboard:
        return QStringLiteral("copied_to_clipboard");
    case CaptureHistorySource::SavedToFile:
        return QStringLiteral("saved_to_file");
    case CaptureHistorySource::PinnedToScreen:
        return QStringLiteral("pinned_to_screen");
    case CaptureHistorySource::CurrentMonitor:
        return QStringLiteral("current_monitor");
    case CaptureHistorySource::FocusedWindow:
        return QStringLiteral("focused_window");
    }
    return {};
}

QJsonObject imageJson(const QSize& size, qint64 bytes, const QString& file) {
    return {{QStringLiteral("width"), size.width()},
            {QStringLiteral("height"), size.height()},
            {QStringLiteral("encoded_bytes"), bytes},
            {QStringLiteral("image_file"), file}};
}

QJsonObject recordJson(const StoredRecord& stored) {
    const auto& record = stored.record;
    QJsonArray displays;
    for (qsizetype i = 0; i < record.displays.size(); ++i) {
        const auto& display = record.displays[i];
        QJsonObject object =
            imageJson(display.imageSize, display.encodedBytes, stored.displayFileNames[i]);
        object.insert(QStringLiteral("stable_id"), display.stableId);
        object.insert(QStringLiteral("display_name"), display.name);
        displays.append(object);
    }
    QJsonObject object{
        {QStringLiteral("id"), record.id},
        {QStringLiteral("created_utc"), record.createdUtc.toString(Qt::ISODateWithMs)},
        {QStringLiteral("source"), sourceText(record.source)},
        {QStringLiteral("canvas_bounds"),
         QJsonObject{{QStringLiteral("x"), record.canvasBounds.x()},
                     {QStringLiteral("y"), record.canvasBounds.y()},
                     {QStringLiteral("width"), record.canvasBounds.width()},
                     {QStringLiteral("height"), record.canvasBounds.height()}}},
        {QStringLiteral("selection"), persistedSelectionToJson(record.selection)},
        {QStringLiteral("canvas_history_file"), stored.canvasFileName},
        {QStringLiteral("canvas_byte_size"), record.canvasBytes},
        {QStringLiteral("total_record_size"), record.totalBytes},
        {QStringLiteral("displays"), displays}};
    if (record.result.has_value()) {
        object.insert(QStringLiteral("result"),
                      imageJson(record.result->imageSize, record.result->encodedBytes,
                                *stored.resultFileName));
    }
    return object;
}

bool parseImage(const QJsonObject& object, const QString& file, QSize* size, qint64* bytes,
                qint64* pixels) {
    qint64 width = 0;
    qint64 height = 0;
    if (object.value(QStringLiteral("image_file")).toString() != file ||
        !integer(object.value(QStringLiteral("width")), 1, kMaximumPixelsPerImage, &width) ||
        !integer(object.value(QStringLiteral("height")), 1, kMaximumPixelsPerImage, &height) ||
        !integer(object.value(QStringLiteral("encoded_bytes")), 1, kMaximumStoredBytes, bytes) ||
        width * height > kMaximumPixelsPerImage ||
        *pixels > kMaximumPixelsPerRecord - width * height) {
        return false;
    }
    *pixels += width * height;
    *size = QSize(static_cast<int>(width), static_cast<int>(height));
    return true;
}

bool parseRecord(const QJsonObject& object, StoredRecord* stored) {
    auto& record = stored->record;
    record.id = object.value(QStringLiteral("id")).toString();
    const QString date = object.value(QStringLiteral("created_utc")).toString();
    record.createdUtc = QDateTime::fromString(date, Qt::ISODateWithMs);
    if (!validUuid(record.id) || !date.endsWith(u'Z') || !record.createdUtc.isValid() ||
        object.value(QStringLiteral("canvas_history_file")).toString() != stored->canvasFileName ||
        !integer(object.value(QStringLiteral("canvas_byte_size")), 1, kMaximumCanvasBytes,
                 &record.canvasBytes) ||
        !integer(object.value(QStringLiteral("total_record_size")), 1, kMaximumStoredBytes,
                 &record.totalBytes)) {
        return false;
    }
    bool sourceFound = false;
    for (const auto source :
         {CaptureHistorySource::CopiedToClipboard, CaptureHistorySource::SavedToFile,
          CaptureHistorySource::PinnedToScreen, CaptureHistorySource::CurrentMonitor,
          CaptureHistorySource::FocusedWindow}) {
        if (object.value(QStringLiteral("source")).toString() == sourceText(source)) {
            record.source = source;
            sourceFound = true;
        }
    }
    const QJsonObject bounds = object.value(QStringLiteral("canvas_bounds")).toObject();
    qint64 x = 0, y = 0, width = 0, height = 0;
    const auto selection = normalizePersistedSelection(object.value(QStringLiteral("selection")));
    if (!sourceFound || !selection.valid ||
        !integer(bounds.value(QStringLiteral("x")), std::numeric_limits<int>::min(),
                 std::numeric_limits<int>::max(), &x) ||
        !integer(bounds.value(QStringLiteral("y")), std::numeric_limits<int>::min(),
                 std::numeric_limits<int>::max(), &y) ||
        !integer(bounds.value(QStringLiteral("width")), 1, std::numeric_limits<int>::max(),
                 &width) ||
        !integer(bounds.value(QStringLiteral("height")), 1, std::numeric_limits<int>::max(),
                 &height) ||
        x + width - 1 > std::numeric_limits<int>::max() ||
        y + height - 1 > std::numeric_limits<int>::max()) {
        return false;
    }
    record.canvasBounds = QRect(static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
                                static_cast<int>(height));
    record.selection = selection.value;
    qint64 pixels = 0;
    qint64 bytes = record.canvasBytes;
    const QJsonValue result = object.value(QStringLiteral("result"));
    if (!result.isUndefined()) {
        CaptureHistoryResultRecord image;
        stored->resultFileName = QStringLiteral("capture_result.png");
        if (!result.isObject() || !parseImage(result.toObject(), *stored->resultFileName,
                                              &image.imageSize, &image.encodedBytes, &pixels)) {
            return false;
        }
        record.result = image;
        bytes += image.encodedBytes;
    }
    const QJsonArray displays = object.value(QStringLiteral("displays")).toArray();
    if (displays.isEmpty() || displays.size() > kMaximumDisplays) {
        return false;
    }
    for (qsizetype i = 0; i < displays.size(); ++i) {
        const QJsonObject display = displays[i].toObject();
        const QString file = QStringLiteral("display_%1.png").arg(i);
        CaptureHistoryDisplayRecord image;
        if (!display.value(QStringLiteral("stable_id")).isString() ||
            !display.value(QStringLiteral("display_name")).isString() ||
            !parseImage(display, file, &image.imageSize, &image.encodedBytes, &pixels)) {
            return false;
        }
        image.stableId = display.value(QStringLiteral("stable_id")).toString();
        image.name = display.value(QStringLiteral("display_name")).toString();
        record.displays.append(image);
        stored->displayFileNames.append(file);
        bytes += image.encodedBytes;
    }
    return record.totalBytes == bytes;
}

QByteArray indexBytes(const Snapshot& snapshot) {
    QJsonArray records;
    for (const auto& record : snapshot.records) {
        records.append(recordJson(record));
    }
    QJsonArray pending;
    for (auto it = snapshot.pendingDeletions.cbegin(); it != snapshot.pendingDeletions.cend();
         ++it) {
        pending.append(
            QJsonObject{{QStringLiteral("id"), it.key()}, {QStringLiteral("bytes"), it.value()}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("format_version"), kIndexVersion},
                                     {QStringLiteral("records"), records},
                                     {QStringLiteral("pending_deletions"), pending}})
        .toJson(QJsonDocument::Compact);
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

// Called only at an actual I/O boundary, never to resolve metadata or URLs.
bool containedPath(const QString& root, const QString& path) {
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QFileInfo info(path);
    if (canonicalRoot.isEmpty() || info.isSymLink()) {
        return false;
    }
    const QString canonical = info.exists() ? info.canonicalFilePath()
                                            : QFileInfo(info.absolutePath()).canonicalFilePath();
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return canonical.compare(canonicalRoot, sensitivity) == 0 ||
           canonical.startsWith(canonicalRoot + u'/', sensitivity);
}

QImage decodeImage(const QString& path) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return snow_shot::image_codec::decodeFileBgra(path, snow::image::Format::png);
#else
    return snow_shot::image_codec::decodeFile(path, snow::image::Format::png);
#endif
}

bool encodeDraft(const CaptureHistoryDraft& draft, qint64 quota, EncodedDraft* result) {
    if (!validUuid(draft.id) || !draft.createdUtc.isValid() ||
        draft.createdUtc.timeSpec() != Qt::UTC || draft.canvasBounds.isEmpty() ||
        draft.selection.rectangle.isEmpty() || !draft.selection.shadowColor.isValid() ||
        draft.selection.cornerRadius < 0 || draft.selection.cornerRadius > 256 ||
        draft.selection.shadowWidth < 0 || draft.selection.shadowWidth > 64 ||
        !validCanvas(draft.canvasHistory) || draft.displays.isEmpty() ||
        draft.displays.size() > kMaximumDisplays || sourceText(draft.source).isEmpty()) {
        return false;
    }
    auto& stored = result->stored;
    auto& record = stored.record;
    record.id = draft.id;
    record.createdUtc = draft.createdUtc;
    record.canvasBounds = draft.canvasBounds;
    record.selection = draft.selection;
    record.source = draft.source;
    record.canvasBytes = draft.canvasHistory.size();
    record.totalBytes = record.canvasBytes;
    result->files.insert(stored.canvasFileName, draft.canvasHistory);
    qint64 pixels = 0;
    const auto addImage = [&](const QSize& size, const QString& name,
                              const std::function<QByteArray()>& encode) -> qint64 {
        const qint64 count = static_cast<qint64>(size.width()) * size.height();
        if (size.isEmpty() || count > kMaximumPixelsPerImage ||
            pixels > kMaximumPixelsPerRecord - count) {
            return 0;
        }
        pixels += count;
        QByteArray bytes = encode();
        if (bytes.isEmpty() || record.totalBytes > quota - bytes.size()) {
            return 0;
        }
        const qint64 length = bytes.size();
        record.totalBytes += length;
        result->files.insert(name, std::move(bytes));
        return length;
    };
    if (draft.resultImage.has_value() || draft.preparedResultImage.has_value()) {
        const QSize size = draft.preparedResultImage ? draft.preparedResultImage->pixelSize()
                                                     : draft.resultImage->size();
        stored.resultFileName = QStringLiteral("capture_result.png");
        const qint64 bytes = addImage(size, *stored.resultFileName, [&]() {
            return draft.preparedResultImage
                       ? draft.preparedResultImage->bytes()
                       : snow_shot::image_codec::encodePng(*draft.resultImage);
        });
        if (bytes == 0)
            return false;
        record.result = CaptureHistoryResultRecord{size, bytes};
    }
    for (qsizetype i = 0; i < draft.displays.size(); ++i) {
        const auto& display = draft.displays[i];
        const QString name = QStringLiteral("display_%1.png").arg(i);
        const qint64 bytes = addImage(display.image.size(), name, [&]() {
            return snow_shot::image_codec::encodePng(display.image);
        });
        if (bytes == 0)
            return false;
        stored.displayFileNames.append(name);
        record.displays.append({display.stableId, display.name, display.image.size(), bytes});
    }
    return record.totalBytes <= quota;
}

void sortRecords(QVector<StoredRecord>& records) {
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        return a.record.createdUtc != b.record.createdUtc
                   ? a.record.createdUtc > b.record.createdUtc
                   : a.record.id < b.record.id;
    });
}
} // namespace

class CaptureHistoryRepositoryImpl final : public CaptureHistoryRepository {
  public:
    CaptureHistoryRepositoryImpl(QString root, CaptureHistoryRepositoryOptions options)
        : m_configurationDirectory(QDir::cleanPath(std::move(root))),
          m_root(QDir(m_configurationDirectory).filePath(QStringLiteral("capture_history"))),
          m_recordsRoot(QDir(m_root).filePath(QStringLiteral("records"))),
          m_indexPath(QDir(m_root).filePath(QStringLiteral("index.json"))),
          m_options(std::move(options)), m_policy(m_options.policy) {
        if (!m_policy.isValid())
            m_policy = {};
        if (!m_options.clock)
            m_options.clock = []() { return QDateTime::currentDateTimeUtc(); };
        loadIndex();
        if (m_options.writeAvailable && m_indexHealthy &&
            (!m_snapshot.pendingDeletions.isEmpty() || hasExpired())) {
            Command command;
            command.kind = Kind::Maintenance;
            enqueue(std::move(command));
        }
    }

    ~CaptureHistoryRepositoryImpl() override {
        {
            std::lock_guard lock(m_queueMutex);
            m_stopping = true;
        }
        m_condition.notify_all();
        if (m_worker.joinable())
            m_worker.join();
    }

    QVector<CaptureHistoryRecord> records() const override {
        std::lock_guard lock(m_stateMutex);
        QVector<CaptureHistoryRecord> result;
        result.reserve(m_snapshot.records.size());
        for (const auto& stored : m_snapshot.records)
            result.append(stored.record);
        return result;
    }

    CaptureHistoryUsage usage() const override {
        std::lock_guard lock(m_stateMutex);
        return m_usage;
    }

    CaptureHistoryPolicy policy() const override {
        std::lock_guard lock(m_stateMutex);
        return m_policy;
    }

    QString lastError() const override {
        std::lock_guard lock(m_stateMutex);
        return m_error;
    }

    std::shared_future<CaptureHistoryPublishResult> publish(CaptureHistoryDraft draft) override {
        {
            std::lock_guard lock(m_stateMutex);
            if (!m_options.writeAvailable || !m_indexHealthy || !m_policy.enabled) {
                return readyFuture(
                    CaptureHistoryPublishResult{StorageResult::failure(QStringLiteral(
                                                    "Capture-history publication is unavailable")),
                                                {}});
            }
        }
        Command command;
        command.kind = Kind::Publish;
        command.draft = std::move(draft);
        command.publication = std::make_shared<std::promise<CaptureHistoryPublishResult>>();
        auto future = command.publication->get_future().share();
        enqueue(std::move(command));
        return future;
    }

    std::optional<CaptureHistoryAssetSet>
    displayAssets(const CaptureHistoryRecord& record) const override {
        const auto stored = find(record);
        if (!stored)
            return std::nullopt;
        CaptureHistoryAssetSet assets;
        assets.recordId = record.id;
        const QDir directory(recordPath(record.id));
        if (record.result) {
            assets.result = CaptureHistoryResultAsset{
                record.id, record.result->imageSize,
                QUrl::fromLocalFile(directory.filePath(*stored->resultFileName))};
        }
        for (qsizetype i = 0; i < record.displays.size(); ++i) {
            const auto& display = record.displays[i];
            assets.displays.append(
                {record.id, display.stableId, display.name, display.imageSize,
                 QUrl::fromLocalFile(directory.filePath(stored->displayFileNames[i]))});
        }
        return assets;
    }

    std::optional<CaptureHistoryPayload> load(const CaptureHistoryRecord& record) const override {
        const auto stored = find(record);
        if (!stored)
            return std::nullopt;
        const QDir directory(recordPath(record.id));
        const QString canvasPath = directory.filePath(stored->canvasFileName);
        observe(CaptureHistoryOperation::PayloadRead);
        QFile canvas(canvasPath);
        CaptureHistoryPayload payload;
        if (!containedPath(m_root, canvasPath) || !canvas.open(QIODevice::ReadOnly)) {
            readFailed(record);
            return std::nullopt;
        }
        payload.canvasHistory = canvas.read(kMaximumCanvasBytes + 1);
        const bool readError = canvas.error() != QFileDevice::NoError;
        canvas.close();
        if (readError || payload.canvasHistory.size() != record.canvasBytes ||
            !validCanvas(payload.canvasHistory)) {
            readFailed(record);
            return std::nullopt;
        }
        for (qsizetype i = 0; i < stored->displayFileNames.size(); ++i) {
            const auto image = readImage(record, directory.filePath(stored->displayFileNames[i]),
                                         record.displays[i].imageSize);
            if (!image)
                return std::nullopt;
            payload.displayImages.append(*image);
        }
        return payload;
    }

    std::optional<QImage> loadResultImage(const CaptureHistoryRecord& record) const override {
        const auto stored = find(record);
        if (!stored || !record.result)
            return std::nullopt;
        return readImage(record, QDir(recordPath(record.id)).filePath(*stored->resultFileName),
                         record.result->imageSize);
    }

    void reportReadFailure(const CaptureHistoryRecord& record, const QString& reason) override {
        if (!find(record))
            return;
        Command command;
        command.kind = Kind::ReadFailure;
        command.record = record;
        command.reason = reason;
        enqueue(std::move(command));
    }

    std::shared_future<StorageResult> remove(const QString& id) override {
        Command command;
        command.kind = Kind::Remove;
        command.id = id;
        return submit(std::move(command));
    }

    std::shared_future<StorageResult> updatePolicy(CaptureHistoryPolicy policy) override {
        if (!policy.isValid())
            return readyFuture(
                StorageResult::failure(QStringLiteral("The capture-history policy is invalid")));
        Command command;
        command.kind = Kind::Policy;
        command.policy = policy;
        return submit(std::move(command));
    }

    std::shared_future<StorageResult> requestClear() override {
        Command command;
        command.kind = Kind::Clear;
        return submit(std::move(command));
    }

    void drain() override {
        std::unique_lock lock(m_queueMutex);
        m_drained.wait(lock, [&]() { return m_queue.empty() && !m_active; });
    }

  private:
    enum class Kind { Publish, Remove, Policy, Clear, Maintenance, ReadFailure };
    struct Command {
        Kind kind = Kind::Maintenance;
        CaptureHistoryDraft draft;
        CaptureHistoryRecord record;
        CaptureHistoryPolicy policy;
        QString id;
        QString reason;
        std::shared_ptr<std::promise<CaptureHistoryPublishResult>> publication;
        std::shared_ptr<std::promise<StorageResult>> completion;
    };

    void observe(CaptureHistoryOperation operation) const {
        if (m_options.operationObserved)
            m_options.operationObserved(operation);
    }

    QString recordPath(const QString& id) const {
        return QDir(m_recordsRoot).filePath(id);
    }

    std::optional<StoredRecord> find(const CaptureHistoryRecord& record) const {
        std::lock_guard lock(m_stateMutex);
        const auto index = m_recordIndex.constFind(record.id);
        if (index == m_recordIndex.cend() || !(m_snapshot.records[*index].record == record)) {
            return std::nullopt;
        }
        return m_snapshot.records[*index];
    }

    void readFailed(const CaptureHistoryRecord& record) const {
        const_cast<CaptureHistoryRepositoryImpl*>(this)->reportReadFailure(
            record, QStringLiteral("Unable to read a capture-history payload"));
    }

    std::optional<QImage> readImage(const CaptureHistoryRecord& record, const QString& path,
                                    const QSize& size) const {
        observe(CaptureHistoryOperation::PayloadRead);
        const QImage image = containedPath(m_root, path) ? decodeImage(path) : QImage();
        if (image.isNull() || image.size() != size) {
            readFailed(record);
            return std::nullopt;
        }
        return image;
    }

    StorageResult fail(const QString& error) {
        {
            std::lock_guard lock(m_stateMutex);
            m_error = error;
        }
        if (m_options.callbacks.errorChanged)
            m_options.callbacks.errorChanged(error);
        return StorageResult::failure(error);
    }

    Snapshot snapshot() const {
        std::lock_guard lock(m_stateMutex);
        return m_snapshot;
    }

    void install(Snapshot next, qint64 bytes) {
        CaptureHistoryUsage usage;
        usage.entryCount = static_cast<int>(next.records.size());
        usage.indexBytes = bytes;
        for (const auto& stored : next.records)
            usage.recordBytes += stored.record.totalBytes;
        for (const qint64 pending : next.pendingDeletions)
            usage.pendingDeletionBytes += pending;
        usage.totalBytes = usage.recordBytes + usage.indexBytes + usage.pendingDeletionBytes;
        {
            std::lock_guard lock(m_stateMutex);
            m_snapshot = std::move(next);
            m_recordIndex.clear();
            for (qsizetype i = 0; i < m_snapshot.records.size(); ++i) {
                m_recordIndex.insert(m_snapshot.records[i].record.id, i);
            }
            m_usage = usage;
        }
        if (m_options.callbacks.usageChanged)
            m_options.callbacks.usageChanged(usage);
    }

    void changed() {
        if (m_options.callbacks.recordsChanged)
            m_options.callbacks.recordsChanged();
    }

    bool commit(Snapshot next) {
        const QByteArray bytes = indexBytes(next);
        observe(CaptureHistoryOperation::IndexWrite);
        if (!QDir().mkpath(m_root) || !containedPath(m_configurationDirectory, m_root) ||
            !writeFile(m_indexPath, bytes)) {
            fail(QStringLiteral("Unable to commit the capture-history index"));
            return false;
        }
        install(std::move(next), bytes.size());
        return true;
    }

    void loadIndex() {
        observe(CaptureHistoryOperation::IndexRead);
        QFile file(m_indexPath);
        if (!file.open(QIODevice::ReadOnly)) {
            if (!QFileInfo::exists(m_indexPath))
                return;
            indexFailed();
            return;
        }
        const QByteArray bytes = file.readAll();
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        const QJsonObject object = document.object();
        if (file.error() != QFileDevice::NoError || !document.isObject() ||
            object.value(QStringLiteral("format_version")).toInteger() != kIndexVersion ||
            !object.value(QStringLiteral("records")).isArray() ||
            !object.value(QStringLiteral("pending_deletions")).isArray()) {
            indexFailed();
            return;
        }
        Snapshot next;
        QSet<QString> ids;
        qint64 totalBytes = 0;
        for (const auto& value : object.value(QStringLiteral("records")).toArray()) {
            StoredRecord stored;
            if (!value.isObject() || !parseRecord(value.toObject(), &stored) ||
                ids.contains(stored.record.id) ||
                totalBytes > kMaximumStoredBytes - stored.record.totalBytes) {
                indexFailed();
                return;
            }
            totalBytes += stored.record.totalBytes;
            ids.insert(stored.record.id);
            next.records.append(std::move(stored));
        }
        for (const auto& value : object.value(QStringLiteral("pending_deletions")).toArray()) {
            const QJsonObject pending = value.toObject();
            const QString id = pending.value(QStringLiteral("id")).toString();
            qint64 size = 0;
            if (!validUuid(id) || ids.contains(id) ||
                !integer(pending.value(QStringLiteral("bytes")), 0, kMaximumStoredBytes, &size) ||
                totalBytes > kMaximumStoredBytes - size) {
                indexFailed();
                return;
            }
            totalBytes += size;
            ids.insert(id);
            next.pendingDeletions.insert(id, size);
        }
        sortRecords(next.records);
        install(std::move(next), bytes.size());
    }

    void indexFailed() {
        m_indexHealthy = false;
        fail(QStringLiteral("Unable to read the capture-history index; clear history to reset it"));
    }

    bool hasExpired() const {
        const auto current = policy();
        if (!current.enabled)
            return false;
        const auto cutoff = m_options.clock().toUTC().addDays(-current.retentionDays);
        const auto currentSnapshot = snapshot();
        return std::any_of(currentSnapshot.records.cbegin(), currentSnapshot.records.cend(),
                           [&](const auto& record) { return record.record.createdUtc < cutoff; });
    }

    void prune(Snapshot& next, bool capacity, const QString& protectedId = {}) const {
        const auto current = policy();
        if (!current.enabled)
            return;
        const auto cutoff = m_options.clock().toUTC().addDays(-current.retentionDays);
        qint64 bytes = 0;
        for (const auto& record : next.records)
            bytes += record.record.totalBytes;
        qsizetype count = next.records.size();
        QSet<QString> victims;
        for (auto it = next.records.crbegin(); it != next.records.crend(); ++it) {
            const auto& record = it->record;
            if (record.id == protectedId)
                continue;
            if (record.createdUtc < cutoff ||
                (capacity && (count > current.maxEntries ||
                              bytes > static_cast<qint64>(current.maxDiskMiB) * kMiB))) {
                victims.insert(record.id);
                next.pendingDeletions.insert(record.id, record.totalBytes);
                bytes -= record.totalBytes;
                --count;
            }
        }
        next.records.removeIf(
            [&](const auto& record) { return victims.contains(record.record.id); });
    }

    bool cleanup() {
        Snapshot next = snapshot();
        bool removed = false;
        bool success = true;
        for (auto it = next.pendingDeletions.begin(); it != next.pendingDeletions.end();) {
            const QString path = recordPath(it.key());
            if ((!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) ||
                (containedPath(m_root, path) && QDir(path).removeRecursively())) {
                it = next.pendingDeletions.erase(it);
                removed = true;
            } else {
                success = false;
                ++it;
            }
        }
        if (removed && !commit(std::move(next)))
            return false;
        if (!success)
            fail(QStringLiteral("Unable to delete some capture-history payloads"));
        return success;
    }

    CaptureHistoryPublishResult publishNow(const CaptureHistoryDraft& draft) {
        if (!policy().enabled)
            return {fail(QStringLiteral("Capture history is disabled")), {}};
        Snapshot next = snapshot();
        if (next.pendingDeletions.contains(draft.id) ||
            std::any_of(next.records.cbegin(), next.records.cend(),
                        [&](const auto& r) { return r.record.id == draft.id; })) {
            return {fail(QStringLiteral("The capture-history ID already exists")), {}};
        }
        EncodedDraft encoded;
        if (!encodeDraft(draft, static_cast<qint64>(policy().maxDiskMiB) * kMiB, &encoded)) {
            return {
                fail(QStringLiteral("The capture-history draft is invalid or exceeds its quota")),
                {}};
        }
        if (!QDir().mkpath(m_recordsRoot) ||
            !containedPath(m_configurationDirectory, m_recordsRoot)) {
            return {fail(QStringLiteral("Unable to create the capture-history directory")), {}};
        }
        const QString temporary =
            QStringLiteral(".tmp-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir root(m_recordsRoot);
        if (root.exists(draft.id) || !root.mkdir(temporary)) {
            return {fail(QStringLiteral("Unable to create a temporary history record")), {}};
        }
        const QString temporaryPath = root.filePath(temporary);
        bool wrote = true;
        for (auto it = encoded.files.cbegin(); wrote && it != encoded.files.cend(); ++it) {
            wrote = writeFile(QDir(temporaryPath).filePath(it.key()), it.value());
        }
        if (!wrote || !root.rename(temporary, draft.id)) {
            QDir(temporaryPath).removeRecursively();
            return {fail(QStringLiteral("Unable to write a capture-history record")), {}};
        }
        next.records.append(encoded.stored);
        sortRecords(next.records);
        prune(next, true, draft.id);
        if (!commit(std::move(next))) {
            QDir(recordPath(draft.id)).removeRecursively();
            return {StorageResult::failure(lastError()), {}};
        }
        changed();
        cleanup();
        return {StorageResult::ok(), encoded.stored.record};
    }

    StorageResult removeNow(const QString& id) {
        Snapshot next = snapshot();
        const auto found = std::find_if(next.records.begin(), next.records.end(),
                                        [&](const auto& record) { return record.record.id == id; });
        if (found != next.records.end()) {
            next.pendingDeletions.insert(id, found->record.totalBytes);
            next.records.erase(found);
            if (!commit(std::move(next)))
                return StorageResult::failure(lastError());
            changed();
        }
        return cleanup() ? StorageResult::ok() : StorageResult::failure(lastError());
    }

    StorageResult maintenance() {
        Snapshot next = snapshot();
        const auto count = next.records.size();
        prune(next, false);
        if (next.records.size() != count) {
            if (!commit(std::move(next)))
                return StorageResult::failure(lastError());
            changed();
        }
        return cleanup() ? StorageResult::ok() : StorageResult::failure(lastError());
    }

    StorageResult clearNow() {
        // Only explicit clear discovers/removes unmanaged legacy and crash leftovers.
        bool success = true;
        for (const QString& path :
             {m_root,
              QDir(m_configurationDirectory).filePath(QStringLiteral("capture_history_records")),
              QDir(m_configurationDirectory)
                  .filePath(QStringLiteral("capture_history_quarantine"))}) {
            if (QFileInfo::exists(path) && (!containedPath(m_configurationDirectory, path) ||
                                            !QDir(path).removeRecursively())) {
                success = false;
            }
        }
        if (!success) {
            // Keep the old index state when possible; missing files fail on actual reads.
            return fail(QStringLiteral("Unable to clear all managed capture-history data"));
        }
        if (!commit({}))
            return StorageResult::failure(lastError());
        {
            std::lock_guard lock(m_stateMutex);
            m_indexHealthy = true;
            m_error.clear();
        }
        if (m_options.callbacks.errorChanged)
            m_options.callbacks.errorChanged({});
        changed();
        return StorageResult::ok();
    }

    std::shared_future<StorageResult> submit(Command command) {
        command.completion = std::make_shared<std::promise<StorageResult>>();
        auto future = command.completion->get_future().share();
        enqueue(std::move(command));
        return future;
    }

    void finish(Command& command, const StorageResult& result) {
        if (command.completion)
            command.completion->set_value(result);
        if (command.kind == Kind::Policy && m_options.callbacks.policyFinished) {
            m_options.callbacks.policyFinished(result.success, result.error);
        }
        if (command.kind == Kind::Clear && m_options.callbacks.clearFinished) {
            m_options.callbacks.clearFinished(result.success, result.error);
        }
    }

    void reject(Command& command, const QString& reason) {
        const auto result = StorageResult::failure(reason);
        if (command.publication)
            command.publication->set_value({result, {}});
        finish(command, result);
    }

    void enqueue(Command command) {
        std::unique_lock lock(m_queueMutex);
        QString rejection;
        {
            std::lock_guard stateLock(m_stateMutex);
            if (!m_options.writeAvailable || (!m_indexHealthy && command.kind != Kind::Clear)) {
                rejection = QStringLiteral("Capture-history storage is not writable");
            }
        }
        if (m_stopping)
            rejection = QStringLiteral("Capture-history storage is shutting down");
        if (command.kind == Kind::Publish &&
            m_publications >= std::max(0, m_options.maxQueuedPublications) + 1) {
            rejection = QStringLiteral("The capture-history write queue is full");
        }
        if (!rejection.isEmpty()) {
            lock.unlock();
            reject(command, rejection);
            return;
        }
        if (command.kind == Kind::ReadFailure && m_failedReads.contains(command.record.id))
            return;
        if (!m_worker.joinable()) {
            try {
                m_worker = std::thread([this]() { run(); });
            } catch (...) {
                lock.unlock();
                reject(command, QStringLiteral("Unable to start the capture-history worker"));
                return;
            }
        }
        if (command.kind == Kind::ReadFailure)
            m_failedReads.insert(command.record.id);
        if (command.kind == Kind::Publish)
            ++m_publications;
        std::deque<Command> cancelled;
        if (command.kind == Kind::Clear) {
            for (auto it = m_queue.begin(); it != m_queue.end();) {
                if (it->kind == Kind::Publish) {
                    cancelled.push_back(std::move(*it));
                    it = m_queue.erase(it);
                    --m_publications;
                } else
                    ++it;
            }
            m_queue.push_front(std::move(command));
        } else
            m_queue.push_back(std::move(command));
        lock.unlock();
        for (auto& cancelledCommand : cancelled) {
            reject(cancelledCommand,
                   QStringLiteral("The capture-history publication was cancelled by clear"));
        }
        m_condition.notify_one();
    }

    void run() {
        observe(CaptureHistoryOperation::WorkerStarted);
        for (;;) {
            Command command;
            {
                std::unique_lock lock(m_queueMutex);
                m_condition.wait(lock, [&]() { return m_stopping || !m_queue.empty(); });
                if (m_queue.empty())
                    return;
                command = std::move(m_queue.front());
                m_queue.pop_front();
                m_active = true;
            }
            try {
                StorageResult result = StorageResult::ok();
                switch (command.kind) {
                case Kind::Publish:
                    command.publication->set_value(publishNow(command.draft));
                    break;
                case Kind::Remove:
                    result = removeNow(command.id);
                    break;
                case Kind::Clear:
                    result = clearNow();
                    break;
                case Kind::Maintenance:
                    result = maintenance();
                    break;
                case Kind::ReadFailure:
                    if (find(command.record)) {
                        fail(command.reason);
                        result = removeNow(command.record.id);
                    }
                    break;
                case Kind::Policy: {
                    const auto previous = policy();
                    {
                        std::lock_guard lock(m_stateMutex);
                        m_policy = command.policy;
                    }
                    if (command.policy.enabled &&
                        (!previous.enabled ||
                         previous.retentionDays != command.policy.retentionDays))
                        result = maintenance();
                    else if (!cleanup())
                        result = StorageResult::failure(lastError());
                    break;
                }
                }
                finish(command, result);
            } catch (...) {
                reject(command, QStringLiteral("Capture-history storage operation failed"));
            }
            {
                std::lock_guard lock(m_queueMutex);
                if (command.kind == Kind::Publish)
                    --m_publications;
                if (command.kind == Kind::ReadFailure)
                    m_failedReads.remove(command.record.id);
                m_active = false;
                if (m_queue.empty())
                    m_drained.notify_all();
            }
        }
    }

    QString m_configurationDirectory;
    QString m_root;
    QString m_recordsRoot;
    QString m_indexPath;
    CaptureHistoryRepositoryOptions m_options;
    mutable std::mutex m_stateMutex;
    Snapshot m_snapshot;
    QHash<QString, qsizetype> m_recordIndex;
    CaptureHistoryPolicy m_policy;
    CaptureHistoryUsage m_usage;
    QString m_error;
    bool m_indexHealthy = true;
    std::mutex m_queueMutex;
    std::condition_variable m_condition;
    std::condition_variable m_drained;
    std::deque<Command> m_queue;
    QSet<QString> m_failedReads;
    std::thread m_worker;
    int m_publications = 0;
    bool m_stopping = false;
    bool m_active = false;
};

std::unique_ptr<CaptureHistoryRepository>
makeCaptureHistoryRepository(QString configurationDirectory,
                             CaptureHistoryRepositoryOptions options) {
    return std::make_unique<CaptureHistoryRepositoryImpl>(std::move(configurationDirectory),
                                                          std::move(options));
}
} // namespace snow_shot::storage
