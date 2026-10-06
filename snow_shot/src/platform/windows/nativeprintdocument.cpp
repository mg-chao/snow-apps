#include "nativeprintdocument.h"
#include "snow_shot/presentation/screenshotprintservice.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
using Microsoft::WRL::ComPtr;
using Service = ScreenshotPrintService;
using namespace winrt::Windows::Graphics::Printing;

struct DrawingContext {
    ComPtr<ID3D11Device> graphics;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<IWICImagingFactory2> imaging;

    DrawingContext() {
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                               D3D11_SDK_VERSION, &graphics, nullptr, nullptr));
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(graphics.As(&dxgi));
        ComPtr<ID2D1Factory1> factory;
        winrt::check_hresult(
            D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, IID_PPV_ARGS(&factory)));
        winrt::check_hresult(factory->CreateDevice(dxgi.Get(), &device));
        winrt::check_hresult(
            device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging)));
    }

    ComPtr<ID2D1CommandList> page(const QImage& image, const PrintPageDescription& description) {
        ComPtr<ID2D1Bitmap1> bitmap;
        const int maximum = static_cast<int>(context->GetMaximumBitmapSize());
        // Tall scrolling captures can exceed the Direct2D texture limit. They
        // still print on one page; retain enough raster detail for that page.
        const QImage raster =
            image.width() > maximum || image.height() > maximum
                ? image.scaled(maximum, maximum, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                : image;
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(raster.width()), static_cast<UINT32>(raster.height())),
            raster.constBits(), static_cast<UINT32>(raster.bytesPerLine()), properties, &bitmap));
        const auto area = description.ImageableRect;
        const QRectF fitted =
            Service::fittedRect(image.size(), {area.X, area.Y, area.Width, area.Height});
        if (fitted.isEmpty())
            winrt::throw_hresult(E_INVALIDARG);
        ComPtr<ID2D1CommandList> commands;
        winrt::check_hresult(context->CreateCommandList(&commands));
        context->SetTarget(commands.Get());
        context->BeginDraw();
        context->Clear(D2D1::ColorF(D2D1::ColorF::White));
        context->DrawBitmap(
            bitmap.Get(),
            D2D1::RectF(static_cast<float>(fitted.left()), static_cast<float>(fitted.top()),
                        static_cast<float>(fitted.right()), static_cast<float>(fitted.bottom())),
            1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        winrt::check_hresult(context->EndDraw());
        context->SetTarget(nullptr);
        winrt::check_hresult(commands->Close());
        return commands;
    }
};

} // namespace

ScreenshotWindowsPrintDocument::ScreenshotWindowsPrintDocument(QImage image, float displayDpi,
                                                               std::function<void()> confirmed)
    : m_image(std::move(image)), m_displayDpi(displayDpi), m_confirmed(std::move(confirmed)) {}

HRESULT ScreenshotWindowsPrintDocument::failure() const noexcept {
    return m_failure.load();
}

HRESULT ScreenshotWindowsPrintDocument::rememberFailure(HRESULT result) noexcept {
    if (FAILED(result)) {
        HRESULT expected = S_OK;
        m_failure.compare_exchange_strong(expected, result);
    }
    return result;
}

void ScreenshotWindowsPrintDocument::releasePreview() {
    ComPtr<IPrintPreviewDxgiPackageTarget> preview;
    {
        std::lock_guard lock(m_mutex);
        m_preview.Swap(preview);
        ++m_previewGeneration;
    }
    // Releasing the package target can reenter COM. Keep it outside the lock.
}

HRESULT __stdcall ScreenshotWindowsPrintDocument::GetPreviewPageCollection(
    IPrintDocumentPackageTarget* target, IPrintPreviewPageCollection** collection) noexcept {
    try {
        if (!target || !collection)
            return rememberFailure(E_POINTER);
        ComPtr<IPrintPreviewDxgiPackageTarget> preview;
        winrt::check_hresult(
            target->GetPackageTarget(ID_PREVIEWPACKAGETARGET_DXGI, IID_PPV_ARGS(&preview)));
        {
            std::lock_guard lock(m_mutex);
            m_preview.Swap(preview);
        }
        return rememberFailure(QueryInterface(__uuidof(IPrintPreviewPageCollection),
                                              reinterpret_cast<void**>(collection)));
    } catch (...) {
        return rememberFailure(winrt::to_hresult());
    }
}

HRESULT __stdcall ScreenshotWindowsPrintDocument::Paginate(UINT32,
                                                           ::IInspectable* options) noexcept {
    try {
        const auto description = pageDescription(options);
        ComPtr<IPrintPreviewDxgiPackageTarget> preview;
        {
            std::lock_guard lock(m_mutex);
            m_description = description;
            ++m_previewGeneration;
            preview = m_preview;
        }
        if (!preview)
            return rememberFailure(E_UNEXPECTED);
        // InvalidatePreview requests another Paginate. Windows already requested
        // this layout; publish its page count without restarting pagination.
        return rememberFailure(preview->SetJobPageCount(FinalPageCount, 1));
    } catch (...) {
        return rememberFailure(winrt::to_hresult());
    }
}

HRESULT __stdcall ScreenshotWindowsPrintDocument::MakePage(UINT32 pageNumber, FLOAT width,
                                                           FLOAT height) noexcept {
    try {
        // Windows can ask the application to choose the next preview page.
        // This document always contains exactly one page.
        const UINT32 jobPage = pageNumber == JOB_PAGE_APPLICATION_DEFINED ? 1 : pageNumber;
        PrintPageDescription description{};
        ComPtr<IPrintPreviewDxgiPackageTarget> preview;
        quint64 generation;
        {
            std::lock_guard lock(m_mutex);
            description = m_description;
            preview = m_preview;
            generation = m_previewGeneration;
        }
        if (jobPage != 1 || !std::isfinite(width) || !std::isfinite(height) || width <= 0 ||
            height <= 0 || !preview || !std::isfinite(m_displayDpi) || m_displayDpi <= 0 ||
            !std::isfinite(description.PageSize.Width) ||
            !std::isfinite(description.PageSize.Height) || description.PageSize.Width <= 0 ||
            description.PageSize.Height <= 0)
            return rememberFailure(E_INVALIDARG);
        // MakePage supplies display DIPs. Use one DPI for the entire paper and
        // round its pixel dimensions, as the Windows preview renderer does.
        const double pixelsWide = std::round(static_cast<double>(width) * m_displayDpi / 96.0);
        const double pixelsHigh = std::round(static_cast<double>(height) * m_displayDpi / 96.0);
        const float dpi = static_cast<float>(std::min(pixelsWide / description.PageSize.Width,
                                                      pixelsHigh / description.PageSize.Height) *
                                             96.0);
        const double surfaceWidth = std::round(description.PageSize.Width * dpi / 96.0);
        const double surfaceHeight = std::round(description.PageSize.Height * dpi / 96.0);
        if (!std::isfinite(dpi) || dpi <= 0 || surfaceWidth < 1 || surfaceHeight < 1 ||
            surfaceWidth > std::numeric_limits<UINT>::max() ||
            surfaceHeight > std::numeric_limits<UINT>::max())
            return rememberFailure(E_INVALIDARG);
        DrawingContext drawing;
        auto commands = drawing.page(m_image, description);
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = static_cast<UINT>(surfaceWidth);
        textureDescription.Height = static_cast<UINT>(surfaceHeight);
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(
            drawing.graphics->CreateTexture2D(&textureDescription, nullptr, &texture));
        ComPtr<IDXGISurface> surface;
        winrt::check_hresult(texture.As(&surface));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        ComPtr<ID2D1Bitmap1> target;
        winrt::check_hresult(
            drawing.context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target));
        drawing.context->SetTarget(target.Get());
        drawing.context->SetDpi(dpi, dpi);
        drawing.context->BeginDraw();
        drawing.context->Clear(D2D1::ColorF(D2D1::ColorF::White));
        drawing.context->DrawImage(commands.Get());
        winrt::check_hresult(drawing.context->EndDraw());
        drawing.context->SetTarget(nullptr);
        {
            std::lock_guard lock(m_mutex);
            if (generation != m_previewGeneration)
                return S_OK;
        }
        const HRESULT result = preview->DrawPage(jobPage, surface.Get(), dpi, dpi);
        // Windows can reject a surface after advancing its preview layout, even
        // after our generation check. Like Win2D, let its next request refresh
        // that preview instead of turning this normal race into a failed job.
        return result == E_INVALIDARG ? S_OK : rememberFailure(result);
    } catch (...) {
        return rememberFailure(winrt::to_hresult());
    }
}

HRESULT __stdcall ScreenshotWindowsPrintDocument::MakeDocument(
    ::IInspectable* options, IPrintDocumentPackageTarget* target) noexcept {
    try {
        // Windows requests the final document only after the user clicks Print.
        std::call_once(m_confirmation, [this] {
            if (m_confirmed)
                m_confirmed();
        });
        const auto description = pageDescription(options);
        DrawingContext drawing;
        auto commands = drawing.page(m_image, description);
        ComPtr<ID2D1PrintControl> control;
        const D2D1_PRINT_CONTROL_PROPERTIES properties{D2D1_PRINT_FONT_SUBSET_MODE_DEFAULT, 300.0f,
                                                       D2D1_COLOR_SPACE_SRGB};
        winrt::check_hresult(drawing.device->CreatePrintControl(drawing.imaging.Get(), target,
                                                                &properties, &control));
        winrt::check_hresult(control->AddPage(
            commands.Get(), {description.PageSize.Width, description.PageSize.Height}, nullptr));
        return rememberFailure(control->Close());
    } catch (...) {
        return rememberFailure(winrt::to_hresult());
    }
}

PrintPageDescription ScreenshotWindowsPrintDocument::pageDescription(::IInspectable* options) {
    winrt::Windows::Foundation::IInspectable value{nullptr};
    winrt::copy_from_abi(value, options);
    return value.as<PrintTaskOptions>().GetPageDescription(1);
}
