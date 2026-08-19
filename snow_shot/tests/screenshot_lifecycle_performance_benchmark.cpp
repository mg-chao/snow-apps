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

qint64 privateWorkingSetBytes(HANDLE process) {
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
            return static_cast<qint64>(result);
        }
        require(GetLastError() == ERROR_BAD_LENGTH, "QueryWorkingSet failed");
        capacity *= 2;
    }
    throw std::runtime_error("QueryWorkingSet buffer did not converge");
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
};

StableMemorySample waitForStableMemory(const ChildProcess& process, int minimumWaitMilliseconds,
                                       int pollMilliseconds, int windowSamples,
                                       qint64 maximumRangeBytes, int timeoutMilliseconds) {
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(timeoutMilliseconds);
    std::deque<qint64> window;
    qint64 peakBytes = 0;
    while (process.alive() && std::chrono::steady_clock::now() < deadline) {
        const qint64 currentBytes = privateWorkingSetBytes(process.handle());
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
            return StableMemorySample{currentBytes, peakBytes, *maximum - *minimum, elapsed};
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
        row(QStringLiteral("first_screenshot_to_composited_frame"),
            QStringLiteral("First screenshot to composited frame (ms)"), QStringLiteral("ms"));
    const QString embeddedJson =
        htmlEscape(QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented)));
    return QStringLiteral(
               "<!doctype html><html><head><meta charset=utf-8><title>Snow Shot screenshot "
               "lifecycle performance</title><style>body{font:14px "
               "system-ui;margin:32px;color:#202124}"
               "table{border-collapse:collapse;margin:20px 0}th,td{border:1px solid "
               "#dadce0;padding:8px 12px}"
               "th{text-align:left;background:#f8f9fa}pre{background:#f8f9fa;padding:16px;overflow:"
               "auto}"
               "</style></head><body><h1>Screenshot lifecycle performance</h1>"
               "<table><thead><tr><th>Metric</th><th>Min</th><th>Mean</th><th>P50</th><th>P90</th>"
               "<th>P95</th><th>Max</th></tr></thead><tbody>%1</tbody></table><h2>Report JSON</h2>"
               "<pre>%2</pre></body></html>")
        .arg(rows, embeddedJson);
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
    int postEndMinimumWaitMilliseconds = 15000;
    int stabilityWindowSamples = 20;
    qint64 stabilityRangeBytes = 1024 * 1024;
    int timeoutMilliseconds = 90000;
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
    configuration.postEndMinimumWaitMilliseconds =
        parser.value(QStringLiteral("post-end-min-wait-ms")).toInt();
    configuration.stabilityWindowSamples = parser.value(QStringLiteral("stability-window")).toInt();
    configuration.stabilityRangeBytes =
        parser.value(QStringLiteral("stability-range-kib")).toLongLong() * kBytesPerKibibyte;
    configuration.timeoutMilliseconds = parser.value(QStringLiteral("timeout-ms")).toInt();
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
    require(configuration.postEndMinimumWaitMilliseconds >= 0,
            "post-end minimum wait must be nonnegative");
    require(configuration.stabilityWindowSamples > 1,
            "stability window must contain at least two samples");
    require(configuration.stabilityRangeBytes > 0, "stability range must be positive");
    require(configuration.timeoutMilliseconds > configuration.baselineMinimumWaitMilliseconds &&
                configuration.timeoutMilliseconds > configuration.postEndMinimumWaitMilliseconds,
            "timeout must exceed both minimum memory waits");
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

    ChildProcess request;
    QStringList requestArguments = baseArguments;
    requestArguments.push_back(QStringLiteral("--e2e-start-screenshot"));
    require(request.start(configuration.appPath, requestArguments),
            "could not issue screenshot command");
    require(request.waitForExit(10000), "screenshot command helper did not exit");
    require(request.exitCode() == 0, "screenshot command helper failed");
    static_cast<void>(waitForTraceEvent(tracePath, primary.processId(),
                                        {QStringLiteral("capture_command_accepted")}, traceCursor,
                                        primary, configuration.timeoutMilliseconds));
    const QJsonObject presentation = waitForTraceEvent(
        tracePath, primary.processId(),
        {QStringLiteral("first_capture_presented"), QStringLiteral("capture_released")},
        traceCursor, primary, configuration.timeoutMilliseconds);
    require(presentation.value(QStringLiteral("event")).toString() ==
                QStringLiteral("first_capture_presented"),
            "screenshot ended before its first frame was presented");
    const qint64 elapsedNanoseconds = presentation.value(QStringLiteral("elapsed_ns")).toInteger();
    require(elapsedNanoseconds > 0, "first screenshot duration was not recorded");

    dragSelection(monitor.bounds);
    ComPtr<IUIAutomationElement> cancelButton = waitForVisibleElement(
        automation, primary.processId(), QStringLiteral("screenshotCancelButton"), primary,
        configuration.timeoutMilliseconds);
    require(cancelButton.get() != nullptr, "screenshot cancel button did not appear");
    clickElement(*cancelButton.get());

    const QJsonObject released =
        waitForTraceEvent(tracePath, primary.processId(), {QStringLiteral("capture_released")},
                          traceCursor, primary, configuration.timeoutMilliseconds);
    require(released.value(QStringLiteral("capture_presented")).toBool(),
            "capture release did not follow a presented screenshot");
    const StableMemorySample postEnd =
        waitForStableMemory(primary, configuration.postEndMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);

    const qint64 deltaBytes = postEnd.bytes - baseline.bytes;
    QJsonObject record{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("iteration"), iteration},
        {QStringLiteral("process_id"), static_cast<qint64>(primary.processId())},
        {QStringLiteral("cold_start_private_working_set_bytes"), baseline.bytes},
        {QStringLiteral("post_end_private_working_set_bytes"), postEnd.bytes},
        {QStringLiteral("private_working_set_delta_bytes"), deltaBytes},
        {QStringLiteral("first_screenshot_elapsed_ns"), elapsedNanoseconds},
        {QStringLiteral("cold_start_peak_private_working_set_bytes"), baseline.peakBytes},
        {QStringLiteral("post_end_peak_private_working_set_bytes"), postEnd.peakBytes},
        {QStringLiteral("cold_start_stability_range_bytes"), baseline.rangeBytes},
        {QStringLiteral("post_end_stability_range_bytes"), postEnd.rangeBytes},
        {QStringLiteral("cold_start_convergence_ms"), baseline.elapsedMilliseconds},
        {QStringLiteral("post_end_convergence_ms"), postEnd.elapsedMilliseconds},
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
              << " MiB, first-frame=" << static_cast<double>(elapsedNanoseconds) / 1e6 << " ms\n";
    primary.stop();
    return record;
}

int runBenchmark(const BenchmarkConfiguration& configuration) {
    const QVector<MonitorInfo> displayList = monitors();
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
    for (int iteration = 1; iteration <= configuration.samples; ++iteration) {
        const QJsonObject record = runSample(
            configuration, displayList.at(configuration.screenIndex), iteration, *automation.get());
        rawFile.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
        rawFile.write("\n");
        rawFile.flush();
        baselineValues.push_back(
            static_cast<double>(
                record.value(QStringLiteral("cold_start_private_working_set_bytes")).toInteger()) /
            kBytesPerMebibyte);
        postEndValues.push_back(
            static_cast<double>(
                record.value(QStringLiteral("post_end_private_working_set_bytes")).toInteger()) /
            kBytesPerMebibyte);
        deltaValues.push_back(
            static_cast<double>(
                record.value(QStringLiteral("private_working_set_delta_bytes")).toInteger()) /
            kBytesPerMebibyte);
        durationValues.push_back(
            static_cast<double>(
                record.value(QStringLiteral("first_screenshot_elapsed_ns")).toInteger()) /
            1e6);
    }
    rawFile.close();

    const QJsonObject metrics{
        {QStringLiteral("cold_start_private_working_set"), statistics(baselineValues)},
        {QStringLiteral("post_end_private_working_set"), statistics(postEndValues)},
        {QStringLiteral("private_working_set_delta"), statistics(deltaValues)},
        {QStringLiteral("first_screenshot_to_composited_frame"),
         statistics(durationValues, QStringLiteral("ms"))},
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
             {QStringLiteral("post_end_min_wait_ms"), configuration.postEndMinimumWaitMilliseconds},
             {QStringLiteral("stability_window_samples"), configuration.stabilityWindowSamples},
             {QStringLiteral("stability_range_kib"),
              configuration.stabilityRangeBytes / kBytesPerKibibyte},
             {QStringLiteral("timeout_ms"), configuration.timeoutMilliseconds},
         }},
        {QStringLiteral("metrics"), metrics},
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
    return 0;
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

    require(privateWorkingSetBytes(GetCurrentProcess()) > 0,
            "private working-set self-test failed");
    const QJsonObject syntheticReport{
        {QStringLiteral("metrics"),
         QJsonObject{{QStringLiteral("cold_start_private_working_set"), statistics(values)},
                     {QStringLiteral("post_end_private_working_set"), statistics(values)},
                     {QStringLiteral("private_working_set_delta"), statistics(values)},
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
