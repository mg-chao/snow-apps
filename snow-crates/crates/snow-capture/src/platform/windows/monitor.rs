use std::mem;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use rustc_hash::FxHashMap;

use anyhow::Context;
use windows::Win32::Devices::Display::{
    DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,
    DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
    DISPLAYCONFIG_DEVICE_INFO_HEADER, DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO,
    DISPLAYCONFIG_MODE_INFO, DISPLAYCONFIG_PATH_INFO, DISPLAYCONFIG_SDR_WHITE_LEVEL,
    DISPLAYCONFIG_SOURCE_DEVICE_NAME, DisplayConfigGetDeviceInfo, GetDisplayConfigBufferSizes,
    QDC_ONLY_ACTIVE_PATHS, QueryDisplayConfig,
};
use windows::Win32::Foundation::POINT;
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020,
};
use windows::Win32::Graphics::Dxgi::{
    CreateDXGIFactory1, DXGI_ERROR_NOT_FOUND, IDXGIAdapter, IDXGIFactory1, IDXGIOutput,
    IDXGIOutput6,
};
use windows::Win32::Graphics::Gdi::{HMONITOR, MONITOR_DEFAULTTOPRIMARY, MonitorFromPoint};
use windows::core::Interface;

use crate::convert::HdrFrameContext;
use crate::error::{CaptureError, CaptureResult};
use crate::monitor::{MonitorId, MonitorKey};
use crate::region::{MonitorGeometry, MonitorLayout};

use super::display_change::DisplayInfoCache;

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct HdrMonitorMetadata {
    pub advanced_color_enabled: bool,
    pub hdr_enabled: bool,
    pub color_space: i32,
    pub sdr_white_nits: Option<f32>,
    pub hdr_peak_nits: Option<f32>,
}

#[derive(Clone)]
pub(crate) struct ResolvedMonitor {
    pub key: MonitorKey,
    pub name: String,
    pub handle: HMONITOR,
    pub adapter: IDXGIAdapter,
    pub output: IDXGIOutput,
    pub hdr_metadata: HdrMonitorMetadata,
}

#[derive(Default)]
struct MonitorCache {
    monitors: Vec<MonitorId>,
    refreshed_at: Option<Instant>,
}

pub(crate) struct MonitorResolver {
    /// Event-driven cache backed by WM_DISPLAYCHANGE.
    display_cache: Option<Arc<DisplayInfoCache>>,
    /// Fallback TTL-based cache when no event-driven cache is provided.
    cache: Mutex<MonitorCache>,
    ttl: Duration,
}

impl MonitorResolver {
    #[allow(dead_code)]
    pub(crate) fn new(ttl: Duration) -> Self {
        Self {
            display_cache: None,
            cache: Mutex::new(MonitorCache::default()),
            ttl,
        }
    }

    /// Create a resolver backed by a shared `DisplayInfoCache`.
    /// When present, enumeration reads from the event-driven cache
    /// instead of polling DXGI on a TTL.
    pub(crate) fn with_display_cache(display_cache: Arc<DisplayInfoCache>) -> Self {
        Self {
            display_cache: Some(display_cache),
            cache: Mutex::new(MonitorCache::default()),
            ttl: Duration::ZERO,
        }
    }

    pub(crate) fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        if let Some(dc) = &self.display_cache {
            return dc.monitors();
        }
        self.current_monitors(false)
    }

    pub(crate) fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        self.enumerate_monitors()?
            .into_iter()
            .find(|monitor| monitor.is_primary())
            .ok_or(CaptureError::NoPrimaryMonitor)
    }

    /// Return the display-change generation counter, if backed by an
    /// event-driven cache.  Returns `None` when no `DisplayInfoCache` is
    /// present (TTL-only mode).  The value is bumped every time
    /// `WM_DISPLAYCHANGE` fires.
    pub(crate) fn display_generation(&self) -> Option<u64> {
        self.display_cache.as_ref().map(|dc| dc.generation())
    }

    pub(crate) fn refresh_display_configuration(&self) -> CaptureResult<()> {
        if let Some(dc) = &self.display_cache {
            return dc.refresh();
        }
        self.current_monitors(true).map(|_| ())
    }

    pub(crate) fn resolve_monitor(&self, id: &MonitorId) -> CaptureResult<ResolvedMonitor> {
        if let Some(dc) = &self.display_cache {
            let resolved = dc.resolved()?;
            return resolved
                .into_iter()
                .find(|candidate| candidate.key == id.key())
                .ok_or(CaptureError::MonitorLost);
        }
        let resolved = enumerate_resolved()?;
        self.update_cache_from_resolved(&resolved)?;
        resolved
            .into_iter()
            .find(|candidate| candidate.key == id.key())
            .ok_or(CaptureError::MonitorLost)
    }

    fn current_monitors(&self, force_refresh: bool) -> CaptureResult<Vec<MonitorId>> {
        {
            let cache = self.cache.lock().map_err(|_| {
                CaptureError::platform(anyhow::anyhow!("windows monitor cache mutex was poisoned"))
            })?;
            let should_refresh = force_refresh
                || cache.monitors.is_empty()
                || cache
                    .refreshed_at
                    .map(|ts| ts.elapsed() >= self.ttl)
                    .unwrap_or(true);
            if !should_refresh {
                return Ok(cache.monitors.clone());
            }
        }

        let resolved = enumerate_resolved()?;
        self.update_cache_from_resolved(&resolved)
    }

    fn update_cache_from_resolved(
        &self,
        resolved: &[ResolvedMonitor],
    ) -> CaptureResult<Vec<MonitorId>> {
        let monitors = to_monitor_ids(resolved);
        let mut cache = self.cache.lock().map_err(|_| {
            CaptureError::platform(anyhow::anyhow!("windows monitor cache mutex was poisoned"))
        })?;
        cache.monitors = monitors.clone();
        cache.refreshed_at = Some(Instant::now());
        Ok(monitors)
    }
}

pub(crate) fn to_monitor_ids(resolved: &[ResolvedMonitor]) -> Vec<MonitorId> {
    let primary_hmon = primary_hmonitor();
    resolved
        .iter()
        .map(|monitor| {
            MonitorId::from_parts(
                monitor.key.adapter_luid,
                monitor.key.output_id,
                monitor.handle.0 as isize,
                monitor.name.clone(),
                monitor.handle == primary_hmon,
            )
        })
        .collect()
}

pub(crate) fn snapshot_layout_from_monitors(
    monitors: Vec<MonitorId>,
) -> CaptureResult<MonitorLayout> {
    use std::mem::size_of;
    use windows::Win32::Graphics::Gdi::{GetMonitorInfoW, HMONITOR, MONITORINFO, MONITORINFOEXW};

    if monitors.is_empty() {
        return Err(CaptureError::NoPrimaryMonitor);
    }

    let mut geometries = Vec::with_capacity(monitors.len());
    for monitor in monitors {
        let hmon = HMONITOR(monitor.raw_handle() as *mut std::ffi::c_void);
        if hmon.0.is_null() {
            return Err(CaptureError::MonitorLost);
        }

        let mut info = MONITORINFOEXW {
            monitorInfo: MONITORINFO {
                cbSize: size_of::<MONITORINFOEXW>() as u32,
                ..Default::default()
            },
            ..Default::default()
        };

        if !unsafe { GetMonitorInfoW(hmon, (&mut info as *mut MONITORINFOEXW).cast()) }.as_bool() {
            return Err(CaptureError::MonitorLost);
        }

        let rect = info.monitorInfo.rcMonitor;
        let width = rect.right - rect.left;
        let height = rect.bottom - rect.top;
        if width <= 0 || height <= 0 {
            continue;
        }

        geometries.push(MonitorGeometry {
            monitor,
            x: rect.left,
            y: rect.top,
            width: width as u32,
            height: height as u32,
        });
    }

    if geometries.is_empty() {
        return Err(CaptureError::MonitorLost);
    }

    let mut virtual_left = i32::MAX;
    let mut virtual_top = i32::MAX;
    let mut virtual_right = i32::MIN;
    let mut virtual_bottom = i32::MIN;
    for geometry in &geometries {
        virtual_left = virtual_left.min(geometry.x);
        virtual_top = virtual_top.min(geometry.y);
        virtual_right = virtual_right.max(geometry.x + geometry.width as i32);
        virtual_bottom = virtual_bottom.max(geometry.y + geometry.height as i32);
    }

    Ok(MonitorLayout {
        monitors: geometries,
        virtual_left,
        virtual_top,
        virtual_width: (virtual_right - virtual_left) as u32,
        virtual_height: (virtual_bottom - virtual_top) as u32,
    })
}

fn primary_hmonitor() -> HMONITOR {
    unsafe { MonitorFromPoint(POINT { x: 0, y: 0 }, MONITOR_DEFAULTTOPRIMARY) }
}

fn luid_to_u64(luid: windows::Win32::Foundation::LUID) -> u64 {
    (u64::from(luid.HighPart as u32) << 32) | u64::from(luid.LowPart)
}

#[derive(Clone, Copy, Debug, Default)]
struct DisplayConfigHdrInfo {
    advanced_color_enabled: bool,
    sdr_white_nits: Option<f32>,
}

fn utf16z_to_string(input: &[u16]) -> String {
    let len = input.iter().position(|&ch| ch == 0).unwrap_or(input.len());
    String::from_utf16_lossy(&input[..len])
}

fn query_displayconfig_hdr_map() -> FxHashMap<String, DisplayConfigHdrInfo> {
    let mut path_count = 0u32;
    let mut mode_count = 0u32;
    if unsafe {
        GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut path_count, &mut mode_count)
    }
    .ok()
    .is_err()
    {
        return FxHashMap::default();
    }

    if path_count == 0 {
        return FxHashMap::default();
    }

    let mut paths = vec![DISPLAYCONFIG_PATH_INFO::default(); path_count as usize];
    let mut modes = vec![DISPLAYCONFIG_MODE_INFO::default(); mode_count as usize];
    if unsafe {
        QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &mut path_count,
            paths.as_mut_ptr(),
            &mut mode_count,
            modes.as_mut_ptr(),
            None,
        )
    }
    .ok()
    .is_err()
    {
        return FxHashMap::default();
    }

    let mut map = FxHashMap::default();
    let count = usize::min(path_count as usize, paths.len());
    for path in &paths[..count] {
        let mut source = DISPLAYCONFIG_SOURCE_DEVICE_NAME {
            header: DISPLAYCONFIG_DEVICE_INFO_HEADER {
                r#type: DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                size: mem::size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32,
                adapterId: path.sourceInfo.adapterId,
                id: path.sourceInfo.id,
            },
            ..Default::default()
        };
        if unsafe { DisplayConfigGetDeviceInfo(&mut source.header) } != 0 {
            continue;
        }
        let gdi_name = utf16z_to_string(&source.viewGdiDeviceName);
        if gdi_name.is_empty() {
            continue;
        }

        let mut advanced = DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO {
            header: DISPLAYCONFIG_DEVICE_INFO_HEADER {
                r#type: DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,
                size: mem::size_of::<DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO>() as u32,
                adapterId: path.targetInfo.adapterId,
                id: path.targetInfo.id,
            },
            ..Default::default()
        };
        let advanced_color_enabled =
            if unsafe { DisplayConfigGetDeviceInfo(&mut advanced.header) } == 0 {
                let flags = unsafe { advanced.Anonymous.value };

                (flags & 0x1) != 0 && (flags & 0x2) != 0
            } else {
                false
            };

        let mut sdr_white = DISPLAYCONFIG_SDR_WHITE_LEVEL {
            header: DISPLAYCONFIG_DEVICE_INFO_HEADER {
                r#type: DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,
                size: mem::size_of::<DISPLAYCONFIG_SDR_WHITE_LEVEL>() as u32,
                adapterId: path.targetInfo.adapterId,
                id: path.targetInfo.id,
            },
            ..Default::default()
        };
        let sdr_white_nits = if advanced_color_enabled
            && unsafe { DisplayConfigGetDeviceInfo(&mut sdr_white.header) } == 0
        {
            Some((sdr_white.SDRWhiteLevel as f32) * 80.0 / 1000.0)
        } else {
            None
        };

        let entry = map
            .entry(gdi_name)
            .or_insert_with(DisplayConfigHdrInfo::default);
        entry.advanced_color_enabled |= advanced_color_enabled;
        if entry.sdr_white_nits.is_none() {
            entry.sdr_white_nits = sdr_white_nits;
        }
    }

    map
}

fn query_dxgi_hdr_metadata(output: &IDXGIOutput) -> HdrMonitorMetadata {
    let mut metadata = HdrMonitorMetadata::default();
    let Ok(output6) = output.cast::<IDXGIOutput6>() else {
        return metadata;
    };
    let Ok(desc1) = (unsafe { output6.GetDesc1() }) else {
        return metadata;
    };

    metadata.color_space = desc1.ColorSpace.0;
    metadata.hdr_enabled = matches!(
        desc1.ColorSpace,
        DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 | DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020
    );

    if desc1.MaxLuminance.is_finite() && desc1.MaxLuminance > 0.0 {
        metadata.hdr_peak_nits = Some(desc1.MaxLuminance);
    }
    metadata
}

pub(crate) fn enumerate_resolved() -> CaptureResult<Vec<ResolvedMonitor>> {
    let displayconfig_hdr_map = query_displayconfig_hdr_map();

    let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }
        .context("CreateDXGIFactory1 failed")
        .map_err(CaptureError::platform)?;

    let mut monitors = Vec::new();
    let mut adapter_idx = 0u32;

    loop {
        let adapter1 = match unsafe { factory.EnumAdapters1(adapter_idx) } {
            Ok(a) => a,
            Err(e) if e.code() == DXGI_ERROR_NOT_FOUND => break,
            Err(e) => {
                return Err(CaptureError::platform(
                    anyhow::Error::from(e).context(format!("EnumAdapters1({adapter_idx}) failed")),
                ));
            }
        };
        let adapter_desc = unsafe { adapter1.GetDesc1() }
            .context("IDXGIAdapter1::GetDesc1 failed")
            .map_err(CaptureError::platform)?;
        let adapter_luid = luid_to_u64(adapter_desc.AdapterLuid);

        let adapter: IDXGIAdapter = adapter1
            .cast()
            .context("failed to cast IDXGIAdapter1 to IDXGIAdapter")
            .map_err(CaptureError::platform)?;

        let mut output_idx = 0u32;
        loop {
            let output = match unsafe { adapter.EnumOutputs(output_idx) } {
                Ok(o) => o,
                Err(e) if e.code() == DXGI_ERROR_NOT_FOUND => break,
                Err(e) => {
                    return Err(CaptureError::platform(anyhow::Error::from(e).context(
                        format!("EnumOutputs({output_idx}) on adapter {adapter_idx} failed"),
                    )));
                }
            };

            let desc = unsafe { output.GetDesc() }
                .context("GetDesc failed")
                .map_err(CaptureError::platform)?;

            if desc.AttachedToDesktop.as_bool() {
                let name = utf16z_to_string(&desc.DeviceName);
                let mut hdr_metadata = query_dxgi_hdr_metadata(&output);
                if let Some(displayconfig_hdr) = displayconfig_hdr_map.get(&name) {
                    hdr_metadata.advanced_color_enabled = displayconfig_hdr.advanced_color_enabled;
                    if hdr_metadata.sdr_white_nits.is_none() {
                        hdr_metadata.sdr_white_nits = displayconfig_hdr.sdr_white_nits;
                    }
                }
                monitors.push(ResolvedMonitor {
                    key: MonitorKey::from_device_name(adapter_luid, &name),
                    name,
                    handle: desc.Monitor,
                    adapter: adapter.clone(),
                    output,
                    hdr_metadata,
                });
            }

            output_idx += 1;
        }

        adapter_idx += 1;
    }

    Ok(monitors)
}
pub(crate) fn hdr_to_sdr_params(hdr: HdrMonitorMetadata) -> Option<HdrFrameContext> {
    if !hdr.advanced_color_enabled {
        return None;
    }

    if !hdr.hdr_enabled {
        return None;
    }

    let sdr_white_nits = hdr.sdr_white_nits.unwrap_or(80.0);
    let hdr_peak_nits = hdr.hdr_peak_nits.unwrap_or(1000.0);

    Some(HdrFrameContext {
        sdr_white_nits,
        hdr_peak_nits,
        ..HdrFrameContext::default()
    })
}
