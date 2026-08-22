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
    DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DRIVER_INTERNAL_ERROR, IDXGIAdapter, IDXGIDevice,
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
use crate::frame::Frame;
use crate::monitor::MonitorId;
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
const WGC_FRAME_POOL_BUFFERS: i32 = 3;
// The coalesced pending snapshot owns one pool frame until the next request.
// A spare buffer is required so an idle session can keep replacing it.
const WGC_SNAPSHOT_FRAME_POOL_BUFFERS: i32 = 2;
const WGC_ORDERED_QUEUE_CAPACITY: usize = 32;
const WGC_COMMAND_CAPACITY: usize = 8;
const WGC_ORDERED_FAULT_LIMIT: u8 = 3;

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

type WgcD3dDevice = (ID3D11Device, ID3D11DeviceContext);

fn spawn_wgc_device_creation(
    adapter: Option<IDXGIAdapter>,
) -> CaptureResult<JoinHandle<CaptureResult<WgcD3dDevice>>> {
    thread::Builder::new()
        .name("snow-wgc-device".into())
        .spawn(move || {
            match adapter.as_ref() {
                Some(adapter) => d3d11::create_d3d11_device_for_adapter(adapter, false),
                None => d3d11::create_d3d11_device_default(false),
            }
            .map_err(CaptureError::platform)
        })
        .context("failed to spawn WGC device bootstrap thread")
        .map_err(CaptureError::platform)
}

fn join_wgc_device_creation(
    join: JoinHandle<CaptureResult<WgcD3dDevice>>,
) -> CaptureResult<WgcD3dDevice> {
    join.join().map_err(|_| {
        CaptureError::platform(anyhow::anyhow!("WGC device bootstrap thread panicked"))
    })?
}

fn initialize_runtime() -> CaptureResult<()> {
    let _com = CoInitGuard::init_multithreaded().map_err(CaptureError::platform)?;
    // Item/session creation is the authoritative support check. Calling
    // GraphicsCaptureSession::IsSupported here duplicates WinRT startup work.
    super::com::ensure_process_mta_usage().map_err(CaptureError::platform)
}

#[derive(Clone)]
enum WorkerTarget {
    Monitor {
        adapter: IDXGIAdapter,
        monitor: usize,
        hdr_metadata: HdrMonitorMetadata,
    },
    Window {
        hwnd: usize,
        adapter: IDXGIAdapter,
        hdr_metadata: HdrMonitorMetadata,
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
    CloseAccess {
        response: Sender<CaptureResult<()>>,
    },
    Shutdown,
}

#[derive(Clone, Copy)]
struct WgcWorkerConfig {
    capture_mode: CaptureMode,
    gpu_hdr_conversion_enabled: bool,
    hdr_tonemap_lut_enabled: bool,
    update_mode: WgcUpdateMode,
}

impl WgcWorkerConfig {
    fn frame_pool_buffers(self) -> i32 {
        if self.capture_mode == CaptureMode::Snapshot
            && self.update_mode != WgcUpdateMode::OrderedIncremental
        {
            WGC_SNAPSHOT_FRAME_POOL_BUFFERS
        } else {
            WGC_FRAME_POOL_BUFFERS
        }
    }
}

struct WindowsGraphicsCaptureCapturer {
    commands: Sender<WorkerCommand>,
    join: Option<JoinHandle<()>>,
}

impl WindowsGraphicsCaptureCapturer {
    fn spawn(
        target: WorkerTarget,
        config: WgcWorkerConfig,
        startup_timeout: Duration,
    ) -> CaptureResult<Self> {
        let (command_tx, command_rx) = crossbeam_channel::bounded(WGC_COMMAND_CAPACITY);
        let (startup_tx, startup_rx) = crossbeam_channel::bounded(1);
        let join = thread::Builder::new()
            .name("snow-wgc".into())
            .spawn(move || match WgcWorker::new(target, config) {
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

    fn close_access(&mut self) -> Option<JoinHandle<()>> {
        let Some(join) = self.join.take() else {
            return None;
        };
        let (response_tx, response_rx) = crossbeam_channel::bounded(1);
        let access_closed = self
            .commands
            .send(WorkerCommand::CloseAccess {
                response: response_tx,
            })
            .is_ok()
            && matches!(response_rx.recv(), Ok(Ok(())));
        if !access_closed {
            // Without an acknowledgement, only joining proves that worker
            // unwinding has run WgcWorker::drop and closed capture access.
            let _ = join.join();
            return None;
        }
        // Capture access is already closed. The owner retains this handle so
        // teardown is outside the snapshot return path but can never outlive
        // the CaptureSession that loaded this code.
        Some(join)
    }
}

struct PreparedWgcCapturer {
    target: WorkerTarget,
    active: Option<WindowsGraphicsCaptureCapturer>,
    capture_mode: CaptureMode,
    gpu_hdr_conversion_enabled: bool,
    hdr_tonemap_lut_enabled: bool,
    update_mode: WgcUpdateMode,
    retired_workers: Vec<JoinHandle<()>>,
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
            retired_workers: Vec::new(),
        }
    }

    fn reap_finished_workers(&mut self) {
        let mut index = 0;
        while index < self.retired_workers.len() {
            if self.retired_workers[index].is_finished() {
                let worker = self.retired_workers.swap_remove(index);
                let _ = worker.join();
            } else {
                index += 1;
            }
        }
    }

    fn ensure_active(&mut self) -> CaptureResult<&mut WindowsGraphicsCaptureCapturer> {
        self.reap_finished_workers();
        if self.active.is_none() {
            self.active = Some(WindowsGraphicsCaptureCapturer::spawn(
                self.target.clone(),
                WgcWorkerConfig {
                    capture_mode: self.capture_mode,
                    gpu_hdr_conversion_enabled: self.gpu_hdr_conversion_enabled,
                    hdr_tonemap_lut_enabled: self.hdr_tonemap_lut_enabled,
                    update_mode: self.update_mode,
                },
                WGC_WORKER_START_TIMEOUT,
            )?);
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

    fn release_capture_access(&mut self) {
        if let Some(mut active) = self.active.take() {
            if let Some(worker) = active.close_access() {
                self.retired_workers.push(worker);
            }
        }
    }

    fn capture_access_active(&self) -> bool {
        self.active.is_some()
    }
}

impl Drop for PreparedWgcCapturer {
    fn drop(&mut self) {
        self.release_capture_access();
        for worker in self.retired_workers.drain(..) {
            let _ = worker.join();
        }
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
    frame_pool_buffers: i32,
    source_phase: SourcePhase,
    update_mode: WgcUpdateMode,
    ordered_health: OrderedHealth,
    dirty_regions_supported: bool,
    canonical: CanonicalSurface,
    readback: ReadbackPipeline,
    capture_mode: CaptureMode,
    hdr_to_sdr: Option<HdrFrameContext>,
    gpu_tonemapper: Option<GpuTonemapper>,
    gpu_f16_converter: Option<GpuF16Converter>,
    gpu_hdr_conversion_enabled: bool,
    hdr_tonemap_lut_enabled: bool,
    last_delivery: Option<DeliveredGeneration>,
    pending_complete_snapshot: Option<FramePacket>,
    closed: bool,
    terminal_error: Option<CaptureError>,
    access_closed: bool,
    _com: CoInitGuard,
}

impl WgcWorker {
    fn new(target: WorkerTarget, config: WgcWorkerConfig) -> CaptureResult<Self> {
        let com = CoInitGuard::init_multithreaded().map_err(CaptureError::platform)?;
        let (device, context, item, mut hdr_to_sdr) = match target {
            WorkerTarget::Monitor {
                adapter,
                monitor,
                hdr_metadata,
            } => {
                let device_creation = spawn_wgc_device_creation(Some(adapter))?;
                let monitor = HMONITOR(monitor as *mut c_void);
                let item_result = create_monitor_capture_item(monitor);
                let device_result = join_wgc_device_creation(device_creation);
                let item = item_result?;
                let (device, context) = device_result?;
                (device, context, item, hdr_to_sdr_params(hdr_metadata))
            }
            WorkerTarget::Window {
                hwnd,
                adapter,
                hdr_metadata,
            } => {
                let device_creation = spawn_wgc_device_creation(Some(adapter))?;
                let hwnd = HWND(hwnd as *mut c_void);
                let item_result = create_window_capture_item(hwnd);
                let device_result = join_wgc_device_creation(device_creation);
                let item = item_result?;
                let (device, context) = device_result?;
                (device, context, item, hdr_to_sdr_params(hdr_metadata))
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
        let frame_pool_buffers = config.frame_pool_buffers();
        let frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            &winrt_device,
            pixel_format,
            frame_pool_buffers,
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

        let source_phase =
            if dirty_regions_supported && config.update_mode == WgcUpdateMode::OrderedIncremental {
                SourcePhase::BaselineForOrdered
            } else {
                SourcePhase::Complete
            };
        if let Some(params) = hdr_to_sdr.as_mut() {
            params.tonemap_use_lut = config.hdr_tonemap_lut_enabled;
        }

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
            frame_pool_buffers,
            source_phase,
            update_mode: config.update_mode,
            ordered_health: OrderedHealth::default(),
            dirty_regions_supported,
            canonical: CanonicalSurface::new(),
            readback: ReadbackPipeline::new(),
            capture_mode: config.capture_mode,
            hdr_to_sdr,
            gpu_tonemapper: None,
            gpu_f16_converter: None,
            gpu_hdr_conversion_enabled: config.gpu_hdr_conversion_enabled,
            hdr_tonemap_lut_enabled: config.hdr_tonemap_lut_enabled,
            last_delivery: None,
            pending_complete_snapshot: None,
            closed: false,
            terminal_error: None,
            access_closed: false,
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
            WorkerCommand::CloseAccess { response } => {
                let result = self.close_capture_access();
                let _ = response.send(result);
                return false;
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

        let direct_complete_snapshot = self.capture_mode == CaptureMode::Snapshot
            && self.source_phase == SourcePhase::Complete
            && reported_mode == GraphicsCaptureDirtyRegionMode::ReportOnly
            && source_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT
            // A region target can change after topology replanning. Retain the
            // canonical texture so that the current frame can be resubmitted
            // for the new crop even when WGC has not produced a fresher frame.
            && supports_textureless_snapshot(self.readback.target());
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
            !direct_complete_snapshot,
        )?;
        match outcome {
            ApplyOutcome::Updated(metadata) => {
                if direct_complete_snapshot {
                    let target = self.readback.target().ok_or(CaptureError::Timeout)?;
                    if let Err(error) = self.readback.ensure_submitted(
                        &self.device,
                        &self.context,
                        &texture,
                        source_desc,
                        None,
                        &metadata,
                        target,
                    ) {
                        // Publication and direct submission form one logical
                        // state transition. Never leave valid metadata behind
                        // after dropping both its canonical source and slot.
                        self.canonical.invalidate();
                        self.invalidate_delivery_pipeline();
                        return Err(error);
                    }
                } else {
                    self.prefetch_current(&metadata)?;
                }
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
                    self.frame_pool_buffers,
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
        frame.reset_metadata();
        let target = ReadbackTarget::Full;
        self.acquire_current(target, destination_has_history)?;
        let canonical = self
            .canonical
            .latest()
            .cloned()
            .ok_or(CaptureError::Timeout)?;

        if self.same_delivered_generation(target, &canonical, destination_has_history) {
            frame.metadata.set_timing_with_format(
                Some(Instant::now()),
                nonzero_timestamp(canonical.system_relative_time_hns),
                TickFormat::Hns100,
            );
            frame.metadata.is_duplicate = true;
            frame.metadata.dirty_rects.clear();
            return Ok(frame);
        }

        self.ensure_current_submitted(&canonical, target)?;
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
        frame.reset_metadata();
        let target = ReadbackTarget::Region(blit);
        self.acquire_current(target, destination_has_history)?;
        let canonical = self
            .canonical
            .latest()
            .cloned()
            .ok_or(CaptureError::Timeout)?;

        if self.same_delivered_generation(target, &canonical, destination_has_history) {
            return Ok(CaptureSampleMetadata {
                capture_time: Some(Instant::now()),
                raw_os_ticks: nonzero_timestamp(canonical.system_relative_time_hns),
                tick_format: TickFormat::Hns100,
                is_duplicate: true,
                dirty_rects: Vec::new(),
            });
        }

        self.ensure_current_submitted(&canonical, target)?;
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

    fn close_capture_access(&mut self) -> CaptureResult<()> {
        if self.access_closed {
            return Ok(());
        }
        let mut first_error = self.transport.pause_and_clear().err();
        if let Err(error) = self.frame_pool.RemoveFrameArrived(self.frame_arrived_token) {
            first_error.get_or_insert_with(|| {
                map_platform_error(
                    error,
                    "Direct3D11CaptureFramePool::RemoveFrameArrived failed during close",
                )
            });
        }
        if let Err(error) = self.item.RemoveClosed(self.closed_token) {
            first_error.get_or_insert_with(|| {
                map_platform_error(
                    error,
                    "GraphicsCaptureItem::RemoveClosed failed during close",
                )
            });
        }
        if let Err(error) = self.session.Close() {
            first_error.get_or_insert_with(|| {
                map_platform_error(error, "GraphicsCaptureSession::Close failed")
            });
        }
        if let Err(error) = self.frame_pool.Close() {
            first_error.get_or_insert_with(|| {
                map_platform_error(error, "Direct3D11CaptureFramePool::Close failed")
            });
        }
        if first_error.is_none() {
            self.access_closed = true;
        }
        first_error.map_or(Ok(()), Err)
    }
}

impl Drop for WgcWorker {
    fn drop(&mut self) {
        let _ = self.close_capture_access();
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

fn supports_textureless_snapshot(target: Option<ReadbackTarget>) -> bool {
    matches!(target, Some(ReadbackTarget::Full))
}

pub(crate) struct WindowsMonitorCapturer {
    inner: PreparedWgcCapturer,
}

impl WindowsMonitorCapturer {
    pub(crate) fn new(monitor: &MonitorId, resolver: Arc<MonitorResolver>) -> CaptureResult<Self> {
        initialize_runtime()?;
        let resolved = resolver.resolve_monitor(monitor)?;
        let adapter = resolved.adapter.clone();
        let monitor = resolved.handle.0 as usize;
        let hdr_metadata = resolved.hdr_metadata;
        drop(resolved);
        let inner = PreparedWgcCapturer::new(WorkerTarget::Monitor {
            adapter,
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
    pub(crate) fn new(window: &WindowId, resolver: Arc<MonitorResolver>) -> CaptureResult<Self> {
        initialize_runtime()?;
        let hwnd = window.raw_handle();
        if hwnd == 0 {
            return Err(CaptureError::InvalidTarget(format!(
                "window handle is null: {}",
                window.stable_id()
            )));
        }
        let resolved = resolver.resolve_monitor_for_window(HWND(hwnd as *mut c_void))?;
        let adapter = resolved.adapter.clone();
        let hdr_metadata = resolved.hdr_metadata;
        drop(resolved);
        let inner = PreparedWgcCapturer::new(WorkerTarget::Window {
            hwnd: hwnd as usize,
            adapter,
            hdr_metadata,
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

    const TEST_TIMEOUT: Duration = Duration::from_secs(5);

    fn worker_config(capture_mode: CaptureMode, update_mode: WgcUpdateMode) -> WgcWorkerConfig {
        WgcWorkerConfig {
            capture_mode,
            gpu_hdr_conversion_enabled: true,
            hdr_tonemap_lut_enabled: true,
            update_mode,
        }
    }

    #[test]
    fn default_update_policy_is_complete_surface() {
        assert!(!matches!(
            WgcUpdateMode::default(),
            WgcUpdateMode::OrderedIncremental
        ));
    }

    #[test]
    fn snapshot_complete_mode_keeps_one_spare_pool_buffer() {
        assert_eq!(WGC_SNAPSHOT_FRAME_POOL_BUFFERS, 2);
        assert_eq!(
            worker_config(CaptureMode::Snapshot, WgcUpdateMode::Auto).frame_pool_buffers(),
            WGC_SNAPSHOT_FRAME_POOL_BUFFERS
        );
        assert_eq!(
            worker_config(CaptureMode::Snapshot, WgcUpdateMode::CompleteOnly).frame_pool_buffers(),
            WGC_SNAPSHOT_FRAME_POOL_BUFFERS
        );
        assert_eq!(
            worker_config(CaptureMode::Snapshot, WgcUpdateMode::OrderedIncremental)
                .frame_pool_buffers(),
            WGC_FRAME_POOL_BUFFERS
        );
        assert_eq!(
            worker_config(CaptureMode::Continuous, WgcUpdateMode::CompleteOnly)
                .frame_pool_buffers(),
            WGC_FRAME_POOL_BUFFERS
        );
    }

    #[test]
    fn textureless_snapshot_fast_path_is_full_frame_only() {
        assert!(supports_textureless_snapshot(Some(ReadbackTarget::Full)));
        assert!(!supports_textureless_snapshot(Some(
            ReadbackTarget::Region(CaptureBlitRegion {
                src_x: 1,
                src_y: 2,
                width: 3,
                height: 4,
                dst_x: 0,
                dst_y: 0,
            })
        )));
        assert!(!supports_textureless_snapshot(None));
    }

    #[test]
    fn close_access_returns_cleanup_handle_after_access_ack() {
        let (command_tx, command_rx) = crossbeam_channel::bounded(WGC_COMMAND_CAPACITY);
        let (access_closed_tx, access_closed_rx) = crossbeam_channel::bounded(1);
        let (release_tail_tx, release_tail_rx) = crossbeam_channel::bounded(1);
        let (tail_done_tx, tail_done_rx) = crossbeam_channel::bounded(1);
        let worker = thread::spawn(move || {
            match command_rx.recv().expect("close command") {
                WorkerCommand::CloseAccess { response } => {
                    access_closed_tx.send(()).expect("access-closed marker");
                    response.send(Ok(())).expect("close acknowledgement");
                }
                _ => panic!("unexpected worker command"),
            }
            release_tail_rx.recv().expect("cleanup-tail release");
            tail_done_tx.send(()).expect("cleanup-tail marker");
        });
        let capturer = WindowsGraphicsCaptureCapturer {
            commands: command_tx,
            join: Some(worker),
        };
        let (returned_tx, returned_rx) = crossbeam_channel::bounded(1);
        let close_call = thread::spawn(move || {
            let mut capturer = capturer;
            let cleanup = capturer
                .close_access()
                .expect("successful close must return its cleanup handle");
            returned_tx.send(cleanup).expect("close-return marker");
        });

        access_closed_rx
            .recv_timeout(TEST_TIMEOUT)
            .expect("access closure did not precede the acknowledgement");
        let cleanup = returned_rx
            .recv_timeout(TEST_TIMEOUT)
            .expect("close_access waited for cleanup after capture access was closed");
        release_tail_tx.send(()).expect("release cleanup tail");
        tail_done_rx
            .recv_timeout(TEST_TIMEOUT)
            .expect("detached cleanup tail did not finish");
        cleanup.join().expect("cleanup tail panicked");
        close_call.join().expect("close-access caller panicked");
    }

    #[test]
    fn close_access_joins_worker_after_failed_close_acknowledgement() {
        let (command_tx, command_rx) = crossbeam_channel::bounded(WGC_COMMAND_CAPACITY);
        let (release_worker_tx, release_worker_rx) = crossbeam_channel::bounded(1);
        let worker = thread::spawn(move || {
            match command_rx.recv().expect("close command") {
                WorkerCommand::CloseAccess { response } => response
                    .send(Err(CaptureError::platform(anyhow::anyhow!(
                        "injected close failure"
                    ))))
                    .expect("failed-close acknowledgement"),
                _ => panic!("unexpected worker command"),
            }
            release_worker_rx.recv().expect("worker release");
        });
        let capturer = WindowsGraphicsCaptureCapturer {
            commands: command_tx,
            join: Some(worker),
        };
        let (returned_tx, returned_rx) = crossbeam_channel::bounded(1);
        let close_call = thread::spawn(move || {
            let mut capturer = capturer;
            assert!(capturer.close_access().is_none());
            returned_tx.send(()).expect("close-return marker");
        });

        assert!(
            returned_rx.recv_timeout(Duration::from_millis(50)).is_err(),
            "close_access returned after a failed close acknowledgement but before worker exit"
        );
        release_worker_tx.send(()).expect("release worker");
        returned_rx
            .recv_timeout(TEST_TIMEOUT)
            .expect("close_access did not return after worker exit");
        close_call.join().expect("close-access caller panicked");
    }

    #[test]
    fn close_access_joins_worker_when_close_cannot_be_acknowledged() {
        let (command_tx, command_rx) = crossbeam_channel::bounded(WGC_COMMAND_CAPACITY);
        drop(command_rx);
        let (worker_waiting_tx, worker_waiting_rx) = crossbeam_channel::bounded(1);
        let (release_worker_tx, release_worker_rx) = crossbeam_channel::bounded(1);
        let worker = thread::spawn(move || {
            worker_waiting_tx.send(()).expect("worker-waiting marker");
            release_worker_rx.recv().expect("worker release");
        });
        let capturer = WindowsGraphicsCaptureCapturer {
            commands: command_tx,
            join: Some(worker),
        };
        let (returned_tx, returned_rx) = crossbeam_channel::bounded(1);
        let close_call = thread::spawn(move || {
            let mut capturer = capturer;
            let _ = capturer.close_access();
            returned_tx.send(()).expect("close-return marker");
        });

        worker_waiting_rx
            .recv_timeout(TEST_TIMEOUT)
            .expect("worker did not reach the teardown gate");
        assert!(
            returned_rx.try_recv().is_err(),
            "close_access returned without an acknowledgement or worker exit"
        );
        release_worker_tx.send(()).expect("release worker");
        returned_rx
            .recv_timeout(TEST_TIMEOUT)
            .expect("close_access did not return after worker exit");
        close_call.join().expect("close-access caller panicked");
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
