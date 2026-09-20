#include "../../presentation/pinned/pinnedimagepresenter.h"

#include <qt_windows.h>
#include <limits>

namespace snow_shot::presentation {
namespace {

class DibSurface final : public PinnedImageSurface {
  public:
    ~DibSurface() override {
        m_image = {};
        if (m_dc && m_previous)
            SelectObject(m_dc, m_previous);
        if (m_bitmap)
            DeleteObject(m_bitmap);
        if (m_dc)
            DeleteDC(m_dc);
    }

    bool initialize(const QSize& pixels) {
        if (pixels.isEmpty() || pixels.width() > std::numeric_limits<int>::max() / 4)
            return false;
        m_dc = CreateCompatibleDC(nullptr);
        if (!m_dc)
            return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = pixels.width();
        info.bmiHeader.biHeight = -pixels.height();
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_bitmap = CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!m_bitmap || !bits)
            return false;
        m_previous = SelectObject(m_dc, m_bitmap);
        if (!m_previous || m_previous == HGDI_ERROR) {
            m_previous = nullptr;
            return false;
        }
        m_image =
            QImage(static_cast<uchar*>(bits), pixels.width(), pixels.height(),
                   static_cast<qsizetype>(pixels.width()) * 4, QImage::Format_ARGB32_Premultiplied);
        return !m_image.isNull();
    }

    QImage& image() override {
        return m_image;
    }
    HDC dc() const {
        return m_dc;
    }

  private:
    QImage m_image;
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_previous = nullptr;
};

class WindowsBackend final : public PinnedImagePresentationBackend {
  public:
    std::unique_ptr<PinnedImageSurface> allocate(const QSize& pixels) override {
        auto surface = std::make_unique<DibSurface>();
        if (!surface->initialize(pixels))
            return {};
        return surface;
    }

    bool publish(WId window, const QRect& geometry, PinnedImageSurface& surface, quint8 opacity,
                 const QRegion& dirtyPixels) override {
        const HWND hwnd = reinterpret_cast<HWND>(window);
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (!(style & WS_EX_LAYERED)) {
            SetLastError(ERROR_SUCCESS);
            if (!SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED) &&
                GetLastError() != ERROR_SUCCESS)
                return false;
        }
        const POINT destination{geometry.x(), geometry.y()};
        const POINT source{};
        const SIZE size{geometry.width(), geometry.height()};
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
        const QRect bounds = dirtyPixels.boundingRect();
        const RECT dirty{bounds.left(), bounds.top(), bounds.x() + bounds.width(),
                         bounds.y() + bounds.height()};
        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof(info);
        info.pptDst = &destination;
        info.psize = &size;
        info.hdcSrc = static_cast<DibSurface&>(surface).dc();
        info.pptSrc = &source;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = bounds.isEmpty() ? nullptr : &dirty;
        return UpdateLayeredWindowIndirect(hwnd, &info) != FALSE;
    }

    bool move(WId window, const QRect& geometry) override {
        return SetWindowPos(reinterpret_cast<HWND>(window), nullptr, geometry.x(), geometry.y(), 0,
                            0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
    }
};

} // namespace

std::unique_ptr<PinnedImagePresentationBackend> createWindowsPinnedImagePresentationBackend() {
    return std::make_unique<WindowsBackend>();
}

} // namespace snow_shot::presentation
