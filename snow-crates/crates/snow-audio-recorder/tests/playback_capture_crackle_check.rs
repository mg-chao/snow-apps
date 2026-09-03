#![cfg(windows)]
//! End-to-end capture fidelity check: play the clean `sample-15s.wav` fixture
//! through the default render device, record the system output with the real
//! [`AudioRecordingSession`] capture path, and verify the captured track
//! contains no buzzing distortion.
//!
//! Known failure (expected test result: FAIL until fixed): the capture
//! timeline intermittently slips by a few samples, displacing the recorded
//! waveform and producing a faint buzzing/crackling distortion during
//! playback. Measured on a real recording on 2026-09-03: ten bursts of 20 ms
//! residual between -16 and -25 dBFS (baseline: -42 dBFS), plus a ~1 s
//! time-warp at the start of playback. The bursts correlate with the signal
//! slope, i.e. local timing displacement, not added noise.
//!
//! The test renders at the endpoint mix format (no engine sample-rate
//! conversion) and compares the capture against the exact samples that were
//! written to the endpoint, so the residual isolates the capture path: any
//! 20 ms window whose residual exceeds -25 dBFS is a timing slip.
//!
//! The loopback taps the whole endpoint mix, so the test refuses to measure
//! while other audio is playing (up to three takes are attempted when the
//! endpoint is quiet at probe time but becomes busy mid-run); a take whose
//! baseline residual is polluted is discarded rather than judged.
//!
//! Requires a working default stereo audio render device (the same one the
//! loopback capture listens to); keep the system quiet while audio plays.

use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use snow_audio_recorder::{AudioRecordingConfig, AudioRecordingSession, AudioTrackConfig};
use snow_core::recording_clock::RecordingClock;
use windows::Win32::Media::Audio::{
    AUDCLNT_BUFFERFLAGS_SILENT, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
    AUDCLNT_STREAMFLAGS_NOPERSIST, IAudioCaptureClient, IAudioClient, IAudioRenderClient,
    IMMDeviceEnumerator, MMDeviceEnumerator, WAVEFORMATEX, eConsole, eRender,
};
use windows::Win32::System::Com::{
    CLSCTX_ALL, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx, CoTaskMemFree,
};

const FIXTURE_WAV: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/sample-15s.wav");
const OUTPUT_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/output/playback-capture");

/// A burst block whose residual exceeds this level is audible buzzing.
const BURST_BLOCK_DBFS: f64 = -25.0;

#[test]
fn captured_playback_of_clean_source_is_free_of_crackle() {
    let source = read_wav_pcm16(Path::new(FIXTURE_WAV))
        .unwrap_or_else(|err| panic!("failed to read WAV fixture {FIXTURE_WAV}: {err}"));
    let source_frames = source.samples.len() / usize::from(source.channels);
    println!(
        "source fixture: {} Hz, {} ch, {:.3} s ({} frames)",
        source.sample_rate_hz,
        source.channels,
        source_frames as f64 / f64::from(source.sample_rate_hz),
        source_frames
    );

    let (mix_rate_hz, mix_channels) = probe_mix_format()
        .unwrap_or_else(|err| panic!("audio render device probe failed (needs a device): {err}"));
    assert_eq!(
        mix_channels, 2,
        "this check requires a stereo default render device"
    );
    println!("endpoint mix format: {} Hz stereo", mix_rate_hz);

    // The loopback taps the whole endpoint mix: any other audio playing on
    // the system would pollute the measurement, so a take that fails the
    // baseline check is retried (up to three full attempts).
    let mut last_report = None;
    for attempt in 1..=3 {
        let mut quiet = false;
        for probe in 1..=20 {
            let noise = measure_loopback_noise()
                .unwrap_or_else(|err| panic!("loopback noise probe failed: {err}"));
            println!(
                "attempt {attempt}, loopback pre-check {probe}/20: endpoint peak {noise:.1} dBFS"
            );
            if noise <= -60.0 {
                quiet = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(1500));
        }
        if !quiet {
            println!(
                "SKIPPED: other audio is playing on the system; the loopback would capture it and the measurement would be meaningless. Pause all playback and re-run."
            );
            return;
        }

        let report = measure_once(&source, mix_rate_hz);
        println!(
            "crackle report (attempt {attempt}): playback onset {:.3}s, capture gain {:.3}, median 20ms-block residual {:.1} dBFS, worst block {:.1} dBFS",
            report.playback_onset_s,
            report.capture_gain,
            report.median_block_dbfs,
            report.worst_block_dbfs
        );
        println!("crackle bursts: {:?}", report.bursts);

        if report.median_block_dbfs > -38.0 {
            println!(
                "attempt {attempt} polluted: baseline residual {:.1} dBFS is far above the clean floor (-50 dBFS or lower), so other audio played during the run.",
                report.median_block_dbfs
            );
            last_report = Some(report);
            continue;
        }
        last_report = Some(report);
        break;
    }

    let report = last_report.expect("at least one measurement attempt should have run");
    if report.median_block_dbfs > -38.0 {
        println!(
            "SKIPPED: every attempt had a polluted baseline (other audio was playing throughout). Re-run on a quiet system."
        );
        return;
    }

    assert!(
        report.bursts.is_empty(),
        "captured audio contains buzzing distortion: {} crackle burst(s) caused by capture timing slips, worst {:.1} dBFS at {:.2}s into the playback (bursts at {:?}); expected: no bursts. This documents the known capture timing-slip defect.",
        report.bursts.len(),
        report.worst_burst_dbfs.unwrap_or(f64::NAN),
        report.worst_burst_time.unwrap_or(f64::NAN),
        report
            .bursts
            .iter()
            .map(|b| format!("{:.2}-{:.2}s", b.start_s, b.end_s))
            .collect::<Vec<_>>()
            .join(", ")
    );
}

/// One full take: record the system output, play the fixture, analyze.
fn measure_once(source: &WavInput, mix_rate_hz: u32) -> CrackleReport {
    let output_dir = PathBuf::from(OUTPUT_DIR);
    std::fs::create_dir_all(&output_dir).expect("output directory should be created");
    let config = AudioRecordingConfig {
        output_dir: output_dir.clone(),
        sample_rate_hz: mix_rate_hz,
        tracks: vec![AudioTrackConfig::system_default("system")],
        ..AudioRecordingConfig::default()
    };
    let session = AudioRecordingSession::start(config, RecordingClock::new(Instant::now()))
        .unwrap_or_else(|err| {
            panic!("audio recording should start (needs an audio device): {err}")
        });

    let rendered = play_samples_via_wasapi(source, mix_rate_hz)
        .unwrap_or_else(|err| panic!("WAV playback should work: {err}"));
    std::thread::sleep(Duration::from_millis(200));

    let artifact = session
        .finish()
        .expect("audio recording should finish cleanly");
    let track = artifact
        .tracks
        .iter()
        .find(|track| track.manifest.track_id == "system")
        .expect("system track should have been recorded");
    let recorded = read_raw_pcm_i16(&track.path);
    let recorded_frames = recorded.len() / usize::from(track.manifest.channels);
    println!(
        "captured track: {} Hz, {} ch, {:.3} s ({} frames) -> {}",
        track.manifest.sample_rate_hz,
        track.manifest.channels,
        recorded_frames as f64 / f64::from(track.manifest.sample_rate_hz),
        recorded_frames,
        track.path.display()
    );
    let rendered_frames = rendered.len() as f64 / 2.0;
    assert!(
        recorded_frames as f64 >= rendered_frames * 0.98,
        "captured track is shorter than the played content; playback was cut off"
    );

    detect_crackle(&rendered, &recorded, track.manifest.sample_rate_hz)
}

#[derive(Debug)]
struct CrackleReport {
    playback_onset_s: f64,
    capture_gain: f64,
    median_block_dbfs: f64,
    worst_block_dbfs: f64,
    bursts: Vec<BurstInfo>,
    worst_burst_dbfs: Option<f64>,
    worst_burst_time: Option<f64>,
}

#[derive(Debug)]
struct BurstInfo {
    start_s: f64,
    end_s: f64,
    peak_dbfs: f64,
}

/// Compares the exact samples rendered to the endpoint against the captured
/// track and locates every 20 ms window whose residual (captured minus
/// rendered, after gain normalization) exceeds the crackle threshold. A
/// residual that peaks far above the baseline while local alignment cannot
/// remove it means the captured timeline slipped.
fn detect_crackle(rendered: &[f32], recorded: &[i16], rate_hz: u32) -> CrackleReport {
    let reference: Vec<f64> = rendered
        .chunks_exact(2)
        .map(|frame| (f64::from(frame[0]) + f64::from(frame[1])) / 2.0)
        .collect();
    let captured: Vec<f64> = recorded
        .chunks_exact(2)
        .map(|frame| (f64::from(frame[0]) + f64::from(frame[1])) / (2.0 * 32768.0))
        .collect();
    let rate = f64::from(rate_hz);
    let block = rate_hz as usize / 50; // 20 ms

    let onset = playback_onset(&captured, rate_hz, block, -45.0, Duration::from_millis(160))
        .unwrap_or_else(|| {
            panic!("captured track contains no audible playback; the render produced silence")
        });
    println!(
        "playback onset in capture: sample {onset} ({:.3} s)",
        onset as f64 / rate
    );

    // Coarse alignment on the full-clip energy envelope (5 ms block RMS).
    // The fixture is strongly periodic (~130.8 Hz, one period = ~367 samples
    // at 48 kHz), so raw waveform correlation has many near-equal peaks;
    // the envelope over 19 s of content is unambiguous.
    let env_block = rate_hz as usize / 200; // 5 ms
    let env_ref = block_rms(&reference, env_block);
    let env_cap = block_rms(&captured, env_block);
    let max_shift_blocks = 100i64; // +/- 500 ms around the measured onset
    let mut coarse_blocks = 0i64;
    let mut coarse_score = f64::NEG_INFINITY;
    for shift in -max_shift_blocks..=max_shift_blocks {
        // captured[i] ~ reference[i - shift]
        let cap_lo = shift.max(0) as usize;
        let ref_lo = (-shift).max(0) as usize;
        let overlap = env_cap
            .len()
            .saturating_sub(cap_lo)
            .min(env_ref.len().saturating_sub(ref_lo));
        if overlap == 0 {
            continue;
        }
        let mut acc = 0.0;
        let mut norm_c = 0.0;
        let mut norm_r = 0.0;
        for i in 0..overlap {
            let c = env_cap[cap_lo + i];
            let r = env_ref[ref_lo + i];
            acc += c * r;
            norm_c += c * c;
            norm_r += r * r;
        }
        let score = acc / (norm_c.sqrt() * norm_r.sqrt() + 1e-12);
        if score > coarse_score {
            coarse_score = score;
            coarse_blocks = shift;
        }
    }
    let coarse = coarse_blocks * env_block as i64;
    // Sample-level refinement of the envelope estimate with a 1 s excerpt
    // (the true peak dominates the periodic aliases at this length).
    let excerpt_start = (reference.len() / 3).min(5 * rate_hz as usize);
    let excerpt_len = rate_hz as usize;
    let coarse = best_lag(
        &captured,
        &reference,
        onset + excerpt_start,
        excerpt_start,
        excerpt_len,
        coarse - 300,
        coarse + 300,
        1,
    );
    println!("coarse alignment lag: {coarse} samples");
    // The capture starts before the content, so a negative lag is never
    // legitimate; clamp instead of wrapping the tracker's usize casts.
    let coarse = coarse.max(0);

    // Fine per-block lag tracking, then a global least-squares gain fit so
    // endpoint volume differences do not count as distortion.
    let mut lag = coarse;
    let mut times = Vec::new();
    let mut blocks = Vec::new();
    let mut a = onset.saturating_sub(block / 2);
    while a + block < captured.len() {
        if a < lag as usize || a - lag as usize + block >= reference.len() {
            a += block;
            continue;
        }
        let r = &captured[a..a + block];
        let mut best = lag;
        let mut best_v = f64::NEG_INFINITY;
        for c in (lag - 64).max(0)..=(lag + 64) {
            if a < c as usize || a - c as usize + block >= reference.len() {
                continue;
            }
            let s = &reference[a - c as usize..a - c as usize + block];
            let v = dot(r, s);
            if v > best_v {
                best_v = v;
                best = c;
            }
        }
        let s = &reference[a - best as usize..a - best as usize + block];
        let mut acc_r = 0.0;
        let mut acc_rr = 0.0;
        for i in 0..block {
            acc_r += r[i] * s[i];
            acc_rr += s[i] * s[i];
        }
        blocks.push((a, best, acc_r, acc_rr));
        times.push(a as f64 / rate);
        lag = best;
        a += block;
    }
    let gain_num: f64 = blocks.iter().map(|(_, _, r, _)| *r).sum();
    let gain_den: f64 = blocks.iter().map(|(_, _, _, rr)| *rr).sum();
    let capture_gain = gain_num / gain_den.max(1e-12);

    let mut levels = Vec::with_capacity(blocks.len());
    for (a, best, _, _) in &blocks {
        let s = &reference[*a - *best as usize..*a - *best as usize + block];
        let r = &captured[*a..*a + block];
        let mut acc = 0.0;
        for i in 0..block {
            let d = r[i] - capture_gain * s[i];
            acc += d * d;
        }
        levels.push(amplitude_to_dbfs((acc / block as f64).sqrt()));
    }
    let mut sorted = levels.clone();
    sorted.sort_by(|a, b| a.total_cmp(b));
    assert!(
        !sorted.is_empty(),
        "no residual blocks could be aligned; alignment failed (coarse lag {coarse})"
    );
    let median_dbfs = sorted[sorted.len() / 2];
    let worst_block_dbfs = *sorted.last().expect("at least one residual block");

    let mut bursts = Vec::new();
    let mut idx = 0;
    while idx < times.len() {
        if levels[idx] > BURST_BLOCK_DBFS {
            let start = idx;
            // Extend the cluster only across blocks that are themselves
            // bursts; quiet stretches end it.
            let mut end = start;
            while end + 1 < times.len()
                && times[end + 1] - times[end] <= 0.06
                && levels[end + 1] > BURST_BLOCK_DBFS
            {
                end += 1;
            }
            let peak = levels[start..=end]
                .iter()
                .copied()
                .fold(f64::NEG_INFINITY, f64::max);
            bursts.push(BurstInfo {
                start_s: times[start],
                end_s: times[end] + block as f64 / rate,
                peak_dbfs: peak,
            });
            idx = end;
        }
        idx += 1;
    }
    let worst = bursts
        .iter()
        .enumerate()
        .max_by(|a, b| a.1.peak_dbfs.total_cmp(&b.1.peak_dbfs));
    let (worst_burst_dbfs, worst_burst_time) = match worst {
        Some((_, info)) => (Some(info.peak_dbfs), Some(info.start_s)),
        None => (None, None),
    };

    CrackleReport {
        playback_onset_s: onset as f64 / rate,
        capture_gain,
        median_block_dbfs: median_dbfs,
        worst_block_dbfs,
        bursts,
        worst_burst_dbfs,
        worst_burst_time,
    }
}

/// First index where the signal sustains `level_dbfs` for `sustain`.
fn playback_onset(
    x: &[f64],
    rate: u32,
    win: usize,
    level_dbfs: f64,
    sustain: Duration,
) -> Option<usize> {
    let needed = (sustain.as_secs_f64() * rate as f64 / win as f64).ceil() as usize;
    let mut run = 0;
    let mut i = 0;
    while i + win < x.len() {
        let rms = {
            let mut acc = 0.0;
            for v in &x[i..i + win] {
                acc += v * v;
            }
            (acc / win as f64).sqrt()
        };
        if amplitude_to_dbfs(rms) > level_dbfs {
            run += 1;
            if run >= needed {
                return Some(i.saturating_sub((needed - 1) * win));
            }
        } else {
            run = 0;
        }
        i += win;
    }
    None
}

#[allow(clippy::too_many_arguments)]
fn best_lag(
    rec: &[f64],
    src: &[f64],
    rec_start: usize,
    src_start: usize,
    len: usize,
    lag_lo: i64,
    lag_hi: i64,
    step: i64,
) -> i64 {
    let mut best = lag_lo;
    let mut best_v = f64::NEG_INFINITY;
    for lag in (lag_lo..=lag_hi).step_by(step.max(1) as usize) {
        let a = rec_start;
        let s = src_start as i64 - lag;
        if s < 0 || (a + len) > rec.len() || (s as usize + len) > src.len() {
            continue;
        }
        let r = &rec[a..a + len];
        let ss = &src[s as usize..s as usize + len];
        let v = dot(r, ss);
        if v > best_v {
            best_v = v;
            best = lag;
        }
    }
    best
}

fn dot(a: &[f64], b: &[f64]) -> f64 {
    a.iter().zip(b).map(|(x, y)| x * y).sum()
}

fn block_rms(x: &[f64], block: usize) -> Vec<f64> {
    x.chunks(block)
        .map(|chunk| {
            let mut acc = 0.0;
            for v in chunk {
                acc += v * v;
            }
            (acc / chunk.len() as f64).sqrt()
        })
        .collect()
}

fn amplitude_to_dbfs(amplitude: f64) -> f64 {
    20.0 * amplitude.max(1e-12).log10()
}

struct WavInput {
    sample_rate_hz: u32,
    channels: u16,
    samples: Vec<i16>,
}

fn read_wav_pcm16(path: &Path) -> Result<WavInput, String> {
    let mut bytes = Vec::new();
    File::open(path)
        .and_then(|mut f| f.read_to_end(&mut bytes))
        .map_err(|err| format!("{}: {err}", path.display()))?;
    let read_u16 = |at: usize| u16::from_le_bytes(bytes[at..at + 2].try_into().unwrap());
    let read_u32 = |at: usize| u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap());

    if bytes.len() < 12 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err("not a RIFF/WAVE file".to_string());
    }
    let mut fmt = None;
    let mut data = None;
    let mut pos = 12usize;
    while pos + 8 <= bytes.len() {
        let id = &bytes[pos..pos + 4];
        let size = read_u32(pos + 4) as usize;
        let body = pos + 8;
        if body + size > bytes.len() {
            return Err("truncated RIFF chunk".to_string());
        }
        let chunk = &bytes[body..body + size];
        match id {
            b"fmt " if chunk.len() >= 16 => {
                fmt = Some((
                    read_u16(body),
                    read_u16(body + 2),
                    read_u32(body + 4),
                    read_u16(body + 14),
                ));
            }
            b"data" => data = Some(chunk.to_vec()),
            _ => {}
        }
        pos = body + size + (size & 1);
    }
    let (format_tag, channels, sample_rate_hz, bits_per_sample) =
        fmt.ok_or_else(|| "WAV fixture has no fmt chunk".to_string())?;
    if format_tag != 1 {
        return Err(format!("unsupported WAV format tag {format_tag}"));
    }
    if bits_per_sample != 16 {
        return Err(format!("unsupported bit depth {bits_per_sample}"));
    }
    let samples = data
        .ok_or_else(|| "WAV fixture has no data chunk".to_string())?
        .chunks_exact(2)
        .map(|c| i16::from_le_bytes([c[0], c[1]]))
        .collect();
    Ok(WavInput {
        sample_rate_hz,
        channels,
        samples,
    })
}

fn read_raw_pcm_i16(path: &Path) -> Vec<i16> {
    let mut bytes = Vec::new();
    File::open(path)
        .and_then(|mut f| f.read_to_end(&mut bytes))
        .expect("recorded track file should be readable");
    bytes
        .chunks_exact(2)
        .map(|c| i16::from_le_bytes([c[0], c[1]]))
        .collect()
}

/// Peak level (dBFS) carried by the endpoint loopback over a 400 ms window;
/// used to detect other audio playing on the system before measuring.
fn measure_loopback_noise() -> Result<f64, String> {
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        let enumerator: IMMDeviceEnumerator =
            CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)
                .map_err(|err| format!("create device enumerator: {err}"))?;
        let device = enumerator
            .GetDefaultAudioEndpoint(eRender, eConsole)
            .map_err(|err| format!("no default audio render device: {err}"))?;
        let client: IAudioClient = device
            .Activate(CLSCTX_ALL, None)
            .map_err(|err| format!("activate audio client: {err}"))?;
        let mix_format_ptr = client
            .GetMixFormat()
            .map_err(|err| format!("get mix format: {err}"))?;
        let mix: &WAVEFORMATEX = &*mix_format_ptr;
        let channels = usize::from(mix.nChannels.max(1));
        client
            .Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                2_000_000,
                0,
                mix_format_ptr,
                None,
            )
            .map_err(|err| format!("initialize loopback capture: {err}"))?;
        let capture: IAudioCaptureClient = client
            .GetService()
            .map_err(|err| format!("get loopback capture client: {err}"))?;
        client
            .Start()
            .map_err(|err| format!("start loopback capture: {err}"))?;
        std::thread::sleep(Duration::from_millis(400));
        let mut peak = 0.0f64;
        loop {
            let next = capture
                .GetNextPacketSize()
                .map_err(|err| format!("get next packet size: {err}"))?;
            if next == 0 {
                break;
            }
            let mut data_ptr = std::ptr::null_mut::<u8>();
            let mut frames = 0u32;
            let mut flags = 0u32;
            let mut device_position = 0u64;
            let mut qpc_position = 0u64;
            capture
                .GetBuffer(
                    &mut data_ptr,
                    &mut frames,
                    &mut flags,
                    Some(&mut device_position),
                    Some(&mut qpc_position),
                )
                .map_err(|err| format!("loopback GetBuffer: {err}"))?;
            let silent = flags & AUDCLNT_BUFFERFLAGS_SILENT.0 as u32 != 0;
            if silent || data_ptr.is_null() {
                capture
                    .ReleaseBuffer(frames)
                    .map_err(|err| format!("loopback ReleaseBuffer: {err}"))?;
                continue;
            }
            let slice =
                std::slice::from_raw_parts(data_ptr as *const f32, frames as usize * channels);
            for v in slice {
                peak = peak.max(f64::from(v.abs()));
            }
            capture
                .ReleaseBuffer(frames)
                .map_err(|err| format!("loopback ReleaseBuffer: {err}"))?;
        }
        client
            .Stop()
            .map_err(|err| format!("stop loopback capture: {err}"))?;
        CoTaskMemFree(Some(mix_format_ptr.cast()));
        Ok(amplitude_to_dbfs(peak))
    }
}

/// Opens the default render device and reports its mix format.
fn probe_mix_format() -> Result<(u32, u16), String> {
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        let enumerator: IMMDeviceEnumerator =
            CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)
                .map_err(|err| format!("create device enumerator: {err}"))?;
        let device = enumerator
            .GetDefaultAudioEndpoint(eRender, eConsole)
            .map_err(|err| format!("no default audio render device: {err}"))?;
        let client: IAudioClient = device
            .Activate(CLSCTX_ALL, None)
            .map_err(|err| format!("activate audio client: {err}"))?;
        let mix_format_ptr = client
            .GetMixFormat()
            .map_err(|err| format!("get mix format: {err}"))?;
        assert!(
            !mix_format_ptr.is_null(),
            "endpoint returned a null mix format"
        );
        let mix: &WAVEFORMATEX = &*mix_format_ptr;
        let mix_rate = mix.nSamplesPerSec;
        let mix_channels = mix.nChannels;
        CoTaskMemFree(Some(mix_format_ptr.cast()));
        Ok((mix_rate, mix_channels))
    }
}

/// Plays the source on the default render device at the endpoint mix format
/// (float, no engine sample-rate conversion) and returns the exact
/// interleaved f32 stream that was written to the endpoint, so the analysis
/// can compare the capture against what actually entered it.
fn play_samples_via_wasapi(source: &WavInput, mix_rate_hz: u32) -> Result<Vec<f32>, String> {
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        let enumerator: IMMDeviceEnumerator =
            CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)
                .map_err(|err| format!("create device enumerator: {err}"))?;
        let device = enumerator
            .GetDefaultAudioEndpoint(eRender, eConsole)
            .map_err(|err| format!("no default audio render device: {err}"))?;
        let client: IAudioClient = device
            .Activate(CLSCTX_ALL, None)
            .map_err(|err| format!("activate audio client: {err}"))?;
        let mix_format_ptr = client
            .GetMixFormat()
            .map_err(|err| format!("get mix format: {err}"))?;
        assert!(
            !mix_format_ptr.is_null(),
            "endpoint returned a null mix format"
        );
        let mix: &WAVEFORMATEX = &*mix_format_ptr;
        let mix_rate = mix.nSamplesPerSec;
        let mix_channels = mix.nChannels;
        if mix_rate != mix_rate_hz {
            CoTaskMemFree(Some(mix_format_ptr.cast()));
            return Err(format!(
                "mix format changed between probe and play: now {mix_rate} Hz / {mix_channels} ch"
            ));
        }

        // Resample each source channel to the mix rate and interleave as f32.
        let left: Vec<f64> = source
            .samples
            .chunks_exact(mix_channels as usize)
            .map(|f| f64::from(f[0]))
            .collect();
        let right: Vec<f64> = source
            .samples
            .chunks_exact(mix_channels as usize)
            .map(|f| f64::from(f[1]))
            .collect();
        let left = resample_cubic(&left, source.sample_rate_hz, mix_rate);
        let right = resample_cubic(&right, source.sample_rate_hz, mix_rate);
        let rendered: Vec<f32> = left
            .iter()
            .zip(&right)
            .flat_map(|(l, r)| [(*l / 32768.0) as f32, (*r / 32768.0) as f32])
            .collect();

        client
            .Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_NOPERSIST,
                2_000_000,
                0,
                mix_format_ptr,
                None,
            )
            .map_err(|err| format!("initialize render client: {err}"))?;
        CoTaskMemFree(Some(mix_format_ptr.cast()));
        let render: IAudioRenderClient = client
            .GetService()
            .map_err(|err| format!("get render client: {err}"))?;
        client
            .Start()
            .map_err(|err| format!("start render: {err}"))?;

        let total_frames = rendered.len() / 2;
        let mut written: u32 = 0;
        while written < total_frames as u32 {
            let padding = client
                .GetCurrentPadding()
                .map_err(|err| format!("get current padding: {err}"))?;
            let buffer_frames = client
                .GetBufferSize()
                .map_err(|err| format!("get buffer size: {err}"))?;
            let available = buffer_frames.saturating_sub(padding);
            if available == 0 {
                std::thread::sleep(Duration::from_millis(10));
                continue;
            }
            let frames = available.min(total_frames as u32 - written);
            let start = written as usize * 2;
            let end = start + frames as usize * 2;
            let dst: *mut f32 = render
                .GetBuffer(frames)
                .map_err(|err| format!("get render buffer: {err}"))?
                .cast();
            std::ptr::copy_nonoverlapping(rendered[start..end].as_ptr(), dst, end - start);
            render
                .ReleaseBuffer(frames, 0)
                .map_err(|err| format!("release render buffer: {err}"))?;
            written += frames;
            std::thread::sleep(Duration::from_millis(10));
        }
        // Wait for the tail to drain out of the endpoint buffer.
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            let padding = client
                .GetCurrentPadding()
                .map_err(|err| format!("get current padding: {err}"))?;
            if padding == 0 || Instant::now() > deadline {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        client.Stop().map_err(|err| format!("stop render: {err}"))?;
        Ok(rendered)
    }
}

/// Catmull-Rom cubic interpolation, adequate as an alignment reference.
fn resample_cubic(x: &[f64], from_rate: u32, to_rate: u32) -> Vec<f64> {
    if from_rate == to_rate {
        return x.to_vec();
    }
    let ratio = f64::from(from_rate) / f64::from(to_rate);
    let out_len = ((x.len() as f64) / ratio).ceil() as usize;
    let mut out = Vec::with_capacity(out_len);
    for i in 0..out_len {
        let t = i as f64 * ratio;
        let i1 = t.floor() as usize;
        let frac = t - i1 as f64;
        let p0 = *x
            .get(i1.wrapping_sub(1))
            .unwrap_or(x.first().unwrap_or(&0.0));
        let p1 = *x.get(i1).unwrap_or(x.last().unwrap_or(&0.0));
        let p2 = *x.get(i1 + 1).unwrap_or(x.last().unwrap_or(&0.0));
        let p3 = *x.get(i1 + 2).unwrap_or(x.last().unwrap_or(&0.0));
        let v = 0.5
            * ((2.0 * p1)
                + (-p0 + p2) * frac
                + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * frac * frac
                + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * frac * frac * frac);
        out.push(v);
    }
    out
}
