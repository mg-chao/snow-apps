#include "../../presentation/services/nativeprintbackend.h"
#include "nativeprintdocument.h"
#include "nativeprintdialog.h"
#include "nativeprintdiagnostics.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFutureWatcher>
#include <QPointer>
#include <QTemporaryDir>
#include <QWidget>
#include <QtConcurrentRun>

#include <windows.h>
#include <winternl.h>
#include <shlobj.h>
#include <PrintManagerInterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Printing.h>

#include <mutex>
#include <atomic>
#include <optional>
#include <unordered_map>
#include <utility>

namespace {
using Service = ScreenshotPrintService;
using namespace winrt::Windows::Graphics::Printing;
using snow_shot::print_detail::logPrintEvent;
using snow_shot::print_detail::logWindowsPrintResult;

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
        const char* stage = "RoInitialize";
        try {
            const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
            winrt::check_hresult(initialized);
            m_initialized = true;
            stage = "IsSupported";
            if (!PrintManager::IsSupported()) {
                logPrintEvent("print.native_ui_unavailable",
                              {{QStringLiteral("backend"), QStringLiteral("windows_modern")},
                               {QStringLiteral("reason"), QStringLiteral("not_supported")}});
                finish({Service::Status::Unavailable, {}});
                return;
            }
            stage = "create_document";
            m_document =
                winrt::make_self<ScreenshotWindowsPrintDocument>(std::move(image), m_lifecycle);
            stage = "activate_PrintManager";
            const auto interop =
                winrt::get_activation_factory<PrintManager, IPrintManagerInterop>();
            if (!ownerAlive) {
                finish({Service::Status::Cancelled, {}});
                return;
            }
            const HWND handle = reinterpret_cast<HWND>(ownerAlive->winId());
            stage = "GetForWindow";
            winrt::check_hresult(interop->GetForWindow(handle, winrt::guid_of<PrintManager>(),
                                                       winrt::put_abi(m_manager)));
            stage = "register_PrintTaskRequested";
            m_requested = m_manager.PrintTaskRequested(
                [self](const auto&, const PrintTaskRequestedEventArgs& event) {
                    const char* stage = "CreatePrintTask";
                    try {
                        winrt::com_ptr<ScreenshotWindowsPrintDocument> document;
                        {
                            std::lock_guard lock(self->m_mutex);
                            if (self->m_lifecycle->finished())
                                return;
                            document = self->m_document;
                        }
                        auto task = event.Request().CreatePrintTask(
                            L"SnowShot", [self, document](const auto& args) {
                                try {
                                    args.SetSource(document.template as<IPrintDocumentSource>());
                                } catch (...) {
                                    const auto code = logWindowsPrintResult(
                                        winrt::to_hresult(), "windows_modern", "SetSource");
                                    self->finish({Service::Status::Failed, printError(code)});
                                    throw;
                                }
                            });
                        logPrintEvent(
                            "print.native_task_created",
                            {{QStringLiteral("backend"), QStringLiteral("windows_modern")}});
                        stage = "register_Completed";
                        const auto token = task.Completed(
                            [self](const auto&, const PrintTaskCompletedEventArgs& args) {
                                logPrintEvent(
                                    "print.native_task_completed",
                                    {{QStringLiteral("backend"), QStringLiteral("windows_modern")},
                                     {QStringLiteral("status"),
                                      static_cast<int>(args.Completion())}},
                                    args.Completion() == PrintTaskCompletion::Failed ? QtWarningMsg
                                                                                     : QtInfoMsg);
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
                            removeHandler = self->m_lifecycle->finished();
                            if (!removeHandler) {
                                self->m_task = task;
                                self->m_completed = token;
                            }
                        }
                        if (removeHandler)
                            task.Completed(token);
                    } catch (...) {
                        const auto code =
                            logWindowsPrintResult(winrt::to_hresult(), "windows_modern", stage);
                        self->finish({Service::Status::Failed, printError(code)});
                    }
                });
            stage = "ShowPrintUIForWindowAsync";
            winrt::check_hresult(interop->ShowPrintUIForWindowAsync(
                handle, winrt::guid_of<winrt::Windows::Foundation::IAsyncOperation<bool>>(),
                winrt::put_abi(m_ui)));
            logPrintEvent("print.native_ui_requested",
                          {{QStringLiteral("backend"), QStringLiteral("windows_modern")}});
            stage = "register_ui_Completed";
            m_ui.Completed(
                [self](const auto& operation, winrt::Windows::Foundation::AsyncStatus status) {
                    try {
                        if (status == winrt::Windows::Foundation::AsyncStatus::Canceled)
                            self->finish({Service::Status::Cancelled, {}});
                        else if (!operation.GetResults()) {
                            logPrintEvent(
                                "print.native_ui_unavailable",
                                {{QStringLiteral("backend"), QStringLiteral("windows_modern")},
                                 {QStringLiteral("reason"), QStringLiteral("ui_not_shown")}},
                                QtWarningMsg);
                            self->finish({Service::Status::Unavailable, {}});
                        }
                    } catch (...) {
                        const auto code = logWindowsPrintResult(winrt::to_hresult(),
                                                                "windows_modern", "ui_GetResults");
                        self->finish({Service::Status::Unavailable, printError(code)});
                    }
                });
        } catch (...) {
            const auto code = logWindowsPrintResult(winrt::to_hresult(), "windows_modern", stage);
            finish({Service::Status::Unavailable, printError(code)});
        }
    }

  private:
    void finish(Service::Result result) {
        {
            std::lock_guard lock(m_mutex);
            auto terminal = m_lifecycle->finish(std::move(result));
            if (!terminal)
                return;
            result = std::move(*terminal);
        }
        if (result.status == Service::Status::Unavailable)
            logPrintEvent("print.native_fallback_requested",
                          {{QStringLiteral("backend"), QStringLiteral("windows_modern")},
                           {QStringLiteral("reason"), QStringLiteral("failure_before_document")}},
                          QtWarningMsg);
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
                // Cancel before opening the legacy UI. Every cleanup step must still run
                // if Windows rejects cancellation or removal of another event handler.
                const auto cleanup = [](const char* stage, auto action) {
                    try {
                        action();
                    } catch (...) {
                        logWindowsPrintResult(winrt::to_hresult(), "windows_modern", stage);
                    }
                };
                if (self->m_ui && (result.status == Service::Status::Cancelled ||
                                   result.status == Service::Status::Unavailable))
                    cleanup("cancel_ui", [&] { self->m_ui.Cancel(); });
                if (task)
                    cleanup("remove_Completed", [&] { task.Completed(self->m_completed); });
                if (self->m_manager)
                    cleanup("remove_PrintTaskRequested",
                            [&] { self->m_manager.PrintTaskRequested(self->m_requested); });
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
    std::shared_ptr<ScreenshotWindowsPrintLifecycle> m_lifecycle =
        std::make_shared<ScreenshotWindowsPrintLifecycle>();
    bool m_initialized = false;
    QMetaObject::Connection m_ownerDestroyed;
    Service::Completion m_completion;
    winrt::com_ptr<ScreenshotWindowsPrintDocument> m_document;
    PrintManager m_manager{nullptr};
    PrintTask m_task{nullptr};
    winrt::Windows::Foundation::IAsyncOperation<bool> m_ui{nullptr};
    winrt::event_token m_requested{};
    winrt::event_token m_completed{};
};

class PhotoPrintJob;

// Forward the Shell formats and keep the snapshot alive during an asynchronous handoff.
class PhotoPrintDataObject : public winrt::implements<PhotoPrintDataObject, IDataObject> {
  public:
    PhotoPrintDataObject(std::shared_ptr<QTemporaryDir> directory, winrt::com_ptr<IDataObject> data,
                         std::shared_ptr<PhotoPrintJob> job)
        : m_directory(std::move(directory)), m_data(std::move(data)), m_job(std::move(job)) {
        // Shell apartment caches may keep this object alive until process exit,
        // when COM no longer guarantees running its final Release/destructor.
        m_shutdown = QObject::connect(qApp, &QObject::destroyed,
                                      [directory = m_directory] { directory->remove(); });
    }
    ~PhotoPrintDataObject();

    HRESULT __stdcall GetData(FORMATETC* format, STGMEDIUM* medium) noexcept override {
        return m_data->GetData(format, medium);
    }
    HRESULT __stdcall GetDataHere(FORMATETC* format, STGMEDIUM* medium) noexcept override {
        return m_data->GetDataHere(format, medium);
    }
    HRESULT __stdcall QueryGetData(FORMATETC* format) noexcept override {
        return m_data->QueryGetData(format);
    }
    HRESULT __stdcall GetCanonicalFormatEtc(FORMATETC* input, FORMATETC* output) noexcept override {
        return m_data->GetCanonicalFormatEtc(input, output);
    }
    HRESULT __stdcall SetData(FORMATETC* format, STGMEDIUM* medium,
                              BOOL release) noexcept override {
        return m_data->SetData(format, medium, release);
    }
    HRESULT __stdcall EnumFormatEtc(DWORD direction, IEnumFORMATETC** formats) noexcept override {
        return m_data->EnumFormatEtc(direction, formats);
    }
    HRESULT __stdcall DAdvise(FORMATETC* format, DWORD flags, IAdviseSink* sink,
                              DWORD* connection) noexcept override {
        return m_data->DAdvise(format, flags, sink, connection);
    }
    HRESULT __stdcall DUnadvise(DWORD connection) noexcept override {
        return m_data->DUnadvise(connection);
    }
    HRESULT __stdcall EnumDAdvise(IEnumSTATDATA** connections) noexcept override {
        return m_data->EnumDAdvise(connections);
    }

  private:
    std::shared_ptr<QTemporaryDir> m_directory;
    QMetaObject::Connection m_shutdown;
    winrt::com_ptr<IDataObject> m_data;
    std::shared_ptr<PhotoPrintJob> m_job;
};

class PhotoPrintJob final : public std::enable_shared_from_this<PhotoPrintJob> {
  public:
    void start(QWidget* owner, const QImage& image, Service::Completion completion,
               ScreenshotWindowsPrintDialogApi api) {
        m_owner = owner;
        m_completion = std::move(completion);
        const auto self = shared_from_this();
        m_ownerDestroyed = QObject::connect(owner, &QObject::destroyed, qApp, [self] {
            self->m_cancelled->store(true, std::memory_order_release);
            if (self->m_window)
                PostMessageW(self->m_window, WM_CLOSE, 0, 0);
        });
        m_directory = std::make_shared<QTemporaryDir>(QDir::tempPath() +
                                                      QStringLiteral("/snow-shot-print-XXXXXX"));
        const QString path =
            QDir::toNativeSeparators(m_directory->filePath(QStringLiteral("Screenshot.png")));
        logPrintEvent("print.native_snapshot_started",
                      {{QStringLiteral("backend"), QStringLiteral("windows_photo_wizard")},
                       {QStringLiteral("width"), image.width()},
                       {QStringLiteral("height"), image.height()}});
        auto* watcher = new QFutureWatcher<bool>(qApp);
        QObject::connect(
            watcher, &QFutureWatcher<bool>::finished, qApp, [self, watcher, path, api] {
                const bool saved = watcher->result();
                watcher->deleteLater();
                if (!self->m_owner) {
                    self->completeLater({Service::Status::Cancelled, {}});
                } else if (!saved) {
                    logWindowsPrintResult(
                        HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), "windows_photo_wizard",
                        self->m_directory->isValid() ? "write_snapshot"
                                                     : "create_snapshot_directory");
                    self->completeLater({Service::Status::Failed,
                                         printError(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT))});
                } else {
                    logPrintEvent(
                        "print.native_snapshot_prepared",
                        {{QStringLiteral("backend"), QStringLiteral("windows_photo_wizard")}});
                    self->startWizard(path, api);
                }
            });
        watcher->setFuture(
            QtConcurrent::run([image, directory = m_directory, cancelled = m_cancelled, path] {
                if (cancelled->load(std::memory_order_acquire) || !directory->isValid())
                    return false;
                const bool saved = image.save(path, "PNG");
                return saved && !cancelled->load(std::memory_order_acquire);
            }));
    }

  private:
    void startWizard(const QString& path, ScreenshotWindowsPrintDialogApi api) {
        const auto self = shared_from_this();
        const char* stage = "CoInitializeEx";
        try {
            winrt::check_hresult(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            m_initialized = true;
            m_ownerHandle = reinterpret_cast<HWND>(m_owner->winId());
            winrt::com_ptr<IShellItem> item;
            stage = "SHCreateItemFromParsingName";
            winrt::check_hresult(SHCreateItemFromParsingName(
                reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, IID_PPV_ARGS(item.put())));
            winrt::com_ptr<IShellItemArray> items;
            stage = "SHCreateShellItemArrayFromShellItem";
            winrt::check_hresult(
                SHCreateShellItemArrayFromShellItem(item.get(), IID_PPV_ARGS(items.put())));
            winrt::com_ptr<IDataObject> shellData;
            stage = "BindToHandler";
            winrt::check_hresult(
                items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(shellData.put())));
            stage = "create_data_object";
            const auto data =
                winrt::make<PhotoPrintDataObject>(m_directory, std::move(shellData), self);
            m_dataCreated = true;
            // Microsoft's documented Photo Printing Wizard drop target. Avoid the
            // print verb, which may invoke a different default image application.
            constexpr CLSID photoPrintWizard{
                0x60fd46de, 0xf830, 0x4894, {0xa6, 0x28, 0x6f, 0xa8, 0x1b, 0xc0, 0x19, 0x0d}};
            winrt::com_ptr<IDropTarget> target;
            stage = "activate_photo_wizard";
            winrt::check_hresult(api.create(photoPrintWizard, nullptr, CLSCTX_INPROC_SERVER,
                                            __uuidof(IDropTarget), target.put_void()));
            // COM activation reports HRESULTs, not GetLastError(). A successful
            // activation with no target is a pointer failure even if LastError is zero.
            if (!target)
                winrt::throw_hresult(E_POINTER);
            if (!m_owner) {
                completeLater({Service::Status::Cancelled, {}});
                return;
            }
            stage = "SetWinEventHook";
            m_hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, &wizardEvent,
                                     GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
            if (!m_hook) {
                winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
            }
            s_jobs.emplace(m_hook, self);
            POINTL point{};
            DWORD effect = DROPEFFECT_COPY;
            stage = "DragEnter";
            winrt::check_hresult(target->DragEnter(data.get(), MK_LBUTTON, point, &effect));
            if (!(effect & DROPEFFECT_COPY)) {
                stage = "image_rejected";
                target->DragLeave();
                winrt::throw_hresult(E_FAIL);
            }
            effect = DROPEFFECT_COPY;
            stage = "Drop";
            const HRESULT status = target->Drop(data.get(), MK_LBUTTON, point, &effect);
            if (status == DRAGDROP_S_CANCEL || status == S_FALSE ||
                status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                completeLater({Service::Status::Cancelled, {}});
            } else {
                winrt::check_hresult(status);
                m_handedOff = true;
                logPrintEvent(
                    "print.native_handed_off",
                    {{QStringLiteral("backend"), QStringLiteral("windows_photo_wizard")}});
            }
        } catch (...) {
            const auto code =
                logWindowsPrintResult(winrt::to_hresult(), "windows_photo_wizard", stage);
            completeLater({Service::Status::Failed, printError(code)});
        }
    }

  public:
    void dataReleased() {
        if (!qApp)
            return;
        // Final Release may run on the wizard thread. Cleanup, COM balancing and
        // the application's completion must run on the initiating GUI thread.
        QMetaObject::invokeMethod(
            qApp,
            [self = shared_from_this()] {
                self->finish(self->m_result.value_or(
                    Service::Result{self->m_owner && self->m_handedOff ? Service::Status::HandedOff
                                                                       : Service::Status::Cancelled,
                                    {}}));
            },
            Qt::QueuedConnection);
    }

  private:
    void completeLater(Service::Result result) {
        m_result = std::move(result);
        // Even a failing Drop may retain the data. Let its final Release drive
        // cleanup so a native consumer never observes a deleted snapshot.
        if (m_dataCreated)
            return;
        QMetaObject::invokeMethod(
            qApp, [self = shared_from_this()] { self->finish(*self->m_result); },
            Qt::QueuedConnection);
    }

    static void CALLBACK wizardEvent(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object,
                                     LONG child, DWORD, DWORD) {
        if (object != OBJID_WINDOW || child != CHILDID_SELF || !window)
            return;
        const auto found = s_jobs.find(hook);
        if (found == s_jobs.end())
            return;
        const auto job = found->second.lock();
        if (!job)
            return;
        if (event == EVENT_OBJECT_HIDE) {
            if (window == job->m_window && !IsWindowVisible(window)) {
                logPrintEvent("print.native_ui_closed", {{QStringLiteral("backend"),
                                                          QStringLiteral("windows_photo_wizard")}});
                job->finish(
                    job->m_result.value_or(Service::Result{Service::Status::HandedOff, {}}));
            }
            return;
        }
        if (job->m_window || GetWindow(window, GW_OWNER) ||
            (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD))
            return;
        wchar_t className[32]{};
        GetClassNameW(window, className, 32);
        if (QString::fromWCharArray(className) != QStringLiteral("NativeHWNDHost"))
            return;
        // The wizard exposes no owner interface and uses another GUI thread.
        // Cross-thread HWND ownership can deadlock its COM calls to this apartment.
        job->m_window = window;
        logPrintEvent("print.native_ui_shown",
                      {{QStringLiteral("backend"), QStringLiteral("windows_photo_wizard")}});
        if (!job->m_owner) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        const bool topmost =
            (GetWindowLongPtrW(job->m_ownerHandle, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        SetWindowPos(window, topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }

    void finish(Service::Result result) {
        if (!m_completion)
            return;
        auto completion = std::move(m_completion);
        if (!m_owner)
            result = {Service::Status::Cancelled, {}};
        if (m_hook) {
            s_jobs.erase(m_hook);
            UnhookWinEvent(m_hook);
            m_hook = nullptr;
        }
        QObject::disconnect(m_ownerDestroyed);
        m_directory.reset();
        if (m_initialized) {
            CoUninitialize();
            m_initialized = false;
        }
        completion(std::move(result));
    }

    static inline std::unordered_map<HWINEVENTHOOK, std::weak_ptr<PhotoPrintJob>> s_jobs;
    QPointer<QWidget> m_owner;
    HWND m_ownerHandle = nullptr;
    HWND m_window = nullptr;
    HWINEVENTHOOK m_hook = nullptr;
    bool m_initialized = false;
    bool m_handedOff = false;
    bool m_dataCreated = false;
    std::optional<Service::Result> m_result;
    QMetaObject::Connection m_ownerDestroyed;
    std::shared_ptr<QTemporaryDir> m_directory;
    std::shared_ptr<std::atomic_bool> m_cancelled = std::make_shared<std::atomic_bool>(false);
    Service::Completion m_completion;
};

PhotoPrintDataObject::~PhotoPrintDataObject() {
    QObject::disconnect(m_shutdown);
    m_job->dataReleased();
}
} // namespace

ScreenshotPrintService::Backend screenshotNativePrintBackend(bool legacy) {
    if (legacy)
        return screenshotLegacyWindowsPrintBackend();
    return [](QWidget* owner, QImage image, Service::Completion completion) {
        if (!isWindows11()) {
            logPrintEvent("print.native_ui_unavailable",
                          {{QStringLiteral("backend"), QStringLiteral("windows_modern")},
                           {QStringLiteral("reason"), QStringLiteral("requires_windows_11")}});
            completion({Service::Status::Unavailable, {}});
            return;
        }
        std::make_shared<ModernJob>()->start(owner, std::move(image), std::move(completion));
    };
}

ScreenshotPrintService::Backend screenshotClassicWindowsPrintBackend() {
    return screenshotLegacyWindowsPrintBackend();
}

ScreenshotPrintService::Backend
screenshotLegacyWindowsPrintBackend(ScreenshotWindowsPrintDialogApi api) {
    return [api](QWidget* owner, QImage image, Service::Completion completion) {
        std::make_shared<PhotoPrintJob>()->start(owner, image, std::move(completion), api);
    };
}
