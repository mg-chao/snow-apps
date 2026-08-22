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
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_SAMPLE_DESC,
};
use windows::core::Interface;

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
#[derive(Clone, Copy)]
struct GpuParams {
    sdr_white_nits: f32,
    hdr_peak_nits: f32,
    sdr_identity_eps: f32,
    flags: u32,
    tex_width: u32,
    tex_height: u32,
    lut_size_minus_one: u32,
    _pad0: u32,
    lut_input_max: f32,
    lut_inv_step: f32,
    _pad1: f32,
    _pad2: f32,
}

const GPU_FLAG_USE_LUT: u32 = 1;

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
    /// Combined cache of tonemap params and dimensions written to the
    cached_cbuf_state: Option<(HdrFrameContext, u32, u32, u32)>,
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
            cached_cbuf_state: None,
            cached_lut_state: None,
            lut_tex: None,
            lut_srv: None,
            lut_disabled: false,
        })
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
    ) -> CaptureResult<&ID3D11Texture2D> {
        let params = params.sanitized();
        let width = source_desc.Width;
        let height = source_desc.Height;
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

        let needs_cbuf_update = self
            .cached_cbuf_state
            .is_none_or(|(p, w, h, f)| p != params || w != width || h != height || f != flags);
        if needs_cbuf_update {
            let gpu_params = GpuParams {
                sdr_white_nits: params.sdr_white_nits,
                hdr_peak_nits: params.hdr_peak_nits,
                sdr_identity_eps: 1e-3,
                flags,
                tex_width: width,
                tex_height: height,
                lut_size_minus_one: (HDR_LUMA_LUT_SIZE - 1) as u32,
                _pad0: 0,
                lut_input_max,
                lut_inv_step,
                _pad1: 0.0,
                _pad2: 0.0,
            };
            self.pass.update_cbuf(context, &gpu_params)?;
            self.cached_cbuf_state = Some((params, width, height, flags));
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
    cached_cbuf_state: Option<(u32, u32)>,
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
    ) -> CaptureResult<&ID3D11Texture2D> {
        let width = source_desc.Width;
        let height = source_desc.Height;
        self.pass.ensure_output(device, width, height)?;

        let needs_cbuf_update = self.cached_cbuf_state != Some((width, height));
        if needs_cbuf_update {
            let gpu_params = GpuParams {
                sdr_white_nits: 0.0,
                hdr_peak_nits: 0.0,
                sdr_identity_eps: 0.0,
                flags: 0,
                tex_width: width,
                tex_height: height,
                lut_size_minus_one: 0,
                _pad0: 0,
                lut_input_max: 0.0,
                lut_inv_step: 0.0,
                _pad1: 0.0,
                _pad2: 0.0,
            };
            self.pass.update_cbuf(context, &gpu_params)?;
            self.cached_cbuf_state = Some((width, height));
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
