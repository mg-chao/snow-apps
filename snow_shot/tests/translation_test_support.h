#ifndef SNOW_SHOT_TESTS_TRANSLATION_TEST_SUPPORT_H
#define SNOW_SHOT_TESTS_TRANSLATION_TEST_SUPPORT_H

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>

namespace translation_tests {
inline void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

inline void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

inline void waitUntil(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        flushEvents();
        QThread::msleep(1);
    }
    require(condition(), message);
}

class Server final : public QObject {
  public:
    struct Stream {
        QPointer<QTcpSocket> socket;
        QJsonObject body;
    };
    Server() {
        require(m_server.listen(QHostAddress::LocalHost), "listen on local translation test port");
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server.hasPendingConnections()) {
                auto* socket = m_server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { read(socket); });
            }
        });
    }
    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }
    void respondModels() {
        for (auto socket : pendingModels) {
            if (socket == nullptr || socket->state() != QAbstractSocket::ConnectedState) {
                continue;
            }
            const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("data"), models}})
                                        .toJson(QJsonDocument::Compact);
            socket->write(
                QByteArrayLiteral("HTTP/1.1 ") +
                (rejectModels ? QByteArrayLiteral("503 Unavailable")
                              : QByteArrayLiteral("200 OK")) +
                QByteArrayLiteral("\r\nContent-Type: application/json\r\nContent-Length: ") +
                QByteArray::number(body.size()) +
                QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
            socket->disconnectFromHost();
        }
        pendingModels.clear();
    }
    bool disconnected(int index) const {
        return streams.at(index).socket == nullptr ||
               streams.at(index).socket->state() == QAbstractSocket::UnconnectedState;
    }
    void send(int index, const QByteArray& data) {
        auto socket = streams.at(index).socket;
        require(socket != nullptr && !disconnected(index), "translation stream remains connected");
        socket->write(data);
        socket->flush();
    }
    void delta(int index, const QString& text) {
        const QJsonObject json{
            {QStringLiteral("choices"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("delta"), QJsonObject{{QStringLiteral("content"), text}}}}}}};
        send(index, QByteArrayLiteral("data: ") +
                        QJsonDocument(json).toJson(QJsonDocument::Compact) +
                        QByteArrayLiteral("\n\n"));
    }
    void finish(int index) {
        send(index, QByteArrayLiteral("data: [DONE]\n\n"));
        streams.at(index).socket->disconnectFromHost();
    }
    void fail(int index) {
        send(index, QByteArrayLiteral("event: error\ndata: {\"message\":\"test failure\"}\n\n"));
    }

    int modelRequests = 0;
    bool holdModels = false;
    bool rejectModels = false;
    QVector<Stream> streams;
    QVector<QPointer<QTcpSocket>> pendingModels;
    QJsonArray models{QJsonObject{{QStringLiteral("model"), QStringLiteral("vision")},
                                  {QStringLiteral("name"), QStringLiteral("Vision model")},
                                  {QStringLiteral("supports_vision"), true}},
                      QJsonObject{{QStringLiteral("model"), QStringLiteral("general")},
                                  {QStringLiteral("name"), QStringLiteral("AI Translation")},
                                  {QStringLiteral("translation_mode"), QStringLiteral("default")}},
                      QJsonObject{{QStringLiteral("model"), QStringLiteral("specialist")},
                                  {QStringLiteral("name"), QStringLiteral("Translation Model")},
                                  {QStringLiteral("translation_mode"), QStringLiteral("qwen-mt")}}};

  private:
    void read(QTcpSocket* socket) {
        if (socket->property("handled").toBool()) {
            return;
        }
        QByteArray bytes = socket->property("request").toByteArray() + socket->readAll();
        socket->setProperty("request", bytes);
        const qsizetype end = bytes.indexOf("\r\n\r\n");
        if (end < 0) {
            return;
        }
        qsizetype length = 0;
        for (const auto& line : bytes.left(end).split('\n')) {
            if (line.toLower().startsWith("content-length:")) {
                length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
            }
        }
        if (bytes.size() < end + 4 + length) {
            return;
        }
        socket->setProperty("handled", true);
        if (bytes.startsWith("GET /api/v2/chat/models")) {
            ++modelRequests;
            pendingModels.push_back(socket);
            if (!holdModels) {
                respondModels();
            }
        } else {
            require(bytes.startsWith("POST /api/v1/chat/completions"),
                    "translation uses the existing chat endpoint");
            streams.push_back({socket, QJsonDocument::fromJson(bytes.mid(end + 4)).object()});
            socket->write(QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                            "Connection: close\r\n\r\n"));
            socket->flush();
        }
    }
    QTcpServer m_server;
};
} // namespace translation_tests
#endif
