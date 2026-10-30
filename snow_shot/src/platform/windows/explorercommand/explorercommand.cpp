#include "explorercommand.h"
#include "supportedformats.h"

#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <span>
#include <utility>

namespace snow_shot::platform::windows::explorer {
namespace {

HMODULE commandModule = nullptr;
std::atomic<ULONG> objectCount{0};
std::atomic<ULONG> serverLockCount{0};

constexpr size_t kMaximumCommandLineLength = 32767;

packaged_context_menu::Preference contextMenuPreference() {
    return packaged_context_menu::readPreference(packaged_context_menu::preferencePath());
}

wchar_t asciiLower(wchar_t character) noexcept {
    return character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character + (L'a' - L'A'))
                                                  : character;
}

bool isSeparator(wchar_t character) noexcept {
    return character == L'\\' || character == L'/';
}

bool isAbsolutePath(std::wstring_view path) noexcept {
    if (path.size() >= 3 && asciiLower(path[0]) >= L'a' && asciiLower(path[0]) <= L'z' &&
        path[1] == L':' && isSeparator(path[2]))
        return true;
    if (path.size() < 5 || !isSeparator(path[0]) || !isSeparator(path[1]))
        return false;
    const size_t serverEnd = path.find_first_of(L"\\/", 2);
    if (serverEnd == std::wstring_view::npos || serverEnd <= 2 || serverEnd + 1 >= path.size() ||
        isSeparator(path[serverEnd + 1]))
        return false;
    const size_t shareEnd = path.find_first_of(L"\\/", serverEnd + 1);
    return shareEnd != std::wstring_view::npos && shareEnd + 1 < path.size();
}

HRESULT modulePath(HMODULE module, std::wstring* path) {
    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD size =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0)
            return HRESULT_FROM_WIN32(GetLastError());
        if (size < buffer.size()) {
            buffer.resize(size);
            *path = std::move(buffer);
            return S_OK;
        }
        if (buffer.size() >= kMaximumCommandLineLength)
            return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
        buffer.resize((std::min)(buffer.size() * 2, kMaximumCommandLineLength));
    }
}

HRESULT executablePath(HMODULE module, std::wstring* path) {
    HRESULT result = modulePath(module, path);
    if (FAILED(result))
        return result;
    const size_t separator = path->find_last_of(L"\\/");
    if (separator == std::wstring::npos)
        return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    path->resize(separator + 1);
    path->append(L"snow_shot.exe");
    return S_OK;
}

// Quote every operand using the Windows CRT's backslash/quote rules. No command shell is involved.
void appendQuoted(std::wstring_view argument, std::wstring* commandLine) {
    commandLine->push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        commandLine->append(backslashes * (character == L'"' ? 2 : 1), L'\\');
        if (character == L'"')
            commandLine->push_back(L'\\');
        backslashes = 0;
        commandLine->push_back(character);
    }
    commandLine->append(backslashes * 2, L'\\');
    commandLine->push_back(L'"');
}

HRESULT selectionPaths(IShellItemArray* items, std::vector<std::wstring>* paths,
                       ShellContextMenuMode mode) {
    if (!items)
        return E_INVALIDARG;
    DWORD count = 0;
    HRESULT result = items->GetCount(&count);
    if (FAILED(result))
        return result;
    if (count == 0)
        return E_INVALIDARG;
    // Even empty arguments cannot fit a larger selection in one CreateProcess invocation.
    if (count > kMaximumCommandLineLength / sizeof(" --edit-with-snow-shot \"\""))
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    paths->reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        result = items->GetItemAt(index, &item);
        if (FAILED(result))
            return result;
        if (!item)
            return E_UNEXPECTED;
        SFGAOF attributes = 0;
        result = item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes);
        if (FAILED(result))
            return result;
        if ((attributes & SFGAO_FILESYSTEM) == 0 || (attributes & SFGAO_FOLDER) != 0)
            return E_INVALIDARG;
        PWSTR nativePath = nullptr;
        result = item->GetDisplayName(SIGDN_FILESYSPATH, &nativePath);
        if (FAILED(result))
            return result;
        std::wstring path;
        try {
            if (nativePath)
                path.assign(nativePath);
        } catch (...) {
            CoTaskMemFree(nativePath);
            throw;
        }
        CoTaskMemFree(nativePath);
        if (!isSupportedPath(path, mode))
            return E_INVALIDARG;
        paths->push_back(std::move(path));
    }
    return S_OK;
}

HRESULT launchProcess(const std::wstring& executable, const std::wstring& commandLine) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommandLine = commandLine;
    if (!CreateProcessW(executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process))
        return HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return S_OK;
}

class EditCommand final : public IExplorerCommand {
  public:
    EditCommand(HMODULE module, ProcessLauncher launcher,
                PreferenceReader preferenceReader) noexcept
        : m_module(module), m_launcher(launcher), m_preferenceReader(preferenceReader) {
        ++objectCount;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IExplorerCommand)
            return E_NOINTERFACE;
        *object = static_cast<IExplorerCommand*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_references;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --m_references;
        if (references == 0)
            delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE GetTitle(IShellItemArray*, PWSTR* title) override {
        if (!title)
            return E_POINTER;
        *title = nullptr;
        const wchar_t* text = nullptr;
        const int length = LoadStringW(m_module, 101, reinterpret_cast<PWSTR>(&text), 0);
        if (length <= 0)
            return HRESULT_FROM_WIN32(ERROR_RESOURCE_NAME_NOT_FOUND);
        try {
            std::wstring label;
            for (int index = 0; index < length; ++index) {
                label.push_back(text[index]);
                if (text[index] == L'&')
                    label.push_back(L'&');
            }
            const auto accessKey = m_preferenceReader().accessKey;
            if (packaged_context_menu::isAccessKey(accessKey)) {
                label.append(L" (&");
                label.push_back(accessKey);
                label.push_back(L')');
            }
            return SHStrDupW(label.c_str(), title);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT STDMETHODCALLTYPE GetIcon(IShellItemArray*, PWSTR* icon) override {
        if (!icon)
            return E_POINTER;
        *icon = nullptr;
        try {
            std::wstring path;
            HRESULT result = modulePath(m_module, &path);
            if (FAILED(result))
                return result;
            path.append(L",-102");
            return SHStrDupW(path.c_str(), icon);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT STDMETHODCALLTYPE GetToolTip(IShellItemArray*, PWSTR* tooltip) override {
        if (!tooltip)
            return E_POINTER;
        *tooltip = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalName(GUID* name) override {
        if (!name)
            return E_POINTER;
        *name = kEditCommandClsid;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
        if (!state)
            return E_POINTER;
        *state = ECS_HIDDEN;
        try {
            const auto preference = m_preferenceReader();
            if (preference.mode == ShellContextMenuMode::Disabled)
                return S_OK;
            std::vector<std::wstring> paths;
            if (FAILED(selectionPaths(items, &paths, preference.mode)))
                return S_OK;
            std::wstring executable;
            std::wstring commandLine;
            if (SUCCEEDED(executablePath(m_module, &executable)) &&
                SUCCEEDED(buildCommandLine(executable, paths, &commandLine)))
                *state = ECS_ENABLED;
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT STDMETHODCALLTYPE Invoke(IShellItemArray* items, IBindCtx*) override {
        try {
            const auto preference = m_preferenceReader();
            if (preference.mode == ShellContextMenuMode::Disabled)
                return E_ACCESSDENIED;
            std::vector<std::wstring> paths;
            HRESULT result = selectionPaths(items, &paths, preference.mode);
            if (FAILED(result))
                return result;
            std::wstring executable;
            result = executablePath(m_module, &executable);
            if (FAILED(result))
                return result;
            std::wstring commandLine;
            result = buildCommandLine(executable, paths, &commandLine);
            if (FAILED(result))
                return result;
            return m_launcher(executable, commandLine);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT STDMETHODCALLTYPE GetFlags(EXPCMDFLAGS* flags) override {
        if (!flags)
            return E_POINTER;
        *flags = ECF_DEFAULT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumSubCommands(IEnumExplorerCommand** commands) override {
        if (!commands)
            return E_POINTER;
        *commands = nullptr;
        return E_NOTIMPL;
    }

  private:
    ~EditCommand() {
        --objectCount;
    }

    std::atomic<ULONG> m_references{1};
    HMODULE m_module;
    ProcessLauncher m_launcher;
    PreferenceReader m_preferenceReader;
};

class CommandFactory final : public IClassFactory {
  public:
    CommandFactory() noexcept {
        ++objectCount;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory)
            return E_NOINTERFACE;
        *object = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_references;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --m_references;
        if (references == 0)
            delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        Microsoft::WRL::ComPtr<IExplorerCommand> command;
        HRESULT result = createEditCommand(commandModule, launchProcess, &command);
        return FAILED(result) ? result : command->QueryInterface(iid, object);
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock)
            ++serverLockCount;
        else
            --serverLockCount;
        return S_OK;
    }

  private:
    ~CommandFactory() {
        --objectCount;
    }

    std::atomic<ULONG> m_references{1};
};

} // namespace

bool isSupportedPath(std::wstring_view path, ShellContextMenuMode mode) noexcept {
    if (mode == ShellContextMenuMode::Disabled || !isAbsolutePath(path))
        return false;
    for (wchar_t character : path)
        if (character == L'"' || character < L' ' || (character >= 127 && character <= 159))
            return false;
    const size_t separator = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos ||
        (separator != std::wstring_view::npos && dot <= separator))
        return false;
    const auto suffix = path.substr(dot);
    const auto extensions = mode == ShellContextMenuMode::ImageFiles
                                ? std::span<const std::wstring_view>(kSupportedImageExtensions)
                                : std::span<const std::wstring_view>(kSupportedExtensions);
    for (const auto extension : extensions) {
        if (suffix.size() != extension.size())
            continue;
        bool matches = true;
        for (size_t index = 0; index < suffix.size(); ++index)
            matches = matches && asciiLower(suffix[index]) == extension[index];
        if (matches)
            return true;
    }
    return false;
}

HRESULT buildCommandLine(const std::wstring& executable, const std::vector<std::wstring>& paths,
                         std::wstring* commandLine) {
    if (!commandLine)
        return E_POINTER;
    commandLine->clear();
    if (!isAbsolutePath(executable) || executable.find(L'\0') != std::wstring::npos ||
        paths.empty())
        return E_INVALIDARG;
    std::wstring result;
    appendQuoted(executable, &result);
    for (const auto& path : paths) {
        if (!isSupportedPath(path))
            return E_INVALIDARG;
        result.append(L" --edit-with-snow-shot ");
        appendQuoted(path, &result);
        // CreateProcess's limit includes the terminating NUL.
        if (result.size() >= kMaximumCommandLineLength)
            return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    }
    *commandLine = std::move(result);
    return S_OK;
}

HRESULT createEditCommand(HMODULE module, ProcessLauncher launcher, IExplorerCommand** command,
                          PreferenceReader preferenceReader) noexcept {
    if (!command)
        return E_POINTER;
    *command = nullptr;
    if (!launcher)
        return E_INVALIDARG;
    auto* instance = new (std::nothrow)
        EditCommand(module, launcher, preferenceReader ? preferenceReader : contextMenuPreference);
    if (!instance)
        return E_OUTOFMEMORY;
    *command = instance;
    return S_OK;
}

} // namespace snow_shot::platform::windows::explorer

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    using namespace snow_shot::platform::windows::explorer;
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (clsid != kEditCommandClsid)
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) CommandFactory;
    if (!factory)
        return E_OUTOFMEMORY;
    const HRESULT result = factory->QueryInterface(iid, object);
    factory->Release();
    return result;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    using namespace snow_shot::platform::windows::explorer;
    return objectCount == 0 && serverLockCount == 0 ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        snow_shot::platform::windows::explorer::commandModule = module;
    return TRUE;
}
