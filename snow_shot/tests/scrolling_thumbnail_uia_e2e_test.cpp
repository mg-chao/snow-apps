#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <UIAutomation.h>
#include <objbase.h>

#include "snow_stitch_images.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

constexpr wchar_t kMainWindowName[] = L"SnowShot";
constexpr wchar_t kScreenshotControlAutomationIdSuffix[] = L".settings-item-quick-screenshot";
constexpr wchar_t kScrollingScreenshotControlAutomationIdSuffix[] =
    L".screenshotScrollingScreenshotButton";
constexpr wchar_t kScrollingThumbnailAutomationIdSuffix[] = L".screenshot-scrolling-thumbnail";
constexpr char kCaptureExcludedArgument[] = "--capture-excluded";

constexpr LONG kSelectionLeft = 64;
constexpr LONG kSelectionTop = 64;
constexpr LONG kSelectionRight = 564;
constexpr LONG kSelectionBottom = 564;
constexpr int kScrollCount = 10;
constexpr int kTextLineHeight = 36;
constexpr int kScrollDistance = 96;

constexpr wchar_t kArtifactsDirectory[] = L"e2e-artifacts";
constexpr char kThumbnailBeforeClicksFile[] = "e2e-artifacts/scrolling-thumbnail-before-clicks.bmp";
constexpr char kThumbnailAfterClicksFile[] = "e2e-artifacts/scrolling-thumbnail-after-clicks.bmp";
constexpr char kThumbnailBeforeDragFile[] = "e2e-artifacts/scrolling-thumbnail-before-drag.bmp";
constexpr char kThumbnailAfterDragFile[] = "e2e-artifacts/scrolling-thumbnail-after-drag.bmp";

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

    bool start(const std::wstring& executablePath, bool captureExcluded) {
        std::wstring command = L'"' + executablePath + L"\" --show-main-window";
        if (!captureExcluded) {
            command += L" --e2e-allow-overlay-capture";
        }
        command += L" --e2e-instance-id=" + std::to_wstring(GetCurrentProcessId());
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

struct ScrollingSourceState {
    int offset = 0;
};

[[nodiscard]] std::uint32_t nextRandom(std::uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

[[nodiscard]] std::wstring randomTextLine(int lineNumber) {
    constexpr wchar_t alphabet[] = L"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    constexpr std::size_t alphabetLength = std::size(alphabet) - 1;
    std::uint32_t randomState =
        0x6d2b79f5U ^ (static_cast<std::uint32_t>(lineNumber) * 0x9e3779b9U);
    std::wstring characters = L"L" + std::to_wstring(lineNumber);
    while (characters.size() < 8) {
        characters.push_back(alphabet[nextRandom(randomState) % alphabetLength]);
    }

    std::wstring line;
    for (std::size_t index = 0; index < characters.size(); ++index) {
        line.push_back(characters[index]);
        if (index + 1 < characters.size()) {
            line.append(1 + nextRandom(randomState) % 3, L' ');
        }
    }
    return line;
}

[[nodiscard]] std::uint32_t scrollingSourcePixel(int x, int documentY) {
    const std::uint32_t cellX = static_cast<std::uint32_t>(x / 16);
    const std::uint32_t cellY = static_cast<std::uint32_t>(documentY / 16);
    std::uint32_t hash = cellX * 0x9e3779b9U ^ cellY * 0x85ebca6bU;
    hash ^= hash >> 16U;
    int red = 30 + static_cast<int>(hash & 0xbfU);
    int green = 30 + static_cast<int>((hash >> 8U) & 0xbfU);
    int blue = 30 + static_cast<int>((hash >> 16U) & 0xbfU);
    if (x % 37 < 2 || documentY % 43 < 2) {
        red = 245;
        green = 245;
        blue = 245;
    } else if ((x + documentY) % 61 < 3) {
        red = 10;
        green = 10;
        blue = 10;
    }
    return static_cast<std::uint32_t>(blue | (green << 8) | (red << 16));
}

void presentScrollingSource(HWND window, const ScrollingSourceState& state) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC screen = GetDC(nullptr);
    require(screen != nullptr, "could not obtain the desktop DC for the scrolling source");
    HDC memory = CreateCompatibleDC(screen);
    require(memory != nullptr, "could not create the scrolling source memory DC");

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bitmapPixels = nullptr;
    HBITMAP bitmap =
        CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &bitmapPixels, nullptr, 0);
    require(bitmap != nullptr && bitmapPixels != nullptr,
            "could not allocate the scrolling source surface");

    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    auto* pixels = static_cast<std::uint32_t*>(bitmapPixels);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(x)] = scrollingSourcePixel(x, y + state.offset);
        }
    }

    HFONT font =
        CreateFontW(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    require(font != nullptr, "could not create the scrolling source font");
    HGDIOBJ previousFont = SelectObject(memory, font);
    SetBkMode(memory, TRANSPARENT);

    constexpr std::array<COLORREF, 10> textColors{
        RGB(255, 82, 82),   RGB(64, 196, 255),  RGB(105, 240, 174), RGB(225, 135, 255),
        RGB(255, 183, 77),  RGB(72, 255, 245),  RGB(255, 241, 118), RGB(255, 128, 191),
        RGB(238, 238, 238), RGB(179, 157, 219),
    };
    const int firstLine = state.offset / kTextLineHeight;
    const int firstLineY = firstLine * kTextLineHeight - state.offset;
    for (int lineNumber = firstLine, y = firstLineY; y < height;
         ++lineNumber, y += kTextLineHeight) {
        std::uint32_t colorState =
            0xa511e9b3U ^ (static_cast<std::uint32_t>(lineNumber) * 0x85ebca6bU);
        SetTextColor(memory, textColors[nextRandom(colorState) % textColors.size()]);
        const std::wstring line = randomTextLine(lineNumber);
        require(TextOutW(memory, 12, y + 2, line.c_str(), static_cast<int>(line.size())) != FALSE,
                "could not render scrolling source text");

        HPEN separator = CreatePen(PS_SOLID, 1, RGB(222, 226, 234));
        require(separator != nullptr, "could not create the scrolling source separator");
        HGDIOBJ previousPen = SelectObject(memory, separator);
        MoveToEx(memory, 8, y + kTextLineHeight - 1, nullptr);
        LineTo(memory, width - 8, y + kTextLineHeight - 1);
        SelectObject(memory, previousPen);
        DeleteObject(separator);
    }

    SelectObject(memory, previousFont);
    DeleteObject(font);
    POINT destination{kSelectionLeft, kSelectionTop};
    SIZE size{width, height};
    POINT source{};
    const BOOL presented = UpdateLayeredWindow(window, screen, &destination, &size, memory, &source,
                                               0, nullptr, ULW_OPAQUE);
    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    require(presented != FALSE, "could not present the scrolling source surface");
}

LRESULT CALLBACK scrollingSourceWindowProc(HWND window, UINT message, WPARAM wParam,
                                           LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

class ScrollingSourceWindow final {
  public:
    ScrollingSourceWindow() {
        m_instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = m_instance;
        windowClass.lpfnWndProc = scrollingSourceWindowProc;
        windowClass.lpszClassName = m_className;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        require(RegisterClassExW(&windowClass) != 0,
                "could not register the scrolling source window class");
        m_window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, m_className, L"",
            WS_POPUP, kSelectionLeft, kSelectionTop, kSelectionRight - kSelectionLeft,
            kSelectionBottom - kSelectionTop, nullptr, nullptr, m_instance, nullptr);
        require(m_window != nullptr, "could not create the scrolling source window");
        ShowWindow(m_window, SW_SHOWNOACTIVATE);
        render();
    }

    ~ScrollingSourceWindow() {
        if (m_window != nullptr) {
            DestroyWindow(m_window);
        }
        if (m_instance != nullptr) {
            UnregisterClassW(m_className, m_instance);
        }
    }

    void scrollDown() {
        m_state.offset += kScrollDistance;
        render();
    }

  private:
    void render() {
        presentScrollingSource(m_window, m_state);
    }

    static constexpr wchar_t m_className[] = L"SnowShotScrollingThumbnailE2ESource";
    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    ScrollingSourceState m_state;
};

struct TestConfiguration {
    std::wstring executablePath;
    bool captureExcluded = false;
};

[[nodiscard]] TestConfiguration configurationFromArguments(int argc, char* argv[]) {
    require(argc == 2 || argc == 3,
            "expected the snow_shot executable path and optional --capture-excluded argument");
    const std::string scenarioArgument = argc == 3 ? std::string(argv[2]) : std::string();
    const bool captureExcluded = scenarioArgument == kCaptureExcludedArgument;
    require(argc == 2 || captureExcluded, "unknown scrolling thumbnail E2E test argument");

    const int requiredLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, nullptr, 0);
    require(requiredLength > 0, "could not convert the executable path to UTF-16");

    std::wstring path(static_cast<std::size_t>(requiredLength), L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, path.data(),
                                requiredLength) == requiredLength,
            "could not convert the executable path to UTF-16");
    path.pop_back();
    return {std::move(path), captureExcluded};
}

[[nodiscard]] ComPtr<IUIAutomationElement> findProcessWindow(IUIAutomation& automation,
                                                             DWORD processId) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation.GetRootElement(root.put()))) {
        return {};
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
        return {};
    }

    ComPtr<IUIAutomationElementArray> candidates;
    if (FAILED(
            root.get()->FindAll(TreeScope_Descendants, processCondition.get(), candidates.put()))) {
        return {};
    }

    int length = 0;
    if (FAILED(candidates.get()->get_Length(&length))) {
        return {};
    }

    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> candidate;
        if (FAILED(candidates.get()->GetElement(index, candidate.put()))) {
            continue;
        }

        CONTROLTYPEID controlType = 0;
        BSTR name = nullptr;
        const HRESULT typeResult = candidate.get()->get_CurrentControlType(&controlType);
        const HRESULT nameResult = candidate.get()->get_CurrentName(&name);
        const bool isMainWindow = SUCCEEDED(typeResult) && SUCCEEDED(nameResult) &&
                                  controlType == UIA_WindowControlTypeId && name != nullptr &&
                                  wcscmp(name, kMainWindowName) == 0;
        SysFreeString(name);
        if (isMainWindow) {
            return candidate;
        }
    }

    return {};
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

[[nodiscard]] bool invoke(IUIAutomationElement& element) {
    ComPtr<IUIAutomationInvokePattern> invokePattern;
    if (SUCCEEDED(
            element.GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invokePattern.put())))) {
        return SUCCEEDED(invokePattern.get()->Invoke());
    }

    ComPtr<IUIAutomationLegacyIAccessiblePattern> legacyPattern;
    if (SUCCEEDED(element.GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                              IID_PPV_ARGS(legacyPattern.put())))) {
        return SUCCEEDED(legacyPattern.get()->DoDefaultAction());
    }

    return false;
}

template <typename Finder>
[[nodiscard]] ComPtr<IUIAutomationElement> waitForElement(Finder&& finder) {
    constexpr auto timeout = 5s;
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

void leftClick() {
    INPUT input[2]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    require(SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT)) ==
                std::size(input),
            "could not send a left click");
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

void dragThumbnailHeadHandle(LONG x, LONG startY, LONG endY) {
    moveCursorWithInput(x, startY);
    std::this_thread::sleep_for(100ms);

    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    require(SendInput(1, &down, sizeof(down)) == 1, "could not press the thumbnail crop handle");

    std::this_thread::sleep_for(50ms);
    constexpr int movementSteps = 6;
    for (int step = 1; step <= movementSteps; ++step) {
        const LONG y = startY + ((endY - startY) * static_cast<LONG>(step)) / movementSteps;
        moveCursorWithInput(x, y);
        std::this_thread::sleep_for(25ms);
    }

    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    require(SendInput(1, &up, sizeof(up)) == 1, "could not release the thumbnail crop handle");
}

[[nodiscard]] std::vector<COLORREF> captureScreenRegion(const RECT& region) {
    const LONG width = region.right - region.left;
    const LONG height = region.bottom - region.top;
    require(width > 0 && height > 0, "screen capture region must not be empty");

    HDC screenDc = GetDC(nullptr);
    require(screenDc != nullptr, "could not obtain the screen device context");
    HDC memoryDc = CreateCompatibleDC(screenDc);
    require(memoryDc != nullptr, "could not create the memory device context");

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    require(bitmap != nullptr && pixels != nullptr, "could not allocate the screenshot bitmap");

    HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    const BOOL copied = BitBlt(memoryDc, 0, 0, width, height, screenDc, region.left, region.top,
                               SRCCOPY | CAPTUREBLT);

    std::vector<COLORREF> image(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    if (copied != FALSE) {
        const auto* const source = static_cast<const std::uint32_t*>(pixels);
        for (std::size_t index = 0; index < image.size(); ++index) {
            image[index] = static_cast<COLORREF>(source[index]);
        }
    }

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    require(copied != FALSE, "could not capture the requested screen region");
    return image;
}

void verifyScrollingSourceOffsets() {
    constexpr LONG sourceWidth = kSelectionRight - kSelectionLeft;
    constexpr LONG sourceHeight = kSelectionBottom - kSelectionTop;
    const RECT sourceBounds{kSelectionLeft, kSelectionTop, kSelectionRight, kSelectionBottom};
    ScrollingSourceWindow source;

    SnowStitchFramePool* const pool = snow_stitch_frame_pool_create(sourceWidth, sourceHeight, 2);
    require(pool != nullptr, "could not create the scrolling offset preflight frame pool");

    SnowStitchConfig config{};
    require(snow_stitch_config_default(&config) != 0,
            "could not create the scrolling offset preflight configuration");
    SnowStitchSession* const session = snow_stitch_session_create(&config);
    require(session != nullptr, "could not create the scrolling offset preflight session");

    for (int frameIndex = 0; frameIndex <= kScrollCount; ++frameIndex) {
        if (frameIndex > 0) {
            source.scrollDown();
        }
        const std::vector<COLORREF> sourceImage = captureScreenRegion(sourceBounds);
        SnowStitchFrameBuffer* frame = snow_stitch_frame_pool_acquire(pool);
        require(frame != nullptr, "could not acquire a scrolling offset preflight frame");

        SnowStitchMutableImageInfo frameInfo{};
        require(snow_stitch_frame_buffer_info(frame, &frameInfo) != 0 &&
                    frameInfo.width == sourceWidth && frameInfo.height == sourceHeight &&
                    frameInfo.rgba_bytes != nullptr,
                "could not access a scrolling offset preflight frame");
        for (std::size_t pixelIndex = 0; pixelIndex < sourceImage.size(); ++pixelIndex) {
            const COLORREF pixel = sourceImage[pixelIndex];
            const std::size_t outputOffset = pixelIndex * 4;
            frameInfo.rgba_bytes[outputOffset] = GetBValue(pixel);
            frameInfo.rgba_bytes[outputOffset + 1] = GetGValue(pixel);
            frameInfo.rgba_bytes[outputOffset + 2] = GetRValue(pixel);
            frameInfo.rgba_bytes[outputOffset + 3] = 255;
        }

        SnowStitchFrameOutcome outcome{};
        require(snow_stitch_session_push_owned(session, &frame, &outcome) != 0 && frame == nullptr,
                "could not run the scrolling offset preflight stitcher");
        if (frameIndex == 0) {
            require(outcome.event == SNOW_STITCH_FRAME_EVENT_INITIAL,
                    "the scrolling offset preflight did not accept its initial frame");
        } else {
            require(outcome.event == SNOW_STITCH_FRAME_EVENT_EXTENDED_BOTTOM &&
                        outcome.added_rows == kScrollDistance,
                    "the scrolling stitcher did not recover a scripted 96-pixel offset");
        }
    }

    snow_stitch_session_destroy(session);
    snow_stitch_frame_pool_destroy(pool);
    std::cout << "verified " << kScrollCount << " scrolling stitch offsets of " << kScrollDistance
              << " pixels\n";
}

[[nodiscard]] std::size_t differentPixelCount(const std::vector<COLORREF>& first,
                                              const std::vector<COLORREF>& second) {
    require(first.size() == second.size(), "screen captures must have matching dimensions");
    std::size_t different = 0;
    for (std::size_t index = 0; index < first.size(); ++index) {
        if ((first[index] & 0x00ffffffU) != (second[index] & 0x00ffffffU)) {
            ++different;
        }
    }
    return different;
}

[[nodiscard]] std::size_t differentPixelCount(const std::vector<COLORREF>& first, LONG firstWidth,
                                              LONG firstHeight, const std::vector<COLORREF>& second,
                                              LONG secondWidth, LONG secondHeight) {
    require(first.size() ==
                    static_cast<std::size_t>(firstWidth) * static_cast<std::size_t>(firstHeight) &&
                second.size() ==
                    static_cast<std::size_t>(secondWidth) * static_cast<std::size_t>(secondHeight),
            "thumbnail captures must match their declared dimensions");

    const LONG commonWidth = std::min(firstWidth, secondWidth);
    const LONG commonHeight = std::min(firstHeight, secondHeight);
    std::size_t different = 0;
    for (LONG y = 0; y < commonHeight; ++y) {
        for (LONG x = 0; x < commonWidth; ++x) {
            const COLORREF firstPixel =
                first[static_cast<std::size_t>(y) * static_cast<std::size_t>(firstWidth) +
                      static_cast<std::size_t>(x)];
            const COLORREF secondPixel =
                second[static_cast<std::size_t>(y) * static_cast<std::size_t>(secondWidth) +
                       static_cast<std::size_t>(x)];
            if ((firstPixel & 0x00ffffffU) != (secondPixel & 0x00ffffffU)) {
                ++different;
            }
        }
    }
    for (LONG y = 0; y < firstHeight; ++y) {
        for (LONG x = 0; x < firstWidth; ++x) {
            if (x >= commonWidth || y >= commonHeight) {
                const COLORREF pixel =
                    first[static_cast<std::size_t>(y) * static_cast<std::size_t>(firstWidth) +
                          static_cast<std::size_t>(x)];
                if ((pixel & 0x00ffffffU) != 0) {
                    ++different;
                }
            }
        }
    }
    for (LONG y = 0; y < secondHeight; ++y) {
        for (LONG x = 0; x < secondWidth; ++x) {
            if (x >= commonWidth || y >= commonHeight) {
                const COLORREF pixel =
                    second[static_cast<std::size_t>(y) * static_cast<std::size_t>(secondWidth) +
                           static_cast<std::size_t>(x)];
                if ((pixel & 0x00ffffffU) != 0) {
                    ++different;
                }
            }
        }
    }
    return different;
}

[[nodiscard]] bool isScrollingThumbnailTrimControlPixel(COLORREF pixel) {
    // captureScreenRegion stores the native 32-bit DIB pixels directly.  A
    // Windows DIB is BGRA in memory, whereas COLORREF accessors expect a
    // 0x00bbggrr value.  Read red and blue from the opposite COLORREF channels
    // to preserve the original screen colours.
    const int red = GetBValue(pixel);
    const int green = GetGValue(pixel);
    const int blue = GetRValue(pixel);
    return red >= 220 && green >= 130 && green <= 210 && blue <= 60;
}

[[nodiscard]] bool hasScrollingThumbnailTrimControl(const std::vector<COLORREF>& image) {
    // The thumbnail's head/tail crop controls are painted with #faad14.  The
    // threshold leaves room for desktop-composition color conversion while
    // requiring a substantial part of the 2 px horizontal trim line.
    std::size_t cropHandlePixels = 0;
    for (const COLORREF pixel : image) {
        if (isScrollingThumbnailTrimControlPixel(pixel)) {
            ++cropHandlePixels;
        }
    }
    return cropHandlePixels >= 80;
}

[[nodiscard]] std::optional<LONG>
scrollingThumbnailTrimControlRow(const std::vector<COLORREF>& image, LONG width, LONG height) {
    require(image.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
            "the thumbnail analysis dimensions do not match its captured pixels");

    LONG strongestRow = -1;
    std::size_t strongestRowPixels = 0;
    for (LONG y = 0; y < height; ++y) {
        std::size_t cropHandlePixels = 0;
        for (LONG x = 0; x < width; ++x) {
            const COLORREF pixel =
                image[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)];
            if (isScrollingThumbnailTrimControlPixel(pixel)) {
                ++cropHandlePixels;
            }
        }
        if (cropHandlePixels > strongestRowPixels) {
            strongestRow = y;
            strongestRowPixels = cropHandlePixels;
        }
    }

    return strongestRowPixels >= 80 ? std::optional<LONG>(strongestRow) : std::nullopt;
}

void saveCaptureAsBmp(const char* fileName, const std::vector<COLORREF>& image, LONG width,
                      LONG height) {
    require(image.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
            "the captured image dimensions do not match the BMP dimensions");

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(image.size() * sizeof(COLORREF));

    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = width;
    infoHeader.biHeight = -height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = static_cast<DWORD>(image.size() * sizeof(COLORREF));

    std::ofstream output(fileName, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "could not create the screenshot artifact");
    output.write(reinterpret_cast<const char*>(&fileHeader),
                 static_cast<std::streamsize>(sizeof(fileHeader)));
    output.write(reinterpret_cast<const char*>(&infoHeader),
                 static_cast<std::streamsize>(sizeof(infoHeader)));
    output.write(reinterpret_cast<const char*>(image.data()),
                 static_cast<std::streamsize>(image.size() * sizeof(COLORREF)));
    require(static_cast<bool>(output), "could not write the screenshot artifact");
}

[[nodiscard]] std::string scrollingArtifactFile(const char* surface, int scrollIndex) {
    return "e2e-artifacts/scrolling-" + std::string(surface) + "-scroll-" +
           (scrollIndex < 10 ? "0" : "") + std::to_string(scrollIndex) + ".bmp";
}

[[nodiscard]] bool thumbnailTrimControlVisible(const char* artifactFileName,
                                               const RECT& thumbnailBounds) {
    const LONG cropTop = std::max<LONG>(0, thumbnailBounds.top - 3);
    const RECT thumbnailCrop{
        thumbnailBounds.left,
        cropTop,
        thumbnailBounds.right,
        std::min(thumbnailBounds.bottom, thumbnailBounds.top + 25),
    };
    const std::vector<COLORREF> image = captureScreenRegion(thumbnailCrop);
    saveCaptureAsBmp(artifactFileName, image, thumbnailCrop.right - thumbnailCrop.left,
                     thumbnailCrop.bottom - thumbnailCrop.top);
    return hasScrollingThumbnailTrimControl(image);
}

[[nodiscard]] std::optional<LONG> thumbnailTrimControlRow(const char* artifactFileName,
                                                          const RECT& thumbnailBounds) {
    const LONG cropTop = std::max<LONG>(0, thumbnailBounds.top - 3);
    const RECT headCrop{
        thumbnailBounds.left,
        cropTop,
        thumbnailBounds.right,
        thumbnailBounds.top + (thumbnailBounds.bottom - thumbnailBounds.top) / 2,
    };
    const std::vector<COLORREF> image = captureScreenRegion(headCrop);
    const LONG width = headCrop.right - headCrop.left;
    const LONG height = headCrop.bottom - headCrop.top;
    saveCaptureAsBmp(artifactFileName, image, width, height);
    const std::optional<LONG> localRow = scrollingThumbnailTrimControlRow(image, width, height);
    return localRow.has_value() ? std::optional<LONG>(*localRow + headCrop.top) : std::nullopt;
}

void requireDesktopCanRunScenario() {
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    require(screenWidth >= kSelectionRight && screenHeight >= kSelectionBottom,
            "the primary display must contain the 64,64-564,564 selection region");
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        // Keep UIA, cursor positions, and BitBlt coordinates in physical pixels.
        SetProcessDPIAware();
        requireDesktopCanRunScenario();
        const BOOL artifactsDirectoryCreated = CreateDirectoryW(kArtifactsDirectory, nullptr);
        require(artifactsDirectoryCreated != FALSE || GetLastError() == ERROR_ALREADY_EXISTS,
                "could not create the screenshot artifact directory");
        const TestConfiguration configuration = configurationFromArguments(argc, argv);
        verifyScrollingSourceOffsets();
        ScrollingSourceWindow scrollingSource;

        const ScopedCom com;
        require(SUCCEEDED(com.result()), "could not initialize COM for UI Automation");

        ComPtr<IUIAutomation> automation;
        require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(automation.put()))),
                "could not create the UI Automation client");

        ScopedProcess application;
        require(application.start(configuration.executablePath, configuration.captureExcluded),
                "could not start snow_shot");

        std::this_thread::sleep_for(2s);

        ComPtr<IUIAutomationElement> mainWindow = waitForElement([&automation, &application]() {
            return findProcessWindow(*automation.get(), application.processId());
        });
        require(mainWindow.get() != nullptr, "could not find the SnowShot main window through UIA");

        ComPtr<IUIAutomationElement> screenshotControl = findProcessDescendantByAutomationIdSuffix(
            *automation.get(), application.processId(), kScreenshotControlAutomationIdSuffix);
        if (screenshotControl.get() == nullptr) {
            reportProcessUiAutomationElements(*automation.get(), application.processId());
        }
        require(screenshotControl.get() != nullptr,
                "could not find the Screenshot control through UIA");
        require(invoke(*screenshotControl.get()), "could not start screenshot capture through UIA");

        // The capture overlay owns the selection input surface and needs to be
        // fully shown before the scripted rectangle drag begins.
        std::this_thread::sleep_for(2s);

        dragSelect500By500();

        std::this_thread::sleep_for(1s);

        ComPtr<IUIAutomationElement> scrollingControl =
            waitForElement([&automation, &application]() {
                return findProcessDescendantByAutomationIdSuffix(
                    *automation.get(), application.processId(),
                    kScrollingScreenshotControlAutomationIdSuffix);
            });
        if (scrollingControl.get() == nullptr) {
            reportProcessUiAutomationElements(*automation.get(), application.processId());
        }
        require(scrollingControl.get() != nullptr,
                "could not find the Scrolling screenshot control through UIA");
        require(invoke(*scrollingControl.get()),
                "could not enter scrolling screenshot through UIA");

        ComPtr<IUIAutomationElement> thumbnail = waitForElement([&automation, &application]() {
            ComPtr<IUIAutomationElement> element = findProcessDescendantByAutomationIdSuffix(
                *automation.get(), application.processId(), kScrollingThumbnailAutomationIdSuffix);
            if (element.get() == nullptr) {
                return element;
            }
            BOOL offscreen = TRUE;
            RECT bounds{};
            if (FAILED(element.get()->get_CurrentIsOffscreen(&offscreen)) || offscreen != FALSE ||
                FAILED(element.get()->get_CurrentBoundingRectangle(&bounds)) ||
                bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
                return ComPtr<IUIAutomationElement>{};
            }
            return element;
        });
        require(thumbnail.get() != nullptr,
                "could not find the visible scrolling thumbnail through UIA");
        RECT thumbnailBounds{};
        require(SUCCEEDED(thumbnail.get()->get_CurrentBoundingRectangle(&thumbnailBounds)) &&
                    thumbnailBounds.right > thumbnailBounds.left &&
                    thumbnailBounds.bottom > thumbnailBounds.top,
                "could not determine the scrolling thumbnail bounds through UIA");

        moveCursor((kSelectionLeft + kSelectionRight) / 2, (kSelectionTop + kSelectionBottom) / 2);
        std::this_thread::sleep_for(250ms);
        const LONG thumbnailWidth = thumbnailBounds.right - thumbnailBounds.left;
        const LONG thumbnailHeight = thumbnailBounds.bottom - thumbnailBounds.top;
        LONG previousThumbnailWidth = thumbnailWidth;
        std::vector<COLORREF> previousThumbnail;
        if (!configuration.captureExcluded) {
            previousThumbnail = captureScreenRegion(thumbnailBounds);
            const std::string initialThumbnailFile = scrollingArtifactFile("thumbnail", 0);
            saveCaptureAsBmp(initialThumbnailFile.c_str(), previousThumbnail, thumbnailWidth,
                             thumbnailHeight);
        }
        constexpr LONG sourceProbeExtent = 128;
        const LONG sourceProbeLeft = (kSelectionLeft + kSelectionRight - sourceProbeExtent) / 2;
        const LONG sourceProbeTop = (kSelectionTop + kSelectionBottom - sourceProbeExtent) / 2;
        const RECT sourceProbe{sourceProbeLeft, sourceProbeTop, sourceProbeLeft + sourceProbeExtent,
                               sourceProbeTop + sourceProbeExtent};
        std::vector<COLORREF> previousSource = captureScreenRegion(sourceProbe);
        const std::string initialSourceFile = scrollingArtifactFile("source", 0);
        saveCaptureAsBmp(initialSourceFile.c_str(), previousSource, sourceProbeExtent,
                         sourceProbeExtent);

        const int scrollCount = configuration.captureExcluded ? 1 : kScrollCount;
        std::vector<int> staleThumbnailScrolls;
        for (int scrollIndex = 1; scrollIndex <= scrollCount; ++scrollIndex) {
            const RECT previousThumbnailBounds = thumbnailBounds;
            const LONG previousThumbnailHeight =
                previousThumbnailBounds.bottom - previousThumbnailBounds.top;

            scrollingSource.scrollDown();
            std::this_thread::sleep_for(2s);

            RECT updatedThumbnailBounds = previousThumbnailBounds;
            std::vector<COLORREF> updatedSource;
            std::vector<COLORREF> updatedThumbnailComparison;
            LONG updatedThumbnailWidth =
                previousThumbnailBounds.right - previousThumbnailBounds.left;
            LONG updatedThumbnailHeight = previousThumbnailHeight;
            std::size_t changedSourcePixels = 0;
            std::size_t changedThumbnailPixels = 0;
            std::size_t comparedThumbnailPixels = 0;
            bool sourceAdvanced = false;
            bool pixelsPresented = configuration.captureExcluded;
            constexpr auto presentationPollInterval = 100ms;
            constexpr auto presentationPollTimeout = 5s;
            const auto presentationDeadline =
                std::chrono::steady_clock::now() + presentationPollTimeout;
            do {
                updatedSource = captureScreenRegion(sourceProbe);
                changedSourcePixels = differentPixelCount(previousSource, updatedSource);
                sourceAdvanced = changedSourcePixels >= previousSource.size() / 20;

                RECT candidateBounds{};
                if (SUCCEEDED(thumbnail.get()->get_CurrentBoundingRectangle(&candidateBounds)) &&
                    candidateBounds.right > candidateBounds.left &&
                    candidateBounds.bottom > candidateBounds.top) {
                    updatedThumbnailBounds = candidateBounds;
                }
                if (!configuration.captureExcluded) {
                    updatedThumbnailWidth =
                        updatedThumbnailBounds.right - updatedThumbnailBounds.left;
                    updatedThumbnailHeight =
                        updatedThumbnailBounds.bottom - updatedThumbnailBounds.top;
                    updatedThumbnailComparison = captureScreenRegion(updatedThumbnailBounds);
                    changedThumbnailPixels = differentPixelCount(
                        previousThumbnail, previousThumbnailWidth, previousThumbnailHeight,
                        updatedThumbnailComparison, updatedThumbnailWidth, updatedThumbnailHeight);
                    comparedThumbnailPixels =
                        std::max(previousThumbnail.size(), updatedThumbnailComparison.size());
                    pixelsPresented = changedThumbnailPixels >= comparedThumbnailPixels / 100;
                }
                if (sourceAdvanced) {
                    break;
                }
                std::this_thread::sleep_for(presentationPollInterval);
            } while (std::chrono::steady_clock::now() < presentationDeadline);

            require(sourceAdvanced,
                    "the colored-text source did not repaint after a scripted scroll");
            if (!configuration.captureExcluded) {
                if (!pixelsPresented) {
                    staleThumbnailScrolls.push_back(scrollIndex);
                }
            }

            const std::string sourceFile = scrollingArtifactFile("source", scrollIndex);
            saveCaptureAsBmp(sourceFile.c_str(), updatedSource, sourceProbeExtent,
                             sourceProbeExtent);
            if (!configuration.captureExcluded) {
                const std::string thumbnailFile = scrollingArtifactFile("thumbnail", scrollIndex);
                saveCaptureAsBmp(thumbnailFile.c_str(), updatedThumbnailComparison,
                                 updatedThumbnailWidth, updatedThumbnailHeight);
                previousThumbnail = updatedThumbnailComparison;
                previousThumbnailWidth = updatedThumbnailWidth;
                std::cout << "scroll " << scrollIndex << ": source pixels changed "
                          << changedSourcePixels << '/' << previousSource.size()
                          << ", thumbnail pixels changed " << changedThumbnailPixels << '/'
                          << comparedThumbnailPixels << ", thumbnail height "
                          << previousThumbnailHeight << " -> " << updatedThumbnailHeight << '\n';
            }

            previousSource = std::move(updatedSource);
            thumbnailBounds = updatedThumbnailBounds;
            std::this_thread::sleep_for(2s);
        }

        if (!staleThumbnailScrolls.empty()) {
            std::string failedScrolls;
            for (const int scrollIndex : staleThumbnailScrolls) {
                if (!failedScrolls.empty()) {
                    failedScrolls += ", ";
                }
                failedScrolls += std::to_string(scrollIndex);
            }
            throw std::runtime_error("the scrolling thumbnail did not present newly stitched "
                                     "content without mouse input after scroll(s): " +
                                     failedScrolls);
        }

        if (configuration.captureExcluded) {
            return 0;
        }

        const bool thumbnailVisibleBeforeClicks =
            thumbnailTrimControlVisible(kThumbnailBeforeClicksFile, thumbnailBounds);
        std::cout << "thumbnail crop control before clicks: "
                  << (thumbnailVisibleBeforeClicks ? "detected" : "not detected") << '\n';

        moveCursor(250, 250);
        leftClick();
        moveCursor((thumbnailBounds.left + thumbnailBounds.right) / 2,
                   (thumbnailBounds.top + thumbnailBounds.bottom) / 2);
        leftClick();

        std::this_thread::sleep_for(1s);
        const bool thumbnailVisibleAfterClicks =
            thumbnailTrimControlVisible(kThumbnailAfterClicksFile, thumbnailBounds);
        std::cout << "thumbnail crop control after clicks: "
                  << (thumbnailVisibleAfterClicks ? "detected" : "not detected") << '\n';

        const std::optional<LONG> thumbnailRowBeforeDrag =
            thumbnailTrimControlRow(kThumbnailBeforeDragFile, thumbnailBounds);
        if (thumbnailRowBeforeDrag.has_value()) {
            const LONG dragStartY = std::clamp(*thumbnailRowBeforeDrag, thumbnailBounds.top + 2,
                                               thumbnailBounds.bottom - 3);
            const LONG targetY = std::min(thumbnailBounds.bottom - 1, thumbnailBounds.top + 100);
            dragThumbnailHeadHandle((thumbnailBounds.left + thumbnailBounds.right) / 2, dragStartY,
                                    targetY);
        }
        std::this_thread::sleep_for(1s);
        const std::optional<LONG> thumbnailRowAfterDrag =
            thumbnailTrimControlRow(kThumbnailAfterDragFile, thumbnailBounds);
        const bool thumbnailHandleMoved = thumbnailRowBeforeDrag.has_value() &&
                                          thumbnailRowAfterDrag.has_value() &&
                                          *thumbnailRowAfterDrag >= *thumbnailRowBeforeDrag + 50;
        std::cout << "thumbnail crop control row before drag: "
                  << (thumbnailRowBeforeDrag.has_value() ? std::to_string(*thumbnailRowBeforeDrag)
                                                         : "not detected")
                  << '\n';
        std::cout << "thumbnail crop control row after drag: "
                  << (thumbnailRowAfterDrag.has_value() ? std::to_string(*thumbnailRowAfterDrag)
                                                        : "not detected")
                  << '\n';
        std::this_thread::sleep_for(1s);
        require(thumbnailVisibleBeforeClicks && thumbnailVisibleAfterClicks && thumbnailHandleMoved,
                "the scrolling screenshot thumbnail did not repaint automatically when shown or "
                "when its crop handle moved");

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
