//! Whole-region ownership for large CPU rasters. FrameBuffer retains its usual
//! recycling and copy-on-write; the last owner releases these pages to the OS.

pub(crate) const MIN_PAGE_BUFFER_BYTES: usize = 1024 * 1024;

pub(crate) struct PageAllocation {
    pub(crate) ptr: *mut u8,
    pub(crate) len: usize,
    pub(crate) capacity: usize,
}

// The allocation is exclusively mutable through FrameBuffer's Arc::get_mut.
// Its address is stable, and either platform permits release on another thread.
unsafe impl Send for PageAllocation {}
unsafe impl Sync for PageAllocation {}

impl PageAllocation {
    pub(crate) fn new(size: usize) -> Option<Self> {
        if size == 0 || size > isize::MAX as usize {
            return None;
        }
        allocate(size)
    }
}

#[cfg(windows)]
fn allocate(size: usize) -> Option<PageAllocation> {
    use windows::Win32::System::Memory::{
        GetLargePageMinimum, MEM_COMMIT, MEM_LARGE_PAGES, MEM_RESERVE, PAGE_READWRITE, VirtualAlloc,
    };
    // Preserve the existing large-page optimization when the process has the
    // required privilege. Ordinary pages are also released explicitly on Drop.
    if size >= 4 * 1024 * 1024 {
        let page = unsafe { GetLargePageMinimum() };
        if page != 0
            && let Some(capacity) = size.checked_add(page - 1).map(|n| n & !(page - 1))
        {
            let ptr = unsafe {
                VirtualAlloc(
                    None,
                    capacity,
                    MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
                    PAGE_READWRITE,
                )
            };
            if !ptr.is_null() {
                return Some(PageAllocation {
                    ptr: ptr.cast(),
                    len: 0,
                    capacity,
                });
            }
        }
    }
    let ptr = unsafe { VirtualAlloc(None, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
    (!ptr.is_null()).then_some(PageAllocation {
        ptr: ptr.cast(),
        len: 0,
        capacity: size,
    })
}

#[cfg(windows)]
impl Drop for PageAllocation {
    fn drop(&mut self) {
        use windows::Win32::System::Memory::{MEM_RELEASE, VirtualFree};
        unsafe {
            let _ = VirtualFree(self.ptr.cast(), 0, MEM_RELEASE);
        }
    }
}

#[cfg(target_os = "macos")]
mod mach {
    // Public 64-bit Mach VM ABI from mach/mach_vm.h. Both supported macOS
    // architectures use 32-bit ports/return codes and 64-bit VM addresses/sizes.
    unsafe extern "C" {
        pub(super) static mach_task_self_: u32;
        pub(super) fn mach_vm_allocate(task: u32, address: *mut u64, size: u64, flags: i32) -> i32;
        pub(super) fn mach_vm_deallocate(task: u32, address: u64, size: u64) -> i32;
    }
}

#[cfg(target_os = "macos")]
fn allocate(size: usize) -> Option<PageAllocation> {
    let mut address = 0;
    // VM_FLAGS_ANYWHERE = 1. Fresh anonymous VM is zero-initialized.
    let result =
        unsafe { mach::mach_vm_allocate(mach::mach_task_self_, &mut address, size as u64, 1) };
    (result == 0).then_some(PageAllocation {
        ptr: address as *mut u8,
        len: 0,
        capacity: size,
    })
}

#[cfg(target_os = "macos")]
impl Drop for PageAllocation {
    fn drop(&mut self) {
        unsafe {
            mach::mach_vm_deallocate(mach::mach_task_self_, self.ptr as u64, self.capacity as u64);
        }
    }
}

#[cfg(test)]
pub(crate) fn is_mapped(address: *const u8) -> bool {
    #[cfg(target_os = "macos")]
    {
        unsafe extern "C" {
            fn mach_vm_read_overwrite(
                task: u32,
                address: u64,
                size: u64,
                destination: u64,
                copied: *mut u64,
            ) -> i32;
        }
        let mut byte = 0u8;
        let mut copied = 0;
        unsafe {
            mach_vm_read_overwrite(
                mach::mach_task_self_,
                address as u64,
                1,
                (&mut byte as *mut u8) as u64,
                &mut copied,
            ) == 0
        }
    }
    #[cfg(windows)]
    {
        use windows::Win32::System::Memory::{MEM_COMMIT, MEMORY_BASIC_INFORMATION, VirtualQuery};
        let mut information = MEMORY_BASIC_INFORMATION::default();
        (unsafe {
            VirtualQuery(
                Some(address.cast()),
                &mut information,
                std::mem::size_of::<MEMORY_BASIC_INFORMATION>(),
            ) != 0
        }) && information.State == MEM_COMMIT
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn page_allocation_is_zeroed_and_rejects_unaddressable_sizes() {
        assert!(PageAllocation::new(0).is_none());
        assert!(PageAllocation::new(usize::MAX).is_none());
        let pages = PageAllocation::new(MIN_PAGE_BUFFER_BYTES + 1).unwrap();
        let bytes = unsafe { std::slice::from_raw_parts_mut(pages.ptr, pages.capacity) };
        assert!(bytes.iter().all(|&byte| byte == 0));
        bytes.fill(0x5a);
        assert_eq!(bytes[bytes.len() - 1], 0x5a);
    }

    #[test]
    fn dropping_allocation_releases_the_vm_region() {
        let pages = PageAllocation::new(MIN_PAGE_BUFFER_BYTES).unwrap();
        let address = unsafe { pages.ptr.add(pages.capacity / 2) };
        assert!(is_mapped(address));
        drop(pages);
        assert!(!is_mapped(address));
    }
}
