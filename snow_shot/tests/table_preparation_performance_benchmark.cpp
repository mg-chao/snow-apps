#include "snow_shot/network/snowshotapiclient.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <iostream>

namespace {
#ifdef NDEBUG
int benchmarkStreams() {
    constexpr int frames = 8192;
    const QByteArray frame = "data: {\"choices\":[{\"delta\":{\"content\":\"1\"}}]}\n\n";
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (const bool table : {true, false}) {
        for (int sample = 0; sample < 3; ++sample) {
            const QString prefix =
                table ? QStringLiteral("<table><tr><td>") : QStringLiteral("x^{");
            const QString suffix =
                table ? QStringLiteral("</td></tr></table>") : QStringLiteral("}");
            const auto content = [](const QString& text) {
                return "data: " +
                       QJsonDocument(
                           QJsonObject{{QStringLiteral("choices"),
                                        QJsonArray{QJsonObject{
                                            {QStringLiteral("delta"),
                                             QJsonObject{{QStringLiteral("content"), text}}}}}}})
                           .toJson(QJsonDocument::Compact) +
                       "\n\n";
            };
            const QByteArray burst =
                content(prefix) + frame.repeated(frames) + content(suffix) + "data: [DONE]\n\n";
            QTcpServer server;
            if (!server.listen(QHostAddress::LocalHost))
                return 4;
            QEventLoop loop;
            QElapsedTimer elapsed;
            QTimer heartbeat;
            heartbeat.setTimerType(Qt::PreciseTimer);
            double largestGap = 0;
            qint64 previousTick = 0;
            int ticks = 0;
            QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] {
                const qint64 now = elapsed.nsecsElapsed();
                largestGap = std::max(largestGap, double(now - previousTick) / 1e6);
                previousTick = now;
                ++ticks;
            });
            QObject::connect(&server, &QTcpServer::newConnection, &loop, [&] {
                auto* socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, &loop, [&, socket] {
                    QByteArray request =
                        socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    const qsizetype end = request.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("answered").toBool())
                        return;
                    qsizetype length = 0;
                    for (const auto& line : request.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    if (request.size() < end + 4 + length)
                        return;
                    socket->setProperty("answered", true);
                    elapsed.start();
                    heartbeat.start(1);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Content-Length: " +
                                  QByteArray::number(burst.size()) +
                                  "\r\nConnection: close\r\n\r\n" + burst);
                    socket->disconnectFromHost();
                });
            });
            SnowShotApiClient client(
                QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
            bool completed = false;
            const auto completion = [&](auto result) {
                if constexpr (requires { result.html; })
                    completed = result.succeeded() &&
                                result.html == prefix + QString(frames, u'1') + suffix;
                else
                    completed = result.succeeded() &&
                                result.latex == prefix + QString(frames, u'1') + suffix;
                const qint64 now = elapsed.nsecsElapsed();
                largestGap = std::max(largestGap, double(now - previousTick) / 1e6);
                heartbeat.stop();
                loop.quit();
            };
            const auto token = table ? client.extractTableVision(image, QStringLiteral("benchmark"),
                                                                 &client, completion)
                                     : client.extractLatexVision(image, QStringLiteral("benchmark"),
                                                                 &client, completion);
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            if (!token || !completed || !ticks)
                return 5;
            std::cout << "sample=" << sample
                      << " workflow=" << (table ? "table-stream" : "latex-stream")
                      << " frames=" << frames << " response_bytes=" << burst.size()
                      << " stream_ms=" << double(elapsed.nsecsElapsed()) / 1e6
                      << " largest_ui_heartbeat_gap_ms=" << largestGap << " ui_ticks=" << ticks
                      << '\n';
        }
    }
    return 0;
}
#endif
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
#ifndef NDEBUG
    std::cerr << "Run with windows-msvc-performance Release.\n";
    return 1;
#else
    if (app.arguments().contains(QStringLiteral("--stream-only")))
        return benchmarkStreams();
    QImage image(2880, 1620, QImage::Format_RGB32);
    quint32 seed = 12345;
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            seed = seed * 1664525U + 1013904223U;
            row[x] = 0xff000000U | (seed & 0x00ffffffU);
        }
    }
    const bool visionOnly = app.arguments().contains(QStringLiteral("--vision-only"));
    for (int sample = 0; sample < 3; ++sample) {
        QElapsedTimer elapsed;
        elapsed.start();
        const QByteArray bytes =
            SnowShotApiClient::encodeWebp(SnowShotApiClient::prepareImage(image));
        const double blockingMs = static_cast<double>(elapsed.nsecsElapsed()) / 1e6;
        if (bytes.isEmpty()) {
            return 2;
        }
        for (int workflow = visionOnly ? 1 : 0; workflow < 3; ++workflow) {
            SnowShotApiClient client(QStringLiteral("http://127.0.0.1:1"));
            QEventLoop loop;
            bool finished = false;
            double heartbeatMs = -1;
            const auto completion = [&](const auto&) {
                finished = true;
                loop.quit();
            };
            elapsed.restart();
            const auto token = workflow == 0 ? client.extractTable(image, &client, completion)
                               : workflow == 1
                                   ? client.extractTableVision(image, QStringLiteral("benchmark"),
                                                               &client, completion)
                                   : client.extractLatexVision(image, QStringLiteral("benchmark"),
                                                               &client, completion);
            const double submitMs = static_cast<double>(elapsed.nsecsElapsed()) / 1e6;
            QTimer::singleShot(0, &loop, [&]() {
                heartbeatMs = static_cast<double>(elapsed.nsecsElapsed()) / 1e6;
            });
            QTimer::singleShot(40000, &loop, &QEventLoop::quit);
            loop.exec();
            if (!token || !finished || heartbeatMs < 0) {
                return 3;
            }
            const char* name = workflow == 0   ? "table-dedicated"
                               : workflow == 1 ? "table-vision"
                                               : "latex-vision";
            std::cout << "sample=" << sample << " workflow=" << name
                      << " size=2880x1620 baseline_blocking_ms=" << blockingMs
                      << " async_submit_ms=" << submitMs << " ui_heartbeat_ms=" << heartbeatMs
                      << " async_total_ms=" << static_cast<double>(elapsed.nsecsElapsed()) / 1e6
                      << '\n';
        }
    }
    return 0;
#endif
}
