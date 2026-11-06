// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRPROXYCONFIGURATION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRPROXYCONFIGURATION_H

#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/storage/configurationstore.h"

#include <QNetworkProxyFactory>
#include <QUrl>

namespace snow_shot::presentation {
[[nodiscard]] inline QString resolvedOcrProxyUrl(const storage::ConfigurationStore& configuration) {
    const QString mode = configuration.value(QStringLiteral("network/proxy")).toString();
    if (mode == QStringLiteral("manual"))
        return configuration.value(QStringLiteral("network/http_proxy")).toString();
    if (mode != QStringLiteral("system"))
        return {};

    const QNetworkProxyQuery query(QUrl(QStringLiteral("https://www.modelscope.cn/")));
    const auto proxies = QNetworkProxyFactory::systemProxyForQuery(query);
    for (const QNetworkProxy& proxy : proxies) {
        if (proxy.type() != QNetworkProxy::HttpProxy && proxy.type() != QNetworkProxy::Socks5Proxy)
            continue;
        QUrl url;
        url.setScheme(proxy.type() == QNetworkProxy::Socks5Proxy ? QStringLiteral("socks5")
                                                                 : QStringLiteral("http"));
        url.setHost(proxy.hostName());
        url.setPort(proxy.port());
        if (!proxy.user().isEmpty())
            url.setUserName(proxy.user());
        if (!proxy.password().isEmpty())
            url.setPassword(proxy.password());
        return url.toString(QUrl::FullyEncoded);
    }
    return {};
}

// Bind once when the service is created, before enabling residency or queuing OCR work.
// The service owns the subscription independently of its screenshot consumers.
inline void bindOcrProxyConfiguration(ScreenshotOcrRecognitionService& service,
                                      storage::ConfigurationStore& configuration) {
    const auto apply = [&service, &configuration] {
        service.setProxyUrl(resolvedOcrProxyUrl(configuration));
    };
    apply();
    QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &service,
                     [apply](const QString& key, const QJsonValue&) {
                         if (key == QStringLiteral("network/proxy") ||
                             key == QStringLiteral("network/http_proxy"))
                             apply();
                     });
}
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRPROXYCONFIGURATION_H
