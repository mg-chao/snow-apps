#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <UIAutomation.h>
#include <objbase.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

// This driver starts snow_shot in the background (the main window is never
// shown; its presence skews the scenario) and drives the capture for several
// consecutive rounds through a global screenshot hotkey.  Each round
// drags out a rectangle and injects Ctrl+C.  The shortcut is considered
// recognized only when the capture ends and the clipboard receives a valid
// image.  The scenario repeats because the regression this guards against
// only drops the hotkey-triggered capture on a later attempt.
//
// The hotkey binding is seeded into the isolated e2e storage instance before
// the application starts: <APPDATA>/SnowShot/snow_shot-e2e-<pid>/config.json.
// F24 is used so the seeded binding cannot collide with the hotkeys of a
// real snow_shot instance running on the same machine.

constexpr wchar_t kMainWindowName[] = L"SnowShot";
constexpr wchar_t kToolbarButtonAutomationIdSuffix[] = L".screenshotScrollingScreenshotButton";

constexpr LONG kSelectionLeft = 64;
constexpr LONG kSelectionTop = 64;
constexpr LONG kSelectionRight = 564;
constexpr LONG kSelectionBottom = 564;

constexpr int kRoundCount = 5;

template <typename T> class ComPtr final {
  public:
    ComPtr() = default;
    ~ComPtr() {
        reset();
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : m_pointer(other.detach()) {}

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset(other.detach());
        }
        return *this;
    }

    [[nodiscard]] T* get() const {
        return m_pointer;
    }

    T** put() {
        reset();
        return &m_pointer;
    }

    T* detach() {
        T* const pointer = m_pointer;
        m_pointer = nullptr;
        return pointer;
    }

    void reset(T* pointer = nullptr) {
        if (m_pointer != nullptr) {
            m_pointer->Release();
        }
        m_pointer = pointer;
    }

  private:
    T* m_pointer = nullptr;
};

class ScopedCom final {
  public:
    ScopedCom() {
        m_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }

    ~ScopedCom() {
        if (SUCCEEDED(m_result)) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const {
        return m_result;
    }

  private:
    HRESULT m_result = E_FAIL;
};

class ScopedProcess final {
  public:
    ScopedProcess() = default;
    ~ScopedProcess() {
        terminate();
    }

    ScopedProcess(const ScopedProcess&) = delete;
    ScopedProcess& operator=(const ScopedProcess&) = delete;

    bool start(const std::wstring& executablePath) {
        std::wstring command = L'"' + executablePath +
                               L"\" --e2e-allow-overlay-capture --e2e-instance-id=" +
                               std::to_wstring(GetCurrentProcessId());
        std::vector<wchar_t> commandLine(command.begin(), command.end());
        commandLine.push_back(L'\0');

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessW(executablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &startupInfo, &processInfo)) {
            return false;
        }

        CloseHandle(processInfo.hThread);
        m_process = processInfo.hProcess;
        m_processId = processInfo.dwProcessId;
        return true;
    }

    [[nodiscard]] DWORD processId() const {
        return m_processId;
    }

    [[nodiscard]] bool isRunning() const {
        return m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
    }

  private:
    void terminate() {
        if (m_process == nullptr) {
            return;
        }

        if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(m_process, 1);
            WaitForSingleObject(m_process, 5000);
        }
        CloseHandle(m_process);
        m_process = nullptr;
    }

    HANDLE m_process = nullptr;
    DWORD m_processId = 0;
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] std::wstring executablePathFromArguments(int argc, char* argv[]) {
    require(argc == 2, "expected the snow_shot executable path as the only argument");

    const int requiredLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, nullptr, 0);
    require(requiredLength > 0, "could not convert the executable path to UTF-16");

    std::wstring path(static_cast<std::size_t>(requiredLength), L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, path.data(),
                                requiredLength) == requiredLength,
            "could not convert the executable path to UTF-16");
    path.pop_back();
    return path;
}

// The e2e instance id isolates the application's storage under
// %APPDATA%/SnowShot/snow_shot-e2e-<test pid>/, so the hotkey binding can be
// seeded without touching the user's real configuration.
[[nodiscard]] std::wstring e2eStorageDirectory() {
    wchar_t appData[MAX_PATH]{};
    require(GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) != 0,
            "could not resolve the APPDATA directory for the e2e configuration");
    return std::wstring(appData) + L"\\SnowShot\\snow_shot-e2e-" +
           std::to_wstring(GetCurrentProcessId());
}

void writeSeededConfiguration(const std::wstring& storageDirectory) {
    const std::wstring snowShotDirectory =
        storageDirectory.substr(0, storageDirectory.rfind(L'\\'));
    require(CreateDirectoryW(snowShotDirectory.c_str(), nullptr) != FALSE ||
                GetLastError() == ERROR_ALREADY_EXISTS,
            "could not create the SnowShot configuration directory");
    require(CreateDirectoryW(storageDirectory.c_str(), nullptr) != FALSE ||
                GetLastError() == ERROR_ALREADY_EXISTS,
            "could not create the e2e configuration directory");

    const std::wstring configurationPath = storageDirectory + L"\\config.json";
    const HANDLE file =
        CreateFileW(configurationPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "could not seed the e2e configuration file");
    // storage/schema_version is mandatory; without it the store discards the
    // seeded document as an invalid schema.
    static constexpr char kSeededDocument[] =
        R"({"storage":{"schema_version":1},"global_shortcuts":{"screenshot":["F24"]}})";
    DWORD written = 0;
    const BOOL writeResult =
        WriteFile(file, kSeededDocument, static_cast<DWORD>(sizeof(kSeededDocument) - 1), &written,
                  nullptr);
    CloseHandle(file);
    require(writeResult != FALSE && written == sizeof(kSeededDocument) - 1,
            "could not write the seeded e2e configuration");
}

void removeSeededStorage(const std::wstring& directory) {
    const std::wstring pattern = directory + L"\\*";
    WIN32_FIND_DATAW findData{};
    const HANDLE search = FindFirstFileW(pattern.c_str(), &findData);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring child = directory + L"\\" + findData.cFileName;
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                removeSeededStorage(child);
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(search, &findData));
        FindClose(search);
    }
    RemoveDirectoryW(directory.c_str());
}

[[nodiscard]] ComPtr<IUIAutomationElement>
findProcessDescendantByAutomationIdSuffix(IUIAutomation& automation, DWORD processId,
                                          const wchar_t* automationIdSuffix) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation.GetRootElement(root.put()))) {
        return {};
    }

    VARIANT processIdValue;
    VariantInit(&processIdValue);
    processIdValue.vt = VT_I4;
    processIdValue.lVal = static_cast<LONG>(processId);
    ComPtr<IUIAutomationCondition> processCondition;
    const HRESULT processConditionResult = automation.CreatePropertyCondition(
        UIA_ProcessIdPropertyId, processIdValue, processCondition.put());
    VariantClear(&processIdValue);
    if (FAILED(processConditionResult)) {
        return {};
    }

    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(
            root.get()->FindAll(TreeScope_Descendants, processCondition.get(), elements.put()))) {
        return {};
    }

    int length = 0;
    if (FAILED(elements.get()->get_Length(&length))) {
        return {};
    }

    const std::size_t suffixLength = std::wcslen(automationIdSuffix);
    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements.get()->GetElement(index, element.put()))) {
            continue;
        }

        BSTR automationId = nullptr;
        if (FAILED(element.get()->get_CurrentAutomationId(&automationId))) {
            continue;
        }
        const std::size_t automationIdLength = SysStringLen(automationId);
        const bool matches = automationIdLength >= suffixLength &&
                             std::wmemcmp(automationId + automationIdLength - suffixLength,
                                          automationIdSuffix, suffixLength) == 0;
        SysFreeString(automationId);
        if (matches) {
            return element;
        }
    }
    return {};
}

void reportProcessUiAutomationElements(IUIAutomation& automation, DWORD processId) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation.GetRootElement(root.put()))) {
        return;
    }

    VARIANT processIdValue;
    VariantInit(&processIdValue);
    processIdValue.vt = VT_I4;
    processIdValue.lVal = static_cast<LONG>(processId);
    ComPtr<IUIAutomationCondition> processCondition;
    const HRESULT conditionResult = automation.CreatePropertyCondition(
        UIA_ProcessIdPropertyId, processIdValue, processCondition.put());
    VariantClear(&processIdValue);
    if (FAILED(conditionResult)) {
        return;
    }

    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(
            root.get()->FindAll(TreeScope_Descendants, processCondition.get(), elements.put()))) {
        return;
    }

    int length = 0;
    if (FAILED(elements.get()->get_Length(&length))) {
        return;
    }

    std::wcerr << L"UIA elements exposed by snow_shot:\n";
    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements.get()->GetElement(index, element.put()))) {
            continue;
        }
        BSTR name = nullptr;
        BSTR automationId = nullptr;
        if (SUCCEEDED(element.get()->get_CurrentName(&name)) &&
            SUCCEEDED(element.get()->get_CurrentAutomationId(&automationId)) &&
            ((name != nullptr && name[0] != L'\0') ||
             (automationId != nullptr && automationId[0] != L'\0'))) {
            std::wcerr << L"  name='" << (name != nullptr ? name : L"") << L"', automationId='"
                       << (automationId != nullptr ? automationId : L"") << L"'\n";
        }
        SysFreeString(name);
        SysFreeString(automationId);
    }
}

template <typename Finder>
[[nodiscard]] ComPtr<IUIAutomationElement> waitForElement(Finder&& finder,
                                                          std::chrono::steady_clock::duration
                                                              timeout = 5s) {
    constexpr auto pollInterval = 50ms;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        ComPtr<IUIAutomationElement> element = finder();
        if (element.get() != nullptr) {
            return element;
        }
        std::this_thread::sleep_for(pollInterval);
    } while (std::chrono::steady_clock::now() < deadline);
    return {};
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate, std::chrono::steady_clock::duration timeout) {
    constexpr auto pollInterval = 50ms;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(pollInterval);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

[[nodiscard]] bool windowBelongsToProcess(HWND window, DWORD processId) {
    if (window == nullptr) {
        return false;
    }
    DWORD ownerProcess = 0;
    GetWindowThreadProcessId(window, &ownerProcess);
    return ownerProcess == processId;
}

// WindowShortcutManager is installed as an application-wide event filter, so
// the configured Ctrl+C binding is dispatched whenever any capture window of
// the process (overlay or floating toolbar) owns the keyboard.
[[nodiscard]] bool captureOwnsForeground(DWORD processId) {
    const HWND foreground = GetForegroundWindow();
    return foreground != nullptr && windowBelongsToProcess(foreground, processId);
}

void reportForegroundWindow() {
    const HWND foreground = GetForegroundWindow();
    DWORD foregroundProcess = 0;
    if (foreground != nullptr) {
        GetWindowThreadProcessId(foreground, &foregroundProcess);
    }
    wchar_t title[256]{};
    wchar_t className[256]{};
    if (foreground != nullptr) {
        GetWindowTextW(foreground, title, static_cast<int>(std::size(title)));
        GetClassNameW(foreground, className, static_cast<int>(std::size(className)));
    }
    std::wcerr << L"foreground window: hwnd=" << foreground << L", pid=" << foregroundProcess
               << L", class='" << className << L"', title='" << title << L"'\n";
}

struct OverlaySearchContext {
    DWORD processId;
    HWND result;
};

BOOL CALLBACK overlaySearchCallback(HWND window, LPARAM param) {
    auto* context = reinterpret_cast<OverlaySearchContext*>(param);
    if (!windowBelongsToProcess(window, context->processId) || !IsWindowVisible(window)) {
        return TRUE;
    }
    RECT rect{};
    if (GetWindowRect(window, &rect) == FALSE) {
        return TRUE;
    }
    // The capture overlay covers the region the test drags across; no other
    // visible top-level window of the process is expected at this point.
    if (rect.left <= kSelectionLeft && rect.top <= kSelectionTop &&
        rect.right >= kSelectionRight && rect.bottom >= kSelectionBottom) {
        context->result = window;
        return FALSE;
    }
    return TRUE;
}

// The hotkey starts the capture regardless of which window held the
// foreground, so find the overlay as the process's visible top-level window
// covering the selection region instead of relying on foreground changes.
[[nodiscard]] HWND waitForOverlayWindow(DWORD processId) {
    HWND overlay = nullptr;
    const bool found = waitUntil(
        [&]() {
            OverlaySearchContext context{processId, nullptr};
            EnumWindows(overlaySearchCallback, reinterpret_cast<LPARAM>(&context));
            overlay = context.result;
            return overlay != nullptr;
        },
        10s);
    return found ? overlay : nullptr;
}

// A hotkey injected through SendInput does not always grant the application
// the foreground rights a physical keypress would, which can leave the
// freshly shown overlay in the background.  Attach both the driver's thread
// and the overlay's thread to the foreground thread, then let the caller
// retry until Windows accepts the activation.
[[nodiscard]] bool tryActivateWindow(HWND window) {
    const HWND foreground = GetForegroundWindow();
    if (foreground == window) {
        return true;
    }

    const DWORD currentThread = GetCurrentThreadId();
    const DWORD targetThread = GetWindowThreadProcessId(window, nullptr);
    const DWORD foregroundThread =
        foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attachedCurrent = foregroundThread != 0 && foregroundThread != currentThread &&
                                 AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    const bool attachedTarget =
        foregroundThread != 0 && targetThread != 0 && targetThread != foregroundThread &&
        AttachThreadInput(targetThread, foregroundThread, TRUE) != FALSE;
    BringWindowToTop(window);
    const BOOL setResult = SetForegroundWindow(window);
    if (attachedTarget) {
        AttachThreadInput(targetThread, foregroundThread, FALSE);
    }
    if (attachedCurrent) {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
    return setResult != FALSE || GetForegroundWindow() == window;
}

struct WindowTitleSearchContext {
    DWORD processId;
    const wchar_t* title;
    HWND result;
};

BOOL CALLBACK windowTitleSearchCallback(HWND window, LPARAM param) {
    auto* context = reinterpret_cast<WindowTitleSearchContext*>(param);
    if (!windowBelongsToProcess(window, context->processId) || !IsWindowVisible(window)) {
        return TRUE;
    }
    wchar_t title[256]{};
    if (GetWindowTextW(window, title, static_cast<int>(std::size(title))) > 0 &&
        wcscmp(title, context->title) == 0) {
        context->result = window;
        return FALSE;
    }
    return TRUE;
}

// The scenario must run without the main window; any visible top-level window
// carrying its title means the application opened it unexpectedly.
[[nodiscard]] HWND findVisibleWindowByTitle(DWORD processId, const wchar_t* title) {
    WindowTitleSearchContext context{processId, title, nullptr};
    EnumWindows(windowTitleSearchCallback, reinterpret_cast<LPARAM>(&context));
    return context.result;
}

void moveCursor(LONG x, LONG y) {
    require(SetCursorPos(x, y) != FALSE, "could not move the cursor");
}

void moveCursorWithInput(LONG x, LONG y) {
    const LONG virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    require(virtualWidth > 1 && virtualHeight > 1,
            "the virtual desktop is too small for absolute mouse input");

    INPUT move{};
    move.type = INPUT_MOUSE;
    move.mi.dx = static_cast<LONG>((static_cast<long long>(x - virtualLeft) * 65535LL) /
                                   static_cast<long long>(virtualWidth - 1));
    move.mi.dy = static_cast<LONG>((static_cast<long long>(y - virtualTop) * 65535LL) /
                                   static_cast<long long>(virtualHeight - 1));
    move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    require(SendInput(1, &move, sizeof(move)) == 1, "could not send mouse movement");
}

void dragSelect500By500() {
    moveCursor(kSelectionLeft, kSelectionTop);

    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    require(SendInput(1, &down, sizeof(down)) == 1, "could not press the left mouse button");

    // Let the overlay consume the press before the movement.  Without this
    // boundary Windows can coalesce the messages and leave the app in its
    // intelligent-selection state instead of starting a manual rectangle.
    std::this_thread::sleep_for(25ms);

    moveCursorWithInput(kSelectionRight, kSelectionBottom);
    std::this_thread::sleep_for(25ms);

    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    require(SendInput(1, &up, sizeof(up)) == 1, "could not release the left mouse button");
}

void sendCtrlCDown() {
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_CONTROL;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = 'C';
    require(SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT)) ==
                std::size(input),
            "could not inject the Ctrl+C press");
}

void sendCtrlCUp() {
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = 'C';
    input[0].ki.dwFlags = KEYEVENTF_KEYUP;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = VK_CONTROL;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT)) ==
                std::size(input),
            "could not inject the Ctrl+C release");
}

void sendHotkey() {
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_F24;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = VK_F24;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT)) ==
                std::size(input),
            "could not inject the screenshot hotkey");
}

class ScopedClipboard final {
  public:
    ScopedClipboard() = default;
    ~ScopedClipboard() {
        if (m_open) {
            CloseClipboard();
        }
    }

    ScopedClipboard(const ScopedClipboard&) = delete;
    ScopedClipboard& operator=(const ScopedClipboard&) = delete;

    [[nodiscard]] bool open() {
        constexpr int maxAttempts = 20;
        for (int attempt = 0; attempt < maxAttempts && !m_open; ++attempt) {
            m_open = OpenClipboard(nullptr) != FALSE;
            if (!m_open) {
                std::this_thread::sleep_for(50ms);
            }
        }
        return m_open;
    }

  private:
    bool m_open = false;
};

void clearClipboard() {
    ScopedClipboard clipboard;
    require(clipboard.open() && EmptyClipboard() != FALSE,
            "could not clear the clipboard before Ctrl+C");
}

[[nodiscard]] bool dibDataIsValid(HGLOBAL data) {
    if (data == nullptr) {
        return false;
    }
    const SIZE_T size = GlobalSize(data);
    if (size < sizeof(BITMAPINFOHEADER)) {
        return false;
    }
    const auto* header = static_cast<const BITMAPINFOHEADER*>(GlobalLock(data));
    if (header == nullptr) {
        return false;
    }
    const bool valid = header->biSize >= sizeof(BITMAPINFOHEADER) &&
                       static_cast<SIZE_T>(header->biSize) <= size && header->biWidth > 0 &&
                       header->biHeight != 0 && header->biPlanes == 1 && header->biBitCount >= 24;
    GlobalUnlock(data);
    return valid;
}

[[nodiscard]] bool pngDataIsValid(HGLOBAL data) {
    if (data == nullptr) {
        return false;
    }
    constexpr unsigned char signature[] = {0x89, 'P', 'N', 'G'};
    const SIZE_T size = GlobalSize(data);
    if (size < sizeof(signature)) {
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(GlobalLock(data));
    if (bytes == nullptr) {
        return false;
    }
    const bool valid = std::memcmp(bytes, signature, sizeof(signature)) == 0;
    GlobalUnlock(data);
    return valid;
}

// The copy is exported asynchronously, so recognition is judged by the
// clipboard result: a structurally valid screenshot image must arrive.
[[nodiscard]] bool clipboardContainsValidImage() {
    ScopedClipboard clipboard;
    if (!clipboard.open()) {
        return false;
    }

    for (const UINT format : {CF_DIBV5, CF_DIB}) {
        if (IsClipboardFormatAvailable(format) && dibDataIsValid(GetClipboardData(format))) {
            return true;
        }
    }
    if (IsClipboardFormatAvailable(CF_BITMAP) && GetClipboardData(CF_BITMAP) != nullptr) {
        return true;
    }
    static const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (pngFormat != 0 && IsClipboardFormatAvailable(pngFormat) &&
        pngDataIsValid(GetClipboardData(pngFormat))) {
        return true;
    }
    return false;
}

void requireDesktopCanRunScenario() {
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    require(screenWidth >= kSelectionRight && screenHeight >= kSelectionBottom,
            "the primary display must contain the 64,64-564,564 selection region");
}

// Messages are numbered by round so a failure points at the exact attempt.
[[nodiscard]] std::string roundMessage(int round, const char* text) {
    return "round " + std::to_string(round + 1) + ": " + text;
}

void requireRound(bool condition, int round, const char* text) {
    if (!condition) {
        throw std::runtime_error(roundMessage(round, text));
    }
}

void runCtrlCRound(IUIAutomation& automation, const ScopedProcess& application, int round) {
    // Remember the foreground window before the capture starts; the Ctrl+C
    // release below is deliberately routed away from the application (see the
    // release site for why).
    const HWND foregroundBeforeCapture = GetForegroundWindow();
    sendHotkey();
    // The injected hotkey does not always hand the application the foreground
    // rights a physical keypress would; lend it the driver's rights (best
    // effort) so its own overlay activation behaves like real usage.
    AllowSetForegroundWindow(application.processId());

    const HWND overlay = waitForOverlayWindow(application.processId());
    requireRound(overlay != nullptr, round,
                 "the screenshot hotkey did not show the capture overlay");
    if (!waitUntil([&]() { return GetForegroundWindow() == overlay; }, 1500ms)) {
        std::cout << "round " << round + 1
                  << ": the capture overlay did not take the foreground on its own; "
                     "activating it from the driver\n";
        reportForegroundWindow();
        requireRound(waitUntil([&]() { return tryActivateWindow(overlay); }, 5s), round,
                     "the screenshot capture overlay could not be activated");
    }
    if (!captureOwnsForeground(application.processId())) {
        reportForegroundWindow();
        throw std::runtime_error(roundMessage(round,
                                              "the screenshot capture UI did not own "
                                              "keyboard/mouse input before the area drag"));
    }

    // Give the overlay a beat to settle after activation so the press below
    // starts a manual selection instead of being consumed as an activation
    // click.
    std::this_thread::sleep_for(300ms);
    dragSelect500By500();

    ComPtr<IUIAutomationElement> toolbarButton = waitForElement([&]() {
        return findProcessDescendantByAutomationIdSuffix(automation, application.processId(),
                                                         kToolbarButtonAutomationIdSuffix);
    });
    if (toolbarButton.get() == nullptr) {
        reportProcessUiAutomationElements(automation, application.processId());
    }
    requireRound(toolbarButton.get() != nullptr, round,
                 "the screenshot selection did not expose its toolbar after the area drag");
    if (!captureOwnsForeground(application.processId())) {
        reportForegroundWindow();
        throw std::runtime_error(roundMessage(round,
                                              "the screenshot capture UI did not own keyboard "
                                              "input before Ctrl+C with a selected area"));
    }

    clearClipboard();
    // Hold the chord until the capture has fully ended, then release it. This
    // matches physical usage, where the release lands after the capture UI is
    // already gone and is therefore delivered to whatever regained the
    // foreground. Sending the press and release as one batch would hide the
    // lost-release regression this test guards against.
    sendCtrlCDown();

    // The export runs asynchronously; an unrecognized shortcut never lands an
    // image on the clipboard, which is what fails this round.
    const bool copied = waitUntil(clipboardContainsValidImage, 10s);
    if (!copied) {
        // Diagnose the failure before failing the round. The guarded
        // regression swallows the first press only: releasing the chord onto
        // the still-open overlay lets the platform drop its stale pressed-key
        // record, so an immediate retry succeeds. Confirming that signature
        // keeps this failure specific to the lost key release instead of an
        // unrelated export/clipboard problem.
        sendCtrlCUp();
        sendCtrlCDown();
        const bool recovered = waitUntil(clipboardContainsValidImage, 10s);
        sendCtrlCUp();
        if (recovered) {
            throw std::runtime_error(
                roundMessage(round,
                             "the first Ctrl+C press was swallowed and only the retry copied: "
                             "the previous capture's key release never reached the app, so the "
                             "press arrived mislabeled as an auto-repeat"));
        }
        throw std::runtime_error(
            roundMessage(round, "Ctrl+C did not complete the screenshot copy to the clipboard"));
    }

    const bool captureEnded =
        waitUntil([&]() { return !IsWindowVisible(overlay) || GetForegroundWindow() != overlay; },
                  5s);
    requireRound(captureEnded, round,
                 "the screenshot did not end after Ctrl+C with a selected area");

    // Match physical usage: the user releases the chord only once the capture
    // UI is gone, so the release is routed to whatever window owns the
    // foreground then.  Windows does not reassign the foreground immediately
    // when the foreground window hides, so without help the release could
    // still land on the hidden overlay, reach the application, and mask the
    // lost-release regression.  Force the foreground to a window that does
    // not belong to the application and wait for the switch before releasing;
    // if that cannot be achieved the round must fail loudly rather than
    // silently skip the regression scenario.
    HWND releaseTarget = foregroundBeforeCapture;
    if (releaseTarget == nullptr || IsWindow(releaseTarget) == FALSE ||
        windowBelongsToProcess(releaseTarget, application.processId())) {
        releaseTarget = GetShellWindow();
    }
    const HWND foregroundAfterCapture = GetForegroundWindow();
    if (foregroundAfterCapture == nullptr ||
        windowBelongsToProcess(foregroundAfterCapture, application.processId())) {
        requireRound(waitUntil([&]() { return tryActivateWindow(releaseTarget); }, 5s), round,
                     "could not hand the foreground back to a non-snow_shot window");
    }
    requireRound(waitUntil(
                     [&]() {
                         const HWND foreground = GetForegroundWindow();
                         return foreground != nullptr &&
                                !windowBelongsToProcess(foreground, application.processId());
                     },
                     5s),
                 round, "the foreground did not leave snow_shot before the Ctrl+C release");
    sendCtrlCUp();

    requireRound(application.isRunning(), round, "snow_shot exited after the Ctrl+C scenario");
    requireRound(findVisibleWindowByTitle(application.processId(), kMainWindowName) == nullptr,
                 round, "the main window unexpectedly appeared");
    std::cout << "round " << round + 1 << ": hotkey+Ctrl+C recognized\n";
}
} // namespace

int main(int argc, char* argv[]) {
    std::wstring storageDirectory;
    try {
        // Keep UIA, cursor positions, and window coordinates in physical pixels.
        SetProcessDPIAware();
        requireDesktopCanRunScenario();
        const std::wstring executablePath = executablePathFromArguments(argc, argv);

        storageDirectory = e2eStorageDirectory();
        removeSeededStorage(storageDirectory);
        writeSeededConfiguration(storageDirectory);

        const ScopedCom com;
        require(SUCCEEDED(com.result()), "could not initialize COM for UI Automation");

        ComPtr<IUIAutomation> automation;
        require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(automation.put()))),
                "could not create the UI Automation client");

        {
            ScopedProcess application;
            require(application.start(executablePath), "could not start snow_shot");

            std::this_thread::sleep_for(2s);

            require(findVisibleWindowByTitle(application.processId(), kMainWindowName) == nullptr,
                    "the main window unexpectedly appeared at startup");
            std::cout << "starting " << kRoundCount
                      << " hotkey rounds without showing the main window\n";
            for (int round = 0; round < kRoundCount; ++round) {
                runCtrlCRound(*automation.get(), application, round);
                if (round + 1 < kRoundCount) {
                    std::this_thread::sleep_for(500ms);
                }
            }
        }

        removeSeededStorage(storageDirectory);
        std::cout << "screenshot Ctrl+C end-to-end test passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (!storageDirectory.empty()) {
            removeSeededStorage(storageDirectory);
        }
        std::cerr << "screenshot Ctrl+C end-to-end test failed: " << error.what() << '\n';
        return 1;
    }
}
