use rapid_ocr_rs::{
    EngineConfig, LangDet, LangRec, ModelType, OcrCallOptions, OcrInput, OcrResult, OcrVersion,
    ProviderPreference, Quad, RapidOcr, RapidOcrError, ResolvedExecutionProvider,
    directml_is_available, initialize_onnx_runtime,
};
use std::{
    cell::RefCell,
    ffi::{CStr, CString, c_char},
    mem::size_of,
    panic::{AssertUnwindSafe, catch_unwind},
    path::PathBuf,
    ptr, slice,
    sync::{
        OnceLock,
        atomic::{AtomicUsize, Ordering},
    },
};

static ONNX_RUNTIME_INITIALIZATION: OnceLock<Result<(), String>> = OnceLock::new();
static DIRECTML_AVAILABILITY: OnceLock<bool> = OnceLock::new();
static LIVE_ENGINES: AtomicUsize = AtomicUsize::new(0);
static LIVE_RESULTS: AtomicUsize = AtomicUsize::new(0);

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
    pub model_store_dir_utf8: *const c_char,
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
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SnowOcrQuad {
    pub points: [f32; 8],
}

#[repr(C)]
pub struct SnowOcrLineInfoV1 {
    pub struct_size: u32,
    pub text_utf8: *const u8,
    pub text_len: usize,
    pub confidence: f32,
    pub quad: SnowOcrQuad,
}

pub struct SnowOcrEngine {
    pipeline: RapidOcr,
    backend: ResolvedExecutionProvider,
}

pub struct SnowOcrResult {
    lines: Vec<OwnedLine>,
}

struct OwnedLine {
    text: Vec<u8>,
    confidence: f32,
    quad: Quad,
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

#[derive(Clone)]
struct EngineSettings {
    intra_threads: Option<usize>,
    inter_threads: Option<usize>,
    rayon_threads: Option<usize>,
    enable_cpu_mem_arena: bool,
    use_directml: bool,
    model_store_dir: Option<PathBuf>,
}

impl EngineSettings {
    fn legacy(use_directml: bool) -> Self {
        Self {
            intra_threads: None,
            inter_threads: None,
            rayon_threads: None,
            enable_cpu_mem_arena: true,
            use_directml,
            model_store_dir: None,
        }
    }
}

fn engine_settings_from_config(
    config: &SnowOcrEngineConfigV2,
) -> std::result::Result<EngineSettings, String> {
    let nonzero = |value: u32| (value > 0).then_some(value as usize);
    let model_store_dir = if config.model_store_dir_utf8.is_null() {
        None
    } else {
        // The caller owns the pointer only for the duration of this FFI call;
        // copy it before any worker-visible state is created.
        let value = unsafe { CStr::from_ptr(config.model_store_dir_utf8) }
            .to_str()
            .map_err(|_| "OCR model store directory is not valid UTF-8".to_string())?;
        (!value.trim().is_empty()).then(|| PathBuf::from(value))
    };
    Ok(EngineSettings {
        intra_threads: nonzero(config.intra_threads),
        inter_threads: nonzero(config.inter_threads),
        rayon_threads: nonzero(config.rayon_threads),
        enable_cpu_mem_arena: config.enable_cpu_mem_arena != 0,
        use_directml: config.use_directml != 0,
        model_store_dir,
    })
}

fn create_engine_with_settings(
    settings: EngineSettings,
) -> rapid_ocr_rs::Result<(RapidOcr, ResolvedExecutionProvider)> {
    ensure_onnx_runtime().map_err(rapid_ocr_rs::RapidOcrError::Config)?;
    if settings.use_directml {
        let config = pipeline_config(&settings);
        match RapidOcr::new(config) {
            Ok(pipeline) => return Ok((pipeline, ResolvedExecutionProvider::DirectMl)),
            Err(directml_error) => {
                let mut cpu_settings = settings.clone();
                cpu_settings.use_directml = false;
                let config = pipeline_config(&cpu_settings);
                return RapidOcr::new(config)
                    .map(|pipeline| (pipeline, ResolvedExecutionProvider::Cpu))
                    .map_err(|cpu_error| {
                        RapidOcrError::Config(format!(
                            "DirectML initialization failed ({directml_error}); CPU fallback failed ({cpu_error})"
                        ))
                    });
            }
        }
    }

    let config = pipeline_config(&settings);
    RapidOcr::new(config).map(|pipeline| (pipeline, ResolvedExecutionProvider::Cpu))
}

fn pipeline_config(settings: &EngineSettings) -> EngineConfig {
    let mut config = EngineConfig::default();
    config.global.use_det = true;
    config.global.use_cls = false;
    config.global.use_rec = true;
    config.det.lang = LangDet::Multi;
    config.det.ocr_version = OcrVersion::PPocrV6;
    config.det.model_type = ModelType::Small;
    config.det.allow_download = true;
    config.det.model_store_dir = settings.model_store_dir.clone();
    config.rec.model.lang = LangRec::Ch;
    config.rec.model.ocr_version = OcrVersion::PPocrV6;
    config.rec.model.model_type = ModelType::Small;
    config.rec.model.allow_download = true;
    config.rec.model_store_dir = settings.model_store_dir.clone();
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

    config
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
    let settings = match engine_settings_from_config(config) {
        Ok(settings) => settings,
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    create_engine_handle(settings)
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
    let mut bgr = Vec::with_capacity(width * height * 3);
    for row in source.chunks(stride).take(height) {
        let pixels = &row[..row_bytes];
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

    let lines = match result {
        OcrResult::Empty => Vec::new(),
        OcrResult::Full(full) => full
            .boxes
            .into_iter()
            .zip(full.lines)
            .map(|(quad, line)| OwnedLine {
                text: line.text.into_bytes(),
                confidence: line.score,
                quad,
            })
            .collect(),
        _ => return Err("OCR pipeline returned an incomplete result".to_string()),
    };

    Ok(SnowOcrResult { lines })
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

    #[test]
    fn ffi_result_exposes_complete_lines() {
        let result = SnowOcrResult {
            lines: vec![OwnedLine {
                text: b"one complete line".to_vec(),
                confidence: 0.9,
                quad: [[1.0, 2.0], [9.0, 2.0], [9.0, 6.0], [1.0, 6.0]],
            }],
        };
        let mut line = SnowOcrLineInfoV1 {
            struct_size: size_of::<SnowOcrLineInfoV1>() as u32,
            text_utf8: ptr::null(),
            text_len: 0,
            confidence: 0.0,
            quad: SnowOcrQuad::default(),
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
            model_store_dir_utf8: ptr::null(),
        })
        .expect("valid engine config should parse");

        assert_eq!(settings.intra_threads, None);
        assert_eq!(settings.inter_threads, Some(2));
        assert_eq!(settings.rayon_threads, Some(3));
        assert!(!settings.enable_cpu_mem_arena);
        assert!(settings.use_directml);
    }

    #[test]
    fn disk_pipeline_config_enables_downloads() {
        let path = PathBuf::from("test-model-store");
        let settings = EngineSettings {
            model_store_dir: Some(path.clone()),
            ..EngineSettings::legacy(false)
        };
        let config = pipeline_config(&settings);
        assert!(config.det.allow_download);
        assert_eq!(config.det.model_store_dir, Some(path.clone()));
        assert!(config.rec.model.allow_download);
        assert_eq!(config.rec.model_store_dir, Some(path));
    }

    #[test]
    #[ignore = "downloads PP-OCRv6 models and runs ONNX inference"]
    fn disk_pipeline_recognizes_an_rgba_image() {
        let (pipeline, backend) = create_engine_with_settings(EngineSettings::legacy(false))
            .expect("disk-backed OCR pipeline should initialize");
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
        let result = recognize(&mut engine, &request).expect("disk-backed OCR pipeline should run");
        assert!(result.lines.is_empty());
    }
}
