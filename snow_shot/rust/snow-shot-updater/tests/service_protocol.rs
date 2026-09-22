#![cfg(windows)]

use serde_json::{Value, json};
use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::time::{Duration, Instant};
use tempfile::TempDir;

struct ServiceProcess {
    child: Child,
    stdin: Option<ChildStdin>,
    stdout: BufReader<ChildStdout>,
}

impl ServiceProcess {
    fn start() -> (TempDir, Self) {
        Self::start_with_result(None)
    }

    fn start_with_result(result: Option<&str>) -> (TempDir, Self) {
        let temporary = TempDir::new().unwrap();
        let root = temporary.path().join("installation");
        let cache = temporary.path().join("cache");
        if let Some(result) = result {
            std::fs::create_dir_all(&cache).unwrap();
            std::fs::write(cache.join("result.txt"), result).unwrap();
        }
        std::fs::create_dir_all(root.join("bin")).unwrap();
        std::fs::write(root.join("bin/snow_shot.exe"), []).unwrap();
        std::fs::write(
            root.join("snow-shot-installation.json"),
            serde_json::to_vec(&json!({
                "schema": 1,
                "variant": "portable",
                "version": "1.0.0",
                "files": [{
                    "path": "bin/snow_shot.exe",
                    "size": 0,
                    "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
                }]
            }))
            .unwrap(),
        )
        .unwrap();
        let mut child = Command::new(env!("CARGO_BIN_EXE_snow-shot-updater"))
            .args([
                "--service",
                "--target",
                root.to_str().unwrap(),
                "--cache",
                cache.to_str().unwrap(),
                "--base-url",
                "http://127.0.0.1:9",
                "--allow-local-http",
                "--parent",
                &std::process::id().to_string(),
            ])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .unwrap();
        let stdin = child.stdin.take().unwrap();
        let stdout = BufReader::new(child.stdout.take().unwrap());
        (
            temporary,
            Self {
                child,
                stdin: Some(stdin),
                stdout,
            },
        )
    }

    fn read(&mut self) -> Value {
        let mut line = String::new();
        assert_ne!(self.stdout.read_line(&mut line).unwrap(), 0);
        serde_json::from_str(&line).unwrap()
    }

    fn send(&mut self, value: &Value) {
        let stdin = self.stdin.as_mut().unwrap();
        serde_json::to_writer(&mut *stdin, value).unwrap();
        stdin.write_all(b"\n").unwrap();
        stdin.flush().unwrap();
    }

    fn close_input(&mut self) {
        self.stdin.take();
    }

    fn wait(mut self) -> std::process::ExitStatus {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                return status;
            }
            if Instant::now() >= deadline {
                let _ = self.child.kill();
                let _ = self.child.wait();
                panic!("service did not exit within five seconds");
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }
}

#[test]
fn worker_result_preserves_registry_diagnostics_in_service_status() {
    let error = snow_shot_updater::UpdateError::new(
        "registered_version_update_failed",
        "Could not update the registered application version",
    )
    .detail("RegCreateKeyExW HKLM\\Software\\test (32-bit view): Win32 error 5");
    for structured in [false, true] {
        let message = if structured {
            error.handoff_message()
        } else {
            error.message.to_string()
        };
        let (temporary, mut service) =
            ServiceProcess::start_with_result(Some(&format!("failed:{message}")));
        assert_eq!(service.read()["type"], "hello");
        let status = service.read();
        assert_eq!(status["status"]["state"], "Failed");
        assert_eq!(status["status"]["error"]["message"], error.message.as_ref());
        if structured {
            assert_eq!(status["status"]["error"]["code"], error.code.as_ref());
            assert_eq!(
                status["status"]["error"]["detail"],
                error.detail.as_deref().unwrap()
            );
        }
        assert!(!temporary.path().join("cache/result.txt").exists());
        service.close_input();
        assert!(service.wait().success());
    }
}

#[test]
fn service_handshake_rejects_duplicate_ids_and_shuts_down_orderly() {
    let (_temporary, mut service) = ServiceProcess::start();
    let hello = service.read();
    assert_eq!(hello["protocol"], 2);
    assert_eq!(hello["type"], "hello");
    assert_eq!(hello["platform"], "windows-x64");
    assert!(
        hello["capabilities"]
            .as_array()
            .unwrap()
            .contains(&json!("apply"))
    );
    assert_eq!(service.read()["status"]["state"], "Idle");

    service.send(&json!({"protocol": 2, "id": 1, "command": "cancel"}));
    assert_eq!(service.read()["ok"], true);
    assert_eq!(service.read()["status"]["state"], "Idle");

    service.send(&json!({"protocol": 2, "id": 1, "command": "cancel"}));
    let duplicate = service.read();
    assert_eq!(duplicate["ok"], false);
    assert_eq!(duplicate["error"]["code"], "protocol_request_id_invalid");

    service.send(&json!({"protocol": 2, "id": 2, "command": "shutdown"}));
    assert_eq!(service.read()["ok"], true);
    assert_eq!(service.read()["status"]["state"], "Idle");
    service.close_input();
    assert!(service.wait().success());
}

#[test]
fn malformed_frame_emits_fatal_and_closes() {
    let (_temporary, mut service) = ServiceProcess::start();
    let _hello = service.read();
    let _status = service.read();
    let stdin = service.stdin.as_mut().unwrap();
    stdin.write_all(b"{not-json}\n").unwrap();
    stdin.flush().unwrap();
    let fatal = service.read();
    assert_eq!(fatal["type"], "fatal");
    assert_eq!(fatal["error"]["code"], "protocol_message_invalid");
    service.close_input();
    assert!(service.wait().success());
}

#[test]
fn peer_closure_stops_the_service_without_reconnect_endpoint() {
    let (_temporary, mut service) = ServiceProcess::start();
    let _hello = service.read();
    let _status = service.read();
    service.close_input();
    assert!(service.wait().success());
}

#[test]
fn probe_reports_cached_status_and_exits_after_completion() {
    let (_temporary, mut service) = ServiceProcess::start();
    let _hello = service.read();
    assert_eq!(service.read()["status"]["state"], "Idle");
    service.send(&json!({
        "protocol": 2,
        "id": 1,
        "command": "execute",
        "operation": "probe",
        "trigger": "startup",
        "mode": "manual",
        "systemProxy": false
    }));
    assert_eq!(service.read()["ok"], true);
    assert_eq!(service.read()["status"]["state"], "Idle");
    let completed = service.read();
    assert_eq!(completed["type"], "operation_complete");
    assert_eq!(completed["operation"], "probe");
    assert_eq!(completed["outcome"], "success");
    assert_eq!(completed["status"]["state"], "Idle");
    assert!(service.wait().success());
}
