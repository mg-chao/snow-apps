# Pinned ort logging fix

The Cargo overrides in `snow-crates/Cargo.toml` consume the logging fix from
https://github.com/mg-chao/ort without vendoring its source in Snow Shot.

- Branch: `fix/native-log-diagnostics`
- Pinned commit: `90018ee581fb1bb1baf8e82eba8e679728ff27dc`
- Base: `ort` 2.0.0-rc.13, revision `002f41a8e175eac7f6695ff361d2e51a50874c48`
- License: MIT OR Apache-2.0; release packaging collects the original notices
  from Cargo's Git checkout.

The patch changes only `src/logging.rs`. It preserves readable native diagnostic
text and escapes invalid UTF-8 bytes instead of discarding the entire message.
It also forwards the actual category to custom loggers rather than the code
location. Callback regressions reproduce both defects without an inference model.

Both `ort` and `ort-sys` use the same pinned commit because the upstream wrapper
references its sibling native crate, which Snow Shot's static backend also uses
directly. Pinning both keeps a single native API type identity and prevents branch
updates from changing builds implicitly.

Run the focused regressions from an ort checkout:

```powershell
cargo test --no-default-features --features std,ndarray,api-28,load-dynamic,directml,tracing --lib native_logger_tests
cargo test --no-default-features --features std,ndarray,api-28,load-dynamic,directml --lib native_logger_tests
cargo check -p ort --no-default-features --features api-28,alternative-backend
```

Remove both overrides after an upstream release includes the fixes and the
callback regressions pass against it. The patch cannot recover diagnostic bytes
already discarded in historical logs.
