# Snow Audio Recorder

The `Auto` backend uses WASAPI on Windows and native Apple capture APIs on macOS 14 or newer.

## macOS

- System audio uses ScreenCaptureKit's system mix. The render-device list therefore contains one
  `macos-system-audio` entry, rather than pretending to offer output-device-specific loopback.
- Microphones use AVCaptureSession and persistent CoreAudio device UIDs. Selecting a microphone
  does not change the system default input device.
- AVAudioConverter produces interleaved signed 16-bit PCM in the requested mono or stereo format,
  at sample rates up to 192 kHz. Packet timestamps use the native host clock, converted to `Instant`
  and 100 ns units for the shared audio timeline.
- Native queues are bounded to at most 128 packets per source. Overflow is reported through
  `AudioEvent::PacketDropped`; native callbacks never retain pointers into Rust buffers.
- A microphone disconnect or capture-service error ends capture. Restart recording after repairing
  the device or permission. The system mix follows output routing without rebinding a device.

The application bundle must contain `NSMicrophoneUsageDescription`. Microphone capture requires
permission under **System Settings → Privacy & Security → Microphone**. System audio requires
**Screen & System Audio Recording** permission. Permission failures include this guidance and are
not retried in a loop.

Only enabled sources request access. Library callers can mark a source optional through
`SourceConfig::required`; startup then continues if another enabled source is available.

## Focused verification

Run `cargo test -p snow-audio-recorder --lib` for deterministic audio, timeline, and backend checks.
Do not run hardware capture tests as part of unattended unit tests.

The `macos_capture_probe` example is an interactive diagnostic. Run it from a macOS app bundle with
these permissions and its microphone usage description:

```text
macos_capture_probe --system --seconds 3 --output-dir /tmp/snow-system-audio
macos_capture_probe --microphone --microphone-id BuiltInMicrophoneDevice --seconds 1
```

Play a generated test tone for the system-audio check. The probe checks packet sizes, formats, and
monotonic timestamps and prints packet counts, peak amplitude, and RMS. `--output-dir` writes PCM
WAV files; omit it for the microphone check to avoid saving ambient audio. Silent input is valid, so
nonzero amplitude alone is not a unit-test requirement. The system WAV's expected tone frequency
must be checked separately when verifying end-to-end loopback.
