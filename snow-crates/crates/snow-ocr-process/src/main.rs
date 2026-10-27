mod acceleration;
mod buffer;
use buffer::{SLOT_HEADER, SharedImage};
mod protocol;
mod session;
mod worker;
use worker::{Work, WorkResult, worker_loop};

use memmap2::Mmap;
use protocol::{Decoder, Kind, put_f32, put_string, put_u8, put_u32, read_frame, write_frame};
mod engine;
use engine::{Engine, InputImage, OcrLine, make_engine, make_engine_for_models};
use std::{
    fs::File,
    io::{self, BufReader, BufWriter},
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc::{self},
    },
    thread::{self},
    time::Instant,
};

#[derive(Clone, PartialEq, Eq)]
struct Config {
    detector_resize_policy: DetectorResizePolicy,
    text_detection_processing: TextDetectionProcessing,
    hardware_acceleration: bool,
    detector_model: PathBuf,
    recognizer_model: PathBuf,
    dictionary: PathBuf,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DetectorResizePolicy {
    Max,
    Min,
}

impl DetectorResizePolicy {
    fn from_wire(value: u8) -> io::Result<Self> {
        match value {
            0 => Ok(Self::Max),
            1 => Ok(Self::Min),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid OCR detector resize policy",
            )),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
enum TextDetectionProcessing {
    #[default]
    AccuracyFirst,
    SpeedFirst,
}

impl TextDetectionProcessing {
    fn from_wire(value: u8) -> io::Result<Self> {
        match value {
            0 => Ok(Self::AccuracyFirst),
            1 => Ok(Self::SpeedFirst),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid OCR text detection processing",
            )),
        }
    }

    fn recognition_width(self) -> usize {
        match self {
            Self::AccuracyFirst => 320,
            Self::SpeedFirst => 48,
        }
    }
}

struct Job {
    id: u64,
    queued_at: Instant,
    input: InputImage,
    cancelled: Arc<AtomicBool>,
}
struct Completion {
    id: u64,
    result: Result<Vec<OcrLine>, String>,
    cancelled: bool,
}

enum Event {
    Command(protocol::Frame),
    Completed(WorkResult),
    CommandsClosed,
    AccelerationReady,
}

fn read_commands(mut reader: impl io::Read, tx: mpsc::Sender<Event>) {
    while let Ok(frame) = read_frame(&mut reader) {
        if tx.send(Event::Command(frame)).is_err() {
            return;
        }
    }
    // The worker also owns a sender, so command EOF must explicitly wake the
    // event loop instead of waiting for every sender to disconnect.
    let _ = tx.send(Event::CommandsClosed);
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

fn initialize_engine(
    config: &Config,
    thread_budget: usize,
    previous: Option<Engine>,
    acceleration: &acceleration::Acceleration,
) -> Option<Engine> {
    let started = Instant::now();
    let gpu = config
        .hardware_acceleration
        .then(|| acceleration.snapshot())
        .flatten();
    let previous = previous.filter(|engine| engine.uses_gpu() == gpu.is_some());
    let reused = previous.is_some();
    let use_gpu = gpu.is_some();
    let result = match previous {
        Some(engine) => engine.recreate(),
        None => make_engine(config, thread_budget, gpu),
    }
    .or_else(|error| {
        if !use_gpu || !worker::gpu_failure(error.kind()) {
            return Err(error);
        }
        acceleration.retire();
        worker_event(
            "ocr.gpu_fallback",
            0,
            "preparation",
            "cpu",
            "fallback",
            0,
            &error.to_string(),
        );
        make_engine(config, thread_budget, None)
    })
    .map_err(|e| e.to_string());
    worker_event(
        "ocr.engine_ready",
        0,
        if reused {
            "request_reset"
        } else {
            "initialization"
        },
        result.as_ref().map_or("cpu", Engine::backend),
        if result.is_ok() {
            "succeeded"
        } else {
            "failed"
        },
        started.elapsed().as_millis(),
        result.as_ref().err().map_or("", String::as_str),
    );
    result.ok()
}

fn session_config(payload: &[u8]) -> io::Result<Config> {
    let mut d = Decoder::new(payload);
    let detector_resize_policy = DetectorResizePolicy::from_wire(d.u8()?)?;
    let text_detection_processing = TextDetectionProcessing::from_wire(d.u8()?)?;
    let hardware_acceleration = wire_boolean(d.u8()?)?;
    let detector_model = PathBuf::from(d.string()?);
    let recognizer_model = PathBuf::from(d.string()?);
    let dictionary = PathBuf::from(d.string()?);
    if !d.done() {
        return Err(io::Error::other("invalid OCR session configuration"));
    }
    Ok(Config {
        detector_resize_policy,
        text_detection_processing,
        hardware_acceleration,
        detector_model,
        recognizer_model,
        dictionary,
    })
}
fn wire_boolean(value: u8) -> io::Result<bool> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid OCR boolean",
        )),
    }
}
fn ready_payload() -> Vec<u8> {
    let mut p = Vec::new();
    put_u8(&mut p, 1);
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
            put_string(&mut p, "");
            put_u32(&mut p, result.len() as u32);
            for line in result {
                put_string(&mut p, &line.text);
                put_f32(&mut p, line.score);
                for point in line.points.iter().flatten() {
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
    if let StartupMode::ValidateModelSet {
        detector,
        recognizer,
        dictionary,
    } = startup_mode
    {
        make_engine_for_models(
            &detector,
            &recognizer,
            &dictionary,
            1,
            DetectorResizePolicy::Max,
            TextDetectionProcessing::default(),
            None,
        )
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
    if !startup.payload.is_empty() || startup.request_id != 0 {
        return Err(io::Error::other("invalid OCR hello"));
    }
    let (work_tx, work_rx) = mpsc::channel();
    let (event_tx, event_rx) = mpsc::channel();
    let result_tx = event_tx.clone();
    let acceleration = acceleration::Acceleration::new();
    let worker_acceleration = acceleration.clone();
    let worker = thread::Builder::new()
        .name("snow-ocr-inference".into())
        .spawn(move || worker_loop(work_rx, result_tx, worker_acceleration))?;
    write_frame(&mut writer, Kind::Ready, 0, &ready_payload())?;
    let command_tx = event_tx.clone();
    thread::spawn(move || read_commands(reader, command_tx));
    let mut shared: Option<(u64, SharedImage)> = None;
    let mut staged: Option<(u64, InputImage)> = None;
    let mut active: Option<(u64, Arc<AtomicBool>)> = None;
    let mut last_sequence = 0;
    loop {
        let frame = match event_rx.recv() {
            Ok(Event::Command(frame)) => frame,
            Ok(Event::Completed(result)) => {
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
                continue;
            }
            Ok(Event::CommandsClosed) | Err(_) => break,
            Ok(Event::AccelerationReady) => {
                // Disabled/stale discoveries never change the host's session generation.
                if acceleration.snapshot().is_some() {
                    write_frame(&mut writer, Kind::AccelerationReady, 0, &[])?;
                }
                continue;
            }
        };
        match frame.kind {
            Kind::ConfigureAcceleration => {
                if frame.request_id != 0 || frame.payload.len() != 1 {
                    return Err(io::Error::other("invalid OCR acceleration configuration"));
                }
                let enabled = wire_boolean(frame.payload[0])?;
                let notification_tx = event_tx.clone();
                acceleration.configure(
                    enabled,
                    acceleration::initialize,
                    move |available, reason| {
                        worker_event(
                            "ocr.gpu_initialization",
                            0,
                            "device",
                            if available { "gpu" } else { "cpu" },
                            if available {
                                "succeeded"
                            } else {
                                "unavailable"
                            },
                            0,
                            &reason,
                        );
                        if available {
                            let _ = notification_tx.send(Event::AccelerationReady);
                        }
                    },
                );
            }
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
                    InputImage {
                        width,
                        height,
                        pixels: data,
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
                        session_config(&frame.payload)?,
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
    use super::{DetectorResizePolicy, TextDetectionProcessing, session_config};
    use crate::protocol::{put_string, put_u8};

    #[test]
    fn command_eof_is_an_event_even_while_the_worker_sender_is_alive() {
        let (tx, rx) = std::sync::mpsc::channel();
        let worker_sender = tx.clone();
        let mut commands = Vec::new();
        super::write_frame(&mut commands, super::Kind::ReleaseSession, 17, &[]).unwrap();
        super::read_commands(std::io::Cursor::new(commands), tx);
        assert!(matches!(rx.recv().unwrap(), super::Event::Command(frame)
            if frame.kind == super::Kind::ReleaseSession && frame.request_id == 17));
        assert!(matches!(rx.recv().unwrap(), super::Event::CommandsClosed));
        drop(worker_sender);
        assert!(rx.recv().is_err());
    }

    #[test]
    fn worker_results_wake_the_same_channel_without_another_command() {
        let (work_tx, work_rx) = std::sync::mpsc::channel();
        let (event_tx, event_rx) = std::sync::mpsc::channel();
        let command_sender = event_tx.clone();
        let worker = std::thread::spawn(move || {
            super::worker_loop(work_rx, event_tx, crate::acceleration::Acceleration::new())
        });
        work_tx.send(super::Work::Release(42)).unwrap();
        assert!(matches!(
            event_rx
                .recv_timeout(std::time::Duration::from_secs(5))
                .unwrap(),
            super::Event::Completed(super::WorkResult::Released(42))
        ));
        drop(work_tx);
        worker.join().unwrap();
        drop(command_sender);
        assert!(event_rx.recv().is_err());
    }

    fn session_payload(detector_resize_policy: u8, text_detection_processing: u8) -> Vec<u8> {
        let mut payload = Vec::new();
        put_u8(&mut payload, detector_resize_policy);
        put_u8(&mut payload, text_detection_processing);
        put_u8(&mut payload, 0);
        for path in ["det.onnx", "rec.onnx", "dict.txt"] {
            put_string(&mut payload, path);
        }
        payload
    }

    #[test]
    fn session_config_selects_and_validates_detector_resize_policy() {
        for (wire, expected) in [
            (0, DetectorResizePolicy::Max),
            (1, DetectorResizePolicy::Min),
        ] {
            let mut payload = session_payload(wire, 0);
            let config = session_config(&payload).unwrap();
            assert_eq!(config.detector_resize_policy, expected);
            payload[0] = 2;
            assert!(session_config(&payload).is_err());
        }
    }

    #[test]
    fn session_config_selects_and_validates_text_detection_processing() {
        for (wire, expected) in [
            (0, TextDetectionProcessing::AccuracyFirst),
            (1, TextDetectionProcessing::SpeedFirst),
        ] {
            for detector_policy in [0, 1] {
                let mut payload = session_payload(detector_policy, wire);
                let config = session_config(&payload).unwrap();
                assert_eq!(config.text_detection_processing, expected);
                assert_eq!(config.detector_model, PathBuf::from("det.onnx"));
                assert_eq!(config.recognizer_model, PathBuf::from("rec.onnx"));
                assert_eq!(config.dictionary, PathBuf::from("dict.txt"));
                payload[1] = 2;
                assert_eq!(
                    session_config(&payload).err().unwrap().kind(),
                    io::ErrorKind::InvalidData
                );
            }
        }
    }

    #[test]
    fn session_acceleration_is_explicit_and_strictly_boolean() {
        let mut payload = session_payload(0, 0);
        assert!(!session_config(&payload).unwrap().hardware_acceleration);
        payload[2] = 1;
        assert!(session_config(&payload).unwrap().hardware_acceleration);
        payload[2] = 2;
        assert_eq!(
            session_config(&payload).err().unwrap().kind(),
            io::ErrorKind::InvalidData
        );
    }

    #[test]
    fn session_config_rejects_missing_processing_and_truncated_paths() {
        for processing in [0, 1] {
            let payload = session_payload(0, processing);
            for length in 0..payload.len() {
                assert_eq!(
                    session_config(&payload[..length]).err().unwrap().kind(),
                    io::ErrorKind::UnexpectedEof,
                    "truncated session configuration length {length}"
                );
            }
        }
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
