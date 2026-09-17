use std::process::{Command, Output};

fn run(arguments: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_snow-ocr-process"))
        .args(arguments)
        .output()
        .expect("snow-ocr-process should launch")
}

#[test]
fn validation_command_reports_process_success_and_failures() {
    let version = run(&["--version"]);
    assert!(version.status.success());
    assert!(
        String::from_utf8_lossy(&version.stdout)
            .contains(concat!("snow-ocr-process ", env!("CARGO_PKG_VERSION")))
    );

    let malformed = run(&["--validate-model-set", "detector.onnx"]);
    assert!(!malformed.status.success());
    assert!(String::from_utf8_lossy(&malformed.stderr).contains(
        "usage: snow-ocr-process --validate-model-set <detector> <recognizer> <dictionary>"
    ));

    let invalid = run(&[
        "--validate-model-set",
        "missing-detector.onnx",
        "missing-recognizer.onnx",
        "missing-dictionary.txt",
    ]);
    assert!(!invalid.status.success());
    assert!(!invalid.stderr.is_empty());
}

#[cfg(target_os = "macos")]
#[test]
fn missing_bundled_library_reports_a_load_error_without_aborting() {
    let directory =
        std::env::temp_dir().join(format!("snow OCR missing dylib {}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let worker = directory.join("snow-ocr-process");
    std::fs::copy(env!("CARGO_BIN_EXE_snow-ocr-process"), &worker).unwrap();
    let output = Command::new(&worker)
        .current_dir("/")
        .args([
            "--validate-model-set",
            "missing-detector",
            "missing-recognizer",
            "missing-dictionary",
        ])
        .output()
        .unwrap();
    std::fs::remove_dir_all(&directory).unwrap();
    assert_eq!(output.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&output.stderr).contains("cannot load bundled ONNX Runtime"));
}
