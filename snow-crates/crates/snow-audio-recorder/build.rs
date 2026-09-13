fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        return;
    }
    println!("cargo:rerun-if-changed=src/platform/macos/native.m");
    println!("cargo:rerun-if-changed=src/platform/macos/native.h");
    cc::Build::new()
        .file("src/platform/macos/native.m")
        .flag("-fobjc-arc")
        .flag("-fblocks")
        .flag("-mmacosx-version-min=14.0")
        .warnings_into_errors(true)
        .compile("snow_macos_audio");
    for framework in [
        "AVFoundation",
        "AudioToolbox",
        "CoreAudio",
        "CoreMedia",
        "ScreenCaptureKit",
        "CoreGraphics",
        "Foundation",
    ] {
        println!("cargo:rustc-link-lib=framework={framework}");
    }
}
