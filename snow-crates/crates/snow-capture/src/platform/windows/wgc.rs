use std::ffi::c_void;
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use anyhow::Context;
use crossbeam_channel::{Receiver, Sender};
use snow_core::timestamp::TickFormat;
use windows::Foundation::TypedEventHandler;
use windows::Graphics::Capture::{
    Direct3D11CaptureFramePool, GraphicsCaptureDirtyRegionMode, GraphicsCaptureItem,
    GraphicsCaptureSession,
};
use windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
use windows::Graphics::DirectX::DirectXPixelFormat;
use windows::Graphics::SizeInt32;
use windows::Win32::Foundation::HWND;
use windows::Win32::Graphics::Direct3D11::{
    D3D11_TEXTURE2D_DESC, ID3D11Device, ID3D11DeviceContext, ID3D11Texture2D,
};
use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_R16G16B16A16_FLOAT;
use windows::Win32::Graphics::Dxgi::{
    DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DEVICE_REMOVED,
    DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DRIVER_INTERNAL_ERROR, IDXGIDevice,
};
use windows::Win32::Graphics::Gdi::HMONITOR;
use windows::Win32::System::WinRT::Direct3D11::{
    CreateDirect3D11DeviceFromDXGIDevice, IDirect3DDxgiInterfaceAccess,
};
use windows::Win32::System::WinRT::Graphics::Capture::IGraphicsCaptureItemInterop;
use windows::core::{IInspectable, Interface};

use crate::backend::{
    CaptureBackendKind, CaptureBlitRegion, CaptureMode, CaptureSampleMetadata, WgcUpdateMode,
};
use crate::convert::HdrFrameContext;
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{CapturePixelFormat, Frame};
use crate::monitor::MonitorId;
use crate::timing::stage_mark;
#[cfg(feature = "stage-timing")]
use crate::timing::{
    StageScope, attach_stage_timings, qpc_ticks_to_duration, stage_record, stage_recording,
};
use crate::window::WindowId;

use super::com::CoInitGuard;
use super::d3d11;
use super::gpu_tonemap::{GpuF16Converter, GpuTonemapper};
use super::monitor::{HdrMonitorMetadata, MonitorResolver, hdr_to_sdr_params};

mod readback;
mod transport;
mod update;

use readback::{DeliveredGeneration, ReadbackPipeline, ReadbackTarget};
use transport::{DrainPolicy, FramePacket, FrameTransport};
use update::{ApplyOutcome, CanonicalFrameMetadata, CanonicalSurface};

const WGC_FRAME_TIMEOUT: Duration = Duration::from_millis(250);
const WGC_SNAPSHOT_FRESH_WAIT: Duration = Duration::from_millis(2);
const WGC_CONTINUOUS_FRESH_WAIT: Duration = Duration::from_millis(1);
const WGC_WORKER_START_TIMEOUT: Duration = Duration::from_secs(10);
const WGC_SNAPSHOT_WORKER_START_TIMEOUT: Duration = Duration::from_millis(250);
const WGC_FRAME_POOL_BUFFERS: i32 = 3;
const WGC_ORDERED_QUEUE_CAPACITY: usize = 32;
const WGC_COMMAND_CAPACITY: usize = 8;
const WGC_ORDERED_FAULT_LIMIT: u8 = 3;

/// Record the age of the OS-delivered frame relative to now, derived from
/// the WGC system-relative timestamp (100 ns units since boot, the same
/// clock base as QPC on current Windows releases).
#[cfg(feature = "stage-timing")]
fn record_wgc_frame_age(system_relative_time_hns: i64) {
    if !stage_recording() || system_relative_time_hns <= 0 {
        return;
    }
    let Some(now_qpc) = crate::frame::query_qpc_now() else {
        return;
    };
    let Some(now_since_boot) = qpc_ticks_to_duration(now_qpc) else {
        return;
    };
    let frame_since_boot = Duration::from_nanos(system_relative_time_hns as u64 * 100);
    stage_record(
        "wgc.frame_age",
        now_since_boot.saturating_sub(frame_since_boot),
    );
}

fn is_device_lost_hresult(code: windows::core::HRESULT) -> bool {
    matches!(
        code,
        DXGI_ERROR_ACCESS_LOST
            | DXGI_ERROR_DEVICE_HUNG
            | DXGI_ERROR_DEVICE_REMOVED
            | DXGI_ERROR_DEVICE_RESET
            | DXGI_ERROR_DRIVER_INTERNAL_ERROR
    )
}

fn normalize_device_error(device: &ID3D11Device, error: CaptureError) -> CaptureError {
    if matches!(error, CaptureError::AccessLost) {
        return error;
    }
    match unsafe { device.GetDeviceRemovedReason() } {
        Ok(()) => error,
        Err(_) => CaptureError::AccessLost,
    }
}

pub(super) fn map_platform_error(error: windows::core::Error, context: &str) -> CaptureError {
    if is_device_lost_hresult(error.code()) {
        return CaptureError::AccessLost;
    }
    CaptureError::platform(anyhow::Error::from(error).context(context.to_owned()))
}

fn create_winrt_device(device: &ID3D11Device) -> CaptureResult<IDirect3DDevice> {
    let dxgi_device: IDXGIDevice = device
        .cast()
        .context("failed to cast ID3D11Device to IDXGIDevice")
        .map_err(CaptureError::platform)?;
    let inspectable = unsafe { CreateDirect3D11DeviceFromDXGIDevice(&dxgi_device) }
        .context("CreateDirect3D11DeviceFromDXGIDevice failed")
        .map_err(CaptureError::platform)?;
    inspectable
        .cast()
        .context("failed to cast IInspectable to IDirect3DDevice")
        .map_err(CaptureError::platform)
}

fn create_monitor_capture_item(monitor: HMONITOR) -> CaptureResult<GraphicsCaptureItem> {
    let interop = windows::core::factory::<GraphicsCaptureItem, IGraphicsCaptureItemInterop>()
        .context("failed to get IGraphicsCaptureItemInterop factory")
        .map_err(CaptureError::platform)?;
    unsafe { interop.CreateForMonitor(monitor) }
        .context("IGraphicsCaptureItemInterop::CreateForMonitor failed")
        .map_err(CaptureError::platform)
}

fn create_window_capture_item(window: HWND) -> CaptureResult<GraphicsCaptureItem> {
    let interop = windows::core::factory::<GraphicsCaptureItem, IGraphicsCaptureItemInterop>()
        .context("failed to get IGraphicsCaptureItemInterop factory")
        .map_err(CaptureError::platform)?;
    unsafe { interop.CreateForWindow(window) }
        .context("IGraphicsCaptureItemInterop::CreateForWindow failed")
        .map_err(CaptureError::platform)
}

pub(crate) fn validate_support() -> CaptureResult<()> {
    let _com = CoInitGuard::init_multithreaded().map_err(CaptureError::platform)?;
    super::com::ensure_process_mta_usage().map_err(CaptureError::platform)?;
    let supported = GraphicsCaptureSession::IsSupported()
        .context("GraphicsCaptureSession::IsSupported failed")
        .map_err(CaptureError::platform)?;
    if supported {
        Ok(())
    } else {
        Err(CaptureError::BackendUnavailable(
            "Windows Graphics Capture is not supported on this system".into(),
        ))
    }
}

#[derive(Clone, Copy)]
enum WorkerTarget {
    Monitor {
        adapter_luid: u64,
        monitor: usize,
        hdr_metadata: HdrMonitorMetadata,
    },
    Window {
        hwnd: usize,
    },
}

enum WorkerCommand {
    CaptureFull {
        frame: Frame,
        destination_has_history: bool,
        response: Sender<CaptureResult<Frame>>,
    },
    CaptureRegion {
        blit: CaptureBlitRegion,
        frame: Frame,
        destination_has_history: bool,
        response: Sender<(Frame, CaptureResult<CaptureSampleMetadata>)>,
    },
    SetCaptureMode {
        mode: CaptureMode,
        response: Sender<CaptureResult<()>>,
    },
    SetGpuHdrConversion {
        enabled: bool,
        response: Sender<CaptureResult<()>>,
    },
    SetHdrTonemapLut {
        enabled: bool,
        response: Sender<CaptureResult<()>>,
    },
    SetUpdateMode {
        mode: WgcUpdateMode,
        response: Sender<CaptureResult<()>>,
    },
    SetOutputPixelFormat {
        format: CapturePixelFormat,
        response: Sender<CaptureResult<()>>,
    },
    #[cfg(feature = "stage-timing")]
    SetRecordStageTimings {
        enabled: bool,
        response: Sender<CaptureResult<()>>,
    },
    Shutdown,
}

struct WindowsGraphicsCaptureCapturer {
    commands: Sender<WorkerCommand>,
    join: Option<JoinHandle<()>>,
}

impl WindowsGraphicsCaptureCapturer {
    fn spawn(target: WorkerTarget, startup_timeout: Duration) -> CaptureResult<Self> {
        let (command_tx, command_rx) = crossbeam_channel::bounded(WGC_COMMAND_CAPACITY);
        let (startup_tx, startup_rx) = crossbeam_channel::bounded(1);
        let join = thread::Builder::new()
            .name("snow-wgc".into())
            .spawn(move || match WgcWorker::new(target) {
                Ok(mut worker) => {
                    let _ = startup_tx.send(Ok(()));
                    worker.run(command_rx);
                }
                Err(error) => {
                    let _ = startup_tx.send(Err(error));
                }
            })
            .context("failed to spawn WGC worker thread")
            .map_err(CaptureError::platform)?;

        match startup_rx.recv_timeout(startup_timeout) {
            Ok(Ok(())) => Ok(Self {
                commands: command_tx,
                join: Some(join),
            }),
            Ok(Err(error)) => {
                let _ = join.join();
                Err(error)
            }
            Err(_) => {
                drop(command_tx);
                // A late startup may already have created a frame pool and
                // GraphicsCaptureSession. Reap the worker synchronously so a
                // timeout can never return while that capture access is live.
                let _ = join.join();
                Err(CaptureError::Timeout)
            }
        }
    }

    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        let (response_tx, response_rx) = crossbeam_channel::bounded(1);
        self.commands
            .send(WorkerCommand::CaptureFull {
                frame: reuse.unwrap_or_else(Frame::empty),
                destination_has_history,
                response: response_tx,
            })
            .map_err(|_| CaptureError::WorkerDead)?;
        response_rx.recv().map_err(|_| CaptureError::WorkerDead)?
    }

    fn capture_region_into(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<CaptureSampleMetadata> {
        let frame = std::mem::replace(destination, Frame::empty());
        let (response_tx, response_rx) = crossbeam_channel::bounded(1);
        let command = WorkerCommand::CaptureRegion {
            blit,
            frame,
            destination_has_history,
            response: response_tx,
        };
        if let Err(error) = self.commands.send(command) {
            if let WorkerCommand::CaptureRegion { frame, .. } = error.0 {
                *destination = frame;
            }
            return Err(CaptureError::WorkerDead);
        }
        let (frame, result) = response_rx.recv().map_err(|_| CaptureError::WorkerDead)?;
        *destination = frame;
        result
    }

    fn set_capture_mode(&mut self, mode: CaptureMode) -> CaptureResult<()> {
        self.configure(|response| WorkerCommand::SetCaptureMode { mode, response })
    }

    fn set_gpu_hdr_conversion(&mut self, enabled: bool) -> CaptureResult<()> {
        self.configure(|response| WorkerCommand::SetGpuHdrConversion { enabled, response })
    }

    fn set_hdr_tonemap_lut(&mut self, enabled: bool) -> CaptureResult<()> {
        self.configure(|response| WorkerCommand::SetHdrTonemapLut { enabled, response })
    }

    fn set_wgc_update_mode(&mut self, mode: WgcUpdateMode) -> CaptureResult<()> {
        self.configure(|response| WorkerCommand::SetUpdateMode { mode, response })
    }

    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.configure(|response| WorkerCommand::SetOutputPixelFormat { format, response })
    }

    #[cfg(feature = "stage-timing")]
    fn set_record_stage_timings(&mut self, enabled: bool) -> CaptureResult<()> {
        self.configure(|response| WorkerCommand::SetRecordStageTimings { enabled, response })
    }

    fn configure(
        &self,
        command: impl FnOnce(Sender<CaptureResult<()>>) -> WorkerCommand,
    ) -> CaptureResult<()> {
        let (response_tx, response_rx) = crossbeam_channel::bounded(1);
        self.commands
            .send(command(response_tx))
            .map_err(|_| CaptureError::WorkerDead)?;
        response_rx.recv().map_err(|_| CaptureError::WorkerDead)?
    }
}

struct PreparedWgcCapturer {
    target: WorkerTarget,
    active: Option<WindowsGraphicsCaptureCapturer>,
    capture_mode: CaptureMode,
    gpu_hdr_conversion_enabled: bool,
    hdr_tonemap_lut_enabled: bool,
    update_mode: WgcUpdateMode,
    output_pixel_format: CapturePixelFormat,
    #[cfg(feature = "stage-timing")]
    record_stage_timings: bool,
}

impl PreparedWgcCapturer {
    fn new(target: WorkerTarget) -> Self {
        Self {
            target,
            active: None,
            capture_mode: CaptureMode::Snapshot,
            gpu_hdr_conversion_enabled: true,
            hdr_tonemap_lut_enabled: true,
            update_mode: WgcUpdateMode::Auto,
            output_pixel_format: CapturePixelFormat::Rgba8,
            #[cfg(feature = "stage-timing")]
            record_stage_timings: false,
        }
    }

    fn ensure_active(&mut self) -> CaptureResult<&mut WindowsGraphicsCaptureCapturer> {
        if self.active.is_none() {
            let startup_timeout = if self.capture_mode == CaptureMode::Snapshot {
                WGC_SNAPSHOT_WORKER_START_TIMEOUT
            } else {
                WGC_WORKER_START_TIMEOUT
            };
            let mut active = WindowsGraphicsCaptureCapturer::spawn(self.target, startup_timeout)?;
            active.set_wgc_update_mode(self.update_mode)?;
            active.set_gpu_hdr_conversion(self.gpu_hdr_conversion_enabled)?;
            active.set_hdr_tonemap_lut(self.hdr_tonemap_lut_enabled)?;
            active.set_capture_mode(self.capture_mode)?;
            active.set_output_pixel_format(self.output_pixel_format)?;
            #[cfg(feature = "stage-timing")]
            active.set_record_stage_timings(self.record_stage_timings)?;
            self.active = Some(active);
        }
        self.active.as_mut().ok_or(CaptureError::WorkerDead)
    }

    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        self.ensure_active()?
            .capture_with_history_hint(reuse, destination_has_history)
    }

    fn capture_region_into(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<CaptureSampleMetadata> {
        self.ensure_active()?
            .capture_region_into(blit, destination, destination_has_history)
    }

    fn set_capture_mode(&mut self, mode: CaptureMode) -> CaptureResult<()> {
        self.capture_mode = mode;
        if let Some(active) = self.active.as_mut() {
            active.set_capture_mode(mode)?;
        }
        Ok(())
    }

    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.output_pixel_format = format;
        if let Some(active) = self.active.as_mut() {
            active.set_output_pixel_format(format)?;
        }
        Ok(())
    }

    fn set_gpu_hdr_conversion(&mut self, enabled: bool) -> CaptureResult<()> {
        self.gpu_hdr_conversion_enabled = enabled;
        if let Some(active) = self.active.as_mut() {
            active.set_gpu_hdr_conversion(enabled)?;
        }
        Ok(())
    }

    fn set_hdr_tonemap_lut(&mut self, enabled: bool) -> CaptureResult<()> {
        self.hdr_tonemap_lut_enabled = enabled;
        if let Some(active) = self.active.as_mut() {
            active.set_hdr_tonemap_lut(enabled)?;
        }
        Ok(())
    }

    fn set_wgc_update_mode(&mut self, mode: WgcUpdateMode) -> CaptureResult<()> {
        self.update_mode = mode;
        if let Some(active) = self.active.as_mut() {
            active.set_wgc_update_mode(mode)?;
        }
        Ok(())
    }

    #[cfg(feature = "stage-timing")]
    fn set_record_stage_timings(&mut self, enabled: bool) -> CaptureResult<()> {
        self.record_stage_timings = enabled;
        if let Some(active) = self.active.as_mut() {
            active.set_record_stage_timings(enabled)?;
        }
        Ok(())
    }

    fn release_capture_access(&mut self) {
        self.active = None;
    }

    fn capture_access_active(&self) -> bool {
        self.active.is_some()
    }
}

impl Drop for WindowsGraphicsCaptureCapturer {
    fn drop(&mut self) {
        let _ = self.commands.send(WorkerCommand::Shutdown);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SourcePhase {
    Complete,
    BaselineForOrdered,
    Ordered,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum OrderedFault {
    UnsupportedContract,
    Continuity,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct OrderedHealth {
    faults: u8,
    disabled: bool,
}

impl OrderedHealth {
    fn reset(&mut self) {
        *self = Self::default();
    }

    fn enabled_for(self, mode: WgcUpdateMode) -> bool {
        mode == WgcUpdateMode::OrderedIncremental && !self.disabled
    }

    fn record_fault(&mut self, fault: OrderedFault) {
        match fault {
            OrderedFault::UnsupportedContract => self.disabled = true,
            OrderedFault::Continuity => {
                self.faults = self.faults.saturating_add(1);
                if self.faults >= WGC_ORDERED_FAULT_LIMIT {
                    self.disabled = true;
                }
            }
        }
    }
}

impl SourcePhase {
    fn drain_policy(self) -> DrainPolicy {
        match self {
            Self::Ordered => DrainPolicy::Ordered,
            Self::Complete | Self::BaselineForOrdered => DrainPolicy::CompleteLatest,
        }
    }
}

struct WgcWorker {
    device: ID3D11Device,
    context: ID3D11DeviceContext,
    winrt_device: IDirect3DDevice,
    item: GraphicsCaptureItem,
    frame_pool: Direct3D11CaptureFramePool,
    session: GraphicsCaptureSession,
    frame_arrived_token: i64,
    closed_token: i64,
    transport: FrameTransport,
    frame_notifications: Receiver<()>,
    pool_size: SizeInt32,
    pixel_format: DirectXPixelFormat,
    source_phase: SourcePhase,
    update_mode: WgcUpdateMode,
    ordered_health: OrderedHealth,
    dirty_regions_supported: bool,
    canonical: CanonicalSurface,
    readback: ReadbackPipeline,
    capture_mode: CaptureMode,
    output_pixel_format: CapturePixelFormat,
    hdr_to_sdr: Option<HdrFrameContext>,
    gpu_tonemapper: Option<GpuTonemapper>,
    gpu_f16_converter: Option<GpuF16Converter>,
    gpu_hdr_conversion_enabled: bool,
    hdr_tonemap_lut_enabled: bool,
    last_delivery: Option<DeliveredGeneration>,
    pending_complete_snapshot: Option<FramePacket>,
    #[cfg(feature = "stage-timing")]
    record_stage_timings: bool,
    closed: bool,
    terminal_error: Option<CaptureError>,
    _com: CoInitGuard,
}

impl WgcWorker {
    fn new(target: WorkerTarget) -> CaptureResult<Self> {
        let com = CoInitGuard::init_multithreaded().map_err(CaptureError::platform)?;
        let (device, context, item, hdr_to_sdr) = match target {
            WorkerTarget::Monitor {
                adapter_luid,
                monitor,
                hdr_metadata,
            } => {
                let adapter = super::monitor::resolve_adapter_by_luid(adapter_luid)?;
                let (device, context) = d3d11::create_d3d11_device_for_adapter(&adapter, false)
                    .map_err(CaptureError::platform)?;
                let monitor = HMONITOR(monitor as *mut c_void);
                let item = create_monitor_capture_item(monitor)?;
                (device, context, item, hdr_to_sdr_params(hdr_metadata))
            }
            WorkerTarget::Window { hwnd } => {
                let (device, context) =
                    d3d11::create_d3d11_device_default(false).map_err(CaptureError::platform)?;
                let hwnd = HWND(hwnd as *mut c_void);
                let item = create_window_capture_item(hwnd)?;
                (device, context, item, None)
            }
        };
        let winrt_device = create_winrt_device(&device)?;
        let pool_size = item
            .Size()
            .context("GraphicsCaptureItem::Size failed")
            .map_err(CaptureError::platform)?;
        if pool_size.Width <= 0 || pool_size.Height <= 0 {
            return Err(CaptureError::InvalidTarget(
                "WGC capture item has empty dimensions".into(),
            ));
        }

        let pixel_format = if hdr_to_sdr.is_some() {
            DirectXPixelFormat::R16G16B16A16Float
        } else {
            DirectXPixelFormat::B8G8R8A8UIntNormalized
        };
        let frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            &winrt_device,
            pixel_format,
            WGC_FRAME_POOL_BUFFERS,
            pool_size,
        )
        .context("Direct3D11CaptureFramePool::CreateFreeThreaded failed")
        .map_err(CaptureError::platform)?;
        let session = frame_pool
            .CreateCaptureSession(&item)
            .context("Direct3D11CaptureFramePool::CreateCaptureSession failed")
            .map_err(CaptureError::platform)?;
        let _ = session.SetIsCursorCaptureEnabled(false);
        let _ = session.SetIsBorderRequired(false);
        let dirty_regions_supported =
            match session.SetDirtyRegionMode(GraphicsCaptureDirtyRegionMode::ReportOnly) {
                Ok(()) => true,
                Err(error) if is_device_lost_hresult(error.code()) => {
                    return Err(CaptureError::AccessLost);
                }
                Err(_) => false,
            };

        let (notification_tx, notification_rx) = crossbeam_channel::bounded(1);
        let transport = FrameTransport::new(WGC_ORDERED_QUEUE_CAPACITY, notification_tx);
        let transport_for_frames = transport.clone();
        let frame_arrived_token = frame_pool
            .FrameArrived(
                &TypedEventHandler::<Direct3D11CaptureFramePool, IInspectable>::new(
                    move |sender, _| {
                        if let Some(pool) = sender.as_ref() {
                            transport_for_frames.drain_frame_pool(pool);
                        }
                        Ok(())
                    },
                ),
            )
            .context("Direct3D11CaptureFramePool::FrameArrived registration failed")
            .map_err(CaptureError::platform)?;

        let transport_for_closed = transport.clone();
        let closed_token = item
            .Closed(
                &TypedEventHandler::<GraphicsCaptureItem, IInspectable>::new(move |_, _| {
                    transport_for_closed.mark_closed();
                    Ok(())
                }),
            )
            .context("GraphicsCaptureItem::Closed registration failed")
            .map_err(CaptureError::platform)?;

        session
            .StartCapture()
            .context("GraphicsCaptureSession::StartCapture failed")
            .map_err(CaptureError::platform)?;

        Ok(Self {
            device,
            context,
            winrt_device,
            item,
            frame_pool,
            session,
            frame_arrived_token,
            closed_token,
            transport,
            frame_notifications: notification_rx,
            pool_size,
            pixel_format,
            source_phase: SourcePhase::Complete,
            update_mode: WgcUpdateMode::Auto,
            ordered_health: OrderedHealth::default(),
            dirty_regions_supported,
            canonical: CanonicalSurface::new(),
            readback: ReadbackPipeline::new(),
            capture_mode: CaptureMode::Snapshot,
            output_pixel_format: CapturePixelFormat::Rgba8,
            hdr_to_sdr,
            gpu_tonemapper: None,
            gpu_f16_converter: None,
            gpu_hdr_conversion_enabled: true,
            hdr_tonemap_lut_enabled: true,
            last_delivery: None,
            pending_complete_snapshot: None,
            #[cfg(feature = "stage-timing")]
            record_stage_timings: false,
            closed: false,
            terminal_error: None,
            _com: com,
        })
    }

    fn run(&mut self, commands: Receiver<WorkerCommand>) {
        loop {
            crossbeam_channel::select! {
                recv(self.frame_notifications) -> _ => {
                    let result = if self.capture_mode == CaptureMode::Snapshot
                        && self.source_phase == SourcePhase::Complete
                    {
                        self.coalesce_complete_snapshot()
                    } else {
                        self.pump_frames()
                    };
                    if let Err(error) = result {
                        self.terminal_error =
                            Some(normalize_device_error(&self.device, error));
                    }
                }
                recv(commands) -> command => {
                    let Ok(command) = command else {
                        break;
                    };
                    if !self.handle_command(command) {
                        break;
                    }
                }
            }
        }
    }

    fn handle_command(&mut self, command: WorkerCommand) -> bool {
        match command {
            WorkerCommand::CaptureFull {
                frame,
                destination_has_history,
                response,
            } => {
                let result = self
                    .terminal_error
                    .clone()
                    .map_or_else(|| self.capture_full(frame, destination_has_history), Err);
                let _ = response
                    .send(result.map_err(|error| normalize_device_error(&self.device, error)));
            }
            WorkerCommand::CaptureRegion {
                blit,
                mut frame,
                destination_has_history,
                response,
            } => {
                let result = self.terminal_error.clone().map_or_else(
                    || self.capture_region(blit, &mut frame, destination_has_history),
                    Err,
                );
                let result = result.map_err(|error| normalize_device_error(&self.device, error));
                let _ = response.send((frame, result));
            }
            WorkerCommand::SetCaptureMode { mode, response } => {
                self.capture_mode = mode;
                let result = if mode == CaptureMode::Continuous {
                    self.pump_frames()
                } else {
                    Ok(())
                };
                let _ = response
                    .send(result.map_err(|error| normalize_device_error(&self.device, error)));
            }
            WorkerCommand::SetGpuHdrConversion { enabled, response } => {
                if self.gpu_hdr_conversion_enabled != enabled {
                    self.gpu_hdr_conversion_enabled = enabled;
                    self.invalidate_delivery_pipeline();
                }
                let _ = response.send(Ok(()));
            }
            WorkerCommand::SetHdrTonemapLut { enabled, response } => {
                if self.hdr_tonemap_lut_enabled != enabled {
                    self.hdr_tonemap_lut_enabled = enabled;
                    if let Some(params) = self.hdr_to_sdr.as_mut() {
                        params.tonemap_use_lut = enabled;
                    }
                    self.invalidate_delivery_pipeline();
                }
                let _ = response.send(Ok(()));
            }
            WorkerCommand::SetUpdateMode { mode, response } => {
                let result = self.configure_update_mode(mode);
                let _ = response
                    .send(result.map_err(|error| normalize_device_error(&self.device, error)));
            }
            WorkerCommand::SetOutputPixelFormat { format, response } => {
                self.output_pixel_format = format;
                self.readback.set_output_pixel_format(format);
                let _ = response.send(Ok(()));
            }
            #[cfg(feature = "stage-timing")]
            WorkerCommand::SetRecordStageTimings { enabled, response } => {
                self.record_stage_timings = enabled;
                let _ = response.send(Ok(()));
            }
            WorkerCommand::Shutdown => return false,
        }
        true
    }

    fn configure_update_mode(&mut self, mode: WgcUpdateMode) -> CaptureResult<()> {
        self.update_mode = mode;
        self.ordered_health.reset();
        self.resynchronize()
    }

    fn ordered_requested(&self) -> bool {
        self.dirty_regions_supported && self.ordered_health.enabled_for(self.update_mode)
    }

    fn resynchronize(&mut self) -> CaptureResult<()> {
        self.transport.pause_and_clear()?;
        let contract_result = self.set_complete_contract();
        self.source_phase = if self.ordered_requested() {
            SourcePhase::BaselineForOrdered
        } else {
            SourcePhase::Complete
        };
        self.pending_complete_snapshot = None;
        self.canonical.invalidate();
        self.invalidate_delivery_pipeline();
        let barrier_result = self.transport.discard_and_resume(&self.frame_pool);
        barrier_result?;
        contract_result
    }

    fn set_complete_contract(&self) -> CaptureResult<()> {
        if !self.dirty_regions_supported {
            return Ok(());
        }
        self.session
            .SetDirtyRegionMode(GraphicsCaptureDirtyRegionMode::ReportOnly)
            .map_err(|error| {
                map_platform_error(
                    error,
                    "GraphicsCaptureSession::SetDirtyRegionMode(ReportOnly) failed",
                )
            })
    }

    fn handle_ordered_fault(&mut self, fault: OrderedFault) -> CaptureResult<()> {
        self.ordered_health.record_fault(fault);
        self.resynchronize()
    }

    fn invalidate_delivery_pipeline(&mut self) {
        self.readback.invalidate_submissions();
        self.last_delivery = None;
    }

    fn pump_frames(&mut self) -> CaptureResult<()> {
        let transport::FrameBatch {
            mut frames,
            overflowed,
            discarded,
            closed,
        } = self.transport.drain(self.source_phase.drain_policy())?;
        if closed {
            self.closed = true;
        }
        if self.source_phase == SourcePhase::Ordered && (overflowed || discarded != 0) {
            drop(frames);
            self.handle_ordered_fault(OrderedFault::Continuity)?;
            return Ok(());
        }

        if self.source_phase == SourcePhase::Complete {
            if frames.is_empty() {
                if let Some(packet) = self.pending_complete_snapshot.take() {
                    frames.push(packet);
                }
            } else {
                self.pending_complete_snapshot = None;
            }
        }

        let mut packets = frames.into_iter();
        let mut action = None;
        for packet in packets.by_ref() {
            match self.process_frame(packet)? {
                FrameProcessing::Updated | FrameProcessing::Ignored => {}
                FrameProcessing::Resize(size) => {
                    action = Some(PumpAction::Resize(size));
                    break;
                }
                FrameProcessing::Resynchronize => {
                    action = Some(PumpAction::Resynchronize);
                    break;
                }
                FrameProcessing::OrderedFault(fault) => {
                    action = Some(PumpAction::OrderedFault(fault));
                    break;
                }
            }
        }
        drop(packets);

        match action {
            Some(PumpAction::Resize(size)) => self.recreate_frame_pool(size)?,
            Some(PumpAction::Resynchronize) => self.resynchronize()?,
            Some(PumpAction::OrderedFault(fault)) => self.handle_ordered_fault(fault)?,
            None => {}
        }
        Ok(())
    }

    fn coalesce_complete_snapshot(&mut self) -> CaptureResult<()> {
        let batch = self.transport.drain(DrainPolicy::CompleteLatest)?;
        if batch.closed {
            self.closed = true;
        }
        if let Some(packet) = batch.frames.into_iter().next() {
            self.pending_complete_snapshot = Some(packet);
        }
        Ok(())
    }

    fn process_frame(&mut self, packet: FramePacket) -> CaptureResult<FrameProcessing> {
        let content_size = packet.frame.ContentSize().map_err(|error| {
            map_platform_error(error, "Direct3D11CaptureFrame::ContentSize failed")
        })?;
        if content_size.Width <= 0 || content_size.Height <= 0 {
            return Ok(FrameProcessing::Ignored);
        }
        if content_size != self.pool_size {
            return Ok(FrameProcessing::Resize(content_size));
        }

        let surface = packet
            .frame
            .Surface()
            .map_err(|error| map_platform_error(error, "Direct3D11CaptureFrame::Surface failed"))?;
        let access: IDirect3DDxgiInterfaceAccess = surface
            .cast()
            .context("failed to cast WGC surface to IDirect3DDxgiInterfaceAccess")
            .map_err(CaptureError::platform)?;
        let texture: ID3D11Texture2D = unsafe { access.GetInterface() }.map_err(|error| {
            map_platform_error(error, "IDirect3DDxgiInterfaceAccess::GetInterface failed")
        })?;
        let mut source_desc = D3D11_TEXTURE2D_DESC::default();
        unsafe { texture.GetDesc(&mut source_desc) };

        let reported_mode = match packet.frame.DirtyRegionMode() {
            Ok(mode) => mode,
            Err(error) if is_device_lost_hresult(error.code()) => {
                return Err(CaptureError::AccessLost);
            }
            Err(_) if self.source_phase == SourcePhase::BaselineForOrdered => {
                self.ordered_health
                    .record_fault(OrderedFault::UnsupportedContract);
                self.source_phase = SourcePhase::Complete;
                GraphicsCaptureDirtyRegionMode::ReportOnly
            }
            Err(_) if self.source_phase != SourcePhase::Ordered => {
                GraphicsCaptureDirtyRegionMode::ReportOnly
            }
            Err(_) => {
                return Ok(FrameProcessing::OrderedFault(
                    OrderedFault::UnsupportedContract,
                ));
            }
        };

        if reported_mode != GraphicsCaptureDirtyRegionMode::ReportOnly
            && reported_mode != GraphicsCaptureDirtyRegionMode::ReportAndRender
        {
            return Ok(if self.source_phase == SourcePhase::Complete {
                FrameProcessing::Resynchronize
            } else {
                FrameProcessing::OrderedFault(OrderedFault::UnsupportedContract)
            });
        }

        if self.source_phase != SourcePhase::Ordered
            && reported_mode != GraphicsCaptureDirtyRegionMode::ReportOnly
        {
            return Ok(if self.source_phase == SourcePhase::BaselineForOrdered {
                FrameProcessing::OrderedFault(OrderedFault::Continuity)
            } else {
                FrameProcessing::Resynchronize
            });
        }

        let transition_to_ordered = self.source_phase == SourcePhase::BaselineForOrdered;
        let mut entered_ordered = false;
        let ordered_next = if transition_to_ordered {
            self.transport.pause()?;
            match self
                .session
                .SetDirtyRegionMode(GraphicsCaptureDirtyRegionMode::ReportAndRender)
            {
                Ok(()) => {
                    self.source_phase = SourcePhase::Ordered;
                    entered_ordered = true;
                    true
                }
                Err(error) if is_device_lost_hresult(error.code()) => {
                    let barrier_result = self.transport.discard_and_resume(&self.frame_pool);
                    barrier_result?;
                    return Err(CaptureError::AccessLost);
                }
                Err(_) => {
                    self.ordered_health
                        .record_fault(OrderedFault::UnsupportedContract);
                    self.source_phase = SourcePhase::Complete;
                    false
                }
            }
        } else {
            self.source_phase == SourcePhase::Ordered
        };

        if transition_to_ordered {
            if entered_ordered {
                self.transport.resume_and_drain(&self.frame_pool)?;
            } else {
                self.transport.discard_and_resume(&self.frame_pool)?;
            }
        }

        let outcome = self.canonical.apply(
            &self.device,
            &self.context,
            &packet.frame,
            &texture,
            source_desc,
            reported_mode,
            packet.system_relative_time_hns,
            packet.received_at,
            ordered_next,
        )?;
        match outcome {
            ApplyOutcome::Updated(metadata) => {
                self.prefetch_current(&metadata)?;
                Ok(FrameProcessing::Updated)
            }
            ApplyOutcome::Duplicate => Ok(FrameProcessing::Ignored),
            ApplyOutcome::Resynchronize => Ok(if self.source_phase == SourcePhase::Ordered {
                FrameProcessing::OrderedFault(OrderedFault::Continuity)
            } else {
                FrameProcessing::Resynchronize
            }),
        }
    }

    fn recreate_frame_pool(&mut self, content_size: SizeInt32) -> CaptureResult<()> {
        self.transport.pause_and_clear()?;
        self.pending_complete_snapshot = None;
        let recreate_result = self.set_complete_contract().and_then(|()| {
            self.frame_pool
                .Recreate(
                    &self.winrt_device,
                    self.pixel_format,
                    WGC_FRAME_POOL_BUFFERS,
                    content_size,
                )
                .map_err(|error| {
                    map_platform_error(error, "Direct3D11CaptureFramePool::Recreate failed")
                })
        });
        let barrier_result = self.transport.discard_and_resume(&self.frame_pool);
        barrier_result?;
        recreate_result?;
        self.pool_size = content_size;
        self.canonical.invalidate();
        self.invalidate_delivery_pipeline();
        self.source_phase = if self.ordered_requested() {
            SourcePhase::BaselineForOrdered
        } else {
            SourcePhase::Complete
        };
        Ok(())
    }

    fn prefetch_current(&mut self, metadata: &CanonicalFrameMetadata) -> CaptureResult<()> {
        if self.readback.target().is_none() {
            return Ok(());
        }
        let (source, desc, hdr_to_sdr) = self.effective_canonical_source()?;
        let _ = self.readback.prefetch(
            &self.device,
            &self.context,
            &source,
            desc,
            hdr_to_sdr,
            metadata,
        )?;
        Ok(())
    }

    fn effective_canonical_source(
        &mut self,
    ) -> CaptureResult<(
        ID3D11Texture2D,
        D3D11_TEXTURE2D_DESC,
        Option<HdrFrameContext>,
    )> {
        let source = self
            .canonical
            .texture()
            .cloned()
            .ok_or(CaptureError::Timeout)?;
        let source_desc = self.canonical.desc().ok_or(CaptureError::Timeout)?;
        if source_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT {
            return Ok((source, source_desc, self.hdr_to_sdr));
        }

        if self.gpu_hdr_conversion_enabled {
            if let Some(params) = self.hdr_to_sdr {
                if self.gpu_tonemapper.is_none() {
                    self.gpu_tonemapper = Some(GpuTonemapper::new(&self.device)?);
                }
                let tonemapper = self.gpu_tonemapper.as_mut().ok_or_else(|| {
                    CaptureError::platform(anyhow::anyhow!("failed to initialize WGC tonemapper"))
                })?;
                let output = tonemapper
                    .tonemap(
                        &self.device,
                        &self.context,
                        &source,
                        &source_desc,
                        params.sanitized(),
                    )?
                    .clone();
                return Ok((output, tonemapper.output_desc(), None));
            }

            if self.gpu_f16_converter.is_none() {
                self.gpu_f16_converter = Some(GpuF16Converter::new(&self.device)?);
            }
            let converter = self.gpu_f16_converter.as_mut().ok_or_else(|| {
                CaptureError::platform(anyhow::anyhow!("failed to initialize WGC F16 converter"))
            })?;
            let output = converter
                .convert(&self.device, &self.context, &source, &source_desc)?
                .clone();
            return Ok((output, converter.output_desc(), None));
        }

        Ok((source, source_desc, self.hdr_to_sdr))
    }

    fn ensure_current_submitted(
        &mut self,
        canonical: &CanonicalFrameMetadata,
        target: ReadbackTarget,
    ) -> CaptureResult<()> {
        if self
            .readback
            .contains(canonical.epoch, canonical.generation, target)
        {
            return Ok(());
        }
        let (source, desc, hdr_to_sdr) = self.effective_canonical_source()?;
        self.readback.ensure_submitted(
            &self.device,
            &self.context,
            &source,
            desc,
            hdr_to_sdr,
            canonical,
            target,
        )
    }

    fn acquire_current(
        &mut self,
        target: ReadbackTarget,
        destination_has_history: bool,
    ) -> CaptureResult<()> {
        self.readback.set_target(target);
        self.pump_frames()?;
        if self.closed {
            return Err(CaptureError::MonitorLost);
        }

        let initial_generation = self.canonical.generation();
        let already_delivered = destination_has_history
            && self.last_delivery.is_some_and(|delivered| {
                delivered.target == target
                    && delivered.epoch
                        == self.canonical.latest().map_or(0, |metadata| metadata.epoch)
                    && delivered.generation == initial_generation
            });
        let wait_for = if !self.canonical.has_baseline() {
            WGC_FRAME_TIMEOUT
        } else if already_delivered {
            match self.capture_mode {
                CaptureMode::Snapshot => WGC_SNAPSHOT_FRESH_WAIT,
                CaptureMode::Continuous => WGC_CONTINUOUS_FRESH_WAIT,
            }
        } else {
            Duration::ZERO
        };

        if !wait_for.is_zero() {
            let deadline = Instant::now() + wait_for;
            loop {
                let current_generation = self.canonical.generation();
                if self.canonical.has_baseline()
                    && (!already_delivered || current_generation != initial_generation)
                {
                    break;
                }
                let now = Instant::now();
                if now >= deadline {
                    break;
                }
                match self
                    .frame_notifications
                    .recv_timeout(deadline.duration_since(now))
                {
                    Ok(()) => self.pump_frames()?,
                    Err(crossbeam_channel::RecvTimeoutError::Timeout) => break,
                    Err(crossbeam_channel::RecvTimeoutError::Disconnected) => {
                        return Err(CaptureError::WorkerDead);
                    }
                }
                if self.closed {
                    return Err(CaptureError::MonitorLost);
                }
            }
        }

        if self.canonical.has_baseline() {
            Ok(())
        } else {
            Err(CaptureError::Timeout)
        }
    }

    fn capture_full(
        &mut self,
        mut frame: Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        #[cfg(feature = "stage-timing")]
        let stages = StageScope::enter(self.record_stage_timings);
        frame.reset_metadata();
        let target = ReadbackTarget::Full;
        self.acquire_current(target, destination_has_history)?;
        stage_mark("wgc.transport_wait");
        let canonical = self
            .canonical
            .latest()
            .cloned()
            .ok_or(CaptureError::Timeout)?;
        #[cfg(feature = "stage-timing")]
        record_wgc_frame_age(canonical.system_relative_time_hns);

        if self.same_delivered_generation(target, &canonical, destination_has_history) {
            frame.metadata.set_timing_with_format(
                Some(Instant::now()),
                nonzero_timestamp(canonical.system_relative_time_hns),
                TickFormat::Hns100,
            );
            frame.metadata.is_duplicate = true;
            frame.metadata.dirty_rects.clear();
            #[cfg(feature = "stage-timing")]
            attach_stage_timings(&mut frame, stages);
            return Ok(frame);
        }

        self.ensure_current_submitted(&canonical, target)?;
        stage_mark("wgc.submit");
        let delivery = self.readback.read_into(
            &self.context,
            canonical.epoch,
            canonical.generation,
            target,
            &mut frame,
            self.last_delivery,
            destination_has_history,
        )?;
        frame.metadata.set_timing_with_format(
            Some(delivery.capture_time),
            nonzero_timestamp(delivery.system_relative_time_hns),
            TickFormat::Hns100,
        );
        frame.metadata.is_duplicate = delivery.is_duplicate;
        frame.metadata.dirty_rects = delivery.dirty_rects;
        self.last_delivery = Some(DeliveredGeneration {
            epoch: delivery.epoch,
            generation: delivery.generation,
            target: delivery.target,
        });
        #[cfg(feature = "stage-timing")]
        attach_stage_timings(&mut frame, stages);
        Ok(frame)
    }

    fn capture_region(
        &mut self,
        blit: CaptureBlitRegion,
        frame: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<CaptureSampleMetadata> {
        if blit.width == 0 || blit.height == 0 {
            return Err(CaptureError::InvalidConfig(
                "capture region dimensions must be non-zero".into(),
            ));
        }
        #[cfg(feature = "stage-timing")]
        let stages = StageScope::enter(self.record_stage_timings);
        frame.reset_metadata();
        let target = ReadbackTarget::Region(blit);
        self.acquire_current(target, destination_has_history)?;
        stage_mark("wgc.transport_wait");
        let canonical = self
            .canonical
            .latest()
            .cloned()
            .ok_or(CaptureError::Timeout)?;
        #[cfg(feature = "stage-timing")]
        record_wgc_frame_age(canonical.system_relative_time_hns);

        if self.same_delivered_generation(target, &canonical, destination_has_history) {
            #[cfg(feature = "stage-timing")]
            attach_stage_timings(frame, stages);
            return Ok(CaptureSampleMetadata {
                capture_time: Some(Instant::now()),
                raw_os_ticks: nonzero_timestamp(canonical.system_relative_time_hns),
                tick_format: TickFormat::Hns100,
                is_duplicate: true,
                dirty_rects: Vec::new(),
            });
        }

        self.ensure_current_submitted(&canonical, target)?;
        stage_mark("wgc.submit");
        let delivery = self.readback.read_into(
            &self.context,
            canonical.epoch,
            canonical.generation,
            target,
            frame,
            self.last_delivery,
            destination_has_history,
        )?;
        self.last_delivery = Some(DeliveredGeneration {
            epoch: delivery.epoch,
            generation: delivery.generation,
            target: delivery.target,
        });
        #[cfg(feature = "stage-timing")]
        attach_stage_timings(frame, stages);
        Ok(CaptureSampleMetadata {
            capture_time: Some(delivery.capture_time),
            raw_os_ticks: nonzero_timestamp(delivery.system_relative_time_hns),
            tick_format: TickFormat::Hns100,
            is_duplicate: delivery.is_duplicate,
            dirty_rects: delivery.dirty_rects,
        })
    }

    fn same_delivered_generation(
        &self,
        target: ReadbackTarget,
        canonical: &CanonicalFrameMetadata,
        destination_has_history: bool,
    ) -> bool {
        destination_has_history
            && self.last_delivery.is_some_and(|delivered| {
                delivered.target == target
                    && delivered.epoch == canonical.epoch
                    && delivered.generation == canonical.generation
            })
    }
}

impl Drop for WgcWorker {
    fn drop(&mut self) {
        let _ = self.frame_pool.RemoveFrameArrived(self.frame_arrived_token);
        let _ = self.item.RemoveClosed(self.closed_token);
        let _ = self.session.Close();
        let _ = self.frame_pool.Close();
    }
}

#[derive(Clone, Copy, Debug)]
enum FrameProcessing {
    Updated,
    Ignored,
    Resize(SizeInt32),
    Resynchronize,
    OrderedFault(OrderedFault),
}

enum PumpAction {
    Resize(SizeInt32),
    Resynchronize,
    OrderedFault(OrderedFault),
}

fn nonzero_timestamp(value: i64) -> Option<i64> {
    (value != 0).then_some(value)
}

pub(crate) struct WindowsMonitorCapturer {
    inner: PreparedWgcCapturer,
}

impl WindowsMonitorCapturer {
    pub(crate) fn new(monitor: &MonitorId, resolver: Arc<MonitorResolver>) -> CaptureResult<Self> {
        validate_support()?;
        let resolved = resolver.resolve_monitor(monitor)?;
        let adapter_luid = resolved.key.adapter_luid;
        let monitor = resolved.handle.0 as usize;
        let hdr_metadata = resolved.hdr_metadata;
        drop(resolved);
        let inner = PreparedWgcCapturer::new(WorkerTarget::Monitor {
            adapter_luid,
            monitor,
            hdr_metadata,
        });
        Ok(Self { inner })
    }
}

impl crate::backend::MonitorCapturer for WindowsMonitorCapturer {
    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::WindowsGraphicsCapture
    }

    fn prewarm_environment(&mut self) -> CaptureResult<()> {
        Ok(())
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        self.inner.capture_with_history_hint(reuse, false)
    }

    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        self.inner
            .capture_with_history_hint(reuse, destination_has_history)
    }

    fn capture_region_into(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        self.inner
            .capture_region_into(blit, destination, destination_has_history)
            .map(Some)
    }

    fn set_capture_mode(&mut self, mode: CaptureMode) -> CaptureResult<()> {
        self.inner.set_capture_mode(mode)
    }

    fn set_gpu_hdr_conversion(&mut self, enabled: bool) -> CaptureResult<()> {
        self.inner.set_gpu_hdr_conversion(enabled)
    }

    fn set_hdr_tonemap_lut(&mut self, enabled: bool) -> CaptureResult<()> {
        self.inner.set_hdr_tonemap_lut(enabled)
    }

    fn set_wgc_update_mode(&mut self, mode: WgcUpdateMode) -> CaptureResult<()> {
        self.inner.set_wgc_update_mode(mode)
    }

    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.inner.set_output_pixel_format(format)
    }

    #[cfg(feature = "stage-timing")]
    fn set_record_stage_timings(&mut self, enabled: bool) -> CaptureResult<()> {
        self.inner.set_record_stage_timings(enabled)
    }

    fn release_capture_access(&mut self) {
        self.inner.release_capture_access();
    }

    fn capture_access_active(&self) -> bool {
        self.inner.capture_access_active()
    }
}

pub(crate) struct WindowsWindowCapturer {
    inner: PreparedWgcCapturer,
}

impl WindowsWindowCapturer {
    pub(crate) fn new(window: &WindowId) -> CaptureResult<Self> {
        validate_support()?;
        let hwnd = window.raw_handle();
        if hwnd == 0 {
            return Err(CaptureError::InvalidTarget(format!(
                "window handle is null: {}",
                window.stable_id()
            )));
        }
        let inner = PreparedWgcCapturer::new(WorkerTarget::Window {
            hwnd: hwnd as usize,
        });
        Ok(Self { inner })
    }
}

impl crate::backend::MonitorCapturer for WindowsWindowCapturer {
    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::WindowsGraphicsCapture
    }

    fn prewarm_environment(&mut self) -> CaptureResult<()> {
        Ok(())
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        self.inner.capture_with_history_hint(reuse, false)
    }

    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        self.inner
            .capture_with_history_hint(reuse, destination_has_history)
    }

    fn set_capture_mode(&mut self, mode: CaptureMode) -> CaptureResult<()> {
        self.inner.set_capture_mode(mode)
    }

    fn set_gpu_hdr_conversion(&mut self, enabled: bool) -> CaptureResult<()> {
        self.inner.set_gpu_hdr_conversion(enabled)
    }

    fn set_hdr_tonemap_lut(&mut self, enabled: bool) -> CaptureResult<()> {
        self.inner.set_hdr_tonemap_lut(enabled)
    }

    fn set_wgc_update_mode(&mut self, mode: WgcUpdateMode) -> CaptureResult<()> {
        self.inner.set_wgc_update_mode(mode)
    }

    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.inner.set_output_pixel_format(format)
    }

    #[cfg(feature = "stage-timing")]
    fn set_record_stage_timings(&mut self, enabled: bool) -> CaptureResult<()> {
        self.inner.set_record_stage_timings(enabled)
    }

    fn release_capture_access(&mut self) {
        self.inner.release_capture_access();
    }

    fn capture_access_active(&self) -> bool {
        self.inner.capture_access_active()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_update_policy_is_complete_surface() {
        assert!(!matches!(
            WgcUpdateMode::default(),
            WgcUpdateMode::OrderedIncremental
        ));
    }

    #[test]
    fn only_ordered_phase_requires_lossless_queue_drain() {
        assert_eq!(
            SourcePhase::Complete.drain_policy(),
            DrainPolicy::CompleteLatest
        );
        assert_eq!(
            SourcePhase::BaselineForOrdered.drain_policy(),
            DrainPolicy::CompleteLatest
        );
        assert_eq!(SourcePhase::Ordered.drain_policy(), DrainPolicy::Ordered);
    }

    #[test]
    fn zero_wgc_timestamp_is_not_exported() {
        assert_eq!(nonzero_timestamp(0), None);
        assert_eq!(nonzero_timestamp(42), Some(42));
    }

    #[test]
    fn unsupported_contract_opens_ordered_circuit_immediately() {
        let mut health = OrderedHealth::default();
        health.record_fault(OrderedFault::UnsupportedContract);
        assert!(health.disabled);
        assert!(!health.enabled_for(WgcUpdateMode::OrderedIncremental));
    }

    #[test]
    fn continuity_fault_limit_opens_ordered_circuit() {
        let mut health = OrderedHealth::default();
        for _ in 0..WGC_ORDERED_FAULT_LIMIT - 1 {
            health.record_fault(OrderedFault::Continuity);
            assert!(!health.disabled);
        }
        health.record_fault(OrderedFault::Continuity);
        assert!(health.disabled);
    }

    #[test]
    fn ordered_health_reset_closes_circuit() {
        let mut health = OrderedHealth::default();
        health.record_fault(OrderedFault::UnsupportedContract);
        health.reset();
        assert_eq!(health, OrderedHealth::default());
        assert!(health.enabled_for(WgcUpdateMode::OrderedIncremental));
    }

    #[test]
    fn known_device_loss_codes_are_retryable_access_loss() {
        for code in [
            DXGI_ERROR_ACCESS_LOST,
            DXGI_ERROR_DEVICE_HUNG,
            DXGI_ERROR_DEVICE_REMOVED,
            DXGI_ERROR_DEVICE_RESET,
            DXGI_ERROR_DRIVER_INTERNAL_ERROR,
        ] {
            assert!(is_device_lost_hresult(code));
            assert!(matches!(
                map_platform_error(windows::core::Error::from_hresult(code), "test"),
                CaptureError::AccessLost
            ));
        }
    }
}
