#include "screenshotrecognitionworkerprocess.h"

#include <QDir>
#include <QProcess>

#include <algorithm>
#include <limits>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <Windows.h>
#endif

namespace snow_shot::presentation {

#if defined(Q_OS_WIN) || defined(_WIN32)
namespace {
struct NativeHandleCloser {
    void operator()(void* handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle));
        }
    }
};

using NativeHandle = std::unique_ptr<void, NativeHandleCloser>;
} // namespace
#endif

class RecognitionWorkerProcess::Impl final {
  public:
#if defined(Q_OS_WIN) || defined(_WIN32)
    NativeHandle process;
    NativeHandle input;
    NativeHandle output;
#else
    QProcess process;
#endif
};

RecognitionWorkerProcess::RecognitionWorkerProcess(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}

RecognitionWorkerProcess::~RecognitionWorkerProcess() {
    terminate();
}

std::unique_ptr<RecognitionWorkerProcess>
RecognitionWorkerProcess::start(const QString& executable, const QString& argument) {
    auto impl = std::make_unique<Impl>();
#if defined(Q_OS_WIN) || defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE childInputRead = nullptr;
    HANDLE parentInputWrite = nullptr;
    HANDLE parentOutputRead = nullptr;
    HANDLE childOutputWrite = nullptr;
    if (CreatePipe(&childInputRead, &parentInputWrite, &attributes, 64 * 1024) == FALSE) {
        return nullptr;
    }
    NativeHandle childInput(childInputRead);
    NativeHandle parentInput(parentInputWrite);
    if (CreatePipe(&parentOutputRead, &childOutputWrite, &attributes, 64 * 1024) == FALSE) {
        return nullptr;
    }
    NativeHandle parentOutput(parentOutputRead);
    NativeHandle childOutput(childOutputWrite);
    if (SetHandleInformation(parentInput.get(), HANDLE_FLAG_INHERIT, 0) == FALSE ||
        SetHandleInformation(parentOutput.get(), HANDLE_FLAG_INHERIT, 0) == FALSE) {
        return nullptr;
    }

    const QString nativeExecutable = QDir::toNativeSeparators(executable);
    std::wstring executablePath = nativeExecutable.toStdWString();
    std::wstring commandLine =
        L"\"" + executablePath + L"\" " + argument.toStdWString();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childInput.get();
    startup.hStdOutput = childOutput.get();
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION processInfo{};
    if (CreateProcessW(executablePath.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo) == FALSE) {
        return nullptr;
    }
    impl->process.reset(processInfo.hProcess);
    NativeHandle thread(processInfo.hThread);
    childInput.reset();
    childOutput.reset();
    impl->input = std::move(parentInput);
    impl->output = std::move(parentOutput);
#else
    impl->process.setProcessChannelMode(QProcess::SeparateChannels);
    impl->process.start(executable, {argument}, QIODevice::ReadWrite);
    if (!impl->process.waitForStarted(5'000)) {
        return nullptr;
    }
#endif
    return std::unique_ptr<RecognitionWorkerProcess>(
        new RecognitionWorkerProcess(std::move(impl)));
}

RecognitionWorkerIoResult
RecognitionWorkerProcess::writeExact(const std::atomic_bool& cancelled, const void* source,
                                     std::uint64_t length) {
    if (m_impl == nullptr) {
        return RecognitionWorkerIoResult::Failed;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    constexpr DWORD kChunkBytes = 64 * 1024;
    const auto* input = static_cast<const std::uint8_t*>(source);
    std::uint64_t offset = 0;
    while (offset < length) {
        if (cancelled.load(std::memory_order_acquire)) {
            return RecognitionWorkerIoResult::Cancelled;
        }
        const DWORD requested = static_cast<DWORD>(
            (std::min)(static_cast<std::uint64_t>(kChunkBytes), length - offset));
        DWORD written = 0;
        if (WriteFile(m_impl->input.get(), input + offset, requested, &written, nullptr) == FALSE ||
            written == 0) {
            return RecognitionWorkerIoResult::Failed;
        }
        offset += written;
    }
#else
    constexpr qint64 kChunkBytes = 64 * 1024;
    const auto* input = static_cast<const char*>(source);
    std::uint64_t offset = 0;
    while (offset < length) {
        if (cancelled.load(std::memory_order_acquire)) {
            return RecognitionWorkerIoResult::Cancelled;
        }
        const qint64 requested = static_cast<qint64>(
            (std::min)(static_cast<std::uint64_t>(kChunkBytes), length - offset));
        const qint64 written = m_impl->process.write(input + offset, requested);
        if (written <= 0) {
            return RecognitionWorkerIoResult::Failed;
        }
        offset += static_cast<std::uint64_t>(written);
        while (m_impl->process.bytesToWrite() > 0) {
            if (cancelled.load(std::memory_order_acquire)) {
                return RecognitionWorkerIoResult::Cancelled;
            }
            if (!m_impl->process.waitForBytesWritten(25) &&
                m_impl->process.state() == QProcess::NotRunning) {
                return RecognitionWorkerIoResult::Failed;
            }
        }
    }
#endif
    return RecognitionWorkerIoResult::Complete;
}

RecognitionWorkerIoResult
RecognitionWorkerProcess::readExact(const std::atomic_bool& cancelled, void* destination,
                                    std::uint64_t length) {
    if (m_impl == nullptr) {
        return RecognitionWorkerIoResult::Failed;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    auto* output = static_cast<std::uint8_t*>(destination);
    std::uint64_t offset = 0;
    while (offset < length) {
        if (cancelled.load(std::memory_order_acquire)) {
            return RecognitionWorkerIoResult::Cancelled;
        }
        DWORD available = 0;
        if (PeekNamedPipe(m_impl->output.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE) {
            return RecognitionWorkerIoResult::Failed;
        }
        if (available == 0) {
            if (WaitForSingleObject(m_impl->process.get(), 25) != WAIT_TIMEOUT) {
                return RecognitionWorkerIoResult::Failed;
            }
            continue;
        }
        const DWORD requested = static_cast<DWORD>(
            (std::min)(static_cast<std::uint64_t>(available), length - offset));
        DWORD consumed = 0;
        if (ReadFile(m_impl->output.get(), output + offset, requested, &consumed, nullptr) == FALSE ||
            consumed == 0) {
            return RecognitionWorkerIoResult::Failed;
        }
        offset += consumed;
    }
#else
    auto* output = static_cast<char*>(destination);
    std::uint64_t offset = 0;
    while (offset < length) {
        if (cancelled.load(std::memory_order_acquire)) {
            return RecognitionWorkerIoResult::Cancelled;
        }
        const qint64 requested = static_cast<qint64>(
            (std::min)(static_cast<std::uint64_t>((std::numeric_limits<qint64>::max)()),
                       length - offset));
        const qint64 count = m_impl->process.read(output + offset, requested);
        if (count > 0) {
            offset += static_cast<std::uint64_t>(count);
            continue;
        }
        if (count < 0 || m_impl->process.state() == QProcess::NotRunning) {
            return RecognitionWorkerIoResult::Failed;
        }
        static_cast<void>(m_impl->process.waitForReadyRead(25));
    }
#endif
    return RecognitionWorkerIoResult::Complete;
}

void RecognitionWorkerProcess::closeInput() {
    if (m_impl == nullptr) {
        return;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    m_impl->input.reset();
#else
    m_impl->process.closeWriteChannel();
#endif
}

RecognitionWorkerIoResult
RecognitionWorkerProcess::waitForFinished(const std::atomic_bool& cancelled) {
    if (m_impl == nullptr) {
        return RecognitionWorkerIoResult::Failed;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    while (true) {
        if (cancelled.load(std::memory_order_acquire)) {
            return RecognitionWorkerIoResult::Cancelled;
        }
        const DWORD wait = WaitForSingleObject(m_impl->process.get(), 25);
        if (wait == WAIT_OBJECT_0) {
            return RecognitionWorkerIoResult::Complete;
        }
        if (wait != WAIT_TIMEOUT) {
            return RecognitionWorkerIoResult::Failed;
        }
    }
#else
    while (m_impl->process.state() != QProcess::NotRunning) {
        if (cancelled.load(std::memory_order_acquire)) {
            return RecognitionWorkerIoResult::Cancelled;
        }
        static_cast<void>(m_impl->process.waitForFinished(25));
    }
    return cancelled.load(std::memory_order_acquire) ? RecognitionWorkerIoResult::Cancelled
                                                     : RecognitionWorkerIoResult::Complete;
#endif
}

bool RecognitionWorkerProcess::exitedSuccessfullyWithoutOutput() const {
    if (m_impl == nullptr) {
        return false;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    DWORD exitCode = 0;
    DWORD extraBytes = 0;
    const BOOL pipeReadable =
        PeekNamedPipe(m_impl->output.get(), nullptr, 0, nullptr, &extraBytes, nullptr);
    const bool cleanPipeEnd = pipeReadable != FALSE || GetLastError() == ERROR_BROKEN_PIPE;
    return GetExitCodeProcess(m_impl->process.get(), &exitCode) != FALSE && exitCode == 0 &&
           cleanPipeEnd && extraBytes == 0;
#else
    return m_impl->process.exitStatus() == QProcess::NormalExit &&
           m_impl->process.exitCode() == 0 && m_impl->process.bytesAvailable() == 0;
#endif
}

QString RecognitionWorkerProcess::errorString() const {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return {};
#else
    return m_impl != nullptr ? QString::fromUtf8(m_impl->process.readAllStandardError()).trimmed()
                             : QString{};
#endif
}

void RecognitionWorkerProcess::terminate() {
    if (m_impl == nullptr) {
        return;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (WaitForSingleObject(m_impl->process.get(), 0) == WAIT_TIMEOUT) {
        static_cast<void>(TerminateProcess(m_impl->process.get(), 2));
    }
    static_cast<void>(WaitForSingleObject(m_impl->process.get(), INFINITE));
#else
    if (m_impl->process.state() != QProcess::NotRunning) {
        m_impl->process.kill();
        m_impl->process.waitForFinished(-1);
    }
#endif
}

} // namespace snow_shot::presentation
