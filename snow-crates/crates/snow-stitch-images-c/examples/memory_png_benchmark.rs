//! Actual C ABI asynchronous PNG export, compatible with the earlier revision.
//! `cargo run --release -p snow-stitch-images-c --example memory_png_benchmark -- scrolling-png-export 7`
//! Encoding RSS is sampled after the first encoded strip while the worker is running.
//! Setup's input/freeze/canvas copies can dominate external whole-process peak RSS;
//! compare the `encoding` checkpoints to isolate export's additional resident storage.

#[path = "../../../benchmark-support/memory.rs"]
mod memory;

use snow_stitch_images_c::*;
use std::ffi::{CStr, CString, c_char};
use std::mem::{MaybeUninit, align_of, size_of};
use std::ptr;
use std::time::Duration;

// These public C-header layouts let the example access fields that are private in
// the Rust implementation. Keep size/alignment checked against the exported ABI.
#[repr(C)]
struct MutableImageInfo {
    width: u32,
    height: u32,
    stride_bytes: u32,
    rgba_bytes: *mut u8,
    rgba_len: usize,
}

#[repr(C)]
struct PngExportConfig {
    output_path_utf8: *const c_char,
    compression: SnowStitchPngCompression,
    overwrite: u8,
    reserved: [u8; 31],
}

#[repr(C)]
struct ExportProgress {
    stage: SnowStitchExportStage,
    rows_written: u32,
    total_rows: u32,
    percent: f32,
}

const _: () = {
    assert!(size_of::<MutableImageInfo>() == size_of::<SnowStitchMutableImageInfo>());
    assert!(align_of::<MutableImageInfo>() == align_of::<SnowStitchMutableImageInfo>());
    assert!(size_of::<PngExportConfig>() == size_of::<SnowStitchPngExportConfig>());
    assert!(align_of::<PngExportConfig>() == align_of::<SnowStitchPngExportConfig>());
    assert!(size_of::<ExportProgress>() == size_of::<SnowStitchExportProgress>());
    assert!(align_of::<ExportProgress>() == align_of::<SnowStitchExportProgress>());
};

fn last_error() -> String {
    unsafe { CStr::from_ptr(snow_stitch_last_error_message()) }
        .to_string_lossy()
        .into_owned()
}

fn main() {
    let (scenario, samples) = memory::arguments("scrolling-png-export");
    assert_eq!(scenario, "scrolling-png-export");
    let (width, height) = (3840, 21600);
    let length = width as usize * height as usize * 4;
    let row_bytes = width as usize * 4;
    memory::phase(&scenario, "empty", width, height, 0);

    let snapshot = unsafe {
        let pool = snow_stitch_frame_pool_create(width, height, 1);
        assert!(!pool.is_null(), "{}", last_error());
        let mut frame = snow_stitch_frame_pool_acquire(pool);
        assert!(!frame.is_null(), "{}", last_error());
        let mut info = MutableImageInfo {
            width: 0,
            height: 0,
            stride_bytes: 0,
            rgba_bytes: ptr::null_mut(),
            rgba_len: 0,
        };
        assert_eq!(
            snow_stitch_frame_buffer_info(frame, (&mut info as *mut MutableImageInfo).cast(),),
            1,
            "{}",
            last_error()
        );
        assert_eq!(
            (info.width, info.height, info.rgba_len),
            (width, height, length)
        );
        assert_eq!(info.stride_bytes as usize, row_bytes);
        std::slice::from_raw_parts_mut(info.rgba_bytes, info.rgba_len).fill(0x5a);
        let mut config = MaybeUninit::<SnowStitchConfig>::uninit();
        assert_eq!(snow_stitch_config_default(config.as_mut_ptr()), 1);
        let session = snow_stitch_session_create(config.as_ptr());
        assert!(!session.is_null(), "{}", last_error());
        let mut outcome = MaybeUninit::<SnowStitchFrameOutcome>::uninit();
        assert_eq!(
            snow_stitch_session_push_owned(session, &mut frame, outcome.as_mut_ptr()),
            1,
            "{}",
            last_error()
        );
        assert!(frame.is_null());
        let snapshot = snow_stitch_session_snapshot_axis(session, 0, height);
        assert!(!snapshot.is_null(), "{}", last_error());
        snow_stitch_session_destroy(session);
        snow_stitch_frame_pool_destroy(pool);
        snapshot
    };
    memory::phase(&scenario, "snapshot", width, height, length);
    let temporary = tempfile::tempdir().unwrap();
    let output = temporary.path().join("stitched.png");
    let path = CString::new(output.to_str().unwrap()).unwrap();
    let config = PngExportConfig {
        output_path_utf8: path.as_ptr(),
        compression: SnowStitchPngCompression::Fast,
        overwrite: 1,
        reserved: [0; 31],
    };
    memory::measure(&scenario, width, height, samples, length, || unsafe {
        let task =
            snow_stitch_snapshot_export_png(snapshot, (&config as *const PngExportConfig).cast());
        assert!(!task.is_null(), "{}", last_error());
        let mut captured_encoding_rss = false;
        loop {
            let mut progress = ExportProgress {
                stage: SnowStitchExportStage::Preparing,
                rows_written: 0,
                total_rows: 0,
                percent: 0.0,
            };
            let mut has_progress = 0;
            let status = snow_stitch_export_task_poll(
                task,
                (&mut progress as *mut ExportProgress).cast(),
                &mut has_progress,
            );
            if !captured_encoding_rss
                && status == SnowStitchExportStatus::Running
                && has_progress != 0
                && matches!(progress.stage, SnowStitchExportStage::Encoding)
            {
                assert_eq!(progress.total_rows, height);
                memory::phase(&scenario, "encoding", width, height, length);
                captured_encoding_rss = true;
            }
            if status != SnowStitchExportStatus::Running {
                let error = CStr::from_ptr(snow_stitch_export_task_error_message(task))
                    .to_string_lossy()
                    .into_owned();
                snow_stitch_export_task_destroy(task);
                assert!(status == SnowStitchExportStatus::Complete, "{error}");
                break;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        assert!(
            captured_encoding_rss,
            "worker completed before encoding RSS was sampled"
        );
        memory::checksum(&std::fs::read(&output).unwrap())
    });

    // Validate all decoded pixels without allocating another complete raster.
    let mut reader = png::Decoder::new(std::io::BufReader::new(
        std::fs::File::open(&output).unwrap(),
    ))
    .read_info()
    .unwrap();
    assert_eq!((reader.info().width, reader.info().height), (width, height));
    assert_eq!(reader.info().color_type, png::ColorType::Rgba);
    assert_eq!(reader.info().bit_depth, png::BitDepth::Eight);
    for _ in 0..height {
        let row = reader.next_row().unwrap().unwrap();
        assert_eq!(row.data().len(), row_bytes);
        assert!(row.data().iter().all(|&byte| byte == 0x5a));
    }
    assert!(reader.next_row().unwrap().is_none());
    drop(reader);
    unsafe { snow_stitch_snapshot_destroy(snapshot) };
    drop(path);
    drop(temporary);
    memory::phase(&scenario, "drop", width, height, 0);
    println!(
        "{{\"record\":\"metadata\",\"scenario\":\"{scenario}\",\"operation\":\"C-ABI-async-export\",\"compression\":\"fast\",\"validation\":\"all-decoded-RGBA-pixels\",\"encoding_rss_sampled\":true}}"
    );
}
