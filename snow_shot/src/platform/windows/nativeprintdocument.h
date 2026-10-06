#ifndef SNOW_SHOT_WINDOWS_NATIVEPRINTDOCUMENT_H
#define SNOW_SHOT_WINDOWS_NATIVEPRINTDOCUMENT_H

#include <QImage>

#include <windows.h>
#include <DocumentSource.h>
#include <PrintPreview.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Printing.h>

#include <atomic>
#include <functional>
#include <mutex>

// The native document is independent of the print UI so its preview protocol can
// be exercised without opening a dialog or submitting a printer job.
class ScreenshotWindowsPrintDocument
    : public winrt::implements<ScreenshotWindowsPrintDocument,
                               winrt::Windows::Graphics::Printing::IPrintDocumentSource,
                               IPrintDocumentPageSource, IPrintPreviewPageCollection> {
  public:
    explicit ScreenshotWindowsPrintDocument(QImage image, float displayDpi = 96.0f,
                                            std::function<void()> confirmed = {});

    HRESULT failure() const noexcept;
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
    HRESULT rememberFailure(HRESULT result) noexcept;

    QImage m_image;
    float m_displayDpi;
    std::function<void()> m_confirmed;
    std::once_flag m_confirmation;
    std::atomic<HRESULT> m_failure{S_OK};
    std::mutex m_mutex;
    winrt::Windows::Graphics::Printing::PrintPageDescription m_description{};
    quint64 m_previewGeneration = 0;
    Microsoft::WRL::ComPtr<IPrintPreviewDxgiPackageTarget> m_preview;
};

#endif
