#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <QBuffer>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QProcessEnvironment>
#include <QSysInfo>
#include <QVector>
#include <QStringList>

#include <Windows.h>
#include <UIAutomation.h>
#include <dwmapi.h>
#include <objbase.h>
#include <psapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cwchar>

namespace {
using namespace std::chrono_literals;

constexpr int kScrollingSourceExtent = 500;
constexpr int kScrollingTextLineHeight = 36;

template <typename T> class ComPtr final {
  public:
    ComPtr() = default;
    ComPtr(ComPtr&& other) noexcept : m_value(other.m_value) { other.m_value = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept { if (this != &other) { reset(other.m_value); other.m_value = nullptr; } return *this; }
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const { return m_value; }
    T** put() { reset(); return &m_value; }
    void reset(T* value = nullptr) { if (m_value) m_value->Release(); m_value = value; }
  private:
    T* m_value = nullptr;
};

class ScopedCom final {
  public:
    ScopedCom() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() { if (SUCCEEDED(m_result)) CoUninitialize(); }
    HRESULT result() const { return m_result; }
  private: HRESULT m_result;
};

class ChildProcess final {
  public:
    ~ChildProcess() { stop(); }
    bool start(const QString& executable) {
        std::wstring path = executable.toStdWString();
        std::wstring command = L"\"" + path + L"\" --show-main-window --e2e-allow-overlay-capture --e2e-instance-id=" + std::to_wstring(GetCurrentProcessId());
        std::vector<wchar_t> commandLine(command.begin(), command.end()); commandLine.push_back(L'\0');
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        if (!CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return false;
        CloseHandle(process.hThread); m_process = process.hProcess; m_pid = process.dwProcessId; return true;
    }
    DWORD pid() const { return m_pid; }
    bool alive() const { return m_process && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT; }
    qint64 workingSetBytes(bool peak) const {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!m_process || !GetProcessMemoryInfo(m_process,
                                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                                sizeof(counters))) {
            return 0;
        }
        return static_cast<qint64>(peak ? counters.PeakWorkingSetSize
                                       : counters.WorkingSetSize);
    }
    void stop() { if (!m_process) return; if (alive()) { TerminateProcess(m_process, 1); WaitForSingleObject(m_process, 5000); } CloseHandle(m_process); m_process = nullptr; }
  private: HANDLE m_process = nullptr; DWORD m_pid = 0;
};

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

ComPtr<IUIAutomation> createAutomation() {
    ComPtr<IUIAutomation> automation;
    require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(automation.put()))), "UI Automation initialization failed");
    return automation;
}

bool automationIdMatches(const wchar_t* automationId, const wchar_t* needle) {
    if (automationId == nullptr) return false;
    if (wcscmp(automationId, needle) == 0) return true;
    const std::wstring candidate(automationId);
    const std::wstring suffix = std::wstring(L".") + needle;
    return candidate.size() >= suffix.size() &&
           candidate.compare(candidate.size() - suffix.size(), suffix.size(), suffix) == 0;
}

ComPtr<IUIAutomationElement> findByAutomationId(IUIAutomation& automation, DWORD pid,
                                                const wchar_t* needle) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation.GetRootElement(root.put()))) return {};
    VARIANT value{}; value.vt = VT_I4; value.lVal = static_cast<LONG>(pid);
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation.CreatePropertyCondition(UIA_ProcessIdPropertyId, value, condition.put()))) return {};
    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(root.get()->FindAll(TreeScope_Descendants, condition.get(), elements.put()))) return {};
    int length = 0; elements.get()->get_Length(&length);
    for (int i = 0; i < length; ++i) {
        ComPtr<IUIAutomationElement> element; if (FAILED(elements.get()->GetElement(i, element.put()))) continue;
        BSTR id = nullptr; if (FAILED(element.get()->get_CurrentAutomationId(&id))) continue;
        const bool match = automationIdMatches(id, needle); SysFreeString(id);
        if (match) return element;
    }
    return {};
}

ComPtr<IUIAutomationElement> findVisibleByAutomationId(IUIAutomation& automation, DWORD pid,
                                                       const wchar_t* needle) {
    ComPtr<IUIAutomationElement> element = findByAutomationId(automation, pid, needle);
    if (element.get() == nullptr) return {};
    BOOL offscreen = TRUE; RECT bounds{};
    if (FAILED(element.get()->get_CurrentIsOffscreen(&offscreen)) || offscreen != FALSE ||
        FAILED(element.get()->get_CurrentBoundingRectangle(&bounds)) ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return {};
    }
    return element;
}

template <typename Finder> ComPtr<IUIAutomationElement> waitFor(Finder finder, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto result = finder(); if (result.get() != nullptr) return result;
        std::this_thread::sleep_for(25ms);
    }
    return {};
}

bool invoke(IUIAutomationElement& element) {
    ComPtr<IUIAutomationInvokePattern> pattern;
    if (SUCCEEDED(element.GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(pattern.put()))))
        return SUCCEEDED(pattern.get()->Invoke());
    return false;
}

RECT bounds(IUIAutomationElement& element) {
    RECT result{}; element.get_CurrentBoundingRectangle(&result); return result;
}

LONG absoluteCoordinate(LONG value, LONG origin, LONG extent) {
    return extent <= 1 ? 0 : static_cast<LONG>((static_cast<double>(value - origin) * 65535.0) / (extent - 1));
}

void sendMouse(int x, int y, DWORD flags) {
    const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT input{}; input.type = INPUT_MOUSE; input.mi.dx = absoluteCoordinate(x, left, width); input.mi.dy = absoluteCoordinate(y, top, height);
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    require(SendInput(1, &input, sizeof(input)) == 1, "SendInput failed");
}

void clickCenter(IUIAutomationElement& element) {
    const RECT rect = bounds(element);
    const int x = (rect.left + rect.right) / 2, y = (rect.top + rect.bottom) / 2;
    sendMouse(x, y, MOUSEEVENTF_MOVE);
    sendMouse(x, y, MOUSEEVENTF_LEFTDOWN);
    sendMouse(x, y, MOUSEEVENTF_LEFTUP);
}

void dragRect(const RECT& rect) {
    sendMouse(rect.left, rect.top, MOUSEEVENTF_MOVE);
    sendMouse(rect.left, rect.top, MOUSEEVENTF_LEFTDOWN);
    std::this_thread::sleep_for(20ms);
    sendMouse(rect.right, rect.bottom, MOUSEEVENTF_MOVE);
    sendMouse(rect.right, rect.bottom, MOUSEEVENTF_LEFTUP);
}

void dragSelection(const RECT& monitor, int fraction) {
    const int width = std::max(64, static_cast<int>((monitor.right - monitor.left) * fraction / 100));
    const int height = std::max(64, static_cast<int>((monitor.bottom - monitor.top) * fraction / 100));
    const RECT rect{monitor.left + (monitor.right - monitor.left - width) / 2,
                    monitor.top + (monitor.bottom - monitor.top - height) / 2,
                    monitor.left + (monitor.right - monitor.left + width) / 2,
                    monitor.top + (monitor.bottom - monitor.top + height) / 2};
    dragRect(rect);
}

QVector<RECT> monitors() {
    QVector<RECT> result;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO info{}; info.cbSize = sizeof(info); GetMonitorInfoW(monitor, &info);
        static_cast<QVector<RECT>*>(reinterpret_cast<void*>(data))->push_back(info.rcWork); return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

int traceLineCount(const QString& path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return 0;
    return file.readAll().count('\n');
}

QJsonObject readTraceLine(const QString& path, int expectedLine) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "could not open app trace");
    const QList<QByteArray> lines = file.readAll().split('\n');
    require(expectedLine > 0 && expectedLine <= lines.size(), "trace line was unavailable");
    QJsonParseError error{}; const QJsonDocument document = QJsonDocument::fromJson(lines.at(expectedLine - 1), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "trace JSON was invalid");
    return document.object();
}

class TraceCollector final {
  public:
    explicit TraceCollector(QString path) : m_path(std::move(path)) {}
    QJsonObject next(const QString& expectedScenario, int timeoutMs, const ChildProcess& child) {
        const int targetLine = m_line + 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (traceLineCount(m_path) < targetLine && child.alive() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(25ms);
        }
        require(traceLineCount(m_path) >= targetLine, "pin sample timed out");
        m_line = targetLine;
        QJsonObject record = readTraceLine(m_path, m_line);
        require(record.value(QStringLiteral("scenario")).toString() == expectedScenario,
                "trace scenario did not match the driven pin action");
        return record;
    }

  private:
    QString m_path;
    int m_line = 0;
};

QJsonObject statistics(const QVector<double>& source,
                       const QString& unit = QStringLiteral("ms")) {
    if (source.isEmpty()) return {};
    QVector<double> values = source; std::sort(values.begin(), values.end());
    auto at = [&values](double p) { return values[std::min(values.size() - 1, static_cast<qsizetype>(std::ceil(p * values.size()) - 1))]; };
    const double mean = std::accumulate(values.cbegin(), values.cend(), 0.0) / values.size();
    double variance = 0.0; for (double value : values) variance += (value - mean) * (value - mean);
    const auto key = [&unit](const char* name) {
        return QString::fromLatin1(name) + QLatin1Char('_') + unit;
    };
    return {{QStringLiteral("count"), values.size()}, {key("min"), values.first()},
            {key("mean"), mean}, {key("p50"), at(.50)}, {key("p90"), at(.90)},
            {key("p95"), at(.95)}, {key("p99"), at(.99)}, {key("max"), values.last()},
            {key("stddev"), std::sqrt(variance / values.size())}};
}

QString htmlEscape(QString value) { return value.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;"); }

// --- Scrolling source window ------------------------------------------------
// A layered top-level window that renders a deterministic, dense document and
// can scroll it under program control. The scrolling stitch engine recovers
// the scripted pixel offsets, which turns the live capture stream into a long
// screenshot without any real application window.
struct ScrollingSourceState {
    int offset = 0;
};

std::uint32_t nextRandom(std::uint32_t& state) {
    state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
    return state;
}

std::uint32_t scrollingSourcePixel(int x, int documentY) {
    const std::uint32_t cellX = static_cast<std::uint32_t>(x / 16);
    const std::uint32_t cellY = static_cast<std::uint32_t>(documentY / 16);
    std::uint32_t hash = cellX * 0x9e3779b9U ^ cellY * 0x85ebca6bU;
    hash ^= hash >> 16U;
    int red = 30 + static_cast<int>(hash & 0xbfU);
    int green = 30 + static_cast<int>((hash >> 8U) & 0xbfU);
    int blue = 30 + static_cast<int>((hash >> 16U) & 0xbfU);
    if (x % 37 < 2 || documentY % 43 < 2) { red = 245; green = 245; blue = 245; }
    else if ((x + documentY) % 61 < 3) { red = 10; green = 10; blue = 10; }
    return static_cast<std::uint32_t>(blue | (green << 8) | (red << 16));
}

std::wstring scrollingSourceTextLine(int lineNumber) {
    constexpr wchar_t alphabet[] = L"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    constexpr std::size_t alphabetLength = std::size(alphabet) - 1;
    std::uint32_t randomState = 0x6d2b79f5U ^ (static_cast<std::uint32_t>(lineNumber) * 0x9e3779b9U);
    std::wstring characters = L"L" + std::to_wstring(lineNumber);
    while (characters.size() < 8) characters.push_back(alphabet[nextRandom(randomState) % alphabetLength]);
    std::wstring line;
    for (std::size_t index = 0; index < characters.size(); ++index) {
        line.push_back(characters[index]);
        if (index + 1 < characters.size()) line.append(1 + nextRandom(randomState) % 3, L' ');
    }
    return line;
}

class ScrollingSourceWindow final {
  public:
    ScrollingSourceWindow(LONG left, LONG top, int extent) : m_left(left), m_top(top), m_extent(extent) {
        m_instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = m_instance;
        windowClass.lpfnWndProc = scrollingSourceWindowProc;
        windowClass.lpszClassName = m_className;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        require(RegisterClassExW(&windowClass) != 0, "could not register the scrolling source class");
        m_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                   m_className, L"", WS_POPUP, left, top, extent, extent,
                                   nullptr, nullptr, m_instance, nullptr);
        require(m_window != nullptr, "could not create the scrolling source window");
        ShowWindow(m_window, SW_SHOWNOACTIVATE);
        render();
    }
    ~ScrollingSourceWindow() {
        if (m_window != nullptr) DestroyWindow(m_window);
        if (m_instance != nullptr) UnregisterClassW(m_className, m_instance);
    }
    ScrollingSourceWindow(const ScrollingSourceWindow&) = delete;
    ScrollingSourceWindow& operator=(const ScrollingSourceWindow&) = delete;

    void scrollDown(int distance) { m_state.offset += distance; render(); }
    void reset() { m_state.offset = 0; render(); }
    [[nodiscard]] RECT rect() const {
        return RECT{m_left, m_top, m_left + m_extent, m_top + m_extent};
    }

  private:
    static LRESULT CALLBACK scrollingSourceWindowProc(HWND window, UINT message, WPARAM wParam,
                                                      LPARAM lParam) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    void render() { presentScrollingSource(m_window, m_state); }
    void presentScrollingSource(HWND window, const ScrollingSourceState& state) {
        const int width = m_extent, height = m_extent;
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
        HBITMAP bitmap = CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &bitmapPixels, nullptr, 0);
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

        HFONT font = CreateFontW(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        require(font != nullptr, "could not create the scrolling source font");
        HGDIOBJ previousFont = SelectObject(memory, font);
        SetBkMode(memory, TRANSPARENT);

        constexpr std::array<COLORREF, 10> textColors{
            RGB(255, 82, 82),   RGB(64, 196, 255),  RGB(105, 240, 174), RGB(225, 135, 255),
            RGB(255, 183, 77),  RGB(72, 255, 245),  RGB(255, 241, 118), RGB(255, 128, 191),
            RGB(238, 238, 238), RGB(179, 157, 219),
        };
        const int firstLine = state.offset / kScrollingTextLineHeight;
        const int firstLineY = firstLine * kScrollingTextLineHeight - state.offset;
        for (int lineNumber = firstLine, y = firstLineY; y < height;
             ++lineNumber, y += kScrollingTextLineHeight) {
            std::uint32_t colorState =
                0xa511e9b3U ^ (static_cast<std::uint32_t>(lineNumber) * 0x85ebca6bU);
            SetTextColor(memory, textColors[nextRandom(colorState) % textColors.size()]);
            const std::wstring line = scrollingSourceTextLine(lineNumber);
            require(TextOutW(memory, 12, y + 2, line.c_str(), static_cast<int>(line.size())) != FALSE,
                    "could not render scrolling source text");

            HPEN separator = CreatePen(PS_SOLID, 1, RGB(222, 226, 234));
            require(separator != nullptr, "could not create the scrolling source separator");
            HGDIOBJ previousPen = SelectObject(memory, separator);
            MoveToEx(memory, 8, y + kScrollingTextLineHeight - 1, nullptr);
            LineTo(memory, width - 8, y + kScrollingTextLineHeight - 1);
            SelectObject(memory, previousPen);
            DeleteObject(separator);
        }

        SelectObject(memory, previousFont);
        DeleteObject(font);
        POINT destination{m_left, m_top};
        SIZE size{width, height};
        POINT source{};
        const BOOL presented = UpdateLayeredWindow(window, screen, &destination, &size, memory,
                                                   &source, 0, nullptr, ULW_OPAQUE);
        SelectObject(memory, previousBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        require(presented != FALSE, "could not present the scrolling source surface");
    }

    static constexpr wchar_t m_className[] = L"SnowShotPinPerfScrollSource";
    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    LONG m_left = 0;
    LONG m_top = 0;
    int m_extent = 0;
    ScrollingSourceState m_state;
};

// --- Clipboard payloads ------------------------------------------------------
class ClipboardScope final {
  public:
    explicit ClipboardScope(int attempts = 20) {
        for (int attempt = 0; attempt < attempts && !m_open; ++attempt) {
            m_open = OpenClipboard(nullptr) != FALSE;
            if (!m_open) std::this_thread::sleep_for(50ms);
        }
    }
    ~ClipboardScope() { if (m_open) CloseClipboard(); }
    ClipboardScope(const ClipboardScope&) = delete;
    ClipboardScope& operator=(const ClipboardScope&) = delete;
    [[nodiscard]] bool open() const { return m_open; }
  private: bool m_open = false;
};

void setClipboardPayload(UINT format, const void* data, std::size_t size) {
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, size);
    require(handle != nullptr, "could not allocate the clipboard payload");
    void* target = GlobalLock(handle);
    if (target == nullptr) { GlobalFree(handle); require(false, "could not lock the clipboard payload"); }
    std::memcpy(target, data, size);
    GlobalUnlock(handle);
    if (SetClipboardData(format, handle) == nullptr) {
        GlobalFree(handle);
        require(false, "could not set the clipboard payload");
    }
}

UINT registeredClipboardFormat(const wchar_t* name) {
    const UINT format = RegisterClipboardFormatW(name);
    require(format != 0, "could not register the clipboard format");
    return format;
}

QImage makeClipboardImage(const QSize& size) {
    require(size.isValid() && !size.isEmpty() && size.width() <= 8192 && size.height() <= 8192,
            "invalid clipboard image size");
    QImage image(size, QImage::Format_ARGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const std::uint32_t texture = scrollingSourcePixel(x, y);
            row[x] = 0xff000000u | texture;
        }
    }
    return image;
}

QByteArray encodeClipboardPng(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly), "could not open the PNG encode buffer");
    require(image.save(&buffer, "PNG"), "could not encode the clipboard PNG payload");
    return bytes;
}

QByteArray encodeClipboardDibV5(const QImage& image) {
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    const int width = source.width(), height = source.height();
    const int rowBytes = width * 4;
    QByteArray buffer(static_cast<int>(sizeof(BITMAPV5HEADER)) + rowBytes * height, Qt::Uninitialized);
    auto* header = reinterpret_cast<BITMAPV5HEADER*>(buffer.data());
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = width;
    header->bV5Height = height;
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = static_cast<DWORD>(rowBytes * height);
    header->bV5RedMask = 0x00ff0000;
    header->bV5GreenMask = 0x0000ff00;
    header->bV5BlueMask = 0x000000ff;
    header->bV5AlphaMask = 0xff000000;
    header->bV5CSType = LCS_sRGB;
    auto* pixels = reinterpret_cast<std::uint32_t*>(buffer.data() + sizeof(BITMAPV5HEADER));
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(source.scanLine(y));
        auto* destination = pixels + static_cast<std::size_t>(height - 1 - y) *
                                           static_cast<std::size_t>(width);
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = row[x];
            destination[x] = (static_cast<std::uint32_t>(qAlpha(pixel)) << 24) |
                             (static_cast<std::uint32_t>(qRed(pixel)) << 16) |
                             (static_cast<std::uint32_t>(qGreen(pixel)) << 8) |
                             static_cast<std::uint32_t>(qBlue(pixel));
        }
    }
    return buffer;
}

QByteArray buildClipboardHtmlFragment() {
    QString fragment = QStringLiteral(
        "<h1 style=\"font-size:24px;color:#1f3b73;margin:10px 0\">Snow Shot clipboard HTML pin "
        "benchmark</h1>");
    for (int paragraph = 0; paragraph < 18; ++paragraph) {
        fragment += QStringLiteral(
                        "<p style=\"margin:6px 0\">Paragraph %1: the <b>quick</b> brown fox "
                        "<i>jumps</i> over the lazy dog <span style=\"color:#c0392b\">%2</span> "
                        "times while the rich text document is laid out and rendered.</p>")
                        .arg(paragraph + 1)
                        .arg(paragraph * 7 + 3);
    }
    fragment += QStringLiteral(
        "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" "
        "style=\"border-collapse:collapse;margin:8px 0\">");
    for (int row = 0; row < 8; ++row) {
        fragment += QStringLiteral("<tr>");
        for (int column = 0; column < 4; ++column) {
            fragment += QStringLiteral("<td style=\"padding:4px 10px\">row %1 col %2</td>")
                            .arg(row)
                            .arg(column);
        }
        fragment += QStringLiteral("</tr>");
    }
    fragment += QStringLiteral("</table><ul>");
    for (int item = 0; item < 10; ++item) {
        const int hue = (item * 37) % 360;
        fragment += QStringLiteral("<li>List item %1 with <span style=\"color:hsl(%2,70%%,40%%)\">"
                                   "colored</span> inline text</li>")
                        .arg(item)
                        .arg(hue);
    }
    fragment += QStringLiteral("</ul>");
    return fragment.toUtf8();
}

QByteArray wrapClipboardHtml(const QByteArray& fragment) {
    const QByteArray marker = QByteArrayLiteral("<!--StartFragment-->");
    const QByteArray endMarker = QByteArrayLiteral("<!--EndFragment-->");
    const QByteArray head = QByteArrayLiteral("<html><body>\r\n") + marker;
    const QByteArray tail = endMarker + QByteArrayLiteral("\r\n</body>\r\n</html>");
    const int numberWidth = 10;
    const QByteArray headerTemplate =
        QByteArrayLiteral("Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
                          "StartFragment:0000000000\r\nEndFragment:0000000000\r\n");
    const auto offsetField = [numberWidth](int value) {
        return QByteArray::number(value).rightJustified(numberWidth, '0');
    };
    const int headerSize = headerTemplate.size();
    const int startHtml = headerSize;
    const int startFragment = headerSize + head.size();
    const int endFragment = startFragment + fragment.size();
    const int endHtml = startFragment + fragment.size() + tail.size();
    QByteArray header = QByteArrayLiteral("Version:0.9\r\n");
    header += "StartHTML:" + offsetField(startHtml) + "\r\n";
    header += "EndHTML:" + offsetField(endHtml) + "\r\n";
    header += "StartFragment:" + offsetField(startFragment) + "\r\n";
    header += "EndFragment:" + offsetField(endFragment) + "\r\n";
    require(header.size() == headerSize, "the CF_HTML header length changed while building");
    return header + head + fragment + tail;
}

void setClipboardDibV5(const QByteArray& bytes) {
    ClipboardScope clipboard; require(clipboard.open(), "could not open the clipboard for DIBV5");
    require(EmptyClipboard() != FALSE, "could not empty the clipboard for DIBV5");
    setClipboardPayload(CF_DIBV5, bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// Qt's Windows clipboard maps a raw "PNG" flavor to a live QImage instead of
// image/png bytes, so encoded clipboard payloads always surface through the
// detached fast path. The reachable decode-first image flow is a local file
// reference (Explorer "copy" of an image file), driven here through CF_HDROP.
void setClipboardFileDrop(const QString& path) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const std::size_t payloadBytes = (native.size() + 2) * sizeof(wchar_t);
    QByteArray buffer(static_cast<int>(sizeof(DROPFILES) + payloadBytes), 0);
    auto* dropFiles = reinterpret_cast<DROPFILES*>(buffer.data());
    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->fWide = TRUE;
    std::memcpy(buffer.data() + sizeof(DROPFILES), native.c_str(),
                (native.size() + 1) * sizeof(wchar_t));
    ClipboardScope clipboard; require(clipboard.open(), "could not open the clipboard for HDROP");
    require(EmptyClipboard() != FALSE, "could not empty the clipboard for HDROP");
    setClipboardPayload(CF_HDROP, buffer.constData(), static_cast<std::size_t>(buffer.size()));
}

void setClipboardHtml(const QByteArray& bytes) {
    ClipboardScope clipboard; require(clipboard.open(), "could not open the clipboard for HTML");
    require(EmptyClipboard() != FALSE, "could not empty the clipboard for HTML");
    setClipboardPayload(registeredClipboardFormat(L"HTML Format"), bytes.constData(),
                        static_cast<std::size_t>(bytes.size()));
}

// --- Per-stage aggregation ---------------------------------------------------
QVector<QPair<QString, double>> milestoneStages(const QJsonObject& milestonesNanoseconds) {
    QVector<QPair<QString, double>> ordered;
    for (auto iterator = milestonesNanoseconds.begin(); iterator != milestonesNanoseconds.end();
         ++iterator) {
        ordered.append({iterator.key(), iterator.value().toDouble() / 1e6});
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& left, const auto& right) { return left.second < right.second; });
    QVector<QPair<QString, double>> stages;
    QString previous = QStringLiteral("begin");
    double previousValue = 0.0;
    for (const auto& entry : ordered) {
        stages.append({QStringLiteral("%1→%2").arg(previous, entry.first),
                       entry.second - previousValue});
        previous = entry.first;
        previousValue = entry.second;
    }
    return stages;
}

struct ScenarioSeries final {
    QHash<QString, QVector<double>> milestones;
    QHash<QString, QVector<double>> stages;
    QHash<QString, QVector<double>> spans;
    QHash<QString, QVector<double>> counters;
    QVector<double> endToEnd;
    QVector<double> workingSet;
    QVector<double> peakWorkingSet;
    QVector<double> materializationMegabytes;
    QVector<double> firstContentFrameResidual;
    int shellHits = 0;
    int samples = 0;
};

void accumulateRecord(ScenarioSeries& series, const QJsonObject& record) {
    const QJsonObject milestones = record.value(QStringLiteral("milestones_ns")).toObject();
    const auto milestoneValue = [&milestones](const QString& key) {
        const QJsonValue value = milestones.value(key);
        return value.isDouble() ? std::optional<double>(value.toDouble()) : std::nullopt;
    };
    const std::optional<double> firstContentFrame =
        milestoneValue(QStringLiteral("window.first_content_frame"));
    if (!firstContentFrame.has_value()) {
        throw std::runtime_error("pin sample did not publish a first content frame");
    }
    const QStringList completionMilestones{
        QStringLiteral("workflow.destination_complete"),
        QStringLiteral("controller.presentation_complete"),
        QStringLiteral("clipboard.presentation_complete")};
    for (const QString& completionKey : completionMilestones) {
        const std::optional<double> completion = milestoneValue(completionKey);
        if (completion.has_value() && *firstContentFrame > *completion) {
            throw std::runtime_error("first content frame occurred after destination completion");
        }
    }
    ++series.samples;
    series.endToEnd.push_back(record.value(QStringLiteral("end_to_end_ns")).toDouble() / 1e6);
    series.workingSet.push_back(record.value(QStringLiteral("working_set_bytes")).toDouble() /
                                (1024.0 * 1024.0));
    series.peakWorkingSet.push_back(
        record.value(QStringLiteral("peak_working_set_bytes")).toDouble() / (1024.0 * 1024.0));
    for (const auto& stage : milestoneStages(record.value(QStringLiteral("milestones_ns")).toObject())) {
        series.stages[stage.first].push_back(stage.second);
    }
    for (auto iterator = milestones.begin(); iterator != milestones.end(); ++iterator) {
        series.milestones[iterator.key()].push_back(iterator.value().toDouble() / 1e6);
    }
    const std::optional<double> renderFinished =
        milestoneValue(QStringLiteral("export.render_finished"));
    if (renderFinished.has_value()) {
        series.firstContentFrameResidual.push_back(
            (*firstContentFrame - *renderFinished) / 1e6);
    }
    const auto appendMilestoneDelta = [&series, &milestoneValue](const QString& from,
                                                                  const QString& to) {
        const std::optional<double> start = milestoneValue(from);
        const std::optional<double> end = milestoneValue(to);
        if (start.has_value() && end.has_value()) {
            series.stages[from + QStringLiteral("→") + to].push_back((*end - *start) / 1e6);
        }
    };
    appendMilestoneDelta(QStringLiteral("export.render_finished"),
                         QStringLiteral("export.result_published"));
    appendMilestoneDelta(QStringLiteral("export.result_published"),
                         QStringLiteral("window.first_content_frame"));
    appendMilestoneDelta(QStringLiteral("window.shell_visible"),
                         QStringLiteral("window.first_content_frame"));
    const QJsonObject spans = record.value(QStringLiteral("spans_ns")).toObject();
    for (auto iterator = spans.begin(); iterator != spans.end(); ++iterator) {
        series.spans[iterator.key()].push_back(iterator.value().toDouble() / 1e6);
    }
    const QJsonObject counters = record.value(QStringLiteral("counters")).toObject();
    for (auto iterator = counters.begin(); iterator != counters.end(); ++iterator) {
        const QString key = iterator.key();
        const double value = iterator.value().toDouble();
        if (key == QStringLiteral("clipboard.reader_snapshot_ns")) {
            series.spans[QStringLiteral("clipboard.reader_snapshot")].push_back(value / 1e6);
            continue;
        }
        series.counters[key].push_back(value);
    }
    const double bytes = counters.value(QStringLiteral("materialization.bytes")).toDouble();
    if (bytes > 0.0) series.materializationMegabytes.push_back(bytes / (1024.0 * 1024.0));
    if (counters.contains(QStringLiteral("shell.hit"))) ++series.shellHits;
}

QJsonObject statisticsMap(const QHash<QString, QVector<double>>& source) {
    QJsonObject report;
    QVector<QString> keys = source.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString& key : keys) report.insert(key, statistics(source.value(key)));
    return report;
}

// The curated per-scenario core metrics: end-to-end next to the export or
// decode work and the phases around it, each with its share of the total so
// the report answers "where does the display time go" directly.
QJsonObject coreReport(const ScenarioSeries& series) {
    const double totalMean =
        series.endToEnd.isEmpty()
            ? 0.0
            : std::accumulate(series.endToEnd.cbegin(), series.endToEnd.cend(), 0.0) /
                  series.endToEnd.size();
    QJsonObject core;
    const auto add = [&](const QString& name, const QVector<double>& values) {
        if (values.isEmpty()) return;
        QJsonObject entry = statistics(values);
        if (totalMean > 0.0) {
            entry.insert(QStringLiteral("share_pct"),
                         entry.value(QStringLiteral("mean_ms")).toDouble() / totalMean * 100.0);
        }
        core.insert(name, entry);
    };
    add(QStringLiteral("end_to_end"), series.endToEnd);
    add(QStringLiteral("residual_first_content_frame_after_export"),
        series.firstContentFrameResidual);
    const QStringList milestoneKeys{
        QStringLiteral("controller.enter"), QStringLiteral("window.hwnd_created"),
        QStringLiteral("window.before_show"), QStringLiteral("window.show_returned"),
        QStringLiteral("window.shell_visible"), QStringLiteral("window.first_content_frame"),
        QStringLiteral("window.canvas_repainted"), QStringLiteral("export.render_started"),
        QStringLiteral("export.dispatch_started"), QStringLiteral("export.render_finished"),
        QStringLiteral("export.result_published"),
        QStringLiteral("controller.snapshot_ready"),
        QStringLiteral("controller.materialize_started"),
        QStringLiteral("clipboard.decode_started"), QStringLiteral("clipboard.decode_finished"),
        QStringLiteral("controller.presentation_complete")};
    for (const QString& key : milestoneKeys) {
        add(QStringLiteral("time_to_") + key, series.milestones.value(key));
    }
    const QStringList spanKeys{
        QStringLiteral("export.prepare_pin_plan"), QStringLiteral("export.serialize_document"),
        QStringLiteral("export.render_selection"), QStringLiteral("export.render_canvas"),
        QStringLiteral("export.compose_result"), QStringLiteral("export.convert_argb32"),
        QStringLiteral("export.scrolling_trimmed_snapshot"),
        QStringLiteral("export.materialize_scrolling_snapshot"),
        QStringLiteral("clipboard.reader_snapshot"),
        QStringLiteral("clipboard.decode_file_image"),
        QStringLiteral("clipboard.decode_html_document"),
        QStringLiteral("clipboard.html_document_layout"),
        QStringLiteral("clipboard.html_document_render"),
        QStringLiteral("clipboard.decode_detached_image"),
        QStringLiteral("ui.present_pinned_selection"), QStringLiteral("ui.present_pinned_image"),
        QStringLiteral("window.present"), QStringLiteral("window.materialize_image")};
    for (const QString& key : spanKeys) {
        add(QStringLiteral("duration_") + key, series.spans.value(key));
    }
    const QStringList stageKeys{
        QStringLiteral("window.present_returned→export.render_started"),
        QStringLiteral("export.render_finished→export.result_published"),
        QStringLiteral("export.result_published→window.first_content_frame"),
        QStringLiteral("window.shell_visible→window.first_content_frame"),
        QStringLiteral("clipboard.decode_scheduled→clipboard.decode_started"),
        QStringLiteral("controller.materialize_submitted→controller.materialize_started"),
        QStringLiteral("controller.snapshot_requested→controller.snapshot_ready")};
    for (const QString& key : stageKeys) {
        add(QStringLiteral("stage_") + key, series.stages.value(key));
    }
    return core;
}

QJsonObject firstContentFrameAcceptance(const ScenarioSeries& series) {
    constexpr double targetP95Milliseconds = 8.0;
    const QJsonObject measured = statistics(series.firstContentFrameResidual);
    const double p95 = measured.value(QStringLiteral("p95_ms")).toDouble();
    return {{QStringLiteral("metric"),
             QStringLiteral("first_content_frame_minus_export_render_finished")},
            {QStringLiteral("target_p95_ms"), targetP95Milliseconds},
            {QStringLiteral("p95_ms"), p95},
            {QStringLiteral("passed"), series.firstContentFrameResidual.isEmpty() ||
                                           p95 <= targetP95Milliseconds},
            {QStringLiteral("applicable"), !series.firstContentFrameResidual.isEmpty()},
            {QStringLiteral("samples"), series.firstContentFrameResidual.size()}};
}

QJsonObject scenarioReport(const QString& id, const ScenarioSeries& series) {
    QJsonObject counters;
    QVector<QString> keys = series.counters.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString& key : keys) {
        const QVector<double>& values = series.counters.value(key);
        const double sum = std::accumulate(values.cbegin(), values.cend(), 0.0);
        counters.insert(key, QJsonObject{{QStringLiteral("present"), values.size()},
                                         {QStringLiteral("sum"), sum},
                                         {QStringLiteral("mean"), sum / values.size()}});
    }
    return {{QStringLiteral("id"), id},
            {QStringLiteral("samples"), series.samples},
            {QStringLiteral("end_to_end"), statistics(series.endToEnd)},
            {QStringLiteral("core"), coreReport(series)},
            {QStringLiteral("milestones_ms"), statisticsMap(series.milestones)},
            {QStringLiteral("stages_ms"), statisticsMap(series.stages)},
            {QStringLiteral("spans_ms"), statisticsMap(series.spans)},
            {QStringLiteral("first_content_frame_acceptance"),
             firstContentFrameAcceptance(series)},
            {QStringLiteral("materialization_mb"), statistics(series.materializationMegabytes)},
            {QStringLiteral("shell_pool_hit_rate"),
             series.samples == 0 ? 0.0 : static_cast<double>(series.shellHits) / series.samples},
            {QStringLiteral("working_set"), statistics(series.workingSet, QStringLiteral("mb"))},
            {QStringLiteral("peak_working_set"),
             statistics(series.peakWorkingSet, QStringLiteral("mb"))},
            {QStringLiteral("counters"), counters}};
}

// --- Scenario drivers --------------------------------------------------------
struct BenchmarkConfiguration {
    QString appPath;
    QString output;
    QStringList scenarios;
    int warmups = 3;
    int samples = 40;
    int timeout = 30000;
    int screenIndex = 0;
    int scrollSteps = 8;
    int scrollDistance = 96;
    int scrollSettleMilliseconds = 700;
    QSize clipboardImageSize{1920, 1080};
};

void invokeQuickScreenshot(IUIAutomation& automation, const ChildProcess& child, int timeout) {
    auto screenshot = waitFor([&]() { return findByAutomationId(automation, child.pid(),
                                                                L"settings-item-quick-screenshot"); },
                              timeout);
    require(screenshot.get() != nullptr && invoke(*screenshot.get()),
            "could not invoke screenshot capture");
}

void clickPinButton(IUIAutomation& automation, const ChildProcess& child, int timeout) {
    auto pin = waitFor([&]() { return findByAutomationId(automation, child.pid(),
                                                         L"screenshotPinToScreenButton"); },
                       timeout);
    require(pin.get() != nullptr, "pin button did not appear");
    clickCenter(*pin.get());
}

// A scripted drag occasionally lands before the capture overlay is ready to
// own the input surface, leaving the session without a selection. Re-drag on
// the still-open overlay instead of failing the sample.
void dragWithRetry(IUIAutomation& automation, const ChildProcess& child,
                   const wchar_t* expectedControl, int timeout,
                   const std::function<void()>& drag) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        drag();
        if (waitFor([&]() { return findByAutomationId(automation, child.pid(), expectedControl); },
                    attempt == 0 ? std::min(timeout, 4000) : timeout)
                .get() != nullptr) {
            return;
        }
    }
    require(false, "toolbar control did not appear after the selection drag");
}

struct ScenarioPayloads {
    QByteArray pngBytes;
    QByteArray dibV5Bytes;
    QByteArray htmlBytes;
    QString imageFilePath;
    QSize imageSize;
};

QString writeClipboardImageFile(const QString& directory, const QByteArray& pngBytes) {
    const QString path = QDir(directory).filePath(QStringLiteral("clipboard-pin-payload.png"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "could not create the clipboard image file");
    require(file.write(pngBytes) == pngBytes.size(), "could not write the clipboard image file");
    file.close();
    return path;
}

void driveNormalSelection(IUIAutomation& automation, const ChildProcess& child,
                          const BenchmarkConfiguration& configuration, const RECT& monitor,
                          int fraction) {
    invokeQuickScreenshot(automation, child, configuration.timeout);
    std::this_thread::sleep_for(1000ms);
    dragWithRetry(automation, child, L"screenshotPinToScreenButton", configuration.timeout,
                  [&]() { dragSelection(monitor, fraction); });
    clickPinButton(automation, child, configuration.timeout);
}

void driveScrollingSelection(IUIAutomation& automation, const ChildProcess& child,
                             const BenchmarkConfiguration& configuration,
                             ScrollingSourceWindow& source) {
    source.reset();
    invokeQuickScreenshot(automation, child, configuration.timeout);
    std::this_thread::sleep_for(1000ms);
    dragWithRetry(automation, child, L"screenshotScrollingScreenshotButton",
                  configuration.timeout, [&]() { dragRect(source.rect()); });
    auto scrolling = waitFor([&]() { return findByAutomationId(
                                      automation, child.pid(), L"screenshotScrollingScreenshotButton"); },
                             configuration.timeout);
    require(scrolling.get() != nullptr && invoke(*scrolling.get()),
            "could not enter scrolling screenshot");
    auto thumbnail = waitFor(
        [&]() { return findVisibleByAutomationId(automation, child.pid(),
                                                 L"screenshot-scrolling-thumbnail"); },
        configuration.timeout);
    require(thumbnail.get() != nullptr, "scrolling thumbnail did not appear");
    const RECT selection = source.rect();
    sendMouse((selection.left + selection.right) / 2, (selection.top + selection.bottom) / 2,
              MOUSEEVENTF_MOVE);
    std::this_thread::sleep_for(250ms);
    for (int step = 0; step < configuration.scrollSteps; ++step) {
        source.scrollDown(configuration.scrollDistance);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(configuration.scrollSettleMilliseconds));
    }
    clickPinButton(automation, child, configuration.timeout);
}

void invokePinClipboard(IUIAutomation& automation, const ChildProcess& child, int timeout,
                        const RECT& monitor) {
    sendMouse((monitor.left + monitor.right) / 2, (monitor.top + monitor.bottom) / 2,
              MOUSEEVENTF_MOVE);
    auto item = waitFor([&]() { return findByAutomationId(
                                    automation, child.pid(),
                                    L"settings-item-quick-pin-clipboard-content"); },
                        timeout);
    require(item.get() != nullptr && invoke(*item.get()),
            "could not invoke pin clipboard content");
}

QString expectedTraceScenario(const QString& scenario) {
    if (scenario.startsWith(QStringLiteral("normal-selection"))) {
        return QStringLiteral("normal-selection");
    }
    if (scenario == QStringLiteral("clipboard-image-file")) {
        return QStringLiteral("clipboard-file-image");
    }
    return scenario;
}

// --- Self-tests --------------------------------------------------------------
int clipboardHtmlHeaderValue(const QByteArray& payload, const char* key) {
    const int offset = payload.indexOf(key);
    require(offset >= 0, "the CF_HTML header is missing a field");
    return payload.mid(offset + static_cast<int>(std::strlen(key)), 10).trimmed().toInt();
}

bool runSelfTest() {
    const QVector<double> values{1.0, 2.0, 3.0, 4.0, 5.0};
    if (!qFuzzyCompare(statistics(values).value(QStringLiteral("p50_ms")).toDouble() + 1.0, 4.0)) {
        return false;
    }

    const QJsonObject milestones{{QStringLiteral("controller.enter"), 100},
                                 {QStringLiteral("window.before_show"), 600},
                                 {QStringLiteral("window.show_returned"), 900}};
    const QVector<QPair<QString, double>> stages = milestoneStages(milestones);
    if (stages.size() != 3 || !qFuzzyCompare(stages.at(0).second, 0.0001) ||
        !qFuzzyCompare(stages.at(1).second, 0.0005) ||
        !qFuzzyCompare(stages.at(2).second, 0.0003) ||
        stages.at(0).first != QStringLiteral("begin→controller.enter") ||
        stages.at(2).first != QStringLiteral("window.before_show→window.show_returned")) {
        return false;
    }

    const QByteArray fragment = QByteArrayLiteral("<p>benchmark fragment</p>");
    const QByteArray html = wrapClipboardHtml(fragment);
    const int startFragment = clipboardHtmlHeaderValue(html, "StartFragment:");
    const int endFragment = clipboardHtmlHeaderValue(html, "EndFragment:");
    const int startHtml = clipboardHtmlHeaderValue(html, "StartHTML:");
    const int endHtml = clipboardHtmlHeaderValue(html, "EndHTML:");
    if (html.mid(startFragment, endFragment - startFragment) != fragment ||
        html.mid(startHtml, 5) != "<html" || html.mid(endHtml - 7, 7) != "</html>") {
        return false;
    }

    const QImage image = makeClipboardImage(QSize(4, 4));
    const QByteArray png = encodeClipboardPng(image);
    if (png.size() < 8 || png.startsWith("\x89PNG") == false) return false;

    const QByteArray dib = encodeClipboardDibV5(image);
    if (dib.size() != static_cast<int>(sizeof(BITMAPV5HEADER)) + 4 * 4 * 4) return false;
    const auto* header = reinterpret_cast<const BITMAPV5HEADER*>(dib.constData());
    if (header->bV5Width != 4 || header->bV5Height != 4 || header->bV5BitCount != 32 ||
        header->bV5Compression != BI_BITFIELDS) {
        return false;
    }
    const QImage bottomLeft = image.copy(0, 3, 1, 1);
    const auto* lastRow = reinterpret_cast<const std::uint32_t*>(
        dib.constData() + sizeof(BITMAPV5HEADER));
    const QRgb expected = bottomLeft.pixel(0, 0);
    if (lastRow[0] != ((0xffu << 24) | (static_cast<unsigned>(qRed(expected)) << 16) |
                       (static_cast<unsigned>(qGreen(expected)) << 8) |
                       static_cast<unsigned>(qBlue(expected)))) {
        return false;
    }

    ScenarioSeries series;
    accumulateRecord(series, QJsonObject{
        {QStringLiteral("scenario"), QStringLiteral("clipboard-html")},
        {QStringLiteral("end_to_end_ns"), 2000000},
        {QStringLiteral("working_set_bytes"), 2 * 1024 * 1024},
        {QStringLiteral("peak_working_set_bytes"), 3 * 1024 * 1024},
        {QStringLiteral("milestones_ns"),
         QJsonObject{{QStringLiteral("controller.enter"), 100000},
                     {QStringLiteral("window.shell_visible"), 800000},
                     {QStringLiteral("export.render_finished"), 1200000},
                     {QStringLiteral("window.first_content_frame"), 1500000}}},
        {QStringLiteral("spans_ns"), QJsonObject{{QStringLiteral("window.present"), 500000}}},
        {QStringLiteral("counters"),
         QJsonObject{{QStringLiteral("shell.hit"), 1},
                     {QStringLiteral("clipboard.reader_snapshot_ns"), 250000},
                     {QStringLiteral("materialization.bytes"), 1024 * 1024}}}});
    if (series.samples != 1 || series.shellHits != 1 || series.endToEnd != QVector<double>{2.0} ||
        series.spans.value(QStringLiteral("clipboard.reader_snapshot")) != QVector<double>{0.25} ||
        series.materializationMegabytes.size() != 1 ||
        !qFuzzyCompare(series.materializationMegabytes.first(), 1.0)) {
        return false;
    }
    const QJsonObject report = scenarioReport(QStringLiteral("self-test"), series);
    if (report.value(QStringLiteral("samples")).toInt() != 1 ||
        !qFuzzyCompare(report.value(QStringLiteral("shell_pool_hit_rate")).toDouble(), 1.0)) {
        return false;
    }
    const QJsonObject core = report.value(QStringLiteral("core")).toObject();
    if (!core.contains(QStringLiteral("end_to_end")) ||
        !qFuzzyCompare(core.value(QStringLiteral("end_to_end")).toObject()
                           .value(QStringLiteral("share_pct"))
                           .toDouble(),
                       100.0) ||
        !core.contains(QStringLiteral("time_to_controller.enter")) ||
        !core.contains(QStringLiteral("duration_window.present")) ||
        !core.contains(QStringLiteral("duration_clipboard.reader_snapshot"))) {
        return false;
    }
    const QJsonObject acceptance = report.value(QStringLiteral("first_content_frame_acceptance")).toObject();
    if (!acceptance.value(QStringLiteral("passed")).toBool() ||
        !qFuzzyCompare(acceptance.value(QStringLiteral("p95_ms")).toDouble() + 1.0, 1.3)) {
        return false;
    }
    return true;
}

int run(const QCommandLineParser& parser) {
    BenchmarkConfiguration configuration;
    configuration.appPath = parser.value(QStringLiteral("app"));
    configuration.output = QDir::cleanPath(parser.value(QStringLiteral("output")));
    configuration.warmups = parser.value(QStringLiteral("warmups")).toInt();
    configuration.samples = parser.value(QStringLiteral("samples")).toInt();
    configuration.timeout = parser.value(QStringLiteral("timeout-ms")).toInt();
    configuration.screenIndex = parser.value(QStringLiteral("screen-index")).toInt();
    configuration.scrollSteps = parser.value(QStringLiteral("scroll-steps")).toInt();
    configuration.scrollDistance = parser.value(QStringLiteral("scroll-distance")).toInt();
    configuration.scrollSettleMilliseconds =
        parser.value(QStringLiteral("scroll-settle-ms")).toInt();
    const QStringList sizeParts =
        parser.value(QStringLiteral("clipboard-image-size"))
            .split(QLatin1Char('x'), Qt::SkipEmptyParts);
    require(sizeParts.size() == 2, "invalid --clipboard-image-size (expected WxH)");
    configuration.clipboardImageSize = {sizeParts.at(0).toInt(), sizeParts.at(1).toInt()};
    configuration.scenarios =
        parser.value(QStringLiteral("scenarios"))
            .split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (configuration.scenarios == QStringList{QStringLiteral("all")}) {
        configuration.scenarios = QStringList{
            QStringLiteral("normal-selection-small"), QStringLiteral("normal-selection-medium"),
            QStringLiteral("normal-selection-large"), QStringLiteral("scrolling-selection"),
            QStringLiteral("clipboard-image-detached"), QStringLiteral("clipboard-image-file"),
            QStringLiteral("clipboard-html")};
    }
    for (const QString& scenario : configuration.scenarios) {
        require(scenario == QStringLiteral("normal-selection-small") ||
                    scenario == QStringLiteral("normal-selection-medium") ||
                    scenario == QStringLiteral("normal-selection-large") ||
                    scenario == QStringLiteral("scrolling-selection") ||
                    scenario == QStringLiteral("clipboard-image-detached") ||
                    scenario == QStringLiteral("clipboard-image-file") ||
                    scenario == QStringLiteral("clipboard-html"),
                "unknown benchmark scenario");
    }
    require(!configuration.appPath.isEmpty() && configuration.warmups >= 0 &&
                configuration.samples > 0 && configuration.timeout > 0 &&
                !configuration.scenarios.isEmpty() && configuration.scrollSteps > 0 &&
                configuration.scrollDistance > 0 && configuration.scrollSettleMilliseconds >= 0,
            "invalid benchmark arguments");
    const QVector<RECT> displayList = monitors();
    require(configuration.screenIndex >= 0 && configuration.screenIndex < displayList.size(),
            "monitor index unavailable");
    const RECT monitor = displayList.at(configuration.screenIndex);

    QDir().mkpath(configuration.output);
    const QString tracePath =
        QDir(configuration.output).filePath(QStringLiteral("app-trace.jsonl"));
    QFile::remove(tracePath);
    _putenv_s("SNOW_SHOT_PIN_PERF_TRACE", tracePath.toLocal8Bit().constData());
    ScopedCom com; require(SUCCEEDED(com.result()), "COM initialization failed");
    auto automation = createAutomation();
    ChildProcess child; require(child.start(configuration.appPath), "could not start snow_shot");
    TraceCollector collector(tracePath);

    ScenarioPayloads payloads;
    const bool needsClipboardPayloads =
        configuration.scenarios.contains(QStringLiteral("clipboard-image-detached")) ||
        configuration.scenarios.contains(QStringLiteral("clipboard-image-file")) ||
        configuration.scenarios.contains(QStringLiteral("clipboard-html"));
    if (needsClipboardPayloads) {
        const QImage image = makeClipboardImage(configuration.clipboardImageSize);
        payloads.imageSize = image.size();
        payloads.pngBytes = encodeClipboardPng(image);
        payloads.dibV5Bytes = encodeClipboardDibV5(image);
        payloads.htmlBytes = wrapClipboardHtml(buildClipboardHtmlFragment());
        if (configuration.scenarios.contains(QStringLiteral("clipboard-image-file"))) {
            payloads.imageFilePath =
                writeClipboardImageFile(configuration.output, payloads.pngBytes);
        }
    }
    std::unique_ptr<ScrollingSourceWindow> scrollingSource;
    if (configuration.scenarios.contains(QStringLiteral("scrolling-selection"))) {
        const RECT work = monitor;
        scrollingSource = std::make_unique<ScrollingSourceWindow>(
            work.left + (work.right - work.left - kScrollingSourceExtent) / 2,
            work.top + (work.bottom - work.top - kScrollingSourceExtent) / 2,
            kScrollingSourceExtent);
    }

    QVector<QJsonObject> records;
    for (const QString& scenario : configuration.scenarios) {
        for (int iteration = -configuration.warmups; iteration < configuration.samples;
             ++iteration) {
            const QString traceScenario = expectedTraceScenario(scenario);
            if (scenario == QStringLiteral("normal-selection-small")) {
                driveNormalSelection(*automation.get(), child, configuration, monitor, 25);
            } else if (scenario == QStringLiteral("normal-selection-medium")) {
                driveNormalSelection(*automation.get(), child, configuration, monitor, 50);
            } else if (scenario == QStringLiteral("normal-selection-large")) {
                driveNormalSelection(*automation.get(), child, configuration, monitor, 90);
            } else if (scenario == QStringLiteral("scrolling-selection")) {
                driveScrollingSelection(*automation.get(), child, configuration, *scrollingSource);
            } else if (scenario == QStringLiteral("clipboard-image-detached")) {
                setClipboardDibV5(payloads.dibV5Bytes);
                invokePinClipboard(*automation.get(), child, configuration.timeout, monitor);
            } else if (scenario == QStringLiteral("clipboard-image-file")) {
                setClipboardFileDrop(payloads.imageFilePath);
                invokePinClipboard(*automation.get(), child, configuration.timeout, monitor);
            } else if (scenario == QStringLiteral("clipboard-html")) {
                setClipboardHtml(payloads.htmlBytes);
                invokePinClipboard(*automation.get(), child, configuration.timeout, monitor);
            }
            QJsonObject record =
                collector.next(traceScenario, configuration.timeout, child);
            record.insert(QStringLiteral("scenario"), scenario);
            record.insert(QStringLiteral("iteration"), iteration);
            record.insert(QStringLiteral("warmup"), iteration < 0);
            record.insert(QStringLiteral("working_set_bytes"), child.workingSetBytes(false));
            record.insert(QStringLiteral("peak_working_set_bytes"), child.workingSetBytes(true));
            record.insert(QStringLiteral("payload_width"), payloads.imageSize.width());
            record.insert(QStringLiteral("payload_height"), payloads.imageSize.height());
            record.insert(QStringLiteral("payload_png_bytes"), payloads.pngBytes.size());
            record.insert(QStringLiteral("payload_dib_bytes"), payloads.dibV5Bytes.size());
            record.insert(QStringLiteral("payload_html_bytes"), payloads.htmlBytes.size());
            record.insert(QStringLiteral("payload_file"), payloads.imageFilePath);
            record.insert(QStringLiteral("scroll_steps"), configuration.scrollSteps);
            record.insert(QStringLiteral("scroll_distance"), configuration.scrollDistance);
            records.push_back(record);
            const qint64 hwndValue =
                record.value(QStringLiteral("counters")).toObject()
                    .value(QStringLiteral("window.hwnd")).toInteger();
            if (hwndValue != 0) {
                PostMessageW(reinterpret_cast<HWND>(static_cast<quintptr>(hwndValue)), WM_CLOSE, 0,
                             0);
            }
            std::this_thread::sleep_for(400ms);
        }
    }
    child.stop();

    QFile raw(QDir(configuration.output).filePath(QStringLiteral("raw.jsonl")));
    require(raw.open(QIODevice::WriteOnly | QIODevice::Truncate), "could not write raw report");
    for (const QJsonObject& record : records) {
        raw.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
        raw.write("\n");
    }
    raw.close();

    QJsonArray scenarioReports;
    for (const QString& scenario : configuration.scenarios) {
        ScenarioSeries series;
        for (const QJsonObject& record : records) {
            if (record.value(QStringLiteral("scenario")).toString() != scenario ||
                record.value(QStringLiteral("warmup")).toBool()) {
                continue;
            }
            accumulateRecord(series, record);
        }
        require(series.samples > 0, "a measured scenario produced no samples");
        scenarioReports.append(scenarioReport(scenario, series));
    }
    const QJsonObject report{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("benchmark"), QStringLiteral("screenshot_pin_to_screen")},
        {QStringLiteral("generated_utc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("warmups"), configuration.warmups},
        {QStringLiteral("samples"), configuration.samples},
        {QStringLiteral("screen_index"), configuration.screenIndex},
        {QStringLiteral("scroll_steps"), configuration.scrollSteps},
        {QStringLiteral("scroll_distance"), configuration.scrollDistance},
        {QStringLiteral("clipboard_image"),
         QJsonObject{{QStringLiteral("width"), payloads.imageSize.width()},
                     {QStringLiteral("height"), payloads.imageSize.height()},
                     {QStringLiteral("png_bytes"), payloads.pngBytes.size()},
                     {QStringLiteral("dib_bytes"), payloads.dibV5Bytes.size()},
                     {QStringLiteral("html_bytes"), payloads.htmlBytes.size()}}},
        {QStringLiteral("scenarios"), scenarioReports},
        {QStringLiteral("environment"),
         QJsonObject{{QStringLiteral("os"), QSysInfo::prettyProductName()},
                     {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
                     {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()}}}};
    QFile reportFile(QDir(configuration.output).filePath(QStringLiteral("report.json")));
    require(reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "could not write report.json");
    reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    reportFile.close();
    QFile html(QDir(configuration.output).filePath(QStringLiteral("report.html")));
    require(html.open(QIODevice::WriteOnly | QIODevice::Truncate), "could not write report.html");
    html.write(("<!doctype html><meta charset=utf-8><title>Snow Shot pin performance</title>"
                "<h1>Pin-to-screen performance</h1><pre>" +
                htmlEscape(QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented))) +
                "</pre>")
                   .toUtf8());
    html.close();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("snow-shot-pin-to-screen-performance-benchmark"));
    SetProcessDPIAware();
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Native Windows pin-to-screen performance benchmark"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("app"), QStringLiteral("snow_shot executable"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("output directory"), QStringLiteral("directory"), QStringLiteral("pin-to-screen-performance")});
    parser.addOption({QStringLiteral("screen-index"), QStringLiteral("monitor index"), QStringLiteral("index"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("warmups"), QStringLiteral("warmup samples"), QStringLiteral("count"), QStringLiteral("3")});
    parser.addOption({QStringLiteral("samples"), QStringLiteral("measured samples"), QStringLiteral("count"), QStringLiteral("40")});
    parser.addOption({QStringLiteral("timeout-ms"), QStringLiteral("sample timeout"), QStringLiteral("milliseconds"), QStringLiteral("30000")});
    parser.addOption({QStringLiteral("scenarios"), QStringLiteral("comma-separated scenarios (normal-selection-small, normal-selection-medium, normal-selection-large, scrolling-selection, clipboard-image-detached, clipboard-image-file, clipboard-html, or all)"), QStringLiteral("list"), QStringLiteral("all")});
    parser.addOption({QStringLiteral("scroll-steps"), QStringLiteral("scroll steps before pinning a scrolling screenshot"), QStringLiteral("count"), QStringLiteral("8")});
    parser.addOption({QStringLiteral("scroll-distance"), QStringLiteral("scroll distance per step in pixels"), QStringLiteral("pixels"), QStringLiteral("96")});
    parser.addOption({QStringLiteral("scroll-settle-ms"), QStringLiteral("settle time after each scroll step"), QStringLiteral("milliseconds"), QStringLiteral("700")});
    parser.addOption({QStringLiteral("clipboard-image-size"), QStringLiteral("clipboard image size"), QStringLiteral("WxH"), QStringLiteral("1920x1080")});
    parser.addOption({QStringLiteral("self-test"), QStringLiteral("run report self-tests")});
    parser.process(application);
    try {
        if (parser.isSet(QStringLiteral("self-test"))) return runSelfTest() ? 0 : 1;
        return run(parser);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
