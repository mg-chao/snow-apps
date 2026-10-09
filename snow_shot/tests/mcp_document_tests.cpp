#include "snow_shot/app/mcp/mcpdocumentservice.h"
#include "image_orientation_fixture.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/app/mcp/mcpapplicationservice.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "snowimageqtcodec.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QSet>
#include <QSaveFile>
#include <QJsonDocument>
#include <QSemaphore>
#include <QBuffer>
#include <QMimeData>
#include <QScopeGuard>
#include <QtEndian>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <utility>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace snow_shot::app::mcp;
void runMcpApplicationTests();
namespace {
std::optional<double> currentThreadCpuMilliseconds() {
#ifdef Q_OS_WIN
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
        return {};
    const auto ticks = [](FILETIME value) {
        return (static_cast<quint64>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#else
    return {};
#endif
}
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
class MemoryClipboard final : public McpDocumentService::Clipboard {
  public:
    bool automatic = true;
    bool reject = false;
    ScreenshotClipboardCommitFailure failure = ScreenshotClipboardCommitFailure::None;
    int imagePublications = 0;
    int mimePublications = 0;
    int cancellations = 0;
    QByteArray png;
    std::unique_ptr<QMimeData> mime;
    struct Publication {
        QPointer<QObject> receiver;
        QByteArray png;
        std::unique_ptr<QMimeData> mime;
        Completion completion;
        ScreenshotClipboardCommitFailure failure;
        bool canceled = false;
    };
    QList<std::shared_ptr<Publication>> pending;

    Cancel publishImage(QObject* receiver, ScreenshotClipboardPayload payload,
                        Completion completion) override {
        require(payload.isValid() && !QImage::fromData(payload.pngBytes(), "PNG").isNull(),
                "clipboard receives a real encoded image");
        return enqueue(receiver, payload.pngBytes(), {}, std::move(completion));
    }
    Cancel publishMimeData(QObject* receiver, std::unique_ptr<QMimeData> data,
                           Completion completion) override {
        require(data != nullptr, "clipboard owns the recognition MIME data");
        return enqueue(receiver, {}, std::move(data), std::move(completion));
    }
    void complete(const std::shared_ptr<Publication>& publication) {
        pending.removeOne(publication);
        if (publication->canceled || !publication->receiver)
            return;
        if (publication->failure == ScreenshotClipboardCommitFailure::None) {
            if (publication->mime) {
                mime = std::move(publication->mime);
                png.clear();
                ++mimePublications;
            } else {
                png = std::move(publication->png);
                mime.reset();
                ++imagePublications;
            }
        }
        publication->completion({publication->failure});
    }

  private:
    Cancel enqueue(QObject* receiver, QByteArray bytes, std::unique_ptr<QMimeData> data,
                   Completion completion) {
        if (reject)
            return {};
        auto publication = std::make_shared<Publication>(Publication{
            receiver, std::move(bytes), std::move(data), std::move(completion), failure});
        pending.append(publication);
        if (automatic)
            QTimer::singleShot(0, receiver, [this, publication] { complete(publication); });
        return [this, publication] {
            if (!std::exchange(publication->canceled, true))
                ++cancellations;
        };
    }
};

class FixtureOcr final : public ScreenshotOcrRecognitionPort {
  public:
    RequestToken recognize(ScreenshotOcrRequest request, QObject* receiver,
                           Completion completion) override {
        const auto token = ++m_next;
        QTimer::singleShot(
            100, receiver,
            [this, token, request = std::move(request), completion = std::move(completion)] {
                if (m_canceled.remove(token))
                    return;
                auto presentation = std::make_shared<ScreenshotOcrPresentation>();
                presentation->selection = request.canvasRect.toAlignedRect();
                ScreenshotOcrLine line;
                line.text = QStringLiteral("Fixture recognized text");
                line.confidence = 1;
                line.quad =
                    QPolygonF{QPointF(0, 0), QPointF(70, 0), QPointF(70, 20), QPointF(0, 20)};
                presentation->lines.append(line);
                presentation->prepareForRendering();
                completion({presentation});
            });
        return token;
    }
    void cancel(RequestToken token) override {
        m_canceled.insert(token);
    }
    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return true;
    }

  private:
    RequestToken m_next = 0;
    QSet<RequestToken> m_canceled;
};
class FixtureQr final : public ScreenshotQrRecognitionPort {
  public:
    RequestToken
    recognize(QImage, QObject* receiver, Completion completion,
              ScreenshotQrRecognitionMode = ScreenshotQrRecognitionMode::QrAndBarcode) override {
        const auto token = ++m_next;
        QTimer::singleShot(100, receiver, [this, token, completion = std::move(completion)] {
            if (!m_canceled.remove(token))
                completion({{QStringLiteral("fixture-qr-payload")}, {}, {}});
        });
        return token;
    }
    void cancel(RequestToken token) override {
        m_canceled.insert(token);
    }

  private:
    RequestToken m_next = 0;
    QSet<RequestToken> m_canceled;
};
int serve(QApplication& application, const QString& directory) {
    QImage image(80, 60, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    require(image.save(QDir(directory).filePath(QStringLiteral("source.png"))), "fixture image");
    FixtureOcr ocr;
    FixtureQr qr;
    const auto providerUrl = qEnvironmentVariable("SNOW_SHOT_MCP_FIXTURE_API");
    require(providerUrl.isEmpty() || providerUrl.startsWith(QStringLiteral("http://127.0.0.1:")),
            "fixture provider is loopback only");
    std::unique_ptr<SnowShotApiClient> api;
    if (!providerUrl.isEmpty()) {
        api = std::make_unique<SnowShotApiClient>(providerUrl);
        api->setUseSystemProxy(false);
    }
    McpJobRegistry registry;
    McpDocumentService::Ports ports;
    const auto clipboard = std::make_shared<MemoryClipboard>();
    ports.clipboard = clipboard;
    ports.jobs = &registry;
    ports.recognition = &ocr;
    ports.qrRecognition = &qr;
    ports.api = api.get();
    qint64 fixturePins = 0, fixturePresentations = 0, fixtureDetections = 0;
    std::atomic<qint64> fixtureRenders = 0, fixtureEncodes = 0;
    ports.workObserved = [&](const QString&, const QString& operation) {
        if (operation == QStringLiteral("render"))
            ++fixtureRenders;
        else if (operation == QStringLiteral("encode"))
            ++fixtureEncodes;
    };
    QHash<QString, QPointer<QTimer>> sourceTimers;
    ports.resolveSource = [&](const ScreenshotMcpRequest& request, auto completion, auto budget) {
        if (!budget(image.sizeInBytes())) {
            completion({}, QStringLiteral("resource_limit"));
            return;
        }
        McpDocumentService::Source source;
        source.image = image;
        source.metadata = {
            {QStringLiteral("kind"), request.params.value(QStringLiteral("source"))}};
        source.originalContent.text = request.params.value(QStringLiteral("text"))
                                          .toString(QStringLiteral("Fixture original text"));
        source.originalContent.html = request.params.value(QStringLiteral("html"))
                                          .toString(QStringLiteral("<b>Fixture original text</b>"));
        const auto key = QString::number(request.connectionId) + u':' + request.requestId;
        if (request.params.value(QStringLiteral("delay_seconds")).toDouble() > 0) {
            auto* timer = new QTimer(&application);
            timer->setSingleShot(true);
            sourceTimers.insert(key, timer);
            QObject::connect(timer, &QTimer::timeout, &application,
                             [&, timer, key, source = std::move(source),
                              completion = std::move(completion)]() mutable {
                                 sourceTimers.remove(key);
                                 timer->deleteLater();
                                 completion(std::move(source), {});
                             });
            timer->start(100);
        } else
            completion(std::move(source), {});
    };
    ports.cancelSource = [&](quint64 owner, const QString& requestId) {
        if (auto timer = sourceTimers.take(QString::number(owner) + u':' + requestId))
            delete timer;
    };
    ports.present = [&](McpDocumentService::Source source, auto completion) {
        require(!source.images.isEmpty() && !source.documentSession.isEmpty(),
                "fixture presentation receives actual editable source snapshot");
        ++fixturePresentations;
        completion(true);
        return true;
    };
    ports.pinDocument = [&](McpDocumentService::Source source, QImage background, auto completion) {
        require(!source.images.isEmpty() && !background.isNull(),
                "fixture pin receives source snapshot and independent raster");
        ++fixturePins;
        completion(true);
        return true;
    };
    ports.autoFilter = [&](QImage raster, auto completion) {
        ++fixtureDetections;
        const QRectF detected = QRectF(4, 4, 20, 12).intersected(QRectF(QPointF(), raster.size()));
        QTimer::singleShot(50, &application,
                           [completion = std::move(completion), detected]() mutable {
                               completion({{1, detected, QStringLiteral("text")}}, {});
                           });
    };
    McpDocumentService documents(std::move(ports));
    registry.setArtifactPublisher([&](quint64 owner, QByteArray bytes, const QString& mime) {
        return documents.storeArtifact(owner, std::move(bytes), mime);
    });
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    const auto suspended = shortcuts.suspendRegistrations();
    Q_UNUSED(suspended);
    namespace settings = snow_shot::presentation::settings;
    const auto settingsRegistry = settings::buildBuiltInSettingsRegistry();
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession settingsSession(settingsRegistry, backend);
    McpApplicationService::Ports applicationPorts;
    applicationPorts.storage = &snow_shot::storage::ApplicationStorage::instance();
    applicationPorts.settings = &settingsSession;
    applicationPorts.jobs = &registry;
    if (api) {
        applicationPorts.translation = &snow_shot::translation::TranslationService::forClient(
            *api, applicationPorts.storage->configuration(), QLocale(QLocale::English));
        applicationPorts.selectedText = [](auto completion) {
            completion(QStringLiteral("Fixture selected text"), {});
            return std::function<void()>{};
        };
    }
    applicationPorts.action = [](const QString&, const QJsonObject&) { return true; };
    applicationPorts.restartAllowed = [] { return true; };
    McpApplicationService applicationService(std::move(applicationPorts));
    ScreenshotMcpServer server(nullptr, directory);
    QTimer heartbeat;
    QElapsedTimer heartbeatClock;
    heartbeatClock.start();
    const auto initialCpu = currentThreadCpuMilliseconds();
    qint64 ticks = 0, latenessTotal = 0, latenessMaximum = 0, previousTick = 0;
    QObject::connect(&heartbeat, &QTimer::timeout, &application, [&] {
        const auto elapsed = heartbeatClock.elapsed();
        const auto late = std::max<qint64>(0, elapsed - previousTick - 10);
        previousTick = elapsed;
        latenessTotal += late;
        latenessMaximum = std::max(latenessMaximum, late);
        ++ticks;
        if (ticks % 100 != 0)
            return;
        QSaveFile metrics(QDir(directory).filePath(QStringLiteral("metrics.json")));
        if (metrics.open(QIODevice::WriteOnly)) {
            QJsonObject values{
                {QStringLiteral("ticks"), ticks},
                {QStringLiteral("max_lateness_ms"), latenessMaximum},
                {QStringLiteral("mean_lateness_ms"), static_cast<double>(latenessTotal) / ticks},
                {QStringLiteral("wall_ms"), elapsed},
                {QStringLiteral("fixture_pins"), fixturePins},
                {QStringLiteral("fixture_presentations"), fixturePresentations},
                {QStringLiteral("fixture_auto_filter_requests"), fixtureDetections},
                {QStringLiteral("fixture_image_copies"), clipboard->imagePublications},
                {QStringLiteral("fixture_mime_copies"), clipboard->mimePublications},
                {QStringLiteral("document_renders"), fixtureRenders.load()},
                {QStringLiteral("png_encodes"), fixtureEncodes.load()}};
            if (const auto currentCpu = currentThreadCpuMilliseconds(); currentCpu && initialCpu) {
                const auto cpu = *currentCpu - *initialCpu;
                values.insert(QStringLiteral("cpu_ms"), cpu);
                values.insert(QStringLiteral("cpu_utilization"),
                              elapsed > 0 ? cpu / static_cast<double>(elapsed) : 0);
            }
            metrics.write(QJsonDocument(values).toJson(QJsonDocument::Compact));
            static_cast<void>(metrics.commit());
        }
    });
    heartbeat.start(10);
    server.setRequestHandler([&](const ScreenshotMcpRequest& request, auto completion) {
        if (request.method == QStringLiteral("snow_shot_mcp_status")) {
            ScreenshotMcpResponse response;
            response.ok = true;
            response.result = {{QStringLiteral("reachable"), true},
                               {QStringLiteral("mcp_enabled"), true},
                               {QStringLiteral("capabilities"),
                                QJsonArray::fromStringList(documents.capabilities() +
                                                           McpApplicationService::methods())}};
            completion(response);
        } else if (applicationService.handles(request.method))
            applicationService.request(request, std::move(completion));
        else
            documents.request(request, std::move(completion));
    });
    server.setClientDisconnectedHandler([&](quint64 owner) {
        documents.disconnected(owner);
        applicationService.disconnected(owner);
    });
    server.setRequestCancellationHandler([&](quint64 owner, const QString& id) {
        const auto document = documents.cancelRequest(owner, id);
        return applicationService.cancelRequest(owner, id) || document;
    });
    QObject::connect(
        &documents, &McpDocumentService::artifactChanged, &server,
        [&](quint64 owner, const QString& id) {
            server.publishEvent(
                owner, {{QStringLiteral("event"), QStringLiteral("resource_changed")},
                        {QStringLiteral("uri"), QStringLiteral("snow-shot://artifacts/") + id}});
        });
    QObject::connect(
        &registry, &McpJobRegistry::changed, &server, [&](quint64 owner, const QString& id) {
            server.publishEvent(
                owner, {{QStringLiteral("event"), QStringLiteral("resource_changed")},
                        {QStringLiteral("uri"), QStringLiteral("snow-shot://jobs/") + id}});
        });
    QObject::connect(
        &documents, &McpDocumentService::changed, &server, [&](quint64 owner, const QString& id) {
            server.publishEvent(
                owner, {{QStringLiteral("event"), QStringLiteral("resource_changed")},
                        {QStringLiteral("uri"), QStringLiteral("snow-shot://documents/") + id}});
        });
    QString error;
    require(server.start(&error), qPrintable(error));
    const int result = application.exec();
    server.stop();
    applicationService.shutdown();
    documents.shutdown();
    return result;
}
void orientedFileDocuments(const QString& directory) {
    QImage image(64, 32, QImage::Format_RGB32);
    image.fill(Qt::blue);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width() / 2; ++x)
            image.setPixelColor(x, y, Qt::red);
    McpDocumentService service({});
    quint64 sequence = 0;
    const auto call = [&](QString method, QJsonObject params) {
        ScreenshotMcpRequest request;
        request.connectionId = 1;
        request.requestId = QString::number(++sequence);
        request.idempotencyKey = request.requestId;
        request.method = std::move(method);
        request.params = std::move(params);
        request.expectedRevision = 1;
        std::optional<ScreenshotMcpResponse> response;
        service.request(request, [&](auto value) { response = std::move(value); });
        QElapsedTimer timer;
        timer.start();
        while (!response && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(response.has_value(), "oriented document operation completed");
        return *response;
    };
    for (const quint8 orientation : {quint8(6), quint8(8)}) {
        const QByteArray encoded =
            image_orientation_fixture::jpegWithExifOrientation(image, orientation);
        const QString path =
            QDir(directory).filePath(QStringLiteral("oriented-source-%1.jpg").arg(orientation));
        QFile file(path);
        require(!encoded.isEmpty() && file.open(QIODevice::WriteOnly) &&
                    file.write(encoded) == encoded.size(),
                "oriented document fixture is writable");
        file.close();
        const auto opened = call(
            QStringLiteral("snow_shot_document_open"),
            {{QStringLiteral("source"), QStringLiteral("file")}, {QStringLiteral("path"), path}});
        require(opened.ok && opened.result.value(QStringLiteral("canvas_bounds")) ==
                                 QJsonArray{0, 0, 32, 64},
                "admitted document canvas must use EXIF transformed dimensions");
        const QJsonObject document{{QStringLiteral("document_id"),
                                    opened.result.value(QStringLiteral("document_id")).toString()}};
        const auto rendered = call(QStringLiteral("snow_shot_document_render"), document);
        const QImage raster = QImage::fromData(rendered.attachment);
        require(rendered.ok && raster.size() == QSize(32, 64),
                "oriented document output must match its admitted canvas dimensions");
        const QColor top = raster.pixelColor(16, 16);
        const QColor bottom = raster.pixelColor(16, 48);
        require(orientation == 6 ? top.red() > 240 && bottom.blue() > 240
                                 : top.blue() > 240 && bottom.red() > 240,
                "document imports must preserve both EXIF quarter-turn directions");
        require(call(QStringLiteral("snow_shot_document_close"), document).ok,
                "oriented source releases its document slot");
    }
    service.shutdown();
}

void documentReservations(const QString& directory) {
    QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    const auto smallPath = QDir(directory).filePath(QStringLiteral("reservation-small.png"));
    const auto largePath = QDir(directory).filePath(QStringLiteral("reservation-large-header.png"));
    require(image.save(smallPath), "reservation source image");
    QFile sourceFile(smallPath);
    require(sourceFile.open(QIODevice::ReadOnly), "reservation source bytes");
    auto header = sourceFile.readAll();
    qToBigEndian<quint32>(8000, header.data() + 16);
    qToBigEndian<quint32>(8000, header.data() + 20);
    quint32 crc = 0xffffffff;
    for (const char byte : header.mid(12, 17)) {
        crc ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    qToBigEndian<quint32>(crc ^ 0xffffffff, header.data() + 29);
    QFile large(largePath);
    require(large.open(QIODevice::WriteOnly) && large.write(header) == header.size(),
            "large image metadata fixture");
    large.close();
    for (const bool sameOwner : {true, false}) {
        QSemaphore entered;
        QSemaphore release;
        std::atomic_int decodeCalls = 0;
        McpDocumentService::Ports ports;
        ports.resolveSource = [image](const ScreenshotMcpRequest&, auto completion, auto budget) {
            require(budget(image.sizeInBytes()), "fixture source bytes reserved");
            McpDocumentService::Source source;
            source.image = image;
            completion(std::move(source), {});
        };
        ports.beforeWorkerDecode = [&](const ScreenshotMcpRequest& request) {
            if (request.params.value(QStringLiteral("path")).toString() != largePath)
                return;
            if (++decodeCalls == 1) {
                entered.release();
                release.acquire();
            }
        };
        McpDocumentService service(std::move(ports));
        quint64 sequence = 0;
        const auto dispatch = [&](QString method, QJsonObject params, quint64 owner,
                                  ScreenshotMcpServer::Completion completion) {
            ScreenshotMcpRequest request;
            request.connectionId = owner;
            request.requestId = QString::number(++sequence);
            request.idempotencyKey = request.requestId;
            request.method = std::move(method);
            request.params = std::move(params);
            request.expectedRevision = 1;
            service.request(request, std::move(completion));
            return request.requestId;
        };
        const auto wait = [](auto condition) {
            QElapsedTimer timer;
            timer.start();
            while (!condition() && timer.elapsed() < 5000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            require(condition(), "source reservation operation timed out");
        };
        const auto call = [&](QString method, QJsonObject params, quint64 owner = 1) {
            std::optional<ScreenshotMcpResponse> result;
            dispatch(std::move(method), std::move(params), owner,
                     [&](auto response) { result = std::move(response); });
            wait([&] { return result.has_value(); });
            return *result;
        };
        QStringList ids;
        for (int index = 0; index < 3; ++index) {
            const auto result = call(QStringLiteral("snow_shot_document_open"),
                                     {{QStringLiteral("source"), QStringLiteral("clipboard")}});
            require(result.ok, "source reservation baseline documents");
            ids.append(result.result.value(QStringLiteral("document_id")).toString());
        }
        std::optional<ScreenshotMcpResponse> blocked;
        const auto requestId = dispatch(QStringLiteral("snow_shot_document_open"),
                                        {{QStringLiteral("source"), QStringLiteral("file")},
                                         {QStringLiteral("path"), largePath}},
                                        1, [&](auto response) { blocked = std::move(response); });
        wait([&] { return entered.available() == 1; });
        const auto rejected = call(QStringLiteral("snow_shot_document_open"),
                                   {{QStringLiteral("source"), QStringLiteral("file")},
                                    {QStringLiteral("path"), largePath}},
                                   sameOwner ? 1 : 2);
        require(
            rejected.errorCode == QStringLiteral("resource_limit") && decodeCalls == 1,
            "concurrent source decode reserves both global bytes and owner slot before allocation");
        require(service.cancelRequest(1, requestId) && blocked &&
                    blocked->errorCode == QStringLiteral("canceled"),
                "pending decode reservation is cancelable");
        release.release();
        require(call(QStringLiteral("snow_shot_document_state"),
                     {{QStringLiteral("document_id"), ids.at(1)}})
                    .ok,
                "canceled decode lane drains before reservation reuse");
        require(call(QStringLiteral("snow_shot_document_open"),
                     {{QStringLiteral("source"), QStringLiteral("file")},
                      {QStringLiteral("path"), smallPath}})
                    .ok,
                "cancellation releases reserved source bytes and document slot");
        service.shutdown();
    }
}
void documentCacheWork(const QString& directory) {
    for (const bool evict : {false, true}) {
        std::atomic_int renders = 0, encodes = 0;
        McpDocumentService::Ports ports;
        auto clipboard = std::make_shared<MemoryClipboard>();
        ports.clipboard = clipboard;
        if (evict)
            ports.artifactCacheBytes = 80 * 60 * 4;
        ports.workObserved = [&](const QString&, const QString& operation) {
            if (operation == QStringLiteral("render"))
                ++renders;
            else if (operation == QStringLiteral("encode"))
                ++encodes;
        };
        ports.resolveSource = [](const ScreenshotMcpRequest&, auto completion, auto budget) {
            require(budget(80 * 60 * 4), "cache fixture source reservation");
            McpDocumentService::Source source;
            source.image = QImage(80, 60, QImage::Format_ARGB32_Premultiplied);
            source.image.fill(Qt::white);
            completion(std::move(source), {});
        };
        McpDocumentService service(std::move(ports));
        quint64 sequence = 0, revision = 1;
        const auto call = [&](QString method, QJsonObject params = {}) {
            ScreenshotMcpRequest request;
            request.connectionId = 1;
            request.requestId = QString::number(++sequence);
            request.idempotencyKey = request.requestId;
            request.method = std::move(method);
            request.params = std::move(params);
            request.expectedRevision = revision;
            std::optional<ScreenshotMcpResponse> response;
            service.request(request, [&](auto value) { response = std::move(value); });
            QElapsedTimer timer;
            timer.start();
            while (!response && timer.elapsed() < 5000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            require(response.has_value(), "cache instrumentation request completed");
            return *response;
        };
        const auto opened = call(QStringLiteral("snow_shot_document_open"),
                                 {{QStringLiteral("source"), QStringLiteral("clipboard")}});
        require(opened.ok, "cache instrumentation document opened");
        const auto id = opened.result.value(QStringLiteral("document_id")).toString();
        const QJsonObject document{{QStringLiteral("document_id"), id}};
        if (!evict) {
            const auto copied = call(QStringLiteral("snow_shot_document_copy"), document);
            require(copied.ok && renders == 1 && encodes == 1 && !clipboard->png.isEmpty(),
                    "cold copy creates one reusable raster and PNG");
        }
        const auto cold = call(QStringLiteral("snow_shot_document_render"), document);
        require(cold.ok && renders == 1 && encodes == 1,
                "first render reaches or reuses exactly one real raster and encoder boundary");
        if (!evict)
            require(clipboard->png == cold.attachment,
                    "render after cold copy retains the identical PNG bytes");
        const auto warm = call(QStringLiteral("snow_shot_document_render"), document);
        if (evict) {
            require(
                warm.ok && !warm.result.value(QStringLiteral("cache_hit")).toBool() &&
                    renders == 1 && encodes == 2 && warm.attachment == cold.attachment,
                "encoded eviction re-encodes retained raster without reporting a full cache hit");
        } else {
            require(warm.ok && warm.result.value(QStringLiteral("cache_hit")).toBool() &&
                        renders == 1 && encodes == 1 && warm.attachment == cold.attachment,
                    "valid render cache hit performs zero additional raster or encoder work");
            for (int index = 0; index < 2; ++index) {
                const auto copied = call(QStringLiteral("snow_shot_document_copy"), document);
                require(copied.ok && renders == 1 && encodes == 1 &&
                            clipboard->png == cold.attachment,
                        "copy reuses the same immutable PNG as render without another encode");
            }
            auto save = document;
            save.insert(QStringLiteral("path"),
                        QDir(directory).filePath(QStringLiteral("cache-save.png")));
            save.insert(QStringLiteral("format"), QStringLiteral("png"));
            save.insert(QStringLiteral("compression_level"),
                        snow_shot::storage::ScreenshotSettings().compressionLevel());
            for (int index = 0; index < 2; ++index) {
                const auto saved = call(QStringLiteral("snow_shot_document_save"), save);
                require(saved.ok && saved.result.value(QStringLiteral("cache_hit")).toBool() &&
                            renders == 1 && encodes == 1,
                        "valid repeated PNG save cache hit performs zero additional raster or "
                        "encoder work");
            }
            const auto edit = call(
                QStringLiteral("snow_shot_document_apply_annotations"),
                {{QStringLiteral("document_id"), id},
                 {QStringLiteral("operations"),
                  QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                         {QStringLiteral("bounds"), QJsonArray{5, 5, 20, 15}}}}}});
            require(edit.ok && edit.revision == 2, "cache revision invalidation edit accepted");
            revision = 2;
            const auto changed = call(QStringLiteral("snow_shot_document_render"), document);
            require(changed.ok && !changed.result.value(QStringLiteral("cache_hit")).toBool() &&
                        renders == 2 && encodes == 2,
                    "new document revision performs fresh raster and encoder work");
        }
        service.shutdown();
    }
}
void documentConcurrency() {
    QSemaphore entered;
    QSemaphore release;
    QString blockedId;
    int sourceResolutions = 0;
    McpDocumentService::Ports ports;
    ports.beforeWorkerRequest = [&](const ScreenshotMcpRequest& request) {
        if (request.method == QStringLiteral("snow_shot_document_render") &&
            request.params.value(QStringLiteral("document_id")).toString() == blockedId) {
            entered.release();
            release.acquire();
        }
    };
    ports.resolveSource = [&](const ScreenshotMcpRequest&, auto completion, auto budget) {
        ++sourceResolutions;
        require(budget(16 * 16 * 4), "fixture source bytes reserved");
        McpDocumentService::Source source;
        source.image = QImage(16, 16, QImage::Format_ARGB32_Premultiplied);
        source.image.fill(Qt::white);
        completion(std::move(source), {});
    };
    McpDocumentService service(std::move(ports));
    quint64 sequence = 0;
    auto dispatch = [&](const QString& method, QJsonObject params,
                        ScreenshotMcpServer::Completion completion, quint64 owner = 1) {
        ScreenshotMcpRequest request;
        request.connectionId = owner;
        request.requestId = QString::number(++sequence);
        request.idempotencyKey = request.requestId;
        request.method = method;
        request.params = std::move(params);
        request.expectedRevision = 1;
        service.request(request, std::move(completion));
    };
    const auto wait = [](auto condition) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(condition(), "independent document operation timed out");
    };
    const auto call = [&](const QString& method, QJsonObject params = {}, quint64 owner = 1) {
        std::optional<ScreenshotMcpResponse> result;
        dispatch(
            method, std::move(params), [&](auto response) { result = std::move(response); }, owner);
        wait([&] { return result.has_value(); });
        return *result;
    };
    QStringList ids;
    for (int index = 0; index < 4; ++index) {
        const auto opened = call(QStringLiteral("snow_shot_document_open"),
                                 {{QStringLiteral("source"), QStringLiteral("clipboard")}});
        require(opened.ok, "documents admitted across independent worker lanes");
        ids.append(opened.result.value(QStringLiteral("document_id")).toString());
    }
    require(call(QStringLiteral("snow_shot_document_open"),
                 {{QStringLiteral("source"), QStringLiteral("clipboard")}})
                    .errorCode == QStringLiteral("resource_limit"),
            "owner document limit is shared across all worker lanes");
    require(sourceResolutions == 4,
            "non-file sources reserve document slots before resolving pixels");
    blockedId = ids.first();
    std::optional<ScreenshotMcpResponse> rendered;
    dispatch(QStringLiteral("snow_shot_document_render"),
             {{QStringLiteral("document_id"), blockedId}},
             [&](auto response) { rendered = std::move(response); });
    wait([&] { return entered.available() == 1; });
    std::optional<ScreenshotMcpResponse> sameDocument;
    dispatch(QStringLiteral("snow_shot_document_state"),
             {{QStringLiteral("document_id"), blockedId}},
             [&](auto response) { sameDocument = std::move(response); });
    const auto independent = call(QStringLiteral("snow_shot_document_state"),
                                  {{QStringLiteral("document_id"), ids.at(1)}});
    require(independent.ok && !rendered && !sameDocument,
            "unrelated lane progresses while blocked document retains its ordered queue");
    release.release();
    wait([&] { return rendered.has_value() && sameDocument.has_value(); });
    require(rendered->ok && sameDocument->ok,
            "blocked lane resumes and completes all queued document work");
    require(call(QStringLiteral("snow_shot_document_list"))
                    .result.value(QStringLiteral("documents"))
                    .toArray()
                    .size() == 4,
            "document listing aggregates every lane exactly once");
    require(call(QStringLiteral("snow_shot_document_close"),
                 {{QStringLiteral("document_id"), ids.at(2)}})
                    .ok &&
                call(QStringLiteral("snow_shot_document_open"),
                     {{QStringLiteral("source"), QStringLiteral("clipboard")}})
                    .ok,
            "closing a document releases shared admission capacity");
    for (quint64 owner = 2; owner <= 4; ++owner)
        for (int index = 0; index < 4; ++index)
            require(call(QStringLiteral("snow_shot_document_open"),
                         {{QStringLiteral("source"), QStringLiteral("clipboard")}}, owner)
                        .ok,
                    "global document admission reaches the shared sixteen-document limit");
    require(call(QStringLiteral("snow_shot_document_open"),
                 {{QStringLiteral("source"), QStringLiteral("clipboard")}}, 5)
                    .errorCode == QStringLiteral("resource_limit"),
            "adding worker lanes does not multiply the global document quota");
    service.shutdown();
}
void documentMemoryActivity() {
    using snow_shot::runtime::RuntimeActivityTracker;
    auto& activity = RuntimeActivityTracker::shared();
    const auto wait = [](auto condition) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QThread::msleep(1);
        }
        require(condition(), "document memory activity did not settle");
    };
    wait([&] { return activity.snapshot().activeCount == 0; });
    const auto baseline = activity.snapshot().activeCount;
    const auto requireTrimBlocked = [&] {
        const auto snapshot = activity.snapshot();
        bool trimmed = false;
        require(snapshot.activeCount > baseline &&
                    !activity.tryRunWhenIdle(snapshot.generation, [&] { trimmed = true; }) &&
                    !trimmed,
                "pending document work and cleanup must block memory trimming");
    };
    const auto waitForIdle = [&] {
        wait([&] { return activity.snapshot().activeCount == baseline; });
        const auto snapshot = activity.snapshot();
        bool trimmed = false;
        require(activity.tryRunWhenIdle(snapshot.generation, [&] { trimmed = true; }) && trimmed,
                "completed document work must release memory activity");
    };
    for (const bool cancelBeforeSource : {false, true}) {
        McpDocumentService::Ports::SourceCompletion pendingSource;
        McpJobRegistry jobs;
        McpDocumentService::Ports ports;
        ports.jobs = &jobs;
        ports.resolveSource = [&](const ScreenshotMcpRequest&, auto completion, auto budget) {
            require(budget(16 * 16 * 4), "reserve a delayed document source");
            pendingSource = std::move(completion);
        };
        McpDocumentService service(std::move(ports));
        ScreenshotMcpRequest request;
        request.connectionId = 1;
        request.requestId = QStringLiteral("delayed-source");
        request.idempotencyKey = request.requestId;
        request.method = QStringLiteral("snow_shot_document_open");
        request.params = {{QStringLiteral("source"), QStringLiteral("clipboard")}};
        int completions = 0;
        std::optional<ScreenshotMcpResponse> opened;
        service.request(request, [&](auto response) {
            requireTrimBlocked();
            ++completions;
            opened = std::move(response);
        });
        require(pendingSource && completions == 0 && !jobs.hasRunningJobs(),
                "accepted source resolution waits without a worker or registry job");
        requireTrimBlocked();
        if (cancelBeforeSource) {
            require(service.cancelRequest(1, request.requestId) && completions == 1 && opened &&
                        opened->errorCode == QStringLiteral("canceled"),
                    "cancel a document request while its external source is pending");
            waitForIdle();
            require(bool(pendingSource),
                    "a retained uninvoked source callback does not pin canceled request activity");
        }
        McpDocumentService::Source source;
        source.image = QImage(16, 16, QImage::Format_ARGB32_Premultiplied);
        source.image.fill(Qt::white);
        pendingSource(std::move(source), {});
        wait([&] { return completions == 1; });
        waitForIdle();
        require(
            pendingSource && opened &&
                (cancelBeforeSource ? opened->errorCode == QStringLiteral("canceled") : opened->ok),
            "late or completed source delivery retires activity without another reply");
        service.shutdown();
        waitForIdle();
    }
    enum class Outcome { Completed, Cancelled, Disconnected };
    for (const auto outcome : {Outcome::Completed, Outcome::Cancelled, Outcome::Disconnected}) {
        QSemaphore entered;
        QSemaphore release;
        QList<McpDocumentService::Ports::SourceCompletion> retainedSourceCompletions;
        McpJobRegistry jobs;
        McpDocumentService::Ports ports;
        ports.jobs = &jobs;
        ports.beforeWorkerRequest = [&](const ScreenshotMcpRequest& request) {
            if (request.method == QStringLiteral("snow_shot_document_render")) {
                entered.release();
                release.acquire();
            }
        };
        ports.resolveSource = [&](const ScreenshotMcpRequest&, auto completion, auto budget) {
            require(budget(16 * 16 * 4), "reserve the memory activity fixture source");
            McpDocumentService::Source source;
            source.image = QImage(16, 16, QImage::Format_ARGB32_Premultiplied);
            source.image.fill(Qt::white);
            retainedSourceCompletions.append(completion);
            completion(std::move(source), {});
        };
        McpDocumentService service(std::move(ports));
        quint64 sequence = 0;
        const auto dispatch = [&](const QString& method, QJsonObject params,
                                  ScreenshotMcpServer::Completion completion, quint64 owner = 1) {
            ScreenshotMcpRequest request;
            request.connectionId = owner;
            request.requestId = QString::number(++sequence);
            request.idempotencyKey = request.requestId;
            request.method = method;
            request.params = std::move(params);
            request.expectedRevision = 1;
            const auto id = request.requestId;
            service.request(request, std::move(completion));
            return id;
        };
        std::optional<ScreenshotMcpResponse> opened;
        dispatch(QStringLiteral("snow_shot_document_open"),
                 {{QStringLiteral("source"), QStringLiteral("clipboard")}},
                 [&](auto response) { opened = std::move(response); });
        wait([&] { return opened.has_value(); });
        require(opened->ok && !retainedSourceCompletions.isEmpty(),
                "open a document while retaining an already-invoked source callback");
        waitForIdle();
        const auto documentId = opened->result.value(QStringLiteral("document_id"));
        int completions = 0;
        std::optional<ScreenshotMcpResponse> rendered;
        const auto renderRequest =
            dispatch(QStringLiteral("snow_shot_document_render"),
                     {{QStringLiteral("document_id"), documentId}}, [&](auto response) {
                         requireTrimBlocked();
                         ++completions;
                         rendered = std::move(response);
                     });
        wait([&] { return entered.available() > 0; });
        require(completions == 0 && !jobs.hasRunningJobs(),
                "blocked document rendering has no registry job or premature completion");
        requireTrimBlocked();
        if (outcome == Outcome::Cancelled) {
            require(service.cancelRequest(1, renderRequest) && completions == 1 && rendered &&
                        rendered->errorCode == QStringLiteral("canceled"),
                    "request cancellation acknowledges before the blocked worker returns");
            requireTrimBlocked();
        } else if (outcome == Outcome::Disconnected) {
            service.disconnected(1);
            require(completions == 0, "disconnect suppresses the pending render completion");
            requireTrimBlocked();
        }
        release.release();
        if (outcome == Outcome::Completed) {
            wait([&] { return completions == 1; });
            require(rendered && rendered->ok && !rendered->attachment.isEmpty(),
                    "the blocked render publishes its image after actual worker completion");
        }
        waitForIdle();
        require(completions == (outcome == Outcome::Disconnected ? 0 : 1) &&
                    !retainedSourceCompletions.isEmpty(),
                "late worker completion does not reply twice or pin retained source callbacks");
        if (outcome == Outcome::Disconnected) {
            std::optional<ScreenshotMcpResponse> listed;
            dispatch(QStringLiteral("snow_shot_document_list"), {},
                     [&](auto response) { listed = std::move(response); });
            wait([&] { return listed.has_value(); });
            require(listed->ok &&
                        listed->result.value(QStringLiteral("documents")).toArray().isEmpty(),
                    "reconnecting the same owner finds no documents after worker cleanup");
            waitForIdle();
        }
        service.shutdown();
        waitForIdle();
    }
    {
        McpDocumentService::Ports::SourceCompletion pendingSource;
        int sourceResolutions = 0;
        McpJobRegistry jobs;
        McpDocumentService::Ports ports;
        ports.jobs = &jobs;
        ports.resolveSource = [&](const ScreenshotMcpRequest&, auto completion, auto) {
            ++sourceResolutions;
            pendingSource = std::move(completion);
        };
        McpDocumentService service(std::move(ports));
        require(!service.storeArtifact(1, QByteArray("shutdown"), QStringLiteral("text/plain"))
                     .isEmpty(),
                "register the shutdown fixture owner");
        int cancellations = 0;
        int completions = 0;
        std::optional<ScreenshotMcpResponse> response;
        const auto job = jobs.start(1, QStringLiteral("shutdown"), [&] {
            ++cancellations;
            requireTrimBlocked();
            ScreenshotMcpRequest request;
            request.connectionId = 2;
            request.requestId = QStringLiteral("during-shutdown");
            request.idempotencyKey = request.requestId;
            request.method = QStringLiteral("snow_shot_document_open");
            request.params = {{QStringLiteral("source"), QStringLiteral("clipboard")}};
            service.request(request, [&](auto result) {
                requireTrimBlocked();
                ++completions;
                response = std::move(result);
            });
            require(completions == 1 && response &&
                        response->errorCode == QStringLiteral("disabled") &&
                        sourceResolutions == 0 && !pendingSource,
                    "shutdown rejects reentrant requests synchronously before source admission");
        });
        require(!job.isEmpty(), "admit a job with a reentrant shutdown cancellation callback");
        service.shutdown();
        require(cancellations == 1 && completions == 1 && !jobs.hasRunningJobs(),
                "shutdown cancels the job once and leaves no reentrant request pending");
        waitForIdle();
    }
}
void recognitionModelDocuments(const QString& directory) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION && SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    using Settings = snow_shot::storage::ScreenshotRecognitionModelSettings;
    const auto savedTableModel = Settings().tableModel();
    const auto savedLatexModel = Settings().latexModel();
    const auto restoreSettings = qScopeGuard([&] {
        Settings().setTableModel(savedTableModel);
        Settings().setLatexModel(savedLatexModel);
    });
    require(Settings().setTableModel(QStringLiteral("dedicated")) &&
                Settings().setLatexModel(QStringLiteral("snow-shot:vision")),
            "MCP recognition defaults differ from the saved source choices");

    ScreenshotRecognitionResults saved;
    saved.key = QStringLiteral("saved-source");
    saved.tableModelSelection = QStringLiteral("vision-b");
    saved.latexModelSelection = QStringLiteral("vision-b");
    ScreenshotTableRecognitionEntry tableA;
    tableA.model = QStringLiteral("vision-a");
    tableA.result.html = QStringLiteral("<table><tr><td>A baseline</td></tr></table>");
    ScreenshotTableRecognitionEntry tableB;
    tableB.model = QStringLiteral("vision-b");
    tableB.result.html = QStringLiteral("<table><tr><th rowspan=\"2\">B heading</th>"
                                        "<td>B baseline top</td></tr><tr><td></td></tr></table>");
    tableB.draftHtml = QStringLiteral("<table><tr><th rowspan=\"2\">B heading</th>"
                                      "<td>B saved top</td></tr><tr><td></td></tr></table>");
    saved.tableEntries = {tableA, tableB};
    ScreenshotLatexRecognitionEntry latexA;
    latexA.model = QStringLiteral("vision-a");
    latexA.result.latex = QStringLiteral("a^2");
    latexA.draft = QStringLiteral("A saved formula");
    ScreenshotLatexRecognitionEntry latexB;
    latexB.model = QStringLiteral("vision-b");
    latexB.result.latex = QStringLiteral("b^2");
    latexB.draft = QString{};
    saved.latexEntries = {latexA, latexB};

    McpJobRegistry registry;
    auto clipboard = std::make_shared<MemoryClipboard>();
    McpDocumentService::Ports ports;
    ports.jobs = &registry;
    ports.clipboard = clipboard;
    int sourceResolutions = 0;
    ports.resolveSource = [&](const ScreenshotMcpRequest&, auto completion, auto budget) {
        ++sourceResolutions;
        require(budget(80 * 60 * 4), "MCP saved recognition source reserves image bytes");
        McpDocumentService::Source source;
        source.image = QImage(80, 60, QImage::Format_ARGB32_Premultiplied);
        source.image.fill(Qt::white);
        source.recognitionResults = saved;
        completion(std::move(source), {});
    };
    QVector<ScreenshotRecognitionResults> handoffs;
    ports.present = [&](McpDocumentService::Source source, auto completion) {
        handoffs.append(std::move(source.recognitionResults));
        completion(true);
        return true;
    };
    ports.pinDocument = [&](McpDocumentService::Source source, QImage image, auto completion) {
        require(!image.isNull(), "MCP recognition pin retains the document background");
        handoffs.append(std::move(source.recognitionResults));
        completion(true);
        return true;
    };
    McpDocumentService service(std::move(ports));
    quint64 sequence = 0, revision = 1;
    qint64 recognitionRevision = 1;
    QString id;
    const auto wait = [](auto condition) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(condition(), "MCP selected recognition operation completes");
    };
    const auto call = [&](const QString& method, QJsonObject params = {}) {
        ScreenshotMcpRequest request;
        request.connectionId = 17;
        request.requestId = QString::number(++sequence);
        request.idempotencyKey = request.requestId;
        request.expectedRevision = revision;
        request.method = QStringLiteral("snow_shot_") + method;
        if (!id.isEmpty())
            params.insert(QStringLiteral("document_id"), id);
        request.params = std::move(params);
        std::optional<ScreenshotMcpResponse> response;
        service.request(request, [&](auto value) { response = std::move(value); });
        wait([&] { return response.has_value(); });
        if (response->ok && response->revision)
            revision = *response->revision;
        return *response;
    };
    const auto recognize = [&](const QString& kind) {
        const auto started =
            call(QStringLiteral("document_recognize"), {{QStringLiteral("kind"), kind}});
        require(started.ok, "MCP recognizes saved model results without an API provider");
        const auto job = started.result.value(QStringLiteral("job_id")).toString();
        wait([&] {
            const auto value = registry.get(17, job);
            return value && value->value(QStringLiteral("status")) != QStringLiteral("running");
        });
        const auto result = *registry.get(17, job);
        require(
            result.value(QStringLiteral("status")) == QStringLiteral("completed") &&
                result.value(QStringLiteral("result")).toObject().value(QStringLiteral("kind")) ==
                    kind,
            "MCP recognition jobs retain the selected Table or LaTeX workflow");
        recognitionRevision = 1;
        return call(QStringLiteral("document_recognition_state"));
    };
    const auto edit = [&](const QString& action, QJsonObject params = {}) {
        params.insert(QStringLiteral("action"), action);
        params.insert(QStringLiteral("expected_recognition_revision"), recognitionRevision);
        const auto result = call(QStringLiteral("document_edit_recognition"), std::move(params));
        if (action != QStringLiteral("set_model"))
            require(result.ok, "MCP selected recognition edit succeeds");
        if (result.ok)
            recognitionRevision =
                result.result.value(QStringLiteral("recognition_revision")).toInteger();
        return result;
    };
    const auto opened = call(QStringLiteral("document_open"),
                             {{QStringLiteral("source"), QStringLiteral("clipboard")}});
    require(opened.ok, "MCP opens a source carrying independent recognition model results");
    id = opened.result.value(QStringLiteral("document_id")).toString();
    const auto table = recognize(QStringLiteral("table"));
    const auto cells = table.result.value(QStringLiteral("cells")).toArray();
    require(table.ok && table.result.value(QStringLiteral("rows")).toInt() == 2 &&
                table.result.value(QStringLiteral("columns")).toInt() == 2 && cells.size() == 3 &&
                cells.at(0).toObject().value(QStringLiteral("row_span")).toInt() == 2 &&
                cells.at(0).toObject().value(QStringLiteral("header")).toBool() &&
                cells.at(1).toObject().value(QStringLiteral("text")) ==
                    QStringLiteral("B saved top") &&
                cells.at(2).toObject().value(QStringLiteral("text")).toString().isEmpty(),
            "MCP consumes selected vision Table draft with merged headers and empty cells");
    require(edit(QStringLiteral("set_cell"), {{QStringLiteral("row"), 0},
                                              {QStringLiteral("column"), 1},
                                              {QStringLiteral("text"), QStringLiteral("B edited")}})
                .ok,
            "MCP edits the selected model's Table draft");
    const auto tableCopy = call(QStringLiteral("document_export_recognition"),
                                {{QStringLiteral("output"), QStringLiteral("copy")},
                                 {QStringLiteral("format"), QStringLiteral("html")}});
    require(tableCopy.ok && clipboard->mime && clipboard->mime->hasHtml() &&
                clipboard->mime->html().contains(QStringLiteral("B edited")) &&
                !clipboard->mime->html().contains(QStringLiteral("A baseline")),
            "MCP HTML copy publishes only the edited selected Table");
    const auto tablePath = QDir(directory).filePath(QStringLiteral("selected-table.html"));
    require(call(QStringLiteral("document_export_recognition"),
                 {{QStringLiteral("output"), QStringLiteral("save")},
                  {QStringLiteral("format"), QStringLiteral("html")},
                  {QStringLiteral("path"), tablePath}})
                .ok,
            "MCP saves the selected Table through the existing export workflow");
    QFile tableFile(tablePath);
    require(tableFile.open(QIODevice::ReadOnly) &&
                tableFile.readAll() == clipboard->mime->html().toUtf8(),
            "MCP Table save and copy retain the identical canonical selected draft");

    const auto latex = recognize(QStringLiteral("latex"));
    require(latex.ok && latex.result.contains(QStringLiteral("text")) &&
                latex.result.value(QStringLiteral("text")).toString().isEmpty(),
            "MCP consumes a valid empty LaTeX draft from the selected model");
    const auto emptyCopy = call(QStringLiteral("document_export_recognition"),
                                {{QStringLiteral("output"), QStringLiteral("copy")},
                                 {QStringLiteral("format"), QStringLiteral("text")}});
    require(emptyCopy.ok && clipboard->mimePublications == 2 && clipboard->mime &&
                clipboard->mime->text().isEmpty(),
            "MCP empty LaTeX copy publishes an empty draft instead of another model's formula");
    const auto latexPath = QDir(directory).filePath(QStringLiteral("selected-empty-latex.txt"));
    require(call(QStringLiteral("document_export_recognition"),
                 {{QStringLiteral("output"), QStringLiteral("save")},
                  {QStringLiteral("format"), QStringLiteral("text")},
                  {QStringLiteral("path"), latexPath}})
                    .ok &&
                QFileInfo::exists(latexPath) && QFileInfo(latexPath).size() == 0,
            "MCP saves an empty selected LaTeX draft losslessly");
    require(edit(QStringLiteral("set_text"),
                 {{QStringLiteral("text"), QStringLiteral("B edited formula")}})
                    .ok &&
                edit(QStringLiteral("undo"))
                    .result.value(QStringLiteral("text"))
                    .toString()
                    .isEmpty() &&
                edit(QStringLiteral("redo")).result.value(QStringLiteral("text")) ==
                    QStringLiteral("B edited formula"),
            "MCP selected LaTeX edits preserve empty drafts in undo and redo");
    require(
        edit(QStringLiteral("set_text"), {{QStringLiteral("text"), QString{}}}).ok &&
            edit(QStringLiteral("reset_text")).result.value(QStringLiteral("text")) ==
                QStringLiteral("b^2") &&
            edit(QStringLiteral("undo")).result.value(QStringLiteral("text")).toString().isEmpty(),
        "MCP LaTeX reset uses the selected baseline and undo restores an empty draft");
    require(
        edit(QStringLiteral("set_model"), {{QStringLiteral("model"), QStringLiteral("vision-a")}})
                    .errorCode == QStringLiteral("action_unavailable") &&
            !service.handles(QStringLiteral("snow_shot_document_set_recognition_model")),
        "MCP keeps its existing recognition interface without adding model selection actions");
    const auto tableAgain = recognize(QStringLiteral("table"));
    require(tableAgain.ok && tableAgain.result.value(QStringLiteral("html"))
                                 .toString()
                                 .contains(QStringLiteral("B edited")),
            "MCP workflow changes retain the selected Table's edited result");
    const auto latexAgain = recognize(QStringLiteral("latex"));
    require(latexAgain.ok && latexAgain.result.value(QStringLiteral("text")).toString().isEmpty() &&
                call(QStringLiteral("document_export_recognition"),
                     {{QStringLiteral("output"), QStringLiteral("return")},
                      {QStringLiteral("format"), QStringLiteral("text")}})
                    .result.value(QStringLiteral("text"))
                    .toString()
                    .isEmpty(),
            "MCP returns the selected empty LaTeX draft after switching workflows");
    require(call(QStringLiteral("document_present")).ok &&
                call(QStringLiteral("document_pin")).ok && handoffs.size() == 2,
            "MCP present and pin carry selected recognition snapshots");
    for (const auto& snapshot : handoffs) {
        require(!snapshot.key.isEmpty() &&
                    snapshot.tableModelSelection == QStringLiteral("vision-b") &&
                    snapshot.latexModelSelection == QStringLiteral("vision-b") &&
                    snapshot.tableEntries.size() == 2 && snapshot.latexEntries.size() == 2,
                "MCP handoff preserves both selected identities and independent model entries");
        for (const auto& entry : snapshot.tableEntries)
            require(entry.model == QStringLiteral("vision-a")
                        ? entry.result.html == tableA.result.html && !entry.draftHtml
                        : entry.draftHtml && entry.draftHtml->contains(QStringLiteral("B edited")),
                    "MCP handoff keeps Table edits isolated to the selected model");
        for (const auto& entry : snapshot.latexEntries)
            require(entry.model == QStringLiteral("vision-a")
                        ? entry.draft == latexA.draft
                        : entry.draft && entry.draft->isEmpty() &&
                              entry.result.latex == QStringLiteral("b^2"),
                    "MCP handoff keeps selected empty LaTeX drafts and their reset baseline");
    }
    require(
        sourceResolutions == 1 && Settings().tableModel() == QStringLiteral("dedicated") &&
            Settings().latexModel() == QStringLiteral("snow-shot:vision"),
        "MCP saved choices remain document-local without resolving pixels or changing settings");
    service.shutdown();
#else
    Q_UNUSED(directory)
#endif
}

void recognitionModelSourceBudget() {
    // QString copies share storage: each fixture retains only one 8 MiB allocation, while the
    // admission charge accounts for every model's independent baseline and draft.
    const QString sharedText(4 * 1024 * 1024, QLatin1Char('x'));
    for (const bool latex : {false, true}) {
        bool largeSource = true;
        McpDocumentService::Ports ports;
        ports.resolveSource = [&](const ScreenshotMcpRequest&, auto completion, auto budget) {
            require(budget(16 * 16 * 4), "MCP recognition quota fixture reserves source pixels");
            McpDocumentService::Source source;
            source.image = QImage(16, 16, QImage::Format_ARGB32_Premultiplied);
            source.image.fill(Qt::white);
            if (largeSource) {
                for (int index = 0; index < 33; ++index) {
                    const auto model = QStringLiteral("vision-%1").arg(index);
                    if (latex) {
                        ScreenshotLatexRecognitionEntry entry;
                        entry.model = model;
                        entry.result.latex = sharedText;
                        entry.draft = sharedText;
                        source.recognitionResults.latexEntries.append(std::move(entry));
                    } else {
                        ScreenshotTableRecognitionEntry entry;
                        entry.model = model;
                        entry.result.html = sharedText;
                        entry.draftHtml = sharedText;
                        source.recognitionResults.tableEntries.append(std::move(entry));
                    }
                }
            }
            completion(std::move(source), {});
        };
        McpDocumentService service(std::move(ports));
        quint64 sequence = 0;
        const auto call = [&](const QString& method, QJsonObject params = {}) {
            ScreenshotMcpRequest request;
            request.connectionId = 23;
            request.requestId = QString::number(++sequence);
            request.idempotencyKey = request.requestId;
            request.expectedRevision = 1;
            request.method = QStringLiteral("snow_shot_") + method;
            request.params = std::move(params);
            std::optional<ScreenshotMcpResponse> response;
            service.request(request, [&](auto result) { response = std::move(result); });
            QElapsedTimer timer;
            timer.start();
            while (!response && timer.elapsed() < 5000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            require(response.has_value(), "MCP recognition quota operation completes");
            return *response;
        };
        require(
            call(QStringLiteral("document_open"),
                 {{QStringLiteral("source"), QStringLiteral("clipboard")}})
                    .errorCode == QStringLiteral("resource_limit"),
            "all per-model Table or LaTeX baselines and drafts count toward source memory limits");
        largeSource = false;
        for (int index = 0; index < 4; ++index)
            require(
                call(QStringLiteral("document_open"),
                     {{QStringLiteral("source"), QStringLiteral("clipboard")}})
                    .ok,
                "rejected recognition snapshots release source bytes and document admission slots");
        service.shutdown();
    }
}

void documentWorkflows() {
    FixtureOcr ocr;
    FixtureQr qr;
    McpJobRegistry registry;
    McpDocumentService::Ports ports;
    const auto clipboard = std::make_shared<MemoryClipboard>();
    ports.clipboard = clipboard;
    ports.jobs = &registry;
    ports.recognition = &ocr;
    ports.qrRecognition = &qr;
    QImage image(80, 60, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::green);
    McpDocumentService::Ports::SourceCompletion pendingSource;
    int sourceCancellations = 0;
    ports.cancelSource = [&](quint64 owner, const QString& requestId) {
        require(owner == 8 && !requestId.isEmpty(),
                "source cancellation preserves owner and child request");
        ++sourceCancellations;
    };
    ports.resolveSource = [&](const ScreenshotMcpRequest& request, auto completion, auto budget) {
        if (!budget(image.sizeInBytes())) {
            completion({}, QStringLiteral("resource_limit"));
            return;
        }
        if (request.params.value(QStringLiteral("delay_seconds")).toInt() > 0) {
            pendingSource = std::move(completion);
            return;
        }
        McpDocumentService::Source source;
        source.image = image;
        source.originalContent.text = QStringLiteral("Original text");
        source.originalContent.html = QStringLiteral("<b>Original text</b>");
        completion(std::move(source), {});
    };
    int presentations = 0;
    int pins = 0;
    std::function<void(QList<SnowCanvasAutoFilterRegion>, QString)> detection;
    ports.autoFilter = [&](QImage pixels, auto completion) {
        require(pixels.size() == QSize(80, 60), "detector receives independent document raster");
        detection = std::move(completion);
    };
    ports.pinDocument = [&](McpDocumentService::Source source, QImage background, auto completion) {
        require(!source.documentSession.isEmpty() && !source.documentHistory.isEmpty() &&
                    background.size() == QSize(80, 60) &&
                    background.pixelColor(12, 12) == QColor(Qt::green),
                "pin carries editable history and unannotated background");
        ++pins;
        completion(true);
        return true;
    };
    ports.present = [&](McpDocumentService::Source source, auto completion) {
        require(!source.images.isEmpty() && !source.documentSession.isEmpty() &&
                    !source.documentHistory.isEmpty() && source.selection.has_value(),
                "handoff carries editable private snapshot and history");
        ++presentations;
        completion(true);
        return true;
    };
    McpDocumentService service(std::move(ports));
    registry.setArtifactPublisher([&](quint64 owner, QByteArray bytes, const QString& mime) {
        return service.storeArtifact(owner, std::move(bytes), mime);
    });
    quint64 sequence = 0, revision = 1;
    QString id;
    const auto call = [&](const QString& method, QJsonObject params = {}) {
        ScreenshotMcpRequest request;
        request.connectionId = 8;
        request.requestId = QString::number(++sequence);
        request.idempotencyKey = request.requestId;
        request.expectedRevision = revision;
        request.method = QStringLiteral("snow_shot_") + method;
        if (!id.isEmpty())
            params.insert(QStringLiteral("document_id"), id);
        request.params = std::move(params);
        std::optional<ScreenshotMcpResponse> result;
        service.request(request, [&](auto response) { result = std::move(response); });
        QElapsedTimer timer;
        timer.start();
        while (!result && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(result.has_value(), "advanced document request completes");
        if (result->ok && result->revision > 0)
            revision = *result->revision;
        return *result;
    };
    const auto opened = call(QStringLiteral("document_open"),
                             {{QStringLiteral("source"), QStringLiteral("clipboard")}});
    require(opened.ok, "source resolver creates independent editable document");
    id = opened.result.value(QStringLiteral("document_id")).toString();
    require(call(QStringLiteral("document_copy")).result.value(QStringLiteral("copied")).toBool() &&
                clipboard->imagePublications == 1 &&
                QImage::fromData(clipboard->png, "PNG").pixelColor(2, 2) == QColor(Qt::green),
            "document copy publishes actual source pixels through the injected clipboard");
    require(
        call(QStringLiteral("document_sample_color"), {{QStringLiteral("point"), QJsonArray{2, 2}}})
                .result.value(QStringLiteral("rgba"))
                .toArray() == QJsonArray{0, 255, 0, 255},
        "sample reads canonical document pixels");
    const auto original = call(QStringLiteral("document_original_content"));
    require(original.ok &&
                original.result.value(QStringLiteral("text")) == QStringLiteral("Original text"),
            "original text and html survive source rendering");
    const QJsonObject annotation{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                 {QStringLiteral("bounds"), QJsonArray{10, 10, 25, 25}}};
    const auto annotated = call(QStringLiteral("document_apply_annotations"),
                                {{QStringLiteral("operations"), QJsonArray{annotation}}});
    require(annotated.ok, "headless edit fixture annotation");
    const auto element = annotated.result.value(QStringLiteral("transaction"))
                             .toObject()
                             .value(QStringLiteral("created_element_ids"))
                             .toArray()
                             .first();
    require(call(QStringLiteral("document_edit_elements"),
                 {{QStringLiteral("action"), QStringLiteral("select")},
                  {QStringLiteral("id"), element}})
                .ok,
            "explicitly select created element before template export");
    const auto exported = call(QStringLiteral("document_draw_template"),
                               {{QStringLiteral("action"), QStringLiteral("export")}});
    require(exported.ok && !exported.result.value(QStringLiteral("payload")).toString().isEmpty(),
            "headless template export preserves editable elements");
    require(call(QStringLiteral("document_edit_elements"),
                 {{QStringLiteral("action"), QStringLiteral("duplicate")}})
                .ok,
            "headless duplicate uses engine selection and undo stack");
    require(call(QStringLiteral("document_edit_elements"),
                 {{QStringLiteral("action"), QStringLiteral("opacity")},
                  {QStringLiteral("opacity"), 0.4}})
                .ok,
            "headless selection opacity uses shared engine");
    require(call(QStringLiteral("document_draw_template"),
                 {{QStringLiteral("action"), QStringLiteral("insert")},
                  {QStringLiteral("payload"), exported.result.value(QStringLiteral("payload"))}})
                .ok,
            "headless template insertion validates format");
    const QStringList targets{QStringLiteral("rectangle"),
                              QStringLiteral("arrow"),
                              QStringLiteral("line"),
                              QStringLiteral("freehand"),
                              QStringLiteral("rectangle_highlight"),
                              QStringLiteral("pen_highlight"),
                              QStringLiteral("rectangle_filter"),
                              QStringLiteral("pen_filter"),
                              QStringLiteral("watermark"),
                              QStringLiteral("spotlight"),
                              QStringLiteral("text"),
                              QStringLiteral("serial_number")};
    for (const auto& target : targets) {
        require(call(QStringLiteral("document_set_tool"), {{QStringLiteral("tool"), target}}).ok,
                "headless drawing tool accepted");
        const bool shape = targets.indexOf(target) < 6;
        const auto styled = call(
            QStringLiteral("document_set_tool_style"),
            {{QStringLiteral("target"), target},
             {QStringLiteral("style"), shape ? QJsonObject{{QStringLiteral("stroke_width"), 4}}
                                             : QJsonObject{{QStringLiteral("opacity"), 0.8}}}});
        if (!styled.ok)
            std::cerr << "style failed: " << target.toStdString() << '\n';
        require(styled.ok, "all drawing style families share validated tool styles");
    }
    const auto beforeInvalid = revision;
    require(!call(QStringLiteral("document_set_tool_style"),
                  {{QStringLiteral("target"), QStringLiteral("text")},
                   {QStringLiteral("style"), QJsonObject{{QStringLiteral("opacity"), 2}}}})
                    .ok &&
                revision == beforeInvalid,
            "invalid style is atomic");
    require(call(QStringLiteral("document_recapture"),
                 {{QStringLiteral("source"), QStringLiteral("clipboard")}})
                .ok,
            "source refresh preserves compatible geometry and history");
    const auto present = call(QStringLiteral("document_present"));
    require(present.ok && presentations == 1 &&
                present.result.value(QStringLiteral("ownership")) == QStringLiteral("user"),
            "visible snapshot ownership transfers explicitly");
    require(call(QStringLiteral("document_pin")).ok && pins == 1,
            "pin hands off a full editable document with user ownership");
    const auto filter = [&] {
        return call(QStringLiteral("document_auto_filter"),
                    {{QStringLiteral("categories"), QJsonArray{QStringLiteral("text")}}});
    };
    const auto awaitJob = [&](const QString& jobId) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (registry.get(8, jobId)->value(QStringLiteral("status")) ==
                   QStringLiteral("running") &&
               elapsed.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        return *registry.get(8, jobId);
    };
    const auto canceledFilter = filter();
    const auto canceledId = canceledFilter.result.value(QStringLiteral("job_id")).toString();
    require(canceledFilter.ok && registry.cancel(8, canceledId),
            "detector job cancels before commit");
    const auto beforeCanceledFilter = revision;
    std::exchange(detection, {})({{1, QRectF(10, 10, 20, 15), QStringLiteral("text")}}, {});
    require(call(QStringLiteral("document_state")).revision == beforeCanceledFilter &&
                awaitJob(canceledId).value(QStringLiteral("status")) == QStringLiteral("canceled"),
            "late canceled detector result does not mutate document");
    const auto staleFilter = filter();
    require(call(QStringLiteral("document_set_tool"),
                 {{QStringLiteral("tool"), QStringLiteral("rectangle")}})
                .ok,
            "concurrent document mutation advances revision");
    std::exchange(detection, {})({{1, QRectF(10, 10, 20, 15), QStringLiteral("text")}}, {});
    const auto staleResult =
        awaitJob(staleFilter.result.value(QStringLiteral("job_id")).toString());
    require(staleResult.value(QStringLiteral("status")) == QStringLiteral("failed") &&
                staleResult.value(QStringLiteral("result"))
                        .toObject()
                        .value(QStringLiteral("error"))
                        .toObject()
                        .value(QStringLiteral("code")) == QStringLiteral("stale_revision"),
            "detector result commits only against its original source revision");
    const auto completedFilter = filter();
    std::exchange(detection, {})({{1, QRectF(10, 10, 20, 15), QStringLiteral("text")}}, {});
    const auto filtered =
        awaitJob(completedFilter.result.value(QStringLiteral("job_id")).toString());
    require(filtered.value(QStringLiteral("status")) == QStringLiteral("completed"),
            "detector commits actual engine filter regions");
    const auto oldRevision = revision;
    const auto filteredState = call(QStringLiteral("document_state"));
    require(filteredState.ok && revision == oldRevision + 1,
            "detector advances document revision once");
    const auto start = call(QStringLiteral("document_recognize"),
                            {{QStringLiteral("kind"), QStringLiteral("text")}});
    require(start.ok, "background recognition starts an owned job");
    const auto job = start.result.value(QStringLiteral("job_id")).toString();
    QElapsedTimer timer;
    timer.start();
    while (registry.get(8, job)->value(QStringLiteral("status")) == QStringLiteral("running") &&
           timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(registry.get(8, job)->value(QStringLiteral("status")) == QStringLiteral("completed"),
            "headless provider result retained");
    const auto recognized = call(QStringLiteral("document_recognition_state"));
    require(recognized.ok && recognized.result.value(QStringLiteral("text"))
                                 .toString()
                                 .contains(QStringLiteral("Fixture")),
            "recognition state is retained beyond job completion");
    const auto edited = call(QStringLiteral("document_edit_recognition"),
                             {{QStringLiteral("expected_recognition_revision"), 1},
                              {QStringLiteral("action"), QStringLiteral("set_text")},
                              {QStringLiteral("text"), QStringLiteral("Edited result")}});
    require(edited.ok &&
                edited.result.value(QStringLiteral("recognition_revision")).toInteger() == 2,
            "recognition edits have independent optimistic revision");
    require(call(QStringLiteral("document_edit_recognition"),
                 {{QStringLiteral("expected_recognition_revision"), 1},
                  {QStringLiteral("action"), QStringLiteral("reset_text")}})
                    .errorCode == QStringLiteral("stale_revision"),
            "stale recognition edit is rejected");
    const QJsonObject copyText{{QStringLiteral("output"), QStringLiteral("copy")},
                               {QStringLiteral("format"), QStringLiteral("text")}};
    require(call(QStringLiteral("document_export_recognition"), copyText)
                    .result.value(QStringLiteral("copied"))
                    .toBool() &&
                clipboard->mimePublications == 1 && clipboard->mime &&
                clipboard->mime->text() == QStringLiteral("Edited result"),
            "recognition copy publishes edited text through the same clipboard port");
    for (const bool rejected : {false, true}) {
        clipboard->reject = rejected;
        clipboard->failure = ScreenshotClipboardCommitFailure::PublishFailed;
        require(call(QStringLiteral("document_copy")).errorCode ==
                        QStringLiteral("clipboard_failed") &&
                    call(QStringLiteral("document_export_recognition"), copyText).errorCode ==
                        QStringLiteral("clipboard_failed") &&
                    clipboard->imagePublications == 1 && clipboard->mimePublications == 1,
                "image and text copy report rejection and publication failure without success");
    }
    clipboard->reject = false;
    clipboard->failure = ScreenshotClipboardCommitFailure::None;
    clipboard->automatic = false;
    for (const bool text : {false, true}) {
        ScreenshotMcpRequest request;
        request.connectionId = 8;
        request.requestId = QString::number(++sequence);
        request.idempotencyKey = request.requestId;
        request.expectedRevision = revision;
        request.method = text ? QStringLiteral("snow_shot_document_export_recognition")
                              : QStringLiteral("snow_shot_document_copy");
        request.params = text ? copyText : QJsonObject{};
        request.params.insert(QStringLiteral("document_id"), id);
        int completions = 0;
        service.request(request, [&](ScreenshotMcpResponse response) {
            require(!response.ok, "canceled clipboard requests must never report success");
            ++completions;
        });
        timer.restart();
        while (clipboard->pending.isEmpty() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(clipboard->pending.size() == 1 && completions == 0,
                "copy waits for clipboard publication before replying");
        const auto publication = clipboard->pending.first();
        const int cancellations = clipboard->cancellations;
        require(service.cancelRequest(8, request.requestId) && publication->canceled &&
                    clipboard->cancellations == cancellations + 1 && completions == 1,
                "canceling image and text copies cancels the external publication exactly once");
        clipboard->complete(publication);
        require(completions == 1 && clipboard->imagePublications == 1 &&
                    clipboard->mimePublications == 1,
                "a canceled publication cannot write or complete twice");
    }
    clipboard->automatic = true;
    require(call(QStringLiteral("document_export_recognition"),
                 {{QStringLiteral("output"), QStringLiteral("return")},
                  {QStringLiteral("format"), QStringLiteral("text")}})
                    .result.value(QStringLiteral("text")) == QStringLiteral("Edited result"),
            "edited recognition exports canonical text");
    require(call(QStringLiteral("document_set_selection"),
                 {{QStringLiteral("bounds"), QJsonArray{0, 0, 40, 30}}})
                .ok,
            "selection invalidates derived recognition");
    require(call(QStringLiteral("document_recognition_state")).errorCode ==
                QStringLiteral("recognition_required"),
            "stale recognition is unavailable after document change");
    const auto largeJob = registry.start(8, QStringLiteral("large"));
    require(registry.complete(largeJob,
                              {{QStringLiteral("text"), QString(1100000, QLatin1Char('x'))}}) &&
                registry.get(8, largeJob)
                    ->value(QStringLiteral("result"))
                    .toObject()
                    .contains(QStringLiteral("artifact")),
            "large job results externalize losslessly into owned artifacts");
    const auto opening = call(QStringLiteral("document_open"),
                              {{QStringLiteral("source"), QStringLiteral("clipboard")},
                               {QStringLiteral("as_job"), true}});
    require(opening.ok && opening.result.contains(QStringLiteral("job_id")),
            "asynchronous open returns an owned job before source processing completes");
    const auto openResult = awaitJob(opening.result.value(QStringLiteral("job_id")).toString());
    require(openResult.value(QStringLiteral("status")) == QStringLiteral("completed") &&
                !openResult.value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("document_id"))
                     .toString()
                     .isEmpty(),
            "open job retains full resulting document identity");
    const int cancellationsBeforeOpen = sourceCancellations;
    const auto delayed = call(QStringLiteral("document_open"),
                              {{QStringLiteral("source"), QStringLiteral("capture")},
                               {QStringLiteral("delay_seconds"), 1}});
    require(delayed.ok && delayed.result.contains(QStringLiteral("job_id")) && pendingSource,
            "delayed capture defaults to an owned asynchronous job");
    require(registry.cancel(8, delayed.result.value(QStringLiteral("job_id")).toString()) &&
                sourceCancellations == cancellationsBeforeOpen + 1,
            "open job cancellation reaches the source acquisition port");
    McpDocumentService::Source lateSource;
    lateSource.image = image;
    std::exchange(pendingSource, {})(std::move(lateSource), {});
    require(call(QStringLiteral("document_list"))
                    .result.value(QStringLiteral("documents"))
                    .toArray()
                    .size() == 2,
            "late canceled source cannot allocate an orphan document");
    clipboard->automatic = false;
    ScreenshotMcpRequest pendingCopy;
    pendingCopy.connectionId = 8;
    pendingCopy.requestId = QString::number(++sequence);
    pendingCopy.idempotencyKey = pendingCopy.requestId;
    pendingCopy.expectedRevision = revision;
    pendingCopy.method = QStringLiteral("snow_shot_document_copy");
    pendingCopy.params = {{QStringLiteral("document_id"), id}};
    int copyCompletions = 0;
    service.request(pendingCopy, [&](auto) { ++copyCompletions; });
    timer.restart();
    while (clipboard->pending.isEmpty() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(clipboard->pending.size() == 1 && copyCompletions == 0,
            "hold publication before disconnecting its owner");
    const auto publication = clipboard->pending.first();
    service.disconnected(8);
    require(publication->canceled, "disconnect cancels admitted clipboard work");
    clipboard->complete(publication);
    require(copyCompletions == 0 && clipboard->imagePublications == 1,
            "disconnected clients cannot publish or receive late clipboard replies");
    service.shutdown();
}
void documentClipboardTeardown() {
    for (const bool destroy : {false, true}) {
        const auto clipboard = std::make_shared<MemoryClipboard>();
        clipboard->automatic = false;
        McpDocumentService::Ports ports;
        ports.clipboard = clipboard;
        ports.resolveSource = [](const ScreenshotMcpRequest&, auto completion, auto budget) {
            QImage image(4, 4, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::red);
            require(budget(image.sizeInBytes()), "teardown source fits the document budget");
            McpDocumentService::Source source;
            source.image = image;
            completion(std::move(source), {});
        };
        auto service = std::make_unique<McpDocumentService>(std::move(ports));
        const auto waitUntil = [](auto ready) {
            QElapsedTimer timer;
            timer.start();
            while (!ready() && timer.elapsed() < 5000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            require(ready(), "clipboard teardown request must reach its expected state");
        };
        ScreenshotMcpRequest request;
        request.connectionId = 11;
        request.requestId = QStringLiteral("open");
        request.idempotencyKey = request.requestId;
        request.method = QStringLiteral("snow_shot_document_open");
        request.params = {{QStringLiteral("source"), QStringLiteral("clipboard")}};
        std::optional<ScreenshotMcpResponse> opened;
        service->request(request, [&](auto response) { opened = std::move(response); });
        waitUntil([&] { return opened.has_value(); });
        require(opened->ok, "create the teardown document");
        request.requestId = QStringLiteral("copy");
        request.idempotencyKey = request.requestId;
        request.expectedRevision = opened->revision;
        request.method = QStringLiteral("snow_shot_document_copy");
        request.params = {
            {QStringLiteral("document_id"), opened->result.value(QStringLiteral("document_id"))}};
        int completions = 0;
        service->request(request, [&](auto) { ++completions; });
        waitUntil([&] { return !clipboard->pending.isEmpty(); });
        const auto publication = clipboard->pending.first();
        require(completions == 0, "publication is pending at teardown");
        if (destroy)
            service.reset();
        else
            service->shutdown();
        require(publication->canceled && clipboard->cancellations == 1,
                "shutdown and destruction cancel outstanding publications");
        clipboard->complete(publication);
        require(completions == 0 && clipboard->imagePublications == 0,
                "teardown prevents late clipboard writes and callbacks");
    }
}

void jobs() {
    McpJobRegistry registry;
    require(!registry.hasRunningJobs(), "empty registry permits storage migration");
    QString id;
    int canceled = 0;
    id = registry.start(1, QStringLiteral("test"), [&] {
        ++canceled;
        require(!registry.complete(id, {{QStringLiteral("late"), true}}),
                "late synchronous completion cannot resurrect canceled job");
    });
    require(registry.hasRunningJobs(), "running jobs block storage migration");
    require(!id.isEmpty() && registry.list(1).size() == 1 && registry.list(2).isEmpty(),
            "jobs are independently owned");
    require(!registry.get(2, id) && !registry.cancel(2, id), "job lookup enforces owner");
    require(registry.cancel(1, id) && canceled == 1 && registry.cancel(1, id) && canceled == 1,
            "job cancellation is idempotent and retires callback first");
    require(!registry.hasRunningJobs(), "canceled jobs no longer block migration");
    const auto committed = registry.start(1, QStringLiteral("committed"));
    require(registry.retainInput(committed,
                                 {{QStringLiteral("secret_input"), QStringLiteral("private")}}) &&
                registry.input(1, committed).has_value() && !registry.input(2, committed) &&
                !QJsonDocument(*registry.get(1, committed)).toJson().contains("secret_input"),
            "retry input remains private and owner scoped");
    require(!registry.retainInput(committed, {{QStringLiteral("text"), QString(1048576, u'x')}}),
            "private retry input has a bounded allocation");
    require(!registry.cancel(1, committed), "committed jobs cannot pretend to cancel");
    require(registry.complete(committed, {{QStringLiteral("saved"), true}}),
            "committed job completes");
    require(registry.input(1, committed)->value(QStringLiteral("secret_input")) ==
                QStringLiteral("private"),
            "terminal result preserves bounded private retry input");
    const auto removed = registry.start(1, QStringLiteral("disconnect"), [&] { ++canceled; });
    registry.disconnected(1);
    require(canceled == 2 && registry.list(1).isEmpty() && !registry.complete(removed, {}),
            "disconnect cancels and retires all owned jobs");
    for (int index = 0; index < 8; ++index)
        require(!registry.start(3, QStringLiteral("bounded")).isEmpty(), "bounded jobs admitted");
    require(registry.start(3, QStringLiteral("overflow")).isEmpty(), "job queue is bounded");
    const auto reserved = registry.start(9, QStringLiteral("internal"), [&] { ++canceled; });
    int retired = 0;
    QObject::connect(&registry, &McpJobRegistry::changed, &registry,
                     [&](quint64 owner, const QString& changedId) {
                         if (owner == 9 && changedId == reserved && !registry.get(owner, changedId))
                             ++retired;
                     });
    const auto beforeDiscard = canceled;
    require(!registry.discard(1, reserved) && registry.discard(9, reserved) &&
                canceled == beforeDiscard && retired == 1 && !registry.get(9, reserved),
            "unpublished job reservations retire privately without invoking cancellation");
    qint64 now = 1700000000000;
    McpJobRegistry expiring(nullptr, [&] { return now; });
    int expiredNotifications = 0;
    QObject::connect(&expiring, &McpJobRegistry::changed, &expiring,
                     [&](quint64 owner, const QString& changedId) {
                         if (!expiring.get(owner, changedId))
                             ++expiredNotifications;
                     });
    QString first;
    for (int index = 0; index < 64; ++index) {
        const auto retained = expiring.start(4, QStringLiteral("retained"));
        require(!retained.isEmpty() && expiring.complete(retained, {}),
                "terminal retention slots admitted");
        if (first.isEmpty())
            first = retained;
    }
    require(expiring.get(4, first).has_value() &&
                expiring.start(4, QStringLiteral("overflow")).isEmpty(),
            "nonexpired terminal jobs are never evicted to admit new work");
    now += 900000;
    require(!expiring.get(4, first) && !expiring.complete(first, {}) && !expiring.cancel(4, first),
            "expired jobs cannot be read or resurrected");
    int expiredCancellation = 0;
    const auto waiting =
        expiring.start(4, QStringLiteral("pending"), [&] { ++expiredCancellation; });
    require(!waiting.isEmpty() && expiredNotifications == 64,
            "expiry releases retained capacity and notifies observers after removal");
    now += 900000;
    require(!expiring.start(5, QStringLiteral("trigger_trim")).isEmpty() &&
                expiredCancellation == 1 && expiredNotifications == 65,
            "expiry cancels pending work exactly once after retiring its record");
}
void eraserDocuments(const QString& directory) {
    QImage image(100, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(20, 40, 70));
    const QString path = QDir(directory).filePath(QStringLiteral("eraser-source.png"));
    require(image.save(path), "write eraser background");
    McpJobRegistry registry;
    McpDocumentService::Ports ports;
    ports.jobs = &registry;
    McpDocumentService service(std::move(ports));
    quint64 sequence = 0;
    QString id;
    quint64 revision = 1;
    const auto call = [&](QString method, QJsonObject parameters = {}, bool mutation = false) {
        ScreenshotMcpRequest request;
        request.connectionId = 1;
        request.requestId = QString::number(++sequence);
        request.idempotencyKey = request.requestId;
        request.method = std::move(method);
        if (!id.isEmpty()) {
            parameters.insert(QStringLiteral("document_id"), id);
            request.expectedRevision = revision;
        }
        request.params = std::move(parameters);
        std::optional<ScreenshotMcpResponse> response;
        service.request(request, [&](auto result) { response = std::move(result); });
        QElapsedTimer elapsed;
        elapsed.start();
        while (!response && elapsed.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(response.has_value(), "eraser document command completes");
        if (mutation && response->ok)
            revision = response->revision.value_or(revision);
        return *response;
    };
    const auto opened =
        call(QStringLiteral("snow_shot_document_open"), {{QStringLiteral("path"), path}});
    require(opened.ok, "open eraser document");
    id = opened.result.value(QStringLiteral("document_id")).toString();
    const auto render = [&] {
        const auto response = call(QStringLiteral("snow_shot_document_render"));
        require(response.ok && response.attachmentMime == QStringLiteral("image/png"),
                "eraser document renders a PNG attachment at the current revision");
        const auto rendered = QImage::fromData(response.attachment);
        require(rendered.size() == image.size(),
                "eraser document attachment decodes to the original canvas size");
        return rendered;
    };
    require(
        call(QStringLiteral("snow_shot_document_apply_annotations"),
             {{QStringLiteral("operations"),
               QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                      {QStringLiteral("bounds"), QJsonArray{10, 10, 60, 50}}}}}},
             true)
            .ok,
        "create annotation under eraser masks");
    const auto before = render();
    require(before.pixelColor(10, 25) != image.pixelColor(10, 25) &&
                before.pixelColor(70, 25) != image.pixelColor(70, 25),
            "annotation changes local and distant source pixels");
    for (const QString& tool :
         {QStringLiteral("rectangle_eraser"), QStringLiteral("brush_eraser")}) {
        require(call(QStringLiteral("snow_shot_document_set_tool"),
                     {{QStringLiteral("tool"), tool}}, true)
                    .ok,
                "MCP accepts each new eraser tool");
        if (tool == QStringLiteral("brush_eraser")) {
            require(
                call(QStringLiteral("snow_shot_document_set_tool_style"),
                     {{QStringLiteral("target"), tool},
                      {QStringLiteral("style"), QJsonObject{{QStringLiteral("stroke_width"), 12}}}},
                     true)
                    .ok,
                "brush MCP style accepts creation width");
            const auto invalid =
                call(QStringLiteral("snow_shot_document_set_tool_style"),
                     {{QStringLiteral("target"), tool},
                      {QStringLiteral("style"), QJsonObject{{QStringLiteral("opacity"), 0.5}}}},
                     true);
            require(!invalid.ok, "brush MCP style rejects immutable opacity");
        }
        const QJsonArray points = tool == QStringLiteral("rectangle_eraser")
                                      ? QJsonArray{QJsonArray{5, 20}, QJsonArray{20, 35}}
                                      : QJsonArray{QJsonArray{10, 20}, QJsonArray{10, 35}};
        if (tool == QStringLiteral("rectangle_eraser")) {
            require(!call(QStringLiteral("snow_shot_document_edit_elements"),
                          {{QStringLiteral("action"), QStringLiteral("erase_path")},
                           {QStringLiteral("points"), QJsonArray{QJsonArray{10, 25}}}},
                          true)
                         .ok,
                    "rectangle MCP eraser requires two corners");
        }
        require(call(QStringLiteral("snow_shot_document_edit_elements"),
                     {{QStringLiteral("action"), QStringLiteral("erase_path")},
                      {QStringLiteral("points"), points}},
                     true)
                    .ok,
                "existing erase_path creates the active background eraser");
        const auto erased = render();
        require(erased.pixelColor(10, 25) == image.pixelColor(10, 25) &&
                    erased.pixelColor(70, 25) == before.pixelColor(70, 25),
                "MCP eraser restores local background and preserves distant annotation");
        require(call(QStringLiteral("snow_shot_document_undo"), {}, true).ok,
                "MCP eraser gesture is one undo transaction");
        require(render() == before, "undo restores complete annotation with original geometry");
    }
    service.shutdown();
}

void documents(const QString& directory) {
    QImage image(80, 60, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    const QString sourcePath = QDir(directory).filePath(QStringLiteral("source.png"));
    require(image.save(sourcePath), "write test image");
    McpJobRegistry registry;
    McpDocumentService::Ports ports;
    ports.jobs = &registry;
    qint64 artifactClock = 1700000000000;
    ports.clock = [&] { return artifactClock; };
    McpDocumentService service(std::move(ports));
    quint64 sequence = 0;
    const auto call = [&](QString method, QJsonObject params = {}, quint64 owner = 1,
                          std::optional<quint64> revision = std::nullopt, QString key = {}) {
        ScreenshotMcpRequest request;
        request.connectionId = owner;
        request.requestId = QString::number(++sequence);
        request.idempotencyKey = key.isEmpty() ? request.requestId : key;
        request.expectedRevision = revision;
        request.method = std::move(method);
        request.params = std::move(params);
        std::optional<ScreenshotMcpResponse> result;
        service.request(request, [&](auto response) { result = std::move(response); });
        QElapsedTimer timer;
        timer.start();
        while (!result && timer.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(result.has_value(), "document command completed");
        return *result;
    };
    const auto opened =
        call(QStringLiteral("snow_shot_document_open"), {{QStringLiteral("path"), sourcePath}}, 1,
             {}, QStringLiteral("open-once"));
    require(opened.ok && opened.revision == 1, "open isolated background document");
    const QString id = opened.result.value(QStringLiteral("document_id")).toString();
    const auto duplicate =
        call(QStringLiteral("snow_shot_document_open"), {{QStringLiteral("path"), sourcePath}}, 1,
             {}, QStringLiteral("open-once"));
    require(duplicate.ok && duplicate.result.value(QStringLiteral("document_id")) == id,
            "idempotent open never allocates a second document");
    const auto intruder =
        call(QStringLiteral("snow_shot_document_state"), {{QStringLiteral("document_id"), id}}, 2);
    require(intruder.errorCode == QStringLiteral("document_not_found") &&
                intruder.errorDetails.isEmpty(),
            "private background state is never exposed to another client");
    const auto annotation =
        call(QStringLiteral("snow_shot_document_apply_annotations"),
             {{QStringLiteral("document_id"), id},
              {QStringLiteral("operations"),
               QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                      {QStringLiteral("bounds"), QJsonArray{10, 10, 20, 20}}}}}},
             1, 1);
    require(annotation.ok && annotation.revision == 2, "independent runtime applies annotations");
    const auto stale = call(QStringLiteral("snow_shot_document_undo"),
                            {{QStringLiteral("document_id"), id}}, 1, 1);
    require(stale.errorCode == QStringLiteral("stale_revision"), "stale document edits rejected");
    const auto cloned = call(QStringLiteral("snow_shot_document_clone"),
                             {{QStringLiteral("document_id"), id}}, 1, 2);
    require(cloned.ok && cloned.result.value(QStringLiteral("can_undo")).toBool(),
            "clone preserves independent history");
    const QString clone = cloned.result.value(QStringLiteral("document_id")).toString();
    require(call(QStringLiteral("snow_shot_document_undo"),
                 {{QStringLiteral("document_id"), clone}}, 1, 1)
                .ok,
            "clone can undo its own annotation history");
    require(call(QStringLiteral("snow_shot_document_state"), {{QStringLiteral("document_id"), id}})
                    .revision == 2,
            "clone edits do not affect original revision");
    require(
        call(QStringLiteral("snow_shot_document_apply_annotations"),
             {{QStringLiteral("document_id"), clone},
              {QStringLiteral("operations"),
               QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                      {QStringLiteral("bounds"), QJsonArray{10, 10, 10, 10}}},
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                      {QStringLiteral("bounds"), QJsonArray{50, 10, 10, 10}}}}}},
             1, 2)
            .ok,
        "eraser fixture creates separated elements");
    const auto beforeErase = call(QStringLiteral("snow_shot_document_render"),
                                  {{QStringLiteral("document_id"), clone}}, 1, 3);
    require(call(QStringLiteral("snow_shot_document_edit_elements"),
                 {{QStringLiteral("document_id"), clone},
                  {QStringLiteral("action"), QStringLiteral("erase_path")},
                  {QStringLiteral("points"), QJsonArray{QJsonArray{9, 15}, QJsonArray{21, 15}}}},
                 1, 3)
                .ok,
            "eraser uses pointer gesture geometry");
    const auto erased = call(QStringLiteral("snow_shot_document_render"),
                             {{QStringLiteral("document_id"), clone}}, 1, 4);
    const auto beforePixels = QImage::fromData(beforeErase.attachment);
    const auto erasedPixels = QImage::fromData(erased.attachment);
    require(beforeErase.ok && erased.ok && beforePixels != erasedPixels &&
                erasedPixels.copy(45, 5, 20, 20) == beforePixels.copy(45, 5, 20, 20),
            "eraser removes intersected geometry while preserving distant elements");
    require(call(QStringLiteral("snow_shot_document_undo"),
                 {{QStringLiteral("document_id"), clone}}, 1, 4)
                .ok,
            "eraser is one undoable gesture");
    require(QImage::fromData(call(QStringLiteral("snow_shot_document_render"),
                                  {{QStringLiteral("document_id"), clone}}, 1, 5)
                                 .attachment) == beforePixels,
            "one undo restores the complete erased path transaction");
    require(call(QStringLiteral("snow_shot_document_edit_elements"),
                 {{QStringLiteral("document_id"), clone},
                  {QStringLiteral("action"), QStringLiteral("erase_path")},
                  {QStringLiteral("points"), QJsonArray{QJsonArray{-1, 15}}}},
                 1, 5)
                    .errorCode == QStringLiteral("invalid_parameters"),
            "eraser rejects points outside source canvas");
    const auto selected = call(
        QStringLiteral("snow_shot_document_set_selection"),
        {{QStringLiteral("document_id"), id}, {QStringLiteral("bounds"), QJsonArray{0, 0, 40, 30}}},
        1, 2);
    require(selected.ok && selected.revision == 3, "background selection is independent");
    const auto rendered = call(QStringLiteral("snow_shot_document_render"),
                               {{QStringLiteral("document_id"), id}}, 1, 3);
    require(rendered.ok && QImage::fromData(rendered.attachment).size() == QSize(40, 30),
            "background render uses selected region without overlay widgets");
    require(
        rendered.result.value(QStringLiteral("sha256")).toString() ==
            QString::fromLatin1(
                QCryptographicHash::hash(rendered.attachment, QCryptographicHash::Sha256).toHex()),
        "render digest describes actual bytes");
    const auto cached = call(QStringLiteral("snow_shot_document_render"),
                             {{QStringLiteral("document_id"), id}}, 1, 3);
    require(cached.ok && cached.result.value(QStringLiteral("cache_hit")).toBool() &&
                cached.attachment == rendered.attachment,
            "unchanged background render reuses immutable cached image");
    const QString output = QDir(directory).filePath(QStringLiteral("saved.png"));
    const auto saved =
        call(QStringLiteral("snow_shot_document_save"),
             {{QStringLiteral("document_id"), id}, {QStringLiteral("path"), output}}, 1, 3);
    require(saved.ok && QFileInfo::exists(output),
            "background save writes through canonical file service");
    for (const auto& format :
         {QStringLiteral("png"), QStringLiteral("jpeg"), QStringLiteral("webp"),
          QStringLiteral("jxl"), QStringLiteral("avif"), QStringLiteral("bmp"),
          QStringLiteral("pdf")}) {
        const auto path = QDir(directory).filePath(QStringLiteral("all-formats.") + format);
        const auto exported =
            call(QStringLiteral("snow_shot_document_save"),
                 {{QStringLiteral("document_id"), id},
                  {QStringLiteral("path"), path},
                  {QStringLiteral("format"), format},
                  {QStringLiteral("quality"), 77},
                  {QStringLiteral("compression_level"), QStringLiteral("low")},
                  {QStringLiteral("pdf_page_size"), QStringLiteral("a4_landscape")},
                  {QStringLiteral("pdf_title"), QStringLiteral("MCP document")}},
                 1, 3);
        if (!exported.ok)
            std::cerr << "save failed: " << format.toStdString() << ": "
                      << exported.errorCode.toStdString() << '\n';
        require(exported.ok, "every native file format exports through document service");
        QFile file(exported.result.value(QStringLiteral("path")).toString());
        require(file.open(QIODevice::ReadOnly), "saved file opens");
        const auto bytes = file.readAll();
        require(exported.result.value(QStringLiteral("byte_count")).toInteger() == bytes.size() &&
                    exported.result.value(QStringLiteral("sha256")).toString() ==
                        QString::fromLatin1(
                            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()),
                "saved metadata verifies actual format bytes");
        if (format == QStringLiteral("pdf")) {
            require(bytes.startsWith("%PDF-1.7") &&
                        bytes.contains("/MediaBox [0 0 841.88976378 595.27559055]") &&
                        bytes.contains("/DCTDecode"),
                    "PDF preserves requested landscape layout and lossy quality");
        } else {
            const auto fileFormat = ScreenshotImageFileService::formatForKey(format);
            const auto decoded = snow_shot::image_codec::decodeFile(
                file.fileName(), ScreenshotImageFileService::snowImageFormat(fileFormat));
            require(decoded.size() == QSize(40, 30), "saved image decodes with native codec");
            const auto reopened = call(QStringLiteral("snow_shot_document_open"),
                                       {{QStringLiteral("path"), file.fileName()}});
            if (!reopened.ok ||
                reopened.result.value(QStringLiteral("canvas_bounds")) != QJsonArray{0, 0, 40, 30})
                std::cerr
                    << "reopen failed: " << format.toStdString()
                    << " error=" << reopened.errorCode.toStdString() << " result="
                    << QJsonDocument(reopened.result).toJson(QJsonDocument::Compact).toStdString()
                    << '\n';
            require(reopened.ok && reopened.result.value(QStringLiteral("canvas_bounds")) ==
                                       QJsonArray{0, 0, 40, 30},
                    "every saved raster format reopens through actual document source path");
            require(call(QStringLiteral("snow_shot_document_close"),
                         {{QStringLiteral("document_id"),
                           reopened.result.value(QStringLiteral("document_id"))}},
                         1, 1)
                        .ok,
                    "reopened format document releases its capacity");
            if (format == QStringLiteral("png"))
                require(bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")), "PNG signature");
            else if (format == QStringLiteral("jpeg"))
                require(bytes.startsWith(QByteArray::fromHex("ffd8ff")), "JPEG signature");
            else if (format == QStringLiteral("webp"))
                require(bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP", "WebP signature");
            else if (format == QStringLiteral("jxl"))
                require(bytes.startsWith(QByteArray::fromHex("ff0a")) ||
                            bytes.startsWith(QByteArray::fromHex("0000000c4a584c20")),
                        "JXL signature");
            else if (format == QStringLiteral("avif"))
                require(bytes.mid(4, 4) == "ftyp" && bytes.left(64).contains("avif"),
                        "AVIF signature");
            else if (format == QStringLiteral("bmp"))
                require(bytes.startsWith("BM"), "BMP signature");
        }
    }
    for (const auto& page : {QStringLiteral("image_size"), QStringLiteral("a4_portrait")}) {
        const auto path = QDir(directory).filePath(page + QStringLiteral(".pdf"));
        require(call(QStringLiteral("snow_shot_document_save"),
                     {{QStringLiteral("document_id"), id},
                      {QStringLiteral("path"), path},
                      {QStringLiteral("format"), QStringLiteral("pdf")},
                      {QStringLiteral("pdf_page_size"), page}},
                     1, 3)
                    .ok,
                "PDF page modes all export");
        QFile pdf(path);
        require(pdf.open(QIODevice::ReadOnly), "PDF page mode result opens");
        const auto bytes = pdf.readAll();
        require(bytes.contains(page == QStringLiteral("image_size")
                                   ? "/MediaBox [0 0 30.00000000 22.50000000]"
                                   : "/MediaBox [0 0 595.27559055 841.88976378]"),
                "PDF requested page geometry survives service routing");
    }
    require(call(QStringLiteral("snow_shot_document_save"),
                 {{QStringLiteral("document_id"), id},
                  {QStringLiteral("path"), output},
                  {QStringLiteral("pdf_page_size"), QStringLiteral("invalid")}},
                 1, 3)
                    .errorCode == QStringLiteral("invalid_parameters"),
            "invalid export options are rejected");
    const auto unavailable = call(
        QStringLiteral("snow_shot_document_recognize"),
        {{QStringLiteral("document_id"), id}, {QStringLiteral("kind"), QStringLiteral("text")}}, 1,
        3);
    require(unavailable.errorCode == QStringLiteral("provider_unavailable"),
            "missing providers are reported honestly");
    const QByteArray payload(700000, 'a');
    const auto artifact = service.storeArtifact(1, payload, QStringLiteral("application/json"));
    const auto artifactId = artifact.value(QStringLiteral("artifact_id")).toString();
    require(!artifactId.isEmpty(), "large outputs create an owned bounded artifact");
    require(call(QStringLiteral("snow_shot_artifact_read"),
                 {{QStringLiteral("artifact_id"), artifactId}}, 2)
                    .errorCode == QStringLiteral("artifact_not_found"),
            "artifact ownership is private");
    const auto part =
        call(QStringLiteral("snow_shot_artifact_read"),
             {{QStringLiteral("artifact_id"), artifactId}, {QStringLiteral("max_bytes"), 262144}});
    require(part.ok && part.attachment.size() == 262144 &&
                !part.result.value(QStringLiteral("eof")).toBool(),
            "artifact reads are bounded and resumable");
    require(part.result.value(QStringLiteral("sha256")).toString() ==
                QString::fromLatin1(
                    QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()),
            "artifact digest covers all bytes");
    require(
        call(QStringLiteral("snow_shot_artifact_read"),
             {{QStringLiteral("artifact_id"), artifactId}, {QStringLiteral("max_bytes"), 262145}})
                .errorCode == QStringLiteral("invalid_parameters"),
        "artifact read rejects oversized chunks");
    QByteArray reconstructed = part.attachment;
    while (reconstructed.size() < payload.size()) {
        const auto next = call(QStringLiteral("snow_shot_artifact_read"),
                               {{QStringLiteral("artifact_id"), artifactId},
                                {QStringLiteral("offset"), reconstructed.size()},
                                {QStringLiteral("max_bytes"), 262144}});
        require(next.ok && !next.attachment.isEmpty(),
                "artifact chunk resumes at exact byte offset");
        reconstructed.append(next.attachment);
    }
    require(reconstructed == payload, "artifact chunks reconstruct exact original bytes");
    const auto eof = call(
        QStringLiteral("snow_shot_artifact_read"),
        {{QStringLiteral("artifact_id"), artifactId}, {QStringLiteral("offset"), payload.size()}});
    require(eof.ok && eof.attachment.isEmpty() && eof.result.value(QStringLiteral("eof")).toBool(),
            "artifact EOF is an explicit empty binary chunk");
    const auto mediaPath = QDir(directory).filePath(QStringLiteral("recording-fixture.bin"));
    QFile media(mediaPath);
    require(media.open(QIODevice::WriteOnly) && media.write(payload) == payload.size(),
            "file artifact source");
    media.close();
    std::optional<QJsonObject> fileArtifact;
    service.storeFileArtifact(1, mediaPath, QStringLiteral("video/mp4"),
                              [&](QJsonObject value, QString error) {
                                  require(error.isEmpty(), "file artifact copy succeeds");
                                  fileArtifact = std::move(value);
                              });
    QElapsedTimer fileWait;
    fileWait.start();
    while (!fileArtifact && fileWait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(fileArtifact && !fileArtifact->isEmpty(),
            "file artifact snapshots immutable owned storage");
    require(media.open(QIODevice::WriteOnly | QIODevice::Truncate) && media.write("changed") == 7,
            "original file changes independently");
    media.close();
    const auto fileId = fileArtifact->value(QStringLiteral("artifact_id")).toString();
    const auto filePart =
        call(QStringLiteral("snow_shot_artifact_read"),
             {{QStringLiteral("artifact_id"), fileId}, {QStringLiteral("offset"), 660000}});
    require(filePart.ok && filePart.attachment == payload.mid(660000) &&
                filePart.result.value(QStringLiteral("sha256")) ==
                    part.result.value(QStringLiteral("sha256")) &&
                !QJsonDocument(filePart.result).toJson().contains("snow-shot-mcp-artifact-"),
            "file artifact reads immutable bytes without exposing private filesystem path");
    require(call(QStringLiteral("snow_shot_artifact_read"),
                 {{QStringLiteral("artifact_id"), fileId}}, 2)
                    .errorCode == QStringLiteral("artifact_not_found"),
            "file artifacts enforce owner isolation");
    require(call(QStringLiteral("snow_shot_artifact_release"),
                 {{QStringLiteral("artifact_id"), fileId}})
                    .ok &&
                call(QStringLiteral("snow_shot_artifact_read"),
                     {{QStringLiteral("artifact_id"), fileId}})
                        .errorCode == QStringLiteral("artifact_not_found"),
            "releasing file artifact retires its handle");
    for (int index = 0; index < 16; ++index)
        require(!service.storeArtifact(7, QByteArray("x"), QStringLiteral("text/plain")).isEmpty(),
                "per-owner artifact slots admitted");
    require(service.storeArtifact(7, QByteArray("x"), QStringLiteral("text/plain")).isEmpty(),
            "artifact count quota rejects allocation");
    QString fileError;
    service.storeFileArtifact(
        7, mediaPath, QStringLiteral("video/mp4"), [&](QJsonObject value, QString error) {
            require(value.isEmpty(), "capacity failure has no artifact handle");
            fileError = std::move(error);
        });
    require(fileError == QStringLiteral("capacity_exceeded"),
            "file artifact capacity is rejected explicitly before copying");
    service.disconnected(7);
    service.storeFileArtifact(1, QDir(directory).filePath(QStringLiteral("missing.mp4")),
                              QStringLiteral("video/mp4"),
                              [&](QJsonObject, QString error) { fileError = std::move(error); });
    require(fileError == QStringLiteral("file_unavailable"),
            "missing artifact source has stable error");
    artifactClock += 900000;
    require(call(QStringLiteral("snow_shot_artifact_read"),
                 {{QStringLiteral("artifact_id"), artifactId}})
                    .errorCode == QStringLiteral("artifact_not_found"),
            "artifact expires at its advertised lifetime without waiting for timer tick");
    service.disconnected(1);
    require(call(QStringLiteral("snow_shot_document_list"), {}, 2)
                .result.value(QStringLiteral("documents"))
                .toArray()
                .isEmpty(),
            "disconnect releases owned documents without affecting other clients");
    require(call(QStringLiteral("snow_shot_document_state"), {{QStringLiteral("document_id"), id}})
                    .errorCode == QStringLiteral("document_not_found"),
            "disconnected document handle expires");
    require(call(QStringLiteral("snow_shot_artifact_read"),
                 {{QStringLiteral("artifact_id"), artifactId}})
                    .errorCode == QStringLiteral("artifact_not_found"),
            "disconnect releases artifacts");
    service.shutdown();
}
} // namespace
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated document test directory");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "isolated application directory");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({executable, temporary.path(), 60000})
                .success,
            "isolated application storage");
    const qsizetype serveIndex = application.arguments().indexOf(QStringLiteral("--serve"));
    if (serveIndex >= 0 && application.arguments().size() > serveIndex + 1) {
        const int result = serve(application, application.arguments().at(serveIndex + 1));
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return result;
    }
    if (application.arguments().contains(QStringLiteral("--application-only"))) {
        runMcpApplicationTests();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--eraser-only"))) {
        eraserDocuments(temporary.path());
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--activity-only"))) {
        documentMemoryActivity();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--recognition-models-only"))) {
        recognitionModelDocuments(temporary.path());
        recognitionModelSourceBudget();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    jobs();
    if (application.arguments().contains(QStringLiteral("--jobs-only"))) {
        snow_shot::storage::ApplicationStorage::instance().shutdown();
        return 0;
    }
    orientedFileDocuments(temporary.path());
    documentReservations(temporary.path());
    documentCacheWork(temporary.path());
    documentConcurrency();
    documentMemoryActivity();
    documents(temporary.path());
    documentWorkflows();
    recognitionModelDocuments(temporary.path());
    recognitionModelSourceBudget();
    documentClipboardTeardown();
    runMcpApplicationTests();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
