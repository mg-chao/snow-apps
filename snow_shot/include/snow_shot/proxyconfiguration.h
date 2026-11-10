#ifndef SNOW_SHOT_PROXYCONFIGURATION_H
#define SNOW_SHOT_PROXYCONFIGURATION_H

#include <QString>
#include <QUrl>
#include <optional>

namespace snow_shot {
// HTTP proxies also tunnel HTTPS requests using CONNECT.
[[nodiscard]] inline std::optional<QString> normalizedHttpProxyUrl(const QString& value) {
    QString normalized = value.trimmed();
    if (normalized.isEmpty())
        return normalized;
    if (!normalized.contains(QStringLiteral("://")))
        normalized.prepend(QStringLiteral("http://"));
    QUrl url(normalized, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || url.scheme() != QStringLiteral("http") ||
        url.port(80) <= 0 || (!url.path().isEmpty() && url.path() != QStringLiteral("/")) ||
        url.hasQuery() || url.hasFragment())
        return std::nullopt;
    url.setPath(QString());
    url.setPort(url.port(80));
    return url.toString(QUrl::FullyEncoded);
}
} // namespace snow_shot

#endif // SNOW_SHOT_PROXYCONFIGURATION_H
