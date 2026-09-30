#include "snow_shot/presentation/screenshotclipboardservice.h"

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QMimeData>
#include <QTimer>

#import <AppKit/AppKit.h>
#include <ImageIO/ImageIO.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class PasteboardRestore final {
  public:
    PasteboardRestore() {
        NSMutableArray* copies = [NSMutableArray array];
        for (NSPasteboardItem* item in [NSPasteboard generalPasteboard].pasteboardItems) {
            NSPasteboardItem* copy = [[NSPasteboardItem alloc] init];
            for (NSString* type in item.types) {
                NSData* data = [item dataForType:type];
                if (data)
                    [copy setData:data forType:type];
            }
            [copies addObject:copy];
            [copy release];
        }
        m_items = [copies copy];
    }
    ~PasteboardRestore() {
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        if (![board.types containsObject:NSPasteboardTypePNG] ||
            ![board.types containsObject:NSPasteboardTypeTIFF]) {
            std::cerr << "Native formats: "
                      << QString::fromNSString([board.types description]).toStdString() << '\n';
        }
        [board clearContents];
        if (m_items.count)
            [board writeObjects:m_items];
        [m_items release];
    }

  private:
    NSArray* m_items = nil;
};

bool hasSamePixels(const QImage& actual, const QImage& expected) {
    if (actual.size() != expected.size())
        return false;
    // QImage equality also compares storage format and color-space metadata.
    // Native and codec readers may represent the same pixels differently.
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            if (actual.pixelColor(x, y) != expected.pixelColor(x, y))
                return false;
        }
    }
    return true;
}

void nativeImageFormatsRoundTrip() {
    QImage source(17, 11, QImage::Format_ARGB32);
    source.fill(QColor(40, 80, 120, 255));
    source.setPixelColor(0, 0, QColor(0, 0, 0, 0));
    source.setPixelColor(8, 5, QColor(40, 80, 120, 128));
    QObject receiver;
    for (int cycle = 0; cycle < 24; ++cycle) {
        QEventLoop loop;
        bool completed = false;
        const auto handle = ScreenshotClipboardService::commit(
            QApplication::clipboard(), &receiver, ScreenshotClipboardService::prepareImage(source),
            [&](ScreenshotClipboardCommitResult result) {
                require(result.succeeded(), "native image publication must succeed");
                completed = true;
                loop.quit();
            });
        require(handle.isValid(), "native clipboard commit must be scheduled");
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
        require(completed && handle.isFinished(), "native clipboard commit timed out");
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        require([board.types containsObject:NSPasteboardTypePNG] &&
                    [board.types containsObject:NSPasteboardTypeTIFF],
                "native clipboard must retain both PNG and TIFF compatibility");
        const NSData* png = [board dataForType:NSPasteboardTypePNG];
        require(hasSamePixels(QImage::fromData(static_cast<const uchar*>(png.bytes),
                                               static_cast<int>(png.length), "PNG"),
                              source),
                "native PNG must preserve screenshot pixels and transparency");
        NSData* tiff = [board dataForType:NSPasteboardTypeTIFF];
        require(tiff.length > 0, "native consumers must receive the promised TIFF bytes");
        CGImageSourceRef decoder =
            CGImageSourceCreateWithData(reinterpret_cast<CFDataRef>(tiff), nullptr);
        CGImageRef image = decoder ? CGImageSourceCreateImageAtIndex(decoder, 0, nullptr) : nullptr;
        require(image && CGImageGetWidth(image) == 17 && CGImageGetHeight(image) == 11,
                "promised TIFF must decode with the original pixel dimensions");
        CGImageRelease(image);
        CFRelease(decoder);
        const QImage local = QApplication::clipboard()->image();
        require(hasSamePixels(local, source),
                "local Qt image readers must retain the original screenshot pixels");

        // Read a foreign TIFF-only pasteboard through the same converter.
        [tiff retain];
        [board clearContents];
        [board setData:tiff forType:NSPasteboardTypeTIFF];
        QCoreApplication::processEvents();
        const QImage foreign = QApplication::clipboard()->image();
        require(hasSamePixels(foreign, source),
                "foreign TIFF images must retain dimensions, color, orientation, and alpha");
        [tiff release];
        QApplication::clipboard()->clear();
    }
    QApplication::clipboard()->setText(QStringLiteral("native text conversion"));
    require([[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString] &&
                QApplication::clipboard()->text() == QStringLiteral("native text conversion"),
            "the image converter must preserve ordinary native clipboard converters");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        PasteboardRestore restore;
        nativeImageFormatsRoundTrip();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
