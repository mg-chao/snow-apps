use std::env;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=src/platform/windows/tonemap_cs.hlsl");
    println!("cargo:rustc-check-cfg=cfg(has_precompiled_shader)");
    println!("cargo:rustc-check-cfg=cfg(has_precompiled_shader_1d)");
    println!("cargo:rustc-check-cfg=cfg(has_precompiled_shader_f16)");
    println!("cargo:rustc-check-cfg=cfg(has_precompiled_shader_f16_1d)");
    println!("cargo:rerun-if-env-changed=SNOW_CAPTURE_FXC_PATH");

    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os != "windows" {
        return;
    }

    compile_wgc_interop();

    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let hlsl_path = PathBuf::from("src/platform/windows/tonemap_cs.hlsl");

    if !hlsl_path.exists() {
        return;
    }

    // Optional escape hatch:
    // SNOW_CAPTURE_PRECOMPILE_SHADER=0 disables build-time fxc compilation.
    println!("cargo:rerun-if-env-changed=SNOW_CAPTURE_PRECOMPILE_SHADER");
    let precompile_enabled = env::var("SNOW_CAPTURE_PRECOMPILE_SHADER")
        .map(|v| {
            let v = v.trim().to_ascii_lowercase();
            !(v == "0" || v == "false" || v == "no" || v == "off")
        })
        .unwrap_or(true);
    if !precompile_enabled {
        println!(
            "cargo:warning=SNOW_CAPTURE_PRECOMPILE_SHADER is disabled; will use runtime D3DCompile fallback"
        );
        return;
    }

    let cso_path = out_dir.join("tonemap_cs.cso");
    let cso_1d_path = out_dir.join("tonemap_cs_1d.cso");
    match compile_with_fxc(&hlsl_path, &cso_path, "main") {
        Ok(()) => {
            // Tell rustc where the compiled shader is so gpu_tonemap.rs
            // can include_bytes! it, and set a cfg flag to enable that path.
            println!("cargo:rustc-env=TONEMAP_CSO_PATH={}", cso_path.display());
            println!("cargo:rustc-cfg=has_precompiled_shader");
        }
        Err(detail) => {
            println!(
                "cargo:warning=failed to precompile shader with fxc ({detail}); will use runtime D3DCompile fallback"
            );
        }
    }
    match compile_with_fxc(&hlsl_path, &cso_1d_path, "main_1d") {
        Ok(()) => {
            println!(
                "cargo:rustc-env=TONEMAP_1D_CSO_PATH={}",
                cso_1d_path.display()
            );
            println!("cargo:rustc-cfg=has_precompiled_shader_1d");
        }
        Err(detail) => {
            println!(
                "cargo:warning=failed to precompile 1D shader with fxc ({detail}); will use runtime D3DCompile fallback"
            );
        }
    }

    let cso_f16_path = out_dir.join("f16_convert_cs.cso");
    let cso_f16_1d_path = out_dir.join("f16_convert_cs_1d.cso");
    match compile_with_fxc(&hlsl_path, &cso_f16_path, "main_f16") {
        Ok(()) => {
            println!(
                "cargo:rustc-env=F16_CONVERT_CSO_PATH={}",
                cso_f16_path.display()
            );
            println!("cargo:rustc-cfg=has_precompiled_shader_f16");
        }
        Err(detail) => {
            println!(
                "cargo:warning=failed to precompile F16 shader with fxc ({detail}); will use runtime D3DCompile fallback"
            );
        }
    }
    match compile_with_fxc(&hlsl_path, &cso_f16_1d_path, "main_f16_1d") {
        Ok(()) => {
            println!(
                "cargo:rustc-env=F16_CONVERT_1D_CSO_PATH={}",
                cso_f16_1d_path.display()
            );
            println!("cargo:rustc-cfg=has_precompiled_shader_f16_1d");
        }
        Err(detail) => {
            println!(
                "cargo:warning=failed to precompile F16 1D shader with fxc ({detail}); will use runtime D3DCompile fallback"
            );
        }
    }
}

fn compile_wgc_interop() {
    for source in [
        "native/wgc_interop.h",
        "native/wgc_interop.cpp",
        "native/wgc_interop_tests.cpp",
    ] {
        println!("cargo:rerun-if-changed={source}");
    }
    cc::Build::new()
        .cpp(true)
        .std("c++20")
        .flag_if_supported("/EHsc")
        .warnings_into_errors(true)
        .file("native/wgc_interop.cpp")
        // A separate archive member is only linked when Rust tests use the fixture.
        .file("native/wgc_interop_tests.cpp")
        .compile("snow_capture_wgc_interop");
}

fn compile_with_fxc(hlsl_path: &Path, cso_path: &Path, entry_point: &str) -> Result<(), String> {
    let mut attempts = Vec::new();
    let mut attempted = false;
    for fxc in fxc_candidates() {
        if !is_path_lookup(&fxc) && !fxc.is_file() {
            continue;
        }
        attempted = true;
        match Command::new(&fxc)
            .args(["/T", "cs_5_0", "/E", entry_point, "/O3", "/Fo"])
            .arg(cso_path)
            .arg(hlsl_path)
            .output()
        {
            Ok(output) if output.status.success() => return Ok(()),
            Ok(output) => {
                attempts.push(format!("{}: {}", fxc.display(), summarize_output(&output)))
            }
            Err(err) => attempts.push(format!("{}: {}", fxc.display(), err)),
        }
    }

    if !attempted {
        return Err(
            "no usable fxc.exe found (PATH/Windows SDK). set SNOW_CAPTURE_FXC_PATH to an explicit fxc path".to_string()
        );
    }

    Err(attempts.join(" | "))
}

fn is_path_lookup(path: &Path) -> bool {
    path.file_name().is_some()
        && path.parent().is_none()
        && path
            .file_name()
            .is_some_and(|name| name.eq_ignore_ascii_case("fxc.exe"))
}

fn summarize_output(output: &Output) -> String {
    let status = output
        .status
        .code()
        .map_or_else(|| "terminated".to_string(), |code| format!("exit {code}"));
    let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
    let stdout = String::from_utf8_lossy(&output.stdout).trim().to_string();
    let mut diagnostic = if !stderr.is_empty() {
        stderr
    } else if !stdout.is_empty() {
        stdout
    } else {
        "no compiler diagnostic output".to_string()
    };
    if diagnostic.len() > 260 {
        diagnostic.truncate(260);
        diagnostic.push_str("...");
    }
    format!("{status}, {diagnostic}")
}

fn fxc_candidates() -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(path) = env::var("SNOW_CAPTURE_FXC_PATH") {
        let path = path.trim();
        if !path.is_empty() {
            out.push(PathBuf::from(path));
        }
    }

    out.push(PathBuf::from("fxc.exe"));

    if let Ok(bin_path) = env::var("WindowsSdkVerBinPath") {
        let bin = PathBuf::from(bin_path);
        out.push(bin.join("x64").join("fxc.exe"));
        out.push(bin.join("x86").join("fxc.exe"));
    }

    if let (Ok(sdk_dir), Ok(sdk_version)) =
        (env::var("WindowsSdkDir"), env::var("WindowsSDKVersion"))
    {
        let sdk_bin_subdir = sdk_version.trim_matches(|c| c == '\\' || c == '/');
        if !sdk_bin_subdir.is_empty() {
            let bin = PathBuf::from(sdk_dir).join("bin").join(sdk_bin_subdir);
            out.push(bin.join("x64").join("fxc.exe"));
            out.push(bin.join("x86").join("fxc.exe"));
        }
    }

    out.extend(scan_windows_kits_fxc());

    dedup_paths(out)
}

fn scan_windows_kits_fxc() -> Vec<PathBuf> {
    let mut out = Vec::new();
    let Ok(program_files) = env::var("ProgramFiles(x86)") else {
        return out;
    };

    let bin_root = PathBuf::from(program_files)
        .join("Windows Kits")
        .join("10")
        .join("bin");
    let Ok(entries) = std::fs::read_dir(bin_root) else {
        return out;
    };

    let mut dirs = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            dirs.push(path);
        }
    }
    dirs.sort();
    dirs.reverse();

    for dir in dirs {
        out.push(dir.join("x64").join("fxc.exe"));
        out.push(dir.join("x86").join("fxc.exe"));
    }

    out
}

fn dedup_paths(paths: Vec<PathBuf>) -> Vec<PathBuf> {
    let mut unique = Vec::new();
    for path in paths {
        if unique.iter().any(|seen| seen == &path) {
            continue;
        }
        unique.push(path);
    }
    unique
}
