#[cfg(windows)]
use std::time::Duration;
use std::{
    alloc::{GlobalAlloc, Layout, System},
    path::PathBuf,
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
};

use criterion::{
    BatchSize, BenchmarkId, Criterion, Throughput, black_box, criterion_group, criterion_main,
};
#[cfg(windows)]
use snow_audio_recorder::benchmark::AccumulatorBenchHarness;
use snow_audio_recorder::benchmark::{
    BenchSampleFormat, ConverterBenchHarness, GainBenchHarness, make_f32_bytes, make_i16_bytes,
    make_sine_f32_samples, make_sine_i16_samples,
};
use snow_audio_recorder::{AudioFormat, AudioPacket, AudioPacketMetadata, AudioSourceKind};

// Count only the explicitly delimited hot-path probe. Criterion setup, report
// formatting, and preallocated input/control state are outside that window.
struct CountingAllocator;
static COUNT_ALLOCATIONS: AtomicBool = AtomicBool::new(false);
static ALLOCATION_CALLS: AtomicUsize = AtomicUsize::new(0);
static ALLOCATED_BYTES: AtomicUsize = AtomicUsize::new(0);
static REALLOCATION_CALLS: AtomicUsize = AtomicUsize::new(0);
static DEALLOCATION_CALLS: AtomicUsize = AtomicUsize::new(0);
#[global_allocator]
static ALLOCATOR: CountingAllocator = CountingAllocator;

fn record_allocation(pointer: *mut u8, size: usize, reallocation: bool) {
    if !pointer.is_null() && COUNT_ALLOCATIONS.load(Ordering::Relaxed) {
        ALLOCATION_CALLS.fetch_add(1, Ordering::Relaxed);
        ALLOCATED_BYTES.fetch_add(size, Ordering::Relaxed);
        if reallocation {
            REALLOCATION_CALLS.fetch_add(1, Ordering::Relaxed);
        }
    }
}

// SAFETY: Every allocation operation delegates unchanged to the system allocator.
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc(layout) };
        record_allocation(pointer, layout.size(), false);
        pointer
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc_zeroed(layout) };
        record_allocation(pointer, layout.size(), false);
        pointer
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        let pointer = unsafe { System.realloc(pointer, layout, size) };
        record_allocation(pointer, size, true);
        pointer
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        if COUNT_ALLOCATIONS.load(Ordering::Relaxed) {
            DEALLOCATION_CALLS.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.dealloc(pointer, layout) };
    }
}

fn gain_packet() -> AudioPacket {
    AudioPacket {
        source: AudioSourceKind::System,
        format: AudioFormat::new(48_000, 2),
        frames: 480,
        data: make_sine_i16_samples(480, 2, 440.0, 48_000),
        metadata: AudioPacketMetadata::default(),
    }
}

fn benchmark_output_directory() -> Option<PathBuf> {
    std::env::var_os("SNOW_AUDIO_BENCH_OUTPUT").map(PathBuf::from)
}

fn criterion_config() -> Criterion {
    let criterion = Criterion::default();
    match benchmark_output_directory() {
        Some(path) => criterion.output_directory(&path),
        None => criterion,
    }
}

fn verify_gain_allocations() {
    const ITERATIONS: usize = 10_000;
    let mut reports = Vec::new();
    for (name, gain, meter, ramping) in [
        ("raw_unprocessed", None, false, false),
        ("unity_hidden", Some(0), false, false),
        ("unity_meter", Some(0), true, false),
        ("attenuate_meter", Some(-24), true, false),
        ("boost_meter", Some(24), true, false),
        ("ramping_meter", Some(0), true, true),
    ] {
        let mut harness = gain.map(|db| GainBenchHarness::new(db, meter).unwrap());
        let mut packet = gain_packet();
        let source = packet.data.clone();
        let capacity = packet.data.capacity();
        let pointer = packet.data.as_ptr();
        let mut target = 6;
        ALLOCATION_CALLS.store(0, Ordering::Relaxed);
        ALLOCATED_BYTES.store(0, Ordering::Relaxed);
        REALLOCATION_CALLS.store(0, Ordering::Relaxed);
        DEALLOCATION_CALLS.store(0, Ordering::Relaxed);
        COUNT_ALLOCATIONS.store(true, Ordering::Release);
        for _ in 0..ITERATIONS {
            packet.data.copy_from_slice(black_box(&source));
            if let Some(harness) = &mut harness {
                if ramping {
                    target = -target;
                    harness.set_gain_db(target).unwrap();
                }
                harness.process(black_box(&mut packet));
            } else {
                black_box(&mut packet);
            }
            black_box(&packet.data);
        }
        COUNT_ALLOCATIONS.store(false, Ordering::Release);
        let calls = ALLOCATION_CALLS.load(Ordering::Acquire);
        let bytes = ALLOCATED_BYTES.load(Ordering::Acquire);
        let reallocations = REALLOCATION_CALLS.load(Ordering::Acquire);
        let deallocations = DEALLOCATION_CALLS.load(Ordering::Acquire);
        assert_eq!(calls, 0, "{name} must not allocate while processing PCM");
        assert_eq!(deallocations, 0, "{name} must retain its PCM buffer");
        assert_eq!(packet.data.capacity(), capacity);
        assert_eq!(packet.data.as_ptr(), pointer);
        reports.push(format!(
            "{{\"case\":\"{name}\",\"packets\":{ITERATIONS},\"allocation_calls\":{calls},\"allocated_bytes\":{bytes},\"reallocation_calls\":{reallocations},\"deallocation_calls\":{deallocations},\"pcm_capacity_samples\":{capacity},\"pcm_buffer_reused\":true}}"
        ));
    }
    if let Some(directory) = benchmark_output_directory() {
        std::fs::create_dir_all(&directory).unwrap();
        let report = format!(
            "{{\"sample_rate_hz\":48000,\"channels\":2,\"frames_per_packet\":480,\"setup_allocations_excluded\":true,\"allocator_operations_counted\":[\"alloc\",\"alloc_zeroed\",\"realloc\",\"dealloc\"],\"cases\":[{}]}}\n",
            reports.join(",")
        );
        std::fs::write(directory.join("allocations.json"), report).unwrap();
    }
}

fn bench_converter(c: &mut Criterion) {
    let mut group = c.benchmark_group("converter");

    let stereo_48k_20ms_frames = 960u32;
    let stereo_i16_samples = make_sine_i16_samples(stereo_48k_20ms_frames, 2, 440.0, 48_000);
    let stereo_i16_bytes = make_i16_bytes(&stereo_i16_samples);
    group.throughput(Throughput::Bytes(stereo_i16_bytes.len() as u64));
    group.bench_with_input(
        BenchmarkId::new("i16_passthrough_48k_stereo_20ms", stereo_48k_20ms_frames),
        &stereo_i16_bytes,
        |b, input| {
            b.iter_batched(
                || {
                    ConverterBenchHarness::new(48_000, 2, BenchSampleFormat::I16, 48_000, 2)
                        .expect("converter harness")
                },
                |mut harness| {
                    let out = harness
                        .convert(black_box(input.as_slice()), stereo_48k_20ms_frames)
                        .expect("conversion should succeed");
                    black_box(out);
                },
                BatchSize::SmallInput,
            );
        },
    );

    let stereo_f32_samples = make_sine_f32_samples(stereo_48k_20ms_frames, 2, 440.0, 48_000);
    let stereo_f32_bytes = make_f32_bytes(&stereo_f32_samples);
    group.throughput(Throughput::Bytes(stereo_f32_bytes.len() as u64));
    group.bench_with_input(
        BenchmarkId::new("f32_quantize_48k_stereo_20ms", stereo_48k_20ms_frames),
        &stereo_f32_bytes,
        |b, input| {
            b.iter_batched(
                || {
                    ConverterBenchHarness::new(48_000, 2, BenchSampleFormat::F32, 48_000, 2)
                        .expect("converter harness")
                },
                |mut harness| {
                    let out = harness
                        .convert(black_box(input.as_slice()), 882)
                        .expect("conversion should succeed");
                    black_box(out);
                },
                BatchSize::SmallInput,
            );
        },
    );

    let stereo_44k_frames = 882u32;
    let stereo_44k_samples = make_sine_i16_samples(stereo_44k_frames, 2, 440.0, 44_100);
    let stereo_44k_bytes = make_i16_bytes(&stereo_44k_samples);
    group.throughput(Throughput::Bytes(stereo_44k_bytes.len() as u64));
    group.bench_with_input(
        BenchmarkId::new("i16_resample_44k1_to_48k_stereo_20ms", stereo_44k_frames),
        &stereo_44k_bytes,
        |b, input| {
            b.iter_batched(
                || {
                    ConverterBenchHarness::new(44_100, 2, BenchSampleFormat::I16, 48_000, 2)
                        .expect("converter harness")
                },
                |mut harness| {
                    let out = harness
                        .convert(black_box(input.as_slice()), stereo_48k_20ms_frames)
                        .expect("conversion should succeed");
                    black_box(out);
                },
                BatchSize::SmallInput,
            );
        },
    );

    group.finish();
}

#[cfg(windows)]
fn bench_accumulator(c: &mut Criterion) {
    let mut group = c.benchmark_group("accumulator");

    let packet_duration = Duration::from_millis(20);
    let frames = 960u32;
    let samples = make_sine_i16_samples(frames, 2, 440.0, 48_000);

    group.throughput(Throughput::Elements(frames as u64));
    group.bench_function("exact_packet_passthrough_48k_stereo_20ms", |b| {
        b.iter_batched(
            || {
                (
                    AccumulatorBenchHarness::new(
                        AudioSourceKind::System,
                        48_000,
                        2,
                        packet_duration,
                    )
                    .expect("accumulator harness"),
                    samples.clone(),
                )
            },
            |(mut harness, chunk)| {
                let packets = harness
                    .push_chunk(black_box(chunk), frames)
                    .expect("accumulation should succeed");
                black_box(packets);
            },
            BatchSize::SmallInput,
        );
    });

    let half_frames = frames / 2;
    let half_samples = make_sine_i16_samples(half_frames, 2, 440.0, 48_000);
    group.bench_function("split_packet_two_half_chunks_48k_stereo_20ms", |b| {
        b.iter_batched(
            || {
                (
                    AccumulatorBenchHarness::new(
                        AudioSourceKind::System,
                        48_000,
                        2,
                        packet_duration,
                    )
                    .expect("accumulator harness"),
                    half_samples.clone(),
                )
            },
            |(mut harness, chunk)| {
                let first = harness
                    .push_chunk(black_box(chunk.clone()), half_frames)
                    .expect("first half should succeed");
                let second = harness
                    .push_chunk(black_box(chunk), half_frames)
                    .expect("second half should succeed");
                black_box((first, second));
            },
            BatchSize::SmallInput,
        );
    });

    group.finish();
}

fn bench_gain(c: &mut Criterion) {
    verify_gain_allocations();
    let mut group = c.benchmark_group("gain");
    group.throughput(Throughput::Elements(960));
    let mut raw_packet = gain_packet();
    let raw_source = raw_packet.data.clone();
    group.bench_function("raw_unprocessed", |b| {
        b.iter(|| {
            raw_packet.data.copy_from_slice(&raw_source);
            // This is the identical input refill/observation path, with the
            // newly introduced gain/control processing call omitted.
            black_box(&mut raw_packet);
            black_box(&raw_packet.data);
        })
    });
    for (name, gain, meter) in [
        ("unity_hidden", 0, false),
        ("unity_meter", 0, true),
        ("attenuate_meter", -24, true),
        ("boost_meter", 24, true),
    ] {
        let mut harness = GainBenchHarness::new(gain, meter).unwrap();
        let mut packet = gain_packet();
        let source = packet.data.clone();
        group.bench_function(name, |b| {
            b.iter(|| {
                packet.data.copy_from_slice(&source);
                harness.process(black_box(&mut packet));
                black_box(&packet.data);
            })
        });
    }
    let mut harness = GainBenchHarness::new(0, true).unwrap();
    let mut packet = AudioPacket {
        source: AudioSourceKind::System,
        format: AudioFormat::new(48_000, 2),
        frames: 480,
        data: vec![1000; 960],
        metadata: AudioPacketMetadata::default(),
    };
    let mut target = 6;
    group.bench_function("ramping_meter", |b| {
        b.iter(|| {
            target = -target;
            harness.set_gain_db(target).unwrap();
            packet.data.fill(1000);
            harness.process(black_box(&mut packet));
            black_box(&packet.data);
        })
    });
    group.finish();
}

#[cfg(windows)]
criterion_group! {
    name = benches;
    config = criterion_config();
    targets = bench_converter, bench_accumulator, bench_gain
}
#[cfg(not(windows))]
criterion_group! {
    name = benches;
    config = criterion_config();
    targets = bench_converter, bench_gain
}
criterion_main!(benches);
