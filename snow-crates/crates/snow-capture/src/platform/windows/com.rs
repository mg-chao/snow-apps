use anyhow::{Context, Result};
use std::sync::OnceLock;
use windows::Win32::Foundation::RPC_E_CHANGED_MODE;
use windows::Win32::System::Com::{
    COINIT_MULTITHREADED, CoIncrementMTAUsage, CoInitializeEx, CoUninitialize,
};

pub(crate) fn ensure_process_mta_usage() -> Result<()> {
    static MTA_COOKIE: OnceLock<Result<usize, String>> = OnceLock::new();
    match MTA_COOKIE.get_or_init(|| {
        // windows-core caches agile WinRT factories for process lifetime. Keep
        // the MTA alive for the same lifetime so their implementation DLLs
        // cannot unload while a cached factory pointer is still reachable.
        unsafe { CoIncrementMTAUsage() }
            .map(|cookie| cookie.0 as usize)
            .map_err(|error| error.to_string())
    }) {
        Ok(_) => Ok(()),
        Err(error) => Err(anyhow::anyhow!(error.clone()))
            .context("failed to retain process MTA usage for WinRT capture"),
    }
}

pub(crate) struct CoInitGuard {
    should_uninit: bool,
}

impl CoInitGuard {
    pub fn init_multithreaded() -> Result<Self> {
        let hr = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) };
        if hr == RPC_E_CHANGED_MODE {
            return Ok(Self {
                should_uninit: false,
            });
        }

        hr.ok()
            .context("failed to initialize COM with CoInitializeEx(COINIT_MULTITHREADED)")?;
        Ok(Self {
            should_uninit: true,
        })
    }
}

impl Drop for CoInitGuard {
    fn drop(&mut self) {
        if self.should_uninit {
            unsafe {
                CoUninitialize();
            }
        }
    }
}
