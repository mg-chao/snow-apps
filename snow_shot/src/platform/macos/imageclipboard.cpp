#include "imageclipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QUtiMimeConverter>
#include <QVariant>

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <memory>
#include <type_traits>

namespace snow_shot::platform::macos {
namespace {
template <typename T>
using CfPointer = std::unique_ptr<std::remove_pointer_t<T>, decltype(&CFRelease)>;

QByteArray encodeImage(CGImageRef image, CFStringRef type) {
    if (image == nullptr)
        return {};
    CfPointer<CFMutableDataRef> bytes(CFDataCreateMutable(nullptr, 0), CFRelease);
    if (!bytes)
        return {};
    CfPointer<CGImageDestinationRef> destination(
        CGImageDestinationCreateWithData(bytes.get(), type, 1, nullptr), CFRelease);
    if (!destination)
        return {};
    CGImageDestinationAddImage(destination.get(), image, nullptr);
    if (!CGImageDestinationFinalize(destination.get()))
        return {};
    return QByteArray::fromCFData(bytes.get());
}

class ImageClipboardConverter final : public QObject, public QUtiMimeConverter {
  public:
    QString utiForMime(const QString& mime) const override {
        if (mime == QStringLiteral("image/png"))
            return QStringLiteral("public.png");
        return mime == QStringLiteral("application/x-qt-image") ? QStringLiteral("public.tiff")
                                                                : QString{};
    }
    QString mimeForUti(const QString& uti) const override {
        if (uti == QStringLiteral("public.png"))
            return QStringLiteral("image/png");
        return uti == QStringLiteral("public.tiff") ? QStringLiteral("application/x-qt-image")
                                                    : QString{};
    }
    QList<QByteArray> convertFromMime(const QString& mime, const QVariant& data,
                                      const QString& uti) const override {
        if (!canConvert(mime, uti))
            return {};
        if (mime == QStringLiteral("image/png"))
            return {data.toByteArray()};
        // Qt 6.11.1's TIFF converter loses this retained CGImage. Keep the
        // native representation and its pixel provider owned through encoding.
        const QImage source = qvariant_cast<QImage>(data);
        CfPointer<CGImageRef> image(source.toCGImage(), CFRelease);
        const QByteArray bytes = encodeImage(image.get(), CFSTR("public.tiff"));
        return bytes.isEmpty() ? QList<QByteArray>{} : QList<QByteArray>{bytes};
    }
    QVariant convertToMime(const QString& mime, const QList<QByteArray>& data,
                           const QString& uti) const override {
        if (!canConvert(mime, uti) || data.isEmpty())
            return {};
        if (mime == QStringLiteral("image/png"))
            return data.first();
        CfPointer<CFDataRef> bytes(data.first().toCFData(), CFRelease);
        if (!bytes)
            return {};
        CfPointer<CGImageSourceRef> source(CGImageSourceCreateWithData(bytes.get(), nullptr),
                                           CFRelease);
        if (!source)
            return {};
        CfPointer<CGImageRef> image(CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr),
                                    CFRelease);
        // ImageIO accepts native TIFF variants without requiring a Qt TIFF
        // plugin. PNG bridges back to QImage while retaining alpha and color metadata.
        return QImage::fromData(encodeImage(image.get(), CFSTR("public.png")), "PNG");
    }
};
} // namespace

void initializeImageClipboardConverter() {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    static QPointer<ImageClipboardConverter> converter;
    if (!converter) {
        // Initialize Qt's lazy built-in MIME registry before prepending ours.
        static_cast<void>(QGuiApplication::clipboard()->mimeData());
        // The platform registry deletes registered converters at shutdown.
        // An application QObject parent would delete this earlier, while Qt
        // deliberately stops unregistering converters during closingDown().
        converter = new ImageClipboardConverter;
    }
}
} // namespace snow_shot::platform::macos
