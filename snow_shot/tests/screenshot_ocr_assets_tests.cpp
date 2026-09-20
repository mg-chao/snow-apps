#include "snow_shot/presentation/screenshotocrassets.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QSemaphore>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdlib>
#include <atomic>
#include <functional>
#include <iostream>
#ifdef Q_OS_WIN
#include <Windows.h>
#endif

#include <mz.h>
#include <mz_strm.h>
#include <mz_strm_mem.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace {
#ifdef Q_OS_MACOS
const QString kWorkerName = QStringLiteral("snow-ocr-process");
#endif
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool waitUntil(const std::function<bool()>& condition, int timeoutMs) {
    if (condition()) {
        return true;
    }
    QEventLoop loop;
    QTimer poll;
    QTimer timeout;
    poll.setInterval(5);
    timeout.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (condition()) {
            loop.quit();
        }
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    timeout.start(timeoutMs);
    loop.exec();
    return condition();
}

void processEventsFor(int durationMs) {
    QEventLoop loop;
    QTimer::singleShot(durationMs, &loop, &QEventLoop::quit);
    loop.exec();
}

QJsonObject assetFile(const QString& name, const QByteArray& contents, const QString& url = {}) {
    QJsonObject result{
        {QStringLiteral("name"), name},
        {QStringLiteral("size"), contents.size()},
        {QStringLiteral("sha256"),
         QString::fromLatin1(
             QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex())}};
    if (!url.isEmpty())
        result.insert(QStringLiteral("url"), url);
    return result;
}

void writeFixture(const QString& path, const QByteArray& contents) {
    require(QDir().mkpath(QFileInfo(path).dir().absolutePath()),
            "OCR asset fixture directory should be writable");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "OCR asset fixture should be writable");
    require(file.write(contents) == contents.size(), "OCR asset fixture write should complete");
}

void writeAssetManifest(const QString& root, bool completePayload) {
    const QByteArray process("process");
    const QByteArray directMl("directml");
    const QByteArray runtimeManifest("runtime");
    const QByteArray detector("detector");
    const QByteArray recognizer("recognizer");
    const QByteArray dictionary("dictionary");
    const QString runtimeDirectory =
        QDir(root).filePath(QStringLiteral("runtimes/1.0.7/windows-x64"));
    const QString modelDirectory =
        QDir(root).filePath(QStringLiteral("models/ppocrv6-small-463ea9f"));
    if (completePayload) {
        writeFixture(QDir(runtimeDirectory)
                         .filePath(QStringLiteral("snow-ocr-process-1.0.7-windows-x64.exe")),
                     process);
        writeFixture(QDir(runtimeDirectory).filePath(QStringLiteral("DirectML.dll")), directMl);
        writeFixture(QDir(runtimeDirectory).filePath(QStringLiteral("runtime-manifest.json")),
                     runtimeManifest);
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral("PP-OCRv6_det_small.onnx")),
                     detector);
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral("PP-OCRv6_rec_small.onnx")),
                     recognizer);
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral("ppocrv6_dict.txt")), dictionary);
        writeFixture(QDir(runtimeDirectory).filePath(QStringLiteral(".complete.json")),
                     R"({"schema":1,"component":"1.0.7"})");
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral(".complete.json")),
                     R"({"schema":1,"component":"ppocrv6-small-463ea9f"})");
    }
    const QJsonArray runtimeFiles{
        assetFile(QStringLiteral("snow-ocr-process-1.0.7-windows-x64.exe"), process),
        assetFile(QStringLiteral("DirectML.dll"), directMl),
        assetFile(QStringLiteral("runtime-manifest.json"), runtimeManifest)};
    const auto model = [](const QString& type, const QString& id, const QString& detectorName,
                          const QByteArray& detectorContents, const QString& recognizerName,
                          const QByteArray& recognizerContents, const QString& dictionaryName,
                          const QByteArray& dictionaryContents) {
        return QJsonObject{
            {QStringLiteral("type"), type},
            {QStringLiteral("id"), id},
            {QStringLiteral("detector"), detectorName},
            {QStringLiteral("recognizer"), recognizerName},
            {QStringLiteral("dictionary"), dictionaryName},
            {QStringLiteral("files"),
             QJsonArray{
                 assetFile(detectorName, detectorContents,
                           QStringLiteral("https://example.invalid/") + detectorName),
                 assetFile(recognizerName, recognizerContents,
                           QStringLiteral("https://example.invalid/") + recognizerName),
                 assetFile(dictionaryName, dictionaryContents,
                           QStringLiteral("https://example.invalid/") + dictionaryName),
             }},
        };
    };
    const QByteArray archive("archive");
    QJsonObject manifest{
        {QStringLiteral("schema"), 2},
        {QStringLiteral("default_model"), QStringLiteral("small")},
        {QStringLiteral("runtime"),
         QJsonObject{{QStringLiteral("version"), QStringLiteral("1.0.7")},
                     {QStringLiteral("platform"), QStringLiteral("windows-x64")},
                     {QStringLiteral("archive"),
                      assetFile(QStringLiteral("snow-ocr-runtime-1.0.7-windows-x64.zip"), archive,
                                QStringLiteral("https://example.invalid/runtime"))},
                     {QStringLiteral("files"), runtimeFiles}}},
        {QStringLiteral("models"),
         QJsonArray{
             model(QStringLiteral("extra_small"), QStringLiteral("ppocrv6-tiny-cd609a1"),
                   QStringLiteral("PP-OCRv6_det_tiny.onnx"), QByteArray("tiny-detector"),
                   QStringLiteral("PP-OCRv6_rec_tiny.onnx"), QByteArray("tiny-recognizer"),
                   QStringLiteral("ppocrv6_tiny_dict.txt"), QByteArray("tiny-dictionary")),
             model(QStringLiteral("small"), QStringLiteral("ppocrv6-small-463ea9f"),
                   QStringLiteral("PP-OCRv6_det_small.onnx"), detector,
                   QStringLiteral("PP-OCRv6_rec_small.onnx"), recognizer,
                   QStringLiteral("ppocrv6_dict.txt"), dictionary),
             model(QStringLiteral("medium"), QStringLiteral("ppocrv6-medium-f5063c6"),
                   QStringLiteral("PP-OCRv6_det_medium.onnx"), QByteArray("medium-detector"),
                   QStringLiteral("PP-OCRv6_rec_medium.onnx"), QByteArray("medium-recognizer"),
                   QStringLiteral("ppocrv6_dict.txt"), dictionary),
             model(QStringLiteral("small_v5"), QStringLiteral("ppocrv5-small-7b2a75a"),
                   QStringLiteral("ch_PP-OCRv5_det_mobile.onnx"), QByteArray("detector"),
                   QStringLiteral("ch_PP-OCRv5_rec_mobile.onnx"), QByteArray("recognizer"),
                   QStringLiteral("ppocrv5_dict.txt"), dictionary),
             model(QStringLiteral("medium_v5"), QStringLiteral("ppocrv5-medium-7b2a75a"),
                   QStringLiteral("ch_PP-OCRv5_det_server.onnx"), QByteArray("detector"),
                   QStringLiteral("ch_PP-OCRv5_rec_server.onnx"), QByteArray("recognizer"),
                   QStringLiteral("ppocrv5_dict.txt"), dictionary),
             model(QStringLiteral("small_v4"), QStringLiteral("ppocrv4-small-7b2a75a"),
                   QStringLiteral("ch_PP-OCRv4_det_mobile.onnx"), QByteArray("detector"),
                   QStringLiteral("ch_PP-OCRv4_rec_mobile.onnx"), QByteArray("recognizer"),
                   QStringLiteral("ppocr_keys_v1.txt"), dictionary),
             model(QStringLiteral("medium_v4"), QStringLiteral("ppocrv4-medium-7b2a75a"),
                   QStringLiteral("ch_PP-OCRv4_det_server.onnx"), QByteArray("detector"),
                   QStringLiteral("ch_PP-OCRv4_rec_server.onnx"), QByteArray("recognizer"),
                   QStringLiteral("ppocr_keys_v1.txt"), dictionary),
         }}};
#ifdef Q_OS_MACOS
    const QByteArray armHeader = QByteArray::fromHex("cffaedfe0c000001") + QByteArray(24, '\0');
    const QByteArray macProcess = armHeader + process;
    const QByteArray macLibrary = armHeader + directMl;
    writeFixture(QDir(root).filePath(kWorkerName), macProcess);
    require(QFile::setPermissions(QDir(root).filePath(kWorkerName), QFileDevice::ReadOwner |
                                                                        QFileDevice::WriteOwner |
                                                                        QFileDevice::ExeOwner),
            "the fixture worker must have execute permission");
    writeFixture(QDir(root).filePath(QStringLiteral("libonnxruntime.dylib")), macLibrary);
    manifest.insert(QStringLiteral("schema"), 3);
    manifest.insert(
        QStringLiteral("runtime"),
        QJsonObject{{QStringLiteral("version"), QStringLiteral("1.0.7")},
                    {QStringLiteral("platform"), QStringLiteral("macos-arm64")},
                    {QStringLiteral("delivery"), QStringLiteral("bundled")},
                    {QStringLiteral("protocol"), 3},
                    {QStringLiteral("executable"), kWorkerName},
                    {QStringLiteral("files"),
                     QJsonArray{assetFile(kWorkerName, macProcess),
                                assetFile(QStringLiteral("libonnxruntime.dylib"), macLibrary)}}});
#endif
    writeFixture(QDir(root).filePath(QStringLiteral("asset-manifest.json")),
                 QJsonDocument(manifest).toJson(QJsonDocument::Compact));
}

QByteArray modelFixtureContents(const QString& name) {
    if (name.startsWith(QStringLiteral("ch_PP-OCRv")))
        return name.contains(QStringLiteral("_det_")) ? QByteArray("detector")
                                                      : QByteArray("recognizer");
    if (name == QStringLiteral("ppocrv5_dict.txt") || name == QStringLiteral("ppocr_keys_v1.txt"))
        return QByteArray("dictionary");
    if (name == QStringLiteral("PP-OCRv6_det_tiny.onnx"))
        return QByteArray("tiny-detector");
    if (name == QStringLiteral("PP-OCRv6_rec_tiny.onnx"))
        return QByteArray("tiny-recognizer");
    if (name == QStringLiteral("ppocrv6_tiny_dict.txt"))
        return QByteArray("tiny-dictionary");
    if (name == QStringLiteral("PP-OCRv6_det_small.onnx"))
        return QByteArray("detector");
    if (name == QStringLiteral("PP-OCRv6_rec_small.onnx"))
        return QByteArray("recognizer");
    if (name == QStringLiteral("PP-OCRv6_det_medium.onnx"))
        return QByteArray("medium-detector");
    if (name == QStringLiteral("PP-OCRv6_rec_medium.onnx"))
        return QByteArray("medium-recognizer");
    if (name == QStringLiteral("ppocrv6_dict.txt"))
        return QByteArray("dictionary");
    return {};
}

bool writeDownloadedModelFixture(const QString& destination, QString* error) {
    const QByteArray contents = modelFixtureContents(QFileInfo(destination).fileName());
    if (contents.isEmpty()) {
        *error = QStringLiteral("unexpected fixture download");
        return false;
    }
    writeFixture(destination, contents);
    return true;
}

// Builds the runtime archive in memory so the fixture itself never depends on
// path encoding choices made by the production archive reader.
QByteArray buildRuntimeArchiveBytes() {
    const QList<QPair<QString, QByteArray>> entries{
        {QStringLiteral("snow-ocr-process-1.0.7-windows-x64.exe"), QByteArray("process")},
        {QStringLiteral("DirectML.dll"), QByteArray("directml")},
        {QStringLiteral("runtime-manifest.json"), QByteArray("runtime")},
    };
    void* stream = mz_stream_mem_create();
    require(stream != nullptr, "the fixture memory stream must be created");
    mz_stream_mem_set_grow_size(stream, 64 * 1024);
    require(mz_stream_mem_open(stream, nullptr, MZ_OPEN_MODE_CREATE) == MZ_OK,
            "the fixture memory stream must open");
    void* writer = mz_zip_writer_create();
    require(writer != nullptr, "the fixture archive writer must be created");
    require(mz_zip_writer_open(writer, stream, 0) == MZ_OK, "the fixture archive writer must open");
    for (const auto& entry : entries) {
        const QByteArray name = entry.first.toUtf8();
        mz_zip_file info{};
        info.filename = name.constData();
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        require(mz_zip_writer_add_buffer(writer, entry.second.constData(),
                                         static_cast<int32_t>(entry.second.size()), &info) == MZ_OK,
                "the fixture archive entry must be written");
    }
    require(mz_zip_writer_close(writer) == MZ_OK, "the fixture archive must close");
    const void* buffer = nullptr;
    require(mz_stream_mem_get_buffer(stream, &buffer) == MZ_OK && buffer != nullptr,
            "the fixture archive buffer must be readable");
    int32_t length = 0;
    mz_stream_mem_get_buffer_length(stream, &length);
    const QByteArray archive(static_cast<const char*>(buffer), length);
    mz_zip_writer_delete(&writer);
    mz_stream_mem_close(stream);
    mz_stream_mem_delete(&stream);
    return archive;
}

void validOfflineAssetsAreSelectedWithoutNetwork() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(), "temporary OCR asset roots should be available");
    writeAssetManifest(offline.path(), true);
    int downloads = 0;
    bool ready = false;
    bool failed = false;
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.bundledRuntimeRoot = offline.path();
    options.cacheRoot = cache.path();
    options.downloadOverride = [&](const QString&, const QString&, QString*) {
        ++downloads;
        return false;
    };
    ScreenshotOcrAssets assets(options);
    QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                     [&](const ScreenshotOcrResolvedAssets& result) {
                         ready = result.offline && result.valid();
                     });
    QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets,
                     [&](const QString&) { failed = true; });
    assets.prepare();
    require(waitUntil([&]() { return ready || failed; }, 5'000),
            "offline OCR asset validation should complete");
    require(ready && !failed, "a complete hash-valid offline payload should be selected");
    require(downloads == 0, "valid offline OCR assets must not use the network");
}

void incompleteOfflineAssetsFallBackToOnlineAcquisition() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(),
            "temporary OCR fallback roots should be available");
    writeAssetManifest(offline.path(), false);
    int downloads = 0;
    bool finished = false;
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.bundledRuntimeRoot = offline.path();
    options.cacheRoot = cache.path();
    options.downloadOverride = [&](const QString&, const QString&, QString* error) {
        ++downloads;
        *error = QStringLiteral("fixture download stopped");
        return false;
    };
    ScreenshotOcrAssets assets(options);
    QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets,
                     [&](const QString&) { finished = true; });
    assets.prepare();
    require(waitUntil([&]() { return finished; }, 5'000),
            "invalid offline OCR assets should attempt online acquisition");
    require(downloads == 1, "incomplete offline assets must enter online acquisition once");
}

void everyModelMapsToItsOwnFilesAndCachesAreRetained() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(), "temporary OCR model roots should be available");
    writeAssetManifest(offline.path(), true);
    QSet<QString> downloads;
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.bundledRuntimeRoot = offline.path();
    options.cacheRoot = cache.path();
    options.modelType = ScreenshotOcrModelType::Medium;
    options.downloadOverride = [&](const QString&, const QString& destination, QString* error) {
        downloads.insert(QFileInfo(destination).fileName());
        return writeDownloadedModelFixture(destination, error);
    };
    ScreenshotOcrAssets assets(options);
    ScreenshotOcrResolvedAssets resolved;
    int readyCount = 0;
    QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                     [&](const ScreenshotOcrResolvedAssets& result) {
                         resolved = result;
                         ++readyCount;
                     });
    assets.prepare();
    require(waitUntil([&]() { return readyCount == 1; }, 5'000),
            "the Medium model fixture should be acquired");
    require(resolved.modelType == ScreenshotOcrModelType::Medium &&
                resolved.modelId == QStringLiteral("ppocrv6-medium-f5063c6") &&
                resolved.detectorModelPath.endsWith(QStringLiteral("PP-OCRv6_det_medium.onnx")) &&
                resolved.recognizerModelPath.endsWith(QStringLiteral("PP-OCRv6_rec_medium.onnx")) &&
                resolved.dictionaryPath.endsWith(QStringLiteral("ppocrv6_dict.txt")) &&
                !resolved.offline,
            "Medium must combine the bundled runtime with only its cached model files");
    require(downloads == QSet<QString>{QStringLiteral("PP-OCRv6_det_medium.onnx"),
                                       QStringLiteral("PP-OCRv6_rec_medium.onnx"),
                                       QStringLiteral("ppocrv6_dict.txt")},
            "Medium acquisition must download exactly its three role files");

    downloads.clear();
    assets.setModelType(ScreenshotOcrModelType::ExtraSmall);
    processEventsFor(100);
    require(downloads.isEmpty(),
            "changing the OCR model selection must not start acquisition without demand");
    assets.prepare();
    require(waitUntil([&]() { return readyCount == 2; }, 5'000),
            "the Extra Small model fixture should be acquired");
    require(resolved.modelType == ScreenshotOcrModelType::ExtraSmall &&
                resolved.modelId == QStringLiteral("ppocrv6-tiny-cd609a1") &&
                resolved.detectorModelPath.endsWith(QStringLiteral("PP-OCRv6_det_tiny.onnx")) &&
                resolved.recognizerModelPath.endsWith(QStringLiteral("PP-OCRv6_rec_tiny.onnx")) &&
                resolved.dictionaryPath.endsWith(QStringLiteral("ppocrv6_tiny_dict.txt")),
            "Extra Small must resolve its own detector, recognizer, and dictionary");
    require(
        QDir(cache.path()).exists(QStringLiteral("models/ppocrv6-medium-f5063c6/.complete.json")) &&
            QDir(cache.path()).exists(QStringLiteral("models/ppocrv6-tiny-cd609a1/.complete.json")),
        "switching models must retain every manifest-approved cached model");

    downloads.clear();
    assets.setModelType(ScreenshotOcrModelType::Small);
    assets.prepare();
    require(waitUntil([&]() { return readyCount == 3; }, 5'000),
            "the bundled Small model should be selected");
    require(resolved.modelType == ScreenshotOcrModelType::Small && resolved.offline &&
                downloads.isEmpty(),
            "Small must continue to resolve entirely offline without downloading");
    struct ExpectedModel {
        ScreenshotOcrModelType type;
        const char* value;
        const char* id;
        const char* detector;
        const char* recognizer;
        const char* dictionary;
    };
    const ExpectedModel versionedModels[] = {
        {ScreenshotOcrModelType::SmallV5, "small_v5", "ppocrv5-small-7b2a75a",
         "ch_PP-OCRv5_det_mobile.onnx", "ch_PP-OCRv5_rec_mobile.onnx", "ppocrv5_dict.txt"},
        {ScreenshotOcrModelType::MediumV5, "medium_v5", "ppocrv5-medium-7b2a75a",
         "ch_PP-OCRv5_det_server.onnx", "ch_PP-OCRv5_rec_server.onnx", "ppocrv5_dict.txt"},
        {ScreenshotOcrModelType::SmallV4, "small_v4", "ppocrv4-small-7b2a75a",
         "ch_PP-OCRv4_det_mobile.onnx", "ch_PP-OCRv4_rec_mobile.onnx", "ppocr_keys_v1.txt"},
        {ScreenshotOcrModelType::MediumV4, "medium_v4", "ppocrv4-medium-7b2a75a",
         "ch_PP-OCRv4_det_server.onnx", "ch_PP-OCRv4_rec_server.onnx", "ppocr_keys_v1.txt"},
    };
    for (const auto& expected : versionedModels) {
        downloads.clear();
        assets.setModelType(expected.type);
        const int previous = readyCount;
        assets.prepare();
        require(waitUntil([&]() { return readyCount == previous + 1; }, 5'000),
                "versioned model acquisition completes");
        const QString detector = QString::fromLatin1(expected.detector);
        const QString recognizer = QString::fromLatin1(expected.recognizer);
        const QString dictionary = QString::fromLatin1(expected.dictionary);
        require(resolved.modelType == expected.type &&
                    resolved.modelId == QString::fromLatin1(expected.id) &&
                    resolved.detectorModelPath.endsWith(detector) &&
                    resolved.recognizerModelPath.endsWith(recognizer) &&
                    resolved.dictionaryPath.endsWith(dictionary) &&
                    downloads == QSet<QString>{detector, recognizer, dictionary},
                "versioned model downloads and resolves exactly its matching role files");
        require(screenshotOcrModelTypeValue(expected.type) == QString::fromLatin1(expected.value) &&
                    screenshotOcrModelTypeFromValue(QString::fromLatin1(expected.value)) ==
                        expected.type,
                "versioned model identifiers round trip");
        downloads.clear();
        assets.setModelType(ScreenshotOcrModelType::Small);
        assets.prepare();
        require(waitUntil([&]() { return readyCount == previous + 2; }, 5'000),
                "switching to bundled V6 succeeds");
        assets.setModelType(expected.type);
        assets.prepare();
        require(waitUntil([&]() { return readyCount == previous + 3; }, 5'000) &&
                    downloads.isEmpty(),
                "switching back reuses the versioned model cache without downloading");
    }
}

void selectedModelFailureNeverFallsBackToSmallAndCanRetry() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(),
            "temporary OCR failure roots should be available");
    writeAssetManifest(offline.path(), true);
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.bundledRuntimeRoot = offline.path();
    options.cacheRoot = cache.path();
    options.modelType = ScreenshotOcrModelType::MediumV4;
    bool allowDownload = false;
    int downloadAttempts = 0;
    options.downloadOverride = [&](const QString&, const QString& destination, QString* error) {
        ++downloadAttempts;
        if (!allowDownload) {
            *error = QStringLiteral("selected model unavailable");
            return false;
        }
        return writeDownloadedModelFixture(destination, error);
    };
    ScreenshotOcrAssets assets(options);
    bool ready = false;
    int failureCount = 0;
    ScreenshotOcrResolvedAssets resolved;
    QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                     [&](const ScreenshotOcrResolvedAssets& result) {
                         resolved = result;
                         ready = true;
                     });
    QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets, [&](const QString&) {
        ++failureCount;
        if (failureCount == 1) {
            allowDownload = true;
            assets.prepare();
        }
    });
    assets.prepare();
    require(waitUntil([&]() { return ready; }, 5'000),
            "a request arriving from the failure callback should retry model acquisition");
    require(failureCount == 1 && resolved.modelType == ScreenshotOcrModelType::MediumV4 &&
                !resolved.offline && downloadAttempts == 4,
            "a failed Medium acquisition must retry safely without falling back to bundled Small");
}

void invalidSchemaTwoManifestsAreRejectedBeforeDownloading() {
    const auto rejectMutation = [](const std::function<void(QJsonObject*)>& mutate) {
        QTemporaryDir offline;
        QTemporaryDir cache;
        require(offline.isValid() && cache.isValid(),
                "temporary invalid-manifest roots should be available");
        writeAssetManifest(offline.path(), false);
        const QString manifestPath =
            QDir(offline.path()).filePath(QStringLiteral("asset-manifest.json"));
        QFile input(manifestPath);
        require(input.open(QIODevice::ReadOnly), "valid fixture manifest should be readable");
        QJsonObject manifest = QJsonDocument::fromJson(input.readAll()).object();
        input.close();
        mutate(&manifest);
        writeFixture(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        int downloads = 0;
        bool failed = false;
        ScreenshotOcrAssets::Options options;
        options.offlineRoot = offline.path();
        options.bundledRuntimeRoot = offline.path();
        options.cacheRoot = cache.path();
        options.downloadOverride = [&](const QString&, const QString&, QString*) {
            ++downloads;
            return false;
        };
        ScreenshotOcrAssets assets(options);
        QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets,
                         [&](const QString&) { failed = true; });
        assets.prepare();
        require(waitUntil([&]() { return failed; }, 5'000),
                "invalid schema-2 manifest should be rejected");
        require(downloads == 0, "an invalid trusted manifest must never initiate downloads");
    };

    rejectMutation([](QJsonObject* manifest) { manifest->insert(QStringLiteral("schema"), 1); });
    rejectMutation([](QJsonObject* manifest) {
        manifest->insert(QStringLiteral("default_model"), QStringLiteral("medium"));
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        models.removeAt(2);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject medium = models.at(2).toObject();
        medium.insert(QStringLiteral("type"), QStringLiteral("small"));
        models.replace(2, medium);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject medium = models.at(2).toObject();
        medium.insert(QStringLiteral("id"), QStringLiteral("ppocrv6-tiny-cd609a1"));
        models.replace(2, medium);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject medium = models.at(2).toObject();
        medium.insert(QStringLiteral("type"), QStringLiteral("large"));
        models.replace(2, medium);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        smallModel.insert(QStringLiteral("detector"), QStringLiteral("missing.onnx"));
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        QJsonArray files = smallModel.value(QStringLiteral("files")).toArray();
        QJsonObject recognizer = files.at(1).toObject();
        recognizer.insert(QStringLiteral("name"),
                          files.at(0).toObject().value(QStringLiteral("name")));
        files.replace(1, recognizer);
        smallModel.insert(QStringLiteral("files"), files);
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        QJsonArray files = smallModel.value(QStringLiteral("files")).toArray();
        QJsonObject detector = files.at(0).toObject();
        detector.insert(QStringLiteral("size"), 0);
        files.replace(0, detector);
        smallModel.insert(QStringLiteral("files"), files);
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        QJsonArray files = smallModel.value(QStringLiteral("files")).toArray();
        QJsonObject detector = files.at(0).toObject();
        detector.insert(QStringLiteral("sha256"), QStringLiteral("not-a-sha256"));
        files.replace(0, detector);
        smallModel.insert(QStringLiteral("files"), files);
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        QJsonArray files = smallModel.value(QStringLiteral("files")).toArray();
        QJsonObject detector = files.at(0).toObject();
        detector.insert(QStringLiteral("url"), QStringLiteral("http://example.invalid/model"));
        files.replace(0, detector);
        smallModel.insert(QStringLiteral("files"), files);
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
}

void modelSelectionDuringAcquisitionIsLastSelectionWins() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(), "temporary OCR race roots should be available");
    writeAssetManifest(offline.path(), true);
    QSemaphore mediumStarted;
    QSemaphore releaseMedium;
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.bundledRuntimeRoot = offline.path();
    options.cacheRoot = cache.path();
    options.modelType = ScreenshotOcrModelType::MediumV5;
    options.downloadOverride = [&](const QString&, const QString& destination, QString* error) {
        if (QFileInfo(destination).fileName() == QStringLiteral("ch_PP-OCRv5_det_server.onnx")) {
            mediumStarted.release();
            if (!releaseMedium.tryAcquire(1, 5'000)) {
                *error = QStringLiteral("timed out waiting for model switch");
                return false;
            }
        }
        return writeDownloadedModelFixture(destination, error);
    };
    ScreenshotOcrAssets assets(options);
    ScreenshotOcrResolvedAssets resolved;
    int readyCount = 0;
    QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                     [&](const ScreenshotOcrResolvedAssets& result) {
                         resolved = result;
                         ++readyCount;
                     });
    assets.prepare();
    require(waitUntil([&]() { return mediumStarted.available() > 0; }, 5'000),
            "Medium acquisition should reach the controlled download");
    require(mediumStarted.tryAcquire(), "the controlled Medium download should be observed");
    assets.setModelType(ScreenshotOcrModelType::SmallV4);
    assets.prepare();
    releaseMedium.release();
    require(waitUntil([&]() { return readyCount == 1; }, 10'000),
            "the replacement Small V4 acquisition should complete");
    require(resolved.modelType == ScreenshotOcrModelType::SmallV4 &&
                QDir(cache.path())
                    .exists(QStringLiteral("models/ppocrv5-medium-7b2a75a/.complete.json")),
            "a stale Medium V5 download may remain cached but must never become active");
}

// Destruction must interrupt the worker even while it sits in the
// cross-process cache-lock wait, which no event loop is pumping: without the
// interruption plumbing the destructor blocks the full 120 s lock timeout.
void assetDestructionInterruptsTheCacheLockWait() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(),
            "temporary OCR asset lock roots should be available");
    writeAssetManifest(offline.path(), false);
#ifdef Q_OS_WIN
    const QByteArray digest =
        QCryptographicHash::hash(QFileInfo(cache.path()).absoluteFilePath().toLower().toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex();
    const QString lockName =
        QStringLiteral("Local\\SnowShotOcrAssets-%1").arg(QString::fromLatin1(digest.left(32)));
    HANDLE lock = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(lockName.utf16()));
    require(lock != nullptr, "the test must be able to create the OCR cache lock");
    require(WaitForSingleObject(lock, 2'000) == WAIT_OBJECT_0,
            "the test must own the OCR cache lock before acquisition starts");
#else
    QLockFile lock(QDir(cache.path()).filePath(QStringLiteral(".assets.lock")));
    require(lock.tryLock(), "the test must own the OCR cache lock");
#endif
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.bundledRuntimeRoot = offline.path();
    options.cacheRoot = cache.path();
    int downloads = 0;
    options.downloadOverride = [&](const QString&, const QString&, QString*) {
        ++downloads;
        return false;
    };
    ScreenshotOcrAssets* assets = new ScreenshotOcrAssets(options);
    assets->prepare();
    processEventsFor(500);
    QElapsedTimer shutdown;
    shutdown.start();
    delete assets;
    const qint64 elapsedMs = shutdown.elapsed();
#ifdef Q_OS_WIN
    ReleaseMutex(lock);
    CloseHandle(lock);
#else
    lock.unlock();
#endif
    require(elapsedMs < 15'000,
            "destroying ScreenshotOcrAssets during the cache-lock wait must not block on the "
            "lock timeout");
    require(downloads == 0, "the held cache lock must gate every OCR asset download");
}

void concurrentAcquisitionAndInterruptedDownload() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    writeAssetManifest(offline.path(), true);
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.bundledRuntimeRoot = offline.path();
    options.cacheRoot = cache.path();
    options.modelType = ScreenshotOcrModelType::Medium;
    std::atomic<int> downloads = 0;
    options.downloadOverride = [&](const QString&, const QString& destination, QString* error) {
        ++downloads;
        return writeDownloadedModelFixture(destination, error);
    };
    {
        ScreenshotOcrAssets first(options), second(options);
        int ready = 0;
        for (auto* assets : {&first, &second}) {
            QObject::connect(assets, &ScreenshotOcrAssets::ready, assets,
                             [&](const ScreenshotOcrResolvedAssets&) { ++ready; });
            assets->prepare();
        }
        require(waitUntil([&] { return ready == 2; }, 5'000),
                "concurrent asset managers must both acquire the selected model");
        require(downloads == 3, "the second cache owner must reuse the first verified download");
    }
    options.modelType = ScreenshotOcrModelType::ExtraSmall;
    QSemaphore started;
    options.downloadOverride = [&](const QString&, const QString& destination, QString* error) {
        writeFixture(destination, "partial");
        started.release();
        while (!QThread::currentThread()->isInterruptionRequested())
            QThread::msleep(5);
        *error = QStringLiteral("interrupted");
        return false;
    };
    auto assets = std::make_unique<ScreenshotOcrAssets>(options);
    assets->prepare();
    require(waitUntil([&] { return started.available() > 0; }, 5'000),
            "the interrupted download must have started");
    QElapsedTimer shutdown;
    shutdown.start();
    assets.reset();
    require(shutdown.elapsed() < 2'000, "download cancellation must allow bounded shutdown");
    require(
        !QDir(cache.path()).exists(QStringLiteral("models/ppocrv6-tiny-cd609a1/.complete.json")),
        "an interrupted model must never become active");
    require(QDir(QDir(cache.path()).filePath(QStringLiteral(".staging")))
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty(),
            "interrupted downloads must remove partial staging files");
}

void runtimeArchivesExtractThroughUnicodeCachePaths() {
#ifndef Q_OS_MACOS
    // The macOS fixture manifest switches to the bundled runtime schema, which
    // never reaches archive extraction; the downloaded runtime path below is
    // the Windows delivery.
    QTemporaryDir offline;
    QTemporaryDir cache(QDir::tempPath() +
                        QStringLiteral("/snow OCR caf\u00e9 缓存-\U0001F9CA-XXXXXX"));
    require(offline.isValid() && cache.isValid(),
            "temporary OCR extraction roots should be available");
    writeAssetManifest(offline.path(), false);
    const QByteArray archive = buildRuntimeArchiveBytes();
    const QString manifestPath =
        QDir(offline.path()).filePath(QStringLiteral("asset-manifest.json"));
    QFile input(manifestPath);
    require(input.open(QIODevice::ReadOnly), "fixture manifest should be readable");
    QJsonObject manifest = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    QJsonObject runtime = manifest.value(QStringLiteral("runtime")).toObject();
    QJsonObject archiveEntry = runtime.value(QStringLiteral("archive")).toObject();
    archiveEntry.insert(QStringLiteral("size"), archive.size());
    archiveEntry.insert(
        QStringLiteral("sha256"),
        QString::fromLatin1(QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex()));
    runtime.insert(QStringLiteral("archive"), archiveEntry);
    manifest.insert(QStringLiteral("runtime"), runtime);
    writeFixture(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact));

    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.bundledRuntimeRoot = offline.path();
    options.cacheRoot = cache.path();
    options.downloadOverride = [&](const QString& url, const QString& destination, QString* error) {
        if (url == QStringLiteral("https://example.invalid/runtime")) {
            QFile archiveDestination(destination);
            if (!archiveDestination.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                archiveDestination.write(archive) != archive.size()) {
                *error = QStringLiteral("fixture runtime archive write failed");
                return false;
            }
            return true;
        }
        return writeDownloadedModelFixture(destination, error);
    };
    ScreenshotOcrAssets assets(options);
    bool ready = false;
    bool failed = false;
    QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                     [&](const ScreenshotOcrResolvedAssets& result) { ready = result.valid(); });
    QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets,
                     [&](const QString&) { failed = true; });
    assets.prepare();
    require(waitUntil([&]() { return ready || failed; }, 5'000),
            "OCR runtime extraction should complete through a unicode cache path");
    require(ready && !failed,
            "runtime archives must extract from directories outside the ANSI code page");
#endif
}

void macosBundledRuntimeTests() {
#ifdef Q_OS_MACOS
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/snow OCR 空间-XXXXXX"));
    require(root.isValid(), "Unicode runtime paths must be available");
    QTemporaryDir cache;
    const auto rejects = [&](const std::function<void(QJsonObject&)>& mutate) {
        writeAssetManifest(root.path(), true);
        const QString manifestPath =
            QDir(root.path()).filePath(QStringLiteral("asset-manifest.json"));
        QFile input(manifestPath);
        require(input.open(QIODevice::ReadOnly), "fixture manifest must open");
        auto manifest = QJsonDocument::fromJson(input.readAll()).object();
        input.close();
        mutate(manifest);
        writeFixture(manifestPath, QJsonDocument(manifest).toJson());
        ScreenshotOcrAssets::Options options;
        options.offlineRoot = root.path();
        options.bundledRuntimeRoot = root.path();
        options.cacheRoot = cache.path();
        int downloads = 0;
        options.downloadOverride = [&](const QString&, const QString&, QString*) {
            ++downloads;
            return false;
        };
        ScreenshotOcrAssets assets(options);
        bool failed = false;
        QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets, [&](const QString& error) {
            failed = error == QStringLiteral("bundled_runtime_invalid");
        });
        assets.prepare();
        require(waitUntil([&] { return failed; }, 5'000), "invalid bundled code must fail closed");
        require(downloads == 0, "bundled runtime failure must never download executable code");
        QFile::setPermissions(QDir(root.path()).filePath(kWorkerName), QFileDevice::ReadOwner |
                                                                           QFileDevice::WriteOwner |
                                                                           QFileDevice::ExeOwner);
    };
    for (const auto& field : {QStringLiteral("platform"), QStringLiteral("protocol"),
                              QStringLiteral("delivery"), QStringLiteral("executable")}) {
        rejects([&](QJsonObject& manifest) {
            auto runtime = manifest.value(QStringLiteral("runtime")).toObject();
            runtime.insert(field, QStringLiteral("invalid"));
            manifest.insert(QStringLiteral("runtime"), runtime);
        });
    }
    rejects([&](QJsonObject&) { QFile::remove(QDir(root.path()).filePath(kWorkerName)); });
    rejects([&](QJsonObject&) {
        QFile::setPermissions(QDir(root.path()).filePath(kWorkerName), QFileDevice::ReadOwner);
    });
    rejects([&](QJsonObject&) {
        writeFixture(QDir(root.path()).filePath(QStringLiteral("libonnxruntime.dylib")), "corrupt");
    });
    rejects([&](QJsonObject& manifest) {
        const QByteArray intel = QByteArray::fromHex("cffaedfe07000001") + QByteArray(32, '\0');
        writeFixture(QDir(root.path()).filePath(QStringLiteral("libonnxruntime.dylib")), intel);
        auto runtime = manifest.value(QStringLiteral("runtime")).toObject();
        auto files = runtime.value(QStringLiteral("files")).toArray();
        files.replace(1, assetFile(QStringLiteral("libonnxruntime.dylib"), intel));
        runtime.insert(QStringLiteral("files"), files);
        manifest.insert(QStringLiteral("runtime"), runtime);
    });

    // A read-only bundle must recognize offline without writing state alongside code.
    writeAssetManifest(root.path(), true);
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = root.path();
    options.bundledRuntimeRoot = root.path();
    options.cacheRoot = cache.path();
    const auto permissions = QFileInfo(root.path()).permissions();
    require(QFile::setPermissions(root.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner),
            "the fixture bundle must become read-only");
    {
        ScreenshotOcrAssets assets(options);
        bool ready = false;
        QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                         [&](const ScreenshotOcrResolvedAssets& result) {
                             ready =
                                 result.offline && result.stateDirectory.startsWith(cache.path());
                         });
        assets.prepare();
        require(waitUntil([&] { return ready; }, 5'000),
                "read-only Unicode bundles must work offline");
    }
    QFile::setPermissions(root.path(), permissions);

    // Signed bundles expose resources through MacOS/assets. Resolve the runtime
    // lexically before the operating system follows that directory symlink.
    QTemporaryDir linkedBundle;
    const QDir app(linkedBundle.path());
    const QString runtime = app.filePath(QStringLiteral("Contents/MacOS"));
    const QString resources = app.filePath(QStringLiteral("Contents/Resources/assets"));
    const QString payload = QDir(resources).filePath(QStringLiteral("ocr"));
    writeAssetManifest(payload, true);
    require(QDir().mkpath(runtime), "the linked bundle runtime directory must exist");
    for (const auto& name : {kWorkerName, QStringLiteral("libonnxruntime.dylib")})
        require(QFile::rename(QDir(payload).filePath(name), QDir(runtime).filePath(name)),
                "the runtime must reside beside the app executable");
    require(QFile::link(resources, QDir(runtime).filePath(QStringLiteral("assets"))),
            "the resource link must be created");
    options.offlineRoot = QDir(runtime).filePath(QStringLiteral("assets/ocr"));
    options.bundledRuntimeRoot.clear();
    {
        ScreenshotOcrAssets assets(options);
        bool ready = false;
        QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                         [&](const ScreenshotOcrResolvedAssets& result) {
                             ready = result.offline && result.runtimeDirectory == runtime;
                         });
        assets.prepare();
        require(waitUntil([&] { return ready; }, 5000),
                "resource links must not redirect runtime lookup into Resources");
    }

    // A separate process owns the lock; terminating it must make the lock recoverable.
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--cache-lock-child"), cache.path()});
    require(child.waitForStarted() && child.waitForReadyRead() &&
                child.readAllStandardOutput().trimmed() == "locked",
            "the child must own the cache lock");
    QLockFile contender(QDir(cache.path()).filePath(QStringLiteral(".assets.lock")));
    contender.setStaleLockTime(0);
    require(!contender.tryLock(), "concurrent processes must not share the cache lock");
    child.kill();
    require(child.waitForFinished(), "the lock owner must terminate");
    require(contender.tryLock(1000), "cache locks must recover after the owner crashes");
#endif
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--cache-lock-child"))) {
        QLockFile lock(
            QDir(application.arguments().last()).filePath(QStringLiteral(".assets.lock")));
        require(lock.tryLock(), "the lock child must acquire the cache");
        std::cout << "locked" << std::endl;
        return application.exec();
    }
    validOfflineAssetsAreSelectedWithoutNetwork();
    incompleteOfflineAssetsFallBackToOnlineAcquisition();
    everyModelMapsToItsOwnFilesAndCachesAreRetained();
    selectedModelFailureNeverFallsBackToSmallAndCanRetry();
    invalidSchemaTwoManifestsAreRejectedBeforeDownloading();
    modelSelectionDuringAcquisitionIsLastSelectionWins();
    assetDestructionInterruptsTheCacheLockWait();
    concurrentAcquisitionAndInterruptedDownload();
    runtimeArchivesExtractThroughUnicodeCachePaths();
    macosBundledRuntimeTests();
    return 0;
}
