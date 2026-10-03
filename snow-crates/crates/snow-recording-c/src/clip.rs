use super::*;
use snow_recording_export::clip::ClipSource;
use snow_recording_export::playback::{ClipExportTask, Playback};
use snow_recording_export::{ExportExecutionMode, SoftwareH264Priority, StreamingEncoderConfig};
use std::sync::Arc;

#[repr(C)]
pub struct SnowRecordingClipOptions {
    version: u32,
    struct_size: u32,
    format: u32,
    codec: u32,
    preset: u32,
    quality: u32,
    fps: u32,
    hardware: u32,
    loop_images: u32,
}
#[repr(C)]
pub struct SnowRecordingClipInfo {
    width: u32,
    height: u32,
    duration_us: u64,
    frame_count: u64,
}
#[repr(C)]
pub struct SnowRecordingClipPreview {
    revision: u64,
    position_us: u64,
    playing: u32,
    rgba: *const u8,
    byte_count: usize,
}
pub struct SnowRecordingClipImpl {
    source: ClipSource,
    playback: Playback,
}
pub struct SnowRecordingClipFrameImpl {
    _pixels: Arc<snow_memory::RasterBuffer>,
}
pub struct SnowRecordingClipExportImpl {
    task: ClipExportTask,
}

unsafe fn path(value: *const c_char) -> Result<PathBuf, String> {
    if value.is_null() {
        return Err("clip path is null".into());
    }
    let value = unsafe { CStr::from_ptr(value) }
        .to_str()
        .map_err(|e| e.to_string())?;
    if value.is_empty() {
        return Err("clip path is empty".into());
    }
    Ok(value.into())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_open(
    file: *const c_char,
    options: *const SnowRecordingClipOptions,
) -> *mut SnowRecordingClipImpl {
    let result = (|| -> Result<_, String> {
        let file = unsafe { path(file) }?;
        let config = if options.is_null() {
            None
        } else {
            let header = unsafe {
                ptr::read_unaligned(options.cast::<SnowCaptureDirectRecordingConfigHeader>())
            };
            if header.version != 1
                || (header.struct_size as usize) < std::mem::size_of::<SnowRecordingClipOptions>()
            {
                return Err("unsupported clip options".into());
            }
            let options = unsafe { &*options };
            if options.quality > 100
                || options.fps == 0
                || options.fps > 1000
                || options.hardware > 1
                || options.loop_images > 1
            {
                return Err("invalid clip settings".into());
            }
            Some(StreamingEncoderConfig {
                output_path: file.clone(),
                format: match options.format {
                    0 => ExportFormat::Mp4,
                    1 => ExportFormat::Gif,
                    2 => ExportFormat::Apng,
                    3 => ExportFormat::Webp,
                    _ => return Err("invalid clip format".into()),
                },
                codec: match options.codec {
                    0 => VideoCodec::H264,
                    1 => VideoCodec::H265,
                    _ => return Err("invalid clip codec".into()),
                },
                video: VideoEncodeConfig {
                    quality: options.quality as u8,
                    speed: match options.preset {
                        0 => VideoEncodingSpeed::UltraFast,
                        1 => VideoEncodingSpeed::VeryFast,
                        2 => VideoEncodingSpeed::Medium,
                        3 => VideoEncodingSpeed::VerySlow,
                        4 => VideoEncodingSpeed::Placebo,
                        _ => return Err("invalid clip preset".into()),
                    },
                },
                width: 1,
                height: 1,
                fps: options.fps,
                prefer_hardware_h264: options.hardware != 0,
                execution_mode: if options.hardware != 0 {
                    ExportExecutionMode::HardwarePreferred
                } else {
                    ExportExecutionMode::SoftwareOnly
                },
                software_h264_priority: SoftwareH264Priority::X264First,
                encode_threads: 0,
                audio: Vec::new(),
                loop_animated_images: options.loop_images != 0,
            })
        };
        let source = ClipSource::open(&file, config).map_err(|e| e.to_string())?;
        let playback = Playback::open(source.clone()).map_err(|e| e.to_string())?;
        Ok(SnowRecordingClipImpl { source, playback })
    })();
    match result {
        Ok(clip) => {
            clear_last_error();
            Box::into_raw(Box::new(clip))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_destroy(clip: *mut SnowRecordingClipImpl) {
    if !clip.is_null() {
        drop(unsafe { Box::from_raw(clip) });
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_info(
    clip: *const SnowRecordingClipImpl,
    info: *mut SnowRecordingClipInfo,
) -> u8 {
    let (Some(clip), Some(info)) = (unsafe { clip.as_ref() }, unsafe { info.as_mut() }) else {
        return 0;
    };
    *info = SnowRecordingClipInfo {
        width: clip.source.config.width,
        height: clip.source.config.height,
        duration_us: clip.source.duration_us,
        frame_count: clip.source.frame_count(),
    };
    1
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_boundary(
    clip: *const SnowRecordingClipImpl,
    frame: u64,
) -> u64 {
    unsafe { clip.as_ref() }.map_or(0, |clip| clip.source.boundary(frame))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_seek(
    clip: *mut SnowRecordingClipImpl,
    frame: u64,
    end: u64,
    play: u8,
) -> u64 {
    let Some(clip) = (unsafe { clip.as_ref() }) else {
        return 0;
    };
    if frame >= end || end > clip.source.frame_count() {
        return 0;
    }
    clip.playback.seek(
        clip.source.boundary(frame),
        clip.source.boundary(end),
        play != 0,
    )
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_acquire(
    clip: *const SnowRecordingClipImpl,
    preview: *mut SnowRecordingClipPreview,
) -> *mut SnowRecordingClipFrameImpl {
    let (Some(clip), Some(preview)) = (unsafe { clip.as_ref() }, unsafe { preview.as_mut() })
    else {
        return ptr::null_mut();
    };
    let frame = clip.playback.snapshot();
    *preview = SnowRecordingClipPreview {
        revision: frame.revision,
        position_us: frame.position_us,
        playing: u32::from(frame.playing),
        rgba: frame.pixels.as_ptr(),
        byte_count: frame.pixels.len(),
    };
    Box::into_raw(Box::new(SnowRecordingClipFrameImpl {
        _pixels: frame.pixels,
    }))
}
unsafe fn text(value: &str, buffer: *mut c_char, capacity: usize) -> usize {
    let value = sanitize_cstring(value);
    let bytes = value.as_bytes();
    if !buffer.is_null() && capacity > 0 {
        unsafe {
            let count = bytes.len().min(capacity - 1);
            ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast(), count);
            *buffer.add(count) = 0;
        }
    }
    bytes.len() + 1
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_frame_destroy(frame: *mut SnowRecordingClipFrameImpl) {
    if !frame.is_null() {
        drop(unsafe { Box::from_raw(frame) });
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_error(
    clip: *const SnowRecordingClipImpl,
    buffer: *mut c_char,
    capacity: usize,
) -> usize {
    unsafe {
        text(
            &clip
                .as_ref()
                .map_or_else(String::new, |clip| clip.playback.snapshot().error),
            buffer,
            capacity,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_export_start(
    clip: *const SnowRecordingClipImpl,
    first: u64,
    end: u64,
    destination: *const c_char,
) -> *mut SnowRecordingClipExportImpl {
    let result = (|| -> Result<_, String> {
        let clip = unsafe { clip.as_ref() }.ok_or("clip is null")?;
        let range = clip.source.range(first, end).map_err(|e| e.to_string())?;
        ClipExportTask::start(clip.source.clone(), range, unsafe { path(destination) }?)
            .map_err(|e| e.to_string())
    })();
    match result {
        Ok(task) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowRecordingClipExportImpl { task }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_export_poll(
    task: *const SnowRecordingClipExportImpl,
    percent: *mut f32,
) -> u32 {
    let Some(task) = (unsafe { task.as_ref() }) else {
        return 3;
    };
    let state = task.task.snapshot();
    if let Some(percent) = unsafe { percent.as_mut() } {
        *percent = state.percent;
    }
    state.state
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_export_error(
    task: *const SnowRecordingClipExportImpl,
    buffer: *mut c_char,
    capacity: usize,
) -> usize {
    unsafe {
        text(
            &task
                .as_ref()
                .map_or_else(String::new, |task| task.task.snapshot().error),
            buffer,
            capacity,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_export_cancel(task: *mut SnowRecordingClipExportImpl) {
    if let Some(task) = unsafe { task.as_ref() } {
        task.task.cancel();
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_recording_clip_export_destroy(
    task: *mut SnowRecordingClipExportImpl,
) {
    if !task.is_null() {
        drop(unsafe { Box::from_raw(task) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn owned_clip_abi_keeps_frames_and_exports_alive_after_editor_close() {
        let directory = std::env::temp_dir().join(format!(
            "snow-clip-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&directory).unwrap();
        let config = StreamingEncoderConfig {
            output_path: directory.join("source.mp4"),
            width: 32,
            height: 24,
            fps: 10,
            prefer_hardware_h264: false,
            execution_mode: ExportExecutionMode::SoftwareOnly,
            software_h264_priority: SoftwareH264Priority::X264First,
            format: ExportFormat::Mp4,
            codec: VideoCodec::H264,
            video: VideoEncodeConfig {
                quality: 90,
                speed: VideoEncodingSpeed::UltraFast,
            },
            encode_threads: 1,
            audio: Vec::new(),
            loop_animated_images: false,
        };
        let mut encoder = snow_recording_export::StreamingEncoder::create(config.clone()).unwrap();
        encoder
            .push_rgba_frame_at_pts(0, &[255; 32 * 24 * 4])
            .unwrap();
        encoder.finish_at_duration_ms(500).unwrap();
        let file = CString::new(config.output_path.to_str().unwrap()).unwrap();
        let destination = CString::new(directory.join("trim.mp4").to_str().unwrap()).unwrap();
        let mut options = SnowRecordingClipOptions {
            version: 1,
            struct_size: std::mem::size_of::<SnowRecordingClipOptions>() as u32,
            format: 0,
            codec: 0,
            preset: 0,
            quality: 90,
            fps: 10,
            hardware: 0,
            loop_images: 0,
        };
        unsafe {
            options.version = 999;
            assert!(snow_recording_clip_open(file.as_ptr(), &options).is_null());
            options.version = 1;
            let clip = snow_recording_clip_open(file.as_ptr(), &options);
            assert!(!clip.is_null());
            assert_eq!(snow_recording_clip_seek(clip, 3, 2, 1), 0);
            let revision = snow_recording_clip_seek(clip, 1, 3, 0);
            let start = std::time::Instant::now();
            let (frame, view) = loop {
                let mut view = SnowRecordingClipPreview {
                    revision: 0,
                    position_us: 0,
                    playing: 0,
                    rgba: ptr::null(),
                    byte_count: 0,
                };
                let frame = snow_recording_clip_acquire(clip, &mut view);
                if view.revision == revision && view.byte_count != 0 {
                    break (frame, view);
                }
                snow_recording_clip_frame_destroy(frame);
                assert!(start.elapsed().as_secs() < 5);
                std::thread::yield_now();
            };
            let export = snow_recording_clip_export_start(clip, 1, 3, destination.as_ptr());
            assert!(!export.is_null());
            snow_recording_clip_destroy(clip);
            assert_eq!(view.byte_count, 32 * 24 * 4);
            assert_eq!(
                *view.rgba.add(3),
                255,
                "frame lease owns its pixels independently"
            );
            snow_recording_clip_frame_destroy(frame);
            let mut percent = 0.0;
            while snow_recording_clip_export_poll(export, &mut percent) == 0 {
                assert!(start.elapsed().as_secs() < 10);
                std::thread::yield_now();
            }
            assert_eq!(snow_recording_clip_export_poll(export, &mut percent), 1);
            assert_eq!(percent, 100.0);
            snow_recording_clip_export_destroy(export);
            snow_recording_clip_destroy(ptr::null_mut());
        }
        assert!(config.output_path.exists());
        for file in [config.output_path, directory.join("trim.mp4")] {
            std::fs::remove_file(file).unwrap();
        }
        std::fs::remove_dir(directory).unwrap();
    }
}
