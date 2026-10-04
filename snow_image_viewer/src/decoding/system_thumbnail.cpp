#include "decoding/system_thumbnail.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#if defined(Q_OS_WIN)
#include <objbase.h>
#include <propkey.h>
#include <shobjidl.h>
#include <windows.h>
#endif

namespace snow::image_viewer {
namespace {

bool isCancelled(const DecodeCancellation* cancellation) {
    return cancellation && cancellation->isCancelled();
}

#if defined(Q_OS_WIN)

QImage prepareThumbnailPixels(QImage pixels) {
    if (pixels.isNull()) {
        return {};
    }

    const QColorSpace sourceSpace = pixels.colorSpace();
    if (sourceSpace.isValid() && sourceSpace != QColorSpace(QColorSpace::SRgb)) {
        pixels = pixels.convertedToColorSpace(QColorSpace(QColorSpace::SRgb));
    }
    pixels = pixels.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    pixels.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return pixels;
}

class ComApartment final {
  public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    bool isUsable() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

  private:
    HRESULT result_;
};

QImage imageFromBitmap(HBITMAP bitmap) {
    BITMAP object{};
    if (!bitmap || GetObjectW(bitmap, sizeof(object), &object) != sizeof(object) ||
        object.bmWidth <= 0 || object.bmHeight == 0) {
        return {};
    }

    const int width = object.bmWidth;
    const int height = std::abs(object.bmHeight);
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {};
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC deviceContext = GetDC(nullptr);
    const int copiedRows = deviceContext
                               ? GetDIBits(deviceContext, bitmap, 0, static_cast<UINT>(height),
                                           image.bits(), &info, DIB_RGB_COLORS)
                               : 0;
    if (deviceContext) {
        ReleaseDC(nullptr, deviceContext);
    }
    if (copiedRows != height) {
        return {};
    }

    bool hasAlpha = false;
    for (int y = 0; y < height && !hasAlpha; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (qAlpha(row[x]) != 0) {
                hasAlpha = true;
                break;
            }
        }
    }
    if (!hasAlpha) {
        for (int y = 0; y < height; ++y) {
            QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                row[x] |= 0xff000000U;
            }
        }
    }
    return image;
}

QSize shellImageSize(IShellItem2* item) {
    if (!item) {
        return {};
    }

    ULONG width = 0;
    ULONG height = 0;
    if (FAILED(item->GetUInt32(PKEY_Image_HorizontalSize, &width)) ||
        FAILED(item->GetUInt32(PKEY_Image_VerticalSize, &height)) || width == 0 || height == 0 ||
        width > static_cast<ULONG>(std::numeric_limits<int>::max()) ||
        height > static_cast<ULONG>(std::numeric_limits<int>::max())) {
        return {};
    }
    QSize size(static_cast<int>(width), static_cast<int>(height));
    ULONG orientation = 0;
    if (SUCCEEDED(item->GetUInt32(PKEY_Photo_Orientation, &orientation)) && orientation >= 5 &&
        orientation <= 8) {
        size.transpose();
    }
    return size;
}

ImageThumbnail loadWindowsThumbnail(const QString& filePath, int maximumExtent,
                                    const DecodeCancellation* cancellation) {
    if (isCancelled(cancellation)) {
        return {};
    }
    ComApartment apartment;
    if (!apartment.isUsable()) {
        return {};
    }

    IShellItem2* item = nullptr;
    const QString nativePath = QDir::toNativeSeparators(filePath);
    const HRESULT itemResult = SHCreateItemFromParsingName(
        reinterpret_cast<const wchar_t*>(nativePath.utf16()), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(itemResult) || !item) {
        return {};
    }

    QSize sourceSize = shellImageSize(item);
    IShellItemImageFactory* imageFactory = nullptr;
    const HRESULT factoryResult = item->QueryInterface(IID_PPV_ARGS(&imageFactory));
    if (FAILED(factoryResult) || !imageFactory) {
        item->Release();
        return {};
    }

    HBITMAP bitmap = nullptr;
    const SIZE requestedSize{maximumExtent, maximumExtent};
    const SIIGBF flags = static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK);
    const HRESULT thumbnailResult = imageFactory->GetImage(requestedSize, flags, &bitmap);
    imageFactory->Release();
    item->Release();
    if (isCancelled(cancellation) || FAILED(thumbnailResult) || !bitmap) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return {};
    }

    QImage pixels = imageFromBitmap(bitmap);
    DeleteObject(bitmap);
    if (isCancelled(cancellation) || pixels.isNull()) {
        return {};
    }
    if (!sourceSize.isValid()) {
        sourceSize = pixels.size();
    }
    if (pixels.width() > maximumExtent || pixels.height() > maximumExtent) {
        pixels = pixels.scaled(maximumExtent, maximumExtent, Qt::KeepAspectRatio,
                               Qt::FastTransformation);
    }

    ImageThumbnail thumbnail;
    thumbnail.filePath = filePath;
    thumbnail.sourceSize = sourceSize;
    thumbnail.pixels = prepareThumbnailPixels(std::move(pixels));
    return thumbnail;
}

#endif

} // namespace

ImageThumbnail loadSystemThumbnail(const QString& filePath, int maximumExtent,
                                   const DecodeCancellation* cancellation) {
    if (isCancelled(cancellation)) {
        return {};
    }
    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();
    const int boundedExtent = std::clamp(maximumExtent, 32, kSystemThumbnailMaximumExtent);
#if defined(Q_OS_WIN)
    return loadWindowsThumbnail(absolutePath, boundedExtent, cancellation);
#else
    Q_UNUSED(boundedExtent)
    Q_UNUSED(absolutePath)
    return {};
#endif
}

} // namespace snow::image_viewer
