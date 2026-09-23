//! Main-run-loop host for explicit macOS capture acceptance checks.
#[cfg(not(target_os = "macos"))]
fn main() {
    eprintln!("This harness requires macOS 15 or later.");
}
#[cfg(target_os = "macos")]
fn main() {
    use snow_macos::{
        MacResult,
        capture::{CaptureConfig, Target, VideoStream},
        content, permission,
    };
    use std::time::Duration;
    let args: Vec<_> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some("request-screen") {
        println!(
            "screen_capture_authorized={}",
            permission::request_screen_capture_access()
        );
        return;
    }
    if args.is_empty() || args[0] == "status" {
        println!(
            "macos_15_plus={} architecture={} screen_authorized={} microphone_authorized={} input_authorized={}",
            permission::supported_os(),
            std::env::consts::ARCH,
            permission::screen_capture_authorized(),
            snow_macos::microphone::microphone_authorized(),
            snow_macos::input::authorized()
        );
        return;
    }
    if args[0] == "request-input" {
        println!("input_authorized={}", snow_macos::input::request_access());
        return;
    }
    if args[0] == "request-microphone" {
        let done = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let complete = done.clone();
        snow_macos::microphone::request_microphone_access(move |allowed| {
            println!("microphone_authorized={allowed}");
            complete.store(true, std::sync::atomic::Ordering::Release);
        });
        snow_macos::run_loop::drive_until(|| done.load(std::sync::atomic::Ordering::Acquire));
        return;
    }
    let worker = std::thread::spawn(move || -> MacResult<()> {
        if args[0] == "inputs" {
            for device in snow_macos::microphone::input_devices()? {
                println!(
                    "uid={} name={} default={}",
                    device.uid, device.name, device.is_default
                );
            }
            return Ok(());
        }
        if args[0] == "microphone" {
            let stream =
                snow_macos::microphone::MicrophoneStream::start(args.get(1).map(String::as_str))?;
            let started = std::time::Instant::now();
            let mut packets = 0u64;
            let mut discontinuities = 0u64;
            let mut duration = 0.0;
            let mut first = None;
            let mut end = None;
            while started.elapsed() < Duration::from_secs(3) {
                let sample = stream.next_samples(Duration::from_secs(1))?;
                let seconds = sample.data.len() as f64
                    / f64::from(sample.channels)
                    / f64::from(sample.sample_rate);
                if first.is_none() {
                    first = Some(sample.timestamp);
                }
                end = sample
                    .timestamp
                    .checked_add_duration(Duration::from_secs_f64(seconds));
                duration += seconds;
                packets += 1;
                discontinuities += u64::from(sample.discontinuity);
            }
            let host = end
                .zip(first)
                .and_then(|(end, first)| end.duration_since(first))
                .ok_or(snow_macos::MacError::Inactive)?
                .as_secs_f64();
            println!(
                "microphone_packets={packets} pcm_seconds={duration:.6} host_seconds={host:.6} discontinuities={discontinuities}"
            );
            return Ok(());
        }
        if args[0] == "input" {
            let observer = snow_macos::input::InputObserver::start(true, true)?;
            let started = std::time::Instant::now();
            let mut events = 0u64;
            while started.elapsed() < Duration::from_secs(3) {
                if observer
                    .events
                    .recv_timeout(Duration::from_millis(100))
                    .is_ok()
                {
                    events += 1;
                }
            }
            println!(
                "input_events={events} status={:?} drops={}",
                observer.status(),
                observer.dropped()
            );
            return Ok(());
        }
        if args[0] == "benchmark-desktop" {
            if cfg!(debug_assertions) {
                return Err(snow_macos::MacError::InvalidConfig(
                    "benchmarks require --release".into(),
                ));
            }
            use snow_macos::desktop::{DesktopConfig, DesktopSession, DesktopTarget};
            let mut setup = Vec::new();
            let mut first = Vec::new();
            let mut warm = Vec::new();
            for _ in 0..16 {
                let mut config = DesktopConfig::new(DesktopTarget::PrimaryDisplay);
                config.opaque = true;
                let started = std::time::Instant::now();
                let mut session = DesktopSession::new(config)?;
                setup.push(started.elapsed().as_secs_f64() * 1000.0);
                let started = std::time::Instant::now();
                std::hint::black_box(session.snapshot()?);
                first.push(started.elapsed().as_secs_f64() * 1000.0);
                let started = std::time::Instant::now();
                std::hint::black_box(session.snapshot()?);
                warm.push(started.elapsed().as_secs_f64() * 1000.0);
            }
            for (label, mut samples) in [("setup", setup), ("first", first), ("warm", warm)] {
                samples.sort_by(f64::total_cmp);
                println!(
                    "desktop_{label}_samples={} p50_ms={:.3} p95_ms={:.3}",
                    samples.len(),
                    samples[samples.len() / 2],
                    samples[samples.len() * 95 / 100]
                );
            }
            return Ok(());
        }
        let displays = content::displays(Duration::from_secs(5))?;
        if args[0] == "list" {
            for display in displays {
                println!(
                    "display={} points={:?} pixels={:?} primary={}",
                    display.id, display.bounds, display.pixels, display.primary
                );
            }
            for window in content::windows(Duration::from_secs(5))? {
                println!(
                    "window={} pid={} points={:?} on_screen={}",
                    window.id, window.process_id, window.bounds, window.on_screen
                );
            }
            return Ok(());
        }
        let target = if args[0] == "window" {
            Target::Window(args.get(1).and_then(|s| s.parse().ok()).ok_or_else(|| {
                snow_macos::MacError::InvalidConfig(
                    "window requires an enumerated window ID".into(),
                )
            })?)
        } else {
            Target::Display(
                displays
                    .iter()
                    .find(|d| d.primary)
                    .ok_or(snow_macos::MacError::TargetUnavailable)?
                    .id,
            )
        };
        let mut config = CaptureConfig::new(target);
        if args.iter().any(|s| s == "hdr") {
            config.dynamic_range = snow_media::DynamicRange::Hdr;
        }
        if args[0] == "benchmark" {
            if cfg!(debug_assertions) {
                return Err(snow_macos::MacError::InvalidConfig(
                    "benchmarks require --release".into(),
                ));
            }
            let mut samples = Vec::new();
            for _ in 0..30 {
                let begin = std::time::Instant::now();
                let frame = snow_macos::capture::screenshot(&config)?;
                samples.push(begin.elapsed().as_secs_f64() * 1000.0);
                drop(frame);
            }
            let cold = samples.remove(0);
            samples.sort_by(f64::total_cmp);
            println!(
                "snapshot_samples=30 cold_ms={cold:.3} warm_p50_ms={:.3} warm_p95_ms={:.3} warm_p99_ms={:.3} cpu_readbacks=0",
                samples[samples.len() / 2],
                samples[samples.len() * 95 / 100],
                samples[samples.len() * 99 / 100]
            );
            for size in [(1920, 1080), (3840, 2160)] {
                config.output = Some(snow_media::geometry::PixelSize::new(size.0, size.1).unwrap());
                let mut stream = VideoStream::start(&config)?;
                let start = std::time::Instant::now();
                let mut count = 0;
                let mut age = Vec::new();
                while start.elapsed() < Duration::from_secs(5) {
                    match stream.next_frame(Duration::from_millis(100)) {
                        Ok(frame) => {
                            count += 1;
                            age.push(frame.acquired_at.elapsed().as_secs_f64() * 1000.0);
                        }
                        Err(snow_macos::MacError::Timeout) => {}
                        Err(e) => return Err(e),
                    }
                }
                let elapsed = start.elapsed().as_secs_f64();
                stream.stop()?;
                age.sort_by(f64::total_cmp);
                if age.is_empty() {
                    return Err(snow_macos::MacError::Timeout);
                }
                println!(
                    "stream={}x{} frames={count} fps={:.2} source_age_p50_ms={:.3} source_age_p95_ms={:.3} drops={} cpu_readbacks=0",
                    size.0,
                    size.1,
                    count as f64 / elapsed,
                    age[age.len() / 2],
                    age[age.len() * 95 / 100],
                    stream.dropped_frames()
                );
            }
            return Ok(());
        }
        if args[0] == "stream" {
            println!("starting_stream");
            let mut stream = VideoStream::start(&config)?;
            println!("stream_started");
            let mut last = None;
            let start = std::time::Instant::now();
            let mut frames = 0;
            while start.elapsed() < Duration::from_secs(5) {
                match stream.next_frame(Duration::from_millis(100)) {
                    Ok(frame) => {
                        frames += 1;
                        last = Some(frame);
                    }
                    Err(snow_macos::MacError::Timeout) => {}
                    Err(error) => return Err(error),
                }
            }
            stream.stop()?;
            let dropped = stream.dropped_frames();
            drop(stream);
            let retained = last.ok_or(snow_macos::MacError::Timeout)?;
            let cpu = retained
                .image
                .to_cpu()
                .map_err(|e| snow_macos::MacError::Unsupported(e.to_string()))?;
            println!(
                "frames={frames} dropped={dropped} retained_after_stop_bytes={}",
                cpu.bytes.len()
            );
        } else {
            let begin = std::time::Instant::now();
            println!("acquiring_snapshot range={:?}", config.dynamic_range);
            let frame = snow_macos::capture::screenshot(&config)?;
            let acquired_ms = begin.elapsed().as_secs_f64() * 1000.0;
            let cpu = frame
                .image
                .to_cpu()
                .map_err(|e| snow_macos::MacError::Unsupported(e.to_string()))?;
            println!(
                "snapshot={:?} format={:?} color={:?} bytes={} acquisition_ms={acquired_ms:.3}",
                cpu.size,
                cpu.format,
                cpu.color,
                cpu.bytes.len()
            );
        }
        Ok(())
    });
    while !worker.is_finished() {
        unsafe {
            objc2_core_foundation::CFRunLoop::run_in_mode(
                objc2_core_foundation::kCFRunLoopDefaultMode,
                0.02,
                false,
            );
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    match worker.join() {
        Ok(Ok(())) => {}
        Ok(Err(error)) => {
            eprintln!("{error}");
            std::process::exit(1);
        }
        Err(_) => {
            eprintln!("capture worker panicked");
            std::process::exit(2);
        }
    }
}
