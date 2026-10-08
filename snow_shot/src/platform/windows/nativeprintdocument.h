#ifndef SNOW_SHOT_WINDOWS_NATIVEPRINTDOCUMENT_H
#define SNOW_SHOT_WINDOWS_NATIVEPRINTDOCUMENT_H

#include "nativeprintlifecycle.h"

#include <QImage>

#include <windows.h>
#include <DocumentSource.h>
#include <PrintPreview.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Printing.h>

#include <memory>
#include <mutex>

// The native document is independent of the print UI so its preview protocol can
// be exercised without opening a dialog or submitting a printer job.
class ScreenshotWindowsPrintDocument
    : public winrt::implements<ScreenshotWindowsPrintDocument,
                               winrt::Windows::Graphics::Printing::IPrintDocumentSource,
                               IPrintDocumentPageSource, IPrintPreviewPageCollection> {
  public:
    explicit ScreenshotWindowsPrintDocument(
        QImage image, std::shared_ptr<ScreenshotWindowsPrintLifecycle> lifecycle =
                          std::make_shared<ScreenshotWindowsPrintLifecycle>());

    void releasePreview();
    HRESULT __stdcall GetPreviewPageCollection(
        IPrintDocumentPackageTarget* target,
        IPrintPreviewPageCollection** collection) noexcept override;
    HRESULT __stdcall Paginate(UINT32 currentJobPage, ::IInspectable* options) noexcept override;
    HRESULT __stdcall MakePage(UINT32 pageNumber, FLOAT width, FLOAT height) noexcept override;
    HRESULT __stdcall MakeDocument(::IInspectable* options,
                                   IPrintDocumentPackageTarget* target) noexcept override;

  private:
    static winrt::Windows::Graphics::Printing::PrintPageDescription
    pageDescription(::IInspectable* options);

    QImage m_image;
    std::shared_ptr<ScreenshotWindowsPrintLifecycle> m_lifecycle;
    std::mutex m_mutex;
    winrt::Windows::Graphics::Printing::PrintPageDescription m_description{};
    quint64 m_previewGeneration = 0;
    Microsoft::WRL::ComPtr<IPrintPreviewDxgiPackageTarget> m_preview;
};

#endif
