#[path = "../link_manifest.rs"]
mod link_manifest;

#[test]
fn translates_apple_framework_link_items_to_native_cargo_directives() {
    assert_eq!(
        link_manifest::cargo_directives("-framework Foundation"),
        ["cargo:rustc-link-lib=framework=Foundation"]
    );
}

#[test]
fn preserves_single_linker_arguments_and_ignores_manifest_comments() {
    assert_eq!(
        link_manifest::cargo_directives("/a library path/libonnxruntime.a"),
        ["cargo:rustc-link-arg=/a library path/libonnxruntime.a"]
    );
    assert!(link_manifest::cargo_directives("  # generated  ").is_empty());
    assert!(link_manifest::cargo_directives("  ").is_empty());
}
