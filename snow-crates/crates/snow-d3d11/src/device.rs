use std::marker::PhantomData;
use std::rc::Rc;
use std::sync::Arc;

use anyhow::{Context, Result, ensure};
use windows::Win32::Foundation::HMODULE;
use windows::Win32::Graphics::Direct3D::{D3D_DRIVER_TYPE_UNKNOWN, D3D_FEATURE_LEVEL_11_0};
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT, DXGI_SAMPLE_DESC};
use windows::Win32::Graphics::Dxgi::IDXGIAdapter;
use windows::core::Interface;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AdapterIdentity {
    pub luid: (u32, i32),
    pub vendor: u32,
    pub description: String,
}

impl AdapterIdentity {
    pub fn inspect(adapter: &IDXGIAdapter) -> Result<Self> {
        let desc = unsafe { adapter.GetDesc() }?;
        let end = desc
            .Description
            .iter()
            .position(|value| *value == 0)
            .unwrap_or(128);
        Ok(Self {
            luid: (desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart),
            vendor: desc.VendorId,
            description: String::from_utf16_lossy(&desc.Description[..end]),
        })
    }
}

struct DeviceInner {
    device: ID3D11Device,
    context: ID3D11DeviceContext,
    multithread: ID3D11Multithread,
    identity: AdapterIdentity,
}

#[derive(Clone)]
pub struct SharedDevice(Arc<DeviceInner>);

impl std::fmt::Debug for SharedDevice {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.identity().fmt(f)
    }
}

impl SharedDevice {
    pub fn create(adapter: &IDXGIAdapter) -> Result<Self> {
        let mut device = None;
        let mut context = None;
        unsafe {
            D3D11CreateDevice(
                adapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                HMODULE::default(),
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                Some(&[D3D_FEATURE_LEVEL_11_0]),
                D3D11_SDK_VERSION,
                Some(&mut device),
                None,
                Some(&mut context),
            )?;
        }
        let device = device.context("D3D11 did not return a device")?;
        let context = context.context("D3D11 did not return an immediate context")?;
        let multithread: ID3D11Multithread = context.cast()?;
        let _ = unsafe { multithread.SetMultithreadProtected(true) };
        Ok(Self(Arc::new(DeviceInner {
            device,
            context,
            multithread,
            identity: AdapterIdentity::inspect(adapter)?,
        })))
    }

    pub fn identity(&self) -> &AdapterIdentity {
        &self.0.identity
    }

    pub fn device(&self) -> &ID3D11Device {
        &self.0.device
    }

    /// Immediate-context sequences must hold `lock()`, including video processing.
    pub fn context(&self) -> &ID3D11DeviceContext {
        &self.0.context
    }

    /// The D3D11 multithread critical section is recursive. FFmpeg uses this
    /// same lock, so its internal calls may safely re-enter it.
    pub fn lock(&self) -> DeviceGuard<'_> {
        unsafe { self.0.multithread.Enter() };
        DeviceGuard {
            device: self,
            _thread: PhantomData,
        }
    }

    /// # Safety
    /// Must be paired with `leave` on the same thread. Intended for FFmpeg callbacks.
    pub unsafe fn enter(&self) {
        unsafe { self.0.multithread.Enter() };
    }

    /// # Safety
    /// The current thread must own a matching `enter`.
    pub unsafe fn leave(&self) {
        unsafe { self.0.multithread.Leave() };
    }

    pub fn check(&self) -> Result<()> {
        unsafe { self.device().GetDeviceRemovedReason() }.context("recording D3D11 device lost")
    }

    pub fn same_device(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }

    /// Current process allocation on this adapter, including capture and codec
    /// driver surfaces. Intended for benchmark sampling, not per-frame polling.
    pub fn video_memory_usage(&self) -> Result<u64> {
        use windows::Win32::Graphics::Dxgi::{
            DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_QUERY_VIDEO_MEMORY_INFO, IDXGIAdapter3,
            IDXGIDevice,
        };
        let dxgi: IDXGIDevice = self.device().cast()?;
        let adapter: IDXGIAdapter3 = unsafe { dxgi.GetAdapter() }?.cast()?;
        let mut info = DXGI_QUERY_VIDEO_MEMORY_INFO::default();
        unsafe { adapter.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mut info) }?;
        Ok(info.CurrentUsage)
    }

    pub fn texture(
        &self,
        width: u32,
        height: u32,
        format: DXGI_FORMAT,
        binds: u32,
    ) -> Result<Texture> {
        ensure!(width > 0 && height > 0, "empty GPU texture");
        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: binds,
            ..Default::default()
        };
        let mut texture = None;
        unsafe {
            self.device()
                .CreateTexture2D(&desc, None, Some(&mut texture))
        }?;
        Ok(Texture(Arc::new(TextureInner {
            texture: texture.context("D3D11 did not return a texture")?,
            device: self.clone(),
            desc,
        })))
    }
}

pub struct DeviceGuard<'a> {
    device: &'a SharedDevice,
    _thread: PhantomData<Rc<()>>,
}

impl Drop for DeviceGuard<'_> {
    fn drop(&mut self) {
        unsafe { self.device.leave() };
    }
}

struct TextureInner {
    texture: ID3D11Texture2D,
    device: SharedDevice,
    desc: D3D11_TEXTURE2D_DESC,
}

/// Clones are immutable leases. A pool may overwrite a texture only after every
/// published lease is released. Queued GPU operations use the same context.
#[derive(Clone)]
pub struct Texture(Arc<TextureInner>);

impl Texture {
    pub fn raw(&self) -> &ID3D11Texture2D {
        &self.0.texture
    }
    pub fn device(&self) -> &SharedDevice {
        &self.0.device
    }
    pub fn desc(&self) -> D3D11_TEXTURE2D_DESC {
        self.0.desc
    }
    pub fn dimensions(&self) -> (u32, u32) {
        (self.0.desc.Width, self.0.desc.Height)
    }
}

pub struct TexturePool {
    device: SharedDevice,
    capacity: usize,
    textures: Vec<Texture>,
    pressure: u64,
}

impl TexturePool {
    pub fn new(device: SharedDevice, capacity: usize) -> Self {
        Self {
            device,
            capacity,
            textures: Vec::new(),
            pressure: 0,
        }
    }

    pub fn pressure_count(&self) -> u64 {
        self.pressure
    }

    pub fn acquire(
        &mut self,
        width: u32,
        height: u32,
        format: DXGI_FORMAT,
        binds: u32,
    ) -> Result<Option<Texture>> {
        // Retired geometry counts against the budget until all users release it.
        self.textures.retain(|texture| {
            let desc = texture.desc();
            Arc::strong_count(&texture.0) > 1
                || (desc.Width == width
                    && desc.Height == height
                    && desc.Format == format
                    && desc.BindFlags == binds)
        });
        if let Some(texture) = self.textures.iter().find(|texture| {
            let desc = texture.desc();
            Arc::strong_count(&texture.0) == 1
                && desc.Width == width
                && desc.Height == height
                && desc.Format == format
                && desc.BindFlags == binds
        }) {
            return Ok(Some(texture.clone()));
        }
        if self.textures.len() >= self.capacity {
            self.pressure += 1;
            return Ok(None);
        }
        let texture = self.device.texture(width, height, format, binds)?;
        self.textures.push(texture.clone());
        Ok(Some(texture))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_B8G8R8A8_UNORM;
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};

    #[test]
    #[ignore = "requires an offscreen D3D11 device"]
    fn leases_bound_reuse_and_retired_geometry() -> Result<()> {
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }?;
        let adapter = unsafe { factory.EnumAdapters1(0) }?.cast()?;
        let device = SharedDevice::create(&adapter)?;
        let mut pool = TexturePool::new(device, 2);
        let first = pool
            .acquire(32, 32, DXGI_FORMAT_B8G8R8A8_UNORM, 0)?
            .unwrap();
        let retained = first.clone();
        let second = pool
            .acquire(64, 64, DXGI_FORMAT_B8G8R8A8_UNORM, 0)?
            .unwrap();
        assert!(
            pool.acquire(128, 128, DXGI_FORMAT_B8G8R8A8_UNORM, 0)?
                .is_none()
        );
        drop(first);
        assert!(
            pool.acquire(128, 128, DXGI_FORMAT_B8G8R8A8_UNORM, 0)?
                .is_none()
        );
        drop(retained);
        let replacement = pool
            .acquire(128, 128, DXGI_FORMAT_B8G8R8A8_UNORM, 0)?
            .unwrap();
        let raw = replacement.raw().clone();
        assert_ne!(raw, *second.raw());
        drop(replacement);
        assert_eq!(
            raw,
            *pool
                .acquire(128, 128, DXGI_FORMAT_B8G8R8A8_UNORM, 0)?
                .unwrap()
                .raw()
        );
        Ok(())
    }
}
