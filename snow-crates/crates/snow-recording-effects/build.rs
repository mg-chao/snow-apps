fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        return;
    }
    println!("cargo:rerun-if-changed=src/keyboard_macos.m");
    println!("cargo:rerun-if-changed=src/keyboard_macos.h");
    cc::Build::new()
        .file("src/keyboard_macos.m")
        .flag("-fobjc-arc")
        .flag("-mmacosx-version-min=14.0")
        .warnings_into_errors(true)
        .compile("snow_recording_keyboard_macos");
    for framework in ["Carbon", "CoreGraphics", "CoreText", "Foundation"] {
        println!("cargo:rustc-link-lib=framework={framework}");
    }
}
