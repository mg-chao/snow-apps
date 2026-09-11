fn main() {
    #[cfg(target_os = "macos")]
    {
        println!("cargo:rerun-if-changed=src/native.m");
        println!("cargo:rerun-if-changed=src/native.h");
        cc::Build::new()
            .file("src/native.m")
            .flag("-fobjc-arc")
            .flag("-fblocks")
            .flag("-mmacosx-version-min=14.0")
            .warnings_into_errors(true)
            .compile("snow_macos_native");
        for framework in ["AppKit", "CoreGraphics", "ScreenCaptureKit", "Foundation"] {
            println!("cargo:rustc-link-lib=framework={framework}");
        }
    }
}
