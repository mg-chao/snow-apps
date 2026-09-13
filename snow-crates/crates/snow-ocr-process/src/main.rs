mod buffer;
use buffer::{SLOT_HEADER, SharedImage};
mod protocol;
mod session;
mod worker;
use worker::{Work, WorkResult, worker_loop};

use memmap2::Mmap;
use protocol::{Decoder, Kind, put_f32, put_string, put_u8, put_u32, read_frame, write_frame};
use rapid_ocr_rs::{
    DictionarySource, EngineConfig, LangDet, LangRec, ModelSource, ModelType, OcrCallOptions,
    OcrInput, OcrResult, PipelineSources, ProviderPreference, RapidOcr, ResolvedExecutionProvider,
    directml_is_available, initialize_onnx_runtime,
};
use serde::{Deserialize, Serialize};
use std::{
    fs::{self, File},
    io::{self, BufReader, BufWriter, Write},
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc::{self},
    },
    thread::{self},
    time::{Instant, SystemTime, UNIX_EPOCH},
};

#[derive(Clone)]
struct Config {
    directml: bool,
    directml_enabled: Arc<AtomicBool>,
    directml_cache: Arc<DirectMlCapabilityCache>,
    detector_model: PathBuf,
    recognizer_model: PathBuf,
    dictionary: PathBuf,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct DirectMlCacheRecord {
    schema: u32,
    key: String,
    available: bool,
}

struct DirectMlCapabilityCache {
    path: PathBuf,
    key: String,
}

impl DirectMlCapabilityCache {
    fn new(state_dir: Option<&Path>) -> Self {
        let path = state_dir
            .map(|dir| dir.join("directml-capability.json"))
            .unwrap_or_else(|| std::env::temp_dir().join("snow-shot-directml-capability.json"));
        let key = capability_key();
        Self { path, key }
    }

    fn read(&self) -> Option<bool> {
        let bytes = fs::read(&self.path).ok()?;
        let record: DirectMlCacheRecord = serde_json::from_slice(&bytes).ok()?;
        (record.schema == 1 && record.key == self.key).then_some(record.available)
    }

    fn write(&self, available: bool) {
        let record = DirectMlCacheRecord {
            schema: 1,
            key: self.key.clone(),
            available,
        };
        let Ok(bytes) = serde_json::to_vec(&record) else {
            return;
        };
        let Some(parent) = self.path.parent() else {
            return;
        };
        if fs::create_dir_all(parent).is_err() {
            return;
        }
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|value| value.as_nanos())
            .unwrap_or_default();
        let temp = self
            .path
            .with_extension(format!("tmp-{}-{nonce}", std::process::id()));
        let Ok(mut file) = File::create(&temp) else {
            return;
        };
        if file.write_all(&bytes).is_err() || file.sync_all().is_err() {
            let _ = fs::remove_file(&temp);
            return;
        }
        atomic_replace(&temp, &self.path);
    }
}

fn capability_key() -> String {
    let mut parts = vec![
        "snow-shot-directml".to_string(),
        env!("CARGO_PKG_VERSION").to_string(),
        "ort-2.0.0-rc.13".to_string(),
        std::env::consts::OS.to_string(),
        std::env::consts::ARCH.to_string(),
        std::env::var("PROCESSOR_IDENTIFIER").unwrap_or_default(),
        std::env::var("DXGI_ADAPTER_LUID").unwrap_or_default(),
    ];
    for name in [
        "onnxruntime.dll",
        "DirectML.dll",
        "libonnxruntime.so",
        "libDirectML.so",
    ] {
        let candidates = [
            std::env::current_exe()
                .ok()
                .and_then(|path| path.parent().map(|dir| dir.join(name))),
            Some(PathBuf::from(name)),
        ];
        let fingerprint = candidates
            .into_iter()
            .flatten()
            .find_map(|path| {
                let metadata = fs::metadata(&path).ok()?;
                let modified = metadata
                    .modified()
                    .ok()?
                    .duration_since(UNIX_EPOCH)
                    .ok()?
                    .as_secs();
                Some(format!(
                    "{}:{}:{}",
                    path.display(),
                    metadata.len(),
                    modified
                ))
            })
            .unwrap_or_default();
        parts.push(fingerprint);
    }
    parts.join("|")
}

#[cfg(not(target_os = "windows"))]
fn atomic_replace(source: &Path, destination: &Path) {
    let _ = fs::rename(source, destination);
}

#[cfg(target_os = "windows")]
fn atomic_replace(source: &Path, destination: &Path) {
    use std::{ffi::OsStr, os::windows::ffi::OsStrExt};
    use windows_sys::Win32::Storage::FileSystem::{
        MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, MoveFileExW,
    };
    let source_w: Vec<u16> = OsStr::new(source)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let destination_w: Vec<u16> = OsStr::new(destination)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let moved = unsafe {
        MoveFileExW(
            source_w.as_ptr(),
            destination_w.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    } != 0;
    if !moved {
        let _ = fs::remove_file(source);
    }
}

fn directml_capability(config: &Config) -> bool {
    if !config.directml {
        return false;
    }
    if let Some(value) = config.directml_cache.read() {
        return value;
    }
    let value = directml_is_available();
    config.directml_cache.write(value);
    value
}

struct Job {
    id: u64,
    queued_at: Instant,
    input: OcrInput,
    cancelled: Arc<AtomicBool>,
}
struct Completion {
    id: u64,
    result: Result<OcrResult, String>,
    cancelled: bool,
}

fn make_engine(
    config: &Config,
    directml: bool,
    thread_budget: usize,
) -> rapid_ocr_rs::Result<RapidOcr> {
    make_engine_for_models(
        &config.detector_model,
        &config.recognizer_model,
        &config.dictionary,
        directml,
        thread_budget,
    )
}

fn make_engine_for_models(
    detector_model: &Path,
    recognizer_model: &Path,
    dictionary: &Path,
    directml: bool,
    thread_budget: usize,
) -> rapid_ocr_rs::Result<RapidOcr> {
    let mut engine = EngineConfig::default();
    engine.global.use_det = true;
    engine.global.use_cls = false;
    engine.global.use_rec = true;
    engine.det.lang = LangDet::Multi;
    engine.det.ocr_version = rapid_ocr_rs::OcrVersion::PPocrV6;
    engine.det.model_type = ModelType::Small;
    engine.det.allow_download = false;
    engine.det.model_path = Some(detector_model.to_path_buf());
    engine.rec.model.lang = LangRec::Ch;
    engine.rec.model.ocr_version = rapid_ocr_rs::OcrVersion::PPocrV6;
    engine.rec.model.model_type = ModelType::Small;
    engine.rec.model.allow_download = false;
    engine.rec.model.model_path = Some(recognizer_model.to_path_buf());
    engine.rec.model.rec_keys_path = Some(dictionary.to_path_buf());
    let budget = thread_budget.max(1);
    for runtime in [
        &mut engine.det.runtime,
        &mut engine.cls.runtime,
        &mut engine.rec.runtime,
    ] {
        runtime.thread_budget = Some(budget);
        runtime.intra_threads = Some(budget);
        runtime.inter_threads = Some(1);
        runtime.rayon_threads = Some(budget);
        runtime.auto_tune_threads = false;
        runtime.enable_cpu_mem_arena = true;
        runtime.provider_preference = ProviderPreference::Cpu;
    }
    if directml {
        for runtime in [&mut engine.det.runtime, &mut engine.rec.runtime] {
            runtime.intra_threads = Some(1);
            runtime.inter_threads = Some(1);
            runtime.rayon_threads = Some(1);
            runtime.provider_preference = ProviderPreference::DirectMl { device_id: 0 };
            runtime.fail_if_provider_unavailable = false;
        }
    }
    RapidOcr::new_with_sources(
        engine,
        PipelineSources {
            det: Some(ModelSource::File(detector_model)),
            cls: None,
            rec: Some(ModelSource::File(recognizer_model)),
            rec_dictionary: Some(DictionarySource::File(dictionary)),
        },
    )
}

fn run_unless_cancelled<T>(
    cancelled: &AtomicBool,
    work: impl FnOnce() -> Result<T, String>,
) -> Result<T, String> {
    if cancelled.load(Ordering::Acquire) {
        Err("cancelled".to_string())
    } else {
        work()
    }
}

fn worker_event(
    event: &str,
    id: u64,
    stage: &str,
    backend: &str,
    outcome: &str,
    elapsed: u128,
    message: &str,
) {
    eprintln!(
        "{}",
        serde_json::json!({
            "event": event, "message": message,
            "fields": {"operation": id.to_string(), "stage": stage, "backend": backend,
                       "outcome": outcome, "duration_ms": elapsed}
        })
    );
}

fn cpu_fallback_engine(
    config: &Config,
    thread_budget: usize,
    id: u64,
    cancelled: &AtomicBool,
) -> Option<RapidOcr> {
    let started = Instant::now();
    let result = run_unless_cancelled(cancelled, || {
        make_engine(config, false, thread_budget).map_err(|error| error.to_string())
    });
    worker_event(
        "ocr.backend_fallback",
        id,
        "cpu_initialization",
        "cpu",
        if cancelled.load(Ordering::Acquire) {
            "cancelled"
        } else if result.is_ok() {
            "succeeded"
        } else {
            "failed"
        },
        started.elapsed().as_millis(),
        result.as_ref().err().map_or("", String::as_str),
    );
    result.ok()
}

fn initialize_engine(
    config: &Config,
    thread_budget: usize,
    operation: u64,
    cancelled: &AtomicBool,
) -> (Option<RapidOcr>, &'static str) {
    let mut engine_backend = "cpu";
    let initializing = Instant::now();
    let wants_directml = config.directml && config.directml_enabled.load(Ordering::Acquire);
    let engine = match make_engine(config, wants_directml, thread_budget) {
        Ok(candidate) => {
            let provider_ok = !wants_directml
                || candidate
                    .provider_resolutions()
                    .det
                    .into_iter()
                    .chain(candidate.provider_resolutions().rec)
                    .any(|resolution| resolution.resolved == ResolvedExecutionProvider::DirectMl);
            if wants_directml && !provider_ok {
                worker_event(
                    "ocr.backend_fallback",
                    operation,
                    "provider_resolution",
                    "cpu",
                    "started",
                    0,
                    "DirectML was not selected",
                );
                config.directml_cache.write(false);
                config.directml_enabled.store(false, Ordering::Release);
                cpu_fallback_engine(config, thread_budget, operation, cancelled)
            } else {
                engine_backend = if wants_directml { "directml" } else { "cpu" };
                Some(candidate)
            }
        }
        Err(error) if wants_directml => {
            worker_event(
                "ocr.backend_fallback",
                operation,
                "initialization",
                "cpu",
                "started",
                0,
                &error.to_string(),
            );
            config.directml_cache.write(false);
            config.directml_enabled.store(false, Ordering::Release);
            cpu_fallback_engine(config, thread_budget, operation, cancelled)
        }
        Err(error) => {
            worker_event(
                "ocr.engine_ready",
                operation,
                "initialization",
                "cpu",
                "failed",
                initializing.elapsed().as_millis(),
                &error.to_string(),
            );
            None
        }
    };
    worker_event(
        "ocr.engine_ready",
        operation,
        "initialization",
        engine_backend,
        if engine.is_some() {
            "succeeded"
        } else {
            "failed"
        },
        initializing.elapsed().as_millis(),
        "",
    );
    (engine, engine_backend)
}

fn session_config(payload: &[u8], state: &str) -> io::Result<Config> {
    let mut d = Decoder::new(payload);
    let directml = d.u8()? != 0;
    let detector_model = PathBuf::from(d.string()?);
    let recognizer_model = PathBuf::from(d.string()?);
    let dictionary = PathBuf::from(d.string()?);
    if !d.done() {
        return Err(io::Error::other("invalid OCR session configuration"));
    }
    let state_dir = (!state.is_empty()).then(|| PathBuf::from(state));
    Ok(Config {
        directml,
        directml_enabled: Arc::new(AtomicBool::new(false)),
        directml_cache: Arc::new(DirectMlCapabilityCache::new(state_dir.as_deref())),
        detector_model,
        recognizer_model,
        dictionary,
    })
}
fn ready_payload() -> Vec<u8> {
    let mut p = Vec::new();
    put_u8(&mut p, 1);
    put_u8(&mut p, 0);
    put_string(&mut p, "unloaded");
    put_string(&mut p, env!("CARGO_PKG_VERSION"));
    put_u32(&mut p, protocol::VERSION as u32);
    p
}
fn completion_payload(completion: &Completion) -> Vec<u8> {
    let mut p = Vec::new();
    if completion.cancelled {
        put_u8(&mut p, 2);
        put_string(&mut p, "cancelled");
        return p;
    }
    match &completion.result {
        Err(error) => {
            put_u8(&mut p, 0);
            put_string(&mut p, error);
        }
        Ok(result) => {
            put_u8(&mut p, 1);
            let (lines, boxes): (&[_], &[_]) = match result {
                OcrResult::Full(full) => (&full.lines, &full.boxes),
                _ => (&[], &[]),
            };
            put_string(&mut p, "");
            put_u32(&mut p, lines.len() as u32);
            for (index, line) in lines.iter().enumerate() {
                put_string(&mut p, &line.text);
                put_f32(&mut p, line.score);
                let quad = boxes.get(index).copied().unwrap_or([[0.0; 2]; 4]);
                for point in quad.iter().flatten() {
                    put_f32(&mut p, *point);
                }
            }
        }
    }
    p
}

mod diagnostics;

#[derive(Debug, PartialEq)]
enum StartupMode {
    Worker,
    Version,
    ValidateModelSet {
        detector: PathBuf,
        recognizer: PathBuf,
        dictionary: PathBuf,
    },
}

fn parse_startup_mode(arguments: &[std::ffi::OsString]) -> io::Result<StartupMode> {
    if arguments.is_empty() {
        return Ok(StartupMode::Worker);
    }
    if arguments.len() == 1 && arguments[0] == "--version" {
        return Ok(StartupMode::Version);
    }
    if arguments
        .first()
        .is_some_and(|argument| argument == "--validate-model-set")
    {
        if arguments.len() != 4 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "usage: snow-ocr-process --validate-model-set <detector> <recognizer> <dictionary>",
            ));
        }
        return Ok(StartupMode::ValidateModelSet {
            detector: PathBuf::from(&arguments[1]),
            recognizer: PathBuf::from(&arguments[2]),
            dictionary: PathBuf::from(&arguments[3]),
        });
    }
    Err(io::Error::new(
        io::ErrorKind::InvalidInput,
        "unknown snow-ocr-process command",
    ))
}

fn main() -> io::Result<()> {
    let arguments: Vec<_> = std::env::args_os().skip(1).collect();
    let startup_mode = parse_startup_mode(&arguments)?;
    if startup_mode == StartupMode::Version {
        println!(
            "snow-ocr-process {} {}-{} protocol {}",
            env!("CARGO_PKG_VERSION"),
            std::env::consts::OS,
            std::env::consts::ARCH,
            protocol::VERSION
        );
        return Ok(());
    }
    // Native ONNX Runtime diagnostics must never share stdout with the binary
    // IPC stream. Severity 3 suppresses the cpuinfo debug chatter emitted by
    // the Windows runtime before its custom logger is installed.
    unsafe {
        std::env::set_var("ORT_LOG_SEVERITY_LEVEL", "3");
        std::env::set_var("CPUINFO_LOG_LEVEL", "error");
    }
    if let StartupMode::ValidateModelSet {
        detector,
        recognizer,
        dictionary,
    } = startup_mode
    {
        initialize_onnx_runtime().map_err(|error| io::Error::other(error.to_string()))?;
        make_engine_for_models(&detector, &recognizer, &dictionary, false, 1)
            .map_err(|error| io::Error::other(error.to_string()))?;
        return Ok(());
    }
    diagnostics::initialize();
    let mut reader = BufReader::new(std::io::stdin());
    let mut writer = BufWriter::new(std::io::stdout());
    let startup = match read_frame(&mut reader) {
        Ok(frame) => frame,
        Err(error) => {
            return Err(error);
        }
    };
    if startup.kind != Kind::Hello {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "OCR process expected Hello",
        ));
    }
    let mut hello = Decoder::new(&startup.payload);
    let state = hello.string()?;
    if !hello.done() {
        return Err(io::Error::other("invalid OCR hello"));
    }
    let (work_tx, work_rx) = mpsc::channel();
    let (result_tx, result_rx) = mpsc::channel();
    let worker = thread::Builder::new()
        .name("snow-ocr-inference".into())
        .spawn(move || worker_loop(work_rx, result_tx))?;
    write_frame(&mut writer, Kind::Ready, 0, &ready_payload())?;
    let (command_tx, command_rx) = mpsc::channel();
    thread::spawn(move || {
        while let Ok(frame) = read_frame(&mut reader) {
            if command_tx.send(frame).is_err() {
                break;
            }
        }
    });
    let mut shared: Option<(u64, SharedImage)> = None;
    let mut staged: Option<(u64, OcrInput)> = None;
    let mut active: Option<(u64, Arc<AtomicBool>)> = None;
    let mut last_sequence = 0;
    loop {
        while let Ok(result) = result_rx.try_recv() {
            match result {
                WorkResult::Prepared(id, ok) => {
                    write_frame(&mut writer, Kind::SessionReady, id, &[u8::from(ok)])?
                }
                WorkResult::Released(id) => {
                    write_frame(&mut writer, Kind::SessionReleased, id, &[])?
                }
                WorkResult::Complete(completion) => {
                    active = None;
                    write_frame(
                        &mut writer,
                        Kind::Complete,
                        completion.id,
                        &completion_payload(&completion),
                    )?;
                }
            }
        }
        let frame = match command_rx.recv_timeout(std::time::Duration::from_millis(5)) {
            Ok(frame) => frame,
            Err(mpsc::RecvTimeoutError::Timeout) => continue,
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        };
        match frame.kind {
            Kind::AttachBuffer => {
                if shared.is_some() {
                    return Err(io::Error::other("OCR buffer already attached"));
                }
                let mut d = Decoder::new(&frame.payload);
                let path = d.string()?;
                let bytes = usize::try_from(d.u64()?).map_err(io::Error::other)?;
                if !d.done() || !(SLOT_HEADER + 4..=SLOT_HEADER + 3840 * 2160 * 4).contains(&bytes)
                {
                    return Err(io::Error::other("invalid OCR buffer capacity"));
                }
                let file = File::open(path)?;
                if file.metadata()?.len() != bytes as u64 {
                    return Err(io::Error::other("OCR buffer size mismatch"));
                }
                let mmap = Arc::new(unsafe { Mmap::map(&file)? });
                shared = Some((
                    frame.request_id,
                    SharedImage {
                        mmap,
                        slot_bytes: bytes,
                    },
                ));
                last_sequence = 0;
                write_frame(&mut writer, Kind::BufferAttached, frame.request_id, &[])?;
            }
            Kind::DetachBuffer => {
                if shared.as_ref().map(|(id, _)| *id) != Some(frame.request_id) {
                    return Err(io::Error::other("stale OCR buffer detach"));
                }
                shared = None;
                write_frame(&mut writer, Kind::BufferDetached, frame.request_id, &[])?;
            }
            Kind::Submit => {
                let mut d = Decoder::new(&frame.payload);
                let generation = d.u64()?;
                let width = d.u32()? as usize;
                let height = d.u32()? as usize;
                let stride = d.u32()? as usize;
                let sequence = d.u64()?;
                if !d.done() || staged.is_some() || sequence <= last_sequence {
                    return Err(io::Error::other("invalid OCR image transfer"));
                }
                let Some((id, image)) = &shared else {
                    return Err(io::Error::other("OCR buffer is absent"));
                };
                if *id != generation {
                    return Err(io::Error::other("stale OCR image transfer"));
                }
                let data = image.read_bgr(0, width, height, stride, sequence)?;
                last_sequence = sequence;
                staged = Some((
                    frame.request_id,
                    OcrInput::BgrU8 {
                        width,
                        height,
                        data,
                    },
                ));
                let mut ack = Vec::new();
                ack.extend_from_slice(&generation.to_le_bytes());
                ack.extend_from_slice(&sequence.to_le_bytes());
                write_frame(&mut writer, Kind::ImageConsumed, frame.request_id, &ack)?;
            }
            Kind::Recognize => {
                if active.is_some() {
                    return Err(io::Error::other("OCR inference is already running"));
                }
                let Some((id, input)) = staged.take() else {
                    return Err(io::Error::other("OCR image is absent"));
                };
                if id != frame.request_id {
                    return Err(io::Error::other("OCR image ownership mismatch"));
                }
                let cancelled = Arc::new(AtomicBool::new(false));
                active = Some((id, Arc::clone(&cancelled)));
                work_tx
                    .send(Work::Recognize(Job {
                        id,
                        queued_at: Instant::now(),
                        input,
                        cancelled,
                    }))
                    .map_err(io::Error::other)?;
            }
            Kind::DiscardImage => {
                if staged.as_ref().map(|(id, _)| *id) == Some(frame.request_id) {
                    staged = None;
                }
            }
            Kind::PrepareSession => {
                if active.is_some() {
                    return Err(io::Error::other("cannot replace an executing OCR session"));
                }
                work_tx
                    .send(Work::Prepare(
                        frame.request_id,
                        session_config(&frame.payload, &state)?,
                    ))
                    .map_err(io::Error::other)?;
            }
            Kind::ReleaseSession => {
                if active.is_some() {
                    return Err(io::Error::other("cannot release an executing OCR session"));
                }
                work_tx
                    .send(Work::Release(frame.request_id))
                    .map_err(io::Error::other)?;
            }
            Kind::Cancel => {
                if let Some((id, flag)) = &active
                    && *id == frame.request_id
                {
                    flag.store(true, Ordering::Release);
                }
            }
            Kind::Shutdown => break,
            _ => return Err(io::Error::other("unexpected OCR command")),
        }
    }
    drop(staged);
    drop(shared);
    drop(work_tx);
    let _ = worker.join();
    write_frame(&mut writer, Kind::ShutdownAck, 0, &[])
}

#[cfg(test)]
mod tests {
    #[test]
    fn cancellation_checkpoint_skips_expensive_work_and_preserves_results() {
        let cancelled = std::sync::atomic::AtomicBool::new(true);
        let result: Result<(), String> =
            super::run_unless_cancelled(&cancelled, || panic!("cancelled inference must not run"));
        assert_eq!(result.unwrap_err(), "cancelled");
        cancelled.store(false, std::sync::atomic::Ordering::Release);
        assert_eq!(super::run_unless_cancelled(&cancelled, || Ok(42)), Ok(42));
        let error: Result<(), String> =
            super::run_unless_cancelled(&cancelled, || Err("provider failed".into()));
        assert_eq!(error.unwrap_err(), "provider failed");
    }
    use super::{StartupMode, parse_startup_mode};
    use std::{ffi::OsString, io, path::PathBuf};

    fn arguments(values: &[&str]) -> Vec<OsString> {
        values.iter().map(OsString::from).collect()
    }

    #[test]
    fn startup_modes_accept_only_the_documented_shapes() {
        assert_eq!(parse_startup_mode(&[]).unwrap(), StartupMode::Worker);
        assert_eq!(
            parse_startup_mode(&arguments(&["--version"])).unwrap(),
            StartupMode::Version
        );
        assert_eq!(
            parse_startup_mode(&arguments(&[
                "--validate-model-set",
                "det.onnx",
                "rec.onnx",
                "dict.txt"
            ]))
            .unwrap(),
            StartupMode::ValidateModelSet {
                detector: PathBuf::from("det.onnx"),
                recognizer: PathBuf::from("rec.onnx"),
                dictionary: PathBuf::from("dict.txt"),
            }
        );
        assert_eq!(
            parse_startup_mode(&arguments(&["--validate-model-set", "det.onnx"]))
                .unwrap_err()
                .kind(),
            io::ErrorKind::InvalidInput
        );
        assert_eq!(
            parse_startup_mode(&arguments(&["--unknown"]))
                .unwrap_err()
                .kind(),
            io::ErrorKind::InvalidInput
        );
    }
}
