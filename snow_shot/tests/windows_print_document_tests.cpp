#include "../src/platform/windows/nativeprintdocument.h"

#include <d3d11.h>

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace {
using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics::Printing;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct PrintOptions : winrt::implements<PrintOptions, IPrintTaskOptionsCore> {
    PrintPageDescription description{{816, 1056}, {48, 48, 720, 960}, 300, 300};

    PrintPageDescription GetPageDescription(uint32_t page) const {
        require(page == 1, "the single-page document must request page 1's layout");
        return description;
    }
};

// Only the Windows-owned preview target is replaced. MakePage uses the production
// Direct2D/WARP renderer, and DrawPage reads back its actual DXGI surface.
struct PreviewTarget : winrt::implements<PreviewTarget, IPrintPreviewDxgiPackageTarget> {
    int pageCountUpdates = 0;
    int invalidations = 0;
    int draws = 0;
    QImage image;
    float dpiX = 0;
    float dpiY = 0;

    HRESULT __stdcall SetJobPageCount(PageCountType type, UINT32 count) noexcept override {
        require(type == FinalPageCount && count == 1, "preview must publish one final page");
        ++pageCountUpdates;
        return S_OK;
    }

    HRESULT __stdcall DrawPage(UINT32 page, IDXGISurface* surface, FLOAT x,
                               FLOAT y) noexcept override {
        try {
            require(page == 1 && surface, "preview must deliver page 1 and a DXGI surface");
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(surface->QueryInterface(IID_PPV_ARGS(&texture)));
            ComPtr<ID3D11Device> device;
            texture->GetDevice(&device);
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            description.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> readable;
            winrt::check_hresult(device->CreateTexture2D(&description, nullptr, &readable));
            ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            context->CopyResource(readable.Get(), texture.Get());
            D3D11_MAPPED_SUBRESOURCE pixels{};
            winrt::check_hresult(context->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &pixels));
            image =
                QImage(static_cast<const uchar*>(pixels.pData), static_cast<int>(description.Width),
                       static_cast<int>(description.Height),
                       static_cast<qsizetype>(pixels.RowPitch), QImage::Format_ARGB32_Premultiplied)
                    .copy();
            context->Unmap(readable.Get(), 0);
            dpiX = x;
            dpiY = y;
            ++draws;
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT __stdcall InvalidatePreview() noexcept override {
        // Windows schedules another Paginate in response to this call. Counting
        // it avoids an unbounded callback loop when exercising the old behavior.
        ++invalidations;
        return S_OK;
    }
};

struct PackageTarget : winrt::implements<PackageTarget, IPrintDocumentPackageTarget> {
    explicit PackageTarget(winrt::com_ptr<PreviewTarget> target) : preview(std::move(target)) {}

    HRESULT __stdcall GetPackageTargetTypes(UINT32*, GUID**) noexcept override {
        return E_NOTIMPL;
    }
    HRESULT __stdcall GetPackageTarget(REFGUID type, REFIID iid, void** target) noexcept override {
        if (type != ID_PREVIEWPACKAGETARGET_DXGI)
            return E_NOINTERFACE;
        return preview->QueryInterface(iid, target);
    }
    HRESULT __stdcall Cancel() noexcept override {
        return S_OK;
    }

    winrt::com_ptr<PreviewTarget> preview;
};

struct PreviewSession {
    winrt::com_ptr<ScreenshotWindowsPrintDocument> document;
    winrt::com_ptr<PrintOptions> options = winrt::make_self<PrintOptions>();
    winrt::com_ptr<PreviewTarget> target = winrt::make_self<PreviewTarget>();
    ComPtr<IPrintPreviewPageCollection> pages;

    PreviewSession() {
        QImage image(400, 200, QImage::Format_RGB32);
        image.fill(Qt::red);
        document = winrt::make_self<ScreenshotWindowsPrintDocument>(std::move(image));
        auto package = winrt::make_self<PackageTarget>(target);
        auto source = document.as<IPrintDocumentPageSource>();
        require(source->GetPreviewPageCollection(package.get(), &pages) == S_OK,
                "the native document must expose its preview collection");
    }

    HRESULT paginate() const {
        const auto inspectable = options.as<winrt::Windows::Foundation::IInspectable>();
        return pages->Paginate(JOB_PAGE_APPLICATION_DEFINED,
                               reinterpret_cast<::IInspectable*>(winrt::get_abi(inspectable)));
    }
};

void paginationDoesNotRequestPaginationAgain() {
    PreviewSession session;
    require(session.paginate() == S_OK, "initial pagination must succeed");
    require(session.target->pageCountUpdates == 1 && session.target->invalidations == 0,
            "Paginate must publish the page count without invalidating and restarting pagination");
    session.options->description = {{1056, 816}, {48, 48, 960, 720}, 300, 300};
    require(session.paginate() == S_OK, "orientation changes must paginate successfully");
    require(session.target->pageCountUpdates == 2 && session.target->invalidations == 0,
            "Windows-triggered layout changes must not start another pagination cycle");
}

void verifyRenderedPage(const PreviewSession& session, QSize size) {
    const QImage& preview = session.target->image;
    require(preview.size() == size && session.target->dpiX > 0 && session.target->dpiY > 0,
            "preview must deliver the requested surface size and its resolution");
    require(preview.pixelColor(0, 0) == QColor(Qt::white), "paper margins must render white");
    require(preview.pixelColor(size.width() / 2, size.height() / 2) == QColor(Qt::red),
            "preview must contain the centered screenshot pixels");
}

void applicationDefinedAndRepeatedPageRequests() {
    PreviewSession session;
    require(session.paginate() == S_OK, "pagination must precede page requests");
    require(session.pages->MakePage(JOB_PAGE_APPLICATION_DEFINED, 204, 264) == S_OK,
            "Windows' application-defined preview request must render the document's first page");
    verifyRenderedPage(session, {204, 264});
    require(session.pages->MakePage(1, 408, 528) == S_OK,
            "explicit page 1 requests must render again without requiring pagination");
    verifyRenderedPage(session, {408, 528});
    require(session.target->draws == 2, "every valid preview request must deliver a page");
    require(session.pages->MakePage(0, 204, 264) == E_INVALIDARG &&
                session.pages->MakePage(2, 204, 264) == E_INVALIDARG,
            "application-defined requests must not admit nonexistent explicit pages");
    require(session.target->draws == 2, "invalid page requests must not submit preview content");

    session.options->description = {{1056, 816}, {48, 48, 960, 720}, 300, 300};
    require(session.paginate() == S_OK &&
                session.pages->MakePage(JOB_PAGE_APPLICATION_DEFINED, 264, 204) == S_OK,
            "orientation changes must render using the new page description");
    verifyRenderedPage(session, {264, 204});

    session.document->releasePreview();
    require(FAILED(session.pages->MakePage(1, 264, 204)) && session.target->draws == 3,
            "a completed session must stop sending pages to its released preview target");
}
} // namespace

int main(int argc, char** argv) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (argc > 1 && std::string_view(argv[1]) == "--page-requests-only") {
        applicationDefinedAndRepeatedPageRequests();
    } else {
        paginationDoesNotRequestPaginationAgain();
        applicationDefinedAndRepeatedPageRequests();
    }
    winrt::uninit_apartment();
    return 0;
}
