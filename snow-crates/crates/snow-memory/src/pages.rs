#[cfg(any(windows, target_os = "macos", target_os = "linux"))]
use std::{io, ptr::NonNull};

#[cfg(any(windows, target_os = "macos", target_os = "linux"))]
pub(super) struct PageAllocation {
    pointer: NonNull<u8>,
    pub(super) capacity: usize,
}

// Only a unique RasterBuffer can expose mutable bytes. An allocation has a
// stable address and may be released by a different thread on all platforms.
#[cfg(any(windows, target_os = "macos", target_os = "linux"))]
unsafe impl Send for PageAllocation {}
#[cfg(any(windows, target_os = "macos", target_os = "linux"))]
unsafe impl Sync for PageAllocation {}

#[cfg(any(windows, target_os = "macos", target_os = "linux"))]
impl PageAllocation {
    pub(super) fn as_slice(&self, len: usize) -> &[u8] {
        debug_assert!(len <= self.capacity);
        // OS allocations are zero-initialized, including unused capacity.
        unsafe { std::slice::from_raw_parts(self.pointer.as_ptr(), len) }
    }
    pub(super) fn as_mut_slice(&mut self, len: usize) -> &mut [u8] {
        debug_assert!(len <= self.capacity);
        unsafe { std::slice::from_raw_parts_mut(self.pointer.as_ptr(), len) }
    }
}

#[cfg(windows)]
mod native {
    use std::ffi::c_void;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        pub(super) fn VirtualAlloc(
            address: *mut c_void,
            size: usize,
            flags: u32,
            protection: u32,
        ) -> *mut c_void;
        pub(super) fn VirtualFree(address: *mut c_void, size: usize, flags: u32) -> i32;
        pub(super) fn GetLargePageMinimum() -> usize;
    }
}
#[cfg(windows)]
impl PageAllocation {
    pub(super) fn new(size: usize) -> io::Result<Self> {
        // Preserve capture's privileged large-page optimization. Failure to
        // acquire large pages uses ordinary OS pages, never the retained heap.
        if size >= 4 * 1024 * 1024 {
            let page = unsafe { native::GetLargePageMinimum() };
            if page != 0
                && let Some(capacity) = size.checked_add(page - 1).map(|n| n & !(page - 1))
                && capacity <= isize::MAX as usize
            {
                let pointer = unsafe {
                    native::VirtualAlloc(std::ptr::null_mut(), capacity, 0x3000 | 0x2000_0000, 0x04)
                };
                if let Some(pointer) = NonNull::new(pointer.cast()) {
                    return Ok(Self { pointer, capacity });
                }
            }
        }
        let pointer = unsafe { native::VirtualAlloc(std::ptr::null_mut(), size, 0x3000, 0x04) };
        NonNull::new(pointer.cast())
            .map(|pointer| Self {
                pointer,
                capacity: size,
            })
            .ok_or_else(io::Error::last_os_error)
    }
}
#[cfg(windows)]
impl Drop for PageAllocation {
    fn drop(&mut self) {
        // MEM_RELEASE requires the original base and a zero size.
        unsafe {
            native::VirtualFree(self.pointer.as_ptr().cast(), 0, 0x8000);
        }
    }
}

#[cfg(target_os = "macos")]
mod native {
    unsafe extern "C" {
        pub(super) static mach_task_self_: u32;
        pub(super) fn mach_vm_allocate(task: u32, address: *mut u64, size: u64, flags: i32) -> i32;
        pub(super) fn mach_vm_deallocate(task: u32, address: u64, size: u64) -> i32;
    }
}
#[cfg(target_os = "macos")]
impl PageAllocation {
    pub(super) fn new(size: usize) -> io::Result<Self> {
        let mut address = 0;
        let result = unsafe {
            native::mach_vm_allocate(native::mach_task_self_, &mut address, size as u64, 1)
        };
        if result != 0 {
            return Err(io::Error::other(format!(
                "Mach VM allocation failed: {result}"
            )));
        }
        let pointer = NonNull::new(address as *mut u8).expect("Mach returned a null allocation");
        Ok(Self {
            pointer,
            capacity: size,
        })
    }
}
#[cfg(target_os = "macos")]
impl Drop for PageAllocation {
    fn drop(&mut self) {
        unsafe {
            native::mach_vm_deallocate(
                native::mach_task_self_,
                self.pointer.as_ptr() as u64,
                self.capacity as u64,
            );
        }
    }
}

#[cfg(target_os = "linux")]
mod native {
    use std::ffi::c_void;
    unsafe extern "C" {
        pub(super) fn mmap(
            address: *mut c_void,
            size: usize,
            protection: i32,
            flags: i32,
            fd: i32,
            offset: i64,
        ) -> *mut c_void;
        pub(super) fn munmap(address: *mut c_void, size: usize) -> i32;
    }
}
#[cfg(target_os = "linux")]
impl PageAllocation {
    pub(super) fn new(size: usize) -> io::Result<Self> {
        let pointer = unsafe { native::mmap(std::ptr::null_mut(), size, 0x03, 0x02 | 0x20, -1, 0) };
        if pointer as isize == -1 {
            return Err(io::Error::last_os_error());
        }
        Ok(Self {
            pointer: NonNull::new(pointer.cast()).expect("mmap returned a null allocation"),
            capacity: size,
        })
    }
}
#[cfg(target_os = "linux")]
impl Drop for PageAllocation {
    fn drop(&mut self) {
        unsafe {
            native::munmap(self.pointer.as_ptr().cast(), self.capacity);
        }
    }
}

#[cfg(all(test, any(windows, target_os = "macos", target_os = "linux")))]
mod tests {
    use crate::{MIN_PAGE_BUFFER_BYTES, RasterBuffer};
    #[test]
    fn whole_region_is_released_after_truncate_and_last_shared_owner() {
        // Other tests can reuse a just-freed VM address. Perform the OS query
        // in an isolated test process so parallel raster allocations cannot
        // masquerade as a retained region.
        const CHILD: &str = "SNOW_MEMORY_RELEASE_TEST_CHILD";
        if std::env::var_os(CHILD).is_none() {
            let status = std::process::Command::new(std::env::current_exe().unwrap())
                .args([
                    "--exact",
                    "pages::tests::whole_region_is_released_after_truncate_and_last_shared_owner",
                    "--test-threads=1",
                ])
                .env(CHILD, "1")
                .status()
                .unwrap();
            assert!(status.success());
            return;
        }
        let mut buffer = RasterBuffer::zeroed(MIN_PAGE_BUFFER_BYTES + 17);
        buffer.fill(0x5a);
        let middle = unsafe { buffer.as_ptr().add(MIN_PAGE_BUFFER_BYTES / 2) };
        buffer.truncate(3);
        let original = std::sync::Arc::new(buffer);
        let last = original.clone();
        drop(original);
        assert!(is_mapped(middle));
        drop(last);
        assert!(!is_mapped(middle));
    }
    fn is_mapped(address: *const u8) -> bool {
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
                    super::native::mach_task_self_,
                    address as u64,
                    1,
                    (&mut byte as *mut u8) as u64,
                    &mut copied,
                ) == 0
            }
        }
        #[cfg(target_os = "linux")]
        {
            std::fs::read_to_string("/proc/self/maps")
                .unwrap()
                .lines()
                .any(|line| {
                    let (start, end) = line
                        .split_whitespace()
                        .next()
                        .unwrap()
                        .split_once('-')
                        .unwrap();
                    (usize::from_str_radix(start, 16).unwrap()
                        ..usize::from_str_radix(end, 16).unwrap())
                        .contains(&(address as usize))
                })
        }
        #[cfg(windows)]
        {
            #[repr(C)]
            struct Information {
                base: *mut std::ffi::c_void,
                allocation: *mut std::ffi::c_void,
                protection: u32,
                partition: u16,
                size: usize,
                state: u32,
                protect: u32,
                kind: u32,
            }
            #[link(name = "kernel32")]
            unsafe extern "system" {
                fn VirtualQuery(
                    address: *const u8,
                    information: *mut Information,
                    size: usize,
                ) -> usize;
            }
            let mut information: Information = unsafe { std::mem::zeroed() };
            (unsafe {
                VirtualQuery(
                    address,
                    &mut information,
                    std::mem::size_of::<Information>(),
                )
            } != 0)
                && information.state == 0x1000
        }
    }
}
