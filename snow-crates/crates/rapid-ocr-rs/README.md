# rapid-ocr-rs

Rust components for running PaddleOCR-compatible detection, classification,
and recognition models through ONNX Runtime.

This crate is licensed under the [Apache License 2.0](LICENSE).

The default features preserve the `rapidocr` CLI, encoded image and URL inputs,
YAML configuration, and automatic model downloads. Applications that supply raw
pixels and local or in-memory models can use `default-features = false` with
their selected ONNX Runtime/provider features. This omits encoded image decoders,
EXIF, HTTP/TLS, model registry YAML, and CLI parsing from the dependency closure.

Enable convenience features independently when needed:

- `image-io`: PNG/JPEG file and encoded-byte inputs, including EXIF orientation.
- `full-image-io`: the additional image formats supported by the default build.
- `remote-input`: URL image inputs, including `image-io`.
- `model-download`: built-in model discovery and downloads.
- `config-yaml`: YAML configuration loading.
- `cli`: the CLI and its image, URL, model download and YAML features.

Without `model-download`, config-based engine construction requires explicit
model paths; source-based construction also accepts in-memory model data. The
recognition dictionary can be supplied as a local file or in-memory text, or
read from model metadata.
