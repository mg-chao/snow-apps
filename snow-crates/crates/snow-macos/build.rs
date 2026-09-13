fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        return;
    }
    println!("cargo:rerun-if-changed=src/native.m");
    println!("cargo:rerun-if-changed=src/native.h");
    println!("cargo:rerun-if-changed=src/stream.m");
    cc::Build::new()
        .file("src/native.m")
        .file("src/stream.m")
        .flag("-fobjc-arc")
        .flag("-fblocks")
        .flag("-mmacosx-version-min=14.0")
        .warnings_into_errors(true)
        .compile("snow_macos_native");
    for framework in [
        "AppKit",
        "ApplicationServices",
        "CoreGraphics",
        "CoreMedia",
        "CoreVideo",
        "ScreenCaptureKit",
        "Foundation",
    ] {
        println!("cargo:rustc-link-lib=framework={framework}");
    }
}
