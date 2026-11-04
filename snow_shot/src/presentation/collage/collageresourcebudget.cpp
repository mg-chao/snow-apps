// SPDX-License-Identifier: GPL-3.0-only
#include "collageresourcebudget_p.h"
#include "snow_shot/presentation/collagedocument.h"

#include <QCoreApplication>

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace snow_shot::presentation::collage_detail {
namespace {
constexpr qint64 maximumBudget = qint64{32} * 1024 * 1024 * 1024;
constexpr qint64 minimumHeadroom = qint64{512} * 1024 * 1024;
constexpr qint64 maximumPreviewBytes = qint64{256} * 1024 * 1024;

qint64 physicalMemory() {
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<qint64>(std::min<quint64>(
            status.ullTotalPhys, static_cast<quint64>(std::numeric_limits<qint64>::max())));
#elif defined(Q_OS_MACOS)
    uint64_t bytes = 0;
    size_t length = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &length, nullptr, 0) == 0)
        return static_cast<qint64>(
            std::min<uint64_t>(bytes, static_cast<uint64_t>(std::numeric_limits<qint64>::max())));
#endif
    return maximumBudget * 2;
}

qint64 availableMemory() {
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<qint64>(
            std::min<quint64>(std::min(status.ullAvailPhys, status.ullAvailPageFile),
                              static_cast<quint64>(std::numeric_limits<qint64>::max())));
#elif defined(Q_OS_MACOS)
    vm_statistics64_data_t status{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    const auto host = mach_host_self();
    const bool obtained =
        host_page_size(host, &pageSize) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&status), &count) ==
            KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (obtained)
        return (static_cast<qint64>(status.free_count) +
                static_cast<qint64>(status.inactive_count)) *
               static_cast<qint64>(pageSize);
#endif
    return std::numeric_limits<qint64>::max();
}

struct Budget final {
    std::mutex mutex;
    std::unordered_map<const ResourceReservation*, qint64> reservations;
    qint64 total = 0;
    qint64 working = 0;
    qint64 preview = 0;
    const qint64 limit = std::min(maximumBudget, physicalMemory() / 2);
};

Budget& budget() {
    // Immutable image snapshots can outlive application windows and retire on workers.
    // Keep the accounting service valid during late static/application teardown.
    static auto* shared = new Budget;
    return *shared;
}

void setError(QString* error) {
    if (error)
        *error = QCoreApplication::translate(
            "CollageDocument",
            "There is not enough available memory for this collage operation. Try fewer images "
            "or close other collage windows");
}
} // namespace

ResourceReservation::~ResourceReservation() {
    static_cast<void>(resize(0));
}

bool ResourceReservation::resize(qint64 bytes, QString* error) {
    auto& shared = budget();
    const std::lock_guard lock(shared.mutex);
    const auto found = shared.reservations.find(this);
    const qint64 previous = found == shared.reservations.end() ? 0 : found->second;
    if (bytes < 0 || bytes > shared.limit) {
        setError(error);
        return false;
    }
    const qint64 additional = bytes - previous;
    if ((m_working && additional > 0 && additional > maximumWorkingBytes - shared.working) ||
        (m_preview && additional > 0 && additional > maximumPreviewBytes - shared.preview) ||
        (additional > 0 && (additional > shared.limit - shared.total ||
                            availableMemory() < additional + minimumHeadroom))) {
        setError(error);
        return false;
    }
    if (bytes != 0) {
        try {
            if (found == shared.reservations.end())
                shared.reservations.emplace(this, bytes);
            else
                found->second = bytes;
        } catch (const std::bad_alloc&) {
            setError(error);
            return false;
        }
    }
    shared.total += additional;
    if (m_working)
        shared.working += additional;
    if (m_preview)
        shared.preview += additional;
    if (bytes == 0)
        shared.reservations.erase(this);
    return true;
}

std::shared_ptr<ResourceReservation> reserve(qint64 bytes, QString* error) {
    try {
        auto reservation = std::make_shared<ResourceReservation>();
        return reservation->resize(bytes, error) ? reservation : nullptr;
    } catch (const std::bad_alloc&) {
        setError(error);
        return {};
    }
}

std::shared_ptr<ResourceReservation> reserveWorking(qint64 bytes, QString* error) {
    try {
        auto reservation = std::make_shared<ResourceReservation>(true);
        return reservation->resize(bytes, error) ? reservation : nullptr;
    } catch (const std::bad_alloc&) {
        setError(error);
        return {};
    }
}

std::shared_ptr<ResourceReservation> reservePreview(qint64 bytes, QString* error) {
    try {
        auto reservation = std::make_shared<ResourceReservation>(false, true);
        return reservation->resize(bytes, error) ? reservation : nullptr;
    } catch (const std::bad_alloc&) {
        setError(error);
        return {};
    }
}

qint64 reservedBytes() {
    auto& shared = budget();
    const std::lock_guard lock(shared.mutex);
    return shared.total;
}

qint64 budgetLimit() {
    return budget().limit;
}
} // namespace snow_shot::presentation::collage_detail

namespace snow_shot::presentation {
struct CollageImportReservation::Impl final {
    // Imports own encoded input and retained rasters as well as decode scratch. Charging all
    // of that storage to the 512 MiB painting pool rejects otherwise admissible long images.
    collage_detail::ResourceReservation reservation;
};

CollageImportReservation::CollageImportReservation() : m_impl(std::make_unique<Impl>()) {}
CollageImportReservation::~CollageImportReservation() = default;

bool CollageImportReservation::resize(qint64 bytes, QString* error) {
    return m_impl->reservation.resize(bytes, error);
}

std::shared_ptr<CollageImportReservation> reserveCollageImport(qint64 bytes, QString* error) {
    try {
        auto reservation = std::make_shared<CollageImportReservation>();
        return reservation->resize(bytes, error) ? reservation : nullptr;
    } catch (const std::bad_alloc&) {
        if (error)
            *error = QCoreApplication::translate(
                "CollageDocument",
                "There is not enough available memory for this collage operation. Try fewer images "
                "or close other collage windows");
        return {};
    }
}

std::shared_ptr<void> reserveCollagePreview(qint64 bytes, QString* error) {
    return collage_detail::reservePreview(bytes, error);
}
} // namespace snow_shot::presentation
