use std::time::{Duration, Instant};

use anyhow::Result;
use snow_audio_recorder::{
    AudioEvent, AudioSession, AudioStreamConfig, AudioTimestampAnchorExt, RecvTimeoutError,
    audio_anchor_from_origin_instant,
};

fn main() -> Result<()> {
    let session = AudioSession::new()?;

    let stream = session.start_streaming(AudioStreamConfig::default())?;
    let stats = stream.stats().clone();
    let stream_origin = Instant::now();
    let stream_anchor = audio_anchor_from_origin_instant(stream_origin);

    let start = Instant::now();
    println!("capturing system and microphone audio for 5 seconds...");

    while start.elapsed() < Duration::from_secs(5) {
        match stream.recv_timeout(Duration::from_millis(500)) {
            Ok(AudioEvent::Packet(packet)) => {
                let ts = stream_anchor.audio_stream_relative(&packet);
                if packet.metadata.sequence % 50 == 0 {
                    println!(
                        "source={:?} seq={} frames={} silent={} stream=[{:?}..{:?}]",
                        packet.source,
                        packet.metadata.sequence,
                        packet.frames,
                        packet.metadata.is_silent,
                        ts.start,
                        ts.end,
                    );
                }
            }
            Ok(AudioEvent::PacketDropped {
                source,
                dropped_frames,
            }) => {
                println!("packet dropped source={source:?} frames={dropped_frames}");
            }
            Ok(AudioEvent::SourceRestarted {
                source,
                old_device_id,
                new_device_id,
                downtime,
            }) => {
                println!(
                    "source restarted source={source:?} old={old_device_id:?} new={new_device_id} downtime={downtime:?}"
                );
            }
            Ok(AudioEvent::Paused { at }) => {
                println!("stream paused at {at:?}");
            }
            Ok(AudioEvent::Resumed { at, gap }) => {
                println!("stream resumed at {at:?} gap={gap:?}");
            }
            Ok(AudioEvent::Error(err)) => {
                println!("stream error: {err}");
                break;
            }
            Ok(AudioEvent::StreamEnded) => {
                println!("stream ended");
                break;
            }
            Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Closed) => {
                println!("stream closed");
                break;
            }
        }
    }

    let tail = stream.stop_and_drain();
    println!("drained {} tail events", tail.len());

    let snapshot = stats.snapshot();
    println!(
        "captured={} dropped={} restarts={}",
        snapshot.packets_captured, snapshot.packets_dropped, snapshot.source_restarts,
    );

    Ok(())
}
