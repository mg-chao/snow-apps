#ifndef SNOW_SHOT_NETWORK_BOUNDEDNETWORKRESPONSE_H
#define SNOW_SHOT_NETWORK_BOUNDEDNETWORKRESPONSE_H

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>

#include <algorithm>

namespace snow_shot::network {

inline constexpr qsizetype kMaximumJsonResponseBytes = 4 * 1024 * 1024;

// Owns a bounded response body until its reply is deleted. Construct before connecting a
// consumer's finished handler so the final bytes and size failure are available to it.
class BoundedNetworkResponse final : public QObject {
  public:
    explicit BoundedNetworkResponse(QNetworkReply* reply, qsizetype maximumBytes)
        : QObject(reply), m_reply(reply), m_maximumBytes(maximumBytes) {
        reply->setReadBufferSize(kReadBufferBytes);
        connect(reply, &QNetworkReply::metaDataChanged, this, [this] { checkContentLength(); });
        connect(reply, &QIODevice::readyRead, this, [this] { readAvailable(); });
        connect(reply, &QNetworkReply::finished, this, [this] { readAvailable(); });
    }

    [[nodiscard]] const QByteArray& body() const {
        return m_body;
    }
    [[nodiscard]] bool tooLarge() const {
        return m_tooLarge;
    }

  private:
    void reject() {
        m_tooLarge = true;
        m_body.clear();
        m_reply->abort();
    }

    void checkContentLength() {
        if (m_tooLarge)
            return;
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // Qt exposes intermediate redirect headers but discards their bodies when following.
        if (status >= 300 && status < 400)
            return;
        // Content-Length can describe compressed wire bytes. The decoded body is always
        // checked while reading; only reject a declared length when it describes that body.
        const auto encoding = m_reply->rawHeader("Content-Encoding").trimmed().toLower();
        if ((!encoding.isEmpty() && encoding != "identity") ||
            m_reply->attribute(QNetworkRequest::OriginalContentLengthAttribute).isValid())
            return;
        bool valid = false;
        const qint64 length =
            m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&valid);
        if (valid && length > m_maximumBytes)
            reject();
    }

    void readAvailable() {
        if (m_tooLarge)
            return;
        while (m_reply->bytesAvailable() > 0) {
            const qsizetype remaining = m_maximumBytes - m_body.size();
            const QByteArray bytes =
                m_reply->read(std::min<qint64>(kReadBufferBytes, remaining + 1));
            if (bytes.isEmpty())
                return;
            if (bytes.size() > remaining) {
                reject();
                return;
            }
            m_body += bytes;
        }
    }

    static constexpr qint64 kReadBufferBytes = 64 * 1024;
    QNetworkReply* const m_reply;
    const qsizetype m_maximumBytes;
    QByteArray m_body;
    bool m_tooLarge = false;
};

} // namespace snow_shot::network

#endif // SNOW_SHOT_NETWORK_BOUNDEDNETWORKRESPONSE_H
