//! Media Foundation H.264 Encoder MFT driven directly from D3D11 textures.
//!
//! Wraps the async hardware encoder MFT (`CLSID_CMSH264EncoderMFT`) with
//! an `IMFDXGIDeviceManager` bound to the capture device, mirroring the
//! in-process settings of the FFmpeg `h264_mf` path (CBR, no B-frames,
//! GOP 60, High profile). Output samples are Annex-B byte streams.

use std::mem::ManuallyDrop;
use std::time::{Duration, Instant};

use windows::Win32::Foundation::RPC_E_CHANGED_MODE;
use windows::Win32::Graphics::Direct3D11::{ID3D11Device, ID3D11Texture2D};
use windows::Win32::Media::MediaFoundation::{
    CODECAPI_AVEncCommonMeanBitRate, CODECAPI_AVEncCommonRateControlMode,
    CODECAPI_AVEncMPVDefaultBPictureCount, CODECAPI_AVEncMPVGOPSize, ICodecAPI, IMFActivate,
    IMFAttributes, IMFDXGIDeviceManager, IMFMediaBuffer, IMFMediaEvent, IMFMediaEventGenerator,
    IMFMediaType, IMFSample, IMFTransform, METransformDrainComplete, METransformHaveOutput,
    METransformNeedInput, MF_E_NO_EVENTS_AVAILABLE, MF_E_TRANSFORM_STREAM_CHANGE,
    MF_EVENT_FLAG_NO_WAIT, MF_MT_AVG_BITRATE, MF_MT_FRAME_RATE, MF_MT_FRAME_SIZE,
    MF_MT_INTERLACE_MODE, MF_MT_MAJOR_TYPE, MF_MT_MPEG_SEQUENCE_HEADER, MF_MT_SUBTYPE,
    MF_TRANSFORM_ASYNC, MF_TRANSFORM_ASYNC_UNLOCK, MF_VERSION, MFCreateDXGIDeviceManager,
    MFCreateDXGISurfaceBuffer, MFCreateMediaType, MFCreateSample, MFMediaType_Video,
    MFSTARTUP_LITE, MFSampleExtension_CleanPoint, MFShutdown, MFStartup,
    MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE, MFT_ENUM_FLAG_SORTANDFILTER,
    MFT_MESSAGE_COMMAND_DRAIN, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,
    MFT_MESSAGE_NOTIFY_END_OF_STREAM, MFT_MESSAGE_NOTIFY_END_STREAMING,
    MFT_MESSAGE_NOTIFY_START_OF_STREAM, MFT_MESSAGE_SET_D3D_MANAGER, MFT_OUTPUT_DATA_BUFFER,
    MFT_OUTPUT_STREAM_PROVIDES_SAMPLES, MFT_REGISTER_TYPE_INFO, MFTEnumEx, MFVideoFormat_H264,
    MFVideoFormat_NV12, MFVideoInterlace_Progressive, eAVEncCommonRateControlMode_CBR,
};
use windows::Win32::System::Com::{
    COINIT_MULTITHREADED, CoInitializeEx, CoTaskMemFree, CoUninitialize,
};
use windows::Win32::System::Variant::{VARIANT, VT_UI4};
use windows::core::{GUID, Interface};

use crate::error::{GpuEncoderError, Result};

/// Keyframe interval; matches the FFmpeg `h264_mf` speed options (`g=60`).
pub(crate) const GOP_SIZE: u32 = 60;

const NEED_INPUT_TIMEOUT: Duration = Duration::from_secs(1);
const DRAIN_TIMEOUT: Duration = Duration::from_secs(5);

/// Event type as a plain integer for comparison with `MF_EVENT_TYPE`
/// constants (their `GetType` binding returns the raw value).
fn event_kind(event: &IMFMediaEvent) -> Result<u32> {
    unsafe { event.GetType() }
        .map_err(|error| GpuEncoderError::Output(format!("event type failed: {error}")))
}

/// Balance a successful `CoInitializeEx` on drop.
struct ComGuard {
    initialized: bool,
}

impl ComGuard {
    fn init() -> Result<Self> {
        let result = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) };
        if result.is_ok() {
            // S_OK and S_FALSE both require a balancing CoUninitialize.
            Ok(Self { initialized: true })
        } else if result == RPC_E_CHANGED_MODE {
            Ok(Self { initialized: false })
        } else {
            Err(GpuEncoderError::EncoderInit(format!(
                "CoInitializeEx failed: {result}"
            )))
        }
    }
}

impl Drop for ComGuard {
    fn drop(&mut self) {
        if self.initialized {
            unsafe { CoUninitialize() };
        }
    }
}

/// Activate the first hardware H.264 encoder MFT, mirroring ffmpeg's
/// `MFTEnumEx`-based hw path. `CoCreateInstance` of the CMSH264EncoderMFT
/// CLSID yields a shell without D3D11 support on current Windows, so the
/// enumeration route is required.
fn activate_hardware_mft() -> Result<IMFTransform> {
    let output_info = MFT_REGISTER_TYPE_INFO {
        guidMajorType: MFMediaType_Video,
        guidSubtype: MFVideoFormat_H264,
    };
    let mut activates = std::ptr::null_mut();
    let mut count = 0u32;
    unsafe {
        MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            None,
            Some(&output_info),
            &mut activates,
            &mut count,
        )
    }
    .map_err(|error| {
        GpuEncoderError::EncoderInit(format!("MFTEnumEx(video encoder) failed: {error}"))
    })?;
    if count == 0 || activates.is_null() {
        if !activates.is_null() {
            unsafe { CoTaskMemFree(Some(activates.cast())) };
        }
        return Err(GpuEncoderError::EncoderInit(
            "no hardware H.264 encoder MFT is registered".into(),
        ));
    }
    let result = (|| -> Result<IMFTransform> {
        let activate: IMFActivate =
            unsafe { std::slice::from_raw_parts(activates, count as usize) }[0]
                .clone()
                .expect("enumerated activation object");
        let transform = unsafe { activate.ActivateObject::<IMFTransform>() }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("ActivateObject failed: {error}"))
        })?;
        Ok(transform)
    })();
    unsafe { CoTaskMemFree(Some(activates.cast())) };
    result
}

fn ui4_variant(value: u32) -> VARIANT {
    use windows::Win32::System::Variant::{VARIANT_0, VARIANT_0_0};
    let inner = VARIANT_0_0 {
        vt: VT_UI4,
        wReserved1: 0,
        wReserved2: 0,
        wReserved3: 0,
        Anonymous: unsafe { std::mem::zeroed() },
    };
    // The vt tag rides in the same union arm as the payload.
    let mut inner = inner;
    inner.Anonymous.ulVal = value;
    VARIANT {
        Anonymous: VARIANT_0 {
            Anonymous: ManuallyDrop::new(inner),
        },
    }
}

fn set_codec_u32(codec_api: &ICodecAPI, api: &GUID, value: u32) -> Result<()> {
    let variant = ui4_variant(value);
    unsafe { codec_api.SetValue(api, &variant) }.map_err(|error| {
        GpuEncoderError::EncoderInit(format!("codec API setting {api:?} failed: {error}"))
    })
}

fn set_media_type_u64(media_type: &IMFMediaType, key: &GUID, value: u64) -> Result<()> {
    unsafe { media_type.SetUINT64(key, value) }.map_err(|error| {
        GpuEncoderError::EncoderInit(format!("media type setting {key:?} failed: {error}"))
    })
}

/// Pick the first available media type with the wanted subtype, mirroring
/// ffmpeg's negotiation: hardware MFTs validate against their offered
/// types and reject freshly built ones. `Ok(None)` ends the list;
/// `MF_E_TRANSFORM_TYPE_NOT_SET` propagates so the caller can negotiate
/// the other direction first.
fn read_attribute_blob(attributes: &IMFAttributes, key: &GUID) -> Option<Vec<u8>> {
    let size = unsafe { attributes.GetBlobSize(key) }.ok()? as usize;
    let mut data = vec![0u8; size];
    unsafe { attributes.GetBlob(key, &mut data, None) }.ok()?;
    Some(data)
}

/// One raw encoded sample, already converted to plain bytes.
pub(crate) struct RawPacket {
    pub timestamp_hns: i64,
    pub is_keyframe: bool,
    pub data: Vec<u8>,
}

pub(crate) struct MfH264Encoder {
    transform: Option<IMFTransform>,
    _device_manager: Option<IMFDXGIDeviceManager>,
    events: Option<IMFMediaEventGenerator>,
    need_input: bool,
    _com: ComGuard,
    finished: bool,
}

impl MfH264Encoder {
    /// Negotiate the encoder. Returns the encoder plus the negotiated
    /// Annex-B sequence header (SPS/PPS), if the encoder reports one.
    pub(crate) fn new(
        device: &ID3D11Device,
        width: u32,
        height: u32,
        fps: u32,
        bitrate_bps: usize,
    ) -> Result<(Self, Vec<u8>)> {
        let com = ComGuard::init()?;
        unsafe { MFStartup(MF_VERSION, MFSTARTUP_LITE) }
            .map_err(|error| GpuEncoderError::EncoderInit(format!("MFStartup failed: {error}")))?;

        let transform = activate_hardware_mft()?;

        let attributes = unsafe { transform.GetAttributes() }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("MFT attributes unavailable: {error}"))
        })?;
        let is_async = unsafe { attributes.GetUINT32(&MF_TRANSFORM_ASYNC) }.unwrap_or(0) != 0;
        if !is_async {
            return Err(GpuEncoderError::EncoderInit(
                "hardware H.264 encoder MFT is not async".into(),
            ));
        }
        unsafe { attributes.SetUINT32(&MF_TRANSFORM_ASYNC_UNLOCK, 1) }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("unlocking async MFT failed: {error}"))
        })?;
        let events: IMFMediaEventGenerator = transform.cast().map_err(|error| {
            GpuEncoderError::EncoderInit(format!("MFT exposes no event generator: {error}"))
        })?;

        // Fixed-stream MFTs return an error here and use stream id 0.
        let (input_stream_id, output_stream_id) = {
            let mut input_ids = [0u32; 1];
            let mut output_ids = [0u32; 1];
            let queried =
                unsafe { transform.GetStreamIDs(&mut input_ids, &mut output_ids) }.is_ok();
            if queried {
                (input_ids[0], output_ids[0])
            } else {
                (0, 0)
            }
        };

        let mut reset_token = 0u32;
        let mut manager = None;
        unsafe { MFCreateDXGIDeviceManager(&mut reset_token, &mut manager) }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("MFCreateDXGIDeviceManager failed: {error}"))
        })?;
        let manager = manager.ok_or_else(|| {
            GpuEncoderError::EncoderInit("MFCreateDXGIDeviceManager returned no manager".into())
        })?;
        // SAFETY: the windows-rs binding types ResetDevice's parameter as
        // IDirect3DDevice9 (the legacy DXVA signature), but the method
        // accepts the IUnknown of the device it manages; passing the raw
        // D3D11 device pointer matches the documented contract.
        unsafe {
            (Interface::vtable(&manager).ResetDevice)(
                Interface::as_raw(&manager),
                Interface::as_raw(device),
                reset_token,
            )
        }
        .ok()
        .map_err(|error| GpuEncoderError::EncoderInit(format!("ResetDevice failed: {error}")))?;

        let frame_size = (u64::from(width) << 32) | u64::from(height);
        let frame_rate = (u64::from(fps.max(1)) << 32) | 1;

        // Output type first, built from scratch: hardware encoder MFTs
        // accept complete client-built types while the offered skeletons
        // stay attribute-less templates (verified against AMD's encoder;
        // ffmpeg fills offered types the same way).
        let output_type = unsafe { MFCreateMediaType() }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("MFCreateMediaType failed: {error}"))
        })?;
        unsafe { output_type.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video) }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("setting major type failed: {error}"))
        })?;
        unsafe { output_type.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_H264) }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("setting subtype failed: {error}"))
        })?;

        let codec_api: ICodecAPI = transform.cast().map_err(|error| {
            GpuEncoderError::EncoderInit(format!("MFT exposes no ICodecAPI: {error}"))
        })?;
        set_codec_u32(
            &codec_api,
            &CODECAPI_AVEncCommonRateControlMode,
            eAVEncCommonRateControlMode_CBR.0 as u32,
        )?;
        set_codec_u32(
            &codec_api,
            &CODECAPI_AVEncCommonMeanBitRate,
            bitrate_bps.clamp(1, u32::MAX as usize) as u32,
        )?;
        set_codec_u32(&codec_api, &CODECAPI_AVEncMPVDefaultBPictureCount, 0)?;
        set_codec_u32(&codec_api, &CODECAPI_AVEncMPVGOPSize, GOP_SIZE)?;

        set_media_type_u64(&output_type, &MF_MT_FRAME_SIZE, frame_size)?;
        set_media_type_u64(&output_type, &MF_MT_FRAME_RATE, frame_rate)?;
        unsafe {
            output_type.SetUINT32(
                &MF_MT_AVG_BITRATE,
                bitrate_bps.clamp(1, u32::MAX as usize) as u32,
            )
        }
        .map_err(|error| {
            GpuEncoderError::EncoderInit(format!("setting bitrate failed: {error}"))
        })?;
        unsafe { transform.SetOutputType(output_stream_id, &output_type, 0) }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("SetOutputType(H264) failed: {error}"))
        })?;

        // Input type second, now that the encoder accepted the output.
        let input_type = unsafe { MFCreateMediaType() }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("MFCreateMediaType failed: {error}"))
        })?;
        unsafe { input_type.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video) }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("setting major type failed: {error}"))
        })?;
        unsafe { input_type.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_NV12) }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("setting subtype failed: {error}"))
        })?;
        set_media_type_u64(&input_type, &MF_MT_FRAME_SIZE, frame_size)?;
        set_media_type_u64(&input_type, &MF_MT_FRAME_RATE, frame_rate)?;
        unsafe {
            input_type.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)
        }
        .map_err(|error| {
            GpuEncoderError::EncoderInit(format!("setting interlace mode failed: {error}"))
        })?;
        unsafe { transform.SetInputType(input_stream_id, &input_type, 0) }.map_err(|error| {
            GpuEncoderError::EncoderInit(format!("SetInputType(NV12) failed: {error}"))
        })?;

        let stream_info =
            unsafe { transform.GetOutputStreamInfo(output_stream_id) }.map_err(|error| {
                GpuEncoderError::EncoderInit(format!("GetOutputStreamInfo failed: {error}"))
            })?;
        if stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES.0 as u32) == 0 {
            return Err(GpuEncoderError::EncoderInit(
                "encoder does not provide output samples".into(),
            ));
        }

        let sequence_header = unsafe { transform.GetOutputCurrentType(output_stream_id).ok() }
            .and_then(|current| read_attribute_blob(&current, &MF_MT_MPEG_SEQUENCE_HEADER))
            .unwrap_or_default();

        // Bind the device manager after type negotiation, before streaming
        // starts — the order ffmpeg's h264_mf wrapper uses.
        unsafe {
            transform.ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                Interface::as_raw(&manager) as usize,
            )
        }
        .map_err(|error| {
            GpuEncoderError::EncoderInit(format!("binding D3D11 device failed: {error}"))
        })?;

        unsafe { transform.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0) }.map_err(
            |error| GpuEncoderError::EncoderInit(format!("begin streaming failed: {error}")),
        )?;
        unsafe { transform.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0) }.map_err(
            |error| GpuEncoderError::EncoderInit(format!("start of stream failed: {error}")),
        )?;

        Ok((
            Self {
                transform: Some(transform),
                _device_manager: Some(manager),
                events: Some(events),
                need_input: false,
                _com: com,
                finished: false,
            },
            sequence_header,
        ))
    }

    fn transform(&self) -> &IMFTransform {
        self.transform
            .as_ref()
            .expect("encoder transform is available until drop")
    }

    /// Wait until the MFT asks for input, then submit `sample`. Returns any
    /// output packets that became available along the way.
    pub(crate) fn submit(&mut self, sample: &IMFSample) -> Result<Vec<RawPacket>> {
        if self.finished {
            return Err(GpuEncoderError::Submit("encoder already finished".into()));
        }
        let mut packets = Vec::new();
        if !self.need_input {
            packets.extend(self.wait_for_need_input()?);
        }
        let transform = self.transform();
        unsafe { transform.ProcessInput(0, sample, 0) }
            .map_err(|error| GpuEncoderError::Submit(format!("ProcessInput failed: {error}")))?;
        self.need_input = false;
        packets.extend(self.drain_available_output());
        Ok(packets)
    }

    /// End the stream, drain the encoder, and collect the remaining packets.
    pub(crate) fn finish(&mut self) -> Result<Vec<RawPacket>> {
        if self.finished {
            return Ok(Vec::new());
        }
        self.finished = true;
        let transform = self.transform();
        unsafe { transform.ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0) }
            .map_err(|error| GpuEncoderError::Output(format!("end of stream failed: {error}")))?;
        unsafe { transform.ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0) }
            .map_err(|error| GpuEncoderError::Output(format!("drain command failed: {error}")))?;

        let deadline = Instant::now() + DRAIN_TIMEOUT;
        let mut packets = Vec::new();
        let mut drain_complete = false;
        while Instant::now() < deadline {
            match self.next_event() {
                Ok(Some(event)) => {
                    let event_type = event_kind(&event)?;
                    if event_type == METransformDrainComplete.0 as u32 {
                        drain_complete = true;
                    } else if event_type == METransformHaveOutput.0 as u32
                        && let Some(packet) = self.process_output()?
                    {
                        packets.push(packet);
                    }
                }
                Ok(None) => std::thread::sleep(Duration::from_millis(1)),
                Err(error) => return Err(error),
            }
            if drain_complete {
                packets.extend(self.drain_available_output());
                break;
            }
        }
        if !drain_complete {
            return Err(GpuEncoderError::DrainTimeout);
        }
        unsafe {
            self.transform()
                .ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0)
        }
        .ok();
        Ok(packets)
    }

    fn wait_for_need_input(&mut self) -> Result<Vec<RawPacket>> {
        let deadline = Instant::now() + NEED_INPUT_TIMEOUT;
        let mut packets = Vec::new();
        while Instant::now() < deadline {
            match self.next_event() {
                Ok(Some(event)) => {
                    let event_type = event_kind(&event)?;
                    if event_type == METransformNeedInput.0 as u32 {
                        self.need_input = true;
                        return Ok(packets);
                    }
                    if event_type == METransformHaveOutput.0 as u32
                        && let Some(packet) = self.process_output()?
                    {
                        packets.push(packet);
                    }
                }
                Ok(None) => std::hint::spin_loop(),
                Err(error) => return Err(error),
            }
        }
        Err(GpuEncoderError::Submit(
            "encoder did not request input in time".into(),
        ))
    }

    fn drain_available_output(&mut self) -> Vec<RawPacket> {
        let mut packets = Vec::new();
        loop {
            match self.next_event() {
                Ok(Some(event)) => {
                    let Ok(event_type) = event_kind(&event) else {
                        return packets;
                    };
                    if event_type == METransformNeedInput.0 as u32 {
                        self.need_input = true;
                    } else if event_type == METransformHaveOutput.0 as u32 {
                        match self.process_output() {
                            Ok(Some(packet)) => packets.push(packet),
                            Ok(None) => {}
                            // Keep the packets collected so far even when a
                            // single retrieval fails.
                            Err(_) => return packets,
                        }
                    }
                }
                _ => return packets,
            }
        }
    }

    /// One non-blocking event fetch; `Ok(None)` when none is queued.
    fn next_event(&self) -> Result<Option<IMFMediaEvent>> {
        let events = self
            .events
            .as_ref()
            .expect("event generator is available until drop");
        match unsafe { events.GetEvent(MF_EVENT_FLAG_NO_WAIT) } {
            Ok(event) => Ok(Some(event)),
            Err(error) if error.code() == MF_E_NO_EVENTS_AVAILABLE => Ok(None),
            Err(error) => Err(GpuEncoderError::Output(format!("GetEvent failed: {error}"))),
        }
    }

    fn process_output(&mut self) -> Result<Option<RawPacket>> {
        let mut buffer = MFT_OUTPUT_DATA_BUFFER {
            dwStreamID: 0,
            pSample: ManuallyDrop::new(None),
            dwStatus: 0,
            pEvents: ManuallyDrop::new(None),
        };
        let mut status = 0u32;
        let result = unsafe {
            self.transform()
                .ProcessOutput(0, std::slice::from_mut(&mut buffer), &mut status)
        };
        match result {
            Ok(()) => {
                // SAFETY: the buffer is a stack local initialized with
                // empty ManuallyDrop slots for this single ProcessOutput
                // call; taking them releases the COM references the MFT
                // stored.
                let sample = unsafe { ManuallyDrop::take(&mut buffer.pSample) };
                if let Some(events) = unsafe { ManuallyDrop::take(&mut buffer.pEvents) } {
                    drop(events);
                }
                match sample {
                    Some(sample) => Ok(Some(sample_to_packet(&sample)?)),
                    None => Ok(None),
                }
            }
            Err(error) if error.code() == MF_E_TRANSFORM_STREAM_CHANGE => {
                // Stream-format changes were settled during negotiation;
                // treat as a skip rather than an error.
                Ok(None)
            }
            Err(error) => Err(GpuEncoderError::Output(format!(
                "ProcessOutput failed: {error}"
            ))),
        }
    }
}

impl Drop for MfH264Encoder {
    fn drop(&mut self) {
        if !self.finished
            && let Some(transform) = self.transform.as_ref()
        {
            unsafe { transform.ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0) }.ok();
        }
        // Release every MFT reference before tearing down Media Foundation;
        // hardware MFTs crash when they outlive MFShutdown.
        self.events.take();
        self.transform.take();
        self._device_manager.take();
        unsafe { MFShutdown() }.ok();
    }
}

fn sample_to_packet(sample: &IMFSample) -> Result<RawPacket> {
    let buffer: IMFMediaBuffer =
        unsafe { sample.ConvertToContiguousBuffer() }.map_err(|error| {
            GpuEncoderError::Output(format!("ConvertToContiguousBuffer failed: {error}"))
        })?;
    let mut pointer = std::ptr::null_mut();
    let mut capacity = 0u32;
    let mut length = 0u32;
    unsafe { buffer.Lock(&mut pointer, Some(&mut capacity), Some(&mut length)) }
        .map_err(|error| GpuEncoderError::Output(format!("buffer lock failed: {error}")))?;
    let data = if pointer.is_null() || length == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(pointer.cast::<u8>(), length as usize).to_vec() }
    };
    unsafe { buffer.Unlock() }.ok();

    let timestamp_hns = unsafe { sample.GetSampleTime() }.unwrap_or(0);
    let is_keyframe = unsafe {
        sample
            .GetUINT64(&MFSampleExtension_CleanPoint)
            .is_ok_and(|value| value != 0)
    };
    Ok(RawPacket {
        timestamp_hns,
        is_keyframe,
        data,
    })
}

/// Wrap a D3D11 texture as an MF input sample carrying `timestamp_hns`.
pub(crate) fn texture_to_sample(
    texture: &ID3D11Texture2D,
    timestamp_hns: i64,
    duration_hns: i64,
) -> Result<IMFSample> {
    let buffer = unsafe { MFCreateDXGISurfaceBuffer(&ID3D11Texture2D::IID, texture, 0, false) }
        .map_err(|error| {
            GpuEncoderError::Submit(format!("MFCreateDXGISurfaceBuffer failed: {error}"))
        })?;
    let sample = unsafe { MFCreateSample() }
        .map_err(|error| GpuEncoderError::Submit(format!("MFCreateSample failed: {error}")))?;
    unsafe { sample.AddBuffer(&buffer) }
        .map_err(|error| GpuEncoderError::Submit(format!("AddBuffer failed: {error}")))?;
    unsafe { sample.SetSampleTime(timestamp_hns) }
        .map_err(|error| GpuEncoderError::Submit(format!("SetSampleTime failed: {error}")))?;
    unsafe { sample.SetSampleDuration(duration_hns) }
        .map_err(|error| GpuEncoderError::Submit(format!("SetSampleDuration failed: {error}")))?;
    Ok(sample)
}
