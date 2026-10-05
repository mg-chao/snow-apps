#ifndef SNOW_SHOT_CLOUD_UPLOAD_TEST_SUPPORT_H
#define SNOW_SHOT_CLOUD_UPLOAD_TEST_SUPPORT_H
#include "translation_test_support.h"
#include "snow_shot/clouduploadconfiguration.h"
#include <memory>
namespace cloud_upload_tests {
using namespace snow_shot;
using translation_tests::require;
inline CloudUploadConfiguration
configuration(const QString& endpoint = QStringLiteral("https://s3.example.test")) {
    CloudUploadConfiguration value;
    value.id = QStringLiteral("e235b4dd-f54b-4d59-a83e-39a903f585a3");
    value.name = QStringLiteral("Images");
    value.endpoint = endpoint;
    value.bucket = QStringLiteral("images");
    value.accessKeyId = QStringLiteral("ACCESS");
    value.secretAccessKey = QStringLiteral("secret");
    return value;
}
class S3Server final : public QObject {
  public:
    QTcpServer server;
    QByteArray headers;
    QByteArray body;
    int count = 0;
    int status = 200;
    bool hold = false;
    S3Server() {
        require(server.listen(QHostAddress::LocalHost), "listen on fake S3 endpoint");
        connect(&server, &QTcpServer::newConnection, this, [this] {
            auto* socket = server.nextPendingConnection();
            auto bytes = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                *bytes += socket->readAll();
                const auto split = bytes->indexOf("\r\n\r\n");
                if (split < 0)
                    return;
                const auto head = bytes->left(split);
                qint64 size = -1;
                for (const auto& line : head.split('\n'))
                    if (line.trimmed().toLower().startsWith("content-length:"))
                        size = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                if (size < 0 || bytes->size() - split - 4 < size ||
                    socket->property("handled").toBool())
                    return;
                socket->setProperty("handled", true);
                headers = head;
                body = bytes->mid(split + 4, static_cast<qsizetype>(size));
                ++count;
                if (hold)
                    return;
                const QByteArray response =
                    status == 200
                        ? QByteArray{}
                        : QByteArray(
                              "<Error><Code>AccessDenied</Code><Message>secret</Message></Error>");
                const QByteArray redirect =
                    status == 307 ? "Location: " + url().toUtf8() + "/redirect\r\n" : QByteArray{};
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\n" +
                              redirect + "Connection: close\r\nContent-Length: " +
                              QByteArray::number(response.size()) + "\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
        });
    }
    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1/root").arg(server.serverPort());
    }
};

} // namespace cloud_upload_tests
#endif
