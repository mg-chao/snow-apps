#include "snow_shot/platform/macos/pngmimeconverter.h"

#include <QGuiApplication>
#include <QPointer>
#include <QThread>

namespace snow_shot::platform::macos {

QString PngMimeConverter::utiForMime(const QString& mime) const {
    return mime == QStringLiteral("image/png") ? QStringLiteral("public.png") : QString{};
}

QString PngMimeConverter::mimeForUti(const QString& uti) const {
    return uti == QStringLiteral("public.png") ? QStringLiteral("image/png") : QString{};
}

QList<QByteArray> PngMimeConverter::convertFromMime(const QString& mime, const QVariant& data,
                                                    const QString& uti) const {
    if (mime != QStringLiteral("image/png") || uti != QStringLiteral("public.png") ||
        data.metaType() != QMetaType::fromType<QByteArray>()) {
        return {};
    }
    const QByteArray bytes = data.toByteArray();
    return bytes.isEmpty() ? QList<QByteArray>{} : QList<QByteArray>{bytes};
}

QVariant PngMimeConverter::convertToMime(const QString& mime, const QList<QByteArray>& data,
                                         const QString& uti) const {
    if (mime != QStringLiteral("image/png") || uti != QStringLiteral("public.png") ||
        data.size() != 1 || data.first().isEmpty()) {
        return {};
    }
    return data.first();
}

bool ensurePngMimeConverter() {
    auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (application == nullptr || QThread::currentThread() != application->thread()) {
        return false;
    }
    static QPointer<QGuiApplication> registeredApplication;
    if (registeredApplication != application) {
        // Qt's built-in image converter accepts QImage and produces TIFF. Keep
        // the export artifact's encoded PNG intact and expose a native image UTI.
        new PngMimeConverter();
        registeredApplication = application;
    }
    return true;
}

} // namespace snow_shot::platform::macos
