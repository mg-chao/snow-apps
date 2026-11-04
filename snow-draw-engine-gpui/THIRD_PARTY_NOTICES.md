# Third-party notices

This project is licensed under Apache-2.0; see [LICENSE](LICENSE). It uses the
Apache-2.0 UI-independent Snow Draw Engine crates from `../snow_draw_engine_qt/crates`.
The Smart Erase implementation adapts the surface, periodic, perceptual guide and
PatchMatch algorithm in that project's `src/rendering/snow_canvas_smart_erase_algorithm.cpp`.
It is independently expressed in Rust and links neither Qt nor OpenCV.

Direct dependencies include the [GPUI fork](https://github.com/mg-chao/gpui)
at revision `2c0bf70b54c018f99419f386d1cf1105dca57661` (`gpui` 0.2.2 and
`gpui_platform` / `gpui_windows` 0.1.0, Apache-2.0), Lyon (MIT OR Apache-2.0),
i_overlay 9.0.0 (MIT OR Apache-2.0) for robust arrow ink union and difference,
the Windows Rust bindings (`windows`, `windows-core` and `windows-numerics`,
MIT OR Apache-2.0),
usvg 0.45.1 (Apache-2.0 OR MIT) for fonts and vector path types,
tiny-skia 0.11.4 (BSD-3-Clause) for CPU Smart Erase masks,
png 0.18.1 (MIT OR Apache-2.0), image
(MIT OR Apache-2.0), fontdb (MIT), Rustybuzz (MIT), serde/serde_json
(MIT OR Apache-2.0), base64 (MIT OR Apache-2.0), anyhow (MIT OR Apache-2.0),
unicode-segmentation (MIT OR Apache-2.0), and unicode-bidi (MIT OR Apache-2.0).
GPUI also resolves its own resvg/usvg 0.46.0. The full resolved graph and the
original license texts, rather than this summary, control redistribution.

The repository collector was run against the current Windows dependency graph on 2026-11-04 with `--locked --offline` graph resolution. Its scoped output is:

`../build/arrow-outline-license-audit/third-party/INDEX.md`

It collected 512 notice files covering 285 external Rust packages, 21 vcpkg
packages and repository attribution notices. The collector is shared with Snow Shot and its
index retains its generic Snow Media title and superset vcpkg/project entries;
this GPUI binary does not link Qt or that vcpkg package set.

Some published crate archives omit their workspace license files. Exact upstream
texts and provenance are supplied in `licenses/cargo/`. They were copied from the
existing audited reference checkout; each `SOURCE.md` records its upstream URL.
The new `taffy-0.15.0` text was retrieved from the exact upstream revision recorded
by its published `.cargo_vcs_info.json`,
`eb6450694f33e2b7413e40677f230ff63f0fc194`. `seahash`'s provenance explicitly
records the later upstream license revision.
The `i_key_sort-0.11.0` MIT text was retrieved from the exact upstream revision
recorded by its published `.cargo_vcs_info.json`,
`d8c914dc29afd4d4824dacd2c134dbc16168e78c`; its `SOURCE.md` records the URL.
A canonical Apache-2.0 text supports dual-licensed archives that omit a notice.
No license files or package cache contents were changed in the reference checkout.

System fonts are loaded from the user's operating system; no proprietary font
files are distributed. The procedural showcase background and sample annotations
are generated within this project and require no external image attribution.
