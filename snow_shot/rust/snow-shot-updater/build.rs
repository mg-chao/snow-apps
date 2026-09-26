use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=SNOW_SHOT_UPDATE_KEYS_PATH");
    println!("cargo:rerun-if-env-changed=SNOW_SHOT_VERSION");
    println!("cargo:rerun-if-env-changed=SNOW_SHOT_ICON_PATH");

    let keys = env::var_os("SNOW_SHOT_UPDATE_KEYS_PATH")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("../../resources/update-trusted-keys.json"));
    println!("cargo:rerun-if-changed={}", keys.display());
    let output = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is required"));
    fs::copy(&keys, output.join("update-trusted-keys.json")).unwrap_or_else(|error| {
        panic!(
            "failed to embed update keys from {}: {error}",
            keys.display()
        )
    });

    #[cfg(windows)]
    compile_windows_resources(&output);
}

#[cfg(windows)]
fn compile_windows_resources(output: &std::path::Path) {
    let version =
        env::var("SNOW_SHOT_VERSION").unwrap_or_else(|_| env::var("CARGO_PKG_VERSION").unwrap());
    let numeric = version
        .split_once('-')
        .map_or(version.as_str(), |value| value.0);
    let mut parts = numeric.split('.');
    let major = parts.next().expect("major version");
    let minor = parts.next().expect("minor version");
    let patch = parts.next().expect("patch version");
    assert!(
        parts.next().is_none(),
        "Snow Shot version must be major.minor.patch[-suffix]"
    );
    for part in [major, minor, patch] {
        part.parse::<u16>()
            .expect("version components must be integers");
    }

    let resource_path = output.join("snow-shot-updater.rc");
    // Normal startup runs --transaction-state through CreateProcess. Declare
    // asInvoker so Windows never guesses that this "updater" needs elevation.
    // Protected installation writes still use the explicit runas handoff.
    let manifest =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("windows.manifest");
    println!("cargo:rerun-if-changed={}", manifest.display());
    let manifest_path = manifest.display().to_string().replace('\\', "\\\\");
    let icon = env::var_os("SNOW_SHOT_ICON_PATH").map(PathBuf::from);
    if let Some(icon) = &icon {
        println!("cargo:rerun-if-changed={}", icon.display());
    }
    let icon_line = icon
        .as_ref()
        .map(|path| {
            format!(
                "1 ICON \"{}\"\n",
                path.display().to_string().replace('\\', "\\\\")
            )
        })
        .unwrap_or_default();
    let contents = format!(
        r#"#pragma code_page(65001)
1 24 "{manifest_path}"
{icon_line}1 VERSIONINFO
FILEVERSION {major},{minor},{patch},0
PRODUCTVERSION {major},{minor},{patch},0
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x0L
FILEOS 0x40004L
FILETYPE 0x1L
FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "000004b0"
        BEGIN
            VALUE "CompanyName", "Snow Apps\0"
            VALUE "FileDescription", "Snow Shot update service\0"
            VALUE "FileVersion", "{numeric}.0\0"
            VALUE "InternalName", "snow-shot-updater\0"
            VALUE "LegalCopyright", "Copyright (C) 2025-2026 mg-chao\0"
            VALUE "OriginalFilename", "snow-shot-updater.exe\0"
            VALUE "ProductName", "Snow Shot\0"
            VALUE "ProductVersion", "{version}\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0, 0x04b0
    END
END
"#
    );
    fs::write(&resource_path, contents).expect("failed to write updater resource file");
    let mut resource = winres::WindowsResource::new();
    resource.set_resource_file(resource_path.to_str().expect("resource path must be UTF-8"));
    resource
        .compile()
        .expect("failed to compile updater resources");
}
