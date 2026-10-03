//! Test-only VM observation. Production allocation is owned by snow-memory.
#[cfg(test)]
pub(crate) fn is_mapped(address: *const u8) -> bool {
    #[cfg(target_os = "macos")]
    {
        unsafe extern "C" {
            static mach_task_self_: u32;
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
                mach_task_self_,
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
