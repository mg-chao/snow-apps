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
#include <QSaveFile>
#include <QSysInfo>
#include <QStringList>
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
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

constexpr qint64 kBytesPerKibibyte = 1024;
constexpr qint64 kBytesPerMebibyte = 1024 * 1024;
constexpr int kSelectionPixels = 800;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void configurePerMonitorDpiAwareness() {
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE) {
        require(GetLastError() == ERROR_ACCESS_DENIED,
                "could not configure per-monitor DPI awareness");
    }
    require(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) ==
                DPI_AWARENESS_PER_MONITOR_AWARE,
            "benchmark process is not per-monitor DPI aware");
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

class ScopedEnvironmentVariable final {
  public:
    ScopedEnvironmentVariable(QByteArray name, QByteArray value)
        : m_name(std::move(name)), m_wasSet(qEnvironmentVariableIsSet(m_name.constData())),
          m_previous(qgetenv(m_name.constData())) {
        require(qputenv(m_name.constData(), value), "could not set benchmark environment variable");
    }

    ~ScopedEnvironmentVariable() {
        if (m_wasSet) {
            static_cast<void>(qputenv(m_name.constData(), m_previous));
        } else {
            static_cast<void>(qunsetenv(m_name.constData()));
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

  private:
    QByteArray m_name;
    bool m_wasSet;
    QByteArray m_previous;
};

QString quotedCommand(const QString& executable, const QStringList& arguments) {
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
        const DWORD processId = m_processId;
        if (!stop()) {
            std::cerr << "could not terminate child process " << processId << '\n';
            forceStopAndWait();
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const QString& executable, const QStringList& arguments = {}) {
        m_lastStartError = ERROR_SUCCESS;
        if (!stop()) {
            m_lastStartError = ERROR_PROCESS_ABORTED;
            return false;
        }
        const std::wstring executablePath = executable.toStdWString();
        const std::wstring command = quotedCommand(executable, arguments).toStdWString();
        std::vector<wchar_t> commandLine(command.cbegin(), command.cend());
        commandLine.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job == nullptr) {
            m_lastStartError = GetLastError();
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
        jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jobLimits,
                                    sizeof(jobLimits)) == FALSE) {
            m_lastStartError = GetLastError();
            CloseHandle(job);
            return false;
        }

        PROCESS_INFORMATION process{};
        if (CreateProcessW(executablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                           CREATE_SUSPENDED, nullptr, nullptr, &startup, &process) == FALSE) {
            m_lastStartError = GetLastError();
            CloseHandle(job);
            return false;
        }

        if (AssignProcessToJobObject(job, process.hProcess) == FALSE) {
            m_lastStartError = GetLastError();
            terminateCreatedProcessAndWait(nullptr, process.hProcess);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(job);
            return false;
        }
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            m_lastStartError = GetLastError();
            terminateCreatedProcessAndWait(job, process.hProcess);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(job);
            return false;
        }

        CloseHandle(process.hThread);
        m_job = job;
        m_process = process.hProcess;
        m_processId = process.dwProcessId;
        return true;
    }

    [[nodiscard]] bool alive() const {
        return m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
    }

    [[nodiscard]] bool waitForExit(int timeoutMilliseconds) const {
        return m_process != nullptr &&
               WaitForSingleObject(m_process, static_cast<DWORD>(timeoutMilliseconds)) ==
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

    [[nodiscard]] DWORD lastStartError() const {
        return m_lastStartError;
    }

    [[nodiscard]] bool stop() {
        if (m_process == nullptr) {
            return true;
        }
        DWORD waitResult = WaitForSingleObject(m_process, 0);
        if (waitResult == WAIT_TIMEOUT) {
            bool terminationRequested = m_job != nullptr && TerminateJobObject(m_job, 1) != FALSE;
            if (!terminationRequested) {
                terminationRequested = TerminateProcess(m_process, 1) != FALSE;
            }
            if (!terminationRequested && WaitForSingleObject(m_process, 0) != WAIT_OBJECT_0) {
                return false;
            }
            waitResult = WaitForSingleObject(m_process, INFINITE);
        }
        if (waitResult != WAIT_OBJECT_0) {
            return false;
        }
        releaseHandles();
        return true;
    }

  private:
    static void terminateCreatedProcessAndWait(HANDLE job, HANDLE process) {
        if (job == nullptr || TerminateJobObject(job, 1) == FALSE) {
            static_cast<void>(TerminateProcess(process, 1));
        }
        static_cast<void>(WaitForSingleObject(process, INFINITE));
    }

    void forceStopAndWait() {
        if (m_job != nullptr) {
            CloseHandle(m_job);
            m_job = nullptr;
        } else if (m_process != nullptr) {
            static_cast<void>(TerminateProcess(m_process, 1));
        }
        if (m_process != nullptr && WaitForSingleObject(m_process, INFINITE) == WAIT_OBJECT_0) {
            CloseHandle(m_process);
            m_process = nullptr;
        }
        m_processId = 0;
    }

    void releaseHandles() {
        if (m_process != nullptr) {
            CloseHandle(m_process);
            m_process = nullptr;
        }
        if (m_job != nullptr) {
            CloseHandle(m_job);
            m_job = nullptr;
        }
        m_processId = 0;
    }

    HANDLE m_job = nullptr;
    HANDLE m_process = nullptr;
    DWORD m_processId = 0;
    DWORD m_lastStartError = ERROR_SUCCESS;
};

std::runtime_error childStartError(const char* context, const ChildProcess& process) {
    const DWORD error = process.lastStartError();
    const std::string systemMessage =
        std::error_code(static_cast<int>(error), std::system_category()).message();
    return std::runtime_error(std::string(context) + ": " + systemMessage + " (Windows error " +
                              std::to_string(error) + ")");
}

class CursorRestore final {
  public:
    CursorRestore() : m_valid(GetCursorPos(&m_position) != FALSE) {}
    ~CursorRestore() {
        if (m_valid) {
            static_cast<void>(SetCursorPos(m_position.x, m_position.y));
        }
    }

    CursorRestore(const CursorRestore&) = delete;
    CursorRestore& operator=(const CursorRestore&) = delete;

  private:
    POINT m_position{};
    bool m_valid = false;
};

ComPtr<IUIAutomation> createAutomation() {
    ComPtr<IUIAutomation> automation;
    require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(automation.put()))),
            "UI Automation initialization failed");
    return automation;
}

enum class VisibleElementLookupStatus {
    Found,
    Absent,
    Error,
};

struct VisibleElementLookup {
    VisibleElementLookupStatus status = VisibleElementLookupStatus::Error;
    ComPtr<IUIAutomationElement> element;
};

bool provesElementAbsent(VisibleElementLookupStatus status) {
    return status == VisibleElementLookupStatus::Absent;
}

VisibleElementLookup findVisibleElementByAutomationIdSuffix(IUIAutomation& automation,
                                                            DWORD processId,
                                                            const QString& automationIdSuffix) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation.GetRootElement(root.put()))) {
        return {VisibleElementLookupStatus::Error, {}};
    }

    VARIANT processValue{};
    processValue.vt = VT_I4;
    processValue.lVal = static_cast<LONG>(processId);
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation.CreatePropertyCondition(UIA_ProcessIdPropertyId, processValue,
                                                  condition.put()))) {
        return {VisibleElementLookupStatus::Error, {}};
    }

    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(root.get()->FindAll(TreeScope_Descendants, condition.get(), elements.put()))) {
        return {VisibleElementLookupStatus::Error, {}};
    }

    int length = 0;
    if (FAILED(elements.get()->get_Length(&length))) {
        return {VisibleElementLookupStatus::Error, {}};
    }
    bool lookupFailed = false;
    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements.get()->GetElement(index, element.put()))) {
            lookupFailed = true;
            continue;
        }
        BSTR rawId = nullptr;
        if (FAILED(element.get()->get_CurrentAutomationId(&rawId))) {
            lookupFailed = true;
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
        if (FAILED(element.get()->get_CurrentIsOffscreen(&offscreen))) {
            lookupFailed = true;
            continue;
        }
        if (offscreen == FALSE) {
            return {VisibleElementLookupStatus::Found, std::move(element)};
        }
    }
    return {lookupFailed ? VisibleElementLookupStatus::Error : VisibleElementLookupStatus::Absent,
            {}};
}

ComPtr<IUIAutomationElement> waitForVisibleElement(IUIAutomation& automation, DWORD processId,
                                                   const QString& automationIdSuffix,
                                                   const ChildProcess& process,
                                                   int timeoutMilliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (process.alive() && std::chrono::steady_clock::now() < deadline) {
        VisibleElementLookup lookup =
            findVisibleElementByAutomationIdSuffix(automation, processId, automationIdSuffix);
        if (lookup.status == VisibleElementLookupStatus::Found) {
            return std::move(lookup.element);
        }
        std::this_thread::sleep_for(25ms);
    }
    return {};
}

bool waitForElementToDisappear(IUIAutomation& automation, DWORD processId,
                               const QString& automationIdSuffix, const ChildProcess& process,
                               int timeoutMilliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (process.alive() && std::chrono::steady_clock::now() < deadline) {
        const VisibleElementLookup lookup =
            findVisibleElementByAutomationIdSuffix(automation, processId, automationIdSuffix);
        if (provesElementAbsent(lookup.status)) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

void sendEscape() {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_ESCAPE;
    require(SendInput(1, &input, sizeof(input)) == 1, "SendInput Escape key-down failed");
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(1, &input, sizeof(input)) == 1, "SendInput Escape key-up failed");
}

LONG absoluteCoordinate(LONG value, LONG origin, LONG extent) {
    return extent <= 1 ? 0
                       : static_cast<LONG>((static_cast<double>(value - origin) * 65535.0) /
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
    require(SendInput(1, &input, sizeof(input)) == 1, "SendInput mouse event failed");
}

void sendMouseWheel(int x, int y, int delta) {
    const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = absoluteCoordinate(x, left, width);
    input.mi.dy = absoluteCoordinate(y, top, height);
    input.mi.mouseData = static_cast<DWORD>(delta);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_WHEEL | MOUSEEVENTF_ABSOLUTE |
                       MOUSEEVENTF_VIRTUALDESK;
    require(SendInput(1, &input, sizeof(input)) == 1, "SendInput mouse wheel event failed");
}

void clickElement(IUIAutomationElement& element) {
    RECT bounds{};
    require(SUCCEEDED(element.get_CurrentBoundingRectangle(&bounds)) &&
                bounds.right > bounds.left && bounds.bottom > bounds.top,
            "UI element has invalid screen bounds");
    const int x = bounds.left + (bounds.right - bounds.left) / 2;
    const int y = bounds.top + (bounds.bottom - bounds.top) / 2;
    sendMouse(x, y, MOUSEEVENTF_MOVE);
    std::this_thread::sleep_for(25ms);
    sendMouse(x, y, MOUSEEVENTF_LEFTDOWN);
    sendMouse(x, y, MOUSEEVENTF_LEFTUP);
}

void clickVisibleElement(IUIAutomation& automation, DWORD processId, const QString& automationId,
                         const ChildProcess& process, int timeoutMilliseconds,
                         const char* errorMessage) {
    ComPtr<IUIAutomationElement> element =
        waitForVisibleElement(automation, processId, automationId, process, timeoutMilliseconds);
    require(element.get() != nullptr, errorMessage);
    clickElement(*element.get());
}

void closeNativeWindow(IUIAutomationElement& element, DWORD expectedProcessId) {
    UIA_HWND automationWindow = nullptr;
    require(SUCCEEDED(element.get_CurrentNativeWindowHandle(&automationWindow)) &&
                automationWindow != nullptr,
            "UI element does not expose a native window");
    const HWND window = static_cast<HWND>(automationWindow);
    require(IsWindow(window) != FALSE, "UI element native window is no longer valid");
    DWORD processId = 0;
    static_cast<void>(GetWindowThreadProcessId(window, &processId));
    require(processId == expectedProcessId,
            "UI element native window belongs to a different process");
    require(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE,
            "could not post the native window close command");
}

void positionCursorForCapture(const RECT& monitor) {
    const int width = monitor.right - monitor.left;
    const int height = monitor.bottom - monitor.top;
    require(width > 0 && height > 0, "selected monitor has invalid bounds");
    sendMouse(monitor.left + width / 2, monitor.top + height / 2, MOUSEEVENTF_MOVE);
    std::this_thread::sleep_for(20ms);
}

bool monitorSupportsFixedSelectionDrag(const RECT& monitor) {
    const qint64 monitorWidth =
        static_cast<qint64>(monitor.right) - static_cast<qint64>(monitor.left);
    const qint64 monitorHeight =
        static_cast<qint64>(monitor.bottom) - static_cast<qint64>(monitor.top);
    // Both endpoints are pixels on the monitor. A delta of 800 therefore needs 801 pixels.
    return monitorWidth > kSelectionPixels && monitorHeight > kSelectionPixels;
}

void dragFixedSelection(const RECT& monitor) {
    const int monitorWidth = monitor.right - monitor.left;
    const int monitorHeight = monitor.bottom - monitor.top;
    require(monitorSupportsFixedSelectionDrag(monitor),
            "selected monitor cannot contain the required 800x800 drag");
    const int x0 = monitor.left + (monitorWidth - kSelectionPixels) / 2;
    const int y0 = monitor.top + (monitorHeight - kSelectionPixels) / 2;
    sendMouse(x0, y0, MOUSEEVENTF_MOVE);
    sendMouse(x0, y0, MOUSEEVENTF_LEFTDOWN);
    std::this_thread::sleep_for(80ms);
    sendMouse(x0 + kSelectionPixels, y0 + kSelectionPixels, MOUSEEVENTF_MOVE);
    std::this_thread::sleep_for(80ms);
    sendMouse(x0 + kSelectionPixels, y0 + kSelectionPixels, MOUSEEVENTF_LEFTUP);
}

QVector<QJsonObject> readJsonLines(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QVector<QJsonObject> records;
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray& line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error == QJsonParseError::NoError && document.isObject()) {
            records.push_back(document.object());
        }
    }
    return records;
}

QJsonObject waitForLifecycleEvent(const QString& path, DWORD processId, const QString& event,
                                  qsizetype& cursor, const ChildProcess& process,
                                  int timeoutMilliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (process.alive() && std::chrono::steady_clock::now() < deadline) {
        const QVector<QJsonObject> records = readJsonLines(path);
        while (cursor < records.size()) {
            const QJsonObject record = records.at(cursor++);
            if (record.value(QStringLiteral("process_id")).toInteger() == processId &&
                record.value(QStringLiteral("event")).toString() == event) {
                return record;
            }
        }
        std::this_thread::sleep_for(25ms);
    }
    throw std::runtime_error("lifecycle trace event did not arrive");
}

void advanceLifecycleCursor(const QString& path, qsizetype& cursor) {
    cursor = readJsonLines(path).size();
}

void validateIdleMemoryReclaim(const QJsonObject& record, bool expectedWorkingSetTrim) {
    require(record.value(QStringLiteral("event")).toString() ==
                QStringLiteral("idle_memory_reclaim_completed"),
            "idle-memory reclaim trace reported an unexpected event");
    require(record.value(QStringLiteral("trim_working_set")).toBool() == expectedWorkingSetTrim,
            "idle-memory reclaim trace reported the wrong trim mode");
    require(record.value(QStringLiteral("success")).toBool(),
            "idle-memory reclaim did not complete successfully");
    const qint64 attemptCount = record.value(QStringLiteral("attempt_count")).toInteger(-1);
    require(expectedWorkingSetTrim ? attemptCount >= 1 : attemptCount == 0,
            "idle-memory reclaim trace reported an invalid attempt count");
}

void validatePinRecord(const QJsonObject& record) {
    require(record.value(QStringLiteral("scenario")).toString() ==
                QStringLiteral("normal-selection"),
            "pin trace scenario was not normal-selection");
    require(record.value(QStringLiteral("width")).toInteger() == kSelectionPixels &&
                record.value(QStringLiteral("height")).toInteger() == kSelectionPixels,
            "pin trace did not report an exact 800x800 selection");
    const QJsonObject milestones = record.value(QStringLiteral("milestones_ns")).toObject();
    require(milestones.contains(QStringLiteral("window.native_paint_synchronized")),
            "pin trace did not reach synchronized native paint");
    require(record.value(QStringLiteral("success")).toBool(),
            "fixed screenshot pin operation failed");
}

QJsonObject waitForPinTrace(const QString& path, const ChildProcess& process, qsizetype& cursor,
                            int timeoutMilliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (process.alive() && std::chrono::steady_clock::now() < deadline) {
        const QVector<QJsonObject> records = readJsonLines(path);
        while (cursor < records.size()) {
            const QJsonObject record = records.at(cursor++);
            if (!record.contains(QStringLiteral("success"))) {
                continue;
            }
            validatePinRecord(record);
            return record;
        }
        std::this_thread::sleep_for(25ms);
    }
    throw std::runtime_error("pin trace did not report a completed fixed screenshot");
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
            return {static_cast<qint64>(result), PrivateWorkingSetMethod::QueryWorkingSet};
        }
        require(GetLastError() == ERROR_BAD_LENGTH, "QueryWorkingSet failed");
        capacity *= 2;
    }
    throw std::runtime_error("QueryWorkingSet buffer did not converge");
}

PrivateWorkingSetSample privateWorkingSet(HANDLE process) {
    require(process != nullptr, "process handle is unavailable");
#if defined(NTDDI_VERSION) && defined(NTDDI_WIN10_CU) && (NTDDI_VERSION >= NTDDI_WIN10_CU)
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters),
                             sizeof(counters)) != FALSE &&
        counters.PrivateWorkingSetSize > 0) {
        require(counters.PrivateWorkingSetSize <=
                    static_cast<SIZE_T>(std::numeric_limits<qint64>::max()),
                "private working set exceeds the supported range");
        return {static_cast<qint64>(counters.PrivateWorkingSetSize),
                PrivateWorkingSetMethod::ProcessMemoryCountersEx2};
    }
#endif
    return queryWorkingSetPrivateBytes(process);
}

struct StableMemorySample {
    qint64 bytes = 0;
    qint64 peakBytes = 0;
    qint64 minimumBytes = 0;
    qint64 maximumBytes = 0;
    qint64 rangeBytes = 0;
    qint64 elapsedMilliseconds = 0;
    PrivateWorkingSetMethod method = PrivateWorkingSetMethod::QueryWorkingSet;
};

bool stableWindow(const std::deque<qint64>& values, int requiredSamples, qint64 maximumRangeBytes) {
    if (requiredSamples <= 0 || values.size() != static_cast<size_t>(requiredSamples)) {
        return false;
    }
    const auto [minimum, maximum] = std::minmax_element(values.cbegin(), values.cend());
    return *maximum - *minimum < maximumRangeBytes;
}

qint64 medianValue(std::deque<qint64> values) {
    require(!values.empty(), "cannot calculate the median of an empty memory window");
    std::sort(values.begin(), values.end());
    const size_t midpoint = values.size() / 2;
    if ((values.size() % 2) != 0) {
        return values.at(midpoint);
    }
    const qint64 lower = values.at(midpoint - 1);
    const qint64 upper = values.at(midpoint);
    return lower + (upper - lower) / 2;
}

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
        method = current.method;
        peakBytes = std::max(peakBytes, current.bytes);
        window.push_back(current.bytes);
        while (window.size() > static_cast<size_t>(windowSamples)) {
            window.pop_front();
        }

        const auto now = std::chrono::steady_clock::now();
        const qint64 elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        if (elapsed >= minimumWaitMilliseconds &&
            stableWindow(window, windowSamples, maximumRangeBytes)) {
            const auto [minimum, maximum] = std::minmax_element(window.cbegin(), window.cend());
            return {medianValue(window), peakBytes, *minimum, *maximum,
                    *maximum - *minimum, elapsed,   method};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMilliseconds));
    }
    throw std::runtime_error(process.alive() ? "private working set did not stabilize"
                                             : "snow_shot exited while memory was stabilizing");
}

struct MonitorInfo {
    RECT bounds{};
    QString deviceName;
};

QVector<MonitorInfo> monitorInfo() {
    QVector<MonitorInfo> result;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) == FALSE) {
                return FALSE;
            }
            static_cast<QVector<MonitorInfo>*>(reinterpret_cast<void*>(data))
                ->push_back({info.rcMonitor, QString::fromWCharArray(info.szDevice)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

enum class BenchmarkScenario {
    MainInterface,
    ScreenshotWindow,
    PinToScreen,
    ScreenRecording,
    RightClickMenu,
};

struct ScenarioMetadata {
    QString commandLineName;
    QString reportName;
    QString activeStage;
    QString closedStage;
};

const ScenarioMetadata& scenarioMetadata(BenchmarkScenario scenario) {
    static const ScenarioMetadata mainInterface{
        QStringLiteral("main-interface"), QStringLiteral("main_interface"),
        QStringLiteral("main_interface"), QStringLiteral("main_interface_closed")};
    static const ScenarioMetadata screenshotWindow{
        QStringLiteral("screenshot-window"), QStringLiteral("screenshot_window"),
        QStringLiteral("screenshot_window"), QStringLiteral("screenshot_window_closed")};
    static const ScenarioMetadata pinToScreen{
        QStringLiteral("pin-to-screen"), QStringLiteral("pin_to_screen"),
        QStringLiteral("pin_to_screen"), QStringLiteral("pin_to_screen_closed")};
    static const ScenarioMetadata screenRecording{
        QStringLiteral("screen-recording"), QStringLiteral("screen_recording"),
        QStringLiteral("screen_recording"), QStringLiteral("screen_recording_closed")};
    static const ScenarioMetadata rightClickMenu{
        QStringLiteral("right-click-menu"), QStringLiteral("right_click_menu"),
        QStringLiteral("right_click_menu"), QStringLiteral("right_click_menu_closed")};
    switch (scenario) {
    case BenchmarkScenario::MainInterface:
        return mainInterface;
    case BenchmarkScenario::ScreenshotWindow:
        return screenshotWindow;
    case BenchmarkScenario::PinToScreen:
        return pinToScreen;
    case BenchmarkScenario::ScreenRecording:
        return screenRecording;
    case BenchmarkScenario::RightClickMenu:
        return rightClickMenu;
    }
    throw std::runtime_error("unknown memory benchmark scenario");
}

BenchmarkScenario benchmarkScenario(const QString& name) {
    const BenchmarkScenario scenarios[]{BenchmarkScenario::MainInterface,
                                        BenchmarkScenario::ScreenshotWindow,
                                        BenchmarkScenario::PinToScreen,
                                        BenchmarkScenario::ScreenRecording,
                                        BenchmarkScenario::RightClickMenu};
    for (const BenchmarkScenario scenario : scenarios) {
        const ScenarioMetadata& metadata = scenarioMetadata(scenario);
        if (metadata.commandLineName == name ||
            (scenario == BenchmarkScenario::ScreenshotWindow &&
             name == QStringLiteral("fixed-screenshot")) ||
            (scenario == BenchmarkScenario::RightClickMenu &&
             name == QStringLiteral("tray-menu"))) {
            return scenario;
        }
    }
    throw std::runtime_error(
        QStringLiteral("unsupported --scenario '%1'; expected main-interface, screenshot-window, "
                       "pin-to-screen, screen-recording, or right-click-menu")
            .arg(name)
            .toStdString());
}

struct StageValues {
    StableMemorySample coldStart;
    StableMemorySample active;
    StableMemorySample closed;
};

struct BenchmarkConfiguration {
    QString appPath;
    QString outputDirectory;
    int samples = 10;
    int screenIndex = 0;
    int pollMilliseconds = 250;
    int coldStartMinimumWaitMilliseconds = 20000;
    int stageMinimumWaitMilliseconds = 5000;
    int stabilityWindowSamples = 20;
    qint64 stabilityRangeBytes = kBytesPerMebibyte;
    qint64 scenarioReclaimToleranceBytes = 3 * kBytesPerMebibyte;
    int timeoutMilliseconds = 90000;
    BenchmarkScenario scenario = BenchmarkScenario::ScreenshotWindow;
};

QStringList benchmarkStageOrder(BenchmarkScenario scenario) {
    const ScenarioMetadata& metadata = scenarioMetadata(scenario);
    return {QStringLiteral("cold_start"), metadata.activeStage, metadata.closedStage};
}

QJsonObject statistics(QVector<double> values) {
    if (values.isEmpty()) {
        return {};
    }
    std::sort(values.begin(), values.end());
    const auto percentile = [&values](double probability) {
        const qsizetype index = std::min(
            values.size() - 1, static_cast<qsizetype>(std::ceil(probability * values.size()) - 1));
        return values.at(std::max<qsizetype>(0, index));
    };
    const double mean = std::accumulate(values.cbegin(), values.cend(), 0.0) / values.size();
    double variance = 0.0;
    for (const double value : values) {
        variance += (value - mean) * (value - mean);
    }
    return {{QStringLiteral("count"), values.size()},
            {QStringLiteral("min_mib"), values.first()},
            {QStringLiteral("mean_mib"), mean},
            {QStringLiteral("p50_mib"), percentile(0.50)},
            {QStringLiteral("p90_mib"), percentile(0.90)},
            {QStringLiteral("p95_mib"), percentile(0.95)},
            {QStringLiteral("max_mib"), values.last()},
            {QStringLiteral("stddev_mib"), std::sqrt(variance / values.size())}};
}

QJsonObject stageObject(const StableMemorySample& sample) {
    return {{QStringLiteral("bytes"), sample.bytes},
            {QStringLiteral("mib"), static_cast<double>(sample.bytes) / kBytesPerMebibyte},
            {QStringLiteral("representative"), QStringLiteral("median_of_stability_window")},
            {QStringLiteral("peak_bytes"), sample.peakBytes},
            {QStringLiteral("stability_min_bytes"), sample.minimumBytes},
            {QStringLiteral("stability_max_bytes"), sample.maximumBytes},
            {QStringLiteral("stability_range_bytes"), sample.rangeBytes},
            {QStringLiteral("convergence_ms"), sample.elapsedMilliseconds},
            {QStringLiteral("method"),
             QString::fromLatin1(privateWorkingSetMethodName(sample.method))}};
}

QJsonObject scenarioReclaimComparisonForBounds(const QString& scenario, qint64 coldStartBytes,
                                               qint64 postScenarioBytes,
                                               qint64 coldStartLowerBoundBytes,
                                               qint64 postScenarioUpperBoundBytes,
                                               qint64 toleranceBytes) {
    require(!scenario.isEmpty(), "memory comparison scenario must not be empty");
    require(coldStartBytes >= 0 && postScenarioBytes >= 0 && coldStartLowerBoundBytes >= 0 &&
                postScenarioUpperBoundBytes >= 0 && toleranceBytes >= 0,
            "memory comparison inputs must be nonnegative");
    const qint64 deltaBytes = postScenarioBytes - coldStartBytes;
    const qint64 excessBytes = std::max<qint64>(0, deltaBytes);
    const qint64 acceptanceDeltaBytes = postScenarioUpperBoundBytes - coldStartLowerBoundBytes;
    const qint64 limitBytes =
        coldStartLowerBoundBytes > std::numeric_limits<qint64>::max() - toleranceBytes
            ? std::numeric_limits<qint64>::max()
            : coldStartLowerBoundBytes + toleranceBytes;
    const bool withinUpperBound = postScenarioUpperBoundBytes <= limitBytes;
    return {
        {QStringLiteral("scenario"), scenario},
        {QStringLiteral("comparison_kind"),
         QStringLiteral("one_sided_upper_bound_using_stability_bounds")},
        {QStringLiteral("criterion"),
         QStringLiteral("post_scenario_stability_max_bytes <= "
                        "cold_start_stability_min_bytes + tolerance_bytes")},
        {QStringLiteral("representative"), QStringLiteral("median_of_stability_window")},
        {QStringLiteral("cold_start_bytes"), coldStartBytes},
        {QStringLiteral("post_scenario_bytes"), postScenarioBytes},
        {QStringLiteral("delta_bytes"), deltaBytes},
        {QStringLiteral("delta_mib"), static_cast<double>(deltaBytes) / kBytesPerMebibyte},
        {QStringLiteral("excess_bytes"), excessBytes},
        {QStringLiteral("excess_mib"), static_cast<double>(excessBytes) / kBytesPerMebibyte},
        {QStringLiteral("acceptance_cold_start_bytes"), coldStartLowerBoundBytes},
        {QStringLiteral("acceptance_post_scenario_bytes"), postScenarioUpperBoundBytes},
        {QStringLiteral("acceptance_delta_bytes"), acceptanceDeltaBytes},
        {QStringLiteral("acceptance_delta_mib"),
         static_cast<double>(acceptanceDeltaBytes) / kBytesPerMebibyte},
        {QStringLiteral("post_scenario_to_cold_ratio"),
         coldStartBytes > 0 ? static_cast<double>(postScenarioBytes) / coldStartBytes : 0.0},
        {QStringLiteral("tolerance_bytes"), toleranceBytes},
        {QStringLiteral("tolerance_mib"), static_cast<double>(toleranceBytes) / kBytesPerMebibyte},
        {QStringLiteral("limit_bytes"), limitBytes},
        {QStringLiteral("within_upper_bound"), withinUpperBound},
        {QStringLiteral("within_tolerance"), withinUpperBound}};
}

QJsonObject scenarioReclaimComparison(const QString& scenario, qint64 coldStartBytes,
                                      qint64 postScenarioBytes, qint64 toleranceBytes) {
    return scenarioReclaimComparisonForBounds(scenario, coldStartBytes, postScenarioBytes,
                                              coldStartBytes, postScenarioBytes, toleranceBytes);
}

QJsonObject scenarioReclaimComparison(const QString& scenario,
                                      const StableMemorySample& coldStart,
                                      const StableMemorySample& postScenario,
                                      qint64 toleranceBytes) {
    return scenarioReclaimComparisonForBounds(
        scenario, coldStart.bytes, postScenario.bytes, coldStart.minimumBytes,
        postScenario.maximumBytes, toleranceBytes);
}

QJsonObject environmentReport(const QString& appPath, const MonitorInfo& monitor) {
    QJsonObject monitorReport;
    monitorReport.insert(QStringLiteral("device"), monitor.deviceName);
    monitorReport.insert(QStringLiteral("x"), static_cast<qint64>(monitor.bounds.left));
    monitorReport.insert(QStringLiteral("y"), static_cast<qint64>(monitor.bounds.top));
    monitorReport.insert(QStringLiteral("width"),
                         static_cast<qint64>(monitor.bounds.right - monitor.bounds.left));
    monitorReport.insert(QStringLiteral("height"),
                         static_cast<qint64>(monitor.bounds.bottom - monitor.bounds.top));

    QJsonObject result;
    result.insert(QStringLiteral("os"), QSysInfo::prettyProductName());
    result.insert(QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture());
    result.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    result.insert(QStringLiteral("application"), QDir::toNativeSeparators(appPath));
    const QFileInfo applicationInfo(appPath);
    result.insert(QStringLiteral("application_size_bytes"), applicationInfo.size());
    result.insert(QStringLiteral("application_last_modified_utc"),
                  applicationInfo.lastModified().toUTC().toString(Qt::ISODateWithMs));

    const bool commitWasProvided = qEnvironmentVariableIsSet("SNOW_SHOT_PERF_GIT_COMMIT");
    const QString commit = qEnvironmentVariable("SNOW_SHOT_PERF_GIT_COMMIT").trimmed();
    result.insert(QStringLiteral("git_commit"), commitWasProvided && !commit.isEmpty()
                                                    ? QJsonValue(commit)
                                                    : QJsonValue(QJsonValue::Null));

    QJsonValue dirty = QJsonValue(QJsonValue::Null);
    if (qEnvironmentVariableIsSet("SNOW_SHOT_PERF_GIT_DIRTY")) {
        bool parsed = false;
        const int value = qEnvironmentVariable("SNOW_SHOT_PERF_GIT_DIRTY").toInt(&parsed);
        if (parsed && (value == 0 || value == 1)) {
            dirty = QJsonValue(value == 1);
        }
    }
    result.insert(QStringLiteral("git_dirty"), dirty);
    result.insert(QStringLiteral("monitor"), monitorReport);
    return result;
}

QString reportHtml(const QJsonObject& report) {
    const QJsonObject metrics = report.value(QStringLiteral("metrics")).toObject();
    const QJsonObject reclaimSummary =
        report.value(QStringLiteral("scenario_reclaim_vs_cold_start")).toObject();
    const QJsonObject deltaStatistics =
        reclaimSummary.value(QStringLiteral("delta_mib_statistics")).toObject();
    QString rows;
    QStringList names;
    for (const QJsonValue& value : report.value(QStringLiteral("stage_order")).toArray()) {
        if (!value.toString().isEmpty()) {
            names.push_back(value.toString());
        }
    }
    if (names.isEmpty()) {
        names = benchmarkStageOrder(BenchmarkScenario::ScreenshotWindow);
    }
    for (const QString& name : names) {
        const QJsonObject metric = metrics.value(name).toObject();
        if (metric.isEmpty()) {
            continue;
        }
        rows += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td>"
                               "<td>%6</td><td>%7</td></tr>")
                    .arg(name)
                    .arg(metric.value(QStringLiteral("min_mib")).toDouble())
                    .arg(metric.value(QStringLiteral("mean_mib")).toDouble())
                    .arg(metric.value(QStringLiteral("p50_mib")).toDouble())
                    .arg(metric.value(QStringLiteral("p90_mib")).toDouble())
                    .arg(metric.value(QStringLiteral("p95_mib")).toDouble())
                    .arg(metric.value(QStringLiteral("max_mib")).toDouble());
    }
    if (rows.isEmpty()) {
        rows = QStringLiteral("<tr><td colspan=7>No successful samples</td></tr>");
    }
    const QString embedded =
        QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented)).toHtmlEscaped();
    const bool complete = reclaimSummary.value(QStringLiteral("complete_sample_set")).toBool() &&
                          reclaimSummary.value(QStringLiteral("complete_comparison_set")).toBool();
    const QString acceptance =
        reclaimSummary.value(QStringLiteral("benchmark_passed")).toBool()
            ? QStringLiteral("PASS")
            : (complete ? QStringLiteral("FAIL") : QStringLiteral("INCOMPLETE"));
    const QString p95Delta =
        deltaStatistics.isEmpty()
            ? QStringLiteral("n/a")
            : QString::number(deltaStatistics.value(QStringLiteral("p95_mib")).toDouble()) +
                  QStringLiteral(" MiB");
    return QStringLiteral(
               "<!doctype html><html><head><meta charset=utf-8><title>Snow Shot memory footprint"
               "</title><style>body{font:14px system-ui;margin:32px;color:#202124}table{border-"
               "collapse:collapse;margin:20px 0}th,td{border:1px solid #dadce0;padding:8px 12px}"
               "th{text-align:left;background:#f8f9fa}pre{background:#f8f9fa;padding:16px;"
               "overflow:auto}</style></head><body><h1>Screenshot memory footprint</h1>"
               "<p><strong>Benchmark: %1</strong> (%2/%3 requested samples completed; %4/%5 "
               "post-scenario reclaim checks were no more than %6 MiB above cold start using "
               "conservative stability-window bounds; P95 signed median delta %7)</p>"
               "<table><thead><tr><th>Stage</th><th>Min</th><th>Mean</th><th>P50</th><th>P90</th>"
               "<th>P95</th><th>Max</th></tr></thead><tbody>%8</tbody></table><pre>%9</pre>"
               "</body></html>")
        .arg(acceptance)
        .arg(reclaimSummary.value(QStringLiteral("sample_count")).toInteger())
        .arg(reclaimSummary.value(QStringLiteral("requested_sample_count")).toInteger())
        .arg(reclaimSummary.value(QStringLiteral("within_upper_bound_comparison_count")).toInteger())
        .arg(reclaimSummary.value(QStringLiteral("requested_comparison_count")).toInteger())
        .arg(reclaimSummary.value(QStringLiteral("tolerance_mib")).toDouble())
        .arg(p95Delta)
        .arg(rows, embedded);
}

void addBenchmarkCommandLineOptions(QCommandLineParser& parser) {
    parser.setApplicationDescription(
        QStringLiteral("Native Windows screenshot private working-set benchmark"));
    parser.addHelpOption();
    parser.addOption(
        {QStringLiteral("app"), QStringLiteral("snow_shot executable"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("artifact directory"),
                      QStringLiteral("directory"),
                      QStringLiteral("build/screenshot-memory-footprint/latest")});
    parser.addOption({QStringLiteral("samples"), QStringLiteral("fresh-process samples"),
                      QStringLiteral("count"), QStringLiteral("10")});
    parser.addOption({QStringLiteral("scenario"),
                      QStringLiteral("independent scenario: main-interface, screenshot-window, "
                                     "pin-to-screen, screen-recording, or right-click-menu"),
                      QStringLiteral("name"), QStringLiteral("screenshot-window")});
    parser.addOption({QStringLiteral("screen-index"), QStringLiteral("monitor index"),
                      QStringLiteral("index"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("poll-ms"), QStringLiteral("memory polling interval"),
                      QStringLiteral("milliseconds"), QStringLiteral("250")});
    parser.addOption({QStringLiteral("cold-start-min-wait-ms"),
                      QStringLiteral("minimum cold-start stabilization time"),
                      QStringLiteral("milliseconds"), QStringLiteral("20000")});
    parser.addOption({QStringLiteral("stage-min-wait-ms"),
                      QStringLiteral("minimum subsequent-stage stabilization time"),
                      QStringLiteral("milliseconds"), QStringLiteral("5000")});
    parser.addOption({QStringLiteral("stability-window"),
                      QStringLiteral("memory samples required for convergence"),
                      QStringLiteral("count"), QStringLiteral("20")});
    parser.addOption({QStringLiteral("stability-range-kib"),
                      QStringLiteral("maximum private working-set range"),
                      QStringLiteral("kibibytes"), QStringLiteral("1024")});
    parser.addOption({QStringLiteral("final-idle-tolerance-kib"),
                      QStringLiteral("maximum post-scenario excess over cold start"),
                      QStringLiteral("kibibytes"), QStringLiteral("3072")});
    parser.addOption({QStringLiteral("timeout-ms"), QStringLiteral("per-stage timeout"),
                      QStringLiteral("milliseconds"), QStringLiteral("90000")});
    parser.addOption({QStringLiteral("self-test"), QStringLiteral("run benchmark self-tests")});
}

qint64 nonnegativeKibibytes(const QCommandLineParser& parser, const QString& option,
                            const char* errorMessage) {
    bool parsed = false;
    const qint64 kibibytes = parser.value(option).toLongLong(&parsed);
    require(parsed && kibibytes >= 0 &&
                kibibytes <= std::numeric_limits<qint64>::max() / kBytesPerKibibyte,
            errorMessage);
    return kibibytes * kBytesPerKibibyte;
}

int integerOption(const QCommandLineParser& parser, const QString& option) {
    bool parsed = false;
    const int value = parser.value(option).toInt(&parsed);
    if (!parsed) {
        throw std::runtime_error(
            QStringLiteral("--%1 is invalid or outside the supported integer range")
                .arg(option)
                .toStdString());
    }
    return value;
}

BenchmarkConfiguration configurationFromParser(const QCommandLineParser& parser) {
    BenchmarkConfiguration configuration;
    configuration.appPath = QFileInfo(parser.value(QStringLiteral("app"))).absoluteFilePath();
    configuration.outputDirectory = QDir::cleanPath(parser.value(QStringLiteral("output")));
    configuration.samples = integerOption(parser, QStringLiteral("samples"));
    configuration.scenario = benchmarkScenario(parser.value(QStringLiteral("scenario")));
    configuration.screenIndex = integerOption(parser, QStringLiteral("screen-index"));
    configuration.pollMilliseconds = integerOption(parser, QStringLiteral("poll-ms"));
    configuration.coldStartMinimumWaitMilliseconds =
        integerOption(parser, QStringLiteral("cold-start-min-wait-ms"));
    configuration.stageMinimumWaitMilliseconds =
        integerOption(parser, QStringLiteral("stage-min-wait-ms"));
    configuration.stabilityWindowSamples =
        integerOption(parser, QStringLiteral("stability-window"));
    configuration.stabilityRangeBytes = nonnegativeKibibytes(
        parser, QStringLiteral("stability-range-kib"), "stability range is invalid or too large");
    configuration.scenarioReclaimToleranceBytes =
        nonnegativeKibibytes(parser, QStringLiteral("final-idle-tolerance-kib"),
                             "scenario reclaim tolerance is invalid or too large");
    configuration.timeoutMilliseconds = integerOption(parser, QStringLiteral("timeout-ms"));
    return configuration;
}

void removeStaleTrace(const QString& path, const char* errorMessage) {
    if (QFile::exists(path)) {
        require(QFile::remove(path), errorMessage);
    }
    require(!QFile::exists(path), errorMessage);
}

void removeStaleSampleTraces(const QString& outputDirectory) {
    QDir traceDirectory(QDir(outputDirectory).filePath(QStringLiteral("app-traces")));
    if (!traceDirectory.exists()) {
        return;
    }
    const QStringList patterns{QStringLiteral("sample-*-lifecycle.jsonl"),
                               QStringLiteral("sample-*-pin.jsonl")};
    const QStringList staleFiles = traceDirectory.entryList(patterns, QDir::Files);
    for (const QString& fileName : staleFiles) {
        require(traceDirectory.remove(fileName), "could not remove a stale app trace");
    }
}

void validateConfiguration(const BenchmarkConfiguration& configuration,
                           const QVector<MonitorInfo>& monitorsList) {
    require(QFileInfo(configuration.appPath).isFile(), "snow_shot executable was not found");
    require(configuration.samples > 0, "samples must be positive");
    require(configuration.screenIndex >= 0 && configuration.screenIndex < monitorsList.size(),
            "screen index is unavailable");
    require(monitorSupportsFixedSelectionDrag(monitorsList.at(configuration.screenIndex).bounds),
            "selected monitor cannot contain the required 800x800 drag");
    require(configuration.pollMilliseconds > 0, "poll interval must be positive");
    require(configuration.coldStartMinimumWaitMilliseconds >= 0,
            "cold-start minimum wait must be nonnegative");
    require(configuration.stageMinimumWaitMilliseconds >= 0,
            "stage minimum wait must be nonnegative");
    require(configuration.stabilityWindowSamples > 1,
            "stability window must contain at least two samples");
    require(configuration.stabilityRangeBytes > 0, "stability range must be positive");
    require(configuration.scenarioReclaimToleranceBytes >= 0,
            "scenario reclaim tolerance must be nonnegative");
    require(configuration.timeoutMilliseconds > configuration.coldStartMinimumWaitMilliseconds &&
                configuration.timeoutMilliseconds > configuration.stageMinimumWaitMilliseconds,
            "timeout must exceed both minimum waits");
}

void forwardCommand(const BenchmarkConfiguration& configuration, const QStringList& baseArguments,
                    const QString& command) {
    ChildProcess request;
    QStringList arguments = baseArguments;
    arguments.push_back(command);
    if (!request.start(configuration.appPath, arguments)) {
        throw childStartError("could not issue stage command", request);
    }
    require(request.waitForExit(10000), "stage command helper did not exit");
    require(request.exitCode() == 0, "stage command helper failed");
}

QJsonObject runSample(const BenchmarkConfiguration& configuration, const MonitorInfo& monitor,
                      int iteration, IUIAutomation& automation) {
    const QString traceDirectory =
        QDir(configuration.outputDirectory).filePath(QStringLiteral("app-traces"));
    require(QDir().mkpath(traceDirectory), "could not create app trace directory");
    const QString lifecyclePath = QDir(traceDirectory)
                                      .filePath(QStringLiteral("sample-%1-lifecycle.jsonl")
                                                    .arg(iteration, 3, 10, QLatin1Char('0')));
    const QString pinPath =
        QDir(traceDirectory)
            .filePath(
                QStringLiteral("sample-%1-pin.jsonl").arg(iteration, 3, 10, QLatin1Char('0')));
    removeStaleTrace(lifecyclePath, "could not remove the previous lifecycle trace");
    // Retain the pin trace file and report aliases for consumers of the existing benchmark
    // schema. The pin-to-screen stage below still records its native-window milestones.
    removeStaleTrace(pinPath, "could not remove the previous pin trace");
    const ScopedEnvironmentVariable lifecycleEnvironment(
        QByteArrayLiteral("SNOW_SHOT_SCREENSHOT_LIFECYCLE_PERF_TRACE"),
        QFile::encodeName(lifecyclePath));
    const ScopedEnvironmentVariable pinEnvironment(QByteArrayLiteral("SNOW_SHOT_PIN_PERF_TRACE"),
                                                   QFile::encodeName(pinPath));

    const QString instanceId = QStringLiteral("memory-%1-%2-%3")
                                   .arg(GetCurrentProcessId())
                                   .arg(iteration)
                                   .arg(QDateTime::currentMSecsSinceEpoch() % 1000000);
    const QStringList baseArguments{QStringLiteral("--e2e-allow-overlay-capture"),
                                    QStringLiteral("--e2e-instance-id=%1").arg(instanceId)};
    ChildProcess primary;
    if (!primary.start(configuration.appPath, baseArguments)) {
        throw childStartError("could not start snow_shot", primary);
    }
    qsizetype lifecycleCursor = 0;
    static_cast<void>(waitForLifecycleEvent(lifecyclePath, primary.processId(),
                                            QStringLiteral("app_ready"), lifecycleCursor, primary,
                                            configuration.timeoutMilliseconds));

    StageValues stages;
    stages.coldStart =
        waitForStableMemory(primary, configuration.coldStartMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);
    const ScenarioMetadata& metadata = scenarioMetadata(configuration.scenario);
    QJsonObject scenarioComparison;
    QJsonObject scenarioTrace;

    if (configuration.scenario == BenchmarkScenario::MainInterface) {
    // Exercise the main-window lifecycle, including its two heaviest lazy pages. Post
    // WM_CLOSE directly to the validated HWND so unrelated foreground-window changes cannot
    // redirect the close command.
    forwardCommand(configuration, baseArguments, QStringLiteral("--e2e-open-main-interface"));
    ComPtr<IUIAutomationElement> mainWindow =
        waitForVisibleElement(automation, primary.processId(), QStringLiteral("snowShotMainWindow"),
                              primary, configuration.timeoutMilliseconds);
    require(mainWindow.get() != nullptr, "main interface did not become visible");
    forwardCommand(configuration, baseArguments,
                   QStringLiteral("--e2e-open-screenshot-history"));
    require(waitForVisibleElement(automation, primary.processId(),
                                  QStringLiteral("screenshotHistoryPage"), primary,
                                  configuration.timeoutMilliseconds)
                    .get() != nullptr,
            "screenshot history did not become visible");
    forwardCommand(configuration, baseArguments,
                   QStringLiteral("--e2e-open-interface-settings"));
    require(waitForVisibleElement(automation, primary.processId(),
                                  QStringLiteral("settings-page-interface-settings"), primary,
                                  configuration.timeoutMilliseconds)
                    .get() != nullptr,
            "interface settings did not become visible");
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("screenshotHistoryPage"), primary,
                                      configuration.timeoutMilliseconds),
            "screenshot history remained visible after opening interface settings");
    stages.active =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);
    advanceLifecycleCursor(lifecyclePath, lifecycleCursor);
    closeNativeWindow(*mainWindow.get(), primary.processId());
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("snowShotMainWindow"), primary,
                                      configuration.timeoutMilliseconds),
            "main interface did not close");
    const QJsonObject mainInterfaceReclaim = waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("idle_memory_reclaim_completed"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds);
    validateIdleMemoryReclaim(mainInterfaceReclaim, false);
    stages.closed =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);
    scenarioComparison = scenarioReclaimComparison(metadata.reportName, stages.coldStart,
                                                   stages.closed,
                                                   configuration.scenarioReclaimToleranceBytes);
    scenarioTrace = mainInterfaceReclaim;

    } else if (configuration.scenario == BenchmarkScenario::ScreenshotWindow) {
    // Exercise the normal screenshot window through every drawing-tool path, then enter a
    // scrolling capture before ending the screenshot. Keep the selection fixed so memory samples
    // remain comparable between iterations.
    positionCursorForCapture(monitor.bounds);
    forwardCommand(configuration, baseArguments, QStringLiteral("--e2e-start-screenshot"));
    static_cast<void>(waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("first_capture_presented"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds));
    static_cast<void>(waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("capture_interaction_ready"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds));
    dragFixedSelection(monitor.bounds);
    const char* const drawingToolError = "screenshot drawing tool did not become visible";
    const auto clickDrawingTool = [&](const QString& automationId) {
        clickVisibleElement(automation, primary.processId(), automationId, primary,
                            configuration.timeoutMilliseconds, drawingToolError);
    };
    const auto clickGroupedDrawingTool = [&](const QString& triggerId,
                                             const QString& optionId) {
        clickDrawingTool(triggerId);
        clickVisibleElement(automation, primary.processId(), optionId, primary,
                            configuration.timeoutMilliseconds,
                            "screenshot drawing-tool option did not become visible");
    };

    // Shape, pen, text, serial number, filter, eraser, and watermark are standalone toolbar
    // entries. Arrow/line and highlighter/spotlight are configured as two-option popovers, so
    // reopen each popover for both options to exercise the actual tool switch.
    const QStringList standaloneDrawingTools{
        QStringLiteral("screenshotSelectButton"), QStringLiteral("screenshotShapeButton"),
        QStringLiteral("screenshotFreeDrawButton"), QStringLiteral("screenshotTextButton"),
        QStringLiteral("screenshotSerialNumberButton"), QStringLiteral("screenshotFilterButton"),
        QStringLiteral("screenshotEraserButton"), QStringLiteral("screenshotWatermarkButton")};
    for (const QString& buttonId : standaloneDrawingTools) {
        clickDrawingTool(buttonId);
    }
    clickGroupedDrawingTool(QStringLiteral("screenshotArrowLineButton"),
                            QStringLiteral("screenshotDrawingToolGroupOption-arrow"));
    clickGroupedDrawingTool(QStringLiteral("screenshotArrowLineButton"),
                            QStringLiteral("screenshotDrawingToolGroupOption-line"));
    clickGroupedDrawingTool(QStringLiteral("screenshotHighlightButton"),
                            QStringLiteral("screenshotDrawingToolGroupOption-highlighter"));
    clickGroupedDrawingTool(QStringLiteral("screenshotHighlightButton"),
                            QStringLiteral("screenshotDrawingToolGroupOption-spotlight"));

    clickDrawingTool(QStringLiteral("screenshotScrollingScreenshotButton"));
    require(waitForVisibleElement(automation, primary.processId(),
                                  QStringLiteral("screenshot-scrolling-thumbnail"), primary,
                                  configuration.timeoutMilliseconds)
                    .get() != nullptr,
            "scrolling screenshot thumbnail did not become visible");

    const int monitorCenterX =
        monitor.bounds.left + (monitor.bounds.right - monitor.bounds.left) / 2;
    const int monitorCenterY =
        monitor.bounds.top + (monitor.bounds.bottom - monitor.bounds.top) / 2;
    // The scrolling overlay leaves the selected rectangle pass-through, so wheel input at its
    // center reaches the captured window and drives the native scrolling stream.
    for (int index = 0; index < 3; ++index) {
        sendMouseWheel(monitorCenterX, monitorCenterY, -WHEEL_DELTA);
        std::this_thread::sleep_for(100ms);
    }
    stages.active =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);

    clickDrawingTool(QStringLiteral("screenshotCancelButton"));
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("screenshot-scrolling-thumbnail"), primary,
                                      configuration.timeoutMilliseconds),
            "scrolling screenshot thumbnail did not close");
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("screenshotSelectionToolbarPanel"), primary,
                                      configuration.timeoutMilliseconds),
            "selection toolbar did not close after ending screenshot");
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("screenshotCancelButton"), primary,
                                      configuration.timeoutMilliseconds),
            "screenshot toolbar did not close after ending screenshot");
    static_cast<void>(waitForLifecycleEvent(lifecyclePath, primary.processId(),
                                            QStringLiteral("capture_released"), lifecycleCursor,
                                            primary, configuration.timeoutMilliseconds));
    const QJsonObject screenshotReclaim = waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("idle_memory_reclaim_completed"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds);
    validateIdleMemoryReclaim(screenshotReclaim, false);
    // Keep the historical stage name as a report compatibility alias. The sample now represents
    // the normal screenshot teardown, rather than a pinned-window close.
    stages.closed =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);
    scenarioComparison = scenarioReclaimComparison(metadata.reportName, stages.coldStart,
                                                   stages.closed,
                                                   configuration.scenarioReclaimToleranceBytes);
    scenarioTrace = screenshotReclaim;

    } else if (configuration.scenario == BenchmarkScenario::PinToScreen) {
    // Pin a separate 800x800 selection, open drawing mode, visit every available drawing tool,
    // confirm the edit, and close the pinned surface before measuring its reclaim checkpoint.
    positionCursorForCapture(monitor.bounds);
    forwardCommand(configuration, baseArguments, QStringLiteral("--e2e-start-fixed-screenshot"));
    static_cast<void>(waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("first_capture_presented"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds));
    static_cast<void>(waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("capture_interaction_ready"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds));
    dragFixedSelection(monitor.bounds);
    qsizetype pinCursor = 0;
    static_cast<void>(waitForPinTrace(pinPath, primary, pinCursor,
                                      configuration.timeoutMilliseconds));
    ComPtr<IUIAutomationElement> pinnedWindow = waitForVisibleElement(
        automation, primary.processId(), QStringLiteral("screenshotPinnedWindow"), primary,
        configuration.timeoutMilliseconds);
    require(pinnedWindow.get() != nullptr, "pinned window did not become visible");
    const int pinCenterX =
        monitor.bounds.left + (monitor.bounds.right - monitor.bounds.left) / 2;
    const int pinCenterY =
        monitor.bounds.top + (monitor.bounds.bottom - monitor.bounds.top) / 2;
    sendMouse(pinCenterX, pinCenterY, MOUSEEVENTF_MOVE);
    clickVisibleElement(automation, primary.processId(),
                        QStringLiteral("screenshotPinnedEditButton"), primary,
                        configuration.timeoutMilliseconds,
                        "pinned drawing button did not become visible");
    require(waitForVisibleElement(automation, primary.processId(),
                                  QStringLiteral("screenshotPinnedDrawingToolbar"), primary,
                                  configuration.timeoutMilliseconds)
                    .get() != nullptr,
            "pinned window did not enter drawing mode");
    const auto clickPinnedDrawingTool = [&](const QString& buttonId) {
        clickVisibleElement(automation, primary.processId(), buttonId, primary,
                            configuration.timeoutMilliseconds,
                            "pinned drawing tool did not become visible");
    };
    const auto clickPinnedDrawingGroupOption = [&](const QString& optionId) {
        const QString triggerId = optionId.contains(QStringLiteral("arrow")) ||
                                          optionId.contains(QStringLiteral("line"))
                                      ? QStringLiteral("screenshotArrowLineButton")
                                      : QStringLiteral("screenshotHighlightButton");
        clickVisibleElement(automation, primary.processId(), triggerId, primary,
                            configuration.timeoutMilliseconds,
                            "pinned drawing-tool group did not become visible");
        clickVisibleElement(automation, primary.processId(), optionId, primary,
                            configuration.timeoutMilliseconds,
                            "pinned drawing-tool option did not become visible");
    };
    clickPinnedDrawingTool(QStringLiteral("screenshotSelectButton"));
    clickPinnedDrawingTool(QStringLiteral("screenshotShapeButton"));
    clickPinnedDrawingGroupOption(QStringLiteral("screenshotDrawingToolGroupOption-arrow"));
    clickPinnedDrawingGroupOption(QStringLiteral("screenshotDrawingToolGroupOption-line"));
    clickPinnedDrawingTool(QStringLiteral("screenshotFreeDrawButton"));
    clickPinnedDrawingTool(QStringLiteral("screenshotTextButton"));
    clickPinnedDrawingTool(QStringLiteral("screenshotSerialNumberButton"));
    clickPinnedDrawingTool(QStringLiteral("screenshotFilterButton"));
    clickPinnedDrawingTool(QStringLiteral("screenshotEraserButton"));
    clickPinnedDrawingTool(QStringLiteral("screenshotWatermarkButton"));
    clickPinnedDrawingGroupOption(
        QStringLiteral("screenshotDrawingToolGroupOption-highlighter"));
    clickPinnedDrawingGroupOption(QStringLiteral("screenshotDrawingToolGroupOption-spotlight"));
    stages.active =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);
    clickVisibleElement(automation, primary.processId(),
                        QStringLiteral("screenshotConfirmButton"), primary,
                        configuration.timeoutMilliseconds,
                        "pinned drawing confirmation button did not become visible");
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("screenshotPinnedDrawingToolbar"), primary,
                                      configuration.timeoutMilliseconds),
            "pinned drawing mode did not confirm");
    closeNativeWindow(*pinnedWindow.get(), primary.processId());
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("screenshotPinnedWindow"), primary,
                                      configuration.timeoutMilliseconds),
            "pinned window did not close");
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("screenshotSelectionToolbarPanel"), primary,
                                      configuration.timeoutMilliseconds),
            "selection toolbar did not close after pinning");
    static_cast<void>(waitForLifecycleEvent(lifecyclePath, primary.processId(),
                                            QStringLiteral("capture_released"), lifecycleCursor,
                                            primary, configuration.timeoutMilliseconds));
    const QJsonObject pinToScreenReclaim = waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("idle_memory_reclaim_completed"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds);
    validateIdleMemoryReclaim(pinToScreenReclaim, true);
    stages.closed =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);
    scenarioComparison = scenarioReclaimComparison(metadata.reportName, stages.coldStart,
                                                   stages.closed,
                                                   configuration.scenarioReclaimToleranceBytes);
    scenarioTrace = pinToScreenReclaim;

    } else if (configuration.scenario == BenchmarkScenario::ScreenRecording) {
    // Reuse the same 800x800 screenshot gesture to enter screen recording, then exercise the
    // complete recorder lifecycle before measuring its reclaimed working set.
    positionCursorForCapture(monitor.bounds);
    advanceLifecycleCursor(lifecyclePath, lifecycleCursor);
    forwardCommand(configuration, baseArguments,
                   QStringLiteral("--e2e-start-screen-recording"));
    static_cast<void>(waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("first_capture_presented"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds));
    static_cast<void>(waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("capture_interaction_ready"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds));
    dragFixedSelection(monitor.bounds);
    require(waitForVisibleElement(automation, primary.processId(),
                                  QStringLiteral("screenRecordingAreaWindow"), primary,
                                  configuration.timeoutMilliseconds)
                    .get() != nullptr,
            "screen recording area did not become visible");
    require(waitForVisibleElement(automation, primary.processId(),
                                  QStringLiteral("screenRecordingToolbar"), primary,
                                  configuration.timeoutMilliseconds)
                    .get() != nullptr,
            "screen recording toolbar did not become visible");
    stages.active =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);

    clickVisibleElement(automation, primary.processId(),
                        QStringLiteral("screenRecordingStartButton"), primary,
                        configuration.timeoutMilliseconds,
                        "screen recording start button did not become visible");
    require(waitForVisibleElement(automation, primary.processId(),
                                  QStringLiteral("screenRecordingStopButton"), primary,
                                  configuration.timeoutMilliseconds)
                    .get() != nullptr,
            "screen recording did not start");
    std::this_thread::sleep_for(1s);
    clickVisibleElement(automation, primary.processId(),
                        QStringLiteral("screenRecordingStopButton"), primary,
                        configuration.timeoutMilliseconds,
                        "screen recording stop button did not become visible");
    require(waitForVisibleElement(automation, primary.processId(),
                                  QStringLiteral("screenRecordingStartButton"), primary,
                                  configuration.timeoutMilliseconds)
                    .get() != nullptr,
            "screen recording export did not complete");
    clickVisibleElement(automation, primary.processId(),
                        QStringLiteral("screenRecordingCloseButton"), primary,
                        configuration.timeoutMilliseconds,
                        "screen recording close button did not become visible");
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("screenRecordingToolbar"), primary,
                                      configuration.timeoutMilliseconds),
            "screen recording toolbar did not close");
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("screenRecordingAreaWindow"), primary,
                                      configuration.timeoutMilliseconds),
            "screen recording area did not close");
    static_cast<void>(waitForLifecycleEvent(lifecyclePath, primary.processId(),
                                            QStringLiteral("capture_released"), lifecycleCursor,
                                            primary, configuration.timeoutMilliseconds));
    const QJsonObject screenRecordingReclaim = waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("idle_memory_reclaim_completed"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds);
    validateIdleMemoryReclaim(screenRecordingReclaim, false);
    stages.closed =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);
    scenarioComparison = scenarioReclaimComparison(metadata.reportName, stages.coldStart,
                                                   stages.closed,
                                                   configuration.scenarioReclaimToleranceBytes);
    scenarioTrace = screenRecordingReclaim;

    } else if (configuration.scenario == BenchmarkScenario::RightClickMenu) {
    // Open the tray menu only after every screenshot surface has closed. Escape is sent after
    // focusing the menu so the hide operation is deterministic on a busy desktop.
    forwardCommand(configuration, baseArguments, QStringLiteral("--e2e-open-tray-menu"));
    ComPtr<IUIAutomationElement> trayMenu =
        waitForVisibleElement(automation, primary.processId(), QStringLiteral("systemTrayMenu"),
                              primary, configuration.timeoutMilliseconds);
    require(trayMenu.get() != nullptr, "tray menu did not become visible");
    stages.active =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);
    require(SUCCEEDED(trayMenu.get()->SetFocus()), "could not focus the tray menu");
    // Discard reclaim records from prior stages immediately before the action under test. The
    // tray is visible here, so the application cannot complete a new idle reclaim until Escape
    // hides it.
    advanceLifecycleCursor(lifecyclePath, lifecycleCursor);
    sendEscape();
    require(waitForElementToDisappear(automation, primary.processId(),
                                      QStringLiteral("systemTrayMenu"), primary,
                                      configuration.timeoutMilliseconds),
            "tray menu did not close");
    const QJsonObject rightClickMenuReclaim = waitForLifecycleEvent(
        lifecyclePath, primary.processId(), QStringLiteral("idle_memory_reclaim_completed"),
        lifecycleCursor, primary, configuration.timeoutMilliseconds);
    validateIdleMemoryReclaim(rightClickMenuReclaim, false);
    // One converged post-hide sample is both the explicit tray-menu-closed stage and the final
    // idle sample. Keeping these as separate schema fields makes the close sequence auditable
    // without adding another five-second stabilization window to every iteration.
    stages.closed =
        waitForStableMemory(primary, configuration.stageMinimumWaitMilliseconds,
                            configuration.pollMilliseconds, configuration.stabilityWindowSamples,
                            configuration.stabilityRangeBytes, configuration.timeoutMilliseconds);
    scenarioComparison = scenarioReclaimComparison(metadata.reportName, stages.coldStart,
                                                   stages.closed,
                                                   configuration.scenarioReclaimToleranceBytes);
    scenarioTrace = rightClickMenuReclaim;
    }

    const auto mib = [](qint64 bytes) { return static_cast<double>(bytes) / kBytesPerMebibyte; };
    QJsonObject monitorReport;
    monitorReport.insert(QStringLiteral("device"), monitor.deviceName);
    monitorReport.insert(QStringLiteral("x"), static_cast<qint64>(monitor.bounds.left));
    monitorReport.insert(QStringLiteral("y"), static_cast<qint64>(monitor.bounds.top));
    monitorReport.insert(QStringLiteral("width"),
                         static_cast<qint64>(monitor.bounds.right - monitor.bounds.left));
    monitorReport.insert(QStringLiteral("height"),
                         static_cast<qint64>(monitor.bounds.bottom - monitor.bounds.top));
    const QJsonObject reclaimComparisons{{metadata.reportName, scenarioComparison}};
    const QJsonObject record{
        {QStringLiteral("schema_version"), 5},
        {QStringLiteral("memory_stage_representative"),
         QStringLiteral("median_of_stability_window")},
        {QStringLiteral("acceptance_uses_conservative_stability_bounds"), true},
        {QStringLiteral("scenario"), metadata.commandLineName},
        {QStringLiteral("iteration"), iteration},
        {QStringLiteral("process_id"), static_cast<qint64>(primary.processId())},
        {QStringLiteral("stage_order"),
         QJsonArray::fromStringList(benchmarkStageOrder(configuration.scenario))},
        {QStringLiteral("cold_start"), stageObject(stages.coldStart)},
        {metadata.activeStage, stageObject(stages.active)},
        {metadata.closedStage, stageObject(stages.closed)},
        {QStringLiteral("scenario_reclaim_vs_cold_start"), reclaimComparisons},
        {QStringLiteral("final_idle_vs_cold_start"), scenarioComparison},
        {QStringLiteral("cold_start_private_working_set_bytes"), stages.coldStart.bytes},
        {metadata.activeStage + QStringLiteral("_private_working_set_bytes"), stages.active.bytes},
        {metadata.closedStage + QStringLiteral("_private_working_set_bytes"), stages.closed.bytes},
        {QStringLiteral("cold_start_private_working_set_mib"), mib(stages.coldStart.bytes)},
        {metadata.activeStage + QStringLiteral("_private_working_set_mib"),
         mib(stages.active.bytes)},
        {metadata.closedStage + QStringLiteral("_private_working_set_mib"),
         mib(stages.closed.bytes)},
        {QStringLiteral("monitor"), monitorReport},
        {QStringLiteral("traces"),
         QJsonObject{{QStringLiteral("lifecycle"), QDir::toNativeSeparators(lifecyclePath)},
                      {QStringLiteral("pin"), QDir::toNativeSeparators(pinPath)},
                      {QStringLiteral("scenario_reclaim"), scenarioTrace}}}};
    std::cout << "sample " << iteration << ": cold_start=" << mib(stages.coldStart.bytes)
               << " MiB, " << metadata.reportName.toStdString() << '=' << mib(stages.active.bytes)
               << " MiB, closed=" << mib(stages.closed.bytes) << " MiB, reclaimed="
               << (scenarioComparison.value(QStringLiteral("within_upper_bound")).toBool()
                       ? "yes" : "no")
               << '\n';
    require(primary.stop(), "could not terminate snow_shot after the sample");
    return record;
}

QJsonObject metricFor(const QVector<QJsonObject>& records, const QString& stage) {
    QVector<double> values;
    for (const QJsonObject& record : records) {
        values.push_back(record.value(stage).toObject().value(QStringLiteral("mib")).toDouble());
    }
    return statistics(std::move(values));
}

QJsonObject scenarioComparisonSummary(const QVector<QJsonObject>& records,
                                      const QString& scenario, qint64 toleranceBytes,
                                      qsizetype requestedSampleCount) {
    require(!scenario.isEmpty(), "summary scenario must not be empty");
    require(requestedSampleCount > 0, "requested sample count must be positive");
    QVector<double> deltas;
    deltas.reserve(records.size());
    qsizetype withinUpperBound = 0;
    for (const QJsonObject& record : records) {
        const QJsonObject comparison =
            record.value(QStringLiteral("scenario_reclaim_vs_cold_start"))
                .toObject()
                .value(scenario)
                .toObject();
        if (comparison.isEmpty()) {
            continue;
        }
        deltas.push_back(comparison.value(QStringLiteral("delta_mib")).toDouble());
        if (comparison.value(QStringLiteral("within_upper_bound")).toBool()) {
            ++withinUpperBound;
        }
    }
    const qsizetype comparisonCount = deltas.size();
    const bool completeSampleSet = comparisonCount == requestedSampleCount;
    const bool allSuccessfulSamplesWithinUpperBound =
        comparisonCount > 0 && withinUpperBound == comparisonCount;
    const bool benchmarkPassed = completeSampleSet && allSuccessfulSamplesWithinUpperBound;
    return {
        {QStringLiteral("scenario"), scenario},
        {QStringLiteral("comparison_kind"),
         QStringLiteral("one_sided_upper_bound_using_stability_bounds")},
        {QStringLiteral("criterion"),
         QStringLiteral("post_scenario_stability_max_bytes <= "
                        "cold_start_stability_min_bytes + tolerance_bytes")},
        {QStringLiteral("requested_sample_count"), requestedSampleCount},
        {QStringLiteral("sample_count"), comparisonCount},
        {QStringLiteral("within_upper_bound_sample_count"), withinUpperBound},
        {QStringLiteral("outside_upper_bound_sample_count"), comparisonCount - withinUpperBound},
        {QStringLiteral("within_tolerance_sample_count"), withinUpperBound},
        {QStringLiteral("outside_tolerance_sample_count"), comparisonCount - withinUpperBound},
        {QStringLiteral("complete_sample_set"), completeSampleSet},
        {QStringLiteral("all_successful_samples_within_upper_bound"),
         allSuccessfulSamplesWithinUpperBound},
        {QStringLiteral("all_samples_within_tolerance"), benchmarkPassed},
        {QStringLiteral("benchmark_passed"), benchmarkPassed},
        {QStringLiteral("delta_representative"), QStringLiteral("median_of_stability_window")},
        {QStringLiteral("acceptance_uses_conservative_stability_bounds"), true},
        {QStringLiteral("tolerance_bytes"), toleranceBytes},
        {QStringLiteral("tolerance_mib"), static_cast<double>(toleranceBytes) / kBytesPerMebibyte},
        {QStringLiteral("delta_mib_statistics"), statistics(std::move(deltas))}};
}

QJsonObject scenarioReclaimSummary(const QVector<QJsonObject>& records, const QString& scenario,
                                   qint64 toleranceBytes, qsizetype requestedSampleCount) {
    require(requestedSampleCount > 0, "requested sample count must be positive");
    QJsonObject scenarios;
    QVector<double> deltas;
    qsizetype comparisonCount = 0;
    qsizetype withinUpperBound = 0;
    const QJsonObject summary =
        scenarioComparisonSummary(records, scenario, toleranceBytes, requestedSampleCount);
    scenarios.insert(scenario, summary);
    comparisonCount = summary.value(QStringLiteral("sample_count")).toInteger();
    withinUpperBound =
        summary.value(QStringLiteral("within_upper_bound_sample_count")).toInteger();
    for (const QJsonObject& record : records) {
        const QJsonObject comparison = record.value(QStringLiteral("scenario_reclaim_vs_cold_start"))
                                           .toObject()
                                           .value(scenario)
                                           .toObject();
        if (!comparison.isEmpty()) {
            deltas.push_back(comparison.value(QStringLiteral("delta_mib")).toDouble());
        }
    }
    const qsizetype requestedComparisonCount = requestedSampleCount;
    const bool completeSampleSet = records.size() == requestedSampleCount;
    const bool completeComparisonSet = comparisonCount == requestedComparisonCount;
    const bool allComparisonsWithinUpperBound =
        comparisonCount > 0 && withinUpperBound == comparisonCount;
    const bool benchmarkPassed =
        completeSampleSet && completeComparisonSet && allComparisonsWithinUpperBound;
    return {
        {QStringLiteral("comparison_kind"),
         QStringLiteral("one_sided_upper_bound_using_stability_bounds")},
        {QStringLiteral("criterion"),
         QStringLiteral("each post_scenario_stability_max_bytes <= "
                        "cold_start_stability_min_bytes + tolerance_bytes")},
        {QStringLiteral("requested_sample_count"), requestedSampleCount},
        {QStringLiteral("sample_count"), records.size()},
        {QStringLiteral("requested_comparison_count"), requestedComparisonCount},
        {QStringLiteral("comparison_count"), comparisonCount},
        {QStringLiteral("within_upper_bound_comparison_count"), withinUpperBound},
        {QStringLiteral("outside_upper_bound_comparison_count"),
         comparisonCount - withinUpperBound},
        {QStringLiteral("complete_sample_set"), completeSampleSet},
        {QStringLiteral("complete_comparison_set"), completeComparisonSet},
        {QStringLiteral("all_comparisons_within_upper_bound"), allComparisonsWithinUpperBound},
        {QStringLiteral("benchmark_passed"), benchmarkPassed},
        {QStringLiteral("delta_representative"), QStringLiteral("median_of_stability_window")},
        {QStringLiteral("acceptance_uses_conservative_stability_bounds"), true},
        {QStringLiteral("tolerance_bytes"), toleranceBytes},
        {QStringLiteral("tolerance_mib"), static_cast<double>(toleranceBytes) / kBytesPerMebibyte},
        {QStringLiteral("delta_mib_statistics"), statistics(std::move(deltas))},
        {QStringLiteral("scenarios"), scenarios}};
}

int runBenchmark(const BenchmarkConfiguration& configuration) {
    require(QDir().mkpath(configuration.outputDirectory), "could not create output directory");
    const QString reportPath =
        QDir(configuration.outputDirectory).filePath(QStringLiteral("report.json"));
    const QString htmlPath =
        QDir(configuration.outputDirectory).filePath(QStringLiteral("report.html"));
    removeStaleTrace(reportPath, "could not remove the previous report.json");
    removeStaleTrace(htmlPath, "could not remove the previous report.html");
    removeStaleSampleTraces(configuration.outputDirectory);
    QFile rawFile(QDir(configuration.outputDirectory).filePath(QStringLiteral("raw.jsonl")));
    require(rawFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "could not create raw.jsonl");

    const QVector<MonitorInfo> displayList = monitorInfo();
    validateConfiguration(configuration, displayList);

    ScopedCom com;
    require(SUCCEEDED(com.result()), "COM initialization failed");
    ComPtr<IUIAutomation> automation = createAutomation();
    CursorRestore restoreCursor;
    const ScenarioMetadata& metadata = scenarioMetadata(configuration.scenario);

    QVector<QJsonObject> records;
    records.reserve(configuration.samples);
    QJsonArray sampleErrors;
    for (int iteration = 1; iteration <= configuration.samples; ++iteration) {
        QJsonObject record;
        try {
            record = runSample(configuration, displayList.at(configuration.screenIndex), iteration,
                               *automation.get());
        } catch (const std::exception& error) {
            record = {{QStringLiteral("schema_version"), 5},
                      {QStringLiteral("scenario"), metadata.commandLineName},
                      {QStringLiteral("iteration"), iteration},
                      {QStringLiteral("error"), QString::fromLocal8Bit(error.what())}};
            sampleErrors.append(record);
            std::cerr << "sample " << iteration << " failed: " << error.what() << '\n';
        }
        QByteArray serializedRecord = QJsonDocument(record).toJson(QJsonDocument::Compact);
        serializedRecord.append('\n');
        require(rawFile.write(serializedRecord) == serializedRecord.size() && rawFile.flush(),
                "could not append the sample to raw.jsonl");
        if (record.contains(QStringLiteral("cold_start"))) {
            records.push_back(record);
        }
    }
    rawFile.close();

    const QJsonObject metrics{
        {QStringLiteral("cold_start"), metricFor(records, QStringLiteral("cold_start"))},
        {metadata.activeStage, metricFor(records, metadata.activeStage)},
        {metadata.closedStage, metricFor(records, metadata.closedStage)}};
    const QJsonObject reclaimSummary = scenarioReclaimSummary(
        records, metadata.reportName, configuration.scenarioReclaimToleranceBytes,
        configuration.samples);
    const bool completeSampleSet = records.size() == configuration.samples;
    const bool benchmarkPassed =
        reclaimSummary.value(QStringLiteral("benchmark_passed")).toBool();
    const QString benchmarkStatus =
        !completeSampleSet
            ? QStringLiteral("incomplete")
            : (benchmarkPassed ? QStringLiteral("pass") : QStringLiteral("retention_failed"));
    const QJsonObject report{
        {QStringLiteral("schema_version"), 5},
        {QStringLiteral("benchmark"), QStringLiteral("screenshot_memory_footprint")},
        {QStringLiteral("scenario"), metadata.commandLineName},
        {QStringLiteral("benchmark_status"), benchmarkStatus},
        {QStringLiteral("generated_utc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("stage_order"),
         QJsonArray::fromStringList(benchmarkStageOrder(configuration.scenario))},
        {QStringLiteral("configuration"),
         QJsonObject{
              {QStringLiteral("samples"), configuration.samples},
              {QStringLiteral("scenario"), metadata.commandLineName},
             {QStringLiteral("screen_index"), configuration.screenIndex},
             {QStringLiteral("poll_ms"), configuration.pollMilliseconds},
             {QStringLiteral("cold_start_min_wait_ms"),
              configuration.coldStartMinimumWaitMilliseconds},
             {QStringLiteral("stage_min_wait_ms"), configuration.stageMinimumWaitMilliseconds},
             {QStringLiteral("stability_window_samples"), configuration.stabilityWindowSamples},
             {QStringLiteral("stability_range_kib"),
              configuration.stabilityRangeBytes / kBytesPerKibibyte},
             {QStringLiteral("memory_stage_representative"),
              QStringLiteral("median_of_stability_window")},
             {QStringLiteral("acceptance_uses_conservative_stability_bounds"), true},
             {QStringLiteral("scenario_reclaim_tolerance_kib"),
              configuration.scenarioReclaimToleranceBytes / kBytesPerKibibyte},
             // Retained for command-line and report consumers using the version 3 name.
             {QStringLiteral("final_idle_tolerance_kib"),
              configuration.scenarioReclaimToleranceBytes / kBytesPerKibibyte},
             {QStringLiteral("timeout_ms"), configuration.timeoutMilliseconds}}},
        {QStringLiteral("metrics"), metrics},
        {QStringLiteral("scenario_reclaim_vs_cold_start"), reclaimSummary},
        {QStringLiteral("final_idle_vs_cold_start"),
         reclaimSummary.value(QStringLiteral("scenarios"))
             .toObject()
             .value(metadata.reportName)},
        {QStringLiteral("successful_sample_count"), records.size()},
        {QStringLiteral("failed_sample_count"), configuration.samples - records.size()},
        {QStringLiteral("sample_errors"), sampleErrors},
        {QStringLiteral("environment"),
         environmentReport(configuration.appPath, displayList.at(configuration.screenIndex))}};

    QSaveFile reportFile(reportPath);
    require(reportFile.open(QIODevice::WriteOnly), "could not create report.json");
    const QByteArray serializedReport = QJsonDocument(report).toJson(QJsonDocument::Indented);
    require(reportFile.write(serializedReport) == serializedReport.size() && reportFile.commit(),
            "could not write report.json");

    QSaveFile htmlFile(htmlPath);
    require(htmlFile.open(QIODevice::WriteOnly), "could not create report.html");
    const QByteArray serializedHtml = reportHtml(report).toUtf8();
    require(htmlFile.write(serializedHtml) == serializedHtml.size() && htmlFile.commit(),
            "could not write report.html");
    if (!completeSampleSet) {
        return 2;
    }
    return benchmarkPassed ? 0 : 3;
}

bool runSelfTest() {
    require(statistics(QVector<double>{1.0, 2.0, 3.0, 4.0, 5.0})
                    .value(QStringLiteral("p50_mib"))
                    .toDouble() == 3.0,
            "statistics self-test failed");
    require(stableWindow(std::deque<qint64>{100, 120, 110}, 3, 21) &&
                !stableWindow(std::deque<qint64>{100, 120, 110}, 3, 20) &&
                !stableWindow(std::deque<qint64>{100, 110}, 3, 21),
            "stable-window self-test failed");
    require(medianValue(std::deque<qint64>{100, 120, 110}) == 110 &&
                medianValue(std::deque<qint64>{120, 100, 110}) == 110 &&
                medianValue(std::deque<qint64>{100, 110, 120, 130}) == 115,
            "stable-window median self-test failed");
    require(provesElementAbsent(VisibleElementLookupStatus::Absent) &&
                !provesElementAbsent(VisibleElementLookupStatus::Found) &&
                !provesElementAbsent(VisibleElementLookupStatus::Error),
            "UI Automation absence self-test failed");

    const RECT exactSizedMonitor{0, 0, kSelectionPixels, kSelectionPixels};
    const RECT exactWidthMonitor{0, 0, kSelectionPixels, kSelectionPixels + 1};
    const RECT exactHeightMonitor{0, 0, kSelectionPixels + 1, kSelectionPixels};
    const RECT minimumSupportedMonitor{10, 20, 10 + kSelectionPixels + 1,
                                       20 + kSelectionPixels + 1};
    require(!monitorSupportsFixedSelectionDrag(exactSizedMonitor) &&
                !monitorSupportsFixedSelectionDrag(exactWidthMonitor) &&
                !monitorSupportsFixedSelectionDrag(exactHeightMonitor) &&
                monitorSupportsFixedSelectionDrag(minimumSupportedMonitor),
            "fixed-selection monitor boundary self-test failed");

    const QStringList integerOptions{QStringLiteral("samples"),
                                     QStringLiteral("screen-index"),
                                     QStringLiteral("poll-ms"),
                                     QStringLiteral("cold-start-min-wait-ms"),
                                     QStringLiteral("stage-min-wait-ms"),
                                     QStringLiteral("stability-window"),
                                     QStringLiteral("stability-range-kib"),
                                     QStringLiteral("final-idle-tolerance-kib"),
                                     QStringLiteral("timeout-ms")};
    const QStringList invalidIntegerValues{QStringLiteral("not-an-integer"),
                                           QStringLiteral("9223372036854775808")};
    for (const QString& option : integerOptions) {
        for (const QString& invalidValue : invalidIntegerValues) {
            QCommandLineParser invalidParser;
            addBenchmarkCommandLineOptions(invalidParser);
            const QString invalidArgument = QStringLiteral("--%1=%2").arg(option).arg(invalidValue);
            require(invalidParser.parse({QStringLiteral("benchmark-self-test"), invalidArgument}),
                    "invalid integer command-line self-test setup failed");
            bool rejected = false;
            try {
                static_cast<void>(configurationFromParser(invalidParser));
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            require(rejected, "invalid integer command-line option was accepted");
        }
    }
    QCommandLineParser defaultParser;
    addBenchmarkCommandLineOptions(defaultParser);
    require(defaultParser.parse({QStringLiteral("benchmark-self-test")}),
            "default command-line self-test setup failed");
    require(configurationFromParser(defaultParser).scenarioReclaimToleranceBytes ==
                3 * kBytesPerMebibyte &&
                configurationFromParser(defaultParser).scenario ==
                    BenchmarkScenario::ScreenshotWindow &&
                benchmarkScenario(QStringLiteral("right-click-menu")) ==
                    BenchmarkScenario::RightClickMenu,
            "default scenario reclaim tolerance self-test failed");

    QTemporaryDir temporary;
    require(temporary.isValid(), "trace self-test directory could not be created");
    const QString tracePath = temporary.filePath(QStringLiteral("trace.jsonl"));
    QFile traceFile(tracePath);
    require(traceFile.open(QIODevice::WriteOnly), "trace self-test file could not be created");
    traceFile.write("{\"event\":\"app_ready\",\"process_id\":123}\n");
    traceFile.close();
    const QVector<QJsonObject> trace = readJsonLines(tracePath);
    require(trace.size() == 1 && trace.first().value(QStringLiteral("event")).toString() ==
                                     QStringLiteral("app_ready"),
            "trace parsing self-test failed");
    removeStaleTrace(tracePath, "stale trace removal self-test failed");
    require(!QFile::exists(tracePath), "stale trace removal self-test failed");
    removeStaleTrace(tracePath, "absent trace removal self-test failed");

    validatePinRecord(
        QJsonObject{{QStringLiteral("scenario"), QStringLiteral("normal-selection")},
                    {QStringLiteral("width"), kSelectionPixels},
                    {QStringLiteral("height"), kSelectionPixels},
                    {QStringLiteral("success"), true},
                    {QStringLiteral("milestones_ns"),
                     QJsonObject{{QStringLiteral("window.native_paint_synchronized"), 1}}}});
    validateIdleMemoryReclaim(
        QJsonObject{{QStringLiteral("event"), QStringLiteral("idle_memory_reclaim_completed")},
                    {QStringLiteral("trim_working_set"), true},
                    {QStringLiteral("success"), true},
                    {QStringLiteral("attempt_count"), 2}},
        true);
    bool failedReclaimRejected = false;
    try {
        validateIdleMemoryReclaim(
            QJsonObject{{QStringLiteral("event"), QStringLiteral("idle_memory_reclaim_completed")},
                        {QStringLiteral("trim_working_set"), true},
                        {QStringLiteral("success"), false},
                        {QStringLiteral("attempt_count"), 3}},
            true);
    } catch (const std::runtime_error&) {
        failedReclaimRejected = true;
    }
    require(failedReclaimRejected, "failed idle-memory reclaim was accepted");
    require(privateWorkingSet(GetCurrentProcess()).bytes > 0,
            "private working-set self-test failed");
    const QJsonObject passingComparison = scenarioReclaimComparison(
        QStringLiteral("main_interface"), 100 * kBytesPerMebibyte, 108 * kBytesPerMebibyte,
        8 * kBytesPerMebibyte);
    const QJsonObject failingComparison = scenarioReclaimComparison(
        QStringLiteral("main_interface"), 100 * kBytesPerMebibyte,
        108 * kBytesPerMebibyte + 1, 8 * kBytesPerMebibyte);
    const QJsonObject belowColdComparison = scenarioReclaimComparison(
        QStringLiteral("main_interface"), 100 * kBytesPerMebibyte, 60 * kBytesPerMebibyte,
        8 * kBytesPerMebibyte);
    require(passingComparison.value(QStringLiteral("within_tolerance")).toBool() &&
                passingComparison.value(QStringLiteral("within_upper_bound")).toBool() &&
                passingComparison.value(QStringLiteral("delta_mib")).toDouble() == 8.0 &&
                !failingComparison.value(QStringLiteral("within_tolerance")).toBool() &&
                belowColdComparison.value(QStringLiteral("within_upper_bound")).toBool() &&
                belowColdComparison.value(QStringLiteral("delta_mib")).toDouble() == -40.0,
            "scenario reclaim comparison self-test failed");
    StableMemorySample biasedCold;
    biasedCold.bytes = 102;
    biasedCold.minimumBytes = 100;
    biasedCold.maximumBytes = 104;
    StableMemorySample biasedFinal;
    biasedFinal.bytes = 104;
    biasedFinal.minimumBytes = 102;
    biasedFinal.maximumBytes = 106;
    const QJsonObject conservativeComparison = scenarioReclaimComparison(
        QStringLiteral("main_interface"), biasedCold, biasedFinal, 3);
    require(
        conservativeComparison.value(QStringLiteral("delta_bytes")).toInteger() == 2 &&
            conservativeComparison.value(QStringLiteral("acceptance_delta_bytes")).toInteger() ==
                6 &&
            !conservativeComparison.value(QStringLiteral("within_upper_bound")).toBool(),
        "conservative stability-bound comparison self-test failed");
    const auto syntheticRecord = [](const QJsonObject& comparison) {
        return QJsonObject{{QStringLiteral("scenario_reclaim_vs_cold_start"),
                            QJsonObject{{QStringLiteral("main_interface"), comparison}}}};
    };
    const QJsonObject secondMainComparison = scenarioReclaimComparison(
        QStringLiteral("main_interface"), 100 * kBytesPerMebibyte, 99 * kBytesPerMebibyte,
        8 * kBytesPerMebibyte);
    const QJsonObject syntheticSummary = scenarioReclaimSummary(
        QVector<QJsonObject>{syntheticRecord(passingComparison),
                             syntheticRecord(secondMainComparison)},
        QStringLiteral("main_interface"), 8 * kBytesPerMebibyte, 2);
    const QJsonObject incompleteSummary = scenarioReclaimSummary(
        QVector<QJsonObject>{syntheticRecord(passingComparison)},
        QStringLiteral("main_interface"), 8 * kBytesPerMebibyte, 2);
    const QJsonObject retainedScenarioSummary = scenarioReclaimSummary(
        QVector<QJsonObject>{syntheticRecord(failingComparison),
                             syntheticRecord(secondMainComparison)},
        QStringLiteral("main_interface"), 8 * kBytesPerMebibyte, 2);
    require(
        syntheticSummary.value(QStringLiteral("benchmark_passed")).toBool() &&
            syntheticSummary.value(QStringLiteral("within_upper_bound_comparison_count"))
                    .toInteger() == 2 &&
            !incompleteSummary.value(QStringLiteral("complete_sample_set")).toBool() &&
            !incompleteSummary.value(QStringLiteral("benchmark_passed")).toBool() &&
            !retainedScenarioSummary.value(QStringLiteral("benchmark_passed")).toBool() &&
            retainedScenarioSummary.value(QStringLiteral("scenarios"))
                    .toObject()
                    .value(QStringLiteral("main_interface"))
                    .toObject()
                    .value(QStringLiteral("benchmark_passed"))
                    .toBool() == false,
        "scenario reclaim summary self-test failed");
    const QJsonObject syntheticReport{
        {QStringLiteral("metrics"),
         QJsonObject{{QStringLiteral("cold_start"), statistics(QVector<double>{1.0})},
                      {QStringLiteral("main_interface"), statistics(QVector<double>{3.0})},
                      {QStringLiteral("main_interface_closed"),
                       statistics(QVector<double>{1.5})}}},
        {QStringLiteral("stage_order"),
         QJsonArray::fromStringList(benchmarkStageOrder(BenchmarkScenario::MainInterface))},
        {QStringLiteral("scenario_reclaim_vs_cold_start"), syntheticSummary}};
    const QString syntheticHtml = reportHtml(syntheticReport);
    require(
            syntheticHtml.contains(QStringLiteral("Screenshot memory footprint")) &&
            syntheticHtml.contains(QStringLiteral("Benchmark: PASS")) &&
            syntheticHtml.contains(QStringLiteral("no more than 8 MiB above cold start")) &&
            syntheticHtml.contains(QStringLiteral("main_interface_closed")) &&
            reportHtml(QJsonObject{{QStringLiteral("scenario_reclaim_vs_cold_start"),
                                    incompleteSummary}})
                .contains(QStringLiteral("Benchmark: INCOMPLETE")),
        "HTML report self-test failed");
    std::cout << "screenshot memory footprint benchmark self-tests passed\n";
    return true;
}
} // namespace

int main(int argc, char** argv) {
    try {
        configurePerMonitorDpiAwareness();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("snow-shot-screenshot-memory-footprint-benchmark"));

    QCommandLineParser parser;
    addBenchmarkCommandLineOptions(parser);
    parser.process(application);

    try {
        const BenchmarkConfiguration configuration = configurationFromParser(parser);
        if (parser.isSet(QStringLiteral("self-test"))) {
            return runSelfTest() ? 0 : 1;
        }
        return runBenchmark(configuration);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
