// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotocrproxyconfiguration.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#ifdef Q_OS_WIN
#include <QTcpServer>
#include <QTcpSocket>
#endif

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

void savedProxyAppliesWithoutScreenshotConsumers() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated OCR proxy settings directory");
    const QString path = temporary.filePath(QStringLiteral("config.json"));
    const QString address = QStringLiteral("http://user:p%40ss@[::1]:7890");
    {
        snow_shot::storage::ConfigurationStore settings(path, true, true);
        require(settings.setValues({{QStringLiteral("network/proxy"), QStringLiteral("manual")},
                                    {QStringLiteral("network/http_proxy"), address}}) &&
                    settings.flushNow().success,
                "manual proxy settings persist before service startup");
    }

    snow_shot::storage::ConfigurationStore settings(path, true, true);
    ScreenshotOcrRecognitionService service;
    snow_shot::presentation::bindOcrProxyConfiguration(service, settings);
    require(service.proxyUrl() == address,
            "reloaded proxy applies before any screenshot consumer or OCR request exists");

    require(settings.setValue(QStringLiteral("network/http_proxy"),
                              QStringLiteral("proxy.example:8080")) &&
                service.proxyUrl() == QStringLiteral("http://proxy.example:8080"),
            "address edits update the shared service without a screenshot controller");
    require(settings.setValue(QStringLiteral("network/proxy"), QStringLiteral("none")) &&
                service.proxyUrl().isEmpty(),
            "direct mode clears the active OCR proxy");
    require(settings.setValue(QStringLiteral("network/http_proxy"), address) &&
                service.proxyUrl().isEmpty(),
            "editing a retained manual address does not enable it in direct mode");
    require(settings.setValue(QStringLiteral("network/proxy"), QStringLiteral("manual")) &&
                service.proxyUrl() == address,
            "manual mode restores the retained address");
    require(settings.setValue(QStringLiteral("network/http_proxy"), QString()) &&
                service.proxyUrl().isEmpty(),
            "clearing a manual address clears the OCR proxy");

    service.suspendStorage();
    service.drainStorage();
    require(settings.setValue(QStringLiteral("network/http_proxy"), address) &&
                service.proxyUrl() == address,
            "proxy changes are retained while OCR storage is suspended");
    service.resumeStorage(temporary.filePath(QStringLiteral("relocated-cache")));
    require(service.proxyUrl() == address && service.processId() == 0,
            "storage relocation retains the latest proxy without starting an OCR worker");
    service.suspendStorage();
    service.drainStorage();
    require(settings.setValue(QStringLiteral("network/proxy"), QStringLiteral("none")),
            "proxy can be disabled while storage is suspended");
    service.resumeStorage(temporary.filePath(QStringLiteral("second-cache")));
    require(service.proxyUrl().isEmpty(), "relocation does not restore an obsolete proxy");
    {
        ScreenshotOcrRecognitionService temporaryService;
        snow_shot::presentation::bindOcrProxyConfiguration(temporaryService, settings);
    }
    require(settings.setValue(QStringLiteral("network/proxy"), QStringLiteral("manual")) &&
                service.proxyUrl() == address,
            "destroying another bound service safely disconnects its subscription");
}

#ifdef Q_OS_WIN
bool waitUntil(const std::function<bool()>& condition) {
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (condition())
            loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    poll.start();
    if (!condition())
        loop.exec();
    return condition();
}

class RejectingProxy {
  public:
    RejectingProxy() {
        require(server.listen(QHostAddress::LocalHost), "local OCR proxy fixture listens");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(
                    socket, &QTcpSocket::readyRead, socket,
                    [this, socket, buffer = QByteArray()]() mutable {
                        buffer += socket->readAll();
                        if (!buffer.contains("\r\n\r\n"))
                            return;
                        require(buffer.startsWith("CONNECT ocr-proxy.invalid:443 HTTP/1.1"),
                                "OCR HTTPS downloads use the configured proxy");
                        ++requests;
                        socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                                      "Connection: close\r\n\r\n");
                        socket->disconnectFromHost();
                    });
            }
        });
    }

    QString address() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }

    int requests = 0;

  private:
    QTcpServer server;
};

void downloadsUseTheSavedAndUpdatedProxy() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated OCR download fixture");
    QFile manifest(QDir(QCoreApplication::applicationDirPath())
                       .filePath(QStringLiteral("assets/ocr/asset-manifest.json")));
    require(manifest.open(QIODevice::ReadOnly), "staged OCR descriptor is available");
    auto descriptor = QJsonDocument::fromJson(manifest.readAll()).object();
    auto models = descriptor.value(QStringLiteral("models")).toArray();
    for (qsizetype index = 0; index < models.size(); ++index) {
        auto model = models.at(index).toObject();
        auto files = model.value(QStringLiteral("files")).toArray();
        for (qsizetype fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
            auto file = files.at(fileIndex).toObject();
            file.insert(QStringLiteral("url"),
                        QStringLiteral("https://ocr-proxy.invalid/%1")
                            .arg(file.value(QStringLiteral("name")).toString()));
            files[fileIndex] = file;
        }
        model.insert(QStringLiteral("files"), files);
        models[index] = model;
    }
    descriptor.insert(QStringLiteral("models"), models);
    QFile fixture(temporary.filePath(QStringLiteral("asset-manifest.json")));
    require(fixture.open(QIODevice::WriteOnly), "isolated OCR descriptor is writable");
    const auto bytes = QJsonDocument(descriptor).toJson();
    require(fixture.write(bytes) == bytes.size(), "isolated OCR descriptor is complete");
    fixture.close();

    RejectingProxy first;
    RejectingProxy second;
    const QString path = temporary.filePath(QStringLiteral("config.json"));
    {
        snow_shot::storage::ConfigurationStore settings(path, true, true);
        require(settings.setValues({{QStringLiteral("network/proxy"), QStringLiteral("manual")},
                                    {QStringLiteral("network/http_proxy"), first.address()}}) &&
                    settings.flushNow().success,
                "save the proxy before starting the shared OCR service");
    }
    snow_shot::storage::ConfigurationStore settings(path, true, true);
    ScreenshotOcrRecognitionService::Options options;
    options.offlineRoot = temporary.path();
    options.cacheRoot = temporary.filePath(QStringLiteral("cache"));
    ScreenshotOcrRecognitionService service(options);
    snow_shot::presentation::bindOcrProxyConfiguration(service, settings);
    const auto download = [&service](RejectingProxy& proxy) {
        const int before = proxy.requests;
        QObject receiver;
        bool completed = false;
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        require(service.recognize({image, QRectF(0, 0, 16, 16)}, &receiver,
                                  [&](ScreenshotOcrRecognitionResult result) {
                                      require(
                                          !result.error.isEmpty(),
                                          "fixture rejects the download without fetching assets");
                                      completed = true;
                                  }) != 0,
                "OCR request starts component acquisition");
        require(waitUntil([&] { return completed; }) && proxy.requests > before,
                "OCR component acquisition reaches the selected local proxy");
    };
    download(first);
    require(settings.setValue(QStringLiteral("network/http_proxy"), second.address()),
            "change the active OCR proxy");
    download(second);
    service.suspendStorage();
    service.drainStorage();
    require(settings.setValue(QStringLiteral("network/http_proxy"), first.address()),
            "change the proxy during OCR storage relocation");
    service.resumeStorage(temporary.filePath(QStringLiteral("relocated-cache")));
    download(first);
}
#endif
} // namespace

void ocrProxyConfigurationTests() {
    savedProxyAppliesWithoutScreenshotConsumers();
#ifdef Q_OS_WIN
    downloadsUseTheSavedAndUpdatedProxy();
#endif
}
