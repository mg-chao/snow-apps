#include "imageclipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QUtiMimeConverter>
#include <QVariant>

#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <memory>
#include <type_traits>

namespace snow_shot::platform::macos {
namespace {
template <typename T>
using CfPointer = std::unique_ptr<std::remove_pointer_t<T>, decltype(&CFRelease)>;

CfPointer<CFMutableDataRef> encodeImageData(CGImageRef image, CFStringRef type) {
    if (image == nullptr)
        return {nullptr, CFRelease};
    CfPointer<CFMutableDataRef> bytes(CFDataCreateMutable(nullptr, 0), CFRelease);
    if (!bytes)
        return {nullptr, CFRelease};
    CfPointer<CGImageDestinationRef> destination(
        CGImageDestinationCreateWithData(bytes.get(), type, 1, nullptr), CFRelease);
    if (!destination)
        return {nullptr, CFRelease};
    CGImageDestinationAddImage(destination.get(), image, nullptr);
    if (!CGImageDestinationFinalize(destination.get()))
        return {nullptr, CFRelease};
    return bytes;
}

QByteArray encodeImage(CGImageRef image, CFStringRef type) {
    const auto bytes = encodeImageData(image, type);
    if (!bytes)
        return {};
    return QByteArray::fromCFData(bytes.get());
}

CfPointer<CFMutableDataRef> tiffFromPng(NSData* png) {
    CfPointer<CGImageSourceRef> source(
        CGImageSourceCreateWithData(reinterpret_cast<CFDataRef>(png), nullptr), CFRelease);
    CfPointer<CGImageRef> image(
        source ? CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr) : nullptr, CFRelease);
    return encodeImageData(image.get(), CFSTR("public.tiff"));
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

// The pasteboard server owns the canonical PNG. This owner retains only its
// revision, so losing ownership while the app is inactive cannot retain pixels
// or encoded image data. TIFF is materialized only for consumers that need it.
@interface SnowShotImagePasteboardOwner : NSObject <NSPasteboardTypeOwner> {
  @public
    NSInteger revision;
}
- (void)pasteboard:(NSPasteboard*)pasteboard provideDataForType:(NSPasteboardType)type;
@end

@implementation SnowShotImagePasteboardOwner
- (void)pasteboard:(NSPasteboard*)pasteboard provideDataForType:(NSPasteboardType)type {
    @autoreleasepool {
        if (![type isEqualToString:NSPasteboardTypeTIFF] || pasteboard.changeCount != revision)
            return;
        NSData* png = [pasteboard dataForType:NSPasteboardTypePNG];
        if (png == nil)
            return;
        const auto tiff = snow_shot::platform::macos::tiffFromPng(png);
        if (tiff && pasteboard.changeCount == revision)
            [pasteboard setData:reinterpret_cast<NSData*>(tiff.get()) forType:type];
    }
}
@end

namespace snow_shot::platform::macos {
namespace {
class ImagePasteboardOwner final : public QObject {
  public:
    explicit ImagePasteboardOwner(QObject* parent) : QObject(parent) {
        native->revision = -1;
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
                &ImagePasteboardOwner::finishPromises);
    }
    ~ImagePasteboardOwner() override {
        finishPromises();
        [native release];
    }

    void finishPromises() const {
        @autoreleasepool {
            NSPasteboard* board = NSPasteboard.generalPasteboard;
            if (board.changeCount == native->revision)
                static_cast<void>([board dataForType:NSPasteboardTypeTIFF]);
        }
    }

    SnowShotImagePasteboardOwner* native = [[SnowShotImagePasteboardOwner alloc] init];
    bool published = false;
};

QPointer<ImagePasteboardOwner> imagePasteboardOwner;
} // namespace

bool imageClipboardBitmapIsDerivedFromPng() {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa") || !imagePasteboardOwner ||
        !imagePasteboardOwner->published)
        return false;
    return NSPasteboard.generalPasteboard.changeCount == imagePasteboardOwner->native->revision;
}

bool publishImageClipboard(QClipboard* clipboard, const QByteArray& png,
                           const QByteArray& placement, const QByteArray& appearance) {
    if (clipboard == nullptr || png.isEmpty())
        return false;
    initializeImageClipboardConverter();
    if (!imagePasteboardOwner)
        imagePasteboardOwner = new ImagePasteboardOwner(QCoreApplication::instance());
    auto* owner = imagePasteboardOwner.data();
    owner->published = false;
    bool published = false;
    @autoreleasepool {
        NSPasteboard* board = NSPasteboard.generalPasteboard;
        NSMutableArray<NSPasteboardType>* types =
            [NSMutableArray arrayWithObjects:NSPasteboardTypePNG, NSPasteboardTypeTIFF, nil];
        if (!placement.isEmpty())
            [types addObject:@"com.snowshot.screenshot-placement"];
        if (!appearance.isEmpty())
            [types addObject:@"com.snowshot.screenshot-appearance"];
        owner->native->revision = [board declareTypes:types owner:owner->native];
        published = [board setData:png.toNSData() forType:NSPasteboardTypePNG];
        if (published && !placement.isEmpty())
            published = [board setData:placement.toNSData()
                               forType:@"com.snowshot.screenshot-placement"];
        if (published && !appearance.isEmpty())
            published = [board setData:appearance.toNSData()
                               forType:@"com.snowshot.screenshot-appearance"];
    }
    owner->published = published;
    // Synchronize Qt's native reader to retire any previously owned QMimeData.
    // Reading here neither decodes the PNG nor changes the native clipboard.
    static_cast<void>(clipboard->mimeData());
    if (published) {
        emit clipboard->dataChanged();
        emit clipboard->changed(QClipboard::Clipboard);
    }
    return published;
}
} // namespace snow_shot::platform::macos
