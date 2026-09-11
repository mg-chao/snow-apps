#include "snow_shot/platform/windows/selectedfiles.h"

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#endif

namespace snow_shot::platform::windows {
namespace {
#if defined(Q_OS_WIN)
using Microsoft::WRL::ComPtr;

bool hasClass(HWND window, const wchar_t* name) {
    wchar_t buffer[128]{};
    return GetClassNameW(window, buffer, 128) > 0 && wcscmp(buffer, name) == 0;
}

HWND visibleChild(HWND parent, const wchar_t* name) {
    struct Search {
        const wchar_t* name;
        HWND result = nullptr;
    } search{name};
    EnumChildWindows(
        parent,
        [](HWND child, LPARAM context) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(context);
            if (IsWindowVisible(child) && hasClass(child, search.name)) {
                search.result = child;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.result;
}

QStringList filesFromView(IDispatch* dispatch, const SelectedFileTarget& target,
                          const std::function<bool()>& cancelled) {
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    if (FAILED(dispatch->QueryInterface(IID_PPV_ARGS(&provider))) ||
        FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view))) {
        return {};
    }
    HWND viewWindow = nullptr;
    if (FAILED(view->GetWindow(&viewWindow)) || !IsWindowVisible(viewWindow)) {
        return {};
    }
    // Explorer tabs can share their top-level HWND. Only the captured visible
    // Shell view (and tab, when available) may supply the selection.
    if (reinterpret_cast<quintptr>(viewWindow) != target.view ||
        (target.tab != 0 && !IsChild(reinterpret_cast<HWND>(target.tab), viewWindow))) {
        return {};
    }
    ComPtr<IFolderView2> folder;
    ComPtr<IShellItemArray> items;
    if (FAILED(view.As(&folder)) || FAILED(folder->Items(SVGIO_SELECTION, IID_PPV_ARGS(&items)))) {
        return {};
    }
    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) {
        return {};
    }
    QStringList paths;
    for (DWORD index = 0; index < count && !cancelled(); ++index) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(index, &item)) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.append(QString::fromWCharArray(path));
        }
        CoTaskMemFree(path);
    }
    return cancelled() ? QStringList{} : paths;
}
#endif

class NativeSelectedFileBackend final : public SelectedFileBackend {
  public:
    SelectedFileTarget captureTarget() const override {
        SelectedFileTarget target;
#if defined(Q_OS_WIN)
        const HWND window = GetForegroundWindow();
        const bool desktop = hasClass(window, L"Progman") || hasClass(window, L"WorkerW");
        if (!desktop && !hasClass(window, L"CabinetWClass") &&
            !hasClass(window, L"ExploreWClass")) {
            return target;
        }
        HWND view = visibleChild(window, L"SHELLDLL_DefView");
        if (view == nullptr) {
            return target;
        }
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        target.window = reinterpret_cast<quintptr>(window);
        target.view = reinterpret_cast<quintptr>(view);
        target.tab = reinterpret_cast<quintptr>(visibleChild(window, L"ShellTabWindowClass"));
        target.processId = process;
        target.desktop = desktop;
#endif
        return target;
    }

    QStringList selectedFiles(const SelectedFileTarget& target,
                              const std::function<bool()>& cancelled) const override {
#if defined(Q_OS_WIN)
        DWORD process = 0;
        if (target.window == 0 || target.view == 0 || cancelled() ||
            GetWindowThreadProcessId(reinterpret_cast<HWND>(target.window), &process) == 0 ||
            process != target.processId) {
            return {};
        }
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) {
            return {};
        }
        struct Apartment {
            ~Apartment() {
                CoUninitialize();
            }
        } apartment;
        ComPtr<IShellWindows> windows;
        if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                    IID_PPV_ARGS(&windows)))) {
            return {};
        }
        if (target.desktop) {
            VARIANT location{};
            location.vt = VT_I4;
            location.lVal = CSIDL_DESKTOP;
            VARIANT root{};
            long handle = 0;
            ComPtr<IDispatch> dispatch;
            if (SUCCEEDED(windows->FindWindowSW(&location, &root, SWC_DESKTOP, &handle,
                                                SWFO_NEEDDISPATCH, &dispatch)) &&
                dispatch) {
                return filesFromView(dispatch.Get(), target, cancelled);
            }
            return {};
        }
        long count = 0;
        windows->get_Count(&count);
        for (long index = 0; index < count && !cancelled(); ++index) {
            VARIANT position{};
            position.vt = VT_I4;
            position.lVal = index;
            ComPtr<IDispatch> dispatch;
            ComPtr<IWebBrowserApp> app;
            SHANDLE_PTR handle = 0;
            if (FAILED(windows->Item(position, &dispatch)) || !dispatch ||
                FAILED(dispatch.As(&app)) || FAILED(app->get_HWND(&handle)) ||
                static_cast<quintptr>(handle) != target.window) {
                continue;
            }
            const QStringList paths = filesFromView(dispatch.Get(), target, cancelled);
            if (!paths.isEmpty()) {
                return paths;
            }
        }
#else
        Q_UNUSED(target)
        Q_UNUSED(cancelled)
#endif
        return {};
    }
};
} // namespace

std::shared_ptr<SelectedFileBackend> createSelectedFileBackend() {
    return std::make_shared<NativeSelectedFileBackend>();
}
} // namespace snow_shot::platform::windows
