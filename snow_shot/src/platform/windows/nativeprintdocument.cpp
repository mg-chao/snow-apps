#include "nativeprintdocument.h"
#include "nativeprintdiagnostics.h"
#include "snow_shot/presentation/screenshotprintservice.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <wincodec.h>

#include <cmath>
#include <utility>

namespace {
using Microsoft::WRL::ComPtr;
using Service = ScreenshotPrintService;
using namespace winrt::Windows::Graphics::Printing;
using snow_shot::print_detail::logPrintEvent;
using snow_shot::print_detail::logWindowsPrintResult;

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
        if (raster.size() != image.size())
            logPrintEvent("print.native_raster_scaled",
                          {{QStringLiteral("backend"), QStringLiteral("windows_modern")},
                           {QStringLiteral("width"), image.width()},
                           {QStringLiteral("height"), image.height()},
                           {QStringLiteral("expected_width"), raster.width()},
                           {QStringLiteral("expected_height"), raster.height()}});
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

ScreenshotWindowsPrintDocument::ScreenshotWindowsPrintDocument(
    QImage image, std::shared_ptr<ScreenshotWindowsPrintLifecycle> lifecycle)
    : m_image(std::move(image)), m_lifecycle(std::move(lifecycle)) {}

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
            return logWindowsPrintResult(E_POINTER, "windows_modern", "GetPreviewPageCollection");
        *collection = nullptr;
        if (m_lifecycle->finished())
            return E_ABORT;
        ComPtr<IPrintPreviewDxgiPackageTarget> preview;
        winrt::check_hresult(
            target->GetPackageTarget(ID_PREVIEWPACKAGETARGET_DXGI, IID_PPV_ARGS(&preview)));
        {
            std::lock_guard lock(m_mutex);
            if (m_lifecycle->finished())
                return E_ABORT;
            m_preview.Swap(preview);
        }
        return logWindowsPrintResult(QueryInterface(__uuidof(IPrintPreviewPageCollection),
                                                    reinterpret_cast<void**>(collection)),
                                     "windows_modern", "GetPreviewPageCollection");
    } catch (...) {
        return logWindowsPrintResult(winrt::to_hresult(), "windows_modern",
                                     "GetPreviewPageCollection");
    }
}

HRESULT __stdcall ScreenshotWindowsPrintDocument::Paginate(UINT32,
                                                           ::IInspectable* options) noexcept {
    try {
        if (m_lifecycle->finished())
            return E_ABORT;
        const auto description = pageDescription(options);
        ComPtr<IPrintPreviewDxgiPackageTarget> preview;
        {
            std::lock_guard lock(m_mutex);
            m_description = description;
            ++m_previewGeneration;
            preview = m_preview;
        }
        if (!preview)
            return logWindowsPrintResult(E_UNEXPECTED, "windows_modern", "Paginate");
        const auto area = description.ImageableRect;
        logPrintEvent("print.native_paginated",
                      {{QStringLiteral("backend"), QStringLiteral("windows_modern")},
                       {QStringLiteral("width"), description.PageSize.Width},
                       {QStringLiteral("height"), description.PageSize.Height},
                       {QStringLiteral("x"), area.X},
                       {QStringLiteral("y"), area.Y},
                       {QStringLiteral("expected_width"), area.Width},
                       {QStringLiteral("expected_height"), area.Height}});
        // InvalidatePreview requests another Paginate. Windows already requested
        // this layout; publish its page count without restarting pagination.
        return logWindowsPrintResult(preview->SetJobPageCount(FinalPageCount, 1), "windows_modern",
                                     "SetJobPageCount");
    } catch (...) {
        return logWindowsPrintResult(winrt::to_hresult(), "windows_modern", "Paginate");
    }
}

HRESULT __stdcall ScreenshotWindowsPrintDocument::MakePage(UINT32 pageNumber, FLOAT width,
                                                           FLOAT height) noexcept {
    const char* stage = "MakePage";
    try {
        if (m_lifecycle->finished())
            return E_ABORT;
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
        if (jobPage != 1 || width <= 0 || height <= 0 || !preview ||
            description.PageSize.Width <= 0 || description.PageSize.Height <= 0)
            return logWindowsPrintResult(E_INVALIDARG, "windows_modern", "MakePage",
                                         {{QStringLiteral("count"), static_cast<qint64>(jobPage)},
                                          {QStringLiteral("width"), width},
                                          {QStringLiteral("height"), height}});
        stage = "create_drawing_context";
        DrawingContext drawing;
        stage = "render_page";
        auto commands = drawing.page(m_image, description);
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = static_cast<UINT>(std::ceil(width));
        textureDescription.Height = static_cast<UINT>(std::ceil(height));
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        stage = "CreateTexture2D";
        winrt::check_hresult(
            drawing.graphics->CreateTexture2D(&textureDescription, nullptr, &texture));
        ComPtr<IDXGISurface> surface;
        winrt::check_hresult(texture.As(&surface));
        const float dpiX = width * 96.0f / description.PageSize.Width;
        const float dpiY = height * 96.0f / description.PageSize.Height;
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX,
            dpiY);
        ComPtr<ID2D1Bitmap1> target;
        stage = "CreateBitmapFromDxgiSurface";
        winrt::check_hresult(
            drawing.context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target));
        stage = "render_preview";
        drawing.context->SetTarget(target.Get());
        drawing.context->SetDpi(dpiX, dpiY);
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
        return logWindowsPrintResult(preview->DrawPage(jobPage, surface.Get(), dpiX, dpiY),
                                     "windows_modern", "DrawPage");
    } catch (...) {
        return logWindowsPrintResult(
            winrt::to_hresult(), "windows_modern", stage,
            {{QStringLiteral("width"), width}, {QStringLiteral("height"), height}});
    }
}

HRESULT __stdcall ScreenshotWindowsPrintDocument::MakeDocument(
    ::IInspectable* options, IPrintDocumentPackageTarget* target) noexcept {
    const char* stage = "MakeDocument";
    try {
        if (!m_lifecycle->beginDocument())
            return E_ABORT;
        logPrintEvent("print.native_document_started",
                      {{QStringLiteral("backend"), QStringLiteral("windows_modern")}});
        if (!options || !target)
            return logWindowsPrintResult(E_POINTER, "windows_modern", "MakeDocument");
        stage = "GetPageDescription";
        const auto description = pageDescription(options);
        stage = "create_drawing_context";
        DrawingContext drawing;
        stage = "render_page";
        auto commands = drawing.page(m_image, description);
        ComPtr<ID2D1PrintControl> control;
        const D2D1_PRINT_CONTROL_PROPERTIES properties{D2D1_PRINT_FONT_SUBSET_MODE_DEFAULT, 300.0f,
                                                       D2D1_COLOR_SPACE_SRGB};
        stage = "CreatePrintControl";
        winrt::check_hresult(drawing.device->CreatePrintControl(drawing.imaging.Get(), target,
                                                                &properties, &control));
        stage = "AddPage";
        winrt::check_hresult(control->AddPage(
            commands.Get(), {description.PageSize.Width, description.PageSize.Height}, nullptr));
        const auto code = control->Close();
        if (SUCCEEDED(code))
            logPrintEvent("print.native_document_created",
                          {{QStringLiteral("backend"), QStringLiteral("windows_modern")},
                           {QStringLiteral("width"), description.PageSize.Width},
                           {QStringLiteral("height"), description.PageSize.Height}});
        return logWindowsPrintResult(code, "windows_modern", "ClosePrintControl");
    } catch (...) {
        return logWindowsPrintResult(winrt::to_hresult(), "windows_modern", stage);
    }
}

PrintPageDescription ScreenshotWindowsPrintDocument::pageDescription(::IInspectable* options) {
    winrt::Windows::Foundation::IInspectable value{nullptr};
    winrt::copy_from_abi(value, options);
    return value.as<PrintTaskOptions>().GetPageDescription(1);
}
