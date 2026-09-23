// SPDX-License-Identifier: Apache-2.0
#include "diagnosticsbridge.h"

#include "client/annotation.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
crashpad::CrashpadClient client;
crashpad::SimpleStringDictionary annotations;
std::atomic<int> emergencyHandle{-1};
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
std::array<char, 64> sessionIdentity{};
std::terminate_handler previousTerminate = nullptr;
std::atomic<bool> fatalEntered{false};
std::atomic<bool> collectorReady{false};

void emergencyWrite(const char* record, size_t length) noexcept {
    emergencyUsers.fetch_add(1);
    const int fd = emergencyHandle.load();
    if (fd >= 0) {
        while (length != 0) {
            const ssize_t written = write(fd, record, length);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                break;
            record += written;
            length -= static_cast<size_t>(written);
        }
        fsync(fd);
    }
    emergencyUsers.fetch_sub(1);
}

void closeEmergency(int fd) {
    if (fd < 0)
        return;
    while (emergencyUsers.load() != 0)
        std::this_thread::yield();
    close(fd);
}

void terminateNow() noexcept {
    snow_diag_fatal("cpp.terminate");
    snow_diag_panic(reinterpret_cast<const unsigned char*>("cpp.terminate"), 13);
}

void configure(const char* role, const char* session, const char* version) {
    std::snprintf(sessionIdentity.data(), sessionIdentity.size(), "%s", session);
    annotations.SetKeyValue("product", "Snow Shot");
    annotations.SetKeyValue("role", role);
    annotations.SetKeyValue("session", session);
    annotations.SetKeyValue("version", version);
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

// Mach exception ports survive fork/exec (including QProcess' posix_spawn).
// Query the inherited port instead of publishing a globally discoverable service.
bool hasCrashPort() {
    exception_mask_t masks[EXC_TYPES_COUNT]{};
    mach_port_t ports[EXC_TYPES_COUNT]{};
    exception_behavior_t behaviors[EXC_TYPES_COUNT]{};
    thread_state_flavor_t flavors[EXC_TYPES_COUNT]{};
    mach_msg_type_number_t count = EXC_TYPES_COUNT;
    if (task_get_exception_ports(mach_task_self(), EXC_MASK_CRASH, masks, &count, ports, behaviors,
                                 flavors) != KERN_SUCCESS)
        return false;
    bool available = false;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        mach_port_type_t type = 0;
        if (MACH_PORT_VALID(ports[i])) {
            available |= mach_port_type(mach_task_self(), ports[i], &type) == KERN_SUCCESS &&
                         (type & MACH_PORT_TYPE_SEND) != 0;
            mach_port_deallocate(mach_task_self(), ports[i]);
        }
    }
    return available;
}
} // namespace

void snow_diag_prepare(const char* session, const char* version, const char* revision) {
    try {
        configure("application", session, version);
        annotations.SetKeyValue("revision", revision);
    } catch (...) {
    }
}

int snow_diag_start(const char* handler, const char* database, const char* session,
                    const char* version, const char* revision) {
    try {
        snow_diag_prepare(session, version, revision);
        const bool started =
            client.StartHandler(base::FilePath(handler), base::FilePath(database), base::FilePath(),
                                "", {}, {"--no-periodic-tasks"}, true, false);
        collectorReady.store(started);
        return started ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int snow_diag_attach(const char* endpoint, const char* session, const char* version) {
    try {
        if (std::strcmp(endpoint, "mach-inherited") != 0 || !hasCrashPort())
            return 0;
        configure("ocr", session, version);
        collectorReady.store(true);
        return 1;
    } catch (...) {
        return 0;
    }
}

const char* snow_diag_pipe(void) {
    return snow_diag_healthy() ? "mach-inherited" : "";
}

int snow_diag_healthy(void) {
    return collectorReady.load() && hasCrashPort() ? 1 : 0;
}

void snow_diag_open_emergency(const char* path) {
    // O_NOFOLLOW prevents replacing a diagnostic file with a symlink target.
    const int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat info{};
    if (fd >= 0 && (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))) {
        close(fd);
        return;
    }
    const int previous = emergencyHandle.exchange(fd);
    closeEmergency(previous);
    emergencyBytes.store(fd >= 0 ? static_cast<size_t>(info.st_size) : 0);
}

void snow_diag_fatal(const char* event) {
    // Called from explicit fatal/terminate/panic paths, never a POSIX signal handler.
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    tm utc{};
    gmtime_r(&now.tv_sec, &utc);
    uint64_t thread = 0;
    pthread_threadid_np(nullptr, &thread);
    std::array<char, 1024> record{};
    const int length = std::snprintf(
        record.data(), record.size(),
        "{\"time\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ\",\"level\":\"FATAL\","
        "\"category\":\"snow_shot.runtime\",\"pid\":%d,\"tid\":%llu,\"session\":\"%s\","
        "\"sequence\":0,\"event\":\"%s\",\"message\":\"\"}\n",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
        now.tv_nsec / 1000000, getpid(), static_cast<unsigned long long>(thread),
        sessionIdentity.data(), event);
    if (length > 0 && static_cast<size_t>(length) < record.size())
        emergencyWrite(record.data(), static_cast<size_t>(length));
}

void snow_diag_emergency(const char* record, size_t length) {
    const size_t count = std::min<size_t>(length, 16384);
    if (emergencyBytes.fetch_add(count) < 64 * 1024)
        emergencyWrite(record, count);
}

void snow_diag_breadcrumb(const char* record, size_t length) {
    const size_t index = breadcrumbIndex.fetch_add(1) % breadcrumbs.size();
    if (breadcrumbBusy[index].exchange(true))
        return;
    auto& slot = breadcrumbs[index];
    const size_t count = std::min(length, slot.size() - 1);
    std::memset(slot.data(), 0, slot.size());
    std::memcpy(slot.data(), record, count);
    slot[count] = '\n';
    breadcrumbBusy[index].store(false);
}

void snow_diag_panic(const unsigned char* location, size_t length) {
    if (fatalEntered.exchange(true))
        _exit(3);
    const size_t count = std::min(length, fatalLocation.size() - 1);
    std::memcpy(fatalLocation.data(), location, count);
    fatalAnnotation.SetSize(static_cast<crashpad::Annotation::ValueSizeType>(count));
    snow_diag_fatal("runtime.panic");
    // EXC_CRASH is delivered to the out-of-process collector on abort. No unsafe
    // Qt logging, allocation, or custom signal handler runs during crash capture.
    std::abort();
}

void snow_diag_shutdown(void) {
    collectorReady.store(false);
    if (previousTerminate != nullptr) {
        std::set_terminate(previousTerminate);
        previousTerminate = nullptr;
    }
    closeEmergency(emergencyHandle.exchange(-1));
}
