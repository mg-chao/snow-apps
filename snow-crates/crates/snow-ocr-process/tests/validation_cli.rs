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
    assert!(String::from_utf8_lossy(&version.stdout).contains("snow-ocr-process 1.0.3"));

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
