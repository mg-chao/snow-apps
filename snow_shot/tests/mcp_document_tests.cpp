#include "snow_shot/app/mcp/mcpdocumentservice.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/app/mcp/mcpapplicationservice.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/translation/translationservice.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snowimageqtcodec.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QSet>
#include <QSaveFile>
#include <QJsonDocument>
#include <QSemaphore>
#include <QBuffer>
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
    RequestToken recognize(QImage, QObject* receiver, Completion completion) override {
        const auto token = ++m_next;
        QTimer::singleShot(100, receiver, [this, token, completion = std::move(completion)] {
            if (!m_canceled.remove(token))
                completion({{QStringLiteral("fixture-qr-payload")}, {}});
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
                {QStringLiteral("document_renders"), fixtureRenders.load()},
                {QStringLiteral("png_encodes"), fixtureEncodes.load()}};
            if (const auto currentCpu = currentThreadCpuMilliseconds(); currentCpu && initialCpu) {
                const auto cpu = *currentCpu - *initialCpu;
                values.insert(QStringLiteral("cpu_ms"), cpu);
                values.insert(QStringLiteral("cpu_utilization"), elapsed > 0 ? cpu / elapsed : 0);
            }
            metrics.write(QJsonDocument(values).toJson(QJsonDocument::Compact));
            static_cast<void>(metrics.commit());
        }
    });
    heartbeat.start(10);
    server.setRequestHandler([&](const ScreenshotMcpRequest& request, auto completion) {
        if (request.method == QStringLiteral("snow_shot_status")) {
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
void documentReservations(const QString& directory) {
    QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    const auto smallPath = QDir(directory).filePath(QStringLiteral("reservation-small.png"));
    const auto largePath = QDir(directory).filePath(QStringLiteral("reservation-large-header.png"));
    require(image.save(smallPath), "reservation source image");
    QFile small(smallPath);
    require(small.open(QIODevice::ReadOnly), "reservation source bytes");
    auto header = small.readAll();
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
        const auto cold = call(QStringLiteral("snow_shot_document_render"), document);
        require(cold.ok && renders == 1 && encodes == 1,
                "cold render reaches exactly one real raster and encoder boundary");
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
void documentWorkflows() {
    FixtureOcr ocr;
    FixtureQr qr;
    McpJobRegistry registry;
    McpDocumentService::Ports ports;
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
    const auto delayed = call(QStringLiteral("document_open"),
                              {{QStringLiteral("source"), QStringLiteral("capture")},
                               {QStringLiteral("delay_seconds"), 1}});
    require(delayed.ok && delayed.result.contains(QStringLiteral("job_id")) && pendingSource,
            "delayed capture defaults to an owned asynchronous job");
    require(registry.cancel(8, delayed.result.value(QStringLiteral("job_id")).toString()) &&
                sourceCancellations == 1,
            "open job cancellation reaches the source acquisition port");
    McpDocumentService::Source lateSource;
    lateSource.image = image;
    std::exchange(pendingSource, {})(std::move(lateSource), {});
    require(call(QStringLiteral("document_list"))
                    .result.value(QStringLiteral("documents"))
                    .toArray()
                    .size() == 2,
            "late canceled source cannot allocate an orphan document");
    service.disconnected(8);
    service.shutdown();
}
void jobs() {
    McpJobRegistry registry;
    QString id;
    int canceled = 0;
    id = registry.start(1, QStringLiteral("test"), [&] {
        ++canceled;
        require(!registry.complete(id, {{QStringLiteral("late"), true}}),
                "late synchronous completion cannot resurrect canceled job");
    });
    require(!id.isEmpty() && registry.list(1).size() == 1 && registry.list(2).isEmpty(),
            "jobs are independently owned");
    require(!registry.get(2, id) && !registry.cancel(2, id), "job lookup enforces owner");
    require(registry.cancel(1, id) && canceled == 1 && registry.cancel(1, id) && canceled == 1,
            "job cancellation is idempotent and retires callback first");
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
    const int serveIndex = application.arguments().indexOf(QStringLiteral("--serve"));
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
    jobs();
    documentReservations(temporary.path());
    documentCacheWork(temporary.path());
    documentConcurrency();
    documents(temporary.path());
    documentWorkflows();
    runMcpApplicationTests();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
