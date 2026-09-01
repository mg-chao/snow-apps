use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
    sync::{OnceLock, RwLock},
    time::Duration,
};

#[cfg(not(target_os = "windows"))]
use std::sync::Mutex;

#[cfg(target_os = "windows")]
use std::{ffi::OsStr, os::windows::ffi::OsStrExt, ptr};

use reqwest::blocking::Client;
use sha2::{Digest, Sha256};

use crate::error::{RapidOcrError, Result};

static DOWNLOAD_PROXY: OnceLock<RwLock<Option<String>>> = OnceLock::new();

/// Configure the proxy used for model downloads. The value is process-local and
/// should be set before constructing any OCR engine.
pub fn set_download_proxy(proxy: Option<String>) {
    let slot = DOWNLOAD_PROXY.get_or_init(|| RwLock::new(None));
    if let Ok(mut value) = slot.write() {
        *value = proxy.filter(|value| !value.trim().is_empty());
    }
}

#[cfg(not(target_os = "windows"))]
static DOWNLOAD_LOCK: OnceLock<Mutex<()>> = OnceLock::new();

struct DownloadGuard {
    #[cfg(target_os = "windows")]
    handle: windows_sys::Win32::Foundation::HANDLE,
    #[cfg(not(target_os = "windows"))]
    _guard: std::sync::MutexGuard<'static, ()>,
}

fn acquire_download_guard(save_dir: &Path) -> Result<DownloadGuard> {
    #[cfg(target_os = "windows")]
    {
        use windows_sys::Win32::{
            Foundation::{CloseHandle, WAIT_ABANDONED, WAIT_OBJECT_0},
            System::Threading::{CreateMutexW, INFINITE, WaitForSingleObject},
        };

        // A named kernel mutex serializes model downloads across all Snow Shot
        // processes that share the same application-storage directory.
        let mut hasher = Sha256::new();
        hasher.update(save_dir.to_string_lossy().as_bytes());
        let name = format!("Local\\SnowShotRapidOcrModel-{:x}", hasher.finalize());
        let wide_name: Vec<u16> = OsStr::new(&name)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect();
        let handle = unsafe { CreateMutexW(ptr::null(), 0, wide_name.as_ptr()) };
        if handle.is_null() {
            return Err(RapidOcrError::Download(
                "unable to create OCR model download mutex".to_string(),
            ));
        }
        let wait_result = unsafe { WaitForSingleObject(handle, INFINITE) };
        if wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED {
            unsafe {
                CloseHandle(handle);
            }
            return Err(RapidOcrError::Download(
                "waiting for OCR model download mutex failed".to_string(),
            ));
        }
        Ok(DownloadGuard { handle })
    }

    #[cfg(not(target_os = "windows"))]
    {
        let guard = DOWNLOAD_LOCK
            .get_or_init(|| Mutex::new(()))
            .lock()
            .map_err(|_| RapidOcrError::Download("model download lock was poisoned".to_string()))?;
        Ok(DownloadGuard { _guard: guard })
    }
}

impl Drop for DownloadGuard {
    fn drop(&mut self) {
        #[cfg(target_os = "windows")]
        unsafe {
            use windows_sys::Win32::{Foundation::CloseHandle, System::Threading::ReleaseMutex};
            ReleaseMutex(self.handle);
            CloseHandle(self.handle);
        }
    }
}

pub fn default_model_store_dir() -> PathBuf {
    if let Ok(local_app_data) = std::env::var("LOCALAPPDATA") {
        return PathBuf::from(local_app_data)
            .join("rapid-ocr-rs")
            .join("models");
    }

    PathBuf::from("models")
}

pub fn verify_existing_file(path: impl AsRef<Path>) -> Result<PathBuf> {
    let path = path.as_ref().to_path_buf();
    if !path.exists() {
        return Err(RapidOcrError::FileNotFound(path));
    }
    if !path.is_file() {
        return Err(RapidOcrError::Config(format!(
            "expected a file path, got directory: {}",
            path.display()
        )));
    }
    Ok(path)
}

pub fn ensure_downloaded(
    file_url: &str,
    expected_sha256: Option<&str>,
    save_dir: impl AsRef<Path>,
) -> Result<PathBuf> {
    let save_dir = save_dir.as_ref();
    fs::create_dir_all(save_dir)?;

    // Hold the guard across the existence check and atomic replacement so only
    // one transfer owns the shared .part file at a time.
    let _lock = acquire_download_guard(save_dir)?;

    let file_name = extract_file_name(file_url)?;
    let target_path = save_dir.join(file_name);

    if target_path.exists() {
        if let Some(expected) = expected_sha256 {
            let actual = sha256_file(&target_path)?;
            if actual.eq_ignore_ascii_case(expected) {
                return Ok(target_path);
            }
        } else {
            return Ok(target_path);
        }
    }

    let tmp_path = target_path.with_extension("part");
    if tmp_path.exists() {
        let _ = fs::remove_file(&tmp_path);
    }

    let client = build_http_client()?;
    let mut response = client
        .get(file_url)
        .header(
            reqwest::header::USER_AGENT,
            "Mozilla/5.0 (compatible; rapid-ocr-rs model downloader)",
        )
        .header(reqwest::header::REFERER, "https://www.modelscope.cn/")
        .send()?;
    if !response.status().is_success() {
        return Err(RapidOcrError::Download(format!(
            "failed to download {file_url}: HTTP {}",
            response.status()
        )));
    }

    let mut hasher = Sha256::new();
    let mut file = fs::File::create(&tmp_path)?;
    let mut buf = [0_u8; 16 * 1024];
    loop {
        let read = response.read(&mut buf)?;
        if read == 0 {
            break;
        }
        hasher.update(&buf[..read]);
        file.write_all(&buf[..read])?;
    }
    file.flush()?;
    file.sync_all()?;

    if let Some(expected) = expected_sha256 {
        let actual = format!("{:x}", hasher.finalize());
        if !actual.eq_ignore_ascii_case(expected) {
            let _ = fs::remove_file(&tmp_path);
            return Err(RapidOcrError::HashMismatch {
                path: target_path,
                expected: expected.to_string(),
                actual,
            });
        }
    }

    if target_path.exists() {
        fs::remove_file(&target_path)?;
    }
    fs::rename(&tmp_path, &target_path)?;

    Ok(target_path)
}

fn build_http_client() -> Result<Client> {
    let mut builder = Client::builder();
    if let Some(proxy) = DOWNLOAD_PROXY
        .get()
        .and_then(|slot| slot.read().ok())
        .and_then(|value| value.clone())
    {
        let configured = reqwest::Proxy::all(&proxy).map_err(|error| {
            RapidOcrError::Download(format!("invalid OCR model proxy: {error}"))
        })?;
        builder = builder.proxy(configured);
    }
    builder
        .timeout(Duration::from_secs(60))
        .build()
        .map_err(Into::into)
}

fn extract_file_name(url: &str) -> Result<String> {
    let trimmed = url.split('?').next().unwrap_or(url);
    let file_name = trimmed
        .rsplit('/')
        .next()
        .filter(|s| !s.is_empty())
        .ok_or_else(|| {
            RapidOcrError::Download(format!("cannot extract file name from url: {url}"))
        })?;
    Ok(file_name.to_string())
}

fn sha256_file(path: impl AsRef<Path>) -> Result<String> {
    let bytes = fs::read(path.as_ref())?;
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    Ok(format!("{:x}", hasher.finalize()))
}
