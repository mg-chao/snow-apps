use anyhow::Context;
use std::sync::OnceLock;
use windows::Win32::Graphics::Direct3D11::{
    D3D11_BIND_CONSTANT_BUFFER, D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_UNORDERED_ACCESS,
    D3D11_BUFFER_DESC, D3D11_CPU_ACCESS_WRITE, D3D11_MAP_WRITE_DISCARD, D3D11_MAPPED_SUBRESOURCE,
    D3D11_SUBRESOURCE_DATA, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT, D3D11_USAGE_DYNAMIC,
    ID3D11Buffer, ID3D11ComputeShader, ID3D11Device, ID3D11DeviceContext, ID3D11ShaderResourceView,
    ID3D11Texture2D, ID3D11UnorderedAccessView,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_MODE_ROTATION,
    DXGI_MODE_ROTATION_IDENTITY, DXGI_SAMPLE_DESC,
};
use windows::core::Interface;

use crate::color_effect::ScreenColorTransform;
use crate::convert::{HDR_LUMA_LUT_SIZE, HdrFrameContext, build_bt2390_luma_lut};
use crate::error::{CaptureError, CaptureResult};

/// HLSL source kept as fallback for runtime compilation when fxc.exe
/// was not available at build time.
#[cfg(not(has_precompiled_shader))]
const HLSL_SOURCE: &str = include_str!("tonemap_cs.hlsl");

/// Pre-compiled shader bytecode, embedded at build time when fxc.exe is available.
#[cfg(has_precompiled_shader)]
const PRECOMPILED_CSO: &[u8] = include_bytes!(env!("TONEMAP_CSO_PATH"));

/// Pre-compiled 1D shader bytecode for small textures.
#[cfg(has_precompiled_shader_1d)]
const PRECOMPILED_1D_CSO: &[u8] = include_bytes!(env!("TONEMAP_1D_CSO_PATH"));

#[cfg(has_precompiled_shader_f16)]
const PRECOMPILED_F16_CSO: &[u8] = include_bytes!(env!("F16_CONVERT_CSO_PATH"));

#[cfg(has_precompiled_shader_f16_1d)]
const PRECOMPILED_F16_1D_CSO: &[u8] = include_bytes!(env!("F16_CONVERT_1D_CSO_PATH"));

#[cfg(not(has_precompiled_shader))]
fn compile_shader_runtime() -> CaptureResult<Vec<u8>> {
    compile_shader_runtime_with_entry(b"main\0")
}

/// Returns cached shader bytecode. Prefers build-time compiled .cso
/// (embedded via `TONEMAP_CSO_PATH` env var from build.rs), falls back
/// to runtime D3DCompile on first call.
fn cached_bytecode() -> &'static CaptureResult<Vec<u8>> {
    static BYTECODE: OnceLock<CaptureResult<Vec<u8>>> = OnceLock::new();
    BYTECODE.get_or_init(|| {
        #[cfg(has_precompiled_shader)]
        {
            Ok(PRECOMPILED_CSO.to_vec())
        }
        #[cfg(not(has_precompiled_shader))]
        {
            compile_shader_runtime()
        }
    })
}

/// Returns cached 1D shader bytecode for small-texture dispatch.
fn cached_bytecode_1d() -> &'static CaptureResult<Vec<u8>> {
    static BYTECODE: OnceLock<CaptureResult<Vec<u8>>> = OnceLock::new();
    BYTECODE.get_or_init(|| {
        #[cfg(has_precompiled_shader_1d)]
        {
            Ok(PRECOMPILED_1D_CSO.to_vec())
        }
        #[cfg(not(has_precompiled_shader_1d))]
        {
            compile_shader_runtime_with_entry(b"main_1d\0")
        }
    })
}

fn cached_bytecode_f16() -> &'static CaptureResult<Vec<u8>> {
    static BYTECODE: OnceLock<CaptureResult<Vec<u8>>> = OnceLock::new();
    BYTECODE.get_or_init(|| {
        #[cfg(has_precompiled_shader_f16)]
        {
            Ok(PRECOMPILED_F16_CSO.to_vec())
        }
        #[cfg(not(has_precompiled_shader_f16))]
        {
            compile_shader_runtime_with_entry(b"main_f16\0")
        }
    })
}

fn cached_bytecode_f16_1d() -> &'static CaptureResult<Vec<u8>> {
    static BYTECODE: OnceLock<CaptureResult<Vec<u8>>> = OnceLock::new();
    BYTECODE.get_or_init(|| {
        #[cfg(has_precompiled_shader_f16_1d)]
        {
            Ok(PRECOMPILED_F16_1D_CSO.to_vec())
        }
        #[cfg(not(has_precompiled_shader_f16_1d))]
        {
            compile_shader_runtime_with_entry(b"main_f16_1d\0")
        }
    })
}

#[cfg(any(
    not(has_precompiled_shader),
    not(has_precompiled_shader_1d),
    not(has_precompiled_shader_f16),
    not(has_precompiled_shader_f16_1d),
))]
fn compile_shader_runtime_with_entry(entry: &[u8]) -> CaptureResult<Vec<u8>> {
    use windows::Win32::Graphics::Direct3D::Fxc::D3DCompile;
    use windows::core::PCSTR;

    let source = include_str!("tonemap_cs.hlsl").as_bytes();
    let entry_pcstr = PCSTR::from_raw(entry.as_ptr());
    let target = PCSTR::from_raw(b"cs_5_0\0".as_ptr());
    let mut blob = None;
    let mut errors = None;

    let hr = unsafe {
        D3DCompile(
            source.as_ptr() as *const _,
            source.len(),
            None,
            None,
            None,
            entry_pcstr,
            target,
            0,
            0,
            &mut blob,
            Some(&mut errors),
        )
    };

    if let Err(e) = hr {
        let msg = errors
            .map(|b| {
                let ptr = unsafe { b.GetBufferPointer() } as *const u8;
                let len = unsafe { b.GetBufferSize() };
                let slice = unsafe { std::slice::from_raw_parts(ptr, len) };
                String::from_utf8_lossy(slice).to_string()
            })
            .unwrap_or_default();
        return Err(CaptureError::platform(
            anyhow::anyhow!("HLSL compile failed: {msg}").context(e.to_string()),
        ));
    }

    let blob =
        blob.ok_or_else(|| CaptureError::platform(anyhow::anyhow!("D3DCompile returned no blob")))?;
    let ptr = unsafe { blob.GetBufferPointer() } as *const u8;
    let len = unsafe { blob.GetBufferSize() };
    Ok(unsafe { std::slice::from_raw_parts(ptr, len) }.to_vec())
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
struct GpuParams {
    sdr_white_nits: f32,
    hdr_peak_nits: f32,
    sdr_identity_eps: f32,
    flags: u32,
    tex_width: u32,
    tex_height: u32,
    lut_size_minus_one: u32,
    rotation: u32,
    lut_input_max: f32,
    lut_inv_step: f32,
    _pad1: f32,
    _pad2: f32,
    color_rows: [[f32; 4]; 3],
}

impl GpuParams {
    fn with_color_transform(mut self, transform: Option<ScreenColorTransform>) -> Self {
        if let Some(transform) = transform {
            self.flags |= GPU_FLAG_RESTORE_COLORS;
            self.color_rows = transform.linear_rows();
        }
        self
    }
}

const GPU_FLAG_USE_LUT: u32 = 1;
const GPU_FLAG_RESTORE_COLORS: u32 = 2;

/// Threshold below which we use the 1D dispatch path.
/// For textures smaller than 512px on either axis, the 16x16 thread
/// groups waste significant threads on boundary tiles.
const SMALL_TEXTURE_THRESHOLD: u32 = 512;

/// Shared GPU compute-shader pass infrastructure.
///
/// Encapsulates the D3D11 resources and caching logic common to both
/// texture/UAV management, SRV caching, and the dispatch call.
struct GpuComputePass {
    cs: ID3D11ComputeShader,
    /// 1D compute shader for small textures (256x1 thread groups).
    cs_1d: Option<ID3D11ComputeShader>,
    cbuf: ID3D11Buffer,
    output_tex: Option<ID3D11Texture2D>,
    output_uav: Option<ID3D11UnorderedAccessView>,
    /// Cached SRV for the source texture. Reused when the source texture
    /// COM pointer hasn't changed between frames (common when the desktop
    /// hasn't updated).
    cached_srv: Option<ID3D11ShaderResourceView>,
    cached_srv_source: usize, // raw COM pointer of the texture the SRV was created for
    cached_width: u32,
    cached_height: u32,
    output_desc: Option<D3D11_TEXTURE2D_DESC>,
}

impl GpuComputePass {
    /// Creates a new compute pass from the given shader bytecodes.
    /// `bytecode_1d` failure is non-fatal (falls back to 2D dispatch).
    fn new(
        device: &ID3D11Device,
        bytecode: &[u8],
        bytecode_1d: Option<&[u8]>,
        label: &str,
    ) -> CaptureResult<Self> {
        let mut cs: Option<ID3D11ComputeShader> = None;
        unsafe { device.CreateComputeShader(bytecode, None, Some(&mut cs)) }
            .context(format!("CreateComputeShader ({label}) failed"))
            .map_err(CaptureError::platform)?;
        let cs = cs
            .context(format!("CreateComputeShader ({label}) returned None"))
            .map_err(CaptureError::platform)?;

        let cs_1d = bytecode_1d.and_then(|bc| {
            let mut shader: Option<ID3D11ComputeShader> = None;
            unsafe { device.CreateComputeShader(bc, None, Some(&mut shader)) }.ok()?;
            shader
        });

        let cbuf_desc = D3D11_BUFFER_DESC {
            ByteWidth: std::mem::size_of::<GpuParams>() as u32,
            Usage: D3D11_USAGE_DYNAMIC,
            BindFlags: D3D11_BIND_CONSTANT_BUFFER.0 as u32,
            CPUAccessFlags: D3D11_CPU_ACCESS_WRITE.0 as u32,
            ..Default::default()
        };
        let mut cbuf: Option<ID3D11Buffer> = None;
        unsafe { device.CreateBuffer(&cbuf_desc, None, Some(&mut cbuf)) }
            .context(format!("CreateBuffer ({label}) for constant buffer failed"))
            .map_err(CaptureError::platform)?;
        let cbuf = cbuf
            .context(format!("CreateBuffer ({label}) returned None"))
            .map_err(CaptureError::platform)?;

        Ok(Self {
            cs,
            cs_1d,
            cbuf,
            output_tex: None,
            output_uav: None,
            cached_srv: None,
            cached_srv_source: 0,
            cached_width: 0,
            cached_height: 0,
            output_desc: None,
        })
    }

    fn ensure_output(
        &mut self,
        device: &ID3D11Device,
        width: u32,
        height: u32,
    ) -> CaptureResult<()> {
        if self.cached_width == width && self.cached_height == height && self.output_tex.is_some() {
            return Ok(());
        }

        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_UNORDERED_ACCESS.0 as u32,
            ..Default::default()
        };

        let mut tex: Option<ID3D11Texture2D> = None;
        unsafe { device.CreateTexture2D(&desc, None, Some(&mut tex)) }
            .context("CreateTexture2D for compute output failed")
            .map_err(CaptureError::platform)?;
        let tex = tex
            .context("CreateTexture2D returned None")
            .map_err(CaptureError::platform)?;

        let mut uav: Option<ID3D11UnorderedAccessView> = None;
        unsafe { device.CreateUnorderedAccessView(&tex, None, Some(&mut uav)) }
            .context("CreateUnorderedAccessView failed")
            .map_err(CaptureError::platform)?;
        let uav = uav
            .context("CreateUnorderedAccessView returned None")
            .map_err(CaptureError::platform)?;

        self.output_tex = Some(tex);
        self.output_uav = Some(uav);
        self.cached_width = width;
        self.cached_height = height;
        self.output_desc = Some(desc);
        Ok(())
    }

    /// Returns a cached or freshly created SRV for the given source texture.
    fn get_or_create_srv(
        &mut self,
        device: &ID3D11Device,
        source: &ID3D11Texture2D,
    ) -> CaptureResult<ID3D11ShaderResourceView> {
        let source_ptr = source.as_raw() as usize;
        if source_ptr == self.cached_srv_source
            && let Some(ref srv) = self.cached_srv
        {
            return Ok(srv.clone());
        }

        let mut srv: Option<ID3D11ShaderResourceView> = None;
        unsafe { device.CreateShaderResourceView(source, None, Some(&mut srv)) }
            .context("CreateShaderResourceView for source failed")
            .map_err(CaptureError::platform)?;
        let srv = srv
            .context("CreateShaderResourceView returned None")
            .map_err(CaptureError::platform)?;

        self.cached_srv = Some(srv.clone());
        self.cached_srv_source = source_ptr;
        Ok(srv)
    }

    /// Uploads `gpu_params` to the constant buffer via Map/Unmap.
    fn update_cbuf(
        &self,
        context: &ID3D11DeviceContext,
        gpu_params: &GpuParams,
    ) -> CaptureResult<()> {
        let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
        unsafe { context.Map(&self.cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, Some(&mut mapped)) }
            .context("Map constant buffer failed")
            .map_err(CaptureError::platform)?;
        unsafe {
            std::ptr::copy_nonoverlapping(
                gpu_params as *const GpuParams as *const u8,
                mapped.pData as *mut u8,
                std::mem::size_of::<GpuParams>(),
            );
            context.Unmap(&self.cbuf, 0);
        }
        Ok(())
    }

    /// Binds resources, dispatches the compute shader, and unbinds.
    fn dispatch(
        &self,
        context: &ID3D11DeviceContext,
        source_srv: ID3D11ShaderResourceView,
        lut_srv: Option<ID3D11ShaderResourceView>,
        width: u32,
        height: u32,
    ) {
        let uav = self.output_uav.as_ref().unwrap();

        unsafe {
            let use_1d = (width < SMALL_TEXTURE_THRESHOLD || height < SMALL_TEXTURE_THRESHOLD)
                && self.cs_1d.is_some();

            if use_1d {
                context.CSSetShader(self.cs_1d.as_ref().unwrap(), None);
            } else {
                context.CSSetShader(&self.cs, None);
            }
            context.CSSetConstantBuffers(0, Some(&[Some(self.cbuf.clone())]));
            context.CSSetShaderResources(0, Some(&[Some(source_srv), lut_srv]));
            context.CSSetUnorderedAccessViews(0, 1, Some(&Some(uav.clone()) as *const _), None);

            if use_1d {
                let groups_x = width.div_ceil(256);
                context.Dispatch(groups_x, height, 1);
            } else {
                let groups_x = width.div_ceil(16);
                let groups_y = height.div_ceil(16);
                context.Dispatch(groups_x, groups_y, 1);
            }

            let no_srv: Option<ID3D11ShaderResourceView> = None;
            context.CSSetShaderResources(0, Some(&[no_srv.clone(), no_srv]));
            context.CSSetUnorderedAccessViews(0, 1, Some(&None as *const _), None);
        }
    }

    fn output_tex(&self) -> &ID3D11Texture2D {
        self.output_tex.as_ref().unwrap()
    }

    fn output_desc(&self) -> D3D11_TEXTURE2D_DESC {
        self.output_desc
            .expect("output descriptor must be available after ensure_output")
    }

    fn release_capture_surfaces(&mut self) {
        self.output_uav = None;
        self.output_tex = None;
        self.cached_srv = None;
        self.cached_srv_source = 0;
        self.cached_width = 0;
        self.cached_height = 0;
        self.output_desc = None;
    }
}

pub(crate) struct GpuTonemapper {
    pass: GpuComputePass,
    rotation: DXGI_MODE_ROTATION,
    /// Combined cache of tonemap params and dimensions written to the
    cached_cbuf_state: Option<GpuParams>,
    cached_lut_state: Option<(HdrFrameContext, f32, f32)>,
    lut_tex: Option<ID3D11Texture2D>,
    lut_srv: Option<ID3D11ShaderResourceView>,
    lut_disabled: bool,
}

impl GpuTonemapper {
    pub(crate) fn new(device: &ID3D11Device) -> CaptureResult<Self> {
        let bytecode = cached_bytecode().as_ref().map_err(|e| {
            CaptureError::platform(anyhow::anyhow!("shader compilation failed: {e}"))
        })?;
        let bytecode_1d = cached_bytecode_1d().as_ref().ok().map(|v| v.as_slice());

        let pass = GpuComputePass::new(device, bytecode, bytecode_1d, "tonemap")?;
        Ok(Self {
            pass,
            rotation: DXGI_MODE_ROTATION_IDENTITY,
            cached_cbuf_state: None,
            cached_lut_state: None,
            lut_tex: None,
            lut_srv: None,
            lut_disabled: false,
        })
    }

    pub(crate) fn set_rotation(&mut self, rotation: DXGI_MODE_ROTATION) {
        self.rotation = rotation;
    }

    fn ensure_lut(
        &mut self,
        device: &ID3D11Device,
        params: HdrFrameContext,
    ) -> CaptureResult<(ID3D11ShaderResourceView, f32, f32)> {
        if let Some((cached_params, input_max, inv_step)) = self.cached_lut_state
            && cached_params == params
            && let Some(ref srv) = self.lut_srv
        {
            return Ok((srv.clone(), input_max, inv_step));
        }

        let lut = build_bt2390_luma_lut(params);
        let desc = D3D11_TEXTURE2D_DESC {
            Width: HDR_LUMA_LUT_SIZE as u32,
            Height: 1,
            MipLevels: 1,
            ArraySize: 1,
            Format: DXGI_FORMAT_R32_FLOAT,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_SHADER_RESOURCE.0 as u32,
            ..Default::default()
        };
        let init = D3D11_SUBRESOURCE_DATA {
            pSysMem: lut.values_ptr() as *const _,
            SysMemPitch: (HDR_LUMA_LUT_SIZE * std::mem::size_of::<f32>()) as u32,
            SysMemSlicePitch: 0,
        };

        let mut tex: Option<ID3D11Texture2D> = None;
        unsafe { device.CreateTexture2D(&desc, Some(&init), Some(&mut tex)) }
            .context("CreateTexture2D for tonemap LUT failed")
            .map_err(CaptureError::platform)?;
        let tex = tex
            .context("CreateTexture2D for tonemap LUT returned None")
            .map_err(CaptureError::platform)?;

        let mut srv: Option<ID3D11ShaderResourceView> = None;
        unsafe { device.CreateShaderResourceView(&tex, None, Some(&mut srv)) }
            .context("CreateShaderResourceView for tonemap LUT failed")
            .map_err(CaptureError::platform)?;
        let srv = srv
            .context("CreateShaderResourceView for tonemap LUT returned None")
            .map_err(CaptureError::platform)?;

        self.cached_lut_state = Some((params, lut.input_max(), lut.inv_step()));
        self.lut_tex = Some(tex);
        self.lut_srv = Some(srv.clone());
        Ok((srv, lut.input_max(), lut.inv_step()))
    }

    /// Runs the HDR-to-SDR compute shader on the GPU.
    /// `source` must be an R16G16B16A16_FLOAT texture.
    /// Returns a reference to the RGBA8 output texture.
    pub(crate) fn tonemap(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        source: &ID3D11Texture2D,
        source_desc: &D3D11_TEXTURE2D_DESC,
        params: HdrFrameContext,
        screen_color_transform: Option<ScreenColorTransform>,
    ) -> CaptureResult<&ID3D11Texture2D> {
        let params = params.sanitized();
        let (width, height) =
            super::rotation::oriented_size(source_desc.Width, source_desc.Height, self.rotation);
        self.pass.ensure_output(device, width, height)?;

        let (lut_srv, lut_input_max, lut_inv_step, flags) =
            if params.tonemap_use_lut && !self.lut_disabled {
                match self.ensure_lut(device, params) {
                    Ok((srv, input_max, inv_step)) => {
                        (Some(srv), input_max, inv_step, GPU_FLAG_USE_LUT)
                    }
                    Err(_) => {
                        self.lut_disabled = true;
                        (None, 0.0, 0.0, 0)
                    }
                }
            } else {
                if !params.tonemap_use_lut {
                    self.lut_disabled = false;
                }
                (None, 0.0, 0.0, 0)
            };

        let gpu_params = GpuParams {
            sdr_white_nits: params.sdr_white_nits,
            hdr_peak_nits: params.hdr_peak_nits,
            sdr_identity_eps: 1e-3,
            flags,
            tex_width: width,
            tex_height: height,
            lut_size_minus_one: (HDR_LUMA_LUT_SIZE - 1) as u32,
            rotation: if super::rotation::is_rotated(self.rotation) {
                (self.rotation.0 - 1) as u32
            } else {
                0
            },
            lut_input_max,
            lut_inv_step,
            _pad1: 0.0,
            _pad2: 0.0,
            color_rows: [[0.; 4]; 3],
        }
        .with_color_transform(screen_color_transform);
        if self.cached_cbuf_state != Some(gpu_params) {
            self.pass.update_cbuf(context, &gpu_params)?;
            self.cached_cbuf_state = Some(gpu_params);
        }

        let srv = self.pass.get_or_create_srv(device, source)?;
        self.pass.dispatch(context, srv, lut_srv, width, height);
        Ok(self.pass.output_tex())
    }

    pub(crate) fn output_desc(&self) -> D3D11_TEXTURE2D_DESC {
        self.pass.output_desc()
    }

    pub(crate) fn release_capture_surfaces(&mut self) {
        self.pass.release_capture_surfaces();
    }
}

///
/// Used when the source is RGBA16Float but no HDR-to-SDR tonemap is needed.
/// Converts linear light values directly to sRGB gamma on the GPU, so the
/// CPU readback path only needs to handle RGBA8 (a simple memcpy-equivalent).
pub(crate) struct GpuF16Converter {
    pass: GpuComputePass,
    cached_cbuf_state: Option<GpuParams>,
}

impl GpuF16Converter {
    pub(crate) fn new(device: &ID3D11Device) -> CaptureResult<Self> {
        let bytecode = cached_bytecode_f16().as_ref().map_err(|e| {
            CaptureError::platform(anyhow::anyhow!("F16 shader compilation failed: {e}"))
        })?;
        let bytecode_1d = cached_bytecode_f16_1d().as_ref().ok().map(|v| v.as_slice());

        let pass = GpuComputePass::new(device, bytecode, bytecode_1d, "F16")?;
        Ok(Self {
            pass,
            cached_cbuf_state: None,
        })
    }

    /// Converts an F16 linear texture to RGBA8 sRGB on the GPU.
    /// Returns a reference to the RGBA8 output texture.
    pub(crate) fn convert(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        source: &ID3D11Texture2D,
        source_desc: &D3D11_TEXTURE2D_DESC,
        screen_color_transform: Option<ScreenColorTransform>,
    ) -> CaptureResult<&ID3D11Texture2D> {
        let width = source_desc.Width;
        let height = source_desc.Height;
        self.pass.ensure_output(device, width, height)?;

        let gpu_params = GpuParams {
            sdr_white_nits: 0.0,
            hdr_peak_nits: 0.0,
            sdr_identity_eps: 0.0,
            flags: 0,
            tex_width: width,
            tex_height: height,
            lut_size_minus_one: 0,
            rotation: 0,
            lut_input_max: 0.0,
            lut_inv_step: 0.0,
            _pad1: 0.0,
            _pad2: 0.0,
            color_rows: [[0.; 4]; 3],
        }
        .with_color_transform(screen_color_transform);
        if self.cached_cbuf_state != Some(gpu_params) {
            self.pass.update_cbuf(context, &gpu_params)?;
            self.cached_cbuf_state = Some(gpu_params);
        }

        let srv = self.pass.get_or_create_srv(device, source)?;
        self.pass.dispatch(context, srv, None, width, height);
        Ok(self.pass.output_tex())
    }

    pub(crate) fn output_desc(&self) -> D3D11_TEXTURE2D_DESC {
        self.pass.output_desc()
    }

    pub(crate) fn release_capture_surfaces(&mut self) {
        self.pass.release_capture_surfaces();
    }
}

#[cfg(test)]
mod tests {
    use super::super::{rotation, surface};
    use super::*;
    use crate::backend::CaptureBlitRegion;
    use crate::frame::{CapturePixelFormat, Frame};
    use windows::Win32::Graphics::Direct3D::{D3D_DRIVER_TYPE_WARP, D3D_FEATURE_LEVEL_11_0};
    use windows::Win32::Graphics::Direct3D11::*;
    use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_R16G16B16A16_FLOAT;

    fn hdr_source(
        device: &ID3D11Device,
        width: u32,
        height: u32,
    ) -> anyhow::Result<(ID3D11Texture2D, D3D11_TEXTURE2D_DESC)> {
        let pixels: Vec<[u16; 4]> = (0..width * height)
            .map(|i| {
                [
                    ((i % 17) as f32) / 4.0,
                    ((i % 31) as f32) / 6.0,
                    ((i % 13) as f32) / 3.0,
                    0.5,
                ]
                .map(|v| half::f16::from_f32(v).to_bits())
            })
            .collect();
        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: DXGI_FORMAT_R16G16B16A16_FLOAT,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_SHADER_RESOURCE.0 as u32,
            ..Default::default()
        };
        let data = D3D11_SUBRESOURCE_DATA {
            pSysMem: pixels.as_ptr().cast(),
            SysMemPitch: width * 8,
            SysMemSlicePitch: 0,
        };
        let mut source = None;
        unsafe { device.CreateTexture2D(&desc, Some(&data), Some(&mut source)) }?;
        Ok((source.unwrap(), desc))
    }

    struct Readback {
        texture: ID3D11Texture2D,
        desc: D3D11_TEXTURE2D_DESC,
        frame: Frame,
        format: CapturePixelFormat,
    }

    impl Readback {
        fn new(
            device: &ID3D11Device,
            width: u32,
            height: u32,
            format: CapturePixelFormat,
        ) -> anyhow::Result<Self> {
            let desc = D3D11_TEXTURE2D_DESC {
                Width: width,
                Height: height,
                MipLevels: 1,
                ArraySize: 1,
                Format: DXGI_FORMAT_R8G8B8A8_UNORM,
                SampleDesc: DXGI_SAMPLE_DESC {
                    Count: 1,
                    Quality: 0,
                },
                Usage: D3D11_USAGE_STAGING,
                CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
                ..Default::default()
            };
            let mut texture = None;
            unsafe { device.CreateTexture2D(&desc, None, Some(&mut texture)) }?;
            Ok(Self {
                texture: texture.unwrap(),
                desc,
                frame: Frame::empty(),
                format,
            })
        }

        fn copy(
            &mut self,
            context: &ID3D11DeviceContext,
            source: &ID3D11Texture2D,
            x: u32,
            y: u32,
        ) -> anyhow::Result<()> {
            let bounds = D3D11_BOX {
                left: x,
                top: y,
                front: 0,
                right: x + self.desc.Width,
                bottom: y + self.desc.Height,
                back: 1,
            };
            unsafe {
                context.CopySubresourceRegion(&self.texture, 0, 0, 0, 0, source, 0, Some(&bounds));
            }
            surface::map_staging_to_frame_blocking(
                context,
                &self.texture,
                None,
                &self.desc,
                &mut self.frame,
                crate::convert::SurfaceConversionOptions {
                    output_pixel_format: self.format,
                    ..Default::default()
                },
                "HDR rotation test readback",
            )?;
            Ok(())
        }
    }

    #[test]
    fn hdr_fused_rotation_matches_cpu_for_dispatches_crops_and_cached_state() -> anyhow::Result<()>
    {
        use windows::Win32::Graphics::Dxgi::Common::{
            DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270,
        };
        let mut device = None;
        let mut context = None;
        unsafe {
            D3D11CreateDevice(
                None,
                D3D_DRIVER_TYPE_WARP,
                windows::Win32::Foundation::HMODULE::default(),
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                Some(&[D3D_FEATURE_LEVEL_11_0]),
                D3D11_SDK_VERSION,
                Some(&mut device),
                None,
                Some(&mut context),
            )?;
        }
        let device = device.unwrap();
        let context = context.unwrap();
        for (width, height) in [(3, 2), (33, 17), (17, 17)] {
            let (source, desc) = hdr_source(&device, width, height)?;
            for use_1d in [false, true] {
                for use_lut in [false, true] {
                    let mut mapper = GpuTonemapper::new(&device)?;
                    if !use_1d {
                        mapper.pass.cs_1d = None;
                    } else {
                        assert!(mapper.pass.cs_1d.is_some());
                    }
                    let params = HdrFrameContext {
                        tonemap_use_lut: use_lut,
                        ..Default::default()
                    };
                    for format in [CapturePixelFormat::Rgba8, CapturePixelFormat::Bgra8] {
                        mapper.set_rotation(DXGI_MODE_ROTATION_IDENTITY);
                        let baseline =
                            mapper.tonemap(&device, &context, &source, &desc, params, None)?;
                        let mut native = Readback::new(&device, width, height, format)?;
                        native.copy(&context, baseline, 0, 0)?;
                        for rotation in [
                            DXGI_MODE_ROTATION_ROTATE90,
                            DXGI_MODE_ROTATION_ROTATE270,
                            DXGI_MODE_ROTATION_ROTATE180,
                            DXGI_MODE_ROTATION_IDENTITY,
                        ] {
                            mapper.set_rotation(rotation);
                            let expected =
                                rotation::orient_frame(&native.frame, Frame::empty(), rotation)?;
                            let output = mapper
                                .tonemap(&device, &context, &source, &desc, params, None)?
                                .clone();
                            let mut actual = Readback::new(
                                &device,
                                expected.width(),
                                expected.height(),
                                format,
                            )?;
                            actual.copy(&context, &output, 0, 0)?;
                            assert_eq!(
                                actual.frame.as_bytes(),
                                expected.as_bytes(),
                                "{width}x{height} {rotation:?} 1D={use_1d} LUT={use_lut}"
                            );
                            let output_again =
                                mapper.tonemap(&device, &context, &source, &desc, params, None)?;
                            assert_eq!(
                                output.as_raw(),
                                output_again.as_raw(),
                                "stable output must reuse its texture"
                            );
                            let mut crop =
                                Readback::new(&device, 1, expected.height() - 1, format)?;
                            crop.copy(&context, &output, 1, 1)?;
                            for y in 1..expected.height() {
                                let offset = ((y * expected.width() + 1) * 4) as usize;
                                assert_eq!(
                                    &crop.frame.as_bytes()
                                        [((y - 1) * 4) as usize..(y * 4) as usize],
                                    &expected.as_bytes()[offset..offset + 4]
                                );
                            }
                        }
                    }
                }
            }
        }
        Ok(())
    }

    #[test]
    #[ignore = "Release hardware benchmark; run scripts/run-capture-rotation-perf.ps1 -Hdr"]
    fn hdr_rotation_performance_benchmark() -> anyhow::Result<()> {
        use std::time::Instant;
        use windows::Win32::Graphics::Dxgi::Common::DXGI_MODE_ROTATION_ROTATE90;
        anyhow::ensure!(
            !cfg!(debug_assertions),
            "HDR rotation benchmark requires Release"
        );
        let (device, context) = super::super::d3d11::create_d3d11_device_default(true)?;
        let dxgi: windows::Win32::Graphics::Dxgi::IDXGIDevice = device.cast()?;
        let adapter = unsafe { dxgi.GetAdapter()?.GetDesc()? };
        println!(
            "adapter={}",
            String::from_utf16_lossy(&adapter.Description).trim_end_matches('\0')
        );
        println!("source,crop,mode,median_ms,p95_ms");
        for (width, height) in [(1920, 1080), (3840, 2160)] {
            let (source, desc) = hdr_source(&device, width, height)?;
            for (crop_width, crop_height) in [(height, width), (320, 180)] {
                let desktop = CaptureBlitRegion {
                    src_x: (height - crop_width) / 2,
                    src_y: (width - crop_height) / 2,
                    width: crop_width,
                    height: crop_height,
                    dst_x: 0,
                    dst_y: 0,
                };
                let crop =
                    rotation::native_blit(desktop, height, width, DXGI_MODE_ROTATION_ROTATE90)?;
                let mut baseline = GpuTonemapper::new(&device)?;
                let mut fused = GpuTonemapper::new(&device)?;
                fused.set_rotation(DXGI_MODE_ROTATION_ROTATE90);
                let mut native =
                    Readback::new(&device, crop.width, crop.height, CapturePixelFormat::Bgra8)?;
                let mut upright =
                    Readback::new(&device, crop_width, crop_height, CapturePixelFormat::Bgra8)?;
                let mut cpu_output = Frame::empty();
                let mut samples = [Vec::new(), Vec::new()];
                for iteration in 0..60 {
                    for mode in [iteration % 2, 1 - iteration % 2] {
                        let started = Instant::now();
                        if mode == 0 {
                            let output = baseline.tonemap(
                                &device,
                                &context,
                                &source,
                                &desc,
                                HdrFrameContext::default(),
                                None,
                            )?;
                            native.copy(&context, output, crop.src_x, crop.src_y)?;
                            cpu_output = rotation::orient_frame(
                                &native.frame,
                                cpu_output,
                                DXGI_MODE_ROTATION_ROTATE90,
                            )?;
                            std::hint::black_box(cpu_output.as_bytes());
                        } else {
                            let output = fused.tonemap(
                                &device,
                                &context,
                                &source,
                                &desc,
                                HdrFrameContext::default(),
                                None,
                            )?;
                            upright.copy(&context, output, desktop.src_x, desktop.src_y)?;
                            std::hint::black_box(upright.frame.as_bytes());
                        }
                        if iteration >= 10 {
                            samples[mode].push(started.elapsed().as_secs_f64() * 1000.0);
                        }
                    }
                }
                assert_eq!(cpu_output.as_bytes(), upright.frame.as_bytes());
                for (mode, mut times) in samples.into_iter().enumerate() {
                    times.sort_by(f64::total_cmp);
                    println!(
                        "{width}x{height},{crop_width}x{crop_height},{},{:.4},{:.4}",
                        ["tone-map+CPU-rotation", "fused-HDR-rotation"][mode],
                        times[25],
                        times[47]
                    );
                }
            }
        }
        Ok(())
    }

    fn read_pixel(
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        texture: &ID3D11Texture2D,
    ) -> anyhow::Result<[u8; 4]> {
        let mut desc = D3D11_TEXTURE2D_DESC::default();
        unsafe { texture.GetDesc(&mut desc) };
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ.0 as u32;
        let mut staging = None;
        unsafe { device.CreateTexture2D(&desc, None, Some(&mut staging)) }?;
        let staging = staging.unwrap();
        let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
        unsafe {
            context.CopyResource(&staging, texture);
            context.Map(&staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))?;
            let pixel = std::ptr::read_unaligned(mapped.pData.cast::<[u8; 4]>());
            context.Unmap(&staging, 0);
            Ok(pixel)
        }
    }

    #[test]
    fn hdr_color_correction_shader_restores_sdr_and_updates_cached_constants() -> anyhow::Result<()>
    {
        let mut device = None;
        let mut context = None;
        unsafe {
            D3D11CreateDevice(
                None,
                D3D_DRIVER_TYPE_WARP,
                windows::Win32::Foundation::HMODULE::default(),
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                Some(&[D3D_FEATURE_LEVEL_11_0]),
                D3D11_SDK_VERSION,
                Some(&mut device),
                None,
                Some(&mut context),
            )?;
        }
        let device = device.unwrap();
        let context = context.unwrap();
        let original = [0.07f32, 0.35, 2.8, 0.5];
        let filtered = [0.93f32, 0.65, -1.8, 0.5].map(|v| half::f16::from_f32(v).to_bits());
        let desc = D3D11_TEXTURE2D_DESC {
            Width: 1,
            Height: 1,
            MipLevels: 1,
            ArraySize: 1,
            Format: DXGI_FORMAT_R16G16B16A16_FLOAT,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_SHADER_RESOURCE.0 as u32,
            ..Default::default()
        };
        let data = D3D11_SUBRESOURCE_DATA {
            pSysMem: filtered.as_ptr().cast(),
            SysMemPitch: 8,
            SysMemSlicePitch: 0,
        };
        let mut source = None;
        unsafe { device.CreateTexture2D(&desc, Some(&data), Some(&mut source)) }?;
        let source = source.unwrap();
        let transform = ScreenColorTransform {
            inverted: true,
            rows: [
                [-1., 0., 0., 255.],
                [0., -1., 0., 255.],
                [0., 0., -1., 255.],
            ],
        };
        let params = HdrFrameContext {
            sdr_white_nits: 280.,
            ..Default::default()
        };
        let bytes = original
            .into_iter()
            .flat_map(|v| half::f16::from_f32(v).to_bits().to_ne_bytes())
            .collect::<Vec<_>>();
        for use_1d in [true, false] {
            for hdr in [true, false] {
                let mut mapper = GpuTonemapper::new(&device)?;
                let mut converter = GpuF16Converter::new(&device)?;
                if !use_1d {
                    mapper.pass.cs_1d = None;
                    converter.pass.cs_1d = None;
                }
                let mut expected = [0u8; 4];
                crate::convert::convert_row_to_rgba_with_options(
                    crate::convert::SurfacePixelFormat::Rgba16Float,
                    &bytes,
                    &mut expected,
                    1,
                    crate::convert::SurfaceConversionOptions {
                        hdr_to_sdr: hdr.then_some(params),
                        ..Default::default()
                    },
                );
                let mut baseline = None;
                for correction in [None, Some(transform), None] {
                    let output = if hdr {
                        mapper.tonemap(&device, &context, &source, &desc, params, correction)?
                    } else {
                        converter.convert(&device, &context, &source, &desc, correction)?
                    };
                    let actual = read_pixel(&device, &context, output)?;
                    if correction.is_some() {
                        for (a, b) in actual.into_iter().zip(expected) {
                            assert!(
                                a.abs_diff(b) <= 1,
                                "GPU {actual:?} != SDR baseline {expected:?}"
                            );
                        }
                    } else if let Some(baseline) = baseline {
                        assert_eq!(
                            actual, baseline,
                            "disabling correction must update GPU constants"
                        );
                    } else {
                        baseline = Some(actual);
                    }
                }
            }
        }
        Ok(())
    }
}
