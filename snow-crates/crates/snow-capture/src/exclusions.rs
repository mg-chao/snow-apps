//! Owned, normalized ScreenCaptureKit filters and their shared C boundary.
use std::sync::Arc;

pub const MAX_EXCLUSIONS: usize = 4096;
pub type OwnedExclusions = (Arc<[u32]>, Arc<[i32]>);

/// Arrays are borrowed only during a C create call. Window IDs are macOS
/// WindowServer IDs, never NSWindow/NSView pointers or durable identities.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SnowCaptureExclusions {
    pub windows: *const u32,
    pub window_count: usize,
    pub processes: *const i32,
    pub process_count: usize,
}

impl SnowCaptureExclusions {
    /// # Safety
    /// Each nonempty array must contain `count` readable, aligned entries for
    /// this call. The returned arrays own their storage.
    pub unsafe fn to_owned(self) -> Result<OwnedExclusions, String> {
        if self.window_count > MAX_EXCLUSIONS
            || self.process_count > MAX_EXCLUSIONS
            || (self.window_count != 0 && self.windows.is_null())
            || (self.process_count != 0 && self.processes.is_null())
        {
            return Err("capture exclusion list is invalid or exceeds 4096 entries".into());
        }
        let mut windows = if self.window_count == 0 {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(self.windows, self.window_count) }.to_vec()
        };
        let mut processes = if self.process_count == 0 {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(self.processes, self.process_count) }.to_vec()
        };
        windows.sort_unstable();
        windows.dedup();
        processes.sort_unstable();
        processes.dedup();
        Ok((windows.into(), processes.into()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn copies_and_normalizes_both_lists() {
        let mut windows = [9, 7, 9];
        let processes = [42, 21, 42];
        let raw = SnowCaptureExclusions {
            windows: windows.as_ptr(),
            window_count: windows.len(),
            processes: processes.as_ptr(),
            process_count: processes.len(),
        };
        let (owned_windows, owned_processes) = unsafe { raw.to_owned() }.unwrap();
        windows[0] = 100;
        assert_eq!(&*owned_windows, &[7, 9]);
        assert_eq!(&*owned_processes, &[21, 42]);
        assert_eq!(windows[0], 100);
    }
    #[test]
    fn rejects_invalid_lists_before_reading() {
        for raw in [
            SnowCaptureExclusions {
                window_count: 1,
                ..Default::default()
            },
            SnowCaptureExclusions {
                process_count: 1,
                ..Default::default()
            },
            SnowCaptureExclusions {
                window_count: 4097,
                windows: std::ptr::dangling(),
                ..Default::default()
            },
            SnowCaptureExclusions {
                process_count: 4097,
                processes: std::ptr::dangling(),
                ..Default::default()
            },
        ] {
            assert!(unsafe { raw.to_owned() }.is_err());
        }
        assert!(unsafe { SnowCaptureExclusions::default().to_owned() }.is_ok());
    }
}
