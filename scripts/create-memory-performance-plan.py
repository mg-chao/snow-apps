#!/usr/bin/env python3
"""Create a focused macOS memory optimization comparison plan.

Build the named targets with snow-shot-macos-arm64-performance first. The
baseline uses memorybenchmark-baseline.cmake to share current benchmark sources.
Each job runs alone in a fresh process; no full test or benchmark suite is used.
"""
import argparse
import json
from pathlib import Path

IMAGE_CASES = (
    "allocation", "copy", "resize", "resize-stream", "palette", "flatten",
    "flatten-partial", "animation-compose", "gray-region", "yuv420-region",
)
CANVAS_CASES = (
    "fresh_allocate_fill_release", "mixed-allocate", "reuse_fill", "copy", "crop",
    "cow", "convert_rgba_premul", "convert_rgba64", "color_p3_srgb", "rotate90",
    "flip_horizontal_smooth", "scale_half_smooth", "rotate17_smooth",
)
SCREENSHOT_CASES = (
    "recognition-native-20", "recognition-rgba-20", "recognition-native-500",
    "compositor-rounded", "compositor-region-cold", "compositor-region-warm",
    "clipboard-image-import", "clipboard-rich-text", "qt-reader-rgba",
    "qt-reader-scaled", "qt-reader-rgba64",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before-build", type=Path, required=True)
    parser.add_argument("--after-build", type=Path, required=True)
    parser.add_argument("--before-revision", required=True)
    parser.add_argument("--after-revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, help="Optional deterministic codec files")
    parser.add_argument("--before-rust", type=Path, help="Baseline release examples directory")
    parser.add_argument("--after-rust", type=Path, help="Optimized release examples directory")
    parser.add_argument("--samples", type=int, default=20)
    args = parser.parse_args()
    if args.samples < 1:
        parser.error("--samples must be positive")
    if bool(args.before_rust) != bool(args.after_rust):
        parser.error("Supply both --before-rust and --after-rust")
    builds = {"before": args.before_build.resolve(), "after": args.after_build.resolve()}
    for directory in builds.values():
        try:
            cache = (directory / "CMakeCache.txt").read_text(encoding="utf-8")
        except OSError as error:
            parser.error(f"Cannot read configured build cache: {error}")
        entries = {}
        for line in cache.splitlines():
            if line.startswith(("#", "//")) or "=" not in line or ":" not in line:
                continue
            key, value = line.split("=", 1)
            entries[key.split(":", 1)[0]] = value
        if entries.get("CMAKE_BUILD_TYPE") != "Release" or entries.get("SNOW_APPS_BUILD_BENCHMARKS") != "ON":
            parser.error(f"Use a configured macOS performance preset: {directory}")
    jobs = []

    def add(name, paths, arguments):
        jobs.append({"name": name, "commands": {
            label: [str(builds[label] / paths[label]), *map(str, arguments)]
            for label in builds}})

    image = {"before": "snow_image/snow-image-memory-performance-benchmark-baseline",
             "after": "snow_image/tools/snow_image_memory_performance_benchmark"}
    canvas = {label: "snow_draw_engine_qt/snow-canvas-image-memory-benchmark" +
              ("-baseline" if label == "before" else "") for label in builds}
    shot = {label: "snow_shot/snow-shot-memory-performance-benchmark" +
            ("-baseline" if label == "before" else "") for label in builds}
    for width, height in ((1920, 1080), (3840, 2160)):
        size = ["--width", width, "--height", height, "--warmup", 3]
        for scenario in IMAGE_CASES:
            add(f"image-{scenario}-{width}x{height}", image,
                ["--scenario", scenario, *size, "--count", args.samples])
        for scenario in CANVAS_CASES:
            add(f"canvas-{scenario}-{width}x{height}", canvas,
                ["--scenario", scenario, *size, "--count", args.samples])
        for scenario in SCREENSHOT_CASES:
            add(f"shot-{scenario}-{width}x{height}", shot,
                ["--scenario", scenario, *size, "--samples", args.samples])
    # The 1 MiB transition is checked independently of real screen sizes.
    for width, height in ((512, 511), (512, 512), (512, 513)):
        for prefix, paths, scenario in (("image", image, "allocation"),
                                        ("canvas", canvas, "fresh_allocate_fill_release")):
            add(f"{prefix}-threshold-{width}x{height}", paths,
                ["--scenario", scenario, "--width", width, "--height", height,
                 "--warmup", 3, "--count", args.samples])
    # A few large cases test scaling without repeating the whole matrix at 8K.
    for prefix, paths, scenario, count in (
        ("image", image, "allocation", "--count"),
        ("image", image, "yuv420-region", "--count"),
        ("canvas", canvas, "copy", "--count"),
        ("shot", shot, "compositor-region-cold", "--samples"),
    ):
        add(f"{prefix}-{scenario}-7680x4320", paths,
            ["--scenario", scenario, "--width", 7680, "--height", 4320,
             "--warmup", 3, count, max(7, args.samples // 2)])
    for kind, scenarios in (
        ("filter", ("kernel_gaussian_very_low_1920x1080", "renderer_full_gaussian_1080p")),
        ("watermark", ("renderer_dense_1920x1080", "workflow_export_watermark_3840x2160")),
    ):
        paths = {label: f"snow_draw_engine_qt/snow-canvas-{kind}-benchmark" for label in builds}
        for scenario in scenarios:
            add(f"{kind}-{scenario}", paths,
                ["--scenario", scenario, "--warmup", 3, "--iterations", args.samples,
                 "--csv", "{run_dir}/" + kind + ".csv", "-platform", "offscreen"])
    for scenario in ("4k-text", "nonrepeat-medium"):
        paths = {label: "snow_draw_engine_qt/snow-canvas-smart-erase-benchmark" for label in builds}
        add("erase-" + scenario, paths,
            ["--scenario", scenario, "--warmup", 1, "--repeat", 7, "--jobs", 1])
    paths = {label: "snow_shot/snow-shot-main-window-skin-performance-benchmark" for label in builds}
    add("skin", paths, ["--output", "{run_dir}/skin.json", "--samples", 7])
    if args.fixtures:
        for path in sorted(args.fixtures.resolve().iterdir()):
            if path.suffix.lower() in (".png", ".jpg", ".webp", ".bmp", ".gif", ".jxl"):
                add("decode-" + path.stem + "-" + path.suffix[1:], image,
                    ["--scenario", "decode", "--input", path, "--warmup", 3,
                     "--count", args.samples])
    if args.before_rust:
        rust = {"before": args.before_rust.resolve(), "after": args.after_rust.resolve()}
        for binary, scenarios in (
            ("memory_buffer_benchmark", ("capture-reuse", "capture-resize", "capture-cow",
                                         "capture-import", "capture-churn")),
            ("memory_paths_benchmark", ("stitch-repaint", "stitch-retained", "stitch-export",
                                        "stitch-materialize", "stitch-horizontal", "stitch-orb")),
            ("memory_snapshot_benchmark", ("stitch-small-snapshot",)),
            ("memory_decode_benchmark", ("recording-decode",)),
            ("memory_transfer_benchmark", ("ocr-transfer",)),
            ("memory_overwrite_benchmark", ("overwrite", "preserve")),
            ("memory_reference_benchmark", ("stitch-reference-vertical",
                                            "stitch-reference-horizontal")),
            ("memory_png_benchmark", ("scrolling-png-export",)),
        ):
            paths = {label: rust[label] / binary for label in builds}
            for scenario in scenarios:
                # PNG includes durable file output and needs fewer repetitions
                # than CPU buffer operations. Both revisions keep equal counts.
                samples = min(args.samples, 7) if binary == "memory_png_benchmark" else args.samples
                name = "capture-full-" + scenario if binary == "memory_overwrite_benchmark" else scenario
                add("rust-" + name, paths, [scenario, samples])
    plan = {"versions": {label: {"revision": revision, "cwd": str(builds[label])}
                         for label, revision in (("before", args.before_revision),
                                                 ("after", args.after_revision))},
            "env": {"QT_QPA_PLATFORM": "offscreen", "RAYON_NUM_THREADS": "4"},
            "jobs": jobs}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(jobs)} explicit jobs to {args.output}")


if __name__ == "__main__":
    main()
