#![cfg(windows)]
//! Explicit hardware smoke: no playback, persisted samples, or provider calls.

use snow_audio_recorder::{AudioEvent, AudioSession, AudioSourceKind, AudioStreamConfig};
use std::time::{Duration, Instant};

#[test]
#[ignore = "opens default Windows audio endpoints for one second; run explicitly"]
fn default_endpoints_enumerate_stream_and_stop() {
    let session = AudioSession::new().expect("construct native audio session");
    let render = session
        .enumerate_render_devices()
        .expect("enumerate render endpoints");
    let capture = session
        .enumerate_capture_devices()
        .expect("enumerate capture endpoints");
    println!(
        "active render endpoints={}, capture endpoints={}",
        render.len(),
        capture.len()
    );
    for (source, available) in [
        (
            AudioSourceKind::System,
            render
                .iter()
                .any(|device| device.is_default && device.is_active),
        ),
        (
            AudioSourceKind::Microphone,
            capture
                .iter()
                .any(|device| device.is_default && device.is_active),
        ),
    ] {
        if !available {
            println!("SKIP {source:?}: no active default endpoint");
            continue;
        }
        let mut config = AudioStreamConfig::default();
        config.system.enabled = source == AudioSourceKind::System;
        config.microphone.enabled = source == AudioSourceKind::Microphone;
        config.restart_policy.max_attempts = 1;
        let stream = session
            .start_streaming(config)
            .expect("start native endpoint stream");
        let deadline = Instant::now() + Duration::from_secs(1);
        let mut packets = 0_u64;
        let mut frames = 0_u64;
        while Instant::now() < deadline {
            match stream.recv_timeout(Duration::from_millis(50)) {
                Ok(AudioEvent::Packet(packet)) => {
                    assert_eq!(packet.source, source);
                    assert_eq!(
                        packet.data.len(),
                        packet.frames as usize * usize::from(packet.format.channels)
                    );
                    assert!(packet.format.sample_rate > 0);
                    packets += 1;
                    frames += u64::from(packet.frames);
                    // Samples are dropped immediately without reading their values.
                }
                Ok(AudioEvent::Error(error)) => panic!("native capture failed: {error}"),
                Ok(AudioEvent::StreamEnded) => panic!("native stream ended before stop"),
                Ok(_) | Err(snow_audio_recorder::RecvTimeoutError::Timeout) => {}
                Err(error) => panic!("native receive failed: {error:?}"),
            }
        }
        stream.pause();
        stream.resume();
        let tail = stream.stop_and_drain();
        assert!(
            !tail
                .iter()
                .any(|event| matches!(event, AudioEvent::Error(_)))
        );
        if source == AudioSourceKind::Microphone {
            assert!(packets > 0, "active microphone must supply framed packets");
        }
        println!("{source:?}: packets={packets} frames={frames}; pause/resume/stop succeeded");
        if source == AudioSourceKind::System && packets == 0 {
            println!("System packet delivery unverified: silent endpoint produced no packets");
        }
    }
}
