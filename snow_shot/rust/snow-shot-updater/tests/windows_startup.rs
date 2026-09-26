#![cfg(windows)]

use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::process::Command;
use windows::Win32::System::ApplicationInstallationAndServicing::{
    ACTCTX_REQUESTED_RUN_LEVEL, ACTCTX_RUN_LEVEL_AS_INVOKER, ACTCTXW, CreateActCtxW, QueryActCtxW,
    ReleaseActCtx,
};
use windows::Win32::System::SystemServices::RunlevelInformationInActivationContext;
use windows::Win32::System::WindowsProgramming::ACTCTX_FLAG_RESOURCE_NAME_VALID;
use windows::core::PCWSTR;

// ACTCTX_RUN_LEVEL_INFORMATION from winnt.h is absent from windows crate metadata.
#[repr(C)]
#[derive(Default)]
struct RunLevelInformation {
    flags: u32,
    run_level: ACTCTX_REQUESTED_RUN_LEVEL,
    ui_access: u32,
}

#[test]
fn updater_declares_unelevated_startup_in_embedded_manifest() {
    let executable = Path::new(env!("CARGO_BIN_EXE_snow-shot-updater"));
    let source: Vec<u16> = executable
        .as_os_str()
        .encode_wide()
        .chain(Some(0))
        .collect();
    let options = ACTCTXW {
        cbSize: size_of::<ACTCTXW>() as u32,
        dwFlags: ACTCTX_FLAG_RESOURCE_NAME_VALID,
        lpSource: PCWSTR(source.as_ptr()),
        // MAKEINTRESOURCEW(1): an integer resource identifier, not a string pointer.
        lpResourceName: PCWSTR(std::ptr::without_provenance(1)),
        ..Default::default()
    };
    // Ask Windows to load the actual executable resource, not a source-side XML
    // file. Missing, malformed, unembedded, or elevated manifests must fail.
    let context = unsafe { CreateActCtxW(&options) }
        .expect("the updater must embed a valid Windows application manifest");
    let mut information = RunLevelInformation::default();
    let result = unsafe {
        QueryActCtxW(
            0,
            context,
            None,
            RunlevelInformationInActivationContext.0 as u32,
            Some((&mut information as *mut RunLevelInformation).cast()),
            size_of::<RunLevelInformation>(),
            None,
        )
    };
    unsafe { ReleaseActCtx(context) };
    result.expect("Windows must recognize the updater execution level");
    assert_eq!(information.run_level, ACTCTX_RUN_LEVEL_AS_INVOKER);
    assert_eq!(information.ui_access, 0);
}

#[test]
fn startup_transaction_query_runs_from_a_fresh_installation_path() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path().join("Snow Shot");
    let bin = root.join("bin");
    std::fs::create_dir_all(&bin).unwrap();
    let executable = bin.join("snow-shot-updater.exe");
    std::fs::copy(env!("CARGO_BIN_EXE_snow-shot-updater"), &executable).unwrap();
    for pending in [false, true] {
        if pending {
            let work = root.join(snow_shot_updater::transaction::UPDATE_WORK);
            std::fs::create_dir_all(&work).unwrap();
            std::fs::write(work.join("journal.json"), b"{}").unwrap();
        }
        // Matches QProcess's non-shell launch before Snow Shot creates its UI.
        let output = Command::new(&executable)
            .arg("--transaction-state")
            .arg("--target")
            .arg(&root)
            .output()
            .expect("the startup query must launch without requesting elevation");
        assert!(output.status.success(), "{output:?}");
        assert_eq!(
            String::from_utf8(output.stdout).unwrap().trim(),
            if pending { "pending" } else { "clean" }
        );
    }
}
