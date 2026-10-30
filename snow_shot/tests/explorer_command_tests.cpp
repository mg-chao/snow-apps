#include "explorercommand.h"
#include "supportedformats.h"

#include <shellapi.h>
#include <shlwapi.h>
#ifdef _MSC_VER
#pragma warning(push)
// Windows SDK WRL wrappers intentionally own their objects through COM Release().
#pragma warning(disable : 4265)
#endif
#include <wrl.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

namespace {
using namespace snow_shot::platform::windows::explorer;
using Microsoft::WRL::ComPtr;
using snow_shot::platform::windows::ShellContextMenuMode;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Entry {
    std::wstring path;
    SFGAOF attributes = SFGAO_FILESYSTEM;
    HRESULT displayResult = S_OK;
    HRESULT attributeResult = S_OK;
};

class ShellItem final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IShellItem> {
  public:
    explicit ShellItem(Entry entry) : m_entry(std::move(entry)) {}

    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*, REFGUID, REFIID, void**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetParent(IShellItem**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayName(SIGDN name, PWSTR* path) override {
        require(name == SIGDN_FILESYSPATH, "command requests a filesystem path from the shell");
        if (FAILED(m_entry.displayResult))
            return m_entry.displayResult;
        return SHStrDupW(m_entry.path.c_str(), path);
    }

    HRESULT STDMETHODCALLTYPE GetAttributes(SFGAOF mask, SFGAOF* attributes) override {
        *attributes = m_entry.attributes & mask;
        return m_entry.attributeResult;
    }

    HRESULT STDMETHODCALLTYPE Compare(IShellItem*, SICHINTF, int*) override {
        return E_NOTIMPL;
    }

  private:
    Entry m_entry;
};

class ShellItems final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IShellItemArray> {
  public:
    explicit ShellItems(std::vector<Entry> entries) : m_entries(std::move(entries)) {}

    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*, REFGUID, REFIID, void**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS, REFIID, void**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY, REFIID, void**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS, SFGAOF, SFGAOF*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override {
        *count = static_cast<DWORD>(m_entries.size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD index, IShellItem** item) override {
        if (index >= m_entries.size())
            return E_INVALIDARG;
        return Microsoft::WRL::Make<ShellItem>(m_entries[index]).CopyTo(item);
    }

    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems**) override {
        return E_NOTIMPL;
    }

  private:
    std::vector<Entry> m_entries;
};

int launchCount = 0;
std::wstring launchedExecutable;
std::wstring launchedCommandLine;
HRESULT launchResult = S_OK;
snow_shot::platform::windows::packaged_context_menu::Preference menuPreference;

auto readMenuPreference() {
    return menuPreference;
}

HRESULT captureLaunch(const std::wstring& executable, const std::wstring& commandLine) {
    ++launchCount;
    launchedExecutable = executable;
    launchedCommandLine = commandLine;
    return launchResult;
}

std::vector<std::wstring> parseCommandLine(const std::wstring& commandLine) {
    int count = 0;
    auto* arguments = CommandLineToArgvW(commandLine.c_str(), &count);
    require(arguments != nullptr, "Windows parses the generated command line");
    std::vector<std::wstring> result;
    for (int index = 0; index < count; ++index)
        result.emplace_back(arguments[index]);
    LocalFree(arguments);
    return result;
}

void pathAndCommandLinePolicy() {
    for (auto suffix : kSupportedExtensions)
        require(isSupportedPath(std::wstring(L"C:\\images\\file") + std::wstring(suffix)),
                "every declared package extension is enabled by the Explorer command");
    for (const std::wstring path :
         {L"C:\\images\\图片 one.PNG", L"\\\\server\\share\\photo.JpEg",
          L"C:/images/notes.Markdown", L"\\\\?\\C:\\long path\\image.avif"})
        require(isSupportedPath(path), "Unicode, UNC, long and mixed-case supported paths work");
    for (const std::wstring path :
         {L"", L"relative.png", L"C:relative.png", L"/rooted.png", L"\\\\server\\photo.png",
          L"C:\\images\\archive.zip", L"C:\\images\\directory.png\\no-suffix",
          L"C:\\images\\quoted\".png", L"C:\\images\\line\n.png", L"C:\\images\\control\x7f.png"})
        require(!isSupportedPath(path), "unsupported and malformed shell paths are rejected");
    std::wstring embeddedNul = L"C:\\images\\nul";
    embeddedNul.push_back(L'\0');
    embeddedNul.append(L".png");
    require(!isSupportedPath(embeddedNul), "embedded NUL paths cannot truncate a shell operand");

    const std::wstring executable = L"C:\\Program Files\\Snow Shot\\snow_shot.exe";
    const std::vector<std::wstring> paths{L"C:\\images\\图片 & (100%) one.png",
                                          L"\\\\server\\share\\note's two.MD"};
    std::wstring commandLine;
    require(buildCommandLine(executable, paths, &commandLine) == S_OK,
            "supported paths produce a launch command");
    require(parseCommandLine(commandLine) ==
                std::vector<std::wstring>{executable, L"--edit-with-snow-shot", paths[0],
                                          L"--edit-with-snow-shot", paths[1]},
            "Windows reparses every typed Unicode operand in order without shell expansion");
    require(buildCommandLine(executable, {}, &commandLine) == E_INVALIDARG && commandLine.empty(),
            "empty selections cannot activate the application");
    require(buildCommandLine(executable, {paths[0], L"C:\\bad.exe"}, &commandLine) ==
                    E_INVALIDARG &&
                commandLine.empty(),
            "a mixed selection is rejected as a whole");
    const std::wstring hugePath = L"C:\\" + std::wstring(33000, L'x') + L".png";
    require(buildCommandLine(executable, {hugePath}, &commandLine) ==
                    HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE) &&
                commandLine.empty(),
            "oversized selections do not truncate or partially launch");
    require(buildCommandLine(executable, paths, nullptr) == E_POINTER,
            "command-line output pointers are checked");
}

void commandBehavior() {
    ComPtr<IExplorerCommand> command;
    require(createEditCommand(GetModuleHandleW(nullptr), captureLaunch, &command,
                              readMenuPreference) == S_OK,
            "native command can be constructed without application dependencies");
    const std::vector<Entry> selection{
        {L"C:\\images\\one.png"}, {L"\\\\server\\share\\图片 two.JPEG"}, {L"C:\\notes\\three.md"}};
    const auto valid = Microsoft::WRL::Make<ShellItems>(selection);
    EXPCMDSTATE state = ECS_HIDDEN;
    require(command->GetState(valid.Get(), FALSE, &state) == S_OK && state == ECS_ENABLED,
            "modern menu enables supported mixed image and document selections");
    require(command->Invoke(valid.Get(), nullptr) == S_OK && launchCount == 1,
            "a complete multiple selection launches Snow Shot once");
    std::wstring expectedExecutable(32768, L'\0');
    expectedExecutable.resize(GetModuleFileNameW(nullptr, expectedExecutable.data(),
                                                 static_cast<DWORD>(expectedExecutable.size())));
    expectedExecutable.resize(expectedExecutable.find_last_of(L"\\/") + 1);
    expectedExecutable.append(L"snow_shot.exe");
    require(launchedExecutable == expectedExecutable,
            "launch targets snow_shot.exe beside the extension instead of the COM surrogate");
    const auto arguments = parseCommandLine(launchedCommandLine);
    require(arguments == std::vector<std::wstring>{expectedExecutable, L"--edit-with-snow-shot",
                                                   selection[0].path, L"--edit-with-snow-shot",
                                                   selection[1].path, L"--edit-with-snow-shot",
                                                   selection[2].path},
            "Invoke forwards every supported selected path with its own typed operand");

    menuPreference.mode = ShellContextMenuMode::Disabled;
    require(command->GetState(valid.Get(), FALSE, &state) == S_OK && state == ECS_HIDDEN &&
                command->Invoke(valid.Get(), nullptr) == E_ACCESSDENIED && launchCount == 1,
            "disabling hides an existing command and prevents stale menu invocations");
    menuPreference.mode = ShellContextMenuMode::ImageAndDocumentFiles;
    require(command->GetState(valid.Get(), FALSE, &state) == S_OK && state == ECS_ENABLED,
            "reenabling is visible without recreating the command or restarting Explorer");

    menuPreference.mode = ShellContextMenuMode::ImageFiles;
    const auto images = Microsoft::WRL::Make<ShellItems>(
        std::vector<Entry>{{L"C:\\images\\one.PNG"}, {L"C:\\images\\two.JPEG"}});
    require(command->GetState(images.Get(), FALSE, &state) == S_OK && state == ECS_ENABLED,
            "image-only mode accepts multiple images case-insensitively");
    require(command->GetState(valid.Get(), FALSE, &state) == S_OK && state == ECS_HIDDEN &&
                command->Invoke(valid.Get(), nullptr) == E_INVALIDARG && launchCount == 1,
            "switching to image-only hides mixed selections and rejects stale invocations");
    for (const auto extension : kSupportedExtensions) {
        const auto path = std::wstring(L"C:\\files\\item") + std::wstring(extension);
        const bool image =
            std::find(std::begin(kSupportedImageExtensions), std::end(kSupportedImageExtensions),
                      extension) != std::end(kSupportedImageExtensions);
        require(isSupportedPath(path, ShellContextMenuMode::ImageFiles) == image,
                "image-only mode excludes every supported document suffix");
    }
    menuPreference.mode = ShellContextMenuMode::ImageAndDocumentFiles;

    for (const auto& entries :
         {std::vector<Entry>{}, std::vector<Entry>{{L"C:\\images\\valid.png"}, {L"C:\\bad.exe"}},
          std::vector<Entry>{{L"C:\\images\\directory.png", SFGAO_FILESYSTEM | SFGAO_FOLDER}},
          std::vector<Entry>{{L"C:\\images\\virtual.png", 0}},
          std::vector<Entry>{{L"C:\\images\\missing.png", SFGAO_FILESYSTEM, E_ACCESSDENIED}},
          std::vector<Entry>{
              {L"C:\\images\\blocked.png", SFGAO_FILESYSTEM, S_OK, E_ACCESSDENIED}}}) {
        const auto invalid = Microsoft::WRL::Make<ShellItems>(entries);
        state = ECS_ENABLED;
        require(command->GetState(invalid.Get(), FALSE, &state) == S_OK && state == ECS_HIDDEN,
                "invalid, folder, virtual, unavailable, and empty selections hide the command");
        require(FAILED(command->Invoke(invalid.Get(), nullptr)) && launchCount == 1,
                "invocation revalidates the selection before any process starts");
    }
    state = ECS_ENABLED;
    require(command->GetState(nullptr, FALSE, &state) == S_OK && state == ECS_HIDDEN &&
                command->Invoke(nullptr, nullptr) == E_INVALIDARG,
            "folder backgrounds cannot invoke a file command");
    launchResult = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    require(command->Invoke(valid.Get(), nullptr) == launchResult,
            "process launch failure propagates to Explorer");

    PWSTR title = nullptr;
    require(command->GetTitle(nullptr, &title) == S_OK && title && title[0] != L'\0',
            "command title loads from the package's localized native resources");
    const std::wstring unassignedTitle(title);
    CoTaskMemFree(title);
    for (const wchar_t accessKey : {L'E', L'7', L'\0'}) {
        menuPreference.accessKey = accessKey;
        const auto expected = unassignedTitle + (accessKey ? std::wstring(L" (&") + accessKey + L")"
                                                           : std::wstring());
        require(command->GetTitle(nullptr, &title) == S_OK && title && title == expected,
                "existing commands reflect assigned, changed and cleared classic menu mnemonics");
        CoTaskMemFree(title);
    }
    PWSTR icon = nullptr;
    require(command->GetIcon(nullptr, &icon) == S_OK && icon &&
                std::wstring_view(icon).ends_with(L",-102"),
            "modern menu obtains the native extension's embedded application icon");
    CoTaskMemFree(icon);
    GUID canonical{};
    EXPCMDFLAGS flags = ECF_HASSUBCOMMANDS;
    require(command->GetCanonicalName(&canonical) == S_OK && canonical == kEditCommandClsid &&
                command->GetFlags(&flags) == S_OK && flags == ECF_DEFAULT,
            "one stable, non-flyout command is exposed to Explorer");
    require(command->GetTitle(nullptr, nullptr) == E_POINTER &&
                command->GetIcon(nullptr, nullptr) == E_POINTER &&
                command->GetState(nullptr, FALSE, nullptr) == E_POINTER &&
                command->GetFlags(nullptr) == E_POINTER &&
                command->GetCanonicalName(nullptr) == E_POINTER,
            "COM output pointer contracts are respected");
}

void comLifecycle() {
    require(DllCanUnloadNow() == S_OK, "an idle extension can unload");
    ComPtr<IClassFactory> factory;
    require(DllGetClassObject(kEditCommandClsid, IID_PPV_ARGS(&factory)) == S_OK &&
                DllCanUnloadNow() == S_FALSE,
            "manifest CLSID activates a tracked COM class factory");
    require(factory->LockServer(TRUE) == S_OK, "COM can lock the extension server");
    ComPtr<IExplorerCommand> command;
    require(factory->CreateInstance(nullptr, IID_PPV_ARGS(&command)) == S_OK,
            "factory exposes IExplorerCommand for packaged Explorer activation");
    ComPtr<IUnknown> identity;
    require(command.As(&identity) == S_OK, "command has a canonical IUnknown identity");
    IExplorerCommand* aggregated = nullptr;
    require(factory->CreateInstance(identity.Get(), IID_PPV_ARGS(&aggregated)) ==
                    CLASS_E_NOAGGREGATION &&
                aggregated == nullptr,
            "COM aggregation is rejected without creating an object");
    identity.Reset();
    command.Reset();
    require(factory->LockServer(FALSE) == S_OK, "server lock is released");
    factory.Reset();
    require(DllCanUnloadNow() == S_OK, "factory and command releases allow DLL unload");
    IClassFactory* unknown = nullptr;
    require(DllGetClassObject(GUID_NULL, IID_PPV_ARGS(&unknown)) == CLASS_E_CLASSNOTAVAILABLE &&
                unknown == nullptr,
            "unknown class registration cannot instantiate the command");
}

template <typename Function> Function exportedFunction(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    require(address != nullptr, "native DLL exports the undecorated COM activation entry points");
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    memcpy(&function, &address, sizeof(function));
    return function;
}

std::wstring titleResource(HMODULE module, WORD language) {
    const HRSRC resource = FindResourceExW(module, RT_STRING, MAKEINTRESOURCEW(7), language);
    require(resource != nullptr, "the native title has a resource for each package language");
    const auto* text = static_cast<const wchar_t*>(LockResource(LoadResource(module, resource)));
    require(text != nullptr, "the native string table can be read");
    for (int index = 0; index < (101 % 16); ++index)
        text += 1 + static_cast<size_t>(*text);
    return std::wstring(text + 1, static_cast<size_t>(*text));
}

void localizedTitles(HMODULE module, IExplorerCommand* command) {
    ULONG count = 0;
    ULONG size = 0;
    require(GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) != FALSE,
            "the test saves the thread's original UI language preferences");
    std::vector<wchar_t> originalLanguages(size);
    require(GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, &count, originalLanguages.data(),
                                          &size) != FALSE,
            "the test reads the thread's original UI language preferences");
    struct PreferredLanguage {
        const wchar_t* names;
        WORD language;
    };
    const PreferredLanguage languages[]{
        {L"en-US\0", MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)},
        {L"zh-CN\0", MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)},
        {L"zh-TW\0", MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)},
        {L"de-DE\0en-US\0", MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)}};
    for (const auto& [names, language] : languages) {
        require(SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, names, &count) != FALSE,
                "the test selects a Windows shell UI language preference");
        PWSTR title = nullptr;
        require(command->GetTitle(nullptr, &title) == S_OK && title &&
                    title == titleResource(module, language),
                "the actual command follows Windows preferred UI language and fallback ordering");
        CoTaskMemFree(title);
        ComPtr<IExplorerCommand> assigned;
        require(createEditCommand(module, captureLaunch, &assigned, readMenuPreference) == S_OK,
                "create a command with the saved access key and the DLL's native resources");
        menuPreference.accessKey = L'E';
        require(assigned->GetTitle(nullptr, &title) == S_OK && title &&
                    title == titleResource(module, language) + L" (&E)",
                "every localized menu title keeps the assigned access key");
        CoTaskMemFree(title);
        menuPreference.accessKey = L'\0';
    }
    require(SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, originalLanguages.data(), &count) !=
                FALSE,
            "the test restores the original thread UI language preferences");
}

void packagedDll(const wchar_t* path) {
    const HMODULE module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    require(module != nullptr, "the packaged native DLL loads without the Qt application runtime");
    const auto getClassObject =
        exportedFunction<decltype(&DllGetClassObject)>(module, "DllGetClassObject");
    const auto canUnloadNow =
        exportedFunction<decltype(&DllCanUnloadNow)>(module, "DllCanUnloadNow");
    require(canUnloadNow() == S_OK, "newly loaded DLL is initially idle");
    {
        ComPtr<IClassFactory> factory;
        require(getClassObject(kEditCommandClsid, IID_PPV_ARGS(&factory)) == S_OK,
                "actual DLL resolves the class registered by the package manifest");
        ComPtr<IExplorerCommand> command;
        require(factory->CreateInstance(nullptr, IID_PPV_ARGS(&command)) == S_OK,
                "actual DLL creates the Windows Explorer command");
        std::wstring expectedIcon(32768, L'\0');
        expectedIcon.resize(GetModuleFileNameW(module, expectedIcon.data(),
                                               static_cast<DWORD>(expectedIcon.size())));
        expectedIcon.append(L",-102");
        PWSTR icon = nullptr;
        require(command->GetIcon(nullptr, &icon) == S_OK && icon && icon == expectedIcon,
                "DllMain captures the DLL's own module rather than the shell host executable");
        CoTaskMemFree(icon);
        PWSTR title = nullptr;
        require(command->GetTitle(nullptr, &title) == S_OK && title && title[0] != L'\0',
                "actual DLL includes localized command title resources");
        CoTaskMemFree(title);
        localizedTitles(module, command.Get());
        require(canUnloadNow() == S_FALSE, "Explorer's live command keeps the actual DLL loaded");
    }
    require(canUnloadNow() == S_OK, "actual DLL unloads after Explorer releases its COM objects");
    FreeLibrary(module);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    require(argc == 2, "pass the built Explorer command DLL for activation smoke tests");
    pathAndCommandLinePolicy();
    commandBehavior();
    comLifecycle();
    packagedDll(argv[1]);
    return EXIT_SUCCESS;
}
