#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QVector>
#include <QStringList>

#include <Windows.h>
#include <UIAutomation.h>
#include <objbase.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cwchar>

namespace {
using namespace std::chrono_literals;

template <typename T> class ComPtr final {
  public:
    ComPtr() = default;
    ComPtr(ComPtr&& other) noexcept : m_value(other.m_value) {
        other.m_value = nullptr;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset(other.m_value);
            other.m_value = nullptr;
        }
        return *this;
    }
    ~ComPtr() {
        reset();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const {
        return m_value;
    }
    T** put() {
        reset();
        return &m_value;
    }
    void reset(T* value = nullptr) {
        if (m_value) {
            m_value->Release();
            m_value = value;
        }
    }

  private:
    T* m_value = nullptr;
};

class ScopedCom final {
  public:
    ScopedCom() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() {
        if (SUCCEEDED(m_result))
            CoUninitialize();
    }
    HRESULT result() const {
        return m_result;
    }

  private:
    HRESULT m_result;
};

class ChildProcess final {
  public:
    ~ChildProcess() {
        stop();
    }
    bool start(const QString& executable) {
        std::wstring path = executable.toStdWString();
        std::wstring command =
            L"\"" + path + L"\" --show-main-window --e2e-allow-overlay-capture --e2e-instance-id=" +
            std::to_wstring(GetCurrentProcessId());
        std::vector<wchar_t> commandLine(command.begin(), command.end());
        commandLine.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            nullptr, &startup, &process))
            return false;
        CloseHandle(process.hThread);
        m_process = process.hProcess;
        m_pid = process.dwProcessId;
        return true;
    }
    DWORD pid() const {
        return m_pid;
    }
    bool alive() const {
        return m_process && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
    }
    qint64 workingSetBytes(bool peak) const {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!m_process ||
            !GetProcessMemoryInfo(m_process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                  sizeof(counters))) {
            return 0;
        }
        return static_cast<qint64>(peak ? counters.PeakWorkingSetSize : counters.WorkingSetSize);
    }
    void stop() {
        if (!m_process)
            return;
        if (alive()) {
            TerminateProcess(m_process, 1);
            WaitForSingleObject(m_process, 5000);
        }
        CloseHandle(m_process);
        m_process = nullptr;
    }

  private:
    HANDLE m_process = nullptr;
    DWORD m_pid = 0;
};

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

ComPtr<IUIAutomation> createAutomation() {
    ComPtr<IUIAutomation> automation;
    require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(automation.put()))),
            "UI Automation initialization failed");
    return automation;
}

ComPtr<IUIAutomationElement> findByAutomationIdSuffix(IUIAutomation& automation, DWORD pid,
                                                      const wchar_t* suffix) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation.GetRootElement(root.put())))
        return {};
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = static_cast<LONG>(pid);
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation.CreatePropertyCondition(UIA_ProcessIdPropertyId, value, condition.put())))
        return {};
    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(root.get()->FindAll(TreeScope_Descendants, condition.get(), elements.put())))
        return {};
    int length = 0;
    elements.get()->get_Length(&length);
    for (int i = 0; i < length; ++i) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements.get()->GetElement(i, element.put())))
            continue;
        BSTR id = nullptr;
        if (FAILED(element.get()->get_CurrentAutomationId(&id)))
            continue;
        const bool match = id != nullptr && wcsstr(id, suffix) != nullptr;
        SysFreeString(id);
        if (match)
            return element;
    }
    return {};
}

template <typename Finder> ComPtr<IUIAutomationElement> waitFor(Finder finder, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto result = finder();
        if (result.get() != nullptr)
            return result;
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

LONG absoluteCoordinate(LONG value, LONG origin, LONG extent) {
    return extent <= 1
               ? 0
               : static_cast<LONG>((static_cast<double>(value - origin) * 65535.0) / (extent - 1));
}

void sendMouse(int x, int y, DWORD flags) {
    const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN),
               top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG width = GetSystemMetrics(SM_CXVIRTUALSCREEN),
               height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = absoluteCoordinate(x, left, width);
    input.mi.dy = absoluteCoordinate(y, top, height);
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    require(SendInput(1, &input, sizeof(input)) == 1, "SendInput failed");
}

void rightClickAt(int x, int y) {
    sendMouse(x, y, MOUSEEVENTF_MOVE);
    sendMouse(x, y, MOUSEEVENTF_RIGHTDOWN);
    sendMouse(x, y, MOUSEEVENTF_RIGHTUP);
}

QVector<RECT> monitors() {
    QVector<RECT> result;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            static_cast<QVector<RECT>*>(reinterpret_cast<void*>(data))->push_back(info.rcWork);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

int traceLineCount(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    return file.readAll().count('\n');
}

QJsonObject readTraceLine(const QString& path, int expectedLine) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "could not open app trace");
    const QList<QByteArray> lines = file.readAll().split('\n');
    require(expectedLine > 0 && expectedLine <= lines.size(), "trace line was unavailable");
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(lines.at(expectedLine - 1), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            "trace JSON was invalid");
    return document.object();
}

qint64 milestoneNs(const QJsonObject& record, const QString& name) {
    return record.value(QStringLiteral("milestones_ns")).toObject().value(name).toInteger();
}

qint64 spanNs(const QJsonObject& record, const QString& name) {
    return record.value(QStringLiteral("spans_ns")).toObject().value(name).toInteger();
}

double asMilliseconds(double nanoseconds) {
    return nanoseconds / 1e6;
}

struct MetricSource {
    QString name;
    std::function<qint64(const QJsonObject&)> valueNs;
};

QVector<MetricSource> derivedMetrics() {
    const auto milestone = [](const char* name) {
        const QString key = QString::fromLatin1(name);
        return [key](const QJsonObject& record) { return milestoneNs(record, key); };
    };
    const auto interval = [](const char* beginName, const char* endName) {
        const QString beginKey = QString::fromLatin1(beginName);
        const QString endKey = QString::fromLatin1(endName);
        return [beginKey, endKey](const QJsonObject& record) {
            const qint64 begin = milestoneNs(record, beginKey);
            const qint64 end = milestoneNs(record, endKey);
            return begin > 0 && end >= begin ? end - begin : 0;
        };
    };
    const auto span = [](const char* name) {
        const QString key = QString::fromLatin1(name);
        return [key](const QJsonObject& record) { return spanNs(record, key); };
    };
    return {
        {QStringLiteral("end_to_end"),
         [](const QJsonObject& record) {
             return record.value(QStringLiteral("end_to_end_ns")).toInteger();
         }},
        {QStringLiteral("capture_image_acquired_native"), milestone("capture.native_returned")},
        {QStringLiteral("capture_image_applied"), milestone("capture.displays_applied")},
        {QStringLiteral("capture_worker_dispatch"), milestone("capture.worker_entry")},
        {QStringLiteral("capture_ui_marshalling"),
         interval("capture.native_returned", "capture.ui_finish_entry")},
        {QStringLiteral("capture_native_ffi"), span("capture.native_ffi")},
        {QStringLiteral("smart_selection_first_result"), milestone("selector.initial_resolved")},
        {QStringLiteral("selector_refresh_latency"),
         interval("selector.refresh_dispatched", "selector.refresh_finished")},
        {QStringLiteral("selector_hit_test_latency"),
         interval("selector.hit_test_dispatched", "selector.hit_test_finished")},
        {QStringLiteral("selector_service_create"), span("selector.service_create")},
        {QStringLiteral("reveal_span"),
         interval("presentation.reveal_begin", "presentation.composited")},
        {QStringLiteral("native_to_composited"),
         interval("capture.native_returned", "presentation.composited")},
        {QStringLiteral("overlay_sync_reveal_total"), span("presentation.window.sync_reveal")},
        {QStringLiteral("overlay_surface_commit"), span("presentation.window.surface_commit")},
    };
}

QJsonObject statistics(const QVector<double>& source, const QString& unit = QStringLiteral("ms")) {
    if (source.isEmpty())
        return {};
    QVector<double> values = source;
    std::sort(values.begin(), values.end());
    auto at = [&values](double p) {
        return values[std::min(values.size() - 1,
                               static_cast<qsizetype>(std::ceil(p * values.size()) - 1))];
    };
    const double mean = std::accumulate(values.cbegin(), values.cend(), 0.0) / values.size();
    double variance = 0.0;
    for (double value : values)
        variance += (value - mean) * (value - mean);
    const auto key = [&unit](const char* name) {
        return QString::fromLatin1(name) + QLatin1Char('_') + unit;
    };
    return {{QStringLiteral("count"), values.size()},
            {key("min"), values.first()},
            {key("mean"), mean},
            {key("p50"), at(.50)},
            {key("p90"), at(.90)},
            {key("p95"), at(.95)},
            {key("p99"), at(.99)},
            {key("max"), values.last()},
            {key("stddev"), std::sqrt(variance / values.size())}};
}

QString htmlEscape(QString value) {
    return value.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
}

QJsonObject milestoneStatistics(const QVector<QJsonObject>& records, const QString& key) {
    QVector<double> values;
    for (const QJsonObject& record : records) {
        const qint64 nanoseconds = milestoneNs(record, key);
        if (nanoseconds > 0)
            values.push_back(asMilliseconds(nanoseconds));
    }
    return statistics(values);
}

QJsonObject spanStatistics(const QVector<QJsonObject>& records, const QString& key) {
    QVector<double> values;
    for (const QJsonObject& record : records) {
        const qint64 nanoseconds = spanNs(record, key);
        if (nanoseconds > 0)
            values.push_back(asMilliseconds(nanoseconds));
    }
    return statistics(values);
}

QJsonObject counterStatistics(const QVector<QJsonObject>& records, const QString& key) {
    QVector<double> values;
    for (const QJsonObject& record : records) {
        const QJsonValue value = record.value(QStringLiteral("counters")).toObject().value(key);
        if (!value.isUndefined())
            values.push_back(value.toDouble());
    }
    return statistics(values, QStringLiteral("count"));
}

QStringList collectKeys(const QVector<QJsonObject>& records, const char* container) {
    QStringList keys;
    for (const QJsonObject& record : records) {
        const QJsonObject object = record.value(QLatin1String(container)).toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!keys.contains(it.key()))
                keys.push_back(it.key());
        }
    }
    keys.sort();
    return keys;
}

bool runSelfTest() {
    const QVector<double> values{1.0, 2.0, 3.0, 4.0, 5.0};
    if (!qFuzzyCompare(statistics(values).value(QStringLiteral("p50_ms")).toDouble() + 1.0, 4.0)) {
        return false;
    }
    QJsonObject record;
    QJsonObject milestones;
    milestones[QStringLiteral("capture.native_returned")] = 5000000;
    milestones[QStringLiteral("capture.ui_finish_entry")] = 8000000;
    record.insert(QStringLiteral("milestones_ns"), milestones);
    record.insert(QStringLiteral("end_to_end_ns"), 20000000);
    const auto metrics = derivedMetrics();
    bool hasEndToEnd = false;
    bool hasMarshalling = false;
    for (const MetricSource& metric : metrics) {
        if (metric.name == QStringLiteral("end_to_end")) {
            hasEndToEnd = metric.valueNs(record) == 20000000;
        }
        if (metric.name == QStringLiteral("capture_ui_marshalling")) {
            hasMarshalling = metric.valueNs(record) == 3000000;
        }
    }
    return hasEndToEnd && hasMarshalling;
}

int run(const QCommandLineParser& parser) {
    const QString appPath = parser.value(QStringLiteral("app"));
    const QString output = QDir::cleanPath(parser.value(QStringLiteral("output")));
    const int captures = parser.value(QStringLiteral("captures")).toInt();
    const int settleMs = parser.value(QStringLiteral("settle-ms")).toInt();
    const int timeout = parser.value(QStringLiteral("timeout-ms")).toInt();
    const int screenIndex = parser.value(QStringLiteral("screen-index")).toInt();
    const QString revealStrategy = parser.value(QStringLiteral("reveal-strategy")).trimmed();
    require(!appPath.isEmpty() && captures >= 2 && settleMs >= 0 && timeout > 0,
            "invalid benchmark arguments");
    const QStringList supportedRevealStrategies{QStringLiteral("single-repaint"),
                                                QStringLiteral("posted-update"),
                                                QStringLiteral("native-update"),
                                                QStringLiteral("native-invalidate"),
                                                QStringLiteral("native-invalidate-suppressed")};
    require(revealStrategy.isEmpty() || supportedRevealStrategies.contains(revealStrategy),
            "unsupported reveal strategy");
    const QVector<RECT> displayList = monitors();
    require(screenIndex >= 0 && screenIndex < displayList.size(), "monitor index unavailable");
    const RECT screen = displayList.at(screenIndex);
    const int cursorX = (screen.left + screen.right) / 2;
    const int cursorY = (screen.top + screen.bottom) / 2;

    QDir().mkpath(output);
    const QString tracePath = QDir(output).filePath(QStringLiteral("app-trace.jsonl"));
    QFile::remove(tracePath);
    _putenv_s("SNOW_SHOT_CAPTURE_PERF_TRACE", tracePath.toLocal8Bit().constData());
    _putenv_s("SNOW_SHOT_CAPTURE_REVEAL_STRATEGY", revealStrategy.toLocal8Bit().constData());

    ScopedCom com;
    require(SUCCEEDED(com.result()), "COM initialization failed");
    auto automation = createAutomation();
    ChildProcess child;
    require(child.start(appPath), "could not start snow_shot");

    auto quickScreenshotItem = [&]() {
        return findByAutomationIdSuffix(*automation.get(), child.pid(),
                                        L"settings-item-quick-screenshot");
    };
    require(waitFor(quickScreenshotItem, timeout).get() != nullptr,
            "settings quick screenshot item did not appear");

    QVector<QJsonObject> records;
    int line = 0;
    for (int capture = 1; capture <= captures; ++capture) {
        // Park the cursor deterministically so every initial smart-selection
        // hit test targets the same screen content.
        sendMouse(cursorX, cursorY, MOUSEEVENTF_MOVE);
        auto screenshot = waitFor(quickScreenshotItem, timeout);
        require(screenshot.get() != nullptr && invoke(*screenshot.get()),
                "could not invoke screenshot capture");
        const auto invokedAt = std::chrono::steady_clock::now();

        const int targetLine = line + 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        while (traceLineCount(tracePath) < targetLine && child.alive() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        require(traceLineCount(tracePath) >= targetLine, "capture sample timed out");
        line = targetLine;
        const auto traceLineAt = std::chrono::steady_clock::now();
        const double invokeToLineMs =
            std::chrono::duration<double, std::milli>(traceLineAt - invokedAt).count();

        QJsonObject record = readTraceLine(tracePath, line);
        require(record.value(QStringLiteral("success")).toBool(),
                "capture sample was not presented successfully");
        record.insert(QStringLiteral("capture_index"), capture);
        record.insert(QStringLiteral("group"), capture == 1   ? QStringLiteral("first")
                                               : capture == 2 ? QStringLiteral("second")
                                                              : QStringLiteral("steady"));
        record.insert(QStringLiteral("invoke_to_trace_line_ms"), invokeToLineMs);
        record.insert(QStringLiteral("working_set_bytes"), child.workingSetBytes(false));
        record.insert(QStringLiteral("peak_working_set_bytes"), child.workingSetBytes(true));
        records.push_back(record);

        // Dismiss the selection overlay so the app returns to idle before the
        // next capture. A right-click while intelligent-selecting cancels.
        rightClickAt(cursorX, cursorY);
        std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
    }
    child.stop();

    QFile raw(QDir(output).filePath(QStringLiteral("raw.jsonl")));
    require(raw.open(QIODevice::WriteOnly | QIODevice::Truncate), "could not write raw report");
    for (const QJsonObject& record : records) {
        raw.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
        raw.write("\n");
    }
    raw.close();

    const QStringList groups{QStringLiteral("first"), QStringLiteral("second"),
                             QStringLiteral("steady")};
    const auto metrics = derivedMetrics();
    QJsonArray groupReports;
    for (const QString& group : groups) {
        QVector<QJsonObject> groupRecords;
        for (const QJsonObject& record : records) {
            if (record.value(QStringLiteral("group")).toString() == group) {
                groupRecords.push_back(record);
            }
        }
        if (groupRecords.isEmpty())
            continue;

        QJsonObject metricsReport;
        for (const MetricSource& metric : metrics) {
            QVector<double> values;
            for (const QJsonObject& record : groupRecords) {
                const qint64 nanoseconds = metric.valueNs(record);
                if (nanoseconds > 0)
                    values.push_back(asMilliseconds(nanoseconds));
            }
            metricsReport.insert(metric.name, statistics(values));
        }
        QVector<double> invokeToLine;
        QVector<double> workingSet;
        for (const QJsonObject& record : groupRecords) {
            invokeToLine.push_back(
                record.value(QStringLiteral("invoke_to_trace_line_ms")).toDouble());
            workingSet.push_back(record.value(QStringLiteral("working_set_bytes")).toDouble() /
                                 (1024.0 * 1024.0));
        }
        metricsReport.insert(QStringLiteral("invoke_to_trace_line"), statistics(invokeToLine));
        metricsReport.insert(QStringLiteral("working_set"),
                             statistics(workingSet, QStringLiteral("mb")));

        QJsonObject milestonesReport;
        for (const QString& key : collectKeys(groupRecords, "milestones_ns")) {
            milestonesReport.insert(key, milestoneStatistics(groupRecords, key));
        }
        QJsonObject spansReport;
        for (const QString& key : collectKeys(groupRecords, "spans_ns")) {
            spansReport.insert(key, spanStatistics(groupRecords, key));
        }
        QJsonObject countersReport;
        for (const QString& key : collectKeys(groupRecords, "counters")) {
            countersReport.insert(key, counterStatistics(groupRecords, key));
        }

        groupReports.append(QJsonObject{{QStringLiteral("id"), group},
                                        {QStringLiteral("count"), groupRecords.size()},
                                        {QStringLiteral("metrics"), metricsReport},
                                        {QStringLiteral("milestones"), milestonesReport},
                                        {QStringLiteral("spans"), spansReport},
                                        {QStringLiteral("counters"), countersReport}});
    }

    const QJsonObject report{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("benchmark"), QStringLiteral("screenshot_capture_startup")},
        {QStringLiteral("generated_utc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("captures"), captures},
        {QStringLiteral("screen_index"), screenIndex},
        {QStringLiteral("settle_ms"), settleMs},
        {QStringLiteral("reveal_strategy"),
         revealStrategy.isEmpty() ? QStringLiteral("default") : revealStrategy},
        {QStringLiteral("groups"), groupReports},
        {QStringLiteral("environment"),
         QJsonObject{{QStringLiteral("os"), QSysInfo::prettyProductName()},
                     {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
                     {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
                     {QStringLiteral("monitor_count"), displayList.size()}}}};
    QFile reportFile(QDir(output).filePath(QStringLiteral("report.json")));
    require(reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "could not write report.json");
    reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    reportFile.close();
    QFile html(QDir(output).filePath(QStringLiteral("report.html")));
    require(html.open(QIODevice::WriteOnly | QIODevice::Truncate), "could not write report.html");
    html.write(
        ("<!doctype html><meta charset=utf-8><title>Snow Shot capture startup "
         "performance</title><h1>Capture startup performance (first/second capture)</h1><pre>" +
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
        QStringLiteral("snow-shot-capture-startup-performance-benchmark"));
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Native Windows screenshot capture startup performance benchmark"));
    parser.addHelpOption();
    parser.addOption(
        {QStringLiteral("app"), QStringLiteral("snow_shot executable"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("output directory"),
                      QStringLiteral("directory"), QStringLiteral("capture-startup-performance")});
    parser.addOption({QStringLiteral("screen-index"), QStringLiteral("monitor index"),
                      QStringLiteral("index"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("captures"),
                      QStringLiteral("total capture count (first, second, then steady state)"),
                      QStringLiteral("count"), QStringLiteral("12")});
    parser.addOption({QStringLiteral("settle-ms"), QStringLiteral("idle delay between captures"),
                      QStringLiteral("milliseconds"), QStringLiteral("500")});
    parser.addOption({QStringLiteral("timeout-ms"), QStringLiteral("sample timeout"),
                      QStringLiteral("milliseconds"), QStringLiteral("30000")});
    parser.addOption({QStringLiteral("reveal-strategy"),
                      QStringLiteral("prepared-overlay reveal strategy"),
                      QStringLiteral("strategy")});
    parser.addOption({QStringLiteral("self-test"), QStringLiteral("run report self-tests")});
    parser.process(application);
    try {
        if (parser.isSet(QStringLiteral("self-test")))
            return runSelfTest() ? 0 : 1;
        return run(parser);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
