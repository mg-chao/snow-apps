//! Forwards the CMake-generated ONNX Runtime link manifest to the final
//! executable link when the `static-onnx-runtime` feature is enabled.
//!
//! `rapid-ocr-rs/static-onnx-runtime` disables ort-sys's own linking, so this
//! build script is the only source of the ONNX Runtime link closure. The
//! manifest is produced by the Snow Shot CMake build from the vcpkg
//! `onnxruntime::onnxruntime` imported targets and contains one linker
//! argument (library path, system library, or `/DELAYLOAD:...` option) per
//! line.

use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    if env::var_os("CARGO_FEATURE_STATIC_ONNX_RUNTIME").is_none() {
        return;
    }
    println!("cargo:rerun-if-env-changed=SNOW_ONNXRUNTIME_LINK_FILE");
    let Some(manifest_path) = env::var_os("SNOW_ONNXRUNTIME_LINK_FILE") else {
        println!(
            "cargo:warning=snow-ocr-process was built with static-onnx-runtime but \
             SNOW_ONNXRUNTIME_LINK_FILE is not set; the link will fail unless the \
             ONNX Runtime closure is provided another way"
        );
        return;
    };
    let manifest_path = PathBuf::from(&manifest_path);
    println!("cargo:rerun-if-changed={}", manifest_path.display());
    let manifest = fs::read_to_string(&manifest_path).unwrap_or_else(|error| {
        panic!(
            "failed to read the ONNX Runtime link manifest {}: {error}",
            manifest_path.display()
        )
    });
    for line in manifest.lines() {
        let argument = line.trim();
        if argument.is_empty() || argument.starts_with('#') {
            continue;
        }
        println!("cargo:rustc-link-arg={argument}");
    }
}
