# Legacy container fixture

`legacy-matroska-h264.mkv` is a generated 16 × 16, 10 FPS fixture containing
three lossless H.264 YUV420 frames (Y = 60, 120, 180; U = V = 128).
It uses no captured or third-party media. The compatibility FFmpeg Matroska
muxer generated it; the application profile must import and trim it without
requiring that muxer or adding compatibility output formats to the installer.

# HDR decoder fixtures

These original, self-generated fixtures let HDR decoding, tone mapping, trimming,
and recovery tests run against an FFmpeg prefix containing only the 8-bit x265
encoder. They are test inputs and are not packaged in Snow Shot.

Both files contain losslessly encoded 32 × 24 YUV420P10LE HEVC (`hvc1`), limited
range BT.2020 primaries/nonconstant luminance with SMPTE ST 2084 (PQ) transfer,
at a 1/30 second time base. Chroma samples are 512. Each picture has uniform luma:

| File | Picture PTS | Luma | Duration |
| --- | --- | --- | --- |
| `hdr10-two-frame.mp4` | 0, 15 | 256, 512 | 30 ticks |
| `hdr10-sparse.mp4` | 0, 30, 60 | 256, 512, 768 | 90 ticks |

Generated with the repository's pre-Main8-only Windows static FFmpeg
63.1.100 / libavformat 63.1.100 and libx265 build, using
`generate_hdr_fixtures.c`. This source uses no capture or external media.
Container encoder version metadata can differ when regenerating with another
FFmpeg version; decoded pixels, color description, PTS and durations are the
contract.

To regenerate, compile the generator against a Main10-capable FFmpeg prefix,
link the libraries listed by its `libavformat`/`libavcodec` pkg-config metadata,
then run:

```text
generate_hdr_fixtures hdr10-two-frame.mp4 hdr10-sparse.mp4
```

Checked-in SHA-256:

```text
hdr10-two-frame.mp4 b0e41f1e23e221b1449efbad2fdf151f2b8fca763aabdc1b028af8a3ed2afc39
hdr10-sparse.mp4    baebdc9317fc02aa773c5f416dee8270181cb1d9913778973d3a2169ef34a19b
```
