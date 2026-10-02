// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_TEST_SUPPORT_VIRTUAL_MEMORY_H
#define SNOW_TEST_SUPPORT_VIRTUAL_MEMORY_H

#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace snow::test_support {
// Query the address map, not RSS or residency. In particular, macOS mincore
// can succeed with zero residency bits for an unmapped address.
inline bool virtualMemoryMapped(const void* address) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION information{};
    return VirtualQuery(address, &information, sizeof(information)) != 0 &&
           information.State == MEM_COMMIT;
#elif defined(__APPLE__)
    mach_vm_address_t base = reinterpret_cast<mach_vm_address_t>(address);
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto result = mach_vm_region(mach_task_self(), &base, &size, VM_REGION_BASIC_INFO_64,
                                       reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), object);
    const auto query = reinterpret_cast<mach_vm_address_t>(address);
    return result == KERN_SUCCESS && base <= query && query - base < size;
#else
    const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    void* base = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(address) & ~(page - 1));
    unsigned char residency = 0;
    return mincore(base, page, &residency) == 0;
#endif
}
} // namespace snow::test_support
#endif
