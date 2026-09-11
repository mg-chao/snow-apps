#include "snow_shot/platform/windows/selectedfiles.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>
#include <qt_windows.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using Microsoft::WRL::ComPtr;
using namespace snow_shot::platform::windows;
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <typename Predicate> bool waitFor(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (predicate()) {
            return true;
        }
        QThread::msleep(50);
    } while (timer.elapsed() < 10000);
    return false;
}
ComPtr<IShellView> shellView(IDispatch* dispatch) {
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    if (dispatch && SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&provider))) &&
        SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) {
        browser->QueryActiveShellView(&view);
    }
    return view;
}
QString itemPath(IShellItem* item) {
    PWSTR value = nullptr;
    QString path;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
        path = QDir::fromNativeSeparators(QString::fromWCharArray(value));
    }
    CoTaskMemFree(value);
    return path;
}
QStringList selection(IShellView* view) {
    ComPtr<IFolderView2> folder;
    ComPtr<IShellItemArray> items;
    QStringList result;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&folder))) &&
        SUCCEEDED(folder->Items(SVGIO_SELECTION, IID_PPV_ARGS(&items)))) {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(items->GetItemAt(index, &item))) {
                result.append(itemPath(item.Get()));
            }
        }
    }
    return result;
}
bool selectPaths(IShellView* view, const QStringList& paths) {
    ComPtr<IFolderView2> folder;
    if (FAILED(view->QueryInterface(IID_PPV_ARGS(&folder)))) {
        return false;
    }
    int count = 0;
    if (FAILED(folder->ItemCount(SVGIO_ALLVIEW, &count))) {
        return false;
    }
    QList<int> indices;
    for (int index = 0; index < count; ++index) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(folder->GetItem(index, IID_PPV_ARGS(&item))) &&
            paths.contains(itemPath(item.Get()))) {
            indices.append(index);
        }
    }
    if (indices.size() != paths.size()) {
        return false;
    }
    view->SelectItem(nullptr, SVSI_DESELECTOTHERS);
    for (int index : indices) {
        folder->SelectItem(index, SVSI_SELECT | SVSI_ENSUREVISIBLE);
    }
    return true;
}
ComPtr<IDispatch> explorerAt(IShellWindows* windows, const QString& path) {
    long count = 0;
    windows->get_Count(&count);
    for (long index = 0; index < count; ++index) {
        VARIANT position{};
        position.vt = VT_I4;
        position.lVal = index;
        ComPtr<IDispatch> dispatch;
        ComPtr<IWebBrowser2> browser;
        BSTR url = nullptr;
        if (SUCCEEDED(windows->Item(position, &dispatch)) && dispatch &&
            SUCCEEDED(dispatch.As(&browser)) && SUCCEEDED(browser->get_LocationURL(&url))) {
            const QString local = QUrl(QString::fromWCharArray(url)).toLocalFile();
            SysFreeString(url);
            if (QDir::cleanPath(local) == QDir::cleanPath(path)) {
                return dispatch;
            }
        }
    }
    return {};
}
void verifyCapture(const QStringList& expected, bool desktop) {
    const auto backend = createSelectedFileBackend();
    SelectedFileTarget target;
    require(waitFor([&] {
                target = backend->captureTarget();
                return target.window != 0 && target.desktop == desktop;
            }),
            "foreground selection target was not detected");
    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    auto future = std::async(std::launch::async, [backend, target] {
        return backend->selectedFiles(target, [] { return false; });
    });
    require(waitFor([&] {
                return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
            }),
            "native selection timed out");
    QStringList actual = future.get();
    for (QString& path : actual) {
        path = QDir::fromNativeSeparators(path);
    }
    QStringList sorted = expected;
    actual.sort();
    sorted.sort();
    if (actual != sorted) {
        std::cerr << "Expected " << sorted.join(';').toStdString() << " actual "
                  << actual.join(';').toStdString() << '\n';
    }
    require(actual == sorted, "native selection differs from the active view selection");
    require(GetClipboardSequenceNumber() == clipboardSequence,
            "selection capture changed the clipboard");
}
void chord(WORD key) {
    INPUT events[4]{};
    for (auto& event : events) {
        event.type = INPUT_KEYBOARD;
    }
    events[0].ki.wVk = VK_CONTROL;
    events[1].ki.wVk = key;
    events[2].ki.wVk = key;
    events[2].ki.dwFlags = KEYEVENTF_KEYUP;
    events[3].ki.wVk = VK_CONTROL;
    events[3].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(4, events, sizeof(INPUT)) == 4, "Explorer tab shortcut could not be sent");
}
void smoke() {
    ComPtr<IShellWindows> windows;
    require(SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                       IID_PPV_ARGS(&windows))),
            "Shell windows must be available");
    QTemporaryDir directory;
    QStringList files;
    for (int index = 0; index < 2; ++index) {
        const QString path = directory.filePath(QStringLiteral("selection-%1.png").arg(index));
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write("fixture") == 7,
                "fixture creation failed");
        files.append(path);
    }
    const HWND previous = GetForegroundWindow();
    struct RestoreForeground {
        HWND window;
        ~RestoreForeground() {
            SetForegroundWindow(window);
        }
    } restore{previous};
    const std::wstring arguments =
        (QStringLiteral("/n,\"") + QDir::toNativeSeparators(directory.path()) +
         QStringLiteral("\""))
            .toStdWString();
    require(reinterpret_cast<INT_PTR>(ShellExecuteW(
                nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL)) > 32,
            "test Explorer window must open");
    ComPtr<IDispatch> dispatch;
    require(waitFor([&] {
                dispatch = explorerAt(windows.Get(), directory.path());
                return dispatch != nullptr;
            }),
            "test Explorer window was not registered");
    ComPtr<IWebBrowser2> browser;
    dispatch.As(&browser);
    SHANDLE_PTR handle = 0;
    browser->get_HWND(&handle);
    const HWND window = reinterpret_cast<HWND>(handle);
    struct CloseWindow {
        HWND window;
        ~CloseWindow() {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    } close{window};
    auto view = shellView(dispatch.Get());
    require(view && waitFor([&] { return selectPaths(view.Get(), files); }),
            "Explorer fixtures must be selectable");
    SetForegroundWindow(window);
    verifyCapture(files, false);
    std::cout << "Explorer multi-selection and clipboard preservation passed\n";

    // Keep the first tab selected while opening a different folder in a new tab.
    QTemporaryDir tabDirectory;
    const QString tabPath = tabDirectory.filePath(QStringLiteral("active-tab.png"));
    QFile tabFile(tabPath);
    require(tabFile.open(QIODevice::WriteOnly), "tab fixture must open");
    tabFile.write("fixture");
    tabFile.close();
    const auto previousTarget = createSelectedFileBackend()->captureTarget();
    chord('T');
    require(waitFor([&] {
                const auto target = createSelectedFileBackend()->captureTarget();
                return target.window == previousTarget.window && target.tab != previousTarget.tab;
            }),
            "new Explorer tab must become active");
    ComPtr<IWebBrowser2> activeBrowser;
    require(waitFor([&] {
                const auto target = createSelectedFileBackend()->captureTarget();
                long count = 0;
                windows->get_Count(&count);
                for (long index = 0; index < count; ++index) {
                    VARIANT position{};
                    position.vt = VT_I4;
                    position.lVal = index;
                    ComPtr<IDispatch> candidate;
                    if (FAILED(windows->Item(position, &candidate))) {
                        continue;
                    }
                    auto candidateView = shellView(candidate.Get());
                    HWND candidateWindow = nullptr;
                    if (candidateView && SUCCEEDED(candidateView->GetWindow(&candidateWindow)) &&
                        reinterpret_cast<quintptr>(candidateWindow) == target.view) {
                        candidate.As(&activeBrowser);
                        return activeBrowser != nullptr;
                    }
                }
                return false;
            }),
            "new tab must expose its automation interface");
    VARIANT destination{};
    destination.vt = VT_BSTR;
    destination.bstrVal =
        SysAllocString(QDir::toNativeSeparators(tabDirectory.path()).toStdWString().c_str());
    VARIANT empty{};
    const HRESULT navigated =
        activeBrowser->Navigate2(&destination, &empty, &empty, &empty, &empty);
    VariantClear(&destination);
    require(SUCCEEDED(navigated), "tab navigation failed");
    ComPtr<IDispatch> tabDispatch;
    require(waitFor([&] {
                tabDispatch = explorerAt(windows.Get(), tabDirectory.path());
                return tabDispatch != nullptr;
            }),
            "new Explorer tab must navigate to its fixture folder");
    auto tabView = shellView(tabDispatch.Get());
    HWND tabWindow = nullptr;
    require(tabView && SUCCEEDED(tabView->GetWindow(&tabWindow)) && IsChild(window, tabWindow),
            "second folder must be in the same Explorer window");
    require(waitFor([&] { return selectPaths(tabView.Get(), {tabPath}); }),
            "active tab fixture must be selectable");
    verifyCapture({tabPath}, false);
    std::cout << "Active Explorer tab isolation passed\n";

    VARIANT location{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT root{};
    long desktopHandle = 0;
    ComPtr<IDispatch> desktopDispatch;
    require(SUCCEEDED(windows->FindWindowSW(&location, &root, SWC_DESKTOP, &desktopHandle,
                                            SWFO_NEEDDISPATCH, &desktopDispatch)),
            "desktop Shell view must exist");
    auto desktopView = shellView(desktopDispatch.Get());
    require(desktopView != nullptr, "desktop view must be available");
    const auto originalSelection = selection(desktopView.Get());
    struct RestoreSelection {
        ComPtr<IShellView> view;
        QStringList paths;
        ~RestoreSelection() {
            selectPaths(view.Get(), paths);
        }
    } restoreSelection{desktopView, originalSelection};
    QTemporaryFile desktopFile(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) +
                               QStringLiteral("/snow-pin-smoke-XXXXXX.png"));
    require(desktopFile.open(), "desktop fixture must open");
    desktopFile.write("fixture");
    desktopFile.close();
    const std::wstring desktopPath =
        QDir::toNativeSeparators(desktopFile.fileName()).toStdWString();
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, desktopPath.c_str(), nullptr);
    require(waitFor([&] { return selectPaths(desktopView.Get(), {desktopFile.fileName()}); }),
            "desktop fixture must be selectable");
    HWND desktopWindow = nullptr;
    desktopView->GetWindow(&desktopWindow);
    SetForegroundWindow(GetAncestor(desktopWindow, GA_ROOT));
    verifyCapture({desktopFile.fileName()}, true);
    std::cout << "Desktop selection and clipboard preservation passed\n";
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        return 1;
    }
    int result = 0;
    try {
        smoke();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    CoUninitialize();
    return result;
}
