#ifndef SNOW_SHOT_NETWORK_PROXYCONFIGURATION_H
#define SNOW_SHOT_NETWORK_PROXYCONFIGURATION_H

#include "snow_shot/proxyconfiguration.h"

#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>

namespace snow_shot::network {
class SystemProxyFactory final : public QNetworkProxyFactory {
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return systemProxyForQuery(query);
    }
};

inline void configureProxy(QNetworkAccessManager& manager, const QString& mode,
                           const QString& address) {
    if (mode == QStringLiteral("system")) {
        manager.setProxyFactory(new SystemProxyFactory);
    } else if (mode == QStringLiteral("manual")) {
        const QUrl url(normalizedHttpProxyUrl(address).value_or(QString()));
        manager.setProxy(url.isEmpty() ? QNetworkProxy(QNetworkProxy::NoProxy)
                                       : QNetworkProxy(QNetworkProxy::HttpProxy, url.host(),
                                                       static_cast<quint16>(url.port(80)),
                                                       url.userName(), url.password()));
    } else {
        manager.setProxy(QNetworkProxy::NoProxy);
    }
}
} // namespace snow_shot::network

#endif // SNOW_SHOT_NETWORK_PROXYCONFIGURATION_H
