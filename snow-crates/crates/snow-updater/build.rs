//! Emits a deterministic fixed-order VERSIONINFO for the standalone helper.
//!
//! The application version normally arrives from CMake through
//! `SNOW_SHOT_UPDATER_VERSION`; plain cargo builds fall back to the crate
//! version. winres stores generated fields in hash maps, so the resource is
//! written by hand in a fixed order, matching the snow-ocr-process pattern.

use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=SNOW_SHOT_UPDATER_VERSION");
    println!("cargo:rerun-if-changed=../../../snow_shot/resources/update-trusted-keys.json");
    #[cfg(windows)]
    {
        let version = env::var("SNOW_SHOT_UPDATER_VERSION").unwrap_or_else(|_| {
            env::var("CARGO_PKG_VERSION").expect("Cargo package version is required")
        });
        let core = version.split(['-', '+']).next().unwrap_or(&version);
        let mut version_parts = core.split('.');
        let major = version_parts.next().unwrap_or("0");
        let minor = version_parts.next().unwrap_or("0");
        let patch = version_parts.next().unwrap_or("0");
        let numeric = |part: &str| {
            if !part.is_empty() && part.chars().all(|c| c.is_ascii_digit()) {
                part.to_owned()
            } else {
                "0".to_owned()
            }
        };
        let (major, minor, patch) = (numeric(major), numeric(minor), numeric(patch));

        let resource_path =
            PathBuf::from(env::var("OUT_DIR").expect("Cargo output path is required"))
                .join("snow-shot-updater.rc");
        let resource_contents = format!(
            r#"#pragma code_page(65001)
1 VERSIONINFO
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
            VALUE "FileDescription", "Snow Shot update helper\0"
            VALUE "FileVersion", "{version}.0\0"
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
"#,
        );
        fs::write(&resource_path, resource_contents)
            .expect("failed to write deterministic snow-shot-updater Windows resources");
        let mut resource = winres::WindowsResource::new();
        resource.set_resource_file(
            resource_path
                .to_str()
                .expect("Windows resource path must be valid UTF-8"),
        );
        resource
            .compile()
            .expect("failed to compile snow-shot-updater Windows resources");
    }
}
