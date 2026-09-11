#include "snow_shot/presentation/screenshotocrassets.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void writeFile(const QString& path, const QByteArray& bytes) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "fixture parent must be writable");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "fixture file must be writable");
}

QJsonObject fileDescriptor(const QString& name, const QByteArray& bytes) {
    return {
        {QStringLiteral("name"), name},
        {QStringLiteral("size"), bytes.size()},
        {QStringLiteral("sha256"),
         QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
}

struct Fixture {
    QTemporaryDir root;
    QJsonObject manifest;
    QString resources;
    QString runtime;
    QString library;
    QString cache;
    QString modelId;
    QHash<QString, QByteArray> payloads;
    std::atomic<int> downloads{0};

    Fixture() {
        require(root.isValid(), "fixture directory must be available");
        resources = QDir(root.path()).filePath(QStringLiteral("Contents/Resources/assets/ocr"));
        runtime = QDir(root.path()).filePath(QStringLiteral("Contents/MacOS"));
        library =
            QDir(root.path()).filePath(QStringLiteral("Contents/Frameworks/libonnxruntime.dylib"));
        cache = QDir(root.path()).filePath(QStringLiteral("cache"));
        QFile source(QStringLiteral(SNOW_TEST_OCR_MANIFEST));
        require(source.open(QIODevice::ReadOnly), "pinned manifest must be readable");
        manifest = QJsonDocument::fromJson(source.readAll()).object();
        manifest.insert(QStringLiteral("schema"), 3);
        const QByteArray helper("#!/bin/sh\nexit 0\n");
        const QByteArray nativeLibrary("native library fixture");
        const QString process = QDir(runtime).filePath(QStringLiteral("snow-ocr-process"));
        writeFile(process, helper);
        require(
            QFile::setPermissions(process, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "bundled helper must be executable");
        writeFile(library, nativeLibrary);
#if defined(Q_PROCESSOR_ARM_64)
        const QString platform = QStringLiteral("macos-arm64");
#else
        const QString platform = QStringLiteral("macos-x64");
#endif
        manifest.insert(
            QStringLiteral("runtime"),
            QJsonObject{{QStringLiteral("version"), QStringLiteral("1.0.6")},
                        {QStringLiteral("platform"), platform},
                        {QStringLiteral("bundled"), true},
                        {QStringLiteral("files"),
                         QJsonArray{fileDescriptor(QStringLiteral("snow-ocr-process"), helper)}},
                        {QStringLiteral("library"),
                         fileDescriptor(QStringLiteral("libonnxruntime.dylib"), nativeLibrary)}});
        QJsonArray models;
        for (const auto& value : manifest.value(QStringLiteral("models")).toArray()) {
            auto model = value.toObject();
            QJsonArray files;
            for (const auto& entry : model.value(QStringLiteral("files")).toArray()) {
                auto file = entry.toObject();
                const QString name = file.value(QStringLiteral("name")).toString();
                const QByteArray bytes = name.toUtf8();
                const QString url = file.value(QStringLiteral("url")).toString();
                payloads.insert(url, bytes);
                file = fileDescriptor(name, bytes);
                file.insert(QStringLiteral("url"), url);
                files.append(file);
            }
            model.insert(QStringLiteral("files"), files);
            if (model.value(QStringLiteral("type")) == QStringLiteral("small"))
                modelId = model.value(QStringLiteral("id")).toString();
            models.append(model);
        }
        manifest.insert(QStringLiteral("models"), models);
        saveManifest();
    }

    void saveManifest() {
        writeFile(QDir(resources).filePath(QStringLiteral("asset-manifest.json")),
                  QJsonDocument(manifest).toJson());
    }

    ScreenshotOcrAssets::Options options() {
        ScreenshotOcrAssets::Options result;
        result.offlineRoot = resources;
        result.cacheRoot = cache;
        result.bundledRuntimeDirectory = runtime;
        result.downloadOverride = [this](const QString& url, const QString& path, QString*) {
            require(payloads.contains(url),
                    "bundled native runtime must never download an archive");
            ++downloads;
            writeFile(path, payloads.value(url));
            return true;
        };
        return result;
    }
};

bool prepare(ScreenshotOcrAssets& manager, ScreenshotOcrResolvedAssets* assets = nullptr) {
    QEventLoop loop;
    bool ready = false;
    QObject::connect(&manager, &ScreenshotOcrAssets::ready, &loop,
                     [&](const ScreenshotOcrResolvedAssets& result) {
                         ready = true;
                         if (assets != nullptr)
                             *assets = result;
                         loop.quit();
                     });
    QObject::connect(&manager, &ScreenshotOcrAssets::failed, &loop,
                     [&](const QString&) { loop.quit(); });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    manager.prepare();
    loop.exec();
    return ready;
}

void managedModelsDownloadVerifyAndReuse() {
    Fixture fixture;
    ScreenshotOcrResolvedAssets assets;
    {
        ScreenshotOcrAssets manager(fixture.options());
        require(prepare(manager, &assets), "native runtime and downloaded model should resolve");
    }
    require(fixture.downloads == 3 && assets.valid() && !assets.offline,
            "a cold cache should download exactly three model files");
    require(assets.processPath ==
                    QDir(fixture.runtime).filePath(QStringLiteral("snow-ocr-process")) &&
                QDir::cleanPath(assets.onnxRuntimePath) == fixture.library,
            "native helper and ONNX library must resolve inside the trusted bundle");
    {
        ScreenshotOcrAssets manager(fixture.options());
        require(prepare(manager), "verified cache should remain ready");
    }
    require(fixture.downloads == 3, "valid cached models must not download again");
    writeFile(assets.detectorModelPath, QByteArray("corrupt"));
    {
        ScreenshotOcrAssets manager(fixture.options());
        require(prepare(manager), "a corrupt cached model must be repaired");
    }
    require(fixture.downloads == 6, "repair must replace the complete verified model component");
    const QString offlineModels =
        QDir(fixture.resources).filePath(QStringLiteral("models/%1").arg(fixture.modelId));
    require(QDir().mkpath(QFileInfo(offlineModels).absolutePath()) &&
                QDir().rename(QFileInfo(assets.detectorModelPath).absolutePath(), offlineModels),
            "verified model fixture must be movable into offline resources");
    {
        ScreenshotOcrAssets manager(fixture.options());
        require(prepare(manager, &assets) && assets.offline,
                "bundled models must work without a network download");
    }
    require(fixture.downloads == 6, "offline model selection must not download");
}

void rejectsUntrustedBundleAndModels() {
    for (int scenario = 0; scenario < 6; ++scenario) {
        Fixture fixture;
        auto runtime = fixture.manifest.value(QStringLiteral("runtime")).toObject();
        if (scenario == 0)
            writeFile(fixture.library, QByteArray("changed library"));
        else if (scenario == 1)
            writeFile(QDir(fixture.runtime).filePath(QStringLiteral("snow-ocr-process")),
                      QByteArray("changed helper"));
        else if (scenario == 2)
            require(QFile::setPermissions(
                        QDir(fixture.runtime).filePath(QStringLiteral("snow-ocr-process")),
                        QFile::ReadOwner | QFile::WriteOwner),
                    "permission fixture must work");
        else if (scenario == 3)
            runtime.insert(QStringLiteral("platform"), QStringLiteral("windows-x64"));
        else if (scenario == 4) {
            auto library = runtime.value(QStringLiteral("library")).toObject();
            library.insert(QStringLiteral("name"), QStringLiteral("../libonnxruntime.dylib"));
            runtime.insert(QStringLiteral("library"), library);
        } else {
            auto files = runtime.value(QStringLiteral("files")).toArray();
            files.append(fileDescriptor(QStringLiteral("unexpected"), QByteArray("extra")));
            runtime.insert(QStringLiteral("files"), files);
        }
        fixture.manifest.insert(QStringLiteral("runtime"), runtime);
        fixture.saveManifest();
        ScreenshotOcrAssets manager(fixture.options());
        require(!prepare(manager) && fixture.downloads == 0,
                "invalid bundled code must fail before any network acquisition");
    }
    Fixture fixture;
    auto options = fixture.options();
    options.downloadOverride = [](const QString&, const QString& path, QString*) {
        writeFile(path, QByteArray("wrong model bytes"));
        return true;
    };
    ScreenshotOcrAssets manager(options);
    require(!prepare(manager), "downloaded model digest mismatch must fail closed");
    require(
        !QFileInfo(QDir(fixture.cache)
                       .filePath(QStringLiteral("models/%1/.complete.json").arg(fixture.modelId)))
             .exists(),
        "corrupt downloads must not activate a model");
}

void concurrentAcquisitionUsesOneCache() {
    Fixture fixture;
    ScreenshotOcrAssets first(fixture.options());
    ScreenshotOcrAssets second(fixture.options());
    QEventLoop loop;
    int ready = 0;
    const auto onReady = [&]() {
        if (++ready == 2)
            loop.quit();
    };
    QObject::connect(&first, &ScreenshotOcrAssets::ready, &loop, onReady);
    QObject::connect(&second, &ScreenshotOcrAssets::ready, &loop, onReady);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    first.prepare();
    second.prepare();
    loop.exec();
    require(ready == 2 && fixture.downloads == 3,
            "concurrent managers must serialize staging and reuse the verified model");
}

void verifyDefaultBundleOptions() {
    // Exercise both the delegating default constructor and the options used by
    // application/controller consumers, which only supply a writable cache.
    for (int mode = 0; mode < 2; ++mode) {
        ScreenshotOcrRecognitionService::Options options;
        options.cacheRoot =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../cache"));
        auto service = mode == 0 ? std::make_unique<ScreenshotOcrRecognitionService>()
                                 : std::make_unique<ScreenshotOcrRecognitionService>(options);
        auto* manager = service->findChild<ScreenshotOcrAssets*>();
        require(manager != nullptr, "default service must use its managed asset provider");
        ScreenshotOcrResolvedAssets assets;
        require(prepare(*manager, &assets) && assets.offline,
                "default service options must resolve offline assets from Contents/Resources");
        const QDir executableDirectory(QCoreApplication::applicationDirPath());
        require(QFileInfo(assets.processPath).canonicalFilePath() ==
                    QFileInfo(executableDirectory.filePath(QStringLiteral("snow-ocr-process")))
                        .canonicalFilePath(),
                "default service must resolve the helper alongside the application executable");
        require(QFileInfo(assets.onnxRuntimePath).canonicalFilePath() ==
                    QFileInfo(executableDirectory.filePath(
                                  QStringLiteral("../Frameworks/libonnxruntime.dylib")))
                        .canonicalFilePath(),
                "default service must resolve ONNX from Contents/Frameworks");
    }
}

void defaultServiceFindsInstalledBundle() {
    Fixture fixture;
    const QString offlineModels =
        QDir(fixture.resources).filePath(QStringLiteral("models/%1").arg(fixture.modelId));
    for (const auto& value : fixture.manifest.value(QStringLiteral("models")).toArray()) {
        const auto model = value.toObject();
        if (model.value(QStringLiteral("id")).toString() != fixture.modelId)
            continue;
        for (const auto& entry : model.value(QStringLiteral("files")).toArray()) {
            const auto file = entry.toObject();
            writeFile(QDir(offlineModels).filePath(file.value(QStringLiteral("name")).toString()),
                      fixture.payloads.value(file.value(QStringLiteral("url")).toString()));
        }
    }
    writeFile(QDir(offlineModels).filePath(QStringLiteral(".complete.json")),
              QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1},
                                        {QStringLiteral("component"), fixture.modelId}})
                  .toJson());
    // A Windows-style path is deliberately invalid, so this fails if native
    // defaults regress to executableDir/assets/ocr even when that path exists.
    writeFile(QDir(fixture.runtime).filePath(QStringLiteral("assets/ocr/asset-manifest.json")),
              QByteArray("invalid obsolete bundle location"));
    const QString executable =
        QDir(fixture.runtime).filePath(QStringLiteral("snow-shot-macos-ocr-tests"));
    require(QFile::copy(QCoreApplication::applicationFilePath(), executable),
            "test executable must be copied into the temporary application bundle");
    QProcess process;
    process.start(executable, {QStringLiteral("--verify-default-bundle")});
    require(process.waitForFinished(15000), "default-options bundle test must finish");
    const QByteArray errors = process.readAllStandardError();
    if (!errors.isEmpty())
        std::cerr << errors.constData();
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
            "default-options service must work from a native application bundle");
}

QString argument(const QString& key) {
    const QStringList args = QCoreApplication::arguments();
    const qsizetype index = args.indexOf(key);
    return index >= 0 && index + 1 < args.size() ? args.at(index + 1) : QString();
}

void recognizeImage() {
    const QString imagePath = argument(QStringLiteral("--recognize-image"));
    QImage image(imagePath);
    require(!image.isNull(), "OCR fixture image must load");
    ScreenshotOcrRecognitionService::Options options;
    options.offlineRoot = argument(QStringLiteral("--offline-root"));
    options.cacheRoot = argument(QStringLiteral("--cache-root"));
    options.bundledRuntimeDirectory = argument(QStringLiteral("--runtime-root"));
    options.modelType = screenshotOcrModelTypeFromValue(argument(QStringLiteral("--model")));
    ScreenshotOcrRecognitionService service(options);
    QObject receiver;
    QEventLoop loop;
    QString output;
    bool completed = false;
    ScreenshotOcrRequest request;
    request.image = image;
    request.canvasRect = QRectF(QPointF(), QSizeF(image.size()));
    service.recognize(request, &receiver, [&](ScreenshotOcrRecognitionResult result) {
        require(result.error.isEmpty(), qPrintable(result.error));
        require(result.presentation != nullptr, "recognition must return a presentation");
        for (const auto& line : result.presentation->lines)
            output += line.text + u'\n';
        completed = true;
        loop.quit();
    });
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    require(completed, "managed OCR recognition must complete");
    std::cout << output.toStdString() << '\n';
    require(!output.isEmpty(), "recognition must return text");
    const QStringList arguments = QCoreApplication::arguments();
    for (qsizetype index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments.at(index) == QStringLiteral("--expect"))
            require(output.contains(arguments.at(index + 1)),
                    "recognized text must contain each expected fixture string");
    }
    const QString expectedStatus = argument(QStringLiteral("--expect-status"));
    if (!expectedStatus.isEmpty()) {
        const auto phase = service.assetStatus().phase;
        require((expectedStatus == QStringLiteral("offline") &&
                 phase == ScreenshotOcrAssetPhase::ReadyOffline) ||
                    (expectedStatus == QStringLiteral("cached") &&
                     phase == ScreenshotOcrAssetPhase::ReadyCached),
                "actual acquisition status must match the requested test mode");
    }
}
} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    if (QCoreApplication::arguments().contains(QStringLiteral("--verify-default-bundle"))) {
        verifyDefaultBundleOptions();
        return EXIT_SUCCESS;
    }
    const QString fixturePath = argument(QStringLiteral("--write-fixture"));
    if (!fixturePath.isEmpty()) {
        QImage image(1000, 300, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(QFont(QStringLiteral("Arial"), 36));
        painter.drawText(QPoint(40, 90), QStringLiteral("Snow Shot 2026"));
        painter.setFont(QFont(QStringLiteral("PingFang SC"), 36));
        painter.drawText(QPoint(40, 200), QString::fromUtf8("中文识别测试"));
        painter.end();
        require(image.save(fixturePath), "OCR fixture image must save");
        return EXIT_SUCCESS;
    }
    if (!argument(QStringLiteral("--recognize-image")).isEmpty()) {
        recognizeImage();
        return EXIT_SUCCESS;
    }
    managedModelsDownloadVerifyAndReuse();
    rejectsUntrustedBundleAndModels();
    concurrentAcquisitionUsesOneCache();
    defaultServiceFindsInstalledBundle();
    std::cout << "macOS managed OCR asset tests passed\n";
    return EXIT_SUCCESS;
}
