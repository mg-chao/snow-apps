#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QStringList>
#include <QVector>

#include <Windows.h>
#include <UIAutomation.h>
#include <objbase.h>
#include <oleauto.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

constexpr qint64 kBytesPerKibibyte = 1024;
constexpr qint64 kBytesPerMebibyte = 1024 * 1024;
constexpr qint64 kDefaultMaximumFirstScreenshotMilliseconds = 100;
constexpr qint64 kAdditionalMonitorScreenshotBudgetMilliseconds = 50;
constexpr qint64 kDefaultMaximumToolbarShowMilliseconds = 50;
constexpr qint64 kDefaultMaximumPrivateWorkingSetMebibytes = 18;
constexpr qint64 kDefaultMaximumPrivateWorkingSetDeltaMebibytes = 2;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T> class ComPtr final {
  public:
    ComPtr() = default;
    ComPtr(ComPtr&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.m_value, nullptr));
        }
        return *this;
    }
    ~ComPtr() {
        reset();
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    [[nodiscard]] T* get() const {
        return m_value;
    }
    T** put() {
        reset();
        return &m_value;
    }
    void reset(T* value = nullptr) {
        if (m_value != nullptr) {
            m_value->Release();
        }
        m_value = value;
    }

  private:
    T* m_value = nullptr;
};

class ScopedCom final {
  public:
    ScopedCom() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() {
        if (SUCCEEDED(m_result)) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const {
        return m_result;
    }

  private:
    HRESULT m_result;
};

QString quotedExecutable(const QString& executable, const QStringList& arguments) {
    QString command = QLatin1Char('"') + executable + QLatin1Char('"');
    for (const QString& argument : arguments) {
        require(!argument.contains(QLatin1Char('"')), "benchmark arguments cannot contain quotes");
        command += QLatin1Char(' ');
        if (argument.contains(QLatin1Char(' '))) {
            command += QLatin1Char('"') + argument + QLatin1Char('"');
        } else {
            command += argument;
        }
    }
    return command;
}

class ChildProcess final {
  public:
    ChildProcess() = default;
    ~ChildProcess() {
        stop();
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const QString& executable, const QStringList& arguments) {
        stop();
        const std::wstring executablePath = executable.toStdWString();
        const std::wstring command = quotedExecutable(executable, arguments).toStdWString();
        std::vector<wchar_t> commandLine(command.cbegin(), command.cend());
        commandLine.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (CreateProcessW(executablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &startup, &process) == FALSE) {
            return false;
        }

        CloseHandle(process.hThread);
        m_process = process.hProcess;
        m_processId = process.dwProcessId;
        return true;
    }

    [[nodiscard]] bool alive() const {
        return m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
    }

    [[nodiscard]] bool waitForExit(int timeoutMilliseconds) const {
        if (m_process == nullptr) {
            return false;
        }
        return WaitForSingleObject(m_process, static_cast<DWORD>(timeoutMilliseconds)) ==
               WAIT_OBJECT_0;
    }

    [[nodiscard]] DWORD exitCode() const {
        DWORD code = std::numeric_limits<DWORD>::max();
        if (m_process != nullptr) {
            static_cast<void>(GetExitCodeProcess(m_process, &code));
        }
        return code;
    }

    [[nodiscard]] DWORD processId() const {
        return m_processId;
    }

    [[nodiscard]] HANDLE handle() const {
        return m_process;
    }

    void stop() {
        if (m_process == nullptr) {
            return;
        }
        if (alive()) {
            static_cast<void>(TerminateProcess(m_process, 1));
            static_cast<void>(WaitForSingleObject(m_process, 5000));
        }
        CloseHandle(m_process);
        m_process = nullptr;
        m_processId = 0;
    }

  private:
    HANDLE m_process = nullptr;
    DWORD m_processId = 0;
};

class CursorRestore final {
  public:
    CursorRestore() : m_valid(GetCursorPos(&m_position) != FALSE) {}
    ~CursorRestore() {
        if (m_valid) {
            static_cast<void>(SetCursorPos(m_position.x, m_position.y));
        }
    }

  private:
    POINT m_position{};
    bool m_valid = false;
};

class ScopedEnvironmentVariable final {
  public:
    ScopedEnvironmentVariable(QByteArray name, const QByteArray& value)
        : m_name(std::move(name)), m_wasSet(qEnvironmentVariableIsSet(m_name.constData())),
          m_previous(qgetenv(m_name.constData())) {
        require(qputenv(m_name.constData(), value), "could not set benchmark trace environment");
    }

    ~ScopedEnvironmentVariable() {
        if (m_wasSet) {
            static_cast<void>(qputenv(m_name.constData(), m_previous));
        } else {
            static_cast<void>(qunsetenv(m_name.constData()));
        }
    }

  private:
    QByteArray m_name;
    bool m_wasSet = false;
    QByteArray m_previous;
};

struct MonitorInfo {
    RECT bounds{};
    QString deviceName;
};

QVector<MonitorInfo> monitors() {
    QVector<MonitorInfo> result;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) != FALSE) {
                auto* output = reinterpret_cast<QVector<MonitorInfo>*>(data);
                output->push_back(
                    MonitorInfo{info.rcMonitor, QString::fromWCharArray(info.szDevice)});
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

LONG absoluteCoordinate(LONG value, LONG origin, LONG extent) {
    if (extent <= 1) {
        return 0;
    }
    return static_cast<LONG>((static_cast<double>(value - origin) * 65535.0) /
                             static_cast<double>(extent - 1));
}

void sendMouse(int x, int y, DWORD flags) {
    const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = absoluteCoordinate(x, left, width);
    input.mi.dy = absoluteCoordinate(y, top, height);
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    require(SendInput(1, &input, sizeof(input)) == 1, "SendInput failed");
}

void dragSelection(const RECT& monitor) {
    const int width = monitor.right - monitor.left;
    const int height = monitor.bottom - monitor.top;
    require(width > 100 && height > 100, "selected monitor is too small");
    const int startX = monitor.left + width * 3 / 10;
    const int startY = monitor.top + height * 3 / 10;
    const int endX = monitor.left + width * 7 / 10;
    const int endY = monitor.top + height * 7 / 10;
    sendMouse(startX, startY, MOUSEEVENTF_MOVE);
    sendMouse(startX, startY, MOUSEEVENTF_LEFTDOWN);
    std::this_thread::sleep_for(40ms);
    sendMouse(endX, endY, MOUSEEVENTF_MOVE);
    std::this_thread::sleep_for(40ms);
    sendMouse(endX, endY, MOUSEEVENTF_LEFTUP);
}

void positionCursorForCapture(const RECT& monitor) {
    const int width = monitor.right - monitor.left;
    const int height = monitor.bottom - monitor.top;
    require(width > 0 && height > 0, "selected monitor has invalid bounds");
    sendMouse(monitor.left + width / 2, monitor.top + height / 2, MOUSEEVENTF_MOVE);
    std::this_thread::sleep_for(20ms);
}

ComPtr<IUIAutomation> createAutomation() {
    ComPtr<IUIAutomation> automation;
    require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(automation.put()))),
            "UI Automation initialization failed");
    return automation;
}

ComPtr<IUIAutomationElement>
findVisibleElementByAutomationIdSuffix(IUIAutomation& automation, DWORD processId,
                                       const QString& automationIdSuffix) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation.GetRootElement(root.put()))) {
        return {};
    }

    VARIANT processValue{};
    processValue.vt = VT_I4;
    processValue.lVal = static_cast<LONG>(processId);
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation.CreatePropertyCondition(UIA_ProcessIdPropertyId, processValue,
                                                  condition.put()))) {
        return {};
    }

    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(root.get()->FindAll(TreeScope_Descendants, condition.get(), elements.put()))) {
        return {};
    }

    int length = 0;
    if (FAILED(elements.get()->get_Length(&length))) {
        return {};
    }
    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements.get()->GetElement(index, element.put()))) {
            continue;
        }
        BSTR rawId = nullptr;
        if (FAILED(element.get()->get_CurrentAutomationId(&rawId))) {
            continue;
        }
        const QString id =
            rawId != nullptr ? QString::fromWCharArray(rawId, static_cast<int>(SysStringLen(rawId)))
                             : QString();
        SysFreeString(rawId);
        if (!id.endsWith(automationIdSuffix)) {
            continue;
        }
        BOOL offscreen = TRUE;
        if (SUCCEEDED(element.get()->get_CurrentIsOffscreen(&offscreen)) && offscreen == FALSE) {
            return element;
        }
    }
    return {};
}

ComPtr<IUIAutomationElement> waitForVisibleElement(IUIAutomation& automation, DWORD processId,
                                                   const QString& automationId,
                                                   const ChildProcess& process,
                                                   int timeoutMilliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (process.alive() && std::chrono::steady_clock::now() < deadline) {
        ComPtr<IUIAutomationElement> element =
            findVisibleElementByAutomationIdSuffix(automation, processId, automationId);
        if (element.get() != nullptr) {
            return element;
        }
        std::this_thread::sleep_for(25ms);
    }
    return {};
}

void clickElement(IUIAutomationElement& element) {
    RECT bounds{};
    require(SUCCEEDED(element.get_CurrentBoundingRectangle(&bounds)) &&
                bounds.right > bounds.left && bounds.bottom > bounds.top,
            "cancel button has invalid screen bounds");
    const int x = bounds.left + (bounds.right - bounds.left) / 2;
    const int y = bounds.top + (bounds.bottom - bounds.top) / 2;
    sendMouse(x, y, MOUSEEVENTF_MOVE);
    std::this_thread::sleep_for(25ms);
    sendMouse(x, y, MOUSEEVENTF_LEFTDOWN);
    sendMouse(x, y, MOUSEEVENTF_LEFTUP);
}

enum class PrivateWorkingSetMethod {
    ProcessMemoryCountersEx2,
    QueryWorkingSet,
};

const char* privateWorkingSetMethodName(PrivateWorkingSetMethod method) {
    switch (method) {
    case PrivateWorkingSetMethod::ProcessMemoryCountersEx2:
        return "PROCESS_MEMORY_COUNTERS_EX2.PrivateWorkingSetSize";
    case PrivateWorkingSetMethod::QueryWorkingSet:
        return "QueryWorkingSet.Shared==0 fallback";
    }
    return "unknown";
}

struct PrivateWorkingSetSample {
    qint64 bytes = 0;
    PrivateWorkingSetMethod method = PrivateWorkingSetMethod::QueryWorkingSet;
};

PrivateWorkingSetSample queryWorkingSetPrivateBytes(HANDLE process) {
    require(process != nullptr, "process handle is unavailable");

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(process, &counters, sizeof(counters)) != FALSE,
            "GetProcessMemoryInfo failed");

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const size_t pageSize = systemInfo.dwPageSize;
    require(pageSize > 0, "Windows reported an invalid page size");

    size_t capacity = std::max<size_t>(4096, counters.WorkingSetSize / pageSize + 4096);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const size_t bytes = sizeof(PSAPI_WORKING_SET_INFORMATION) +
                             (capacity - 1) * sizeof(PSAPI_WORKING_SET_BLOCK);
        std::vector<std::byte> buffer(bytes);
        auto* workingSet = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
        if (QueryWorkingSet(process, workingSet, static_cast<DWORD>(buffer.size())) != FALSE) {
            const size_t count =
                std::min<size_t>(workingSet->NumberOfEntries, static_cast<ULONG_PTR>(capacity));
            quint64 privatePages = 0;
            for (size_t index = 0; index < count; ++index) {
                if (workingSet->WorkingSetInfo[index].Shared == 0) {
                    ++privatePages;
                }
            }
            const quint64 result = privatePages * pageSize;
            require(result <= static_cast<quint64>(std::numeric_limits<qint64>::max()),
                    "private working set exceeds the supported range");
            return PrivateWorkingSetSample{static_cast<qint64>(result),
                                           PrivateWorkingSetMethod::QueryWorkingSet};
        }
        require(GetLastError() == ERROR_BAD_LENGTH, "QueryWorkingSet failed");
        capacity *= 2;
    }
    throw std::runtime_error("QueryWorkingSet buffer did not converge");
}

PrivateWorkingSetSample privateWorkingSet(HANDLE process) {
    require(process != nullptr, "process handle is unavailable");

#if defined(NTDDI_VERSION) && defined(NTDDI_WIN10_CU) && (NTDDI_VERSION >= NTDDI_WIN10_CU)
    // PROCESS_MEMORY_COUNTERS_EX2 is the native private-working-set value on
    // Windows 10 CU and later. Older Windows versions reject this larger
    // structure, so retain the page-enumeration fallback below.
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters),
                             sizeof(counters)) != FALSE &&
        counters.PrivateWorkingSetSize > 0) {
        require(counters.PrivateWorkingSetSize <=
                    static_cast<SIZE_T>(std::numeric_limits<qint64>::max()),
                "private working set exceeds the supported range");
        return PrivateWorkingSetSample{static_cast<qint64>(counters.PrivateWorkingSetSize),
                                       PrivateWorkingSetMethod::ProcessMemoryCountersEx2};
    }
#endif

    return queryWorkingSetPrivateBytes(process);
}

bool stableWindow(const std::deque<qint64>& values, int requiredSamples, qint64 maximumRangeBytes) {
    if (requiredSamples <= 0 || values.size() != static_cast<size_t>(requiredSamples)) {
        return false;
    }
    const auto [minimum, maximum] = std::minmax_element(values.cbegin(), values.cend());
    return *maximum - *minimum < maximumRangeBytes;
}

struct StableMemorySample {
    qint64 bytes = 0;
    qint64 peakBytes = 0;
    qint64 rangeBytes = 0;
    qint64 elapsedMilliseconds = 0;
    PrivateWorkingSetMethod method = PrivateWorkingSetMethod::QueryWorkingSet;
};

StableMemorySample waitForStableMemory(const ChildProcess& process, int minimumWaitMilliseconds,
                                       int pollMilliseconds, int windowSamples,
                                       qint64 maximumRangeBytes, int timeoutMilliseconds) {
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(timeoutMilliseconds);
    std::deque<qint64> window;
    qint64 peakBytes = 0;
    PrivateWorkingSetMethod method = PrivateWorkingSetMethod::QueryWorkingSet;
    while (process.alive() && std::chrono::steady_clock::now() < deadline) {
        const PrivateWorkingSetSample current = privateWorkingSet(process.handle());
        const qint64 currentBytes = current.bytes;
        method = current.method;
        peakBytes = std::max(peakBytes, currentBytes);
        window.push_back(currentBytes);
        while (window.size() > static_cast<size_t>(windowSamples)) {
            window.pop_front();
        }

        const auto now = std::chrono::steady_clock::now();
        const qint64 elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        if (elapsed >= minimumWaitMilliseconds &&
            stableWindow(window, windowSamples, maximumRangeBytes)) {
            const auto [minimum, maximum] = std::minmax_element(window.cbegin(), window.cend());
            return StableMemorySample{currentBytes, peakBytes, *maximum - *minimum, elapsed,
                                      method};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMilliseconds));
    }
    throw std::runtime_error(process.alive() ? "private working set did not stabilize"
                                             : "snow_shot exited while memory was stabilizing");
}

QVector<QJsonObject> readTrace(const QString& path) {
    QFile file(path);
    if (!file.exists()) {
        return {};
    }
    require(file.open(QIODevice::ReadOnly), "could not open lifecycle trace");
    const QByteArray contents = file.readAll();
    QList<QByteArray> lines = contents.split('\n');
    if (!contents.endsWith('\n') && !lines.isEmpty()) {
        lines.removeLast();
    }

    QVector<QJsonObject> records;
    for (const QByteArray& line : std::as_const(lines)) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        require(error.error == QJsonParseError::NoError && document.isObject(),
                "lifecycle trace contains invalid JSON");
        records.push_back(document.object());
    }
    return records;
}

QJsonObject waitForTraceEvent(const QString& path, qint64 processId, const QStringList& events,
                              qsizetype& cursor, const ChildProcess& process,
                              int timeoutMilliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (process.alive() && std::chrono::steady_clock::now() < deadline) {
        const QVector<QJsonObject> records = readTrace(path);
        while (cursor < records.size()) {
            const QJsonObject record = records.at(cursor++);
            if (record.value(QStringLiteral("process_id")).toInteger() == processId &&
                events.contains(record.value(QStringLiteral("event")).toString())) {
                return record;
            }
        }
        std::this_thread::sleep_for(25ms);
    }
    throw std::runtime_error(process.alive() ? "timed out waiting for lifecycle trace event"
                                             : "snow_shot exited before the lifecycle event");
}

QJsonObject statistics(const QVector<double>& source, const QString& unit = QStringLiteral("mib")) {
    if (source.isEmpty()) {
        return {};
    }
    QVector<double> values = source;
    std::sort(values.begin(), values.end());
    const auto percentile = [&values](double probability) {
        const qsizetype index =
            std::min(values.size() - 1,
                     static_cast<qsizetype>(
                         std::ceil(probability * static_cast<double>(values.size())) - 1));
        return values.at(index);
    };
    const double mean =
        std::accumulate(values.cbegin(), values.cend(), 0.0) / static_cast<double>(values.size());
    double variance = 0.0;
    for (double value : std::as_const(values)) {
        variance += (value - mean) * (value - mean);
    }
    const auto key = [&unit](const char* name) {
        return QString::fromLatin1(name) + QLatin1Char('_') + unit;
    };
    return QJsonObject{{QStringLiteral("count"), values.size()},
                       {key("min"), values.first()},
                       {key("mean"), mean},
                       {key("p50"), percentile(0.50)},
                       {key("p90"), percentile(0.90)},
                       {key("p95"), percentile(0.95)},
                       {key("max"), values.last()},
                       {key("stddev"), std::sqrt(variance / static_cast<double>(values.size()))}};
}

QString htmlEscape(QString value) {
    return value.replace(QLatin1Char('&'), QStringLiteral("&amp;"))
        .replace(QLatin1Char('<'), QStringLiteral("&lt;"))
        .replace(QLatin1Char('>'), QStringLiteral("&gt;"))
        .replace(QLatin1Char('"'), QStringLiteral("&quot;"));
}

QString reportHtml(const QJsonObject& report) {
    const QJsonObject metrics = report.value(QStringLiteral("metrics")).toObject();
    const QJsonObject acceptance = report.value(QStringLiteral("acceptance")).toObject();
    const auto row = [&metrics](const QString& key, const QString& label, const QString& unit) {
        const QJsonObject values = metrics.value(key).toObject();
        const auto number = [&values, &unit](const char* field) {
            return QString::number(
                values.value(QString::fromLatin1(field) + QLatin1Char('_') + unit).toDouble(), 'f',
                3);
        };
        return QStringLiteral("<tr><th>%1</th><td>%2</td><td>%3</td><td>%4</td><td>%5</td>"
                              "<td>%6</td><td>%7</td></tr>")
            .arg(htmlEscape(label), number("min"), number("mean"), number("p50"), number("p90"),
                 number("p95"), number("max"));
    };

    const QString rows =
        row(QStringLiteral("cold_start_private_working_set"),
            QStringLiteral("Cold-start private working set (MiB)"), QStringLiteral("mib")) +
        row(QStringLiteral("post_end_private_working_set"),
            QStringLiteral("Post-end private working set (MiB)"), QStringLiteral("mib")) +
        row(QStringLiteral("private_working_set_delta"),
            QStringLiteral("Private working-set delta (MiB)"), QStringLiteral("mib")) +
        row(QStringLiteral("private_working_set_absolute_delta"),
            QStringLiteral("Absolute private working-set delta (MiB)"), QStringLiteral("mib")) +
        row(QStringLiteral("first_screenshot_to_composited_frame"),
            QStringLiteral("First screenshot to complete smart frame (ms)"), QStringLiteral("ms")) +
        row(QStringLiteral("first_capture_pixels_ready"),
            QStringLiteral("First capture pixels ready (ms)"), QStringLiteral("ms")) +
        row(QStringLiteral("first_initial_selection"),
            QStringLiteral("First initial selector lookup (ms)"), QStringLiteral("ms")) +
        row(QStringLiteral("first_initial_selection_overlap"),
            QStringLiteral("First capture/selector overlap (ms)"), QStringLiteral("ms")) +
        row(QStringLiteral("first_post_barrier_presentation"),
            QStringLiteral("First post-barrier presentation (ms)"), QStringLiteral("ms"));
    const QString embeddedJson =
        htmlEscape(QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented)));
    QString acceptanceSummary;
    if (!acceptance.isEmpty()) {
        const bool passed = acceptance.value(QStringLiteral("passed")).toBool();
        acceptanceSummary =
            QStringLiteral("<p><strong>Acceptance:</strong> %1 (%2 failed of %3 "
                           "samples)</p>")
                .arg(passed ? QStringLiteral("passed") : QStringLiteral("failed"))
                .arg(acceptance.value(QStringLiteral("failed_sample_count")).toInt())
                .arg(acceptance.value(QStringLiteral("sample_count")).toInt());
    }
    return QStringLiteral(
               "<!doctype html><html><head><meta charset=utf-8><title>Snow Shot screenshot "
               "lifecycle performance</title><style>body{font:14px "
               "system-ui;margin:32px;color:#202124}"
               "table{border-collapse:collapse;margin:20px 0}th,td{border:1px solid "
               "#dadce0;padding:8px 12px}"
               "th{text-align:left;background:#f8f9fa}pre{background:#f8f9fa;padding:16px;overflow:"
               "auto}"
               "</style></head><body><h1>Screenshot lifecycle performance</h1>"
               "%3<table><thead><tr><th>Metric</th><th>Min</th><th>Mean</th><th>P50</th><th>P90</"
               "th>"
               "<th>P95</th><th>Max</th></tr></thead><tbody>%1</tbody></table><h2>Report JSON</h2>"
               "<pre>%2</pre></body></html>")
        .arg(rows, embeddedJson, acceptanceSummary);
}

QJsonObject environmentReport(const QString& appPath, const QVector<MonitorInfo>& displayList) {
    QJsonArray monitorReports;
    for (const MonitorInfo& monitor : displayList) {
        monitorReports.append(QJsonObject{
            {QStringLiteral("device"), monitor.deviceName},
            {QStringLiteral("x"), static_cast<qint64>(monitor.bounds.left)},
            {QStringLiteral("y"), static_cast<qint64>(monitor.bounds.top)},
            {QStringLiteral("width"),
             static_cast<qint64>(monitor.bounds.right - monitor.bounds.left)},
            {QStringLiteral("height"),
             static_cast<qint64>(monitor.bounds.bottom - monitor.bounds.top)},
        });
    }
    return QJsonObject{
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
        {QStringLiteral("application"), QDir::toNativeSeparators(appPath)},
        {QStringLiteral("git_commit"), qEnvironmentVariable("SNOW_SHOT_PERF_GIT_COMMIT")},
        {QStringLiteral("git_dirty"),
         qEnvironmentVariableIntValue("SNOW_SHOT_PERF_GIT_DIRTY") != 0},
        {QStringLiteral("monitors"), monitorReports},
    };
}

struct BenchmarkConfiguration {
    QString appPath;
    QString outputDirectory;
    int samples = 10;
    int screenIndex = 0;
    int pollMilliseconds = 250;
    int baselineMinimumWaitMilliseconds = 20000;
    int interCaptureWaitMilliseconds = 0;
    int postEndMinimumWaitMilliseconds = 15000;
    int stabilityWindowSamples = 20;
    qint64 stabilityRangeBytes = 1024 * 1024;
    int timeoutMilliseconds = 90000;
    qint64 maximumFirstScreenshotMilliseconds = kDefaultMaximumFirstScreenshotMilliseconds;
    bool maximumFirstScreenshotExplicit = false;
    qint64 maximumToolbarShowMilliseconds = kDefaultMaximumToolbarShowMilliseconds;
    qint64 maximumPrivateWorkingSetMebibytes = kDefaultMaximumPrivateWorkingSetMebibytes;
    qint64 maximumPrivateWorkingSetDeltaMebibytes = kDefaultMaximumPrivateWorkingSetDeltaMebibytes;
};

BenchmarkConfiguration configurationFromParser(const QCommandLineParser& parser) {
    BenchmarkConfiguration configuration;
    configuration.appPath = QFileInfo(parser.value(QStringLiteral("app"))).absoluteFilePath();
    configuration.outputDirectory = QDir::cleanPath(parser.value(QStringLiteral("output")));
    configuration.samples = parser.value(QStringLiteral("samples")).toInt();
    configuration.screenIndex = parser.value(QStringLiteral("screen-index")).toInt();
    configuration.pollMilliseconds = parser.value(QStringLiteral("poll-ms")).toInt();
    configuration.baselineMinimumWaitMilliseconds =
        parser.value(QStringLiteral("baseline-min-wait-ms")).toInt();
    configuration.interCaptureWaitMilliseconds =
        parser.value(QStringLiteral("inter-capture-wait-ms")).toInt();
    configuration.postEndMinimumWaitMilliseconds =
        parser.value(QStringLiteral("post-end-min-wait-ms")).toInt();
    configuration.stabilityWindowSamples = parser.value(QStringLiteral("stability-window")).toInt();
    configuration.stabilityRangeBytes =
        parser.value(QStringLiteral("stability-range-kib")).toLongLong() * kBytesPerKibibyte;
    configuration.timeoutMilliseconds = parser.value(QStringLiteral("timeout-ms")).toInt();
    configuration.maximumFirstScreenshotMilliseconds =
        parser.value(QStringLiteral("max-first-screenshot-ms")).toLongLong();
    configuration.maximumFirstScreenshotExplicit =
        parser.isSet(QStringLiteral("max-first-screenshot-ms"));
    configuration.maximumToolbarShowMilliseconds =
        parser.value(QStringLiteral("max-toolbar-show-ms")).toLongLong();
    configuration.maximumPrivateWorkingSetMebibytes =
        parser.value(QStringLiteral("max-private-working-set-mib")).toLongLong();
    configuration.maximumPrivateWorkingSetDeltaMebibytes =
        parser.value(QStringLiteral("max-private-working-set-delta-mib")).toLongLong();
    return configuration;
}

void validateConfiguration(const BenchmarkConfiguration& configuration,
                           const QVector<MonitorInfo>& displayList) {
    require(QFileInfo(configuration.appPath).isFile(), "snow_shot executable was not found");
    require(configuration.samples > 0, "samples must be positive");
    require(configuration.screenIndex >= 0 && configuration.screenIndex < displayList.size(),
            "screen index is unavailable");
    require(configuration.pollMilliseconds > 0, "poll interval must be positive");
    require(configuration.baselineMinimumWaitMilliseconds >= 0,
            "baseline minimum wait must be nonnegative");
    require(configuration.interCaptureWaitMilliseconds >= 0,
            "inter-capture wait must be nonnegative");
    require(configuration.postEndMinimumWaitMilliseconds >= 0,
            "post-end minimum wait must be nonnegative");
    require(configuration.stabilityWindowSamples > 1,
            "stability window must contain at least two samples");
    require(configuration.stabilityRangeBytes > 0, "stability range must be positive");
    require(configuration.timeoutMilliseconds > configuration.baselineMinimumWaitMilliseconds &&
                configuration.timeoutMilliseconds > configuration.postEndMinimumWaitMilliseconds,
            "timeout must exceed both minimum memory waits");
    require(configuration.maximumFirstScreenshotMilliseconds > 0,
            "maximum first screenshot latency must be positive");
    require(configuration.maximumToolbarShowMilliseconds > 0,
            "maximum toolbar-show latency must be positive");
    require(configuration.maximumPrivateWorkingSetMebibytes > 0,
            "maximum private working set must be positive");
    require(configuration.maximumPrivateWorkingSetDeltaMebibytes >= 0,
            "maximum private working set delta must be nonnegative");
}

struct CaptureMeasurement {
    qint64 presentationNanoseconds = 0;
    qint64 capturePixelsNanoseconds = 0;
    qint64 initialSelectionNanoseconds = 0;
    qint64 initialSelectionOverlapNanoseconds = 0;
    qint64 postBarrierPresentationNanoseconds = 0;
    qint64 interactionReadyNanoseconds = 0;
    qint64 toolbarShowNanoseconds = 0;
    qint64 selectionToToolbarNanoseconds = 0;
};

// Captures are fanned out to one worker per display, but each worker still has
// to read and marshal a complete native frame. Preserve the 100 ms budget for
// the original single-monitor benchmark and add a bounded allowance for each
// additional display. An explicit CLI value always wins.
void applyTopologyAwareDefaults(BenchmarkConfiguration& configuration,
                                qsizetype monitorCount) {
    if (configuration.maximumFirstScreenshotExplicit || monitorCount <= 1) {
        return;
    }
    configuration.maximumFirstScreenshotMilliseconds =
        kDefaultMaximumFirstScreenshotMilliseconds +
        (monitorCount - 1) * kAdditionalMonitorScreenshotBudgetMilliseconds;
}

qint64 tracePhaseDuration(const QString& tracePath, qint64 processId, qint64 firstSequence,
                          qint64 lastSequence, const QString& beginEvent,
                          const QString& completeEvent) {
    qint64 beginNanoseconds = -1;
    qint64 completeNanoseconds = -1;
    for (const QJsonObject& record : readTrace(tracePath)) {
        const qint64 sequence = record.value(QStringLiteral("event_sequence")).toInteger(-1);
        if (record.value(QStringLiteral("process_id")).toInteger() != processId ||
            sequence <= firstSequence || sequence > lastSequence) {
            continue;
        }
        const QString event = record.value(QStringLiteral("event")).toString();
        if (event == beginEvent) {
            beginNanoseconds = record.value(QStringLiteral("elapsed_ns")).toInteger(-1);
        } else if (event == completeEvent) {
            completeNanoseconds = record.value(QStringLiteral("elapsed_ns")).toInteger(-1);
        }
    }
    require(beginNanoseconds >= 0 && completeNanoseconds >= beginNanoseconds,
            "capture trace did not contain the requested complete phase");
    return completeNanoseconds - beginNanoseconds;
}

qint64 traceEventElapsed(const QString& tracePath, qint64 processId, qint64 firstSequence,
                         qint64 lastSequence, const QString& eventName) {
    for (const QJsonObject& record : readTrace(tracePath)) {
        const qint64 sequence = record.value(QStringLiteral("event_sequence")).toInteger(-1);
        if (record.value(QStringLiteral("process_id")).toInteger() == processId &&
            sequence > firstSequence && sequence <= lastSequence &&
            record.value(QStringLiteral("event")).toString() == eventName) {
            return record.value(QStringLiteral("elapsed_ns")).toInteger();
        }
    }
    return 0;
}

qint64 traceLastEventElapsed(const QString& tracePath, qint64 processId, qint64 firstSequence,
                             qint64 lastSequence, const QString& eventName) {
    qint64 elapsedNanoseconds = 0;
    for (const QJsonObject& record : readTrace(tracePath)) {
        const qint64 sequence = record.value(QStringLiteral("event_sequence")).toInteger(-1);
        if (record.value(QStringLiteral("process_id")).toInteger() == processId &&
            sequence > firstSequence && sequence <= lastSequence &&
            record.value(QStringLiteral("event")).toString() == eventName) {
            elapsedNanoseconds = record.value(QStringLiteral("elapsed_ns")).toInteger();
        }
    }
    return elapsedNanoseconds;
}

CaptureMeasurement runCapture(const BenchmarkConfiguration& configuration,
                              const MonitorInfo& monitor, IUIAutomation& automation,
                              const QStringList& baseArguments, const QString& tracePath,
                              qsizetype& traceCursor, const ChildProcess& primary) {
    positionCursorForCapture(monitor.bounds);

    ChildProcess request;
    QStringList requestArguments = baseArguments;
    requestArguments.push_back(QStringLiteral("--e2e-start-screenshot"));
    require(request.start(configuration.appPath, requestArguments),
            "could not issue screenshot command");
    require(request.waitForExit(10000), "screenshot command helper did not exit");
    require(request.exitCode() == 0, "screenshot command helper failed");
    const QJsonObject accepted = waitForTraceEvent(
        tracePath, primary.processId(), {QStringLiteral("capture_command_accepted")}, traceCursor,
        primary, configuration.timeoutMilliseconds);
    const QJsonObject presentation = waitForTraceEvent(
        tracePath, primary.processId(),
        {QStringLiteral("first_capture_presented"), QStringLiteral("capture_released")},
        traceCursor, primary, configuration.timeoutMilliseconds);
    require(presentation.value(QStringLiteral("event")).toString() ==
                QStringLiteral("first_capture_presented"),
            "screenshot ended before its first frame was presented");

    CaptureMeasurement measurement;
    measurement.presentationNanoseconds =
        presentation.value(QStringLiteral("elapsed_ns")).toInteger();
    require(measurement.presentationNanoseconds > 0,
            "screenshot presentation duration was not recorded");

    ComPtr<IUIAutomationElement> initialSelectionToolbar = waitForVisibleElement(
        automation, primary.processId(), QStringLiteral("screenshotSelectionToolbarPanel"), primary,
        configuration.timeoutMilliseconds);
    require(initialSelectionToolbar.get() != nullptr,
            "selection toolbar did not appear with the first smart-selection frame");

    // Foreground negotiation can be delayed by unrelated windows on a desktop
    // running the benchmark. Mouse selection activates the overlay itself, so
    // keep keyboard-focus readiness as a reported metric rather than a gate.
    std::this_thread::sleep_for(250ms);
    const auto selectionStarted = std::chrono::steady_clock::now();
    dragSelection(monitor.bounds);
    ComPtr<IUIAutomationElement> cancelButton = waitForVisibleElement(
        automation, primary.processId(), QStringLiteral("screenshotCancelButton"), primary,
        configuration.timeoutMilliseconds);
    measurement.selectionToToolbarNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             selectionStarted)
            .count();
    require(cancelButton.get() != nullptr, "screenshot cancel button did not appear");
    clickElement(*cancelButton.get());

    const QJsonObject released =
        waitForTraceEvent(tracePath, primary.processId(), {QStringLiteral("capture_released")},
                          traceCursor, primary, configuration.timeoutMilliseconds);
    require(released.value(QStringLiteral("capture_presented")).toBool(),
            "capture release did not follow a presented screenshot");
    measurement.toolbarShowNanoseconds =
        tracePhaseDuration(tracePath, primary.processId(),
                           accepted.value(QStringLiteral("event_sequence")).toInteger(),
                           released.value(QStringLiteral("event_sequence")).toInteger(),
                           QStringLiteral("presentation.toolbar_show_begin"),
                           QStringLiteral("presentation.toolbar_show_complete"));
    measurement.interactionReadyNanoseconds =
        traceEventElapsed(tracePath, primary.processId(),
                          accepted.value(QStringLiteral("event_sequence")).toInteger(),
                          released.value(QStringLiteral("event_sequence")).toInteger(),
                          QStringLiteral("capture_interaction_ready"));
    measurement.capturePixelsNanoseconds =
        traceEventElapsed(tracePath, primary.processId(),
                          accepted.value(QStringLiteral("event_sequence")).toInteger(),
                          released.value(QStringLiteral("event_sequence")).toInteger(),
                          QStringLiteral("presentation.capture_pixels_ready"));
    const qint64 initialSelectionBeginNanoseconds =
        traceEventElapsed(tracePath, primary.processId(),
                          accepted.value(QStringLiteral("event_sequence")).toInteger(),
                          released.value(QStringLiteral("event_sequence")).toInteger(),
                          QStringLiteral("presentation.initial_selection_begin"));
    const qint64 initialSelectionResolvedNanoseconds =
        traceLastEventElapsed(tracePath, primary.processId(),
                              accepted.value(QStringLiteral("event_sequence")).toInteger(),
                              released.value(QStringLiteral("event_sequence")).toInteger(),
                              QStringLiteral("presentation.initial_selection_resolved"));
    const qint64 completeSmartFrameNanoseconds =
        traceEventElapsed(tracePath, primary.processId(),
                          accepted.value(QStringLiteral("event_sequence")).toInteger(),
                          released.value(QStringLiteral("event_sequence")).toInteger(),
                          QStringLiteral("presentation.first_complete_smart_frame_presented"));
    require(measurement.capturePixelsNanoseconds > 0 && initialSelectionBeginNanoseconds > 0 &&
                initialSelectionResolvedNanoseconds >= initialSelectionBeginNanoseconds &&
                completeSmartFrameNanoseconds >= measurement.capturePixelsNanoseconds &&
                completeSmartFrameNanoseconds >= initialSelectionResolvedNanoseconds,
            "complete smart-frame trace milestones were not recorded in lifecycle order");
    measurement.presentationNanoseconds = completeSmartFrameNanoseconds;
    measurement.initialSelectionNanoseconds =
        initialSelectionResolvedNanoseconds - initialSelectionBeginNanoseconds;
    measurement.initialSelectionOverlapNanoseconds = std::max<qint64>(
        0, std::min(measurement.capturePixelsNanoseconds, initialSelectionResolvedNanoseconds) -
               initialSelectionBeginNanoseconds);
    measurement.postBarrierPresentationNanoseconds =
        completeSmartFrameNanoseconds -
        std::max(measurement.capturePixelsNanoseconds, initialSelectionResolvedNanoseconds);
    return measurement;
}

QJsonObject runSample(const BenchmarkConfiguration& configuration, const MonitorInfo& monitor,
                      int iteration, IUIAutomation& automation) {
    const QString traceDirectory =
        QDir(configuration.outputDirectory).filePath(QStringLiteral("app-traces"));
    require(QDir().mkpath(traceDirectory), "could not create app trace directory");
    const QString tracePath =
        QDir(traceDirectory)
            .filePath(QStringLiteral("sample-%1.jsonl").arg(iteration, 3, 10, QLatin1Char('0')));
    static_cast<void>(QFile::remove(tracePath));
    const ScopedEnvironmentVariable traceEnvironment(
        QByteArrayLiteral("SNOW_SHOT_SCREENSHOT_LIFECYCLE_PERF_TRACE"),
        QFile::encodeName(tracePath));

    const QString instanceId = QStringLiteral("lifecycle-%1-%2-%3")
                                   .arg(GetCurrentProcessId())
                                   .arg(iteration)
                                   .arg(QDateTime::currentMSecsSinceEpoch() % 1000000);
    const QStringList baseArguments{QStringLiteral("--e2e-allow-overlay-capture"),
                                    QStringLiteral("--e2e-instance-id=%1").arg(instanceId)};

    ChildProcess primary;
    require(primary.start(configuration.appPath, baseArguments), "could not start snow_shot");
    qsizetype traceCursor = 0;
    static_cast<void>(waitForTraceEvent(tracePath, primary.processId(),
                                        {QStringLiteral("app_ready")}, traceCursor, primary,
                                        configuration.timeoutMilliseconds));
    const StableMemorySample baseline =
        waitForStableMemory(primary, configuration.baselineMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);

    const CaptureMeasurement firstCapture = runCapture(
        configuration, monitor, automation, baseArguments, tracePath, traceCursor, primary);
    if (configuration.interCaptureWaitMilliseconds > 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(configuration.interCaptureWaitMilliseconds));
    }
    const CaptureMeasurement subsequentCapture = runCapture(
        configuration, monitor, automation, baseArguments, tracePath, traceCursor, primary);
    const StableMemorySample postEnd =
        waitForStableMemory(primary, configuration.postEndMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);

    const qint64 deltaBytes = postEnd.bytes - baseline.bytes;
    const qint64 absoluteDeltaBytes = deltaBytes >= 0 ? deltaBytes : -deltaBytes;
    const qint64 maximumFirstScreenshotNanoseconds =
        configuration.maximumFirstScreenshotMilliseconds * 1000 * 1000;
    const qint64 maximumToolbarShowNanoseconds =
        configuration.maximumToolbarShowMilliseconds * 1000 * 1000;
    const qint64 maximumPrivateWorkingSetBytes =
        configuration.maximumPrivateWorkingSetMebibytes * kBytesPerMebibyte;
    const qint64 maximumPrivateWorkingSetDeltaBytes =
        configuration.maximumPrivateWorkingSetDeltaMebibytes * kBytesPerMebibyte;
    QJsonArray acceptanceFailures;
    if (firstCapture.presentationNanoseconds > maximumFirstScreenshotNanoseconds) {
        acceptanceFailures.append(QStringLiteral("first screenshot latency exceeds %1 ms")
                                      .arg(configuration.maximumFirstScreenshotMilliseconds));
    }
    if (subsequentCapture.presentationNanoseconds > maximumFirstScreenshotNanoseconds) {
        acceptanceFailures.append(QStringLiteral("subsequent screenshot latency exceeds %1 ms")
                                      .arg(configuration.maximumFirstScreenshotMilliseconds));
    }
    if (firstCapture.toolbarShowNanoseconds > maximumToolbarShowNanoseconds) {
        acceptanceFailures.append(QStringLiteral("first toolbar-show latency exceeds %1 ms")
                                      .arg(configuration.maximumToolbarShowMilliseconds));
    }
    if (subsequentCapture.toolbarShowNanoseconds > maximumToolbarShowNanoseconds) {
        acceptanceFailures.append(QStringLiteral("subsequent toolbar-show latency exceeds %1 ms")
                                      .arg(configuration.maximumToolbarShowMilliseconds));
    }
    if (baseline.bytes > maximumPrivateWorkingSetBytes) {
        acceptanceFailures.append(QStringLiteral("cold-start private working set exceeds %1 MiB")
                                      .arg(configuration.maximumPrivateWorkingSetMebibytes));
    }
    if (postEnd.bytes > maximumPrivateWorkingSetBytes) {
        acceptanceFailures.append(QStringLiteral("post-end private working set exceeds %1 MiB")
                                      .arg(configuration.maximumPrivateWorkingSetMebibytes));
    }
    if (absoluteDeltaBytes > maximumPrivateWorkingSetDeltaBytes) {
        acceptanceFailures.append(
            QStringLiteral("absolute private working-set delta exceeds %1 MiB")
                .arg(configuration.maximumPrivateWorkingSetDeltaMebibytes));
    }
    const bool acceptancePassed = acceptanceFailures.isEmpty();
    QJsonObject record{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("iteration"), iteration},
        {QStringLiteral("process_id"), static_cast<qint64>(primary.processId())},
        {QStringLiteral("cold_start_private_working_set_bytes"), baseline.bytes},
        {QStringLiteral("post_end_private_working_set_bytes"), postEnd.bytes},
        {QStringLiteral("private_working_set_delta_bytes"), deltaBytes},
        {QStringLiteral("private_working_set_absolute_delta_bytes"), absoluteDeltaBytes},
        {QStringLiteral("first_screenshot_elapsed_ns"), firstCapture.presentationNanoseconds},
        {QStringLiteral("first_capture_pixels_ready_ns"), firstCapture.capturePixelsNanoseconds},
        {QStringLiteral("first_initial_selection_ns"), firstCapture.initialSelectionNanoseconds},
        {QStringLiteral("first_initial_selection_overlap_ns"),
         firstCapture.initialSelectionOverlapNanoseconds},
        {QStringLiteral("first_post_barrier_presentation_ns"),
         firstCapture.postBarrierPresentationNanoseconds},
        {QStringLiteral("first_screenshot_interaction_ready_ns"),
         firstCapture.interactionReadyNanoseconds},
        {QStringLiteral("first_selection_to_toolbar_ns"),
         firstCapture.selectionToToolbarNanoseconds},
        {QStringLiteral("first_toolbar_show_ns"), firstCapture.toolbarShowNanoseconds},
        {QStringLiteral("subsequent_screenshot_elapsed_ns"),
         subsequentCapture.presentationNanoseconds},
        {QStringLiteral("subsequent_capture_pixels_ready_ns"),
         subsequentCapture.capturePixelsNanoseconds},
        {QStringLiteral("subsequent_initial_selection_ns"),
         subsequentCapture.initialSelectionNanoseconds},
        {QStringLiteral("subsequent_initial_selection_overlap_ns"),
         subsequentCapture.initialSelectionOverlapNanoseconds},
        {QStringLiteral("subsequent_post_barrier_presentation_ns"),
         subsequentCapture.postBarrierPresentationNanoseconds},
        {QStringLiteral("subsequent_screenshot_interaction_ready_ns"),
         subsequentCapture.interactionReadyNanoseconds},
        {QStringLiteral("subsequent_selection_to_toolbar_ns"),
         subsequentCapture.selectionToToolbarNanoseconds},
        {QStringLiteral("subsequent_toolbar_show_ns"), subsequentCapture.toolbarShowNanoseconds},
        {QStringLiteral("private_working_set_method"),
         QString::fromLatin1(privateWorkingSetMethodName(baseline.method))},
        {QStringLiteral("cold_start_private_working_set_method"),
         QString::fromLatin1(privateWorkingSetMethodName(baseline.method))},
        {QStringLiteral("post_end_private_working_set_method"),
         QString::fromLatin1(privateWorkingSetMethodName(postEnd.method))},
        {QStringLiteral("cold_start_peak_private_working_set_bytes"), baseline.peakBytes},
        {QStringLiteral("post_end_peak_private_working_set_bytes"), postEnd.peakBytes},
        {QStringLiteral("cold_start_stability_range_bytes"), baseline.rangeBytes},
        {QStringLiteral("post_end_stability_range_bytes"), postEnd.rangeBytes},
        {QStringLiteral("cold_start_convergence_ms"), baseline.elapsedMilliseconds},
        {QStringLiteral("post_end_convergence_ms"), postEnd.elapsedMilliseconds},
        {QStringLiteral("acceptance_passed"), acceptancePassed},
        {QStringLiteral("acceptance_failure_reasons"), acceptanceFailures},
        {QStringLiteral("acceptance_thresholds"),
         QJsonObject{
             {QStringLiteral("max_first_screenshot_ms"),
              configuration.maximumFirstScreenshotMilliseconds},
             {QStringLiteral("max_toolbar_show_ms"), configuration.maximumToolbarShowMilliseconds},
             {QStringLiteral("max_private_working_set_mib"),
              configuration.maximumPrivateWorkingSetMebibytes},
             {QStringLiteral("max_private_working_set_delta_mib"),
              configuration.maximumPrivateWorkingSetDeltaMebibytes},
         }},
        {QStringLiteral("monitor"),
         QJsonObject{{QStringLiteral("device"), monitor.deviceName},
                     {QStringLiteral("x"), static_cast<qint64>(monitor.bounds.left)},
                     {QStringLiteral("y"), static_cast<qint64>(monitor.bounds.top)},
                     {QStringLiteral("width"),
                      static_cast<qint64>(monitor.bounds.right - monitor.bounds.left)},
                     {QStringLiteral("height"),
                      static_cast<qint64>(monitor.bounds.bottom - monitor.bounds.top)}}},
        {QStringLiteral("trace"), QDir::toNativeSeparators(tracePath)},
    };

    std::cout << "sample " << iteration
              << ": cold=" << static_cast<double>(baseline.bytes) / kBytesPerMebibyte
              << " MiB, post-end=" << static_cast<double>(postEnd.bytes) / kBytesPerMebibyte
              << " MiB, delta=" << static_cast<double>(deltaBytes) / kBytesPerMebibyte
              << " MiB, first-frame="
              << static_cast<double>(firstCapture.presentationNanoseconds) / 1e6
              << " ms (capture=" << static_cast<double>(firstCapture.capturePixelsNanoseconds) / 1e6
              << " ms, selector="
              << static_cast<double>(firstCapture.initialSelectionNanoseconds) / 1e6
              << " ms, overlap="
              << static_cast<double>(firstCapture.initialSelectionOverlapNanoseconds) / 1e6
              << " ms), subsequent-frame="
              << static_cast<double>(subsequentCapture.presentationNanoseconds) / 1e6
              << " ms, first-toolbar="
              << static_cast<double>(firstCapture.toolbarShowNanoseconds) / 1e6
              << " ms, subsequent-toolbar="
              << static_cast<double>(subsequentCapture.toolbarShowNanoseconds) / 1e6 << " ms\n";
    if (!acceptancePassed) {
        QStringList failureReasonText;
        for (const QJsonValue& failure : std::as_const(acceptanceFailures)) {
            failureReasonText.push_back(failure.toString());
        }
        std::cout << "sample " << iteration << " acceptance failure: "
                  << failureReasonText.join(QStringLiteral("; ")).toStdString() << '\n';
    }
    primary.stop();
    return record;
}

int runBenchmark(BenchmarkConfiguration configuration) {
    const QVector<MonitorInfo> displayList = monitors();
    applyTopologyAwareDefaults(configuration, displayList.size());
    validateConfiguration(configuration, displayList);
    require(QDir().mkpath(configuration.outputDirectory), "could not create output directory");

    ScopedCom com;
    require(SUCCEEDED(com.result()), "COM initialization failed");
    ComPtr<IUIAutomation> automation = createAutomation();
    CursorRestore restoreCursor;

    QFile rawFile(QDir(configuration.outputDirectory).filePath(QStringLiteral("raw.jsonl")));
    require(rawFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "could not create raw.jsonl");

    QVector<double> baselineValues;
    QVector<double> postEndValues;
    QVector<double> deltaValues;
    QVector<double> durationValues;
    QVector<double> subsequentDurationValues;
    QVector<double> firstCapturePixelsValues;
    QVector<double> subsequentCapturePixelsValues;
    QVector<double> firstInitialSelectionValues;
    QVector<double> subsequentInitialSelectionValues;
    QVector<double> firstInitialSelectionOverlapValues;
    QVector<double> subsequentInitialSelectionOverlapValues;
    QVector<double> firstPostBarrierPresentationValues;
    QVector<double> subsequentPostBarrierPresentationValues;
    QVector<double> firstToolbarShowValues;
    QVector<double> subsequentToolbarShowValues;
    QVector<double> absoluteDeltaValues;
    QJsonArray failedSamples;
    bool allSamplesAccepted = true;
    for (int iteration = 1; iteration <= configuration.samples; ++iteration) {
        QJsonObject record;
        try {
            record = runSample(configuration, displayList.at(configuration.screenIndex), iteration,
                               *automation.get());
        } catch (const std::exception& error) {
            const QString reason =
                QStringLiteral("benchmark error: %1").arg(QString::fromLocal8Bit(error.what()));
            const QJsonArray reasons{reason};
            record = QJsonObject{
                {QStringLiteral("schema_version"), 1},
                {QStringLiteral("iteration"), iteration},
                {QStringLiteral("acceptance_passed"), false},
                {QStringLiteral("acceptance_failure_reasons"), reasons},
                {QStringLiteral("error"), reason},
            };
            std::cerr << "sample " << iteration << " failed: " << error.what() << '\n';
        } catch (...) {
            const QString reason = QStringLiteral("benchmark error: unknown exception");
            const QJsonArray reasons{reason};
            record = QJsonObject{
                {QStringLiteral("schema_version"), 1},
                {QStringLiteral("iteration"), iteration},
                {QStringLiteral("acceptance_passed"), false},
                {QStringLiteral("acceptance_failure_reasons"), reasons},
                {QStringLiteral("error"), reason},
            };
            std::cerr << "sample " << iteration << " failed: unknown exception\n";
        }
        rawFile.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
        rawFile.write("\n");
        rawFile.flush();
        if (record.contains(QStringLiteral("cold_start_private_working_set_bytes"))) {
            baselineValues.push_back(
                static_cast<double>(
                    record.value(QStringLiteral("cold_start_private_working_set_bytes"))
                        .toInteger()) /
                kBytesPerMebibyte);
        }
        if (record.contains(QStringLiteral("post_end_private_working_set_bytes"))) {
            postEndValues.push_back(
                static_cast<double>(
                    record.value(QStringLiteral("post_end_private_working_set_bytes"))
                        .toInteger()) /
                kBytesPerMebibyte);
        }
        if (record.contains(QStringLiteral("private_working_set_delta_bytes"))) {
            deltaValues.push_back(
                static_cast<double>(
                    record.value(QStringLiteral("private_working_set_delta_bytes")).toInteger()) /
                kBytesPerMebibyte);
        }
        if (record.contains(QStringLiteral("private_working_set_absolute_delta_bytes"))) {
            absoluteDeltaValues.push_back(
                static_cast<double>(
                    record.value(QStringLiteral("private_working_set_absolute_delta_bytes"))
                        .toInteger()) /
                kBytesPerMebibyte);
        }
        if (record.contains(QStringLiteral("first_screenshot_elapsed_ns"))) {
            durationValues.push_back(
                static_cast<double>(
                    record.value(QStringLiteral("first_screenshot_elapsed_ns")).toInteger()) /
                1e6);
        }
        if (record.contains(QStringLiteral("subsequent_screenshot_elapsed_ns"))) {
            subsequentDurationValues.push_back(
                static_cast<double>(
                    record.value(QStringLiteral("subsequent_screenshot_elapsed_ns")).toInteger()) /
                1e6);
        }
        const auto appendMilliseconds = [&record](const char* key, QVector<double>& values) {
            const QString field = QString::fromLatin1(key);
            if (record.contains(field)) {
                values.push_back(static_cast<double>(record.value(field).toInteger()) / 1e6);
            }
        };
        appendMilliseconds("first_capture_pixels_ready_ns", firstCapturePixelsValues);
        appendMilliseconds("subsequent_capture_pixels_ready_ns", subsequentCapturePixelsValues);
        appendMilliseconds("first_initial_selection_ns", firstInitialSelectionValues);
        appendMilliseconds("subsequent_initial_selection_ns", subsequentInitialSelectionValues);
        appendMilliseconds("first_initial_selection_overlap_ns",
                           firstInitialSelectionOverlapValues);
        appendMilliseconds("subsequent_initial_selection_overlap_ns",
                           subsequentInitialSelectionOverlapValues);
        appendMilliseconds("first_post_barrier_presentation_ns",
                           firstPostBarrierPresentationValues);
        appendMilliseconds("subsequent_post_barrier_presentation_ns",
                           subsequentPostBarrierPresentationValues);
        if (record.contains(QStringLiteral("first_toolbar_show_ns"))) {
            firstToolbarShowValues.push_back(
                static_cast<double>(
                    record.value(QStringLiteral("first_toolbar_show_ns")).toInteger()) /
                1e6);
        }
        if (record.contains(QStringLiteral("subsequent_toolbar_show_ns"))) {
            subsequentToolbarShowValues.push_back(
                static_cast<double>(
                    record.value(QStringLiteral("subsequent_toolbar_show_ns")).toInteger()) /
                1e6);
        }
        if (!record.value(QStringLiteral("acceptance_passed")).toBool()) {
            allSamplesAccepted = false;
            failedSamples.append(QJsonObject{
                {QStringLiteral("iteration"), iteration},
                {QStringLiteral("reasons"),
                 record.value(QStringLiteral("acceptance_failure_reasons")).toArray()},
                {QStringLiteral("trace"), record.value(QStringLiteral("trace"))},
            });
        }
    }
    rawFile.close();

    const QJsonObject metrics{
        {QStringLiteral("cold_start_private_working_set"), statistics(baselineValues)},
        {QStringLiteral("post_end_private_working_set"), statistics(postEndValues)},
        {QStringLiteral("private_working_set_delta"), statistics(deltaValues)},
        {QStringLiteral("private_working_set_absolute_delta"), statistics(absoluteDeltaValues)},
        {QStringLiteral("first_screenshot_to_composited_frame"),
         statistics(durationValues, QStringLiteral("ms"))},
        {QStringLiteral("subsequent_screenshot_to_composited_frame"),
         statistics(subsequentDurationValues, QStringLiteral("ms"))},
        {QStringLiteral("first_capture_pixels_ready"),
         statistics(firstCapturePixelsValues, QStringLiteral("ms"))},
        {QStringLiteral("subsequent_capture_pixels_ready"),
         statistics(subsequentCapturePixelsValues, QStringLiteral("ms"))},
        {QStringLiteral("first_initial_selection"),
         statistics(firstInitialSelectionValues, QStringLiteral("ms"))},
        {QStringLiteral("subsequent_initial_selection"),
         statistics(subsequentInitialSelectionValues, QStringLiteral("ms"))},
        {QStringLiteral("first_initial_selection_overlap"),
         statistics(firstInitialSelectionOverlapValues, QStringLiteral("ms"))},
        {QStringLiteral("subsequent_initial_selection_overlap"),
         statistics(subsequentInitialSelectionOverlapValues, QStringLiteral("ms"))},
        {QStringLiteral("first_post_barrier_presentation"),
         statistics(firstPostBarrierPresentationValues, QStringLiteral("ms"))},
        {QStringLiteral("subsequent_post_barrier_presentation"),
         statistics(subsequentPostBarrierPresentationValues, QStringLiteral("ms"))},
        {QStringLiteral("first_toolbar_show"),
         statistics(firstToolbarShowValues, QStringLiteral("ms"))},
        {QStringLiteral("subsequent_toolbar_show"),
         statistics(subsequentToolbarShowValues, QStringLiteral("ms"))},
    };
    const QJsonObject report{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("benchmark"), QStringLiteral("screenshot_lifecycle")},
        {QStringLiteral("generated_utc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("configuration"),
         QJsonObject{
             {QStringLiteral("samples"), configuration.samples},
             {QStringLiteral("screen_index"), configuration.screenIndex},
             {QStringLiteral("poll_ms"), configuration.pollMilliseconds},
             {QStringLiteral("baseline_min_wait_ms"),
              configuration.baselineMinimumWaitMilliseconds},
             {QStringLiteral("inter_capture_wait_ms"), configuration.interCaptureWaitMilliseconds},
             {QStringLiteral("post_end_min_wait_ms"), configuration.postEndMinimumWaitMilliseconds},
             {QStringLiteral("stability_window_samples"), configuration.stabilityWindowSamples},
             {QStringLiteral("stability_range_kib"),
              configuration.stabilityRangeBytes / kBytesPerKibibyte},
             {QStringLiteral("timeout_ms"), configuration.timeoutMilliseconds},
             {QStringLiteral("max_first_screenshot_ms"),
              configuration.maximumFirstScreenshotMilliseconds},
             {QStringLiteral("max_toolbar_show_ms"), configuration.maximumToolbarShowMilliseconds},
             {QStringLiteral("max_private_working_set_mib"),
              configuration.maximumPrivateWorkingSetMebibytes},
             {QStringLiteral("max_private_working_set_delta_mib"),
              configuration.maximumPrivateWorkingSetDeltaMebibytes},
         }},
        {QStringLiteral("metrics"), metrics},
        {QStringLiteral("acceptance"),
         QJsonObject{
             {QStringLiteral("passed"), allSamplesAccepted},
             {QStringLiteral("sample_count"), configuration.samples},
             {QStringLiteral("failed_sample_count"), failedSamples.size()},
             {QStringLiteral("failed_samples"), failedSamples},
             {QStringLiteral("max_first_screenshot_ms"),
              configuration.maximumFirstScreenshotMilliseconds},
             {QStringLiteral("max_toolbar_show_ms"), configuration.maximumToolbarShowMilliseconds},
             {QStringLiteral("max_private_working_set_mib"),
              configuration.maximumPrivateWorkingSetMebibytes},
             {QStringLiteral("max_private_working_set_delta_mib"),
              configuration.maximumPrivateWorkingSetDeltaMebibytes},
         }},
        {QStringLiteral("environment"), environmentReport(configuration.appPath, displayList)},
    };

    QFile reportFile(QDir(configuration.outputDirectory).filePath(QStringLiteral("report.json")));
    require(reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "could not create report.json");
    reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    reportFile.close();

    QFile htmlFile(QDir(configuration.outputDirectory).filePath(QStringLiteral("report.html")));
    require(htmlFile.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "could not create report.html");
    htmlFile.write(reportHtml(report).toUtf8());
    htmlFile.close();
    return allSamplesAccepted ? 0 : 2;
}

bool runSelfTest() {
    const QVector<double> values{1.0, 2.0, 3.0, 4.0, 5.0};
    const QJsonObject summary = statistics(values, QStringLiteral("ms"));
    require(summary.value(QStringLiteral("p50_ms")).toDouble() == 3.0,
            "statistics median self-test failed");
    require(summary.value(QStringLiteral("p90_ms")).toDouble() == 5.0,
            "statistics percentile self-test failed");

    const std::deque<qint64> stable{100, 110, 105, 107};
    const std::deque<qint64> unstable{100, 1100, 105, 107};
    require(stableWindow(stable, 4, 20), "stable-window acceptance self-test failed");
    require(!stableWindow(unstable, 4, 20), "stable-window rejection self-test failed");

    BenchmarkConfiguration topologyDefaults;
    applyTopologyAwareDefaults(topologyDefaults, 1);
    require(topologyDefaults.maximumFirstScreenshotMilliseconds == 100,
            "single-monitor latency default self-test failed");
    topologyDefaults = BenchmarkConfiguration{};
    applyTopologyAwareDefaults(topologyDefaults, 2);
    require(topologyDefaults.maximumFirstScreenshotMilliseconds == 150,
            "multi-monitor latency default self-test failed");
    topologyDefaults.maximumFirstScreenshotMilliseconds = 123;
    topologyDefaults.maximumFirstScreenshotExplicit = true;
    applyTopologyAwareDefaults(topologyDefaults, 2);
    require(topologyDefaults.maximumFirstScreenshotMilliseconds == 123,
            "explicit latency threshold self-test failed");

    QTemporaryDir temporaryDirectory;
    require(temporaryDirectory.isValid(), "temporary directory self-test failed");
    const QString tracePath = temporaryDirectory.filePath(QStringLiteral("trace.jsonl"));
    QFile traceFile(tracePath);
    require(traceFile.open(QIODevice::WriteOnly), "trace self-test file could not be created");
    traceFile.write("{\"event\":\"app_ready\",\"process_id\":123}\n");
    traceFile.close();
    const QVector<QJsonObject> trace = readTrace(tracePath);
    require(trace.size() == 1 && trace.first().value(QStringLiteral("event")).toString() ==
                                     QStringLiteral("app_ready"),
            "trace parser self-test failed");

    require(privateWorkingSet(GetCurrentProcess()).bytes > 0,
            "private working-set self-test failed");
    const QJsonObject syntheticReport{
        {QStringLiteral("metrics"),
         QJsonObject{{QStringLiteral("cold_start_private_working_set"), statistics(values)},
                     {QStringLiteral("post_end_private_working_set"), statistics(values)},
                     {QStringLiteral("private_working_set_delta"), statistics(values)},
                     {QStringLiteral("private_working_set_absolute_delta"), statistics(values)},
                     {QStringLiteral("first_screenshot_to_composited_frame"),
                      statistics(values, QStringLiteral("ms"))}}},
    };
    require(
        reportHtml(syntheticReport).contains(QStringLiteral("Screenshot lifecycle performance")),
        "HTML report self-test failed");
    std::cout << "screenshot lifecycle benchmark self-tests passed\n";
    return true;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("snow-shot-screenshot-lifecycle-performance-benchmark"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Native Windows cold screenshot lifecycle performance benchmark"));
    parser.addHelpOption();
    parser.addOption(
        {QStringLiteral("app"), QStringLiteral("snow_shot executable"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("artifact directory"),
                      QStringLiteral("directory"),
                      QStringLiteral("build/screenshot-lifecycle-perf/latest")});
    parser.addOption({QStringLiteral("samples"), QStringLiteral("fresh-process samples"),
                      QStringLiteral("count"), QStringLiteral("10")});
    parser.addOption({QStringLiteral("screen-index"), QStringLiteral("monitor index"),
                      QStringLiteral("index"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("poll-ms"), QStringLiteral("memory polling interval"),
                      QStringLiteral("milliseconds"), QStringLiteral("250")});
    parser.addOption({QStringLiteral("baseline-min-wait-ms"),
                      QStringLiteral("minimum cold-start stabilization time"),
                      QStringLiteral("milliseconds"), QStringLiteral("20000")});
    parser.addOption({QStringLiteral("inter-capture-wait-ms"),
                      QStringLiteral("idle wait between first and subsequent screenshots"),
                      QStringLiteral("milliseconds"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("post-end-min-wait-ms"),
                      QStringLiteral("minimum post-end stabilization time"),
                      QStringLiteral("milliseconds"), QStringLiteral("15000")});
    parser.addOption({QStringLiteral("stability-window"),
                      QStringLiteral("memory samples required for convergence"),
                      QStringLiteral("count"), QStringLiteral("20")});
    parser.addOption({QStringLiteral("stability-range-kib"),
                      QStringLiteral("maximum private working-set range"),
                      QStringLiteral("kibibytes"), QStringLiteral("1024")});
    parser.addOption({QStringLiteral("timeout-ms"), QStringLiteral("per-phase timeout"),
                      QStringLiteral("milliseconds"), QStringLiteral("90000")});
    parser.addOption({QStringLiteral("max-first-screenshot-ms"),
                      QStringLiteral("maximum first screenshot latency threshold (defaults to "
                                     "100 ms plus 50 ms per additional monitor)"),
                      QStringLiteral("milliseconds"),
                      QString::number(kDefaultMaximumFirstScreenshotMilliseconds)});
    parser.addOption({QStringLiteral("max-toolbar-show-ms"),
                      QStringLiteral("maximum first or subsequent toolbar-show latency"),
                      QStringLiteral("milliseconds"),
                      QString::number(kDefaultMaximumToolbarShowMilliseconds)});
    parser.addOption({QStringLiteral("max-private-working-set-mib"),
                      QStringLiteral("maximum cold/post private working set acceptance threshold"),
                      QStringLiteral("MiB"),
                      QString::number(kDefaultMaximumPrivateWorkingSetMebibytes)});
    parser.addOption({QStringLiteral("max-private-working-set-delta-mib"),
                      QStringLiteral("maximum absolute cold/post private working-set delta"),
                      QStringLiteral("MiB"),
                      QString::number(kDefaultMaximumPrivateWorkingSetDeltaMebibytes)});
    parser.addOption(
        {QStringLiteral("self-test"), QStringLiteral("run benchmark self-tests and exit")});
    parser.process(application);

    try {
        if (parser.isSet(QStringLiteral("self-test"))) {
            return runSelfTest() ? 0 : 1;
        }
        return runBenchmark(configurationFromParser(parser));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
