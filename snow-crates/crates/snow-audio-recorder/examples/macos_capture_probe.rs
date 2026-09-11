//! Interactive macOS capture check. Run from an app bundle with microphone and screen permissions.
//! Play a known tone while running to check that the resulting system WAV contains real audio.
//! Example: macos_capture_probe --system --microphone --output-dir /tmp/snow-audio-probe
#[cfg(target_os = "macos")]
fn main() -> anyhow::Result<()> {
    use std::io::Write;
    use std::time::{Duration, Instant};

    use snow_audio_recorder::{
        AudioEvent, AudioSession, AudioSourceKind, AudioStreamConfig, DeviceSelector,
        RecvTimeoutError,
    };

    let args: Vec<String> = std::env::args().collect();
    let option = |name: &str| {
        args.windows(2)
            .find(|pair| pair[0] == name)
            .map(|pair| pair[1].clone())
    };
    let seconds = option("--seconds")
        .unwrap_or_else(|| "5".into())
        .parse::<u64>()?;
    anyhow::ensure!(
        (1..=60).contains(&seconds),
        "duration must be between 1 and 60 seconds"
    );
    let output_dir = option("--output-dir").map(std::path::PathBuf::from);
    if let Some(directory) = &output_dir {
        std::fs::create_dir_all(directory)?;
    }
    let mut config = AudioStreamConfig::default();
    config.system.enabled = args.iter().any(|arg| arg == "--system");
    config.microphone.enabled = args.iter().any(|arg| arg == "--microphone");
    if let Some(uid) = option("--microphone-id") {
        config.microphone.device = DeviceSelector::Id(uid);
    }
    anyhow::ensure!(
        config.system.enabled || config.microphone.enabled,
        "select --system and/or --microphone"
    );
    let session = AudioSession::new()?;
    println!("microphones: {:?}", session.enumerate_capture_devices()?);
    let stream = session.start_streaming(config.clone())?;
    let mut samples = [Vec::<i16>::new(), Vec::<i16>::new()];
    let mut packets = [0_u64; 2];
    let mut previous_time = [None, None];
    let mut first_packet = None;
    let startup = Instant::now();
    loop {
        if let Some(start) = first_packet {
            if Instant::now().duration_since(start) >= Duration::from_secs(seconds) {
                break;
            }
        } else {
            anyhow::ensure!(
                startup.elapsed() < Duration::from_secs(60),
                "no audio packets received within 60 seconds"
            );
        }
        match stream.recv_timeout(Duration::from_millis(500)) {
            Ok(AudioEvent::Packet(packet)) => {
                let index = usize::from(packet.source == AudioSourceKind::Microphone);
                let format = if index == 0 {
                    config.system.output_format
                } else {
                    config.microphone.output_format
                };
                anyhow::ensure!(packet.format == format, "unexpected audio format");
                anyhow::ensure!(
                    packet.data.len() == packet.frames as usize * usize::from(format.channels),
                    "inconsistent PCM length"
                );
                let timestamp = packet
                    .end_capture_time()
                    .ok_or_else(|| anyhow::anyhow!("missing packet timestamp"))?;
                if let Some(previous) = previous_time[index] {
                    anyhow::ensure!(timestamp >= previous, "audio timestamps moved backwards");
                }
                previous_time[index] = Some(timestamp);
                packets[index] += 1;
                samples[index].extend_from_slice(&packet.data);
                first_packet.get_or_insert_with(Instant::now);
            }
            Ok(AudioEvent::Error(error)) => return Err(error.into()),
            Ok(AudioEvent::StreamEnded) => anyhow::bail!("audio stream ended early"),
            Ok(event) => println!("event: {event:?}"),
            Err(RecvTimeoutError::Timeout) => {}
            Err(error) => return Err(error.into()),
        }
    }
    stream.stop();
    drop(stream);
    for (index, name, source) in [
        (0, "system", &config.system),
        (1, "microphone", &config.microphone),
    ] {
        if !source.enabled {
            continue;
        }
        anyhow::ensure!(packets[index] > 0, "{name} produced no packets");
        let pcm = &samples[index];
        let peak = pcm
            .iter()
            .map(|sample| i32::from(*sample).abs())
            .max()
            .unwrap_or(0);
        let rms = (pcm
            .iter()
            .map(|sample| f64::from(*sample).powi(2))
            .sum::<f64>()
            / pcm.len() as f64)
            .sqrt();
        println!(
            "{name}: packets={} frames={} peak={peak} rms={rms:.2}",
            packets[index],
            pcm.len() / usize::from(source.output_format.channels)
        );
        if let Some(directory) = &output_dir {
            let mut file = std::fs::File::create(directory.join(format!("{name}.wav")))?;
            let bytes = u32::try_from(pcm.len() * 2)?;
            file.write_all(b"RIFF")?;
            file.write_all(&(36 + bytes).to_le_bytes())?;
            file.write_all(b"WAVEfmt ")?;
            file.write_all(&16_u32.to_le_bytes())?;
            file.write_all(&1_u16.to_le_bytes())?;
            file.write_all(&source.output_format.channels.to_le_bytes())?;
            file.write_all(&source.output_format.sample_rate.to_le_bytes())?;
            let alignment = source.output_format.channels * 2;
            file.write_all(
                &(source.output_format.sample_rate * u32::from(alignment)).to_le_bytes(),
            )?;
            file.write_all(&alignment.to_le_bytes())?;
            file.write_all(&16_u16.to_le_bytes())?;
            file.write_all(b"data")?;
            file.write_all(&bytes.to_le_bytes())?;
            for sample in pcm {
                file.write_all(&sample.to_le_bytes())?;
            }
        }
    }
    Ok(())
}

#[cfg(not(target_os = "macos"))]
fn main() {
    eprintln!("This capture probe requires macOS.");
}
