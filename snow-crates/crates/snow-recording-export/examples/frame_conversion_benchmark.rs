//! Compare the existing owned-RGBA conversion with the legacy frame API.
//! Release only. Arguments: report.csv [seconds per measurement, default 2].
use ffmpeg_next as ffmpeg;
use std::{
    ffi::CStr,
    fs::File,
    hint::black_box,
    io::Write,
    ptr,
    time::{Duration, Instant},
};

struct Scaler(*mut ffmpeg::ffi::SwsContext);
impl Drop for Scaler {
    fn drop(&mut self) {
        unsafe {
            ffmpeg::ffi::sws_freeContext(self.0);
        }
    }
}
impl Scaler {
    fn new(width: u32, height: u32, pixel: ffmpeg::format::Pixel, threads: usize) -> Self {
        unsafe {
            let scaler = Self(ffmpeg::ffi::sws_alloc_context());
            assert!(!scaler.0.is_null());
            let format: ffmpeg::ffi::AVPixelFormat = pixel.into();
            let rgba: ffmpeg::ffi::AVPixelFormat = ffmpeg::format::Pixel::RGBA.into();
            for (key, value) in [
                (c"srcw", i64::from(width)),
                (c"srch", i64::from(height)),
                (c"dstw", i64::from(width)),
                (c"dsth", i64::from(height)),
                (c"src_format", rgba as i64),
                (c"dst_format", format as i64),
                (
                    c"sws_flags",
                    i64::from(ffmpeg::software::scaling::Flags::BICUBIC.bits()),
                ),
                (c"threads", threads as i64),
            ] {
                scaler.option(key, value);
            }
            assert!(ffmpeg::ffi::sws_init_context(scaler.0, ptr::null_mut(), ptr::null_mut()) >= 0);
            scaler
        }
    }
    unsafe fn option(&self, key: &CStr, value: i64) {
        assert!(unsafe { ffmpeg::ffi::av_opt_set_int(self.0.cast(), key.as_ptr(), value, 0) } >= 0);
    }
    fn owned(&self, width: u32, height: u32, pixels: &[u8], output: &mut ffmpeg::frame::Video) {
        let source = [pixels.as_ptr(), ptr::null(), ptr::null(), ptr::null()];
        let stride = [width as i32 * 4, 0, 0, 0];
        unsafe {
            assert_eq!(
                ffmpeg::ffi::sws_scale(
                    self.0,
                    source.as_ptr(),
                    stride.as_ptr(),
                    0,
                    height as i32,
                    (*output.as_mut_ptr()).data.as_ptr(),
                    (*output.as_mut_ptr()).linesize.as_ptr()
                ),
                height as i32
            );
        }
    }
    fn frame(&self, input: &ffmpeg::frame::Video, output: &mut ffmpeg::frame::Video) {
        assert!(
            unsafe { ffmpeg::ffi::sws_scale_frame(self.0, output.as_mut_ptr(), input.as_ptr()) }
                >= 0
        );
    }
}
fn copy_input(pixels: &[u8], frame: &mut ffmpeg::frame::Video) {
    let row_bytes = frame.width() as usize * 4;
    let stride = frame.stride(0);
    let height = frame.height() as usize;
    for (source, destination) in pixels
        .chunks_exact(row_bytes)
        .take(height)
        .zip(frame.data_mut(0).chunks_mut(stride))
    {
        destination[..row_bytes].copy_from_slice(source);
    }
}
fn packed(frame: &ffmpeg::frame::Video) -> Vec<u8> {
    unsafe {
        let size = ffmpeg::ffi::av_image_get_buffer_size(
            frame.format().into(),
            frame.width() as i32,
            frame.height() as i32,
            1,
        );
        assert!(size > 0);
        let mut bytes = vec![0; size as usize];
        assert_eq!(
            ffmpeg::ffi::av_image_copy_to_buffer(
                bytes.as_mut_ptr(),
                size,
                (*frame.as_ptr()).data.as_ptr().cast(),
                (*frame.as_ptr()).linesize.as_ptr(),
                frame.format().into(),
                frame.width() as i32,
                frame.height() as i32,
                1
            ),
            size
        );
        bytes
    }
}
fn pixels(width: u32, height: u32) -> Vec<u8> {
    let mut pixels: Vec<_> = (0..width as usize * height as usize * 4)
        .map(|index| ((index * 37 + index / 41) % 251) as u8)
        .collect();
    pixels.resize(
        pixels.len() + ffmpeg::ffi::AV_INPUT_BUFFER_PADDING_SIZE as usize,
        0,
    );
    pixels
}
fn main() -> Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("use windows-msvc-performance Release".into());
    }
    ffmpeg::init()?;
    let args: Vec<_> = std::env::args().collect();
    let mut report = File::create(args.get(1).ok_or("report.csv required")?)?;
    let seconds = args
        .get(2)
        .map(|value| value.parse())
        .transpose()?
        .unwrap_or(2);
    writeln!(
        report,
        "kind,width,height,format,threads,pair,variant,equal,iterations,ns_per_frame"
    )?;
    for (width, height) in [(1, 1), (17, 19), (63, 127), (128, 64), (1920, 1080)] {
        let pixels = pixels(width, height);
        let mut input = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
        copy_input(&pixels, &mut input);
        for format in [
            ffmpeg::format::Pixel::YUV420P,
            ffmpeg::format::Pixel::NV12,
            ffmpeg::format::Pixel::YUV422P,
            ffmpeg::format::Pixel::YUVA420P,
            ffmpeg::format::Pixel::RGB8,
            ffmpeg::format::Pixel::RGB24,
            ffmpeg::format::Pixel::RGBA,
            ffmpeg::format::Pixel::BGRA,
        ] {
            let reference = Scaler::new(width, height, format, 1);
            let mut expected = ffmpeg::frame::Video::new(format, width, height);
            reference.owned(width, height, &pixels, &mut expected);
            let expected = packed(&expected);
            for threads in [1, 2, 4] {
                let scaler = Scaler::new(width, height, format, threads);
                let mut output = ffmpeg::frame::Video::new(format, width, height);
                scaler.frame(&input, &mut output);
                let equal = packed(&output) == expected;
                writeln!(
                    report,
                    "correctness,{width},{height},{format:?},{threads},0,frame,{equal},0,0"
                )?;
            }
        }
    }
    report.flush()?;
    let (width, height) = (1920, 1080);
    let pixels = pixels(width, height);
    let mut input = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
    copy_input(&pixels, &mut input);
    for format in [ffmpeg::format::Pixel::YUV420P, ffmpeg::format::Pixel::NV12] {
        let reference = Scaler::new(width, height, format, 1);
        for threads in [1, 2, 4] {
            let candidate = Scaler::new(width, height, format, threads);
            let mut output = ffmpeg::frame::Video::new(format, width, height);
            for pair in 0..=5 {
                for variant in if pair % 2 == 0 {
                    ["owned", "frame-copy", "frame-owned"]
                } else {
                    ["frame-owned", "frame-copy", "owned"]
                } {
                    let started = Instant::now();
                    let mut iterations = 0;
                    while started.elapsed()
                        < Duration::from_secs(if pair == 0 { 5 } else { seconds })
                    {
                        if variant == "owned" {
                            reference.owned(width, height, black_box(&pixels), &mut output);
                        } else {
                            // Include the copy needed to provide reference-counted source storage.
                            if variant == "frame-copy" {
                                copy_input(black_box(&pixels), &mut input);
                            }
                            candidate.frame(&input, &mut output);
                        }
                        black_box(&output);
                        iterations += 1;
                    }
                    if pair != 0 {
                        writeln!(
                            report,
                            "timing,{width},{height},{format:?},{threads},{pair},{variant},true,{iterations},{}",
                            started.elapsed().as_nanos() / iterations
                        )?;
                    }
                }
            }
            report.flush()?;
        }
    }
    Ok(())
}
