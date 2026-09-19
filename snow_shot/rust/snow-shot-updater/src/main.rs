#![cfg_attr(windows, windows_subsystem = "windows")]

use snow_shot_updater::contract::verify_release_file;
use snow_shot_updater::error::{Result, UpdateError};
use snow_shot_updater::transaction;
use std::path::PathBuf;

fn option(args: &[String], name: &str) -> Result<String> {
    args.iter()
        .position(|argument| argument == name)
        .and_then(|index| args.get(index + 1))
        .cloned()
        .ok_or_else(|| UpdateError::new("missing_updater_argument", "Missing updater argument"))
}

fn path_option(args: &[String], name: &str) -> Result<PathBuf> {
    Ok(PathBuf::from(option(args, name)?))
}

fn async_runtime() -> Result<tokio::runtime::Runtime> {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|error| {
            UpdateError::new(
                "service_not_initialized",
                "The update service could not be initialized",
            )
            .detail(error)
        })
}

fn run() -> Result<i32> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let operation = args.first().ok_or_else(|| {
        UpdateError::new("missing_updater_operation", "Missing updater operation")
    })?;
    match operation.as_str() {
        "--verify-release" => {
            verify_release_file(&path_option(&args, "--manifest")?)?;
            Ok(0)
        }
        "--audit-release" => {
            let release = verify_release_file(&path_option(&args, "--manifest")?)?;
            transaction::audit_release(&path_option(&args, "--directory")?, &release)?;
            Ok(0)
        }
        "--transaction-state" => {
            let root = path_option(&args, "--target")?;
            println!(
                "{}",
                if transaction::transaction_pending(&root) {
                    "pending"
                } else {
                    "clean"
                }
            );
            Ok(0)
        }
        "--recover" => {
            transaction::recover_transaction(&path_option(&args, "--target")?)?;
            Ok(0)
        }
        "--uninstall" => {
            let root = path_option(&args, "--target")?;
            transaction::uninstall(&root, !args.iter().any(|argument| argument == "--upgrade"))?;
            Ok(0)
        }
        "--migrate-startup" => {
            let root = path_option(&args, "--target")?;
            let previous = path_option(&args, "--previous")?;
            transaction::validate_root(&root)?;
            snow_shot_updater::platform::migrate_installation_startup(&previous, &root)?;
            Ok(0)
        }
        "--launch-desktop" => {
            let root = path_option(&args, "--target")?;
            transaction::validate_root(&root)?;
            Ok(
                if snow_shot_updater::platform::launch_on_interactive_desktop(
                    &root.join("bin/snow_shot.exe"),
                )? {
                    0
                } else {
                    1
                },
            )
        }
        "--launch" => snow_shot_updater::coordination::launch(&args),
        "--broker" => async_runtime()?.block_on(snow_shot_updater::coordination::broker(&args)),
        "--bootstrap" => snow_shot_updater::coordination::bootstrap(&args, false),
        "--elevated" => snow_shot_updater::coordination::bootstrap(&args, true),
        "--worker" => async_runtime()?.block_on(snow_shot_updater::coordination::worker(&args)),
        "--service" => {
            let root = path_option(&args, "--target")?;
            let runtime = async_runtime()?;
            let result = runtime.block_on(snow_shot_updater::service::run(
                snow_shot_updater::service::ServiceOptions {
                    root,
                    cache_directory: path_option(&args, "--cache")?,
                    base_url: option(&args, "--base-url")?,
                    allow_local_http: args.iter().any(|argument| argument == "--allow-local-http"),
                    parent_pid: option(&args, "--parent")?.parse().map_err(|_| {
                        UpdateError::new(
                            "invalid_updater_argument",
                            "Invalid updater command argument",
                        )
                    })?,
                },
            ));
            // Tokio's stdin adapter owns a blocking reader thread. The operation-complete frame is
            // already flushed, so do not wait for that reader after an operation-scoped session.
            runtime.shutdown_background();
            result?;
            Ok(0)
        }
        _ => Err(UpdateError::new(
            "unknown_updater_operation",
            "Unknown updater operation",
        )),
    }
}

fn main() {
    tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .without_time()
        .compact()
        .init();
    match run() {
        Ok(code) => std::process::exit(code),
        Err(error) => {
            if let Some(detail) = &error.detail {
                eprintln!("{} [{}]: {}", error.message, error.code, detail);
            } else {
                eprintln!("{} [{}]", error.message, error.code);
            }
            std::process::exit(1);
        }
    }
}
