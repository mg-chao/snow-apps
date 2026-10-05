#ifndef SNOW_SHOT_S3UPLOADPROTOCOL_H
#define SNOW_SHOT_S3UPLOADPROTOCOL_H

#include "snow_shot/clouduploadconfiguration.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include <QNetworkRequest>

namespace snow_shot::s3_upload {
inline QByteArray uriEncode(const QByteArray& bytes) {
    return QUrl::toPercentEncoding(QString::fromUtf8(bytes), QByteArray("/"));
}
inline QByteArray encodedEndpointPath(const QUrl& url) {
    const QByteArray path = url.path(QUrl::FullyEncoded).toLatin1();
    QByteArray result;
    for (qsizetype i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            result += path.mid(i, 3).toUpper();
            i += 2;
        } else {
            result += QByteArray(1, path[i]).toPercentEncoding("/");
        }
    }
    return result;
}
inline QUrl objectUrl(const CloudUploadConfiguration& config, const QString& key,
                      bool publicUrl = false) {
    QUrl url(publicUrl && !config.publicBaseUrl.isEmpty() ? config.publicBaseUrl : config.endpoint);
    QByteArray base = url.toEncoded(QUrl::RemovePath);
    if (!(publicUrl && !config.publicBaseUrl.isEmpty()) &&
        config.addressingStyle == QStringLiteral("virtual")) {
        url.setHost(config.bucket + u'.' + url.host());
        base = url.toEncoded(QUrl::RemovePath);
    }
    QByteArray path = encodedEndpointPath(url);
    while (path.endsWith('/'))
        path.chop(1);
    if (!(publicUrl && !config.publicBaseUrl.isEmpty()) &&
        config.addressingStyle == QStringLiteral("path"))
        path += '/' + QUrl::toPercentEncoding(config.bucket);
    return QUrl::fromEncoded(base + path + '/' + uriEncode(key.toUtf8()), QUrl::StrictMode);
}
inline QByteArray hmac(const QByteArray& key, const QByteArray& value) {
    return QMessageAuthenticationCode::hash(value, key, QCryptographicHash::Sha256);
}
inline QNetworkRequest signedPut(const CloudUploadConfiguration& config, const QString& key,
                                 const QByteArray& payloadHash, const QByteArray& contentType,
                                 const QDateTime& now,
                                 QMap<QByteArray, QByteArray> extraHeaders = {}) {
    const QUrl url = objectUrl(config, key);
    QNetworkRequest request(url);
    const QByteArray date = now.toUTC().toString(QStringLiteral("yyyyMMdd")).toLatin1();
    const QByteArray timestamp =
        now.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
    const QString hostname = url.host();
    QByteArray host =
        hostname.contains(u':') ? '[' + hostname.toLatin1() + ']' : QUrl::toAce(hostname);
    if (url.port() >= 0)
        host += ':' + QByteArray::number(url.port());
    QMap<QByteArray, QByteArray> headers{
        {"host", host}, {"x-amz-content-sha256", payloadHash}, {"x-amz-date", timestamp}};
    if (!contentType.isEmpty())
        headers.insert("content-type", contentType);
    for (auto it = extraHeaders.cbegin(); it != extraHeaders.cend(); ++it)
        headers.insert(it.key().toLower(), it.value());
    if (!config.sessionToken.isEmpty())
        headers.insert("x-amz-security-token", config.sessionToken.toUtf8());
    QByteArray canonicalHeaders;
    QByteArray signedHeaders;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        canonicalHeaders += it.key() + ':' + it.value().simplified() + '\n';
        if (!signedHeaders.isEmpty())
            signedHeaders += ';';
        signedHeaders += it.key();
        request.setRawHeader(it.key(), it.value());
    }
    const QByteArray canonical = "PUT\n" + url.path(QUrl::FullyEncoded).toUtf8() + "\n\n" +
                                 canonicalHeaders + '\n' + signedHeaders + '\n' + payloadHash;
    const QByteArray scope = date + '/' + config.region.toUtf8() + "/s3/aws4_request";
    const QByteArray stringToSign =
        "AWS4-HMAC-SHA256\n" + timestamp + '\n' + scope + '\n' +
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex();
    const QByteArray signingKey = hmac(
        hmac(hmac(hmac("AWS4" + config.secretAccessKey.toUtf8(), date), config.region.toUtf8()),
             "s3"),
        "aws4_request");
    request.setRawHeader("Authorization",
                         "AWS4-HMAC-SHA256 Credential=" + config.accessKeyId.toUtf8() + '/' +
                             scope + ", SignedHeaders=" + signedHeaders +
                             ", Signature=" + hmac(signingKey, stringToSign).toHex());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    return request;
}
} // namespace snow_shot::s3_upload
#endif
