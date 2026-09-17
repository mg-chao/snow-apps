//! Snow Shot standalone update helper and release-contract verifier.
//!
//! The crate is built in two shapes: a size-optimized `snow-shot-updater`
//! executable (the helper launched by the application, NSIS, and recovery),
//! and a library linked into the application through the Rust FFI bundle so
//! the in-app update service shares one contract implementation.

pub mod contract;
pub mod crypto;
pub mod errors;
pub mod installation;
pub mod keys;
pub mod paths;
pub mod platform;
pub mod semver;
#[cfg(any(test, feature = "signing"))]
pub mod testing;
pub mod transaction;
pub mod zipentry;
