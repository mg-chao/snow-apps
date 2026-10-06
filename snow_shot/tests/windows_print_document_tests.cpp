#include <initguid.h>

#include "../src/platform/windows/nativeprintdocument.h"

#include <QApplication>
#include <QTimer>
#include <QWidget>

#include <d3d11.h>
#include <PrintManagerInterop.h>
#include <xpsobjectmodel_1.h>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
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
    std::function<HRESULT()> beforeDraw;
    ComPtr<IPrintPreviewDxgiPackageTarget> native;
    std::atomic<HRESULT> nativeDrawResult{E_PENDING};

    HRESULT __stdcall SetJobPageCount(PageCountType type, UINT32 count) noexcept override {
        require(type == FinalPageCount && count == 1, "preview must publish one final page");
        ++pageCountUpdates;
        return native ? native->SetJobPageCount(type, count) : S_OK;
    }

    HRESULT __stdcall DrawPage(UINT32 page, IDXGISurface* surface, FLOAT x,
                               FLOAT y) noexcept override {
        try {
            require(page == 1 && surface, "preview must deliver page 1 and a DXGI surface");
            if (beforeDraw) {
                const HRESULT result = beforeDraw();
                if (FAILED(result))
                    return result;
            }
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
            if (native) {
                const HRESULT result = native->DrawPage(page, surface, x, y);
                nativeDrawResult = result;
                if (FAILED(result))
                    return result;
            }
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
        return native ? native->InvalidatePreview() : S_OK;
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

// Exercise the real Direct2D print control against an in-memory XPS writer,
// without a printer, native dialog or spooler job.
struct DocumentTarget
    : winrt::implements<DocumentTarget, IPrintDocumentPackageTarget, IXpsDocumentPackageTarget> {
    ComPtr<IXpsOMObjectFactory> factory;
    ComPtr<IStream> stream;
    HRESULT enumerationResult = S_OK;

    DocumentTarget() {
        winrt::check_hresult(CoCreateInstance(CLSID_XpsOMObjectFactory, nullptr,
                                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    }

    HRESULT __stdcall GetPackageTargetTypes(UINT32* count, GUID** types) noexcept override {
        if (FAILED(enumerationResult))
            return enumerationResult;
        *types = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
        if (!*types)
            return E_OUTOFMEMORY;
        **types = ID_DOCUMENTPACKAGETARGET_MSXPS;
        *count = 1;
        return S_OK;
    }
    HRESULT __stdcall GetPackageTarget(REFGUID type, REFIID iid, void** target) noexcept override {
        if (type != ID_DOCUMENTPACKAGETARGET_MSXPS)
            return E_NOINTERFACE;
        return QueryInterface(iid, target);
    }
    HRESULT __stdcall Cancel() noexcept override {
        return S_OK;
    }
    HRESULT __stdcall GetXpsOMPackageWriter(IOpcPartUri* sequence, IOpcPartUri* discard,
                                            IXpsOMPackageWriter** writer) noexcept override {
        return factory->CreatePackageWriterOnStream(stream.Get(), FALSE, XPS_INTERLEAVING_OFF,
                                                    sequence, nullptr, nullptr, nullptr, discard,
                                                    writer);
    }
    HRESULT __stdcall GetXpsOMFactory(IXpsOMObjectFactory** value) noexcept override {
        return factory.CopyTo(value);
    }
    HRESULT __stdcall GetXpsType(XPS_DOCUMENT_TYPE* type) noexcept override {
        *type = XPS_DOCUMENT_TYPE_XPS;
        return S_OK;
    }
};

struct PreviewSession {
    winrt::com_ptr<ScreenshotWindowsPrintDocument> document;
    winrt::com_ptr<PrintOptions> options = winrt::make_self<PrintOptions>();
    winrt::com_ptr<PreviewTarget> target = winrt::make_self<PreviewTarget>();
    ComPtr<IPrintPreviewPageCollection> pages;
    int confirmations = 0;

    explicit PreviewSession(float displayDpi = 96.0f) {
        QImage image(400, 200, QImage::Format_RGB32);
        image.fill(Qt::red);
        document = winrt::make_self<ScreenshotWindowsPrintDocument>(std::move(image), displayDpi,
                                                                    [this] { ++confirmations; });
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

void previewRejectedDuringLayoutChangeIsNotFatal() {
    PreviewSession session;
    require(session.paginate() == S_OK, "pagination must precede the initial preview");
    session.target->beforeDraw = [&] {
        // The Windows target can advance its layout after MakePage's generation
        // check, while DrawPage is in progress. It rejects the obsolete surface.
        session.options->description = {{1056, 816}, {48, 48, 960, 720}, 300, 300};
        require(session.paginate() == S_OK, "the native target may reenter pagination");
        return E_INVALIDARG;
    };
    require(session.pages->MakePage(1, 204, 264) == S_OK,
            "Windows rejecting an obsolete preview must not fail the print task");
    require(session.document->failure() == S_OK,
            "an obsolete surface rejection must not be retained as a document failure");
    require(session.target->draws == 0, "a rejected preview must not be counted as delivered");
    session.target->beforeDraw = {};
    require(session.pages->MakePage(1, 264, 204) == S_OK,
            "the next preview request must deliver the new layout");
    verifyRenderedPage(session, {264, 204});
    session.target->beforeDraw = [] { return E_ACCESSDENIED; };
    require(session.pages->MakePage(1, 264, 204) == E_ACCESSDENIED,
            "other native preview failures must remain terminal");
    require(session.document->failure() == E_ACCESSDENIED,
            "the document must preserve the underlying HRESULT for print task completion");
    session.target->beforeDraw = [] { return E_FAIL; };
    require(session.pages->MakePage(1, 264, 204) == E_FAIL &&
                session.document->failure() == E_ACCESSDENIED,
            "later generic errors must not overwrite the original cause");
}

void previewUsesDisplayDpiAndPreservesPaperShape() {
    for (float dpi : {96.0f, 144.0f, 192.0f}) {
        PreviewSession session(dpi);
        require(session.paginate() == S_OK, "the scaled display must paginate successfully");
        require(session.pages->MakePage(1, 204.1f, 264.1f) == S_OK,
                "fractional preview dimensions must produce a valid surface");
        verifyRenderedPage(
            session, QSize(static_cast<int>(204 * dpi / 96), static_cast<int>(264 * dpi / 96)));
        require(session.target->dpiX == session.target->dpiY,
                "preview must use uniform DPI to preserve the paper and image aspect ratio");
        require(session.pages->MakePage(1, 204, 280) == S_OK,
                "a preview bounding box may differ from the paper's aspect ratio");
        verifyRenderedPage(
            session, QSize(static_cast<int>(204 * dpi / 96), static_cast<int>(264 * dpi / 96)));
    }
}

void invalidPreviewDimensionsNeverReachTarget() {
    PreviewSession session;
    require(session.paginate() == S_OK, "pagination must precede dimension validation");
    for (float invalid : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
        require(session.pages->MakePage(1, invalid, 264) == E_INVALIDARG &&
                    session.pages->MakePage(1, 204, invalid) == E_INVALIDARG,
                "invalid preview bounds must fail before creating or submitting a surface");
    }
    require(session.pages->MakePage(1, std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max()) == E_INVALIDARG,
            "preview pixel dimensions must not overflow the native texture size");
    require(session.target->draws == 0,
            "invalid dimensions must never be treated as a stale surface rejection");
}

void finalDocumentContainsOnePage() {
    PreviewSession session;
    require(session.paginate() == S_OK && session.pages->MakePage(1, 204, 264) == S_OK &&
                session.confirmations == 0,
            "native print preview must not report Print confirmation");
    auto target = winrt::make_self<DocumentTarget>();
    const auto options = session.options.as<winrt::Windows::Foundation::IInspectable>();
    const HRESULT result = session.document->MakeDocument(
        reinterpret_cast<::IInspectable*>(winrt::get_abi(options)), target.get());
    if (FAILED(result))
        std::cerr << "MakeDocument failed: " << std::hex << static_cast<uint32_t>(result) << '\n';
    require(result == S_OK, "final document creation must succeed with the real XPS writer");
    require(session.confirmations == 1,
            "final document creation must report native Print confirmation once");
    winrt::check_hresult(target->stream->Seek({}, STREAM_SEEK_SET, nullptr));
    ComPtr<IXpsOMPackage> package;
    winrt::check_hresult(
        target->factory->CreatePackageFromStream(target->stream.Get(), FALSE, &package));
    ComPtr<IXpsOMDocumentSequence> sequence;
    winrt::check_hresult(package->GetDocumentSequence(&sequence));
    ComPtr<IXpsOMDocumentCollection> documents;
    winrt::check_hresult(sequence->GetDocuments(&documents));
    UINT32 count = 0;
    winrt::check_hresult(documents->GetCount(&count));
    require(count == 1, "printing must produce exactly one document");
    ComPtr<IXpsOMDocument> document;
    winrt::check_hresult(documents->GetAt(0, &document));
    ComPtr<IXpsOMPageReferenceCollection> pages;
    winrt::check_hresult(document->GetPageReferences(&pages));
    winrt::check_hresult(pages->GetCount(&count));
    require(count == 1, "printing must produce exactly one page");
    ComPtr<IXpsOMPageReference> reference;
    winrt::check_hresult(pages->GetAt(0, &reference));
    ComPtr<IXpsOMPage> page;
    winrt::check_hresult(reference->GetPage(&page));
    XPS_SIZE size{};
    winrt::check_hresult(page->GetPageDimensions(&size));
    require(size.width == session.options->description.PageSize.Width &&
                size.height == session.options->description.PageSize.Height,
            "the final document must retain the selected paper size");
    ComPtr<IXpsOMVisualCollection> visuals;
    winrt::check_hresult(page->GetVisuals(&visuals));
    winrt::check_hresult(visuals->GetCount(&count));
    require(count > 0, "the final document must contain rendered content");
    require(session.document->failure() == S_OK,
            "a successful final document must not report a failure");

    target = winrt::make_self<DocumentTarget>();
    target->enumerationResult = E_ACCESSDENIED;
    require(
        session.document->MakeDocument(reinterpret_cast<::IInspectable*>(winrt::get_abi(options)),
                                       target.get()) == E_ACCESSDENIED &&
            session.document->failure() == E_ACCESSDENIED,
        "final document failures must retain the underlying package-target HRESULT");
    require(session.confirmations == 1,
            "repeated final document requests must not repeat feedback");
}

struct NativePreviewProbe
    : winrt::implements<NativePreviewProbe, IPrintDocumentSource, IPrintDocumentPageSource,
                        IPrintPreviewPageCollection> {
    winrt::com_ptr<ScreenshotWindowsPrintDocument> document;
    winrt::com_ptr<PreviewTarget> target = winrt::make_self<PreviewTarget>();
    std::atomic<HRESULT> lastPage{E_PENDING};

    explicit NativePreviewProbe(float displayDpi) {
        QImage image(400, 200, QImage::Format_RGB32);
        image.fill(Qt::red);
        document = winrt::make_self<ScreenshotWindowsPrintDocument>(std::move(image), displayDpi);
    }
    HRESULT __stdcall GetPreviewPageCollection(
        IPrintDocumentPackageTarget* packageTarget,
        IPrintPreviewPageCollection** collection) noexcept override {
        ComPtr<IPrintPreviewPageCollection> pages;
        const HRESULT nativeResult = packageTarget->GetPackageTarget(ID_PREVIEWPACKAGETARGET_DXGI,
                                                                     IID_PPV_ARGS(&target->native));
        if (FAILED(nativeResult))
            return nativeResult;
        auto package = winrt::make_self<PackageTarget>(target);
        const HRESULT result = document->GetPreviewPageCollection(package.get(), &pages);
        std::cout << "GetPreviewPageCollection: " << std::hex << result << std::endl;
        return FAILED(result) ? result
                              : QueryInterface(__uuidof(IPrintPreviewPageCollection),
                                               reinterpret_cast<void**>(collection));
    }
    HRESULT __stdcall Paginate(UINT32 page, ::IInspectable* options) noexcept override {
        const HRESULT result = document->Paginate(page, options);
        std::cout << "Paginate: " << std::hex << result << std::endl;
        return result;
    }
    HRESULT __stdcall MakePage(UINT32 page, FLOAT width, FLOAT height) noexcept override {
        const HRESULT result = document->MakePage(page, width, height);
        lastPage = result;
        std::cout << "MakePage(" << std::dec << page << ", " << width << ", " << height
                  << "): " << std::hex << result << std::endl;
        std::cout << "Native DrawPage: " << std::hex << target->nativeDrawResult.load()
                  << "; surface=" << std::dec << target->image.width() << 'x'
                  << target->image.height() << "; dpi=" << target->dpiX << ',' << target->dpiY
                  << std::endl;
        QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
        return result;
    }
    HRESULT __stdcall MakeDocument(::IInspectable*,
                                   IPrintDocumentPackageTarget*) noexcept override {
        return E_ABORT;
    }
};

void nativePreviewOnly(int argc, char** argv) {
    QApplication app(argc, argv);
    QWidget owner;
    owner.resize(200, 100);
    owner.setWindowTitle(QStringLiteral("Print preview regression (no submission)"));
    owner.show();
    const HWND handle = reinterpret_cast<HWND>(owner.winId());
    std::cout << "Owner DPI: " << GetDpiForWindow(handle) << std::endl;
    const UINT dpi = GetDpiForWindow(handle);
    auto document =
        winrt::make_self<NativePreviewProbe>(dpi == 0 ? 96.0f : static_cast<float>(dpi));
    const auto interop = winrt::get_activation_factory<PrintManager, IPrintManagerInterop>();
    PrintManager manager{nullptr};
    winrt::check_hresult(
        interop->GetForWindow(handle, winrt::guid_of<PrintManager>(), winrt::put_abi(manager)));
    auto requested = manager.PrintTaskRequested([document](const auto&, const auto& event) {
        auto task =
            event.Request().CreatePrintTask(L"SnowShot preview regression", [document](auto args) {
                args.SetSource(document.as<IPrintDocumentSource>());
            });
        task.Completed([](auto, auto args) {
            std::cout << "Task completion: " << static_cast<int>(args.Completion()) << std::endl;
            QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
        });
    });
    winrt::Windows::Foundation::IAsyncOperation<bool> operation{nullptr};
    QTimer::singleShot(100, &owner, [&] {
        winrt::check_hresult(interop->ShowPrintUIForWindowAsync(
            handle, winrt::guid_of<winrt::Windows::Foundation::IAsyncOperation<bool>>(),
            winrt::put_abi(operation)));
    });
    QTimer::singleShot(15000, &app, &QCoreApplication::quit);
    app.exec();
    if (operation)
        operation.Cancel();
    manager.PrintTaskRequested(requested);
    document->document->releasePreview();
    require(document->lastPage.load() == S_OK,
            "the real Windows preview must accept the rendered surface");
    require(document->target->nativeDrawResult.load() == S_OK,
            "native preview validation must reach DrawPage and verify it accepted the surface");
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--native-preview-only") {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        nativePreviewOnly(argc, argv);
        winrt::uninit_apartment();
        return 0;
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (argc > 1 && std::string_view(argv[1]) == "--page-requests-only") {
        applicationDefinedAndRepeatedPageRequests();
    } else {
        paginationDoesNotRequestPaginationAgain();
        applicationDefinedAndRepeatedPageRequests();
        previewRejectedDuringLayoutChangeIsNotFatal();
        previewUsesDisplayDpiAndPreservesPaperShape();
        invalidPreviewDimensionsNeverReachTarget();
        finalDocumentContainsOnePage();
    }
    winrt::uninit_apartment();
    return 0;
}
