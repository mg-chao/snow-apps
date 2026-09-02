mod protocol;

use memmap2::Mmap;
use protocol::{
    Decoder, Frame, Kind, put_f32, put_string, put_u8, put_u32, read_frame, write_frame,
};
use rapid_ocr_rs::{
    DictionarySource, EngineConfig, LangDet, LangRec, ModelSource, ModelType, OcrCallOptions,
    OcrInput, OcrResult, PipelineSources, ProviderPreference, RapidOcr,
    ResolvedExecutionProvider, directml_is_available, initialize_onnx_runtime,
};
use serde::{Deserialize, Serialize};
use std::{
    collections::VecDeque,
    fs::{self, File},
    io::{self, BufReader, BufWriter, Write},
    path::{Path, PathBuf},
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, Ordering, fence},
        mpsc::{self, Receiver, Sender},
    },
    thread::{self, JoinHandle},
    time::{SystemTime, UNIX_EPOCH},
};

const SLOT_HEADER: usize = 32;
const SLOT_SEQUENCE: usize = 0;
const SLOT_STATE: usize = 8;
const SLOT_WIDTH: usize = 12;
const SLOT_HEIGHT: usize = 16;
const SLOT_STRIDE: usize = 20;
const SLOT_BYTES: usize = 24;
const SLOT_MAGIC: usize = 28;
const SLOT_READY: u32 = 1;
const SLOT_MAGIC_VALUE: u32 = 0x544f4c53;

#[derive(Clone)]
struct Config {
    workers: usize,
    directml: bool,
    directml_enabled: Arc<AtomicBool>,
    directml_cache: Arc<DirectMlCapabilityCache>,
    detector_model: PathBuf,
    recognizer_model: PathBuf,
    dictionary: PathBuf,
    slot_bytes: usize,
    slot_count: usize,
    shm_path: PathBuf,
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
        "snow-shot-directml-v1".to_string(),
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

struct SharedImage {
    mmap: Arc<Mmap>,
    slot_bytes: usize,
}
impl SharedImage {
    fn read_bgr(
        &self,
        slot: usize,
        width: usize,
        height: usize,
        stride: usize,
        sequence: u64,
    ) -> io::Result<Vec<u8>> {
        if width == 0 || height == 0 || stride < width.saturating_mul(4) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid OCR image dimensions",
            ));
        }
        let required = stride
            .checked_mul(height)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "OCR image size overflow"))?;
        if required > self.slot_bytes.saturating_sub(SLOT_HEADER) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "OCR image exceeds shared-memory slot",
            ));
        }
        let start = slot
            .checked_mul(self.slot_bytes)
            .and_then(|v| v.checked_add(SLOT_HEADER))
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidData, "invalid OCR shared-memory slot")
            })?;
        let end = start.checked_add(required).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "OCR image range overflow")
        })?;
        if end > self.mmap.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "OCR shared-memory slot is out of range",
            ));
        }
        let header_start = slot.checked_mul(self.slot_bytes).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "invalid OCR shared-memory slot")
        })?;
        let header = self
            .mmap
            .get(header_start..header_start + SLOT_HEADER)
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    "OCR shared-memory slot header is out of range",
                )
            })?;
        fence(Ordering::Acquire);
        let header_sequence =
            u64::from_le_bytes(header[SLOT_SEQUENCE..SLOT_SEQUENCE + 8].try_into().unwrap());
        let state = u32::from_le_bytes(header[SLOT_STATE..SLOT_STATE + 4].try_into().unwrap());
        let header_width =
            u32::from_le_bytes(header[SLOT_WIDTH..SLOT_WIDTH + 4].try_into().unwrap()) as usize;
        let header_height =
            u32::from_le_bytes(header[SLOT_HEIGHT..SLOT_HEIGHT + 4].try_into().unwrap()) as usize;
        let header_stride =
            u32::from_le_bytes(header[SLOT_STRIDE..SLOT_STRIDE + 4].try_into().unwrap()) as usize;
        let header_bytes =
            u32::from_le_bytes(header[SLOT_BYTES..SLOT_BYTES + 4].try_into().unwrap()) as usize;
        let header_magic =
            u32::from_le_bytes(header[SLOT_MAGIC..SLOT_MAGIC + 4].try_into().unwrap());
        if state != SLOT_READY
            || header_sequence != sequence
            || header_width != width
            || header_height != height
            || header_stride != stride
            || header_bytes != required
            || header_magic != SLOT_MAGIC_VALUE
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "OCR shared-memory slot ownership mismatch",
            ));
        }
        let source = &self.mmap[start..end];
        let mut bgr = Vec::with_capacity(width * height * 3);
        for row in source.chunks(stride).take(height) {
            for pixel in row[..width * 4].chunks_exact(4) {
                bgr.extend_from_slice(&[pixel[2], pixel[1], pixel[0]]);
            }
        }
        Ok(bgr)
    }
}

struct Job {
    id: u64,
    input: OcrInput,
    cancelled: Arc<std::sync::atomic::AtomicBool>,
    priority: u8,
}
struct Queue {
    jobs: VecDeque<Job>,
    stopping: bool,
}
struct Completion {
    id: u64,
    result: Result<OcrResult, String>,
    cancelled: bool,
}
struct Scheduler {
    queue: Arc<(Mutex<Queue>, Condvar)>,
    cancellations: Arc<Mutex<std::collections::HashMap<u64, Arc<std::sync::atomic::AtomicBool>>>>,
    workers: Vec<JoinHandle<()>>,
    completions: Receiver<Completion>,
}

impl Scheduler {
    fn new(config: &Config) -> io::Result<Self> {
        let queue = Arc::new((
            Mutex::new(Queue {
                jobs: VecDeque::new(),
                stopping: false,
            }),
            Condvar::new(),
        ));
        let cancellations = Arc::new(Mutex::new(std::collections::HashMap::new()));
        let (completion_tx, completion_rx) = mpsc::channel();
        let mut workers = Vec::new();
        let factory_config = config.clone();
        for index in 0..config.workers {
            let queue = Arc::clone(&queue);
            let cancellations = Arc::clone(&cancellations);
            let tx = completion_tx.clone();
            let cfg = factory_config.clone();
            workers.push(
                thread::Builder::new()
                    .name(format!("snow-ocr-worker-{index}"))
                    .spawn(move || worker_loop(queue, cancellations, tx, cfg))
                    .map_err(|e| io::Error::other(e.to_string()))?,
            );
        }
        Ok(Self {
            queue,
            cancellations,
            workers,
            completions: completion_rx,
        })
    }
    fn submit(&self, id: u64, input: OcrInput, priority: u8) {
        let cancelled = Arc::new(std::sync::atomic::AtomicBool::new(false));
        self.cancellations
            .lock()
            .unwrap()
            .insert(id, Arc::clone(&cancelled));
        let (state, wake) = &*self.queue;
        let mut state = state.lock().unwrap();
        let position = state
            .jobs
            .iter()
            .position(|job| job.priority > priority)
            .unwrap_or(state.jobs.len());
        state.jobs.insert(
            position,
            Job {
                id,
                input,
                cancelled,
                priority,
            },
        );
        wake.notify_one();
    }
    fn cancel(&self, id: u64) {
        if let Some(flag) = self.cancellations.lock().unwrap().get(&id) {
            flag.store(true, std::sync::atomic::Ordering::Release);
        }
    }
    fn try_completion(&self) -> Option<Completion> {
        self.completions.try_recv().ok()
    }
    fn shutdown(&mut self) {
        let (state, wake) = &*self.queue;
        state.lock().unwrap().stopping = true;
        wake.notify_all();
        for worker in self.workers.drain(..) {
            let _ = worker.join();
        }
    }
}

fn make_engine(config: &Config, directml: bool) -> rapid_ocr_rs::Result<RapidOcr> {
    let mut engine = EngineConfig::default();
    engine.global.use_det = true;
    engine.global.use_cls = false;
    engine.global.use_rec = true;
    engine.det.lang = LangDet::Multi;
    engine.det.ocr_version = rapid_ocr_rs::OcrVersion::PPocrV6;
    engine.det.model_type = ModelType::Small;
    engine.det.allow_download = false;
    engine.det.model_path = Some(config.detector_model.clone());
    engine.rec.model.lang = LangRec::Ch;
    engine.rec.model.ocr_version = rapid_ocr_rs::OcrVersion::PPocrV6;
    engine.rec.model.model_type = ModelType::Small;
    engine.rec.model.allow_download = false;
    engine.rec.model.model_path = Some(config.recognizer_model.clone());
    engine.rec.model.rec_keys_path = Some(config.dictionary.clone());
    let physical = num_cpus::get_physical().max(1);
    let budget = (physical / config.workers.max(1)).max(1);
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
            det: Some(ModelSource::File(&config.detector_model)),
            cls: None,
            rec: Some(ModelSource::File(&config.recognizer_model)),
            rec_dictionary: Some(DictionarySource::File(&config.dictionary)),
        },
    )
}

fn worker_loop(
    queue: Arc<(Mutex<Queue>, Condvar)>,
    cancellations: Arc<Mutex<std::collections::HashMap<u64, Arc<std::sync::atomic::AtomicBool>>>>,
    tx: Sender<Completion>,
    config: Config,
) {
    let mut engine: Option<RapidOcr> = None;
    loop {
        let job = {
            let (state, wake) = &*queue;
            let mut state = state.lock().unwrap();
            while state.jobs.is_empty() && !state.stopping {
                state = wake.wait(state).unwrap();
            }
            if state.stopping {
                return;
            }
            state.jobs.pop_front().unwrap()
        };
        let cancelled = job.cancelled.load(std::sync::atomic::Ordering::Acquire);
        let result = if cancelled {
            Err("cancelled".to_string())
        } else {
            if engine.is_none() {
                let wants_directml =
                    config.directml && config.directml_enabled.load(Ordering::Acquire);
                engine = match make_engine(&config, wants_directml) {
                    Ok(candidate) => {
                        let provider_ok = !wants_directml
                            || candidate
                                .provider_resolutions()
                                .det
                                .into_iter()
                                .chain(candidate.provider_resolutions().rec)
                                .any(|resolution| {
                                    resolution.resolved == ResolvedExecutionProvider::DirectMl
                                });
                        if wants_directml && !provider_ok {
                            config.directml_cache.write(false);
                            config.directml_enabled.store(false, Ordering::Release);
                            make_engine(&config, false).ok()
                        } else {
                            Some(candidate)
                        }
                    }
                    Err(_) if wants_directml => {
                        config.directml_cache.write(false);
                        config.directml_enabled.store(false, Ordering::Release);
                        make_engine(&config, false).ok()
                    }
                    Err(_error) => None,
                };
            }
            let options = OcrCallOptions {
                use_det: Some(true),
                use_cls: Some(false),
                use_rec: Some(true),
                ..Default::default()
            };
            let input = job.input;
            let first_attempt = engine.as_mut().map(|engine| {
                engine
                    .run(input.clone(), options.clone())
                    .and_then(OcrResult::try_from)
                    .map_err(|e| e.to_string())
            });
            match first_attempt {
                Some(Ok(result)) => Ok(result),
                Some(Err(error)) if config.directml_enabled.load(Ordering::Acquire) => {
                    // A provider can pass the inexpensive availability check
                    // and still fail while creating or executing a session.
                    // Persist the negative result and retry this request on
                    // CPU so one bad driver does not fail the OCR operation.
                    config.directml_cache.write(false);
                    config.directml_enabled.store(false, Ordering::Release);
                    engine = make_engine(&config, false).ok();
                    match engine.as_mut() {
                        Some(cpu) => cpu
                            .run(input, options)
                            .and_then(OcrResult::try_from)
                            .map_err(|cpu_error| {
                                format!("{error}; CPU fallback failed: {cpu_error}")
                            }),
                        None => Err(format!("{error}; unable to initialize CPU fallback engine")),
                    }
                }
                Some(Err(error)) => Err(error),
                None => Err("unable to initialize OCR engine".to_string()),
            }
        };
        cancellations.lock().unwrap().remove(&job.id);
        let _ = tx.send(Completion {
            id: job.id,
            result,
            cancelled,
        });
    }
}

fn hello(frame: &Frame) -> io::Result<Config> {
    let mut d = Decoder::new(&frame.payload);
    let workers = (d.u32()? as usize).clamp(1, 2);
    let directml = d.u8()? != 0;
    let detector_model = PathBuf::from(d.string()?);
    let recognizer_model = PathBuf::from(d.string()?);
    let dictionary = PathBuf::from(d.string()?);
    let state = d.string()?;
    let shm_path = PathBuf::from(d.string()?);
    let slot_bytes = d.u64()? as usize;
    let slot_count = d.u32()? as usize;
    if !d.done() || slot_bytes < SLOT_HEADER + 4 || slot_count < workers {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid OCR startup configuration",
        ));
    }
    for path in [&detector_model, &recognizer_model, &dictionary] {
        if !path.is_file() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("required OCR asset is missing: {}", path.display()),
            ));
        }
    }
    let state_dir = (!state.trim().is_empty()).then(|| PathBuf::from(state));
    let directml_cache = Arc::new(DirectMlCapabilityCache::new(state_dir.as_deref()));
    Ok(Config {
        workers,
        directml,
        directml_enabled: Arc::new(AtomicBool::new(false)),
        directml_cache,
        detector_model,
        recognizer_model,
        dictionary,
        slot_bytes,
        slot_count,
        shm_path,
    })
}

fn ready_payload(config: &Config) -> Vec<u8> {
    let mut p = Vec::new();
    put_u8(&mut p, 1);
    let available = config.directml_enabled.load(Ordering::Acquire);
    put_u8(&mut p, available as u8);
    put_string(&mut p, if available { "directml" } else { "cpu" });
    put_string(&mut p, env!("CARGO_PKG_VERSION"));
    protocol::put_u32(&mut p, protocol::VERSION as u32);
    p
}
fn error_payload(message: &str) -> Vec<u8> {
    let mut p = Vec::new();
    put_u8(&mut p, 0);
    put_u8(&mut p, 0);
    put_string(&mut p, "cpu");
    put_string(&mut p, message);
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

fn main() -> io::Result<()> {
    if std::env::args_os().any(|argument| argument == "--version") {
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
    let config = match hello(&startup) {
        Ok(config) => config,
        Err(error) => {
            return Err(error);
        }
    };
    initialize_onnx_runtime().map_err(|e| io::Error::other(e.to_string()))?;
    config
        .directml_enabled
        .store(directml_capability(&config), Ordering::Release);
    let file = File::open(&config.shm_path)?;
    let mmap = Arc::new(unsafe { Mmap::map(&file)? });
    let required_shared_memory = config
        .slot_bytes
        .checked_mul(config.slot_count)
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                "OCR shared-memory size overflow",
            )
        })?;
    if mmap.len() < required_shared_memory {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "OCR shared-memory file is too small",
        ));
    }
    let shared = SharedImage {
        mmap,
        slot_bytes: config.slot_bytes,
    };
    let scheduler = Scheduler::new(&config)?;
    write_frame(&mut writer, Kind::Ready, 0, &ready_payload(&config))?;
    let (command_tx, command_rx) = mpsc::channel();
    let _reader_thread = thread::spawn(move || {
        loop {
            match read_frame(&mut reader) {
                Ok(frame) => {
                    if command_tx.send(frame).is_err() {
                        break;
                    }
                }
                Err(error) => {
                    let _ = command_tx.send(Frame {
                        kind: Kind::Shutdown,
                        request_id: 0,
                        payload: error.to_string().into_bytes(),
                    });
                    break;
                }
            }
        }
    });
    let mut stopping = false;
    while !stopping {
        while let Some(completion) = scheduler.try_completion() {
            write_frame(
                &mut writer,
                Kind::Complete,
                completion.id,
                &completion_payload(&completion),
            )?;
        }
        match command_rx.recv_timeout(std::time::Duration::from_millis(5)) {
            Ok(frame) => match frame.kind {
                Kind::Submit => {
                    let mut d = Decoder::new(&frame.payload);
                    let slot = d.u32()? as usize;
                    let width = d.u32()? as usize;
                    let height = d.u32()? as usize;
                    let stride = d.u32()? as usize;
                    let sequence = d.u64()?;
                    let priority = d.u8()?;
                    if !d.done() || slot >= config.slot_count {
                        let p = error_payload("invalid OCR submit payload");
                        write_frame(&mut writer, Kind::Complete, frame.request_id, &p)?;
                    } else {
                        match shared.read_bgr(slot, width, height, stride, sequence) {
                            Ok(data) => scheduler.submit(
                                frame.request_id,
                                OcrInput::BgrU8 {
                                    width,
                                    height,
                                    data,
                                },
                                priority,
                            ),
                            Err(error) => {
                                let p = error_payload(&error.to_string());
                                write_frame(&mut writer, Kind::Complete, frame.request_id, &p)?;
                            }
                        }
                    }
                }
                Kind::Cancel => scheduler.cancel(frame.request_id),
                Kind::Shutdown => {
                    stopping = true;
                }
                _ => {}
            },
            Err(mpsc::RecvTimeoutError::Disconnected) => stopping = true,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
    }
    let mut scheduler = scheduler;
    scheduler.shutdown();
    while let Some(completion) = scheduler.try_completion() {
        write_frame(
            &mut writer,
            Kind::Complete,
            completion.id,
            &completion_payload(&completion),
        )?;
    }
    write_frame(&mut writer, Kind::ShutdownAck, 0, &[])
}
