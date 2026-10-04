#include "../../presentation/services/nativeprintbackend.h"
#include "nativeprintdocument.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPointer>
#include <QWidget>

#include <windows.h>
#include <winternl.h>
#include <commdlg.h>
#include <PrintManagerInterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Printing.h>

#include <mutex>
#include <utility>

namespace {
using Service = ScreenshotPrintService;
using namespace winrt::Windows::Graphics::Printing;

QString printError(HRESULT code) {
    return QCoreApplication::translate("ScreenshotPrintService", "Windows printing failed (%1)")
        .arg(QString::number(static_cast<quint32>(code), 16));
}

bool isWindows11() {
    const auto query = reinterpret_cast<LONG(WINAPI*)(PRTL_OSVERSIONINFOW)>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return query && query(&version) == 0 && version.dwBuildNumber >= 22000;
}

class ModernJob final : public std::enable_shared_from_this<ModernJob> {
  public:
    void start(QWidget* owner, QImage image, Service::Completion completion) {
        m_completion = std::move(completion);
        const QPointer<QWidget> ownerAlive(owner);
        const auto self = shared_from_this();
        m_ownerDestroyed = QObject::connect(owner, &QObject::destroyed, qApp,
                                            [weak = std::weak_ptr<ModernJob>(self)] {
                                                if (auto job = weak.lock())
                                                    job->finish({Service::Status::Cancelled, {}});
                                            });
        try {
            const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
            winrt::check_hresult(initialized);
            m_initialized = true;
            m_document = winrt::make_self<ScreenshotWindowsPrintDocument>(std::move(image));
            const auto interop =
                winrt::get_activation_factory<PrintManager, IPrintManagerInterop>();
            if (!ownerAlive) {
                finish({Service::Status::Cancelled, {}});
                return;
            }
            const HWND handle = reinterpret_cast<HWND>(ownerAlive->winId());
            winrt::check_hresult(interop->GetForWindow(handle, winrt::guid_of<PrintManager>(),
                                                       winrt::put_abi(m_manager)));
            m_requested = m_manager.PrintTaskRequested(
                [self](const auto&, const PrintTaskRequestedEventArgs& event) {
                    try {
                        winrt::com_ptr<ScreenshotWindowsPrintDocument> document;
                        {
                            std::lock_guard lock(self->m_mutex);
                            if (self->m_finishing)
                                return;
                            self->m_taskCreated = true;
                            document = self->m_document;
                        }
                        auto task = event.Request().CreatePrintTask(
                            L"SnowShot", [document](const auto& args) {
                                args.SetSource(document.template as<IPrintDocumentSource>());
                            });
                        const auto token = task.Completed(
                            [self](const auto&, const PrintTaskCompletedEventArgs& args) {
                                switch (args.Completion()) {
                                case PrintTaskCompletion::Submitted:
                                    self->finish({Service::Status::Submitted, {}});
                                    break;
                                case PrintTaskCompletion::Failed:
                                    self->finish({Service::Status::Failed, printError(E_FAIL)});
                                    break;
                                default:
                                    self->finish({Service::Status::Cancelled, {}});
                                    break;
                                }
                            });
                        bool removeHandler;
                        {
                            std::lock_guard lock(self->m_mutex);
                            removeHandler = self->m_finishing;
                            if (!removeHandler) {
                                self->m_task = task;
                                self->m_completed = token;
                            }
                        }
                        if (removeHandler)
                            task.Completed(token);
                    } catch (...) {
                        self->finish({Service::Status::Failed, printError(winrt::to_hresult())});
                    }
                });
            winrt::check_hresult(interop->ShowPrintUIForWindowAsync(
                handle, winrt::guid_of<winrt::Windows::Foundation::IAsyncOperation<bool>>(),
                winrt::put_abi(m_ui)));
            m_ui.Completed([self](const auto& operation,
                                  winrt::Windows::Foundation::AsyncStatus status) {
                try {
                    if (status == winrt::Windows::Foundation::AsyncStatus::Canceled)
                        self->finish({Service::Status::Cancelled, {}});
                    else if (!operation.GetResults())
                        self->finish({Service::Status::Unavailable, {}});
                } catch (...) {
                    self->finish({Service::Status::Unavailable, printError(winrt::to_hresult())});
                }
            });
        } catch (...) {
            finish({Service::Status::Unavailable, printError(winrt::to_hresult())});
        }
    }

  private:
    void finish(Service::Result result) {
        {
            std::lock_guard lock(m_mutex);
            if (m_finishing)
                return;
            // An interface failure after a task starts is terminal, never a reason
            // to open a second dialog and risk submitting the same job twice.
            if (result.status == Service::Status::Unavailable && m_taskCreated)
                result.status = Service::Status::Failed;
            m_finishing = true;
        }
        const auto self = shared_from_this();
        QMetaObject::invokeMethod(
            qApp,
            [self, result = std::move(result)]() mutable {
                Service::Completion completion;
                PrintTask task{nullptr};
                winrt::com_ptr<ScreenshotWindowsPrintDocument> document;
                {
                    std::lock_guard lock(self->m_mutex);
                    completion = std::move(self->m_completion);
                    task = std::exchange(self->m_task, nullptr);
                    document = std::exchange(self->m_document, nullptr);
                }
                if (document)
                    document->releasePreview();
                QObject::disconnect(self->m_ownerDestroyed);
                try {
                    if (task)
                        task.Completed(self->m_completed);
                    if (self->m_manager)
                        self->m_manager.PrintTaskRequested(self->m_requested);
                    if (self->m_ui && result.status == Service::Status::Cancelled)
                        self->m_ui.Cancel();
                } catch (...) {
                    // Cleanup must still release the apartment and complete the request.
                }
                self->m_manager = nullptr;
                self->m_ui = nullptr;
                if (self->m_initialized) {
                    RoUninitialize();
                    self->m_initialized = false;
                }
                completion(std::move(result));
            },
            Qt::QueuedConnection);
    }
    std::mutex m_mutex;
    bool m_initialized = false;
    bool m_finishing = false;
    bool m_taskCreated = false;
    QMetaObject::Connection m_ownerDestroyed;
    Service::Completion m_completion;
    winrt::com_ptr<ScreenshotWindowsPrintDocument> m_document;
    PrintManager m_manager{nullptr};
    PrintTask m_task{nullptr};
    winrt::Windows::Foundation::IAsyncOperation<bool> m_ui{nullptr};
    winrt::event_token m_requested{};
    winrt::event_token m_completed{};
};

void printLegacy(QWidget* owner, QImage image, Service::Completion completion,
                 bool preferExtendedDialog = true) {
    const QPointer<QWidget> ownerAlive(owner);
    PRINTDLGEXW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = reinterpret_cast<HWND>(owner->winId());
    dialog.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS | PD_USEDEVMODECOPIESANDCOLLATE |
                   PD_HIDEPRINTTOFILE;
    dialog.nMinPage = 1;
    dialog.nMaxPage = 1;
    dialog.nCopies = 1;
    dialog.nStartPage = START_PAGE_GENERAL;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT status = preferExtendedDialog ? PrintDlgExW(&dialog) : E_NOTIMPL;
    if (FAILED(status) && ownerAlive) {
        if (dialog.hDC)
            DeleteDC(dialog.hDC);
        if (dialog.hDevMode)
            GlobalFree(dialog.hDevMode);
        if (dialog.hDevNames)
            GlobalFree(dialog.hDevNames);
        dialog.hDC = nullptr;
        dialog.hDevMode = nullptr;
        dialog.hDevNames = nullptr;
        PRINTDLGW legacy{};
        legacy.lStructSize = sizeof(legacy);
        legacy.hwndOwner = dialog.hwndOwner;
        legacy.Flags = dialog.Flags | PD_NOWARNING;
        legacy.nMinPage = 1;
        legacy.nMaxPage = 1;
        legacy.nCopies = 1;
        const bool accepted = PrintDlgW(&legacy) != FALSE;
        const DWORD error = accepted ? 0 : CommDlgExtendedError();
        status = error ? HRESULT_FROM_WIN32(error) : S_OK;
        dialog.dwResultAction = accepted ? PD_RESULT_PRINT : PD_RESULT_CANCEL;
        dialog.hDC = legacy.hDC;
        dialog.hDevMode = legacy.hDevMode;
        dialog.hDevNames = legacy.hDevNames;
    }
    Service::Result result{Service::Status::Cancelled, {}};
    if (!ownerAlive) {
        result = {Service::Status::Cancelled, {}};
    } else if (FAILED(status)) {
        result = {Service::Status::Failed, printError(status)};
    } else if (dialog.dwResultAction == PD_RESULT_PRINT && !dialog.hDC) {
        result = {Service::Status::Failed, printError(E_FAIL)};
    } else if (dialog.dwResultAction == PD_RESULT_PRINT && dialog.hDC) {
        DOCINFOW document{};
        document.cbSize = sizeof(document);
        document.lpszDocName = L"SnowShot";
        const QRectF area(0, 0, GetDeviceCaps(dialog.hDC, HORZRES),
                          GetDeviceCaps(dialog.hDC, VERTRES));
        const QRect fitted = Service::fittedRect(image.size(), area).toAlignedRect();
        const QImage raster =
            !fitted.isEmpty() &&
                    (image.width() > fitted.width() || image.height() > fitted.height())
                ? image.scaled(fitted.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)
                : image;
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = raster.width();
        bitmap.bmiHeader.biHeight = -raster.height();
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        bool submitted = false;
        if (!fitted.isEmpty() && StartDocW(dialog.hDC, &document) > 0) {
            if (StartPage(dialog.hDC) > 0) {
                SetStretchBltMode(dialog.hDC, HALFTONE);
                SetBrushOrgEx(dialog.hDC, 0, 0, nullptr);
                const int lines =
                    StretchDIBits(dialog.hDC, fitted.x(), fitted.y(), fitted.width(),
                                  fitted.height(), 0, 0, raster.width(), raster.height(),
                                  raster.constBits(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
                if (lines != 0 && lines != GDI_ERROR && EndPage(dialog.hDC) > 0)
                    submitted = EndDoc(dialog.hDC) > 0;
            }
            if (!submitted)
                AbortDoc(dialog.hDC);
        }
        result =
            submitted
                ? Service::Result{Service::Status::Submitted, {}}
                : Service::Result{
                      Service::Status::Failed,
                      printError(GetLastError() ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL)};
    }
    if (dialog.hDC)
        DeleteDC(dialog.hDC);
    if (dialog.hDevMode)
        GlobalFree(dialog.hDevMode);
    if (dialog.hDevNames)
        GlobalFree(dialog.hDevNames);
    if (SUCCEEDED(initialized))
        CoUninitialize();
    completion(std::move(result));
}
} // namespace

ScreenshotPrintService::Backend screenshotNativePrintBackend(bool legacy) {
    if (legacy)
        return [](QWidget* owner, QImage image, Service::Completion completion) {
            printLegacy(owner, std::move(image), std::move(completion));
        };
    return [](QWidget* owner, QImage image, Service::Completion completion) {
        if (!isWindows11()) {
            completion({Service::Status::Unavailable, {}});
            return;
        }
        std::make_shared<ModernJob>()->start(owner, std::move(image), std::move(completion));
    };
}

ScreenshotPrintService::Backend screenshotClassicWindowsPrintBackend() {
    return [](QWidget* owner, QImage image, Service::Completion completion) {
        printLegacy(owner, std::move(image), std::move(completion), false);
    };
}
