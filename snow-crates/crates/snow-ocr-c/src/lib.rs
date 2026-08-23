mod fill;

use rapid_ocr_rs::{
    DictionarySource, EngineConfig, LangDet, LangRec, ModelSource, ModelType, OcrCallOptions,
    OcrInput, OcrResult, OcrVersion, PipelineSources, ProviderPreference, Quad, RapidOcr,
    RapidOcrError, ResolvedExecutionProvider, directml_is_available, initialize_onnx_runtime,
};
use std::{
    cell::RefCell,
    ffi::{CString, c_char},
    mem::size_of,
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
    sync::{
        OnceLock,
        atomic::{AtomicUsize, Ordering},
    },
};

const DETECTOR_BYTES: &[u8] = include_bytes!("../assets/ppocrv6-small/PP-OCRv6_det_small.onnx");
const RECOGNIZER_BYTES: &[u8] = include_bytes!("../assets/ppocrv6-small/PP-OCRv6_rec_small.onnx");
const DICTIONARY_TEXT: &str = include_str!("../assets/ppocrv6-small/ppocrv6_dict.txt");
static ONNX_RUNTIME_INITIALIZATION: OnceLock<Result<(), String>> = OnceLock::new();
static DIRECTML_AVAILABILITY: OnceLock<bool> = OnceLock::new();
static LIVE_ENGINES: AtomicUsize = AtomicUsize::new(0);
static LIVE_RESULTS: AtomicUsize = AtomicUsize::new(0);
static LIVE_OWNED_IMAGES: AtomicUsize = AtomicUsize::new(0);

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").expect("empty C string"));
}

#[repr(C)]
pub struct SnowOcrRequestV1 {
    pub struct_size: u32,
    pub width: u32,
    pub height: u32,
    pub stride_bytes: u32,
    pub rgba_bytes: *const u8,
    pub rgba_len: usize,
}

#[repr(C)]
pub struct SnowOcrEngineConfigV2 {
    pub struct_size: u32,
    pub intra_threads: u32,
    pub inter_threads: u32,
    pub rayon_threads: u32,
    pub enable_cpu_mem_arena: u8,
    pub use_directml: u8,
    pub reserved: [u8; 2],
}

#[repr(C)]
pub struct SnowOcrRuntimeInfoV1 {
    pub struct_size: u32,
    pub physical_core_count: u32,
}

#[repr(C)]
pub struct SnowOcrResourceCountsV1 {
    pub struct_size: u32,
    pub engines: usize,
    pub results: usize,
    pub owned_images: usize,
}

#[repr(C)]
pub struct SnowOcrImageInfoV1 {
    pub struct_size: u32,
    pub width: u32,
    pub height: u32,
    pub stride_bytes: u32,
    pub rgba_bytes: *const u8,
    pub rgba_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SnowOcrQuad {
    pub points: [f32; 8],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SnowOcrColor {
    pub red: u8,
    pub green: u8,
    pub blue: u8,
    pub alpha: u8,
}

#[repr(C)]
pub struct SnowOcrLineInfoV1 {
    pub struct_size: u32,
    pub text_utf8: *const u8,
    pub text_len: usize,
    pub confidence: f32,
    pub quad: SnowOcrQuad,
    pub foreground: SnowOcrColor,
}

pub struct SnowOcrEngine {
    pipeline: RapidOcr,
    backend: ResolvedExecutionProvider,
}

pub struct SnowOcrResult {
    width: u32,
    height: u32,
    rgba: Vec<u8>,
    lines: Vec<OwnedLine>,
}

pub struct SnowOcrOwnedImage {
    width: u32,
    height: u32,
    rgba: Vec<u8>,
}

struct OwnedLine {
    text: Vec<u8>,
    confidence: f32,
    quad: Quad,
    foreground: [u8; 4],
}

fn set_last_error(error: impl ToString) {
    let text = error.to_string().replace('\0', " ");
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = CString::new(text).expect("NUL bytes were removed");
    });
}

fn clear_last_error() {
    set_last_error("");
}

#[derive(Clone, Copy)]
struct EngineSettings {
    intra_threads: Option<usize>,
    inter_threads: Option<usize>,
    rayon_threads: Option<usize>,
    enable_cpu_mem_arena: bool,
    use_directml: bool,
}

impl EngineSettings {
    fn legacy(use_directml: bool) -> Self {
        Self {
            intra_threads: None,
            inter_threads: None,
            rayon_threads: None,
            enable_cpu_mem_arena: true,
            use_directml,
        }
    }
}

fn engine_settings_from_config(config: &SnowOcrEngineConfigV2) -> EngineSettings {
    let nonzero = |value: u32| (value > 0).then_some(value as usize);
    EngineSettings {
        intra_threads: nonzero(config.intra_threads),
        inter_threads: nonzero(config.inter_threads),
        rayon_threads: nonzero(config.rayon_threads),
        enable_cpu_mem_arena: config.enable_cpu_mem_arena != 0,
        use_directml: config.use_directml != 0,
    }
}

fn create_engine_with_settings(
    settings: EngineSettings,
) -> rapid_ocr_rs::Result<(RapidOcr, ResolvedExecutionProvider)> {
    ensure_onnx_runtime().map_err(rapid_ocr_rs::RapidOcrError::Config)?;
    if settings.use_directml {
        let (config, sources) = embedded_pipeline(settings);
        match RapidOcr::new_with_sources(config, sources) {
            Ok(pipeline) => return Ok((pipeline, ResolvedExecutionProvider::DirectMl)),
            Err(directml_error) => {
                let mut cpu_settings = settings;
                cpu_settings.use_directml = false;
                let (config, sources) = embedded_pipeline(cpu_settings);
                return RapidOcr::new_with_sources(config, sources)
                    .map(|pipeline| (pipeline, ResolvedExecutionProvider::Cpu))
                    .map_err(|cpu_error| {
                        RapidOcrError::Config(format!(
                            "DirectML initialization failed ({directml_error}); CPU fallback failed ({cpu_error})"
                        ))
                    });
            }
        }
    }

    let (config, sources) = embedded_pipeline(settings);
    RapidOcr::new_with_sources(config, sources)
        .map(|pipeline| (pipeline, ResolvedExecutionProvider::Cpu))
}

fn embedded_pipeline(settings: EngineSettings) -> (EngineConfig, PipelineSources<'static>) {
    let mut config = EngineConfig::default();
    config.global.use_det = true;
    config.global.use_cls = false;
    config.global.use_rec = true;
    config.det.lang = LangDet::Multi;
    config.det.ocr_version = OcrVersion::PPocrV6;
    config.det.model_type = ModelType::Small;
    config.det.allow_download = false;
    config.rec.model.lang = LangRec::Ch;
    config.rec.model.ocr_version = OcrVersion::PPocrV6;
    config.rec.model.model_type = ModelType::Small;
    config.rec.model.allow_download = false;
    for runtime in [
        &mut config.det.runtime,
        &mut config.cls.runtime,
        &mut config.rec.runtime,
    ] {
        runtime.intra_threads = settings.intra_threads;
        runtime.inter_threads = settings.inter_threads;
        runtime.rayon_threads = settings.rayon_threads;
        runtime.enable_cpu_mem_arena = settings.enable_cpu_mem_arena;
        if settings.intra_threads.is_some()
            || settings.inter_threads.is_some()
            || settings.rayon_threads.is_some()
        {
            runtime.auto_tune_threads = false;
        }
    }
    if settings.use_directml {
        let provider = ProviderPreference::DirectMl { device_id: 0 };
        for runtime in [&mut config.det.runtime, &mut config.rec.runtime] {
            runtime.intra_threads = Some(1);
            runtime.inter_threads = Some(1);
            runtime.auto_tune_threads = false;
            runtime.provider_preference = provider;
            runtime.fail_if_provider_unavailable = true;
        }
    } else {
        // The C ABI's flag is an explicit provider choice. Preserve its legacy
        // meaning even though generic Rust defaults use stage-aware Auto.
        config.det.runtime.provider_preference = ProviderPreference::Cpu;
        config.rec.runtime.provider_preference = ProviderPreference::Cpu;
    }

    let sources = PipelineSources {
        det: Some(ModelSource::Memory {
            name: "embedded:PP-OCRv6_det_small.onnx",
            bytes: DETECTOR_BYTES,
        }),
        cls: None,
        rec: Some(ModelSource::Memory {
            name: "embedded:PP-OCRv6_rec_small.onnx",
            bytes: RECOGNIZER_BYTES,
        }),
        rec_dictionary: Some(DictionarySource::Memory {
            name: "embedded:ppocrv6_dict.txt",
            text: DICTIONARY_TEXT,
        }),
    };
    (config, sources)
}

fn ensure_onnx_runtime() -> Result<(), String> {
    ONNX_RUNTIME_INITIALIZATION
        .get_or_init(|| {
            initialize_onnx_runtime()
                .map_err(|error| format!("unable to initialize ONNX Runtime: {error}"))
        })
        .clone()
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_ocr_engine_create() -> *mut SnowOcrEngine {
    snow_ocr_engine_create_with_directml(0)
}

fn create_engine_handle(settings: EngineSettings) -> *mut SnowOcrEngine {
    match catch_unwind(AssertUnwindSafe(|| create_engine_with_settings(settings))) {
        Ok(Ok((pipeline, backend))) => {
            clear_last_error();
            LIVE_ENGINES.fetch_add(1, Ordering::Relaxed);
            Box::into_raw(Box::new(SnowOcrEngine { pipeline, backend }))
        }
        Ok(Err(error)) => {
            set_last_error(error);
            ptr::null_mut()
        }
        Err(_) => {
            set_last_error("OCR engine creation panicked");
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_ocr_engine_create_with_directml(enabled: u8) -> *mut SnowOcrEngine {
    create_engine_handle(EngineSettings::legacy(enabled != 0))
}

#[unsafe(no_mangle)]
/// # Safety
/// `config` must be null or point to a readable `SnowOcrEngineConfigV2`.
pub unsafe extern "C" fn snow_ocr_engine_create_with_config_v2(
    config: *const SnowOcrEngineConfigV2,
) -> *mut SnowOcrEngine {
    let Some(config) = (unsafe { config.as_ref() }) else {
        set_last_error("OCR engine config is null");
        return ptr::null_mut();
    };
    if config.struct_size as usize != size_of::<SnowOcrEngineConfigV2>() {
        set_last_error("OCR engine config has an incompatible struct size");
        return ptr::null_mut();
    }
    create_engine_handle(engine_settings_from_config(config))
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_ocr_directml_is_available() -> u8 {
    *DIRECTML_AVAILABILITY.get_or_init(|| {
        catch_unwind(AssertUnwindSafe(|| {
            ensure_onnx_runtime().is_ok() && directml_is_available()
        }))
        .unwrap_or(false)
    }) as u8
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be null or a live handle returned by an engine creation function.
pub unsafe extern "C" fn snow_ocr_engine_uses_directml(engine: *const SnowOcrEngine) -> u8 {
    unsafe { engine.as_ref() }
        .is_some_and(|engine| engine.backend == ResolvedExecutionProvider::DirectMl) as u8
}

#[unsafe(no_mangle)]
/// # Safety
/// `out_info` must point to a writable `SnowOcrRuntimeInfoV1`.
pub unsafe extern "C" fn snow_ocr_runtime_info_v1(out_info: *mut SnowOcrRuntimeInfoV1) -> u8 {
    let Some(out_info) = (unsafe { out_info.as_mut() }) else {
        set_last_error("OCR runtime info output is null");
        return 0;
    };
    if out_info.struct_size as usize != size_of::<SnowOcrRuntimeInfoV1>() {
        set_last_error("OCR runtime info output has an incompatible struct size");
        return 0;
    }
    *out_info = SnowOcrRuntimeInfoV1 {
        struct_size: size_of::<SnowOcrRuntimeInfoV1>() as u32,
        physical_core_count: num_cpus::get_physical().max(1) as u32,
    };
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// `out_counts` must point to a writable `SnowOcrResourceCountsV1`.
pub unsafe extern "C" fn snow_ocr_resource_counts_v1(
    out_counts: *mut SnowOcrResourceCountsV1,
) -> u8 {
    let Some(out_counts) = (unsafe { out_counts.as_mut() }) else {
        set_last_error("OCR resource counts output is null");
        return 0;
    };
    if out_counts.struct_size as usize != size_of::<SnowOcrResourceCountsV1>() {
        set_last_error("OCR resource counts output has an incompatible struct size");
        return 0;
    }
    *out_counts = SnowOcrResourceCountsV1 {
        struct_size: size_of::<SnowOcrResourceCountsV1>() as u32,
        engines: LIVE_ENGINES.load(Ordering::Relaxed),
        results: LIVE_RESULTS.load(Ordering::Relaxed),
        owned_images: LIVE_OWNED_IMAGES.load(Ordering::Relaxed),
    };
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be null or a live handle returned by `snow_ocr_engine_create`.
pub unsafe extern "C" fn snow_ocr_engine_destroy(engine: *mut SnowOcrEngine) {
    if !engine.is_null() {
        drop(unsafe { Box::from_raw(engine) });
        LIVE_ENGINES.fetch_sub(1, Ordering::Relaxed);
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// The request and its pixel buffer must remain readable for the duration of
/// the call. The engine must be live and exclusively accessed.
pub unsafe extern "C" fn snow_ocr_engine_recognize_rgba(
    engine: *mut SnowOcrEngine,
    request: *const SnowOcrRequestV1,
) -> *mut SnowOcrResult {
    let Some(engine) = (unsafe { engine.as_mut() }) else {
        set_last_error("OCR engine is null");
        return ptr::null_mut();
    };
    let Some(request) = (unsafe { request.as_ref() }) else {
        set_last_error("OCR request is null");
        return ptr::null_mut();
    };
    if request.struct_size as usize != size_of::<SnowOcrRequestV1>() {
        set_last_error("OCR request has an incompatible struct size");
        return ptr::null_mut();
    }

    match catch_unwind(AssertUnwindSafe(|| recognize(engine, request))) {
        Ok(Ok(result)) => {
            clear_last_error();
            LIVE_RESULTS.fetch_add(1, Ordering::Relaxed);
            Box::into_raw(Box::new(result))
        }
        Ok(Err(error)) => {
            set_last_error(error);
            ptr::null_mut()
        }
        Err(_) => {
            set_last_error("OCR recognition panicked");
            ptr::null_mut()
        }
    }
}

fn recognize(
    engine: &mut SnowOcrEngine,
    request: &SnowOcrRequestV1,
) -> Result<SnowOcrResult, String> {
    let width = request.width as usize;
    let height = request.height as usize;
    let stride = request.stride_bytes as usize;
    let row_bytes = width.checked_mul(4).ok_or("OCR row size overflow")?;
    let required = stride
        .checked_mul(height)
        .ok_or("OCR image buffer size overflow")?;
    if width == 0 || height == 0 || stride < row_bytes || request.rgba_bytes.is_null() {
        return Err("OCR image dimensions, stride, or pixel pointer are invalid".to_string());
    }
    if request.rgba_len < required {
        return Err("OCR image buffer is shorter than its declared dimensions".to_string());
    }

    let source = unsafe { slice::from_raw_parts(request.rgba_bytes, required) };
    let mut rgba = Vec::with_capacity(row_bytes * height);
    let mut bgr = Vec::with_capacity(width * height * 3);
    for row in source.chunks(stride).take(height) {
        let pixels = &row[..row_bytes];
        rgba.extend_from_slice(pixels);
        for pixel in pixels.chunks_exact(4) {
            bgr.extend_from_slice(&[pixel[2], pixel[1], pixel[0]]);
        }
    }
    let result = engine
        .pipeline
        .run(
            OcrInput::BgrU8 {
                width,
                height,
                data: bgr,
            },
            OcrCallOptions {
                use_det: Some(true),
                use_cls: Some(false),
                use_rec: Some(true),
                ..OcrCallOptions::default()
            },
        )
        .and_then(OcrResult::try_from)
        .map_err(|error| error.to_string())?;

    let mut lines = match result {
        OcrResult::Empty => Vec::new(),
        OcrResult::Full(full) => full
            .boxes
            .into_iter()
            .zip(full.lines)
            .map(|(quad, line)| OwnedLine {
                text: line.text.into_bytes(),
                confidence: line.score,
                quad,
                foreground: [0, 0, 0, 255],
            })
            .collect(),
        _ => return Err("OCR pipeline returned an incomplete result".to_string()),
    };

    let line_quads = lines.iter().map(|line| line.quad).collect::<Vec<_>>();
    let foregrounds = fill::white_blur_fill(&mut rgba, width, height, &line_quads);
    for (line, foreground) in lines.iter_mut().zip(foregrounds) {
        line.foreground = foreground;
    }

    Ok(SnowOcrResult {
        width: request.width,
        height: request.height,
        rgba,
        lines,
    })
}

fn ffi_quad(quad: Quad) -> SnowOcrQuad {
    SnowOcrQuad {
        points: [
            quad[0][0], quad[0][1], quad[1][0], quad[1][1], quad[2][0], quad[2][1], quad[3][0],
            quad[3][1],
        ],
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `result` must be null or a live result returned by the recognize function.
pub unsafe extern "C" fn snow_ocr_result_destroy(result: *mut SnowOcrResult) {
    if !result.is_null() {
        drop(unsafe { Box::from_raw(result) });
        LIVE_RESULTS.fetch_sub(1, Ordering::Relaxed);
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `result` must be a live result. The image may be taken at most once.
pub unsafe extern "C" fn snow_ocr_result_take_image(
    result: *mut SnowOcrResult,
) -> *mut SnowOcrOwnedImage {
    let Some(result) = (unsafe { result.as_mut() }) else {
        set_last_error("OCR result is null");
        return ptr::null_mut();
    };
    if result.rgba.is_empty() {
        set_last_error("OCR result image has already been taken");
        return ptr::null_mut();
    }
    let image = SnowOcrOwnedImage {
        width: result.width,
        height: result.height,
        rgba: std::mem::take(&mut result.rgba),
    };
    clear_last_error();
    LIVE_OWNED_IMAGES.fetch_add(1, Ordering::Relaxed);
    Box::into_raw(Box::new(image))
}

#[unsafe(no_mangle)]
/// # Safety
/// `image` must be null or a live handle returned by `snow_ocr_result_take_image`.
pub unsafe extern "C" fn snow_ocr_owned_image_destroy(image: *mut SnowOcrOwnedImage) {
    if !image.is_null() {
        drop(unsafe { Box::from_raw(image) });
        LIVE_OWNED_IMAGES.fetch_sub(1, Ordering::Relaxed);
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `image` must be live and `out_image` must be writable.
pub unsafe extern "C" fn snow_ocr_owned_image_info(
    image: *const SnowOcrOwnedImage,
    out_image: *mut SnowOcrImageInfoV1,
) -> u8 {
    let (Some(image), Some(out_image)) = (unsafe { image.as_ref() }, unsafe { out_image.as_mut() })
    else {
        set_last_error("OCR owned image or output is null");
        return 0;
    };
    if out_image.struct_size as usize != size_of::<SnowOcrImageInfoV1>() {
        set_last_error("OCR image output has an incompatible struct size");
        return 0;
    }
    *out_image = SnowOcrImageInfoV1 {
        struct_size: size_of::<SnowOcrImageInfoV1>() as u32,
        width: image.width,
        height: image.height,
        stride_bytes: image.width.saturating_mul(4),
        rgba_bytes: image.rgba.as_ptr(),
        rgba_len: image.rgba.len(),
    };
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// The result must be live and `out_image` must be writable.
pub unsafe extern "C" fn snow_ocr_result_image(
    result: *const SnowOcrResult,
    out_image: *mut SnowOcrImageInfoV1,
) -> u8 {
    let (Some(result), Some(out_image)) = (
        (unsafe { result.as_ref() }),
        (unsafe { out_image.as_mut() }),
    ) else {
        set_last_error("OCR result or image output is null");
        return 0;
    };
    if out_image.struct_size as usize != size_of::<SnowOcrImageInfoV1>() {
        set_last_error("OCR image output has an incompatible struct size");
        return 0;
    }
    *out_image = SnowOcrImageInfoV1 {
        struct_size: size_of::<SnowOcrImageInfoV1>() as u32,
        width: result.width,
        height: result.height,
        stride_bytes: result.width * 4,
        rgba_bytes: result.rgba.as_ptr(),
        rgba_len: result.rgba.len(),
    };
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// `result` must be null or a live result.
pub unsafe extern "C" fn snow_ocr_result_line_count(result: *const SnowOcrResult) -> usize {
    unsafe { result.as_ref() }.map_or(0, |result| result.lines.len())
}

#[unsafe(no_mangle)]
/// # Safety
/// The result must be live and `out_line` must be writable.
pub unsafe extern "C" fn snow_ocr_result_line(
    result: *const SnowOcrResult,
    line_index: usize,
    out_line: *mut SnowOcrLineInfoV1,
) -> u8 {
    let (Some(result), Some(out_line)) =
        ((unsafe { result.as_ref() }), (unsafe { out_line.as_mut() }))
    else {
        set_last_error("OCR result or line output is null");
        return 0;
    };
    if out_line.struct_size as usize != size_of::<SnowOcrLineInfoV1>() {
        set_last_error("OCR line output has an incompatible struct size");
        return 0;
    }
    let Some(line) = result.lines.get(line_index) else {
        set_last_error("OCR line index is out of range");
        return 0;
    };
    *out_line = SnowOcrLineInfoV1 {
        struct_size: size_of::<SnowOcrLineInfoV1>() as u32,
        text_utf8: line.text.as_ptr(),
        text_len: line.text.len(),
        confidence: line.confidence,
        quad: ffi_quad(line.quad),
        foreground: SnowOcrColor {
            red: line.foreground[0],
            green: line.foreground[1],
            blue: line.foreground[2],
            alpha: line.foreground[3],
        },
    };
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_ocr_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::{Digest, Sha256};

    #[test]
    fn embedded_assets_match_pinned_hashes() {
        assert_eq!(DETECTOR_BYTES.len(), 9_929_594);
        assert_eq!(RECOGNIZER_BYTES.len(), 21_234_383);
        assert_eq!(DICTIONARY_TEXT.len(), 74_947);
        assert_eq!(
            format!("{:x}", Sha256::digest(DETECTOR_BYTES)),
            "090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f"
        );
        assert_eq!(
            format!("{:x}", Sha256::digest(RECOGNIZER_BYTES)),
            "6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884"
        );
        assert_eq!(
            format!("{:x}", Sha256::digest(DICTIONARY_TEXT.as_bytes())),
            "b5f2bfe2bdd9448429e3e82b51c789775d9b42f2403d082b00662eb77e401c5d"
        );
    }

    #[test]
    fn ffi_result_exposes_complete_lines() {
        let result = SnowOcrResult {
            width: 1,
            height: 1,
            rgba: vec![0, 0, 0, 255],
            lines: vec![OwnedLine {
                text: b"one complete line".to_vec(),
                confidence: 0.9,
                quad: [[1.0, 2.0], [9.0, 2.0], [9.0, 6.0], [1.0, 6.0]],
                foreground: [12, 34, 56, 255],
            }],
        };
        let mut line = SnowOcrLineInfoV1 {
            struct_size: size_of::<SnowOcrLineInfoV1>() as u32,
            text_utf8: ptr::null(),
            text_len: 0,
            confidence: 0.0,
            quad: SnowOcrQuad::default(),
            foreground: SnowOcrColor::default(),
        };

        assert_eq!(unsafe { snow_ocr_result_line_count(&result) }, 1);
        assert_eq!(unsafe { snow_ocr_result_line(&result, 0, &mut line) }, 1);
        assert_eq!(
            unsafe { slice::from_raw_parts(line.text_utf8, line.text_len) },
            b"one complete line"
        );
        assert_eq!(line.quad.points, [1.0, 2.0, 9.0, 2.0, 9.0, 6.0, 1.0, 6.0]);
    }

    #[test]
    fn ffi_runtime_info_validates_the_versioned_output() {
        let mut invalid = SnowOcrRuntimeInfoV1 {
            struct_size: 0,
            physical_core_count: 0,
        };
        assert_eq!(unsafe { snow_ocr_runtime_info_v1(&mut invalid) }, 0);
        assert_eq!(unsafe { snow_ocr_runtime_info_v1(ptr::null_mut()) }, 0);

        let mut info = SnowOcrRuntimeInfoV1 {
            struct_size: size_of::<SnowOcrRuntimeInfoV1>() as u32,
            physical_core_count: 0,
        };
        assert_eq!(unsafe { snow_ocr_runtime_info_v1(&mut info) }, 1);
        assert_eq!(info.struct_size as usize, size_of::<SnowOcrRuntimeInfoV1>());
        assert!(info.physical_core_count >= 1);
    }

    #[test]
    fn ffi_engine_config_maps_zero_threads_to_automatic_values() {
        let settings = engine_settings_from_config(&SnowOcrEngineConfigV2 {
            struct_size: size_of::<SnowOcrEngineConfigV2>() as u32,
            intra_threads: 0,
            inter_threads: 2,
            rayon_threads: 3,
            enable_cpu_mem_arena: 0,
            use_directml: 1,
            reserved: [0; 2],
        });

        assert_eq!(settings.intra_threads, None);
        assert_eq!(settings.inter_threads, Some(2));
        assert_eq!(settings.rayon_threads, Some(3));
        assert!(!settings.enable_cpu_mem_arena);
        assert!(settings.use_directml);
    }

    #[test]
    fn ffi_owned_image_transfer_has_deterministic_ownership() {
        let baseline = LIVE_OWNED_IMAGES.load(Ordering::Relaxed);
        let mut result = SnowOcrResult {
            width: 2,
            height: 1,
            rgba: vec![1, 2, 3, 4, 5, 6, 7, 8],
            lines: Vec::new(),
        };

        let image = unsafe { snow_ocr_result_take_image(&mut result) };
        assert!(!image.is_null());
        assert!(result.rgba.is_empty());
        assert_eq!(LIVE_OWNED_IMAGES.load(Ordering::Relaxed), baseline + 1);

        let mut info = SnowOcrImageInfoV1 {
            struct_size: size_of::<SnowOcrImageInfoV1>() as u32,
            width: 0,
            height: 0,
            stride_bytes: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
        };
        assert_eq!(unsafe { snow_ocr_owned_image_info(image, &mut info) }, 1);
        assert_eq!((info.width, info.height, info.stride_bytes), (2, 1, 8));
        assert_eq!(
            unsafe { slice::from_raw_parts(info.rgba_bytes, info.rgba_len) },
            &[1, 2, 3, 4, 5, 6, 7, 8]
        );
        assert!(unsafe { snow_ocr_result_take_image(&mut result) }.is_null());

        unsafe { snow_ocr_owned_image_destroy(image) };
        assert_eq!(LIVE_OWNED_IMAGES.load(Ordering::Relaxed), baseline);
    }

    #[test]
    fn embedded_pipeline_recognizes_an_rgba_image() {
        let (pipeline, backend) = create_engine_with_settings(EngineSettings::legacy(false))
            .expect("embedded OCR pipeline should initialize");
        let mut engine = SnowOcrEngine { pipeline, backend };
        let rgba = vec![255_u8; 64 * 64 * 4];
        let request = SnowOcrRequestV1 {
            struct_size: size_of::<SnowOcrRequestV1>() as u32,
            width: 64,
            height: 64,
            stride_bytes: 64 * 4,
            rgba_bytes: rgba.as_ptr(),
            rgba_len: rgba.len(),
        };
        let result = recognize(&mut engine, &request).expect("embedded OCR pipeline should run");
        assert_eq!((result.width, result.height), (64, 64));
        assert_eq!(result.rgba.len(), rgba.len());
    }
}
