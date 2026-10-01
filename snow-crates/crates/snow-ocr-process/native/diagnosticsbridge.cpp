#ifndef SNOW_DIAGNOSTICS_PRODUCT
#define SNOW_DIAGNOSTICS_PRODUCT "Snow Shot"
#endif

// SPDX-License-Identifier: Apache-2.0
#include "diagnosticsbridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#include "client/annotation.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"
#include "util/misc/capture_context.h"

namespace {
crashpad::CrashpadClient client;
crashpad::SimpleStringDictionary annotations;
std::string pipeName;
std::string databaseDirectory;
std::mutex captureMutex;
bool captureAttempted = false;
std::atomic<HANDLE> emergencyHandle{INVALID_HANDLE_VALUE};
std::atomic<unsigned> emergencyUsers{0};
std::atomic<size_t> emergencyBytes{0};
std::atomic<size_t> breadcrumbIndex{0};
std::array<std::array<char, 1024>, 16> breadcrumbs{};
std::array<std::atomic<bool>, 16> breadcrumbBusy{};
crashpad::Annotation breadcrumbAnnotation(crashpad::Annotation::Type::kString, "snow.recent_events",
                                          breadcrumbs.data());
std::array<char, 1024> fatalLocation{};
crashpad::Annotation fatalAnnotation(crashpad::Annotation::Type::kString, "snow.fatal_location",
                                     fatalLocation.data());
std::terminate_handler previousTerminate = nullptr;
std::atomic<bool> fatalEntered{false};
std::atomic<bool> collectorReady{false};
std::array<char, 64> sessionIdentity{};

void emergencyWrite(const char* record, DWORD length) noexcept {
    emergencyUsers.fetch_add(1);
    const HANDLE handle = emergencyHandle.load();
    if (handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(handle, record, length, &written, nullptr);
        FlushFileBuffers(handle);
    }
    emergencyUsers.fetch_sub(1);
}

void closeEmergency(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE)
        return;
    // Readers never wait on the writer; rotation retires a handle after native writes finish.
    while (emergencyUsers.load() != 0)
        std::this_thread::yield();
    CloseHandle(handle);
}

std::wstring wide(const char* value) {
    if (value == nullptr || *value == '\0') {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result.data(), count);
    result.pop_back();
    return result;
}

void terminateNow() noexcept {
    snow_diag_fatal("cpp.terminate");
    snow_diag_panic(reinterpret_cast<const unsigned char*>("cpp.terminate"), 13);
    std::abort();
}

void configure(const char* role, const char* session) {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    strncpy_s(sessionIdentity.data(), sessionIdentity.size(), session, _TRUNCATE);
    annotations.SetKeyValue("product", SNOW_DIAGNOSTICS_PRODUCT);
    annotations.SetKeyValue("role", role);
    annotations.SetKeyValue("session", session);
#ifdef SNOW_DIAGNOSTICS_BUILD
    annotations.SetKeyValue("build", SNOW_DIAGNOSTICS_BUILD);
#endif
#ifdef SNOW_DIAGNOSTICS_REVISION
    annotations.SetKeyValue("revision", SNOW_DIAGNOSTICS_REVISION);
#endif
    crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(&annotations);
    crashpad::CrashpadInfo::GetCrashpadInfo()->set_gather_indirectly_referenced_memory(
        crashpad::TriState::kDisabled, 0);
    breadcrumbAnnotation.SetSize(
        static_cast<crashpad::Annotation::ValueSizeType>(sizeof(breadcrumbs)));
    if (previousTerminate == nullptr)
        previousTerminate = std::set_terminate(terminateNow);
}
} // namespace
#endif

void snow_diag_prepare(const char* session, const char* version, const char* revision) {
#ifdef _WIN32
    try {
        std::lock_guard lock(captureMutex);
        configure("application", session);
        annotations.SetKeyValue("version", version);
        annotations.SetKeyValue("revision", revision);
    } catch (...) {
    }
#else
    static_cast<void>(session);
    static_cast<void>(version);
    static_cast<void>(revision);
#endif
}

int snow_diag_start(const char* handler, const char* database, const char* session,
                    const char* version, const char* revision) {
#ifdef _WIN32
    try {
        std::lock_guard lock(captureMutex);
        if (captureAttempted && (!collectorReady.load() || databaseDirectory != database))
            return 0;
        configure("application", session);
        annotations.SetKeyValue("version", version);
        annotations.SetKeyValue("revision", revision);
        if (captureAttempted)
            return 1;
        // StartHandler mutates process-wide exception state even if handler startup fails.
        // It must never be called again, including after an unsuccessful first attempt.
        captureAttempted = true;
        if (!client.StartHandler(base::FilePath(wide(handler)), base::FilePath(wide(database)),
                                 base::FilePath(), "", {}, {"--no-periodic-tasks"}, true, false)) {
            return 0;
        }
        const std::wstring pipe = client.GetHandlerIPCPipe();
        const int count = WideCharToMultiByte(
            CP_UTF8, 0, pipe.c_str(), static_cast<int>(pipe.size()), nullptr, 0, nullptr, nullptr);
        pipeName.resize(static_cast<size_t>(count));
        WideCharToMultiByte(CP_UTF8, 0, pipe.c_str(), static_cast<int>(pipe.size()),
                            pipeName.data(), count, nullptr, nullptr);
        databaseDirectory = database;
        collectorReady.store(true);
        return 1;
    } catch (...) {
        return 0;
    }
#else
    static_cast<void>(handler);
    static_cast<void>(database);
    static_cast<void>(session);
    static_cast<void>(version);
    static_cast<void>(revision);
    return 0;
#endif
}

int snow_diag_attach(const char* pipe, const char* session, const char* version) {
#ifdef _WIN32
    try {
        std::lock_guard lock(captureMutex);
        if (captureAttempted &&
            (!collectorReady.load() || !databaseDirectory.empty() || pipeName != pipe))
            return 0;
        configure("ocr", session);
        annotations.SetKeyValue("version", version);
        if (captureAttempted)
            return 1;
        captureAttempted = true;
        const bool attached = client.SetHandlerIPCPipe(wide(pipe));
        if (attached)
            pipeName = pipe;
        collectorReady.store(attached);
        return attached ? 1 : 0;
    } catch (...) {
        return 0;
    }
#else
    static_cast<void>(pipe);
    static_cast<void>(session);
    static_cast<void>(version);
    return 0;
#endif
}

const char* snow_diag_pipe(void) {
#ifdef _WIN32
    return collectorReady.load() ? pipeName.c_str() : "";
#else
    return "";
#endif
}

const char* snow_diag_database(void) {
#ifdef _WIN32
    return collectorReady.load() ? databaseDirectory.c_str() : "";
#else
    return "";
#endif
}

void snow_diag_open_emergency(const char* path) {
#ifdef _WIN32
    try {
        const HANDLE handle =
            CreateFileW(wide(path).c_str(), FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        const HANDLE previous = emergencyHandle.exchange(handle);
        closeEmergency(previous);
        LARGE_INTEGER size{};
        emergencyBytes.store(GetFileSizeEx(handle, &size) ? static_cast<size_t>(size.QuadPart) : 0);
    } catch (...) {
        // Logging setup must not make application startup fail.
    }
#else
    static_cast<void>(path);
#endif
}

void snow_diag_fatal(const char* event) {
#ifdef _WIN32
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::array<char, 1024> record{};
    const int length = std::snprintf(
        record.data(), record.size(),
        "{\"time\":\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\",\"level\":\"FATAL\","
        "\"category\":\"snow_shot.runtime\",\"pid\":%lu,\"tid\":%lu,\"session\":\"%s\","
        "\"sequence\":0,\"event\":\"%s\",\"message\":\"\"}\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), sessionIdentity.data(),
        event);
    if (length > 0 && static_cast<size_t>(length) < record.size()) {
        // Reserve fatal evidence independently of the bounded ordinary emergency allowance.
        emergencyWrite(record.data(), static_cast<DWORD>(length));
    }
#else
    static_cast<void>(event);
#endif
}

void snow_diag_emergency(const char* record, size_t length) {
#ifdef _WIN32
    const size_t count = std::min<size_t>(length, 16384);
    if (emergencyBytes.fetch_add(count) >= 64 * 1024) {
        return;
    }
    emergencyWrite(record, static_cast<DWORD>(count));
#else
    static_cast<void>(record);
    static_cast<void>(length);
#endif
}

void snow_diag_breadcrumb(const char* record, size_t length) {
#ifdef _WIN32
    const size_t index = breadcrumbIndex.fetch_add(1) % breadcrumbs.size();
    if (breadcrumbBusy[index].exchange(true))
        return;
    auto& slot = breadcrumbs[index];
    const size_t count = std::min(length, slot.size() - 1);
    std::memcpy(slot.data(), record, count);
    slot[count] = '\n';
    breadcrumbBusy[index].store(false);
#else
    static_cast<void>(record);
    static_cast<void>(length);
#endif
}

void snow_diag_panic(const unsigned char* location, size_t length) {
#ifdef _WIN32
    if (fatalEntered.exchange(true)) {
        TerminateProcess(GetCurrentProcess(), 3);
        return;
    }
    const size_t count = std::min(length, fatalLocation.size() - 1);
    std::memcpy(fatalLocation.data(), location, count);
    fatalAnnotation.SetSize(static_cast<crashpad::Annotation::ValueSizeType>(count));
    snow_diag_fatal("runtime.panic");
    if (!collectorReady.load()) {
        TerminateProcess(GetCurrentProcess(), 3);
        return;
    }
    CONTEXT context{};
    crashpad::CaptureContext(&context);
    EXCEPTION_RECORD exception{};
    exception.ExceptionCode = 0xE0534E4F;
    exception.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    exception.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    EXCEPTION_POINTERS pointers{&exception, &context};
    crashpad::CrashpadClient::DumpAndCrash(&pointers);
#else
    static_cast<void>(location);
    static_cast<void>(length);
#endif
}

void snow_diag_shutdown(void) {
#ifdef _WIN32
    const HANDLE handle = emergencyHandle.exchange(INVALID_HANDLE_VALUE);
    closeEmergency(handle);
#endif
}
